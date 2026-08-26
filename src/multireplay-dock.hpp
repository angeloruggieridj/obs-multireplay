/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Native OBS dock panel. Drives the in-process engine (ReplayCore / ReplayChannel /
EventStore / PlaybackCoordinator) directly via C++ calls. Shows the replay
preview (the "Replay A" OBS input), a seekbar over the live timeline, marker
controls and the searchable, editable event list.

The timeline it draws is the MEASURED one: its live edge is the newest packet
the tap captured, and its start is the oldest instant still replayable (the
recorded files reach further back than the RAM ring). Every time the dock shows
or stores is an absolute master-timeline instant, displayed relative to that
start.
*/

#pragma once

#include "dock-layout.hpp"

#include <obs.h>
#include <util/platform.h>

// kNoInstant: the dock stores master-timeline instants, and "there is none" is
// not zero. See the note there before comparing one against 0.
#include "segment-index.hpp"
// The position bar's axis: recorded spans joined end to end, no wall-clock
// holes. Pure logic, no OBS types.
#include "timeline-map.hpp"
// Which (A/B) — the panel drives two channels now.
#include "replay-channel.hpp"

namespace multireplay {
class PlaybackCoordinator;
}

#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class QEvent;
class QKeyEvent;
class QPushButton;
class QToolButton;
class QSlider;
class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QTabBar;
class QTableWidget;
class QTableWidgetItem;
class QButtonGroup;
class QDockWidget;
class QSplitter;
class QTimer;
class QVBoxLayout;
class QGridLayout;
class QGroupBox;

namespace multireplay {

class OBSQTDisplay;

// ---------------------------------------------------------------------------
// RepaintCensus — how often a bar ASKS Qt to redraw it, and how often Qt
// actually does.
//
// This is not instrumentation for its own sake, it is the counter for a bug
// that took the whole OBS window black on a live rig. The dock's poll runs at
// 30 Hz, and two of the position bar's setters used to call update()
// unconditionally: the bar was therefore fully repainted thirty times a second
// forever — idle, not recording, playhead not moving, nothing on screen
// different from the tick before. Every one of those repaints makes Qt
// re-compose and flush the shared backing store of the OBS MAIN WINDOW, which
// is the same top-level that carries our native flip-model swap-chain children.
// That is exactly the GDI-over-flip-model combination qt-display.cpp warns
// about, and when it breaks it does not break the preview: it breaks the flush,
// so every pixel Qt painted in the OBS window goes black while the swap-chain
// previews keep drawing. The operator gets it back by clicking around until a
// repaint lands.
//
// So "how many repaints per second is this panel asking for" is a number that
// has to stay near zero when nothing is happening, and it has to be readable
// from the OBS log alone — the machine this happens on is a rig in a gallery,
// not one we can attach a debugger to.
// ---------------------------------------------------------------------------
struct RepaintCensus {
	std::atomic<uint64_t> requested{0};  // handed to Qt
	std::atomic<uint64_t> suppressed{0}; // asked for; nothing had changed
	std::atomic<uint64_t> coalesced{0};  // folded into a later repaint
	std::atomic<uint64_t> served{0};     // paintEvent bodies actually run

	void reset()
	{
		requested = 0;
		suppressed = 0;
		coalesced = 0;
		served = 0;
	}
};

// One per bar rather than one shared counter: if the panel still goes black
// after the guards, the first question is WHICH bar is still asking, and a
// single total cannot answer it.
extern RepaintCensus g_seekCensus;
extern RepaintCensus g_clipCensus;

// ---------------------------------------------------------------------------
// SeekBar — the graduated position bar over the recorded project timeline.
//
// A flat custom-painted bar (not a bead-on-rail QSlider): it draws the full
// recorded timeline, highlights the seekable region (footage already flushed
// to disk), fills the played-up-to-position portion with the accent colour and
// renders a slim handle at the playhead. Clicking or dragging emits fractions
// in [0,1]; the host maps them onto the master timeline.
//
// It is GRADUATED, along a ruler strip of its own under the track: regular time
// marks, labelled where the labels fit. Without them the bar is a coloured
// rectangle — the operator can see that something moves in it, but not how far
// back "back" is, and a scrubber whose scale has to be guessed is one nobody
// recognises as a scrubber. The step is chosen from the timeline's own length
// (see tickStepNs), so a 40 s take is marked every 5 s and a two-hour one every
// five minutes.
//
// When there is no timeline (setTimeline(0, …)) it does NOT draw an empty
// track: it goes flat, refuses the mouse and prints the reason it was given.
// An inert rectangle there reads as a broken widget.
//
// That state used to be reached by REOPENING a project, which was wrong: the
// footage was on disk and every event still played, but the timeline was
// measured to the live edge and outside a take there is no live edge, so hours
// of usable material drew a flat bar saying there was nothing to scrub. The
// timeline now ends where the FOOTAGE ends (SegmentIndex::newestNs), so the
// remaining ways to get here are honest ones: nothing recorded yet, or
// recordings that do not say how long they are.
// ---------------------------------------------------------------------------
class SeekBar : public QWidget {
	Q_OBJECT

public:
	explicit SeekBar(QWidget *parent = nullptr);

	// position/duration/seekable expressed as fractions of the timeline
	// [0,1]; -1 (default seekableFrac) means "whole bar is seekable".
	void setProgress(double positionFrac, double seekableFrac = 1.0);
	// How long the timeline the bar stands for is, in ns, and what to say
	// when it is 0. The duration is what the graduations are computed from:
	// fractions alone cannot say whether the bar spans forty seconds or two
	// hours, and that is exactly what a scale has to say.
	void setTimeline(int64_t durationNs, const QString &emptyHint);
	bool hasTimeline() const { return durationNs_ > 0; }
	// How long that timeline is. For the automated gate: "the bar is not
	// flat" and "the bar spans the footage that is actually on disk" are
	// different claims, and the reopened-project check needs the second one.
	int64_t timelineNs() const { return durationNs_; }
	// Labelled graduations the bar is drawing at its current width, 0 when
	// there is no timeline. For the automated gate — see LayoutProbe.
	int graduations() const;
	// Event markers drawn on the timeline as amber rectangles.
	// Each pair is (inFrac, outFrac) in [0,1].
	//
	// BY REFERENCE, not by value, and the reason is the repaint census above:
	// the host hands these over on every 30 Hz tick, so the setter's first job
	// is to notice that they are the same as last tick and do nothing at all.
	// A by-value parameter would have moved the caller's buffer away before
	// that comparison could be made, which is why the caller had to build two
	// fresh vectors thirty times a second to begin with.
	void setEventMarkers(const std::vector<std::pair<double, double>> &markers);
	// Which event each marker belongs to, in the same order. Without it a
	// marker is a rectangle nobody can edit; with it, grabbing its edge
	// moves that event's point (see the markerDragged signal).
	void setEventMarkerIds(const std::vector<int> &ids);
	// The pixel of a fraction of the whole timeline. Public because the
	// painter, the mouse handlers and the automated gate all have to agree
	// on where a marker edge IS, and three copies of that arithmetic would
	// be three things to keep in step.
	int xForFraction(double frac) const;
	size_t markerCount() const { return markers_.size(); }
	std::pair<double, double> markerAt(size_t i) const
	{
		return i < markers_.size() ? markers_[i]
					   : std::pair<double, double>{0.0, 0.0};
	}

