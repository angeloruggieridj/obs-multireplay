/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

ReplayChannel — "Replay A" as a real OBS input.

the reference controller's Replay A and Replay B are ordinary the reference controller inputs: the operator puts them
wherever they like and gets transitions, the audio mixer and every output for
free. This registers the same thing for OBS — a plugin-provided async source the
operator drops into any scene.

That single decision deletes a whole category of problems the old engine had.
There is no managed scene to keep in sync, no managed transition to adopt after
a scene-collection reload, no A/B pair of ffmpeg_sources to pre-roll and cut
between, and no transform to re-apply. Those existed only to work around
ffmpeg_source, and they go away with it.

Playback runs on its own thread: pull the packets for the range out of the ring,
decode, discard the pre-roll before the marked frame, and push frames out paced
by the requested speed. Slow motion is simply a wider spacing between frames —
no re-encoding, no reopening, no seeking.
*/

#pragma once

#include <obs.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "packet-types.hpp"

namespace multireplay {

class ReplayChannel {
public:
	static ReplayChannel &instance();

	// Registers the source type. Call from obs_module_load, before any
	// scene collection can reference it.
	void load();
	void unload();

	// Adopt the existing source of our name or create one. Safe to call
	// repeatedly (FINISHED_LOADING / SCENE_COLLECTION_CHANGED).
	void ensureSource();

	// The OBS input name the operator will look for.
	static const char *sourceName();

	// Play [inNs, outNs] of `camIndex` at `speedPct` (5..100; 100 = 1x).
	// Replaces anything already playing. Returns false if the range cannot
	// be served exactly — the ring refuses rather than clamps.
	bool play(int camIndex, int64_t inNs, int64_t outNs, int speedPct,
		  std::string &errorOut);
	void stop();
	bool playing() const { return playing_.load(); }

	// Add-ref'd; caller releases. Null before ensureSource().
	obs_source_t *acquireSource();

	struct PlaybackStats {
		uint64_t framesPushed = 0;
		uint64_t audioPushed = 0;   // audio buffers handed to OBS
		uint64_t framesPreroll = 0; // decoded to prime, never shown
		int64_t firstFrameNs = 0;
		int64_t lastFrameNs = 0;
		bool lastRunCompleted = false;
	};
	PlaybackStats stats() const;

private:
	ReplayChannel() = default;
	~ReplayChannel();
	ReplayChannel(const ReplayChannel &) = delete;
	ReplayChannel &operator=(const ReplayChannel &) = delete;

	void playbackLoop();
	void joinWorker();

	obs_source_t *source_ = nullptr; // owned (ref held)
	mutable std::mutex mutex_;

	// The clip handed to the worker, snapshotted out of the ring so the
	// encoder thread is never blocked by playback.
	std::vector<LivePacket> clip_;
	StreamConfig config_;
	int64_t presentInNs_ = 0;
	int64_t presentOutNs_ = 0;
	int speedPct_ = 100;

	std::thread worker_;
	std::atomic<bool> playing_{false};
	std::atomic<bool> abort_{false};

	mutable std::mutex statsMutex_;
	PlaybackStats stats_;
};

} // namespace multireplay
