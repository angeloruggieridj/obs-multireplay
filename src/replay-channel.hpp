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

Reverse (v1.3) is the one thing that is NOT just a different spacing, because
nothing here decodes backwards: a GOP is decoded forward into a bounded picture
cache and then handed to OBS newest-first. See reverse-plan.hpp for the schedule
and playReverse() for the two threads that run it.
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

class ReplayDecoder; // playReverse() borrows the caller's, see the .cpp

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
	// Stop playing and DROP the reference to our input, without unregistering
	// the type. Called when OBS is about to clear scene data (a collection
	// change, or shutdown): a reference still held at that moment is reported
	// to the operator as a plugin that failed to release its resources — and
	// reported in a dialog, on the way out. ensureSource() re-adopts afterwards.
	void releaseSource();

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

	// Which way the pictures come out. Forward is a paced walk through the
	// decoder's output; Reverse decodes each GOP forward and shows it
	// newest-first out of a bounded cache (reverse-plan.hpp).
	enum class Direction { Forward, Reverse };

	// Everything one clip needs. A struct rather than a ninth positional
	// argument: `play(cam, in, out, 50, err, Source::Auto, Direction::Reverse,
	// 2)` is a line nobody can read back, and reverse added two knobs that
	// only matter together.
	struct PlayRequest {
		int camIndex = 0;
		int64_t inNs = 0;
		int64_t outNs = 0;
		int speedPct = 100; // 5..400, 100 = 1x
		Source source = Source::Auto;
		Direction direction = Direction::Forward;
		// Reverse only: stop after this many pictures (0 = the whole
		// range). This is what makes a one-frame step back a STEP and
		// not a short backwards clip — the cache is filled a GOP at a
		// time either way, but only two pictures are ever shown.
		int maxFrames = 0;
	};

	// Play the request. Replaces anything already playing. Returns false if
	// the range cannot be served exactly — both sources refuse rather than
	// clamp.
	bool play(const PlayRequest &req, std::string &errorOut);

	// Play [inNs, outNs] of `camIndex` at `speedPct` (5..400; 100 = 1x),
	// forwards. Kept because most call sites mean exactly this.
	bool play(int camIndex, int64_t inNs, int64_t outNs, int speedPct,
		  std::string &errorOut, Source source = Source::Auto);
	void stop();
	// Stop AND forget. A new (or newly opened) project has no clip on any bay,
	// so the stats have to go with it: the dock decides whether a bay's box has
	// anything to show by asking whether it has ever pushed a frame, and a
	// channel that remembers last project's clip kept a stale picture on screen
	// under a project that was empty.
	void reset();
	bool playing() const { return playing_.load(); }

	// --- pause and speed, WITHOUT restarting the clip ----------------------
	// Both used to be done by stopping and playing again from the IN, and both
	// were wrong for the same reason: the operator is looking at a frame, and
	// the answer to "pause" or "half speed" is not "go back to the beginning".
	//
	// The pacing is an ANCHOR (a wall instant paired with the master instant
	// shown at it) rather than a start time plus an offset. A speed change
	// re-anchors on the picture currently on screen, so every frame after it is
	// spaced by the new speed and none of the frames already shown are
	// retro-dated. A pause slides the anchor forward for as long as it lasts,
	// which is exactly what "this time did not happen" means to a clip.
	void setSpeedPct(int pct);
	int speedPct() const;
	void setPaused(bool paused);
	bool paused() const { return paused_.load(); }

	// Add-ref'd; caller releases. Null before ensureSource().
	obs_source_t *acquireSource();

	struct PlaybackStats {
		uint64_t framesPushed = 0;
		uint64_t audioPushed = 0;   // audio buffers handed to OBS
		uint64_t framesPreroll = 0; // decoded to prime, never shown
		int64_t firstFrameNs = 0;
		int64_t lastFrameNs = 0;
		bool lastRunCompleted = false;
		// Which way the last run went. firstFrameNs > lastFrameNs is the
		// same fact, but only for a run of more than one picture — a
		// one-frame step back is indistinguishable from a step forward by
		// its instants alone.
		bool reverse = false;
		// High-water mark of the reverse picture cache, so "did it stay
		// inside its budget" is a measurement and not a belief. 0 for a
		// forward run, which caches nothing.
		size_t cacheBytesPeak = 0;
		// Pictures the reverse plan intended to show, against
		// framesPushed. A schedule that quietly loses a slice of a range
		// is the failure mode of this design, and nothing outside could
		// see it.
		int framesPlanned = 0;
	};
	PlaybackStats stats() const;

	// Master-timeline instant of the last frame handed to OBS. This is the
	// playhead the dock draws — there is no free-running position any more,
	// only the clip that is playing. Ask hasPosition() whether it means
	// anything: an instant of 0 is a legitimate one and instants BEFORE the
	// machine's last boot are negative (session-clock.hpp, kNoInstant).
	int64_t positionNs() const { return stats().lastFrameNs; }
	bool hasPosition() const { return stats().framesPushed > 0; }

	// The reverse picture cache's ceiling, per channel. One GOP of 1080p at
	// keyint_sec = 1 is ~155 MB (reverse-plan.hpp) and the two threads of
	// playReverse() hold at most one cache each, so half of this is what one
	// decode pass may keep.
	static constexpr size_t kReverseCacheBudgetBytes = 160u * 1024u * 1024u;

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
	// The reverse half of playbackLoop. Runs a second thread that decodes
	// the plan's passes into picture caches while this one paces the cache it
	// already has — without it every GOP boundary is a freeze the length of a
	// GOP decode (~100 ms on an iGPU), once a second, on air.
	//
	// Returns true if the run reached its own end (or its maxFrames), false
	// if it was aborted or could not decode.
	// The speed is not a parameter: it lives in the anchor, so that changing
	// it mid-clip is the same operation as setting it (see setSpeedPct).
	bool playReverse(obs_source_t *source, ReplayDecoder &dec,
			 const std::vector<LivePacket> &clip, int64_t presentInNs,
			 int64_t presentOutNs, int maxFrames);
	void joinWorker();
	void fitSceneItems(); // applyCanvasFit's body; no lock held

	// Where the clip is on the wall clock. See setSpeedPct() for why this is
	// an anchor and not a start time.
	struct Pacing {
		uint64_t anchorWall = 0;  // when anchorMaster was due
		int64_t anchorMaster = 0; // master instant of that frame
		double speed = 1.0;
		int dir = +1; // +1 forwards, -1 backwards
		bool paused = false;
		uint64_t pausedAt = 0;
		// The master instant of the frame on screen. A speed change
		// re-anchors here, because that frame is what the operator is
		// looking at when he turns the dial.
		int64_t shownMaster = 0;
		bool seated = false;

		uint64_t dueFor(int64_t masterNs) const
		{
			const double off = (double)(dir * (masterNs - anchorMaster));
			return anchorWall + (uint64_t)(off / (speed > 0 ? speed : 1.0));
		}
	};
	// Seat the anchor on the first frame of a run.
	void seatPacing(int64_t masterNs, int dir);
	// Sleep until that frame is due, honouring a speed change or a pause that
	// arrives mid-wait; returns the wall instant it is due at, or 0 if the run
	// was aborted. Also publishes the frame as the one on screen.
	uint64_t waitForFrame(int64_t masterNs);

	mutable std::mutex pacingMutex_;
	Pacing pacing_;
	std::atomic<bool> paused_{false};

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
	Direction direction_ = Direction::Forward;
	int maxFrames_ = 0;
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
