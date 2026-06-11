/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "event-store.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace multireplay {

// Plays events on channel A (the reference controller: ReplayPlayEvent / PlaySelectedEvent /
// PlayLastEvent, with the optional ...ToOutput scene switch).
//
// "To output" uses obs-frontend-api: the configured replay scene is put in
// program, the events play back-to-back (hard cut between events in M3;
// configurable transitions are M4) and the previous scene is restored when
// the queue completes (the reference controller basic replay flow).
class PlaybackCoordinator {
public:
	static PlaybackCoordinator &instance();

	// Play one or more events (in the given order) on channel A.
	bool playEvents(const std::vector<int> &eventIds, bool toOutput,
			std::string &errorOut);
	bool playLastEvent(bool toOutput, std::string &errorOut);

	// broadcast replayStopEvents.
	void stopEvents();

	bool queueActive() const;

	// the reference controller Loop: when on, the queue restarts from the first event.
	void setLoop(bool loop) { loop_ = loop; }
	bool loop() const { return loop_; }

	// the reference controller music toggle: unmute the configured OBS audio source while a
	// queue plays, mute it again at the end.
	void setMusicEnabled(bool enabled) { musicEnabled_ = enabled; }
	bool musicEnabled() const { return musicEnabled_; }

private:
	PlaybackCoordinator() = default;
	void startNext();        // plays queue_[queuePos_]
	void onEventFinished();  // stop-at-out callback from the player
	void switchToReplayScene();
	void restorePreviousScene();
	void setMusicMuted(bool muted);

	// One queue item per (event, enabled angle): the reference controller plays every checked
	// angle of an event back-to-back. Speed is resolved at queue-build
	// time ("--" inheritance chain).
	struct QueueItem {
		int eventId;
		int64_t tInNs;
		int64_t tOutNs;
		int angle; // 0-based
		double speed;
	};

	mutable std::mutex mutex_;
	std::vector<QueueItem> queue_;
	size_t queuePos_ = 0;
	bool active_ = false;
	bool toOutput_ = false;
	std::atomic<bool> loop_{false};
	std::atomic<bool> musicEnabled_{false};
	std::string previousSceneName_;
};

} // namespace multireplay
