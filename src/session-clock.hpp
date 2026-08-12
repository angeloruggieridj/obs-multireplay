/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

The two clocks, and the one pair of numbers that ties them together.

Everything the live engine does is on `masterNs`: the monotonic system clock
(os_gettime_ns), the same one libobs stamps on every encoded packet as
sys_dts_usec. It is exact, it never jumps, every angle agrees on it — and it
means absolutely nothing once the process is gone, because it counts from an
arbitrary origin (boot, typically) that the next run will not share.

So anything that has to outlive the process — an event marker, a file's anchor —
is stored in wall-clock nanoseconds since the Unix epoch instead. Wall time is
the opposite trade: portable across restarts, but liable to jump under NTP, so
it is never used for anything within a session.

The bridge is a single pair sampled at one instant, the session epoch:

    epochMaster = os_gettime_ns()
    epochWall   = system_clock::now() since 1970, in ns

and then, for any instant:

    wallNs   = epochWall   + (masterNs - epochMaster)
    masterNs = epochMaster + (wallNs   - epochWall)

Note the bracketing: it is not cosmetic. `epochWall - epochMaster + masterNs`
computes the same value mathematically but goes through an intermediate near
1.8e18 (wall ns) that can be pushed over INT64_MAX by a large monotonic value;
subtracting the two same-clock terms FIRST keeps the intermediate down at the
size of an offset. Both helpers below are written that way on purpose.

Header-only and free of OBS types, so the conversions can be unit-tested
directly — they are the whole reason a project recorded yesterday still lines
up with its footage today.
*/

#pragma once

#include <cstdint>

namespace multireplay {

// One instant, sampled on both clocks at once. All-zero = "not seated": the
// conversions then degenerate to the identity, which is what a context with no
// session (the standalone tests) wants.
struct SessionEpoch {
	int64_t masterNs = 0; // os_gettime_ns() at that instant
	int64_t wallNs = 0;   // ns since the Unix epoch at that same instant
};

inline int64_t masterToWallNs(const SessionEpoch &epoch, int64_t masterNs)
{
	return epoch.wallNs + (masterNs - epoch.masterNs);
}

inline int64_t wallToMasterNs(const SessionEpoch &epoch, int64_t wallNs)
{
	return epoch.masterNs + (wallNs - epoch.wallNs);
}

} // namespace multireplay