	// --- zoom -------------------------------------------------------
	// The bar draws a WINDOW of the timeline, not always the whole of it.
	// Everything crossing this class's boundary stays in fractions of the
	// WHOLE timeline — the host knows nothing about the window — so the
	// zoom is a property of the control, which is what it is: at an hour of
	// footage on a 900 px bar one pixel is four seconds, and an operator
	// trimming an in-point cannot aim at a frame with that.
	//
	// zoom = 1 shows everything; 4 shows a quarter of it; the visible span
	// is 1/zoom of the timeline, centred on centreFrac and clamped to the
	// ends (a window that ran off the edge would show emptiness the
	// timeline does not have).
	void setZoom(double zoom, double centreFrac);
	double zoom() const { return zoom_; }
	// Keep `frac` inside the window, panning as little as it takes. Called
	// as the playhead moves so a zoomed bar follows the picture instead of
	// being left behind by it.
	void ensureVisible(double frac);
	double viewStart() const { return viewStart_; }
	double viewSpan() const { return viewSpan_; }
	// the reference controller prints the transport state ON the position bar ("0000 - 00:11.56
	// 100%") instead of beside it, and that is where the operator's eye already
	// is while he scrubs. Drawn centred, over the fill.
	void setOverlayText(const QString &text);
	bool dragging() const { return dragging_; }

signals:
	void scrubStateChanged(bool dragging); // press(true) / release(false)
	void scrubMoved(double frac);          // live drag/hover position
	void seekRequested(double frac);       // committed on release/click
	void zoomChanged(double zoom);         // so the panel can show it
	// An event's IN or OUT was dragged to `frac`. The bar knows nothing
	// about events: it reports the gesture and the host moves the point.
	void markerDragged(int eventId, bool inPoint, double frac);

protected:
	void paintEvent(QPaintEvent *) override;
	// Only to forget the pixel the playhead was last drawn at: it is what
	// setProgress compares against, and it means nothing at a new width.
	void resizeEvent(QResizeEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	// Wheel = zoom about the cursor. On a control whose only job is
	// position, the wheel has nothing else to mean, and zooming about the
	// pointer keeps the frame under it still while the scale changes.
	void wheelEvent(QWheelEvent *) override;

private:
	// --- repaint routing -------------------------------------------------
	// Nothing in this class calls QWidget::update() directly any more. It
	// goes through one of these two, and which one is a statement about what
	// changed:
	//
	//   repaintNow(rect)   the playhead moved. Small, immediate, 30 Hz: the
	//                      dirty band is the few pixels between where the
	//                      hairline was and where it is, so the flush Qt
	//                      makes of the OBS window's backing store is a few
	//                      pixels wide instead of the whole bar.
	//   repaintSoon(rect)  everything else — the graduations, the markers,
	//                      the timecode printed on the track. All of these
	//                      change CONTINUOUSLY while recording, because the
	//                      timeline they are drawn against is growing, and
	//                      all of them change by a THIRD OF A PIXEL per tick.
	//                      Redrawing them thirty times a second buys nothing
	//                      an operator can see and costs a full-width flush
	//                      every 33 ms, so they are coalesced to
	//                      kCoalesceMs and the accumulated rect is repainted
	//                      once. A gesture in progress (a drag) goes through
	//                      repaintNow: that one has to track the hand.
	void repaintNow(const QRect &r);
	void repaintSoon(const QRect &r);
	// The whole widget, as a rect. Used where a change really does alter
	// everything (a new timeline length re-graduates the ruler).
	QRect allOfIt() const { return QRect(0, 0, width(), height()); }
	// The track band only — above the ruler. The fill, the markers, the
	// playhead and the overlay text live in here; the graduations and their
	// labels, which are the expensive part of a repaint, do not.
	QRect trackBand() const;

	double fracAt(int x) const;
	// Time between two LABELLED graduations at the current width, 0 when
	// there is nothing to graduate. One function, so what paintEvent draws
	// and what graduations() reports cannot drift apart — a gate reading a
	// second copy of this arithmetic would be checking itself.
	int64_t tickStepNs() const;
	// Usable track width in pixels (the ruler runs the same width).
	int trackWidth() const;

	double positionFrac_ = 0.0;
	double seekableFrac_ = 1.0;
	double dragFrac_ = 0.0;
	bool dragging_ = false;
	QString overlay_;
	// Length of the timeline drawn, in ns. 0 = there is none, and the bar
	// says so (emptyHint_) rather than pretending to be one.
	int64_t durationNs_ = 0;
	QString emptyHint_;
	std::vector<std::pair<double, double>> markers_;
	std::vector<int> markerIds_;
	// The edge being dragged: which marker, and which end of it. -1 = the
	// gesture in progress is an ordinary scrub.
	int dragMarker_ = -1;
	bool dragMarkerIn_ = true;
	// Is x within grabbing distance of a marker edge? Fills the two fields
	// above when it is.
	bool findMarkerEdge(int x, int &marker, bool &inPoint) const;
	// The window on the timeline, in fractions of the whole (see setZoom).
	double zoom_ = 1.0;
	double viewStart_ = 0.0;
	double viewSpan_ = 1.0;
	// Fraction of the whole timeline → fraction of the window, and back.
	double toView(double frac) const
	{
		return viewSpan_ > 0 ? (frac - viewStart_) / viewSpan_ : frac;
	}
	double fromView(double v) const { return viewStart_ + v * viewSpan_; }

	// --- coalescing state ------------------------------------------------
	// The rect a deferred repaint has accumulated, and the timer that flushes
	// it. Null rect = nothing pending. The timer is single-shot and restarted
	// only when the rect goes from empty to non-empty, so a burst of changes
	// inside one window costs one repaint, not one per change.
	QRect pendingRect_;
	QTimer *coalesceTimer_ = nullptr;
	// Last position we asked Qt to draw the playhead at, in widget pixels.
	// The comparison that decides whether a new position is worth a repaint
	// is made HERE, in pixels, and not on the fraction: while recording the
	// timeline grows every tick, so the fraction is never twice the same and
	// comparing fractions would suppress exactly nothing.
	int lastDrawnPlayheadX_ = INT_MIN;
	void flushPending();
};

// ---------------------------------------------------------------------------
// ClipBar — the green band across the bottom: WHAT IS ON AIR.
//
// It is not a scrubber and must never be mistaken for one (the SeekBar above
// does that job, and lives right underneath it). This bar answers one question
// and answers it from across the room: which clip is playing, on which angle,
// how much of it is left, and at what speed. The fill is its progress through
// that clip, so the operator can see the end coming without reading a number.
//
// Green, filled and bright while something is on air; dim while the bar is only
// describing the clip that WOULD play. the reference controller prints the same information in the
// same place, and an operator reads a colour before he reads a label.
// ---------------------------------------------------------------------------
class ClipBar : public QWidget {
	Q_OBJECT

public:
	explicit ClipBar(QWidget *parent = nullptr);

	// progress in [0,1]; `text` is drawn centred over the fill.
	//
	// `clipJoins` are the fractions where one clip of the SEQUENCE ends and
	// the next begins, drawn as white uprights. With more than one clip queued
	// the progress is the progress through the whole sequence, not through the
	// clip on air: a band that filled up and started again three times told the
	// operator nothing about when the replay would be over, which is the one
	// thing it is there to say. The uprights are what keeps the individual
	// clips visible inside that.
	void setState(double progressFrac, const QString &text, bool onAir,
		      const std::vector<double> &clipJoins = {});

	// What the band is currently saying. For the automated gate: "the bar
	// exists" is not the claim worth checking — "while a clip is on air the
	// bar names it, and its fill has moved" is.
	QString overlayText() const { return text_; }
	double progress() const { return progress_; }
	bool onAir() const { return onAir_; }
	size_t clipJoinCount() const { return joins_.size(); }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	double progress_ = 0.0;
	QString text_;
	bool onAir_ = false;
	std::vector<double> joins_;

	// Same coalescing as the SeekBar, and for the same reason: while a clip
	// runs, the remaining time printed here is in hundredths, so the text
	// differs on every 30 Hz tick and the guard in setState lets every one of
	// them through. The band is small, but a repaint of it is still a flush of
	// the OBS window's backing store. Ten a second is smooth on a bar that
	// takes seconds to fill and is a third of the flushes.
	QTimer *coalesceTimer_ = nullptr;
	bool pending_ = false;
	void repaintSoon();
};

class MultiReplayDock : public QWidget {
	Q_OBJECT

public:
	explicit MultiReplayDock(QWidget *parent = nullptr);
	~MultiReplayDock() override;

	// Drop the owned source references the dock publishes for the graphics
	// thread. Static and reachable from the module's frontend handler, because
	// OBS clears scene data long before the dock is destroyed and a reference
	// still held at that moment is reported to the operator as a plugin that
	// failed to release its resources.
	static void releasePreviewRefs();
	// OBS is going away: stop polling AND let go. The timer re-resolves the
	// references on its own beat, so releasing without stopping it leaves a tick
	// free to take a fresh one before OBS clears scene data.
	static void prepareForShutdown();
	// How many owned source references the dock is holding right now (the two
	// previews plus the multiview tiles). For the automated gate: after the
	// "OBS is about to clear scene data" hook has run this must be ZERO, and
	// nothing else outside the dock can see it. A reference still held there is
	// what OBS reports to the operator as a plugin that failed to release its
	// resources — in a dialog, on the way out, where it is least welcome.
	int heldSourceRefs() const;
	// Does this installation still need the guided setup? Public for the
	// automated gate: the harness configures a rig, so the answer there must
	// be NO — and the way this goes wrong is a wizard in the face of an
	// operator whose panel has been set up for months.
	bool needsSetup();


	// Put the position bar back over the whole timeline — the zoom menu's
	// "100%" entry, and the way back from any span. Public because the gate
	// drives it directly: clicking the key opens a modal menu, which would park
	// the checking thread inside QMenu::exec().
	void zoomWholeTimeline();

	// Type into the search box, as the operator would. For the automated
	// gate: filtering hides rows and clearing the filter brings them back,
	// and what comes back has to be what the store holds. Driving the real
	// widget is the point — the rebuild it triggers is what is on trial.
	void setSearchText(const QString &text);

	// What the preview is showing: true = the replay (a clip, a scrub review,
	// or the frame the last one ended on), false = the live camera mirror.
	//
	// Published for the automated gate, which is the only thing outside the
	// dock that reads it: "the preview flipped back to the live camera between
	// two clips of a sequence" is invisible to every other check, and it is
	// exactly what put the wrong picture on air. Atomic because the gate samples
	// it from its own thread while poll() writes it on the UI thread.
	bool previewShowsReplay() const { return previewShowsReplay_.load(); }

	// WHAT THE ANGLE BOXES ARE SHOWING, for the automated gate.
	//
	// "The tiles follow the review" cannot be seen from outside any other way:
	// a tile black because its feed never played and a tile black because the
	// camera has no signal are the same rectangle, and a tile still mirroring
	// the live camera during a replay is the bug this exists to catch — it
	// looks perfectly fine until you compare it with the picture on air.
	struct MultiviewState {
		// false = the boxes mirror the live cameras (follow-live), true =
		// they are showing the moment being reviewed.
		bool followingReview = false;
		int feeds = 0;            // preview feeds created, one per camera
		int feedsWithPicture = 0; // ...of which have ever shown a picture
		// Camera tiles with a source PUBLISHED for the graphics thread
		// right now. This is the number the operator sees: a tile whose
		// source has been withdrawn is a black rectangle, however healthy
		// the feed behind it is — and withdrawing it at every cue, for a
		// quarter of a second, is exactly the asymmetry that was reported
		// from a real panel. Nothing else outside the dock can see it.
		int tilesPublished = 0;
		// Feeds that have pushed a frame of the clip they are on RIGHT
		// NOW. Per-clip, so it drops back to zero at every cue — which is
		// exactly why it must never gate the publish (see
		// tileFeedHadPicture_), and exactly what makes it the right thing
		// to time a cue with: "when did the boxes get this picture" is a
		// question about this clip, not about the feed's history.
		int feedsWithCurrentClip = 0;
		// The range every feed was last cued to. kNoInstant = never.
		int64_t cueInNs = kNoInstant;
		int64_t cueOutNs = kNoInstant;
	};
	MultiviewState multiviewState() const;

	// The in-point of the FREE REVIEW armed on the position bar, or kNoInstant
	// when there is none. Published for the gate, which has to tell "Play
	// events put the free section on air" apart from "Play events replayed the
	// selected event" — the same queue, with different instants in it.
	int64_t freeReviewInNs() const { return freeReviewInNs_; }

