/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See health.hpp. Collection only — every judgement is in health-rules.hpp, and
every action is the operator's.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "health.hpp"

#include "branch-output-control.hpp"
#include "packet-tap.hpp"
#include "plugin-support.h"

#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace multireplay {

static_assert(health::kMaxHealthAngles == kMaxCameras,
	      "health rules and the camera slots must agree on how many angles "
	      "exist, or a finding would name the wrong camera");

namespace {

// How often sample() really does its work, however often it is called.
constexpr int64_t kSampleIntervalNs = 1'000'000'000LL;
// Stat'ing the session folder can be a network round trip; once every five
// seconds is far more often than a disk fills up.
constexpr int64_t kDiskCheckIntervalNs = 5'000'000'000LL;
// The probe: big enough that the timing is not startup noise, small enough
// that nobody notices it appearing in a session folder.
constexpr int64_t kProbeBytes = 8LL * 1024 * 1024;
constexpr size_t kProbeChunk = 1024 * 1024;
constexpr const char *kProbeFileName = ".mr-diskprobe.tmp";

// Bytes a take will write per second: what Branch Output is configured to
// encode, on every camera it was given.
int64_t requiredBytesPerSec(const Config &cfg, int cameras)
{
	if (cameras <= 0)
		return 0;
	const int64_t kbps = (int64_t)cfg.videoBitrateKbps + cfg.audioBitrateKbps;
	return kbps * 1000 / 8 * cameras;
}

bool encoderIsRegistered(const char *id)
{
	const char *existing = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &existing); i++)
		if (existing && strcmp(existing, id) == 0)
			return true;
	return false;
}

// Can we actually write where the recordings are going? create_directories
// succeeding is not the same answer: a read-only share, a full quota and a
// path that already exists as a FILE all pass that and fail at the first packet.
bool folderIsWritable(const std::string &folder)
{
	if (folder.empty())
		return false;
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	if (!std::filesystem::is_directory(folder, ec))
		return false;
	const std::filesystem::path probe =
		std::filesystem::path(folder) / ".mr-writetest.tmp";
	FILE *f = os_fopen(probe.string().c_str(), "wb");
	if (!f)
		return false;
	const bool wrote = fwrite("mr", 1, 2, f) == 2;
	fclose(f);
	std::filesystem::remove(probe, ec);
	return wrote;
}

// Force the bytes past the page cache. Without this the probe measures RAM and
// reports 3 GB/s on a USB stick, which is worse than not measuring at all.
void flushToDevice(FILE *f)
{
	fflush(f);
#ifdef _WIN32
	_commit(_fileno(f));
#else
	fsync(fileno(f));
#endif
}

} // namespace

std::string findingText(const health::Finding &f)
{
	const std::string key = "Health." + f.id;
	const char *text = obs_module_text(key.c_str());
	// obs_module_text hands back the key when the string is missing. Showing
	// "Health.disk_too_slow" is ugly but still says what happened, which is
	// the point; the id alone reads better than the key.
	std::string out = (text && key != text) ? text : f.id;
	if (!f.detail.empty())
		out += " (" + f.detail + ")";
	return out;
}

std::string findingsBlock(const std::vector<health::Finding> &findings,
			  health::Level min)
{
	std::string out;
	for (const auto &f : findings) {
		if (f.level < min)
			continue;
		if (!out.empty())
			out += "\n";
		out += "• " + findingText(f);
	}
	return out;
}

HealthMonitor &HealthMonitor::instance()
{
	static HealthMonitor inst;
	return inst;
}

// ---------------------------------------------------------------------------
// Pre-flight
// ---------------------------------------------------------------------------

