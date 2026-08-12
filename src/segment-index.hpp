/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

SegmentIndex — knows which Branch Output file holds a given instant, and where
inside it.

The live ring covers the last minutes; everything older lives in the files
Branch Output is already writing, and replay has to reach all of it. This is the
map between the two.

Each file is anchored once, the moment it appears, by matching its opening
packets against the live ring (see segment-anchor.hpp). After that a lookup is
arithmetic, not a search:

    fileTimeNs = masterNs - anchorMasterNs

Two clocks, on purpose. `masterNs` is monotonic system time: it is what markers
and the ring use, it is exact, and it is meaningless the moment OBS restarts.
So every anchor is also stored as an absolute wall-clock instant, which is what
lets a project opened tomorrow still resolve yesterday's footage. Within a
session the monotonic value is authoritative; across sessions the wall value is
all there is.
*/

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multireplay {

constexpr int kMaxSegmentCameras = 8;

struct RecordingSegment {
	std::string path;
	// Master time of this file's first video frame. Valid this session only.
	int64_t anchorMasterNs = 0;
	// The same instant in absolute wall-clock nanoseconds since the Unix
	// epoch, so the file is still resolvable after OBS has restarted.
	int64_t anchorWallNs = 0;
	// Where the file stops covering the timeline: the next segment's anchor,
	// or 0 while this is the newest one and still growing.
	int64_t endMasterNs = 0;
	bool anchored = false;
};

class SegmentIndex {
public:
	static SegmentIndex &instance();

	// Begin watching `folder` for the cameras marked in `cams`. The epoch
	// pair ties this session's monotonic clock to wall-clock time.
	void start(const std::string &folder,
		   const std::array<bool, kMaxSegmentCameras> &cams,
		   int64_t epochMasterNs, int64_t epochWallNs);
	void stop();

	// Which file covers `masterNs` on this camera, and how far into it.
	// False when no anchored segment covers that instant — the caller then
	// has nothing to play, which is the honest answer.
	bool resolve(int camIndex, int64_t masterNs, std::string &pathOut,
		     int64_t &fileTimeNsOut) const;

	// Oldest instant available on disk for a camera, 0 if none anchored.
	int64_t oldestNs(int camIndex) const;

	std::vector<RecordingSegment> segments(int camIndex) const;
	int anchoredCount() const;
	// Files seen but not anchorable (their opening had already left the
	// ring, or the content was too uniform to match). Worth surfacing:
	// footage exists that we deliberately refuse to guess the position of.
	int unanchoredCount() const;

private:
	SegmentIndex() = default;
	~SegmentIndex();
	SegmentIndex(const SegmentIndex &) = delete;
	SegmentIndex &operator=(const SegmentIndex &) = delete;

	void watchLoop();
	void scanFolder();          // pick up new files
	void tryAnchorPending();    // match openings against the ring
	void recomputeBoundaries(); // segment end = next segment's anchor
	void save() const;          // anchors.json, wall-clock based
	void load();

	mutable std::mutex mutex_;
	std::string folder_;
	std::array<bool, kMaxSegmentCameras> cams_{};
	std::array<std::vector<RecordingSegment>, kMaxSegmentCameras> segments_{};
	// Files still waiting for a successful anchor, with their attempt count.
	std::array<std::vector<std::pair<std::string, int>>, kMaxSegmentCameras>
		pending_{};

	int64_t epochMasterNs_ = 0;
	int64_t epochWallNs_ = 0;

	std::thread watcher_;
	std::atomic<bool> running_{false};
	std::condition_variable wake_;
};

} // namespace multireplay