	// What the preview area actually amounts to, for the automated gate.
	// Counting OBSQTDisplay children from outside would say how many widgets
	// exist but not whether each got a real obs_display, and "the tile is
	// there but black" is precisely the failure a multiview can have.
	// UI thread only.
	struct PreviewStats {
		int tiles = 0;       // preview widgets built (big + every tile)
		int visible = 0;     // ...of which on screen
		int withDisplay = 0; // ...of which own a live obs_display
		// ...and of the rest, how many have been WAITING for one longer
		// than any start-up delay explains. "withDisplay == visible" was
		// the whole check, and it is trivially true when nothing is
		// visible — so a dock that never got on screen passed it, and so
		// did the first instants of every run. This is the number that
		// says a rectangle is black because nothing is behind it.
		int starved = 0;
		int64_t worstBlockedMs = 0; // longest current dry spell
	};
	PreviewStats previewStats() const;

	// Where the panel's zones actually ended up, and what the position bar
	// amounts to. Published for the automated gate.
	//
	// Both claims here are ones no widget check can make. "The tabs are above
	// the pictures" was true for a whole milestone while every check that
	// FINDS those widgets passed — they were all present, in the wrong order.
	// And "there is a SeekBar" stayed true the entire time the operator was
	// telling us he could not see one: a bar with no graduations and no
	// timeline behind it is a rectangle. So the order is read off real
	// geometry, and the bar is asked how many marks it is drawing.
	//
	// y coordinates are in the dock's own coordinate system; -1 = no widget.
	// UI thread only.
	struct LayoutProbe {
		int previewBottomY = -1; // lowest edge of the picture block
		int searchY = -1;
		int listTabsY = -1;
		int tableY = -1;
		int clipBarY = -1;
		int seekY = -1;
		int seekHeight = 0;
		int seekGraduations = 0;
		// Is the second bay on screen at all? With Config.enableChannelB
		// off the B box, the A|B/A/B selector and the swap key are ABSENT,
		// not disabled, and nothing else outside the dock can see that.
		bool channelBVisible = false;  // labelled marks the bar draws now
		bool seekEnabled = false; // false = no timeline to scrub
	};
	LayoutProbe layoutProbe() const;

	// Move the selected event's IN or OUT by a plain amount of time. Public
	// for the same reason layoutProbe is: the gate drives the real thing.
	// The keys that call it are registered hotkeys, which have no widget to
	// click (see registerDockHotkeys).
	void nudgeSelectedPointNs(bool inPoint, int64_t deltaNs);

	// Event table geometry. Public because the automated gate reads and edits
	// REAL cells: given only a column count it could not tell "the per-event
	// speed column is gone" from "the table is built differently today", and a
	// gate that hardcodes 4 stops meaning anything the moment this moves.
	//
	// kColId must stay 0 and kColIn 1 — the gate reads the padded id off
	// item(row, 0) and double-clicks column 1 to prove it takes program.
	enum EventColumn {
		kColId = 0,
		kColIn,
		kColOut,
		kColDur,
		// WHAT THE EVENT IS, once. It used to be asked once per camera, and
		// in practice was answered on one of them and left blank on the rest
		// - a goal is a goal on every lens that saw it.
		kColNote,
		kColFirstCam // first per-camera cell
	};
	// ONE column per configured camera, holding the three things an operator
	// says about that angle — plays / how fast / what it is — in a single
	// cell widget (see buildAngleCell). It was two columns and read as two
	// subjects. See the note above EventColumn's definition in the .cpp.
	static constexpr int kColsPerCam = 1;

	// What an OBS hotkey callback is handed (see registerDockHotkeys). Public
	// only because that callback is a plain C function, as libobs requires.
	struct HotkeyCtx {
		MultiReplayDock *dock = nullptr;
		std::function<void(MultiReplayDock *)> fn;
	};

private:
	// --- UI assembly ---
	// Zoned the way the broadcast replay controller is zoned, top to bottom:
	// the channel A preview + multiview with its green status strip FIRST,
	// then the list header (search + Live, then the list tabs), then the
	// event table, then the two rows of controls, the green on-air band and
	// the full-width position bar. buildBottomBar() owns the last three.
	//
	// The pictures lead. Everything that picks WHICH event — the tabs, the
	// search box, Live — belongs to the list and lives with it, between the
	// pictures and the table it filters (see layoutProbe: the gate checks
	// that order on real geometry, because every widget was present and
	// findable the whole time it was wrong).
	QWidget *buildToolbar();
	QWidget *buildPreview();
	QWidget *buildMultiview();
	// The six sections of the control strip, in the order they are added to
	// it: REC | MARK | ANGOLI | RIPRODUZIONE | VELOCITA | EXPORT. Each one
	// declares a tall shape and a flat one and lets ControlStrip decide (see
	// dock-layout.hpp).
	// The gear and its menu — Settings, projects, tags, chapters. It lives in
	// the TOOLBAR with the other panel-wide keys, not inside the record
	// section where it read as part of arming a take.
	QToolButton *buildGearMenu();
	KeyBlock *buildRecBlock();
	KeyBlock *buildMarkers();
	KeyBlock *buildAngleMatrix();
	KeyBlock *buildTransport();
	KeyBlock *buildSpeedBlock();
	KeyBlock *buildExportBlock();
	// The single Export key, which asks on the press whether it is one clip or
	// the whole selection as one file. Built separately because the SPEED
	// section places it — under the dial, with the rest of what is done to a
	// clip once it is marked.
	QPushButton *buildExportKey();
	ControlStrip *strip_ = nullptr;
	// The camera section, kept because switching the second bay on or off
	// changes how many rows it has and the strip has to be told.
	KeyBlock *angleBlock_ = nullptr;
	QWidget *buildEvents();
	QWidget *buildBottomBar();
	// The status line, above the on-air band. It OWNS the modes — loop, music,
	// "in output" — rather than mirroring keys that live somewhere else.
	QWidget *buildStatusBar(QWidget *parent);
	// How tall it is. Shorter than a key row because nothing in it is reached
	// for blind: these are pressed while being looked at.
	// 26, NOT 22. The three toggles on this line are pinned to four less
	// than it, and at 18 px the frame the style draws is exactly 18 too - so
	// the bottom border sat on the widget edge and any rounding put it past.
	// Four more pixels is a border that can be seen rather than deduced.
	static constexpr int kStatusBarH = 26;

	// --- engine interaction ---
	void poll();             // periodic transport/status refresh
	void refreshEvents();    // reload the selected list into the table
	void refreshAngles();    // update angle button labels from camera displayName
	// the reference controller: the 20 lists can be named ("Gol", "Falli"). Re-labels the tabs
	// from the store; the selection is preserved.
	void refreshListNames();
	// One table column per configured camera, headed "N Name" (the reference controller). The set
	// of columns follows the camera configuration, so this rebuilds them (and
	// only when the configuration really changed — rebuilding clears the
	// table).
	void rebuildEventColumns();
	// The per-angle cell: [☑] [speed ▾] [comment ▾]. Owned by the table.
	// The speed dial. Built separately because buildTransport() PLACES it
	// (under the keys) and a widget cannot be added to a layout before it
	// exists.
	void buildSpeedDial();
	// Show one angle key per CONFIGURED camera, labelled with its name. Called
	// on the slow beat of poll(); a no-op unless the cameras changed.
	// The member half of releasePreviewRefs(), so the destructor need not go
	// through the static pointer it has already cleared.
	void dropPreviewRefs();
	// Bring the panel into line with Config.enableChannelB: with one bay there
	// is no B box, no A|B/A/B selector, no swap key and no B angle row, and
	// every command means A. Called at startup and whenever Settings is saved;
	// a no-op unless the answer changed.
	void applyChannelBVisibility();
	bool channelBEnabled_ = false;
	// -1 = never applied, so the first call always runs.
	int channelBApplied_ = -1;
	// Stop both bays and forget what they were showing (a new or newly opened
	// project is empty on both).
	void clearBothBays();
	AspectBox *bBox_ = nullptr;          // B's bay, absent when B is off
	// the reference controller's Monitors key and what it takes away: the whole monitoring block
	// (both decks, the camera previews) and the green strip that belongs under it.
	QPushButton *monitorsBtn_ = nullptr;
	QWidget *monitorsRow_ = nullptr;
	QWidget *monitorsStrip_ = nullptr;
	// The splitter pane the two rows above live in. Hiding the rows is not
	// enough: the splitter keeps the pane's share of the height and the list
	// below it stays exactly as short as it was, with a band of nothing on
	// top. A hidden splitter child gives its height back (and takes its handle
	// with it), which is what "put the monitors down" has to mean.
	QWidget *previewPane_ = nullptr;
	// Applies the Monitors key to the pane and the rows inside it.
	void applyMonitorsVisible(bool on);

