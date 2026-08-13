/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "playback-coordinator.hpp"
#include "replay-core.hpp"
#include "replay-channel.hpp"
#include "packet-tap.hpp"
#include "segment-index.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace multireplay {

namespace {

// obs_frontend_set_current_scene must run on the UI thread.
struct SceneSwitchCtx {
	std::string sceneName;
	std::string *saveCurrentInto; // optional: record the previous scene
};

// obs_queue_task(OBS_TASK_UI, …, wait=true) hands straight to OBS Studio's
// handler, which is a Qt::BlockingQueuedConnection onto the GUI thread — from
// the GUI thread itself that is a self-deadlock, with no early-out in libobs
// (obs.c only short-circuits same-thread dispatch for the GRAPHICS/AUDIO
// queues). Most callers here ARE the GUI thread (dock buttons), so do the
// short-circuit ourselves.
void runOnUi(obs_task_t task, void *param, bool wait)
{
	if (wait && obs_in_task_thread(OBS_TASK_UI)) {
		task(param);
		return;
	}
	obs_queue_task(OBS_TASK_UI, task, param, wait);
}

void switchSceneTask(void *param)
{
	auto *ctx = static_cast<SceneSwitchCtx *>(param);

	if (ctx->saveCurrentInto) {
		obs_source_t *current = obs_frontend_get_current_scene();
		if (current) {
			const char *name = obs_source_get_name(current);
			const std::string cur = name ? name : "";
			// Re-triggering a replay while the replay scene is
			// already on air must not record IT as "the scene to go
			// back to" — that would strand program on the replay
			// scene once the queue drains.
			if (cur != ctx->sceneName)
				*ctx->saveCurrentInto = cur;
			obs_source_release(current);
		}
	}

	obs_source_t *scene =
		obs_get_source_by_name(ctx->sceneName.c_str());
	if (scene) {
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	} else {
		obs_log(LOG_WARNING, "coordinator: scene '%s' not found",
			ctx->sceneName.c_str());
	}
	delete ctx;
}

// The engine reports the end of a clip from its playback thread, and
// ReplayChannel::play() joins that very thread — so advancing the queue inline
// would join self. Hop onto the UI task queue instead, which is where the
// scene switching has to happen anyway.
void finishedTask(void *param)
{
	const uint64_t gen = (uint64_t)(uintptr_t)param;
	PlaybackCoordinator::instance().onClipFinished(gen);
}

} // namespace

PlaybackCoordinator &PlaybackCoordinator::instance()
{
	static PlaybackCoordinator coordinator;
	return coordinator;
}

void PlaybackCoordinator::setDefaultSpeedPct(int pct)
{
	defaultSpeedPct_.store(std::clamp(pct, 5, 400));
}

void PlaybackCoordinator::onClipFinished(uint64_t gen)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_ || gen != playGen_)
		return; // the queue this belonged to is gone
	onEventFinished();
}

