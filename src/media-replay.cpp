/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "media-replay.hpp"
#include "replay-core.hpp"

// obs-module.h must come before plugin-support.h (MSVC linkage).
#include <obs-module.h>
#include "plugin-support.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace multireplay {

namespace {
constexpr int64_t kFrameMs = 33; // ~30 fps step fallback
int clampSpeedPct(double speed)
{
	int pct = (int)std::lround(speed * 100.0);
	return std::clamp(pct, 5, 100);
}
} // namespace

MediaReplay &MediaReplay::instance()
{
	static MediaReplay m;
	return m;
}

MediaReplay::~MediaReplay()
{
	unload();
}

void MediaReplay::load()
{
	if (running_)
		return;
	running_ = true;
	monitor_ = std::thread([this]() { monitorLoop(); });
}

void MediaReplay::unload()
{
	running_ = false;
	wake_.notify_all();
	if (monitor_.joinable())
		monitor_.join();

	std::lock_guard<std::mutex> lock(mutex_);
	if (mediaSource_) {
		obs_source_dec_showing(mediaSource_);
		obs_source_release(mediaSource_);
		mediaSource_ = nullptr;
	}
	index_.reset();
}

// ---------------------------------------------------------------------------
// Managed Media Source lifetime
// ---------------------------------------------------------------------------

void MediaReplay::ensureSource()
{
	// Read config BEFORE locking mutex_: startRecording() holds ReplayCore's
	// lock while calling clearSession() (which locks mutex_), so acquiring
	// ReplayCore's lock under mutex_ here would be the opposite order → AB-BA
	// deadlock.
	std::string cfgName = ReplayCore::instance().getConfig().replaySourceName;

	std::lock_guard<std::mutex> lock(mutex_);

	// Drop any prior ref (e.g. on scene-collection change the old instance
	// is being torn down) and (re)adopt the named source for the current
	// collection, creating it if the operator hasn't added one.
	if (mediaSource_) {
		obs_source_dec_showing(mediaSource_);
		obs_source_release(mediaSource_);
		mediaSource_ = nullptr;
	}
	loadedPath_.clear();
	pendingLoad_ = false;
	eventActive_ = false;
	eventOnDone_ = nullptr;

	// Which source to drive: the one the operator picked in Settings (a
	// Media Source they placed in their output scene), or our own managed
	// source when nothing is picked.
	const char *managedName = obs_module_text("ReplaySource.A");
	std::string targetName = cfgName.empty() ? managedName : cfgName;

	obs_source_t *existing = obs_get_source_by_name(targetName.c_str());
	if (existing) {
		// Adopt the operator's source, or our own restored from the
		// scene collection.
		mediaSource_ = existing; // get_source_by_name already add-ref'd
	} else if (cfgName.empty()) {
		// No explicit pick: create our managed Media Source.
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_bool(s, "looping", false);
		obs_data_set_bool(s, "restart_on_activate", false);
		obs_data_set_bool(s, "close_when_inactive", false);
		obs_data_set_bool(s, "hw_decode", true);
		obs_data_set_int(s, "speed_percent", 100);
		mediaSource_ = obs_source_create("ffmpeg_source", managedName, s,
						 nullptr);
		obs_data_release(s);
		if (!mediaSource_)
			obs_log(LOG_ERROR,
				"MediaReplay: failed to create ffmpeg_source");
	} else {
		// Picked source not present yet (scene still loading) — retry on
		// the next ensureSource()/settings change.
		obs_log(LOG_INFO,
			"MediaReplay: replay source '%s' not found yet",
			cfgName.c_str());
	}

	// Apply our control flags without disturbing any loaded file.
	if (mediaSource_) {
		obs_data_t *s = obs_source_get_settings(mediaSource_);
		obs_data_set_bool(s, "looping", false);
		obs_data_set_bool(s, "restart_on_activate", false);
		obs_data_set_bool(s, "close_when_inactive", false);
		obs_source_update(mediaSource_, s);
		obs_data_release(s);

		// CRITICAL: keep the source "showing" for the plugin's lifetime.
		// A Media Source only opens its file and runs its decode thread
		// while active; without this it never reports a duration, the
		// pending seek never applies and nothing plays / no preview.
		obs_source_inc_showing(mediaSource_);
	}
}

obs_source_t *MediaReplay::acquireSource()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return mediaSource_ ? obs_source_get_ref(mediaSource_) : nullptr;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

