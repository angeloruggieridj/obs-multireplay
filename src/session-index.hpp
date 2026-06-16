/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace multireplay {

constexpr int kIndexMaxCameras = 8; // M5: full reference parity

// One recorded file (Branch Output splits every N minutes).
struct Segment {
	std::string path;
	int64_t localStartNs = 0; // start on the camera-local timeline
	int64_t durationNs = 0;
};

// All segments of one camera, positioned on the master timeline.
struct CameraTrack {
	int index = 0;            // 0-based camera index
	bool inManifest = false;  // camera was present in session.json
	bool valid = false;
	int64_t startOffsetNs = 0; // camera start relative to master t=0
	std::vector<Segment> segments;
	int64_t totalDurationNs = 0;
};

// Maps the master timeline (t=0 = earliest camera start of the session)
// onto per-camera files/offsets. Built from session.json + folder scan;
// durations probed with libavformat.
class SessionIndex {
public:
	// Scan `folder` (session.json + camN_*.mp4). Returns false when the
	// folder has no usable session.
	bool load(const std::string &folder);

	// Re-probe the last segment of each camera (files still growing while
	// recording) and pick up newly split files.
	void refresh();

	// Master timeline duration (max camera end).
	int64_t masterDurationNs() const;

	// Resolve master time -> (file, offset inside file) for a camera.
	bool resolve(int camIndex, int64_t masterNs, std::string &pathOut,
		     int64_t &offsetNsOut) const;

	bool cameraValid(int camIndex) const;
	std::string folder() const;

private:
	// `wallStartSec` = createdWallClock from session.json (Unix time, sec).
	// Files whose filename timestamp predates this by >30 s are skipped —
	// they belong to an older recording run in the same session folder.
	void scanCamera(CameraTrack &track, int64_t wallStartSec);

	mutable std::mutex mutex_;
	std::string folder_;
	std::array<CameraTrack, kIndexMaxCameras> tracks_{};
};

// Probe a media file duration in ns via libavformat (-1 on failure).
int64_t probeDurationNs(const std::string &path);

} // namespace multireplay
