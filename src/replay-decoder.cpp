/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See replay-decoder.hpp.
*/

#include "replay-decoder.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
}

namespace multireplay {

namespace {

AVCodecID codecIdFor(const std::string &name)
{
	if (name == "h264" || name == "avc1")
		return AV_CODEC_ID_H264;
	if (name == "hevc" || name == "h265" || name == "hvc1")
		return AV_CODEC_ID_HEVC;
	if (name == "av1")
		return AV_CODEC_ID_AV1;
	return AV_CODEC_ID_NONE;
}

FrameFormat frameFormatFor(int avPixelFormat)
{
	switch (avPixelFormat) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return FrameFormat::I420;
	case AV_PIX_FMT_NV12:
		return FrameFormat::NV12;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return FrameFormat::I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return FrameFormat::I444;
	default:
		return FrameFormat::Unknown;
	}
}

} // namespace

ReplayDecoder::~ReplayDecoder()
{
	close();
}

bool ReplayDecoder::open(const StreamConfig &cfg, std::string &errorOut)
{
	close();

	if (!cfg.videoUsable()) {
		errorOut = "stream configuration has no usable video";
		return false;
	}

	const AVCodecID id = codecIdFor(cfg.videoCodec);
	if (id == AV_CODEC_ID_NONE) {
		errorOut = "unsupported video codec: " + cfg.videoCodec;
		return false;
	}

	const AVCodec *codec = avcodec_find_decoder(id);
	if (!codec) {
		errorOut = "no decoder available for " + cfg.videoCodec;
		return false;
	}

	ctx_ = avcodec_alloc_context3(codec);
	if (!ctx_) {
		errorOut = "could not allocate the decoder context";
		return false;
	}

	ctx_->width = (int)cfg.width;
	ctx_->height = (int)cfg.height;
	// Packets are stamped in master-timeline nanoseconds, so decoded frames
	// come back on the same clock the markers use - no rescaling anywhere.
	ctx_->pkt_timebase = AVRational{1, 1'000'000'000};
	ctx_->thread_count = 0; // let FFmpeg pick

	if (!cfg.videoExtradata.empty()) {
		ctx_->extradata = (uint8_t *)av_mallocz(
			cfg.videoExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx_->extradata) {
			close();
			errorOut = "could not allocate extradata";
			return false;
		}
		memcpy(ctx_->extradata, cfg.videoExtradata.data(),
		       cfg.videoExtradata.size());
		ctx_->extradata_size = (int)cfg.videoExtradata.size();
	}

	if (avcodec_open2(ctx_, codec, nullptr) < 0) {
		close();
		errorOut = "avcodec_open2 failed for " + cfg.videoCodec;
		return false;
	}

	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	if (!frame_ || !packet_) {
		close();
		errorOut = "could not allocate decoder buffers";
		return false;
	}

	width_ = cfg.width;
	height_ = cfg.height;
	return true;
}

void ReplayDecoder::close()
{
	if (packet_)
		av_packet_free(&packet_);
	if (frame_)
		av_frame_free(&frame_);
	if (ctx_)
		avcodec_free_context(&ctx_);
	width_ = height_ = 0;
}

bool ReplayDecoder::send(const LivePacket &p, std::string &errorOut)
{
	if (!ctx_ || !packet_) {
		errorOut = "decoder is not open";
		return false;
	}
	if (p.kind != PacketKind::Video)
		return true; // audio rides along in a resolved span; skip it
	if (p.data.empty())
		return true;

	av_packet_unref(packet_);
	// avcodec_send_packet does not take ownership, so pointing at the ring's
	// bytes is safe for the duration of this call.
	packet_->data = const_cast<uint8_t *>(p.data.data());
	packet_->size = (int)p.data.size();
	packet_->pts = p.masterNs;
	packet_->dts = p.dtsNs;
	packet_->flags = p.keyframe ? AV_PKT_FLAG_KEY : 0;

	const int rc = avcodec_send_packet(ctx_, packet_);
	packet_->data = nullptr;
	packet_->size = 0;

	if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
		char buf[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(rc, buf, sizeof(buf));
		errorOut = std::string("avcodec_send_packet failed: ") + buf;
		return false;
	}
	return true;
}

bool ReplayDecoder::receive(Frame &out)
{
	if (!ctx_ || !frame_)
		return false;

	av_frame_unref(frame_);
	if (avcodec_receive_frame(ctx_, frame_) < 0)
		return false;

	out.masterNs = frame_->best_effort_timestamp != AV_NOPTS_VALUE
			       ? frame_->best_effort_timestamp
			       : frame_->pts;
	out.width = (uint32_t)frame_->width;
	out.height = (uint32_t)frame_->height;
	out.format = frameFormatFor(frame_->format);
	out.fullRange = frame_->color_range == AVCOL_RANGE_JPEG;
	for (int i = 0; i < 4; i++) {
		out.data[i] = frame_->data[i];
		out.linesize[i] = frame_->linesize[i];
	}
	return true;
}

bool ReplayDecoder::drain(std::string &errorOut)
{
	if (!ctx_) {
		errorOut = "decoder is not open";
		return false;
	}
	const int rc = avcodec_send_packet(ctx_, nullptr);
	if (rc < 0 && rc != AVERROR_EOF) {
		char buf[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(rc, buf, sizeof(buf));
		errorOut = std::string("draining the decoder failed: ") + buf;
		return false;
	}
	return true;
}

void ReplayDecoder::flush()
{
	if (ctx_)
		avcodec_flush_buffers(ctx_);
}

} // namespace multireplay
