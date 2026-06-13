/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "qt-display.hpp"
#include "plugin-support.h"

#include <obs-module.h>

#include <QResizeEvent>
#include <QShowEvent>
#include <QWindow>
#include <QGuiApplication>

namespace multireplay {

namespace {

// Translate a Qt native window into a libobs gs_window. Windows and macOS are
// a direct winId() cast; Linux X11 also needs the X Display pointer. Wayland
// is NOT supported by this path (OBS needs the wl_surface) — run OBS under
// X11/XWayland for the embedded previews.
gs_window qtToGsWindow(QWindow *window)
{
	gs_window gswin = {};
	if (!window)
		return gswin;

#ifdef _WIN32
	gswin.hwnd = (void *)window->winId();
#elif defined(__APPLE__)
	gswin.view = (id)window->winId();
#else // Linux / *nix (X11). Wayland is not supported by this path.
	// NOTE: the embedded preview targets Windows/macOS. On Linux the X
	// Display pointer would need to be wired via the QX11Application native
	// interface; left null here to keep the build free of Qt private
	// modules. Run OBS under X11 if the Linux preview is needed.
	gswin.id = (uint32_t)window->winId();
	gswin.display = nullptr;
#endif
	return gswin;
}

} // namespace

OBSQTDisplay::OBSQTDisplay(QWidget *parent) : QWidget(parent)
{
	// The native surface is rendered by libobs, not by Qt's paint system.
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(64, 36);
}

OBSQTDisplay::~OBSQTDisplay()
{
	destroying_ = true;
	destroyDisplay();
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

void OBSQTDisplay::createDisplay()
{
	if (display_ || destroying_)
		return;

	QWindow *win = windowHandle();
	if (!win || !win->isExposed())
		return;
	if (width() <= 0 || height() <= 0)
		return;

	gs_init_data info = {};
	info.cx = (uint32_t)width();
	info.cy = (uint32_t)height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window = qtToGsWindow(win);

	display_ = obs_display_create(&info, 0x000000);
	if (!display_) {
		obs_log(LOG_ERROR, "OBSQTDisplay: obs_display_create failed");
		return;
	}
	if (drawCb_)
		obs_display_add_draw_callback(display_, drawCb_, drawData_);
}

void OBSQTDisplay::destroyDisplay()
{
	if (!display_)
		return;
	if (drawCb_)
		obs_display_remove_draw_callback(display_, drawCb_, drawData_);
	obs_display_destroy(display_);
	display_ = nullptr;
}

void OBSQTDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_)
		createDisplay();
	if (display_)
		obs_display_resize(display_, (uint32_t)width(),
				   (uint32_t)height());
}

void OBSQTDisplay::paintEvent(QPaintEvent *)
{
	// Nothing: libobs owns the surface. Just make sure the display exists.
	if (!display_)
		createDisplay();
}

bool OBSQTDisplay::event(QEvent *e)
{
	if (e->type() == QEvent::Show || e->type() == QEvent::Expose)
		createDisplay();
	return QWidget::event(e);
}

} // namespace multireplay
