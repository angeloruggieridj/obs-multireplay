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

#include <QLabel>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QTabBar>
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

// ── THE NOTICE LIVES IN MARCA'S FOOTER (S2, B2, D5) ─────────────────────
// TAS «Le due barre»: under MARCA|REVIEW there is only the SeekBar. The status
// row that used to sit there is gone; its one job left — the answer to a key
// just pressed — is a label beside the health badge. The retired row's name is
// kept here so a check can say it is GONE, not merely hidden.
inline QString statusRowName()
{
	return QStringLiteral("mrStatusBar");
}
inline QString marcaFootName()
{
	return QStringLiteral("mrMarcaFoot");
}
inline QString noticeName()
{
	return QStringLiteral("mrNotice");
}

// The notice label, when it sits inside MARCA's footer; nullptr otherwise.
inline QLabel *noticeInMarcaFoot(const QWidget *panel)
{
	const QWidget *foot = panel->findChild<QWidget *>(marcaFootName());
	return foot ? foot->findChild<QLabel *>(noticeName()) : nullptr;
}

// What the notice SAYS, in full. The label elides a sentence that does not fit
// the footer and carries the whole of it in its tooltip, so the text alone can
// be a shortened copy — a check that looks for a sentence reads this.
inline QString noticeFullText(const QLabel *l)
{
	if (!l)
		return QString();
	return l->toolTip().isEmpty() ? l->text() : l->toolTip();
}

// TALL: MARCA is a tab behind REVIEW, so its footer is out of sight. While the
// footer has something to say and MARCA is not the current tab, the MARCA tab
// title carries « •» and the tab bar's `alert` property (coloured warn by the
// sheet). Tab 1 is MARCA (TwoPanelStrip).
inline bool marcaTabFlagged(const QTabBar *tabs)
{
	return tabs && tabs->count() > 1 &&
	       tabs->tabText(1).endsWith(QStringLiteral(" •")) &&
	       tabs->property("alert").toBool();
}
inline bool marcaTabPlain(const QTabBar *tabs)
{
	return tabs && tabs->count() > 1 &&
	       tabs->tabText(1) == QStringLiteral("MARCA") &&
	       !tabs->property("alert").toBool();
}

// Nothing between the command panel and the position bar: the gap from the
// strip's bottom edge to the bar's top edge. The old status row was 26 px, so
// anything past a few pixels of layout spacing means a row came back.
inline int gapStripToSeek(const QWidget *strip, const QWidget *seek,
			  const QWidget *panel)
{
	return rectIn(seek, panel).top() - rectIn(strip, panel).bottom() - 1;
}

} // namespace multireplay::probe
