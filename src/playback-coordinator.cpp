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

// The delayed restore of the operator's own transition needs a timer that fires
// on the GUI thread, and this plugin already links Qt (ENABLE_QT is not
// optional — see CMakePresets). obs_queue_task has no "later" of its own.
#include <QCoreApplication>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <filesystem> // the music file is checked before it is played
#include <memory>     // the dip's shared step counter
#include <mutex>

namespace multireplay {

namespace {

// obs_frontend_set_current_scene must run on the UI thread.
//
// `owner` replaced a raw `std::string *saveCurrentInto` pointing straight at
// PlaybackCoordinator::previousSceneName_. The name is now read and written
// through the coordinator, under its own small mutex, ON THIS THREAD — which is
// what lets both the switch and the restore be non-blocking tasks and still
// happen in the right order (see switchToReplayScene, B5).
struct SceneSwitchCtx {
	// Empty when `restore` is set: the scene to go to is whatever the switch
	// task recorded, and only the UI thread knows it yet.
	std::string sceneName;
	PlaybackCoordinator *owner = nullptr;
	// THE EVENT TRANSITION (the reference controller): which OBS transition takes the replay to
	// air, and for how long. Empty = do not touch what the operator has set,
	// which is the default and is exactly how this plugin behaved before.
	std::string transitionName;
	int transitionMs = 0;
	// true = "put program back where it was", false = "take program".
	bool restore = false;
};

// Put OBS's transition back the way the operator had it, after ours has had
// time to run. It is a DELAYED restore and that is the whole difficulty: the
// transition is started by the scene switch and runs on for its duration, so
// restoring the selection immediately would take the operator's setting back
// while our own transition is still on screen. Nothing here touches the running
// transition — only which one OBS will reach for next.
struct TransitionRestoreCtx {
	std::string name;
	int durationMs = 0;
};

void restoreTransitionTask(void *param)
{
	auto *ctx = static_cast<TransitionRestoreCtx *>(param);
	if (!ctx->name.empty()) {
		struct obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		for (size_t i = 0; i < list.sources.num; i++) {
			const char *nm = obs_source_get_name(list.sources.array[i]);
			if (nm && ctx->name == nm) {
				obs_frontend_set_current_transition(
					list.sources.array[i]);
				break;
			}
		}
		obs_frontend_source_list_free(&list);
	}
	if (ctx->durationMs > 0)
		obs_frontend_set_transition_duration(ctx->durationMs);
	delete ctx;
}

// Select `name` as OBS's current transition and set its duration, remembering
// what was there so it can be handed back. Returns false when the transition
// does not exist (a renamed or deleted one), and then NOTHING has been changed —
// a replay must not go to air through a transition nobody chose.
bool useTransition(const std::string &name, int durationMs)
{
	if (name.empty())
		return false;
	struct obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	obs_source_t *found = nullptr;
	for (size_t i = 0; i < list.sources.num; i++) {
		const char *nm = obs_source_get_name(list.sources.array[i]);
		if (nm && name == nm) {
			found = list.sources.array[i];
			break;
		}
	}
	if (!found) {
		obs_frontend_source_list_free(&list);
		obs_log(LOG_WARNING,
			"coordinator: transition '%s' no longer exists — using "
			"whatever OBS has",
			name.c_str());
		return false;
	}

	// Remember first, change second, and hand the restore to a task that runs
	// after the transition has played out.
	auto *restore = new TransitionRestoreCtx();
	if (obs_source_t *cur = obs_frontend_get_current_transition()) {
		const char *nm = obs_source_get_name(cur);
		restore->name = nm ? nm : "";
		obs_source_release(cur);
	}
	restore->durationMs = obs_frontend_get_transition_duration();

	obs_frontend_set_current_transition(found);
	if (durationMs > 0)
		obs_frontend_set_transition_duration(durationMs);
	obs_frontend_source_list_free(&list);

	// The margin is not decoration: the switch has not been made yet when this
	// returns, so the restore has to outlast the transition AND the scene change
	// that starts it.
	QTimer::singleShot(durationMs + 400, qApp, [restore]() {
		restoreTransitionTask(restore);
	});
	return true;
}

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

	std::string target = ctx->sceneName;
	if (ctx->restore) {
		// Whatever the switch task recorded, and it has already run:
		// this is the same FIFO queue.
		target = ctx->owner ? ctx->owner->takePreviousScene()
				    : std::string();
		if (target.empty()) {
			delete ctx;
			return; // program was never taken
		}
	} else if (ctx->owner) {
		obs_source_t *current = obs_frontend_get_current_scene();
		if (current) {
			const char *name = obs_source_get_name(current);
			const std::string cur = name ? name : "";
			// Re-triggering a replay while the replay scene is
			// already on air must not record IT as "the scene to go
			// back to" — that would strand program on the replay
			// scene once the queue drains.
			if (cur != target)
				ctx->owner->rememberPreviousScene(cur);
			obs_source_release(current);
		}
	}

