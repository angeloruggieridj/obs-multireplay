/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <obs-module.h>

#include "qt-display.hpp"
#include "plugin-support.h"

#include <QResizeEvent>
#include <QShowEvent>
#include <QWindow>
#include <QGuiApplication>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace multireplay {

namespace {

// Translate a Qt native window into a libobs gs_window. Windows and macOS are
// a direct winId() cast; Linux X11 also needs the X Display pointer. Wayland
// is NOT supported by this path (OBS needs the wl_surface) — run OBS under
// X11/XWayland for the embedded previews.
gs_window qtToGsWindow(WId wid)
{
	gs_window gswin = {};
#ifdef _WIN32
	gswin.hwnd = (void *)wid;
#elif defined(__APPLE__)
	gswin.view = (id)wid;
#else // Linux / *nix (X11). Wayland is not supported by this path.
	// NOTE: the embedded preview targets Windows/macOS. On Linux the X
	// Display pointer would need to be wired via the QX11Application native
	// interface; left null here to keep the build free of Qt private
	// modules. Run OBS under X11 if the Linux preview is needed.
	gswin.id = (uint32_t)wid;
	gswin.display = nullptr;
#endif
	return gswin;
}

// True when the handle still names a window the OS owns. On Windows a re-dock
// can DestroyWindow() the handle behind our back, and binding (or keeping) a
// swap chain on a destroyed HWND is precisely the failure this file guards
// against. Elsewhere there is no cheap equivalent, so a non-zero handle is
// taken at face value.
bool handleAlive(WId wid)
{
	if (!wid)
		return false;
#ifdef _WIN32
	return IsWindow((HWND)wid) != FALSE;
#else
	return true;
#endif
}

// The top-level window the handle belongs to. Logged on creation so a display
// that ended up on the wrong window (a floating dock that is about to be
// re-docked, say) is visible in the log instead of having to be guessed.
unsigned long long rootOf(WId wid)
{
#ifdef _WIN32
	if (!wid)
		return 0;
	return (unsigned long long)(uintptr_t)GetAncestor((HWND)wid, GA_ROOT);
#else
	return (unsigned long long)wid;
#endif
}

} // namespace

OBSQTDisplay::OBSQTDisplay(QWidget *parent) : QWidget(parent)
{
	// These are the attributes OBS Studio itself sets on its previews, and
	// they have to be kept together — they describe ONE arrangement, not a
	// menu to pick from:
	//   WA_NativeWindow            — gives libobs a real window handle to bind
	//                                a D3D11 swap chain to.
	//   WA_PaintOnScreen           — with paintEngine() returning nullptr this
	//                                is not optional. It is what takes this
	//                                widget out of Qt's back buffer: without
	//                                it Qt still composes the widget into the
	//                                top-level back buffer and then BitBlts
	//                                that region onto this very handle, i.e.
	//                                GDI drawing into a flip-model swap chain
	//                                window. That combination is unsupported,
	//                                and when it goes wrong it does not break
	//                                this preview — it breaks the flush of the
	//                                shared back buffer, so every pixel Qt
	//                                paints in the OBS window turns black
	//                                while the swap-chain previews (the
	//                                program) keep drawing. That is the
	//                                reported symptom, and it is why the
	//                                attribute is back.
	//   WA_OpaquePaintEvent /
	//   WA_StaticContents          — tell Qt this rectangle needs neither an
	//                                erase nor a repaint on resize; libobs
	//                                fills it every frame.
	//   WA_NoSystemBackground      — no WM_ERASEBKGND flash before the first
	//                                rendered frame.
	//   WA_DontCreateNativeAncestors — only this widget gets its own handle;
	//                                the parent chain stays alien-free.
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(64, 36);
}

OBSQTDisplay::~OBSQTDisplay()
{
	destroying_ = true;
	destroyDisplay("widget destroyed");
}

void OBSQTDisplay::setRenderCallback(void (*draw)(void *, uint32_t, uint32_t),
				     void *data)
{
	if (display_ && drawCb_)
		obs_display_remove_draw_callback(display_, drawCb_, drawData_);
	drawCb_ = draw;
	drawData_ = data;
	if (display_ && drawCb_)
		obs_display_add_draw_callback(display_, drawCb_, drawData_);
}

