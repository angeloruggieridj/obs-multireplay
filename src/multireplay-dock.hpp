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
// SeekBar — modern timeline scrubber.
//
// A flat custom-painted bar (not a bead-on-rail QSlider): it draws the full
// recorded timeline, highlights the seekable region (footage already flushed
// to disk), fills the played-up-to-position portion with the accent colour and
// renders a slim handle at the playhead. Clicking or dragging emits fractions
// in [0,1]; the host maps them onto the master timeline.
// ---------------------------------------------------------------------------
class SeekBar : public QWidget {
	Q_OBJECT

public:
	explicit SeekBar(QWidget *parent = nullptr);

	// position/duration/seekable expressed as fractions of the timeline
	// [0,1]; -1 (default seekableFrac) means "whole bar is seekable".
	void setProgress(double positionFrac, double seekableFrac = 1.0);
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

	double positionFrac_ = 0.0;
	double seekableFrac_ = 1.0;
	double dragFrac_ = 0.0;
	bool dragging_ = false;
	QString overlay_;
	std::vector<std::pair<double, double>> markers_;
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
	};
	PreviewStats previewStats() const;

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
	// list tabs + search + Live, then the channel A preview with its green
	// status strip, then the event table, then the two rows of controls and
	// the full-width position bar. buildBottomBar() owns the last two.
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
	int64_t playheadNs_ = 0;
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

	// Master-timeline window currently drawn on the seekbar (ns).
	int64_t timelineStartNs_ = 0; // oldest replayable instant, 0 = nothing
	int64_t displayDurNs_ = 0;    // live edge - timelineStartNs_
	// Where the PROJECT's footage begins, which is what event timecodes and
	// YouTube chapters are measured from. Deliberately not timelineStartNs_:
	// that one follows the selected angle, so the whole table renumbered
	// itself when the operator pressed another camera button, and it is 0
	// whenever the selected angle has nothing — which printed marks as raw
	// monotonic time (a five-digit minute count). 0 = no footage at all, and
	// then there is no honest number to print.
	int64_t eventOriginNs_ = 0;
	// Origin the event table was last rendered against, so poll() can tell
	// when the columns need redrawing (see poll()).
	int64_t tableOriginNs_ = 0;

	// Raw ns (in, out) for each completed event — fractions computed in poll()
	// so markers shift leftward as recording time grows.
	std::vector<std::pair<int64_t, int64_t>> markerNs_;
};

} // namespace multireplay
