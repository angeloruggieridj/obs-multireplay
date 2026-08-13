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

#include <QWidget>
#include <atomic>
#include <climits>
#include <cstdint>
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
class QTableWidget;
class QTableWidgetItem;
class QButtonGroup;
class QSplitter;
class QTimer;
class QVBoxLayout;
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

private:
	// --- UI assembly ---
	QWidget *buildPreview();
	QWidget *buildTransport();
	QWidget *buildMarkers();
	QWidget *buildEvents();

	// --- engine interaction ---
	void poll();             // periodic transport/status refresh
	void refreshEvents();    // reload the selected list into the table
	void refreshAngles();    // update angle button labels from camera displayName
	// the reference controller: the 20 lists can be named ("Gol", "Falli"). Re-labels the combo
	// from the store; the selection is preserved.
	void refreshListNames();
	void renameListDialog(); // gear menu → rename the selected list
	void onEventItemChanged(QTableWidgetItem *item); // edit commit
	// Inspector panel below the table: per-angle toggle · comment · vel% for the
	// currently selected event (replaces the cramped in-table camera cards).
	void populateInspector(int eventId);
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
	// Apply a replay speed (5..100): becomes the default for events without a
	// per-angle override, and re-cues the current clip from its in-point at
	// the new speed (broadcast-style).
	void applyReplaySpeed(int pct);

	// --- event filter: double-click on note labels ---
	bool eventFilter(QObject *watched, QEvent *event) override;

	// --- preview render callback (runs on the OBS graphics thread) ---
	static void drawChannelA(void *data, uint32_t cx, uint32_t cy);

	// preview (single replay channel A)
	OBSQTDisplay *displayA_ = nullptr;
	QButtonGroup *anglesA_ = nullptr;

	// recording / status
	QPushButton *recBtn_ = nullptr;
	QLabel *statusLbl_ = nullptr;
	QLabel *projectLbl_ = nullptr; // shows active project name

	// transport
	int currentAngle1_ = 1; // dock-selected angle (1-based) for replay/preview
	// Default replay speed (slider). The engine has no speed of its own any
	// more: it is told the speed of the clip it is asked to play.
	int speedPct_ = 100;
	SeekBar *seek_ = nullptr;
	QSlider *speed_ = nullptr;
	QLabel *speedLbl_ = nullptr;
	QLabel *tcLbl_ = nullptr;
	QPushButton *playPauseBtn_ = nullptr;
	QPushButton *nowBtn_ = nullptr;
	bool seekDragging_ = false;

	// markers
	QCheckBox *liveChk_ = nullptr;

	// events
	QComboBox *listCombo_ = nullptr;
	QLineEdit *search_ = nullptr;
	QTableWidget *events_ = nullptr;
	// Inspector: framed panel under the table; its rows are rebuilt per selection.
	QGroupBox *inspector_ = nullptr;
	QVBoxLayout *inspectorLayout_ = nullptr;
	int inspectorEventId_ = 0;
	bool refreshing_ = false; // guards itemChanged during table rebuilds
	uint64_t lastEventVersion_ = UINT64_MAX; // UINT64_MAX → refresh on first poll
	// Largest event id seen: when a newer one appears (a fresh mark), the table
	// auto-selects it so "Riproduci selezionati" is one click; otherwise the
	// user's current selection is preserved across refreshes.
	int lastMaxEventId_ = 0;
	QCheckBox *toOutputChk_ = nullptr;
	QCheckBox *loopChk_ = nullptr;
	QCheckBox *musicChk_ = nullptr;

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
	// Until when showNotice()'s message owns the status line (master ns).
	int64_t noticeUntilNs_ = 0;

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
