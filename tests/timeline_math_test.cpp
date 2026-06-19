/*
obs-multireplay — unit smoke tests for the pure timeline math.
SPDX-License-Identifier: GPL-2.0-or-later

Standalone (no OBS/FFmpeg): covers the two bug-prone bits found during bring-up:
  - master→file resolution (clamp + segment pick), and
  - the wall-clock OUT recalibration (must not shorten on a stale/forward read).
Build via the obs-multireplay-tests target; run with ctest.
*/

#include "timeline-math.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

using namespace multireplay;

static int g_fail = 0;

#define CHECK(cond)                                                      \
	do {                                                             \
		if (!(cond)) {                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, \
				    #cond);                            \
			++g_fail;                                      \
		}                                                      \
	} while (0)

static void test_resolve_single_segment()
{
	// One 10s segment starting at master 0.
	std::vector<SegmentSpan> segs = {{0, 10000000000LL}};
	int64_t total = 10000000000LL;
	size_t idx = 99;
	int64_t off = -1;

	CHECK(resolveSpan(segs, 4000000000LL, total, idx, off));
	CHECK(idx == 0 && off == 4000000000LL); // 4s in → offset 4s

	// Negative clamps to 0.
	CHECK(resolveSpan(segs, -5, total, idx, off));
	CHECK(idx == 0 && off == 0);

	// Past the end clamps to total-1 (last valid frame), not failure.
	CHECK(resolveSpan(segs, 999000000000LL, total, idx, off));
	CHECK(idx == 0 && off == total - 1);
}

static void test_resolve_multi_segment_and_gap()
{
	// Two segments: [0,5s) and [8s,8s+5s) — a 3s gap between them.
	std::vector<SegmentSpan> segs = {{0, 5000000000LL},
					 {8000000000LL, 5000000000LL}};
	int64_t total = 13000000000LL;
	size_t idx = 99;
	int64_t off = -1;

	// Inside segment 0.
	CHECK(resolveSpan(segs, 2000000000LL, total, idx, off));
	CHECK(idx == 0 && off == 2000000000LL);

	// Inside segment 1 → offset relative to its start.
	CHECK(resolveSpan(segs, 9000000000LL, total, idx, off));
	CHECK(idx == 1 && off == 1000000000LL);

	// In the gap (6s): before segment 1 → clamps to segment 1 start (off 0).
	CHECK(resolveSpan(segs, 6000000000LL, total, idx, off));
	CHECK(idx == 1 && off == 0);

	// Empty → false.
	std::vector<SegmentSpan> empty;
	CHECK(!resolveSpan(empty, 0, 0, idx, off));
}

static void test_resolve_total_past_last_segment()
{
	// Robustness: when totalDurationNs is LARGER than the last segment's end
	// (a trailing inter-session gap, or a sum-of-durations total), a query
	// that lands past the last segment must clamp to its last valid frame —
	// NOT return false. (Production always passes total == last-seg-end, so
	// this guards the contract against a future caller getting it wrong.)
	std::vector<SegmentSpan> segs = {{0, 5000000000LL},
					 {8000000000LL, 5000000000LL}};
	size_t idx = 99;
	int64_t off = -1;

	// total claims 20s but footage ends at 13s; ask for 18s.
	CHECK(resolveSpan(segs, 18000000000LL, 20000000000LL, idx, off));
	CHECK(idx == 1 && off == 5000000000LL - 1); // last frame of segment 1

	// Same with a single segment and an oversized total.
	std::vector<SegmentSpan> one = {{0, 4000000000LL}};
	CHECK(resolveSpan(one, 9000000000LL, 10000000000LL, idx, off));
	CHECK(idx == 0 && off == 4000000000LL - 1);

	// A zero-length last segment clamps the offset to 0, not -1.
	std::vector<SegmentSpan> zero = {{0, 0}};
	CHECK(resolveSpan(zero, 100, 50, idx, off));
	CHECK(idx == 0 && off == 0);
}

static void test_recalc_out_duration()
{
	const int64_t MS = 1000000LL;
	int64_t outNs = 20000 * MS; // OUT at 20s master
	int64_t segBase = 0;        // file offset 0 == master 0
	int64_t nominal = 5000 * MS; // OUT-IN = 5s

	// Landed exactly at the requested IN (15000ms) → duration unchanged (5s).
	CHECK(recalcEventDurationNs(outNs, segBase, 15000, 15000, nominal) ==
	      5000 * MS);

	// Landed 500ms before IN (keyframe) → extend to reach OUT (5.5s).
	CHECK(recalcEventDurationNs(outNs, segBase, 15000, 14500, nominal) ==
	      5500 * MS);

	// Stale/forward read far past IN → keep nominal (must NOT shorten to ~0).
	CHECK(recalcEventDurationNs(outNs, segBase, 15000, 22680, nominal) ==
	      nominal);

	// Stale 0 read → keep nominal.
	CHECK(recalcEventDurationNs(outNs, segBase, 15000, 0, nominal) ==
	      nominal);

	// Landed way before IN (> one GOP, e.g. 2400ms) → keep nominal.
	CHECK(recalcEventDurationNs(outNs, segBase, 15000, 2400, nominal) ==
	      nominal);
}

int main()
{
	test_resolve_single_segment();
	test_resolve_multi_segment_and_gap();
	test_resolve_total_past_last_segment();
	test_recalc_out_duration();
	if (g_fail == 0)
		std::printf("OK: all timeline-math smoke tests passed\n");
	return g_fail == 0 ? 0 : 1;
}
