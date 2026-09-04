/*
obs-multireplay — unit tests for the three updater decisions that decide which
code an operator ends up running.
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, no network, no filesystem. None of this can be tested by
running the plugin — testing it for real would mean publishing releases — which
is precisely why it was the part of the plugin with two blockers in it.

The properties pinned, one per audit finding:

  B1  the SHA-256 is real arithmetic (NIST vectors), and the digest for an
      asset is found in the release body CHECKSUMS.txt is published as;
  B2  the installer script contains NO path, so an operator called O'Brien
      does not get a syntactically invalid PowerShell file; and the command
      line that launches it is quoted the way Windows parses it back;
  A2  an asset belonging to another platform is REFUSED, never fallen back to;
  --  the size cap on a download acts on bytes WRITTEN, not on a declared
      Content-Length a chunked response never sends (size-guard.hpp).
*/

#include "sha256.hpp"
#include "size-guard.hpp"
#include "update-asset.hpp"
#include "update-installer.hpp"

#include <cstdio>
#include <string>
#include <vector>

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

// --- SHA-256 ---------------------------------------------------------------

static void test_sha256_known_vectors()
{
	// FIPS 180-4 / NIST examples. If these pass, the implementation is the
	// algorithm; if they do not, no amount of "it looks right" helps.
	CHECK(sha256::hexOf("") ==
	      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256::hexOf("abc") ==
	      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256::hexOf(
		      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
	      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	// A million 'a', the vector that catches a broken length field.
	{
		sha256::Hasher h;
		const std::string block(1000, 'a');
		for (int i = 0; i < 1000; i++)
			h.update(block.data(), block.size());
		CHECK(h.hex() ==
		      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	}
}

static void test_sha256_is_streaming_and_repeatable()
{
	// The same bytes in different-sized pieces are the same digest: this is
	// what makes hashing a file in 64 KiB blocks legitimate.
	sha256::Hasher a;
	a.update("hello world", 11);
	sha256::Hasher b;
	b.update("hel", 3);
	b.update("lo ", 3);
	b.update("world", 5);
	CHECK(a.hex() == b.hex());
	// And asking twice gives the same answer: padding must not be applied
	// in place, or a checksum would fail on a retry for no reason.
	CHECK(a.hex() == a.hex());
	CHECK(a.hex() == sha256::hexOf("hello world"));
}

// --- which asset (A2) ------------------------------------------------------

static std::vector<update_asset::Asset> aRealRelease()
{
	// The shape push.yaml actually publishes.
	return {
		{"obs-multireplay-1.0.0-windows-x64.zip", "https://x/win.zip",
		 111},
		{"obs-multireplay-1.0.0-macos-universal.pkg",
		 "https://x/mac.pkg", 222},
		{"obs-multireplay-1.0.0-macos-universal.tar.xz",
		 "https://x/mac.tar.xz", 223},
		{"obs-multireplay-1.0.0-x86_64.deb", "https://x/linux.deb", 333},
		{"obs-multireplay-1.0.0-x86_64.tar.xz",
		 "https://x/linux.tar.xz", 334},
		{"obs-multireplay-1.0.0-x86_64-dbgsym.ddeb",
		 "https://x/dbg.ddeb", 444},
		{"obs-multireplay-1.0.0-source.tar.xz", "https://x/src.tar.xz",
		 555},
	};
}

static void test_each_platform_gets_its_own()
{
	using namespace update_asset;
	Asset out;

	CHECK(pick(aRealRelease(), Platform::Windows, out));
	CHECK(out.name == "obs-multireplay-1.0.0-windows-x64.zip");

	// The package, not the tarball: on macOS the .pkg IS the release. The
	// old code accepted neither and fell back to the Windows zip.
	CHECK(pick(aRealRelease(), Platform::MacOS, out));
	CHECK(out.name == "obs-multireplay-1.0.0-macos-universal.pkg");

	CHECK(pick(aRealRelease(), Platform::Linux, out));
	CHECK(out.name == "obs-multireplay-1.0.0-x86_64.deb");
}

static void test_another_platform_is_not_a_fallback()
{
	using namespace update_asset;
	// A release carrying ONLY the Windows archive. The old pickAsset
	// answered "yes, that one" on macOS and Linux, and the operator was
	// told an update had been downloaded.
	const std::vector<Asset> onlyWindows = {
		{"obs-multireplay-1.0.0-windows-x64.zip", "https://x/win.zip",
		 111}};
	Asset out;
	CHECK(pick(onlyWindows, Platform::Windows, out));
	CHECK(!pick(onlyWindows, Platform::MacOS, out));
	CHECK(!pick(onlyWindows, Platform::Linux, out));
}

static void test_debug_symbols_and_sources_are_never_the_update()
{
	using namespace update_asset;
	const std::vector<Asset> junk = {
		{"obs-multireplay-1.0.0-x86_64-dbgsym.ddeb", "https://x/d", 1},
		{"obs-multireplay-1.0.0-source.tar.xz", "https://x/s", 2},
	};
	Asset out;
	CHECK(!pick(junk, Platform::Linux, out));
}

static void test_an_unnamed_archive_still_works()
{
	using namespace update_asset;
	// A small project's first release: one archive, no platform in the
	// name. Accepted — but only because it does not name ANOTHER platform.
	const std::vector<Asset> one = {
		{"obs-multireplay.zip", "https://x/one.zip", 9}};
	Asset out;
	CHECK(pick(one, Platform::Windows, out));
	CHECK(out.name == "obs-multireplay.zip");
	// ...and a .zip is not what Linux installs, so it is still refused
	// there rather than downloaded and left on the operator's disk.
	CHECK(!pick(one, Platform::Linux, out));
}

static void test_asset_names_that_are_paths_are_refused()
{
	using namespace update_asset;
	// The name comes off the network and was used to build a filename.
	CHECK(isSafeAssetName("obs-multireplay-1.0.0-windows-x64.zip"));
	CHECK(!isSafeAssetName("..\\..\\obs64.exe"));
	CHECK(!isSafeAssetName("../../obs64.exe"));
	CHECK(!isSafeAssetName("C:\\Windows\\System32\\evil.dll"));
	CHECK(!isSafeAssetName("plugin.zip; rm -rf /"));
	CHECK(!isSafeAssetName(".hidden.zip"));
	CHECK(!isSafeAssetName(""));
	CHECK(!isSafeAssetName(std::string(200, 'a') + ".zip"));

	// ...and pick() refuses them too, so a poisoned name cannot even be
	// chosen, let alone written.
	const std::vector<Asset> poisoned = {
		{"..\\..\\obs64.zip", "https://x/evil", 1}};
	Asset out;
	CHECK(!pick(poisoned, Platform::Windows, out));
}

static void test_the_names_the_workflow_actually_published()
{
	using namespace update_asset;
	// Copied verbatim from the 1.0.0-beta7 draft release. The list above is
	// the shape the workflow is MEANT to produce; this is the shape it did.
	// They differ — the Linux package is "-x86_64-linux-gnu.deb", not
	// "-x86_64.deb" — and the difference is exactly the kind that decides
	// which binary an operator installs.
	const std::vector<Asset> published = {
		{"obs-multireplay-1.0.0-macos-universal.pkg", "https://x/1",
		 10517557},
		{"obs-multireplay-1.0.0-source.tar.xz", "https://x/2", 752288},
		{"obs-multireplay-1.0.0-windows-x64.zip", "https://x/3", 4016458},
		{"obs-multireplay-1.0.0-x86_64-linux-gnu-dbgsym.ddeb",
		 "https://x/4", 50836},
		{"obs-multireplay-1.0.0-x86_64-linux-gnu.deb", "https://x/5",
		 718894},
	};
	Asset out;
	CHECK(pick(published, Platform::Windows, out));
	CHECK(out.name == "obs-multireplay-1.0.0-windows-x64.zip");
	CHECK(pick(published, Platform::MacOS, out));
	CHECK(out.name == "obs-multireplay-1.0.0-macos-universal.pkg");
	CHECK(pick(published, Platform::Linux, out));
	CHECK(out.name == "obs-multireplay-1.0.0-x86_64-linux-gnu.deb");

	// ...and the body the workflow publishes AS the release notes.
	const std::string body =
		"### Checksums\n"
		"    obs-multireplay-1.0.0-macos-universal.pkg: 7e2fd98f870ae3d0d1f1d09e339d3403d1885047e9fddfcbfc4f43fb02aec971\n"
		"    obs-multireplay-1.0.0-source.tar.xz: 0e45a8d582ce395e8047a1d0188840f16247c626d589d053f75fb9345abd96da\n"
		"    obs-multireplay-1.0.0-windows-x64.zip: 7898911ea7609c643a98b33ee0656aa92392fce45856cb57e2c9b50dc66ceccc\n"
		"    obs-multireplay-1.0.0-x86_64-linux-gnu-dbgsym.ddeb: 96214b45921e9e25c16e4cace4b95cd1f775f70dde38a55064d87ad411befe6c\n"
		"    obs-multireplay-1.0.0-x86_64-linux-gnu.deb: b6e55b134ece72c1bbd5119b46b7a2bbc742c9412436024569ad9aed270adbfd\n";
	CHECK(sha256For(body, "obs-multireplay-1.0.0-windows-x64.zip") ==
	      "7898911ea7609c643a98b33ee0656aa92392fce45856cb57e2c9b50dc66ceccc");
	CHECK(sha256For(body, "obs-multireplay-1.0.0-x86_64-linux-gnu.deb") ==
	      "b6e55b134ece72c1bbd5119b46b7a2bbc742c9412436024569ad9aed270adbfd");
	CHECK(sha256For(body, "obs-multireplay-1.0.0-macos-universal.pkg") ==
	      "7e2fd98f870ae3d0d1f1d09e339d3403d1885047e9fddfcbfc4f43fb02aec971");
}

// --- the checksum in the release body (B1) ---------------------------------

static void test_checksum_is_found_where_the_workflow_puts_it()
{
	// Exactly what push.yaml writes into CHECKSUMS.txt and publishes as the
	// release body.
	const std::string body =
		"### Checksums\n"
		"    obs-multireplay-1.0.0-windows-x64.zip: "
		"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08\n"
		"    obs-multireplay-1.0.0-x86_64.deb: "
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n";

	CHECK(update_asset::sha256For(
		      body, "obs-multireplay-1.0.0-windows-x64.zip") ==
	      "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
	CHECK(update_asset::sha256For(body,
				      "obs-multireplay-1.0.0-x86_64.deb") ==
	      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

	// An asset the body says nothing about: EMPTY, which the updater turns
	// into a refusal to install. Silence is not consent here.
	CHECK(update_asset::sha256For(body, "obs-multireplay-1.0.0-macos.pkg")
		      .empty());
	CHECK(update_asset::sha256For("", "anything").empty());
}

static void test_checksum_reading_is_tolerant_but_strict()
{
	// Tolerant about how a human writes it...
	CHECK(update_asset::sha256For(
		      "`plugin.zip`  "
		      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		      "plugin.zip") ==
	      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	// ...including the "<digest>  <name>" order sha256sum itself prints.
	CHECK(update_asset::sha256For(
		      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  plugin.zip",
		      "plugin.zip") ==
	      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	// UPPERCASE is normalised, because the comparison is against our own
	// lower-case hex.
	CHECK(update_asset::sha256For(
		      "plugin.zip: "
		      "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
		      "plugin.zip") ==
	      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	// ...and strict about what counts as a digest. Sixty-three characters
	// is not a SHA-256, and neither is a word.
	CHECK(update_asset::sha256For("plugin.zip: deadbeef", "plugin.zip")
		      .empty());
	CHECK(update_asset::sha256For(
		      "plugin.zip: "
		      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a",
		      "plugin.zip")
		      .empty());
	CHECK(update_asset::sha256For("plugin.zip: see the website",
				      "plugin.zip")
		      .empty());
}


static void test_a_body_with_release_notes_in_front_of_the_checksums()
{
	// What the published bodies ACTUALLY look like since 1.0.0-beta4: hand
	// written notes, then the checksum block. The notes NAME the assets
	// without giving a digest - the install line is literally
	// "sudo apt install ./obs-multireplay-...deb" - and a table row can carry
	// pipes and backticks. Both are lines that match the asset name and must
	// not be mistaken for the answer, and the second one is the reason the
	// scan continues past a matching line instead of giving up on it.
	const std::string body =
		"**Pre-release.** Requires OBS >= 32.\n"
		"\n"
		"## Fixed\n"
		"\n"
		"| Area | Symptom | Fix |\n"
		"|---|---|---|\n"
		"| Updater | `obs-multireplay-1.0.0-windows-x64.zip` was not "
		"verified | it is now |\n"
		"\n"
		"## Install\n"
		"\n"
		"- **Linux** - `sudo apt install "
		"./obs-multireplay-1.0.0-x86_64-linux-gnu.deb`\n"
		"\n"
		"### Checksums\n"
		"    obs-multireplay-1.0.0-windows-x64.zip: "
		"7898911ea7609c643a98b33ee0656aa92392fce45856cb57e2c9b50dc66ceccc\n"
		"    obs-multireplay-1.0.0-x86_64-linux-gnu.deb: "
		"b6e55b134ece72c1bbd5119b46b7a2bbc742c9412436024569ad9aed270adbfd\n";

	CHECK(update_asset::sha256For(
		      body, "obs-multireplay-1.0.0-windows-x64.zip") ==
	      "7898911ea7609c643a98b33ee0656aa92392fce45856cb57e2c9b50dc66ceccc");
	CHECK(update_asset::sha256For(
		      body, "obs-multireplay-1.0.0-x86_64-linux-gnu.deb") ==
	      "b6e55b134ece72c1bbd5119b46b7a2bbc742c9412436024569ad9aed270adbfd");
	// And an asset the notes mention but the block does not list is still
	// empty, not the digest of whatever came next.
	CHECK(update_asset::sha256For(body, "obs-multireplay-1.0.0-macos.pkg")
		      .empty());
}

// --- the installer (B2) ----------------------------------------------------

static void test_the_script_contains_no_path_at_all()
{
	const std::string s = update_installer::script();
	// The whole point: it is a CONSTANT. Nothing an operator can be called,
	// and nothing an attacker can put in a filename, appears in it.
	CHECK(s.find("O'Brien") == std::string::npos);
	CHECK(s.find("C:\\") == std::string::npos);
	CHECK(s.find("ProgramData") == std::string::npos);
	// It reads its parameters instead, literally.
	CHECK(s.find("install-update.txt") != std::string::npos);
	CHECK(s.find("-LiteralPath") != std::string::npos);
	// The apostrophes it does contain are balanced PowerShell literals: an
	// odd count would mean an unterminated string, which is the exact shape
	// of the bug this replaced.
	size_t quotes = 0;
	for (char c : s)
		if (c == '\'')
			quotes++;
	CHECK(quotes % 2 == 0);
}

static void test_the_parameters_travel_verbatim()
{
	update_installer::Params p;
	p.archivePath = "C:\\Users\\O'Brien\\AppData\\Local\\Temp\\plugin.zip";
	p.targetDir = "C:\\ProgramData\\obs-studio\\plugins\\obs & co";
	p.obsExePath = "C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe";
	const std::string f = update_installer::paramFile(p);

	// Three lines, in order, unescaped and unquoted. Get-Content
	// -LiteralPath does not interpret them, which is why nothing needs
	// escaping — and why the apostrophe that used to break the script is
	// now just a character.
	CHECK(f == p.archivePath + "\n" + p.targetDir + "\n" + p.obsExePath +
			   "\n");
	CHECK(f.find("O'Brien") != std::string::npos);
	CHECK(f.find('&') != std::string::npos);
}

static void test_the_command_line_is_quoted_the_way_windows_reads_it()
{
	using update_installer::quoteArg;
	// Nothing special: left alone.
	CHECK(quoteArg("powershell.exe") == "powershell.exe");
	CHECK(quoteArg("-NoProfile") == "-NoProfile");
	// A space means quotes.
	CHECK(quoteArg("C:\\Program Files\\x.ps1") ==
	      "\"C:\\Program Files\\x.ps1\"");
	// An apostrophe is NOT special to CreateProcessW: no shell, nothing to
	// escape it against.
	CHECK(quoteArg("C:\\Users\\O'Brien\\x.ps1") ==
	      "C:\\Users\\O'Brien\\x.ps1");
	// A trailing backslash before the closing quote must be doubled, or it
	// escapes the quote and the argument swallows the next one. This is the
	// rule everybody gets wrong.
	CHECK(quoteArg("C:\\Program Files\\dir\\") ==
	      "\"C:\\Program Files\\dir\\\\\"");
	// An embedded quote is escaped, and the backslashes before it doubled.
	CHECK(quoteArg("a\\\"b") == "\"a\\\\\\\"b\"");

	const std::string script =
		"C:\\Users\\O'Brien\\AppData\\Local\\Temp\\obs-multireplay-update\\install-update.ps1";
	const std::string cmd = update_installer::commandLine(
		update_installer::argvFor("powershell.exe", script));
	// -File takes ONE path and there is nothing after it: the script has no
	// arguments, on purpose.
	CHECK(cmd.find("-NoProfile") != std::string::npos);
	CHECK(cmd.find("-ExecutionPolicy Bypass") != std::string::npos);
	CHECK(cmd.size() >= script.size());
	CHECK(cmd.compare(cmd.size() - script.size(), script.size(), script) ==
	      0);
	// And no shell metacharacter survives as syntax, because there is no
	// shell: the string goes to CreateProcessW.
	CHECK(cmd.find("start ") == std::string::npos);
	CHECK(cmd.find("/B") == std::string::npos);
}


// --- the installer for SOMEBODY ELSE'S plugin ------------------------------

// The real 1.0.9 release of Branch Output, copied from the GitHub API. Copied
// rather than imagined, for the same reason the list above is: a difference of
// exactly this kind decides which binary an operator ends up running.
static std::vector<update_asset::Asset> aBranchOutputRelease()
{
	return {
		{"osi-branch-output-1.0.9-macos-universal.pkg",
		 "https://x/bo.pkg", 2634748},
		{"osi-branch-output-1.0.9-source.tar.xz", "https://x/bo-src.tar.xz",
		 2508804},
		{"osi-branch-output-1.0.9-windows-x64-Installer-signed.exe",
		 "https://x/bo-setup.exe", 2682576},
		{"osi-branch-output-1.0.9-windows-x64.zip", "https://x/bo.zip",
		 1300063},
		{"osi-branch-output-1.0.9-x86_64-linux-gnu-dbgsym.ddeb",
		 "https://x/bo-dbg.ddeb", 22302},
		{"osi-branch-output-1.0.9-x86_64-linux-gnu.deb", "https://x/bo.deb",
		 174030},
	};
}

static void test_the_installer_wins_over_the_archive()
{
	using namespace update_asset;
	Asset out;

	// On Windows the release carries BOTH a zip and a signed installer, and
	// the installer is the one to run: it asks for elevation through its own
	// manifest, it knows where OBS keeps its plugins, and it is signed. The
	// zip is what our own updater wants for our own plugin — a different job.
	CHECK(pickInstaller(aBranchOutputRelease(), Platform::Windows, out));
	CHECK(out.name ==
	      "osi-branch-output-1.0.9-windows-x64-Installer-signed.exe");
	// ...and pick() still answers with the archive, unchanged.
	CHECK(pick(aBranchOutputRelease(), Platform::Windows, out));
	CHECK(out.name == "osi-branch-output-1.0.9-windows-x64.zip");

	CHECK(pickInstaller(aBranchOutputRelease(), Platform::MacOS, out));
	CHECK(out.name == "osi-branch-output-1.0.9-macos-universal.pkg");

	// The .deb and not the .ddeb beside it — the same trap as pick().
	CHECK(pickInstaller(aBranchOutputRelease(), Platform::Linux, out));
	CHECK(out.name == "osi-branch-output-1.0.9-x86_64-linux-gnu.deb");
}

static void test_an_installer_for_another_platform_is_not_a_fallback()
{
	using namespace update_asset;
	const std::vector<Asset> onlyWindows = {
		{"osi-branch-output-1.0.9-windows-x64-Installer-signed.exe",
		 "https://x/bo-setup.exe", 2682576}};
	Asset out;
	CHECK(pickInstaller(onlyWindows, Platform::Windows, out));
	CHECK(!pickInstaller(onlyWindows, Platform::MacOS, out));
	CHECK(!pickInstaller(onlyWindows, Platform::Linux, out));
	// And a release that carries only one platform's installer offers
	// nothing to the other two, which is the refusal above.
	// Every platform has an installer in that release, and each is the one
	// its own system knows how to open.

}

static void test_the_digest_github_reports_is_read_or_refused()
{
	using namespace update_asset;
	// The real shape, from the API response for the release above.
	CHECK(sha256FromDigest(
		      "sha256:0110117cd665975fd52bc7d64bf7da3e56dfd4b41910dc6098ef474c2e15060e") ==
	      "0110117cd665975fd52bc7d64bf7da3e56dfd4b41910dc6098ef474c2e15060e");
	// Upper case survives the round trip as lower case, because that is what
	// sha256::hex() produces and what it will be compared against.
	CHECK(sha256FromDigest("SHA256:ABCDEF0123456789abcdef0123456789"
			       "ABCDEF0123456789abcdef0123456789") ==
	      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");

	// EVERY refusal matters here: an empty answer means "do not run this",
	// and a parser that shrugged would hand the operator an unverified
	// executable.
	CHECK(sha256FromDigest("").empty());
	CHECK(sha256FromDigest("sha256:").empty());
	CHECK(sha256FromDigest("sha512:0110117cd665975fd52bc7d64bf7da3e"
			       "56dfd4b41910dc6098ef474c2e15060e")
		      .empty());
	// Right prefix, wrong length.
	CHECK(sha256FromDigest("sha256:0110117c").empty());
	// Right length, not hex.
	CHECK(sha256FromDigest("sha256:zz10117cd665975fd52bc7d64bf7da3e"
			       "56dfd4b41910dc6098ef474c2e15060e")
		      .empty());
	// No prefix at all — a bare digest is not the field GitHub sends.
	CHECK(sha256FromDigest("0110117cd665975fd52bc7d64bf7da3e56dfd4b419"
			       "10dc6098ef474c2e15060e")
		      .empty());
}
static void test_only_windows_claims_it_can_install()
{
	using namespace update_asset;
	CHECK(isInstallableHere(Platform::Windows));
	CHECK(!isInstallableHere(Platform::MacOS));
	CHECK(!isInstallableHere(Platform::Linux));
}

// --- the download size cap (chunked responses have no Content-Length) ------

static void test_size_guard()
{
	using size_guard::wouldOverflow;
	CHECK(!wouldOverflow(0, 1024, 200 * 1024 * 1024));
	// Exactly at the cap: still allowed, the last legal byte.
	CHECK(!wouldOverflow(199, 1, 200));
	// One byte past it.
	CHECK(wouldOverflow(199, 2, 200));
	// A single chunk larger than the whole cap, from nothing written yet —
	// the case an infinite chunked stream produces on its very first read.
	CHECK(wouldOverflow(0, 300, 200));
	// Already at the cap: even a zero-sized chunk would not push past it,
	// so it does not overflow, but the very next byte does.
	CHECK(!wouldOverflow(200, 0, 200));
	CHECK(wouldOverflow(200, 1, 200));
}

int main()
{
	test_sha256_known_vectors();
	test_sha256_is_streaming_and_repeatable();
	test_each_platform_gets_its_own();
	test_another_platform_is_not_a_fallback();
	test_debug_symbols_and_sources_are_never_the_update();
	test_an_unnamed_archive_still_works();
	test_asset_names_that_are_paths_are_refused();
	test_the_names_the_workflow_actually_published();
	test_checksum_is_found_where_the_workflow_puts_it();
	test_checksum_reading_is_tolerant_but_strict();
	test_a_body_with_release_notes_in_front_of_the_checksums();
	test_the_script_contains_no_path_at_all();
	test_the_parameters_travel_verbatim();
	test_the_command_line_is_quoted_the_way_windows_reads_it();
	test_only_windows_claims_it_can_install();
	test_the_installer_wins_over_the_archive();
	test_an_installer_for_another_platform_is_not_a_fallback();
	test_the_digest_github_reports_is_read_or_refused();
	test_size_guard();

	if (g_fail) {
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("updater: all checks passed\n");
	return 0;
}
