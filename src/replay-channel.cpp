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

	// A scene-collection change replaces the objects behind the names, so
	// drop what we hold before looking again — keeping a stale ref is how
	// the old engine ended up driving a source nobody could see.
	if (source_) {
		obs_source_release(source_);
		source_ = nullptr;
	}

	obs_source_t *existing = obs_get_source_by_name(kSourceName);
	if (existing) {
		source_ = existing; // already add-ref'd
		return;
	}

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

obs_source_t *ReplayChannel::acquireSource()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return source_ ? obs_source_get_ref(source_) : nullptr;
}

void ReplayChannel::joinWorker()
{
	abort_.store(true);
	if (worker_.joinable())
		worker_.join();
	abort_.store(false);
}

bool ReplayChannel::play(int camIndex, int64_t inNs, int64_t outNs, int speedPct,
			 std::string &errorOut)
{
	if (outNs <= inNs) {
		errorOut = "the range is empty";
		return false;
	}

	std::vector<LivePacket> clip;
	int64_t presentIn = 0, presentOut = 0;
	if (!PacketTap::instance().resolveRange(camIndex, inNs, outNs, clip,
						presentIn, presentOut)) {
		errorOut = "that range is not held in full on this camera";
		return false;
	}

	const StreamConfig cfg = PacketTap::instance().streamConfig(camIndex);
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

	{
		std::lock_guard<std::mutex> lock(mutex_);
		clip = clip_;
		cfg = config_;
		presentIn = presentInNs_;
		speed = speedPct_ / 100.0;
		source = source_ ? obs_source_get_ref(source_) : nullptr;
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
		return;
	}

	const uint64_t startWall = os_gettime_ns();
	uint64_t pushed = 0, preroll = 0;
	int64_t firstNs = 0, lastNs = 0;

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
	};

	bool completed = true;
	ReplayDecoder::Frame frame;
	for (const auto &p : clip) {
		if (abort_.load()) {
			completed = false;
			break;
		}
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

	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_.framesPushed = pushed;
		stats_.framesPreroll = preroll;
		stats_.firstFrameNs = firstNs;
		stats_.lastFrameNs = lastNs;
		stats_.lastRunCompleted = completed;
	}

	obs_source_release(source);
	playing_.store(false);
}

ReplayChannel::PlaybackStats ReplayChannel::stats() const
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	return stats_;
}

} // namespace multireplay