	// Chosen BEFORE the switch, because the switch is what starts it. Nothing
	// happens when the setting is empty or names a transition that is gone, and
	// then program moves exactly as OBS was already set to move it.
	useTransition(ctx->transitionName, ctx->transitionMs);

	obs_source_t *scene = obs_get_source_by_name(target.c_str());
	if (scene) {
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	} else {
		obs_log(LOG_WARNING, "coordinator: scene '%s' not found",
			target.c_str());
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

PlaybackCoordinator &PlaybackCoordinator::instance(Which which)
{
	// Two queues, one per channel. They share nothing: A finishing a clip
	// must not advance B, and B going to program must not stop A.
	static PlaybackCoordinator a(Which::A);
	static PlaybackCoordinator b(Which::B);
	return which == Which::B ? b : a;
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
				     std::string &errorOut, AngleMode mode,
				     ReplayChannel::Direction direction)
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
	// Where the footage ENDS on each angle, for "continue past the OUT": the
	// live edge while a take is running, the end of the last file otherwise.
	// Both are instants and neither may be tested against 0 — except the ring's
	// own edge, which only ever holds this session and whose 0 really does mean
	// empty (see kNoInstant in session-clock.hpp).
	int64_t footageEndNs[kEventAngles];
	for (int a = 0; a < kEventAngles; a++)
		footageEndNs[a] = kNoInstant;
	int64_t continuePastOutNs = 0;
	{
		const Config cfg = ReplayCore::instance().getConfig();
		continuePastOutNs = (int64_t)cfg.continuePastOutMs * 1'000'000;
		for (int a = 0; a < kEventAngles && a < kMaxCameras; a++) {
			const int64_t live = PacketTap::instance().newestNs(a);
			playableAngle[a] =
				!cfg.cameras[a].sourceName.empty() || live > 0 ||
				SegmentIndex::instance().oldestNs(a) !=
					kNoInstant;
			footageEndNs[a] =
				std::max(live > 0 ? live : kNoInstant,
					 SegmentIndex::instance().newestNs(a));
		}
	}
	const auto playable = [&playableAngle](int a) {
		return a >= 0 && a < kEventAngles && playableAngle[a];
	};

	// Snapshot the events too, for the same reason: EventStore has its own
	// lock and the dock mutates it from the GUI thread while this runs.
	std::vector<ReplayEvent> events;
	events.reserve(eventIds.size());
	for (int id : eventIds) {
		ReplayEvent ev;
		if (EventStore::instance().get(id, ev) &&
		    ev.tOutNs != kNoInstant)
			events.push_back(std::move(ev));
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
	// TWO tiers, and only two: the ANGLE's own override (the same event at
	// 100% on the wide and 25% on the tight, which is what a per-angle speed
	// is FOR), else the operator's slider. The event-level tier that used to
	// sit between them is gone with the column that set it — a speed that
	// belongs to neither a camera nor the operator's hand belongs to nothing.
	const auto push = [&](const ReplayEvent &ev, int a) {
		int pct = defPct;
		if (ev.angles[a].speed >= 0)
			pct = (int)std::lround(ev.angles[a].speed * 100.0);
		items.push_back({ev.id, ev.tInNs, ev.tOutNs, a,
				 std::clamp(pct, 5, 400), direction});
	};

	// Set when Single mode had to refuse the angle it was asked for, so the
	// operator gets told WHICH camera has nothing rather than a generic "no".
	int refusedAngle = -1;

	for (const ReplayEvent &ev : events) {
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
				push(ev, a);
			}
			if ((int)items.size() > before)
				continue;
			// No angle enabled at all: fall back to the requested one
			// rather than silently dropping the event.
			push(ev, ang);
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
		push(ev, ang);
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

	// CONTINUE PAST THE OUT (Config.continuePastOutMs, off by default). The LAST
	// clip only: the ones before it have to end where they end, or the sequence
	// never reaches the next angle. Forwards only — a reverse clip runs from its
	// OUT to its IN, so there is nothing past its OUT to carry on into, and the
	// footage-end of the angle is behind it, not ahead.
	//
	// The extension is what the LAST clip is asked for first; startNext() falls
	// back to the plain OUT if the engine will not serve it (see QueueItem).
	if (continuePastOutNs > 0 &&
	    direction == ReplayChannel::Direction::Forward) {
		QueueItem &last = items.back();
		const int64_t end = footageEndNs[last.angle];
		const int64_t want = last.tOutNs + continuePastOutNs;
		// Stop at the footage too: asking past the live edge is a range the
		// ring refuses outright, and that refusal would cost the whole clip.
		const int64_t to = (end != kNoInstant && end < want) ? end : want;
		if (to > last.tOutNs) {
			last.continueToNs = to;
			obs_log(LOG_INFO,
				"coordinator: continuing %.1f s past the OUT of "
				"event %d angle %d",
				(double)(to - last.tOutNs) / 1e9, last.eventId,
				last.angle + 1);
		}
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
			"mode=%s, angles=[%s], direction=%s",
			queue_.size(), eventIds.size(),
			mode == AngleMode::AllEnabled ? "all-enabled" : "single",
			angles.c_str(),
			direction == ReplayChannel::Direction::Reverse
				? "reverse"
				: "forward");
	}
	// Reviewing, not watching the live edge: the preview must show the replay
	// source from here on.
	ReplayCore::instance().setFollowLive(false);

	if (toOutput_)
		switchToReplayScene();
	if (musicEnabled_)
		applyMusic(true);
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

bool PlaybackCoordinator::playRanges(
	const std::vector<std::pair<int64_t, int64_t>> &ranges, int angle0,
	int speedPct, bool toOutput, std::string &errorOut)
{
	if (ranges.empty()) {
		errorOut = "no footage in that range";
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	// The previous queue dies here, before a field of the new one is written —
	// same invariant as playEvents(), and for the same reason: a finish callback
	// still carrying a live generation could advance a queue the operator has
	// already replaced.
	playGen_++;

	const int ang = std::clamp(angle0, 0, kEventAngles - 1);
	const int pct = std::clamp(
		speedPct > 0 ? speedPct : defaultSpeedPct_.load(), 5, 400);
	queue_.clear();
	queue_.reserve(ranges.size());
	for (const auto &[in, out] : ranges) {
		if (out <= in)
			continue;
		// eventId 0: there is no event behind a review, and the green band
		// reads that as "nothing selected" rather than naming a clip that
		// has nothing to do with what is playing.
		queue_.push_back(
			{0, in, out, ang, pct, ReplayChannel::Direction::Forward});
	}
	if (queue_.empty()) {
		errorOut = "no footage in that range";
		return false;
	}
	queuePos_ = 0;
	toOutput_ = toOutput;
	active_ = true;
	obs_log(LOG_INFO,
		"coordinator: review queued %zu range(s) on angle %d at %d%%",
		queue_.size(), ang + 1, pct);
	ReplayCore::instance().setFollowLive(false);
	if (toOutput_)
		switchToReplayScene();
	lastStartError_.clear();
	startNext();
	if (!active_) {
		errorOut = lastStartError_.empty() ? "that instant cannot be played"
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
	channel().stop();
	playGen_++; // any finish callback still in flight is now stale
	queue_.clear();
	queuePos_ = 0; // no queue, no position into one
	if (active_ && toOutput_)
		restorePreviousScene();
	applyMusic(false);
	// Stop can land in the middle of a dip, and a replay input left at half
	// brightness for the rest of the evening is a far worse bug than a
	// transition that did not finish. The generation bump above already tells
	// the running ramp to give up; this is what it gives up TO.
	setDip(1.0);
	active_ = false;
}

bool PlaybackCoordinator::queueActive() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return active_;
}

// WHAT IS ON AIR — asked by the dock's poll thirty times a second, and it must
// never be able to park the UI thread.
//
// It used to take mutex_ outright, and mutex_ is held across startNext(), which
// calls ReplayChannel::play(): that joins the previous playback thread and
// materialises and decodes the packets for the clip. Measured by the dock's own
// phase accounting, that is a poll() tick of up to 1049 ms with 539 ms of it
// spent right here — a second in which Qt processes no paint events, which is
// the black, frozen panel by a second route.
//
// So the lock is only TRIED. When it is held, the last answer is returned
// instead: it is a few milliseconds old and it describes the clip that is still
// on screen, because the thing making us wait is the NEXT one being prepared.
// A slightly stale readout of the green band costs nothing; a frozen interface
// during a match costs the match.
PlaybackCoordinator::PlayState PlaybackCoordinator::playState() const
{
	std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
	if (!lock.owns_lock()) {
		std::lock_guard<std::mutex> snap(snapMutex_);
		return snapshot_;
	}
	PlayState s;
	s.active = active_;
	s.queued = (int)queue_.size();
	s.queuedAngles.reserve(queue_.size());
	for (const QueueItem &q : queue_)
		s.queuedAngles.push_back(q.angle + 1); // 0-based → 1-based
	s.queuedWallNs.reserve(queue_.size());
	for (const QueueItem &q : queue_) {
		// The EFFECTIVE out: with "continue past the OUT" on, the last clip
		// of the queue runs longer than the event says, and a band drawn from
		// the event's own OUT would fill up and then keep playing.
		const int64_t out = q.effectiveOutNs();
		const int64_t len = out > q.tInNs ? out - q.tInNs : 0;
		const int pct = q.speedPct > 0 ? q.speedPct : 100;
		// Wall time, not footage time: at 50% four seconds of footage is
		// eight seconds of watching, and the band counts down what the
		// operator is about to sit through.
		s.queuedWallNs.push_back(len * 100 / pct);
	}
	if (active_ && queuePos_ < queue_.size()) {
		s.eventId = queue_[queuePos_].eventId;
		s.angle1 = queue_[queuePos_].angle + 1; // 0-based → 1-based
		s.queuePos = (int)queuePos_ + 1;
		s.speedPct = queue_[queuePos_].speedPct;
		s.reverse = queue_[queuePos_].direction ==
			    ReplayChannel::Direction::Reverse;
		// The EFFECTIVE out again, for the same reason the green band uses
		// it: with "continue past the OUT" on the clip really runs to
		// there, and a multiview cued to the marked OUT would freeze a
		// second and a half before the picture on air does.
		s.clipInNs = queue_[queuePos_].tInNs;
		s.clipOutNs = queue_[queuePos_].effectiveOutNs();
	}
	// Kept for the next caller that finds the lock busy. Refreshed on every
	// successful read — the dock's own 30 Hz poll — so it is never more than a
	// tick behind, and nothing else has to remember to publish it.
	{
		std::lock_guard<std::mutex> snap(snapMutex_);
		snapshot_ = s;
	}
	return s;
}

bool PlaybackCoordinator::setLiveSpeedPct(int pct)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_ || queuePos_ >= queue_.size() || !channel().playing())
		return false;
	const int clamped = std::clamp(pct, 5, 400);
	// The queue's record too, or playState() would keep reporting the speed
	// the clip STARTED at and the green band would print a number that stopped
	// being true the moment the operator moved the dial.
	queue_[queuePos_].speedPct = clamped;
	channel().setSpeedPct(clamped);
	obs_log(LOG_INFO, "coordinator: live speed → %d%% on event %d angle %d",
		clamped, queue_[queuePos_].eventId, queue_[queuePos_].angle + 1);
	return true;
}

void PlaybackCoordinator::setPaused(bool paused)
{
	// No lock: this is one atomic in the channel, and taking mutex_ here would
	// put the pause key behind whatever the queue is doing.
	channel().setPaused(paused);
}

bool PlaybackCoordinator::paused() const
{
	return channel().paused();
}

bool PlaybackCoordinator::skipToNext()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_)
		return false;
	// The generation is bumped BEFORE the stop, not after: stopping the clip
	// can put a finish callback in flight, and that callback must not advance
	// the queue a second time on top of the advance we are about to do. With
	// the generation moved, onClipFinished() drops it.
	playGen_++;
	channel().stop();
	// Exactly what the natural end of a clip does — including Loop, and
	// including ending the sequence when this was the last item.
	onEventFinished();
	return true;
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
		QueueItem &item = queue_[queuePos_];
		const uint64_t gen = ++playGen_;
		// Installed before play() so the worker picks up the callback that
		// belongs to THIS clip.
		channel().setOnFinished([gen]() {
			obs_queue_task(OBS_TASK_UI, finishedTask,
				       (void *)(uintptr_t)gen, false);
		});
		std::string err;
		ReplayChannel::PlayRequest req;
		req.camIndex = item.angle;
		req.inNs = item.tInNs;
		req.outNs = item.effectiveOutNs();
		req.speedPct = item.speedPct;
		req.direction = item.direction;
		// "Continue past the OUT" asks for more footage than the event has,
		// and more footage is exactly what the engine may not be able to serve
		// in one piece: the continuation can run past the end of the file that
		// holds the IN, or past the live edge. A range is served exactly or not
		// at all, so a refusal here would cost the operator the whole replay
		// over an extra second and a half he asked for as a nicety. Ask, then
		// settle for the event as it was marked — and SAY SO, because a replay
		// that quietly stops honouring a setting teaches the operator that the
		// setting does nothing.
		bool ok = channel().play(req, err);
		if (!ok && item.continueToNs != kNoInstant) {
			obs_log(LOG_INFO,
				"coordinator: cannot continue past the OUT of event "
				"%d angle %d (%s) — playing to the OUT instead",
				item.eventId, item.angle + 1, err.c_str());
			// Cleared, not just ignored: playState() reads it to tell the
			// green band how long this clip runs for, and a band counting
			// down an extension that was refused would be counting down
			// something nobody is going to see.
			item.continueToNs = kNoInstant;
			req.outNs = item.tOutNs;
			ok = channel().play(req, err);
		}
		if (ok) {
			obs_log(LOG_INFO,
				"coordinator: playing event %d angle %d (%d%%%s) "
				"[%zu/%zu]",
				item.eventId, item.angle + 1, item.speedPct,
				item.direction == ReplayChannel::Direction::Reverse
					? ", reverse"
					: "",
				queuePos_ + 1, queue_.size());
			// THE NEXT ONE, FETCHED WHILE THIS ONE PLAYS.
			//
			// The gap between two clips of a sequence used to be the
			// next clip being read — an open, a seek and a demux for
			// anything older than the RAM ring — with the previous
			// clip's last picture still on screen. That is what was
			// reported as the end of an event freezing before the next
			// angle came in. Asked for here, it has the whole of the
			// current clip to be ready in.
			//
			// After the log line and after play() returned, so the
			// picture is already moving: the join inside prefetch()
			// costs the thread that calls it, and that thread is this
			// one.
			if (queuePos_ + 1 < queue_.size()) {
				const QueueItem &nxt = queue_[queuePos_ + 1];
				ReplayChannel::PlayRequest pre;
				pre.camIndex = nxt.angle;
				pre.inNs = nxt.tInNs;
				pre.outNs = nxt.effectiveOutNs();
				pre.speedPct = nxt.speedPct;
				pre.direction = nxt.direction;
				channel().prefetch(pre);
			}
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
	channel().stop();
	active_ = false;
	if (toOutput_)
		restorePreviousScene();
	applyMusic(false);
}

void PlaybackCoordinator::onEventFinished()
{
	// mutex_ held by caller.
	const size_t prev = queuePos_;
	queuePos_++;
	if (queuePos_ < queue_.size()) {
		// A CUT between two angles, a DIP between two events — see
		// Config.eventFadeMs. Two angles are two lenses on one action and
		// the operator is comparing them frame for frame; two events are
		// two different things happening, and running one straight into
		// the next is what a highlights sequence reads badly as.
		const int fadeMs = ReplayCore::instance().getConfig().eventFadeMs;
		const bool newEvent = prev < queue_.size() &&
				      queue_[prev].eventId != queue_[queuePos_].eventId;
		if (fadeMs > 0 && newEvent)
			startNextDipped(fadeMs);
		else
			startNext();
	} else if (loop_ && !queue_.empty()) {
		// the reference controller Loop: restart the selection
		queuePos_ = 0;
		startNext();
	} else {
		active_ = false;
		if (toOutput_)
			restorePreviousScene();
		applyMusic(false);
	}
}

// ---------------------------------------------------------------------------
// The dip between two events
// ---------------------------------------------------------------------------
//
// Not a cross-dissolve, and not by preference: mixing the tail of one clip into
// the head of the next means having both decoded at the same time, and this
// engine pushes ONE decoded clip at a time into ONE OBS input. What it can do
// for free is fade that input out and back in — which is what separates two
// actions, and is exactly what the cut was failing to do in a highlights run.
//
// The fade is OBS's own Color Correction filter (id "color_filter", version 2 —
// checked against obs-filters/color-correction-filter.c, whose opacity is a
// double 0..1). A private filter on our own input: no pixel work of ours, no
// shader of ours, and it disappears with the source. Opacity 1 is a no-op, so
// the filter can live there for the whole session and cost nothing between
// dips.
//
// Timing runs on the GUI thread through QTimer, because that is the thread the
// queue is advanced on (onClipFinished arrives via the OBS UI task queue) and
// because a fade needs a clock the audio thread must not be asked to keep.
namespace {
constexpr const char *kFadeFilterName = "MultiReplay Dip";
constexpr int kFadeStepMs = 16; // ~60 Hz: the eye's limit, not the frame rate's

// Set our fade filter's opacity on `src`, creating it on the way down and
// TAKING IT OFF AGAIN at the top. Returns false when OBS has no
// colour-correction filter to give (a stripped build), and then the caller cuts
// — a dip nobody can draw is not worth holding a replay for.
//
// Removing it at opacity 1 is not tidiness. A filter left on the replay input
// is saved with the scene collection, so it comes back on the next launch and
// sits in the operator's filter list for ever — one more thing on his source
// that he did not put there and cannot explain. Measured: after one gate run
// the collection already carried 'MultiReplay Dip' on both replay inputs. A
// create/destroy per event boundary is a few microseconds once per clip.
bool setDipOpacity(obs_source_t *src, double opacity)
{
	if (!src)
		return false;
	if (opacity >= 0.999) {
		if (obs_source_t *f = obs_source_get_filter_by_name(
			    src, kFadeFilterName)) {
			obs_source_filter_remove(src, f);
			obs_source_release(f);
		}
		return true;
	}
	obs_source_t *f = obs_source_get_filter_by_name(src, kFadeFilterName);
	if (!f) {
		obs_data_t *s = obs_data_create();
		obs_data_set_double(s, "opacity", opacity);
		f = obs_source_create_private("color_filter", kFadeFilterName, s);
		obs_data_release(s);
		if (!f) {
			obs_log(LOG_WARNING,
				"[dip] OBS has no colour-correction filter — "
				"events will be cut, not dipped");
			return false;
		}
		obs_source_filter_add(src, f);
		obs_source_release(f);
		return true;
	}
	obs_data_t *s = obs_source_get_settings(f);
	obs_data_set_double(s, "opacity", opacity);
	obs_source_update(f, s);
	obs_data_release(s);
	obs_source_release(f);
	return true;
}
} // namespace

void PlaybackCoordinator::setDip(double opacity)
{
	obs_source_t *src = channel().acquireSource();
	if (!src)
		return;
	setDipOpacity(src, opacity);
	obs_source_release(src);
}

void PlaybackCoordinator::rampDip(double from, double to, int ms,
				  uint64_t gen, std::function<void()> done)
{
	// A shared step counter rather than a wall clock: the timer is the clock,
	// and reading a second one only lets the two disagree.
	const int steps = std::max(1, ms / kFadeStepMs);
	auto step = std::make_shared<int>(0);
	auto *t = new QTimer(qApp);
	t->setInterval(kFadeStepMs);
	// Every capture named: `[=, this]` is ill-formed before C++20 and this
	// plugin is built on three compilers.
	QObject::connect(t, &QTimer::timeout, qApp,
			 [this, t, from, to, steps, step, gen, done]() {
		// The queue this dip belonged to may have been stopped or
		// replaced while it ran. Leave the input fully opaque and go:
		// a replay held at half brightness because a fade outlived its
		// sequence is worse than any missed transition.
		if (gen != playGen_.load()) {
			t->stop();
			t->deleteLater();
			setDip(1.0);
			return;
		}
		++*step;
		const double k = std::min(1.0, (double)*step / (double)steps);
		setDip(from + (to - from) * k);
		if (*step < steps)
			return;
		t->stop();
		t->deleteLater();
		if (done)
			done();
	});
	t->start();
}

void PlaybackCoordinator::startNextDipped(int fadeMs)
{
	// mutex_ held by caller.
	//
	// Half out, half in. The next clip is started at the BOTTOM of the dip,
	// so the first frames of the new event arrive while the input is still
	// dark — which is what makes the two halves read as one gesture rather
	// than as a gap followed by a fade.
	const int half = std::max(kFadeStepMs, fadeMs / 2);
	const uint64_t gen = playGen_.load();
	rampDip(1.0, 0.0, half, gen, [this, half, gen]() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (gen != playGen_.load()) {
				setDip(1.0);
				return;
			}
			startNext();
		}
		rampDip(0.0, 1.0, half, gen, nullptr);
	});
}

