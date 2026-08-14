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

#include <obs.h>

// kNoInstant: the dock stores master-timeline instants, and "there is none" is
// not zero. See the note there before comparing one against 0.
#include "segment-index.hpp"

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
class QSplitter;
class QTimer;
class QVBoxLayout;
class QGridLayout;
class QGroupBox;

namespace multireplay {

class OBSQTDisplay;

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
	void setEventMarkers(std::vector<std::pair<double, double>> markers);
	// the reference controller prints the transport state ON the position bar ("0000 - 00:11.56
	// 100%") instead of beside it, and that is where the operator's eye already
	// is while he scrubs. Drawn centred, over the fill.
	void setOverlayText(const QString &text);
	bool dragging() const { return dragging_; }

signals:
	void scrubStateChanged(bool dragging); // press(true) / release(false)
	void scrubMoved(double frac);          // live drag/hover position
	void seekRequested(double frac);       // committed on release/click

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;

private:
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
	void setState(double progressFrac, const QString &text, bool onAir);

	// What the band is currently saying. For the automated gate: "the bar
	// exists" is not the claim worth checking — "while a clip is on air the
	// bar names it, and its fill has moved" is.
	QString overlayText() const { return text_; }
	double progress() const { return progress_; }
	bool onAir() const { return onAir_; }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	double progress_ = 0.0;
	QString text_;
	bool onAir_ = false;
};

class MultiReplayDock : public QWidget {
	Q_OBJECT

public:
	explicit MultiReplayDock(QWidget *parent = nullptr);
	~MultiReplayDock() override;

	// What the preview is showing: true = the replay (a clip, a scrub review,
	// or the frame the last one ended on), false = the live camera mirror.
	//
	// Published for the automated gate, which is the only thing outside the
	// dock that reads it: "the preview flipped back to the live camera between
	// two clips of a sequence" is invisible to every other check, and it is
	// exactly what put the wrong picture on air. Atomic because the gate samples
	// it from its own thread while poll() writes it on the UI thread.
	bool previewShowsReplay() const { return previewShowsReplay_.load(); }

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
		int seekGraduations = 0;  // labelled marks the bar draws now
		bool seekEnabled = false; // false = no timeline to scrub
	};
	LayoutProbe layoutProbe() const;

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
		kColFirstCam // first per-camera pair
	};
	// Each configured camera owns two columns: [enable box + speed] and
	// [comment]. See the note above EventColumn's definition in the .cpp.
	static constexpr int kColsPerCam = 2;

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
	QWidget *buildMarkers();
	QWidget *buildAngleMatrix();
	QWidget *buildTransport();
	QWidget *buildEvents();
	QWidget *buildBottomBar();

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
	void setAngle(int angle1Based);
	// (Re)play the selected (or last) completed event from its IN on the
	// current angle at the resolved speed. No-op while following live (the
	// angle buttons then only pick which camera the preview mirrors).
	void replayCurrent();
	// the reference controller frame-by-frame forward: nudge the playhead on by one frame and show
	// it. The engine plays ranges, so a step is a very short range — there is
	// no playhead in it to move (see the definition).
	void stepFrameForward();
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

	// --- preview render callback (runs on the OBS graphics thread) ---
	static void drawChannelA(void *data, uint32_t cx, uint32_t cy);

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

	// What the graphics thread is handed for a tile: it may not dereference
	// anything but this (see drawTile).
	struct TileCtx {
		MultiReplayDock *dock = nullptr;
		int slot = 0; // index into tiles_ / tileSource_
	};
	struct PreviewTile {
		QWidget *box = nullptr;
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

	std::array<PreviewTile, kMaxPreviewTiles> tiles_{};
	std::array<TileCtx, kMaxPreviewTiles> tileCtx_{};
	std::mutex tileMutex_; // pointer copy + addref only
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
	QButtonGroup *anglesA_ = nullptr;
	// the reference controller's green channel strip under the A preview.
	QLabel *chanBadge_ = nullptr; // "A1"
	QLabel *chanStrip_ = nullptr; // the three information lines

	// recording / status
	QPushButton *recBtn_ = nullptr;
	QLabel *statusLbl_ = nullptr;
	QLabel *clockLbl_ = nullptr;   // wall clock + remaining recording time
	QLabel *projectLbl_ = nullptr; // shows active project name

	// transport
	int currentAngle1_ = 1; // dock-selected angle (1-based) for replay/preview
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
	QLabel *tcLbl_ = nullptr;
	QPushButton *playPauseBtn_ = nullptr;
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
	bool refreshing_ = false; // guards itemChanged during table rebuilds
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
	std::mutex previewMutex_;               // pointer copy + addref only
	obs_source_t *previewSource_ = nullptr; // owned ref, read by the GFX thread
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
	int64_t displayDurNs_ = 0;             // how much of it there is
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
};

} // namespace multireplay
