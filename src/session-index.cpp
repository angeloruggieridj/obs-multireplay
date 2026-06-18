/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "session-index.hpp"
#include "timeline-math.hpp"

// obs-module.h must come before plugin-support.h (MSVC blogva linkage).
#include <obs-module.h>
#include "plugin-support.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace multireplay {

namespace fs = std::filesystem;

int64_t probeDurationNs(const std::string &path)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return -1;
	int64_t durationNs = -1;
	if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0)
		durationNs = fmt->duration * 1000; // AV_TIME_BASE (µs) → ns
	avformat_close_input(&fmt);
	return durationNs;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

bool SessionIndex::loadManifestLocked(const std::string &mPath)
{
	obs_data_t *manifest =
		obs_data_create_from_json_file(mPath.c_str());
	if (!manifest)
		return false;

	// baseNs: cumulative master-timeline offset written by new code.
	// Defaults to 0 for legacy session.json files.
	int64_t baseNs = obs_data_get_int(manifest, "baseNs");
	int64_t wallStartSec = obs_data_get_int(manifest, "createdWallClock");

	std::array<int64_t, kIndexMaxCameras> startTs{};
	std::array<bool, kIndexMaxCameras> present{};
	int64_t minStart = INT64_MAX;

	obs_data_array_t *cams = obs_data_get_array(manifest, "cameras");
	if (cams) {
		size_t count = obs_data_array_count(cams);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(cams, i);
			int cam = (int)obs_data_get_int(item, "camera") - 1;
			if (cam >= 0 && cam < kIndexMaxCameras) {
				startTs[cam] = obs_data_get_int(
					item, "startTimestampNs");
				present[cam] = true;
				minStart = std::min(minStart, startTs[cam]);
			}
			obs_data_release(item);
		}
		obs_data_array_release(cams);
	}
	obs_data_release(manifest);

	if (minStart == INT64_MAX) {
		obs_log(LOG_WARNING, "SessionIndex: empty manifest %s",
			mPath.c_str());
		return false;
	}

	SessionInfo si;
	si.sessionId = (int)sessions_.size() + 1;
	si.baseNs = baseNs;
	sessions_.push_back(si);

	bool anyValid = false;
	for (int i = 0; i < kIndexMaxCameras; i++) {
		if (!present[i])
			continue;
		tracks_[i].inManifest = true;
		// Camera's absolute start on the cumulative master timeline:
		// project base + inter-session-camera jitter.
		int64_t camBaseNs = baseNs + (startTs[i] - minStart);
		appendCameraSession(tracks_[i], folder_, wallStartSec,
				    camBaseNs);
		if (tracks_[i].valid)
			anyValid = true;
	}
	return anyValid;
}

