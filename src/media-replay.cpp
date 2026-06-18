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
#include <cstdlib>

namespace multireplay {

namespace {
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
	mediaSource_ = nullptr; // alias only
	if (outSceneSource_) {
		obs_source_release(outSceneSource_);
		outSceneSource_ = nullptr;
	}
	if (transition_) {
		obs_source_dec_showing(transition_);
		obs_source_release(transition_);
		transition_ = nullptr;
	}
	if (srcA_) {
		obs_source_dec_showing(srcA_);
		obs_source_release(srcA_);
		srcA_ = nullptr;
	}
	if (srcB_) {
		obs_source_dec_showing(srcB_);
		obs_source_release(srcB_);
		srcB_ = nullptr;
	}
	index_.reset();
	loadedPath_.clear();
	pendingLoad_ = false;
	pendingSeekApplied_ = false;
	eventActive_ = false;
	eventPlayStarted_ = false;
	eventOnDone_ = nullptr;
}

// ---------------------------------------------------------------------------
// Managed Media Source lifetime
// ---------------------------------------------------------------------------

void MediaReplay::revealLocked(obs_source_t *dest, uint32_t fadeMs)
{
	if (!transition_ || !dest)
		return;
	obs_source_t *old = obs_transition_get_active_source(transition_);
	obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, fadeMs, dest);
	if (old) {
		// Pause the source we just transitioned away from so it stops
		// consuming/advancing; it becomes the next pre-buffer target.
		if (old != dest)
			obs_source_media_play_pause(old, true);
		obs_source_release(old);
	}
}

obs_source_t *MediaReplay::createManagedMediaSource(const char *name)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_bool(s, "looping", false);
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	// hw_decode MUST be false: D3D11VA hardware decode accesses the D3D11
	// device context from ffmpeg's decode thread. On Intel UHD (and any GPU
	// where the encode pipeline also uses D3D11VA via Quick Sync) this races
	// with the OBS render thread → GPU TDR (black screen + mouse freeze).
	obs_data_set_bool(s, "hw_decode", false);
	obs_data_set_int(s, "speed_percent", 100);
	// Adopt an existing same-named source (restored from the scene collection)
	// rather than fail on a duplicate name; otherwise create it fresh.
	obs_source_t *src = obs_get_source_by_name(name);
	if (src) {
		obs_source_update(src, s);
	} else {
		src = obs_source_create("ffmpeg_source", name, s, nullptr);
	}
	obs_data_release(s);
	if (!src) {
		obs_log(LOG_ERROR, "MediaReplay: failed to create '%s'", name);
		return nullptr;
	}
	// Keep it showing for the plugin's lifetime so its decode thread runs even
	// while it is the inactive (pre-buffered) child of the transition.
	obs_source_inc_showing(src);
	return src;
}

