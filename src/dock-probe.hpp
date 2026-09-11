#pragma once
// ---------------------------------------------------------------------------
// dock-probe.hpp — THE GEOMETRY THE CHECKS MEASURE, ONE COPY OF IT
// ---------------------------------------------------------------------------
//
// The mockup's --check (tools/dock-mockup) and the gate (selftest.cpp) ask the
// same questions of the same panel: where a zone sits, whether it spans the
// panel. A helper copied into each binary is two answers to one question, and
// two answers drift — the same lesson monitorRoomFor (dock-layout.hpp) was
// written for. So the measuring lives here, pure Qt with no OBS types (like
// dock-style.hpp), and both include it.

#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

namespace multireplay::probe {

// The toolbar's box — the one widget that holds all its rows (1 in Wide and
// Short, 3 in Tall). Named the same in the dock (dock-build.cpp) and the mockup.
inline QString toolbarBoxName()
{
	return QStringLiteral("mrToolbarBox");
}

// Where `w` sits, in `panel`'s coordinates. `panel` must be an ancestor of `w`.
inline QRect rectIn(const QWidget *w, const QWidget *panel)
{
	return QRect(w->mapTo(panel, QPoint(0, 0)), w->size());
}

// LAY «Le cinque zone» (1 Toolbar … in cima); SPEC §0 «Cinque zone, dall'alto:
// toolbar · blocco monitor · …». The toolbar starts above the first picture.
inline bool toolbarAboveMonitors(const QWidget *toolbar, const QWidget *monitors,
				 const QWidget *panel)
{
	return rectIn(toolbar, panel).top() < rectIn(monitors, panel).top();
}

// LAY Short: the toolbar crosses the whole panel, not the table's column alone.
// `slack` is the panel's own margins (4 + 4) with room to spare.
inline bool spansPanel(const QWidget *w, const QWidget *panel, int slack = 20)
{
	return w->width() >= panel->width() - slack;
}

// The numbers both checks print, so a red line says where each zone was.
inline QString zoneOrderDetail(const QWidget *toolbar, const QWidget *monitors,
			       const QWidget *panel)
{
	const QRect t = rectIn(toolbar, panel);
	const QRect m = rectIn(monitors, panel);
	return QStringLiteral("toolbar y%1 w%2, monitors y%3, panel w%4")
		.arg(t.top())
		.arg(t.width())
		.arg(m.top())
		.arg(panel->width());
}

} // namespace multireplay::probe