void SessionIndex::appendCameraSession(CameraTrack &track,
				       const std::string &folder,
				       int64_t wallStartSec, int64_t baseNs)
{
	std::string prefix = "cam" + std::to_string(track.index + 1) + "_";
	constexpr size_t kDateLen = 19; // "YYYY-MM-DD_HH-MM-SS"
	constexpr int64_t kGraceSec = 30;

	std::vector<std::string> files;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(folder, ec)) {
		if (!entry.is_regular_file(ec))
			continue;
		std::string name = entry.path().filename().string();
		const std::string ext = entry.path().extension().string();
		if (name.rfind(prefix, 0) != 0)
			continue;
		if (ext != ".mp4" && ext != ".mov")
			continue;

		// Filter by filename date: skip files that predate this session.
		if (wallStartSec > 0 &&
		    name.length() > prefix.length() + kDateLen) {
			std::string dateStr =
				name.substr(prefix.length(), kDateLen);
			int yr, mo, dy, hh, mm, ss;
			if (std::sscanf(dateStr.c_str(),
					"%4d-%2d-%2d_%2d-%2d-%2d", &yr, &mo,
					&dy, &hh, &mm, &ss) == 6) {
				struct tm t = {};
				t.tm_year = yr - 1900;
				t.tm_mon = mo - 1;
				t.tm_mday = dy;
				t.tm_hour = hh;
				t.tm_min = mm;
				t.tm_sec = ss;
				t.tm_isdst = -1;
				int64_t fileTime = (int64_t)::mktime(&t);
				if (fileTime > 0 &&
				    fileTime < wallStartSec - kGraceSec) {
					obs_log(LOG_DEBUG,
						"SessionIndex: skip stale %s "
						"(predates session by %lld s)",
						name.c_str(),
						(long long)(wallStartSec -
							    fileTime));
					continue;
				}
			}
		}

		// Skip files already included by an earlier session's append.
		bool dup = false;
		for (const auto &seg : track.segments)
			if (seg.path == entry.path().string()) {
				dup = true;
				break;
			}
		if (dup)
			continue;

		files.push_back(entry.path().string());
	}
	std::sort(files.begin(), files.end());

	// Probe and build segments for this session.
	// localElapsed: duration accumulated within THIS session so far.
	int64_t localElapsed = 0;
	for (const auto &path : files) {
		int64_t dur = probeDurationNs(path);
		if (dur <= 0)
			continue; // growing/unfinished file: skip for now
		Segment seg;
		seg.path = path;
		seg.localStartNs = baseNs + localElapsed;
		seg.durationNs = dur;
		track.segments.push_back(std::move(seg));
		localElapsed += dur;
	}

	// Update totalDurationNs = end of the latest segment on master timeline.
	track.totalDurationNs = 0;
	for (const auto &seg : track.segments)
		track.totalDurationNs = std::max(
			track.totalDurationNs,
			seg.localStartNs + seg.durationNs);
	track.valid = !track.segments.empty();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool SessionIndex::load(const std::string &folder)
{
	std::lock_guard<std::mutex> lock(mutex_);
	folder_ = folder;
	sessions_.clear();

	// Reset all tracks.
	for (int i = 0; i < kIndexMaxCameras; i++) {
		tracks_[i] = CameraTrack{};
		tracks_[i].index = i;
	}

	// Discover session manifests.
	// Numbered session_NNN.json files (N >= 1) are the new format.
	// The legacy session.json is treated as sort-key 0 and only used
	// when no numbered manifests exist (backward compatibility).
	std::vector<std::pair<int, std::string>> manifests;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(folder, ec)) {
		if (!entry.is_regular_file(ec))
			continue;
		std::string name = entry.path().filename().string();
		if (name == "session.json") {
			manifests.push_back({0, entry.path().string()});
		} else {
			int n = 0;
			if (std::sscanf(name.c_str(), "session_%d.json", &n) ==
				    1 &&
			    n > 0)
				manifests.push_back({n, entry.path().string()});
		}
	}

	if (manifests.empty()) {
		obs_log(LOG_DEBUG,
			"SessionIndex: no session manifest in %s "
			"(new or empty project folder)",
			folder.c_str());
		return false;
	}

	// Sort: legacy session.json (key 0) first, then numbered in order.
	// If numbered sessions exist, remove the legacy session.json to avoid
	// double-loading its footage under the new multi-session scheme.
	bool hasNumbered = false;
	for (const auto &[n, _] : manifests)
		if (n > 0) {
			hasNumbered = true;
			break;
		}
	if (hasNumbered) {
		manifests.erase(std::remove_if(manifests.begin(),
					       manifests.end(),
					       [](const auto &m) {
						       return m.first == 0;
					       }),
				manifests.end());
	}
	std::sort(manifests.begin(), manifests.end());

	for (const auto &[n, mPath] : manifests)
		loadManifestLocked(mPath);

	int validCount = 0;
	for (const auto &t : tracks_)
		if (t.valid)
			validCount++;

	obs_log(LOG_INFO,
		"SessionIndex: %d session(s), %d camera track(s) in %s",
		(int)sessions_.size(), validCount, folder.c_str());
	return validCount > 0;
}

void SessionIndex::refresh()
{
	// Snapshot the folder so we can release the lock before probing.
	std::string folder;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		folder = folder_;
	}
	if (folder.empty())
		return;

	// Rebuild the index on a FRESH SessionIndex (uses its own mutex, not ours).
	// This means the monitor-loop's resolve() calls are never blocked by FFmpeg
	// I/O during refresh — the shared mutex is only locked for the brief swap
	// at the end.
	SessionIndex fresh;
	fresh.load(folder);

	// Apply only if fresh data is non-empty (guards against transient
	// probe failures mid-recording when files aren't yet finalized).
	if (fresh.masterDurationNs() > 0) {
		std::lock_guard<std::mutex> lock(mutex_);
		tracks_ = fresh.tracks_;
		sessions_ = fresh.sessions_;
	}
}

int64_t SessionIndex::masterDurationNs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t maxEnd = 0;
	for (const auto &track : tracks_) {
		if (!track.valid)
			continue;
		// startOffsetNs is 0 (unused); totalDurationNs is the end
		// of the last segment on the cumulative master timeline.
		maxEnd = std::max(maxEnd,
				  track.startOffsetNs + track.totalDurationNs);
	}
	return maxEnd;
}

bool SessionIndex::resolve(int camIndex, int64_t masterNs,
			   std::string &pathOut, int64_t &offsetNsOut) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (camIndex < 0 || camIndex >= kIndexMaxCameras)
		return false;
	const CameraTrack &track = tracks_[camIndex];
	if (!track.valid)
		return false;

	// seg.localStartNs carries the ABSOLUTE master-timeline position. Map via
	// the shared pure helper (also unit-tested) → segment index + offset.
	std::vector<SegmentSpan> spans;
	spans.reserve(track.segments.size());
	for (const auto &seg : track.segments)
		spans.push_back({seg.localStartNs, seg.durationNs});

	size_t idx = 0;
	int64_t off = 0;
	if (!resolveSpan(spans, masterNs, track.totalDurationNs, idx, off))
		return false;
	pathOut = track.segments[idx].path;
	offsetNsOut = off;
	return true;
}

bool SessionIndex::cameraValid(int camIndex) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return camIndex >= 0 && camIndex < kIndexMaxCameras &&
	       tracks_[camIndex].valid;
}

std::string SessionIndex::folder() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return folder_;
}

std::vector<SessionInfo> SessionIndex::sessionInfos() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return sessions_;
}

} // namespace multireplay