void MediaReplay::ensureSource()
{
	std::lock_guard<std::mutex> lock(mutex_);

	// Aux-player: create the two managed media sources + the fade transition
	// once, then keep them for the plugin's lifetime. The transition is what
	// the dock preview (and, later, the output scene) renders.
	if (!srcA_)
		srcA_ = createManagedMediaSource(obs_module_text("ReplaySource.A"));
	if (!srcB_)
		srcB_ = createManagedMediaSource("MultiReplay — Replay B");
	struct obs_video_info ovi;
	uint32_t cx = 1920, cy = 1080;
	if (obs_get_video_info(&ovi)) {
		cx = ovi.base_width;
		cy = ovi.base_height;
	}
	if (!transition_) {
		transition_ = obs_source_create("fade_transition",
						"MultiReplay Replay Mix",
						nullptr, nullptr);
		if (transition_) {
			obs_transition_set_size(transition_, cx, cy);
			obs_transition_set_alignment(transition_,
						     OBS_ALIGN_CENTER);
			obs_transition_set_scale_type(transition_,
						      OBS_TRANSITION_SCALE_ASPECT);
			obs_source_inc_showing(transition_);
			if (srcA_)
				obs_transition_set(transition_, srcA_);
		} else {
			obs_log(LOG_ERROR,
				"MediaReplay: failed to create fade_transition");
		}
	}

	// Managed output scene: a plugin-owned scene whose single item is the
	// transition, fit to the canvas. "to output" switches Program to it, so the
	// seamless A/B replay reaches the broadcast transparently (the operator does
	// not place anything). Adopt an existing one (restored from the collection).
	if (transition_ && !outSceneSource_) {
		const char *sceneName = "MultiReplay — Replay";
		obs_source_t *existing = obs_get_source_by_name(sceneName);
		obs_scene_t *scene = nullptr;
		if (existing &&
		    obs_source_get_type(existing) == OBS_SOURCE_TYPE_SCENE) {
			outSceneSource_ = existing; // already add-ref'd
			scene = obs_scene_from_source(existing);
		} else {
			if (existing)
				obs_source_release(existing);
			scene = obs_scene_create(sceneName);
			if (scene)
				outSceneSource_ = obs_source_get_ref(
					obs_scene_get_source(scene));
		}
		if (scene &&
		    !obs_scene_find_source(scene, "MultiReplay Replay Mix")) {
			obs_sceneitem_t *item = obs_scene_add(scene, transition_);
			if (item) {
				struct vec2 pos = {0.0f, 0.0f};
				struct vec2 bounds = {(float)cx, (float)cy};
				obs_sceneitem_set_alignment(
					item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
				obs_sceneitem_set_bounds_type(
					item, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds(item, &bounds);
				obs_sceneitem_set_pos(item, &pos);
			}
		}
		// obs_scene_create returns an owned scene ref; outSceneSource_ now
		// holds a source ref that keeps it alive — release the create ref.
		if (scene && !existing)
			obs_scene_release(scene);
	}
	// Active source = A on first init; existing playback code operates on
	// mediaSource_ (a non-owning alias). Don't reset it on re-entry (e.g. a
	// scene-collection change mid-replay).
	if (!mediaSource_)
		mediaSource_ = srcA_;
}

obs_source_t *MediaReplay::acquireSource()
{
	// MUST NOT block: called on the OBS render/graphics thread (draw callback).
	// If monitorLoop holds mutex_, skip this frame rather than stall the GPU
	// thread — a 2-second stall triggers Intel UHD TDR (black screen + mouse
	// freeze). Returns the TRANSITION (it renders the active A/B child + fades).
	if (!mutex_.try_lock())
		return nullptr;
	obs_source_t *s = transition_ ? obs_source_get_ref(transition_)
				      : (mediaSource_ ? obs_source_get_ref(
							  mediaSource_)
						      : nullptr);
	mutex_.unlock();
	return s;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

bool MediaReplay::loadSession(const std::string &folder, std::string &errorOut)
{
	auto index = std::make_shared<SessionIndex>();
	bool hasData = index->load(folder);
	if (!hasData) {
		// No usable footage yet (new project, or files still being
		// finalized after stop). The poll() will retry every ~2 s;
		// "SessionIndex: no session manifest" is logged at DEBUG level
		// to avoid spamming the OBS log for expected empty-folder cases.
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
	pendingSeekApplied_ = false;
	pendingReopenPath_.clear();
	pendingReopenGap_ = 0;
	eventActive_ = false;
	eventOnDone_ = nullptr;
	followLive_ = false;
	// Blank BOTH A/B sources so no stale footage shows: pause, then clear the
	// input. Used at REC start (startRecording) and on New Project, so a fresh
	// project opens with an empty preview instead of the previous take's frame.
	obs_source_t *both[2] = {srcA_, srcB_};
	for (obs_source_t *src : both) {
		if (!src)
			continue;
		obs_source_media_play_pause(src, true);
		obs_data_t *s = obs_source_get_settings(src);
		obs_data_set_string(s, "local_file", "");
		obs_source_update(src, s);
		obs_data_release(s);
	}
}

bool MediaReplay::sessionLoaded() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return (bool)index_;
}

bool MediaReplay::previewHasContent() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return !loadedPath_.empty();
}

std::string MediaReplay::replaySceneName() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!outSceneSource_)
		return {};
	const char *n = obs_source_get_name(outSceneSource_);
	return n ? n : std::string();
}

