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
		obs_log(LOG_DEBUG, "SessionIndex: no session.json in %s "
				   "(new or empty project folder)",
			folder.c_str());
		return false;
	}

	std::array<int64_t, kIndexMaxCameras> startTs{};
	std::array<bool, kIndexMaxCameras> present{};
	int64_t minStart = INT64_MAX;

	// Read wallStartSec BEFORE releasing manifest — was previously read after
	// obs_data_release() (use-after-free), returning 0 and silently disabling
	// the old-file filter in scanCamera() so stale cam*.mp4 files from prior
	// sessions were included, inflating the timeline on the next REC press.
	int64_t wallStartSec = obs_data_get_int(manifest, "createdWallClock");

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
		tracks_[i].inManifest = true;
		tracks_[i].startOffsetNs = startTs[i] - minStart;
		scanCamera(tracks_[i], wallStartSec);
		if (tracks_[i].valid)
			validCount++;
	}

	obs_log(LOG_INFO, "SessionIndex: %d camera track(s) indexed in %s",
		validCount, folder.c_str());
	return validCount > 0;
}

void SessionIndex::scanCamera(CameraTrack &track, int64_t wallStartSec)
{
	// Files are named cam{N}_%CCYY-%MM-%DD_%hh-%mm-%ss[...].mp4
	// Lexicographic order == chronological order with this naming scheme.
	std::string prefix = "cam" + std::to_string(track.index + 1) + "_";

	// When wallStartSec > 0, skip any file whose filename date is more
	// than 30 s before the session start — those belong to an older
	// recording run that was not cleaned up with Delete All.
	// Filename format (after prefix): YYYY-MM-DD_HH-MM-SS[...].ext
	// We parse only the first 19 characters of the date component.
	constexpr size_t kDateLen = 19; // "YYYY-MM-DD_HH-MM-SS"
	constexpr int64_t kGraceSec = 30;

	std::vector<std::string> files;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(folder_, ec)) {
		if (!entry.is_regular_file())
			continue;
		std::string name = entry.path().filename().string();
		const std::string ext = entry.path().extension().string();
		if (name.rfind(prefix, 0) != 0)
			continue;
		if (ext != ".mp4" && ext != ".mov")
			continue;

		// Filter by filename date when wallStartSec is known.
		if (wallStartSec > 0 &&
		    name.length() > prefix.length() + kDateLen) {
			std::string dateStr =
				name.substr(prefix.length(), kDateLen);
			int yr, mo, dy, hh, mm, ss;
			if (std::sscanf(dateStr.c_str(),
					"%4d-%2d-%2d_%2d-%2d-%2d",
					&yr, &mo, &dy, &hh, &mm, &ss) == 6) {
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
						"SessionIndex: skipping stale "
						"file %s (predates session "
						"by %lld s)",
						name.c_str(),
						(long long)(wallStartSec -
							    fileTime));
					continue; // belongs to an older session
				}
			}
		}

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
		if (!track.inManifest)
			continue;
		// Pass wallStartSec=0 on refresh: the initial load already
		// filtered stale files; refresh only re-probes known-good tracks.
		scanCamera(track, 0);
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
