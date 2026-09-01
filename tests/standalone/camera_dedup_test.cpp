/*
obs-multireplay — unit tests for camera-dedup.hpp: which camera slot owns
the Branch Output filter/encoder when several slots name the same source.
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, because this is a decision about an array of strings and
the bug it closes (eight identical Branch Output filters on one source,
racing for a limited number of hardware encode sessions) never has to touch
libobs to be gotten right or wrong.
*/

#include "camera-dedup.hpp"

#include <cstdio>
#include <string>

using namespace multireplay;

static int g_fail = 0;

#define CHECK(cond)                                                         \
	do {                                                                \
		if (!(cond)) {                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, \
				    #cond);                                 \
			++g_fail;                                           \
		}                                                           \
	} while (0)

static void test_all_distinct_sources_are_all_canonical()
{
	std::array<std::string, 4> names{"C1", "C2", "C3", "C4"};
	const auto canonical = canonicalCameraIndices(names);
	for (std::size_t i = 0; i < names.size(); i++) {
		CHECK(canonical[i] == (int)i);
		CHECK(isCanonicalCamera(canonical, i));
	}
}

static void test_unconfigured_slots_map_to_themselves()
{
	std::array<std::string, 4> names{"", "", "", ""};
	const auto canonical = canonicalCameraIndices(names);
	for (std::size_t i = 0; i < names.size(); i++) {
		CHECK(canonical[i] == (int)i);
		// Unowned, but still reported as "canonical" (there is simply
		// nothing to own) — callers gate on the source name being
		// non-empty before ever asking, exactly as they already do for
		// an ordinary unconfigured slot.
		CHECK(isCanonicalCamera(canonical, i));
	}
}

// The bug report this header exists to close: eight camera slots, all
// pointed at the one source a laptop actually has plugged in.
static void test_eight_slots_one_source_collapse_to_one_owner()
{
	std::array<std::string, 8> names;
	names.fill("Media");
	const auto canonical = canonicalCameraIndices(names);
	CHECK(isCanonicalCamera(canonical, 0));
	for (std::size_t i = 1; i < names.size(); i++) {
		CHECK(canonical[i] == 0);
		CHECK(!isCanonicalCamera(canonical, i));
	}
}

// Contiguous runs of duplicates for two distinct sources, not just one.
static void test_two_groups_of_duplicates()
{
	std::array<std::string, 8> names{"Media", "Media", "Media", "",
					 "C2",    "C2",    "",     ""};
	const auto canonical = canonicalCameraIndices(names);
	const std::array<int, 8> expect{0, 0, 0, 3, 4, 4, 6, 7};
	for (std::size_t i = 0; i < names.size(); i++)
		CHECK(canonical[i] == expect[i]);
	CHECK(isCanonicalCamera(canonical, 0));
	CHECK(!isCanonicalCamera(canonical, 1));
	CHECK(!isCanonicalCamera(canonical, 2));
	CHECK(isCanonicalCamera(canonical, 3)); // empty slot, unowned
	CHECK(isCanonicalCamera(canonical, 4));
	CHECK(!isCanonicalCamera(canonical, 5));
}

// Duplicates do not have to be contiguous: the same source chosen, dropped
// for another camera, then chosen again three slots later must still trace
// back to the FIRST slot that claimed it — that is the one whose Branch
// Output filter and packet ring actually hold the bytes.
static void test_interleaved_duplicates_trace_to_the_first_claim()
{
	std::array<std::string, 4> names{"A", "B", "A", "B"};
	const auto canonical = canonicalCameraIndices(names);
	CHECK(canonical[0] == 0);
	CHECK(canonical[1] == 1);
	CHECK(canonical[2] == 0);
	CHECK(canonical[3] == 1);
}

// One camera, the ordinary case, must be untouched by any of this.
static void test_single_camera_is_its_own_owner()
{
	std::array<std::string, 1> names{"Media"};
	const auto canonical = canonicalCameraIndices(names);
	CHECK(canonical[0] == 0);
	CHECK(isCanonicalCamera(canonical, 0));
}

int main()
{
	test_all_distinct_sources_are_all_canonical();
	test_unconfigured_slots_map_to_themselves();
	test_eight_slots_one_source_collapse_to_one_owner();
	test_two_groups_of_duplicates();
	test_interleaved_duplicates_trace_to_the_first_claim();
	test_single_camera_is_its_own_owner();

	if (g_fail) {
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("camera_dedup: all checks passed\n");
	return 0;
}
