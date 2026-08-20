/*
obs-multireplay — one way to turn a path into bytes, and one way back
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

WHY THIS FILE EXISTS AT ALL, given that it is four lines of code.

`std::filesystem::path::string()` is not "the path as text". On MSVC it is the
path narrowed to the ACTIVE ANSI CODE PAGE, and every character the code page
cannot express is replaced or dropped. Everything this plugin hands a path to
wants UTF-8 instead: `os_fopen` and `obs_data_save_json_safe` convert UTF-8 to
wide characters themselves (libobs, os_utf8_to_wcs), and FFmpeg's `file_open`
does the same. So `os_fopen(p.string().c_str())` on Windows is a UTF-8 API being
fed code-page bytes.

The conversion the other way is the same mistake mirrored: `fs::path(s)` where
`s` holds UTF-8 interprets those bytes as ANSI, so a session folder read out of
`config.json` (obs_data is UTF-8) becomes a different folder.

The bug this closes is invisible on Linux and macOS — there `string()` IS UTF-8,
so the same code is correct — which is exactly why it survived a CI that builds
on three platforms: a session folder or a project called `Partita_Città` broke
recording, `anchors.json`, `events.json` and export on Windows only, and nothing
outside Windows could see it.

Pure: no OBS, no Qt, no FFmpeg. Same rule as master-timeline.hpp — so the
round trip can be unit tested where the plugin cannot be built.
*/

#pragma once

#include <filesystem>
#include <string>

namespace multireplay {

// The path as UTF-8 bytes, on every platform.
inline std::string pathToUtf8(const std::filesystem::path &p)
{
	const std::u8string u8 = p.u8string();
	return std::string(reinterpret_cast<const char *>(u8.c_str()),
			   u8.size());
}

// A path built from UTF-8 bytes, on every platform.
inline std::filesystem::path utf8ToPath(const std::string &s)
{
	return std::filesystem::path(
		std::u8string(reinterpret_cast<const char8_t *>(s.c_str()),
			      s.size()));
}

// `a / b`, in and out as UTF-8. The join most call sites actually wanted, so
// that neither end of it can go through the code page by accident.
inline std::string joinUtf8(const std::string &a, const std::string &b)
{
	return pathToUtf8(utf8ToPath(a) / utf8ToPath(b));
}

} // namespace multireplay