int64_t MediaReplay::footageDurationNs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return index_ ? index_->masterDurationNs() : 0;
}

std::vector<SessionInfo> MediaReplay::sessionInfos() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return index_ ? index_->sessionInfos() : std::vector<SessionInfo>{};
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
	// A direct load supersedes any pending event hard-reopen (e.g. a scrub
	// issued mid-event); the monitor's reopen step is keyed off these.
	pendingReopenGap_ = 0;
	pendingReopenPath_.clear();
	speedPct_ = speedPct;
	appliedSpeedPct_ = speedPct; // speed now baked into the (re)opened source
	obs_data_t *s = obs_source_get_settings(mediaSource_);
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_string(s, "local_file", path.c_str());
	obs_data_set_int(s, "speed_percent", speedPct);
	obs_data_set_bool(s, "looping", false);
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_bool(s, "hw_decode", false); // see ensureSource() for rationale
	obs_source_update(mediaSource_, s);
	obs_data_release(s);

	// Restart is only needed when the source is in a stopped/ended state
	// (e.g. after an event played to completion with ENDED state).
	// For PLAYING/PAUSED sources obs_source_update() is sufficient —
	// calling restart on an actively-rendering source triggers an Intel
	// UHD D3D11 TDR crash (GPU timeout → black screen at OBS startup).
	enum obs_media_state st = obs_source_media_get_state(mediaSource_);
	if (st != OBS_MEDIA_STATE_PLAYING && st != OBS_MEDIA_STATE_PAUSED) {
		obs_source_media_restart(mediaSource_);
	}

	loadedPath_ = path;
	pendingLoad_ = true;
	pendingSeekMs_ = seekMs;
	pendingPlay_ = play;
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
	if (pendingLoad_ || pendingSeekApplied_) {
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
	if (pendingLoad_ || pendingSeekApplied_)
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
	speedPct_ = pct; // applied by the next (re)play

	// Do NOT touch the media source here. Event playback re-plays the clip from
	// its IN at the new speed (the dock calls playEvent again), and an in-place
	// obs_source_update() would restart ffmpeg_source from frame 0. The only
	// case we change rate live is a non-event source actively playing (manual
	// scrub/preview), where there is no clip in/out to preserve.
	if (!mediaSource_ || loadedPath_.empty() || eventActive_ ||
	    pendingLoad_ || pendingSeekApplied_)
		return;
	if (obs_source_media_get_state(mediaSource_) == OBS_MEDIA_STATE_PLAYING) {
		obs_data_t *s = obs_source_get_settings(mediaSource_);
		obs_data_set_int(s, "speed_percent", pct);
		obs_data_set_bool(s, "hw_decode", false);
		obs_source_update(mediaSource_, s);
		obs_data_release(s);
	}
}

void MediaReplay::setAngle(int camIndex0)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (camIndex0 < 0 || camIndex0 >= kIndexMaxCameras)
		return;
	angle_ = camIndex0;
	if (!index_)
		return;
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

// ---------------------------------------------------------------------------
// Event playback
// ---------------------------------------------------------------------------

