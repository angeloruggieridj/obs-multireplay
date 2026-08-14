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

LIFECYCLE — read before touching this class. The obs_display_t owns a D3D swap
chain bound to ONE native window handle. That binding is the fragile part: OBS
re-parents its docks (restoring the saved layout at startup, floating, tabbing,
re-docking), and Qt answers a re-parent by destroying this widget's native
window and making a new one. A display left on the old handle keeps presenting
into a window that no longer exists — silently, because nothing on the Qt side
can see it, and libobs does not complain either. So the handle the display was
created against is remembered, re-checked (see recheckWindow) and every
create/destroy is logged with it: an interface that goes black must be
diagnosable from the OBS log alone.
*/

#pragma once

#include <obs.h>

#include <QWidget>
#include <atomic>
#include <functional>

namespace multireplay {

class OBSQTDisplay : public QWidget {
	Q_OBJECT

public:
	explicit OBSQTDisplay(QWidget *parent = nullptr);
	~OBSQTDisplay() override;

	// Process-wide counters, for the automated gate. "How many displays were
	// created" and "how many were found presenting into a window that no
	// longer exists" were only ever visible by reading the OBS log by eye —
	// and with a preview per angle now, reading the log by eye is exactly the
	// check that stops being done. Nothing else reads these.
	static int createdCount() { return createdCount_.load(); }
	static int strandedCount() { return strandedCount_.load(); }

	// Qt must NOT paint over the native surface OBS renders into.
	QPaintEngine *paintEngine() const override { return nullptr; }

	obs_display_t *display() const { return display_; }

	// Register/replace the per-frame render callback. `cx`/`cy` are the
	// display pixel size. Safe to call before the display exists; the
	// callback is (re)attached once it is created.
	void setRenderCallback(void (*draw)(void *, uint32_t, uint32_t),
			       void *data);

	// Confirm the display still matches this widget's native window, and
	// create it when it is missing. Two integer compares plus an IsWindow()
	// in the common case, so the dock calls it from its poll timer: Qt does
	// not reliably tell a widget that the window under it has been replaced,
	// and a poll is the only thing that can notice a display stranded on a
	// dead handle. Safe to call from the UI thread at any rate.
	void recheckWindow();

protected:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	bool event(QEvent *event) override;

private:
	// `why` is logged verbatim: it is the only breadcrumb left when the
	// preview misbehaves on a machine we cannot attach a debugger to.
	void createDisplay(const char *why);
	void destroyDisplay(const char *why);

	obs_display_t *display_ = nullptr;
	// The native handle `display_` was created against. Compared with the
	// widget's current handle to detect a re-parent Qt did not announce.
	WId createdWinId_ = 0;
	void (*drawCb_)(void *, uint32_t, uint32_t) = nullptr;
	void *drawData_ = nullptr;
	bool destroying_ = false;
	// One-shot guard so "cannot create the display yet" is logged once per
	// dry spell instead of on every paint event.
	bool deferralLogged_ = false;

	static std::atomic<int> createdCount_;
	static std::atomic<int> strandedCount_;
};

} // namespace multireplay