// The spare libobs mixer channel the music bed rides on.
//
// OBS Studio itself uses 0 for the program scene and 1..6 for the audio devices
// configured in Settings ▸ Audio, and libobs has room for far more. 7 is the
// first one nobody is using, and a source put there is in the program mix
// without being in anybody's scene — which is the whole difficulty with playing
// a music FILE from a plugin. It is set while a replay runs and cleared at the
// end, so it cannot outlive the thing it belongs to.
static constexpr uint32_t kMusicChannel = 7;

// WHOSE CHANNEL 7 IS IT (A9).
//
// applyMusic() used to call obs_set_output_source(7, ours) and releaseMusic()
// used to call obs_set_output_source(7, nullptr). Two things were wrong with
// that pair. Whatever the operator — or another plugin — had put on that mixer
// channel disappeared without a word and never came back; and with the second
// bay enabled A and B are two coordinators doing it to each other, so the bed
// of whichever one started first goes silent when the other one plays.
//
// So: the channel is TAKEN by one owner at a time, what was there is held and
// put back, and the second bay is refused with a line in the log instead of
// winning silently. File-scope because there is one mixer, not one per bay.
namespace {
std::mutex g_musicChannelMutex;
const void *g_musicChannelOwner = nullptr;
obs_source_t *g_musicChannelPrev = nullptr; // ref held while we own it

bool takeMusicChannel(const void *owner, obs_source_t *src)
{
	std::lock_guard<std::mutex> lock(g_musicChannelMutex);
	if (g_musicChannelOwner && g_musicChannelOwner != owner) {
		obs_log(LOG_WARNING,
			"[music] the other bay is already playing the bed — "
			"leaving it alone");
		return false;
	}
	if (!g_musicChannelOwner) {
		// Adds a ref, which we hold until we give the channel back.
		g_musicChannelPrev = obs_get_output_source(kMusicChannel);
		g_musicChannelOwner = owner;
	}
	obs_set_output_source(kMusicChannel, src);
	return true;
}

void giveBackMusicChannel(const void *owner)
{
	std::lock_guard<std::mutex> lock(g_musicChannelMutex);
	if (g_musicChannelOwner != owner)
		return;
	// Back to whatever was there, which is usually nothing — and when it is
	// not, it is somebody else's and was never ours to drop.
	obs_set_output_source(kMusicChannel, g_musicChannelPrev);
	if (g_musicChannelPrev) {
		obs_source_release(g_musicChannelPrev);
		g_musicChannelPrev = nullptr;
	}
	g_musicChannelOwner = nullptr;
}
} // namespace