bool PlaybackCoordinator::playEvents(const std::vector<int> &eventIds,
				     int angle0, bool toOutput,
				     std::string &errorOut, AngleMode mode)
{
	// Which angles could go on air at all: a camera wired to that slot, live
	// packets held for it, or footage anchored on it. The enabled flags outlive
	// a config change and the hotkeys can mark on any slot, so without this an
	// event enabled on angle 5 of a two-camera rig queued a clip play() then
	// refused - the queue said three clips and the operator saw two. Keeping
	// the queue equal to what will actually run is what makes the dock's and
	// the gate's reading of it mean anything.
	//
	// Computed BEFORE our own lock is taken, never under it: this asks three
	// other singletons for their state, and holding mutex_ across those makes
	// every future lock order in this plugin our problem. Nothing here needs
	// the queue.
	bool playableAngle[kEventAngles] = {};
	{
		const Config cfg = ReplayCore::instance().getConfig();
		for (int a = 0; a < kEventAngles && a < kMaxCameras; a++)
			playableAngle[a] =
				!cfg.cameras[a].sourceName.empty() ||
				PacketTap::instance().newestNs(a) > 0 ||
				SegmentIndex::instance().oldestNs(a) > 0;
	}
	const auto playable = [&playableAngle](int a) {
		return a >= 0 && a < kEventAngles && playableAngle[a];
	};

	// Snapshot the events too, for the same reason: EventStore has its own
	// lock and the dock mutates it from the GUI thread while this runs.
	std::vector<ReplayEvent> events;
	// The event's own speed WITH the reference controller inheritance (the previous event's, when
	// it has none of its own). Resolved here, next to the snapshot and outside
	// our lock, because it asks EventStore — which the dock is mutating from
	// the GUI thread while this runs.
	std::vector<double> eventSpeed;
	events.reserve(eventIds.size());
	eventSpeed.reserve(eventIds.size());
	for (int id : eventIds) {
		ReplayEvent ev;
		if (EventStore::instance().get(id, ev) && ev.tOutNs >= 0) {
			events.push_back(std::move(ev));
			eventSpeed.push_back(
				EventStore::instance().resolvedSpeed(id));
		}
	}

	std::lock_guard<std::mutex> lock(mutex_);

	// The previous queue dies HERE, before a single field of the new one is
	// written. startNext() bumps the generation too, but only once it has an
	// item it likes: a play request that ends up queueing nothing at all (or
	// whose every item is unplayable) used to leave the old queue's finish
	// callback armed with a generation that was still current, so the clip on
	// air could advance a queue the operator had already replaced. Cancelling
	// up front makes "a new play request cancels the old one" an invariant
	// instead of a side effect of the happy path.
	playGen_++;

	int ang = std::clamp(angle0, 0, kEventAngles - 1);
	const int defPct = defaultSpeedPct_.load();

	std::vector<QueueItem> items;
	// Three tiers, most specific first: the ANGLE's own override (the same
	// event at 100% on the wide and 50% on the tight, which is what the
	// per-angle field is for), then the EVENT's speed with the reference controller inheritance,
	// then the slider default. Each tier only applies where the one above said
	// nothing.
	const auto push = [&](const ReplayEvent &ev, double evSpeed, int a) {
		int pct = defPct;
		if (ev.angles[a].speed >= 0)
			pct = (int)std::lround(ev.angles[a].speed * 100.0);
		else if (evSpeed >= 0)
			pct = (int)std::lround(evSpeed * 100.0);
		items.push_back(
			{ev.id, ev.tInNs, ev.tOutNs, a, std::clamp(pct, 5, 400)});
	};

	// Set when Single mode had to refuse the angle it was asked for, so the
	// operator gets told WHICH camera has nothing rather than a generic "no".
	int refusedAngle = -1;

	for (size_t ei = 0; ei < events.size(); ei++) {
		const ReplayEvent &ev = events[ei];
		const double evSpeed = eventSpeed[ei];
		if (mode == AngleMode::AllEnabled) {
			// One clip per enabled angle, in angle order. Enabling C1
			// and C2 on a mark is a request to SEE both, one after the
			// other; queueing only the operator's current angle threw
			// the other one away, and the log said so - every replay
			// read "[1/1]" no matter how many angles were on.
			int before = (int)items.size();
			for (int a = 0; a < kEventAngles; a++) {
				if (!ev.angles[a].enabled)
					continue;
				if (!playable(a)) {
					// Flagged but unservable (a slot with no
					// camera, no packets and no anchored
					// footage). Dropping it silently made the
					// sequence shorter than the operator's own
					// flags said it would be.
					obs_log(LOG_WARNING,
						"coordinator: event %d has angle %d "
						"enabled but nothing can play it — "
						"left out of the queue",
						ev.id, a + 1);
					continue;
				}
				push(ev, evSpeed, a);
			}
			if ((int)items.size() > before)
				continue;
			// No angle enabled at all: fall back to the requested one
			// rather than silently dropping the event.
			push(ev, evSpeed, ang);
			continue;
		}

		// Single: THE BUTTON WINS.
		//
		// This is the re-cue behind the angle buttons and the speed slider,
		// and the operator pressed camera N because he wants to see camera
		// N. Substituting "the first enabled angle" when N is not flagged on
		// the event was a silent swap: he pressed 1, the log read
		// "angles=[2]", camera 2 played, and nothing anywhere said why — it
		// is precisely what made the angle model impossible to work out from
		// using it. An angle with nothing behind it is refused out loud
		// instead (the dock puts the message on the status line).
		if (!playable(ang)) {
			refusedAngle = ang;
			continue;
		}
		push(ev, evSpeed, ang);
	}
	if (items.empty()) {
		if (refusedAngle >= 0)
			errorOut = "camera " + std::to_string(refusedAngle + 1) +
				   " has no footage for this event (no camera "
				   "wired to it, nothing in the ring and nothing "
				   "recorded)";
		else
			errorOut = "no playable (completed) events selected";
		return false;
	}

	queue_ = std::move(items);
	queuePos_ = 0;
	toOutput_ = toOutput;
	active_ = true;

	// The shape of the queue, once, before anything plays. The per-clip lines
	// below say "[1/2]" but never what the other clip was, so a log from a real
	// session could not answer "did it queue the angles I enabled?" - which is
	// exactly the question the multi-angle reports raised.
	{
		std::string angles;
		for (const QueueItem &q : queue_) {
			if (!angles.empty())
				angles += ",";
			angles += std::to_string(q.angle + 1);
		}
		obs_log(LOG_INFO,
			"coordinator: queued %zu clip(s) from %zu event(s), "
			"mode=%s, angles=[%s]",
			queue_.size(), eventIds.size(),
			mode == AngleMode::AllEnabled ? "all-enabled" : "single",
			angles.c_str());
	}
	// Reviewing, not watching the live edge: the preview must show the replay
	// source from here on.
	ReplayCore::instance().setFollowLive(false);

	if (toOutput_)
		switchToReplayScene();
	if (musicEnabled_)
		setMusicMuted(false);
	lastStartError_.clear();
	startNext();
	// startNext() clears active_ when every item was refused, and it used to
	// return success anyway: the operator pressed play, nothing happened, and
	// no dialog said why. That is the normal outcome for an event of a session
	// whose footage was never anchored, so it has to be said out loud.
	if (!active_) {
		errorOut = lastStartError_.empty()
				   ? "none of those clips can be played: no live "
				     "packets and no anchored recording cover that "
				     "instant"
				   : lastStartError_;
		return false;
	}
	return true;
}

