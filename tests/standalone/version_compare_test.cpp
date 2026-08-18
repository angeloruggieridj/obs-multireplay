/*
obs-multireplay — unit tests for version comparison.
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, no network. This is the one part of the updater that cannot
be tested by running the plugin, because testing it for real would mean
publishing releases — so it is pure, and it is tested here.

The properties pinned:
  - a final release is newer than its own pre-releases;
  - beta10 is newer than beta2 (the text comparison that would not be);
  - 1.10.0 is newer than 1.9.0 (the same trap one level up);
  - a tag that cannot be read is never offered as an update.
*/

#include "version-compare.hpp"

#include <cstdio>

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

static void test_parsing()
{
	CHECK(parseVersion("1.0.0").valid());
	CHECK(parseVersion("1.0.0").major == 1);
	CHECK(parseVersion("2.31.4").minor == 31);
	CHECK(parseVersion("2.31.4").patch == 4);
	CHECK(parseVersion("1.0.0").pre == 0);
	CHECK(parseVersion("1.0.0-beta2").pre == 2);
	// Tagged the other way round by hand: accepted, because a release
	// published with a leading v must not become invisible to everyone.
	CHECK(parseVersion("v1.0.0").valid());
	CHECK(parseVersion("v1.0.0").major == 1);

	// ...and everything that is not a version this project publishes.
	CHECK(!parseVersion("").valid());
	CHECK(!parseVersion("1.0").valid());
	CHECK(!parseVersion("1.0.0.0").valid());
	CHECK(!parseVersion("1.0.0-rc1").valid());  // a form we do not publish
	CHECK(!parseVersion("1.0.0-beta").valid()); // beta of which?
	CHECK(!parseVersion("1.0.0-beta0").valid());
	CHECK(!parseVersion("nightly").valid());
	CHECK(!parseVersion("1.0.0-beta2-dirty").valid());
}

static void test_a_release_beats_its_own_betas()
{
	// The one that is not arithmetic, and the one that would have an updater
	// offering a stable install the beta it came from.
	CHECK(isNewerVersion("1.0.0", "1.0.0-beta2"));
	CHECK(!isNewerVersion("1.0.0-beta2", "1.0.0"));
	CHECK(!isNewerVersion("1.0.0-beta9", "1.0.0"));
}

static void test_numbers_are_numbers_not_text()
{
	// "beta10" sorts before "beta2" as text, so the tenth beta would be
	// invisible to everyone running the second.
	CHECK(isNewerVersion("1.0.0-beta10", "1.0.0-beta2"));
	CHECK(!isNewerVersion("1.0.0-beta2", "1.0.0-beta10"));
	// The same trap one level up.
	CHECK(isNewerVersion("1.10.0", "1.9.0"));
	CHECK(!isNewerVersion("1.9.0", "1.10.0"));
	CHECK(isNewerVersion("2.0.0", "1.99.99"));
}

static void test_same_is_not_newer()
{
	CHECK(!isNewerVersion("1.0.0", "1.0.0"));
	CHECK(!isNewerVersion("1.0.0-beta2", "1.0.0-beta2"));
	CHECK(isNewerVersion("1.0.1", "1.0.0"));
	CHECK(isNewerVersion("1.0.0-beta2", "1.0.0-beta1"));
}

static void test_unreadable_is_never_offered()
{
	// It comes off the network. An update invented from a malformed tag is
	// worse than no update at all.
	CHECK(!isNewerVersion("nightly", "1.0.0"));
	CHECK(!isNewerVersion("", "1.0.0"));
	CHECK(!isNewerVersion("99.99.99", "not-a-version"));
	CHECK(!isNewerVersion("1.0.0-rc1", "1.0.0-beta1"));
}

int main()
{
	test_parsing();
	test_a_release_beats_its_own_betas();
	test_numbers_are_numbers_not_text();
	test_same_is_not_newer();
	test_unreadable_is_never_offered();

	if (g_fail) {
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("version-compare: all checks passed\n");
	return 0;
}
