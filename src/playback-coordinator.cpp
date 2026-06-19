/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "playback-coordinator.hpp"
#include "replay-core.hpp"
#include "media-replay.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <algorithm>

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

} // namespace

PlaybackCoordinator &PlaybackCoordinator::instance()
{
	static PlaybackCoordinator coordinator;
	return coordinator;
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
	double def = MediaReplay::instance().speed();
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
		double sp = ev.angles[useAng].speed >= 0 ? ev.angles[useAng].speed
							 : def;
		items.push_back({ev.id, ev.tInNs, ev.tOutNs, useAng, sp});
	}
	if (items.empty()) {
		errorOut = "no playable (completed) events selected";
		return false;
	}

	if (toOutput && MediaReplay::instance().replaySceneName().empty() &&
	    ReplayCore::instance().getConfig().outputSceneName.empty()) {
		errorOut = "replay output scene not ready";
		return false;
	}

	queue_ = std::move(items);
	queuePos_ = 0;
	toOutput_ = toOutput;
	active_ = true;
	MediaReplay::instance().setFollowLive(false);

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
	MediaReplay::instance().stopEvent();
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
	// One call drives the Media Source: seek to the in-point on the chosen
	// angle, play at the event speed, auto-stop at the out-point. OBS owns
	// the A/V sync, so audio just works. playEvent() returns false for an
	// unplayable item (e.g. footage not indexed) — skip it inline rather
	// than relying on a re-entrant onDone callback (would self-deadlock).
	while (queuePos_ < queue_.size()) {
		const QueueItem &item = queue_[queuePos_];
		if (MediaReplay::instance().playEvent(
			    item.tInNs, item.tOutNs, item.angle, item.speed,
			    [this]() { onEventFinished(); })) {
			obs_log(LOG_INFO,
				"coordinator: playing event %d angle %d (%d%%) "
				"[%zu/%zu]",
				item.eventId, item.angle + 1,
				(int)(item.speed * 100), queuePos_ + 1,
				queue_.size());
			// Pre-roll the next clip on the inactive source so the
			// engine can crossfade to it centered on the OUT (no-op
			// when the configured fade is 0 → plain hard cut).
			maybePrefetchLocked();
			return;
		}
		queuePos_++; // unplayable: advance to the next item
	}

	// Nothing left to play.
	active_ = false;
	if (toOutput_)
		restorePreviousScene();
	setMusicMuted(true);
}

void PlaybackCoordinator::maybePrefetchLocked()
{
	// mutex_ held by caller. Prefetch the immediate next item (no wrap: the
	// loop-restart at the end uses the plain hard-cut path).
	size_t nextPos = queuePos_ + 1;
	if (nextPos >= queue_.size())
		return;
	const QueueItem &n = queue_[nextPos];
	MediaReplay::instance().prefetchNext(
		n.tInNs, n.tOutNs, n.angle, n.speed,
		[this]() { onEventFinished(); },
		[this]() { onClipPromoted(); });
}

void PlaybackCoordinator::onClipPromoted()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_)
		return;
	// The engine has crossfaded to the prefetched next clip and is playing it;
	// advance our position to match, then pre-roll the one after it.
	if (queuePos_ + 1 < queue_.size())
		queuePos_++;
	obs_log(LOG_INFO, "coordinator: crossfaded to event %d [%zu/%zu]",
		queuePos_ < queue_.size() ? queue_[queuePos_].eventId : 0,
		queuePos_ + 1, queue_.size());
	maybePrefetchLocked();
}

void PlaybackCoordinator::onEventFinished()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_)
		return;
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
	// Switch Program to the plugin-managed replay scene (holds the A/B
	// transition) so the seamless replay reaches output. Fall back to the
	// operator's configured output scene if the managed one isn't ready.
	std::string scene = MediaReplay::instance().replaySceneName();
	if (scene.empty())
		scene = ReplayCore::instance().getConfig().outputSceneName;
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