health::PreflightResult HealthMonitor::preflight(const Config &cfg,
						 int ringSecondsWanted) const
{
	health::PreflightInput in;
	in.branchOutputAvailable = branch_output::available();
	in.ringSecondsWanted = ringSecondsWanted;

	// --- cameras ---
	for (int i = 0; i < kMaxCameras; i++) {
		const std::string &name = cfg.cameras[i].sourceName;
		if (name.empty())
			continue;
		in.camerasConfigured++;
		obs_source_t *src = obs_get_source_by_name(name.c_str());
		if (src) {
			in.camerasWithSource++;
			obs_source_release(src);
		}
		// Two slots on one source means two Branch Output filters on it,
		// two files of the same picture, and two angles the operator
		// cannot tell apart on the multiview.
		for (int j = 0; j < i; j++)
			if (cfg.cameras[j].sourceName == name)
				in.duplicateSourceSlots++;
	}

	// --- encoder ---
	// An empty id means auto-detect, which always resolves to something
	// (x264 at worst), so only an explicit choice can be missing.
	in.encoderAvailable = cfg.videoEncoderId.empty() ||
			      encoderIsRegistered(cfg.videoEncoderId.c_str());

	// --- folder, space, bandwidth ---
	std::string folder = cfg.sessionFolder;
	if (!folder.empty() && !cfg.currentProjectName.empty())
		folder = (std::filesystem::path(folder) / cfg.currentProjectName)
				 .string();
	in.sessionFolderSet = !cfg.sessionFolder.empty();
	in.sessionFolderWritable = in.sessionFolderSet && folderIsWritable(folder);
	if (in.sessionFolderSet) {
		std::error_code ec;
		auto space = std::filesystem::space(cfg.sessionFolder, ec);
		if (!ec)
			in.diskFreeBytes = (int64_t)space.available;
	}
	// The measured number is only used for the volume it was measured on: a
	// session folder moved from an NVMe to a USB disk must not inherit the
	// NVMe's verdict. The key is the SESSION folder, not the project inside
	// it — write speed is a property of the disk, and probing inside the
	// project meant leaving an 8 MiB file in a directory somebody else was
	// entitled to delete (the gate does exactly that, and the wipe failed
	// with "used by another process", so that run silently measured the
	// previous run's footage).
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!probedFolder_.empty() && probedFolder_ == cfg.sessionFolder)
			in.diskWriteBytesPerSec = diskWriteBps_.load();
	}

	in.requiredBytesPerSec = requiredBytesPerSec(cfg, in.camerasWithSource);
	in.ringBytesPerSecond = in.requiredBytesPerSec;

	const uint64_t freeRam = os_get_sys_free_size();
	in.freeRamBytes = freeRam > 0 ? (int64_t)freeRam : -1;

	return health::preflight(in);
}

health::PreflightResult HealthMonitor::lastPreflight() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return lastPreflight_;
}

void HealthMonitor::rememberPreflight(const health::PreflightResult &r)
{
	std::lock_guard<std::mutex> lock(mutex_);
	lastPreflight_ = r;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

void HealthMonitor::takeStarted(const std::array<bool, kMaxCameras> &armed,
				int ringSecondsGranted, int64_t requiredBps,
				const std::string &sessionFolder)
{
	// A previous take's sampler must be gone before this one seats its
	// baselines, or the two would sample the same counters.
	if (samplerRun_.exchange(false) && sampler_.joinable())
		sampler_.join();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		const int64_t now = (int64_t)os_gettime_ns();
		armed_ = armed;
		for (int i = 0; i < kMaxCameras; i++)
			armedAtNs_[i] = armed[i] ? now : 0;
		recording_ = true;
		takeStartNs_ = now;
		targetRingSpanNs_ = (int64_t)ringSecondsGranted * 1'000'000'000LL;
		requiredBytesPerSec_ = requiredBps;
		// The baseline is taken here, not at plugin load: what OBS itself
		// allocated for the scene collection is not ours to be blamed for.
		const uint64_t rss = os_get_proc_resident_size();
		rssBaselineBytes_ = rss > 0 ? (int64_t)rss : -1;
		lastLagged_ = obs_get_lagged_frames();
		lastTotal_ = obs_get_total_frames();
		lastDiskCheckNs_ = 0;
		diskFreeBytes_ = -1;
		sessionFolder_ = sessionFolder;
		findings_.clear();
		lastIds_.clear();
	}

	samplerRun_.store(true);
	sampler_ = std::thread([this]() { samplerLoop(); });
}

