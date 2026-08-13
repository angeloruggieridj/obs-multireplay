/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See selftest.hpp. This is the scripted form of the M0 gate.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)
#include <obs-frontend-api.h>

#include "selftest.hpp"

#include "branch-output-control.hpp"
#include "event-store.hpp"
#include "multireplay-dock.hpp"
#include "packet-tap.hpp"
#include "playback-coordinator.hpp"
#include "plugin-support.h"
#include "replay-channel.hpp"
#include "replay-core.hpp"
#include "replay-decoder.hpp"
#include "segment-index.hpp"

#include <util/base.h>
#include <util/platform.h>

#include <QMainWindow>
#include <QObject>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace multireplay {

namespace {

std::atomic<bool> g_started{false};

std::string envStr(const char *key, const std::string &def = {})
{
	const char *v = getenv(key);
	return (v && *v) ? std::string(v) : def;
}

bool envOn(const char *key)
{
	const char *v = getenv(key);
	return v && *v && strcmp(v, "0") != 0;
}

int envInt(const char *key, int def)
{
	const std::string s = envStr(key);
	if (s.empty())
		return def;
	try {
		return std::stoi(s);
	} catch (...) {
		return def;
	}
}

// Split "A,B,C" into names, trimming surrounding blanks.
std::vector<std::string> splitCsv(const std::string &s)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size() && !s.empty()) {
		size_t comma = s.find(',', start);
		if (comma == std::string::npos)
			comma = s.size();
		std::string item = s.substr(start, comma - start);
		size_t b = item.find_first_not_of(" \t");
		size_t e = item.find_last_not_of(" \t");
		if (b != std::string::npos)
			out.push_back(item.substr(b, e - b + 1));
		if (comma == s.size())
			break;
		start = comma + 1;
	}
	return out;
}

// Run `fn` on the OBS UI thread and wait for it.
//
// This matters more than it looks: Branch Output evaluates its start conditions
// from a QTimer owned by the filter. A QTimer created on a plain std::thread
// has no event loop, so it never fires and the filter is created but never
// starts recording. In the normal product flow the filters are created from the
// dock (already the UI thread); the self-test has to ask for it explicitly.
void runOnUi(const std::function<void()> &fn)
{
	if (obs_in_task_thread(OBS_TASK_UI)) {
		fn();
		return;
	}
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) { (*static_cast<const std::function<void()> *>(p))(); },
		const_cast<std::function<void()> *>(&fn), true);
}

// A synthetic camera. OBS renamed the colour source a few times; try newest first.
obs_source_t *createSyntheticCamera(int idx, uint32_t cx, uint32_t cy)
{
	static const char *kColorIds[] = {"color_source_v3", "color_source_v2",
					  "color_source"};
	// Distinct colours so the recordings are visually distinguishable.
	static const uint32_t kColors[] = {0xFF2266DD, 0xFF22AA55, 0xFFDD8822,
					   0xFFCC3355, 0xFF8844CC, 0xFF11AAAA,
					   0xFFBBBB22, 0xFF777777};

	const std::string name = "MRSelfTest Cam" + std::to_string(idx + 1);

	for (const char *id : kColorIds) {
		if (!obs_source_get_display_name(id))
			continue;
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "color",
				 kColors[idx % (int)(sizeof(kColors) /
						     sizeof(kColors[0]))]);
		obs_data_set_int(s, "width", cx);
		obs_data_set_int(s, "height", cy);
		obs_source_t *src = obs_source_create(id, name.c_str(), s, nullptr);
		obs_data_release(s);
		if (src) {
			obs_log(LOG_INFO, "[selftest] created synthetic camera '%s' (%s)",
				name.c_str(), id);
			return src;
		}
	}
	obs_log(LOG_ERROR, "[selftest] no colour source type available");
	return nullptr;
}

// ---------------------------------------------------------------------------
// The dock, driven through its own widgets.
//
// Everything above this point proves the engine. None of it touches the only
// thing the operator ever uses, and the rewrite left the dock re-wired but
// never once run: a dock that throws on construction, freezes the GUI in its
// timer, marks at an instant no footage covers, or fails to play what it just
// marked would sail through the whole M0 gate. These checks close that hole by
// finding the REAL dock OBS registered and clicking its REAL buttons.
// ---------------------------------------------------------------------------

struct DockChecks {
	bool found = false;
	bool pollRuns = false;
	bool pollQuiet = false;
	bool pollResponsive = false;
	bool markOnTimeline = false;
	bool playsMark = false;
	bool playheadInsideClip = false;
	bool multiAngleQueue = false;
	// Any combination of enabled angles, and the combination changing between
	// two plays of the SAME event. These are separate claims: the first is
	// about how a queue is built, the second about the previous queue being
	// gone before the new one is built.
	bool angleCombinations = false;
	bool angleChoiceRepeatable = false;
	bool singleNonFirstAnglePlays = false;
	// The queue does not just hold two clips, it walks them: clip 1 ends and
	// clip 2 goes on air by itself. That hop crosses two threads (the playback
	// worker's finish callback, re-posted onto the OBS UI queue) and is the
	// half of "play both angles" no shape check can see.
	bool queueAdvancesToSecond = false;
	// The preview belongs to the SEQUENCE, not to the clip: between two angles
	// of the same event the engine is briefly idle, and the dock used to put the
	// live camera on screen for that moment - on air too, with "to output".
	bool previewHoldsSequence = false;
	// ...and when the sequence ends, the transport goes back to the live edge by
	// itself, instead of leaving the operator to press NOW.
	bool followsLiveAfterSequence = false;
	// A scrub shows the FOOTAGE at that instant and keeps showing it after the
	// review clip has run out - the live camera belongs to follow-live only.
	bool scrubShowsFootage = false;
	// The angle button wins: Single mode plays the camera it was asked for, and
	// refuses out loud when that camera has nothing, instead of quietly playing
	// a different one.
	bool singleHonoursRequestedAngle = false;
	bool singleReportsUnplayableAngle = false;
	// A mark flags the angle the operator is WATCHING. Tested on an angle that
	// is not the first one, because "always flag camera 1" passes any check made
	// on camera 1 - and that was the real bug on the hotkey path.
	bool markInheritsAngle = false;
	int previewLiveSamples = 0; // frames of the sequence spent on the live camera
	int queuedClips = 0;
	int ticks = 0;
	int64_t worstGapMs = 0;
	int logErrors = 0;
	int64_t markInNs = 0;
	int64_t markOutNs = 0;
	int playedFrames = 0;
};

// Counts OUR error lines while the dock's timer is being watched. Only ours:
// the claim is "the dock's poll is quiet", not "nothing in OBS ever complains".
std::atomic<int> g_ourLogErrors{0};
std::atomic<bool> g_countLogErrors{false};
log_handler_t g_prevLogHandler = nullptr;
void *g_prevLogParam = nullptr;

void countingLogHandler(int level, const char *msg, va_list args, void *)
{
	if (g_countLogErrors.load() && level <= LOG_ERROR && msg &&
	    strstr(msg, PLUGIN_NAME) != nullptr)
		g_ourLogErrors++;
	// Chain, so the run still produces its normal OBS log.
	if (g_prevLogHandler)
		g_prevLogHandler(level, msg, args, g_prevLogParam);
}