bool PlaybackCoordinator::playLastEvent(int angle0, bool toOutput,
				       std::string &errorOut)
{
	int id = EventStore::instance().lastEventId();
	if (id == 0) {
		errorOut = "no completed events yet";
		return false;
	}
	return playEvents({id}, angle0, toOutput, errorOut);
}

void PlaybackCoordinator::stopEvents()
{
	std::lock_guard<std::mutex> lock(mutex_);
	ReplayChannel::instance().stop();
	playGen_++; // any finish callback still in flight is now stale
	queue_.clear();
	queuePos_ = 0; // no queue, no position into one
	if (active_ && toOutput_)
		restorePreviousScene();
	setMusicMuted(true);
	active_ = false;
}

bool PlaybackCoordinator::queueActive() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return active_;
}

PlaybackCoordinator::PlayState PlaybackCoordinator::playState() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	PlayState s;
	s.active = active_;
	s.queued = (int)queue_.size();
	s.queuedAngles.reserve(queue_.size());
	for (const QueueItem &q : queue_)
		s.queuedAngles.push_back(q.angle + 1); // 0-based → 1-based
	if (active_ && queuePos_ < queue_.size()) {
		s.eventId = queue_[queuePos_].eventId;
		s.angle1 = queue_[queuePos_].angle + 1; // 0-based → 1-based
		s.queuePos = (int)queuePos_ + 1;
	}
	return s;
}

