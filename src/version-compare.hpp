/*
obs-multireplay — comparing plugin versions
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Header-only and PURE: no OBS types, no network, no allocation beyond the strings
handed in. Like master-timeline.hpp, health-rules.hpp, reverse-plan.hpp and
audio-stretch.hpp, it is written this way so it can be unit tested — and the
thing it decides is exactly the thing that cannot be tested by running the
plugin, because testing it for real means publishing a release.

WHAT IT HAS TO GET RIGHT, and why each one is a way an updater embarrasses
itself in front of an operator:

  - 1.0.0 is NEWER than 1.0.0-beta2. A pre-release precedes the release it is a
    pre-release OF; an updater that thinks otherwise offers to "update" a stable
    install back onto the beta it came from.
  - 1.0.0-beta10 is newer than 1.0.0-beta2. Compared as text, "beta10" sorts
    before "beta2", and the tenth beta would be invisible to everyone running
    the second.
  - 1.10.0 is newer than 1.9.0. Same trap one level up.
  - Anything it cannot parse is NOT newer. An updater that offers an update it
    invented from a malformed tag is worse than one that says nothing, and the
    tag comes off the network.
*/

#pragma once

#include <cstdint>
#include <string>

namespace multireplay {

struct Version {
	int major = -1; // -1 = did not parse
	int minor = 0;
	int patch = 0;
	// 0 = a final release, N = the Nth pre-release of it. A final release is
	// LATER than every pre-release of the same numbers, which is why this is
	// not simply "bigger is newer" — see compare().
	int pre = 0;

	bool valid() const { return major >= 0; }
};

namespace detail {

// Read digits at `i`, advancing it. Returns -1 when there are none, which is how
// a malformed tag is refused rather than read as zero.
inline int readInt(const std::string &s, size_t &i)
{
	if (i >= s.size() || s[i] < '0' || s[i] > '9')
		return -1;
	int64_t v = 0;
	while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
		v = v * 10 + (s[i] - '0');
		if (v > 1000000) // a version number, not a build id
			return -1;
		i++;
	}
	return (int)v;
}

} // namespace detail

// "1.0.0", "1.0.0-beta2", "v1.2.3" → a Version. Anything else is invalid.
//
// The leading "v" is accepted because half the world tags releases that way and
// this project does not; a release published by hand with the other convention
// must not become invisible.
inline Version parseVersion(const std::string &raw)
{
	Version v;
	size_t i = 0;
	if (i < raw.size() && (raw[i] == 'v' || raw[i] == 'V'))
		i++;

	const int ma = detail::readInt(raw, i);
	if (ma < 0)
		return Version{};
	if (i >= raw.size() || raw[i] != '.')
		return Version{};
	i++;
	const int mi = detail::readInt(raw, i);
	if (mi < 0)
		return Version{};
	if (i >= raw.size() || raw[i] != '.')
		return Version{};
	i++;
	const int pa = detail::readInt(raw, i);
	if (pa < 0)
		return Version{};

	int pre = 0;
	if (i < raw.size()) {
		// Only "-beta<N>" is understood, because it is the only
		// pre-release form this project publishes. Anything else is not
		// guessed at: an unrecognised suffix makes the whole tag invalid,
		// so it can never be offered as an update.
		static const char kBeta[] = "-beta";
		const size_t n = sizeof(kBeta) - 1;
		if (raw.compare(i, n, kBeta) != 0)
			return Version{};
		i += n;
		pre = detail::readInt(raw, i);
		if (pre < 1)
			return Version{};
		if (i != raw.size())
			return Version{};
	}

	v.major = ma;
	v.minor = mi;
	v.patch = pa;
	v.pre = pre;
	return v;
}

// -1 / 0 / +1, in the usual order. Both must be valid; the caller decides what
// an invalid one means (isNewerVersion says "not newer", which is the safe
// reading for something that arrived over the network).
inline int compare(const Version &a, const Version &b)
{
	if (a.major != b.major)
		return a.major < b.major ? -1 : 1;
	if (a.minor != b.minor)
		return a.minor < b.minor ? -1 : 1;
	if (a.patch != b.patch)
		return a.patch < b.patch ? -1 : 1;
	if (a.pre == b.pre)
		return 0;
	// THE ONE THAT IS NOT ARITHMETIC. 0 means "final", and a final release is
	// newer than every pre-release of the same numbers — so the zero sorts
	// ABOVE the others rather than below them.
	if (a.pre == 0)
		return 1;
	if (b.pre == 0)
		return -1;
	return a.pre < b.pre ? -1 : 1;
}

// Is `candidate` something the operator running `current` should be offered?
// False whenever either side cannot be read: an update nobody can name is an
// update nobody should be asked to install.
inline bool isNewerVersion(const std::string &candidate,
			   const std::string &current)
{
	const Version c = parseVersion(candidate);
	const Version r = parseVersion(current);
	if (!c.valid() || !r.valid())
		return false;
	return compare(c, r) > 0;
}

} // namespace multireplay
