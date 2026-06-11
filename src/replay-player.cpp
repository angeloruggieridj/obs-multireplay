/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "replay-player.hpp"
#include "replay-core.hpp"
#include "plugin-support.h"

#include <media-io/video-io.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <chrono>

namespace multireplay {

namespace {
// Cache at most ~4s of decoded frames (GOP is 1s; this leaves headroom for
// scrubbing back and forth without re-seeking).
constexpr size_t kMaxCacheFrames = 240;
// When the target is more than this ahead of the cache, seek instead of
// decoding forward.
constexpr int64_t kForwardSeekThresholdNs = 2000000000; // 2s
} // namespace

ReplayPlayer::ReplayPlayer(char channelId) : channelId_(channelId) {}

ReplayPlayer::~ReplayPlayer()
{
	stop();
}

void ReplayPlayer::start()
{
	if (running_)
		return;
	running_ = true;
	thread_ = std::thread([this]() { threadLoop(); });
}

void ReplayPlayer::stop()
{
	running_ = false;
	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void ReplayPlayer::attachSource(obs_source_t *source)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	source_ = source;
}

void ReplayPlayer::detachSource(obs_source_t *source)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (source_ == source)
		source_ = nullptr;
}

obs_source_t *ReplayPlayer::acquireSource()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return source_ ? obs_source_get_ref(source_) : nullptr;
}

void ReplayPlayer::setStopAt(int64_t ns, std::function<void()> onStop)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	stopAtNs_ = ns;
	onStop_ = std::move(onStop);
}

void ReplayPlayer::setIndex(std::shared_ptr<SessionIndex> index)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	index_ = std::move(index);
	invalidateCache();
	positionNs_ = 0;
	wake_.notify_all();
}

void ReplayPlayer::setPlaying(bool playing)
{
	playing_ = playing;
	wake_.notify_all();
}

void ReplayPlayer::setSpeed(double speed)
{
	speed_ = std::clamp(speed, 0.0, 1.0);
}

void ReplayPlayer::setReverse(bool reverse)
{
	reverse_ = reverse;
}

void ReplayPlayer::stepFrames(int frames)
{
	playing_ = false;
	pendingStepFrames_ += frames;
	wake_.notify_all();
}

void ReplayPlayer::seekMaster(int64_t positionNs)
{
	positionNs_ = std::max<int64_t>(0, positionNs);
	wake_.notify_all();
}

void ReplayPlayer::jumpToEnd()
{
	std::shared_ptr<SessionIndex> index;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		index = index_;
	}
	if (index) {
		index->refresh();
		// land slightly before the absolute end so a frame exists
		seekMaster(index->masterDurationNs() - 1);
	}
}

void ReplayPlayer::setAngle(int camIndex)
{
	angle_ = std::clamp(camIndex, 0, kIndexMaxCameras - 1);
	wake_.notify_all();
}

void ReplayPlayer::invalidateCache()
{
	gopCache_.clear();
	cachedPath_.clear();
	cachedAngle_ = -1;
	decoder_.close();
}

const DecodedFrame *ReplayPlayer::frameAt(int64_t masterNs)
{
	// stateMutex_ must be held by the caller.
	if (!index_)
		return nullptr;

	std::string path;
	int64_t offsetNs = 0;
	if (!index_->resolve(angle_, masterNs, path, offsetNs))
		return nullptr;

	if (path != cachedPath_ || angle_ != cachedAngle_) {
		gopCache_.clear();
		if (!decoder_.open(path))
			return nullptr;
		cachedPath_ = path;
		cachedAngle_ = angle_;
		decoder_.seekTo(offsetNs);
	}

	const bool cacheUsable =
		!gopCache_.empty() && offsetNs >= gopCache_.front().ptsNs;
	const bool needBackwardSeek = !cacheUsable && !gopCache_.empty() &&
				      offsetNs < gopCache_.front().ptsNs;
	const bool farAhead = !gopCache_.empty() &&
			      offsetNs - gopCache_.back().ptsNs >
				      kForwardSeekThresholdNs;

	if (needBackwardSeek || farAhead || gopCache_.empty()) {
		if (needBackwardSeek || farAhead) {
			gopCache_.clear();
			decoder_.seekTo(offsetNs);
		}
		// fill cache up to the target
		DecodedFrame f;
		while (decoder_.nextFrame(f)) {
			gopCache_.push_back(std::move(f));
			f = DecodedFrame{};
			if (gopCache_.back().ptsNs >= offsetNs)
				break;
			if (gopCache_.size() > kMaxCacheFrames)
				gopCache_.pop_front();
		}
	} else if (offsetNs > gopCache_.back().ptsNs) {
		// decode forward to the target
		DecodedFrame f;
		while (gopCache_.back().ptsNs < offsetNs &&
		       decoder_.nextFrame(f)) {
			gopCache_.push_back(std::move(f));
			f = DecodedFrame{};
			if (gopCache_.size() > kMaxCacheFrames)
				gopCache_.pop_front();
		}
	}

	// closest frame at/below the target (cache is pts-ordered)
	const DecodedFrame *best = nullptr;
	for (const auto &cached : gopCache_) {
		if (cached.ptsNs <= offsetNs)
			best = &cached;
		else
			break;
	}
	return best ? best : (!gopCache_.empty() ? &gopCache_.front()
						 : nullptr);
}

