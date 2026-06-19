/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Pure timeline arithmetic shared by the engine and exercised by unit tests.
NO OBS / FFmpeg dependencies on purpose, so it can be compiled standalone in a
tiny test executable (tests/timeline_math_test.cpp) without a libobs runtime.
These are exactly the bug-prone bits found during bring-up: the master→file
offset resolution and the wall-clock OUT recalibration.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace multireplay {

// A camera track's segment placed on the cumulative master timeline.
struct SegmentSpan {
	int64_t startNs;    // absolute master-timeline position of the segment start
	int64_t durationNs; // segment length
};

// Map a master-timeline position to (segment index, offset-into-segment).
// Mirrors SessionIndex::resolve: clamp to [0, totalDurationNs-1], then pick the
// segment that contains it; a position before a segment (inter-session gap)
// clamps to that segment's start. Returns false only if there are no segments.
inline bool resolveSpan(const std::vector<SegmentSpan> &segs, int64_t masterNs,
			int64_t totalDurationNs, size_t &idxOut,
			int64_t &offsetOut)
{
	if (segs.empty())
		return false;
	int64_t localNs = masterNs < 0 ? 0 : masterNs;
	if (totalDurationNs > 0 && localNs >= totalDurationNs)
		localNs = totalDurationNs - 1;
	for (size_t i = 0; i < segs.size(); ++i) {
		const SegmentSpan &s = segs[i];
		if (localNs < s.startNs) {
			idxOut = i;
			offsetOut = 0;
			return true;
		}
		if (localNs < s.startNs + s.durationNs) {
			idxOut = i;
			offsetOut = localNs - s.startNs;
			return true;
		}
	}
	// Past the end of the last segment (only reachable if totalDurationNs is
	// larger than the last segment's end — e.g. a trailing inter-session gap,
	// or a caller passing a sum-of-durations total). Clamp to the last valid
	// frame instead of failing, honouring the [0, total-1] contract so an
	// in-range query never returns false.
	const SegmentSpan &last = segs.back();
	idxOut = segs.size() - 1;
	offsetOut = last.durationNs > 0 ? last.durationNs - 1 : 0;
	return true;
}

// Recalibrate an event's media-duration from where the seek actually landed so
// the wall-clock OUT fires at exactly the master OUT. Only trust a landed read
// that is a plausible keyframe-at-or-before the requested seek within one GOP
// (≈3 s); otherwise keep the nominal duration (a stale/forward read must not
// shorten the clip — that bug played events for ~1 s).
inline int64_t recalcEventDurationNs(int64_t eventOutNs, int64_t segBaseNs,
				     int64_t reqMs, int64_t landedMs,
				     int64_t fallbackDurNs)
{
	if (landedMs > 0 && landedMs <= reqMs && (reqMs - landedMs) < 3000) {
		int64_t masterActual = segBaseNs + landedMs * 1000000LL;
		if (eventOutNs > masterActual)
			return eventOutNs - masterActual;
	}
	return fallbackDurNs;
}

} // namespace multireplay
