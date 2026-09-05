/*
obs-multireplay — MultiReplayDock: the floating/full-screen window
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Split out of multireplay-dock.cpp (pure move, no behaviour change): the
QDockWidget-as-a-window plumbing — hostDock(), full screen, the floating
window's Minimize/Maximize boxes — is one coherent concern that used to sit
in the same 10k+ line file as the settings dialog, the poll loop and the
widget assembly. Splitting them into their own translation units keeps each
concern reviewable on its own.
*/

#include "multireplay-dock.hpp"
#include "angle-channels.hpp"
#include "dock-layout.hpp"
#include "error-locale.hpp"
#include "dock-style.hpp"
#include "dock-assets.hpp"
#include "dock-icons.hpp"
#include "qt-display.hpp"
#include "branch-output-install.hpp"
#include "camera-dedup.hpp"
#include "replay-core.hpp"
#include "updater.hpp"
#include "event-store.hpp"
#include "health.hpp"
#include "packet-tap.hpp"
#include "playback-coordinator.hpp"
#include "export.hpp"
#include "replay-channel.hpp"
#include "segment-index.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h> // obs_frontend_get_scenes (output-scene picker)
#include <util/platform.h>    // os_gettime_ns (arm watchdog deadline)

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QFontInfo>
#include <QSplitterHandle>
#include <QAbstractButton>
#include <QDateTime>
#include <QPushButton>
#include <QTabBar>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QItemSelectionModel>
#include <QHeaderView>
#include <QButtonGroup>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QDir>
#include <QGroupBox>
#include <QFrame>
#include <QListWidget>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QMessageBox>
#include <QStandardPaths>
#include <QProgressBar>
#include <QDesktopServices>
#include <QUrl>
#include <QDockWidget>
#include <QSizePolicy>
#include <QStyle>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QInputDialog>
#include <QMenu>
#include <QAction>
#include <QClipboard>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdlib>
#include <cstring>

