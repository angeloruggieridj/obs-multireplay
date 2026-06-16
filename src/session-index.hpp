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
// localStartNs is the ABSOLUTE position on the master cumulative timeline
// (includes per-session baseNs + per-camera inter-start jitter).
struct Segment {
	std::string path;
	int64_t localStartNs = 0;
	int64_t durationNs = 0;
};

// All segments of one camera, positioned on the master timeline.
struct CameraTrack {
	int index = 0;           // 0-based camera index
	bool inManifest = false; // camera present in at least one session manifest
	bool valid = false;
	int64_t startOffsetNs = 0; // unused — offset now embedded in seg.localStartNs
	std::vector<Segment> segments;
	int64_t totalDurationNs = 0; // max(seg.localStartNs + seg.durationNs)
};

// Describes one recording session within a project (load order).
struct SessionInfo {
	int sessionId; // 1-based
	int64_t baseNs; // cumulative master-timeline start of this session
};

// Maps the master timeline (t=0 = earliest camera start of the earliest session)
// onto per-camera files/offsets across ALL recording sessions within a project.
// Multiple sessions are accumulated from session_NNN.json manifests plus the
// legacy session.json (treated as session 0 when no numbered manifests exist).
class SessionIndex {
public:
	// Scan `folder` for all session manifests + camera files.
	// Returns false when the folder has no usable footage.
	bool load(const std::string &folder);

	// Rebuild the index from disk on a fresh object and swap results in.
	// Non-blocking: the shared mutex is held only briefly at start/end;
	// expensive FFmpeg probes run without blocking resolve() callers.
	void refresh();

	// Master timeline duration across ALL sessions (max camera end).
	int64_t masterDurationNs() const;

	// Resolve cumulative master time -> (file, ns-offset within file).
	bool resolve(int camIndex, int64_t masterNs, std::string &pathOut,
		     int64_t &offsetNsOut) const;

	bool cameraValid(int camIndex) const;
	std::string folder() const;
	std::vector<SessionInfo> sessionInfos() const;

private:
	// Load one session manifest file. mutex_ must be held.
	bool loadManifestLocked(const std::string &mPath);

	// Scan `folder` for camera files belonging to one session and append
	// segments to `track`. `baseNs` = absolute master-timeline position of
	// this camera's first frame. mutex_ must be held.
	void appendCameraSession(CameraTrack &track, const std::string &folder,
				 int64_t wallStartSec, int64_t baseNs);

	mutable std::mutex mutex_;
	std::string folder_;
	std::array<CameraTrack, kIndexMaxCameras> tracks_{};
	std::vector<SessionInfo> sessions_;
};

// Probe a media file duration in ns via libavformat (-1 on failure).
int64_t probeDurationNs(const std::string &path);

} // namespace multireplay
