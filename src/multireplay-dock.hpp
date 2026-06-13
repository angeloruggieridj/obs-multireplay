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

class MultiReplayDock : public QWidget {
	Q_OBJECT

public:
	explicit MultiReplayDock(QWidget *parent = nullptr);
	~MultiReplayDock() override;

private:
	// --- UI assembly ---
	QWidget *buildPreviews();
	QWidget *buildTransport();
	QWidget *buildMarkers();
	QWidget *buildEvents();

	// --- engine interaction ---
	void poll();             // periodic transport/status refresh
	void refreshEvents();    // reload the selected list into the table
	void openSettings();     // configuration dialog
	int64_t markTimeNs() const; // Live=masterNow, Recorded=A playhead
	std::vector<int> selectedEventIds() const;
	void seekToFraction(double frac);
	void setAngle(char channel, int angle1Based);

	// --- preview render callbacks (run on the OBS graphics thread) ---
	static void drawChannelA(void *data, uint32_t cx, uint32_t cy);
	static void drawChannelB(void *data, uint32_t cx, uint32_t cy);
	static void renderChannel(char id, uint32_t cx, uint32_t cy);

	// previews
	OBSQTDisplay *displayA_ = nullptr;
	OBSQTDisplay *displayB_ = nullptr;
	QButtonGroup *anglesA_ = nullptr;
	QButtonGroup *anglesB_ = nullptr;
	QCheckBox *linkedChk_ = nullptr;

	// recording / status
	QPushButton *recBtn_ = nullptr;
	QLabel *statusLbl_ = nullptr;

	// transport
	QSlider *seek_ = nullptr;
	QSlider *speed_ = nullptr;
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
	QCheckBox *toOutputChk_ = nullptr;
	QCheckBox *loopChk_ = nullptr;
	QCheckBox *musicChk_ = nullptr;

	QTimer *pollTimer_ = nullptr;

	// cached live-edge for seek mapping (ns)
	int64_t seekableNs_ = 0;
	int64_t durationNs_ = 0;
};

} // namespace multireplay
