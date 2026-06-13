/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "decoder.hpp"
#include "session-index.hpp"

#include <obs-module.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace multireplay {

// Playback engine for one channel (Replay A or B).
//
// Model: an output tick runs at the recording frame rate; on every tick the
// master position advances by frameDuration * speed (negative when playing
// in reverse) and the frame at that position for the selected camera angle
// is pushed to the attached OBS source (obs_source_output_video).
//
// Reverse playback / frame stepping rely on a GOP cache: recordings use a
// 1-second GOP, so the cache holds at most ~1s of decoded frames.
class ReplayPlayer {
public:
	explicit ReplayPlayer(char channelId);
	~ReplayPlayer();

	void start();
	void stop();

	void attachSource(obs_source_t *source);
	void detachSource(obs_source_t *source);
	// Add-ref'd current source (caller releases), nullptr if none.
	obs_source_t *acquireSource();

	void setIndex(std::shared_ptr<SessionIndex> index);

	// --- Transport ---
	void setPlaying(bool playing);
	bool playing() const { return playing_; }
	void setSpeed(double speed); // 0.0 .. 1.0
	double speed() const { return speed_; }
	void setReverse(bool reverse);
	bool reverse() const { return reverse_; }
	void changeDirection() { setReverse(!reverse_); }
	void stepFrames(int frames); // pause + jump N frames (±)
	void seekMaster(int64_t positionNs);
	int64_t position() const { return positionNs_; }
	void jumpToEnd(); // "jump to now" = latest indexed frame
	void setAngle(int camIndex); // 0-based
	int angle() const { return angle_; }

	// M3: stop automatically when the playhead crosses `ns`
	// (-1 disables). `onStop` fires once on auto-stop.
	void setStopAt(int64_t ns, std::function<void()> onStop = nullptr);

	char channelId() const { return channelId_; }

private:
	void threadLoop();
	// Ensure gopCache_ covers `masterNs` for the given index/angle;
	// returns the frame closest at/below masterNs (nullptr if none).
	// Must be called WITHOUT stateMutex_ held (does I/O + FFmpeg decode).
	const DecodedFrame *frameAt(int64_t masterNs,
				    const std::shared_ptr<SessionIndex> &index);
	// Must be called WITH stateMutex_ held (writes to source_).
	// VIDEO only — audio is handled separately to avoid burst-on-resume.
	void outputFrame(const DecodedFrame &frame);
	// Called WITHOUT stateMutex_ held. Emits audio chunks; discards if not
	// at audible speed or if seekOccurred (pre-roll frames from seek-to-target
	// must not be played — they precede the event in-point).
	void outputAudio(obs_source_t *src,
			 std::vector<AudioChunk> chunks,
			 bool seekOccurred);
	void invalidateCache();

	char channelId_;
	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> playing_{false};
	std::atomic<bool> reverse_{false};
	std::atomic<double> speed_{1.0};
	std::atomic<int64_t> positionNs_{0};
	std::atomic<int> angle_{0};
	std::atomic<int> pendingStepFrames_{0};
	std::atomic<int64_t> stopAtNs_{-1};
	std::function<void()> onStop_; // guarded by stateMutex_
	// Set by seekMaster()/setAngle() to force audio re-anchor on the
	// next output tick (avoids stale timestamps after a seek or cut).
	std::atomic<bool> pendingAudioReset_{false};

	std::mutex stateMutex_; // protects index_, source_ (and onStop_)
	// decoder_, gopCache_, cachedPath_, cachedAngle_, audioPrimed_,
	// outputTimestamp_, audioTimestamp_ are thread-exclusive (threadLoop only)
	// — they must NOT be accessed while stateMutex_ is held to avoid the
	// long I/O+decode stall on the graphics thread.
	std::condition_variable wake_;
	std::shared_ptr<SessionIndex> index_;
	obs_source_t *source_ = nullptr; // weak: set/cleared by the source

	SegmentDecoder decoder_;
	std::deque<DecodedFrame> gopCache_;
	std::string cachedPath_;
	int cachedAngle_ = -1;
	// Audio clock in the os_gettime_ns() domain.  Anchored once at the
	// start of each playback segment, then advanced by chunk duration.
	uint64_t audioTimestamp_ = 0;
	bool audioPrimed_ = false;
};

// Owns the two channels and the shared session index. Linked mode mirrors
// transport commands on both channels (the reference controller A|B behaviour).
class ReplayEngine {
public:
	static ReplayEngine &instance();

	void load();
	void unload();

	// (Re)build the index from the configured session folder.
	bool loadSession(const std::string &folder, std::string &errorOut);
	void refreshSession();
	bool sessionLoaded() const { return (bool)index_; }

	ReplayPlayer &channelA() { return a_; }
	ReplayPlayer &channelB() { return b_; }
	ReplayPlayer *channel(char id);

	void setLinked(bool linked) { linked_ = linked; }
	bool linked() const { return linked_; }

	// the reference controller "live follow": when true, the operator is "at NOW" — camera
	// tiles show the live feed. Any seek into the recorded timeline turns
	// it off; the NOW button turns it back on.
	void setFollowLive(bool follow) { followLive_ = follow; }
	bool followLive() const { return followLive_; }

	// Resolve a master-timeline instant to (file, in-file offset) for an
	// angle. Used by the clip exporter. False when no session is loaded.
	bool resolveTime(int camIndex, int64_t masterNs, std::string &pathOut,
			 int64_t &offsetNsOut) const
	{
		return index_ ? index_->resolve(camIndex, masterNs, pathOut,
						offsetNsOut)
			      : false;
	}

	void clearSession()
	{
		index_.reset();
		a_.setIndex(nullptr);
		b_.setIndex(nullptr);
	}

	// Apply a transport command to a channel, mirrored to the other
	// when linked. `id` is 'A' or 'B'.
	template<typename Fn> bool applyTransport(char id, Fn &&fn)
	{
		ReplayPlayer *p = channel(id);
		if (!p)
			return false;
		fn(*p);
		if (linked_) {
			ReplayPlayer *other = (p == &a_) ? &b_ : &a_;
			fn(*other);
		}
		return true;
	}

	std::string transportJson() const;

private:
	ReplayEngine() : a_('A'), b_('B') {}

	ReplayPlayer a_;
	ReplayPlayer b_;
	std::atomic<bool> linked_{true};
	std::atomic<bool> followLive_{true};
	std::shared_ptr<SessionIndex> index_;
};

} // namespace multireplay
