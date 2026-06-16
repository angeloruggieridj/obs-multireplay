/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

MediaReplay — playback engine built on OBS' own Media Source (ffmpeg_source)
instead of a hand-rolled FFmpeg decode + async-push pipeline.

Rationale (pivot): OBS' Media Source already does robust A/V sync, hardware
decode and variable-speed playback. We point it at the recorded segment file
for the selected camera angle, seek to the in-point and let OBS play it. This
removes the custom audio clock (the source of the audio dropouts) and a large
amount of decode code.

A single managed `ffmpeg_source` named like ReplaySource.A is created/owned by
the plugin; the operator adds it to a scene to see/hear the replay. The dock
renders it in its preview and drives transport here; the coordinator drives
event playback (seek to in-point, auto-stop at the out-point) here too.

Trade-off vs the old engine: no true reverse playback and no sub-frame accurate
stepping (Media Source seeks to the nearest position); the timeline is scrubbed
by seeking. Slow-motion is supported (speed 5%..100%).
*/

#pragma once

#include <obs.h>

#include "session-index.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace multireplay {

class MediaReplay {
public:
	static MediaReplay &instance();

	void load();   // start the monitor thread
	void unload(); // stop monitor, release the media source

	// Create or adopt the managed Media Source. Safe to call repeatedly
	// (on FINISHED_LOADING / SCENE_COLLECTION_CHANGED): adopts an existing
	// source of the same name (restored from the scene collection) or
	// creates a fresh one and applies our control flags.
	void ensureSource();

	// --- Session ---
	bool loadSession(const std::string &folder, std::string &errorOut);
	void refreshSession();
	void clearSession(); // drop the index (stale-session guard at REC start)
	bool sessionLoaded() const;
	int64_t footageDurationNs() const; // indexed footage length (live edge)
	std::vector<SessionInfo> sessionInfos() const;

	// --- Live follow (the reference controller NOW) ---
	void setFollowLive(bool follow) { followLive_ = follow; }
	bool followLive() const { return followLive_; }

	// --- Transport ---
	void setPlaying(bool playing);
	void togglePlay();
	bool playing() const;
	void setSpeed(double speed); // 0.05 .. 1.0
	double speed() const { return speedPct_.load() / 100.0; }
	void setAngle(int camIndex0); // 0-based
	int angle() const { return angle_.load(); }
	void seekMaster(int64_t masterNs);
	int64_t position() const; // master-timeline ns
	void jumpToEnd();
	void stepFrames(int frames);

	// --- Event playback (replay-to-output) ---
	// Seek to `tInNs` on `angle0`, play at `speed`, auto-stop at `tOutNs`
	// (chaining across segment splits). `onDone` fires once, asynchronously,
	// when the event finishes naturally. Returns false (and does NOT call
	// `onDone`) if the event cannot start; the caller advances its queue.
	// Replaces any event in progress.
	bool playEvent(int64_t tInNs, int64_t tOutNs, int angle0, double speed,
		       std::function<void()> onDone);
	void stopEvent();

	// --- Preview / export helpers ---
	obs_source_t *acquireSource(); // add-ref'd, caller releases (nullptr ok)
	bool resolveTime(int camIndex, int64_t masterNs, std::string &pathOut,
			 int64_t &offsetNsOut) const;

	// Lightweight transport snapshot — avoids JSON alloc/serialize on every
	// poll tick. Read by the dock UI at ~30fps.
	struct TransportState {
		bool sessionLoaded = false;
		bool followLive = false;
		bool recording = false;
		int64_t seekableNs = 0;
		int64_t durationNs = 0;
		bool playing = false;
		double speed = 1.0;
		int64_t positionNs = 0;
		int angle = 1;              // 1-based
		bool eventActive = false;   // event replay in progress
	};
	TransportState transportState() const;

private:
	MediaReplay() = default;
	~MediaReplay();
	MediaReplay(const MediaReplay &) = delete;
	MediaReplay &operator=(const MediaReplay &) = delete;

	void monitorLoop();
	// Load `path` into the media source with the given speed (one update),
	// arming a pending seek to `seekMs` applied once the media is ready.
	// Caller holds mutex_.
	void loadFileLocked(const std::string &path, int speedPct,
			    int64_t seekMs, bool play);
	int64_t mediaTimeNs() const; // current media time (ns), 0 if unloaded

	mutable std::mutex mutex_;
	obs_source_t *mediaSource_ = nullptr; // owned (ref held)

	std::shared_ptr<SessionIndex> index_;
	std::atomic<int> angle_{0};           // 0-based camera
	std::atomic<int> speedPct_{100};      // 5..100
	std::atomic<bool> followLive_{true};

	// Currently loaded segment: master ns at media time 0, and its path.
	std::string loadedPath_;
	int64_t segBaseNs_ = 0;

	// Pending load/seek applied by the monitor once the media reports ready.
	bool pendingLoad_ = false;
	int64_t pendingSeekMs_ = 0;
	bool pendingPlay_ = false;
	// True for one monitorLoop cycle after a seek is issued but before play
	// starts. Guards against the async seek race: obs_source_media_set_time()
	// posts to the decode thread; without this delay play() fires before the
	// seek completes and the source briefly plays from frame 0.
	bool pendingSeekApplied_ = false;

	// Event state.
	bool eventActive_ = false;
	int64_t eventOutNs_ = -1;
	int64_t eventDurationNs_ = 0;
	std::function<void()> eventOnDone_;
	// Wall-clock OUT detection: avoids relying on obs_source_media_get_time()
	// which can return stale values immediately after a seek.
	std::chrono::steady_clock::time_point eventPlayStartWall_;
	bool eventPlayStarted_ = false;

	// Monitor thread.
	std::thread monitor_;
	std::atomic<bool> running_{false};
	std::condition_variable wake_;
};

} // namespace multireplay