void HealthMonitor::takeStopped()
{
	// Joined BEFORE the lock is taken: the sampler takes this same mutex to
	// publish, so joining under it would be a deadlock the first time the
	// two coincided — which, being a one-second race, would be in the field
	// and not here.
	if (samplerRun_.exchange(false) && sampler_.joinable())
		sampler_.join();

	std::lock_guard<std::mutex> lock(mutex_);
	recording_ = false;
	armed_.fill(false);
	// The findings go with the take: they described a take that is over, and
	// a red badge left standing over a stopped rig teaches the operator to
	// stop looking at the badge.
	findings_.clear();
	lastIds_.clear();
}

void HealthMonitor::samplerLoop()
{
	// Woken four times a second so STOP is answered promptly, but only one
	// observation a second is actually taken.
	int64_t nextNs = (int64_t)os_gettime_ns();
	while (samplerRun_.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		const int64_t now = (int64_t)os_gettime_ns();
		if (now < nextNs)
			continue;
		nextNs = now + kSampleIntervalNs;
		sampleOnce();
	}
}

void HealthMonitor::sampleOnce()
{
	// --- what the rules need, copied out under a short lock --------------
	health::RuntimeInput in;
	std::array<bool, kMaxCameras> armed{};
	std::array<int64_t, kMaxCameras> armedAt{};
	std::string folder;
	int64_t lastDiskCheck = 0, diskFree = -1;
	uint32_t prevLagged = 0, prevTotal = 0;
	const int64_t now = (int64_t)os_gettime_ns();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!recording_)
			return;
		armed = armed_;
		armedAt = armedAtNs_;
		folder = sessionFolder_;
		lastDiskCheck = lastDiskCheckNs_;
		diskFree = diskFreeBytes_;
		prevLagged = lastLagged_;
		prevTotal = lastTotal_;
		in.takeElapsedMs = (now - takeStartNs_) / 1'000'000LL;
		in.targetRingSpanNs = targetRingSpanNs_;
		in.requiredBytesPerSec = requiredBytesPerSec_;
		in.rssBaselineBytes = rssBaselineBytes_;
	}
	in.recording = true;

	// --- gathering, with NO lock of ours held ----------------------------
	// PacketTap::stats() takes the tap's big lock, and the tap holds that
	// same lock across a detach that can block on Branch Output. Holding
	// mutex_ here would hand that block straight to the dock. See health.hpp.
	auto &tap = PacketTap::instance();
	int64_t ringBytes = 0;
	for (int i = 0; i < kMaxCameras; i++) {
		auto &a = in.angles[i];
		a.armed = armed[i];
		if (!a.armed)
			continue;
		a.armedForMs = (now - armedAt[i]) / 1'000'000LL;
		const TapStats st = tap.stats(i);
		a.attached = st.attached;
		a.malformedPackets = st.malformedPackets;
		a.discontinuities = st.discontinuities;
		a.ringSpanNs = st.ringSpanNs;
		ringBytes += (int64_t)st.ringBytes;
		// The newest packet sits on the master clock, which IS the system
		// clock os_gettime_ns() reads — that is the whole point of
		// sys_dts_usec — so this difference is the age of the live edge.
		const int64_t newest = st.ringNewestNs;
		a.sinceLastPacketMs = newest > 0 ? (now - newest) / 1'000'000LL : -1;
	}
	in.ringBytesTotal = ringBytes;

	const uint32_t lagged = obs_get_lagged_frames();
	const uint32_t total = obs_get_total_frames();
	in.laggedFrames = lagged - prevLagged;
	in.totalFrames = total - prevTotal;

	bool diskChecked = false;
	if (lastDiskCheck == 0 || now - lastDiskCheck >= kDiskCheckIntervalNs) {
		diskChecked = true;
		if (!folder.empty()) {
			std::error_code ec;
			auto space = std::filesystem::space(folder, ec);
			diskFree = ec ? -1 : (int64_t)space.available;
		}
	}
	in.diskFreeBytes = diskFree;

	const uint64_t rss = os_get_proc_resident_size();
	in.rssBytes = rss > 0 ? (int64_t)rss : -1;

	std::vector<health::Finding> fresh = health::runtime(in);

	// --- publish ---------------------------------------------------------
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!recording_)
			return; // STOP won the race: these findings are history
		lastLagged_ = lagged;
		lastTotal_ = total;
		if (diskChecked) {
			lastDiskCheckNs_ = now;
			diskFreeBytes_ = diskFree;
		}
		logDeltaLocked(fresh);
		findings_ = std::move(fresh);
	}
	samples_.fetch_add(1);
}