	// ─── ⛶ THE PANEL TO THE WHOLE SCREEN, AND ONLY WHILE IT FLOATS ───────
	//
	// An operator who pulls this dock out onto a second monitor wants it to
	// fill that monitor. Dragging the four edges of a floating window to the
	// four edges of a screen is a fiddle, and it is a fiddle he does again
	// every time OBS restores a layout that remembered slightly different
	// numbers.
	//
	// DOCKED, THE KEY IS NOT THERE AT ALL — not greyed out. Inside the main
	// window there is nothing to make full screen (OBS owns that window), so
	// the key would do nothing, and a key that does nothing is a key the eye
	// has to discard every time it reads the row. Same rule as the unconfigured
	// angle slots.
	//
	// It works by putting the QDockWidget OBS wrapped us in into full screen —
	// NOT by re-parenting this widget into a window of its own. Re-parenting
	// destroys the native window of every OBSQTDisplay underneath (the bays and
	// every multiview tile) and strands their obs_display; a window-state change
	// leaves every child handle exactly where it was.
	QPushButton *fullScreenBtn_ = nullptr;
	// Where the floating window was before it took the screen. Qt restores a
	// geometry of its own on showNormal(), but a QDockWidget that has been
	// floated, docked and floated again has had that memory rewritten under it
	// more than once; this one is ours and is only ever written here.
	QRect preFullScreenGeom_;
	// -1 = never applied, so the first call always runs. Same idiom as
	// channelBApplied_: isVisible() is false for a child of a closed dock too,
	// which would make the comparison ask for a show on every single tick.
	int fullScreenKeyShown_ = -1;
	// The QDockWidget OBS wrapped this widget in, or null while docked in
	// nothing. Resolved by walking up, and resolved LAZILY: at construction our
	// parent is the main window — obs_frontend_add_dock_by_id re-parents us
	// afterwards (see plugin-main.cpp), so there is nothing to cache in the
	// constructor.
	QDockWidget *hostDock() const;
	bool panelIsFullScreen() const;
	void setPanelFullScreen(bool on);
	// Show the key iff we are floating, and keep its lit state in step with the
	// window. Called on the slow beat of poll() — floating and full screen are
	// both deliberate, rare gestures — and a no-op unless the answer changed.
	// Polled rather than wired to QDockWidget::topLevelChanged because the dock
	// that wraps us is not ours: OBS creates it, hides it, and can hand the
	// layout back a different one.
	void refreshFullScreenKey();
	// Maximised is not "a window the size of a maximised one": the moment
	// anything moves they behave differently, so leaving full screen has to put
	// back the state, not the rectangle.
	bool preFullScreenMaximized_ = false;
	// The QDockWidget we have installed an event filter on, and the two things
	// that filter is for. See eventFilter.
	//
	//  - A DOUBLE CLICK ON THE TITLE BAR. Qt answers one by RE-DOCKING the
	//    panel (QDockWidgetPrivate::nonClientAreaMouseEvent → _q_toggleTopLevel),
	//    and OBS then puts it back wherever the layout says — which, if that
	//    place is behind a tab, means the panel the operator was working in
	//    simply disappears. Everywhere else a double click on a title bar
	//    MAXIMISES the window, and with a native frame that is already what
	//    the platform does behind Qt (a non-client message falls through to
	//    DefWindowProc) — so BLOCKING Qt's handler is the whole fix, and doing
	//    the maximise as well made the panel shrink and grow straight back.
	//    Where the title bar is drawn by Qt (xcb, Wayland) the toggle is ours.
	//  - Qt does not ask for Maximize when it floats a dock, so the window
	//    comes up with it greyed out in the system menu. It is put back (see
	//    equipFloatingWindow) — reapplied on every float, because Qt rewrites
	//    the flags each time it makes one. Minimize deliberately is NOT: an
	//    owned window gets no taskbar button, so that box is a one-way door.
	QPointer<QDockWidget> filteredHost_;
	void equipFloatingWindow(QDockWidget *host);
	// Says the "brought a minimised panel back" line once, not four times a
	// second for as long as the rescue keeps not sticking.
	bool minimizeRescueLogged_ = false;
	// THE JOIN between two clips of one sequence. The coordinator advances the
	// queue on the finished callback, but the engine has not pushed a frame of
	// the next clip yet — so for those few tens of milliseconds positionNs()
	// still reports the LAST frame of the clip that just ended. Read literally
	// that says "this clip is over" about a clip that has not begun, and the
	// green band filled to 100% at every white join and then dropped back.
	// While waiting, the fill is pinned to the join; any change in the reported
	// position clears the wait, because a finished clip pushes nothing.
	int clipBarQueuePos_ = 0;
	int64_t clipBarJoinPos_ = 0;
	bool clipBarAtJoin_ = false;
	// A comment typed on one event, offered on all of them for the rest of the
	// session. Not persisted — see rememberComment() for why a comment must
	// not be able to reach setConfig().
	QStringList sessionComments_;
	static constexpr int kMaxSessionComments = 24;
	// Set when the comment list grew, or when a rebuild had to be deferred
	// because an editor was open. poll() clears it by rebuilding.
	bool commentsDirty_ = false;
	bool eventsDirty_ = false;
	// True while the dock is re-selecting a row itself (refreshEvents keeps the
	// newest mark selected). Distinguishes "the operator picked this event",
	// which cues it, from "the table was rebuilt", which must not.
	bool reselecting_ = false;
	// Where the take being recorded began (the newest anchor on the angle being
	// drawn), so the position bar can span it. The file itself cannot say how
	// long it is until the muxer lets go of it, and without this the bar
	// described only the ring — see the span assembly in poll().
	int64_t takeAnchorNs_ = kNoInstant;

	// The camera key at its full size — a renamed camera cannot stretch it,
	// so the row never moves out from under the operator's fingers…
	static constexpr int kAngleKeyWidth = 64;
	// …and the size it will compress to when the dock is narrow. Eight fixed
	// 64 px slots are 560 px this panel could never give back, which is what
	// stopped the dock from being made narrower at all; with a floor instead,
	// the matrix keeps all eight columns and simply draws them tighter. The
	// name is re-elided at the width the key actually got (refreshAngleRows).
	static constexpr int kAngleKeyMinWidth = 38;
	// The bay selector and the swap: narrower than a camera key, because they
	// carry one or two characters rather than a camera's name.
	static constexpr int kChanKeyWidth = 38;
	// The "A" / "B" prefix column. Fixed too, and for the same reason one
	// step out: it is the lead-in of all three rows of the matrix (A, B, and
	// the A|B / A / B selector under them), so the keys of the three rows
	// stand on one set of columns.
	static constexpr int kAngleLabelWidth = 12;
	// 8 = kMaxCameras (replay-core.hpp, which this header does not include —
	// the .cpp static_asserts the two agree).
	// What the rows were last built for; refreshAngleRows() compares against it
	// rather than rewriting eight labels thirty times a second.

	// Centre every row of a combo's dropdown, in both directions. Needs code
	// rather than a stylesheet: item text goes through the view's delegate.
	static void centreComboItems(QComboBox *cb);

	// `presets` is the operator's comment vocabulary, read ONCE by the caller
	// and handed down. Reading it here meant a whole-Config copy under the
	// core mutex per cell — per event, per camera — and that was the longest
	// thing the dock's poll did on a real session. See buildAngleCell.
	QWidget *buildAngleCell(int eventId, int cam0, bool on, double speed);
	// THE EVENT COMMENT, one per row, right of the duration: text at rest, a
	// caret on the first click and the vocabulary on a chooser beside it.
	QWidget *buildNoteCell(int eventId, const std::string &note,
			       const std::vector<std::string> &presets);
	bool updateNoteCell(QWidget *cell, int eventId, const std::string &note);
	// The fast path: write the three values into a cell that already belongs
	// to this event and angle. False = it does not, and the caller must build
	// a new one. See the note on the function.
	bool updateAngleCell(QWidget *cell, int eventId, int cam0, bool on,
			     double speed);
	// Bumped whenever the list of comments a cell would offer changes — a word
	// typed on any event, or the presets edited in Settings. A cell records the
	// version it was built with; a newer one means it cannot be reused, because
	// its list would be missing the word just added.
	uint64_t commentVocabVersion_ = 0;
	std::vector<std::string> lastCommentPresets_;

	// --- trimming an event already marked --------------------------------
	// A live mark is late by definition: the operator saw the action first.
	// These move the SELECTED event's point — to the position bar
	// (setSelectedPoint) or by whole frames (nudgeSelectedPoint, on hotkeys
	// for the Stream Deck). Both go through EventStore::movePoint, so the
	// clamping rules live in one tested place.
	void setSelectedPoint(bool inPoint);
	void nudgeSelectedPoint(bool inPoint, int frames);
	// The same in SECONDS, for the Stream Deck: "the mark is two seconds
	// late" is how an operator thinks, and a frame key pressed sixty times
	// is not an answer to it.
	// An event edge was dragged on the position bar (SeekBar::markerDragged).
	void onMarkerDragged(int eventId, bool inPoint, double frac);

