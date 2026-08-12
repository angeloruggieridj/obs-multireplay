/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "event-store.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace multireplay {

// Plays events on Replay A (the reference controller: ReplayPlayEvent / PlaySelectedEvent /
// PlayLastEvent, with the optional ...ToOutput scene switch).
//
// One clip at a time: ReplayChannel is handed an explicit (camera, in, out,
// speed) and reports back when that clip reaches its end, which is when the
// queue advances. The A/B pre-roll and crossfade machinery is gone with the
// ffmpeg_source pair it existed to work around — there is one source now.
//
// "To output" uses obs-frontend-api: the configured output scene is put in
// program and the previous scene is restored when the queue completes.
class PlaybackCoordinator {
public:
	static PlaybackCoordinator &instance();

	// Play one or more events (in the given order), all on `angle0` (0-based,
	// the dock's current angle). Speed = the angle's per-angle override if set,
	// else the default (slider) speed.
	bool playEvents(const std::vector<int> &eventIds, int angle0,
			bool toOutput, std::string &errorOut);
	bool playLastEvent(int angle0, bool toOutput, std::string &errorOut);

	// broadcast replayStopEvents.
	void stopEvents();

	bool queueActive() const;

	// Lightweight snapshot of the currently playing item, safe to call from
	// any thread (e.g. the UI poll timer). Returns {false,0,0} when idle.
	struct PlayState {
		bool active = false;
		int eventId = 0;  // id of the event being played
		int angle1 = 0;   // 1-based camera angle currently playing, 0=none
	};
	PlayState playState() const;

	// the reference controller Loop: when on, the queue restarts from the first event.
	void setLoop(bool loop) { loop_ = loop; }
	bool loop() const { return loop_; }

	// the reference controller music toggle: unmute the configured OBS audio source while a
	// queue plays, mute it again at the end.
	void setMusicEnabled(bool enabled) { musicEnabled_ = enabled; }
	bool musicEnabled() const { return musicEnabled_; }

	// Default replay speed (the dock's slider), used for every angle that has
	// no per-angle override. Lives here because the hotkeys queue events too,
	// and they cannot reach the dock.
	void setDefaultSpeedPct(int pct);
	int defaultSpeedPct() const { return defaultSpeedPct_.load(); }

	// Invoked (via the OBS UI task queue) when the playing clip ends by
	// itself. `gen` identifies the queue generation it belongs to, so a
	// callback that outlived its queue is dropped instead of advancing a
	// newer one.
	void onClipFinished(uint64_t gen);

private:
	PlaybackCoordinator() = default;
	void startNext();       // plays queue_[queuePos_]
	void onEventFinished(); // queue advance; caller holds mutex_
	void switchToReplayScene();
	void restorePreviousScene();
	void setMusicMuted(bool muted);

	// One queue item per event, on the angle the operator selected. Speed is
	// resolved at queue-build time (per-angle override, else the default).
	struct QueueItem {
		int eventId;
		int64_t tInNs;
		int64_t tOutNs;
		int angle;    // 0-based
		int speedPct; // 5..400, 100 = 1x
	};

	mutable std::mutex mutex_;
	std::vector<QueueItem> queue_;
	size_t queuePos_ = 0;
	bool active_ = false;
	bool toOutput_ = false;
	// Bumped on every clip start and on every stop: a finish callback that
	// carries an older generation belongs to a queue that no longer exists.
	uint64_t playGen_ = 0;
	std::atomic<bool> loop_{false};
	std::atomic<bool> musicEnabled_{false};
	std::atomic<int> defaultSpeedPct_{100};
	std::string previousSceneName_;
};

} // namespace multireplay