void ReplayPlayer::outputFrame(const DecodedFrame &frame)
{
	// stateMutex_ must be held by the caller.
	if (!source_)
		return;

	struct obs_source_frame out = {};
	out.data[0] = const_cast<uint8_t *>(frame.y.data());
	out.data[1] = const_cast<uint8_t *>(frame.u.data());
	out.data[2] = const_cast<uint8_t *>(frame.v.data());
	out.linesize[0] = (uint32_t)frame.strideY;
	out.linesize[1] = (uint32_t)frame.strideU;
	out.linesize[2] = (uint32_t)frame.strideV;
	out.width = (uint32_t)frame.width;
	out.height = (uint32_t)frame.height;
	out.format = VIDEO_FORMAT_I420;
	out.full_range = frame.fullRange;
	video_format_get_parameters(VIDEO_CS_DEFAULT,
				    frame.fullRange ? VIDEO_RANGE_FULL
						    : VIDEO_RANGE_PARTIAL,
				    out.color_matrix, out.color_range_min,
				    out.color_range_max);

	// Async sources need monotonically increasing timestamps even when
	// the playhead moves backwards.
	outputTimestamp_ += (uint64_t)decoder_.frameDurationNs();
	out.timestamp = outputTimestamp_;

	obs_source_output_video(source_, &out);

	// --- M5: audio playback ---
	// Audio only makes sense forward at (near) normal speed: in slow
	// motion / reverse / scrub broadcast-style replays are effectively mute.
	auto chunks = decoder_.takeAudio();
	bool audible = playing_ && !reverse_ && speed_.load() > 0.99;
	if (!audible)
		return;
	for (auto &chunk : chunks) {
		if (chunk.frames <= 0)
			continue;
		struct obs_source_audio audio = {};
		audio.data[0] = (const uint8_t *)chunk.left.data();
		audio.data[1] = (const uint8_t *)chunk.right.data();
		audio.frames = (uint32_t)chunk.frames;
		audio.speakers = SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = SegmentDecoder::kAudioSampleRate;
		// Map the chunk onto the synthetic output clock, keeping the
		// original A/V offset relative to the current video frame.
		int64_t offset = chunk.ptsNs - frame.ptsNs;
		audio.timestamp = outputTimestamp_ + (uint64_t)std::max<int64_t>(0, offset);
		obs_source_output_audio(source_, &audio);
	}
}

