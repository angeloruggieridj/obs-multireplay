/*
obs-multireplay — which known failure a core error string is, if any
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

The engine logs in English on purpose — the log is for a ticket, and a ticket
travels better in one language — but the same strings used to reach the
operator's panel verbatim through showNotice(QString::fromStdString(err)), so
an Italian dock could say "no music is configured: set a music file or a
music source in Settings" in the middle of a match.

Pure classification, so it can be tested without OBS or Qt: this file only
recognises WHICH known failure a string is, by the substring each one is
built around in playback-coordinator.cpp/export.cpp. It says nothing about
what to show — that is a locale lookup, which needs obs_module_text, which
needs the plugin running — and it never rewrites or discards the original
text: an error this cannot classify is not this file's failure, it is one
more class to add.
*/

#pragma once

#include <string>

namespace multireplay {

namespace error_locale {

enum class Class {
	Unknown, // no recognised class — the caller shows the raw text as-is
	NoFootageForCamera,   // "camera N has no footage for this event..."
	NoPlayableEvents,     // "no playable (completed) events selected"
	NoFootageInRange,     // "no footage in that range"
	NoCompletedEvents,    // "no completed events yet"
	MusicFileMissing,     // "the music file does not exist: ..."
	MusicNotConfigured,   // "no music is configured: ..."
	MusicSourceMissing,   // "...does not exist in this scene collection"
	MusicSourceNotActive, // "...is not in an active scene..."
};

inline Class classify(const std::string &err)
{
	// Longest/most specific substrings first: "no footage in that range"
	// and "no music is configured" do not collide with anything else, but
	// matching order still matters wherever one message could contain
	// another's marker.
	if (err.rfind("camera ", 0) == 0 &&
	    err.find("has no footage for this event") != std::string::npos)
		return Class::NoFootageForCamera;
	if (err == "no playable (completed) events selected")
		return Class::NoPlayableEvents;
	if (err == "no footage in that range")
		return Class::NoFootageInRange;
	if (err == "no completed events yet")
		return Class::NoCompletedEvents;
	if (err.rfind("the music file does not exist", 0) == 0)
		return Class::MusicFileMissing;
	if (err.rfind("no music is configured", 0) == 0)
		return Class::MusicNotConfigured;
	if (err.find("does not exist in this scene collection") !=
	    std::string::npos)
		return Class::MusicSourceMissing;
	if (err.find("is not in an active scene") != std::string::npos)
		return Class::MusicSourceNotActive;
	return Class::Unknown;
}

} // namespace error_locale

} // namespace multireplay