std::string PlaybackCoordinator::musicProblem() const
{
	const Config cfg = ReplayCore::instance().getConfig();
	if (!cfg.musicFilePath.empty()) {
		std::error_code ec;
		if (!std::filesystem::exists(cfg.musicFilePath, ec))
			return "the music file does not exist: " +
			       cfg.musicFilePath;
		return {};
	}
	if (cfg.musicSourceName.empty())
		return "no music is configured: set a music file or a music "
		       "source in Settings";

	obs_source_t *src = obs_get_source_by_name(cfg.musicSourceName.c_str());
	if (!src)
		return "the music source '" + cfg.musicSourceName +
		       "' does not exist in this scene collection";
	// UNMUTING A SOURCE NOBODY IS LISTENING TO IS SILENCE. A source that is
	// not in an active scene contributes nothing to the mix however unmuted
	// it is, and that is the failure that says nothing: the key lights up,
	// the replay rolls, no music. Said out loud instead.
	const bool active = obs_source_active(src);
	obs_source_release(src);
	if (!active)
		return "the music source '" + cfg.musicSourceName +
		       "' is not in an active scene, so unmuting it cannot be "
		       "heard — put it in a scene, or set a music file instead";
	return {};
}

void PlaybackCoordinator::applyMusic(bool on)
{
	const Config cfg = ReplayCore::instance().getConfig();

	// --- a FILE: our own player, no scene required ------------------------
	// This is the half that never worked at all. Config.musicFilePath was
	// only ever read by the reel exporter, so choosing a file and pressing ♫
	// played nothing and reported nothing. A file has no source and no scene
	// item behind it, so there is nothing to unmute — it needs a player, and
	// the player needs somewhere in the mix to be heard from.
	if (on && !cfg.musicFilePath.empty()) {
		std::error_code ec;
		if (!std::filesystem::exists(cfg.musicFilePath, ec)) {
			obs_log(LOG_WARNING, "[music] file not found: %s",
				cfg.musicFilePath.c_str());
			return;
		}
		if (!musicSrc_) {
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "local_file",
					    cfg.musicFilePath.c_str());
			// A bed loops and does not close between replays: a
			// second replay must not wait for the file to be
			// reopened, and one that outlasts the track must not
			// fall silent halfway through.
			obs_data_set_bool(s, "looping", true);
			obs_data_set_bool(s, "close_when_inactive", false);
			obs_data_set_bool(s, "restart_on_activate", true);
			musicSrc_ = obs_source_create_private(
				"ffmpeg_source", "MultiReplay Music", s);
			obs_data_release(s);
			if (!musicSrc_) {
				obs_log(LOG_WARNING,
					"[music] could not create the player for %s",
					cfg.musicFilePath.c_str());
				return;
			}
			obs_log(LOG_INFO, "[music] player created for %s",
				cfg.musicFilePath.c_str());
		} else {
			// The path may have changed in Settings since the last
			// replay; updating is cheaper than rebuilding, and keeps
			// one player for the whole session.
			obs_data_t *s = obs_source_get_settings(musicSrc_);
			const char *cur = obs_data_get_string(s, "local_file");
			if (!cur || cfg.musicFilePath != cur) {
				obs_data_set_string(s, "local_file",
						    cfg.musicFilePath.c_str());
				obs_source_update(musicSrc_, s);
			}
			obs_data_release(s);
		}
		obs_source_set_muted(musicSrc_, false);
		if (!takeMusicChannel(this, musicSrc_))
			return;
		// FROM THE TOP, every time. A bed parked at the end of its track
		// is silence with the volume up, which is exactly what the
		// second replay of an evening would have got.
		obs_source_media_restart(musicSrc_);
		return;
	}

	if (!on && musicSrc_) {
		giveBackMusicChannel(this);
		obs_source_media_stop(musicSrc_);
	}

	// --- an OBS SOURCE: the operator's own, unmuted while the replay runs -
	if (cfg.musicSourceName.empty())
		return;
	obs_source_t *src = obs_get_source_by_name(cfg.musicSourceName.c_str());
	if (!src) {
		if (on)
			obs_log(LOG_WARNING,
				"[music] source '%s' does not exist",
				cfg.musicSourceName.c_str());
		return;
	}
	obs_source_set_muted(src, !on);
	// A media source is not only muted, it is also PARKED. Unmuting one that
	// played to its end during the last replay produces nothing at all, so
	// its transport is driven too when it has one.
	if (obs_source_get_output_flags(src) & OBS_SOURCE_CONTROLLABLE_MEDIA) {
		if (on)
			obs_source_media_restart(src);
		else
			obs_source_media_stop(src);
	}
	if (on && !obs_source_active(src))
		obs_log(LOG_WARNING,
			"[music] source '%s' is not in an active scene — "
			"unmuting it cannot be heard",
			cfg.musicSourceName.c_str());
	obs_source_release(src);
}

