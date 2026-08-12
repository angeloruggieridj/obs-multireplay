/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "playback-coordinator.hpp"
#include "replay-core.hpp"
#include "replay-channel.hpp"
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

void switchSceneTask(void *param)
{
	auto *ctx = static_cast<SceneSwitchCtx *>(param);

	if (ctx->saveCurrentInto) {
		obs_source_t *current = obs_frontend_get_current_scene();
		if (current) {
			const char *name = obs_source_get_name(current);
			*ctx->saveCurrentInto = name ? name : "";
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
				     std::string &errorOut)
{
	std::lock_guard<std::mutex> lock(mutex_);

	// One item per selected event, all on the given angle (the dock's current
	// angle). Speed = that angle's per-angle override if set, else the default
	// (slider) speed. There is no event-level speed.
	int ang = std::clamp(angle0, 0, kEventAngles - 1);
	const int defPct = defaultSpeedPct_.load();
	std::vector<QueueItem> items;
	for (int id : eventIds) {
		ReplayEvent ev;
		if (!EventStore::instance().get(id, ev) || ev.tOutNs < 0)
			continue;
		// Never play a DISABLED angle: if the requested angle isn't enabled
		// for this event, use its first enabled angle. (If none are enabled,
		// fall back to the requested one.)
		int useAng = ang;
		if (!ev.angles[ang].enabled) {
			for (int a = 0; a < kEventAngles; a++) {
				if (ev.angles[a].enabled) {
					useAng = a;
					break;
				}
			}
		}
		int pct = ev.angles[useAng].speed >= 0
				  ? (int)std::lround(ev.angles[useAng].speed * 100.0)
				  : defPct;
		items.push_back({ev.id, ev.tInNs, ev.tOutNs, useAng,
				 std::clamp(pct, 5, 400)});
	}
	if (items.empty()) {
		errorOut = "no playable (completed) events selected";
		return false;
	}

	queue_ = std::move(items);
	queuePos_ = 0;
	toOutput_ = toOutput;
	active_ = true;
	// Reviewing, not watching the live edge: the preview must show the replay
	// source from here on.
	ReplayCore::instance().setFollowLive(false);

	if (toOutput_)
		switchToReplayScene();
	if (musicEnabled_)
		setMusicMuted(false);
	startNext();
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
	if (active_ && queuePos_ < queue_.size()) {
		s.eventId = queue_[queuePos_].eventId;
		s.angle1 = queue_[queuePos_].angle + 1; // 0-based → 1-based
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
		queuePos_++; // unplayable: advance to the next item
	}

	// Nothing left to play.
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
	// calling thread, so wait=true cannot deadlock.
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
	obs_queue_task(OBS_TASK_UI, switchSceneTask, ctx, true);
}

void PlaybackCoordinator::restorePreviousScene()
{
	// mutex_ held by caller
	if (previousSceneName_.empty())
		return;
	auto *ctx = new SceneSwitchCtx{previousSceneName_, nullptr};
	obs_queue_task(OBS_TASK_UI, switchSceneTask, ctx, false);
	previousSceneName_.clear();
}

} // namespace multireplay
