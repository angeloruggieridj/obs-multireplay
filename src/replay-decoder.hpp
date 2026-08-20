/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

ReplayDecoder — turns the encoded packets held in the ring back into frames.

Streaming on purpose: a caller feeds packets and pulls frames one at a time,
because decoding a whole clip up front would not fit in memory. Five seconds of
1080p NV12 is roughly 465 MB, while the same five seconds of encoded packets is
about 8 MB — which is exactly why the ring stores packets and not frames.

Decoding starts from the keyframe PacketRing::resolveRange picked, which is at
or before the requested IN. The frames between that keyframe and the IN are
decoded and thrown away: they exist only to build up the reference state. That
is what makes the replay start on the exact marked frame rather than snapping
back to the nearest keyframe, which is what the old ffmpeg_source path did.

The header keeps FFmpeg types out of sight so callers (and later the OBS source)
deal only with plain plane pointers.
*/

#pragma once

#include "packet-types.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace multireplay {

// Planar layouts we can hand to OBS as-is. Keeping this neutral rather than
// leaking AVPixelFormat means the OBS-facing code never includes FFmpeg.
enum class FrameFormat { Unknown, I420, NV12, I422, I444 };

enum class SampleFormat { Unknown, F32Planar, F32, S16 };

class ReplayDecoder {
public:
	// One decoded picture. The plane pointers belong to the decoder and stay
	// valid until the next receive()/flush()/close(), so a consumer must
	// copy or hand them off before asking for the next frame.
	struct Frame {
		int64_t masterNs = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		FrameFormat format = FrameFormat::Unknown;
		bool fullRange = false;
		const uint8_t *data[4] = {nullptr, nullptr, nullptr, nullptr};
		int linesize[4] = {0, 0, 0, 0};
	};

	ReplayDecoder() = default;
	~ReplayDecoder();
	ReplayDecoder(const ReplayDecoder &) = delete;
	ReplayDecoder &operator=(const ReplayDecoder &) = delete;

	bool open(const StreamConfig &cfg, std::string &errorOut);
	void close();
	bool opened() const { return ctx_ != nullptr; }

	// Feed one video packet. Audio packets are ignored, so a caller can walk
	// a resolved span without filtering it first.
	bool send(const LivePacket &p, std::string &errorOut);
	// Pull one frame; false simply means "needs more input".
	bool receive(Frame &out);
	// Tell the decoder no more packets are coming, so the pictures it is
	// still holding can be pulled out. Without this the tail of a clip -
	// which for a replay is the part the operator cares about - never
	// arrives.
	bool drain(std::string &errorOut);
	// Drop decoder state, e.g. before seeking somewhere else.
	void flush();

	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	// How many packets the decoder refused with EAGAIN and this class had to
	// hold back (M1). Should be zero: every caller drains after every send.
	// It exists because the old code reported EAGAIN as SUCCESS and dropped
	// the packet — a picture gone, with no counter, no log line and nothing
	// downstream able to tell.
	uint64_t stalls() const { return stalls_; }

private:
	// Feed the decoder raw bytes. `mayStash` false means "this is the
	// held-back packet coming round again": a second EAGAIN then is the
	// caller not draining, and is reported instead of swallowed.
	bool sendRaw(const uint8_t *data, size_t size, int64_t pts, int64_t dts,
		     int flags, bool mayStash, std::string &errorOut);
	bool flushPending(std::string &errorOut);

	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;

	// The packet avcodec_send_packet did NOT consume, kept in presentation
	// order ahead of whatever comes next.
	std::vector<uint8_t> pending_;
	int64_t pendingPts_ = 0;
	int64_t pendingDts_ = 0;
	int pendingFlags_ = 0;
	bool havePending_ = false;
	uint64_t stalls_ = 0;
};

// The audio half. Same streaming shape, same master-clock timestamps, so video
// and audio land on one timeline and OBS can sync them itself.
class ReplayAudioDecoder {
public:
	struct Samples {
		int64_t masterNs = 0;
		uint32_t frames = 0; // samples per channel
		uint32_t sampleRate = 0;
		uint32_t channels = 0;
		SampleFormat format = SampleFormat::Unknown;
		const uint8_t *data[8] = {};
	};

	ReplayAudioDecoder() = default;
	~ReplayAudioDecoder();
	ReplayAudioDecoder(const ReplayAudioDecoder &) = delete;
	ReplayAudioDecoder &operator=(const ReplayAudioDecoder &) = delete;

	bool open(const StreamConfig &cfg, std::string &errorOut);
	void close();
	bool opened() const { return ctx_ != nullptr; }

	// Feed one audio packet; video packets are ignored.
	bool send(const LivePacket &p, std::string &errorOut);
	bool receive(Samples &out);
	bool drain(std::string &errorOut);
	// See ReplayDecoder::stalls().
	uint64_t stalls() const { return stalls_; }

private:
	bool sendRaw(const uint8_t *data, size_t size, int64_t pts, int64_t dts,
		     bool mayStash, std::string &errorOut);
	bool flushPending(std::string &errorOut);

	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;

	std::vector<uint8_t> pending_;
	int64_t pendingPts_ = 0;
	int64_t pendingDts_ = 0;
	bool havePending_ = false;
	uint64_t stalls_ = 0;
};

} // namespace multireplay
