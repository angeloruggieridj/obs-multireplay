/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Lightweight OBS video display embedded in a Qt widget. Ported from the
OBSQTDisplay pattern used by OBS Studio's UI (GPLv2) and by community plugins
(e.g. OPENSPHERE source-dock): a borderless QWidget whose native window backs
an obs_display_t; a draw callback renders an OBS source into it every frame.

Used by the dock to show the live "Replay A" program preview without going
through the (now removed) browser layer.
*/

#pragma once

#include <obs.h>

#include <QWidget>
#include <functional>

namespace multireplay {

class OBSQTDisplay : public QWidget {
	Q_OBJECT

public:
	explicit OBSQTDisplay(QWidget *parent = nullptr);
	~OBSQTDisplay() override;

	// Qt must NOT paint over the native surface OBS renders into.
	QPaintEngine *paintEngine() const override { return nullptr; }

	obs_display_t *display() const { return display_; }

	// Register/replace the per-frame render callback. `cx`/`cy` are the
	// display pixel size. Safe to call before the display exists; the
	// callback is (re)attached once it is created.
	void setRenderCallback(void (*draw)(void *, uint32_t, uint32_t),
			       void *data);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	bool event(QEvent *event) override;

private:
	void createDisplay();
	void destroyDisplay();

	obs_display_t *display_ = nullptr;
	void (*drawCb_)(void *, uint32_t, uint32_t) = nullptr;
	void *drawData_ = nullptr;
	bool destroying_ = false;
};

} // namespace multireplay
