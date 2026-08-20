/*
obs-multireplay — unit tests for what a path is made of, and what a project
title is allowed to become on disk.
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, and deliberately no filesystem either — these are decisions
about BYTES, and the bug they close is invisible on the platform this project is
developed on.

  A3  every path travels as UTF-8. `std::filesystem::path::string()` on MSVC
      narrows through the active ANSI code page, while os_fopen,
      obs_data_save_json_safe and FFmpeg all want UTF-8 — so a session folder
      or a project called "Partita_Citta" with an accent broke recording,
      anchors.json, events.json and export ON WINDOWS ONLY. On Linux and macOS
      the same code is correct, which is why a CI that builds on three
      platforms never saw it.

  M9  the sanitising used to be std::isalnum, which is LOCALE-DEPENDENT, so the
      same title produced a different folder depending on a global nobody sets;
      and either way every non-ASCII character was dropped in silence.

  M10 openProject() concatenated its argument onto the session folder with no
      check at all, and one of the places that argument comes from is
      currentProjectName read out of config.json.
*/

#include "path-utf8.hpp"
#include "project-name.hpp"

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

// The bytes of "Partita_Citta" with a grave accent, written out so the test
// does not depend on the encoding of this source file.
static const std::string kCitta = "Partita_Citt\xC3\xA0";
static const std::string kAccents = "\xC3\x89t\xC3\xA9"; // Ete, accented

static void test_utf8_survives_the_round_trip()
{
	// The property the whole fix rests on: bytes in, same bytes out, on
	// every platform. With path::string() this fails on MSVC in any code
	// page that cannot express the character — silently, by substitution.
	CHECK(pathToUtf8(utf8ToPath(kCitta)) == kCitta);
	CHECK(pathToUtf8(utf8ToPath(kAccents)) == kAccents);
	CHECK(pathToUtf8(utf8ToPath("plain")) == "plain");
	CHECK(pathToUtf8(utf8ToPath("")) == "");
	// Japanese, for a character that is three bytes rather than two.
	const std::string jp = "\xE8\xA9\xA6\xE5\x90\x88";
	CHECK(pathToUtf8(utf8ToPath(jp)) == jp);
}

static void test_join_keeps_both_halves()
{
	const std::string joined = joinUtf8("C:/rec", kCitta);
	CHECK(joined.find(kCitta) != std::string::npos);
	CHECK(joined.find("rec") != std::string::npos);
	// And joining onto a joined path is still UTF-8 all the way down: this
	// is projectSettingsPath(), which is <session>/<project>/settings.json.
	const std::string twice = joinUtf8(joined, "settings.json");
	CHECK(twice.find(kCitta) != std::string::npos);
	CHECK(twice.find("settings.json") != std::string::npos);
}

static void test_a_title_keeps_its_accents()
{
	// The headline of M9: the accented "Citta" used to lose its last
	// character altogether.
	const std::string out = project_name::sanitize("Partita Citt\xC3\xA0");
	CHECK(out == kCitta);
	CHECK(project_name::sanitize("\xC3\x89t\xC3\xA9 2026") ==
	      kAccents + "_2026");
}

static void test_a_title_becomes_a_usable_folder()
{
	using project_name::sanitize;
	CHECK(sanitize("Juve-Milan 2026") == "Juve-Milan_2026");
	// Spaces collapse rather than doubling up.
	CHECK(sanitize("a    b") == "a_b");
	// No leading or trailing separator noise.
	CHECK(sanitize("  match  ") == "match");
	CHECK(sanitize("match...") == "match");
	CHECK(sanitize(".hidden") == "hidden");
	// The characters no Windows filesystem accepts simply go.
	CHECK(sanitize("a:b*c?d\"e<f>g|h") == "abcdefgh");
	CHECK(sanitize("a/b\\c") == "abc");
	// Nothing usable at all is EMPTY, which the caller must report rather
	// than turn into a folder called something arbitrary.
	CHECK(sanitize("").empty());
	CHECK(sanitize("///").empty());
	CHECK(sanitize("   ").empty());
	// A reserved device name cannot be a folder on Windows, so it is not
	// offered as one.
	CHECK(sanitize("CON") != "CON");
	CHECK(sanitize("com1") != "com1");
	CHECK(sanitize("PRN.match") != "PRN.match");
	// A long title is cut, and cut on a character boundary: half a UTF-8
	// sequence is not a name.
	const std::string longTitle(300, 'x');
	CHECK(sanitize(longTitle).size() <= project_name::kMaxLength);
	std::string longAccents;
	for (int i = 0; i < 200; i++)
		longAccents += "\xC3\xA0";
	const std::string cut = sanitize(longAccents);
	CHECK(cut.size() <= project_name::kMaxLength);
	CHECK(cut.size() % 2 == 0); // two bytes each; no half one left behind
}

static void test_malformed_utf8_does_not_become_a_folder_name()
{
	using project_name::sanitize;
	// A lone continuation byte, a truncated sequence, an overlong form and
	// a surrogate: none of them are characters, so none of them survive.
	CHECK(sanitize("a\x80" "A") == "aA");
	CHECK(sanitize("a\xC3") == "a");
	CHECK(sanitize("a\xC0\xAF" "b") == "ab");     // overlong slash
	CHECK(sanitize("a\xED\xA0\x80" "z") == "az"); // surrogate
}

static void test_a_name_from_a_config_file_is_checked_before_it_is_a_path()
{
	using project_name::isSafeFolderName;
	CHECK(isSafeFolderName("Partita_Giovinazzo"));
	CHECK(isSafeFolderName(kCitta));
	CHECK(isSafeFolderName("a.b.c"));

	// M10, the whole point: these arrive from config.json and from dialogs.
	CHECK(!isSafeFolderName(".."));
	CHECK(!isSafeFolderName("."));
	CHECK(!isSafeFolderName("../../etc"));
	CHECK(!isSafeFolderName("..\\..\\Windows"));
	CHECK(!isSafeFolderName("C:\\Windows"));
	CHECK(!isSafeFolderName("sub/dir"));
	CHECK(!isSafeFolderName("with\ttab"));
	CHECK(!isSafeFolderName("trailing "));
	CHECK(!isSafeFolderName("trailing."));
	CHECK(!isSafeFolderName("NUL"));
	CHECK(!isSafeFolderName(""));

	// And the two halves agree: whatever sanitize() produces is something
	// openProject() will accept, or a project could be created and then not
	// reopened.
	const char *titles[] = {"Juve-Milan 2026", "Partita Citt\xC3\xA0",
				"\xC3\x89t\xC3\xA9 2026", "a:b*c?d", "CON",
				"  match  "};
	for (const char *t : titles) {
		const std::string folder = project_name::sanitize(t);
		if (!folder.empty())
			CHECK(isSafeFolderName(folder));
	}
}

int main()
{
	test_utf8_survives_the_round_trip();
	test_join_keeps_both_halves();
	test_a_title_keeps_its_accents();
	test_a_title_becomes_a_usable_folder();
	test_malformed_utf8_does_not_become_a_folder_name();
	test_a_name_from_a_config_file_is_checked_before_it_is_a_path();

	if (g_fail) {
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("paths: all checks passed\n");
	return 0;
}