bool MediaReplay::loadSession(const std::string &folder, std::string &errorOut)
{
	auto index = std::make_shared<SessionIndex>();
	if (!index->load(folder)) {
		errorOut = "no indexable session in folder (record something "
			   "first, then stop or wait for a split)";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		index_ = index;
		angle_ = 0;
		loadedPath_.clear();
	}
	jumpToEnd();
	return true;
}

void MediaReplay::refreshSession()
{
	std::shared_ptr<SessionIndex> index;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		index = index_;
	}
	if (index)
		index->refresh();
}

void MediaReplay::clearSession()
{
	std::lock_guard<std::mutex> lock(mutex_);
	index_.reset();
	loadedPath_.clear();
	pendingLoad_ = false;
	eventActive_ = false;
	eventOnDone_ = nullptr;
	followLive_ = false;
	// Pause the media source so old footage stops showing while a new
	// recording is in progress (startRecording calls clearSession).
	if (mediaSource_)
		obs_source_media_play_pause(mediaSource_, true);
}

bool MediaReplay::sessionLoaded() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return (bool)index_;
}

int64_t MediaReplay::footageDurationNs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return index_ ? index_->masterDurationNs() : 0;
}

bool MediaReplay::resolveTime(int camIndex, int64_t masterNs,
			      std::string &pathOut, int64_t &offsetNsOut) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return index_ ? index_->resolve(camIndex, masterNs, pathOut,
					offsetNsOut)
		      : false;
}

// ---------------------------------------------------------------------------
// Internal helpers (caller holds mutex_)
// ---------------------------------------------------------------------------

void MediaReplay::loadFileLocked(const std::string &path, int speedPct,
				 int64_t seekMs, bool play)
{
	if (!mediaSource_)
		return;
	speedPct_ = speedPct;
	obs_data_t *s = obs_source_get_settings(mediaSource_);
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_string(s, "local_file", path.c_str());
	obs_data_set_int(s, "speed_percent", speedPct);
	obs_data_set_bool(s, "looping", false);
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_source_update(mediaSource_, s);
	obs_data_release(s);

	// Force the source back to PLAYING (from frame 0) so it leaves any ENDED
	// or STOPPED state. Without this, if the previous event played to the end
	// the source is in ENDED (durMs = 0, state ≠ PLAYING|PAUSED) and the
	// pending-load handler in monitorLoop never fires.
	obs_source_media_restart(mediaSource_);

	loadedPath_ = path;
	pendingLoad_ = true;
	pendingSeekMs_ = seekMs;
	pendingPlay_ = play;
	pendingGen_++;
	wake_.notify_all();
}

int64_t MediaReplay::mediaTimeNs() const
{
	if (!mediaSource_)
		return 0;
	return obs_source_media_get_time(mediaSource_) * 1000000LL;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void MediaReplay::setPlaying(bool playing)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!mediaSource_)
		return;
	if (pendingLoad_) {
		pendingPlay_ = playing;
		return;
	}
	obs_source_media_play_pause(mediaSource_, !playing);
}

void MediaReplay::togglePlay()
{
	setPlaying(!playing());
}

bool MediaReplay::playing() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!mediaSource_)
		return false;
	if (pendingLoad_)
		return pendingPlay_;
	return obs_source_media_get_state(mediaSource_) ==
	       OBS_MEDIA_STATE_PLAYING;
}

void MediaReplay::setSpeed(double speed)
{
	int pct = clampSpeedPct(speed);
	std::lock_guard<std::mutex> lock(mutex_);
	if (pct == speedPct_.load())
		return;
	speedPct_ = pct;
	if (loadedPath_.empty() || !mediaSource_)
		return;
	// ffmpeg_source applies speed on (re)load; reload in place, restoring
	// the current position and play state.
	int64_t curMs = pendingLoad_ ? pendingSeekMs_
				     : obs_source_media_get_time(mediaSource_);
	bool pl = pendingLoad_ ? pendingPlay_
			       : (obs_source_media_get_state(mediaSource_) ==
				  OBS_MEDIA_STATE_PLAYING);
	loadFileLocked(loadedPath_, pct, curMs, pl);
}

