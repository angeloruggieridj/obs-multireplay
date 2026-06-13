/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "preview.hpp"
#include "decoder.hpp"
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
		ctx->global_quality = FF_QP2LAMBDA * 6;
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

// ---- Double-buffered GPU capture context ---------------------------------
//
// One persistent GPU slot per preview tile. Stage surfaces are ping-ponged:
//   frame N   → render → gs_stage_texture(stage[1-pingIdx])  [async, no stall]
//   frame N+1 → gs_stagesurface_map(stage[pingIdx])          [instant: DMA done]
//             → render → gs_stage_texture(stage[1-pingIdx])   ...
//
// At 20 fps the GPU has ~50 ms between stage and map, so map returns instantly
// with zero stall on the OBS render thread even during A/B 1080p playback.

struct GfxSlot {
	gs_texrender_t *tr = nullptr;
	gs_stagesurf_t *stage[2] = {};
	int pingIdx = 0;        // surface staged last pass (ready to map)
	bool hasPending = false; // a staged frame is waiting to be mapped
	int w = 0, h = 0;

	// Must be called on the OBS graphics thread.
	void destroy()
	{
		if (tr) {
			gs_texrender_destroy(tr);
			tr = nullptr;
		}
		for (auto &s : stage) {
			if (s) {
				gs_stagesurface_destroy(s);
				s = nullptr;
			}
		}
		hasPending = false;
		w = h = 0;
	}

	// Recreate GPU objects if dimensions changed. Graphics thread only.
	bool resize(int nw, int nh)
	{
		if (tr && nw == w && nh == h)
			return true;
		destroy();
		tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		stage[0] = gs_stagesurface_create(nw, nh, GS_RGBA);
		stage[1] = gs_stagesurface_create(nw, nh, GS_RGBA);
		if (!tr || !stage[0] || !stage[1]) {
			destroy();
			return false;
		}
		w = nw;
		h = nh;
		pingIdx = 0;
		return true;
	}
};

struct PreviewManager::GfxCtx {
	GfxSlot slots[kPreviewSlots];

	// Must be called on the OBS graphics thread.
	void destroy()
	{
		for (auto &s : slots)
			s.destroy();
	}
};

// ---- Graphics-thread task helpers ----------------------------------------

namespace {

struct CaptureTaskCtx {
	PreviewManager *mgr;
	void (PreviewManager::*fn)();
	os_event_t *ev;
};

void runCaptureTask(void *param)
{
	auto *task = static_cast<CaptureTaskCtx *>(param);
	(task->mgr->*(task->fn))();
	os_event_signal(task->ev);
}

} // namespace

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

	// Destroy persistent GPU objects on the graphics thread. If the graphics
	// thread is already gone (OBS shutdown) the timedwait expires and we
	// accept the small leak — OBS cleans the GPU context itself.
	if (gfx_) {
		os_event_t *ev = nullptr;
		os_event_init(&ev, OS_EVENT_TYPE_AUTO);
		CaptureTaskCtx ctx{this, &PreviewManager::destroyGfx, ev};
		obs_queue_task(OBS_TASK_GRAPHICS, runCaptureTask, &ctx, false);
		os_event_timedwait(ev, 500);
		os_event_destroy(ev);
	}

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

void PreviewManager::captureAll()
{
	// Graphics thread. Lazy-init the persistent GPU context here so all
	// gs_* allocations happen on the correct thread.
	if (!gfx_)
		gfx_ = std::make_unique<GfxCtx>();

	auto &core = ReplayCore::instance();
	Config cfg = core.getConfig();
	diagCapturePasses_++;

	for (int slot = 0; slot < kPreviewSlots; slot++) {
		obs_source_t *src = nullptr;
		bool wanted = false;
		if (slot < kMaxCameras) {
			// Camera tiles always show the live feed.
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

		GfxSlot &gs = gfx_->slots[slot];

		// STEP 1: Map the surface staged in the PREVIOUS pass.
		// The GPU had a full frame interval (~50 ms at 20 fps) to
		// complete the DMA, so this returns instantly — zero stall on
		// the OBS render thread even during A/B 1080p playback.
		// obs_enter_graphics() is recursive-safe; queued graphics tasks
		// are not guaranteed to hold the context on entry.
		if (gs.hasPending) {
			obs_enter_graphics();
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(gs.stage[gs.pingIdx], &data,
						&linesize)) {
				std::vector<uint8_t> rgba(
					(size_t)gs.w * gs.h * 4);
				for (int row = 0; row < gs.h; ++row)
					memcpy(rgba.data() +
						       (size_t)row * gs.w * 4,
					       data +
						       (size_t)row * linesize,
					       (size_t)gs.w * 4);
				gs_stagesurface_unmap(gs.stage[gs.pingIdx]);
				obs_leave_graphics();
				std::lock_guard<std::mutex> lk(rawMutex_);
				raw_[slot].rgba = std::move(rgba);
				raw_[slot].width = gs.w;
				raw_[slot].height = gs.h;
				raw_[slot].fresh = true;
				diagRendered_[slot]++;
			} else {
				obs_leave_graphics();
				if (diagRenderFail_[slot]++ == 0)
					obs_log(LOG_WARNING,
						"preview: slot %d "
						"stagesurface_map failed",
						slot);
			}
			gs.hasPending = false;
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

		// Compute scaled dimensions (fit in tile, even for 4:2:0 JPEG).
		double scale = std::min((double)kMaxWidth / srcW,
					(double)kMaxHeight / srcH);
		if (scale > 1.0)
			scale = 1.0;
		int w = ((int)(srcW * scale)) & ~1;
		int h = ((int)(srcH * scale)) & ~1;
		if (w < 2 || h < 2) {
			obs_source_release(src);
			continue;
		}

		// STEP 2: Render source into texrender, then initiate an async
		// GPU→RAM DMA into the OTHER surface (non-blocking). The map
		// for this frame happens on the NEXT pass, after the GPU is done.
		obs_enter_graphics();
		if (!gs.resize(w, h)) {
			obs_leave_graphics();
			if (diagRenderFail_[slot]++ == 0)
				obs_log(LOG_WARNING,
					"preview: slot %d GfxSlot resize "
					"failed (%dx%d)",
					slot, w, h);
			obs_source_release(src);
			continue;
		}

		{
			int freshIdx = 1 - gs.pingIdx;
			gs_texrender_reset(gs.tr);
			if (gs_texrender_begin(gs.tr, (uint32_t)w,
					       (uint32_t)h)) {
				struct vec4 clear;
				vec4_zero(&clear);
				gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
				gs_ortho(0.0f, (float)srcW, 0.0f, (float)srcH,
					 -100.0f, 100.0f);
				obs_source_video_render(src);
				gs_texrender_end(gs.tr);
				// Async GPU→RAM copy: starts DMA, no stall.
				gs_stage_texture(
					gs.stage[freshIdx],
					gs_texrender_get_texture(gs.tr));
				gs.pingIdx = freshIdx;
				gs.hasPending = true;
			} else {
				if (diagRenderFail_[slot]++ == 0)
					obs_log(LOG_WARNING,
						"preview: slot %d "
						"gs_texrender_begin failed",
						slot);
			}
		}
		obs_leave_graphics();
		obs_source_release(src);
	}
}

void PreviewManager::destroyGfx()
{
	// Runs on the OBS graphics thread (queued from stop()).
	if (gfx_) {
		obs_enter_graphics();
		gfx_->destroy();
		obs_leave_graphics();
		gfx_.reset();
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
		// graphics thread before obs_module_unload returns. Sync via
		// captureEvent_ with a 500 ms timeout.
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
