/*
obs-multireplay — what a project title is allowed to become on disk
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Two questions, and they are not the same one.

  sanitizeProjectName()  — the operator typed a title; what folder is that?
  isSafeProjectFolderName() — a name arrived from somewhere (config.json, the
                              project list, a dialog); may it be joined onto the
                              session folder at all?

The second exists because `openProject()` concatenated its argument onto the
session folder with no check whatsoever, and one of the places that argument
comes from is `currentProjectName` read out of a config file. `..\..\..` is a
folder name as far as `operator/` is concerned.

The first used to be `std::isalnum`, which is LOCALE-DEPENDENT: in a single-byte
ANSI locale the continuation bytes of a UTF-8 sequence can pass it, and in the
default "C" locale they cannot — so the same title produced a different folder
depending on a global nobody in this plugin sets. Either way every non-ASCII
character was dropped in silence, and "Città" became "Citt". Now the UTF-8 is
VALIDATED and kept (path-utf8.hpp is what makes keeping it safe on Windows), and
only the characters a filesystem genuinely refuses are removed.

Pure: no OBS, no Qt, no filesystem. Same rule as master-timeline.hpp.
*/

#pragma once

#include <string>

namespace multireplay {

namespace project_name {

// Longest folder name we will produce. Well inside MAX_PATH once a session
// folder is prepended, and long enough for any match title anyone types.
inline constexpr size_t kMaxLength = 96;

// Characters no Windows filesystem accepts, plus the separators, plus the two
// that would make the name a traversal.
inline bool isForbiddenAscii(unsigned char c)
{
	if (c < 0x20 || c == 0x7F)
		return true; // control characters
	switch (c) {
	case '<':
	case '>':
	case ':':
	case '"':
	case '/':
	case '\\':
	case '|':
	case '?':
	case '*':
		return true;
	default:
		return false;
	}
}

// CON, PRN, AUX, NUL, COM1..9, LPT1..9 — reserved by Windows whatever the
// extension, and a folder called one of them cannot be created.
inline bool isReservedDeviceName(const std::string &s)
{
	std::string up;
	for (unsigned char c : s) {
		if (c == '.')
			break; // the reservation applies to the stem
		up += (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
	}
	if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL")
		return true;
	if (up.size() == 4 && (up.compare(0, 3, "COM") == 0 ||
			       up.compare(0, 3, "LPT") == 0) &&
	    up[3] >= '1' && up[3] <= '9')
		return true;
	return false;
}

// How many bytes the UTF-8 sequence starting at `s[i]` occupies, 0 when the
// bytes there are not a well-formed sequence. Overlong forms, surrogates and
// anything above U+10FFFF are rejected: a folder name is going to be handed to
// the OS, and malformed UTF-8 is where a "name" stops being one.
inline size_t utf8SequenceLength(const std::string &s, size_t i)
{
	const auto byte = [&](size_t k) -> unsigned {
		return (unsigned char)s[k];
	};
	const unsigned b0 = byte(i);
	size_t len = 0;
	unsigned cp = 0;
	if (b0 < 0x80)
		return 1;
	else if ((b0 & 0xE0) == 0xC0) {
		len = 2;
		cp = b0 & 0x1F;
	} else if ((b0 & 0xF0) == 0xE0) {
		len = 3;
		cp = b0 & 0x0F;
	} else if ((b0 & 0xF8) == 0xF0) {
		len = 4;
		cp = b0 & 0x07;
	} else
		return 0;

	if (i + len > s.size())
		return 0;
	for (size_t k = 1; k < len; k++) {
		const unsigned b = byte(i + k);
		if ((b & 0xC0) != 0x80)
			return 0;
		cp = (cp << 6) | (b & 0x3F);
	}
	// Overlong, surrogate, out of range.
	if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
	    (len == 4 && cp < 0x10000))
		return 0;
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return 0;
	return len;
}

// The folder a title becomes. Empty when the title has nothing usable in it,
// which the caller must report rather than paper over.
inline std::string sanitize(const std::string &title)
{
	std::string out;
	for (size_t i = 0; i < title.size();) {
		const unsigned char c = (unsigned char)title[i];
		if (c < 0x80) {
			if (c == ' ' || c == '\t') {
				// A space becomes an underscore, but never a
				// leading or a doubled one.
				if (!out.empty() && out.back() != '_')
					out += '_';
			} else if (!isForbiddenAscii(c)) {
				out += (char)c;
			}
			i++;
			continue;
		}
		// Non-ASCII: keep it, but only if it really is UTF-8.
		const size_t len = utf8SequenceLength(title, i);
		if (len == 0) {
			i++; // not a sequence: drop this byte
			continue;
		}
		out.append(title, i, len);
		i += len;
	}

	// Windows silently strips trailing dots and spaces from a name, which
	// would leave the folder on disk called something other than what was
	// asked for. Do it here, where it can be seen.
	while (!out.empty() && (out.back() == '.' || out.back() == '_' ||
				out.back() == ' '))
		out.pop_back();
	while (!out.empty() && (out.front() == '.' || out.front() == ' '))
		out.erase(out.begin());

	// Cut on a character boundary, never mid-sequence: half a sequence is
	// not a name.
	if (out.size() > kMaxLength) {
		size_t cut = kMaxLength;
		while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80)
			cut--;
		out.resize(cut);
		while (!out.empty() && (out.back() == '.' || out.back() == '_'))
			out.pop_back();
	}

	if (isReservedDeviceName(out))
		out += "_";
	return out;
}

// May this name be joined onto the session folder? Deliberately a SEPARATE
// question from sanitize(): names also arrive from config.json and from the
// folders already on disk, and those were never passed through sanitize().
inline bool isSafeFolderName(const std::string &name)
{
	if (name.empty() || name.size() > 255)
		return false;
	if (name == "." || name == "..")
		return false;
	for (unsigned char c : name)
		if (isForbiddenAscii(c))
			return false;
	// A trailing dot or space is a name Windows will not give back.
	if (name.back() == '.' || name.back() == ' ')
		return false;
	return !isReservedDeviceName(name);
}

} // namespace project_name

} // namespace multireplay
