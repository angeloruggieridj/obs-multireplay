/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

TimelineMap — the position bar's axis is FOOTAGE, not wall time.

A session is not one continuous recording. The operator records two minutes,
stops, talks to somebody for a minute, records again. On a wall-time axis that
third minute is a hole: the bar grows to three minutes, one of them scrubs to
nothing, and the total reads 3:00 for two minutes of material. That is what
this maps away. The axis is the sum of the recorded spans:

    footage time  0 ─────────────── 2:00 ─────────────── 4:00
    master time   take 1 (0..2:00)  │  take 2 (3:00..5:00)
                                    └── the minute nobody recorded is not there

Every position on the bar is therefore an instant real footage covers, and
"seek to 50%" lands on a frame instead of on silence.

Two rules follow:
  - a gap is a JOIN, not a jump: the instants either side of it are adjacent on
    the bar, which is how an operator thinks about "the recording";
  - an instant inside a gap has no position of its own. Rather than invent one,
    fractionOf() reports the join — the point where the missing time was cut
    out — and covers() lets the caller ask first.

Spans closer together than kJoinToleranceNs are one span: a 20-minute split
makes two files whose ends meet within a frame, and a seam drawn there would be
a lie about the recording.

Header-only and free of OBS types, so it is unit-tested with the rest of the
pure logic.
*/

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace multireplay {

// Two recordings this close together are one recording (file splits, and the
// gap between one file's last frame and the next file's first).
inline constexpr int64_t kJoinToleranceNs = 1'500'000'000;

struct TimelineSpan {
	int64_t startNs = 0;
	int64_t endNs = 0;
	int64_t lengthNs() const { return endNs > startNs ? endNs - startNs : 0; }
};

class TimelineMap {
public:
	// Sorts, drops the empty ones and merges what is adjacent. A span whose
	// end the caller could not measure must be left out by the caller: this
	// class does not guess.
	void setSpans(std::vector<TimelineSpan> spans)
	{
		spans.erase(std::remove_if(spans.begin(), spans.end(),
					   [](const TimelineSpan &s) {
						   return s.endNs <= s.startNs;
					   }),
			    spans.end());
		std::sort(spans.begin(), spans.end(),
			  [](const TimelineSpan &a, const TimelineSpan &b) {
				  return a.startNs < b.startNs;
			  });
		spans_.clear();
		for (const auto &s : spans) {
			if (!spans_.empty() &&
			    s.startNs <= spans_.back().endNs + kJoinToleranceNs) {
				spans_.back().endNs =
					std::max(spans_.back().endNs, s.endNs);
				continue;
			}
			spans_.push_back(s);
		}
		total_ = 0;
		for (const auto &s : spans_)
			total_ += s.lengthNs();
	}

	void clear()
	{
		spans_.clear();
		total_ = 0;
	}

	bool empty() const { return spans_.empty() || total_ <= 0; }
	int64_t totalNs() const { return total_; }
	size_t spanCount() const { return spans_.size(); }
	const std::vector<TimelineSpan> &spans() const { return spans_; }

	int64_t firstNs() const { return spans_.empty() ? 0 : spans_.front().startNs; }
	int64_t lastNs() const { return spans_.empty() ? 0 : spans_.back().endNs; }

	bool covers(int64_t masterNs) const
	{
		for (const auto &s : spans_)
			if (masterNs >= s.startNs && masterNs < s.endNs)
				return true;
		return false;
	}

	// How much footage precedes this instant. An instant inside a gap counts
	// as the join (all the footage before it, none of the gap); one before
	// the beginning is 0, one past the end is the whole thing.
	int64_t footageBefore(int64_t masterNs) const
	{
		int64_t acc = 0;
		for (const auto &s : spans_) {
			if (masterNs <= s.startNs)
				return acc;
			if (masterNs >= s.endNs) {
				acc += s.lengthNs();
				continue;
			}
			return acc + (masterNs - s.startNs);
		}
		return acc;
	}

	// Position on the bar, [0,1]. -1 only when there is no timeline at all —
	// callers draw nothing then, rather than drawing zero.
	double fractionOf(int64_t masterNs) const
	{
		if (total_ <= 0)
			return -1.0;
		const double f = (double)footageBefore(masterNs) / (double)total_;
		return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
	}

	// ...and back. The inverse of fractionOf on every instant footage covers,
	// which is every instant this can return.
	int64_t instantAt(double frac) const
	{
		if (spans_.empty())
			return 0;
		if (frac < 0.0)
			frac = 0.0;
		if (frac > 1.0)
			frac = 1.0;
		int64_t want = (int64_t)(frac * (double)total_);
		for (const auto &s : spans_) {
			const int64_t len = s.lengthNs();
			if (want < len)
				return s.startNs + want;
			want -= len;
		}
		return spans_.back().endNs;
	}

	// The same conversion for a range, which is what an event marker is.
	// False when the range lands nowhere on the bar.
	bool rangeFraction(int64_t inNs, int64_t outNs, double &inFrac,
			   double &outFrac) const
	{
		if (total_ <= 0 || outNs <= inNs)
			return false;
		inFrac = fractionOf(inNs);
		outFrac = fractionOf(outNs);
		return outFrac > inFrac;
	}

private:
	std::vector<TimelineSpan> spans_;
	int64_t total_ = 0;
};

} // namespace multireplay
