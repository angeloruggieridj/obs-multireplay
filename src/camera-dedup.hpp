/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Pure logic (no OBS/Qt/FFmpeg types, like master-timeline.hpp and
health-rules.hpp): which camera SLOT owns the Branch Output filter/encoder
for a source, when several slots are pointed at the same OBS source.

Two camera slots naming the same source are the SAME picture, so recording
it twice is not two angles — it is one angle encoded twice: two Branch
Output filters on that source, two files of identical footage, and two
QSV/NVENC/x264 sessions for bytes that are already being produced once. On
an iGPU with a limited number of concurrent hardware encode sessions, eight
slots pointed at one camera is eight sessions competing for a budget that
may only cover the first few: Branch Output's recording output for the
losing slots never goes active, so the packet tap can never attach to them
(see packet-tap.hpp), and everything downstream that reads a camera's
packets — a multiview tile, a scrub past the live edge, the ring itself —
stays exactly as empty as a camera nobody armed. That is the shape of "only
the first camera's preview ever shows a picture, the rest are black" bug
report this header exists to close.

So only the FIRST slot (lowest index) that names a given source is
CANONICAL: it is the only one branch-output-control.hpp ever creates,
enables, disables or prunes a filter for. Every later slot naming the same
source is a DUPLICATE — it owns no filter and no encoder of its own, and
reads for it (PacketTap's packet ring, SegmentIndex's on-disk segments) are
redirected to the canonical slot's, because that is the only place the
bytes actually exist. The duplicate slot still works exactly like a real
angle everywhere else — it can be marked, played, exported — it is simply
shown the canonical slot's footage, which is correct: it is the same
footage.
*/

#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace multireplay {

// For each slot, the index of the slot that OWNS the filter/encoder for its
// source: the smallest index naming the same non-empty source name. An empty
// source name is unowned and maps to itself — no filter is ever built for an
// unconfigured slot either way, so its "canonical index" is moot.
template <std::size_t N>
std::array<int, N>
canonicalCameraIndices(const std::array<std::string, N> &sourceNames)
{
	std::array<int, N> canonical{};
	for (std::size_t i = 0; i < N; i++) {
		canonical[i] = (int)i;
		if (sourceNames[i].empty())
			continue;
		for (std::size_t j = 0; j < i; j++) {
			if (!sourceNames[j].empty() &&
			    sourceNames[j] == sourceNames[i]) {
				canonical[i] = (int)j;
				break;
			}
		}
	}
	return canonical;
}

// True when slot i is its own canonical owner — the one that should carry a
// Branch Output filter, and the one PacketTap/SegmentIndex actually hold
// data under. False means "duplicate": reads for it must redirect to
// canonical[i] instead.
template <std::size_t N>
bool isCanonicalCamera(const std::array<int, N> &canonical, std::size_t i)
{
	return i < N && canonical[i] == (int)i;
}

} // namespace multireplay
