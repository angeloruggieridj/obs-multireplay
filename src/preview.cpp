/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "preview.hpp"
#include "replay-core.hpp"
#include "replay-player.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstring>

namespace multireplay {

// ---- FFmpeg MJPEG encode context (lazy, shared by all slots) -------------

struct PreviewManager::EncCtx {
	const AVCodec *codec = nullptr;
	AVCodecContext *ctx = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *packet = nullptr;
	SwsContext *sws = nullptr;
	int width = 0, height = 0;

	~EncCtx() { reset(); }

	void reset()
	{
		if (sws) {
			sws_freeContext(sws);
			sws = nullptr;
		}
		if (packet)
			av_packet_free(&packet);
		if (frame)
			av_frame_free(&frame);
		if (ctx)
			avcodec_free_context(&ctx);
		width = height = 0;
	}

	bool ensure(int w, int h)
	{
		if (ctx && w == width && h == height)
			return true;
		reset();
		codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
		if (!codec)
			return false;
		ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			return false;
		ctx->width = w;
		ctx->height = h;
		ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
		ctx->color_range = AVCOL_RANGE_JPEG;
		ctx->time_base = AVRational{1, PreviewManager::kFps};
		ctx->flags |= AV_CODEC_FLAG_QSCALE;
		ctx->global_quality = FF_QP2LAMBDA * 6; // decent quality
		if (avcodec_open2(ctx, codec, nullptr) < 0) {
			reset();
			return false;
		}
		frame = av_frame_alloc();
		packet = av_packet_alloc();
		frame->format = AV_PIX_FMT_YUVJ420P;
		frame->width = w;
		frame->height = h;
		if (av_frame_get_buffer(frame, 0) < 0) {
			reset();
			return false;
		}
		width = w;
		height = h;
		return true;
	}
};

// ---------------------------------------------------------------------------

PreviewManager &PreviewManager::instance()
{
	static PreviewManager mgr;
	return mgr;
}

void PreviewManager::start()
{
	if (running_)
		return;
	enc_ = std::make_unique<EncCtx>();
	running_ = true;
	thread_ = std::thread([this]() { threadLoop(); });
}

void PreviewManager::stop()
{
	running_ = false;
	jpegCv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	enc_.reset();
}

std::shared_ptr<std::vector<uint8_t>>
PreviewManager::latest(int slot, uint64_t &seqOut) const
{
	std::lock_guard<std::mutex> lock(jpegMutex_);
	if (slot < 0 || slot >= kPreviewSlots)
		return nullptr;
	seqOut = seq_[slot];
	return jpeg_[slot];
}

std::shared_ptr<std::vector<uint8_t>>
PreviewManager::waitNext(int slot, uint64_t lastSeq, uint64_t &seqOut,
			 int timeoutMs) const
{
	std::unique_lock<std::mutex> lock(jpegMutex_);
	if (slot < 0 || slot >= kPreviewSlots)
		return nullptr;
	jpegCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
			 [&]() { return seq_[slot] != lastSeq || !running_; });
	seqOut = seq_[slot];
	return seq_[slot] != lastSeq ? jpeg_[slot] : nullptr;
}

namespace {

struct CaptureTaskCtx {
	PreviewManager *mgr;
	void (PreviewManager::*fn)();
};

void runCaptureTask(void *param)
{
	auto *task = static_cast<CaptureTaskCtx *>(param);
	(task->mgr->*(task->fn))();
}

// Render one source scaled into an RGBA buffer. Graphics context required.
bool renderSourceRgba(obs_source_t *src, std::vector<uint8_t> &rgba,
		      int &wOut, int &hOut)
{
	uint32_t srcW = obs_source_get_width(src);
	uint32_t srcH = obs_source_get_height(src);
	if (!srcW || !srcH)
		return false;

	// fit in the preview box, even dimensions for 4:2:0 JPEG
	double scale = std::min((double)PreviewManager::kMaxWidth / srcW,
				(double)PreviewManager::kMaxHeight / srcH);
	if (scale > 1.0)
		scale = 1.0;
	uint32_t w = ((uint32_t)(srcW * scale)) & ~1u;
	uint32_t h = ((uint32_t)(srcH * scale)) & ~1u;
	if (w < 2 || h < 2)
		return false;

	bool ok = false;
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (gs_texrender_begin(tr, w, h)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)srcW, 0.0f, (float)srcH, -100.0f,
			 100.0f);
		obs_source_video_render(src);
		gs_texrender_end(tr);

