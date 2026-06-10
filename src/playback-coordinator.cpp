/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "playback-coordinator.hpp"
#include "replay-core.hpp"
#include "replay-player.hpp"
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

	std::vector<ReplayEvent> resolved;
	for (int id : eventIds) {
		ReplayEvent ev;
		if (EventStore::instance().get(id, ev) && ev.tOutNs >= 0)
			resolved.push_back(ev);
	}
	if (resolved.empty()) {
		errorOut = "no playable (completed) events selected";
		return false;
	}

	std::string sceneName =
		ReplayCore::instance().getConfig().outputSceneName;
	if (toOutput && sceneName.empty()) {
		errorOut = "no output scene configured in settings";
		return false;
	}

	queue_ = std::move(resolved);
	queuePos_ = 0;
	toOutput_ = toOutput;
	inheritedSpeed_ = -1.0;
	active_ = true;

	if (toOutput_)
		switchToReplayScene();
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
	ReplayPlayer &a = ReplayEngine::instance().channelA();
	a.setStopAt(-1);
	a.setPlaying(false);
	queue_.clear();
	if (active_ && toOutput_)
		restorePreviousScene();
	active_ = false;
}

bool PlaybackCoordinator::queueActive() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return active_;
}

void PlaybackCoordinator::startNext()
{
	// mutex_ held by caller
	const ReplayEvent &ev = queue_[queuePos_];
	ReplayPlayer &a = ReplayEngine::instance().channelA();

	// first enabled angle of the event (the reference controller default-angle behaviour)
	int angle = 0;
	for (int i = 0; i < kEventAngles; i++) {
		if (ev.angles[i].enabled) {
			angle = i;
			break;
		}
	}

	// the reference controller per-event speed semantics: explicit speed becomes the new
	// default for following "--" events; "--" inherits.
	double speed = ev.speed >= 0
			       ? ev.speed
			       : (inheritedSpeed_ >= 0 ? inheritedSpeed_
						       : a.speed());
	if (ev.speed >= 0)
		inheritedSpeed_ = ev.speed;

	a.setAngle(angle);
	a.setReverse(false);
	a.setSpeed(speed);
	a.seekMaster(ev.tInNs);
	a.setStopAt(ev.tOutNs, [this]() { onEventFinished(); });
	a.setPlaying(true);

	obs_log(LOG_INFO, "coordinator: playing event %d (angle %d, %d%%)",
		ev.id, angle + 1, (int)(speed * 100));
}

void PlaybackCoordinator::onEventFinished()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_)
		return;
	queuePos_++;
	if (queuePos_ < queue_.size()) {
		// hard cut between events in M3; transitions are M4
		startNext();
	} else {
		active_ = false;
		if (toOutput_)
			restorePreviousScene();
	}
}

void PlaybackCoordinator::switchToReplayScene()
{
	// mutex_ held by caller
	auto *ctx = new SceneSwitchCtx{
		ReplayCore::instance().getConfig().outputSceneName,
		&previousSceneName_};
	obs_queue_task(OBS_TASK_UI, switchSceneTask, ctx, false);
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
