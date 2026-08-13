/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See replay-channel.hpp.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "replay-channel.hpp"

#include "packet-tap.hpp"
#include "plugin-support.h"
#include "replay-decoder.hpp"
#include "segment-reader.hpp"

#include <media-io/video-io.h>
#include <util/platform.h>

#include <chrono>

namespace multireplay {

namespace {

constexpr const char *kSourceId = "multireplay_channel";
constexpr const char *kSourceName = "MultiReplay - Replay A";

// The source object itself is deliberately inert: it owns no playback state and
// makes no decisions. ReplayChannel pushes frames into it. That keeps the OBS
// lifetime (which the scene collection controls) independent of the playback
// lifetime (which the operator controls).
struct ChannelSource {
	obs_source_t *source = nullptr;
};

const char *sourceGetName(void *)
{
	return kSourceName;
}

void *sourceCreate(obs_data_t *, obs_source_t *source)
{
	auto *ctx = new ChannelSource();
	ctx->source = source;
	return ctx;
}

void sourceDestroy(void *data)
{
	delete static_cast<ChannelSource *>(data);
}

audio_format obsAudioFormatFor(SampleFormat f)
{
	switch (f) {
	case SampleFormat::F32Planar:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	case SampleFormat::F32:
		return AUDIO_FORMAT_FLOAT;
	case SampleFormat::S16:
		return AUDIO_FORMAT_16BIT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

speaker_layout speakersFor(uint32_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

// --- "Fit to screen", applied to every scene item that shows the replay ----
// One item at a time; see ReplayChannel::applyCanvasFit for the why.
struct FitCtx {
	obs_source_t *target = nullptr;
	uint32_t cx = 0, cy = 0;
	int changed = 0;
};

bool fitSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<FitCtx *>(param);
	// Groups are their own coordinate space, so "fill the canvas" means
	// nothing inside one: a replay deliberately put in a group is the
	// operator's own composition and is left alone.
	if (obs_sceneitem_get_source(item) != ctx->target)
		return true;

	// Already fitted: do not rewrite the transform on every clip, or the
	// scene collection would be marked dirty thirty times an evening for
	// nothing.
	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_SCALE_INNER) {
		struct vec2 have = {};
		obs_sceneitem_get_bounds(item, &have);
		if ((uint32_t)have.x == ctx->cx && (uint32_t)have.y == ctx->cy)
			return true;
	}

	struct vec2 bounds = {};
	vec2_set(&bounds, (float)ctx->cx, (float)ctx->cy);
	struct vec2 pos = {};
	vec2_set(&pos, (float)ctx->cx / 2.0f, (float)ctx->cy / 2.0f);

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_defer_update_end(item);
	ctx->changed++;
	return true;
}

// Collect the scenes first: obs_enum_scenes runs its callback with libobs'
// global source mutex held, and taking a scene's own mutex under it (which is
// what touching an item does) is a lock order we have no business inventing.
bool collectScene(void *param, obs_source_t *sceneSource)
{
	auto *out = static_cast<std::vector<obs_source_t *> *>(param);
	if (obs_source_t *ref = obs_source_get_ref(sceneSource))
		out->push_back(ref);
	return true;
}

video_format obsFormatFor(FrameFormat f)
{
	switch (f) {
	case FrameFormat::I420:
		return VIDEO_FORMAT_I420;
	case FrameFormat::NV12:
		return VIDEO_FORMAT_NV12;
	case FrameFormat::I422:
		return VIDEO_FORMAT_I422;
	case FrameFormat::I444:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

} // namespace

ReplayChannel &ReplayChannel::instance()
{
	static ReplayChannel ch;
	return ch;
}

ReplayChannel::~ReplayChannel()
{
	unload();
}

const char *ReplayChannel::sourceName()
{
	return kSourceName;
}

void ReplayChannel::load()
{
	static bool registered = false;
	if (registered)
		return;

	struct obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	// Async video + audio, exactly like a capture card: OBS handles the
	// scaling, the mixing and the A/V sync for us.
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = sourceGetName;
	info.create = sourceCreate;
	info.destroy = sourceDestroy;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	obs_register_source(&info);

	registered = true;
	obs_log(LOG_INFO, "[channel] source type registered (%s)", kSourceId);
}

void ReplayChannel::unload()
{
	stop();
	std::lock_guard<std::mutex> lock(mutex_);
	if (source_) {
		obs_source_release(source_);
		source_ = nullptr;
	}
}

void ReplayChannel::ensureSource()
{
	std::lock_guard<std::mutex> lock(mutex_);

	// Look the name up BEFORE dropping what we hold. A scene-collection
	// change replaces the objects behind the names, so the stale ref does
	// have to go — but releasing it first destroys the very input we are
	// about to adopt whenever ours is the last reference, which is the
	// normal case until the operator drags "Replay A" into a scene. OBS then
	// logged two "created OBS input" lines per start and left its audio
	// mixer holding a dead source ("Tried to sort VolumeControl for
	// 'MultiReplay - Replay A' but source is null"). Taking the new ref
	// first keeps the object alive across the swap.
	obs_source_t *existing = obs_get_source_by_name(kSourceName);
	obs_source_t *previous = source_;
	source_ = existing; // already add-ref'd, may be null
	if (previous)
		obs_source_release(previous);
	if (source_)
		return;

	obs_data_t *settings = obs_data_create();
	source_ = obs_source_create(kSourceId, kSourceName, settings, nullptr);
	obs_data_release(settings);

	if (!source_) {
		obs_log(LOG_ERROR, "[channel] could not create '%s'", kSourceName);
		return;
	}
	// We pace playback ourselves, so buffering would only add latency on top
	// of a schedule that is already correct.
	obs_source_set_async_unbuffered(source_, true);
	obs_log(LOG_INFO, "[channel] created OBS input '%s'", kSourceName);
}

void ReplayChannel::applyCanvasFit(bool enable)
{
	fitToCanvas_.store(enable);
	if (enable)
		fitSceneItems();
}

void ReplayChannel::fitSceneItems()
{
	if (!fitToCanvas_.load())
		return;

	obs_source_t *target = acquireSource();
	if (!target)
		return;

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 ||
	    ovi.base_height == 0) {
		obs_source_release(target);
		return;
	}

	std::vector<obs_source_t *> scenes;
	obs_enum_scenes(collectScene, &scenes);

	FitCtx ctx;
	ctx.target = target;
	ctx.cx = ovi.base_width;
	ctx.cy = ovi.base_height;
	for (obs_source_t *s : scenes) {
		if (obs_scene_t *scene = obs_scene_from_source(s))
			obs_scene_enum_items(scene, fitSceneItem, &ctx);
		obs_source_release(s);
	}
	obs_source_release(target);

	if (ctx.changed)
		obs_log(LOG_INFO,
			"[channel] fitted %d scene item(s) of '%s' to the %ux%u "
			"canvas (aspect preserved)",
			ctx.changed, kSourceName, ctx.cx, ctx.cy);
}

obs_source_t *ReplayChannel::acquireSource()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return source_ ? obs_source_get_ref(source_) : nullptr;
}

void ReplayChannel::setOnFinished(std::function<void()> fn)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onFinished_ = std::move(fn);
}

void ReplayChannel::joinWorker()
{
	abort_.store(true);
	if (worker_.joinable())
		worker_.join();
	abort_.store(false);
}

bool ReplayChannel::play(int camIndex, int64_t inNs, int64_t outNs, int speedPct,
			 std::string &errorOut, Source source)
{
	if (outNs <= inNs) {
		errorOut = "the range is empty";
		return false;
	}

	std::vector<LivePacket> clip;
	StreamConfig cfg;
	int64_t presentIn = 0, presentOut = 0;
	bool got = false;

	if (source != Source::Segments) {
		got = PacketTap::instance().resolveRange(camIndex, inNs, outNs,
							 clip, presentIn,
							 presentOut);
		if (got)
			cfg = PacketTap::instance().streamConfig(camIndex);
		else
			errorOut = "that range is not held in full in the ring";
	}
	if (!got && source != Source::Ring) {
		// Older than the RAM window: the same clip, read out of the
		// files Branch Output already wrote.
		got = segment_reader::readRange(camIndex, inNs, outNs, clip, cfg,
						presentIn, presentOut, errorOut);
	}
	if (!got)
		return false;

	if (!cfg.videoUsable()) {
		errorOut = "the camera has no usable video configuration";
		return false;
	}

	joinWorker();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!source_) {
			errorOut = "the replay source does not exist yet";
			return false;
		}
		clip_ = std::move(clip);
		config_ = cfg;
		presentInNs_ = presentIn;
		presentOutNs_ = presentOut;
		speedPct_ = speedPct < 5 ? 5 : (speedPct > 400 ? 400 : speedPct);
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_ = PlaybackStats{};
	}

	// Re-applied on every clip because the picture size changes WITH the
	// angle - a 720p camera then a 1080p one - and because the operator may
	// have dropped the input into a scene since the last one. The bounding
	// box makes both cases right without touching a single pixel: OBS scales
	// on the GPU while compositing, which costs nothing per frame.
	fitSceneItems();

	playing_.store(true);
	worker_ = std::thread([this]() { playbackLoop(); });
	return true;
}

