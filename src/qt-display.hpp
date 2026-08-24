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

The OTHER way this widget goes black is the display that is never created at
all. Creation is deferred whenever the native window is not ready and the
dock's poll retries, and both of those are correct — but a retry that keeps
failing used to look exactly like one that succeeded half a second later: one
line in the log, then silence. Three things fix that, and they belong together:
the dry spell is TIMED rather than flagged (blockedMs), it is re-logged while it
lasts, and once the handle is alive and the widget has a size the display is
created even if Qt never raises an exposure flag that libobs does not need.
*/

#pragma once

#include <obs.h>

#include <QString>
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
	// ...and how many of those creations had to be FORCED past Qt's exposure
	// bookkeeping (see createDisplay). Zero is the normal number; a run where
	// it is not zero worked, but worked the long way round, and that is worth
	// seeing in the verdict instead of only in the log.
	static int forcedCount() { return forcedCount_.load(); }
	// ...and how many were found still sitting on their OWN handle, alive and
	// unchanged, while the TOP-LEVEL that handle lives in had been destroyed
	// and rebuilt underneath them. That is the case strandedCount_ cannot
	// see, and it is measured rather than acted on — see recheckWindow.
	static int reparentedCount() { return reparentedCount_.load(); }

	// How long this widget has been asking for a display it cannot get, in
	// milliseconds; 0 when it is not waiting (it either has one or has not
	// been asked). A preview widget on screen with no display behind it is a
	// black rectangle — indistinguishable from a camera with no signal — and
	// this is the only number that can tell the two apart from outside.
	int64_t blockedMs() const;

	// Qt must NOT paint over the native surface OBS renders into — EXCEPT
	// where there is no native surface to render into. On a platform this
	// widget cannot serve (Wayland: libobs binds a swap chain to an X window
	// and Wayland hands out a wl_surface) it becomes an ordinary widget that
	// paints a sentence saying so, because a black rectangle is
	// indistinguishable from a camera with no signal (A1).
	QPaintEngine *paintEngine() const override
	{
		return unsupported_ ? QWidget::paintEngine() : nullptr;
	}

	// Empty when the previews work here. Set once, in the constructor.
	QString unsupportedReason() const { return unsupportedText_; }

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
	//
	// `why` names the caller and is logged verbatim on every create/destroy.
	// It is not decoration: every display in this plugin has always been
	// logged as created "— recheck", which said nothing about WHICH of the
	// five entry points (poll, paint, resize, show, re-parent) actually got
	// there first, and that is the first thing you want to know when one of
	// them stops arriving.
	void recheckWindow(const char *why = "poll");

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
	// The TOP-LEVEL that handle belonged to at creation. Our own handle
	// surviving is NOT the same as the window surviving: setWindowFlags() on
	// an ancestor rebuilds that ancestor's native window and Qt re-parents
	// the existing native children into the new one, handles untouched. So
	// createdWinId_ still matches, IsWindow() still says yes, and the swap
	// chain is presenting into a composition tree that was rebuilt beneath
	// it — with nothing in this file able to say so.
	unsigned long long createdRootWinId_ = 0;
	// The last ancestor already reported, so a change is logged once rather
	// than at the poll rate.
	unsigned long long reportedRootWinId_ = 0;
	void (*drawCb_)(void *, uint32_t, uint32_t) = nullptr;
	void *drawData_ = nullptr;
	bool destroying_ = false;
	// When the current dry spell began (monotonic ns), 0 = not waiting. The
	// retry itself is driven by the dock's poll, so what this holds is not
	// "have we tried again" but "how long have we been failing" — which is
	// what both the forced create and the gate are decided on. It replaces a
	// one-shot bool that logged the first failure and then said nothing ever
	// again: a preview that never appeared left exactly one line in the log
	// and no way to tell a slow start from a permanent one.
	// Set once in the constructor: this platform cannot back an obs_display,
	// and the widget says so instead of being black.
	bool unsupported_ = false;
	QString unsupportedText_;
	uint64_t blockedSinceNs_ = 0;
	// Last time the dry spell was logged, so the retry is audible without
	// being a flood at 30 Hz.
	uint64_t blockedLoggedNs_ = 0;

	static std::atomic<int> createdCount_;
	static std::atomic<int> strandedCount_;
	static std::atomic<int> forcedCount_;
	static std::atomic<int> reparentedCount_;
};

} // namespace multireplay
