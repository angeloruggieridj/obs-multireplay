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

	// How many clips one event is worth.
	enum class AngleMode {
		// the reference controller "play event": every angle the operator ENABLED on that
		// event, in angle order, one after the other. Marking C1 and C2
		// means seeing C1 then C2 - which is the point of marking two.
		AllEnabled,
		// Just the requested angle: the angle buttons and the speed
		// slider re-cue the clip the operator is watching, and they must
		// show THAT camera, not restart the whole sequence.
		Single,
	};

	// Play one or more events, in the given order. Speed = the played angle's
	// per-angle override if set, else the default (slider) speed. `angle0`
	// (0-based) selects the angle in Single mode, and is the fallback in
	// AllEnabled mode for an event with no angle enabled at all.
	bool playEvents(const std::vector<int> &eventIds, int angle0,
			bool toOutput, std::string &errorOut,
			AngleMode mode = AngleMode::AllEnabled);
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
		// Clips in the queue and which one is on: an event with two
		// angles enabled is TWO clips, so this is how the dock (and the
		// gate) tell "playing one angle" from "playing all of them".
		int queued = 0;
		int queuePos = 0; // 1-based position of the clip on air
		// The whole queue's angles (1-based), in play order. "Which
		// cameras, in which order" is the property the operator actually
		// asked for, and a count alone cannot express it: [1,2] and
		// [2,2] both queue two clips. Nothing outside could see the
		// queue's shape, so no test could ever fail on a wrong one.
		std::vector<int> queuedAngles;
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
