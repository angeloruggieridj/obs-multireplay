/*
obs-multireplay — fetching and verifying the Branch Output installer
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See branch-output-install.hpp for why this exists and what it refuses to do.
*/

#include <obs-module.h>
#include "plugin-support.h"

#include "branch-output-install.hpp"

#include "path-utf8.hpp"
#include "sha256.hpp"
#include "update-asset.hpp"
#include "updater.hpp" // the one HTTPS door

#include <obs.h>
#include <util/platform.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace multireplay {

namespace {

// The LATEST release, not the list: this is not a channel the operator chooses
// between, it is the one version of somebody else's plugin that we can say
// anything about.
constexpr const char *kLatestApi =
	"https://api.github.com/repos/OPENSPHERE-Inc/branch-output/releases/latest";

// Read the file back in blocks and hash it. Streaming rather than slurping,
// because the thing being hashed is an installer and the machine doing it may
// be recording.
bool hashFile(const std::filesystem::path &p, std::string &hexOut)
{
	// os_fopen, not std::fopen: it converts UTF-8 to wide itself, and the
	// temp folder can sit under a user name with an accent in it. That is
	// the whole point of path-utf8.hpp.
	FILE *f = os_fopen(pathToUtf8(p).c_str(), "rb");
	if (!f)
		return false;
	sha256::Hasher h;
	std::vector<unsigned char> buf(64 * 1024);
	for (;;) {
		const size_t n = std::fread(buf.data(), 1, buf.size(), f);
		if (n > 0)
			h.update(buf.data(), n);
		if (n < buf.size())
			break;
	}
	const bool ok = std::ferror(f) == 0;
	std::fclose(f);
	if (!ok)
		return false;
	hexOut = h.hex();
	return true;
}

} // namespace

BranchOutputInstall &BranchOutputInstall::instance()
{
	static BranchOutputInstall inst;
	return inst;
}

BranchOutputInstall::~BranchOutputInstall()
{
	shutdown();
}

BranchOutputInstall::Status BranchOutputInstall::status() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return status_;
}

void BranchOutputInstall::setStatus(const Status &s)
{
	std::lock_guard<std::mutex> lock(mutex_);
	status_ = s;
}

void BranchOutputInstall::setFailed(const std::string &why)
{
	Status s = status();
	s.phase = Phase::Failed;
	s.message = why;
	setStatus(s);
	obs_log(LOG_WARNING, "[bo-install] %s", why.c_str());
}

void BranchOutputInstall::joinWorker()
{
	if (worker_.joinable())
		worker_.join();
}

void BranchOutputInstall::shutdown()
{
	abort_.store(true);
	joinWorker();
}

void BranchOutputInstall::startAsync()
{
	bool expected = false;
	if (!busy_.compare_exchange_strong(expected, true))
		return; // one at a time; a second press is not a second download
	abort_.store(false);
	joinWorker(); // the previous one has finished; collect it

	Status s;
	s.phase = Phase::Asking;
	setStatus(s);

	worker_ = std::thread([this]() {
		run();
		busy_.store(false);
	});
}

void BranchOutputInstall::run()
{
	// --- 1. which release, and which file in it --------------------------
	std::string body, err;
	if (!Updater::fetchText(kLatestApi, body, err)) {
		setFailed("could not reach GitHub: " + err);
		return;
	}
	if (abort_.load())
		return;

	obs_data_t *root = obs_data_create_from_json(body.c_str());
	if (!root) {
		setFailed("GitHub's answer was not JSON this plugin can read");
		return;
	}
	const std::string tag = obs_data_get_string(root, "tag_name");
	obs_data_array_t *assets = obs_data_get_array(root, "assets");

	std::vector<update_asset::Asset> list;
	std::vector<std::string> digests; // parallel to `list`, by index
	const size_t n = assets ? obs_data_array_count(assets) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *a = obs_data_array_item(assets, i);
		update_asset::Asset item;
		item.name = obs_data_get_string(a, "name");
		item.url = obs_data_get_string(a, "browser_download_url");
		item.size = obs_data_get_int(a, "size");
		list.push_back(item);
		digests.push_back(obs_data_get_string(a, "digest"));
		obs_data_release(a);
	}
	if (assets)
		obs_data_array_release(assets);
	obs_data_release(root);

	update_asset::Asset chosen;
	if (!update_asset::pickInstaller(list, update_asset::hostPlatform(),
					 chosen)) {
		setFailed("the latest Branch Output release has no installer "
			  "for this platform");
		return;
	}
	// The digest travels beside the asset, so it is looked up by the name we
	// settled on rather than carried through the pure picker — which has no
	// business knowing about digests.
	std::string wantHex;
	for (size_t i = 0; i < list.size(); i++)
		if (list[i].name == chosen.name)
			wantHex = update_asset::sha256FromDigest(digests[i]);

	// NO DIGEST, NO RUN. This is somebody else's installer about to be
	// started on the operator's machine; the updater already refuses its own
	// asset on the same grounds, and a third party's deserves more care, not
	// less.
	if (wantHex.empty()) {
		setFailed("GitHub published no checksum for " + chosen.name +
			  " — refusing to fetch a file this plugin cannot verify");
		return;
	}

	{
		Status s = status();
		s.phase = Phase::Downloading;
		s.version = tag;
		s.assetName = chosen.name;
		s.percent = 0;
		setStatus(s);
	}
	obs_log(LOG_INFO,
		"[bo-install] Branch Output %s — fetching %s (%lld bytes)",
		tag.c_str(), chosen.name.c_str(), (long long)chosen.size);

	// --- 2. download ------------------------------------------------------
	// Into a folder of our own under temp, so a name collision with whatever
	// else is in there cannot make us hash one file and open another.
	std::error_code ec;
	std::filesystem::path dir =
		std::filesystem::temp_directory_path(ec) / "obs-multireplay-bo";
	if (ec) {
		setFailed("no temporary folder to download into");
		return;
	}
	std::filesystem::create_directories(dir, ec);
	// chosen.name went through isSafeAssetName inside pickInstaller, so it
	// cannot climb out of this folder.
	const std::filesystem::path dest = dir / utf8ToPath(chosen.name);
	const std::string destUtf8 = pathToUtf8(dest);

	if (!Updater::fetchFile(chosen.url, destUtf8, &abort_, err)) {
		setFailed("the download failed: " + err);
		return;
	}
	if (abort_.load())
		return;

	// --- 3. verify --------------------------------------------------------
	{
		Status s = status();
		s.phase = Phase::Verifying;
		setStatus(s);
	}
	std::string gotHex;
	if (!hashFile(dest, gotHex)) {
		std::filesystem::remove(dest, ec);
		setFailed("could not read the download back to check it");
		return;
	}
	if (gotHex != wantHex) {
		// DELETED, not merely rejected. A file that failed this check has
		// no business staying on the operator's disk under a name that
		// looks like an installer.
		std::filesystem::remove(dest, ec);
		setFailed("the download does not match the checksum GitHub "
			  "published for it — deleted");
		return;
	}

	obs_log(LOG_INFO,
		"[bo-install] %s verified (sha256 %s…) — ready to hand to the system",
		chosen.name.c_str(), gotHex.substr(0, 16).c_str());

	Status s = status();
	s.phase = Phase::Ready;
	s.filePath = destUtf8;
	s.percent = 100;
	setStatus(s);
}

} // namespace multireplay
