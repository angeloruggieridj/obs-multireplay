/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

AudioStretch — slow motion WITH SOUND, at the right pitch.

This is the reason a replay used to go silent the moment it left 1x. Slow motion
in this engine is nothing but wider spacing between frames (see the pacing note
in replay-channel.hpp), and the same trick does not work on sound: stretching the
timestamps of decoded audio plays it low and with holes in it, because the samples
themselves still describe one second of air per second of samples. swresample —
already a dependency — resamples, which changes the pitch by exactly the factor
you were trying to hide. What is needed is a time stretch that leaves the pitch
alone, and that is a different algorithm, not a different parameter.

WSOLA is that algorithm (waveform-similarity overlap-add), and it is arithmetic:
cut the input into overlapping blocks, lay them back down at a different spacing,
and — the part that makes it sound like sound rather than like a stutter — slide
each block within a small search window to the position where it best matches what
has already been written, so the waveform joins up in phase instead of cancelling.
A period of a 100 Hz tone is 10 ms; a block laid down half a period out is a click.

Three properties this file exists to hold, all of them testable without OBS,
FFmpeg, a sound card, or a machine that has any of them:
  1. LENGTH: n input frames at speed s become n/s output frames (± one block).
     That is what keeps the sound under the picture: the video pacing stretches
     the same span by the same factor, so both arrive at the same instant.
  2. PITCH: the output's dominant frequency is the input's. This is the whole
     point, and the one property a listener notices instantly.
  3. 1x IS UNTOUCHED. At speed 1 the samples pass through unchanged — no search,
     no cross-fade, no chance of colouring the ordinary case, which is the case
     that is on air most of the time.

Header-only and free of OBS/FFmpeg types, like master-timeline.hpp, health-rules
and reverse-plan: the parts that can be argued about in a unit test live where a
unit test can reach them.

