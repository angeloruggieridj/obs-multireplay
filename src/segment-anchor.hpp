/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Anchoring a Branch Output recording file to the master timeline, exactly.

Replay has to reach back over the whole recording, not just the RAM window, so
the older material has to come from the files Branch Output writes. The problem
is that those files carry their own timestamps: a muxer rebases the first packet
to zero, so file time says nothing about when the footage actually happened.
Guessing that offset from file mtimes or from when REC was pressed is precisely
the kind of estimate this rewrite exists to delete.

The exact answer needs no per-packet index on disk. A file is anchored at the
moment it appears, while its opening packets are still in the live ring: the
sizes of consecutive encoded packets form a fingerprint that is effectively
unique, so matching the file's first few packet sizes against the ring yields
the master time of file-time zero. After that, for anything in that file:

    fileTimeNs = masterNs - anchorMasterNs

which holds exactly, because both sides derive from the same encoder's packet
sequence and the muxer's rebasing is a single constant per file.

This header is the matching itself: pure, no OBS, no FFmpeg, so the property
that matters ("a wrong anchor is never returned") can be tested directly.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace multireplay {

// One candidate from the live ring: a video packet's size and when it happened.
struct AnchorSample {
	int64_t masterNs = 0;
	uint32_t size = 0;
};

// How many consecutive packet sizes must agree before we believe a match.
// Encoded frame sizes vary enough that a run of this length collides only by
// accident; on a static test pattern (where every frame compresses to nearly
// the same size) many runs tie, and ambiguity is then reported rather than
// guessed at.
inline constexpr size_t kAnchorFingerprintLen = 12;

enum class AnchorResult {
	Found,
	TooFewSamples,  // not enough ring history or file packets to compare
	NotFound,       // the file's opening is not in the ring (anchor too late)
	Ambiguous,      // several equally good matches: refuse rather than pick
};

// Find where `fileSizes` (the sizes of the file's first video packets, in order)
// occurs inside `ring` (consecutive video packets, in order). On success
// `anchorMasterNsOut` is the master time of the file's first video packet.
inline AnchorResult findAnchor(const std::vector<AnchorSample> &ring,
			       const std::vector<uint32_t> &fileSizes,
			       int64_t &anchorMasterNsOut)
{
	const size_t need = kAnchorFingerprintLen;
	if (fileSizes.size() < need || ring.size() < need)
		return AnchorResult::TooFewSamples;

	size_t matches = 0;
	size_t firstMatch = 0;

	const size_t last = ring.size() - need;
	for (size_t i = 0; i <= last; i++) {
		bool ok = true;
		for (size_t k = 0; k < need; k++) {
			if (ring[i + k].size != fileSizes[k]) {
				ok = false;
				break;
			}
		}
		if (!ok)
			continue;
		if (matches == 0)
			firstMatch = i;
		if (++matches > 1)
			return AnchorResult::Ambiguous;
	}

	if (matches == 0)
		return AnchorResult::NotFound;

	anchorMasterNsOut = ring[firstMatch].masterNs;
	return AnchorResult::Found;
}

} // namespace multireplay
