/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

PacketRing — the bounded, in-memory history of one camera's encoded packets.

This is what makes the replay live edge live. The old design read the MP4 that
Branch Output was still writing, so the newest replayable moment was whatever
had last been flushed to a fragment (~1 s), and the container's duration was
cached at open. Here the newest replayable moment is whatever the encoder just
emitted — measured at ~138 ms on QSV — because it never touches the filesystem.

The ring holds ENCODED packets, so it is cheap: at 12 Mbps a 90-second window
costs ~135 MB per camera, and eight cameras still fit comfortably in RAM.
Anything older than the ring is served from Branch Output's files instead
(that reader lands in M2); the ring is authoritative for the live edge, the
files for history.

Deliberately NOT thread-safe and free of OBS types: it is a plain data structure
so it can be unit-tested anywhere, and the owner (PacketTap) holds the lock. The
encoder thread must never block for long, so the owner's critical sections stay
short.
*/

#pragma once

#include "packet-types.hpp"

#include <cstddef>
#include <deque>

namespace multireplay {

struct RingLimits {
	size_t maxBytes = 0;       // 0 = unbounded
	int64_t maxDurationNs = 0; // 0 = unbounded
};

// Where to start decoding, and what the range actually covers.
struct ResolvedRange {
	size_t decodeStart = 0; // index of the video keyframe to decode from
	size_t last = 0;        // last index inside the range (inclusive)
	int64_t decodeStartNs = 0;
	int64_t presentInNs = 0;  // first video frame at or after the requested IN
	int64_t presentOutNs = 0; // last video frame at or before the requested OUT
	uint32_t generation = 0;
};

class PacketRing {
public:
	PacketRing() = default;
	explicit PacketRing(const RingLimits &limits) : limits_(limits) {}

	void setLimits(const RingLimits &limits);
	RingLimits limits() const { return limits_; }

	void push(LivePacket &&p);
	void clear();

	// Resolve [inNs, outNs] to a decodable span.
	//
	// Strict on purpose: it returns false rather than clamping when the
	// request starts before what we still hold, runs past the live edge,
	// has no keyframe to decode from, or would straddle a discontinuity. A
	// silently clamped replay is a wrong replay on air, and the old engine's
	// habit of clamping is exactly what made misalignment hard to see.
	bool resolveRange(int64_t inNs, int64_t outNs, ResolvedRange &out) const;

	const LivePacket &at(size_t i) const { return packets_[i]; }
	size_t size() const { return packets_.size(); }
	bool empty() const { return packets_.empty(); }
	size_t bytes() const { return bytes_; }

	int64_t oldestNs() const;
	int64_t newestNs() const;
	int64_t spanNs() const;
	// Earliest instant that can actually be replayed (first video keyframe).
	bool oldestKeyframeNs(int64_t &out) const;

	uint64_t pushedPackets() const { return pushed_; }
	uint64_t evictedPackets() const { return evicted_; }

private:
	void evict();

	std::deque<LivePacket> packets_;
	RingLimits limits_;
	size_t bytes_ = 0;
	int64_t newestNs_ = 0; // maintained on push; eviction only trims the front
	uint64_t pushed_ = 0;
	uint64_t evicted_ = 0;
};

} // namespace multireplay
