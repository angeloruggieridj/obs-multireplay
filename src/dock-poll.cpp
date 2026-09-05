/*
obs-multireplay — MultiReplayDock: the poll loop and the periodic refreshes
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Split out of multireplay-dock.cpp (pure move, no behaviour change): the 30 Hz
poll() tick and everything it drives directly -- the green channel strip,
seeking, the dead-recording watchdog, the UI-thread accounting, and the
angle/event/list-name refreshes -- used to sit in the same 10k+ line file as
the widget assembly and the Settings dialog. Splitting them into their own
translation units keeps each concern reviewable on its own.
*/

#include "multireplay-dock.hpp"
#include "angle-channels.hpp"
#include "dock-internal.hpp"
#include "dock-layout.hpp"
#include "error-locale.hpp"
#include "dock-style.hpp"
#include "dock-assets.hpp"
#include "dock-icons.hpp"
#include "qt-display.hpp"
#include "branch-output-install.hpp"
#include "camera-dedup.hpp"
#include "replay-core.hpp"
#include "updater.hpp"
#include "event-store.hpp"
#include "health.hpp"
#include "packet-tap.hpp"
#include "playback-coordinator.hpp"
#include "export.hpp"
#include "replay-channel.hpp"
#include "segment-index.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h> // obs_frontend_get_scenes (output-scene picker)
#include <util/platform.h>    // os_gettime_ns (arm watchdog deadline)

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QFontInfo>
#include <QSplitterHandle>
#include <QAbstractButton>
#include <QDateTime>
#include <QPushButton>
#include <QTabBar>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QItemSelectionModel>
#include <QHeaderView>
#include <QButtonGroup>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QDir>
#include <QGroupBox>
#include <QFrame>
#include <QListWidget>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QMessageBox>
#include <QStandardPaths>
#include <QProgressBar>
#include <QDesktopServices>
#include <QUrl>
#include <QDockWidget>
#include <QSizePolicy>
#include <QStyle>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QInputDialog>
#include <QMenu>
#include <QAction>
#include <QClipboard>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdlib>
#include <cstring>

