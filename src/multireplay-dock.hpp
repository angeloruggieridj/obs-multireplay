/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Native OBS dock panel. Replaces the former browser UI: it drives the same
in-process engine (ReplayCore / ReplayEngine / EventStore / PlaybackCoordinator)
directly via C++ calls instead of REST. Shows two live A/B program previews,
a seekbar over the recorded timeline, marker controls and the searchable
event list.
*/

#pragma once

#include <QWidget>
#include <cstdint>
#include <vector>

class QPushButton;
class QToolButton;
class QSlider;
class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QTableWidget;
class QButtonGroup;
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
	void onEventItemChanged(class QTableWidgetItem *item); // edit commit
	QWidget *makeCameraCell(int id, const bool *enabled); // 1..8 toggles
	void openSettings();     // configuration dialog
	int64_t markTimeNs() const; // Live=masterNow, Recorded=A playhead
	std::vector<int> selectedEventIds() const;
	void seekToFraction(double frac);
	void setAngle(int angle1Based);

	// --- preview render callback (runs on the OBS graphics thread) ---
	static void drawChannelA(void *data, uint32_t cx, uint32_t cy);

	// preview (single replay channel A)
	OBSQTDisplay *displayA_ = nullptr;
	QButtonGroup *anglesA_ = nullptr;

	// recording / status
	QPushButton *recBtn_ = nullptr;
	QLabel *statusLbl_ = nullptr;

	// transport
	SeekBar *seek_ = nullptr;
	QSlider *speed_ = nullptr;
	QLabel *speedLbl_ = nullptr;
	QLabel *tcLbl_ = nullptr;
	QPushButton *playPauseBtn_ = nullptr;
	QPushButton *reverseBtn_ = nullptr;
	QPushButton *nowBtn_ = nullptr;
	bool seekDragging_ = false;

	// markers
	QCheckBox *liveChk_ = nullptr;

	// events
	QComboBox *listCombo_ = nullptr;
	QLineEdit *search_ = nullptr;
	QTableWidget *events_ = nullptr;
	bool refreshing_ = false; // guards itemChanged during table rebuilds
	QCheckBox *toOutputChk_ = nullptr;
	QCheckBox *loopChk_ = nullptr;
	QCheckBox *musicChk_ = nullptr;

	QTimer *pollTimer_ = nullptr;

	// cached live-edge for seek mapping (ns)
	int64_t seekableNs_ = 0;
	int64_t durationNs_ = 0;
};

} // namespace multireplay
