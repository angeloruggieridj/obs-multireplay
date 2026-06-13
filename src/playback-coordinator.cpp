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
				     bool toOutput, std::string &errorOut)
{
	std::lock_guard<std::mutex> lock(mutex_);

	// Expand into one item per enabled angle (the reference controller plays every checked
	// angle in sequence) and resolve the "--" speed inheritance chain.
	std::vector<QueueItem> items;
	double inherited = -1.0;
	double fallback = MediaReplay::instance().speed();
	for (int id : eventIds) {
		ReplayEvent ev;
		if (!EventStore::instance().get(id, ev) || ev.tOutNs < 0)
			continue;
		double speed = ev.speed >= 0
				       ? ev.speed
				       : (inherited >= 0 ? inherited
							 : fallback);
		if (ev.speed >= 0)
			inherited = ev.speed;
		bool anyAngle = false;
		for (int a = 0; a < kEventAngles; a++) {
			if (ev.angles[a].enabled) {
				items.push_back({ev.id, ev.tInNs, ev.tOutNs,
						 a, speed});
				anyAngle = true;
			}
		}
		if (!anyAngle)
			items.push_back({ev.id, ev.tInNs, ev.tOutNs, 0,
					 speed});
	}
	if (items.empty()) {
		errorOut = "no playable (completed) events selected";
		return false;
	}

	std::string sceneName =
		ReplayCore::instance().getConfig().outputSceneName;
	if (toOutput && sceneName.empty()) {
		errorOut = "no output scene configured in settings";
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

bool PlaybackCoordinator::playLastEvent(bool toOutput, std::string &errorOut)
{
	int id = EventStore::instance().lastEventId();
	if (id == 0) {
		errorOut = "no completed events yet";
		return false;
	}
	return playEvents({id}, toOutput, errorOut);
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
	auto *ctx = new SceneSwitchCtx{
		ReplayCore::instance().getConfig().outputSceneName,
		&previousSceneName_};
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
