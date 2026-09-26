/*
obs-multireplay — which release asset, and is it the one that was announced
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Three decisions the updater used to make inline, lifted out because none of them
can be tested by running the plugin — testing them for real would mean
publishing releases — and all three decide what code the operator ends up
executing.

  pick()            Which asset is FOR THIS PLATFORM. It used to accept only
                    .zip/.tar.gz, so on macOS and Linux the .pkg and .deb the
                    release actually carries were skipped and it FELL BACK to
                    the Windows zip: "downloaded", of the wrong platform,
                    silently. There is no cross-platform fallback here any
                    more. An asset that names another platform is refused, and
                    a release with nothing for this one is reported as having
                    nothing.

  isSafeAssetName() The name comes out of the HTTP response and was used as a
                    filename: `dir / rel.assetName`. `..\..\obs64.exe` is a
                    perfectly good JSON string.

  sha256For()       The release body IS CHECKSUMS.txt (push.yaml publishes it
                    as the body), so the digest of every asset is already
                    there, next to the URL. Reading it is what turns "the file
                    is the size the JSON said" — a source compared with itself
                    — into a check on the bytes.

Pure: no OBS, no curl, no filesystem. Same rule as master-timeline.hpp.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace multireplay {

namespace update_asset {

struct Asset {
	std::string name;
	std::string url;
	int64_t size = 0;
};

enum class Platform { Windows, MacOS, Linux };

constexpr Platform hostPlatform()
{
#if defined(_WIN32)
	return Platform::Windows;
#elif defined(__APPLE__)
	return Platform::MacOS;
#else
	return Platform::Linux;
#endif
}

inline std::string lower(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s)
		out += (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	return out;
}

inline bool endsWith(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() &&
	       s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool contains(const std::string &s, const std::string &needle)
{
	return s.find(needle) != std::string::npos;
}

// A NAME, not a path. Everything else is refused, because this string arrives
// from the network and is used to build a filename.
inline bool isSafeAssetName(const std::string &name)
{
	if (name.empty() || name.size() > 128)
		return false;
	if (name.front() == '.' || name.front() == '-')
		return false; // no dotfiles, no leading dash
	for (unsigned char c : name) {
		const bool ok = (c >= 'a' && c <= 'z') ||
				(c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '.' || c == '_' ||
				c == '-' || c == '+';
		if (!ok)
			return false;
	}
	return !contains(name, "..");
}

// The extensions each platform will actually install, best first. macOS and
// Linux carry their package format ahead of the tarball for a reason: the
// package is what the release is FOR on that platform.
inline std::vector<std::string> extensionsFor(Platform p)
{
	switch (p) {
	case Platform::Windows:
		return {".zip"};
	case Platform::MacOS:
		return {".pkg", ".tar.xz", ".tar.gz"};
	case Platform::Linux:
	default:
		return {".deb", ".tar.xz", ".tar.gz"};
	}
}

// Words that say a file belongs to a platform. Used both to CHOOSE and — this
// is the half that was missing — to REFUSE.
inline std::vector<std::string> tokensFor(Platform p)
{
	switch (p) {
	case Platform::Windows:
		return {"windows", "win64", "win32"};
	case Platform::MacOS:
		return {"macos", "macosx", "darwin", "universal"};
	case Platform::Linux:
	default:
		return {"ubuntu", "linux", "debian", "x86_64"};
	}
}

inline bool namesPlatform(const std::string &lowerName, Platform p)
{
	for (const auto &t : tokensFor(p))
		if (contains(lowerName, t))
			return true;
	return false;
}

inline bool namesAnotherPlatform(const std::string &lowerName, Platform mine)
{
	for (Platform p : {Platform::Windows, Platform::MacOS, Platform::Linux})
		if (p != mine && namesPlatform(lowerName, p))
			return true;
	return false;
}

// "ubuntu-26.04" for an /etc/os-release that says ID=ubuntu, VERSION_ID="26.04";
// empty for anything else, or anything unreadable. Pure: the caller reads the
// file, so this is a unit test and not a Linux-only story.
inline std::string distroTag(const std::string &osRelease)
{
	std::string id, version;
	size_t start = 0;
	while (start <= osRelease.size()) {
		size_t end = osRelease.find('\n', start);
		if (end == std::string::npos)
			end = osRelease.size();
		std::string line = osRelease.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t eq = line.find('=');
		if (eq != std::string::npos) {
			const std::string key = line.substr(0, eq);
			std::string value = line.substr(eq + 1);
			if (value.size() >= 2 &&
			    (value.front() == '"' || value.front() == '\'') &&
			    value.back() == value.front())
				value = value.substr(1, value.size() - 2);
			if (key == "ID")
				id = lower(value);
			else if (key == "VERSION_ID")
				version = value;
		}
		start = end + 1;
	}
	if (id != "ubuntu" || version.empty())
		return {};
	for (char c : version)
		if (!((c >= '0' && c <= '9') || c == '.'))
			return {};
	return id + "-" + version;
}

// The "ubuntu-XX.YY" an asset name carries, or empty. Linux packages are built
// per Ubuntu release (their FFmpeg and Qt package names differ, so a .deb for
// one does not install on the other), and the name says which.
inline std::string assetDistroTag(const std::string &lowerName)
{
	const std::string key = "ubuntu-";
	size_t at = lowerName.find(key);
	while (at != std::string::npos) {
		size_t i = at + key.size();
		const size_t digits = i;
		while (i < lowerName.size() &&
		       ((lowerName[i] >= '0' && lowerName[i] <= '9') ||
			lowerName[i] == '.'))
			i++;
		// Strip a trailing dot: "ubuntu-26.04.deb" is 26.04, not "26.04."
		size_t stop = i;
		while (stop > digits && lowerName[stop - 1] == '.')
			stop--;
		if (stop > digits)
			return lowerName.substr(at, stop - at);
		at = lowerName.find(key, at + 1);
	}
	return {};
}

// The asset for `p`, or false. Debug-symbol bundles and source tarballs are
// never it.
//
// `distro` is distroTag() of the machine asking, on Linux. An asset built for
// ANOTHER Ubuntu release is refused, not ranked lower: its package names do not
// exist here, and handing it over is handing over something apt will reject.
// One built for this release wins over one that names none.
inline bool pick(const std::vector<Asset> &assets, Platform p, Asset &out,
		 const std::string &distro = std::string())
{
	const auto exts = extensionsFor(p);
	// Rank: an asset that names this platform beats one that names none;
	// within that, the earlier extension wins.
	int bestRank = -1;
	for (const Asset &a : assets) {
		if (a.url.empty() || !isSafeAssetName(a.name))
			continue;
		const std::string ln = lower(a.name);
		if (contains(ln, "dbgsym") || contains(ln, "dsym") ||
		    endsWith(ln, ".ddeb") || contains(ln, "source"))
			continue;

		int extIdx = -1;
		for (size_t i = 0; i < exts.size(); i++)
			if (endsWith(ln, exts[i])) {
				extIdx = (int)i;
				break;
			}
		if (extIdx < 0)
			continue;
		// AN ASSET OF ANOTHER PLATFORM IS NOT A FALLBACK. This is the
		// whole point of the file: a .zip that says "windows" on a Linux
		// box is not "the best we have", it is the wrong thing.
		if (namesAnotherPlatform(ln, p))
			continue;

		int rank = (namesPlatform(ln, p) ? 100 : 50) - extIdx * 10;
		const std::string tag = assetDistroTag(ln);
		if (!tag.empty() && !distro.empty()) {
			if (tag != distro)
				continue;
			rank += 5;
		}
		if (rank > bestRank) {
			bestRank = rank;
			out = a;
		}
	}
	return bestRank >= 0;
}

// Can this build install what it downloads, or only hand the file over? The
// panel has to say which, instead of offering an "Install" that returns false.
inline constexpr bool isInstallableHere(Platform p)
{
	return p == Platform::Windows;
}

inline bool isHexDigest(const std::string &s)
{
	if (s.size() != 64)
		return false;
	for (unsigned char c : s) {
		const bool ok = (c >= '0' && c <= '9') ||
				(c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!ok)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The asset a person can simply RUN — for installing somebody ELSE's plugin
//
// This exists for Branch Output, which this plugin cannot record without. Our
// own updater wants the ARCHIVE (pick, above) because it unpacks it over an
// installed plugin with a helper that waits for OBS to exit. A plugin that is
// not installed at all is a different problem, and a much easier one: the
// vendor publishes an installer, and running it beats anything we could do
// ourselves on all three counts that matter — it asks Windows for elevation
// through its own manifest (the OBS plugin folder is not writable by a normal
// user on a machine where OBS was installed by an administrator), it knows
// where that folder is, and it is signed by whoever built it. Unpacking a zip
// into ProgramData ourselves would fake all three, badly.
//
// EVERY PLATFORM, and each with the file its own system knows how to open: the
// signed .exe on Windows, the .pkg on macOS, the .deb on Linux. Handing the
// file to the system is the whole of "install" here — we never place a byte.
// ---------------------------------------------------------------------------

inline std::vector<std::string> installerExtensionsFor(Platform p)
{
	switch (p) {
	case Platform::Windows:
		return {".exe"};
	case Platform::MacOS:
		return {".pkg"};
	case Platform::Linux:
	default:
		return {".deb"};
	}
}

inline bool pickInstaller(const std::vector<Asset> &assets, Platform p,
			  Asset &out)
{
	const auto exts = installerExtensionsFor(p);
	int bestRank = -1;
	for (const Asset &a : assets) {
		if (a.url.empty() || !isSafeAssetName(a.name))
			continue;
		const std::string ln = lower(a.name);
		if (contains(ln, "dbgsym") || contains(ln, "dsym") ||
		    endsWith(ln, ".ddeb") || contains(ln, "source"))
			continue;
		bool extOk = false;
		for (const auto &e : exts)
			if (endsWith(ln, e)) {
				extOk = true;
				break;
			}
		if (!extOk)
			continue;
		// Same refusal as pick(): an installer for another platform is
		// not a fallback, it is the wrong file.
		if (namesAnotherPlatform(ln, p))
			continue;
		// A SIGNED installer beats an unsigned one, and either beats a
		// bare .exe that only happens to end in the right letters.
		int rank = namesPlatform(ln, p) ? 100 : 50;
		if (contains(ln, "installer"))
			rank += 20;
		if (contains(ln, "signed"))
			rank += 10;
		if (rank > bestRank) {
			bestRank = rank;
			out = a;
		}
	}
	return bestRank >= 0;
}

// "sha256:abcdef…" — the shape GitHub's release API reports an asset's digest
// in — reduced to the bare hex this plugin compares against, or empty if it is
// anything else. Empty is a REFUSAL upstream, not a shrug: a download nobody
// can verify is a download this plugin does not run.
inline std::string sha256FromDigest(const std::string &digest)
{
	constexpr const char *kPrefix = "sha256:";
	const size_t n = 7; // strlen(kPrefix)
	if (digest.size() <= n)
		return {};
	if (lower(digest.substr(0, n)) != kPrefix)
		return {};
	const std::string hex = lower(digest.substr(n));
	return isHexDigest(hex) ? hex : std::string();
}

// The digest CHECKSUMS.txt gives for `assetName`, lower-case, or empty.
// The published shape is four spaces, the file name, a colon, the digest:
//
//     ### Checksums
//         obs-multireplay-1.0.0-windows-x64.zip: 9f86d0...
//
// but this is deliberately tolerant about spacing and order (some tools write
// "<digest>  <name>") and strict about what it accepts as a digest — a release
// body is markdown written by a human as often as by a workflow.
inline std::string sha256For(const std::string &notes,
			     const std::string &assetName)
{
	if (assetName.empty())
		return {};
	size_t pos = 0;
	while (pos <= notes.size()) {
		size_t end = notes.find('\n', pos);
		if (end == std::string::npos)
			end = notes.size();
		const std::string line = notes.substr(pos, end - pos);
		const bool wasLast = end == notes.size();
		pos = end + 1;
		if (line.find(assetName) == std::string::npos) {
			if (wasLast)
				break;
			continue;
		}
		// Every separated word on the line; the one that looks like a
		// digest is the answer.
		static const std::string kSeps = " \t:\r`*|";
		size_t i = 0;
		while (i < line.size()) {
			i = line.find_first_not_of(kSeps, i);
			if (i == std::string::npos)
				break;
			size_t j = line.find_first_of(kSeps, i);
			if (j == std::string::npos)
				j = line.size();
			const std::string word = line.substr(i, j - i);
			i = j;
			if (isHexDigest(word))
				return lower(word);
		}
		if (wasLast)
			break;
	}
	return {};
}

// What the Updates page SHOWS of a release body: the changelog, without the
// `### Checksums` block and whatever follows it (`### Verification`, appended
// by CI). Those are for the updater and for someone verifying by hand; to an
// operator reading "what changed" they are a wall of hex. Only a heading line
// counts — a paragraph that says "checksums" is changelog — and the cut also
// drops the blank lines it would leave at the end. sha256For() keeps reading
// the full body: this is presentation only.
inline std::string notesForDisplay(const std::string &body)
{
	size_t pos = 0;
	size_t cut = body.size();
	while (pos < body.size()) {
		size_t end = body.find('\n', pos);
		if (end == std::string::npos)
			end = body.size();
		std::string line = body.substr(pos, end - pos);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		const size_t hashes = line.find_first_not_of('#');
		if (hashes != std::string::npos && hashes >= 1 && hashes <= 6 &&
		    line[hashes] == ' ' &&
		    lower(line.substr(hashes + 1)) == "checksums") {
			cut = pos;
			break;
		}
		pos = end + 1;
	}
	std::string out = body.substr(0, cut);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
				out.back() == ' '))
		out.pop_back();
	return out;
}

} // namespace update_asset

} // namespace multireplay
