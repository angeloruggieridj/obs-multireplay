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

	AVPixelFormat pixFmt = AV_PIX_FMT_YUVJ420P;

	bool tryOpen(int w, int h, AVPixelFormat fmt, int compliance)
	{
		ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			return false;
		ctx->width = w;
		ctx->height = h;
		ctx->pix_fmt = fmt;
		ctx->color_range = AVCOL_RANGE_JPEG;
		ctx->time_base = AVRational{1, PreviewManager::kFps};
		ctx->flags |= AV_CODEC_FLAG_QSCALE;
		ctx->global_quality = FF_QP2LAMBDA * 6; // decent quality
		ctx->strict_std_compliance = compliance;
		if (avcodec_open2(ctx, codec, nullptr) < 0) {
			avcodec_free_context(&ctx);
			return false;
		}
		pixFmt = fmt;
		return true;
	}

	bool ensure(int w, int h)
	{
		if (ctx && w == width && h == height)
			return true;
		reset();
		codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
		if (!codec) {
			obs_log(LOG_WARNING,
				"preview: MJPEG encoder not found in FFmpeg");
			return false;
		}
		// yuvj420p is deprecated in newer FFmpeg: fall back to yuv420p
		// + unofficial compliance when the legacy format is rejected.
		if (!tryOpen(w, h, AV_PIX_FMT_YUVJ420P,
			     FF_COMPLIANCE_NORMAL) &&
		    !tryOpen(w, h, AV_PIX_FMT_YUV420P,
			     FF_COMPLIANCE_UNOFFICIAL)) {
			obs_log(LOG_WARNING,
				"preview: cannot open MJPEG encoder (%dx%d)",
				w, h);
			return false;
		}
		frame = av_frame_alloc();
		packet = av_packet_alloc();
		frame->format = pixFmt;
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
	os_event_init(&captureEvent_, OS_EVENT_TYPE_AUTO);
	enc_ = std::make_unique<EncCtx>();
	running_ = true;
	thread_ = std::thread([this]() { threadLoop(); });
}

void PreviewManager::stop()
{
	running_ = false;
	// Wake the thread immediately if it is blocked in os_event_timedwait.
	if (captureEvent_)
		os_event_signal(captureEvent_);
	jpegCv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	// Destroy the event only after join so any in-flight graphics task
	// cannot signal a dead handle.
	if (captureEvent_) {
		os_event_destroy(captureEvent_);
		captureEvent_ = nullptr;
	}
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
	os_event_t *ev; // signalled when the graphics work is done
};

void runCaptureTask(void *param)
{
	auto *task = static_cast<CaptureTaskCtx *>(param);
	(task->mgr->*(task->fn))();
	os_event_signal(task->ev);
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

	diagCapturePasses_++;

	for (int slot = 0; slot < kPreviewSlots; slot++) {
		obs_source_t *src = nullptr;
		bool wanted = false;
		if (slot < kMaxCameras) {
			const std::string &name =
				cfg.cameras[slot].sourceName;
			if (!name.empty()) {
				wanted = true;
				src = obs_get_source_by_name(name.c_str());
			}
		} else {
			ReplayPlayer &player =
				slot == kPreviewSlotA
					? ReplayEngine::instance().channelA()
					: ReplayEngine::instance().channelB();
			src = player.acquireSource();
		}
		if (!src) {
			if (wanted)
				diagSourceMissing_[slot]++;
			continue;
		}

		uint32_t srcW = obs_source_get_width(src);
		uint32_t srcH = obs_source_get_height(src);
		if (!srcW || !srcH) {
			diagZeroSize_[slot]++;
			obs_source_release(src);
			continue;
		}

		std::vector<uint8_t> rgba;
		int w = 0, h = 0;
		// Queued graphics tasks are not guaranteed to run with the
		// gs context entered: enter it explicitly (recursive-safe).
		obs_enter_graphics();
		bool ok = renderSourceRgba(src, rgba, w, h);
		obs_leave_graphics();
		if (ok) {
			diagRendered_[slot]++;
			std::lock_guard<std::mutex> lock(rawMutex_);
			raw_[slot].rgba = std::move(rgba);
			raw_[slot].width = w;
			raw_[slot].height = h;
			raw_[slot].fresh = true;
		} else {
			if (diagRenderFail_[slot]++ == 0)
				obs_log(LOG_WARNING,
					"preview: slot %d texrender/stage "
					"failed (src %ux%u)",
					slot, srcW, srcH);
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
					 h, enc_->pixFmt, SWS_BILINEAR,
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
	if (diagEncoded_[slot]++ == 0)
		obs_log(LOG_INFO, "preview: slot %d first JPEG produced", slot);
	jpegCv_.notify_all();
}

std::string PreviewManager::debugJson() const
{
	obs_data_t *root = obs_data_create();
	obs_data_set_bool(root, "running", running_);
	obs_data_set_int(root, "capturePasses", diagCapturePasses_);
	obs_data_set_int(root, "captureTimeouts", diagCaptureTimeouts_);
	obs_data_array_t *arr = obs_data_array_create();
	for (int slot = 0; slot < kPreviewSlots; slot++) {
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "slot", slot);
		obs_data_set_int(s, "sourceMissing",
				 diagSourceMissing_[slot]);
		obs_data_set_int(s, "zeroSize", diagZeroSize_[slot]);
		obs_data_set_int(s, "renderFailed", diagRenderFail_[slot]);
		obs_data_set_int(s, "rendered", diagRendered_[slot]);
		obs_data_set_int(s, "encodedJpeg", diagEncoded_[slot]);
		{
			std::lock_guard<std::mutex> lock(jpegMutex_);
			obs_data_set_int(s, "seq", (int64_t)seq_[slot]);
			obs_data_set_bool(s, "hasJpeg",
					  jpeg_[slot] &&
						  !jpeg_[slot]->empty());
		}
		obs_data_array_push_back(arr, s);
		obs_data_release(s);
	}
	obs_data_set_array(root, "slots", arr);
	obs_data_array_release(arr);
	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

void PreviewManager::threadLoop()
{
	os_set_thread_name("multireplay-preview");

	const auto interval = std::chrono::milliseconds(1000 / kFps);

	// captureCtx is stack-local but lives for the entire thread lifetime,
	// so it is always valid when the graphics task dereferences it.
	CaptureTaskCtx captureCtx{this, &PreviewManager::captureAll,
				  captureEvent_};

	while (running_) {
		auto begin = std::chrono::steady_clock::now();

		// Use wait=false to avoid deadlocking when OBS shuts down the
		// graphics thread before obs_module_unload returns.  We sync
		// manually via captureEvent_ with a 500 ms timeout: if the
		// graphics thread is already gone we skip encode and let
		// running_ = false terminate the loop on the next iteration.
		obs_queue_task(OBS_TASK_GRAPHICS, runCaptureTask, &captureCtx,
			       false);
		if (os_event_timedwait(captureEvent_, 500) == 0) {
			for (int slot = 0; slot < kPreviewSlots; slot++)
				encodeSlot(slot);
		} else {
			if (diagCaptureTimeouts_++ == 0)
				obs_log(LOG_WARNING,
					"preview: graphics capture timed out "
					"(will keep retrying)");
		}

		auto elapsed = std::chrono::steady_clock::now() - begin;
		if (elapsed < interval && running_)
			std::this_thread::sleep_for(interval - elapsed);
	}
}

} // namespace multireplay