	// --- which replay channel the controls drive (the reference controller's A|B / A / B) ----
	// The panel is one set of controls over two channels, so every command
	// has to know where it is going. `activeChannel_` is where the eye and
	// the transport are; `linkedAB_` is the reference controller's A|B, where a command goes to
	// BOTH — which is what makes two channels useful without doubling the
	// panel.
	Which activeChannel_ = Which::A;
	bool linkedAB_ = false;
	// The coordinator/channel the next command belongs to.
	PlaybackCoordinator &pc() const;
	ReplayChannel &chan() const;
	// Every channel a command applies to right now: both under A|B, one
	// otherwise. Callers loop over it rather than testing linkedAB_ at
	// twenty call sites and getting one of them wrong.
	std::vector<Which> targetChannels() const;
	void setActiveChannel(Which which, bool linked);
	// the reference controller's ⇄: what A is playing goes to B and the other way round.
	void swapChannels();
	QButtonGroup *chanSel_ = nullptr; // A|B / A / B
	QPushButton *swapBtn_ = nullptr;  // ⇄
	// Everything that exists only because there is a second bay: B's row
	// letter, B's eight camera keys, the A|B / A / B selector and the swap.
	// Held as one list so one loop can take the whole lot away — with a single
	// bay they are not disabled, they are absent.
	QVector<QWidget *> channelBWidgets_;
	// The selector row of the camera matrix, as cells on the matrix's own
	// columns.
	QWidget *buildChannelRow();
	// Zoom factor of the position bar, and the key that resets it.
	QPushButton *zoomBtn_ = nullptr;
	// the reference controller paints the header of the angle being watched green. Cheap enough to
	// call from poll(), which is also the only place that learns the angle
	// changed under a hotkey.
	void updateCamHeaderHighlight();
	// The green strip under the preview: list, clip x/y, remaining, event id,
	// IN/OUT offsets, timecode, speed. Same fields the reference controller puts there.
	void updateChannelStrip();
	void renameListDialog(); // gear menu → rename the selected list
	void onEventItemChanged(QTableWidgetItem *item); // edit commit
	// The take armed Branch Output but nothing started: disarm and say why.
	// Called from poll() when the watchdog below expires.
	void cancelDeadRecording();
	void openSettings();        // configuration dialog
	// BRANCH OUTPUT IS NOT INSTALLED — asked, not logged. Once per launch,
	// and again on the next one until it is there: without it this plugin
	// cannot record a frame, so a warning in a log file is not telling the
	// operator, it is telling the file. See the .cpp.
	void promptForBranchOutput();
	bool branchOutputAsked_ = false;
	// FIRST RUN — the five answers without which nothing works, in one
	// dialog instead of five pages of Settings. needsSetup() (public, above)
	// is the question the panel asks itself once per launch; the wizard is
	// also on the gear menu, because "Later" has to be a real button.
	// See the .cpp.
	void runSetupWizard();
	bool setupAsked_ = false;
	// True for the whole life of a first-run dialog, so the second one cannot
	// open on top of the first. Two modal dialogs stacked on a fresh machine
	// is worse than either of them.
	bool modalOpen_ = false;
	// TAGS — the operator's own marking vocabulary (Config.commentPresets, the
	// list every per-angle comment cell offers). One tag per line in a plain
	// TXT file, so a club's words are written once and carried to every machine
	// instead of being retyped into each one.
	//
	// The import REFUSES while recording, and that is not caution for its own
	// sake: it writes the config, and setConfig() re-points the SegmentIndex and
	// re-creates the Branch Output filters. Nothing that touches the recording
	// path may happen mid-take because somebody opened a menu.
	void importTags();
	void exportTags();
	void newProjectDialog();    // New Project... menu action
	void openProjectDialog();   // Open Project... menu action
	void copyYouTubeChapters(); // copy chapter timestamps to clipboard
	int64_t markTimeNs() const; // Live=live edge, Recorded=replay playhead
	// True when `tNs` is a real instant to mark; tells the operator why not
	// (and refuses) when it is not. See the definition.
	bool markable(int64_t tNs);
	std::vector<int> selectedEventIds() const;
	// The "Play selected" button, factored out so the hotkey of the same name
	// runs the same code (selection, angle and "to output" all included).
	void playSelected();
	// Move the list selection by `delta`, clamped. Two relative hotkeys beat
	// twenty absolute ones.
	void stepList(int delta);
	// Move the selected event `delta` places in its list's running order.
	// Manual order and the chronological auto-sort are mutually exclusive by
	// construction — see the definition.
	void moveSelectedEvent(int delta);
	void seekToFraction(double frac);
	// One-line transient message in the status area. Used for the things the
	// operator triggers with a single press (an angle button, a scrub) where a
	// modal would be worse than the silence it replaces — but silence is what
	// made him think the dock had ignored him.
	void showNotice(const QString &text);
	// M4: the health badge was clicked — show every finding, in full, with
	// the numbers. Read-only, like everything else in the health path.
	void showHealthDetails();
	void setAngle(int angle1Based);
	// The same, on a named channel: B's camera row must set B's angle even
	// while the transport keys are on A.
	void setAngleOn(Which which, int angle1Based);
	// (Re)play the selected (or last) completed event from its IN on the
	// current angle at the resolved speed. No-op while following live (the
	// angle buttons then only pick which camera the preview mirrors).
	void replayCurrent();
	// the reference controller frame-by-frame forward: nudge the playhead on by one frame and show
	// it. The engine plays ranges, so a step is a very short range — there is
	// no playhead in it to move (see the definition).
	void stepFrameForward();
	// ...and backwards, which is NOT the mirror image of it. Playing a short
	// range forwards from one frame back would come to rest on the frame the
	// operator is already looking at, so the step back is a REVERSE run of two
	// pictures: the current one, then the one before it (see the definition).
	void stepFrameBackward();
	// the reference controller's ◀ key: play what is selected (or the last event) BACKWARDS, from
	// its OUT to its IN, through the same queue as ▶.
	void playSelectedReverse();

	// --- unmarked footage -------------------------------------------------
	// Is the bar parked on a stretch that the event ▶ would otherwise replay
	// does not cover? That is the question that decides what ▶ means, and it
	// has to be asked of the SELECTION rather than of the whole list: the
	// table auto-selects the newest mark, so "is there an event somewhere near
	// here" would answer yes all match long.
	bool playheadIsFreeFootage() const;
	// Watch the footage from the bar onwards — no event behind it. `toOutput`
	// is what separates ▶ (never) from Play events (through the "In output"
	// key), and it is the only difference between them.
	bool playFreeReview(bool toOutput);
	// Forget the armed free review. Anything that means "I am talking about
	// something else now" calls this.
	void clearFreeReview();
	// the reference controller's Stop, as a key rather than a menu entry. The free review runs
	// until it is stopped, so the way to stop it cannot be two clicks deep in
	// a dropdown — and ▶ is a PAUSE while something plays, not a stop.
	void stopPlayback();
	// Play on every bay the selector points at (both, under A|B). "To output"
	// can only go to ONE of them — Program is one scene — so it goes to the bay
	// named in Settings and the other plays without touching Program.
	//
	// Always every enabled angle (the play keys' own mode); the direction is
	// what ▶ and ◀ differ by. PlaybackCoordinator is only forward-declared
	// here, so its nested AngleMode cannot appear in this signature.
	bool playOnTargets(const std::vector<int> &ids,
			   ReplayChannel::Direction direction,
			   std::string &errorOut);
	// The position bar's zoom: a menu of SPANS (whole timeline, 1 h, 30/10/5/1
	// min) rather than a factor, because "show me the last five minutes" is what
	// an operator wants and the factor that gets there depends on how long the
	// session happens to be.
	void showZoomMenu();
	// Put the selected event's IN frame on the selected channel(s) without
	// playing it, so choosing a row in the table shows it. Two pictures, which
	// is the shortest thing the engine can serve, and it comes to rest on the
	// first of them.
	void cueSelected();
	// Apply a replay speed (5..200): becomes the default for events without a
	// per-angle override, and re-cues the current clip from its in-point at
	// the new speed (broadcast-style).
	void applyReplaySpeed(int pct);

	// --- hotkeys the DOCK owns ---------------------------------------------
	// ReplayCore registers the hotkeys whose meaning is engine state. These
	// are the ones whose meaning is the DOCK's state — which events are
	// selected, where the bar is parked, what the speed slider says — and no
	// free function in replay-core can see any of it. Registered here so a
	// Stream Deck reaches exactly the same code as the buttons, instead of a
	// second implementation that drifts.
	//
	// OBS calls a hotkey callback on the hotkey thread, so each one hops onto
	// the GUI thread before it touches a widget.
	void registerDockHotkeys();
	std::vector<std::unique_ptr<HotkeyCtx>> hotkeyCtx_;
	std::vector<obs_hotkey_id> hotkeys_;

	// --- event filter: double-click on note labels ---
	bool eventFilter(QObject *watched, QEvent *event) override;

	// --- THE THREE ARRANGEMENTS OF THE PANEL --------------------------------
	// See PanelMode in dock-layout.hpp for what they are and why there are
	// three of them rather than a fluid layout. Everything applyPanelMode()
	// does is a re-cell or a property change:
	//
	//   NOTHING HERE MAY RE-PARENT AN OBSQTDisplay. Qt answers a re-parent by
	//   destroying the widget's native window, which leaves its obs_display
	//   presenting into nothing — the one failure qt-display.hpp exists to
	//   prevent, and the reason the multiview tiles have always been moved
	//   between cells of one grid instead of being rebuilt. So the pictures
	//   live in a QGridLayout that is re-celled, and the preview/list split is
	//   ONE QSplitter whose orientation is turned (setOrientation does not
	//   touch its children).
	void resizeEvent(QResizeEvent *event) override;
	void applyPanelMode(PanelMode m, bool force = false);

public:
	// --- THE PANEL'S COLOURS ------------------------------------------------
	// Rebuilds the style sheet from Config.uiTheme and the application palette
	// — which is where OBS puts the current theme's colours (see schemeFor in
	// dock-style.hpp). Public because the module's frontend handler calls it on
	// OBS_FRONTEND_EVENT_THEME_CHANGED: a panel that keeps yesterday's chrome
	// after the operator switches to a light theme is worse than one that never
	// followed at all.
	void applyTheme();
	// Which of the three arrangements the panel is wearing. Public for the
	// gate: "the cameras have no width in a column" is a fault only a check
	// that knows the arrangement can name, and naming it is the difference
	// between a red line and an afternoon.
	PanelMode panelMode() const { return panelMode_; }
	void applyTableDensity(int level);
	// The comment editor is a delegate, and it lives outside this class: it
	// needs the session vocabulary and the place a chosen word is remembered.
	QStringList sessionComments() const { return sessionComments_; }
	void rememberComment(const QString &text);
	static void restyleDock(); // reaches the live dock from the module

private:
	// How many columns the camera tiles get. Two were cabled in, which is
	// right beside a big A output and wrong in a column.
	int tileColumns(int tileCount) const;
	// How tall the monitoring block may be. Never read off the block itself.
	int monitorRoomH() const;

