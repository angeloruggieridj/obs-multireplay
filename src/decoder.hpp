/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace multireplay {

// One decoded video frame, I420 planes owned by this object.
struct DecodedFrame {
	int64_t ptsNs = 0;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> y, u, v;
	int strideY = 0, strideU = 0, strideV = 0;
	bool fullRange = false;
};

// One decoded audio chunk: planar float stereo 48 kHz (OBS-native format).
struct AudioChunk {
	int64_t ptsNs = 0;
	int frames = 0;
	std::vector<float> left;
	std::vector<float> right;
};

// Decoder for a single recorded segment file. Forward decoding is
// sequential; random access seeks to the keyframe at/before the target
// (recordings use a 1s GOP, see branch-output-control.cpp), then decodes
// forward. Not thread-safe: each ReplayPlayer owns its own decoder.
class SegmentDecoder {
public:
	SegmentDecoder() = default;
	~SegmentDecoder();
	SegmentDecoder(const SegmentDecoder &) = delete;
	SegmentDecoder &operator=(const SegmentDecoder &) = delete;

	bool open(const std::string &path);
	void close();
	bool isOpen() const { return fmt_ != nullptr; }
	const std::string &path() const { return path_; }

	// Seek to the keyframe at or before `ns` (stream time).
	bool seekTo(int64_t ns);

	// Decode the next frame into `out`. Returns false at EOF/error.
	// Audio packets encountered along the way are decoded into the
	// audio queue (drained via takeAudio).
	bool nextFrame(DecodedFrame &out);

	// Move out all audio decoded so far (M5: replay audio playback).
	std::vector<AudioChunk> takeAudio();
	void clearAudio();
	static constexpr int kAudioSampleRate = 48000;

	int width() const { return width_; }
	int height() const { return height_; }
	// Nominal frame duration from the stream frame rate.
	int64_t frameDurationNs() const { return frameDurationNs_; }

private:
	bool convertFrame(const AVFrame *src, DecodedFrame &out);
	void decodeAudioPacket(const AVPacket *pkt);

	std::string path_;
	AVFormatContext *fmt_ = nullptr;
	AVCodecContext *codec_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
	SwsContext *sws_ = nullptr;
	int streamIndex_ = -1;
	int width_ = 0;
	int height_ = 0;
	int64_t frameDurationNs_ = 33333333; // 30 fps fallback

	// audio (optional: missing/failed audio stream just mutes playback)
	AVCodecContext *audioCodec_ = nullptr;
	AVFrame *audioFrame_ = nullptr;
	SwrContext *swr_ = nullptr;
	int audioStreamIndex_ = -1;
	std::vector<AudioChunk> audioQueue_;
};

} // namespace multireplay
