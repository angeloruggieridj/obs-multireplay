/*
obs-multireplay — unit tests for error-locale.hpp
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, no Qt. Every string below is copied verbatim from where
it is actually produced (playback-coordinator.cpp, export's musicProblem()),
not paraphrased — the classifier matches substrings the real code builds,
and a paraphrase would test a string nothing ever produces.
*/

#include "error-locale.hpp"

#include <cstdio>

using namespace multireplay;
using error_locale::Class;
using error_locale::classify;

static int g_fail = 0;

#define CHECK(cond)                                                        \
	do {                                                                \
		if (!(cond)) {                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, \
				    #cond);                                 \
			++g_fail;                                           \
		}                                                           \
	} while (0)

static void test_playback_errors()
{
	// playback-coordinator.cpp: playEvents()'s refusal, camera number
	// interpolated.
	CHECK(classify("camera 3 has no footage for this event (no camera "
		       "wired to it, nothing in the ring and nothing "
		       "recorded)") == Class::NoFootageForCamera);
	CHECK(classify("camera 1 has no footage for this event (no camera "
		       "wired to it, nothing in the ring and nothing "
		       "recorded)") == Class::NoFootageForCamera);
	CHECK(classify("no playable (completed) events selected") ==
	      Class::NoPlayableEvents);
	CHECK(classify("no footage in that range") == Class::NoFootageInRange);
	CHECK(classify("no completed events yet") == Class::NoCompletedEvents);
}

static void test_music_errors()
{
	// PlaybackCoordinator::musicProblem(), paths and source names
	// interpolated.
	CHECK(classify("the music file does not exist: "
		       "C:\\Users\\O'Brien\\music.mp3") ==
	      Class::MusicFileMissing);
	CHECK(classify("no music is configured: set a music file or a music "
		       "source in Settings") == Class::MusicNotConfigured);
	CHECK(classify("the music source 'Music' does not exist in this "
		       "scene collection") == Class::MusicSourceMissing);
	CHECK(classify("the music source 'Music' is not in an active scene, "
		       "so unmuting it cannot be heard — put it in a scene, "
		       "or set a music file instead") ==
	      Class::MusicSourceNotActive);
}

static void test_unrecognised_text_is_unknown()
{
	// Anything this file has no class for is Unknown, not misclassified
	// into the nearest-looking one — the caller's fallback (show the raw
	// text) is the safe default, and a false match here would hide a new
	// error behind the wrong headline.
	CHECK(classify("") == Class::Unknown);
	CHECK(classify("the server answered 403") == Class::Unknown);
	CHECK(classify("cancelled") == Class::Unknown);
	// Close to a real one but not it: "camera" without "has no footage"
	// must not match on the prefix alone.
	CHECK(classify("camera 2 is a fine camera") == Class::Unknown);
}

int main()
{
	test_playback_errors();
	test_music_errors();
	test_unrecognised_text_is_unknown();

	if (g_fail) {
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("error_locale: all checks passed\n");
	return 0;
}
