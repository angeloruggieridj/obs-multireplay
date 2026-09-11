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

#include <QAbstractButton>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QTabBar>
#include <QWidget>

#include <algorithm>
#include <cmath>

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

// ── PIXELS ────────────────────────────────────────────────────────────────
// Read back from a grab of the panel. One copy: the mockup's colour checks and
// the gate's header check use the same two.

// The most common colour in a region: the fill, whatever it happens to be.
inline QColor dominant(const QImage &img, const QRect &r)
{
	QHash<QRgb, int> counts;
	for (int y = r.top(); y <= r.bottom(); y++)
		for (int x = r.left(); x <= r.right(); x++)
			counts[img.pixel(x, y)]++;
	QRgb best = 0;
	int most = -1;
	for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
		if (it.value() > most) {
			most = it.value();
			best = it.key();
		}
	return QColor::fromRgb(best);
}

inline bool sameColour(const QColor &a, const QColor &b, int tol = 6)
{
	return std::abs(a.red() - b.red()) <= tol &&
	       std::abs(a.green() - b.green()) <= tol &&
	       std::abs(a.blue() - b.blue()) <= tol;
}

// ── THE PANEL HEADERS (K1, K2, R1) ──────────────────────────────────────
// TAS .sub .hd: each of MARCA and REVIEW opens with a header ROW — a rule under
// it, not a box around it — whose title is centred on the row; REVIEW's IN
// OUTPUT sits at the far right. `side` is "mrMarca" or "mrReview"
// (TwoPanelStrip), and the header is that panel's #mrPanelHeader.
inline QWidget *panelHeader(const QWidget *panel, const QString &side)
{
	const QWidget *box = panel->findChild<QWidget *>(side);
	return box ? box->findChild<QWidget *>(QStringLiteral("mrPanelHeader"))
		   : nullptr;
}

inline double centreX(const QRect &r)
{
	return r.left() + r.width() / 2.0;
}
inline int rightEdge(const QRect &r)
{
	return r.left() + r.width();
}

// What the header SHOWS, in `panel`'s coordinates: the union of its labels
// that carry text and, with `withKeys`, its keys. An empty label (REVIEW's
// event with nothing selected) is not content: it has no ink to centre.
inline QRect headerContentRect(const QWidget *header, const QWidget *panel,
			       bool withKeys)
{
	QRect u;
	for (QWidget *c : header->findChildren<QWidget *>()) {
		if (!c->isVisible() || c->width() <= 1)
			continue;
		if (const auto *l = qobject_cast<const QLabel *>(c)) {
			if (l->text().isEmpty())
				continue;
		} else if (!(withKeys && qobject_cast<const QAbstractButton *>(c))) {
			continue;
		}
		u = u.united(rectIn(c, panel));
	}
	return u;
}

// TAS .sub .hd{border-bottom:1px solid} — A RULE, NOT A BOX. Read on a grab of
// the panel (`hr` in the panel's logical coordinates; the image's device pixel
// ratio is honoured): the left tenth of the header, where nothing is laid out
// because both headers centre their content, and its top two rows are the
// ground all the way down to the rule; and the bottom row differs from the
// ground across nine tenths of the header's width. A box drawn around the
// header (its left edge, its top line, its corner) fails the first two.
inline bool headerHasNoFrame(const QImage &shot, const QRect &hr,
			     QString *why = nullptr)
{
	const qreal dpr = shot.devicePixelRatio() > 0 ? shot.devicePixelRatio() : 1.0;
	const auto dev = [dpr](int v) { return (int)std::floor(v * dpr); };
	const int L = dev(hr.left()), T = dev(hr.top());
	const int R = std::min(shot.width() - 1,
			       (int)std::ceil((hr.left() + hr.width()) * dpr) - 1);
	const int B = std::min(shot.height() - 1,
			       (int)std::ceil((hr.top() + hr.height()) * dpr) - 1);
	// Clean down to two logical rows above the rule.
	const int clean = dev(hr.top() + hr.height() - 2) - 1;
	if (R - L < 40 || clean - T < 8) {
		if (why)
			*why = QStringLiteral("header too small (%1x%2)")
				       .arg(hr.width())
				       .arg(hr.height());
		return false;
	}
	const QColor bg = dominant(shot, QRect(QPoint(L, T), QPoint(R, clean)));
	const auto off = [&](int x, int y) {
		return !sameColour(QColor::fromRgb(shot.pixel(x, y)), bg);
	};
	const int stripR = L + std::max(8, (R - L) / 10);
	for (int y = T; y <= clean; y++)
		for (int x = L; x < stripR; x++)
			if (off(x, y)) {
				if (why)
					*why = QStringLiteral(
						       "ink at the left edge (%1,%2): "
						       "a box, not a rule")
						       .arg(x - L)
						       .arg(y - T);
				return false;
			}
	for (int y = T; y <= T + 1; y++)
		for (int x = L; x <= R; x++)
			if (off(x, y)) {
				if (why)
					*why = QStringLiteral(
						       "ink on the top line at x%1: "
						       "a box, not a rule")
						       .arg(x - L);
				return false;
			}
	int ruled = 0;
	for (int x = L; x <= R; x++)
		if (off(x, B))
			ruled++;
	const bool rule = ruled * 10 >= (R - L + 1) * 9;
	if (why)
		*why = rule ? QStringLiteral("rule only")
			    : QStringLiteral("no rule under it (%1 of %2 px)")
				      .arg(ruled)
				      .arg(R - L + 1);
	return rule;
}