PLANAR FLOAT ONLY, deliberately. AAC decodes to planar float and that is what
Branch Output's encoders produce, so this covers the real path; anything else is
refused by configure() and the caller stays silent rather than guessing at a
format. ONE offset is searched for and applied to EVERY channel — a per-channel
offset would move the channels relative to each other, which on a stereo pair is
audible as the image wandering.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace multireplay {

class AudioStretch {
public:
	// Block geometry, in milliseconds. These are the classic values for mixed
	// material (speech over crowd noise, which is what a sports replay is):
	// long enough that the search has something to match, short enough that the
	// stretch does not smear a whistle.
	static constexpr double kSequenceMs = 40.0;
	static constexpr double kSeekWindowMs = 15.0;
	static constexpr double kOverlapMs = 8.0;

	// speed 1.0 = untouched, 0.5 = half speed (twice as long), 2.0 = twice as
	// fast. Returns false for anything it will not do — no channels, an absurd
	// rate, a speed outside what the engine itself accepts — and the caller then
	// plays the clip silent, which is what it did at every speed before this
	// existed.
	bool configure(uint32_t sampleRate, uint32_t channels, double speed)
	{
		configured_ = false;
		if (sampleRate < 8000 || sampleRate > 384000 || channels == 0 ||
		    channels > 8 || !(speed >= 0.05) || !(speed <= 4.0))
			return false;
		sampleRate_ = sampleRate;
		channels_ = channels;
		speed_ = speed;

		seqLen_ = msFrames(kSequenceMs);
		seekLen_ = msFrames(kSeekWindowMs);
		overlapLen_ = msFrames(kOverlapMs);
		// The geometry has to survive small block sizes: an overlap as long as
		// the sequence leaves nothing to copy straight through, and a sequence
		// shorter than two overlaps cannot be laid down at all.
		overlapLen_ = std::min<uint32_t>(overlapLen_, seqLen_ / 2);
		if (overlapLen_ == 0 || seqLen_ < 2 * overlapLen_)
			return false;

		// How far the input advances per output block: shorter than the block
		// itself when stretching, which is what makes the output longer.
		nominalSkip_ = speed_ * (double)(seqLen_ - overlapLen_);
		sampleReq_ = (uint32_t)std::ceil(nominalSkip_) + overlapLen_ +
			     seekLen_;
		sampleReq_ = std::max(sampleReq_, seqLen_ + seekLen_);

		reset();
		configured_ = true;
		return true;
	}

	bool configured() const { return configured_; }
	double speed() const { return speed_; }
	uint32_t channels() const { return channels_; }
	// True when the samples are simply forwarded: the caller can then skip this
	// class entirely and hand the decoder's own buffers to OBS.
	bool passthrough() const { return std::fabs(speed_ - 1.0) < 1e-6; }

	void reset()
	{
		in_.assign(channels_, {});
		out_.assign(channels_, {});
		mid_.assign(channels_, std::vector<float>(overlapLen_, 0.0f));
		inPos_ = 0;
		outFrames_ = 0;
		pushed_ = 0;
		skipError_ = 0.0;
		primed_ = false;
	}

	// Feed `frames` samples per channel. `in[c]` must hold at least `frames`.
	void push(const float *const *in, uint32_t frames)
	{
		if (!configured_ || frames == 0)
			return;
		for (uint32_t c = 0; c < channels_; c++) {
			const float *src = in[c];
			if (!src)
				continue;
			in_[c].insert(in_[c].end(), src, src + frames);
		}
		pushed_ += frames;
		process();
	}

	// Output frames waiting to be collected.
	uint32_t available() const
	{
		return out_.empty() ? 0 : (uint32_t)out_[0].size();
	}

	// Move up to `maxFrames` frames into `out` (planar, one array per channel).
	// Returns how many frames were written.
	uint32_t take(float *const *out, uint32_t maxFrames)
	{
		const uint32_t n = std::min(available(), maxFrames);
		if (n == 0)
			return 0;
		for (uint32_t c = 0; c < channels_; c++) {
			if (out[c])
				std::copy(out_[c].begin(), out_[c].begin() + n,
					  out[c]);
			out_[c].erase(out_[c].begin(), out_[c].begin() + n);
		}
		return n;
	}

	// End of the clip: lay down what is left rather than swallowing the last
	// block. A replay that lost the final 40 ms of sound on every clip would
	// sound like a fault, and it would be one.
	//
	// PADDED, then TRIMMED, and the first version of this got it wrong in a way
	// worth keeping written down: it copied the leftover input straight out, so
	// the tail came out at 1x while everything before it was stretched. At half
	// speed that is 48 ms of the clip missing — small enough to pass a listening
	// test and large enough to walk the sound off the picture, growing as the
	// speed drops (at quarter speed it was 152 ms). The length has to come out of
	// the same arithmetic as the rest, so the loop runs on a block of silence and
	// the output is then cut to the length the INPUT asked for: the padding is
	// ours, not the operator's.
	void flush()
	{
		if (!configured_)
			return;
		if (passthrough()) {
			process();
			return;
		}
		if (inAvailable() > 0) {
			const std::vector<float> zeros(sampleReq_, 0.0f);
			for (uint32_t c = 0; c < channels_; c++)
				in_[c].insert(in_[c].end(), zeros.begin(),
					      zeros.end());
			process();
		}
		const uint64_t target =
			(uint64_t)std::llround((double)pushed_ / speed_);
		if (outFrames_ > target) {
			const uint32_t drop = (uint32_t)std::min<uint64_t>(
				outFrames_ - target, available());
			for (uint32_t c = 0; c < channels_; c++)
				out_[c].resize(out_[c].size() - drop);
			outFrames_ -= drop;
		}
	}

	// Total frames produced since configure(). The caller times each output
	// buffer off this, so it must count everything that has left here.
	uint64_t outputFrames() const { return outFrames_; }

private:
	uint32_t msFrames(double ms) const
	{
		return (uint32_t)std::max(1.0,
					  (ms / 1000.0) * (double)sampleRate_);
	}

	uint32_t inAvailable() const
	{
		if (in_.empty() || in_[0].size() <= inPos_)
			return 0;
		return (uint32_t)(in_[0].size() - inPos_);
	}

	// Drop what has been consumed, but only in bulk: erasing from the front on
	// every block would make this quadratic over a long clip.
	void trimInput()
	{
		if (inPos_ < (1u << 16))
			return;
		for (uint32_t c = 0; c < channels_; c++)
			in_[c].erase(in_[c].begin(), in_[c].begin() + inPos_);
		inPos_ = 0;
	}

	void process()
	{
		// 1x: hand the samples straight through. No search, no cross-fade —
		// the ordinary case must not be coloured by machinery it does not need.
		if (passthrough()) {
			const uint32_t n = inAvailable();
			if (n == 0)
				return;
			for (uint32_t c = 0; c < channels_; c++) {
				const float *src = in_[c].data() + inPos_;
				out_[c].insert(out_[c].end(), src, src + n);
			}
			inPos_ += n;
			outFrames_ += n;
			trimInput();
			return;
		}

		while (inAvailable() >= sampleReq_) {
			// The first block has nothing behind it to match, so it is laid
			// down as it is and only fills the overlap buffer.
			const uint32_t offset = primed_ ? bestOverlapOffset() : 0;

			if (primed_) {
				// Cross-fade the tail we kept into the block just
				// found: linear, over the overlap, which at 8 ms is
				// short enough not to smear and long enough not to
				// click.
				for (uint32_t c = 0; c < channels_; c++) {
					const float *src =
						in_[c].data() + inPos_ + offset;
					const float *mid = mid_[c].data();
					for (uint32_t i = 0; i < overlapLen_; i++) {
						const float w =
							(float)(i + 1) /
							(float)(overlapLen_ + 1);
						out_[c].push_back(
							mid[i] * (1.0f - w) +
							src[i] * w);
					}
				}
				outFrames_ += overlapLen_;
			}

			// The middle of the block, copied as it is.
			const uint32_t body = seqLen_ - 2 * overlapLen_;
			if (body > 0) {
				for (uint32_t c = 0; c < channels_; c++) {
					const float *src = in_[c].data() + inPos_ +
							   offset + overlapLen_;
					out_[c].insert(out_[c].end(), src,
						       src + body);
				}
				outFrames_ += body;
			}

			// ...and keep its tail for the next block to fade into.
			for (uint32_t c = 0; c < channels_; c++) {
				const float *src = in_[c].data() + inPos_ + offset +
						   seqLen_ - overlapLen_;
				std::copy(src, src + overlapLen_, mid_[c].begin());
			}
			primed_ = true;

			// Advance the input by the stretched distance, CARRYING THE
			// FRACTION: rounding it away every block would drift the whole
			// clip out from under the picture, which is the one error a
			// listener cannot forgive.
			const double skip = nominalSkip_ + skipError_;
			uint32_t take = (uint32_t)std::llround(skip);
			skipError_ = skip - (double)take;
			// Never less than the overlap just written, or the same audio
			// would be laid down twice and the loop could stand still.
			take = std::max(take, overlapLen_);
			inPos_ += take;
			trimInput();
		}
	}

	// Where, within the seek window, this block matches what has already been
	// written. Normalised cross-correlation on the channel MEAN: one offset for
	// every channel, because moving channels relative to each other is audible
	// as the stereo image wandering.
	uint32_t bestOverlapOffset() const
	{
		uint32_t best = 0;
		double bestScore = -2.0;
		for (uint32_t off = 0; off < seekLen_; off++) {
			double corr = 0.0, norm = 0.0;
			for (uint32_t i = 0; i < overlapLen_; i++) {
				double a = 0.0, b = 0.0;
				for (uint32_t c = 0; c < channels_; c++) {
					a += mid_[c][i];
					b += in_[c][inPos_ + off + i];
				}
				corr += a * b;
				norm += b * b;
			}
			// Normalised, or the loudest offset always wins and the search
			// is a level detector instead of a phase detector.
			const double score =
				norm > 1e-12 ? corr / std::sqrt(norm) : 0.0;
			if (score > bestScore) {
				bestScore = score;
				best = off;
			}
		}
		return best;
	}

	bool configured_ = false;
	uint32_t sampleRate_ = 48000;
	uint32_t channels_ = 0;
	double speed_ = 1.0;

	uint32_t seqLen_ = 0;
	uint32_t seekLen_ = 0;
	uint32_t overlapLen_ = 0;
	uint32_t sampleReq_ = 0;
	double nominalSkip_ = 0.0;
	double skipError_ = 0.0;

	std::vector<std::vector<float>> in_;
	std::vector<std::vector<float>> out_;
	std::vector<std::vector<float>> mid_;
	uint32_t inPos_ = 0;
	uint64_t outFrames_ = 0;
	// Input frames the caller has handed over. flush() needs it: the output
	// length is a property of the whole clip, not of what happens to be left.
	uint64_t pushed_ = 0;
	bool primed_ = false;
};

} // namespace multireplay