void PlaybackCoordinator::releaseMusic()
{
	if (!musicSrc_)
		return;
	giveBackMusicChannel(this);
	obs_source_release(musicSrc_);
	musicSrc_ = nullptr;
	obs_log(LOG_INFO, "[music] player released");
}

void PlaybackCoordinator::switchToReplayScene()
{
	// mutex_ held by caller.
	//
	// NOT wait=true, and this is the fix for a real deadlock (B5).
	//
	// It used to wait so that previousSceneName_ was filled in before this
	// returned — otherwise a very short clip could finish and call
	// restorePreviousScene() while the name was still empty, stranding
	// program on the replay scene. The reasoning about switchSceneTask was
	// right (it takes no lock of ours) and stopped one step short: THE
	// DEADLOCK IS NOT INSIDE THE TASK, it is that the UI thread may already
	// be waiting on mutex_. A Stream Deck "play" enters playEvents on the
	// hotkey thread, takes mutex_ and waits for the UI thread; the operator
	// presses Stop, the UI thread enters stopEvents and waits for mutex_.
	// AB-BA, OBS frozen, with a take recording.
	//
	// The ordering is bought a different way now: the save happens INSIDE the
	// task, and the restore is another task posted afterwards onto the same
	// FIFO UI queue. A queue cannot run the second before the first, which is
	// all the wait was ever guaranteeing.
	//
	// There is no plugin-managed scene any more: "MultiReplay - Replay A" is
	// an ordinary OBS input the operator puts where he wants it, so the only
	// thing to switch to is the scene he configured. With none configured the
	// clip still plays into the input — we just don't take program from him.
	// The scene of THIS channel. A and B are two different inputs, so they
	// live in two different scenes, and sending both to the same one is how
	// pressing play on B put A on air: B's clip went into B's input, which
	// that scene does not contain, so program showed whatever A had last.
	// Reported from the panel, and exactly right.
	const Config cfg = ReplayCore::instance().getConfig();
	std::string scene = which_ == Which::B ? cfg.outputSceneNameB
					       : cfg.outputSceneName;
	if (scene.empty()) {
		obs_log(LOG_INFO,
			"coordinator: no output scene configured for channel %s — "
			"playing into '%s' without switching program",
			channelLetter(which_), channel().sourceName());
		return;
	}
	// the reference controller's event transition: how the replay ARRIVES. Empty = leave OBS's own
	// transition alone, which is the default.
	auto *ctx = new SceneSwitchCtx{scene, this, cfg.transitionInName,
				       cfg.transitionMs, false};
	runOnUi(switchSceneTask, ctx, false);
}

void PlaybackCoordinator::restorePreviousScene()
{
	// mutex_ held by caller.
	//
	// The name is NOT read here. It is read on the UI thread, by the task,
	// after whatever switch task was posted before it has run — which is the
	// ordering that replaced the blocking dispatch (see switchToReplayScene).
	// Reading it here would be reading it too early again, just without the
	// deadlock.
	//
	// ...and how program COMES BACK, which is a different moment and gets its
	// own choice: a dip to the replay and a cut back out is a normal way to work.
	const Config cfg = ReplayCore::instance().getConfig();
	auto *ctx = new SceneSwitchCtx{std::string(), this,
				       cfg.transitionOutName, cfg.transitionMs,
				       true};
	runOnUi(switchSceneTask, ctx, false);
}

void PlaybackCoordinator::rememberPreviousScene(const std::string &name)
{
	std::lock_guard<std::mutex> lock(sceneMutex_);
	previousSceneName_ = name;
}

std::string PlaybackCoordinator::takePreviousScene()
{
	std::lock_guard<std::mutex> lock(sceneMutex_);
	std::string out;
	out.swap(previousSceneName_);
	return out;
}

} // namespace multireplay