		gs_stagesurf_t *stage =
			gs_stagesurface_create(w, h, GS_RGBA);
		gs_stage_texture(stage, gs_texrender_get_texture(tr));

		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			rgba.resize((size_t)w * h * 4);
			for (uint32_t row = 0; row < h; row++)
				memcpy(rgba.data() + (size_t)row * w * 4,
				       data + (size_t)row * linesize, w * 4);
			gs_stagesurface_unmap(stage);
			wOut = (int)w;
			hOut = (int)h;
			ok = true;
		}
		gs_stagesurface_destroy(stage);
	}
	gs_texrender_destroy(tr);
	return ok;
}

} // namespace

void PreviewManager::captureAll()
{
	// Graphics thread. Resolve sources fresh on every pass.
	auto &core = ReplayCore::instance();
	Config cfg = core.getConfig();

	for (int slot = 0; slot < kPreviewSlots; slot++) {
		obs_source_t *src = nullptr;
		if (slot < kMaxCameras) {
			const std::string &name =
				cfg.cameras[slot].sourceName;
			if (!name.empty())
				src = obs_get_source_by_name(name.c_str());
		} else {
			ReplayPlayer &player =
				slot == kPreviewSlotA
					? ReplayEngine::instance().channelA()
					: ReplayEngine::instance().channelB();
			src = player.acquireSource();
		}
		if (!src)
			continue;

		std::vector<uint8_t> rgba;
		int w = 0, h = 0;
		if (renderSourceRgba(src, rgba, w, h)) {
			std::lock_guard<std::mutex> lock(rawMutex_);
			raw_[slot].rgba = std::move(rgba);
			raw_[slot].width = w;
			raw_[slot].height = h;
			raw_[slot].fresh = true;
		}
		obs_source_release(src);
	}
}

void PreviewManager::encodeSlot(int slot)
{
	std::vector<uint8_t> rgba;
	int w = 0, h = 0;
	{
		std::lock_guard<std::mutex> lock(rawMutex_);
		if (!raw_[slot].fresh)
			return;
		rgba = std::move(raw_[slot].rgba);
		w = raw_[slot].width;
		h = raw_[slot].height;
		raw_[slot].fresh = false;
	}

	if (!enc_ || !enc_->ensure(w, h))
		return;

	enc_->sws = sws_getCachedContext(enc_->sws, w, h, AV_PIX_FMT_RGBA, w,
					 h, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR,
					 nullptr, nullptr, nullptr);
	if (!enc_->sws)
		return;

	const uint8_t *srcData[4] = {rgba.data(), nullptr, nullptr, nullptr};
	int srcLinesize[4] = {w * 4, 0, 0, 0};
	sws_scale(enc_->sws, srcData, srcLinesize, 0, h, enc_->frame->data,
		  enc_->frame->linesize);

	enc_->frame->quality = enc_->ctx->global_quality;
	if (avcodec_send_frame(enc_->ctx, enc_->frame) < 0)
		return;
	if (avcodec_receive_packet(enc_->ctx, enc_->packet) < 0)
		return;

	auto jpeg = std::make_shared<std::vector<uint8_t>>(
		enc_->packet->data, enc_->packet->data + enc_->packet->size);
	av_packet_unref(enc_->packet);

	{
		std::lock_guard<std::mutex> lock(jpegMutex_);
		jpeg_[slot] = std::move(jpeg);
		seq_[slot]++;
	}
	jpegCv_.notify_all();
}

void PreviewManager::threadLoop()
{
	os_set_thread_name("multireplay-preview");

	const auto interval = std::chrono::milliseconds(1000 / kFps);

	while (running_) {
		auto begin = std::chrono::steady_clock::now();

		CaptureTaskCtx task{this, &PreviewManager::captureAll};
		obs_queue_task(OBS_TASK_GRAPHICS, runCaptureTask, &task,
			       true /* wait for completion */);

		for (int slot = 0; slot < kPreviewSlots; slot++)
			encodeSlot(slot);

		auto elapsed = std::chrono::steady_clock::now() - begin;
		if (elapsed < interval)
			std::this_thread::sleep_for(interval - elapsed);
	}
}

} // namespace multireplay