DockChecks runDockChecks(int firstCam, int secondCam,
			 const std::string &tempFolder, double canvasFps)
{
	DockChecks c;

	MultiReplayDock *dock = nullptr;
	QTimer *pollTimer = nullptr;
	QPushButton *markBtn = nullptr; // the "-5s" preset
	QPushButton *playBtn = nullptr; // "play selected"

	runOnUi([&]() {
		auto *main = static_cast<QMainWindow *>(
			obs_frontend_get_main_window());
		if (!main)
			return;
		// The registered dock, not a fresh one: this also proves
		// obs_frontend_add_dock_by_id actually took it.
		dock = main->findChild<MultiReplayDock *>();
		if (!dock)
			return;
		for (QTimer *t : dock->findChildren<QTimer *>()) {
			if (t->isActive() && t->interval() > 0 &&
			    t->interval() <= 100) {
				pollTimer = t;
				break;
			}
		}
		// Matched on the text the dock itself builds: "-5s" is composed
		// from a number so it is locale-independent, and the play button
		// goes through the same obs_module_text() we do.
		const QString mark5 = QStringLiteral("-5s");
		const QString playSel = QString::fromUtf8(
			obs_module_text("Dock.PlaySelected"));
		for (QPushButton *b : dock->findChildren<QPushButton *>()) {
			if (b->text() == mark5)
				markBtn = b;
			else if (b->text() == playSel)
				playBtn = b;
		}
	});

	c.found = dock && pollTimer && markBtn && playBtn;
	if (!c.found) {
		obs_log(LOG_ERROR,
			"[selftest] dock not usable (dock=%p timer=%p mark=%p "
			"play=%p)",
			(void *)dock, (void *)pollTimer, (void *)markBtn,
			(void *)playBtn);
		return c;
	}

	// --- Is the timer running, and does it hand the GUI thread back? -----
	// The gap between consecutive ticks is measured AFTER the dock's own
	// slot has run (our connection is made later, so it fires later), which
	// makes the worst gap the honest cost of a poll: a poll that blocks —
	// on a network stat(), a lock, a modal — shows up here and nowhere else.
	std::atomic<int> ticks{0};
	std::atomic<int64_t> worstGapNs{0};
	int64_t lastTickNs = 0; // UI thread only
	QMetaObject::Connection conn;

	g_ourLogErrors.store(0);
	base_get_log_handler(&g_prevLogHandler, &g_prevLogParam);
	base_set_log_handler(countingLogHandler, nullptr);
	g_countLogErrors.store(true);
	runOnUi([&]() {
		lastTickNs = (int64_t)os_gettime_ns();
		conn = QObject::connect(pollTimer, &QTimer::timeout, dock,
					[&]() {
						const int64_t now =
							(int64_t)os_gettime_ns();
						const int64_t gap =
							now - lastTickNs;
						lastTickNs = now;
						if (gap > worstGapNs.load())
							worstGapNs.store(gap);
						ticks++;
					});
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(2000));
	runOnUi([&]() { QObject::disconnect(conn); });
	g_countLogErrors.store(false);
	base_set_log_handler(g_prevLogHandler, g_prevLogParam);

	c.ticks = ticks.load();
	c.worstGapMs = worstGapNs.load() / 1000000;
	c.logErrors = g_ourLogErrors.load();
	// 2 s of a 33 ms timer is ~60 ticks; 40 leaves room for a busy machine
	// while still failing a timer that never started.
	c.pollRuns = c.ticks >= 40;
	// 250 ms is far above normal jitter and far below anything a human would
	// not notice as a freeze.
	c.pollResponsive = c.pollRuns && c.worstGapMs <= 250;
	c.pollQuiet = c.logErrors == 0;
	obs_log(LOG_INFO,
		"[selftest] dock: %d poll ticks in 2 s, worst gap %lld ms, "
		"%d plugin errors logged",
		c.ticks, (long long)c.worstGapMs, c.logErrors);

	// --- Mark, through the button an operator actually hits ---------------
	auto &store = EventStore::instance();
	// Point the store at the throwaway folder first: this must neither read
	// nor overwrite the operator's own events.json.
	store.setSessionFolder(tempFolder);
	store.setLiveMode(true);
	// The dock marks on ITS current angle, which it copies from the core on
	// every tick — so set it there and let one tick carry it across.
	ReplayCore::instance().setCurrentAngle(firstCam);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	runOnUi([&]() { markBtn->click(); });

	ReplayEvent ev;
	const int evId = store.lastEventId();
	const bool haveEvent = evId > 0 && store.get(evId, ev);
	if (haveEvent) {
		c.markInNs = ev.tInNs;
		c.markOutNs = ev.tOutNs;
		const int64_t oldest =
			PacketTap::instance().oldestReplayableNs(firstCam);
		const int64_t newest = PacketTap::instance().newestNs(firstCam);
		// The mark has to fall INSIDE the window the dock is drawing,
		// on the angle that was selected. A mark at master 0 — what a
		// dead live edge produces — fails here, which is the regression
		// this exists to catch.
		c.markOnTimeline = ev.tInNs > 0 && oldest > 0 &&
				   ev.tInNs >= oldest && ev.tOutNs <= newest &&
				   ev.tOutNs > ev.tInNs &&
				   ev.angles[firstCam].enabled;
		obs_log(LOG_INFO,
			"[selftest] dock: marked event %d in=%lld ms out=%lld ms "
			"(ring holds %lld..%lld ms, angle %d enabled: %s)",
			evId, (long long)(ev.tInNs / 1000000),
			(long long)(ev.tOutNs / 1000000),
			(long long)(oldest / 1000000),
			(long long)(newest / 1000000), firstCam + 1,
			ev.angles[firstCam].enabled ? "yes" : "NO");
	} else {
		obs_log(LOG_ERROR, "[selftest] dock: the mark button created "
				   "no event");
	}

	// --- Play it back, through the play button ----------------------------
	// The play button plays the SELECTED rows, and the table only selects the
	// fresh mark on its next poll tick - so clicking straight after the mark
	// is a race the gate loses roughly one run in three. When it loses,
	// playEvents finds no event, the dock opens a modal QMessageBox on the GUI
	// thread, and every runOnUi() after it (including the teardown) blocks
	// forever: the run produces no report at all rather than a failure.
	// So wait for the selection the dock is supposed to make.
	bool selected = false;
	for (int i = 0; i < 40 && !selected; i++) {
		runOnUi([&]() {
			for (QTableWidget *t :
			     dock->findChildren<QTableWidget *>()) {
				for (const auto &idx : t->selectionModel()
							       ->selectedRows()) {
					QTableWidgetItem *it =
						t->item(idx.row(), 0);
					if (it && it->data(Qt::UserRole).toInt() ==
							  evId)
						selected = true;
				}
			}
		});
		if (!selected)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(50));
	}
	if (haveEvent && !selected)
		obs_log(LOG_ERROR,
			"[selftest] dock: the table never selected the event it "
			"just marked — 'play selected' would have nothing to play");

	// Checked also that the range is servable, for the same modal reason.
	bool servable = false;
	if (c.markOnTimeline && selected) {
		std::vector<LivePacket> probe;
		int64_t pIn = 0, pOut = 0;
		servable = PacketTap::instance().resolveRange(
			firstCam, ev.tInNs, ev.tOutNs, probe, pIn, pOut);
	}
	if (servable) {
		auto &chan = ReplayChannel::instance();
		obs_source_t *src = chan.acquireSource();
		if (src) {
			// In no scene during the gate, so OBS would not consume
			// the frames we push (see the M2 pass above).
			obs_source_inc_active(src);
			obs_source_inc_showing(src);
		}

		runOnUi([&]() { playBtn->click(); });

		// 5 s of footage at 1x, generously bounded.
		for (int i = 0; i < 300 && chan.playing(); i++)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(50));

		const auto st = chan.stats();
		const int64_t pos = chan.positionNs();
		c.playedFrames = (int)st.framesPushed;
		const int expected = (int)(5.0 * (canvasFps > 0 ? canvasFps : 30.0));
		c.playsMark = st.lastRunCompleted &&
			      c.playedFrames >= expected * 9 / 10;
		// The playhead the dock draws (and marks against in review) must
		// end up inside the clip that just played, never back at 0.
		c.playheadInsideClip =
			pos >= ev.tInNs && pos <= ev.tOutNs;
		obs_log(LOG_INFO,
			"[selftest] dock: play button pushed %d frames, playhead "
			"at %lld ms (clip %lld..%lld ms)",
			c.playedFrames, (long long)(pos / 1000000),
			(long long)(ev.tInNs / 1000000),
			(long long)(ev.tOutNs / 1000000));

		if (src) {
			obs_source_dec_showing(src);
			obs_source_dec_active(src);
			obs_source_release(src);
		}
	} else if (c.markOnTimeline) {
		obs_log(LOG_ERROR, "[selftest] dock: the marked range is not "
				   "servable — not clicking play");
	}

	// --- Any combination of enabled angles, on the same event -------------
	// Enabling C1 and C2 on the same mark is a request to SEE both, one after
	// the other. The queue used to hold a single clip whatever was enabled -
	// every replay in the log read "[1/1]" - and no check outside a live
	// session ever noticed, because nothing looked at the queue.
	//
	// A count is not enough: [1,2] and [2,2] are both "two clips". These
	// assert the exact angle SEQUENCE, for the combinations an operator
	// actually produces - both cameras, one camera that is not the first, and
	// a set with a hole in it (angle 1 and angle 3 on a two-camera rig, where
	// nothing is wired to 3).
	if (!haveEvent) {
		// nothing to queue: the mark check above already failed loudly
	} else if (secondCam < 0) {
		// One camera: nothing about combinations — nor about what happens
		// BETWEEN two clips — can be proven here.
		c.multiAngleQueue = true;
		c.angleCombinations = true;
		c.angleChoiceRepeatable = true;
		c.singleNonFirstAnglePlays = true;
		c.queueAdvancesToSecond = true;
		c.previewHoldsSequence = true;
		c.followsLiveAfterSequence = true;
		c.singleHonoursRequestedAngle = true;
		c.singleReportsUnplayableAngle = true;
		c.markInheritsAngle = true;
	} else {
		auto &pc = PlaybackCoordinator::instance();
		const int a1 = firstCam + 1;
		const int a2 = secondCam + 1;
		// A slot nothing is wired to. On the operator's two-camera rig
		// this IS his "angle 3": enabling it must not shift, drop or
		// reorder the angles that can actually play.
		const int hole = kEventAngles;

		// Set the enabled set EXACTLY, queue it, and read back the shape
		// of the queue. Requesting `reqCam` as the current angle at the
		// same time proves AllEnabled ignores it whenever the event says
		// something.
		const auto queueFor = [&](const char *what,
					  const std::vector<int> &enable,
					  int reqCam,
					  const std::vector<int> &expect) {
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			for (int a : enable)
				store.setAngle(evId, a, true);
			std::string qerr;
			const bool started =
				pc.playEvents({evId}, reqCam, false, qerr);
			const auto ps = pc.playState();
			const bool ok = started && ps.queuedAngles == expect;
			std::string got, want;
			for (int a : ps.queuedAngles)
				got += std::to_string(a) + " ";
			for (int a : expect)
				want += std::to_string(a) + " ";
			obs_log(ok ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: %s — queued [%s] expected [%s]%s%s",
				what, got.c_str(), want.c_str(),
				started ? "" : " — playEvents refused: ",
				started ? "" : qerr.c_str());
			return ok;
		};

		// Both angles, in angle order, whichever one the operator is on.
		const bool both = queueFor("both angles", {a1, a2}, firstCam,
					   {a1, a2});
		const bool bothFromSecond = queueFor("both angles, on the second",
						     {a1, a2}, secondCam,
						     {a1, a2});
		c.queuedClips = pc.playState().queued;
		// Non-contiguous: the enabled set has a hole in it.
		const bool gapLow = queueFor("angles 1 and a hole", {a1, hole},
					     firstCam, {a1});
		const bool gapHigh = queueFor("angles 2 and a hole", {a2, hole},
					      secondCam, {a2});
		// One angle only, and NOT the first one, requested from the
		// first: the event decides, not the button.
		const bool onlySecond = queueFor("only the second angle", {a2},
						 firstCam, {a2});
		c.angleCombinations = both && bothFromSecond && gapLow &&
				      gapHigh && onlySecond;
		c.multiAngleQueue = both;

		// The single non-first angle must not just queue - it must run.
		auto &chan = ReplayChannel::instance();
		bool ranSecond = false;
		if (onlySecond) {
			std::string qerr;
			if (pc.playEvents({evId}, firstCam, false, qerr)) {
				for (int i = 0; i < 30 &&
						chan.stats().framesPushed == 0;
				     i++)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				const auto ps = pc.playState();
				ranSecond = ps.angle1 == a2 &&
					    chan.stats().framesPushed > 0;
				obs_log(ranSecond ? LOG_INFO : LOG_ERROR,
					"[selftest] dock: only-angle-%d playback — "
					"on air angle %d, %llu frame(s)",
					a2, ps.angle1,
					(unsigned long long)
						chan.stats().framesPushed);
			}
			pc.stopEvents();
		}
		c.singleNonFirstAnglePlays = ranSecond;

		// The SAME event, played three times with a different choice each
		// time: one angle, then both, then one again. Each play must see
		// the current enabled set and nothing of the previous queue.
		const bool r1 = queueFor("repeat 1/3: only the second", {a2},
					 firstCam, {a2});
		const bool r2 = queueFor("repeat 2/3: both", {a1, a2}, firstCam,
					 {a1, a2});
		const bool r3 = queueFor("repeat 3/3: only the first", {a1},
					 secondCam, {a1});
		c.angleChoiceRepeatable = r1 && r2 && r3;
		pc.stopEvents();

		// --- and it must WALK the queue, not just hold it -------------
		// Clip 1 ends on the playback thread, which posts the advance
		// onto the OBS UI queue, filtered by a generation counter. If any
		// link in that chain drops the callback the operator sees one
		// angle and the sequence stops - silently, because the queue
		// still reads "2 clips". So let it run for real.
		if (both) {
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			store.setAngle(evId, a1, true);
			store.setAngle(evId, a2, true);
			std::string qerr;
			if (pc.playEvents({evId}, firstCam, false, qerr)) {
				// Watch the WHOLE sequence, sampling faster than
				// the dock's own 33 ms tick: two 5 s clips plus
				// the hop, capped at 20 s.
				bool reachedSecond = false;
				bool secondRuns = false;
				uint64_t framesAtHop = 0;
				int previewSamples = 0, previewLive = 0;
				for (int i = 0; i < 1000; i++) {
					const auto ps = pc.playState();
					if (!reachedSecond && ps.queuePos == 2 &&
					    ps.angle1 == a2) {
						reachedSecond = true;
						framesAtHop = chan.stats()
								      .framesPushed;
					}
					if (reachedSecond &&
					    chan.stats().framesPushed > 0)
						secondRuns = true;
					// Only once something is really on air: the
					// very first tick is legitimately still on
					// the live camera.
					if (chan.stats().framesPushed > 0 ||
					    reachedSecond) {
						previewSamples++;
						if (!dock->previewShowsReplay())
							previewLive++;
					}
					if (!ps.active)
						break; // sequence finished
					std::this_thread::sleep_for(
						std::chrono::milliseconds(20));
				}
				c.queueAdvancesToSecond = reachedSecond &&
							  secondRuns;
				c.previewLiveSamples = previewLive;
				// Not "mostly": a single frame of the wrong camera
				// between two angles is a flash the operator sees,
				// and puts on air with "to output" on.
				c.previewHoldsSequence =
					previewSamples > 0 && previewLive == 0;
				obs_log(c.queueAdvancesToSecond ? LOG_INFO
								: LOG_ERROR,
					"[selftest] dock: two-angle sequence — "
					"reached clip 2 on angle %d: %s, frames "
					"at the hop %llu, frames after %llu",
					a2, reachedSecond ? "yes" : "NO",
					(unsigned long long)framesAtHop,
					(unsigned long long)
						chan.stats().framesPushed);
				obs_log(c.previewHoldsSequence ? LOG_INFO
							       : LOG_ERROR,
					"[selftest] dock: preview stayed on the "
					"replay for %d/%d samples of the sequence",
					previewSamples - previewLive,
					previewSamples);

				// The queue drained by itself: within a second the
				// dock must have handed the transport back to the
				// live edge (the tap is still attached, so there IS
				// a live edge to go back to).
				for (int i = 0; i < 50; i++) {
					if (ReplayCore::instance().followLive()) {
						c.followsLiveAfterSequence = true;
						break;
					}
					std::this_thread::sleep_for(
						std::chrono::milliseconds(20));
				}
				obs_log(c.followsLiveAfterSequence ? LOG_INFO
								   : LOG_ERROR,
					"[selftest] dock: back to the live edge after "
					"the sequence: %s",
					c.followsLiveAfterSequence ? "yes" : "NO");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] dock: two-angle sequence — "
					"playEvents refused: %s", qerr.c_str());
			}
			pc.stopEvents();
		}

		// --- the angle button wins ------------------------------------
		// Only the SECOND angle is flagged, and the operator presses the
		// FIRST. What he asked for is what must play: the old fallback to
		// "the first enabled angle" played another camera without a word,
		// which is the behaviour that made the angle model unguessable.
		{
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			store.setAngle(evId, a2, true);
			std::string serr;
			const bool started = pc.playEvents(
				{evId}, firstCam, false, serr,
				PlaybackCoordinator::AngleMode::Single);
			const auto ps = pc.playState();
			c.singleHonoursRequestedAngle =
				started && ps.queuedAngles ==
						   std::vector<int>{a1};
			std::string got;
			for (int a : ps.queuedAngles)
				got += std::to_string(a) + " ";
			obs_log(c.singleHonoursRequestedAngle ? LOG_INFO
							      : LOG_ERROR,
				"[selftest] dock: angle button wins — asked for %d "
				"with only %d flagged, queued [%s]%s%s",
				a1, a2, got.c_str(),
				started ? "" : " — refused: ",
				started ? "" : serr.c_str());
			pc.stopEvents();

			// ...and a camera with nothing behind it is REFUSED, with
			// something to say. Silence here is what left the operator
			// hearing a camera he had not asked for.
			std::string herr;
			const bool startedHole = pc.playEvents(
				{evId}, hole - 1, false, herr,
				PlaybackCoordinator::AngleMode::Single);
			c.singleReportsUnplayableAngle =
				!startedHole && !herr.empty();
			obs_log(c.singleReportsUnplayableAngle ? LOG_INFO
							       : LOG_ERROR,
				"[selftest] dock: unplayable angle %d — started=%s, "
				"reason '%s'",
				hole, startedHole ? "yes" : "no", herr.c_str());
			pc.stopEvents();
		}

		// --- a mark flags the angle being WATCHED ---------------------
		// Marking on angle 2 must produce an event enabled on angle 2 and
		// on nothing else. Deliberately not the first angle: an engine
		// that always flags camera 1 passes every check taken on camera 1,
		// and that is exactly what used to happen from a Stream Deck.
		{
			ReplayCore::instance().setCurrentAngle(secondCam);
			// The dock copies the angle from the core on its own tick.
			std::this_thread::sleep_for(
				std::chrono::milliseconds(200));
			const int before = store.lastEventId();
			runOnUi([&]() { markBtn->click(); });
			const int id2 = store.lastEventId();
			ReplayEvent ev2;
			if (id2 != before && store.get(id2, ev2)) {
				c.markInheritsAngle =
					ev2.angles[secondCam].enabled &&
					!ev2.angles[firstCam].enabled;
				obs_log(c.markInheritsAngle ? LOG_INFO : LOG_ERROR,
					"[selftest] dock: mark taken on angle %d — "
					"flagged %d:%s %d:%s",
					a2, a1,
					ev2.angles[firstCam].enabled ? "yes" : "no",
					a2,
					ev2.angles[secondCam].enabled ? "yes"
								      : "no");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] dock: the mark button created no "
					"event on angle %d", a2);
			}
			if (id2 != before)
				store.remove(id2);
			ReplayCore::instance().setCurrentAngle(firstCam);
			std::this_thread::sleep_for(
				std::chrono::milliseconds(200));
		}
	}

	// --- scrubbing shows the FOOTAGE, and keeps showing it ----------------
	// Dragging the seekbar is "review from here". The dock used to hand the
	// preview back to the live camera the moment that review ran out, so a
	// position chosen by hand in the recorded timeline ended up displaying the
	// camera as it is now — presented as the footage of then.
	{
		auto &chan = ReplayChannel::instance();
		SeekBar *bar = nullptr;
		runOnUi([&]() {
			// value() rather than a loop that breaks on the first
			// item: clang rejects that as an unreachable loop
			// increment under -Werror, and the dock has exactly one
			// seekbar anyway.
			bar = dock->findChild<SeekBar *>();
		});
		if (!bar) {
			obs_log(LOG_ERROR, "[selftest] dock: no seekbar found");
		} else {
			// Near the live edge, i.e. inside the ring — the part of the
			// timeline the gate can guarantee is servable.
			runOnUi([&]() { emit bar->seekRequested(0.90); });
			bool played = false;
			for (int i = 0; i < 60 && !played; i++) {
				played = chan.playing() ||
					 chan.stats().framesPushed > 0;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			// End the review and let the dock tick a few times: THIS is
			// where it used to jump back to the camera.
			chan.stop();
			std::this_thread::sleep_for(
				std::chrono::milliseconds(400));
			const bool stillFootage =
				dock->previewShowsReplay() &&
				!ReplayCore::instance().followLive();
			c.scrubShowsFootage = played && stillFootage;
			obs_log(c.scrubShowsFootage ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: scrub review played=%s, preview "
				"still on the footage afterwards=%s",
				played ? "yes" : "NO",
				stillFootage ? "yes" : "NO");
		}
	}

	// Leave the operator's own project exactly as it was found.
	if (evId > 0)
		store.remove(evId);
	store.setSessionFolder(ReplayCore::instance().recordingFolder());
	return c;
}