void ReplayChannel::stop()
{
	joinWorker();
	playing_.store(false);
}

void ReplayChannel::playbackLoop()
{
	std::vector<LivePacket> clip;
	StreamConfig cfg;
	int64_t presentIn = 0;
	double speed = 1.0;
	obs_source_t *source = nullptr;
	std::function<void()> onFinished;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		clip = clip_;
		cfg = config_;
		presentIn = presentInNs_;
		speed = speedPct_ / 100.0;
		source = source_ ? obs_source_get_ref(source_) : nullptr;
		// Taken at START, not at the end: a later play() may already have
		// installed the next clip's callback by the time we get there.
		onFinished = onFinished_;
	}
	if (!source) {
		playing_.store(false);
		return;
	}

	ReplayDecoder dec;
	std::string err;
	if (!dec.open(cfg, err)) {
		obs_log(LOG_ERROR, "[channel] %s", err.c_str());
		obs_source_release(source);
		playing_.store(false);
		// The clip is over before it began — report it, or a queue waiting
		// on this clip would sit there forever.
		if (onFinished)
			onFinished();
		return;
	}

	// Audio only rides along at normal speed. the reference controller ships slow-motion audio as
	// an option that is off by default, and pushing AAC frames at a stretched
	// cadence without time-stretching them would just sound broken - so at
	// anything other than 1x the clip plays silent until a time-stretcher
	// lands (v1.x, alongside reverse).
	const bool wantAudio = speed == 1.0 && !cfg.audioCodec.empty();
	ReplayAudioDecoder adec;
	if (wantAudio && !adec.open(cfg, err))
		obs_log(LOG_WARNING, "[channel] audio unavailable: %s", err.c_str());

	const uint64_t startWall = os_gettime_ns();
	uint64_t pushed = 0, preroll = 0, audioPushed = 0;
	int64_t firstNs = 0, lastNs = 0;

	const auto emitAudio = [&](const ReplayAudioDecoder::Samples &s) {
		if (s.masterNs < presentIn || s.channels == 0)
			return;
		const audio_format afmt = obsAudioFormatFor(s.format);
		const speaker_layout layout = speakersFor(s.channels);
		if (afmt == AUDIO_FORMAT_UNKNOWN || layout == SPEAKERS_UNKNOWN)
			return;

		struct obs_source_audio a = {};
		a.frames = s.frames;
		a.samples_per_sec = s.sampleRate;
		a.speakers = layout;
		a.format = afmt;
		// Same clock transform as the video, so OBS can sync the two
		// itself rather than us hand-rolling an audio clock - which is
		// what caused the dropouts in the first engine.
		a.timestamp = startWall +
			      (uint64_t)((double)(s.masterNs - presentIn) / speed);
		for (int i = 0; i < 8 && i < MAX_AV_PLANES; i++)
			a.data[i] = s.data[i];

		obs_source_output_audio(source, &a);
		audioPushed++;
	};

	const auto pumpAudio = [&](const LivePacket &p) {
		if (!adec.opened() || p.kind != PacketKind::Audio)
			return;
		std::string aerr;
		if (!adec.send(p, aerr))
			return;
		ReplayAudioDecoder::Samples s;
		while (adec.receive(s))
			emitAudio(s);
	};

	const auto emit = [&](const ReplayDecoder::Frame &f) {
		// Frames before the marked IN only primed the decoder.
		if (f.masterNs < presentIn) {
			preroll++;
			return;
		}
		const video_format fmt = obsFormatFor(f.format);
		if (fmt == VIDEO_FORMAT_NONE)
			return;

		// Where this frame belongs on the wall clock: slow motion is
		// nothing more than stretching this offset.
		const uint64_t due =
			startWall + (uint64_t)((double)(f.masterNs - presentIn) / speed);
		const uint64_t now = os_gettime_ns();
		if (due > now) {
			const uint64_t waitNs = due - now;
			std::this_thread::sleep_for(std::chrono::nanoseconds(waitNs));
		}

		struct obs_source_frame2 out = {};
		out.width = f.width;
		out.height = f.height;
		out.timestamp = due;
		out.format = fmt;
		out.range = f.fullRange ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		for (int i = 0; i < 4 && i < MAX_AV_PLANES; i++) {
			out.data[i] = const_cast<uint8_t *>(f.data[i]);
			out.linesize[i] = (uint32_t)f.linesize[i];
		}
		video_format_get_parameters_for_format(
			f.height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601, out.range,
			fmt, out.color_matrix, out.color_range_min,
			out.color_range_max);

		obs_source_output_video2(source, &out);

		if (pushed == 0)
			firstNs = f.masterNs;
		lastNs = f.masterNs;
		pushed++;

		// Publish the playhead as it moves, not once at the end.
		// positionNs() IS this field, and the dock reads it thirty times a
		// second to drive the seekbar, the timecode and — in review mode —
		// markTimeNs(). Left until after the loop it stayed at the 0 that
		// play() resets it to for the whole clip, so the bar never moved and
		// a mark taken while reviewing landed at master 0.
		{
			std::lock_guard<std::mutex> lock(statsMutex_);
			stats_.framesPushed = pushed;
			stats_.firstFrameNs = firstNs;
			stats_.lastFrameNs = lastNs;
		}
	};

	bool completed = true;
	ReplayDecoder::Frame frame;
	for (const auto &p : clip) {
		if (abort_.load()) {
			completed = false;
			break;
		}
		pumpAudio(p);
		if (!dec.send(p, err)) {
			obs_log(LOG_ERROR, "[channel] %s", err.c_str());
			completed = false;
			break;
		}
		while (dec.receive(frame)) {
			emit(frame);
			if (abort_.load()) {
				completed = false;
				break;
			}
		}
	}
	if (completed && dec.drain(err)) {
		while (dec.receive(frame) && !abort_.load())
			emit(frame);
	}
	if (completed && adec.opened()) {
		std::string aerr;
		if (adec.drain(aerr)) {
			ReplayAudioDecoder::Samples s;
			while (adec.receive(s))
				emitAudio(s);
		}
	}

	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_.audioPushed = audioPushed;
		stats_.framesPushed = pushed;
		stats_.framesPreroll = preroll;
		stats_.firstFrameNs = firstNs;
		stats_.lastFrameNs = lastNs;
		stats_.lastRunCompleted = completed;
	}

	obs_source_release(source);
	playing_.store(false);

	// Only a clip that reached its own end reports back: an aborted run was
	// replaced or stopped by the caller, who already knows.
	if (completed && onFinished)
		onFinished();
}

ReplayChannel::PlaybackStats ReplayChannel::stats() const
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	return stats_;
}

} // namespace multireplay
