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

#include "session-clock.hpp" // kNoInstant, and why an instant may be negative

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

// kNoInstant lives in session-clock.hpp, with the note explaining why a master
// instant may be negative and why that is not an error. Every "is there one?"
// in this file is a comparison against it, never against 0.

struct RecordingSegment {
	std::string path;
	// Master time of this file's first video frame. Valid this session only.
	int64_t anchorMasterNs = 0;
	// The same instant in absolute wall-clock nanoseconds since the Unix
	// epoch, so the file is still resolvable after OBS has restarted.
	int64_t anchorWallNs = 0;
	// Where the file stops covering the timeline: the next segment's anchor,
	// or kNoInstant while this is the newest one and still growing.
	int64_t endMasterNs = kNoInstant;
	// The file's REAL duration, demuxed from its own timestamps, 0 = not
	// measured yet or not readable from the file at all.
	//
	// Deliberately NOT folded into endMasterNs. That field is what resolve()
	// refuses instants past, and a duration measured while Branch Output is
	// still appending to the file is stale by construction — it would cut the
	// live end off the newest segment. This one only answers "how far does the
	// footage on disk reach", which is what a timeline needs and a lookup
	// does not.
	int64_t durationNs = 0;
	// True when the measurement was taken with nothing writing to the file,
	// i.e. it is not going to grow any further.
	bool durationFinal = false;
	// Failed measurement attempts. A file whose end we cannot read is stated
	// as unknown, never estimated - so it must also stop being re-demuxed.
	int durationProbes = 0;
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

	// Oldest instant available on disk for a camera, kNoInstant if none is
	// anchored. Read the note on kNoInstant before comparing this against 0.
	int64_t oldestNs(int camIndex) const;

	// Newest instant the footage on disk reaches for a camera, kNoInstant
	// when no segment's end is known.
	//
	// This is what makes a reopened project navigable. The timeline used to
	// be measured from the anchors to the LIVE edge, and outside a take the
	// live edge is 0 — so a project with hours of footage on disk drew a bar
	// of length zero and told the operator there was nothing to scrub. The
	// end of the last file is not something the anchors can say (an anchor is
	// a beginning), so it is demuxed from the file's own timestamps; a file
	// that does not say how long it is stays out of this answer instead of
	// being estimated into it.
	int64_t newestNs(int camIndex) const;
	// The same across ALL cameras: where the project's footage ends,
	// kNoInstant when no end is known anywhere.
	int64_t projectEndNs() const;

	// Oldest anchored instant across ALL cameras, kNoInstant if nothing is
	// anchored.
	// This is where the project's footage begins, and it is the only origin an
	// event timecode may be measured from: a mark is a property of the project,
	// not of the angle the operator happens to be looking at, so it must not
	// renumber itself when he presses another camera button - nor collapse to
	// raw monotonic time (a five-digit minute count) when the selected angle
	// has nothing anchored.
	int64_t projectOriginNs() const;

	// Is that instant inside an anchored segment on any camera? "Can this mark
	// still be played from disk" is a question the event list has to answer for
	// footage of a session that is over, and answering it honestly is better
	// than offering a row that does nothing when clicked.
	bool coversAnyCamera(int64_t masterNs) const;

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
	// Demux the real length of ONE anchored file per pass (see newestNs).
	// One at a time on purpose: this is the same disk I/O the anchoring pass
	// is deliberately rationed for, and it must not show up as latency on the
	// live path.
	void refreshDurations();
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