void MediaReplay::setAngle(int camIndex0)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!index_ || camIndex0 < 0 || camIndex0 >= kIndexMaxCameras)
		return;
	angle_ = camIndex0;
	int64_t curMaster = loadedPath_.empty() ? 0 : segBaseNs_ + mediaTimeNs();
	std::string path;
	int64_t off = 0;
	if (!index_->resolve(camIndex0, curMaster, path, off))
		return;
	bool pl = !loadedPath_.empty() && !pendingLoad_ &&
		  obs_source_media_get_state(mediaSource_) ==
			  OBS_MEDIA_STATE_PLAYING;
	segBaseNs_ = curMaster - off;
	loadFileLocked(path, speedPct_.load(), off / 1000000LL, pl);
}

void MediaReplay::seekMaster(int64_t masterNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!index_)
		return;
	masterNs = std::max<int64_t>(0, masterNs);
	std::string path;
	int64_t off = 0;
	if (!index_->resolve(angle_.load(), masterNs, path, off))
		return;
	if (path != loadedPath_ || pendingLoad_) {
		bool pl = !loadedPath_.empty() && !pendingLoad_ &&
			  obs_source_media_get_state(mediaSource_) ==
				  OBS_MEDIA_STATE_PLAYING;
		segBaseNs_ = masterNs - off;
		loadFileLocked(path, speedPct_.load(), off / 1000000LL, pl);
	} else if (mediaSource_) {
		obs_source_media_set_time(mediaSource_, off / 1000000LL);
		segBaseNs_ = masterNs - off;
	}
}

int64_t MediaReplay::position() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (loadedPath_.empty())
		return 0;
	if (pendingLoad_)
		return segBaseNs_ + pendingSeekMs_ * 1000000LL;
	return segBaseNs_ + mediaTimeNs();
}

void MediaReplay::jumpToEnd()
{
	int64_t edge = footageDurationNs();
	if (edge > 0)
		seekMaster(edge - 1);
	followLive_ = true;
}

void MediaReplay::stepFrames(int frames)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!mediaSource_ || loadedPath_.empty() || pendingLoad_)
		return;
	obs_source_media_play_pause(mediaSource_, true); // pause
	int64_t durMs = obs_source_media_get_duration(mediaSource_);
	int64_t curMs = obs_source_media_get_time(mediaSource_);
	int64_t newMs = curMs + (int64_t)frames * kFrameMs;
	newMs = std::clamp<int64_t>(newMs, 0, durMs > 0 ? durMs - 1 : 0);
	obs_source_media_set_time(mediaSource_, newMs);
}

// ---------------------------------------------------------------------------
// Event playback
// ---------------------------------------------------------------------------

bool MediaReplay::playEvent(int64_t tInNs, int64_t tOutNs, int angle0,
			    double speed, std::function<void()> onDone)
{
	std::lock_guard<std::mutex> lock(mutex_);
	angle_ = std::clamp(angle0, 0, kIndexMaxCameras - 1);
	followLive_ = false;

	std::string path;
	int64_t off = 0;
	if (!index_ || tOutNs <= tInNs ||
	    !index_->resolve(angle_.load(), tInNs, path, off))
		return false; // caller advances its queue; onDone not invoked

	eventActive_ = true;
	eventOutNs_ = tOutNs;
	eventOnDone_ = std::move(onDone);
	segBaseNs_ = tInNs - off;

	int spPct = clampSpeedPct(speed);

	// Fast path: same file already open and not mid-load. Skip the settings
	// update (which causes ffmpeg_source to restart from frame 0) and seek
	// directly. This is the critical path for consecutive plays of the same
	// event — avoids the flash-of-frame-0 and the 20ms pending window.
	auto st = mediaSource_
			  ? obs_source_media_get_state(mediaSource_)
			  : OBS_MEDIA_STATE_NONE;
	bool sameFile = (path == loadedPath_ && !pendingLoad_ &&
			 mediaSource_ != nullptr);
	bool activeState = (st == OBS_MEDIA_STATE_PLAYING ||
			    st == OBS_MEDIA_STATE_PAUSED);

	if (sameFile && activeState && spPct == speedPct_.load()) {
		// File is open + same speed: seek directly, no reload.
		speedPct_ = spPct;
		obs_source_media_set_time(mediaSource_, off / 1000000LL);
		obs_source_media_play_pause(mediaSource_, false); // play
	} else {
		// New file, speed change, or source in bad state: go through the
		// full load path (update + restart + pending seek).
		loadFileLocked(path, spPct, off / 1000000LL, true);
	}
	return true;
}

