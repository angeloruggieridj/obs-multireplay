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
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "packet-types.hpp"

namespace multireplay {

// the reference controller has two replay channels and so do we. They are two INSTANCES of this
// class, not two classes and not one class with a second set of fields: a
// channel is a source, a worker thread and a clip, and there is nothing about
// the second one that differs from the first except its name.
//
// One registered source TYPE, two named inputs. The type is inert (see
// ChannelSource in the .cpp), so nothing about it needs to know which channel
// it belongs to; the instance pushing frames into it does.
enum class Which { A = 0, B = 1 };
inline constexpr int kChannels = 2;
inline const char *channelLetter(Which w)
{
	return w == Which::A ? "A" : "B";
}

class ReplayChannel {
public:
	// Defaulted to A so the call sites that only ever meant "the replay
	// channel" keep saying exactly that.
	static ReplayChannel &instance(Which which = Which::A);

	// Registers the source type (once, however many channels there are) and
	// binds this instance to its own name. Call from obs_module_load, before
	// any scene collection can reference it.
	void load();
	void unload();

	// Adopt the existing source of our name or create one. Safe to call
	// repeatedly (FINISHED_LOADING / SCENE_COLLECTION_CHANGED).
	void ensureSource();

	Which which() const { return which_; }
	// The OBS input name the operator will look for.
	const char *sourceName() const;
	static const char *sourceNameOf(Which which);

	// Make every scene item that shows the replay input fill the canvas,
	// keeping its aspect ratio (the transform OBS calls "Fit to screen":
	// bounding box = canvas, SCALE_INNER, centred).
	//
	// This is a TRANSFORM, not a picture change: the frames stay at the
	// camera's own resolution and the GPU scales them while compositing, the
	// way it already does for every capture card. It is the only layer where
	// the fix belongs, because the replay input changes size with the angle
	// being played - a 720p camera then a 1080p one - and no fixed scale can
	// be right for both, while a bounding box is right for all of them.
	//
	// `enable` is remembered and re-applied when a clip starts. False does not
	// undo anything: it just stops us touching the operator's transform.
	void applyCanvasFit(bool enable);

	// Where the clip comes from. Auto prefers the ring (no I/O, always
	// exact) and falls back to the recording files for anything older;
	// the explicit values exist so both paths can be exercised and
	// compared, which is how we know they agree.
	enum class Source { Auto, Ring, Segments };

	// Play [inNs, outNs] of `camIndex` at `speedPct` (5..100; 100 = 1x).
	// Replaces anything already playing. Returns false if the range cannot
	// be served exactly — both sources refuse rather than clamp.
	bool play(int camIndex, int64_t inNs, int64_t outNs, int speedPct,
		  std::string &errorOut, Source source = Source::Auto);
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

	// Master-timeline instant of the last frame handed to OBS, 0 if nothing
	// has played yet. This is the playhead the dock draws — there is no
	// free-running position any more, only the clip that is playing.
	int64_t positionNs() const { return stats().lastFrameNs; }

	// Called once when a clip reaches its natural end, from the playback
	// thread, with no lock held. It does NOT fire when playback was replaced
	// or stopped: those are the caller's own doing, and firing there would
	// make a queue advance itself while it is being torn down.
	//
	// The callee must not call play()/stop() inline: both join the very
	// thread that is invoking the callback. Hand the work to another thread
	// (PlaybackCoordinator posts it to the OBS UI task queue).
	void setOnFinished(std::function<void()> fn);

private:
	explicit ReplayChannel(Which which = Which::A) : which_(which) {}
	~ReplayChannel();
	ReplayChannel(const ReplayChannel &) = delete;
	ReplayChannel &operator=(const ReplayChannel &) = delete;

	void playbackLoop();
	void joinWorker();
	void fitSceneItems(); // applyCanvasFit's body; no lock held

	const Which which_;
	obs_source_t *source_ = nullptr; // owned (ref held)
	mutable std::mutex mutex_;

	// The clip handed to the worker, snapshotted out of the ring so the
	// encoder thread is never blocked by playback.
	std::vector<LivePacket> clip_;
	StreamConfig config_;
	int64_t presentInNs_ = 0;
	int64_t presentOutNs_ = 0;
	int speedPct_ = 100;
	// Snapshotted by the worker when it starts, so a clip always fires the
	// callback that was installed for IT, never a later one.
	std::function<void()> onFinished_;

	std::thread worker_;
	std::atomic<bool> playing_{false};
	std::atomic<bool> abort_{false};
	std::atomic<bool> fitToCanvas_{true}; // see applyCanvasFit()

	mutable std::mutex statsMutex_;
	PlaybackStats stats_;
};

} // namespace multireplay
