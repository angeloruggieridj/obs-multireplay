/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See packet-ring.hpp.
*/

#include "packet-ring.hpp"

#include <algorithm>

namespace multireplay {

void PacketRing::setLimits(const RingLimits &limits)
{
	limits_ = limits;
	evict();
}

void PacketRing::push(LivePacket &&p)
{
	// Video and audio interleave, so the last pushed packet is not
	// necessarily the latest in presentation order — but the newest is only
	// ever advanced by a push, and eviction only removes from the front, so
	// tracking it here keeps push O(1) instead of rescanning the deque on
	// every packet.
	newestNs_ = std::max(newestNs_, p.masterNs);
	bytes_ += p.bytes();
	packets_.push_back(std::move(p));
	++pushed_;
	evict();
}

void PacketRing::clear()
{
	packets_.clear();
	bytes_ = 0;
	newestNs_ = 0;
}

int64_t PacketRing::oldestNs() const
{
	return packets_.empty() ? 0 : packets_.front().masterNs;
}

int64_t PacketRing::newestNs() const
{
	return packets_.empty() ? 0 : newestNs_;
}

int64_t PacketRing::spanNs() const
{
	if (packets_.empty())
		return 0;
	return newestNs() - oldestNs();
}

bool PacketRing::oldestKeyframeNs(int64_t &out) const
{
	for (const auto &p : packets_) {
		if (p.kind == PacketKind::Video && p.keyframe) {
			out = p.masterNs;
			return true;
		}
	}
	return false;
}

void PacketRing::evict()
{
	if (packets_.empty())
		return;

	const auto overLimit = [&]() {
		if (limits_.maxBytes && bytes_ > limits_.maxBytes)
			return true;
		if (limits_.maxDurationNs && spanNs() > limits_.maxDurationNs)
			return true;
		return false;
	};

	while (overLimit()) {
		// Drop whole GOPs only: the ring must always begin at a video
		// keyframe, otherwise its oldest seconds are held but not
		// decodable, which is worse than not holding them at all.
		size_t nextKey = 0;
		bool found = false;
		for (size_t i = 1; i < packets_.size(); i++) {
			const auto &p = packets_[i];
			if (p.kind == PacketKind::Video && p.keyframe) {
				nextKey = i;
				found = true;
				break;
			}
		}
		if (!found)
			break; // a single GOP over budget: keep it, stay decodable

		for (size_t i = 0; i < nextKey; i++)
			bytes_ -= packets_[i].bytes();
		packets_.erase(packets_.begin(),
			       packets_.begin() + (std::ptrdiff_t)nextKey);
		evicted_ += nextKey;
	}
}

bool PacketRing::resolveRange(int64_t inNs, int64_t outNs, ResolvedRange &out) const
{
	if (packets_.empty() || outNs < inNs)
		return false;
	if (inNs < packets_.front().masterNs)
		return false; // already evicted
	if (outNs > newestNs())
		return false; // beyond the live edge: not captured yet

	// Latest video keyframe at or before IN.
	bool haveKey = false;
	size_t decodeStart = 0;
	for (size_t i = 0; i < packets_.size(); i++) {
		const auto &p = packets_[i];
		if (p.kind != PacketKind::Video || !p.keyframe)
			continue;
		if (p.masterNs > inNs)
			break;
		decodeStart = i;
		haveKey = true;
	}
	if (!haveKey)
		return false;

	const uint32_t gen = packets_[decodeStart].generation;

	// Walk forward to OUT. Video and audio interleave so presentation times
	// are not strictly monotonic across kinds; scan the whole tail rather
	// than stopping at the first packet past OUT.
	bool haveIn = false, haveOut = false;
	size_t last = decodeStart;
	int64_t presentIn = 0, presentOut = 0;

	for (size_t i = decodeStart; i < packets_.size(); i++) {
		const auto &p = packets_[i];
		if (p.masterNs > outNs)
			continue;
		// A range that crosses a discontinuity is not one continuous
		// piece of footage, so refuse it instead of splicing.
		if (p.generation != gen)
			return false;
		last = std::max(last, i);
		if (p.kind != PacketKind::Video)
			continue;
		if (p.masterNs >= inNs && (!haveIn || p.masterNs < presentIn)) {
			presentIn = p.masterNs;
			haveIn = true;
		}
		if (!haveOut || p.masterNs > presentOut) {
			presentOut = p.masterNs;
			haveOut = true;
		}
	}

	if (!haveIn || !haveOut)
		return false; // no video actually covers the requested range

	out.decodeStart = decodeStart;
	out.last = last;
	out.decodeStartNs = packets_[decodeStart].masterNs;
	out.presentInNs = presentIn;
	out.presentOutNs = presentOut;
	out.generation = gen;
	return true;
}

} // namespace multireplay