void runSelfTest()
{
	const int durationSecs = envInt("OBS_MULTIREPLAY_SELFTEST_SECS", 25);
	const std::string sourcesCsv = envStr("OBS_MULTIREPLAY_SELFTEST_SOURCES");
	const bool useRealSources = !sourcesCsv.empty();
	std::vector<std::string> realNames = splitCsv(sourcesCsv);

	int camCount = useRealSources
			       ? (int)realNames.size()
			       : envInt("OBS_MULTIREPLAY_SELFTEST_CAMS", 2);
	camCount = std::max(1, std::min(camCount, kMaxCameras));

	std::string outPath = envStr("OBS_MULTIREPLAY_SELFTEST_OUT");
	if (outPath.empty()) {
		std::error_code ec;
		outPath = (std::filesystem::temp_directory_path(ec) /
			   "obs-multireplay-selftest.json")
				  .string();
	}

	obs_log(LOG_INFO,
		"[selftest] START — cams=%d duration=%ds sources=%s out=%s",
		camCount, durationSecs,
		useRealSources ? sourcesCsv.c_str() : "(synthetic)",
		outPath.c_str());

	// Let OBS finish settling (scene collection, devices, encoders).
	std::this_thread::sleep_for(std::chrono::seconds(3));

	struct obs_video_info ovi = {};
	const bool haveOvi = obs_get_video_info(&ovi);
	const uint32_t cx = haveOvi ? ovi.base_width : 1920;
	const uint32_t cy = haveOvi ? ovi.base_height : 1080;
	const double canvasFps =
		haveOvi && ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 30.0;

	// A throwaway Config: never touches the operator's persisted settings.
	Config cfg;
	std::error_code ec;
	const std::filesystem::path folder =
		std::filesystem::temp_directory_path(ec) / "obs-multireplay-selftest";
	// Start from an empty folder: leftovers from earlier runs are recordings
	// whose openings are long gone from the ring, so they can never be
	// anchored and would drown the check in false negatives.
	std::filesystem::remove_all(folder, ec);
	std::filesystem::create_directories(folder, ec);
	cfg.sessionFolder = folder.string();
	cfg.splitMinutes = 20;

	// --- Acquire the cameras and arm Branch Output ------------------------
	// All of this runs on the UI thread (see runOnUi): a Branch Output filter
	// created off it never starts recording.
	std::vector<obs_source_t *> cams(camCount, nullptr);
	std::vector<bool> owned(camCount, false);
	obs_scene_t *testScene = nullptr;
	std::array<bool, kMaxTapChannels> want{};
	int armed = 0;

	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (useRealSources) {
				cams[i] = obs_get_source_by_name(
					realNames[i].c_str());
				if (!cams[i])
					obs_log(LOG_ERROR,
						"[selftest] source '%s' not found",
						realNames[i].c_str());
			} else {
				cams[i] = createSyntheticCamera(i, cx, cy);
				owned[i] = cams[i] != nullptr;
				if (cams[i]) {
					obs_source_inc_showing(cams[i]);
					obs_source_inc_active(cams[i]);
				}
			}
		}

		// Branch Output ignores a filter whose parent is not part of the
		// frontend scene collection (sourceInFrontend()), so the synthetic
		// cameras need a scene to live in. It does NOT have to be the
		// program scene — Branch Output renders through its own obs_view —
		// so we never touch what the operator has on air.
		if (!useRealSources) {
			testScene = obs_scene_create("MRSelfTest Scene");
			if (testScene) {
				for (int i = 0; i < camCount; i++)
					if (cams[i])
						obs_scene_add(testScene, cams[i]);
				obs_log(LOG_INFO,
					"[selftest] synthetic cameras placed in "
					"'MRSelfTest Scene'");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] could not create the test scene");
			}
		}

		for (int i = 0; i < camCount; i++) {
			if (!cams[i])
				continue;
			// Created, NOT armed: this is exactly what New Project /
			// Open Project / Settings do, and it must not record.
			obs_source_t *filter =
				branch_output::ensureFilter(cams[i], i, cfg);
			if (!filter) {
				obs_log(LOG_ERROR,
					"[selftest] cam%d: filter creation failed",
					i + 1);
				continue;
			}
			obs_source_release(filter);
			want[i] = true;
			armed++;
		}
	});

	// --- A configured filter must NOT be a recording filter ---------------
	// Branch Output re-evaluates its start conditions once a second, so give it
	// three ticks with the filters merely CONFIGURED and check that nothing is
	// writing. This is the regression that made creating a project start the
	// take: the filter was born enabled, BO obliged, and the recording began
	// before the tap existed to anchor it against.
	bool filtersIdleOutsideRec = armed > 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(3000));
	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (!want[i] || !cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			bool enabled = true;
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				enabled = obs_source_enabled(f);
				obs_source_release(f);
			}
			const bool writing = branch_output::recordingOutputActive(i);
			if (enabled || writing) {
				filtersIdleOutsideRec = false;
				obs_log(LOG_ERROR,
					"[selftest] cam%d: filter armed itself outside "
					"REC (enabled=%s, recording output active=%s)",
					i + 1, enabled ? "yes" : "no",
					writing ? "yes" : "no");
			}
		}
	});
	obs_log(filtersIdleOutsideRec ? LOG_INFO : LOG_ERROR,
		"[selftest] filters idle while only configured: %s",
		filtersIdleOutsideRec ? "yes" : "NO");

	// Now arm them, which is what REC does and the only thing that may.
	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (!want[i] || !cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				branch_output::setEnabled(f, true);
				obs_source_release(f);
			}
		}
	});
	obs_log(LOG_INFO, "[selftest] armed %d Branch Output filter(s)", armed);

	// Watch the recording folder so the gate also exercises file anchoring.
	{
		std::array<bool, kMaxSegmentCameras> segCams{};
		for (int i = 0; i < camCount && i < kMaxSegmentCameras; i++)
			segCams[i] = want[i];
		const int64_t epochWallNs =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch())
				.count();
		SegmentIndex::instance().start(cfg.sessionFolder, segCams,
					       (int64_t)os_gettime_ns(), epochWallNs);
	}

	// Keep the synthetic cameras moving. A flat colour compresses to nearly
	// identical frames, which is exactly the case segment anchoring refuses
	// as ambiguous - so without motion the gate could never exercise it.
	std::atomic<bool> animate{!useRealSources};
	std::thread animator;
	if (animate.load()) {
		animator = std::thread([&]() {
			int tick = 0;
			while (animate.load()) {
				for (int i = 0; i < camCount; i++) {
					if (!cams[i])
						continue;
					obs_data_t *s = obs_data_create();
					const uint32_t c =
						0xFF000000u |
						(uint32_t)((tick * 2654435761u +
							    (uint32_t)i * 40503u) &
							   0x00FFFFFFu);
					obs_data_set_int(s, "color", c);
					obs_data_set_int(s, "width", cx);
					obs_data_set_int(s, "height", cy);
					obs_source_update(cams[i], s);
					obs_data_release(s);
				}
				tick++;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(66));
			}
		});
	}

	const uint32_t laggedBefore = obs_get_lagged_frames();
	const uint32_t totalBefore = obs_get_total_frames();

	RingBudget budget;
	budget.kbpsPerCamera = cfg.videoBitrateKbps + cfg.audioBitrateKbps;
	PacketTap::instance().armAsync(want, budget);

	// --- Measure ---------------------------------------------------------
	for (int s = 0; s < durationSecs; s++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (s == 5 || s == durationSecs - 1)
			obs_log(LOG_INFO, "%s",
				PacketTap::instance().report().c_str());
	}

	animate.store(false);
	if (animator.joinable())
		animator.join();

	const int segmentsAnchored = SegmentIndex::instance().anchoredCount();
	const int segmentsUnanchored = SegmentIndex::instance().unanchoredCount();
	// Every recording file that appeared must have been placed on the
	// timeline exactly; anything left unanchored is footage we would refuse
	// to play rather than guess the position of.
	const bool segmentsOk =
		segmentsAnchored >= armed && segmentsUnanchored == 0;
	obs_log(LOG_INFO, "[selftest] segments anchored=%d unanchored=%d",
		segmentsAnchored, segmentsUnanchored);

	// --- The anchors must be ON DISK before the take ends -----------------
	// anchors.json is the only evidence a later run has: a file whose opening
	// has left the ring can never be re-anchored. It used to be written only
	// by stop(), so a take that ended with OBS killed or crashed left its
	// recordings with no anchor at all - which is why the operator's 18:27
	// take (183 MB per camera) can never be replayed. Read the real file,
	// mid-take, and count what is in it.
	int anchorsOnDisk = 0;
	{
		const std::string ap =
			(std::filesystem::path(cfg.sessionFolder) / "anchors.json")
				.string();
		obs_data_t *root = obs_data_create_from_json_file(ap.c_str());
		if (root) {
			obs_data_array_t *arr = obs_data_get_array(root, "segments");
			if (arr) {
				anchorsOnDisk = (int)obs_data_array_count(arr);
				obs_data_array_release(arr);
			}
			obs_data_release(root);
		}
	}
	const bool anchorsPersisted = anchorsOnDisk >= armed;
	obs_log(anchorsPersisted ? LOG_INFO : LOG_ERROR,
		"[selftest] anchors.json holds %d segment(s) mid-take (%d armed) - "
		"this is what a project reopened tomorrow reads",
		anchorsOnDisk, armed);

	// --- Event timecodes have an origin that exists -----------------------
	// A mark is an absolute instant on a clock that started with OBS; it only
	// becomes a timecode relative to where the project's footage begins. That
	// origin must come from the anchored recordings (every camera, not the
	// selected one), or a reopened project prints marks as raw monotonic time
	// - five-digit minute counts, which is exactly what the operator saw.
	const int64_t projectOrigin = SegmentIndex::instance().projectOriginNs();
	const bool projectOriginOk = projectOrigin > 0;
	obs_log(projectOriginOk ? LOG_INFO : LOG_ERROR,
		"[selftest] project origin (oldest anchor on any camera) = %lld ms",
		(long long)(projectOrigin / 1000000));

	const uint32_t laggedAfter = obs_get_lagged_frames();
	const uint32_t totalAfter = obs_get_total_frames();
	const int64_t skewNs = PacketTap::instance().crossAngleSkewNs();
	const int attached = PacketTap::instance().attachedCount();

	std::vector<TapStats> stats;
	for (int i = 0; i < camCount; i++)
		stats.push_back(PacketTap::instance().stats(i));

	// --- M1 acceptance: the exact use case that was broken ----------------
	// Take the last 5 seconds off the live ring on every angle, with no file
	// access at all, and check that one marker resolves to the same instant
	// everywhere. This is "mark it and put it on air now" in miniature.
	const double frameMs = 1000.0 / (canvasFps > 0 ? canvasFps : 30.0);
	bool ringLast5s = armed > 0;
	bool decodeOk = armed > 0;
	bool startsOnMarkedFrame = armed > 0;
	int64_t crossAnglePresentDeltaMs = 0;
	std::vector<int64_t> presentIns;
	std::vector<int> decodedFrames;
	int64_t clipInNs = 0, clipOutNs = 0; // reused by the playback check below
	{
		auto &tap = PacketTap::instance();
		int64_t commonEdge = INT64_MAX;
		for (int i = 0; i < camCount; i++) {
			if (!want[i])
				continue;
			const int64_t n = tap.newestNs(i);
			if (n <= 0) {
				commonEdge = 0;
				break;
			}
			commonEdge = std::min(commonEdge, n);
		}

		if (commonEdge <= 0 || commonEdge == INT64_MAX) {
			ringLast5s = false;
			obs_log(LOG_ERROR, "[selftest] no live edge on the rings");
		} else {
			// Stay a couple of frames inside the edge: the newest
			// packet held may be audio, slightly ahead of the newest
			// decodable video frame.
			const int64_t marginNs =
				(int64_t)(2.0 * 1e9 / (canvasFps > 0 ? canvasFps : 30.0));
			const int64_t outNs = commonEdge - marginNs;
			const int64_t inNs = outNs - 5'000'000'000LL;
			clipInNs = inNs;
			clipOutNs = outNs;

			int64_t lo = INT64_MAX, hi = INT64_MIN;
			for (int i = 0; i < camCount; i++) {
				if (!want[i])
					continue;
				std::vector<LivePacket> clip;
				int64_t pIn = 0, pOut = 0;
				if (!tap.resolveRange(i, inNs, outNs, clip, pIn,
						      pOut)) {
					obs_log(LOG_ERROR,
						"[selftest] cam%d: the last 5 s did "
						"NOT resolve from the ring",
						i + 1);
					ringLast5s = false;
					continue;
				}
				presentIns.push_back(pIn);
				lo = std::min(lo, pIn);
				hi = std::max(hi, pIn);
				obs_log(LOG_INFO,
					"[selftest] cam%d: last 5 s resolved from the "
					"ring - %zu packets, in=%lld ms out=%lld ms",
					i + 1, clip.size(),
					(long long)(pIn / 1000000),
					(long long)(pOut / 1000000));

				// Decode it. Frames before the IN are decoded and
				// dropped on purpose: they only exist to build up
				// reference state from the keyframe, and dropping
				// them is what makes the clip start on the marked
				// frame instead of snapping back to the keyframe.
				ReplayDecoder dec;
				std::string derr;
				if (!dec.open(tap.streamConfig(i), derr)) {
					obs_log(LOG_ERROR,
						"[selftest] cam%d: decoder open failed: %s",
						i + 1, derr.c_str());
					decodeOk = false;
					continue;
				}

				int presented = 0, preroll = 0;
				int64_t firstPresentedNs = 0;
				ReplayDecoder::Frame f;
				const auto pull = [&]() {
					while (dec.receive(f)) {
						if (f.masterNs < pIn) {
							preroll++;
							continue;
						}
						if (presented == 0)
							firstPresentedNs = f.masterNs;
						presented++;
					}
				};
				bool sendOk = true;
				for (const auto &p : clip) {
					if (!dec.send(p, derr)) {
						obs_log(LOG_ERROR,
							"[selftest] cam%d: decode failed: %s",
							i + 1, derr.c_str());
						sendOk = false;
						break;
					}
					pull();
				}
				if (sendOk && dec.drain(derr))
					pull();

				decodedFrames.push_back(presented);
				// The whole point: the first frame we would put on
				// air is the frame that was marked, to the exact
				// timestamp - not the keyframe before it.
				if (presented == 0 || firstPresentedNs != pIn)
					startsOnMarkedFrame = false;

				const int expected =
					(int)(5.0 * (canvasFps > 0 ? canvasFps : 30.0));
				if (!sendOk || presented < expected * 9 / 10)
					decodeOk = false;

				obs_log(LOG_INFO,
					"[selftest] cam%d: decoded %d frames (%d dropped "
					"as pre-roll), first presented=%lld ms, "
					"requested in=%lld ms, %ux%u",
					i + 1, presented, preroll,
					(long long)(firstPresentedNs / 1000000),
					(long long)(pIn / 1000000), dec.width(),
					dec.height());
			}
			if (lo != INT64_MAX && hi != INT64_MIN)
				crossAnglePresentDeltaMs = (hi - lo) / 1000000;
		}
	}
	// One marker, one frame, every angle.
	const bool passRingCrossAngle =
		presentIns.size() < 2 ||
		(double)crossAnglePresentDeltaMs <= frameMs;

	// --- M2 acceptance: through the real OBS input, in slow motion --------
	// Play the same 5 seconds into "Replay A" at 50%. Two things get proven
	// at once: OBS accepts our frames (it reports the picture size back), and
	// the pacing is right (5 s of footage at half speed takes ~10 s).
	bool playsIntoObs = false;
	bool audioPlays = false;
	bool slowMotionPaced = false;
	bool filePlaysFromDisk = false;
	bool fileMatchesRing = false;
	int fileFrames = 0;
	int playedFrames = 0;
	int audioBuffers = 0;
	int64_t playElapsedMs = 0;
	int64_t slowElapsedMs = 0;
	if (ringLast5s && clipOutNs > clipInNs) {
		auto &chan = ReplayChannel::instance();
		int firstCam = -1;
		for (int i = 0; i < camCount; i++) {
			if (want[i]) {
				firstCam = i;
				break;
			}
		}

		obs_source_t *src = chan.acquireSource();
		if (firstCam < 0 || !src) {
			obs_log(LOG_ERROR,
				"[selftest] no replay source to play into");
		} else {
			// The source is in no scene, so OBS would not tick it and
			// would never consume the async frames we push.
			obs_source_inc_active(src);
			obs_source_inc_showing(src);

			// Pass 1 - the full five seconds at 1x, which is where
			// audio rides along.
			std::string perr;
			const uint64_t t0 = os_gettime_ns();
			if (!chan.play(firstCam, clipInNs, clipOutNs, 100, perr)) {
				obs_log(LOG_ERROR, "[selftest] play failed: %s",
					perr.c_str());
			} else {
				while (chan.playing())
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				playElapsedMs =
					(int64_t)((os_gettime_ns() - t0) / 1000000);

				const auto st = chan.stats();
				playedFrames = (int)st.framesPushed;
				audioBuffers = (int)st.audioPushed;
				const uint32_t w = obs_source_get_width(src);
				const uint32_t h = obs_source_get_height(src);

				// ~5 s expected; a wide band so a loaded laptop
				// cannot produce a false failure.
				const bool pacedRight = playElapsedMs > 4200 &&
							playElapsedMs < 7000;
				// Against the CAMERA's size, not the canvas. A
				// replay is the camera's own picture - OBS scales
				// it when it is composited - so a 720p source in a
				// 1080p project is correct, not a failure. The
				// synthetic cameras happen to be canvas-sized,
				// which is why this only showed up the moment the
				// gate was pointed at real footage.
				const StreamConfig scfg =
					PacketTap::instance().streamConfig(firstCam);
				playsIntoObs = st.lastRunCompleted &&
					       playedFrames > 0 &&
					       w == scfg.width &&
					       h == scfg.height && pacedRight;
				audioPlays = audioBuffers > 0;

				obs_log(LOG_INFO,
					"[selftest] played %d frames + %d audio buffers "
					"into '%s' at 1x in %lld ms (OBS reports %ux%u, "
					"%d preroll)",
					playedFrames, audioBuffers,
					ReplayChannel::sourceName(),
					(long long)playElapsedMs, w, h,
					(int)st.framesPreroll);
			}

			// Pass 2 - two seconds at 50%, purely to prove the
			// slow-motion cadence is the speed we asked for.
			const int64_t slowInNs = clipOutNs - 2'000'000'000LL;
			const uint64_t t1 = os_gettime_ns();
			if (chan.play(firstCam, slowInNs, clipOutNs, 50, perr)) {
				while (chan.playing())
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				slowElapsedMs =
					(int64_t)((os_gettime_ns() - t1) / 1000000);
				slowMotionPaced = slowElapsedMs > 3400 &&
						  slowElapsedMs < 5200;
				obs_log(LOG_INFO,
					"[selftest] 2 s of footage at 50%% took %lld ms "
					"(%d frames)",
					(long long)slowElapsedMs,
					(int)chan.stats().framesPushed);
			} else {
				obs_log(LOG_ERROR,
					"[selftest] slow-motion pass failed: %s",
					perr.c_str());
			}

			// Pass 3 - the same footage again, but read from the
			// recording files instead of the ring. Two independent
			// paths must land on the same frame, or one of them is
			// lying. The window sits well back from the live edge so
			// it is certainly flushed to disk.
			const int64_t fileInNs = clipOutNs - 15'000'000'000LL;
			const int64_t fileOutNs = fileInNs + 5'000'000'000LL;

			std::vector<LivePacket> refClip;
			int64_t refIn = 0, refOut = 0;
			const bool haveRef = PacketTap::instance().resolveRange(
				firstCam, fileInNs, fileOutNs, refClip, refIn,
				refOut);

			if (chan.play(firstCam, fileInNs, fileOutNs, 100, perr,
				      ReplayChannel::Source::Segments)) {
				while (chan.playing())
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				const auto st = chan.stats();
				fileFrames = (int)st.framesPushed;
				const int expected =
					(int)(5.0 * (canvasFps > 0 ? canvasFps : 30.0));
				filePlaysFromDisk =
					st.lastRunCompleted &&
					fileFrames >= expected * 9 / 10;
				// The decisive comparison: the file path must
				// present the very frame the ring would have.
				// Within one frame, not to the nanosecond. The
				// reference is resolved a moment before playback
				// starts, and the live edge keeps advancing in
				// between, so demanding exact equality made this
				// fail roughly one run in three on footage that
				// was in fact identical. One frame still catches
				// a genuinely misplaced anchor, which is what
				// this is here to catch - those are seconds out,
				// not milliseconds.
				const int64_t frameNs =
					(int64_t)(1e9 / (canvasFps > 0 ? canvasFps
								       : 30.0));
				const int64_t delta = st.firstFrameNs > refIn
							      ? st.firstFrameNs - refIn
							      : refIn - st.firstFrameNs;
				fileMatchesRing = haveRef && delta <= frameNs;
				obs_log(LOG_INFO,
					"[selftest] from disk: %d frames, first=%lld ms; "
					"ring would present %lld ms (match: %s)",
					fileFrames,
					(long long)(st.firstFrameNs / 1000000),
					(long long)(refIn / 1000000),
					fileMatchesRing ? "yes" : "NO");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] file playback failed: %s",
					perr.c_str());
			}

			obs_source_dec_showing(src);
			obs_source_dec_active(src);
		}
		if (src)
			obs_source_release(src);
	}

	// --- The dock, on the same live ring ---------------------------------
	// Deliberately here, while everything is still armed and the ring is
	// full: the dock is only interesting against a running session, which is
	// exactly the state an operator has when he touches it.
	DockChecks dockChecks;
	{
		int firstArmed = -1, secondArmed = -1;
		for (int i = 0; i < camCount; i++) {
			if (!want[i])
				continue;
			if (firstArmed < 0)
				firstArmed = i;
			else if (secondArmed < 0)
				secondArmed = i;
		}
		if (firstArmed >= 0)
			dockChecks = runDockChecks(firstArmed, secondArmed,
						   cfg.sessionFolder, canvasFps);
		else
			obs_log(LOG_ERROR, "[selftest] no armed camera — dock "
					   "checks skipped");
	}

	// The mark the dock just took, expressed the way the event table expresses
	// it: after the project's footage begins, and inside this take. A mark that
	// lands before the origin (or hours after it) is the "absurd timecode" a
	// reopened project showed, caught here instead of by eye.
	const int64_t markOffsetNs =
		projectOriginOk ? dockChecks.markInNs - projectOrigin : -1;
	const bool eventTimecodeSane =
		projectOriginOk && dockChecks.markInNs > 0 && markOffsetNs > 0 &&
		markOffsetNs < (int64_t)(durationSecs + 120) * 1000000000LL;
	obs_log(eventTimecodeSane ? LOG_INFO : LOG_ERROR,
		"[selftest] marked event sits %lld ms after the project origin",
		(long long)(markOffsetNs / 1000000));

	// --- Tear down in the order the lifecycle demands ---------------------
	// Detach BEFORE disabling the filters: Branch Output frees its encoder
	// in releaseInfrastructureIfIdle() once its own outputs go idle.
	PacketTap::instance().detachAll();
	SegmentIndex::instance().stop();

	// Tearing down a Branch Output filter destroys its QTimer, so this goes
	// back on the UI thread too.
	runOnUi([&]() {
		if (testScene) {
			obs_source_remove(obs_scene_get_source(testScene));
			obs_scene_release(testScene);
		}

		for (int i = 0; i < camCount; i++) {
			if (!cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				branch_output::setEnabled(f, false);
				obs_source_filter_remove(cams[i], f);
				obs_source_release(f);
			}
			if (owned[i]) {
				obs_source_dec_active(cams[i]);
				obs_source_dec_showing(cams[i]);
				obs_source_remove(cams[i]);
			}
			obs_source_release(cams[i]);
		}
	});

	// --- Verdict ----------------------------------------------------------
	bool passAttached = attached == armed && armed > 0;
	bool passNoNewEncoder = armed > 0;
	bool passPackets = armed > 0;
	bool passClean = true;
	int64_t worstMaxAgeMs = 0;
	for (const auto &s : stats) {
		if (!want[s.camIndex])
			continue;
		if (!s.encoderWasAlreadyActive)
			passNoNewEncoder = false;
		if (s.videoPackets == 0)
			passPackets = false;
		// A packet we could not place on the shared clock, or an
		// unexplained break in a healthy session, means the timeline is
		// not trustworthy - which is the whole point of the rewrite.
		if (s.malformedPackets > 0 || s.discontinuities > 0)
			passClean = false;
		worstMaxAgeMs = std::max(worstMaxAgeMs, s.maxAgeUsec / 1000);
	}
	// Live edge must beat the ~1 s fragment flush by a wide margin; we allow
	// 250 ms before calling it a failure, and report the real number anyway.
	// What this must prove is that the live edge no longer waits on a
	// container flush - that was ~1 s, plus a reopen. 400 ms still settles
	// that decisively, while 250 ms was really measuring how busy the
	// machine happened to be: a loaded laptop pushes QSV to ~365 ms on
	// footage that is otherwise perfect, and a gate that cries wolf is a
	// gate people stop reading.
	const bool passLatency = passPackets && worstMaxAgeMs <= 400;
	// Sampling is asynchronous, so allow two frame times of apparent skew.
	const bool passSkew = attached < 2 ||
			      (double)(skewNs / 1000000) <= frameMs * 2.0;
	const uint32_t laggedDelta = laggedAfter - laggedBefore;
	const uint32_t totalDelta = totalAfter - totalBefore;
	const double laggedPct =
		totalDelta ? 100.0 * laggedDelta / totalDelta : 0.0;
	const bool passImpact = laggedPct <= 1.0;

	const bool pass = passAttached && passNoNewEncoder && passPackets &&
			  passLatency && passSkew && passImpact && passClean &&
			  ringLast5s && passRingCrossAngle && decodeOk &&
			  startsOnMarkedFrame && playsIntoObs && audioPlays &&
			  slowMotionPaced && segmentsOk && filePlaysFromDisk &&
			  fileMatchesRing && dockChecks.found &&
			  dockChecks.pollRuns && dockChecks.pollResponsive &&
			  dockChecks.pollQuiet && dockChecks.markOnTimeline &&
			  dockChecks.playsMark && dockChecks.playheadInsideClip &&
			  dockChecks.multiAngleQueue &&
			  dockChecks.angleCombinations &&
			  dockChecks.angleChoiceRepeatable &&
			  dockChecks.singleNonFirstAnglePlays &&
			  dockChecks.queueAdvancesToSecond &&
			  dockChecks.previewHoldsSequence &&
			  dockChecks.followsLiveAfterSequence &&
			  dockChecks.scrubShowsFootage &&
			  dockChecks.singleHonoursRequestedAngle &&
			  dockChecks.singleReportsUnplayableAngle &&
			  dockChecks.markInheritsAngle &&
			  anchorsPersisted && projectOriginOk &&
			  eventTimecodeSane && filtersIdleOutsideRec;

	// --- Report -----------------------------------------------------------
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "verdict", pass ? "PASS" : "FAIL");
	obs_data_set_string(root, "plugin_version", PLUGIN_VERSION);
	obs_data_set_int(root, "cameras_armed", armed);
	obs_data_set_int(root, "cameras_attached", attached);
	obs_data_set_int(root, "duration_secs", durationSecs);
	obs_data_set_bool(root, "synthetic_sources", !useRealSources);
	obs_data_set_int(root, "canvas_width", cx);
	obs_data_set_int(root, "canvas_height", cy);
	obs_data_set_double(root, "canvas_fps", canvasFps);

	obs_data_t *checks = obs_data_create();
	// Configuring a camera is not arming it: only REC records.
	obs_data_set_bool(checks, "filters_idle_outside_rec", filtersIdleOutsideRec);
	obs_data_set_bool(checks, "all_channels_attached", passAttached);
	obs_data_set_bool(checks, "no_new_encoder_created", passNoNewEncoder);
	obs_data_set_bool(checks, "packets_received", passPackets);
	obs_data_set_bool(checks, "live_edge_latency_ok", passLatency);
	obs_data_set_bool(checks, "cross_angle_skew_ok", passSkew);
	obs_data_set_bool(checks, "obs_impact_ok", passImpact);
	obs_data_set_bool(checks, "timeline_clean", passClean);
	obs_data_set_bool(checks, "ring_serves_last_5s", ringLast5s);
	obs_data_set_bool(checks, "ring_cross_angle_same_frame", passRingCrossAngle);
	obs_data_set_bool(checks, "clip_decodes", decodeOk);
	obs_data_set_bool(checks, "clip_starts_on_marked_frame", startsOnMarkedFrame);
	obs_data_set_bool(checks, "plays_into_obs_source", playsIntoObs);
	obs_data_set_bool(checks, "audio_plays", audioPlays);
	obs_data_set_bool(checks, "slow_motion_paced", slowMotionPaced);
	// Every recording file that appeared must have been placed on the
	// timeline exactly; anything left unanchored is footage we would refuse
	// to play rather than guess the position of.
	obs_data_set_bool(checks, "segments_anchored", segmentsOk);
	// Written to anchors.json as they are established, not at STOP: a take
	// that ends with OBS killed still leaves replayable footage behind.
	obs_data_set_bool(checks, "anchors_persisted_during_take",
			  anchorsPersisted);
	// Event timecodes have an origin, and it is the project's footage.
	obs_data_set_bool(checks, "project_origin_from_anchors", projectOriginOk);
	obs_data_set_bool(checks, "event_timecode_sane", eventTimecodeSane);
	obs_data_set_bool(checks, "plays_from_disk", filePlaysFromDisk);
	obs_data_set_bool(checks, "disk_matches_ring", fileMatchesRing);
	obs_data_set_int(root, "disk_played_frames", fileFrames);
	// The dock: found and clicked, not just compiled (see runDockChecks).
	obs_data_set_bool(checks, "dock_registered", dockChecks.found);
	obs_data_set_bool(checks, "dock_poll_runs", dockChecks.pollRuns);
	obs_data_set_bool(checks, "dock_poll_responsive",
			  dockChecks.pollResponsive);
	obs_data_set_bool(checks, "dock_poll_no_errors", dockChecks.pollQuiet);
	obs_data_set_bool(checks, "dock_mark_on_timeline",
			  dockChecks.markOnTimeline);
	obs_data_set_bool(checks, "dock_plays_marked_event",
			  dockChecks.playsMark);
	obs_data_set_bool(checks, "dock_playhead_inside_clip",
			  dockChecks.playheadInsideClip);
	// One clip per enabled angle: a two-angle event is two clips, not one.
	obs_data_set_bool(checks, "dock_multi_angle_queue",
			  dockChecks.multiAngleQueue);
	// ...and the exact angle sequence, for every combination that matters:
	// both, a set with a hole in it, and one angle that is not the first.
	obs_data_set_bool(checks, "dock_angle_combinations",
			  dockChecks.angleCombinations);
	// The same event, replayed with a different choice each time.
	obs_data_set_bool(checks, "dock_angle_choice_repeatable",
			  dockChecks.angleChoiceRepeatable);
	obs_data_set_bool(checks, "dock_single_non_first_angle_plays",
			  dockChecks.singleNonFirstAnglePlays);
	// Clip 1 ends and clip 2 goes on air by itself, across the finish
	// callback and the generation filter.
	obs_data_set_bool(checks, "dock_queue_advances_to_second_angle",
			  dockChecks.queueAdvancesToSecond);
	// The preview belongs to the sequence: no live-camera flash between angles.
	obs_data_set_bool(checks, "dock_preview_holds_sequence",
			  dockChecks.previewHoldsSequence);
	// ...and the transport goes back to the live edge on its own afterwards.
	obs_data_set_bool(checks, "dock_follows_live_after_sequence",
			  dockChecks.followsLiveAfterSequence);
	// A scrub shows the footage at that instant, and still does once the
	// review has run out.
	obs_data_set_bool(checks, "dock_scrub_shows_footage",
			  dockChecks.scrubShowsFootage);
	// The angle button wins, and an angle with nothing behind it says so.
	obs_data_set_bool(checks, "single_mode_honours_requested_angle",
			  dockChecks.singleHonoursRequestedAngle);
	obs_data_set_bool(checks, "single_mode_reports_unplayable_angle",
			  dockChecks.singleReportsUnplayableAngle);
	// A mark flags the angle the operator is watching, not always the first.
	obs_data_set_bool(checks, "dock_mark_inherits_current_angle",
			  dockChecks.markInheritsAngle);
	obs_data_set_obj(root, "checks", checks);
	obs_data_release(checks);

	obs_data_set_int(root, "segments_anchored", segmentsAnchored);
	obs_data_set_int(root, "anchors_on_disk_mid_take", anchorsOnDisk);
	obs_data_set_int(root, "project_origin_ms", projectOrigin / 1000000);
	obs_data_set_int(root, "mark_offset_from_origin_ms",
			 markOffsetNs / 1000000);
	obs_data_set_int(root, "segments_unanchored", segmentsUnanchored);
	obs_data_set_int(root, "played_frames", playedFrames);
	obs_data_set_int(root, "played_audio_buffers", audioBuffers);
	obs_data_set_int(root, "played_elapsed_ms_at_1x", playElapsedMs);
	obs_data_set_int(root, "slow_2s_elapsed_ms_at_50pct", slowElapsedMs);

	obs_data_array_t *decArr = obs_data_array_create();
	for (int n : decodedFrames) {
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "frames", n);
		obs_data_array_push_back(decArr, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "decoded_frames_per_angle", decArr);
	obs_data_array_release(decArr);

	obs_data_set_int(root, "ring_cross_angle_present_delta_ms",
			 crossAnglePresentDeltaMs);

	obs_data_set_int(root, "dock_poll_ticks_in_2s", dockChecks.ticks);
	obs_data_set_int(root, "dock_worst_poll_gap_ms", dockChecks.worstGapMs);
	obs_data_set_int(root, "dock_plugin_errors_logged", dockChecks.logErrors);
	obs_data_set_int(root, "dock_marked_in_ms", dockChecks.markInNs / 1000000);
	obs_data_set_int(root, "dock_marked_out_ms",
			 dockChecks.markOutNs / 1000000);
	obs_data_set_int(root, "dock_played_frames", dockChecks.playedFrames);
	obs_data_set_int(root, "dock_two_angle_clips_queued",
			 dockChecks.queuedClips);
	obs_data_set_int(root, "dock_preview_live_samples_during_sequence",
			 dockChecks.previewLiveSamples);

	obs_data_set_int(root, "worst_max_packet_age_ms", worstMaxAgeMs);
	obs_data_set_int(root, "cross_angle_skew_ms", skewNs / 1000000);
	obs_data_set_int(root, "obs_lagged_frames_delta", laggedDelta);
	obs_data_set_int(root, "obs_total_frames_delta", totalDelta);
	obs_data_set_double(root, "obs_lagged_pct", laggedPct);

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &s : stats) {
		if (!want[s.camIndex])
			continue;
		obs_data_t *c = obs_data_create();
		obs_data_set_int(c, "cam", s.camIndex + 1);
		obs_data_set_string(c, "bo_output_name", s.outputName.c_str());
		obs_data_set_string(c, "encoder_id", s.encoderId.c_str());
		obs_data_set_string(c, "encoder_name", s.encoderName.c_str());
		obs_data_set_bool(c, "encoder_was_already_active",
				  s.encoderWasAlreadyActive);
		obs_data_set_int(c, "video_packets", (long long)s.videoPackets);
		obs_data_set_int(c, "keyframes", (long long)s.keyframes);
		obs_data_set_int(c, "audio_packets", (long long)s.audioPackets);
		obs_data_set_int(c, "video_bytes", (long long)s.videoBytes);
		obs_data_set_int(c, "first_packet_wait_ms", s.firstPacketWaitMs);
		obs_data_set_int(c, "packet_age_avg_ms", s.avgAgeUsec / 1000);
		obs_data_set_int(c, "packet_age_max_ms", s.maxAgeUsec / 1000);
		obs_data_set_int(c, "ring_packets", (long long)s.ringPackets);
		obs_data_set_int(c, "ring_mb", (long long)(s.ringBytes / (1024 * 1024)));
		obs_data_set_int(c, "ring_span_ms", s.ringSpanNs / 1000000);
		obs_data_set_int(c, "ring_evicted", (long long)s.evictedPackets);
		obs_data_set_int(c, "malformed_packets", (long long)s.malformedPackets);
		obs_data_set_int(c, "discontinuities", (long long)s.discontinuities);
		obs_data_array_push_back(arr, c);
		obs_data_release(c);
	}
	obs_data_set_array(root, "channels", arr);
	obs_data_array_release(arr);

	if (!obs_data_save_json_safe(root, outPath.c_str(), "tmp", "bak"))
		obs_log(LOG_ERROR, "[selftest] could not write report to %s",
			outPath.c_str());
	obs_log(LOG_INFO, "[selftest] VERDICT=%s — report written to %s",
		pass ? "PASS" : "FAIL", outPath.c_str());
	obs_data_release(root);

	// There is no public obs_frontend quit API, so the runner script closes
	// OBS once this report file appears. Writing it is the "done" signal.
}

} // namespace

void maybeRunSelfTest()
{
	if (!envOn("OBS_MULTIREPLAY_SELFTEST"))
		return;
	if (g_started.exchange(true))
		return; // FINISHED_LOADING can fire more than once
	std::thread(runSelfTest).detach();
}

} // namespace multireplay