// The MARCA clock: TAS «09:52:20 · rim 01:10:24» — the time of day, no date.
inline bool isClockHms(const QString &s)
{
	if (s.size() != 8)
		return false;
	for (int i = 0; i < 8; i++) {
		const bool colon = i == 2 || i == 5;
		if (colon ? s[i] != QLatin1Char(':') : !s[i].isDigit())
			return false;
	}
	return true;
}

// THE WHOLE HEADER ANSWER, one copy for the mockup and the gate: frame, the
// MARCA group centred, REVIEW's title centred, IN OUTPUT at the far right, the
// clock without a date, and REC the compact key. `recKey`/`toOutput` are found
// by the caller (by mrKey) and may be null, which fails. `keyH` is the height
// the compact key must be (kHeaderKeyH, dock-layout.hpp). `detail` says where
// everything was.
inline bool headersConform(const QWidget *panel, const QImage &shot,
			   const QWidget *recKey, const QWidget *toOutput,
			   int keyH, QString *detail)
{
	const QWidget *mh = panelHeader(panel, QStringLiteral("mrMarca"));
	const QWidget *rh = panelHeader(panel, QStringLiteral("mrReview"));
	if (!mh || !rh || !recKey || !toOutput || !mh->isVisible() ||
	    !rh->isVisible()) {
		if (detail)
			*detail = QStringLiteral("marca header %1, review header %2, "
						 "REC %3, IN OUTPUT %4")
					  .arg(mh ? "found" : "MISSING")
					  .arg(rh ? "found" : "MISSING")
					  .arg(recKey ? "found" : "MISSING")
					  .arg(toOutput ? "found" : "MISSING");
		return false;
	}
	const QRect mr = rectIn(mh, panel), rr = rectIn(rh, panel);
	QString mWhy, rWhy;
	const bool mRule = headerHasNoFrame(shot, mr, &mWhy);
	const bool rRule = headerHasNoFrame(shot, rr, &rWhy);
	// TAS .sub.live .hd{justify-content:center}
	const QRect mc = headerContentRect(mh, panel, true);
	const double mOff = centreX(mc) - centreX(mr);
	// TAS .sub.review .hd .st{left:50%;transform:translateX(-50%)}
	const QRect rt = headerContentRect(rh, panel, false);
	const double rOff = centreX(rt) - centreX(rr);
	// TAS .sub.review .hd{justify-content:flex-end}: IN OUTPUT at the far right
	const int outGap = rightEdge(rr) - rightEdge(rectIn(toOutput, panel));
	const QLabel *clock = mh->findChild<QLabel *>(QStringLiteral("mrClock"));
	const bool hms = clock && isClockHms(clock->text());
	// TAS .key.sm{height:24px} — REC in the header is the compact key.
	const bool compact = recKey->height() == keyH;
	if (detail)
		*detail = QStringLiteral(
				  "MARCA %1 (group off centre %2 px), REVIEW %3 "
				  "(title off centre %4 px, IN OUTPUT %5 px from the "
				  "right), clock '%6', REC %7 px")
				  .arg(mRule ? QStringLiteral("rule") : mWhy)
				  .arg(mOff, 0, 'f', 1)
				  .arg(rRule ? QStringLiteral("rule") : rWhy)
				  .arg(rOff, 0, 'f', 1)
				  .arg(outGap)
				  .arg(clock ? clock->text() : QStringLiteral("NO CLOCK"))
				  .arg(recKey->height());
	return mRule && rRule && std::abs(mOff) <= 3.0 && std::abs(rOff) <= 3.0 &&
	       outGap >= 0 && outGap <= 10 && hms && compact && !mc.isEmpty() &&
	       !rt.isEmpty();
}

} // namespace multireplay::probe