void PlaybackCoordinator::startNext()
{
	// mutex_ held by caller.
	//
	// One call is the whole thing now: the packets for [in, out] on that
	// camera are pulled from the ring (or the recorded files), decoded and
	// paced into the Replay A input. play() returns false when the range
	// cannot be served EXACTLY — the engine refuses rather than clamping, so
	// an unplayable item is skipped inline instead of putting the wrong
	// footage on air.
	while (queuePos_ < queue_.size()) {
		const QueueItem &item = queue_[queuePos_];
		const uint64_t gen = ++playGen_;
		// Installed before play() so the worker picks up the callback that
		// belongs to THIS clip.
		ReplayChannel::instance().setOnFinished([gen]() {
			obs_queue_task(OBS_TASK_UI, finishedTask,
				       (void *)(uintptr_t)gen, false);
		});
		std::string err;
		if (ReplayChannel::instance().play(item.angle, item.tInNs,
						   item.tOutNs, item.speedPct,
						   err)) {
			obs_log(LOG_INFO,
				"coordinator: playing event %d angle %d (%d%%) "
				"[%zu/%zu]",
				item.eventId, item.angle + 1, item.speedPct,
				queuePos_ + 1, queue_.size());
			return;
		}
		obs_log(LOG_WARNING,
			"coordinator: event %d angle %d not playable: %s",
			item.eventId, item.angle + 1, err.c_str());
		// Kept for the caller: "camera 3 has nothing there" is actionable,
		// "nothing played" is not.
		lastStartError_ = "camera " + std::to_string(item.angle + 1) +
				  ": " + err;
		queuePos_++; // unplayable: advance to the next item
	}

	// Nothing left to play. Stop the engine as well: at the natural end of a
	// queue the worker has already exited and this is a no-op join, but when a
	// NEW queue could not start a single item the clip of the OLD one was still
	// running - the operator saw the previous angle carry on over a play he had
	// just replaced, with the dock reporting idle.
	ReplayChannel::instance().stop();
	active_ = false;
	if (toOutput_)
		restorePreviousScene();
	setMusicMuted(true);
}

void PlaybackCoordinator::onEventFinished()
{
	// mutex_ held by caller.
	queuePos_++;
	if (queuePos_ < queue_.size()) {
		// hard cut between events (overlap transitions: future work)
		startNext();
	} else if (loop_ && !queue_.empty()) {
		// the reference controller Loop: restart the selection
		queuePos_ = 0;
		startNext();
	} else {
		active_ = false;
		if (toOutput_)
			restorePreviousScene();
		setMusicMuted(true);
	}
}

void PlaybackCoordinator::setMusicMuted(bool muted)
{
	std::string name =
		ReplayCore::instance().getConfig().musicSourceName;
	if (name.empty())
		return;
	obs_source_t *src = obs_get_source_by_name(name.c_str());
	if (src) {
		obs_source_set_muted(src, muted);
		obs_source_release(src);
	}
}

void PlaybackCoordinator::switchToReplayScene()
{
	// mutex_ held by caller.
	//
	// Use wait=true so the UI thread executes switchSceneTask (which saves
	// the current scene name into previousSceneName_) BEFORE this function
	// returns.  Without this, for short events the event can finish and
	// restorePreviousScene() can be called while previousSceneName_ is still
	// empty (the async task hasn't run yet), leaving OBS stuck on the replay
	// scene with no way to switch back.
	//
	// switchSceneTask does not acquire mutex_ or any lock owned by the
	// calling thread, so waiting cannot deadlock on our own state; runOnUi
	// handles the "already on the GUI thread" case (see above).
	//
	// There is no plugin-managed scene any more: "MultiReplay - Replay A" is
	// an ordinary OBS input the operator puts where he wants it, so the only
	// thing to switch to is the scene he configured. With none configured the
	// clip still plays into the input — we just don't take program from him.
	std::string scene = ReplayCore::instance().getConfig().outputSceneName;
	if (scene.empty()) {
		obs_log(LOG_INFO,
			"coordinator: no output scene configured — playing "
			"into '%s' without switching program",
			ReplayChannel::sourceName());
		return;
	}
	auto *ctx = new SceneSwitchCtx{scene, &previousSceneName_};
	runOnUi(switchSceneTask, ctx, true);
}

void PlaybackCoordinator::restorePreviousScene()
{
	// mutex_ held by caller
	if (previousSceneName_.empty())
		return;
	auto *ctx = new SceneSwitchCtx{previousSceneName_, nullptr};
	runOnUi(switchSceneTask, ctx, false);
	previousSceneName_.clear();
}

} // namespace multireplay
