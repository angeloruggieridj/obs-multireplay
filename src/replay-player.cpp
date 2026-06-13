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
	// Cancel any pending event out-point: a stale stop-at from a previous
	// event would freeze the very next play on its first frame.
	stopAtNs_ = -1;
	positionNs_ = std::max<int64_t>(0, positionNs);
	// Force audio re-anchor so we don't emit stale timestamps after the
	// seek (the old audio clock offset is invalid at the new position).
	pendingAudioReset_ = true;
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
	// Changing the camera angle means a new audio stream: re-anchor the
	// clock so the first chunk from the new stream has a clean timestamp.
	pendingAudioReset_ = true;
	wake_.notify_all();
}

void ReplayPlayer::invalidateCache()
{
	gopCache_.clear();
	cachedPath_.clear();
	cachedAngle_ = -1;
	audioPrimed_ = false;
	decoder_.close();
}

// ---------------------------------------------------------------------------
// frameAt: resolve masterNs → segment file → decoded frame.
//
// MUST be called WITHOUT stateMutex_ held: it does blocking I/O (file open,
// seek) and FFmpeg decoding which can take 50-200 ms. The decoder, cache and
// associated fields are owned exclusively by the player thread.
// ---------------------------------------------------------------------------
const DecodedFrame *
ReplayPlayer::frameAt(int64_t masterNs,
		      const std::shared_ptr<SessionIndex> &index)
{
	if (!index)
		return nullptr;

	int snapAngle = angle_.load();
	std::string path;
	int64_t offsetNs = 0;
	if (!index->resolve(snapAngle, masterNs, path, offsetNs))
		return nullptr;

	if (path != cachedPath_ || snapAngle != cachedAngle_) {
		gopCache_.clear();
		if (!decoder_.open(path))
			return nullptr;
		cachedPath_ = path;
		cachedAngle_ = snapAngle;
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
		// Decode forward to the target.
		DecodedFrame f;
		while (gopCache_.back().ptsNs < offsetNs &&
		       decoder_.nextFrame(f)) {
			gopCache_.push_back(std::move(f));
			f = DecodedFrame{};
			if (gopCache_.size() > kMaxCacheFrames)
				gopCache_.pop_front();
		}
		// If forward decode didn't reach the target (decoder stalled /
		// hit EOF), seek explicitly.  This is a safety net; with
		// invalidateCache() called on every seek in threadLoop, this
		// branch fires only during continuous playback when the player
		// crosses a segment boundary within the same angle.
		if (gopCache_.empty() ||
		    gopCache_.back().ptsNs < offsetNs) {
			gopCache_.clear();
			decoder_.seekTo(offsetNs);
			DecodedFrame f2;
			while (decoder_.nextFrame(f2)) {
				gopCache_.push_back(std::move(f2));
				f2 = DecodedFrame{};
				if (gopCache_.back().ptsNs >= offsetNs)
					break;
				if (gopCache_.size() > kMaxCacheFrames)
					gopCache_.pop_front();
			}
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

// Must be called with stateMutex_ held (accesses source_).
// VIDEO ONLY — audio is handled separately in threadLoop so it can be
// drained every tick (preventing burst-on-resume after a pause).
// `ts` = os_gettime_ns() captured once per tick, shared with outputAudio
// so OBS can align audio and video on the same clock reference.
void ReplayPlayer::outputFrame(const DecodedFrame &frame, uint64_t ts)
{
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
	out.timestamp = ts;
	obs_source_output_video(source_, &out);
}

// Called from threadLoop every tick, WITHOUT stateMutex_ held.
// `seekOccurred` = a seek/angle-change happened this tick; the audio decoded
// during seek-to-target (frames before the in-point) must be discarded.
// `nowTs` = os_gettime_ns() captured once per tick (same value passed to
// outputFrame) so video and audio are timestamped on the same reference.
//
// DRIFT GUARD: the internal audio clock advances by exact chunk duration.
// When decode is slow (> one frame per tick) the clock drifts behind the
// wall clock. OBS's audio mixer discards chunks whose end timestamp falls
// before the current mix time (~20 ms window). Guard: if the clock has
// fallen more than 2 video frames behind nowTs, re-anchor to nowTs. This
// causes a brief silent gap but avoids total audio loss for the event.
void ReplayPlayer::outputAudio(obs_source_t *src,
			       std::vector<AudioChunk> chunks,
			       bool seekOccurred,
			       uint64_t nowTs)
{
	bool audible = playing_ && !reverse_ && speed_.load() > 0.99;
	if (!audible || seekOccurred) {
		// Discard: not audible, or this is the seek pre-roll batch.
		// Reset the anchor so the next real chunk starts cleanly.
		audioPrimed_ = false;
		return;
	}

	// 2-frame drift guard: if the audio clock has fallen this far behind
	// real time, re-anchor rather than emitting audio OBS will discard.
	// 2 frames at 30 fps ≈ 66 ms.
	constexpr uint64_t kDriftThresholdNs = 66666666ULL;
	if (!audioPrimed_ || nowTs > audioTimestamp_ + kDriftThresholdNs) {
		audioTimestamp_ = nowTs;
		audioPrimed_ = true;
	}

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
		audio.timestamp = audioTimestamp_;
		audioTimestamp_ += (uint64_t)chunk.frames * 1000000000ULL /
				   SegmentDecoder::kAudioSampleRate;
		obs_source_output_audio(src, &audio);
	}
}

// ---------------------------------------------------------------------------
// threadLoop
//
// Three key design decisions:
//
//  1. Decode outside stateMutex_: frameAt() can take 50-200 ms (disk seek +
//     FFmpeg). Holding stateMutex_ during that time would block the OBS
//     graphics thread (which calls acquireSource()) → frame drops in OBS.
//
//  2. Wall-clock position advancement: position advances by actual elapsed
//     time (capped to 3×frameDurNs), not by a fixed frameDurNs constant.
//     Combined with a compensated sleep (frameDurNs minus work already done),
//     the loop runs at the correct cadence AND the position matches the JS
//     client-side linear interpolation — no accumulated drift, no snap-back.
//
//  3. Audio reset on seek: seekMaster()/setAngle() set pendingAudioReset_.
//     The thread clears it before outputting the first post-seek frame,
//     re-anchoring the audio clock and preventing timestamp collisions.
// ---------------------------------------------------------------------------
void ReplayPlayer::threadLoop()
{
	os_set_thread_name("multireplay-player");

	using Clock = std::chrono::steady_clock;
	auto lastTick = Clock::now();
	int64_t lastRenderedPos = -1;

	while (running_) {
		auto tickStart = Clock::now();
		// Elapsed since the previous iteration — used to advance the
		// playhead position in real time. Capped to 3 frames so a long
		// pause or the very first tick cannot cause a huge jump.
		int64_t frameDurNs = decoder_.isOpen()
					     ? decoder_.frameDurationNs()
					     : 33333333LL;
		int64_t elapsedNs =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				tickStart - lastTick)
				.count();
		elapsedNs = std::min(elapsedNs, frameDurNs * 3);
		lastTick = tickStart;

		std::function<void()> stopCallback;
		int64_t renderPos = -1;
		std::shared_ptr<SessionIndex> indexSnap;
		bool shouldRender = false;

		// ── 1. Advance playhead (brief mutex hold) ────────────────────
		{
			std::unique_lock<std::mutex> lock(stateMutex_);

			if (!index_ || !source_) {
				wake_.wait_for(lock,
					       std::chrono::milliseconds(50));
				lastTick = Clock::now(); // reset after sleep
				continue;
			}

			// frame stepping
			int step = pendingStepFrames_.exchange(0);
			if (step != 0)
				positionNs_ = std::max<int64_t>(
					0,
					positionNs_ + step * frameDurNs);

			// advance by actual wall-clock elapsed time
			if (playing_) {
				double delta = (double)elapsedNs *
					       speed_.load() *
					       (reverse_ ? -1.0 : 1.0);
				int64_t next =
					positionNs_ + (int64_t)delta;
				int64_t maxPos =
					index_->masterDurationNs() - 1;
				if (next <= 0) {
					next = 0;
					playing_ = false;
				} else if (next >= maxPos) {
					next = maxPos;
				}
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

			renderPos = positionNs_.load();
			indexSnap = index_;
			shouldRender = (renderPos != lastRenderedPos);
		}
		// ── mutex released ─────────────────────────────────────────────

		// ── 2. Decode WITHOUT mutex (I/O + FFmpeg, potentially slow) ──
		if (shouldRender) {
			// Capture and clear the seek/angle-change flag BEFORE
			// calling frameAt(): the flag tells us that the audio
			// decoded during seek pre-roll must be discarded (it
			// covers frames before the event in-point).
			const bool seekOccurred =
				pendingAudioReset_.exchange(false);
			if (seekOccurred) {
				// CRITICAL: close the decoder and clear the cache
				// so frameAt() always reopens the file and seeks to
				// the new position from scratch.  Without this the
				// decoder is at EOF of the previous event's segment;
				// nextFrame() returns false immediately → cache never
				// advances → frozen frame on every subsequent event
				// and on every seek from the bar.
				invalidateCache(); // thread-exclusive, safe here
				lastRenderedPos = -1; // force re-render even if pos unchanged
			}

			const DecodedFrame *frame =
				frameAt(renderPos, indexSnap);

			// Drain audio immediately after decode.  decoder_ is
			// thread-exclusive so takeAudio() is safe here.
			// Draining every tick — not just when we output video —
			// prevents audio accumulating during cache hits or stalls,
			// which would otherwise burst on the next resume → skip.
			auto audioChunks = decoder_.takeAudio();

			// Capture wall-clock time ONCE for both video and audio so
			// OBS sees matching timestamps and can sync them correctly.
			// Capturing after decode (which can take 50–200 ms) ensures
			// the timestamp is always close to "now" → OBS won't discard.
			const uint64_t nowTs = (uint64_t)os_gettime_ns();

			if (frame) {
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (source_) {
					outputFrame(*frame, nowTs);
					lastRenderedPos = renderPos;
				}
			}

			// Output audio outside the mutex.
			// Pass seekOccurred so the first post-seek batch (pre-roll
			// frames decoded to reach the in-point) is discarded.
			if (!audioChunks.empty()) {
				obs_source_t *src = acquireSource();
				if (src) {
					outputAudio(src, std::move(audioChunks),
						    seekOccurred, nowTs);
					obs_source_release(src);
				}
			}
		} else {
			// Paused or position unchanged: drain the decoder buffer
			// and discard so we don't accumulate audio that would
			// burst on the next resume.
			if (decoder_.isOpen())
				decoder_.takeAudio(); // drain & discard
		}

		// ── 3. Stop callback (outside lock: may call back into us) ────
		if (stopCallback)
			stopCallback();

		// ── 4. Compensated sleep: target one-frame cadence ────────────
		// Subtract the time already spent this tick so the total
		// interval (work + sleep) stays close to frameDurNs.  This
		// keeps the real-time position advancement in step with the JS
		// client-side linear interpolation, eliminating snap-back.
		auto tickEnd = Clock::now();
		int64_t tickSpentNs =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				tickEnd - tickStart)
				.count();
		// Always yield at least 1 ms to avoid busy-looping if decode
		// takes longer than one frame interval; 33 ms when paused.
		int64_t sleepNs = std::max<int64_t>(
			1000000LL,
			(playing_ ? frameDurNs : 33333333LL) - tickSpentNs);
		{
			std::unique_lock<std::mutex> idle(stateMutex_);
			wake_.wait_for(idle,
				       std::chrono::nanoseconds(sleepNs));
		}
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
