/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "session-index.hpp"

// obs-module.h must come before plugin-support.h (MSVC blogva linkage).
#include <obs-module.h>
#include "plugin-support.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
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
	if (avformat_find_stream_info(fmt, nullptr) >= 0 &&
	    fmt->duration > 0) {
		// fmt->duration is in AV_TIME_BASE (µs) units
		durationNs = fmt->duration * 1000;
	}
	avformat_close_input(&fmt);
	return durationNs;
}

bool SessionIndex::load(const std::string &folder)
{
	std::lock_guard<std::mutex> lock(mutex_);
	folder_ = folder;

	// --- session.json: per-camera start timestamps (monotonic ns) ---
	fs::path manifestPath = fs::path(folder) / "session.json";
	obs_data_t *manifest =
		obs_data_create_from_json_file(manifestPath.string().c_str());
	if (!manifest) {
		obs_log(LOG_WARNING, "SessionIndex: no session.json in %s",
			folder.c_str());
		return false;
	}

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
		obs_log(LOG_WARNING, "SessionIndex: empty session manifest");
		return false;
	}

	int validCount = 0;
	for (int i = 0; i < kIndexMaxCameras; i++) {
		tracks_[i] = CameraTrack{};
		tracks_[i].index = i;
		if (!present[i])
			continue;
		tracks_[i].startOffsetNs = startTs[i] - minStart;
		scanCamera(tracks_[i]);
		if (tracks_[i].valid)
			validCount++;
	}

	obs_log(LOG_INFO, "SessionIndex: %d camera track(s) indexed in %s",
		validCount, folder.c_str());
	return validCount > 0;
}

void SessionIndex::scanCamera(CameraTrack &track)
{
	// Files are named cam{N}_<timestamp>.mp4 (and .mp4 split suffixes);
	// lexicographic order == chronological order with our name format.
	std::string prefix = "cam" + std::to_string(track.index + 1) + "_";

	std::vector<std::string> files;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(folder_, ec)) {
		if (!entry.is_regular_file())
			continue;
		std::string name = entry.path().filename().string();
		if (name.rfind(prefix, 0) == 0 &&
		    (entry.path().extension() == ".mp4" ||
		     entry.path().extension() == ".mov"))
			files.push_back(entry.path().string());
	}
	std::sort(files.begin(), files.end());

	track.segments.clear();
	track.totalDurationNs = 0;
	for (const auto &path : files) {
		int64_t dur = probeDurationNs(path);
		if (dur <= 0)
			continue; // growing/corrupt file: skip for now
		Segment seg;
		seg.path = path;
		seg.localStartNs = track.totalDurationNs;
		seg.durationNs = dur;
		track.segments.push_back(std::move(seg));
		track.totalDurationNs += dur;
	}
	track.valid = !track.segments.empty();
}

void SessionIndex::refresh()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &track : tracks_) {
		if (track.startOffsetNs == 0 && track.segments.empty() &&
		    !track.valid)
			continue;
		scanCamera(track);
	}
}

int64_t SessionIndex::masterDurationNs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t maxEnd = 0;
	for (const auto &track : tracks_) {
		if (!track.valid)
			continue;
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

	int64_t localNs = masterNs - track.startOffsetNs;
	if (localNs < 0)
		localNs = 0;
	if (localNs >= track.totalDurationNs)
		localNs = track.totalDurationNs - 1;

	for (const auto &seg : track.segments) {
		if (localNs < seg.localStartNs + seg.durationNs) {
			pathOut = seg.path;
			offsetNsOut = localNs - seg.localStartNs;
			return true;
		}
	}
	return false;
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

} // namespace multireplay
