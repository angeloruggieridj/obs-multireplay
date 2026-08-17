/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

reverse_plan — which packets to decode, in which order, to play a range
BACKWARDS.

Reverse is not a decode direction. Every codec we get from Branch Output
(H.264/HEVC/AV1) predicts forward from a keyframe, so the only way to show the
frame before the one on screen is to have decoded it already. Reverse playback
is therefore a scheduling problem, not a decoding one: decode a GOP forward,
hold its pictures, then hand them to OBS newest-first.

The reason this file exists separately from the decoding is memory. A decoded
1080p picture is ~3.1 MB, so a whole 5-second clip is ~465 MB and will not be
held — which is exactly why the ring stores packets. What CAN be held is one
GOP: `keyint_sec = 1` (branch-output-control.cpp) makes that 30-50 pictures,
~155 MB at 1080p, and that number is the reverse cache the plan budgeted for.

So the schedule is: walk the GOPs from the last to the first; inside each one,
walk the frames from the last to the first in slices no larger than the cache
allows. A GOP bigger than the cache (some other encoder's keyint, a 4K stream)
is not refused and is not truncated — it is decoded more than once, each pass
keeping a different slice. Slower, still exact, and bounded in RAM, which is the
property that matters on a machine that is also encoding.

Pure logic on purpose (no OBS, no FFmpeg, like master-timeline.hpp and
health-rules.hpp): "what does a 3-GOP range 60 frames long turn into when only
25 pictures fit" is a question about arithmetic, and arithmetic is the part that
can be argued with cheaply in a unit test.
*/

#pragma once

#include "packet-types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace multireplay::reverse_plan {

// One pass of the decoder. Feed the packets in [decodeStart, decodeEnd) and keep
// the decoded pictures whose master instant falls in [keepFromNs, keepToNs]
// (both inclusive, both real frame instants). Those kept pictures are then shown
// newest-first.
struct Chunk {
	size_t decodeStart = 0; // index of the video keyframe opening the GOP
	size_t decodeEnd = 0;   // one past the last packet of that GOP
	int64_t keepFromNs = 0;
	int64_t keepToNs = 0;
	int frames = 0; // how many pictures this pass is expected to keep
};

// What the frame cache can hold. `frameBytes` is one decoded picture, measured
// from a real frame rather than guessed: the guess would have to assume the
// widest pixel format (I444, 3 bytes/px) to stay safe, which would halve the
// slice for the NV12 streams we actually get and double the number of decode
// passes for nothing.
struct Budget {
	size_t frameBytes = 0; // one decoded picture, 0 = unknown
	size_t maxBytes = 0;   // cache ceiling, 0 = unbounded
	int maxFrames = 0;     // hard picture cap, 0 = derive from maxBytes
};

// Pictures one pass may keep. 0 = unbounded (no budget given at all).
inline int framesPerChunk(const Budget &b)
{
	int cap = b.maxFrames > 0 ? b.maxFrames : 0;
	if (b.maxBytes > 0 && b.frameBytes > 0) {
		const size_t byBytes = b.maxBytes / b.frameBytes;
		// Never zero: a cache too small for one picture still has to show
		// one picture at a time rather than refuse the range. Running slow
		// is a degraded reverse; refusing is no reverse.
		int n = byBytes >= (size_t)INT32_MAX ? INT32_MAX : (int)byBytes;
		if (n < 1)
			n = 1;
		cap = cap > 0 ? std::min(cap, n) : n;
	}
	return cap;
}

// The passes to run, already in the order they must be shown in: newest slice
// of the newest GOP first, oldest slice of the oldest GOP last.
//
// `packets` is a resolved span (PacketRing::resolveRange / segment_reader), so
// it starts on the keyframe at or before the requested IN and ends at OUT.
// Frames outside [inNs, outNs] are decoded when they must be (to build up
// reference state) but never kept.
inline std::vector<Chunk> plan(const std::vector<LivePacket> &packets,
			       int64_t inNs, int64_t outNs, const Budget &budget)
{
	std::vector<Chunk> out;
	if (packets.empty() || outNs < inNs)
		return out;

	const int per = framesPerChunk(budget);

	// A GOP begins at every video keyframe. Anything before the first
	// keyframe cannot be decoded at all, so it belongs to no GOP and is
	// outside the plan — resolveRange already guarantees the span opens on
	// one, and a span that does not is a span with no reverse.
	struct Gop {
		size_t begin = 0;
		size_t end = 0;
		std::vector<int64_t> wanted;
	};
	std::vector<Gop> gops;

	for (size_t i = 0; i < packets.size(); i++) {
		const LivePacket &p = packets[i];
		if (p.kind != PacketKind::Video)
			continue;
		if (p.keyframe) {
			gops.push_back(Gop{i, packets.size(), {}});
			if (gops.size() > 1)
				gops[gops.size() - 2].end = i;
		}
		if (gops.empty())
			continue;
		if (p.masterNs >= inNs && p.masterNs <= outNs)
			gops.back().wanted.push_back(p.masterNs);
	}

	for (size_t g = gops.size(); g-- > 0;) {
		// Presentation order, which is NOT the order the packets sit in:
		// with B-frames the ring holds them in decode order. Reverse is
		// entirely about presentation order, so sort here rather than
		// trusting the stream to be free of reordering.
		std::vector<int64_t> w = gops[g].wanted;
		std::sort(w.begin(), w.end());
		if (w.empty())
			continue;

		size_t hi = w.size(); // exclusive
		while (hi > 0) {
			const size_t lo = (per > 0 && hi > (size_t)per)
						  ? hi - (size_t)per
						  : 0;
			Chunk c;
			c.decodeStart = gops[g].begin;
			c.decodeEnd = gops[g].end;
			c.keepFromNs = w[lo];
			c.keepToNs = w[hi - 1];
			c.frames = (int)(hi - lo);
			out.push_back(c);
			hi = lo;
		}
	}

	return out;
}

// Pictures the whole plan will show. Used for logging and for the gate: a plan
// that quietly drops half a range is the failure mode worth naming.
inline int plannedFrames(const std::vector<Chunk> &chunks)
{
	int n = 0;
	for (const Chunk &c : chunks)
		n += c.frames;
	return n;
}

} // namespace multireplay::reverse_plan