	// --- THE PICTURES GET THE HEIGHT THEIR ASPECT ASKS FOR -------------------
	// A preview box was given whatever height the splitter had spare, and
	// renderSourceFitted() then drew the canvas inside it letterboxed — so the
	// difference between the box and the picture came out as BLACK BARS. In a
	// column that was most of the difference between a usable event list and a
	// cramped one: every pixel of bar is a pixel the table asked for.
	//
	// So the monitoring pane is CAPPED at the height its widest picture
	// actually needs, and the splitter hands the remainder to the list. The
	// ratio comes from the OBS canvas (obs_get_video_info), not from a
	// hardcoded 16:9: a vertical canvas is a real thing an operator streams.
	void applyPreviewAspect();
	// The divider between the pictures and the list, given the height the
	// pictures would like. Split out because it is the half that has to do
	// nothing at all in the Short arrangement, where that divider is a width.
	void applyPreviewSplit(int want);
	// SHORT stacks the keys under the pictures in the LEFT column instead of
	// across the foot of the panel. A no-op unless the answer changed.
	void applyControlsColumn(bool inColumn);
	bool controlsInColumn_ = false;
	QVBoxLayout *rootLayout_ = nullptr;
	QWidget *leftCol_ = nullptr;
	QVBoxLayout *leftColLayout_ = nullptr;
	QWidget *bottomBar_ = nullptr;
	QWidget *bottomSep_ = nullptr;
	// Height of a picture that wide, in the canvas's own ratio.
	static int aspectHeight(int width);
	PanelMode panelMode_ = PanelMode::Wide;
	// The height the WIDE arrangement last declared as its own minimum. Short
	// is chosen when the panel cannot be that tall, and "that tall" moves with
	// the width - see panelModeFor and kShortMaxHeight.
	int wideFloorH_ = 0;
	// The two bays, each keeping the canvas's ratio whatever cell it is given
	// (see AspectBox). They live in one grid, side by side, in every
	// arrangement — nothing here is ever re-parented.
	AspectBox *aBox_ = nullptr;
	QWidget *bays_ = nullptr;
	// The divider between the bays and the cameras. It is the operator's, and
	// because every box keeps its ratio, dragging it in a wide panel changes
	// the pictures' HEIGHT as well as their width.
	QSplitter *monitorSplit_ = nullptr;

	// ── WHERE THE OPERATOR PUT THE DIVIDERS, PER ARRANGEMENT ─────────────
	// A drag is a decision and it has to survive; but it is a decision about
	// ONE arrangement. The split that is right for a wide floating window
	// means nothing in a narrow column where the same divider runs the other
	// way, so carrying it across would hand back a layout he never chose.
	// Indexed by PanelMode.
	QByteArray savedSplit_[3], savedMonitorSplit_[3];
	bool userSplit_[3] = {false, false, false};
	bool userMonitorSplit_[3] = {false, false, false};
	// How many cameras the divider above was chosen for. A different number is
	// a different question, so the choice is dropped (see rebuildMultiview).
	int lastTileCount_ = -1;
	bool splitChosen() const { return userSplit_[(int)panelMode_]; }
	bool monitorSplitChosen() const
	{
		return userMonitorSplit_[(int)panelMode_];
	}
	// How much list is kept whatever the pictures ask for. The pictures get
	// their aspect height or what is left after this, whichever is smaller —
	// a perfect picture over two visible rows of events is the wrong trade on
	// a panel whose point is the list.
	static constexpr int kListPaneFloor = 110;

	// --- THE KEYBOARD: one layer, not a second set of commands -------------
	// Arrows on the timeline, ↑/↓ between events, +/− on the speed, Enter to
	// play. Every one of them CALLS the button of the same name, so there is
	// one implementation of "one frame forward" and one gate check covering
	// both ways of asking for it.
	//
	// Two things it must not do, and both were paid for in this dock already:
	//   - steal keys from text. The search box and the per-angle comment cells
	//     are two pixels from these controls, and an operator typing "Fallo"
	//     must not send the panel backwards a frame (focusIsTextEntry).
	//   - lose to the table. A QTableWidget with focus eats ↑/↓ and Enter for
	//     its own navigation, so the table is filtered (see eventFilter) —
	//     ↑/↓ it may keep (moving down a row IS moving to the next event),
	//     Enter and the arrows it may not.
	void keyPressEvent(QKeyEvent *event) override;
	// True when the key was ours. Shared by keyPressEvent and the table's
	// filter, so the two cannot answer differently.
	bool handleTransportKey(QKeyEvent *event);
	// Is the focus in something that takes typing? Same question
	// refreshEvents() asks before rebuilding the table under an editor.
	static bool focusIsTextEntry();
	// Move the selection `delta` rows in the event table (↑/↓). A selection
	// the OPERATOR makes cues the event, which is the point of the keys.
	void stepEventSelection(int delta);
	// Move the playhead by a plain number of seconds along the FOOTAGE axis
	// (Shift+←/→): a frame at a time is the wrong unit for "a bit earlier".
	void scrubBySeconds(double seconds);
	// +/− on the speed. Percentage points, not the preset ladder: the ladder
	// already has its own five hotkeys, and this is the fine adjustment you
	// make while watching the picture.
	void nudgeSpeed(int deltaPct);
	static constexpr int kSpeedKeyStepPct = 5;

	// --- preview render callback (runs on the OBS graphics thread) ---
	static void drawChannelA(void *data, uint32_t cx, uint32_t cy);
	// Channel B's box: whatever B last played, black before that. No live
	// mirror — see the note on drawChannelB.
	static void drawChannelB(void *data, uint32_t cx, uint32_t cy);

	// --- MULTIVIEW: one small preview per configured angle, plus the replay --
	//
	// The single big preview only ever showed ONE angle, so the operator had to
	// press a camera button to find out what the other cameras were doing —
	// which is exactly the moment he cannot afford to look away. the reference controller puts the A
	// output big and every camera small beside it; so does this.
	//
	// Each tile is an obs_display_t of its own, and that cost is real: they are
	// rendered by the SAME single graphics thread as the OBS program preview.
	// Three things keep it bounded, in order of effect:
	//   1. a tile exists only for a CONFIGURED camera. The others are hidden,
	//      and a hidden OBSQTDisplay never creates its display at all (see
	//      OBSQTDisplay::recheckWindow: it only creates when isVisible()).
	//   2. the whole strip can be switched off (Config.showMultiview) — the
	//      widgets stay, hidden, so nothing is created or destroyed.
	//   3. the tiles are small, so each present() copies few pixels.
	// The draw callback itself does what drawChannelA does and no more: copy an
	// already-resolved pointer and take a ref. No lookup, no libobs mutex.
	static constexpr int kMaxPreviewTiles = 9; // kMaxCameras + the replay tile
	// Narrower than this a tile stops being a picture and becomes a smear.
	static constexpr int kTileMinWidth = 78;
	// …and no WIDER than this in a column. A tile is a confidence monitor, not
	// the picture being watched: left to fill the row, a single configured
	// camera drew itself 340 px wide and 191 px tall, taking as much of the
	// panel as A and telling the operator nothing A was not already telling him.
	// How wide a tile may actually be drawn in the arrangement on screen.
	// The ceiling above, except in a column, where a strip of two or more
	// divides the row between them instead of leaving a band of empty panel
	// beside it. applyPreviewAspect works it out and the tiles are told.
	int tileCap_ = kTileMinWidth;

	// Which mark the two keys that change one are wearing: -1 nothing yet,
	// 0 the resting one, 1 the running one. poll() runs thirty times a
	// second and re-rasterising a pixmap on each tick is work nobody asked
	// for; comparing first is how every other setter in poll() behaves.
	int playPauseIcon_ = -1;
	int recIcon_ = -1;

	// What the graphics thread is handed for a tile: it may not dereference
	// anything but this (see drawTile).
	struct TileCtx {
		MultiReplayDock *dock = nullptr;
		int slot = 0; // index into tiles_ / tileSource_
	};
	struct PreviewTile {
		AspectBox *box = nullptr;
		OBSQTDisplay *display = nullptr;
		QLabel *caption = nullptr;
		int cam0 = -1; // 0-based camera, -1 = the replay tile
	};

	// Re-lay the grid for the cameras that are configured now. Cheap and a
	// no-op unless the configuration really moved: it never re-parents a tile
	// (that would destroy its native window and strand its display), it only
	// moves it between cells of the grid it already belongs to.
	void rebuildMultiview();
	// Resolve every visible tile's source on the UI thread and publish the
	// owned refs for the graphics thread. Same discipline as previewSource_.
	void refreshTileSources();
	// Caption colours: green = the angle being watched, red = the angle on air.
	void updateMultiviewTally();
	static void drawTile(void *data, uint32_t cx, uint32_t cy);

	// --- THE ANGLE BOXES DURING A REVIEW ---------------------------------
	// Out of follow-live, a tile stops being a camera monitor and becomes the
	// reviewed moment on that lens — which is the whole reason an operator
	// looks at a multiview: to pick the angle before the replay goes up. Each
	// configured camera gets a preview FEED of its own (ReplayChannel::
	// makePreview: the same engine, a private input, silent) and every feed is
	// cued to the same range.
	//
	// Cost is bounded by construction, in the same three ways the tiles
	// themselves are: a feed exists only for a configured camera whose tile is
	// on screen (Monitors off or showMultiview off ⇒ no feeds at all), the cue
	// is capped at kTileReviewMaxNs so a feed never materialises more than a
	// minute of packets, and a feed decodes no audio whatsoever.
	void ensureTileFeeds();
	// Show [inNs, outNs] on every feed. A no-op when the range has not moved —
	// this is called from poll(), thirty times a second.
	// `maxFrames` > 0 stops each feed after that many pictures, which is how a
	// CUE asks for the same range the play after it will ask for — same range,
	// same cache key, one fetch between them instead of two.
	void cueTiles(int64_t inNs, int64_t outNs, int speedPct,
		      ReplayChannel::Direction dir, int maxFrames = 0);
	// Get a range ready on every feed WITHOUT showing it. Mirrors what
	// cueSelected() already does for the bay: cueing is what an operator does
	// immediately before playing, so the clip the play will want is fetched
	// while he is looking at the frozen IN. Without it the feeds paid a cold
	// fetch — an open, a seek and a demux when the range is not in the ring —
	// at the moment poll() cued them, which is a tenth of a second AFTER the
	// bay had already started. Nothing waits on it (see ReplayChannel::
	// prefetch): a prefetch that has not finished changes nothing but the
	// timing.
	void prefetchTiles(int64_t inNs, int64_t outNs, int speedPct);
	// Has any feed shown its FIRST picture since the last tick? Run on every
	// poll tick, and it costs nothing once they all have: it only asks the
	// feeds that have not answered yes yet.
	bool pollTileFeedPictures();
	// Stop and destroy every feed, and forget the cue. Called when the panel
	// goes back to live and on the way out.
	void releaseTileFeeds();
	// Longest range a feed is ever asked for. A clip is COMPRESSED packets in
	// RAM (~2.5 MB for five seconds at 4 Mbit/s), so a minute per angle is the
	// order of 30 MB — bounded, and far more footage than anyone chooses an
	// angle over. A longer review simply holds its last frame on the tiles.
	static constexpr int64_t kTileReviewMaxNs = 60'000'000'000LL;

