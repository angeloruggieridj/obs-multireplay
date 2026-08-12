/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

SegmentReader — the same clip, fetched from Branch Output's files instead of the
live ring.

Once SegmentIndex has anchored a file, reading from it is ordinary demuxing: seek
to the keyframe at or before the requested IN and read forward, stamping every
packet with anchor + its in-file timestamp. The packets come out as the very same
LivePacket the ring produces, on the very same master clock, so everything
downstream - decoder, pre-roll trimming, pacing, the OBS input - is untouched and
cannot tell the two apart. That is the point: one playback path, two sources.

The stream configuration comes from the file rather than from the encoder,
because a muxer rewrites it: MP4 carries H.264 parameter sets in avcC form,
while the encoder handed us Annex B. Feeding the encoder's version to a decoder
reading file packets would be subtly wrong.
*/

#pragma once

#include "packet-types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace multireplay {

namespace segment_reader {

// Collect [inNs, outNs] for `camIndex` from the anchored files, crossing a file
// boundary if the range spans one. False when no anchored segment covers the
// range — the caller then has nothing to play, which is the honest answer.
bool readRange(int camIndex, int64_t inNs, int64_t outNs,
	       std::vector<LivePacket> &packetsOut, StreamConfig &configOut,
	       int64_t &presentInNs, int64_t &presentOutNs,
	       std::string &errorOut);

} // namespace segment_reader

} // namespace multireplay
