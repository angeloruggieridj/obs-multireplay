/*
obs-multireplay — see updater.hpp.
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "updater.hpp"

#include "plugin-support.h"
#include "version-compare.hpp"

#include <util/platform.h>

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>

namespace multireplay {

namespace {

// Where the releases are published. One place, so a fork does not have to hunt
// for it, and https because this decides what code the operator ends up running.
constexpr const char *kReleasesApi =
	"https://api.github.com/repos/angeloruggieridj/obs-multireplay/releases?per_page=20";

// GitHub refuses anonymous requests without one, and a plugin that says what it
// is makes its traffic legible to whoever reads the logs at the other end.
constexpr const char *kUserAgent = "obs-multireplay-updater";

// Generous, because a rig on venue wifi is not a datacentre — but finite,
// because a request that never ends is a thread that never ends.
constexpr long kConnectTimeoutSec = 15;
constexpr long kCheckTimeoutSec = 30;
constexpr long kDownloadTimeoutSec = 600;

// An archive far bigger than any release of this plugin is not a release of
// this plugin. The build is a couple of megabytes; this sits two orders above
// it so a redirect to something enormous cannot fill a recording disk.
constexpr int64_t kMaxAssetBytes = 200ll * 1024 * 1024;

size_t appendToString(void *data, size_t size, size_t nmemb, void *user)
{
	auto *out = static_cast<std::string *>(user);
	out->append(static_cast<const char *>(data), size * nmemb);
	return size * nmemb;
}

size_t appendToFile(void *data, size_t size, size_t nmemb, void *user)
{
	auto *out = static_cast<std::ofstream *>(user);
	out->write(static_cast<const char *>(data),
		   (std::streamsize)(size * nmemb));
	return out->good() ? size * nmemb : 0;
}

struct ProgressCtx {
	std::atomic<bool> *abort = nullptr;
	std::function<void(int)> report;
};

int onProgress(void *user, curl_off_t total, curl_off_t now, curl_off_t,
	       curl_off_t)
{
	auto *ctx = static_cast<ProgressCtx *>(user);
	if (ctx->abort && ctx->abort->load())
		return 1; // non-zero aborts the transfer
	if (total > 0 && ctx->report)
		ctx->report((int)((now * 100) / total));
	return 0;
}

// One GET. Shared by the check and the download so there is a single place that
// decides about redirects, timeouts and certificate verification — three
// settings it is very easy to get wrong once per call site.
bool httpGet(const std::string &url, long timeoutSec, std::string *body,
	     std::ofstream *file, ProgressCtx *progress, std::string &errorOut)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		errorOut = "could not initialise the HTTP client";
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	// NOT negotiable. This chooses which binary an operator installs, so a
	// certificate that does not check out is a refusal, never a warning.
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
			 (curl_off_t)kMaxAssetBytes);

	if (file) {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToFile);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
	} else {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
	}
	if (progress) {
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	}

	const CURLcode rc = curl_easy_perform(curl);
	long http = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
	curl_easy_cleanup(curl);

	if (rc == CURLE_ABORTED_BY_CALLBACK) {
		errorOut = "cancelled";
		return false;
	}
	if (rc != CURLE_OK) {
		errorOut = curl_easy_strerror(rc);
		return false;
	}
	if (http < 200 || http >= 300) {
		errorOut = "the server answered " + std::to_string(http);
		return false;
	}
	return true;
}

// The asset to fetch, out of a release's list. Prefers an archive whose name
// says which platform it is for: a release carries one per platform, and
// installing the wrong one is worse than installing nothing.
bool pickAsset(obs_data_array_t *assets, ReleaseInfo &out)
{
#if defined(_WIN32)
	static const char *kWanted[] = {"windows", "win64", "x64"};
#elif defined(__APPLE__)
	static const char *kWanted[] = {"macos", "mac", "darwin"};
#else
	static const char *kWanted[] = {"ubuntu", "linux"};
#endif
	const size_t n = obs_data_array_count(assets);
	std::string fallbackUrl, fallbackName;
	int64_t fallbackBytes = 0;

	for (size_t i = 0; i < n; i++) {
		obs_data_t *a = obs_data_array_item(assets, i);
		const std::string name = obs_data_get_string(a, "name");
		const std::string url =
			obs_data_get_string(a, "browser_download_url");
		const int64_t size = obs_data_get_int(a, "size");
		obs_data_release(a);
		if (url.empty() || name.empty())
			continue;

		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			       [](unsigned char c) { return (char)tolower(c); });
		const bool isArchive =
			(lower.size() > 4 &&
			 lower.rfind(".zip") == lower.size() - 4) ||
			(lower.size() > 7 &&
			 lower.rfind(".tar.gz") == lower.size() - 7);
		if (!isArchive)
			continue;

		for (const char *w : kWanted) {
			if (lower.find(w) != std::string::npos) {
				out.assetUrl = url;
				out.assetName = name;
				out.assetBytes = size;
				return true;
			}
		}
		if (fallbackUrl.empty()) {
			fallbackUrl = url;
			fallbackName = name;
			fallbackBytes = size;
		}
	}
	// A release with a single archive and no platform in its name is the
	// ordinary shape of a small project's first releases.
	if (!fallbackUrl.empty()) {
		out.assetUrl = fallbackUrl;
		out.assetName = fallbackName;
		out.assetBytes = fallbackBytes;
		return true;
	}
	return false;
}

std::filesystem::path stagingDir()
{
	std::error_code ec;
	return std::filesystem::temp_directory_path(ec) /
	       "obs-multireplay-update";
}

} // namespace

const char *updateChannelToString(UpdateChannel c)
{
	return c == UpdateChannel::Beta ? "beta" : "stable";
}

UpdateChannel updateChannelFromString(const std::string &s)
{
	// Anything unrecognised is STABLE. A config file edited by hand, or
	// written by a newer build that knows a channel this one does not, must
	// never quietly opt an operator into pre-releases.
	return s == "beta" ? UpdateChannel::Beta : UpdateChannel::Stable;
}

Updater &Updater::instance()
{
	static Updater u;
	return u;
}

Updater::~Updater()
{
	shutdown();
}

std::string Updater::currentVersion()
{
	return PLUGIN_VERSION;
}

Updater::Status Updater::status() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return status_;
}

void Updater::setStatus(const Status &s)
{
	std::lock_guard<std::mutex> lock(mutex_);
	status_ = s;
}

void Updater::joinWorker()
{
	if (worker_.joinable())
		worker_.join();
}

void Updater::shutdown()
{
	abort_.store(true);
	joinWorker();
	abort_.store(false);
	busy_.store(false);
}

void Updater::checkAsync(UpdateChannel channel)
{
	if (busy_.exchange(true))
		return; // one at a time; a second press is not a second request
	joinWorker();
	abort_.store(false);

	Status s;
	s.phase = Phase::Checking;
	setStatus(s);

	worker_ = std::thread([this, channel]() {
		std::string body, err;
		if (!httpGet(kReleasesApi, kCheckTimeoutSec, &body, nullptr,
			     nullptr, err)) {
			Status f;
			f.phase = Phase::Failed;
			f.message = err;
			setStatus(f);
			obs_log(LOG_WARNING, "[update] check failed: %s",
				err.c_str());
			busy_.store(false);
			return;
		}

		// The endpoint answers with an ARRAY, and obs_data parses
		// objects. Wrapping it costs one string and saves carrying a
		// second JSON library for a single call.
		const std::string wrapped = "{\"releases\":" + body + "}";
		obs_data_t *root = obs_data_create_from_json(wrapped.c_str());
		obs_data_array_t *arr =
			root ? obs_data_get_array(root, "releases") : nullptr;
		if (!arr) {
			if (root)
				obs_data_release(root);
			Status f;
			f.phase = Phase::Failed;
			f.message = "the release list could not be read";
			setStatus(f);
			busy_.store(false);
			return;
		}

		const std::string current = currentVersion();
		ReleaseInfo best;
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *r = obs_data_array_item(arr, i);
			ReleaseInfo info;
			info.version = obs_data_get_string(r, "tag_name");
			info.name = obs_data_get_string(r, "name");
			info.notes = obs_data_get_string(r, "body");
			info.prerelease = obs_data_get_bool(r, "prerelease");
			info.publishedAt = obs_data_get_string(r, "published_at");
			const bool draft = obs_data_get_bool(r, "draft");

			// A draft is not published. A pre-release is only for an
			// operator who asked for pre-releases.
			const bool wanted =
				!draft && (channel == UpdateChannel::Beta ||
					   !info.prerelease);
			if (wanted && isNewerVersion(info.version, current) &&
			    (!best.valid() ||
			     isNewerVersion(info.version, best.version))) {
				obs_data_array_t *assets =
					obs_data_get_array(r, "assets");
				if (assets) {
					if (pickAsset(assets, info))
						best = info;
					obs_data_array_release(assets);
				}
			}
			obs_data_release(r);
		}
		obs_data_array_release(arr);
		obs_data_release(root);

		Status out;
		if (best.valid()) {
			out.phase = Phase::Available;
			out.release = best;
			obs_log(LOG_INFO,
				"[update] %s is available (running %s, channel %s)",
				best.version.c_str(), current.c_str(),
				updateChannelToString(channel));
		} else {
			out.phase = Phase::UpToDate;
			obs_log(LOG_INFO,
				"[update] %s is the newest on the %s channel",
				current.c_str(), updateChannelToString(channel));
		}
		setStatus(out);
		busy_.store(false);
	});
}

void Updater::downloadAsync()
{
	const Status now = status();
	if (now.phase != Phase::Available || !now.release.valid())
		return;
	if (busy_.exchange(true))
		return;
	joinWorker();
	abort_.store(false);

	const ReleaseInfo rel = now.release;
	Status s;
	s.phase = Phase::Downloading;
	s.release = rel;
	s.percent = 0;
	setStatus(s);

	worker_ = std::thread([this, rel]() {
		std::error_code ec;
		const std::filesystem::path dir = stagingDir();
		std::filesystem::create_directories(dir, ec);
		const std::filesystem::path target = dir / rel.assetName;

		auto fail = [&](const std::string &why) {
			Status f;
			f.phase = Phase::Failed;
			f.release = rel;
			f.message = why;
			setStatus(f);
			obs_log(LOG_WARNING, "[update] download failed: %s",
				why.c_str());
			busy_.store(false);
		};

		{
			std::ofstream out(target,
					  std::ios::binary | std::ios::trunc);
			if (!out) {
				fail("cannot write to " + dir.string());
				return;
			}
			ProgressCtx ctx;
			ctx.abort = &abort_;
			ctx.report = [this, rel](int pct) {
				Status p;
				p.phase = Phase::Downloading;
				p.release = rel;
				p.percent = pct;
				setStatus(p);
			};
			std::string err;
			if (!httpGet(rel.assetUrl, kDownloadTimeoutSec, nullptr,
				     &out, &ctx, err)) {
				out.close();
				std::filesystem::remove(target, ec);
				fail(err);
				return;
			}
		}

		// WHAT ARRIVED IS WHAT WAS ANNOUNCED. The release says how many
		// bytes the archive is; a file that is not that size is a
		// truncated download or something else entirely, and either way
		// it is not going to be unpacked over a working install.
		const auto got = (int64_t)std::filesystem::file_size(target, ec);
		if (ec || (rel.assetBytes > 0 && got != rel.assetBytes)) {
			std::filesystem::remove(target, ec);
			fail("the download is " + std::to_string(got) +
			     " bytes, the release says " +
			     std::to_string(rel.assetBytes));
			return;
		}

		Status done;
		done.phase = Phase::Staged;
		done.release = rel;
		done.percent = 100;
		done.stagedPath = target.string();
		setStatus(done);
		obs_log(LOG_INFO, "[update] %s staged at %s",
			rel.version.c_str(), done.stagedPath.c_str());
		busy_.store(false);
	});
}

bool Updater::installStaged(std::string &errorOut)
{
	const Status s = status();
	if (s.phase != Phase::Staged || s.stagedPath.empty()) {
		errorOut = "there is nothing staged to install";
		return false;
	}

#if defined(_WIN32)
	// The helper. It waits for THIS OBS to exit — the DLL is loaded and
	// locked, and more importantly a recording must never be interrupted by
	// an update — then unpacks over the installed plugin and starts OBS
	// again. Written as a script rather than done in-process because
	// whatever does this has to outlive the process being replaced.
	const std::filesystem::path dir = stagingDir();
	const std::filesystem::path script = dir / "install-update.ps1";
	const std::string pluginDir =
		"C:\\ProgramData\\obs-studio\\plugins\\obs-multireplay";

	std::ofstream out(script, std::ios::trunc);
	if (!out) {
		errorOut = "cannot write the installer to " + dir.string();
		return false;
	}
	out << "$ErrorActionPreference = 'Stop'\n"
	    << "# Written by obs-multireplay. Waits for OBS to close, unpacks the\n"
	    << "# update over the installed plugin, then starts OBS again.\n"
	    << "$archive = '" << s.stagedPath << "'\n"
	    << "$target  = '" << pluginDir << "'\n"
	    << "$proc = Get-Process obs64 -ErrorAction SilentlyContinue\n"
	    << "$exe = if ($proc) { $proc[0].Path } else { 'C:\\Program "
	       "Files\\obs-studio\\bin\\64bit\\obs64.exe' }\n"
	    << "for ($i = 0; $i -lt 900; $i++) {\n"
	    << "  if (-not (Get-Process obs64 -ErrorAction SilentlyContinue)) { "
	       "break }\n"
	    << "  Start-Sleep -Seconds 1\n"
	    << "}\n"
	    << "if (Get-Process obs64 -ErrorAction SilentlyContinue) { exit 1 }\n"
	    << "$stage = Join-Path ([IO.Path]::GetTempPath()) "
	       "'obs-multireplay-unpack'\n"
	    << "if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }\n"
	    << "Expand-Archive -LiteralPath $archive -DestinationPath $stage "
	       "-Force\n"
	    << "# The archive may carry the plugin folder at its root or a level\n"
	    << "# down; take whichever actually holds the binary.\n"
	    << "$src = $stage\n"
	    << "$dll = Get-ChildItem $stage -Recurse -Filter "
	       "'obs-multireplay.dll' | Select-Object -First 1\n"
	    << "if ($dll) { $src = $dll.Directory.Parent.Parent.FullName }\n"
	    << "Copy-Item (Join-Path $src '*') $target -Recurse -Force\n"
	    << "Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue\n"
	    << "Start-Process -FilePath $exe\n";
	out.close();

	// Detached: it has to survive the process it is waiting for.
	const std::string cmd =
		"start \"\" /B powershell.exe -NoProfile -ExecutionPolicy Bypass "
		"-WindowStyle Hidden -File \"" +
		script.string() + "\"";
	if (std::system(cmd.c_str()) != 0) {
		errorOut = "could not start the installer";
		return false;
	}
	obs_log(LOG_INFO,
		"[update] installer armed — it unpacks %s once OBS has exited",
		s.release.version.c_str());
	return true;
#else
	// Elsewhere the archive is handed over and the operator unpacks it into
	// his own plugin directory. Guessing at a package layout we do not
	// control would be a worse answer than a clear instruction.
	errorOut = "";
	return false;
#endif
}

} // namespace multireplay