namespace multireplay {
// ---------------------------------------------------------------------------
// The green channel strip (the reference controller's information band under the A output)
// ---------------------------------------------------------------------------

void MultiReplayDock::updateChannelStrip()
{
	// Both are built before the first poll(), but showNotice() can be reached
	// from anywhere and a half-built dock must not be a crash.
	if (!events_)
		return;

	// mm:ss.cc — the reference controller's precision on this strip. Hundredths, because a
	// thousandth is a digit nobody reads while the clip is running.
	auto shortTc = [](int64_t ns) {
		if (ns < 0)
			ns = 0;
		const int64_t cs = ns / 10000000; // centiseconds
		return QString::asprintf("%02d:%02d.%02d", (int)(cs / 6000),
					 (int)((cs / 100) % 60), (int)(cs % 100));
	};
	auto signedTc = [&shortTc](int64_t ns) {
		return (ns < 0 ? QStringLiteral("-") : QStringLiteral("+")) +
		       shortTc(ns < 0 ? -ns : ns);
	};

	auto &store = EventStore::instance();
	const auto ps = pc().playState();

	// Which event this strip is about: the one on air, else the one selected,
	// else the last mark taken — the same order the transport keys use, so
	// the strip always describes what ▶ would play.
	int evId = ps.active ? ps.eventId : 0;
	if (evId <= 0) {
		const auto sel = selectedEventIds();
		evId = sel.empty() ? store.lastEventId() : sel.front();
	}
	ReplayEvent ev;
	const bool haveEv = evId > 0 && store.get(evId, ev);

	const int list = store.selectedList();
	const std::string listNm = store.listName(list);
	const QString listText = listNm.empty()
					 ? QString("%1 %2")
						   .arg(obs_module_text("Dock.List"))
						   .arg(list, 2, 10,
							QLatin1Char('0'))
					 : QString::fromStdString(listNm);

	const int idDigits =
		std::clamp(ReplayCore::instance().getConfig().eventIdDigits, 1, 8);

	// Where the playhead is INSIDE this clip. Clamped on purpose: parked at
	// the live edge (or at 0, with a project reopened and nothing recording)
	// the raw difference is the distance between two unrelated instants, and
	// it printed as "IN -5879:34.56" — a number with no meaning that makes the
	// whole strip look broken. Clamped, an un-cued clip reads exactly as the reference controller
	// shows a cued one: IN +00:00.00, OUT -<duration>, REM = duration.
	int64_t clipPos = playheadNs_;
	if (haveEv) {
		const int64_t hi =
			ev.tOutNs != kNoInstant ? ev.tOutNs : ev.tInNs;
		clipPos = std::clamp(playheadNs_, ev.tInNs, hi);
	}

	// ── ONE LINE, ON THE STATUS BAR ──────────────────────────────────────
	//
	// This was a three-line green band under the pictures — list / clip x of
	// y / remaining, then the event id with the two offsets, then timecode and
	// speed. Forty-four pixels of panel, and nearly all of it a second copy of
	// what the on-air band and the position bar were already saying, in a
	// second green that the eye had to tell apart from the first.
	//
	// What is left here is the one thing that is said NOWHERE else: the answer
	// to a key the operator just pressed. A notice OWNS the line while it
	// lasts, because a refusal nobody sees is a panel that ignored him.
	const bool reverseOnAir = ps.active && ps.reverse;
	QString line;
	// §7.3.8 — advance the mini-queue: the current message's window has
	// closed and something is waiting behind it. A queued notice gets the
	// shorter duration (kQueuedNoticeNs) — see showNotice's own note on why.
	if (!noticeQueue_.isEmpty() &&
	    (int64_t)os_gettime_ns() >= noticeUntilNs_) {
		noticeText_ = noticeQueue_.takeFirst();
		noticeUntilNs_ = (int64_t)os_gettime_ns() + kQueuedNoticeNs;
	}
	const bool notice = noticeUntilNs_ > 0 &&
			    (int64_t)os_gettime_ns() < noticeUntilNs_;
	if (notice) {
		line = noticeText_;
	}
	// AND NOTHING ELSE. The line used to carry the list, the event id, the
	// clip counter and the playhead when there was no notice - and every one
	// of those is said somewhere the eye is already going: the list and the
	// id on the on-air band right under it, the counter there too, the
	// playhead on the position bar in the panel's own timecode. Four values
	// repeated one line apart are four glances that answer nothing, and they
	// were what made a notice hard to notice.
	//
	// So the line is the notice's, and empty the rest of the time. That is
	// not waste: it is the difference between a message and a caption.
	if (statusNotice_) {
		statusNotice_->setText(line);
		statusNotice_->setProperty("notice", notice);
		if (statusNoticeLit_ != notice) {
			statusNoticeLit_ = notice;
			repolish(statusNotice_);
		}
		statusNotice_->setToolTip(notice ? noticeText_ : QString());
	}
	if (statusSpeed_)
		statusSpeed_->setText(
			QString::asprintf("%.2f\xc3\x97", speedPct_ / 100.0));
	// clipPos and signedTc are still what the on-air band below is built from.
	(void)clipPos;
	(void)signedTc;

	// --- the green band: the state of the ANGLE that is on air ------------
	// id · angle · time left · speed, with the fill as the progress through
	// that clip. The speed is the CLIP's (the angle's override when it has
	// one), not the slider's: what has to be readable there is the speed of
	// the picture in front of the operator.
	if (clipBar_) {
		const bool onAir = ps.active && ps.eventId > 0;
		const int barPct = onAir ? ps.speedPct
					 : (speedPct_ > 0 ? speedPct_ : 100);
		// --- the whole SEQUENCE, when there is more than one clip in it ---
		// The fill used to be the progress through the clip on air, so a
		// three-angle replay filled the band up and started again three
		// times: nothing on the panel said when the replay would be over,
		// which is the one thing the band is for. Now the fill spans the
		// queue, the joins between the clips are drawn on it, and the text
		// carries the sequence's own remaining time beside the clip's.
		// --- the join, and why the band used to spike ---------------------
		// See clipBarJoinPos_ in the header: between two clips of a sequence
		// the queue has already moved on while positionNs() still reports the
		// last frame of the clip that ended, which reads as "this clip is
		// 100% done" — and since the sequence fill is built from it, the band
		// jumped to full at every white join before falling back.
		if (!onAir) {
			clipBarQueuePos_ = 0;
			clipBarAtJoin_ = false;
		} else {
			if (ps.queuePos != clipBarQueuePos_) {
				// Not the first clip of the sequence: that one has no
				// previous clip to be showing the tail of.
				if (clipBarQueuePos_ != 0) {
					clipBarJoinPos_ = clipPos;
					clipBarAtJoin_ = true;
				}
				clipBarQueuePos_ = ps.queuePos;
			}
			if (clipBarAtJoin_ && clipPos != clipBarJoinPos_)
				clipBarAtJoin_ = false;
		}
		const bool atJoin = clipBarAtJoin_;

		std::vector<double> joins;
		int64_t seqTotalNs = 0, seqDoneNs = 0;
		if (onAir && ps.queued > 1 &&
		    (int)ps.queuedWallNs.size() == ps.queued) {
			for (int64_t d : ps.queuedWallNs)
				seqTotalNs += d;
			for (int i = 0; i < ps.queuePos - 1 &&
					i < (int)ps.queuedWallNs.size();
			     i++)
				seqDoneNs += ps.queuedWallNs[i];
			if (seqTotalNs > 0) {
				int64_t acc = 0;
				for (size_t i = 0; i + 1 < ps.queuedWallNs.size();
				     i++) {
					acc += ps.queuedWallNs[i];
					joins.push_back((double)acc /
							(double)seqTotalNs);
				}
			}
		}
		double frac = 0.0;
		QString text;
		if (haveEv && ev.tOutNs != kNoInstant &&
		    ev.tOutNs > ev.tInNs) {
			const int64_t dur = ev.tOutNs - ev.tInNs;
			// Backwards the fill DRAINS, which needs no special case:
			// the fill is the playhead's place inside the clip, and in
			// reverse that place walks back towards the IN. An emptying
			// band is what "this replay is running out" looks like from
			// across a gallery, in either direction.
			frac = atJoin ? 0.0
				      : (double)(clipPos - ev.tInNs) / (double)dur;
			// At a join the whole clip is still to come, whichever
			// direction it runs: the position we can see belongs to the
			// clip that just ended.
			const int64_t remNs =
				atJoin ? dur
				       : (reverseOnAir
						  ? (clipPos > ev.tInNs
							     ? clipPos - ev.tInNs
							     : 0)
						  : (ev.tOutNs > clipPos
							     ? ev.tOutNs - clipPos
							     : 0));
			// Remaining in WALL time: at 50% a 4 s clip has 8 s left,
			// and 8 s is how long the operator will be looking at it.
			// The ◀ is not decoration: backwards at 50% and forwards at
			// 50% are the same three numbers and not the same picture.
			// THE LIST, THEN THE EVENT. The band used to open with
			// "A<angle>", which on a rig worked from one camera is the
			// same two characters all match - and the angle is already
			// on the pictures, in colour, as the tally. Which LIST the
			// clip came from was said on the line above instead, where
			// it was one of four things competing for a glance.
			text = QString("%1   %2   %3   %4%5%")
				       .arg(listText)
				       .arg(evId, idDigits, 10, QLatin1Char('0'))
				       .arg(shortTc(remNs * 100 / (barPct > 0
									   ? barPct
									   : 100)))
				       .arg(reverseOnAir ? QStringLiteral("◀ ")
							 : QString())
				       .arg(barPct);
			if (onAir && ps.queued > 1) {
				text += QString("   %1/%2")
						.arg(ps.queuePos)
						.arg(ps.queued);
				// The whole sequence: how far through it the fill
				// is, and how much of it is left. Without the total
				// the operator can see the end of THIS angle coming
				// and has no idea whether two more follow it.
				if (seqTotalNs > 0) {
					const int64_t clipWall =
						remNs * 100 /
						(barPct > 0 ? barPct : 100);
					const int64_t doneInClip =
						ps.queuedWallNs[ps.queuePos - 1] >
								clipWall
							? ps.queuedWallNs[ps.queuePos -
									  1] -
								  clipWall
							: 0;
					const int64_t elapsed = seqDoneNs + doneInClip;
					frac = std::clamp((double)elapsed /
								  (double)seqTotalNs,
							  0.0, 1.0);
					text += QString("   Σ %1").arg(shortTc(
						seqTotalNs > elapsed
							? seqTotalNs - elapsed
							: 0));
				}
			}
		} else {
			text = obs_module_text("Dock.NoEvent");
		}
		clipBar_->setState(frac, text, onAir, joins);
	}
	// >> stays ENABLED, always. It used to follow ps.active, which is read
	// here at 30 Hz — so for the first frames of a sequence the key was still
	// disabled and a press landed on nothing. During a match that is a lost
	// press with no explanation, and "the button was grey for 40 ms" is not
	// something anybody can see. The handler already refuses honestly when
	// there is nothing queued (it says so on the strip and in the log), which
	// is the better place for that answer.
	//
	// It cost the gate three runs to find, because a disabled button swallows
	// click() in silence: the check saw a queue that never advanced and blamed
	// the coordinator.

	// --- the position bar: where the TIMELINE stands ----------------------
	// Not the clip state (that is the band above). Position and length of the
	// recorded timeline, which is the thing this bar actually controls.
	if (seek_) {
		// "How far in" means how much FOOTAGE is behind the playhead, so
		// the number on the bar and the length beside it are the same
		// kind of quantity even when the session has pauses in it.
		const int64_t rel =
			!timeline_.empty() && playheadNs_ != kNoInstant
				? timeline_.footageBefore(playheadNs_)
				: ((timelineStartNs_ != kNoInstant &&
				    playheadNs_ != kNoInstant &&
				    playheadNs_ > timelineStartNs_)
					   ? playheadNs_ - timelineStartNs_
					   : 0);
		seek_->setOverlayText(shortTc(rel) + "  /  " +
				      shortTc(displayDurNs_));
		// The bar needs the LENGTH, not just the fraction: the graduations
		// are computed from it, and a fraction cannot say whether the bar
		// spans forty seconds or two hours.
		//
		// ...and when there is no length, why. Two different nothings,
		// and they are no longer the same two: a reopened project now
		// HAS a timeline (the footage on disk), so the only way to reach
		// this branch with footage anchored is a recording whose own
		// length cannot be read back from it. That is rare, real, and
		// worth naming — the operator is looking at events that play
		// perfectly while the bar under them refuses to move.
		seek_->setTimeline(
			displayDurNs_,
			obs_module_text(timelineStartNs_ != kNoInstant
						? "Dock.SeekLengthUnknown"
						: "Dock.SeekNoTimeline"));
	}
}

void MultiReplayDock::seekToFraction(double frac)
{
	if (timelineStartNs_ == kNoInstant || displayDurNs_ <= 0)
		return;
	frac = std::clamp(frac, 0.0, 1.0);
	// Through the map: the bar's axis is footage, so 50% is halfway through
	// what was RECORDED, not halfway through the afternoon. Without it, a
	// session with a pause in it had a stretch of bar that resolved to an
	// instant no camera covers — a click that could only ever answer "no
	// footage here".
	const int64_t inNs = timeline_.empty()
				     ? timelineStartNs_ +
					       (int64_t)(frac * (double)displayDurNs_)
				     : timeline_.instantAt(frac);
	// FOOTAGE from here, not wall time from here — and cut at the seams.
	//
	// Stop after five minutes and start again: the bar draws the two takes
	// joined, but in master time they are minutes apart. Asking for one range
	// [inNs, inNs + 10 s] therefore asks for the gap as well, the gap is not
	// footage, and the engine refuses a range it cannot serve exactly — so the
	// review stopped dead where one file ended and the next began. Split into
	// per-span pieces (TimelineMap::spansFrom) it is two ranges the engine can
	// serve, and the queue chains them the way it chains the angles of an event.
	std::vector<std::pair<int64_t, int64_t>> ranges;
	if (timeline_.empty()) {
		const int64_t edge = timelineStartNs_ + displayDurNs_;
		const int64_t outNs = std::min(edge, inNs + kScrubReviewNs);
		if (outNs > inNs)
			ranges.push_back({inNs, outNs});
	} else {
		for (const TimelineSpan &s :
		     timeline_.spansFrom(inNs, kScrubReviewNs))
			ranges.push_back({s.startNs, s.endNs});
	}
	if (ranges.empty())
		return;

	// Scrubbing is "review from here" (see kScrubReviewNs): the engine has no
	// playhead to park, it plays ranges. Stop the queue first so its own
	// finish callback cannot cut in over the review clip.
	pc().stopEvents();
	// ...and consume that stop ourselves. poll() sends a finished SEQUENCE back
	// to the live edge, which is right when a replay ends and wrong here: it
	// would drag the operator off the very instant he just chose.
	prevSequenceActive_ = false;
	ReplayCore::instance().setFollowLive(false);
	// Where the timeline now stands, whatever the engine can serve: the bar
	// stays under the operator's finger instead of snapping back.
	playheadNs_ = inNs;

	// DID HE JUST LAND ON FOOTAGE NOBODY MARKED? Then that stretch is what
	// the play keys are about from here on: ▶ watches it off air, and Play
	// events puts it up. Landing inside the selected event instead changes
	// nothing — Play events still replays the event, which is what a scrub
	// through a marked action has always meant.
	if (playheadIsFreeFootage())
		freeReviewInNs_ = inNs;
	else
		clearFreeReview();

	std::string err;
	// Through the QUEUE, not straight at the channel: the queue is what plays
	// one range after another, so a review that crosses a seam carries on into
	// the next take instead of ending there. (stopEvents() above already killed
	// whatever was queued, so this replaces it rather than fighting it.)
	if (!pc().playRanges(ranges, currentAngle1() - 1, speedPct_,
			     /*toOutput*/ false, err)) {
		// Nothing covers that instant on this angle — the ring has evicted
		// it and no anchored file holds it. Saying so is the point: the
		// alternative was the preview quietly showing the live camera, which
		// reads as "this is what was recorded there" and is not.
		const int64_t relMs = (inNs - timelineStartNs_) / 1000000;
		showNotice(QString("%1 (cam %2 @ %3) — %4")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(currentAngle1())
				   .arg(formatTc(relMs * 1000000))
				   .arg(QString::fromStdString(err)));
		obs_log(LOG_WARNING,
			"[dock] no footage on angle %d at %lld ms into the "
			"timeline: %s",
			currentAngle1(), (long long)relMs, err.c_str());
	}
}

// ---------------------------------------------------------------------------
// The take that never started
// ---------------------------------------------------------------------------

void MultiReplayDock::cancelDeadRecording()
{
	auto &core = ReplayCore::instance();

	obs_log(LOG_ERROR,
		"REC armed the Branch Output filters but no recording output "
		"started within %lld s — cancelling the take. Branch Output's "
		"Interlock setting must be 'Always ON'.",
		(long long)(kArmWatchNs / 1'000'000'000LL));

	// Disarm through the same path as the STOP button: the filters go back
	// off, the tap stops retrying and the GUI stops claiming to record. This
	// runs on the GUI thread, so it must not wait on anything — it does not:
	// stopRecording() only flips filter enables and detaches the tap.
	core.stopRecording();

	// The message box is DEFERRED. Showing it from inside poll() would open a
	// nested event loop on top of a running poll tick, re-entering the whole
	// refresh (and its table rebuild) from within itself. Queued, it opens on
	// a clean stack, with `this` as context so a closed dock cancels it.
	QTimer::singleShot(0, this, [this]() {
		QMessageBox::warning(
			this, obs_module_text("Dock.RecNotStartedTitle"),
			obs_module_text("Dock.RecNotStarted"));
	});
}

// ---------------------------------------------------------------------------
// Periodic refresh
// ---------------------------------------------------------------------------


// Is a self-test driving this OBS? Both first-run dialogs are MODAL, and a
// modal dialog on the gate's OBS is not a slow run, it is a dead one: exec()
// parks the UI thread and every runOnUi() the checking thread posts after that
// waits forever. The take goes on recording, no report is ever written, and the
// script times out — which is exactly how a check that called playSelected()
// once cost minutes of a run (see the notes in selftest.cpp).
//
// Read from the environment rather than asked of the self-test module, because
// this must be true before anything in that module has run — poll() reaches
// here on the fifth second, and by then the harness has long since set it.
static bool selfTestIsDriving()
{
	const char *v = getenv("OBS_MULTIREPLAY_SELFTEST");
	return v && *v && std::strcmp(v, "0") != 0;
}
void MultiReplayDock::poll()
{
	// Taken FIRST and handed to accountUiTick last, so what is measured is the
	// whole tick and the gap since the previous one. See accountUiTick.
	const uint64_t tickStartNs = (uint64_t)os_gettime_ns();
	auto &core = ReplayCore::instance();
	auto &chan = this->chan();
	auto &tap = PacketTap::instance();

	// The preview's obs_display is bound to a native window handle, and OBS
	// re-parents its docks whenever the layout is restored, floated, tabbed or
	// re-docked — which destroys that handle. Qt does not reliably tell the
	// widget, so this is the only place that can notice a display left
	// presenting into a window that no longer exists. Two integer compares.
	// Every one, the multiview tiles included: they are re-parented by the
	// same dock moves. Two integer compares each, and a hidden tile
	// early-outs on isVisible().
	for (OBSQTDisplay *d : allDisplays())
		d->recheckWindow();


	// --- is Branch Output even installed? --------------------------------
	// Once per launch, and not on the first tick: OBS emits FINISHED_LOADING
	// from inside OBSBasic::OBSInit, which then spins a NESTED event loop of
	// its own (the YouTube dock) — and a modal dialog dropped into that is the
	// same hazard that crashes Branch Output's status dock. Four seconds of
	// polling is well past it, and past any module still registering.
	if (!branchOutputAsked_ && statusTick_ > 120 && !selfTestIsDriving()) {
		branchOutputAsked_ = true;
		if (!core.branchOutputAvailable())
			QTimer::singleShot(0, this, [this]() {
				promptForBranchOutput();
			});
	}
	// ...and, once that is settled, whether anything is configured at all.
	// AFTER Branch Output, deliberately: two modal dialogs racing each other
	// on a fresh machine is worse than either, and the recording layer is the
	// one that decides whether the rest is worth filling in.
	if (!setupAsked_ && branchOutputAsked_ && !modalOpen_ &&
	    statusTick_ > 150 && !selfTestIsDriving()) {
		setupAsked_ = true;
		if (needsSetup())
			QTimer::singleShot(0, this,
					   [this]() { runSetupWizard(); });
	}
	// The hotkeys change the angle without going through the dock.
	const int hotAngle1 = core.currentAngle() + 1;
	if (hotAngle1 >= 1 && hotAngle1 <= kNCams)
		angle1_[(int)activeChannel_] = hotAngle1;
	const int cam0 = currentAngle1() - 1;

	const bool rec = core.isRecording();
	const bool followLive = core.followLive();
	const bool playing = chan.playing();
	const auto playSt = pc().playState();
	const bool eventActive = playSt.active;

	// --- is the REPLAY on screen? (the sequence, not the clip) -----------
	// ReplayChannel::playing() goes false for a moment between the clips of a
	// multi-angle sequence — the finish callback has to travel from the playback
	// thread through the OBS UI queue before the next clip starts — and asking
	// it alone made the dock show the live camera in that gap: a visible flash,
	// and with "to output" on, a flash on air. The queue knows better: it is
	// still active. The grace window keeps a queue that died badly (nothing
	// playing, nothing coming) from pinning the preview forever.
	const int64_t nowNs = (int64_t)os_gettime_ns();
	if (playing || (eventActive && !prevSequenceActive_))
		lastPlayingNs_ = nowNs;
	const bool sequenceOnAir =
		playing || (eventActive && lastPlayingNs_ > 0 &&
			    nowNs - lastPlayingNs_ < kSequenceGapGraceNs);

	// Counts poll() ticks so the work nobody reads thirty times a second — the
	// status line, and re-resolving the preview source — runs at a fraction of
	// the transport rate. See the status block below for why that matters.
	constexpr int kStatusEveryNTicks = 8; // ~264 ms at 33 ms/tick
	const bool refreshStatus = (statusTick_++ % kStatusEveryNTicks) == 0;

	// --- timeline window ---
	// Nothing is "indexed" any more: the live edge is the newest packet the
	// tap captured, and the timeline starts at the oldest instant that can
	// still be replayed. The recorded files reach further back than the RAM
	// ring, so they win when they have anything.
	//
	// Both ends are INSTANTS, and an instant may be negative: master time is
	// read off a clock that starts at boot, so a project recorded before the
	// last reboot sits at negative master values. Nothing here may test one
	// for > 0 — see kNoInstant in segment-index.hpp. The ring, by contrast,
	// only ever holds this session's packets, so its 0 really does mean
	// "empty".
	const int64_t liveEdgeNs = tap.newestNs(cam0);
	int64_t startNs = SegmentIndex::instance().oldestNs(cam0);
	if (startNs == kNoInstant) {
		const int64_t ringOldest = tap.oldestReplayableNs(cam0);
		startNs = ringOldest > 0 ? ringOldest : kNoInstant;
	}
	timelineStartNs_ = startNs;
	// The timeline ends where the FOOTAGE ends, which is the live edge only
	// while a take is running. Measuring it from the live edge alone is what
	// made a reopened project draw a bar of length zero over hours of usable
	// footage: outside REC nothing feeds the front, so the whole recorded
	// project collapsed to a rectangle that said "no live timeline" and
	// swallowed every click. The end of the last recording is not something
	// the anchors can say (an anchor is a beginning), so SegmentIndex demuxes
	// it from the file itself — and says so when the file does not, which
	// leaves the bar honestly empty instead of drawing a guess.
	//
	// Pressing REC then EXTENDS this: the new file anchors onto the same
	// project origin and the live edge overtakes the disk end, so the bar
	// grows to the right instead of starting over.
	const int64_t diskEndNs = SegmentIndex::instance().newestNs(cam0);
	const int64_t endNs =
		std::max(liveEdgeNs > 0 ? liveEdgeNs : kNoInstant, diskEndNs);

	// --- the axis is FOOTAGE, not wall time -------------------------------
	// Stop after two minutes, talk to somebody for one, press REC again: on a
	// wall-time axis the bar grows to three minutes and one of them scrubs to
	// nothing — a hole the operator has to know is there and step over. So
	// the bar is drawn over the recorded spans only, joined end to end (see
	// timeline-map.hpp): the second take continues the first at 2:00, every
	// position on the bar is an instant footage covers, and the total is how
	// much material there is rather than how long the session lasted.
	{
		// The spans on DISK are read on the slow beat only. They change
		// when a file is anchored or its length measured — a few times a
		// minute — and reading them takes SegmentIndex's lock, which its
		// watcher holds while it demuxes a file. Asking thirty times a
		// second put a disk read in the way of the GUI thread, and the
		// dock went unresponsive for exactly as long as the measurement
		// took (the gate caught it: a >> press that never got serviced).
		if (refreshStatus || diskSpans_.empty()) {
			// Prime suspect for a poll() of most of a second: the
			// watcher holds this lock WHILE IT DEMUXES a file.
			Phase _ph(this, "diskSpans");
			diskSpans_.clear();
			for (const auto &[s, e] :
			     SegmentIndex::instance().recordedSpans())
				diskSpans_.push_back({s, e});
			// WHERE THE TAKE IN PROGRESS BEGAN. This is what made the
			// total wrong, and it got worse the longer the take ran.
			//
			// recordedSpans() cannot report the file being written: its
			// length is not measurable until the muxer has finished with
			// it, and inventing one is not that function's business. So
			// the only thing left describing the current take was the
			// RING — the last ~20 s. Everything between the take's start
			// and the ring's oldest instant was missing from the total,
			// so after five minutes of recording the bar declared about
			// twenty seconds, the graduations were drawn for twenty
			// seconds, and every position on it meant something other
			// than what it said.
			//
			// That footage is not missing: the ring serves the recent
			// part and the file on disk serves the older part (that is
			// what Source::Auto does). So the span of the take is
			// [its anchor, the live edge] — the anchor being the newest
			// one on this camera, i.e. the file currently being written.
			// Read here, on the slow beat, because it takes the segment
			// index's lock and does not move for the whole take.
			takeAnchorNs_ = kNoInstant;
			for (const auto &s : SegmentIndex::instance().segments(cam0))
				if (s.anchored && (takeAnchorNs_ == kNoInstant ||
						   s.anchorMasterNs > takeAnchorNs_))
					takeAnchorNs_ = s.anchorMasterNs;
		}
		// The take in progress: from where it started to the live edge. The
		// live edge itself IS cheap to read (the ring's own lock, held for
		// a compare), so it stays on the fast beat and the bar grows
		// smoothly.
		std::vector<TimelineSpan> spans = diskSpans_;
		const int64_t ringOldest = tap.oldestReplayableNs(cam0);
		int64_t liveStart = ringOldest;
		// Only reach back to the anchor if it really is behind the ring and
		// really is this take's: an anchor AFTER the live edge belongs to
		// nothing we can play, and one from a take that has already been
		// measured is in diskSpans_ already (where it does no harm — the
		// map merges touching spans).
		if (takeAnchorNs_ != kNoInstant && liveEdgeNs > 0 &&
		    takeAnchorNs_ < liveEdgeNs &&
		    (liveStart <= 0 || takeAnchorNs_ < liveStart))
			liveStart = takeAnchorNs_;
		if (liveEdgeNs > 0 && liveStart != kNoInstant &&
		    liveEdgeNs > liveStart)
			spans.push_back({liveStart, liveEdgeNs});
		timeline_.setSpans(std::move(spans));
	}
	// Everything below still speaks in master instants; only the conversion
	// to a position on the bar goes through the map.
	displayDurNs_ = timeline_.totalNs();
	if (displayDurNs_ <= 0)
		displayDurNs_ = (startNs != kNoInstant && endNs != kNoInstant &&
				 endNs > startNs)
					? endNs - startNs
					: 0;
	// Anchored footage counts as content even with a dead live edge: that is
	// exactly a project reopened in a later OBS run (nothing captured yet, but
	// yesterday's files are on the timeline). Without startNs the preview would
	// stay black while the replay input was actually producing frames.
	previewHasContent_ = liveEdgeNs > 0 || startNs != kNoInstant;

	// Event times belong to the PROJECT, not to the angle being watched: the
	// earliest anchored recording on ANY camera is 0:00 for the table and for
	// the YouTube chapters. Reading it off the selected angle renumbered every
	// row when the operator pressed another camera button, and gave nothing at
	// all for an angle with no anchor — which is how a reopened project ended
	// up printing marks as raw monotonic time. The ring is the fallback for a
	// session that has not written a file yet.
	int64_t eventOrigin = SegmentIndex::instance().projectOriginNs();
	if (eventOrigin == kNoInstant)
		eventOrigin = startNs;
	eventOriginNs_ = eventOrigin;

	// The event columns are drawn relative to that origin, so a moved origin
	// has to redraw them — it moves once for real, when the first anchored
	// recording replaces the ring's (constantly evicted) oldest instant. The
	// 1 s of slack is what keeps the ring's drift from rebuilding the table
	// thirty times a second.
	// The subtraction is guarded, not just the result: kNoInstant is INT64_MIN
	// and subtracting it from a real instant overflows.
	if (eventOriginNs_ != kNoInstant &&
	    (tableOriginNs_ == kNoInstant ||
	     std::abs(eventOriginNs_ - tableOriginNs_) > 1'000'000'000LL)) {
		tableOriginNs_ = eventOriginNs_;
		Phase _ph(this, "refreshEvents/origin");
		refreshEvents();
	}

	// Is the live front still being FED? Not "did we press REC": after STOP the
	// tap is detached and the newest instant stops moving, and that is exactly
	// when jumping "back to live" would be a lie — there is no live to go to,
	// only the last frame of a take that is over.
	const bool liveFrontFed = liveEdgeNs > 0 && tap.anyAttached();

	// --- the sequence just ended: give the transport back to the live edge ---
	// The operator should not have to press NOW after every replay. This fires
	// on the QUEUE ending, natural or by Stop, not on each clip — a two-angle
	// event must not snap back to live halfway through.
	if (prevSequenceActive_ && !eventActive) {
		if (freeReviewInNs_ != kNoInstant) {
			// A FREE REVIEW DOES NOT HAND THE PANEL BACK TO LIVE.
			//
			// The operator stopped it on purpose — that is how it
			// ends — and what he does next is decide whether to air
			// it. Snapping to the live edge here would throw away the
			// stretch he just chose, and with it the second function
			// of the Play key, which replays exactly that stretch.
			// The armed in-point is left alone: it is the beginning of
			// what he watched, not wherever he pressed Stop.
		} else if (liveFrontFed) {
			// Back to the front, exactly like NOW.
			core.setFollowLive(true);
			playheadNs_ = liveEdgeNs;
		} else {
			// The take is over, so there is nothing to follow: park the
			// playhead on the last instant of footage and STAY in review,
			// which keeps the replay's picture on the preview instead of
			// swapping in a live camera that has nothing to do with the
			// moment being reviewed.
			if (liveEdgeNs > 0)
				playheadNs_ = liveEdgeNs;
		}
	}
	prevSequenceActive_ = eventActive;

	// Everything inside that window is playable (ring or files), so unlike the
	// file-tailing engine there is no trailing "not yet flushed" region.
	//
	// The playhead is the dock's, not the engine's: ReplayChannel reports the
	// last frame it pushed forever after, so a finished clip left the bar
	// wherever it stopped. While a clip plays it IS that frame; otherwise it is
	// where the operator parked the timeline (scrub, NOW, end of sequence).
	// hasPosition(), not `posNs > 0`: an instant of 0 is a legitimate one and
	// footage recorded before the machine's last boot maps onto NEGATIVE
	// instants (session-clock.hpp, kNoInstant) — the ordinary state of a
	// reopened project. Asking "is it positive?" pinned the bar to the live
	// edge for exactly the footage a step back is for.
	const int64_t posNs = chan.positionNs();
	if (playing && chan.hasPosition())
		playheadNs_ = posNs;
	else if (followLive && liveEdgeNs > 0 && !sequenceOnAir)
		playheadNs_ = liveEdgeNs;
	// Footage behind the playhead, not wall time behind it — the same axis
	// the bar is drawn on (see timeline-map.hpp).
	const int64_t relPosNs =
		!timeline_.empty() && playheadNs_ != kNoInstant
			? timeline_.footageBefore(playheadNs_)
			: ((startNs != kNoInstant && playheadNs_ != kNoInstant &&
			    playheadNs_ > startNs)
				   ? playheadNs_ - startNs
				   : 0);

	if (!seekDragging_) {
		if (followLive && liveFrontFed) {
			// Watching the live edge: the playhead IS the edge.
			seek_->setProgress(1.0, 1.0);
		} else {
			double posFrac = displayDurNs_ > 0
						 ? std::min(1.0,
							    (double)relPosNs /
								    (double)displayDurNs_)
						 : 0.0;
			seek_->setProgress(posFrac, 1.0);
			// Zoomed in, the window follows the picture: a bar
			// showing four seconds of an hour is useless the moment
			// the playhead walks out of it.
			seek_->ensureVisible(posFrac);
		}
	}

	// ONE MARK, and it says what the key will DO. The key carried a drawn
	// mark AND a glyph in its text - a triangle, two bars and a third
	// character on one 30 px key, the last of them turned cyan by the
	// "playing" rule because a style sheet colours text and cannot reach a
	// pixmap. Three marks on the key that gets pressed most.
	if (playPauseIcon_ != (playing ? 1 : 0)) {
		playPauseIcon_ = playing ? 1 : 0;
		setKeyIcon(playPauseBtn_, playing ? Icon::Pause : Icon::Play,
			   tintsFor(sc()));
	}
	if (playPauseBtn_->property("playing").toBool() != playing) {
		playPauseBtn_->setProperty("playing", playing);
		repolish(playPauseBtn_);
	}
	if (nowBtn_->property("live").toBool() != followLive) {
		nowBtn_->setProperty("live", followLive);
		repolish(nowBtn_);
	}
	// The speed slider is the dock's own state (the engine has none), with one
	// exception: the speed HOTKEYS set the default without ever reaching a
	// widget, and a Stream Deck operator pressing 50% must not be left looking
	// at a slider that still says 1×. Only when the two disagree, and never
	// while his finger is on it.
	{
		const int coordPct =
			pc().defaultSpeedPct();
		if (speed_ && coordPct != speedPct_ && !speed_->isSliderDown()) {
			speedPct_ = coordPct;
			QSignalBlocker block(speed_);
			speed_->setValue(coordPct);
			speedLbl_->setText(QString::asprintf(
				"%.2f\xc3\x97", coordPct / 100.0));
		}
		// the reference controller fills the preset that matches the speed in force. The
		// property drives the QSS, so it is only repolished when it moves.
		if (speedChips_) {
			for (QAbstractButton *ab : speedChips_->buttons()) {
				const bool on = speedChips_->id(ab) == speedPct_;
				if (ab->property("active").toBool() == on)
					continue;
				ab->setProperty("active", on);
				repolish(ab);
			}
		}
	}


	// The row on air gets a PGM cue on its id cell. the reference controller colours the whole
	// row, but our row colour is the selection (orange), and repainting a
	// selected row would make "playing" and "selected" indistinguishable —
	// which is the one thing that must never be ambiguous during a match.
	{
		const auto &ps = playSt;
		const bool wasProgrammatic = itemsProgrammatic_;
		itemsProgrammatic_ = true; // colouring is not an operator edit
		for (int row = 0; row < events_->rowCount(); row++) {
			QTableWidgetItem *idItem = events_->item(row, kColId);
			if (!idItem)
				continue;
			int rowEv = idItem->data(Qt::UserRole).toInt();
			bool isActive = ps.active && (ps.eventId == rowEv);
			if (idItem->data(Qt::UserRole + 1).toBool() != isActive) {
				idItem->setData(Qt::UserRole + 1, isActive);
				idItem->setForeground(
					isActive ? QBrush(QColor("#ff5a3c"))
						 : QBrush());
			}
		}
		itemsProgrammatic_ = wasProgrammatic;
	}
	// the reference controller paints the header of the camera being watched green.
	updateCamHeaderHighlight();

	// --- recording status ---
	// Auto-follow the live edge when recording starts so the preview tracks
	// the new take instead of sitting on the last clip that played.
	if (rec && !prevRecording_) {
		core.setFollowLive(true);
		// A new take: whatever stretch of the last one was armed on the
		// bar is not what the play keys are about any more.
		clearFreeReview();
		// Start the watchdog on every take, however it was started (the
		// REC button, a hotkey, the API): the failure it catches is not
		// the dock's, it is Branch Output declining.
		armWatchDeadlineNs_ = (int64_t)os_gettime_ns() + kArmWatchNs;
	}
	prevRecording_ = rec;

	// --- did Branch Output actually START? -------------------------------
	// Our REC only enables the filters. Branch Output re-evaluates its own
	// start conditions on a 1 s timer, and the Interlock setting that gates
	// them is global, lives in ITS dock and is exposed by no API — so on
	// anything but "Always ON" the filters stay armed and nothing records.
	// The old symptom was the worst kind: a red REC button, a running clock,
	// and marks landing on footage that does not exist, discovered only when
	// a replay came up empty (and, before that, only as a log warning 40 s
	// later, from the tap giving up). A take that has not started in a few
	// seconds never will, so it is cancelled here rather than faked.
	if (armWatchDeadlineNs_ > 0) {
		if (!rec || core.branchOutputRecording()) {
			armWatchDeadlineNs_ = 0; // running, or already stopped
		} else if ((int64_t)os_gettime_ns() >= armWatchDeadlineNs_) {
			// Cleared BEFORE cancelling: cancelDeadRecording() queues a
			// modal, whose nested event loop keeps this timer ticking.
			armWatchDeadlineNs_ = 0;
			cancelDeadRecording();
			// Still account for the tick: this branch opens a modal,
			// which is precisely a tick that took a very long time,
			// and losing it would hide the one sample worth having.
			accountUiTick(tickStartNs, (uint64_t)os_gettime_ns());
			return;
		}
	}

	// Live-mirror preview state (see drawChannelA). The preview is a
	// confidence monitor for the selected angle: the operator lines the
	// cameras up BEFORE the take, so it mirrors the live source — recording or
	// not, ever started or not.
	//
	// But ONLY while following live. That is the whole meaning of follow-live,
	// and the two bugs it fixes are the same bug: asking "is a clip playing
	// right now" showed the live camera in the gap between two clips of a
	// sequence, and showed it again the moment a scrub review ran out — so
	// dragging the seekbar to a point in the recorded timeline ended up
	// displaying the camera as it is NOW, presented as the footage of THEN.
	// Once the operator has scrubbed or played something he is reviewing the
	// timeline, and the preview stays on the footage (the replay input holds
	// the last frame it was handed) until he asks for the live edge back.
	//
	// The name→source lookup and the reference counting happen HERE, on the UI
	// thread. The graphics thread gets a ready-made reference (previewSource_).
	{
		const bool live = followLive && !sequenceOnAir;
		previewShowsReplay_.store(!live);
		// Re-resolving costs a lookup under libobs' global source mutex, so
		// only do it when the answer can have changed: a different kind of
		// picture, a different angle, or the 4 Hz revalidation that catches a
		// source renamed, deleted or replaced by a scene-collection change.
		if (live != previewLive_ || cam0 != previewCam0_ || refreshStatus) {
			// Fourth suspect: a name lookup takes libobs' global source
			// mutex, and the coordinator holds it across a scene switch —
			// which is exactly what was happening in the window where a
			// tick was measured at 964 ms.
			Phase _ph(this, "previewSource");
			obs_source_t *next = nullptr;
			if (live) {
				const std::string name =
					core.getConfig().cameras[cam0].sourceName;
				// An unconfigured angle, or a name nothing answers
				// to, publishes nothing: the preview goes black
				// instead of rendering a dangling pointer.
				if (!name.empty())
					next = obs_get_source_by_name(name.c_str());
			} else if (previewHasContent_) {
				// Nothing captured yet: render black rather than
				// whatever the replay input last held.
				next = chan.acquireSource();
			}

			obs_source_t *prev = nullptr;
			{
				std::lock_guard<std::mutex> lk(previewMutex_);
				prev = previewSource_;
				previewSource_ = next;
			}
			// Released OUTSIDE the lock, on purpose: the last release
			// destroys the source, which enters the graphics context —
			// holding previewMutex_ there would be the UI thread waiting
			// on the graphics thread while the graphics thread waits on
			// previewMutex_, which is the deadlock this whole change is
			// about.
			if (prev)
				obs_source_release(prev);
			previewLive_ = live;
			previewCam0_ = cam0;
		}
		// Channel B's box, on the same 4 Hz beat and by the same rules: a
		// lookup here, an owned reference published for the graphics
		// thread. B is a replay bay with no live mirror, so this is
		// simply B's input — and nothing at all until B has played
		// something, which draws black rather than a stale picture.
		if (refreshStatus) {
			// hasPosition(), not positionNs() > 0: instants can be zero
			// or negative (kNoInstant), and "has this bay ever shown a
			// frame" is the actual question.
			obs_source_t *nextB =
				channelBEnabled_ &&
						(PlaybackCoordinator::instance(
							 Which::B)
							 .playState()
							 .active ||
						 ReplayChannel::instance(Which::B)
							 .hasPosition())
					? ReplayChannel::instance(Which::B)
						  .acquireSource()
					: nullptr;
			obs_source_t *prevB = nullptr;
			{
				std::lock_guard<std::mutex> lk(previewMutex_);
				prevB = previewSourceB_;
				previewSourceB_ = nextB;
			}
			if (prevB)
				obs_source_release(prevB);
		}
		// --- the angle boxes follow the REVIEW -----------------------
		// The tiles have to show the moment on air on every other lens,
		// and the only thing that knows which moment that is — through a
		// multi-angle sequence, a review split across a junction, and the
		// chunks of a free run — is the QUEUE. Driving them from here, off
		// playState(), is one hook instead of a cueTiles() call sprinkled
		// over every play path, and the one that would be forgotten is
		// always the path that has no event behind it.
		//
		// cueTiles() is a no-op unless the range really moved, which on an
		// ordinary tick it has not.
		if (!followLive && playSt.active &&
		    playSt.clipInNs != kNoInstant) {
			Phase _ph(this, "cueTiles");
			cueTiles(playSt.clipInNs, playSt.clipOutNs,
				 playSt.speedPct,
				 playSt.reverse
					 ? ReplayChannel::Direction::Reverse
					 : ReplayChannel::Direction::Forward);
		}
		// Back on the live edge: the feeds are decoders and private
		// sources, and keeping eight of them alive to hold a still nobody
		// is looking at is the one way this feature could cost something
		// when it is not in use.
		if (followLive && tileCueInNs_ != kNoInstant)
			releaseTileFeeds();

		// The tiles are resolved on the same 4 Hz beat and by the same
		// rules: a lookup on the UI thread, an owned ref published for the
		// graphics thread. Re-running it periodically is also what catches
		// a camera source renamed, deleted or swapped by a scene-collection
		// change — the tile goes black instead of holding a stale pointer.
		//
		// ...and IMMEDIATELY when live/review flips, not on the next 4 Hz
		// beat: a quarter of a second of eight boxes showing the wrong
		// kind of picture is exactly the sort of thing an operator sees
		// and cannot name.
		//
		// ...and immediately when a feed shows its FIRST picture, which is
		// the other half of the same problem. The flip above fires
		// microseconds after the feeds were started — before any of them
		// has decoded anything — so on its own it published nothing and
		// left the boxes waiting for the slow beat anyway. This is the
		// tick the picture actually exists on.
		const bool tilePictureArrived = pollTileFeedPictures();
		if (refreshStatus || tilesLive_ != followLive || tilePictureArrived)
			refreshTileSources();
	}
	// Cheap (two compares in the common case) and it has to follow the angle
	// buttons and the queue, both of which move outside refreshStatus.
	updateMultiviewTally();

	// --- M4: how is the take actually going? -----------------------------
	// READ ONLY. The monitor samples on a thread of its own, and it has to:
	// sampling reads PacketTap::stats(), whose lock the tap holds across a
	// detach that blocks on Branch Output, which in turn needs this thread —
	// so a sample taken here froze OBS outright the first time a camera went
	// away mid-take. See the note at the top of health.hpp. findings() is a
	// copy under a lock held for exactly as long as the copy.
	{
		auto &monitor = HealthMonitor::instance();
		if (healthBtn_) {
			const auto findings = monitor.findings();
			const health::Level worst = health::worstOf(findings);
			if (findings.empty()) {
				healthBtn_->hide();
			} else {
				const bool bad = worst >= health::Level::Blocker;
				healthBtn_->setText(
					QString("%1 %2")
						.arg(bad ? QStringLiteral("⛔")
							 : QStringLiteral("⚠"))
						.arg(findings.size()));
				healthBtn_->setToolTip(QString::fromStdString(
					findingsBlock(findings,
						      health::Level::Info)));
				const QString state = bad
							      ? QStringLiteral("bad")
							      : QStringLiteral("warn");
				if (healthBtn_->property("level").toString() !=
				    state) {
					healthBtn_->setProperty("level", state);
					repolish(healthBtn_);
				}
				healthBtn_->show();
			}
		}
	}

	// A MARK AND A WORD, not a mark and a word with a second mark inside it.
	// The text used to carry its own bullet, so the key showed the drawn
	// record dot and then another one printed beside it.
	recBtn_->setText(rec ? QStringLiteral("STOP") : QStringLiteral("REC"));
	if (recIcon_ != (rec ? 1 : 0)) {
		recIcon_ = rec ? 1 : 0;
		// ARMED, THE KEY IS FILLED RED and its label is white, so the mark
		// is white too; at rest the key is chrome with a red label, so the
		// mark is red. Two states of one key, and the role says which.
		setKeyIconRole(recBtn_, rec ? Icon::Stop : Icon::Rec,
			       rec ? IconRole::OnSignal : IconRole::Rec,
			       tintsFor(sc()), 13);
	}
	if (recBtn_->property("recording").toBool() != rec) {
		recBtn_->setProperty("recording", rec);
		repolish(recBtn_);
	}

	// Sync the Live button with engine state (startRecording sets liveMode
	// internally without going through the button).
	bool lm = EventStore::instance().liveMode();
	if (liveBtn_ && liveBtn_->isChecked() != lm) {
		QSignalBlocker block(liveBtn_);
		liveBtn_->setChecked(lm);
	}

	// The status line is the only thing in this timer that can block, and it
	// is also the one nobody reads thirty times a second: statusJson() stats
	// the session folder (std::filesystem::space), picks the encoder by
	// enumerating the registered types while holding the core lock, then
	// builds a JSON document we immediately parse back.
	//
	// On a local disk that measures as free — the self-test saw the same tick
	// cadence with and without this throttle — so this is not a speed-up, it
	// is about WHERE that syscall runs: a session folder on a NAS (a normal
	// setup for a replay rig) turns space() into a network round trip, and one
	// that hits an unreachable share blocks for the SMB timeout. Thirty of
	// those a second on the GUI thread is a frozen OBS, not a slow dock. Four
	// a second is still faster than any number in that line can change.
	// The rest of poll() — seekbar, playhead, transport state — is cheap and
	// stays at full rate, because that IS what has to look smooth.
	// (refreshStatus is computed at the top: the preview resolution uses it
	// too.)
	// A live notice owns the status line until it expires: it is the answer to
	// something the operator just did, and overwriting it 250 ms later with the
	// idle line is how "the dock ignored me" happens.
	// On the same slow beat: whether there are two bays at all. It changes
	// when Settings is saved — a few times an evening, not thirty times a
	// second — and it early-outs otherwise, which keeps the getConfig() lock
	// off the fast path.
	if (refreshStatus) {
		applyChannelBVisibility();
		// ...AND HOLD THE REPLAY AUDIO WHERE THE MUTE KEY SAYS. A source
		// re-created on a collection reload comes back unmuted, and OBS's
		// own mixer must not be the thing that lifts a mute the operator
		// set here — so the key's state is re-asserted onto the bay(s) it
		// targets. setMuted() early-outs when the source already agrees.
		if (muteBtn_) {
			const bool want = muteBtn_->isChecked();
			for (Which w : targetChannels())
				ReplayChannel::instance(w).setMuted(want);
		}
		// ...and whether the panel is floating, which is the only state
		// in which there is a screen for it to take. Same beat, same
		// reason: pulling a dock out of OBS is a deliberate gesture, and
		// this early-outs unless the answer changed.
		refreshFullScreenKey();
		// ...AND WHETHER THE MONITORING ROW WAS SIZED AGAINST A PANEL
		// THAT HAS SINCE SETTLED.
		//
		// applyPreviewAspect reads two geometries — the pane's width and
		// the room the row may have — and during a resize both are still
		// moving. Read a beat early they come out smaller than they end
		// up, and the answer STICKS, because the tile ceiling it writes
		// is a maximum: the cameras stay small and the room they should
		// have had is drawn as empty panel. That is the "resizing leaves
		// unused space" report, and it is not one resize path — it is
		// every path that ends without one more pass.
		//
		// Cheap and self-limiting: a pass that agrees with the geometry
		// changes nothing, so it cannot re-trigger itself.
		//
		// A TOLERANCE, and it is not politeness: once the operator has
		// dragged the pictures/list divider, the room IS the pane's
		// height — and this pass writes a maximum onto that pane. A
		// difference of one pixel between the two is a limit cycle at
		// 4 Hz. Two pixels of slack cannot start one, and a stale pass
		// is never off by two.
		//
		// ...and only while the pictures are up. With the monitors down
		// monitorRoomH() reads a hidden pane's stale height, so the two
		// numbers never agree again and this would run every beat to no
		// effect.
		if (monitorsOn_ && previewPane_ && monitorSplit_ &&
		    (std::abs(std::max(80, monitorSplit_->width()) - aspectPaneW_) > 2 ||
		     std::abs(monitorRoomH() - aspectRoomH_) > 2)) {
			MR_DLOG("[mondiag] poll re-settle: paneW %d vs %d, roomH %d vs %d",
				std::max(80, monitorSplit_->width()), aspectPaneW_,
				monitorRoomH(), aspectRoomH_);
			applyPreviewAspect();
		}
	}

	// The other prime suspect: statusJson() calls std::filesystem::space() on
	// the session folder, and on a network share that is a round trip.
	std::unique_ptr<Phase> _statusPh;
	if (refreshStatus && nowNs >= noticeUntilNs_)
		_statusPh = std::make_unique<Phase>(this, "statusJson");
	Data st((refreshStatus && nowNs >= noticeUntilNs_) ? core.statusJson()
							  : std::string());
	_statusPh.reset();
	if (st) {
		QString ver = obs_data_get_string(st, "version");
		int64_t mins = obs_data_get_int(st, "estimatedMinutesRemaining");
		bool boOk = obs_data_get_bool(st, "branchOutputAvailable");
		// the reference controller's second line: how much recording time is left, in
		// hours:minutes, not a bare minute count nobody converts under
		// pressure.
		QString s;
		if (!boOk)
			s = QStringLiteral("⚠ Branch Output");
		else if (mins >= 0)
			s = QString("%1 %2")
				    .arg(QString::asprintf("%02lld:%02lld:00",
							   (long long)(mins / 60),
							   (long long)(mins % 60)))
				    .arg(obs_module_text("Dock.Remaining"));
		else
			s = QString("v%1 • %2")
				    .arg(ver)
				    .arg(rec ? obs_module_text("Dock.Recording")
					     : obs_module_text("Dock.Idle"));
		statusLbl_->setText(s);
	}

	// The wall clock above it, red while the take runs (the reference controller). Same 4 Hz as
	// the rest of the status block — a clock that ticks 30 times a second
	// costs a restyle 30 times a second and reads no better.
	if (refreshStatus && clockLbl_) {
		clockLbl_->setText(QDateTime::currentDateTime().toString(
			QStringLiteral("yyyy-MM-dd HH:mm:ss")));
		if (clockLbl_->property("rec").toBool() != rec) {
			clockLbl_->setProperty("rec", rec);
			repolish(clockLbl_);
		}
	}

	// The green channel strip and the text printed on the position bar.
	{
		Phase _ph(this, "channelStrip");
		updateChannelStrip();
	}

	// --- project label ---
	// Same rate as the status line: it only changes when the operator opens
	// or creates a project, and reading it copies the whole Config.
	if (refreshStatus) {
		std::string proj = core.getConfig().currentProjectName;
		if (projectLbl_) {
			if (proj.empty()) {
				projectLbl_->hide();
			} else {
				projectLbl_->setText(
					QString::fromStdString("[" + proj +
							       "]"));
				projectLbl_->show();
			}
		}
	}

	// --- auto-refresh event list on any external mutation (hotkeys, etc.) ---
	uint64_t ev = EventStore::instance().version();
	if (ev != lastEventVersion_) {
		lastEventVersion_ = ev;
		// TWO phases, not one. They were timed together and the total was
		// read as the table's — but the table's own breakdown never showed
		// a rebuild anywhere near the 567 ms this phase reported, which
		// means the time was in the other half or in neither. A phase that
		// covers two things names neither.
		{
			Phase _ph(this, "refreshAngles");
			refreshAngles();
		}
		Phase _ph(this, "refreshEvents/version");
		refreshEvents(); // rebuilds markerNs_ (raw ns pairs)
	} else if (eventsDirty_ || commentsDirty_) {
		// A rebuild the store did not ask for: one that was deferred past an
		// open editor, or a comment that has to reach the other rows. Both
		// resolve themselves within a tick or two of the operator finishing.
		commentsDirty_ = false;
		refreshEvents();
	}

	// Recompute seekbar marker fractions every tick: the window slides (the
	// live edge grows, the ring drops its oldest), so markers move even when
	// the events themselves do not change.
	//
	// Built into MEMBER scratch buffers, not into two fresh vectors: this ran
	// thirty times a second and allocated twice on every one of them, then
	// handed the result to a setter that took it BY VALUE and repainted
	// unconditionally — so a panel with nothing happening in it was asking Qt
	// to redraw the full-width bar 30 times a second, forever. The setters
	// compare now, which is only possible because the buffers survive the
	// call.
	if (seek_ && displayDurNs_ > 0) {
		markerFracScratch_.clear();
		markerIdScratch_.clear();
		markerFracScratch_.reserve(markerNs_.size());
		markerIdScratch_.reserve(markerNs_.size());
		for (size_t i = 0; i < markerNs_.size(); i++) {
			// Through the same map the bar is drawn with, so a mark
			// sits over its own footage however many takes there
			// have been (and an event in a gap that no longer exists
			// collapses onto the join rather than smearing over it).
			const auto &[inNs, outNs] = markerNs_[i];
			double inf = 0.0, outf = 0.0;
			if (timeline_.rangeFraction(inNs, outNs, inf, outf)) {
				markerFracScratch_.push_back({inf, outf});
				// The two lists are read in lockstep by the
				// bar, so a marker that is not drawn must not
				// leave its id behind either.
				markerIdScratch_.push_back(
					i < markerIds_.size() ? markerIds_[i]
							      : 0);
			}
		}
		seek_->setEventMarkers(markerFracScratch_);
		seek_->setEventMarkerIds(markerIdScratch_);
	} else if (seek_) {
		markerFracScratch_.clear();
		markerIdScratch_.clear();
		seek_->setEventMarkers(markerFracScratch_);
		seek_->setEventMarkerIds(markerIdScratch_);
	}

	// Last thing in the tick, so it measures the whole of it.
	accountUiTick(tickStartNs, (uint64_t)os_gettime_ns());
}

// How the panel is being SERVICED, written to the OBS log.
//
// The black-window failure this exists for is not a failure of the plugin's own
// logic: everything keeps running, the previews keep drawing, the recording
// keeps recording. What stops is Qt being able to put pixels on the OBS window.
// Two numbers say whether that was happening, and neither of them can be
// recovered after the fact from anything else in the log:
//
//   late   how long after its due time the 33 ms tick actually fired. The poll
//          timer is delivered by the same event loop as paint events, so a tick
//          that arrives 900 ms late IS a window that went 900 ms without a
//          repaint. This is the direct measurement of the symptom.
//   cost   how long poll() itself took. That is the part of `late` we own; if
//          cost is small and late is large, the UI thread was being held by
//          something outside this plugin.
//
// ...plus the repaint census, as a RATE. A run where the panel is idle and the
// bars are still asking for tens of repaints a second means the guards have a
// hole in them and this document's diagnosis was incomplete.
// The worst single phase of the window, kept by cost. Not a sum per phase: what
// has to come off this thread is the thing that blocked ONCE for most of a
// second, and an average over ten seconds of 33 ms ticks buries it completely.
void MultiReplayDock::notePhase(const char *name, int64_t ns)
{
	if (ns > uiWorstPhase_.ns) {
		uiWorstPhase_.ns = ns;
		uiWorstPhase_.name = name;
	}
}

void MultiReplayDock::accountUiTick(uint64_t tickStartNs, uint64_t tickEndNs)
{
	constexpr int64_t kExpectedTickNs = 33'000'000;
	// A tick this late is not jitter, it is a stall: at 30 Hz it means eight
	// frames the panel could not have been redrawn in. Logged as it happens
	// (throttled), because the aggregate below would average it away.
	constexpr int64_t kStallNs = 250'000'000;
	constexpr int64_t kStallLogEveryNs = 5'000'000'000;
	constexpr int64_t kReportEveryNs = 10'000'000'000;

	if (lastTickNs_ == 0) {
		lastTickNs_ = tickStartNs;
		uiWindowStartNs_ = tickStartNs;
		uiSeekReqAtWindow_ = g_seekCensus.requested;
		uiSeekSupAtWindow_ = g_seekCensus.suppressed;
		uiSeekServedAtWindow_ = g_seekCensus.served;
		uiClipReqAtWindow_ = g_clipCensus.requested;
		uiClipServedAtWindow_ = g_clipCensus.served;
		return;
	}

	const int64_t late =
		(int64_t)(tickStartNs - lastTickNs_) - kExpectedTickNs;
	lastTickNs_ = tickStartNs;
	const int64_t cost = (int64_t)(tickEndNs - tickStartNs);

	uiTicks_++;
	if (late > 0) {
		uiLateSumNs_ += late;
		uiLateMaxNs_ = std::max(uiLateMaxNs_, late);
	}
	uiCostSumNs_ += cost;
	uiCostMaxNs_ = std::max(uiCostMaxNs_, cost);

	if (late >= kStallNs &&
	    (uiLastStallLogNs_ == 0 ||
	     (int64_t)(tickStartNs - uiLastStallLogNs_) >= kStallLogEveryNs)) {
		uiLastStallLogNs_ = tickStartNs;
		obs_log(LOG_WARNING,
			"[ui] UI thread stalled %lld ms — the panel could not be "
			"repainted for that long (poll itself took %lld ms)",
			(long long)(late / 1'000'000),
			(long long)(cost / 1'000'000));
	}

	if ((int64_t)(tickStartNs - uiWindowStartNs_) < kReportEveryNs)
		return;

	const double secs = (double)(tickStartNs - uiWindowStartNs_) / 1e9;
	const uint64_t seekReq = g_seekCensus.requested - uiSeekReqAtWindow_;
	const uint64_t seekSup = g_seekCensus.suppressed - uiSeekSupAtWindow_;
	const uint64_t seekSrv = g_seekCensus.served - uiSeekServedAtWindow_;
	const uint64_t clipReq = g_clipCensus.requested - uiClipReqAtWindow_;
	const uint64_t clipSrv = g_clipCensus.served - uiClipServedAtWindow_;

	// Periodic telemetry — verbose diagnostic only (Settings ▸ Advanced ▸
	// Verbose log). The window counters below are reset either way, so turning
	// it off stops the log line without letting the accumulators run away.
	if (debugLoggingEnabled())
		obs_log(LOG_INFO,
			"[ui] %.1f tick/s | late avg %lld ms max %lld ms | poll avg "
			"%.1f ms max %lld ms (worst phase: %s %lld ms) | repaints/s "
			"seek %.1f (served %.1f, suppressed %.1f) clip %.1f "
			"(served %.1f)",
			(double)uiTicks_ / secs,
			(long long)(uiTicks_ ? uiLateSumNs_ / uiTicks_ / 1'000'000
					     : 0),
			(long long)(uiLateMaxNs_ / 1'000'000),
			uiTicks_ ? (double)uiCostSumNs_ / (double)uiTicks_ / 1e6
				 : 0.0,
			(long long)(uiCostMaxNs_ / 1'000'000),
			uiWorstPhase_.name[0] ? uiWorstPhase_.name : "-",
			(long long)(uiWorstPhase_.ns / 1'000'000),
			(double)seekReq / secs, (double)seekSrv / secs,
			(double)seekSup / secs, (double)clipReq / secs,
			(double)clipSrv / secs);

	uiWindowStartNs_ = tickStartNs;
	uiTicks_ = 0;
	uiLateSumNs_ = 0;
	uiLateMaxNs_ = 0;
	uiCostSumNs_ = 0;
	uiCostMaxNs_ = 0;
	uiWorstPhase_ = PhaseCost{};
	uiSeekReqAtWindow_ = g_seekCensus.requested;
	uiSeekSupAtWindow_ = g_seekCensus.suppressed;
	uiSeekServedAtWindow_ = g_seekCensus.served;
	uiClipReqAtWindow_ = g_clipCensus.requested;
	uiClipServedAtWindow_ = g_clipCensus.served;
}

void MultiReplayDock::refreshListNames()
{
	if (!listTabs_)
		return;
	auto &store = EventStore::instance();

	// NOTHING TO DO IS THE ORDINARY CASE, and this is where 143 ms went.
	//
	// refreshEvents calls this on every bump of the store's version — every
	// mark, every trim, every ticked checkbox — and it then wrote text,
	// visibility and a tooltip onto all twenty tabs. Each of those makes
	// QTabBar re-lay-out the whole row and recompute its size hints, so twenty
	// writes are twenty relayouts of a widget nothing has changed. Measured
	// from the dock's own phase accounting: a refresh that reused every angle
	// cell and rebuilt none still took 143 ms, all of it here and in
	// rebuildEventColumns, before any row work began.
	//
	// List names change when the operator renames one. The count changes when
	// he changes it in Settings. Both are things he does between matches.
	const int shown =
		std::clamp(ReplayCore::instance().getConfig().eventListCount, 1,
			   kEventLists);
	const int tabs = std::min(kEventLists, listTabs_->count());
	bool same = shown == lastShownTabs_ &&
		    (int)lastListNames_.size() == tabs;
	if (same) {
		for (int i = 1; i <= tabs; i++) {
			if (lastListNames_[(size_t)(i - 1)] != store.listName(i)) {
				same = false;
				break;
			}
		}
	}
	if (same)
		return;

	lastShownTabs_ = shown;
	lastListNames_.resize((size_t)tabs);
	for (int i = 1; i <= tabs; i++)
		lastListNames_[(size_t)(i - 1)] = store.listName(i);
	// Tab text only: changing it does not move the current tab, but blocking
	// the signals keeps a rebuild from ever looking like an operator switching
	// list.
	QSignalBlocker block(listTabs_);

	// How many lists the operator asked to see (computed above, with the
	// early-out). The tabs beyond it are HIDDEN, not removed: what is in those
	// lists is still there, still saved, and comes back the moment he raises
	// the number.
	for (int i = 1; i <= kEventLists && i <= listTabs_->count(); i++)
		listTabs_->setTabVisible(i - 1, i <= shown);
	// A hidden tab must not stay the current one: the table would be showing a
	// list with no tab lit, which reads as "no list at all".
	if (listTabs_->currentIndex() >= shown) {
		listTabs_->setCurrentIndex(shown - 1);
		store.selectList(shown);
	}

	for (int i = 1; i <= kEventLists && i <= listTabs_->count(); i++) {
		const std::string nm = store.listName(i);
		// "1 PARTITA" — the NUMBER STAYS. A named list used to replace the
		// number with the name, and that threw away the one label that is
		// stable: the hotkeys, the log lines and the operator's own "go to
		// three" all mean the number, so a tab that only says PARTITA is a
		// tab he has to count along the row to identify. The name is what it
		// is FOR; the number is how it is addressed.
		listTabs_->setTabText(i - 1,
				      nm.empty() ? QString::number(i)
						 : QString("%1 %2").arg(i).arg(
							   QString::fromStdString(
								   nm)));
		listTabs_->setTabToolTip(
			i - 1, nm.empty()
				       ? QString("%1 %2")
						 .arg(obs_module_text("Dock.List"))
						 .arg(i)
				       : QString("%1 · %2").arg(i).arg(
						 QString::fromStdString(nm)));
	}
}

void MultiReplayDock::refreshAngles()
{
	// The cameras are chosen by clicking their PICTURES now, so the only thing
	// left that follows the camera configuration is the multiview itself. This
	// stayed a function of its own because every path that can change that
	// configuration already calls it, and it is a no-op unless the set of
	// cameras (or one of their names) really moved.
	rebuildMultiview();
}

void MultiReplayDock::refreshEvents()
{
	// NEVER rebuild the table out from under an open editor.
	//
	// Every cell of a camera column is a real widget (a checkbox and two
	// combos), and rebuilding the table deletes them. poll() calls this
	// whenever the store's version moves — a mark, a trim, another angle
	// ticked — so a comment list dropped down during a match was being
	// destroyed a few tens of milliseconds later, with the popup vanishing
	// before the operator could pick anything out of it. That is the whole of
	// the "the dropdown closes too fast" bug: the list was never closing, its
	// combo was being deleted.
	//
	// So: an open popup, or a half-typed comment, defers the rebuild. poll()
	// comes back every 33 ms and picks it up the moment the editor is done.
	if (QApplication::activePopupWidget()) {
		eventsDirty_ = true;
		return;
	}
	if (events_) {
		QWidget *fw = QApplication::focusWidget();
		if (fw && qobject_cast<QLineEdit *>(fw) &&
		    events_->isAncestorOf(fw)) {
			eventsDirty_ = true;
			return;
		}
	}
	eventsDirty_ = false;

	// Here rather than only where a name is edited: opening another project
	// loads that project's list names without ever bumping the version
	// counter, and this is the one function every one of those paths calls.
	// From the TOP, not from where the rows are built. Timed only around the
	// row building, this function reported 62 ms while poll()'s phase around
	// the whole of it reported 307 — so a quarter of a second was going
	// somewhere upstream of the timer, and a breakdown that cannot see it
	// points at the wrong half. listJson() below serialises every event of the
	// list, with all its angles, under the store's lock, and it is called on
	// every refresh.
	const uint64_t refreshStartNs = os_gettime_ns();
	refreshListNames();
	// Same reason: the camera columns follow the camera configuration, and a
	// no-op unless that really changed (it clears the table).
	rebuildEventColumns();
	int list = EventStore::instance().selectedList();
	const uint64_t jsonStartNs = os_gettime_ns();
	Data d(EventStore::instance().listJson(list));
	const int64_t jsonNs = (int64_t)(os_gettime_ns() - jsonStartNs);
	if (!d)
		return;
	QString needle = search_ ? search_->text().trimmed().toLower() : QString();

	// Remember the user's current selection so it survives the rebuild.
	int prevSel = 0;
	{
		auto sel = events_->selectionModel()->selectedRows();
		if (!sel.empty()) {
			QTableWidgetItem *it = events_->item(sel.first().row(),
							     kColId);
			if (it)
				prevSel = it->data(Qt::UserRole).toInt();
		}
	}

	refreshing_ = true;
	// Block selection signals during the rebuild: the row count changes below
	// and selectRow() re-sets the selection, and nothing downstream needs to
	// hear about a selection that is only being restored.
	QSignalBlocker selBlock(events_->selectionModel());
	// NOTE: the table is NOT emptied here any more. It used to be
	// setRowCount(0), which destroys every item and every angle-cell widget,
	// and the rows were then created from nothing — on every bump of the
	// store's version, so on every mark, every trim, every tick of a checkbox.
	// Measured: one row of two angle cells, 50-180 ms, almost all of it Qt
	// resolving this dock's 376 lines of style sheet for the five widgets a
	// cell is made of. Rows are reused in place below; see reuseCount.
	obs_data_array_t *arr = obs_data_get_array(d, "events");
	if (!arr) {
		refreshing_ = false;
		return;
	}
	const Qt::Alignment mid = Qt::AlignVCenter | Qt::AlignHCenter;
	// ONCE for the whole table, not once per cell. getConfig() copies the
	// entire Config under the core mutex; doing it per event per camera is
	// what made this function the longest thing in poll(). See buildAngleCell.
	const std::vector<std::string> commentPresets =
		ReplayCore::instance().getConfig().commentPresets;
	// The operator's vocabulary can also change in Settings, which does not go
	// through rememberComment. Same consequence for a reused cell, so it counts
	// as a move of the same version.
	if (commentPresets != lastCommentPresets_) {
		lastCommentPresets_ = commentPresets;
		commentVocabVersion_++;
	}
	// Where a slow rebuild goes. This function is the longest thing the dock's
	// poll does, and knowing THAT is not enough to fix it: hoisting a
	// whole-Config copy out of the per-cell path — the obvious culprit from
	// reading the code — moved the number not at all. So it is split the same
	// way poll() was, and for the same reason.
	const uint64_t rebuildStartNs = os_gettime_ns();
	int64_t cellBuildNs = 0, cellInsertNs = 0;
	int cellCount = 0, cellReused = 0;
	// The tallest angle cell built this pass; the rows are sized from it below.
	int tallestCell = 0;
	std::vector<std::pair<int64_t, int64_t>> rawMarkers;
	// ...and which event each of them belongs to, so its edge can be taken
	// hold of on the bar (SeekBar::markerDragged).
	std::vector<int> rawMarkerIds;

	// Marks are absolute instants on a monotonic clock that started with OBS,
	// so a column only means something relative to where this project's
	// footage begins. With NO footage there is no such origin, and printing
	// the raw instant produced the five-digit minute counts a reopened project
	// showed ("5648:09.557" for a mark taken four minutes into a take). A mark
	// we cannot place is shown as unplaceable.
	const int64_t originNs = eventOriginNs_;
	const bool haveOrigin = originNs != kNoInstant;
	auto relTc = [originNs, haveOrigin](int64_t ns) {
		if (!haveOrigin)
			return QStringLiteral("--:--.---");
		return formatTc(ns > originNs ? ns - originNs : 0);
	};

	// Can this mark still be played? The ring holds the last minutes; anything
	// older needs an anchored recording. An event of a session whose files were
	// never anchored can never play again, and saying so in the list beats a
	// row that does nothing when clicked.
	auto &tapRef = PacketTap::instance();
	const Config rowCfg = ReplayCore::instance().getConfig();
	auto footageExists = [&](int64_t ns) {
		if (SegmentIndex::instance().coversAnyCamera(ns))
			return true;
		for (int cam = 0; cam < kNCams; cam++) {
			if (rowCfg.cameras[cam].sourceName.empty())
				continue;
			const int64_t oldest = tapRef.oldestReplayableNs(cam);
			const int64_t newest = tapRef.newestNs(cam);
			if (oldest > 0 && ns >= oldest && ns <= newest)
				return true;
		}
		return false;
	};

	// The rows are COLLECTED first and rendered after, so the list can be drawn
	// in chronological order (the reference controller "sort events by time") rather than in the
	// order the marks were taken. The two orders diverge the moment a −20s
	// preset follows a −5s one, and during a match a list that is not in the
	// order things happened is read wrong, not slowly.
	struct Row {
		int id = 0;
		int64_t tin = 0;
		int64_t tout = -1;
		bool camOn[kEventAngles] = {};
		double camSpeeds[kEventAngles] = {};
		std::string note;
	};
	std::vector<Row> rows;

	size_t n = obs_data_array_count(arr);
	rows.reserve(n);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(arr, i);
		int id = (int)obs_data_get_int(e, "id");
		int64_t tin = obs_data_get_int(e, "tInNs");
		int64_t tout = obs_data_get_int(e, "tOutNs");

		bool camOn[kEventAngles] = {};
		const char *nt = obs_data_get_string(e, "note");
		const std::string note = nt ? nt : "";
		double camSpeeds[kEventAngles];
		for (int k = 0; k < kEventAngles; k++)
			camSpeeds[k] = -1.0;
		QString anglesStr;
		obs_data_array_t *aArr = obs_data_get_array(e, "angles");
		if (aArr) {
			size_t na = obs_data_array_count(aArr);
			for (size_t k = 0; k < na && k < (size_t)kEventAngles; k++) {
				obs_data_t *ad = obs_data_array_item(aArr, k);
				if (obs_data_get_bool(ad, "enabled")) {
					camOn[k] = true;
					anglesStr += QString::number(k + 1) + " ";
				}
				camSpeeds[k] = obs_data_has_user_value(ad, "speed")
						       ? obs_data_get_double(ad,
									     "speed")
						       : -1.0;
				obs_data_release(ad);
			}
			obs_data_array_release(aArr);
		}

		// Collect timeline marker for ALL events (regardless of search
		// filter) so the seekbar shows full density even when filtered.
		// Store raw ns — fractions computed each poll() tick using
		// displayDurNs_ so markers shift left as recording time grows.
		if (tin != kNoInstant && tout != kNoInstant && tout > tin)
			rawMarkers.push_back({tin, tout});
		if (tin != kNoInstant && tout != kNoInstant && tout > tin)
			rawMarkerIds.push_back(id);

		// search filter (id / the event comment / angles)
		if (!needle.isEmpty()) {
			QString hay = QString::number(id) + " " + anglesStr;
			if (!note.empty())
				hay += " " + QString::fromStdString(note).toLower();
			if (!hay.contains(needle)) {
				obs_data_release(e);
				continue;
			}
		}

		Row r;
		r.id = id;
		r.tin = tin;
		r.tout = tout;
		for (int k = 0; k < kEventAngles; k++) {
			r.camOn[k] = camOn[k];
			r.camSpeeds[k] = camSpeeds[k];
		}
		r.note = note;
		rows.push_back(std::move(r));

		obs_data_release(e);
	}
	obs_data_array_release(arr);

	if (rowCfg.sortEventsByTime)
		// Stable: two marks at the same instant keep the order they were
		// taken in, which is the only tie-break that means anything.
		std::stable_sort(rows.begin(), rows.end(),
				 [](const Row &a, const Row &b) {
					 return a.tin < b.tin;
				 });
	const int idDigits = std::clamp(rowCfg.eventIdDigits, 1, 8);

	// Set the text of a read-only cell, REUSING the item that is already
	// there. Creating a QTableWidgetItem is cheap; the row it hangs off is
	// not, and neither is the widget in the camera cell beside it.
	auto setRoCell = [&](int row, int col, const QString &txt,
			     Qt::Alignment al) -> QTableWidgetItem * {
		QTableWidgetItem *it = events_->item(row, col);
		if (!it) {
			it = new QTableWidgetItem(txt);
			it->setTextAlignment(al);
			it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
			events_->setItem(row, col, it);
			return it;
		}
		if (it->text() != txt)
			it->setText(txt);
		return it;
	};

	// Grow or shrink to fit; the rows that survive keep their items and their
	// angle cells. Shrinking destroys the surplus, which is what should happen
	// to a row that no longer exists.
	events_->setRowCount((int)rows.size());

	for (size_t ri = 0; ri < rows.size(); ri++) {
		const Row &r = rows[ri];
		const int row = (int)ri;

		const bool closed = r.tout != kNoInstant;
		QString dur = closed ? formatTc(r.tout - r.tin)
				     : QString::fromUtf8(
					       obs_module_text("Dock.Open"));

		// Nothing behind the mark: flag it here rather than letting the
		// operator find out by pressing play during a match. A closed
		// event shorter than a millisecond is the other unplayable kind:
		// Mark Out taken at the same instant as Mark In, which markOut
		// widens to exactly 1 ns so the event is "closed". No frame can
		// sit in that range - the last session's log has one, event 4,
		// failing with "no video frame at or after the requested
		// in-point" five times in a row.
		const bool degenerate = closed && r.tout - r.tin < 1'000'000;
		const bool playable = r.tin != kNoInstant && !degenerate &&
				      footageExists(r.tin);
		// the reference controller pads ids to a fixed width so they stay the same length for
		// the whole match and can be called out loud.
		const QString idText =
			QString("%1").arg(r.id, idDigits, 10, QLatin1Char('0'));
		QTableWidgetItem *idItem = setRoCell(
			row, kColId,
			playable ? idText : QStringLiteral("⚠ ") + idText,
			Qt::AlignCenter);
		idItem->setData(Qt::UserRole, r.id);
		// Reused rows carry the previous occupant's tooltip, so the
		// "no footage" mark has to be cleared as well as set.
		idItem->setToolTip(playable
					   ? QString()
					   : obs_module_text(
						     "Dock.EventNoFootage"));
		setRoCell(row, kColIn, relTc(r.tin), mid);
		setRoCell(row, kColOut,
			  closed ? relTc(r.tout) : QStringLiteral("--"), mid);
		setRoCell(row, kColDur, dur, mid);

		// THE COMMENT, once, in a column of its own. A WIDGET and not an
		// item: a double click on a row of this table already means "put
		// this event on air", so an editor that opens on one would reach
		// the Program from a cell whose whole job is a word.
		{
			QWidget *nc = events_->cellWidget(row, kColNote);
			if (!updateNoteCell(nc, r.id, r.note)) {
				nc = buildNoteCell(r.id, r.note, commentPresets);
				events_->setCellWidget(row, kColNote, nc);
				const int want = nc->sizeHint().height() + 2;
				if (want > tallestCell)
					tallestCell = want;
			}
		}

		// ONE cell per camera, holding the three things an operator says
		// about that angle: does it play, how fast, and what is it. They
		// were two columns and it read as two subjects; with four cameras
		// on screen the eye had to pair them up again every time. Now the
		// unit on screen is the unit in the operator's head — the angle —
		// and none of it is behind a dialog, because during a match an
		// edit that costs a dialog is an edit that does not get made.
		for (size_t ci = 0; ci < camCols_.size(); ci++) {
			const int cam = camCols_[ci];
			const int col = kColFirstCam + (int)ci * kColsPerCam;

			// REUSE FIRST. The widgets in this cell are connected to
			// an event and an angle; while those two are unchanged
			// the connections stay right and only the three values
			// have to be written. That is the ordinary case by a long
			// way — a mark appended, a point trimmed, an angle
			// ticked — and it is the difference between touching
			// three widgets and building five against a 376-line
			// style sheet.
			QWidget *cell = events_->cellWidget(row, col);
			if (updateAngleCell(cell, r.id, cam, r.camOn[cam],
					    r.camSpeeds[cam])) {
				cellReused++;
			} else {
				const uint64_t t0 = os_gettime_ns();
				cell = buildAngleCell(r.id, cam, r.camOn[cam],
						      r.camSpeeds[cam]);

				const uint64_t t1 = os_gettime_ns();
				// Replaces and deletes whatever was there.
				events_->setCellWidget(row, col, cell);
				// THE ROW IS SIZED FROM THE CELL, not from a
				// constant. It was 22 px against 28 px of
				// content, then 30 px chosen by adding up the
				// style sheet by hand — and it was still
				// clipping the bottom border, because a
				// stylesheet minimum is not a size hint: the
				// font metrics and the frame the combo actually
				// draws are bigger than the numbers in the
				// rule. Asking the built widget is the only
				// version of this that stays true when the
				// sheet or the font changes.
				const int want = cell->sizeHint().height() + 2;
				if (want > tallestCell)
					tallestCell = want;
				cellBuildNs += (int64_t)(t1 - t0);
				cellInsertNs += (int64_t)(os_gettime_ns() - t1);
				cellCount++;
			}
			// The cell still carries an item underneath: the gate and
			// the selection model both address rows through items, and
			// a cell that is only a widget is a hole in that.
			QTableWidgetItem *slot = events_->item(row, col);
			if (!slot) {
				slot = new QTableWidgetItem(QString());
				slot->setFlags(Qt::ItemIsSelectable |
					       Qt::ItemIsEnabled);
				events_->setItem(row, col, slot);
			}
			slot->setData(Qt::UserRole, r.id);
		}
	}
	// Grow the rows to whatever the cells turned out to need, once, and never
	// shrink them back: a row that changes height between refreshes makes the
	// whole list jump under the operator's eyes while he is reading it.
	if (tallestCell > events_->verticalHeader()->defaultSectionSize()) {
		events_->verticalHeader()->setDefaultSectionSize(tallestCell);
		obs_log(LOG_INFO, "[dock] event rows grown to %d px to fit their cells",
			tallestCell);
	}

	refreshing_ = false;
	// The rows were just rebuilt, so any widget cell is new and knows nothing
	// about the selection the view is still painting.
	tintSelectedCells();
	// Only when it actually hurt, and at most once every few seconds: a
	// rebuild is a normal thing that happens on every mark.
	const int64_t rebuildNs = (int64_t)(os_gettime_ns() - refreshStartNs);
	const int64_t rowsNs = (int64_t)(os_gettime_ns() - rebuildStartNs);
	if (rebuildNs > 50'000'000LL &&
	    (lastRebuildLogNs_ == 0 ||
	     (int64_t)(os_gettime_ns() - lastRebuildLogNs_) > 5'000'000'000LL)) {
		lastRebuildLogNs_ = os_gettime_ns();
		obs_log(LOG_INFO,
			"[ui] event table refreshed in %lld ms: %d row(s), %d angle "
			"cell(s) reused, %d rebuilt — listJson %lld ms, rows %lld ms "
			"(building cells %lld, inserting them %lld), the rest %lld ms",
			(long long)(rebuildNs / 1'000'000),
			events_->rowCount(), cellReused, cellCount,
			(long long)(jsonNs / 1'000'000),
			(long long)(rowsNs / 1'000'000),
			(long long)(cellBuildNs / 1'000'000),
			(long long)(cellInsertNs / 1'000'000),
			(long long)((rebuildNs - jsonNs - rowsNs) / 1'000'000));
	}
	markerNs_ = std::move(rawMarkers);
	markerIds_ = std::move(rawMarkerIds);
	// Fraction conversion happens in poll() each tick via displayDurNs_.

	// Keep exactly one event selected: when a newer event appears (a fresh
	// mark), auto-select it so "Riproduci selezionati" is one click; otherwise
	// preserve the user's selection across the rebuild.
	int maxId = 0;
	for (int r = 0; r < events_->rowCount(); r++) {
		QTableWidgetItem *it = events_->item(r, kColId);
		if (it)
			maxId = std::max(maxId, it->data(Qt::UserRole).toInt());
	}
	int target = (maxId > lastMaxEventId_) ? maxId
		     : (prevSel > 0)           ? prevSel
					       : maxId;
	lastMaxEventId_ = maxId;
	if (target > 0) {
		// This selection is OURS, not the operator's, so it must not cue:
		// a cue here would pull the preview off the live camera every time a
		// mark was taken. Only a row he picks himself loads a clip.
		reselecting_ = true;
		for (int r = 0; r < events_->rowCount(); r++) {
			QTableWidgetItem *it = events_->item(r, kColId);
			if (it && it->data(Qt::UserRole).toInt() == target) {
				events_->selectRow(r);
				// ...AND BRING IT INTO VIEW. Selecting a row off
				// the bottom of a scrolled list leaves the panel
				// showing the first marks of the match while the
				// one just taken is somewhere below the edge —
				// so the operator marks a goal and the table
				// does not move. Qt does not scroll for a
				// programmatic selection; it has to be asked.
				events_->scrollToItem(
					it, QAbstractItemView::EnsureVisible);
				break;
			}
		}
		reselecting_ = false;
	}
}

} // namespace multireplay