	std::array<std::unique_ptr<ReplayChannel>, 8> tileFeed_{};
	// HAS THIS FEED EVER SHOWN A PICTURE? Sticky for the life of the feed, and
	// that is the whole point of it.
	//
	// It used to be asked of the feed itself, as hasPosition() — which is
	// framesPushed > 0, and play() zeroes the stats at the start of EVERY
	// clip. So at every cue the feed answered "nothing yet",
	// refreshTileSources() published a null source, and the tile went BLACK
	// and stayed black until the next 4 Hz beat — a quarter of a second late,
	// on a picture the feed already had. The big bay never did that because
	// its gate (previewHasContent_) is about the tap having captured anything
	// at all, not about the clip: its source stays published, so a new clip's
	// first frame appears the moment the decoder pushes it. This is the same
	// question asked the same way, which is what makes the two arrive
	// together.
	//
	// Cleared only when a feed is created or destroyed — the two moments when
	// there really is nothing behind it.
	std::array<bool, 8> tileFeedHadPicture_{};
	int64_t tileCueInNs_ = kNoInstant;
	int64_t tileCueOutNs_ = kNoInstant;
	int tileCueSpeedPct_ = 0;
	ReplayChannel::Direction tileCueDir_ =
		ReplayChannel::Direction::Forward;
	// Part of the cue's identity, not a detail: a cue and the play that
	// follows it now ask for the SAME range and differ only in this. Left out,
	// the play would look like the cue that is already running and be skipped
	// — the boxes would hold the in-point still for the whole replay.
	int tileCueMaxFrames_ = 0;
	// What refreshTileSources() last published: the camera mirrors, or the
	// feeds. Kept so the swap happens the tick the mode changes rather than on
	// the next 4 Hz beat — a quarter of a second of the wrong picture in eight
	// boxes is the kind of thing an operator sees and cannot name.
	bool tilesLive_ = true;

	std::array<PreviewTile, kMaxPreviewTiles> tiles_{};
	std::array<TileCtx, kMaxPreviewTiles> tileCtx_{};
	mutable std::mutex tileMutex_; // pointer copy + addref only
	std::array<obs_source_t *, kMaxPreviewTiles> tileSource_{};
	QWidget *multiviewBox_ = nullptr;
	QGridLayout *multiviewGrid_ = nullptr;
	// Configuration the grid was last laid out for; re-laying it out clears
	// nothing but still costs a relayout, so it is done only when this moves.
	QString multiviewSig_;
	int tileTallyPvw_ = -2; // slot painted green, -2 = never painted
	int tileTallyPgm_ = -2; // slot painted red

	// preview (single replay channel A)
	OBSQTDisplay *displayA_ = nullptr;
	OBSQTDisplay *displayB_ = nullptr;
	// EVERY display this dock owns, in one place. The destructor detached
	// A's draw callback and every tile's, and NOT B's — leaving the graphics
	// thread able to call drawChannelB() on a dock whose previewMutex_ had
	// already been destroyed. Same omission, same object, same reason as the
	// preview ref OBS complained about: B is optional and off by default, so
	// nothing exercised it. Naming them one at a time is what made that
	// possible, so nothing names them one at a time any more.
	std::vector<OBSQTDisplay *> allDisplays() const;
	QLabel *labelA_ = nullptr; // the letter under each box
	QLabel *labelB_ = nullptr;
	void replayCurrentOn(Which which);
	// the reference controller's green channel strip under the A preview.
	// THE STATUS LINE. It replaced a three-line green band under the
	// pictures, nearly all of which was a second copy of what the on-air band
	// and the position bar already say — in a second green the eye had to
	// tell apart from the first. What is here is what was said nowhere else:
	// which list and which event the transport keys are about, where the
	// playhead is, and the answer to a key the operator just pressed.
	QLabel *statusNotice_ = nullptr;
	QLabel *statusSpeed_ = nullptr;
	QLabel *statusTake_ = nullptr; // elapsed / remaining, while recording
	QWidget *statusBar_ = nullptr;
	// Whether the line is currently carrying a notice rather than the resting
	// text. Kept so the restyle that goes with it happens on the CHANGE, not
	// thirty times a second.
	bool statusNoticeLit_ = false;

	// recording / status
	QPushButton *recBtn_ = nullptr;
	QLabel *statusLbl_ = nullptr;
	QLabel *clockLbl_ = nullptr;   // wall clock + remaining recording time
	QLabel *projectLbl_ = nullptr; // shows active project name
	// M4: what the health monitor found, next to the record key. Hidden while
	// there is nothing to say — a badge that is always there is furniture, and
	// furniture does not get looked at when it finally turns red. Amber for a
	// degraded take, red for one that is not usable. It never acts: clicking
	// it opens the list, and the operator decides.
	QPushButton *healthBtn_ = nullptr;

	// transport
	// The angle each channel is on, 1-based. the reference controller has a row of camera keys
	// per channel because A and B are two bays: the operator lines the next
	// replay up on B while A is on air, and one shared angle would make that
	// impossible. currentAngle1() is the ACTIVE channel's — the one the
	// marks, the hotkeys and ReplayCore::currentAngle() all mean.
	int angle1_[kChannels] = {1, 1};
	int currentAngle1() const { return angle1_[(int)activeChannel_]; }
	// Default replay speed (slider). The engine has no speed of its own any
	// more: it is told the speed of the clip it is asked to play.
	int speedPct_ = 100;
	SeekBar *seek_ = nullptr;
	// The green on-air band and the key beside it that drops the clip on air
	// and takes the next item of the queue (another angle, or the next event).
	ClipBar *clipBar_ = nullptr;
	QPushButton *nextClipBtn_ = nullptr;
	QSlider *speed_ = nullptr;
	// The 25/33/50/75/100/2× chips, keyed by percentage so poll() can light
	// the one that matches the current speed (the reference controller fills it green).
	QButtonGroup *speedChips_ = nullptr;
	QLabel *speedLbl_ = nullptr;
	// (There used to be a QLabel here printing "position / length" under the
	// transport keys. The position bar prints it on itself, which is where the
	// eye already is; two copies of the same timecode is two things to
	// reconcile.)
	QPushButton *playPauseBtn_ = nullptr;
	// ■ — Stop. EXACTLY ONE button in this dock may carry this glyph: the gate
	// finds Stop by it.
	QPushButton *stopBtn_ = nullptr;
	QPushButton *nowBtn_ = nullptr;
	bool seekDragging_ = false;

	// markers. the reference controller's Live is a red toggle BUTTON in the top bar, not a
	// checkbox down with the marker keys: it is the mode the whole panel is in.
	QPushButton *liveBtn_ = nullptr;

	// events
	QTabBar *listTabs_ = nullptr; // the 20 lists, broadcast-style tabs
	QLineEdit *search_ = nullptr;
	QTableWidget *events_ = nullptr;
	// (No inspector panel: the per-angle enable box, speed and comment are all
	// in the table now, on the row and in the column they belong to.)
	// TWO FLAGS, AND KEEPING THEM APART IS A BUG FIX, NOT TIDINESS.
	//
	// refreshing_ means "the event table is being rebuilt, so anything a cell
	// widget emits is an echo of that and not an operator's edit". Only
	// refreshEvents sets it, and only the angle-cell handlers read it.
	//
	// itemsProgrammatic_ means "I am writing table ITEMS myself" — the camera
	// headers, the green watched-angle header, the red id of the row on air.
	// Those run on the 30 Hz poll.
	//
	// They used to be the same flag, and that silently threw away the
	// operator's edits. A QComboBox popup runs a NESTED EVENT LOOP, so the
	// dock's 33 ms poll fires while the list is open — and if the pick landed
	// while poll() was inside the on-air colouring, the cell's handler saw
	// "refreshing" and returned without writing. Intermittent by construction,
	// and it looked exactly like what was reported: the speed and the comment
	// appear in the cell and are gone from the store, which a search then
	// redraws from. events.json had speed:-1.0 and note:"" on every angle of
	// every event of a match that had been annotated.
	bool refreshing_ = false;        // table rebuild in progress
	bool itemsProgrammatic_ = false; // we are writing items, not the operator
	uint64_t lastEventVersion_ = UINT64_MAX; // UINT64_MAX → refresh on first poll
	// Largest event id seen: when a newer one appears (a fresh mark), the table
	// auto-selects it so "Riproduci selezionati" is one click; otherwise the
	// user's current selection is preserved across refreshes.
	int lastMaxEventId_ = 0;
	QPushButton *toOutputBtn_ = nullptr;
	QPushButton *loopBtn_ = nullptr;
	QPushButton *musicBtn_ = nullptr;
	// Which camera each per-camera table column PAIR stands for (0-based), in
	// column order starting at kColFirstCam. Empty = no camera configured.
	std::vector<int> camCols_;
	// Header section currently painted green (the angle being watched), -1 =
	// none. Kept so poll() only repaints when it really moved.
	int camHeaderHot_ = -1;

	QSplitter *splitter_ = nullptr;

