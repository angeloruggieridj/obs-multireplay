/*
obs-multireplay — see updater.hpp.
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "updater.hpp"

#include "path-utf8.hpp"
#include "plugin-support.h"
#include "sha256.hpp"
#include "size-guard.hpp"
#include "update-asset.hpp"
#include "update-installer.hpp"
#include "version-compare.hpp"

#include <util/platform.h>

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
//
// Enforced on BYTES WRITTEN (see FileSink/appendToFile below and
// size-guard.hpp), not only on CURLOPT_MAXFILESIZE_LARGE: that option only
// ever looks at a DECLARED Content-Length, so a chunked response — which has
// none — could otherwise be written to this disk for the whole of
// kDownloadTimeoutSec.
constexpr int64_t kMaxAssetBytes = 200ll * 1024 * 1024;

// And the same idea for the JSON. CURLOPT_MAXFILESIZE_LARGE only acts on a
// DECLARED Content-Length, so a chunked response can go on for as long as it
// likes — into a std::string, in a plugin inside somebody's recording software.
// Twenty releases with their notes are tens of kilobytes.
constexpr size_t kMaxBodyBytes = 8u * 1024 * 1024;

// libcurl's global state. OBS has already initialised it on every platform we
// have looked at, but "has already" is not a guarantee — on Linux and macOS the
// libcurl OBS loads may not even be the one this plugin linked against — and
// calling curl_easy_init() before curl_global_init() is undefined rather than
// merely unlucky. Done once, on first use, and deliberately NOT cleaned up: the
// counter inside libcurl is shared with whoever else called it, and an
// unmatched cleanup is a crash where an unreleased count is nothing at all.
void ensureCurlGlobalInit()
{
	static std::once_flag once;
	std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct BodySink {
	std::string data;
	bool overflowed = false;
};

size_t appendToString(void *data, size_t size, size_t nmemb, void *user)
{
	auto *out = static_cast<BodySink *>(user);
	const size_t n = size * nmemb;
	if (out->data.size() + n > kMaxBodyBytes) {
		out->overflowed = true;
		return 0; // a short write aborts the transfer
	}
	out->data.append(static_cast<const char *>(data), n);
	return n;
}

// Unlike BodySink, this one is handed a DECLARED Content-Length by nothing:
// CURLOPT_MAXFILESIZE_LARGE watches that header, not the bytes actually
// arriving, so a chunked response — which never sends one — used to be
// written to this disk for as long as the server kept talking.
struct FileSink {
	std::ofstream *out = nullptr;
	int64_t written = 0;
	bool overflowed = false;
};

size_t appendToFile(void *data, size_t size, size_t nmemb, void *user)
{
	auto *s = static_cast<FileSink *>(user);
	const size_t n = size * nmemb;
	if (size_guard::wouldOverflow(s->written, n, kMaxAssetBytes)) {
		s->overflowed = true;
		return 0; // a short write aborts the transfer (CURLE_WRITE_ERROR)
	}
	s->out->write(static_cast<const char *>(data), (std::streamsize)n);
	s->written += (int64_t)n;
	return s->out->good() ? n : 0;
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
bool httpGet(const std::string &url, long timeoutSec, BodySink *body,
	     FileSink *file, ProgressCtx *progress, std::string &errorOut)
{
	ensureCurlGlobalInit();

	CURL *curl = curl_easy_init();
	if (!curl) {
		errorOut = "could not initialise the HTTP client";
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	// This chooses which binary an operator installs; FOLLOWLOCATION on its
	// own follows a redirect to whatever scheme curl was built to support,
	// which on some builds still includes file:// and legacy ftp. Both the
	// initial request and any redirect it follows are held to https, so a
	// compromised or misconfigured server cannot hand this process a local
	// path or an unauthenticated transfer to read from instead.
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
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
	// ALWAYS, download or check. The progress callback is the only thing
	// abort_ can act through, and registering it for the download only meant
	// a check against a server that had gone quiet was uninterruptible:
	// shutdown() then waited out the whole HTTP timeout, i.e. OBS took up to
	// thirty seconds to close.
	if (progress) {
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	}

	const CURLcode rc = curl_easy_perform(curl);
	long http = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
	curl_easy_cleanup(curl);

	if (body && body->overflowed) {
		errorOut = "the server sent more than this plugin will read";
		return false;
	}
	if (file && file->overflowed) {
		errorOut = "the server is sending more than this plugin will keep";
		return false;
	}
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

// The asset to fetch, out of a release's list. The CHOOSING is in
// update-asset.hpp, pure and unit tested; this only turns obs_data into the
// vector it takes.
bool pickAsset(obs_data_array_t *assets, ReleaseInfo &out)
{
	std::vector<update_asset::Asset> list;
	const size_t n = obs_data_array_count(assets);
	list.reserve(n);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *a = obs_data_array_item(assets, i);
		update_asset::Asset item;
		item.name = obs_data_get_string(a, "name");
		item.url = obs_data_get_string(a, "browser_download_url");
		item.size = obs_data_get_int(a, "size");
		obs_data_release(a);
		if (!item.name.empty() && !item.url.empty())
			list.push_back(std::move(item));
	}

	update_asset::Asset chosen;
	if (!update_asset::pick(list, update_asset::hostPlatform(), chosen))
		return false;
	out.assetUrl = chosen.url;
	out.assetName = chosen.name;
	out.assetBytes = chosen.size;
	return true;
}

std::filesystem::path stagingDir()
{
	std::error_code ec;
	const std::filesystem::path base =
		std::filesystem::temp_directory_path(ec);
#if defined(_WIN32)
	// %TEMP% is already per-user (C:\Users\<user>\AppData\Local\Temp): a
	// fixed name under it is not the cross-account collision it would be
	// on a shared /tmp.
	return base / "obs-multireplay-update";
#else
	// /tmp is shared between every account on the machine, and a fixed
	// name in it is a directory any other local user can pre-create — or
	// replace with a symlink pointing outside /tmp — before this process
	// ever gets to it. Scoping the name by uid is what keeps two accounts
	// from contesting the same path at all; ensureStagingDir() below
	// covers the case where the name was still grabbed first.
	return base / ("obs-multireplay-update-" + std::to_string(geteuid()));
#endif
}

// Creates stagingDir() if needed, and refuses to hand it back if it is not
// safe to write through. On POSIX that means: not a symlink, and owned by
// this account — a directory download/install code goes on to write files
// into and (on Windows) execute a script out of, so anything else it could
// be is exactly what a local attacker plants ahead of a predictable path.
// Windows gets owner-only permissions applied for depth, but not the
// ownership check itself: %TEMP% being per-user already rules out the
// cross-account race this exists for.
bool ensureStagingDir(std::filesystem::path &dir, std::string &errorOut)
{
	dir = stagingDir();
	if (dir.empty()) {
		errorOut = "could not resolve a temp directory";
		return false;
	}
	std::error_code ec;
#if !defined(_WIN32)
	struct stat st{};
	if (lstat(dir.c_str(), &st) == 0) {
		if (S_ISLNK(st.st_mode)) {
			errorOut = pathToUtf8(dir) +
				   " is a symlink, not a directory — refusing "
				   "to use it";
			return false;
		}
		if (!S_ISDIR(st.st_mode)) {
			errorOut = pathToUtf8(dir) +
				   " exists and is not a directory";
			return false;
		}
		if (st.st_uid != geteuid()) {
			errorOut = pathToUtf8(dir) +
				   " exists and is not owned by this account";
			return false;
		}
	}
#endif
	std::filesystem::create_directories(dir, ec);
	if (ec) {
		errorOut = "cannot create " + pathToUtf8(dir) + ": " +
			   ec.message();
		return false;
	}
	std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
				      std::filesystem::perm_options::replace,
				      ec);
	return true;
}

// The SHA-256 of a file, read in blocks: the archive must not be pulled into
// memory to be checked.
bool fileDigest(const std::filesystem::path &p, std::string &hexOut)
{
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return false;
	sha256::Hasher h;
	std::vector<char> buf(64 * 1024);
	while (in) {
		in.read(buf.data(), (std::streamsize)buf.size());
		const std::streamsize got = in.gcount();
		if (got > 0)
			h.update(buf.data(), (size_t)got);
	}
	hexOut = h.hex();
	return true;
}

#if defined(_WIN32)
// Where this very plugin is installed, derived from the module OBS actually
// loaded rather than from a constant. obs_get_module_binary_path() hands back
//   <root>/obs-multireplay/bin/64bit/obs-multireplay.dll
// and the folder an update is unpacked over is three levels up. Hardcoding
// C:\ProgramData\... was right on one machine and wrong on a portable install.
std::string installedPluginDir()
{
	const char *bin = obs_get_module_binary_path(obs_current_module());
	if (bin && *bin) {
		std::filesystem::path p = utf8ToPath(bin);
		// dll -> 64bit -> bin -> the plugin folder
		for (int i = 0; i < 3 && p.has_parent_path(); i++)
			p = p.parent_path();
		if (!p.empty() && p.has_filename())
			return pathToUtf8(p);
	}
	obs_log(LOG_WARNING,
		"[update] OBS did not say where this module lives — falling "
		"back to the default plugin folder");
	return "C:\\ProgramData\\obs-studio\\plugins\\obs-multireplay";
}

// Detached and shell-free. std::system() would have gone through cmd.exe, where
// '&', '^', '|' and %VAR% in a path are syntax before PowerShell sees them.
bool startDetached(const std::string &commandLine, std::string &errorOut)
{
	const int need = MultiByteToWideChar(CP_UTF8, 0, commandLine.c_str(),
					     -1, nullptr, 0);
	if (need <= 0) {
		errorOut = "the installer command line is not valid text";
		return false;
	}
	std::wstring wide((size_t)need, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, commandLine.c_str(), -1, wide.data(),
			    need);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};
	// DETACHED_PROCESS + a new process group: it has to outlive the process
	// it is waiting for.
	const BOOL ok = CreateProcessW(
		nullptr, wide.data(), nullptr, nullptr, FALSE,
		CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS |
			CREATE_UNICODE_ENVIRONMENT,
		nullptr, nullptr, &si, &pi);
	if (!ok) {
		errorOut = "could not start the installer (error " +
			   std::to_string((unsigned long)GetLastError()) + ")";
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

// Write UTF-8 bytes, exactly as given. std::ofstream on MSVC narrows a
// std::string path through the ANSI code page, so the PATH goes through
// utf8ToPath and the CONTENT is written as bytes.
bool writeUtf8File(const std::filesystem::path &p, const std::string &text)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out.write(text.data(), (std::streamsize)text.size());
	out.close();
	return out.good();
}

// The other half of writeUtf8File, for reading the same bytes back — see
// its call site in installStaged() for why the installer re-reads what it
// just wrote instead of trusting its own write call.
bool readUtf8File(const std::filesystem::path &p, std::string &textOut)
{
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	textOut = ss.str();
	return true;
}
#endif // _WIN32

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

bool Updater::canInstallHere()
{
	return update_asset::isInstallableHere(update_asset::hostPlatform());
}

// The two doors declared in the header. They exist so that the ONE place that
// decides about redirects, timeouts and certificate verification stays one
// place even when the thing being fetched is not an update of ours.
bool Updater::fetchText(const std::string &url, std::string &bodyOut,
			std::string &errorOut)
{
	BodySink sink;
	if (!httpGet(url, kCheckTimeoutSec, &sink, nullptr, nullptr, errorOut))
		return false;
	bodyOut = std::move(sink.data);
	return true;
}

bool Updater::fetchFile(const std::string &url, const std::string &destPathUtf8,
			std::atomic<bool> *abort, std::string &errorOut)
{
	// os_fopen wants UTF-8 and converts to wide itself; std::ofstream on
	// MSVC takes the ANSI code page, which is the bug path-utf8.hpp exists
	// for. The temp path this is handed comes from GetTempPathW, so it can
	// carry a user name with an accent in it.
	std::ofstream out(utf8ToPath(destPathUtf8), std::ios::binary);
	if (!out) {
		errorOut = "could not open the download for writing";
		return false;
	}
	FileSink sink{&out};
	ProgressCtx ctx;
	ctx.abort = abort;
	const bool ok = httpGet(url, kDownloadTimeoutSec, nullptr, &sink, &ctx,
				errorOut);
	out.close();
	if (!ok) {
		std::error_code ec;
		std::filesystem::remove(utf8ToPath(destPathUtf8), ec);
	}
	return ok;
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
		BodySink body;
		std::string err;
		// The progress callback is here only so abort_ has something to
		// act through: a check has no percentage worth showing.
		ProgressCtx ctx;
		ctx.abort = &abort_;
		if (!httpGet(kReleasesApi, kCheckTimeoutSec, &body, nullptr,
			     &ctx, err)) {
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
		const std::string wrapped = "{\"releases\":" + body.data + "}";
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
				"[update] %s is available (running %s, channel "
				"%s, asset %s)",
				best.version.c_str(), current.c_str(),
				updateChannelToString(channel),
				best.assetName.c_str());
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
	// The name is used to build a filename. It came out of an HTTP
	// response, so it is checked before it becomes a path and not after: a
	// release whose asset is called `..\..\obs64.exe` writes outside the
	// staging directory otherwise.
	if (!update_asset::isSafeAssetName(now.release.assetName)) {
		Status f;
		f.phase = Phase::Failed;
		f.release = now.release;
		f.message = "the release names its file in a way this plugin "
			    "will not write to disk";
		setStatus(f);
		obs_log(LOG_ERROR, "[update] refused asset name '%s'",
			now.release.assetName.c_str());
		return;
	}
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
		std::filesystem::path dir;

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

		std::string dirErr;
		if (!ensureStagingDir(dir, dirErr)) {
			fail(dirErr);
			return;
		}
		const std::filesystem::path target =
			dir / utf8ToPath(rel.assetName);

		// WHAT IS THIS FILE SUPPOSED TO BE? Asked BEFORE a byte is
		// fetched, because the answer decides whether fetching is worth
		// doing at all: the release body is CHECKSUMS.txt (push.yaml
		// publishes it as the body), so a release with no digest for its
		// own asset is a release this plugin cannot verify — and an
		// update it cannot verify is an update it does not install.
		const std::string wantDigest =
			update_asset::sha256For(rel.notes, rel.assetName);
		if (wantDigest.empty()) {
			fail("this release does not publish a SHA-256 for " +
			     rel.assetName +
			     ", so the download cannot be verified");
			return;
		}

		{
			std::ofstream out(target,
					  std::ios::binary | std::ios::trunc);
			if (!out) {
				fail("cannot write to " + pathToUtf8(dir));
				return;
			}
			FileSink sink{&out};
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
				     &sink, &ctx, err)) {
				out.close();
				std::filesystem::remove(target, ec);
				fail(err);
				return;
			}
		}

		// WHAT ARRIVED IS WHAT WAS ANNOUNCED — the bytes, not the size.
		// The size check that used to stand alone here compared the file
		// with the same JSON response the URL had come from, i.e. a
		// source with itself: anyone able to alter that response (a
		// compromised account, a corporate proxy doing TLS interception,
		// a mirror) handed OBS an arbitrary DLL to load at the next
		// start. It is kept, one step ahead of the real check, because a
		// truncated download deserves a clearer message than "digest
		// mismatch".
		const auto got = (int64_t)std::filesystem::file_size(target, ec);
		if (ec || (rel.assetBytes > 0 && got != rel.assetBytes)) {
			std::filesystem::remove(target, ec);
			fail("the download is " + std::to_string(got) +
			     " bytes, the release says " +
			     std::to_string(rel.assetBytes));
			return;
		}
		std::string gotDigest;
		if (!fileDigest(target, gotDigest)) {
			std::filesystem::remove(target, ec);
			fail("the download could not be read back to be verified");
			return;
		}
		if (gotDigest != wantDigest) {
			std::filesystem::remove(target, ec);
			obs_log(LOG_ERROR,
				"[update] SHA-256 MISMATCH on %s: got %s, the "
				"release says %s — deleted",
				rel.assetName.c_str(), gotDigest.c_str(),
				wantDigest.c_str());
			fail("the downloaded file does not match the checksum "
			     "the release publishes — it has been deleted");
			return;
		}

		Status done;
		done.phase = Phase::Staged;
		done.release = rel;
		done.percent = 100;
		done.stagedPath = pathToUtf8(target);
		setStatus(done);
		obs_log(LOG_INFO,
			"[update] %s staged at %s (sha256 %s, verified)",
			rel.version.c_str(), done.stagedPath.c_str(),
			gotDigest.c_str());
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
	//
	// NOTHING is interpolated into the script (see update-installer.hpp).
	// It is a constant; the three paths travel in a file beside it.
	std::filesystem::path dir;
	if (!ensureStagingDir(dir, errorOut))
		return false;
	const std::filesystem::path script =
		dir / update_installer::kScriptFileName;
	const std::filesystem::path params =
		dir / update_installer::kParamFileName;

	update_installer::Params p;
	p.archivePath = s.stagedPath;
	p.targetDir = installedPluginDir();
	// A fallback only: the script prefers the path of the obs64 process it
	// watched exit, which is right even for a portable install.
	p.obsExePath = "C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe";

	if (!writeUtf8File(params, update_installer::paramFile(p)) ||
	    !writeUtf8File(script, update_installer::script())) {
		errorOut = "cannot write the installer to " + pathToUtf8(dir);
		return false;
	}

	// READ BACK WHAT WAS JUST WRITTEN. ensureStagingDir() has already
	// confirmed the directory is ours, but a window remains between this
	// write and the moment the detached helper reads these two files back
	// — long enough, on a loaded machine, for anything else with write
	// access to this account's files to substitute one. The script has no
	// path inside it to tamper with, but the params file names the plugin
	// folder the helper overwrites.
	std::string paramsBack, scriptBack;
	if (!readUtf8File(params, paramsBack) ||
	    !readUtf8File(script, scriptBack) ||
	    paramsBack != update_installer::paramFile(p) ||
	    scriptBack != update_installer::script()) {
		errorOut =
			"the installer files changed right after being written — refusing to run them";
		return false;
	}

	const std::string cmd = update_installer::commandLine(
		update_installer::argvFor("powershell.exe",
					  pathToUtf8(script)));
	if (!startDetached(cmd, errorOut))
		return false;
	obs_log(LOG_INFO,
		"[update] installer armed — it unpacks %s over %s once OBS has "
		"exited",
		s.release.version.c_str(), p.targetDir.c_str());
	return true;
#else
	// Elsewhere the archive is handed over and the operator unpacks it into
	// his own plugin directory. Guessing at a package layout we do not
	// control would be a worse answer than a clear instruction — and the
	// panel is now told so BEFORE the download (canInstallHere), so this is
	// a last resort rather than a surprise at the end.
	errorOut = "";
	return false;
#endif
}

} // namespace multireplay
