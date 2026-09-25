/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See remote-control.hpp.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "remote-control.hpp"
#include "dock-internal.hpp" // g_dock
#include "multireplay-dock.hpp"
#include "plugin-support.h"

#include <atomic>
#include <cassert> // obs-websocket-api.h uses assert() without including it
#include <chrono>
#include <cstring> // ...and strlen()
#include <functional>
#include <future>
#include <memory>
#include <utility>

#include "obs-websocket-api.h" // vendored upstream header, no link dependency

namespace multireplay::remote_control {
namespace {

// A REQUEST ARRIVES ON A WORKER THREAD, NEVER ON THE UI THREAD.
// obs-websocket hands every message to its own QThreadPool
// (WebSocketServer::onMessage), and a vendor callback runs right there. Every
// dock call touches widgets, so each request is posted to the UI task queue and
// the worker waits for the answer.
//
// The wait is BOUNDED, and the answer is written into an obs_data_t the job
// owns, not into the caller's response. A worker that gave up after the timeout
// has returned, and obs-websocket has freed its response; the job still runs
// later on the UI thread, and it must have somewhere of its own to write.
constexpr auto kUiTimeout = std::chrono::seconds(2);

std::atomic<bool> g_accepting{false};
obs_websocket_vendor g_vendor = nullptr;

struct Job {
	std::function<void(obs_data_t *)> fn;
	obs_data_t *out = obs_data_create();
	std::promise<void> done;
	~Job() { obs_data_release(out); }
};

void runJob(void *param)
{
	auto *job = static_cast<std::shared_ptr<Job> *>(param);
	// The dock is created and destroyed on this thread, so reading it here
	// cannot race its destructor.
	if (g_dock)
		(*job)->fn((*job)->out);
	else
		obs_data_set_string((*job)->out, "error", "panel not ready");
	(*job)->done.set_value();
	delete job;
}

// Runs fn(out) on the UI thread and copies what it wrote into the response.
// Sets "success" false (and says why) when the panel is gone, OBS is closing or
// the UI thread did not answer in time.
void onUiThread(obs_data_t *response, std::function<void(obs_data_t *)> fn)
{
	if (!g_accepting.load()) {
		obs_data_set_bool(response, "success", false);
		obs_data_set_string(response, "error", "OBS is shutting down");
		return;
	}
	auto job = std::make_shared<Job>();
	job->fn = std::move(fn);
	auto finished = job->done.get_future();
	obs_queue_task(OBS_TASK_UI, runJob, new std::shared_ptr<Job>(job),
		       false);
	if (finished.wait_for(kUiTimeout) != std::future_status::ready) {
		obs_data_set_bool(response, "success", false);
		obs_data_set_string(response, "error",
				    "timed out waiting for the UI thread");
		return;
	}
	obs_data_apply(response, job->out);
	obs_data_set_bool(response, "success",
			  !obs_data_has_user_value(response, "error"));
}

// --- The requests ------------------------------------------------------------
// Data in and out is JSON; every response carries "success", and "error" when
// it is false.

// {"delta": int} — frame steps, positive forward (at most 10 per request).
void stepFrames(obs_data_t *req, obs_data_t *res, void *)
{
	const int delta = (int)obs_data_get_int(req, "delta");
	onUiThread(res, [delta](obs_data_t *) {
		g_dock->remoteStepFrames(delta);
	});
}

// {"percent": int} — the default replay speed, 5..200.
void setSpeed(obs_data_t *req, obs_data_t *res, void *)
{
	const int pct = (int)obs_data_get_int(req, "percent");
	onUiThread(res, [pct](obs_data_t *) { g_dock->remoteSetSpeed(pct); });
}

// {"seconds": double} — one jump along the footage, positive forward.
void scrubSeconds(obs_data_t *req, obs_data_t *res, void *)
{
	const double seconds = obs_data_get_double(req, "seconds");
	onUiThread(res, [seconds](obs_data_t *) {
		g_dock->remoteScrubSeconds(seconds);
	});
}

// {"delta": int} — previous/next event in the list, which cues it.
void stepEventSelection(obs_data_t *req, obs_data_t *res, void *)
{
	const int delta = (int)obs_data_get_int(req, "delta");
	onUiThread(res, [delta](obs_data_t *) {
		g_dock->remoteStepEvent(delta);
	});
}

// {"id": int} — select and cue the event with this id.
void selectEventById(obs_data_t *req, obs_data_t *res, void *)
{
	const int id = (int)obs_data_get_int(req, "id");
	onUiThread(res, [id](obs_data_t *out) {
		if (!g_dock->remoteSelectEvent(id))
			obs_data_set_string(out, "error",
					    "no event with this id in the list");
	});
}

// {} — A <-> B.
void toggleActiveChannel(obs_data_t *, obs_data_t *res, void *)
{
	onUiThread(res, [](obs_data_t *out) {
		if (!g_dock->remoteToggleChannel())
			obs_data_set_string(out, "error",
					    "channel B is switched off");
	});
}

// {"delta": int} — previous/next list; answers {"activeList": int}.
void stepListSelection(obs_data_t *req, obs_data_t *res, void *)
{
	const int delta = (int)obs_data_get_int(req, "delta");
	onUiThread(res, [delta](obs_data_t *out) {
		obs_data_set_int(out, "activeList",
				 g_dock->remoteStepList(delta));
	});
}

// {} — what a controller's display shows: {"cursorMs", "speedPercent",
// "eventId", "activeList", "recording", "channel"}.
void getPlaybackStatus(obs_data_t *, obs_data_t *res, void *)
{
	onUiThread(res, [](obs_data_t *out) {
		const auto s = g_dock->remoteStatus();
		obs_data_set_int(out, "cursorMs", s.cursorMs);
		obs_data_set_int(out, "speedPercent", s.speedPct);
		obs_data_set_int(out, "eventId", s.eventId);
		obs_data_set_int(out, "activeList", s.list);
		obs_data_set_bool(out, "recording", s.recording);
		obs_data_set_string(out, "channel", s.channel);
	});
}

struct Request {
	const char *name;
	obs_websocket_request_callback_function fn;
};

constexpr Request kRequests[] = {
	{"step_frames", stepFrames},
	{"set_speed", setSpeed},
	{"scrub_seconds", scrubSeconds},
	{"step_event_selection", stepEventSelection},
	{"select_event_by_id", selectEventById},
	{"toggle_active_channel", toggleActiveChannel},
	{"step_list_selection", stepListSelection},
	{"get_playback_status", getPlaybackStatus},
};

} // namespace

void registerRequests()
{
	g_vendor = obs_websocket_register_vendor("multireplay");
	if (!g_vendor) {
		obs_log(LOG_INFO,
			"obs-websocket not available — external control "
			"requests are off (hotkeys still work)");
		return;
	}
	int registered = 0;
	for (const Request &r : kRequests) {
		if (obs_websocket_vendor_register_request(g_vendor, r.name,
							  r.fn, nullptr))
			registered++;
		else
			obs_log(LOG_WARNING,
				"obs-websocket vendor request '%s' was not "
				"registered",
				r.name);
	}
	g_accepting.store(true);
	obs_log(LOG_INFO,
		"obs-websocket vendor \"multireplay\": %d requests registered",
		registered);
}

void stopAccepting()
{
	g_accepting.store(false);
}

// UNREGISTERED HERE, WHILE obs-websocket IS STILL LOADED. OBS unloads modules in
// the order it loaded them — alphabetical, so obs-multireplay before
// obs-websocket (measured: our "plugin unloaded" line comes before its
// "Shutting down...") — and its server is still running while we go. Left
// registered, a request in that window would reach a module that has already
// torn its panel down.
void shutdown()
{
	stopAccepting();
	if (!g_vendor)
		return;
	for (const Request &r : kRequests)
		obs_websocket_vendor_unregister_request(g_vendor, r.name);
	g_vendor = nullptr;
}

} // namespace multireplay::remote_control