bool MediaReplay::playEvent(int64_t tInNs, int64_t tOutNs, int angle0,
			    double speed, std::function<void()> onDone)
{
	std::lock_guard<std::mutex> lock(mutex_);
	angle_ = std::clamp(angle0, 0, kIndexMaxCameras - 1);
	followLive_ = false;

	if (!index_ || tOutNs <= tInNs)
		return false; // caller advances its queue; onDone not invoked

	eventActive_ = true;
	eventOutNs_ = tOutNs;
	eventDurationNs_ = tOutNs - tInNs;
	eventOnDone_ = std::move(onDone);

	int spPct = clampSpeedPct(speed);

	// Aux-player: prepare the clip on the INACTIVE (hidden) source. The reopen
	// + seek-confirm runs out of sight (the transition still shows the other
	// source), so the from-0 flash and the few-seconds reopen are invisible;
	// once the prepared source is seek-confirmed at the IN point the monitor
	// cuts the transition to it (no black, correct speed). Always reopen on the
	// inactive source — soft-seek would target the wrong source's cached file.
	if (srcA_ && srcB_)
		mediaSource_ = (mediaSource_ == srcA_) ? srcB_ : srcA_;

	if (!armEventReopenLocked(tInNs, spPct)) {
		// Unresolvable (footage not indexed): abort without invoking onDone.
		eventActive_ = false;
		eventOnDone_ = nullptr;
		eventOutNs_ = -1;
		return false;
	}
	obs_log(LOG_INFO, "[ev] playEvent IN=%lldms OUT=%lldms dur=%lldms angle=%d",
		(long long)(tInNs / 1000000), (long long)(tOutNs / 1000000),
		(long long)((tOutNs - tInNs) / 1000000), angle_.load());
	return true;
}

bool MediaReplay::armEventSoftSeekLocked(int64_t tInNs, int speedPct)
{
	if (!index_ || !mediaSource_ || loadedPath_.empty() || pendingLoad_ ||
	    pendingSeekApplied_)
		return false;
	std::string path;
	int64_t off = 0;
	if (!index_->resolve(angle_.load(), tInNs, path, off))
		return false;
	if (path != loadedPath_)
		return false; // different file → needs a real open

	enum obs_media_state st = obs_source_media_get_state(mediaSource_);
	if (st != OBS_MEDIA_STATE_PLAYING && st != OBS_MEDIA_STATE_PAUSED)
		return false;

	// A speed change can't be soft: updating speed_percent restarts
	// ffmpeg_source from 0, racing the seek (clip landed at 0 / wrong speed).
	// Only soft-seek when the speed is unchanged; otherwise return false so the
	// caller reopens (loadFileLocked bakes the speed in and seeks reliably).
	if (speedPct != appliedSpeedPct_)
		return false;

	// Only seek-in-place if the open file already covers the OUT offset. While
	// recording the cached duration is stale (small) → fall back to a reopen.
	int64_t outOffMs = (off + (eventOutNs_ - tInNs)) / 1000000LL;
	int64_t durMs = obs_source_media_get_duration(mediaSource_);
	if (durMs <= 0 || durMs < outOffMs)
		return false;

	speedPct_ = speedPct;
	eventPlayStarted_ = false;
	seekWaitCycles_ = 0;
	segBaseNs_ = tInNs - off;
	obs_source_media_play_pause(mediaSource_, true);
	obs_source_media_set_time(mediaSource_, off / 1000000LL);
	pendingSeekMs_ = off / 1000000LL;
	pendingPlay_ = true;
	pendingSeekApplied_ = true;
	pendingLoad_ = false;
	obs_log(LOG_INFO, "[ev] soft-seek IN file off=%lldms dur=%lldms",
		(long long)(off / 1000000), (long long)durMs);
	wake_.notify_all();
	return true;
}

bool MediaReplay::armEventReopenLocked(int64_t tInNs, int speedPct)
{
	std::string path;
	int64_t off = 0;
	if (!index_ || !index_->resolve(angle_.load(), tInNs, path, off))
		return false;

	speedPct_ = speedPct;
	eventPlayStarted_ = false;
	seekWaitCycles_ = 0;
	segBaseNs_ = tInNs - off;

	// Force a genuine fresh open so ffmpeg re-scans the file's CURRENT flushed
	// duration. Essential for instant replay WHILE recording: the file is still
	// growing and ffmpeg_source caches the duration seen at first open — a
	// same-path update/restart keeps that stale value, so a seek past it lands
	// at EOF (observed: durMs frozen at 3080 for a 10117 ms seek). Clear
	// local_file; the monitor waits a few cycles for the media to tear down,
	// then opens the target fresh and seeks. A cold open re-scans from scratch.
	if (mediaSource_) {
		obs_data_t *s = obs_source_get_settings(mediaSource_);
		obs_data_set_string(s, "local_file", "");
		obs_source_update(mediaSource_, s);
		obs_data_release(s);
	}
	loadedPath_.clear();
	pendingReopenPath_ = path;
	pendingReopenSeekMs_ = off / 1000000LL;
	pendingReopenGap_ = 4; // ~80ms for the cleared media to tear down
	pendingLoad_ = true;
	pendingPlay_ = true;
	pendingSeekApplied_ = false;
	wake_.notify_all();
	return true;
}