namespace multireplay {

// ---------------------------------------------------------------------------
// ⛶  Full screen — only while the panel is floating
//
// The gesture this exists for: the operator pulls the dock out of OBS onto a
// second monitor and wants it to fill that monitor. Doing that by hand means
// dragging four edges to four screen edges, and doing it again every time a
// restored layout comes back with slightly different numbers.
//
// Three decisions worth keeping:
//
//  1. IT IS THE QDockWidget THAT GOES FULL SCREEN, not a window of our own.
//     Re-parenting this widget into a new top level would destroy the native
//     window of every OBSQTDisplay under it — both bays and every multiview
//     tile — and strand the obs_display bound to each (qt-display.hpp: the one
//     failure mode that file exists to avoid). A window STATE change touches no
//     child handle at all.
//  2. THE HOST IS RESOLVED EVERY TIME, never cached. OBS owns that dock: it
//     creates it after our constructor has run, and a layout restore can hand
//     the panel to a different one. Walking three parents is two pointer reads.
//  3. THE KEY IS POLLED, not wired to QDockWidget::topLevelChanged, for the
//     same reason — there is no single dock object whose lifetime we can pin a
//     connection to. Floating and full screen are both deliberate, rare
//     gestures; 4 Hz is far faster than either can be repeated.
// ---------------------------------------------------------------------------

QDockWidget *MultiReplayDock::hostDock() const
{
	for (QWidget *w = parentWidget(); w; w = w->parentWidget())
		if (auto *d = qobject_cast<QDockWidget *>(w))
			return d;
	return nullptr;
}

bool MultiReplayDock::panelIsFullScreen() const
{
	const QDockWidget *host = hostDock();
	return host && host->isFloating() && host->isFullScreen();
}

void MultiReplayDock::setPanelFullScreen(bool on)
{
	QDockWidget *host = hostDock();
	// Docked, there is nothing here to make full screen: the window belongs to
	// OBS. The key is hidden in that state, so this is the race (a re-dock
	// between the paint and the click), not the ordinary path.
	if (!host || !host->isFloating())
		return;
	if (on == host->isFullScreen())
		return;
	if (on) {
		// MAXIMISED IS A STATE, NOT A RECTANGLE. Saving the geometry of a
		// maximised window and setting it back gives a window that merely
		// looks maximised: it is not snapped to anything, and the first
		// thing that moves it proves the difference.
		preFullScreenMaximized_ = host->isMaximized();
		if (!preFullScreenMaximized_)
			preFullScreenGeom_ = host->geometry();
		host->showFullScreen();
	} else if (preFullScreenMaximized_) {
		host->showMaximized();
	} else {
		host->showNormal();
		// Qt keeps a pre-full-screen geometry of its own, but a dock that
		// has been floated, docked and floated again has had that memory
		// rewritten under it more than once — and coming back to a window
		// the size of a postage stamp in the corner of a monitor is worse
		// than not having the key.
		if (preFullScreenGeom_.isValid())
			host->setGeometry(preFullScreenGeom_);
	}
	obs_log(LOG_INFO, "[dock] panel %s",
		on ? "full screen" : "back to a window");
}


// ---------------------------------------------------------------------------
// The floating window gets a Minimize and a Maximize
//
// Qt floats a dock with a title bar it asked for by name — a title and a close
// box and nothing else — so the operator's right click on it finds Minimize and
// Maximize greyed out, and the two things a window on its own monitor most
// obviously wants are the two it cannot do. Reported from a real rig.
//
// The type matters as much as the hints. Qt floats a dock as a Qt::Tool, and a
// tool window on Windows is drawn with the small caption that has NO minimise
// box at all — the hint alone would change nothing there. Made a plain window it
// gets both boxes, and two things the operator also wanted anyway: a place in
// the taskbar and a stop in Alt+Tab, which is what a panel living on a second
// monitor for a whole match should have.
//
// Reapplied on every float because Qt rewrites the flags each time it makes one
// (QDockWidgetPrivate::setWindowState), so this cannot be done once at startup.
// ---------------------------------------------------------------------------

void MultiReplayDock::equipFloatingWindow(QDockWidget *host)
{
	if (!host || !host->isFloating() || host->isFullScreen())
		return;

	// RESCUE: A WINDOW THE PREVIOUS BUILD COULD PUT BEYOND REACH.
	// That build offered a minimise box, and pressing it was a one-way door
	// (see below): the panel went away and the taskbar had nothing to bring it
	// back with. The box is gone now, so a minimised floating dock can only be
	// that leftover — nothing in this build can produce one — and leaving it
	// there would keep the panel unreachable until OBS was restarted.
	//
	// Guarded on the MAIN WINDOW, because "Show desktop" minimises the owner
	// and everything it owns: bringing only this one back would drop a replay
	// panel onto a desktop the operator had just cleared. The state is cleared
	// rather than showNormal()'d so a window that was maximised comes back
	// maximised.
	if (host->isMinimized()) {
		auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
		if (main && !main->isMinimized()) {
			host->setWindowState(host->windowState() &
					     ~Qt::WindowMinimized);
			host->raise();
			if (!minimizeRescueLogged_) {
				minimizeRescueLogged_ = true;
				obs_log(LOG_INFO,
					"[dock] floating panel was left minimised "
					"by an older build — brought back");
			}
		}
	} else {
		minimizeRescueLogged_ = false;
	}
	const Qt::WindowFlags flags = host->windowFlags();
	// MID-DRAG, HANDS OFF. While the dock is being pulled out of OBS, Qt
	// floats it frameless with the mouse grabbed; setWindowFlags there
	// rebuilds the native window underneath the drag and the panel is dropped
	// on the floor. Two independent tells, because either one alone has a
	// moment where it is wrong: the frameless flag is Qt's unplugged state,
	// and a held button is the operator still holding it.
	if (flags.testFlag(Qt::FramelessWindowHint))
		return;
	if (QApplication::mouseButtons() != Qt::NoButton)
		return;

	// MAXIMISE, AND DELIBERATELY NOT MINIMISE. Reported from a real rig: with
	// the minimise box on, one press put the panel away and there was no way
	// back — the taskbar showed only the OBS main window and the log window.
	//
	// That is not a bug to chase, it is what this window IS. An OBS dock is
	// OWNED by the OBS main window (its QWidget parent, and on Windows its
	// owner HWND), and Windows gives an owned window no taskbar button and no
	// Alt+Tab stop of its own — it shows the owner instead. So a minimise box
	// here is a one-way door: the window goes somewhere with no handle on it,
	// and on a live rig that is the worst thing a panel can do. Getting a real
	// taskbar button would mean the dock stopping being OBS's dock, which is
	// also the thing that makes it dockable, save with the layout and come back
	// tomorrow. (An earlier version of this comment claimed the taskbar and
	// Alt+Tab as benefits of the type change below. They were never true.)
	//
	// The MAXIMISE box is real and works, and it is the one the request was
	// actually about: the window grown to the whole screen WITH its title bar,
	// which is a different thing from the ⛶ key's full screen.
	Qt::WindowFlags want = flags | Qt::WindowMaximizeButtonHint;
	want &= ~Qt::WindowFlags(Qt::WindowMinimizeButtonHint);
	// THE TYPE MATTERS AS MUCH AS THE HINT. Qt floats a dock as a Qt::Tool
	// (measured: 0x0a00300b), and a tool window on Windows is drawn with the
	// small caption that has no maximise box at all — the hint alone would
	// change nothing there.
	if ((flags & Qt::WindowType_Mask) == Qt::Tool)
		want = (want & ~Qt::WindowFlags(Qt::WindowType_Mask)) |
		       Qt::Window;
	if (want == flags)
		return;

	// setWindowFlags hides the widget, so the show() is not optional.
	//
	// THE HANDLES ARE LOGGED BECAUSE THE CLAIM THAT USED TO BE HERE WAS NEVER
	// MEASURED — and when it finally was, it was wrong in BOTH directions. It
	// read: "setWindowFlags ... rebuilds its native handle ... every
	// OBSQTDisplay underneath loses its window with it. That is survivable and
	// already handled (recheckWindow on the next tick rebuilds each display)."
	// Neither half happens. No display is ever destroyed or rebuilt after this
	// line in any session on record, and the handle itself does not change:
	// measured 0x23094c -> 0x23094c, because Qt applies these flags with
	// SetWindowLong/SetWindowPos rather than making a new window.
	//
	// The two integers stay printed. This was a candidate cause of the
	// black-screen reports — a swap chain left presenting into a window
	// rebuilt beneath it — and it was killed by printing them (with the
	// ancestor check in qt-display.cpp, which reports 0). Anyone who suspects
	// it again should read the log line rather than this comment: that is the
	// whole reason the line exists.
	//
	// internalWinId(), never winId(): the latter CREATES a native window when
	// there is none, which is the whole hazard this plugin has already paid
	// for once (qt-display.cpp).
	const QRect keep = host->geometry();
	const unsigned long long beforeWin =
		(unsigned long long)host->internalWinId();
	host->setWindowFlags(want);
	host->setGeometry(keep);
	host->show();
	obs_log(LOG_INFO,
		"[dock] floating window equipped: flags 0x%08x -> 0x%08x, "
		"top-level window 0x%llx -> 0x%llx",
		(unsigned)flags.toInt(), (unsigned)host->windowFlags().toInt(),
		beforeWin, (unsigned long long)host->internalWinId());
}

void MultiReplayDock::refreshFullScreenKey()
{
	if (!fullScreenBtn_)
		return;
	QDockWidget *host = hostDock();
	// THE FILTER GOES ON THE DOCK, NOT ON US. The double click the operator
	// aims at the title bar never reaches this widget: with a native frame it
	// is a non-client event delivered to the window, and Qt's own handler on
	// the QDockWidget answers it by re-docking. Filtering the dock is the only
	// place upstream of that. Re-hooked rather than hooked once, because OBS
	// owns that object and a layout restore can hand the panel to another.
	if (host && filteredHost_ != host) {
		if (filteredHost_)
			filteredHost_->removeEventFilter(this);
		host->installEventFilter(this);
		filteredHost_ = host;
	}
	const bool floating = host && host->isFloating();
	if (floating)
		equipFloatingWindow(host);
	const int want = floating ? 1 : 0;
	if (fullScreenKeyShown_ != want) {
		fullScreenKeyShown_ = want;
		fullScreenBtn_->setVisible(floating);
	}
	// Lit means "this panel owns the screen". Read off the WINDOW, not off the
	// last click: OBS can put the dock back into the main window under us, and
	// a key still lit after that is a key that lies about where you are.
	const bool full = floating && host->isFullScreen();
	if (fullScreenBtn_->isChecked() != full) {
		QSignalBlocker block(fullScreenBtn_);
		fullScreenBtn_->setChecked(full);
	}

	// §6.5 — GALLERY: either way the panel ends up owning the whole screen.
	// Fullscreen is the ⛶ key; maximised is a floating window the operator
	// dragged to fill it by hand (the title bar's own button, or a double
	// click — see title_double_click_leaves_the_panel_alone) without ever
	// pressing ⛶. Both put the operator 1-2 m from the screen instead of the
	// ~60 cm a docked panel assumes, and the section keys should read from
	// there the same way either time.
	const bool gallery = floating && (host->isFullScreen() || host->isMaximized());
	if (strip_ && galleryScale() != gallery) {
		setGalleryScale(gallery);
		// refreshAllBlocks(), NOT applyPanelMode(force=true): the
		// arrangement itself (Wide/Short/Tall) has not changed, only the
		// metrics inside it, and ControlStrip::setStacked's own first
		// line — "if (forcedStack_ == on) return" — means asking for the
		// SAME arrangement again is a no-op that never reaches
		// KeyBlock::apply() at all. Measured: without this, entering
		// fullscreen flipped the flag and did nothing else — every key
		// stayed exactly the size it already was.
		strip_->refreshAllBlocks();
	}
}

} // namespace multireplay
