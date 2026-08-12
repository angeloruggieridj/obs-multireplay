/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

MasterTimeline — turns one encoder's private timestamps into positions on a
clock shared by every encoder in the session.

This is the piece that removes ReplayCore::frameLagNs(). The old design guessed
how far the recording lagged the wall clock by watching when the first file
appeared on disk, so cross-angle alignment was an estimate and drifted. Here the
alignment is measured: libobs stamps every encoded packet with `sys_dts_usec`
(obs-encoder.h:120), the system time of that packet's DTS, on the same clock for
every encoder. Each encoder still numbers its own packets from its own zero, so
we learn the constant offset between the two once and apply it:

    masterNs = ptsNs + (sysDtsNs - dtsNs)

Keeping PTS (rather than just using sysDts directly) preserves presentation
order if B-frames are ever re-enabled, while still landing on the shared clock.

One instance per TRACK, not per camera: a camera's video and audio come from two
different encoders, each with its own zero, and both map onto the same clock.

Header-only and free of OBS types so it can be unit-tested anywhere.
*/

#pragma once

#include <cstdint>

namespace multireplay {

// Offset drift beyond this is treated as a discontinuity rather than jitter.
// Comfortably above encoder pipeline jitter (measured ~138 ms worst case on
// QSV) and far below anything a human would perceive as a seam.
inline constexpr int64_t kTimelineDriftToleranceNs = 400'000'000;

// Rescale `value` from `num/den` units to nanoseconds without overflowing on
// long sessions: whole units first, remainder second.
inline bool rescaleToNs(int64_t value, int32_t num, int32_t den, int64_t &out)
{
	if (num <= 0 || den <= 0)
		return false;
	const int64_t whole = value / den;
	const int64_t rem = value % den;
	out = whole * (int64_t)num * 1'000'000'000LL +
	      rem * (int64_t)num * 1'000'000'000LL / den;
	return true;
}

class MasterTimeline {
public:
	struct Input {
		int64_t pts = 0;
		int64_t dts = 0;
		int32_t timebaseNum = 0;
		int32_t timebaseDen = 0;
		int64_t sysDtsUsec = 0;
	};

	struct Result {
		bool ok = false;           // false = malformed, drop the packet
		int64_t masterNs = 0;
		int64_t dtsNs = 0;
		uint32_t generation = 0;
		bool discontinuity = false; // this packet opened a new generation
	};

	Result normalize(const Input &in)
	{
		Result r;

		int64_t ptsNs = 0, dtsNs = 0;
		if (!rescaleToNs(in.pts, in.timebaseNum, in.timebaseDen, ptsNs))
			return r; // zero/negative timebase: unusable
		if (!rescaleToNs(in.dts, in.timebaseNum, in.timebaseDen, dtsNs))
			return r;
		if (in.sysDtsUsec <= 0)
			return r; // no shared-clock stamp: cannot be aligned

		const int64_t sysDtsNs = in.sysDtsUsec * 1000;
		const int64_t observed = sysDtsNs - dtsNs;

		if (!haveOffset_) {
			offsetNs_ = observed;
			haveOffset_ = true;
			r.discontinuity = true; // first packet opens generation 0
		} else {
			int64_t drift = observed - offsetNs_;
			if (drift < 0)
				drift = -drift;
			if (drift > kTimelineDriftToleranceNs) {
				// The encoder restarted or the clock jumped: the
				// old and new packets are not on one continuous
				// timeline, so say so instead of silently
				// producing a misaligned marker.
				offsetNs_ = observed;
				++generation_;
				lastMasterNs_ = INT64_MIN;
				r.discontinuity = true;
			}
		}

		r.ok = true;
		r.masterNs = ptsNs + offsetNs_;
		r.dtsNs = dtsNs + offsetNs_;
		r.generation = generation_;
		if (r.masterNs > lastMasterNs_)
			lastMasterNs_ = r.masterNs;
		return r;
	}

	// Forget the learned offset; the next packet starts a new generation.
	void reset()
	{
		haveOffset_ = false;
		offsetNs_ = 0;
		lastMasterNs_ = INT64_MIN;
		++generation_;
	}

	uint32_t generation() const { return generation_; }
	bool aligned() const { return haveOffset_; }
	int64_t offsetNs() const { return offsetNs_; }
	int64_t newestMasterNs() const { return lastMasterNs_; }

private:
	bool haveOffset_ = false;
	int64_t offsetNs_ = 0;
	int64_t lastMasterNs_ = INT64_MIN;
	uint32_t generation_ = 0;
};

} // namespace multireplay