void ReplayPlayer::threadLoop()
{
	os_set_thread_name("multireplay-player");

	int64_t lastRenderedPos = -1;

	while (running_) {
		int64_t frameDurNs = decoder_.isOpen()
					     ? decoder_.frameDurationNs()
					     : 33333333;
		std::function<void()> stopCallback;

		{
			std::unique_lock<std::mutex> lock(stateMutex_);

			if (!index_ || !source_) {
				wake_.wait_for(lock,
					       std::chrono::milliseconds(50));
				continue;
			}

			// frame stepping (pauses playback)
			int step = pendingStepFrames_.exchange(0);
			if (step != 0)
				positionNs_ = std::max<int64_t>(
					0, positionNs_ + step * frameDurNs);

			// advance playhead
			if (playing_) {
				double delta = (double)frameDurNs *
					       speed_.load() *
					       (reverse_ ? -1.0 : 1.0);
				int64_t next = positionNs_ + (int64_t)delta;
				int64_t maxPos =
					index_->masterDurationNs() - 1;
				if (next <= 0) {
					next = 0;
					playing_ = false; // hit session start
				} else if (next >= maxPos) {
					// at the live edge: try refresh once,
					// otherwise hold
					next = maxPos;
				}

				// M3: event playback stops at the out point
				int64_t stopAt = stopAtNs_.load();
				if (stopAt >= 0 &&
				    ((!reverse_ && next >= stopAt) ||
				     (reverse_ && next <= stopAt))) {
					next = stopAt;
					playing_ = false;
					stopAtNs_ = -1;
					stopCallback = std::move(onStop_);
					onStop_ = nullptr;
				}
				positionNs_ = next;
			}

			if (positionNs_ != lastRenderedPos) {
				const DecodedFrame *frame =
					frameAt(positionNs_);
				if (frame) {
					outputFrame(*frame);
					lastRenderedPos = positionNs_;
				}
			}
		}

		// fire outside the lock: the callback may call back into us
		if (stopCallback)
			stopCallback();

		std::unique_lock<std::mutex> idle(stateMutex_);
		wake_.wait_for(idle,
			       std::chrono::nanoseconds(
				       playing_ ? frameDurNs : 33333333));
	}
}

// --------------------------------------------------------------------------

ReplayEngine &ReplayEngine::instance()
{
	static ReplayEngine engine;
	return engine;
}

void ReplayEngine::load()
{
	a_.start();
	b_.start();
}

void ReplayEngine::unload()
{
	a_.stop();
	b_.stop();
	index_.reset();
}

bool ReplayEngine::loadSession(const std::string &folder,
			       std::string &errorOut)
{
	auto index = std::make_shared<SessionIndex>();
	if (!index->load(folder)) {
		errorOut = "no indexable session in folder (record something "
			   "first, then stop or wait for a split)";
		return false;
	}
	index_ = index;
	a_.setIndex(index);
	b_.setIndex(index);
	// default angles: A=cam1, B=cam2 (the reference controller default behaviour)
	a_.setAngle(0);
	b_.setAngle(1);
	a_.jumpToEnd();
	b_.jumpToEnd();
	return true;
}

void ReplayEngine::refreshSession()
{
	if (index_)
		index_->refresh();
}

ReplayPlayer *ReplayEngine::channel(char id)
{
	if (id == 'A' || id == 'a')
		return &a_;
	if (id == 'B' || id == 'b')
		return &b_;
	return nullptr;
}

std::string ReplayEngine::transportJson() const
{
	obs_data_t *root = obs_data_create();
	obs_data_set_bool(root, "sessionLoaded", (bool)index_);
	obs_data_set_bool(root, "linked", linked_);
	obs_data_set_bool(root, "followLive", followLive_);

	// broadcast-style timeline: the bar always spans up to "NOW".
	// seekableNs   = indexed footage (playable right now)
	// durationNs   = live edge (= NOW while recording)
	int64_t indexed = index_ ? index_->masterDurationNs() : 0;
	int64_t liveEdge = indexed;
	bool rec = ReplayCore::instance().isRecording();
	if (rec) {
		int64_t now = ReplayCore::instance().masterNowNs();
		if (now > liveEdge)
			liveEdge = now;
	}
	obs_data_set_bool(root, "recording", rec);
	obs_data_set_int(root, "seekableNs", indexed);
	obs_data_set_int(root, "durationNs", liveEdge);

	const ReplayPlayer *players[2] = {&a_, &b_};
	const char *names[2] = {"A", "B"};
	for (int i = 0; i < 2; i++) {
		obs_data_t *ch = obs_data_create();
		obs_data_set_bool(ch, "playing", players[i]->playing());
		obs_data_set_bool(ch, "reverse", players[i]->reverse());
		obs_data_set_double(ch, "speed", players[i]->speed());
		obs_data_set_int(ch, "positionNs", players[i]->position());
		obs_data_set_int(ch, "angle", players[i]->angle() + 1);
		obs_data_set_obj(root, names[i], ch);
		obs_data_release(ch);
	}

	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

} // namespace multireplay