	// Live-mirror preview: unless a clip is playing, the preview renders the
	// live camera source for the selected angle (zero latency, exactly what
	// the reference controller shows) instead of the replay input, which only has pictures while a
	// clip is playing. It is a confidence monitor, so it does NOT depend on
	// recording: the angles are checked before the take, not during it.
	//
	// WHICH source that is gets decided and RESOLVED here, on the UI thread,
	// and the resulting owned reference is published for the graphics thread.
	// Every obs_display in OBS — the program preview, every other dock, ours —
	// is rendered by ONE graphics thread, so whatever our draw callback waits
	// on, the whole GUI waits on. It used to call obs_get_source_by_name() per
	// frame, which takes libobs' global source mutex: with the UI thread
	// holding that mutex (Settings dialog, scene switch, source list) and then
	// wanting the graphics context, and the graphics thread holding the
	// graphics context and wanting the source mutex, the two block each other
	// and OBS stops repainting. drawChannelA() now only copies this pointer
	// and takes a ref (a lock-free atomic increment) — no lookup, no libobs
	// mutex, no engine lock.
	mutable std::mutex previewMutex_;       // pointer copy + addref only
	obs_source_t *previewSource_ = nullptr; // owned ref, read by the GFX thread
	obs_source_t *previewSourceB_ = nullptr; // the same, for channel B
	// UI-thread-only bookkeeping: what previewSource_ was resolved FOR, so a
	// tick that cannot have changed the answer does no lookup at all.
	bool previewLive_ = false;
	int previewCam0_ = -1;
	// False until something has been captured: keeps the preview black after a
	// fresh start instead of showing the last frame of the previous clip.
	bool previewHasContent_ = false;
	// Mirror of !previewLive_, readable from any thread (see previewShowsReplay).
	std::atomic<bool> previewShowsReplay_{false};

	// --- sequence bookkeeping (see poll()) ---
	// A multi-angle event is SEVERAL clips, and the engine is idle for a moment
	// between them. Tracking the queue instead of the clip is what keeps the
	// preview (and the program, with "to output" on) on the replay for the whole
	// sequence instead of flashing the live camera between angles.
	bool prevSequenceActive_ = false;
	// Last instant a clip was really being paced out. A queue that claims to be
	// active while nothing has played for a while died badly, and must not pin
	// the preview on a replay that is not coming.
	int64_t lastPlayingNs_ = 0;
	// The playhead the dock draws: the engine's last frame while a clip plays,
	// otherwise where the operator parked the timeline (a scrub, NOW, or the
	// live edge the end of a sequence returns to). ReplayChannel keeps reporting
	// the last frame of the clip that finished, which is why the bar used to
	// stay wherever the replay stopped until NOW was pressed by hand.
	// kNoInstant, not 0: this is an instant on the master clock and instants
	// go negative for footage older than the machine's last boot.
	int64_t playheadNs_ = kNoInstant;

	// --- THE FREE REVIEW: footage nobody marked ---------------------------
	// Where the bar was when the operator asked to watch a stretch that no
	// event covers, or kNoInstant when no such review is armed.
	//
	// It exists because the two keys have to mean different things here, and
	// only a remembered in-point can hold them apart:
	//   ▶            watch it, OFF AIR. Reviewing an action that was never
	//                marked must not be able to reach Program by itself —
	//                that is the whole safety of letting ▶ play unmarked
	//                footage at all.
	//   Play events  put THAT on air (through the "In output" key, like every
	//                other play path). This is the key's second function, and
	//                it is why the in-point outlives the clip: the operator
	//                watches, presses Stop, decides, and then airs it — from
	//                the same instant, not from wherever the bar stopped.
	// Cleared by anything that means "I am no longer talking about that
	// stretch": picking a row, NOW, Live, or a new take.
	int64_t freeReviewInNs_ = kNoInstant;
	// Whether the armed review is currently taking Program. Needed because an
	// angle key re-cues what is being watched on the chosen camera, and doing
	// that to a review that is ON AIR must not quietly drop it off air — nor
	// put an off-air one up.
	bool freeReviewOnAir_ = false;
	// One chunk of a free run. The engine materialises a clip's packets in
	// RAM, so "play from here to the end of the session" cannot be one range —
	// an hour of footage is gigabytes. It is a QUEUE of these instead, which
	// the coordinator already chains the way it chains the angles of an event,
	// and each link is a minute of compressed packets (~30 MB).
	static constexpr int64_t kFreeReviewChunkNs = 60'000'000'000LL;
	// ...and at most this many links, so a free run started by accident on a
	// long session is bounded rather than open-ended. Half an hour of watching
	// is far past the point where the operator would have scrubbed again.
	static constexpr int kFreeReviewMaxChunks = 30;

	// Until when showNotice()'s message owns the channel strip (master ns),
	// and the message itself. It goes on the strip rather than in the corner
	// status line because that line is two inches wide and this is the answer
	// to something the operator just pressed.
	int64_t noticeUntilNs_ = 0;
	QString noticeText_;

	QTimer *pollTimer_ = nullptr;
	bool prevRecording_ = false; // detects REC start
	// Deadline (master ns) by which Branch Output has to have started at least
	// one recording output, 0 = not watching. REC only ENABLES the filters:
	// Branch Output decides on its own, and on any Interlock setting other
	// than "Always ON" it silently declines — which used to leave the dock red
	// over a session recording nothing. See poll() / cancelDeadRecording().
	int64_t armWatchDeadlineNs_ = 0;
	// Counts poll() ticks so the expensive status refresh can run at a
	// fraction of the transport rate (see poll()).
	uint64_t statusTick_ = 0;

	// Master-timeline window currently drawn on the seekbar. The START is an
	// INSTANT (kNoInstant = nothing to draw); the DURATION is a length, so 0
	// really does mean none. Instants may be negative — see kNoInstant in
	// segment-index.hpp — and testing one for > 0 is what made a reopened
	// project draw a flat bar after a reboot.
	int64_t timelineStartNs_ = kNoInstant; // oldest replayable instant
	int64_t displayDurNs_ = 0;             // how much FOOTAGE there is
	// The position bar's axis: the recorded spans joined end to end, so a
	// session with pauses in it has no holes to step over and no stretch of
	// bar that resolves to an instant nothing covers. Rebuilt each poll from
	// SegmentIndex plus the take in progress. See timeline-map.hpp.
	TimelineMap timeline_;
	// The disk half of it, cached: reading it takes SegmentIndex's lock, and
	// its watcher holds that while demuxing a file. See poll().
	std::vector<TimelineSpan> diskSpans_;
	// Where the PROJECT's footage begins, which is what event timecodes and
	// YouTube chapters are measured from. Deliberately not timelineStartNs_:
	// that one follows the selected angle, so the whole table renumbered
	// itself when the operator pressed another camera button, and it is
	// absent whenever the selected angle has nothing — which printed marks as
	// raw monotonic time (a five-digit minute count). kNoInstant = no footage
	// at all, and then there is no honest number to print.
	int64_t eventOriginNs_ = kNoInstant;
	// Origin the event table was last rendered against, so poll() can tell
	// when the columns need redrawing (see poll()).
	int64_t tableOriginNs_ = kNoInstant;

	// Raw ns (in, out) for each completed event — fractions computed in poll()
	// so markers shift leftward as recording time grows.
	std::vector<std::pair<int64_t, int64_t>> markerNs_;
	// The event behind each marker, same order — what makes an edge draggable.
	std::vector<int> markerIds_;

	// Scratch buffers the marker fractions are built into on every tick. They
	// are MEMBERS so the building costs no allocation: poll() used to
	// construct two fresh vectors thirty times a second and hand them to a
	// setter that had no way of telling they were identical to the last two.
	std::vector<std::pair<double, double>> markerFracScratch_;
	std::vector<int> markerIdScratch_;

	// --- UI thread health ------------------------------------------------
	// The panel going black is a symptom of the Qt side not being serviced,
	// so the question that has to be answerable from a log alone is "was the
	// UI thread being serviced?". These measure it: how late the 33 ms tick
	// actually fires (the timer is the same event loop that delivers paint
	// events, so a tick that arrives 800 ms late IS a window that was not
	// repainted for 800 ms) and how long poll() itself takes, which is the
	// part of that we own.
	uint64_t lastTickNs_ = 0;
	uint64_t uiWindowStartNs_ = 0;
	uint32_t uiTicks_ = 0;
	int64_t uiLateSumNs_ = 0;
	int64_t uiLateMaxNs_ = 0;
	int64_t uiCostSumNs_ = 0;
	int64_t uiCostMaxNs_ = 0;
	uint64_t uiLastStallLogNs_ = 0;
	// Throttle for the event-table rebuild breakdown (see refreshEvents).
	uint64_t lastRebuildLogNs_ = 0;
	// What the list tabs currently say, so refreshListNames can do nothing when
	// nothing has changed — which is nearly always. See the note there.
	std::vector<std::string> lastListNames_;
	int lastShownTabs_ = -1;
	// WHERE a slow tick went. The total alone names nothing: a poll() of 964 ms
	// was measured and the only honest thing that could be said about it was
	// that it happened. Each phase reports its own cost here and the [ui] line
	// prints the worst single occurrence of the window WITH ITS NAME, because
	// the fix differs entirely by culprit — the segment index lock is held
	// while a file is demuxed, statfs on a NAS is a network round trip, and
	// rebuilding the event table is our own arithmetic.
	struct PhaseCost {
		const char *name = "";
		int64_t ns = 0;
	};
	PhaseCost uiWorstPhase_;
	void notePhase(const char *name, int64_t ns);
	// RAII stopwatch for one phase of poll(). Scoped so an early return cannot
	// lose the measurement.
	class Phase {
	public:
		Phase(MultiReplayDock *d, const char *n)
			: d_(d), n_(n), t0_(os_gettime_ns())
		{
		}
		~Phase() { d_->notePhase(n_, (int64_t)(os_gettime_ns() - t0_)); }

	private:
		MultiReplayDock *d_;
		const char *n_;
		uint64_t t0_;
	};
	// Census readings at the start of the window, so the report can print a
	// RATE rather than a total that only ever grows.
	uint64_t uiSeekReqAtWindow_ = 0;
	uint64_t uiSeekSupAtWindow_ = 0;
	uint64_t uiSeekServedAtWindow_ = 0;
	uint64_t uiClipReqAtWindow_ = 0;
	uint64_t uiClipServedAtWindow_ = 0;
	void accountUiTick(uint64_t tickStartNs, uint64_t tickEndNs);
};

} // namespace multireplay