void OBSQTDisplay::createDisplay(const char *why)
{
	if (display_ || destroying_)
		return;

	// internalWinId() reports the handle Qt has ALREADY created and never
	// creates one. windowHandle()->winId() does create one, and asking for it
	// while Qt is half-way through re-parenting the dock — which is exactly
	// when WinIdChange arrives, the old handle already destroyed — hands
	// libobs a brand-new orphan window that Qt throws away a moment later.
	// The swap chain then presents into nothing for the rest of the session.
	const WId wid = internalWinId();
	const char *blocked = nullptr;
	if (!handleAlive(wid)) {
		blocked = "no live native window handle yet";
	} else {
		QWindow *win = windowHandle();
		if (!win || !win->isExposed())
			blocked = "native window not exposed yet";
		else if (width() <= 0 || height() <= 0)
			blocked = "widget has no size yet";
	}
	if (blocked) {
		// Not an error by itself (a dock tabbed behind another is never
		// exposed), but a preview that never appears must leave a trace.
		if (!deferralLogged_) {
			deferralLogged_ = true;
			obs_log(LOG_INFO,
				"[display] not created (%s): %s [winId=0x%llx %dx%d]",
				why, blocked, (unsigned long long)wid, width(),
				height());
		}
		return;
	}

	gs_init_data info = {};
	info.cx = (uint32_t)width();
	info.cy = (uint32_t)height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window = qtToGsWindow(wid);

	display_ = obs_display_create(&info, 0x000000);
	if (!display_) {
		obs_log(LOG_ERROR,
			"[display] obs_display_create FAILED on winId=0x%llx (%s)",
			(unsigned long long)wid, why);
		return;
	}
	createdWinId_ = wid;
	deferralLogged_ = false;
	obs_log(LOG_INFO,
		"[display] created on winId=0x%llx (top-level 0x%llx) %ux%u — %s",
		(unsigned long long)wid, rootOf(wid), info.cx, info.cy, why);

	if (drawCb_)
		obs_display_add_draw_callback(display_, drawCb_, drawData_);
}

void OBSQTDisplay::destroyDisplay(const char *why)
{
	if (!display_)
		return;
	obs_log(LOG_INFO, "[display] destroyed (winId=0x%llx) — %s",
		(unsigned long long)createdWinId_, why);
	if (drawCb_)
		obs_display_remove_draw_callback(display_, drawCb_, drawData_);
	obs_display_destroy(display_);
	display_ = nullptr;
	createdWinId_ = 0;
	deferralLogged_ = false;
}

void OBSQTDisplay::recheckWindow()
{
	if (destroying_)
		return;

	const WId wid = internalWinId();

	// Re-docking, floating, tabbing, or OBS restoring its saved dock layout
	// at startup all replace this widget's native window. Qt announces that
	// with WinIdChange, but the handle is the only thing that can be trusted:
	// a display left on a window that has been destroyed keeps presenting a
	// swap chain into it forever, and neither Qt nor libobs says a word.
	if (display_ &&
	    (wid != createdWinId_ || !handleAlive(createdWinId_))) {
		obs_log(LOG_WARNING,
			"[display] stranded: created on winId=0x%llx, widget is now "
			"0x%llx (alive=%d) — rebuilding",
			(unsigned long long)createdWinId_,
			(unsigned long long)wid,
			handleAlive(createdWinId_) ? 1 : 0);
		destroyDisplay("stale native window");
	}

	if (!display_ && isVisible())
		createDisplay("recheck");
}

void OBSQTDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	recheckWindow();
	if (display_)
		obs_display_resize(display_, (uint32_t)width(),
				   (uint32_t)height());
}

void OBSQTDisplay::paintEvent(QPaintEvent *)
{
	// Nothing to paint: libobs owns this surface. A paint request does mean
	// the window is up and mapped, which is a good moment to (re)check it.
	recheckWindow();
}

bool OBSQTDisplay::event(QEvent *e)
{
	switch (e->type()) {
	case QEvent::Show:
		if (display_)
			obs_display_set_enabled(display_, true);
		recheckWindow();
		break;
	case QEvent::Hide:
		// Tabbed behind another dock: stop presenting, keep the display.
		if (display_)
			obs_display_set_enabled(display_, false);
		break;
	case QEvent::ParentChange:
		// The dock was moved, floated, tabbed or re-docked. The window
		// under us may have been swapped without a WinIdChange reaching
		// here first, so verify rather than assume.
		recheckWindow();
		break;
	case QEvent::WinIdChange:
		// Qt has just replaced this widget's native window. Only LET GO of
		// the old display here. The new window is not exposed yet at this
		// point, and forcing it into existence to bind a swap chain to it
		// is how the display used to end up on a handle Qt discarded a
		// moment later. The next show/paint/recheck creates it for real.
		destroyDisplay("winId change");
		break;
	default:
		break;
	}
	return QWidget::event(e);
}

} // namespace multireplay
