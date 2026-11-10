// dock-native-mac.hpp — macOS only: make a floating dock's window an ordinary one.
//
// OBS docks that are torn off become Qt "tool" windows, which macOS keeps on a
// floating level ABOVE every normal window of the app. A big monitor wall on
// that level covers the main window and the control dock and nothing can be
// brought in front of it. Putting its window back on the normal level (and
// telling it not to hide when the app is deactivated) makes it behave like any
// other window: Cmd+` / Mission Control / a click on another window all work.
//
// The window is NOT recreated: only its level is changed through the
// Objective-C runtime, so the obs_display bound to it keeps presenting. No
// Objective-C++ file is needed.
#pragma once

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#include <QWidget>

inline void mrMakeWindowNormalLevel(QWidget *w, QWidget *mainWindow)
{
	if (!w)
		return;
	QWidget *top = w->window();
	// Docked inside the main window: nothing to do (and nothing to touch).
	if (!top || top == mainWindow || !top->isVisible())
		return;
	id view = (id)top->winId();
	if (!view)
		return;
	const SEL selWindow = sel_registerName("window");
	if (!class_respondsToSelector(object_getClass(view), selWindow))
		return;
	id win = ((id(*)(id, SEL))objc_msgSend)(view, selWindow);
	if (!win)
		return;
	const SEL selLevel = sel_registerName("level");
	const SEL selSetLevel = sel_registerName("setLevel:");
	Class cls = object_getClass(win);
	if (class_respondsToSelector(cls, selLevel) &&
	    class_respondsToSelector(cls, selSetLevel)) {
		const long level = ((long (*)(id, SEL))objc_msgSend)(win, selLevel);
		if (level != 0) // NSNormalWindowLevel
			((void (*)(id, SEL, long))objc_msgSend)(win, selSetLevel, 0L);
	}
	const SEL selFloat = sel_registerName("setFloatingPanel:");
	if (class_respondsToSelector(cls, selFloat))
		((void (*)(id, SEL, BOOL))objc_msgSend)(win, selFloat, NO);
	const SEL selHide = sel_registerName("setHidesOnDeactivate:");
	if (class_respondsToSelector(cls, selHide))
		((void (*)(id, SEL, BOOL))objc_msgSend)(win, selHide, NO);
}
#else
class QWidget;
inline void mrMakeWindowNormalLevel(QWidget *, QWidget *) {}
#endif