void HealthMonitor::logDeltaLocked(const std::vector<health::Finding> &now)
{
	// One line when something appears, one when it clears. A finding that
	// re-prints every second is a finding nobody reads by the third minute.
	std::vector<std::string> ids;
	ids.reserve(now.size());
	for (const auto &f : now) {
		ids.push_back(f.id);
		const bool wasThere = std::find(lastIds_.begin(), lastIds_.end(),
						f.id) != lastIds_.end();
		if (wasThere)
			continue;
		raised_.fetch_add(1);
		obs_log(f.level >= health::Level::Blocker ? LOG_ERROR : LOG_WARNING,
			"[health] %s: %s%s%s", health::levelName(f.level),
			f.id.c_str(), f.detail.empty() ? "" : " - ",
			f.detail.c_str());
	}
	for (const auto &old : lastIds_)
		if (std::find(ids.begin(), ids.end(), old) == ids.end())
			obs_log(LOG_INFO, "[health] cleared: %s", old.c_str());
	lastIds_ = std::move(ids);
}

std::vector<health::Finding> HealthMonitor::findings() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return findings_;
}

health::Level HealthMonitor::worst() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return health::worstOf(findings_);
}

// ---------------------------------------------------------------------------
// Disk bandwidth
// ---------------------------------------------------------------------------

void HealthMonitor::probeDiskAsync(const std::string &folder)
{
	if (folder.empty())
		return;
	// Never during a take: the point of the measurement is to protect the
	// recording, not to compete with it for the same disk.
	if (ReplayCore::instance().isRecording())
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (probedFolder_ == folder && diskWriteBps_.load() > 0)
			return; // already known for this folder
	}
	if (probeRunning_.exchange(true))
		return;

	std::thread([this, folder]() {
		int64_t bps = -1;
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		const std::filesystem::path path =
			std::filesystem::path(folder) / kProbeFileName;
		FILE *f = os_fopen(path.string().c_str(), "wb");
		if (f) {
			std::vector<char> chunk(kProbeChunk, 0x5A);
			const auto t0 = std::chrono::steady_clock::now();
			int64_t written = 0;
			bool ok = true;
			while (written < kProbeBytes && ok) {
				ok = fwrite(chunk.data(), 1, chunk.size(), f) ==
				     chunk.size();
				written += (int64_t)chunk.size();
			}
			if (ok)
				flushToDevice(f);
			const auto t1 = std::chrono::steady_clock::now();
			fclose(f);
			std::filesystem::remove(path, ec);
			const double secs =
				std::chrono::duration<double>(t1 - t0).count();
			if (ok && secs > 0.0)
				bps = (int64_t)((double)written / secs);
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			probedFolder_ = folder;
		}
		diskWriteBps_.store(bps);
		obs_log(LOG_INFO, "[health] disk write probe on '%s': %s",
			folder.c_str(),
			bps > 0 ? health::detail::mbps(bps).c_str() : "failed");
		probeRunning_.store(false);
	}).detach();
}

std::string HealthMonitor::probedFolder() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return probedFolder_;
}

} // namespace multireplay
