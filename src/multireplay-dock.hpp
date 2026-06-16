/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Native OBS dock panel. Drives the in-process engine (ReplayCore / MediaReplay /
EventStore / PlaybackCoordinator) directly via C++ calls. Shows the replay
preview (an OBS Media Source), a seekbar over the recorded timeline, marker
controls and the searchable, editable event list.
*/

#pragma once

#include <QWidget>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
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
	void onEventItemChanged(QTableWidgetItem *item); // edit commit
	QWidget *makeCameraCell(int id, const bool *enabled,
				const std::string *notes); // 1..8 toggles
	void openSettings();        // configuration dialog
	void newProjectDialog();    // New Project... menu action
	void openProjectDialog();   // Open Project... menu action
	void copyYouTubeChapters(); // copy chapter timestamps to clipboard
	int64_t markTimeNs() const; // Live=masterNow, Recorded=A playhead
	std::vector<int> selectedEventIds() const;
	void seekToFraction(double frac);
	void setAngle(int angle1Based);

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
	bool refreshing_ = false; // guards itemChanged during table rebuilds
	uint64_t lastEventVersion_ = UINT64_MAX; // UINT64_MAX → refresh on first poll
	QCheckBox *toOutputChk_ = nullptr;
	QCheckBox *loopChk_ = nullptr;
	QCheckBox *musicChk_ = nullptr;

	QSplitter *splitter_ = nullptr;

	QTimer *pollTimer_ = nullptr;
	int pollTick_ = 0;
	bool prevRecording_ = false;                      // detects REC start
	// shared_ptr keeps the atomic alive past dock destruction so the detached
	// refresh thread can safely store(false) without triggering a UAF.
	std::shared_ptr<std::atomic<bool>> sessionRefreshPending_ =
		std::make_shared<std::atomic<bool>>(false);

	// cached live-edge for seek mapping (ns)
	int64_t seekableNs_ = 0;
	int64_t durationNs_ = 0;
	int64_t displayDurNs_ = 0; // max(durationNs_, liveElapsed) updated every poll

	// Raw ns (in, out) for each completed event — fractions computed in poll()
	// so markers shift leftward as recording time grows.
	std::vector<std::pair<int64_t, int64_t>> markerNs_;
};

} // namespace multireplay