void MediaReplay::stopEvent()
{
	std::lock_guard<std::mutex> lock(mutex_);
	eventActive_ = false;
	eventOnDone_ = nullptr;
	eventOutNs_ = -1;
	if (mediaSource_ && !pendingLoad_)
		obs_source_media_play_pause(mediaSource_, true);
}

// ---------------------------------------------------------------------------
// Monitor thread: applies pending seeks once media is ready, enforces event
// out-points, and chains across segment splits.
// ---------------------------------------------------------------------------

void MediaReplay::monitorLoop()
{
	using namespace std::chrono_literals;
	while (running_) {
		std::function<void()> fireDone;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait_for(lock, 20ms);
			if (!running_)
				break;
			obs_source_t *src = mediaSource_;
			if (!src)
				continue;

			enum obs_media_state st =
				obs_source_media_get_state(src);
			int64_t durMs = obs_source_media_get_duration(src);

			if (pendingLoad_) {
				// Wait until the media is open (PLAYING or PAUSED
				// after the restart we issued in loadFileLocked).
				// Do NOT rely on durMs > 0 alone — the previous
				// file's stale duration can trigger this too early.
				bool ready = (st == OBS_MEDIA_STATE_PLAYING ||
					      st == OBS_MEDIA_STATE_PAUSED);
				if (ready) {
					// Pause first so we don't show frames
					// between frame-0 (auto-play after restart)
					// and the actual in-point seek.
					obs_source_media_play_pause(src, true);
					obs_source_media_set_time(src,
								  pendingSeekMs_);
					if (pendingPlay_)
						obs_source_media_play_pause(
							src, false);
					pendingLoad_ = false;
				}
				continue;
			}

			if (!eventActive_)
				continue;

			int64_t curMaster =
				segBaseNs_ +
				obs_source_media_get_time(src) * 1000000LL;
			bool ended = (st == OBS_MEDIA_STATE_ENDED ||
				      st == OBS_MEDIA_STATE_STOPPED);

			if (curMaster >= eventOutNs_) {
				obs_source_media_play_pause(src, true);
				fireDone = std::move(eventOnDone_);
				eventActive_ = false;
				eventOnDone_ = nullptr;
				eventOutNs_ = -1;
			} else if (ended) {
				// Segment split mid-event: chain to the next
				// file at the boundary master time.
				int64_t nextMaster =
					segBaseNs_ + durMs * 1000000LL;
				std::string path;
				int64_t off = 0;
				if (nextMaster < eventOutNs_ && index_ &&
				    index_->resolve(angle_.load(), nextMaster,
						    path, off) &&
				    path != loadedPath_) {
					segBaseNs_ = nextMaster - off;
					loadFileLocked(path, speedPct_.load(),
						       off / 1000000LL, true);
				} else {
					fireDone = std::move(eventOnDone_);
					eventActive_ = false;
					eventOnDone_ = nullptr;
					eventOutNs_ = -1;
				}
			}
		}
		if (fireDone)
			fireDone(); // outside the lock: may call back into us
	}
}

// ---------------------------------------------------------------------------
// Transport JSON (consumed by the dock)
// ---------------------------------------------------------------------------

std::string MediaReplay::transportJson() const
{
	// Call out to other singletons BEFORE locking mutex_ (each of these
	// locks internally): avoids an AB-BA deadlock with startRecording(),
	// which holds ReplayCore's lock while calling into MediaReplay.
	int64_t footage = footageDurationNs();
	int64_t pos = position();
	bool play = playing();
	bool rec = ReplayCore::instance().isRecording();

	std::lock_guard<std::mutex> lock(mutex_);
	obs_data_t *root = obs_data_create();
	obs_data_set_bool(root, "sessionLoaded", (bool)index_);
	obs_data_set_bool(root, "followLive", followLive_);
	obs_data_set_bool(root, "recording", rec);
	obs_data_set_int(root, "seekableNs", footage);
	obs_data_set_int(root, "durationNs", footage);

	obs_data_t *ch = obs_data_create();
	obs_data_set_bool(ch, "playing", play);
	obs_data_set_double(ch, "speed", speedPct_.load() / 100.0);
	obs_data_set_int(ch, "positionNs", pos);
	obs_data_set_int(ch, "angle", angle_.load() + 1);
	obs_data_set_obj(root, "A", ch);
	obs_data_release(ch);

	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

} // namespace multireplay