void MediaReplay::stopEvent()
{
	std::lock_guard<std::mutex> lock(mutex_);
	eventActive_ = false;
	eventPlayStarted_ = false;
	eventOnDone_ = nullptr;
	eventOutNs_ = -1;
	pendingSeekApplied_ = false;
	seekWaitCycles_ = 0;
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
				// Hard-reopen step (event playback): the media was
				// cleared in playEvent; give it a few cycles to tear
				// down, then open the target fresh so ffmpeg re-scans
				// the current (still-growing) file duration instead of
				// reusing the stale cached one.
				if (pendingReopenGap_ > 0) {
					pendingReopenGap_--;
					continue;
				}
				if (!pendingReopenPath_.empty()) {
					std::string p =
						std::move(pendingReopenPath_);
					pendingReopenPath_.clear();
					loadFileLocked(p, speedPct_.load(),
						       pendingReopenSeekMs_, true);
					continue; // next cycles run the seek-confirm
				}
				bool ready = (st == OBS_MEDIA_STATE_PLAYING ||
					      st == OBS_MEDIA_STATE_PAUSED);
				if (!pendingSeekApplied_) {
					// Phase 1: media is open — pause and seek.
					// Do NOT play yet; the seek is async (posted
					// to the decode thread) so playing immediately
					// would show frame 0 before the seek lands.
					if (ready) {
						obs_source_media_play_pause(
							src, true);
						obs_source_media_set_time(
							src, pendingSeekMs_);
						pendingSeekApplied_ = true;
					}
				} else {
					// Phase 2: do NOT arm the event until the async
					// seek has actually landed. set_time() can take
					// many cycles on Intel UHD (especially right after
					// a fresh file open); arming early ran the event
					// from the OLD position (from 0 after a load, or
					// the previous event's end). The source must be
					// PLAYING for the decode thread to process the seek
					// and advance get_time (it does NOT seek while
					// paused on this hardware). Poll until position
					// reaches the request (re-issuing), bail after
					// ~1.5s. A brief frame from the old position may
					// show on the first play after a fresh file open.
					if (eventActive_ && pendingPlay_) {
						if (st != OBS_MEDIA_STATE_PLAYING)
							obs_source_media_play_pause(
								src, false);
						int64_t nowMs =
							obs_source_media_get_time(
								src);
						if (std::llabs(nowMs -
								pendingSeekMs_) >=
							    1100LL &&
						    seekWaitCycles_ < 75) {
							if ((seekWaitCycles_ % 8) ==
							    0)
								obs_source_media_set_time(
									src,
									pendingSeekMs_);
							seekWaitCycles_++;
							continue;
						}
						seekWaitCycles_ = 0;
					}
					if (pendingPlay_)
						obs_source_media_play_pause(
							src, false);
					pendingLoad_ = false;
					pendingSeekApplied_ = false;
					if (eventActive_ && pendingPlay_ &&
					    !eventPlayStarted_) {
						// Recalibrate OUT: keyframe-aligned
						// seek may have landed before the
						// requested IN point. Read the actual
						// position now that the seek settled
						// so the wall-clock OUT fires at
						// exactly tOutNs.
					// Guard: obs_source_media_get_time() returns
					// STALE/garbage values right after a seek on
					// Intel UHD (observed 2400 / 22680 ms for a
					// 13948 ms seek). Only EXTEND the duration for a
					// genuine keyframe-before-IN landing: trust the
					// read solely when it is at or before the request
					// and within one GOP. Otherwise keep dur = OUT−IN
					// (a forward/garbage read previously shortened the
					// event to ~1 s — "stops after a few seconds").
					int64_t landedMs =
						obs_source_media_get_time(src);
					if (landedMs > 0 && landedMs <= pendingSeekMs_ &&
					    (pendingSeekMs_ - landedMs) < 3000LL) {
						int64_t masterActual =
							segBaseNs_ +
							landedMs * 1000000LL;
						if (eventOutNs_ > masterActual)
							eventDurationNs_ =
								eventOutNs_ -
								masterActual;
					}
					obs_log(LOG_INFO,
						"[ev] play START (load) landedMs=%lld reqMs=%lld dur=%lldms",
						(long long)landedMs,
						(long long)pendingSeekMs_,
						(long long)(eventDurationNs_ /
							     1000000));
					// Reveal the prepared (hidden) source now
					// that it is playing at IN → seamless cut
					// (or crossfade if fadeMs_ > 0).
					revealLocked(src, (uint32_t)fadeMs_.load());
					eventPlayStartWall_ =
						std::chrono::steady_clock::now();
					eventPlayStarted_ = true;
				}
			}
			continue;
		}

			// Fast-path seek (playEvent same-file branch): seek was
			// applied last cycle; start play now.
			if (pendingSeekApplied_) {
				// Wait for the async seek to land before arming
				// (same race as Phase 2). The source must be PLAYING
				// for the decode thread to process the seek and
				// advance get_time (it does NOT seek while paused on
				// this hardware). Poll until position reaches the
				// request (re-issuing the seek), bail after ~1.5s.
				if (eventActive_ && pendingPlay_) {
					if (st != OBS_MEDIA_STATE_PLAYING)
						obs_source_media_play_pause(src,
									    false);
					int64_t nowMs =
						obs_source_media_get_time(src);
					if (std::llabs(nowMs - pendingSeekMs_) >=
						    1100LL &&
					    seekWaitCycles_ < 75) {
						if ((seekWaitCycles_ % 8) == 0)
							obs_source_media_set_time(
								src,
								pendingSeekMs_);
						seekWaitCycles_++;
						continue;
					}
					seekWaitCycles_ = 0;
				}
				if (pendingPlay_)
					obs_source_media_play_pause(src, false);
				pendingSeekApplied_ = false;
				if (eventActive_ && !eventPlayStarted_) {
					// Guard: obs_source_media_get_time() returns
					// STALE/garbage right after a seek on Intel UHD.
					// Only EXTEND for a real keyframe-before-IN
					// landing (read at/before request, within a GOP);
					// otherwise keep dur = OUT−IN. A forward/garbage
					// read previously shortened the event to ~1 s.
					int64_t landedMs =
						obs_source_media_get_time(src);
					if (landedMs > 0 && landedMs <= pendingSeekMs_ &&
					    (pendingSeekMs_ - landedMs) < 3000LL) {
						int64_t masterActual =
							segBaseNs_ +
							landedMs * 1000000LL;
						if (eventOutNs_ > masterActual)
							eventDurationNs_ =
								eventOutNs_ -
								masterActual;
					}
					// Always arm the wall-clock timer even
					// when get_time() returned stale 0.
					obs_log(LOG_INFO,
						"[ev] play START (fast) landedMs=%lld reqMs=%lld segBase=%lldms dur=%lldms",
						(long long)landedMs,
						(long long)pendingSeekMs_,
						(long long)(segBaseNs_ / 1000000),
						(long long)(eventDurationNs_ /
							     1000000));
					eventPlayStartWall_ =
						std::chrono::steady_clock::now();
					eventPlayStarted_ = true;
				}
				continue;
			}

			if (!eventActive_)
				continue;

			bool ended = (st == OBS_MEDIA_STATE_ENDED ||
				      st == OBS_MEDIA_STATE_STOPPED);

			// Wall-clock OUT detection: obs_source_media_get_time()
			// returns stale values after a seek on Intel UHD, making
			// curMaster much lower than expected (OUT fires too late
			// or never). Track real elapsed time instead.
			if (eventPlayStarted_) {
				using ns = std::chrono::nanoseconds;
				auto elapsed = std::chrono::steady_clock::now() -
					       eventPlayStartWall_;
				int64_t realNs =
					std::chrono::duration_cast<ns>(elapsed)
						.count();
				// Scale by speed: at 50% speed the source plays
				// half as fast, so real time is 2× media time.
				int64_t mediaNs =
					realNs * (int64_t)speedPct_.load() / 100LL;
				if (mediaNs >= eventDurationNs_) {
					obs_log(LOG_INFO,
						"[ev] OUT reached: mediaNs=%lldms dur=%lldms",
						(long long)(mediaNs / 1000000),
						(long long)(eventDurationNs_ /
							     1000000));
					obs_source_media_play_pause(src, true);
					fireDone = std::move(eventOnDone_);
					eventActive_ = false;
					eventOnDone_ = nullptr;
					eventOutNs_ = -1;
					eventPlayStarted_ = false;
				} else if (ended) {
					// Segment split: chain to the next file.
					int64_t nextMaster =
						segBaseNs_ + durMs * 1000000LL;
					std::string path;
					int64_t off = 0;
					if (nextMaster < eventOutNs_ && index_ &&
					    index_->resolve(angle_.load(),
							    nextMaster, path,
							    off) &&
					    path != loadedPath_) {
						obs_log(LOG_INFO,
							"[ev] EOF chain: durMs=%lld nextMaster=%lldms off=%lldms",
							(long long)durMs,
							(long long)(nextMaster /
								    1000000),
							(long long)(off /
								    1000000));
						segBaseNs_ = nextMaster - off;
						loadFileLocked(path,
							       speedPct_.load(),
							       off / 1000000LL,
							       true);
					} else {
						obs_log(LOG_INFO,
							"[ev] EOF stop (no chain): durMs=%lld nextMaster=%lldms outNs=%lldms",
							(long long)durMs,
							(long long)(nextMaster /
								    1000000),
							(long long)(eventOutNs_ /
								    1000000));
						fireDone = std::move(eventOnDone_);
						eventActive_ = false;
						eventOnDone_ = nullptr;
						eventOutNs_ = -1;
						eventPlayStarted_ = false;
					}
				}
			} else if (ended) {
				obs_log(LOG_INFO,
					"[ev] ended before play start (durMs=%lld)",
					(long long)durMs);
				fireDone = std::move(eventOnDone_);
				eventActive_ = false;
				eventOnDone_ = nullptr;
				eventOutNs_ = -1;
			}
		}
		if (fireDone)
			fireDone(); // outside the lock: may call back into us
	}
}

// ---------------------------------------------------------------------------
// Transport state (consumed by the dock every poll tick)
// ---------------------------------------------------------------------------

MediaReplay::TransportState MediaReplay::transportState() const
{
	// Read other singletons BEFORE locking mutex_ (each locks internally):
	// avoids AB-BA deadlock with startRecording(), which holds ReplayCore's
	// lock while calling clearSession() (which then locks mutex_).
	int64_t footage = footageDurationNs();
	int64_t pos = position();
	bool play = playing();
	bool rec = ReplayCore::instance().isRecording();

	std::lock_guard<std::mutex> lock(mutex_);
	TransportState t;
	t.sessionLoaded = (bool)index_;
	t.followLive = followLive_;
	t.recording = rec;
	t.seekableNs = footage;
	t.durationNs = footage;
	t.playing = play;
	t.speed = speedPct_.load() / 100.0;
	t.positionNs = pos;
	t.angle = angle_.load() + 1; // 1-based for the UI
	t.eventActive = eventActive_;
	return t;
}

} // namespace multireplay
