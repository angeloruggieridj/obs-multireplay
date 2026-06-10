/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "decoder.hpp"

// obs-module.h must come before plugin-support.h: on MSVC, declaring
// blogva() without dllimport before util/base.h declares it with dllimport
// is a hard error (C2375).
#include <obs-module.h>
#include "plugin-support.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace multireplay {

namespace {

inline int64_t rescaleToNs(int64_t value, AVRational timeBase)
{
	return av_rescale_q(value, timeBase, AVRational{1, 1000000000});
}

inline int64_t rescaleFromNs(int64_t ns, AVRational timeBase)
{
	return av_rescale_q(ns, AVRational{1, 1000000000}, timeBase);
}

} // namespace

SegmentDecoder::~SegmentDecoder()
{
	close();
}

bool SegmentDecoder::open(const std::string &path)
{
	close();

	if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
		obs_log(LOG_WARNING, "decoder: cannot open %s", path.c_str());
		return false;
	}
	if (avformat_find_stream_info(fmt_, nullptr) < 0) {
		close();
		return false;
	}

	streamIndex_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1,
					   nullptr, 0);
	if (streamIndex_ < 0) {
		obs_log(LOG_WARNING, "decoder: no video stream in %s",
			path.c_str());
		close();
		return false;
	}

	AVStream *stream = fmt_->streams[streamIndex_];
	const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!dec) {
		close();
		return false;
	}
	codec_ = avcodec_alloc_context3(dec);
	if (!codec_ ||
	    avcodec_parameters_to_context(codec_, stream->codecpar) < 0 ||
	    avcodec_open2(codec_, dec, nullptr) < 0) {
		close();
		return false;
	}

	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	width_ = codec_->width;
	height_ = codec_->height;

	AVRational fr = stream->avg_frame_rate;
	if (fr.num > 0 && fr.den > 0)
		frameDurationNs_ = (int64_t)1000000000 * fr.den / fr.num;

	path_ = path;
	return true;
}

void SegmentDecoder::close()
{
	if (sws_) {
		sws_freeContext(sws_);
		sws_ = nullptr;
	}
	if (packet_)
		av_packet_free(&packet_);
	if (frame_)
		av_frame_free(&frame_);
	if (codec_)
		avcodec_free_context(&codec_);
	if (fmt_)
		avformat_close_input(&fmt_);
	streamIndex_ = -1;
	path_.clear();
}

bool SegmentDecoder::seekTo(int64_t ns)
{
	if (!fmt_)
		return false;
	AVStream *stream = fmt_->streams[streamIndex_];
	int64_t ts = rescaleFromNs(ns, stream->time_base);
	if (stream->start_time != AV_NOPTS_VALUE)
		ts += stream->start_time;
	if (av_seek_frame(fmt_, streamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0)
		return false;
	avcodec_flush_buffers(codec_);
	return true;
}

bool SegmentDecoder::nextFrame(DecodedFrame &out)
{
	if (!fmt_)
		return false;

	while (true) {
		int ret = avcodec_receive_frame(codec_, frame_);
		if (ret == 0) {
			bool ok = convertFrame(frame_, out);
			av_frame_unref(frame_);
			return ok;
		}
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
			return false;

		ret = av_read_frame(fmt_, packet_);
		if (ret < 0) {
			// flush decoder at EOF
			avcodec_send_packet(codec_, nullptr);
			ret = avcodec_receive_frame(codec_, frame_);
			if (ret == 0) {
				bool ok = convertFrame(frame_, out);
				av_frame_unref(frame_);
				return ok;
			}
			return false;
		}
		if (packet_->stream_index == streamIndex_)
			avcodec_send_packet(codec_, packet_);
		av_packet_unref(packet_);
	}
}

bool SegmentDecoder::convertFrame(const AVFrame *src, DecodedFrame &out)
{
	AVStream *stream = fmt_->streams[streamIndex_];

	int64_t pts = src->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		pts = src->pts;
	if (pts != AV_NOPTS_VALUE && stream->start_time != AV_NOPTS_VALUE)
		pts -= stream->start_time;
	out.ptsNs = pts != AV_NOPTS_VALUE ? rescaleToNs(pts, stream->time_base)
					  : 0;
	out.width = src->width;
	out.height = src->height;
	out.fullRange = src->color_range == AVCOL_RANGE_JPEG;

	const int w = src->width, h = src->height;
	const int cw = (w + 1) / 2, ch = (h + 1) / 2;
	out.strideY = w;
	out.strideU = cw;
	out.strideV = cw;
	out.y.resize((size_t)w * h);
	out.u.resize((size_t)cw * ch);
	out.v.resize((size_t)cw * ch);

	if (src->format == AV_PIX_FMT_YUV420P ||
	    src->format == AV_PIX_FMT_YUVJ420P) {
		// fast path: plane copy
		for (int r = 0; r < h; r++)
			memcpy(out.y.data() + (size_t)r * w,
			       src->data[0] + (size_t)r * src->linesize[0], w);
		for (int r = 0; r < ch; r++) {
			memcpy(out.u.data() + (size_t)r * cw,
			       src->data[1] + (size_t)r * src->linesize[1],
			       cw);
			memcpy(out.v.data() + (size_t)r * cw,
			       src->data[2] + (size_t)r * src->linesize[2],
			       cw);
		}
		return true;
	}

	// generic path: convert to I420 with swscale
	sws_ = sws_getCachedContext(sws_, w, h, (AVPixelFormat)src->format, w,
				    h, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
				    nullptr, nullptr, nullptr);
	if (!sws_)
		return false;
	uint8_t *dstData[4] = {out.y.data(), out.u.data(), out.v.data(),
			       nullptr};
	int dstLinesize[4] = {out.strideY, out.strideU, out.strideV, 0};
	sws_scale(sws_, src->data, src->linesize, 0, h, dstData, dstLinesize);
	return true;
}

} // namespace multireplay
