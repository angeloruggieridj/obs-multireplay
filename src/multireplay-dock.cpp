/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multireplay-dock.hpp"
#include "angle-channels.hpp"
#include "dock-internal.hpp"
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
#include <QToolTip>
#include <QHelpEvent>
#include <QClipboard>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdlib>
#include <cstring>

namespace multireplay {

// ---------------------------------------------------------------------------
// Dock-wide stylesheet — the broadcast replay controller's palette.
//
// The colours are SAMPLED from the reference screenshots in "reference-gui/", not
// invented: an operator coming from the reference controller reads this panel by colour before he
// reads it by label, and "green means the angle I am on, orange means the row I
// have selected" is muscle memory worth more than any house style. Nothing here

SeekBar::SeekBar(QWidget *parent) : QWidget(parent)
{
	// Track + ruler + the 1 px of margin each side. The bar used to be 28 px
	// of pure track: enough to click, not enough to be recognised as the
	// control over the whole recording.
	setFixedHeight(kSeekTrackH + kSeekRulerH + 2);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setCursor(Qt::PointingHandCursor);
	setMouseTracking(false);

	coalesceTimer_ = new QTimer(this);
	coalesceTimer_->setSingleShot(true);
	coalesceTimer_->setInterval(kCoalesceMs);
	connect(coalesceTimer_, &QTimer::timeout, this, [this] { flushPending(); });
}

QRect SeekBar::trackBand() const
{
	// The track plus the 1 px above it and the top row of the ruler, which
	// the playhead and the marker edges bleed into.
	return QRect(0, 0, width(), kSeekTrackH + 2);
}

QRect SeekBar::zoomHitRect() const
{
	if (durationNs_ <= 0)
		return QRect(); // nothing to be a fraction of, nothing to click
	// IN THE RULER ROW ONLY, not the track: the track is where a drag
	// starts a scrub, and a hit zone straddling both would make clicking
	// the zoom factor also move the playhead underneath it.
	const int m = 2;
	const int rulerY = 1 + kSeekTrackH;
	const int zw = 38; // "8.4×" plus padding — the widest factor this
			   // control ever shows (past that it reads as "many
			   // hundreds ×", which the entries in showZoomMenu
			   // never ask for).
	return QRect(width() - m - zw, rulerY, zw, kSeekRulerH);
}

void SeekBar::repaintNow(const QRect &r)
{
	if (r.isEmpty())
		return;
	// Anything already deferred is folded in and flushed with it: two
	// repaints of overlapping rectangles in the same millisecond are one
	// repaint, and leaving the deferred one behind would draw a bar that is
	// half new and half old.
	QRect u = r;
	if (!pendingRect_.isNull()) {
		u = u.united(pendingRect_);
		pendingRect_ = QRect();
		coalesceTimer_->stop();
	}
	g_seekCensus.requested++;
	update(u);
}

void SeekBar::repaintSoon(const QRect &r)
{
	if (r.isEmpty())
		return;
	if (pendingRect_.isNull()) {
		pendingRect_ = r;
		coalesceTimer_->start(); // restarts at kCoalesceMs
	} else {
		// Already waiting: widen the rect and let the timer that is
		// already running deliver it. Restarting it here would let a
		// change arriving every 33 ms postpone the repaint forever.
		pendingRect_ = pendingRect_.united(r);
		g_seekCensus.coalesced++;
	}
}

void SeekBar::flushPending()
{
	if (pendingRect_.isNull())
		return;
	const QRect r = pendingRect_;
	pendingRect_ = QRect();
	g_seekCensus.requested++;
	update(r);
}

int SeekBar::trackWidth() const
{
	return std::max(1, width() - 4); // 2 px margin each side
}

int64_t SeekBar::tickStepNs() const
{
	if (durationNs_ <= 0)
		return 0;
	// Only round steps an operator counts in: seconds, then the quarter and
	// half minute, then minutes. A "every 3.7 s" scale is worse than none.
	// Zoomed in, the same list gives finer marks by itself — which is the
	// point of zooming: the scale has to get more precise, not just wider.
	static const int64_t kStepsSec[] = {1,   2,   5,   10,   15,   30,  60,
					    120, 300, 600, 900, 1800, 3600, 7200,
					    10800};
	// Two labels 58 px apart still have air between them at this font; any
	// closer and the scale turns into a smear of digits.
	const double kMinPx = 58.0;
	const double w = (double)trackWidth();
	const double visibleNs = (double)durationNs_ * viewSpan_;
	for (int64_t sec : kStepsSec) {
		const int64_t step = sec * 1'000'000'000LL;
		if (w * (double)step / visibleNs >= kMinPx)
			return step;
	}
	// Longer than the coarsest step: keep the coarsest rather than dropping
	// the scale — sparse marks still say which way time runs.
	return kStepsSec[sizeof(kStepsSec) / sizeof(kStepsSec[0]) - 1] *
	       1'000'000'000LL;
}

int SeekBar::graduations() const
{
	const int64_t step = tickStepNs();
	if (step <= 0)
		return 0;
	// Marks at step, 2*step, … up to the end (0 is the left edge and needs
	// no mark of its own).
	return (int)(durationNs_ / step);
}

void SeekBar::setTimeline(int64_t durationNs, const QString &emptyHint)
{
	if (durationNs < 0)
		durationNs = 0;
	if (durationNs == durationNs_ && emptyHint == emptyHint_)
		return; // called thirty times a second; most ticks change nothing
	const bool had = durationNs_ > 0;
	durationNs_ = durationNs;
	emptyHint_ = emptyHint;
	if (had != (durationNs_ > 0)) {
		// A bar that cannot be scrubbed must not offer the hand cursor:
		// the cursor is the first promise the widget makes.
		setCursor(durationNs_ > 0 ? Qt::PointingHandCursor
					  : Qt::ArrowCursor);
		if (dragging_) {
			dragging_ = false;
			emit scrubStateChanged(false);
		}
	}
	repaintSoon(allOfIt());
}

// THE PLAYHEAD. This is the one thing on the bar that has to answer at 30 Hz,
// and it is also the one that used to cost a full-width repaint to move by a
// pixel — because the setter simply called update() and had nothing to compare
// against. Two things fixed that, and both matter:
//
//  - the comparison is made in PIXELS, not in fractions. While recording, the
//    timeline grows every tick, so positionFrac is never twice the same number
//    and a comparison on it would suppress precisely nothing while the bar sat
//    still. What the operator sees is the pixel.
//  - the dirty rect is the band the hairline moved ACROSS, not the widget. At
//    30 Hz that is a handful of pixels, so what Qt re-composes and flushes out
//    of the OBS main window's backing store is a handful of pixels.
void SeekBar::setProgress(double positionFrac, double seekableFrac)
{
	const double p = std::clamp(positionFrac, 0.0, 1.0);
	const double s = std::clamp(seekableFrac, 0.0, 1.0);
	if (dragging_) {
		// The bar is following the hand, not the engine: mouseMoveEvent
		// owns the repaint while a gesture is in progress.
		positionFrac_ = p;
		seekableFrac_ = s;
		return;
	}

	const int oldX = lastDrawnPlayheadX_;
	const bool seekableMoved = std::abs(s - seekableFrac_) > 0.0005;
	positionFrac_ = p;
	seekableFrac_ = s;
	const int newX = xForFraction(p);

	if (newX == oldX && !seekableMoved) {
		g_seekCensus.suppressed++;
		return;
	}
	lastDrawnPlayheadX_ = newX;

	if (seekableMoved) {
		// The dark "you cannot go here" region changed: that is the whole
		// track, and it is not a per-tick event.
		repaintSoon(trackBand());
		return;
	}

	// The hairline is 2 px (3 while dragging) and the fill edge sits under
	// it; 4 px of slack each side covers both plus rounding. Full HEIGHT
	// because the playhead runs down through the ruler as well.
	const int lo = std::min(oldX == INT_MIN ? newX : oldX, newX) - 4;
	const int hi = std::max(oldX == INT_MIN ? newX : oldX, newX) + 4;
	repaintNow(QRect(lo, 0, hi - lo, height()));
}

void SeekBar::setEventMarkers(const std::vector<std::pair<double, double>> &markers)
{
	// Handed over on EVERY tick of a 30 Hz poll. Outside a take the answer is
	// always "the same as last time", which is the state the panel spends most
	// of a session in and the state it was repainting itself to death in.
	if (markers == markers_) {
		g_seekCensus.suppressed++;
		return;
	}
	markers_ = markers;
	// Markers can appear anywhere on the bar, so this one really is the whole
	// widget. Deferred only far enough to join the other changes arriving in
	// the same tick (see kCoalesceMs) — not far enough to be seen.
	repaintSoon(allOfIt());
}

void SeekBar::setOverlayText(const QString &text)
{
	if (overlay_ == text) {
		g_seekCensus.suppressed++;
		return;
	}
	overlay_ = text;
	// Hundredths of a second: this changes on nearly every tick while a clip
	// runs. It is drawn centred ON THE TRACK, so the ruler and its labels —
	// the expensive half of a repaint — are not involved.
	repaintSoon(trackBand());
}

double SeekBar::fracAt(int x) const
{
	const int m = 2;
	double w = std::max(1, width() - 2 * m);
	// Pixels are window coordinates; everything this class hands out is a
	// fraction of the WHOLE timeline, so the window is undone right here and
	// nowhere else.
	const double v = std::clamp((double)(x - m) / w, 0.0, 1.0);
	return std::clamp(fromView(v), 0.0, 1.0);
}

void SeekBar::setZoom(double zoom, double centreFrac)
{
	// 1× is the whole timeline; past 512× a 1080p frame of a three-hour
	// session is still several pixels wide, and there is nothing finer to
	// aim at.
	zoom_ = std::clamp(zoom, 1.0, 512.0);
	viewSpan_ = 1.0 / zoom_;
	// Clamped to the ends: a window hanging off the edge would draw empty
	// space as if the timeline had it.
	viewStart_ = std::clamp(centreFrac - viewSpan_ / 2.0, 0.0,
				1.0 - viewSpan_);
	emit zoomChanged(zoom_);
	// The window changed: every graduation, every label and every marker
	// moves. Whole widget, and immediate — this one is a gesture, and a
	// zoom that lands a tenth of a second late feels broken.
	repaintNow(allOfIt());
}

void SeekBar::ensureVisible(double frac)
{
	if (viewSpan_ >= 1.0)
		return;
	// A tenth of the window of air on the side it left by: scrolling it to
	// the very edge means it leaves again on the next frame.
	const double pad = viewSpan_ * 0.1;
	double start = viewStart_;
	if (frac < viewStart_ + pad)
		start = frac - pad;
	else if (frac > viewStart_ + viewSpan_ - pad)
		start = frac - viewSpan_ + pad;
	start = std::clamp(start, 0.0, 1.0 - viewSpan_);
	if (std::abs(start - viewStart_) < 1e-9)
		return;
	viewStart_ = start;
	// Panning under a moving playhead: the whole widget changes, but this is
	// driven by poll() at 30 Hz, so it is deferred like everything else on
	// that beat. The playhead itself keeps its own immediate path.
	repaintSoon(allOfIt());
}

void SeekBar::wheelEvent(QWheelEvent *e)
{
	if (durationNs_ <= 0) {
		e->ignore();
		return;
	}
	const int steps = e->angleDelta().y();
	if (steps == 0) {
		e->ignore();
		return;
	}
	// Zoom about the POINTER, not the centre: the operator puts the cursor
	// on the moment he cares about and scrolls, and that moment stays under
	// the cursor while everything else spreads out around it.
	const double at = fracAt((int)e->position().x());
	const double factor = steps > 0 ? 1.25 : 1.0 / 1.25;
	const double next = std::clamp(zoom_ * factor, 1.0, 512.0);
	// Keep `at` where it is on screen: the window shrinks around it rather
	// than around its own middle.
	const double vAt = toView(at); // where it sits in the window now
	const double span = 1.0 / next;
	viewStart_ = std::clamp(at - vAt * span, 0.0, 1.0 - span);
	zoom_ = next;
	viewSpan_ = span;
	emit zoomChanged(zoom_);
	repaintNow(allOfIt()); // a gesture: it has to land under the hand
	e->accept();
}

bool SeekBar::event(QEvent *e)
{
	// §6.6 — the badge's own tooltip. setToolTip (Dock.SeekHint) answers
	// for the rest of the bar; a hover here would otherwise say "click or
	// drag to review, wheel to zoom", which is true of the bar and false
	// of the one spot on it that does neither.
	if (e->type() == QEvent::ToolTip) {
		auto *he = static_cast<QHelpEvent *>(e);
		if (zoomHitRect().contains(he->pos())) {
			QToolTip::showText(
				he->globalPos(),
				obs_module_text("Dock.ZoomFit"), this,
				zoomHitRect());
			return true;
		}
	}
	return QWidget::event(e);
}

void SeekBar::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	// The pixel the playhead was last drawn at is the thing setProgress
	// compares against, and it means nothing once the bar is a different
	// width. Left stale, it would suppress the first real move after every
	// resize — a bar that comes back from a dock drag with a frozen playhead.
	lastDrawnPlayheadX_ = INT_MIN;
	pendingRect_ = QRect(); // Qt repaints the whole widget after a resize
	if (coalesceTimer_)
		coalesceTimer_->stop();
}

void SeekBar::paintEvent(QPaintEvent *)
{
	g_seekCensus.served++;
	QPainter p(this);
	// Antialiasing OFF: everything here is axis-aligned, and a 1 px
	// graduation drawn with AA is a 2 px grey smudge — which is how a scale
	// stops being readable at the exact size where it matters.
	p.setRenderHint(QPainter::Antialiasing, false);

	// The whole widget IS the bar (no bead on a rail): it is the control the
	// operator's hand lives on, so it is as wide and as tall as the panel can
	// afford.
	//
	// SLATE BLUE, not green. The green band is the ClipBar directly above,
	// and that one is a STATUS: what is on air, how much of it is left. This
	// one is a CONTROL over the whole recorded timeline. Two stacked bars in
	// the same colour would be two bars the operator has to tell apart by
	// reading, every time, under pressure.
	const int m = 2;             // horizontal margin
	const int h = kSeekTrackH;   // track thickness
	const int y = 1;
	const int rulerY = y + h;    // graduations live under the track
	const int w = width() - 2 * m;
	// Everything below draws in WINDOW coordinates: a fraction of the whole
	// timeline becomes a fraction of what is on screen through toView(). At
	// 1× the two are the same and none of this costs anything.
	const double pos = toView(dragging_ ? dragFrac_ : positionFrac_);

	// --- no timeline: say so, and mean it ---------------------------------
	// This is not a defensive branch, it is a state the operator reaches by
	// opening yesterday's project: the files are on disk but nothing is
	// feeding a live edge, so there is no span to scrub. Drawing the usual
	// empty track there gave him a scrubber that swallowed every click.
	if (durationNs_ <= 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(sc().sink1));
		p.drawRect(QRect(m, y, w, h + kSeekRulerH));
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(sc().border), 1));
		p.drawRect(QRect(m, y, w - 1, h + kSeekRulerH - 1));
		if (!emptyHint_.isEmpty()) {
			QFont f = p.font();
			f.setBold(true);
			p.setFont(f);
			p.setPen(QColor(sc().textMuted));
			p.drawText(QRect(m + 6, y, w - 12, h + kSeekRulerH),
				   Qt::AlignCenter, emptyHint_);
		}
		return;
	}

	// Track (the part of the timeline behind/ahead of the playhead)
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(sc().sink2));
	p.drawRect(QRectF(m, y, w, h));

	// Anything outside the seekable region is darker still: it is not a place
	// the operator can go, and painting it like the rest would say it is.
	if (seekableFrac_ < 1.0) {
		p.setBrush(QColor(sc().panel));
		p.drawRect(QRectF(m + w * seekableFrac_, y,
				  w * (1.0 - seekableFrac_), h));
	}

	// Behind the playhead — steel blue, and it goes down FIRST, under the
	// markers. The events are the content of this bar; the progress is where
	// the operator happens to be in it, and a position that hides the marks
	// it has already passed is a bar that forgets what is on it. The marker
	// fill is semi-transparent, so on the played side both read at once:
	// orange event over blue ground.
	if (pos > 0.0) {
		p.setBrush(QColor(sc().seekBar));
		p.drawRect(QRectF(m, y, w * std::clamp(pos, 0.0, 1.0), h));
	}

	// Event markers — orange, the colour the reference controller gives an event everywhere else
	// (the selected row). Semi-transparent fill + bright edges.
	static const QColor kFill[2] = {
		QColor(0xdb, 0x50, 0x26, 0x80), // orange
		QColor(0xe0, 0x80, 0x20, 0x80), // amber (alternating, so two
						// adjacent events stay legible)
	};
	static const QColor kEdge[2] = {
		QColor(0xff, 0x76, 0x40),
		QColor(0xff, 0xa8, 0x40),
	};
	for (int mi = 0; mi < (int)markers_.size(); mi++) {
		const auto &mk = markers_[mi];
		if (mk.second <= mk.first)
			continue;
		const int ci = mi % 2;
		const double v0 = toView(mk.first);
		const double v1 = toView(mk.second);
		if (v1 < 0.0 || v0 > 1.0)
			continue; // outside the window entirely
		double x0 = m + w * std::clamp(v0, 0.0, 1.0);
		double x1 = m + w * std::clamp(v1, 0.0, 1.0);
		double mw = std::max(x1 - x0, 2.0);

		// Fill
		p.setPen(Qt::NoPen);
		p.setBrush(kFill[ci]);
		p.drawRect(QRectF(x0, y, mw, h));

		// Left edge (bright, 2px)
		p.setPen(QPen(kEdge[ci], 2.0, Qt::SolidLine, Qt::FlatCap));
		p.drawLine(QPointF(x0, y), QPointF(x0, y + h));

		// Right edge (dimmer, 1px, only when wide enough)
		if (x1 - x0 > 4) {
			p.setPen(QPen(kEdge[ci].darker(150), 1.0,
				      Qt::SolidLine, Qt::FlatCap));
			p.drawLine(QPointF(x1, y), QPointF(x1, y + h));
		}
	}
	p.setPen(Qt::NoPen);

	// --- the graduations --------------------------------------------------
	// The scale, on its own strip under the track plus a matching tick INTO
	// the track. This is what tells the operator that the bar spans forty
	// seconds and not two hours — the number in the middle only says where he
	// is, never how far the ends are.
	//
	// Two densities: labelled marks at tickStepNs(), and four unlabelled ones
	// between each pair (the fifths of the step) as long as they are far
	// enough apart to still be separate lines. The minor marks are what makes
	// the strip read as a ruler at a glance, before any digit is read.
	//
	// §6.6: reserved (both here and inside the block below) so a
	// graduation label near the right edge never prints UNDER the zoom
	// factor — the badge itself is drawn last, over anything that still
	// reaches this far.
	const QRect zoomRect = zoomHitRect();
	const int labelRight = zoomRect.isEmpty() ? m + w : zoomRect.left() - 4;
	{
		p.setBrush(Qt::NoBrush);
		// Ruler ground, a shade darker than the track: the strip is part
		// of the control, not the panel behind it.
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(sc().panel));
		p.drawRect(QRect(m, rulerY, w, kSeekRulerH));

		const int64_t step = tickStepNs();
		// Pixels per nanosecond OF WHAT IS ON SCREEN, and where the
		// window starts in time: the marks are absolute timecodes drawn
		// at their own place in the window, so zooming changes the
		// spacing and never the labels.
		const int64_t viewStartNs =
			(int64_t)((double)durationNs_ * viewStart_);
		const int64_t viewEndNs =
			(int64_t)((double)durationNs_ * (viewStart_ + viewSpan_));
		const double pxPerNs =
			(double)w / std::max(1.0, (double)(viewEndNs - viewStartNs));
		const double minorPx = pxPerNs * (double)step / 5.0;
		const bool drawMinor = minorPx >= 7.0;

		QFont lf = p.font();
		if (lf.pointSizeF() > 0)
			lf.setPointSizeF(std::max(6.5, lf.pointSizeF() * 0.78));
		p.setFont(lf);
		const QFontMetrics fm(lf);

		const QColor kMajor(0x8e, 0xa4, 0xbc);
		const QColor kMinor(0x46, 0x54, 0x66);
		// Where the last label ended, so two of them can never overlap
		// even at the clamped edges.
		int lastLabelRight = -10000;

		// Start at the first whole step inside the window, not at zero:
		// zoomed into the fiftieth minute, counting from 0 would walk
		// three thousand invisible marks before drawing one.
		const int64_t first = (viewStartNs / step) * step;
		for (int64_t t = first; t <= viewEndNs; t += step) {
			if (t < 0)
				continue;
			const double x =
				(double)m + pxPerNs * (double)(t - viewStartNs);

			// Minor marks between this graduation and the next.
			if (drawMinor) {
				p.setPen(QPen(kMinor, 1));
				for (int k = 1; k < 5; k++) {
					const double mx =
						x + pxPerNs * (double)step *
							    ((double)k / 5.0);
					if (mx > (double)(m + w))
						break;
					const int mxi = (int)mx;
					p.drawLine(mxi, rulerY + 1, mxi,
						   rulerY + 4);
				}
			}

			const int xi = (int)x;
			// The mark itself: a tick into the bottom of the track so
			// the scale is tied to the thing it measures, and a taller
			// one on the ruler.
			p.setPen(QPen(tint(sc().text, 0x38), 1));
			p.drawLine(xi, y + h - 5, xi, y + h - 1);
			p.setPen(QPen(kMajor, 1));
			p.drawLine(xi, rulerY + 1, xi, rulerY + 6);

			// ...and its label, when there is room for it. The first
			// and last are pulled inside the bar instead of being cut
			// in half by its edge.
			const QString lbl = tickLabel(t);
			const int lw = fm.horizontalAdvance(lbl);
			int lx = xi - lw / 2;
			lx = std::clamp(lx, m + 1, labelRight - lw - 1);
			if (lx > lastLabelRight + 6) {
				p.setPen(kMajor);
				p.drawText(QRect(lx, rulerY + 5, lw, kSeekRulerH - 5),
					   Qt::AlignHCenter | Qt::AlignTop, lbl);
				lastLabelRight = lx + lw;
			}
		}
	}

	// §6.6 — THE ZOOM FACTOR, DRAWN INTO THE RULER instead of a separate
	// key beside the bar (zoomBtn_). Last, so it sits over any graduation
	// tick that still reaches this far right. A background of its own
	// marks it as a control rather than another label — click resets to
	// the whole timeline, right click opens the same spans menu
	// (showZoomMenu) the old key did.
	if (!zoomRect.isEmpty()) {
		const bool zoomedIn = zoom_ > 1.001;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(sc().raise1));
		p.drawRect(zoomRect);
		QFont zf = p.font();
		if (zf.pointSizeF() > 0)
			zf.setPointSizeF(std::max(6.5, zf.pointSizeF() * 0.78));
		zf.setBold(zoomedIn);
		p.setFont(zf);
		p.setPen(QColor(zoomedIn ? sc().text : sc().textMuted));
		const QString zlbl =
			zoomedIn ? QString("%1\xc3\x97").arg(zoom_, 0, 'f',
							      zoom_ < 10 ? 1 : 0)
				 : QStringLiteral("1\xc3\x97");
		p.drawText(zoomRect, Qt::AlignCenter, zlbl);
	}

	// Playhead: a thin bright line, no bead. the reference controller draws a hairline, and a
	// bead on a 24 px band hides the very frame it is pointing at. It runs
	// through the ruler too — a scale is only useful if the position can be
	// read against it.
	const double hx = m + w * pos;
	p.setPen(QPen(tint(sc().text, dragging_ ? 0xff : 0xc0),
		      dragging_ ? 3.0 : 2.0, Qt::SolidLine, Qt::FlatCap));
	p.drawLine(QPointF(hx, y), QPointF(hx, y + h + kSeekRulerH));

	// Where the timeline stands, printed ON the bar (the reference controller). NOT the clip
	// state — that is the green band above; this is position and length of
	// the recorded timeline, which is what a scrubber is about.
	if (!overlay_.isEmpty()) {
		QFont f = p.font();
		f.setPointSizeF(f.pointSizeF() * 1.05);
		f.setBold(true);
		p.setFont(f);
		const QRectF tr(m, y, w, h);
		// The drop shadow is the BAR's own colour, not black: this text is
		// drawn over the played band and the event markers, so what it
		// needs is separation from whatever is behind it — and on a light
		// theme a black shadow under dark text is a dark halo rather than
		// a shadow.
		p.setPen(tint(sc().sink2, 0xd0));
		p.drawText(tr.adjusted(1, 1, 1, 1), Qt::AlignCenter, overlay_);
		p.setPen(QColor(sc().text));
		p.drawText(tr, Qt::AlignCenter, overlay_);
	}
}

int SeekBar::xForFraction(double frac) const
{
	const int m = 2;
	const int w = std::max(1, width() - 2 * m);
	return m + (int)std::lround(w * std::clamp(toView(frac), 0.0, 1.0));
}

void SeekBar::setEventMarkerIds(const std::vector<int> &ids)
{
	// No repaint of its own — ids are not drawn, they are what makes an edge
	// draggable. The comparison is still worth making: this is handed over on
	// every tick, and reassigning a vector thirty times a second is an
	// allocation thirty times a second for a list that changes when the
	// operator marks something.
	if (ids != markerIds_)
		markerIds_ = ids;
}

bool SeekBar::findMarkerEdge(int x, int &marker, bool &inPoint) const
{
	// Six pixels either side. Wider and a click meaning "scrub here" starts
	// grabbing edges the operator was not aiming at; narrower and the edge
	// cannot be hit at all on a bar this short. Zoom is what makes the aim
	// easy, and that is the other half of this feature.
	constexpr int kGrabPx = 6;
	int best = -1;
	int bestDist = kGrabPx + 1;
	bool bestIn = true;
	for (size_t i = 0; i < markers_.size() && i < markerIds_.size(); i++) {
		const int xin = xForFraction(markers_[i].first);
		const int xout = xForFraction(markers_[i].second);
		if (std::abs(x - xin) < bestDist) {
			bestDist = std::abs(x - xin);
			best = (int)i;
			bestIn = true;
		}
		if (std::abs(x - xout) < bestDist) {
			bestDist = std::abs(x - xout);
			best = (int)i;
			bestIn = false;
		}
	}
	if (best < 0)
		return false;
	marker = best;
	inPoint = bestIn;
	return true;
}

void SeekBar::mousePressEvent(QMouseEvent *e)
{
	// §6.6 — THE ZOOM BADGE, checked before anything else claims the
	// click: it sits in the ruler row, which a scrub would otherwise treat
	// as "start dragging from here". Right click opens the same spans
	// menu the old key did (showZoomMenu, via the host); left click resets
	// straight to the whole timeline — the one gesture short enough that
	// it does not need a menu of its own, the same as the old key's
	// primary action.
	if (zoomHitRect().contains(e->pos())) {
		if (e->button() == Qt::RightButton)
			emit zoomMenuRequested();
		else if (e->button() == Qt::LeftButton)
			setZoom(1.0, 0.5);
		return;
	}
	// No timeline, no scrub. The host would refuse the seek anyway
	// (seekToFraction), but silently: the click has to be refused HERE, so
	// the playhead does not jump to where the finger landed on a bar that
	// spans nothing.
	if (e->button() != Qt::LeftButton || durationNs_ <= 0)
		return;

	// An edge under the finger means "move this point", not "go there". A
	// mark taken live is late, and the fastest fix is to take hold of the
	// mark itself: no selection, no keys, no dialog.
	int marker = -1;
	bool inPoint = true;
	if (findMarkerEdge(e->pos().x(), marker, inPoint)) {
		dragMarker_ = marker;
		dragMarkerIn_ = inPoint;
		dragging_ = true;
		dragFrac_ = fracAt(e->pos().x());
		emit scrubStateChanged(true);
		repaintNow(allOfIt());
		return;
	}
	dragMarker_ = -1;
	dragging_ = true;
	// Clamp to seekable region so the user can't drag into unindexed territory.
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	emit scrubStateChanged(true);
	emit scrubMoved(dragFrac_);
	repaintNow(allOfIt());
}

void SeekBar::mouseMoveEvent(QMouseEvent *e)
{
	if (!dragging_) {
		// Hovering an edge says so with the cursor, which is the only
		// hint there is that a marker can be taken hold of at all.
		int m = -1;
		bool in = true;
		setCursor(findMarkerEdge(e->pos().x(), m, in) ? Qt::SizeHorCursor
							      : Qt::ArrowCursor);
		return;
	}
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	if (dragMarker_ >= 0) {
		// Live feedback on the marker itself, so the operator sees the
		// clip's edge follow his hand rather than a number changing
		// after he lets go.
		auto &mk = markers_[(size_t)dragMarker_];
		if (dragMarkerIn_)
			mk.first = std::min(dragFrac_, mk.second);
		else
			mk.second = std::max(dragFrac_, mk.first);
		repaintNow(allOfIt());
		return;
	}
	emit scrubMoved(dragFrac_);
	repaintNow(allOfIt());
}

void SeekBar::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !dragging_)
		return;
	dragging_ = false;
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	if (dragMarker_ >= 0) {
		// The bar knows nothing about events: it reports the gesture and
		// the host decides what it means (and whether the store will
		// even accept it — an IN cannot pass its OUT).
		const int id = (size_t)dragMarker_ < markerIds_.size()
				       ? markerIds_[(size_t)dragMarker_]
				       : 0;
		dragMarker_ = -1;
		emit scrubStateChanged(false);
		if (id > 0)
			emit markerDragged(id, dragMarkerIn_, dragFrac_);
		repaintNow(allOfIt());
		return;
	}
	positionFrac_ = dragFrac_;
	emit seekRequested(dragFrac_);
	emit scrubStateChanged(false);
	repaintNow(allOfIt());
}

// ---------------------------------------------------------------------------
// ClipBar — the green on-air band (see the header for what it is FOR)
// ---------------------------------------------------------------------------

ClipBar::ClipBar(QWidget *parent) : QWidget(parent)
{
	// 24, and the >> key on it is 18: a stylesheet min-height LARGER than the
	// widget's own height makes the style draw a taller frame than the widget
	// owns and the bottom border lands outside it.
	// 28, NOT 24. The skip key sits ON this band and is pinned to four less
	// than it; at 20 px the frame its style draws is 20 as well, so the bottom
	// border landed on the widget edge and any rounding put it past. Same
	// arithmetic as the status line above it, same four pixels of headroom.
	setFixedHeight(28);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	// Deliberately NOT a pointing-hand cursor and deliberately not clickable:
	// the bar directly under it IS clickable, and a bar that looks draggable
	// but is not is worse than one that looks inert.

	coalesceTimer_ = new QTimer(this);
	coalesceTimer_->setSingleShot(true);
	coalesceTimer_->setInterval(kCoalesceMs);
	connect(coalesceTimer_, &QTimer::timeout, this, [this] {
		if (!pending_)
			return;
		pending_ = false;
		g_clipCensus.requested++;
		update();
	});
}

void ClipBar::repaintSoon()
{
	if (pending_) {
		g_clipCensus.coalesced++;
		return;
	}
	pending_ = true;
	coalesceTimer_->start();
}

void ClipBar::setState(double progressFrac, const QString &text, bool onAir,
		       const std::vector<double> &clipJoins)
{
	const double p = std::clamp(progressFrac, 0.0, 1.0);
	if (std::abs(p - progress_) < 0.0005 && text == text_ &&
	    onAir == onAir_ && clipJoins == joins_) {
		g_clipCensus.suppressed++;
		return; // 30 times a second, most ticks change nothing
	}
	progress_ = p;
	text_ = text;
	onAir_ = onAir;
	joins_ = clipJoins;
	// Something really did change: the fill moved or the text did. It is
	// deferred by at most one frame, purely so that a tick which changes both
	// costs one repaint rather than two — the fill an operator watches cross
	// this band must not lose a single step of it.
	repaintSoon();
}

void ClipBar::paintEvent(QPaintEvent *)
{
	g_clipCensus.served++;
	QPainter p(this);
	const int m = 2;
	const int h = height() - 2;
	const int y = 1;
	const int w = width() - 2 * m;

	// Track, then the played part of THIS clip. Bright while it is on air,
	// muted while the bar is only describing what would play: "is something
	// on air" must be answerable without reading a word.
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(sc().onAirDim));
	p.drawRect(QRectF(m, y, w, h));
	if (progress_ > 0.0) {
		p.setBrush(onAir_ ? QColor(sc().onAir)
				  : QColor(sc().onAirDim));
		p.drawRect(QRectF(m, y, w * progress_, h));
	}

	// The joins between the clips of the sequence. White, thin, full height:
	// they are a scale, not another fill, and the operator has to be able to
	// count "three more angles to go" without reading anything.
	if (!joins_.empty()) {
		p.setPen(QPen(tint(sc().text, 0xcc), 1));
		for (double j : joins_) {
			if (j <= 0.0 || j >= 1.0)
				continue;
			const int x = m + (int)(w * j);
			p.drawLine(x, y, x, y + h - 1);
		}
		p.setPen(Qt::NoPen);
	}

	if (text_.isEmpty())
		return;
	QFont f = p.font();
	f.setBold(true);
	p.setFont(f);
	// The right end belongs to the >> key, which is a child of this widget:
	// centring the text across the whole width would run it under the button.
	int rightGap = 12;
	for (const QObject *o : children())
		if (auto *cw = qobject_cast<const QWidget *>(o))
			if (cw->isVisible())
				rightGap = std::max(rightGap, cw->width() + 12);
	const QRectF tr(m + 6, y, w - 6 - rightGap, h);
	// Dark halo first: the text crosses both greens and has to stay legible
	// over either.
	p.setPen(QColor(0x00, 0x20, 0x0c, 0xb0));
	p.drawText(tr.adjusted(1, 1, 1, 1), Qt::AlignCenter, text_);
	p.setPen(onAir_ ? QColor(0xff, 0xff, 0xff) : QColor(0xa8, 0xc8, 0xb0));
	p.drawText(tr, Qt::AlignCenter, text_);
}

// ---------------------------------------------------------------------------
// Preview render callback (runs on the OBS graphics thread)
// ---------------------------------------------------------------------------

void MultiReplayDock::drawChannelA(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiReplayDock *>(data);
	if (!self || cx == 0 || cy == 0)
		return;

	// This runs on the ONE graphics thread that renders every obs_display in
	// OBS, so it does the least work that can possibly show a picture: copy
	// the source poll() already resolved and take a reference to it. That
	// reference is an atomic increment on the source's own control block — no
	// name lookup, no libobs global source mutex, no engine lock. A draw
	// callback that blocks on any of those blocks the whole interface, which
	// is what turned the OBS window black while its taskbar thumbnail (drawn
	// from an already-composed surface) stayed correct. See previewSource_.
	obs_source_t *src = nullptr;
	{
		std::lock_guard<std::mutex> lk(self->previewMutex_);
		if (self->previewSource_)
			src = obs_source_get_ref(self->previewSource_);
	}
	if (!src)
		return;
	renderSourceFitted(src, cx, cy);
	obs_source_release(src);
}

// Channel B's box. Same contract as drawChannelA, and deliberately simpler:
// B has no live-camera mirror. A is where the operator watches the match
// (follow-live puts the selected camera in it); B is a replay bay — what it
// shows is whatever B last played, and black when B has played nothing. Giving
// it a live mirror too would put the same picture in both boxes for most of a
// match, which is two boxes saying one thing.
void MultiReplayDock::drawChannelB(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiReplayDock *>(data);
	if (!self || cx == 0 || cy == 0)
		return;
	obs_source_t *src = nullptr;
	{
		std::lock_guard<std::mutex> lk(self->previewMutex_);
		if (self->previewSourceB_)
			src = obs_source_get_ref(self->previewSourceB_);
	}
	if (!src)
		return;
	renderSourceFitted(src, cx, cy);
	obs_source_release(src);
}

// One multiview tile. Same contract as drawChannelA — and the same reason for
// it: this runs on the ONE graphics thread that renders every obs_display in
// OBS, and with up to nine tiles a single blocking lookup in here would be nine
// chances per frame to freeze the whole interface. All it may touch is the
// pointer poll() already resolved for this slot.
void MultiReplayDock::drawTile(void *data, uint32_t cx, uint32_t cy)
{
	auto *ctx = static_cast<TileCtx *>(data);
	if (!ctx || !ctx->dock || cx == 0 || cy == 0)
		return;
	MultiReplayDock *self = ctx->dock;
	if (ctx->slot < 0 || ctx->slot >= kMaxPreviewTiles)
		return;

	obs_source_t *src = nullptr;
	{
		std::lock_guard<std::mutex> lk(self->tileMutex_);
		if (self->tileSource_[ctx->slot])
			src = obs_source_get_ref(self->tileSource_[ctx->slot]);
	}
	if (!src)
		return;
	renderSourceFitted(src, cx, cy);
	obs_source_release(src);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MultiReplayDock::MultiReplayDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("MultiReplayDock");
	// THE PANEL PAINTS ITS OWN BACKGROUND, and it has to be told to.
	//
	// `#MultiReplayDock { background: @panel@; }` is the first line of the
	// style sheet and until now it did NOTHING. Qt only honours a style
	// sheet background on a widget that carries WA_StyledBackground, and it
	// sets that attribute by itself for a PLAIN QWidget (and for QFrame,
	// QDialog and a short list of others) — not for a subclass, which this
	// is. So the panel's background was never ours: what showed through was
	// the window colour of whatever palette OBS had applied.
	//
	// That is invisible for as long as the panel and OBS are the same kind
	// of dark, which is why it survived every screenshot until the day the
	// panel went light: the control strip sat on a dark band, all four
	// margins were a dark frame, and the pictures and the event list were
	// correct — because those two live inside a QSplitter, and a QSplitter
	// is a QFrame, which Qt does style.
	setAttribute(Qt::WA_StyledBackground, true);
	// WIDTH is the only hard floor. A hard minimum HEIGHT is what let the
	// panel be dragged shorter than its own controls need: an explicit
	// minimumSize wins over the layout's computed one, so Qt happily handed
	// out 340 px and the QVBoxLayout paid for it by squeezing every child
	// towards a minimum of nothing — buttons a few pixels tall that cannot be
	// hit. With the height left to the layout (and the control bands pinned at
	// Minimum, see buildBottomBar) the panel simply refuses to go shorter than
	// the keys, and the picture above is what gives way instead.
	// THE FLOOR OF A VERTICAL DOCK. It was 560, and an OBS dock down one side
	// is 300-400 px wide: the panel simply could not be put there. The number
	// was never a design decision either — it was the width of the WIDE
	// arrangement of the widest section, demanded at every size, including the
	// sizes at which the strip would have folded instead (see
	// ControlStrip::minimumSizeHint).
	setMinimumWidth(300);
	// THE SHEET IS BUILT, NOT A CONSTANT. It carries the operator's theme
	// choice and the colours OBS is currently themed with; applyTheme() is
	// called again whenever either changes. A first pass here, before any
	// child exists, so every widget is polished into the right colours as it
	// is created.
	//
	// AND IT IS THE SAME CALL, not a second copy of it. It used to be three
	// lines that built the sheet WITHOUT the marks a sub-control can only be
	// handed as a file (dock-assets.hpp) — so at start-up the panel had no
	// drop-down arrows, no spin arrows and no tick anywhere, and they only
	// appeared the first time the operator changed theme and the real
	// applyTheme() ran. Everything applyTheme does is safe here: it null-checks
	// every widget it touches and there are none yet.
	applyTheme();

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(3);

	// Helper: 1px separator line (each call returns a fresh widget).
	auto mkSep = [this]() -> QWidget * {
		auto *s = new QWidget(this);
		s->setObjectName("mrSepLine");
		s->setFixedHeight(1);
		return s;
	};

	// ── The broadcast replay controller's zoning, top to bottom ────────────
	// 1. the pictures: channel A big, the multiview beside it, green strip
	// 2. search · Live
	// 3. the list tabs
	// 4. the event list, one column per camera
	// 5. mark keys · angle row · export
	// 6. record + transport + slow-motion speed
	// 7. the green on-air band
	// 8. the full-width position bar
	//
	// An operator who has used the reference controller finds every control where his hand
	// already goes, which is the entire point of this layout.
	//
	// THE PICTURES COME FIRST. They did not, for a while: the tabs, the
	// search box and Live sat above them, which put a row of small text
	// where the operator's eye goes for the picture and pushed the picture
	// down. In the controller this panel is modelled on, everything that
	// SELECTS an event (which list, which words, live or parked) belongs to
	// the list — so it lives with the list, directly under the pictures and
	// directly above the table it filters.
	{
		// Preview above, list below: the reference controller stacks them, and the previous
		// side-by-side split had no equivalent there. Draggable, because an
		// OBS dock can be a wide strip or a tall column.
		splitter_ = new QSplitter(Qt::Vertical, this);
		splitter_->setChildrenCollapsible(false);
		splitter_->setHandleWidth(5);
		// THE LEFT COLUMN. In the wide and column arrangements it holds
		// only the pictures and the splitter divides height. In the SHORT
		// one — docked under the OBS preview, wide and shallow — the
		// splitter divides WIDTH instead, the list takes the right-hand
		// half, and the keys, the on-air band and the position bar come
		// down here to stack under the pictures. It is the only shape in
		// which a 340 px tall dock still shows a usable table.
		leftCol_ = new QWidget(this);
		leftCol_->setObjectName(QStringLiteral("mrLeftCol"));
		leftColLayout_ = new QVBoxLayout(leftCol_);
		leftColLayout_->setContentsMargins(0, 0, 0, 0);
		leftColLayout_->setSpacing(3);
		leftColLayout_->addWidget(buildPreview(), 1);
		splitter_->addWidget(leftCol_);

		// The list pane: what picks the events, then the events. One
		// widget so the splitter treats them as the single zone they are
		// — dragging the handle must not be able to leave the search row
		// stranded away from its table.
		auto *listPane = new QWidget(this);
		listPane->setObjectName(QStringLiteral("mrListPane"));
		auto *lv = new QVBoxLayout(listPane);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(2);
		lv->addWidget(buildToolbar());
		lv->addWidget(buildEvents(), 1);
		splitter_->addWidget(listPane);
		listPane_ = listPane;

		connect(splitter_, &QSplitter::splitterMoved, this,
			[this](int, int) {
				userSplit_[(int)panelMode_] = true;
				savedSplit_[(int)panelMode_] =
					splitter_->saveState();
			});
		splitter_->setStretchFactor(0, 3);
		splitter_->setStretchFactor(1, 2);
		root->addWidget(splitter_, 1);
	}

	rootLayout_ = root;
	bottomSep_ = mkSep();
	root->addWidget(bottomSep_);
	bottomBar_ = buildBottomBar();
	bottomBar_->setObjectName(QStringLiteral("mrBottomBar"));
	root->addWidget(bottomBar_);

	// THE KEYS HAVE TO WORK WHEREVER THE FOCUS IS, and in this panel the focus
	// is nearly always on a button the operator has just pressed. A QPushButton
	// that belongs to a QButtonGroup — every angle key, every speed chip — takes
	// the arrows for focus navigation instead of ignoring them, so ←/→ would walk
	// the highlight along the camera row rather than step a frame. Filtering the
	// buttons keeps focus (and its ring) working while the keys stay uniform.
	//
	// The panel itself takes focus too, so clicking an empty patch of it does not
	// leave the keyboard pointing at whatever was focused before.
	setFocusPolicy(Qt::StrongFocus);
	for (QAbstractButton *b : findChildren<QAbstractButton *>())
		b->installEventFilter(this);

	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(33); // ~30 fps — smooth seekbar + responsive transport
	connect(pollTimer_, &QTimer::timeout, this, &MultiReplayDock::poll);
	pollTimer_->start();

	// After the widgets exist: every one of these acts on one of them.
	registerDockHotkeys();
	// Reachable from the module's frontend handler (see releasePreviewRefs).
	g_dock = this;

	// HOW MANY BAYS, and therefore what the panel even shows. One call sets the
	// flag, the badge, the two letters, the checked key and the visibility of
	// the whole B half together, so they cannot disagree: with B off everything
	// means A, with B on the resting state is A|B (the operator who asked for a
	// second bay asked for it to be used).
	applyChannelBVisibility();

	refreshAngles();
	refreshEvents();

	// THE ARRANGEMENT, once, now that every widget it places exists. Forced,
	// because panelMode_ already says Wide and the point of this call is to do
	// the celling rather than to agree with itself. resizeEvent takes over
	// from here — including the first real resize OBS gives the dock, which is
	// usually not Wide.
	applyTableDensity(ReplayCore::instance().getConfig().tableDensity);
	applyPanelMode(panelModeFor(size(), panelMode_, wideFloorH_),
		       /*force*/ true);

	// ...AND THE COLOURS AGAIN, NOW THAT THE TABLE EXISTS. The first pass ran
	// from the top of this constructor, before any child, so the size it gave
	// the two cells that are widgets was the APPLICATION font — the table it
	// needed to measure had not been built. Once it has, and once OBS's own
	// `QWidget { font-size }` has been resolved onto it, the number can be
	// asked of the thing that will actually draw the row. applyTheme is
	// idempotent and notices when nothing moved.
	applyTheme();

	poll();
}

MultiReplayDock::~MultiReplayDock()
{
	// FIRST of all: nothing may reach a dock that is being taken apart.
	if (g_dock == this)
		g_dock = nullptr;
	// FIRST, before anything else is torn down: a hotkey firing from the
	// hotkey thread must not find a half-destroyed dock. obs_hotkey_unregister
	// takes libobs' hotkey mutex, so it returns only once no callback is in
	// flight; the contexts it was pointing at die with the vector below.
	for (obs_hotkey_id id : hotkeys_)
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);
	hotkeys_.clear();
	hotkeyCtx_.clear();

	// Order matters here. The display is a CHILD widget, so ~QWidget takes it
	// down only after this body has returned — leaving a window in which the
	// graphics thread can still call drawChannelA() on a half-destroyed dock.
	// Detaching the callback first closes it: obs_display_remove_draw_callback
	// takes the display's callback mutex, so it returns only once the draw in
	// flight has finished.
	// Every one of them — the two bays and every multiview tile: each has a
	// draw callback pointing at this object, and B's used to be left attached
	// (see allDisplays).
	for (OBSQTDisplay *d : allDisplays())
		d->setRenderCallback(nullptr, nullptr);
	if (pollTimer_)
		pollTimer_->stop();
	// BOTH preview refs. B's was missed, and it was the only source OBS ever
	// complained about: "Not all sources were cleared when clearing scene data:
	// MultiReplay - Replay B", which OBS shows the operator as a plugin that
	// failed to release its resources — a dialog on the way out. A held ref is
	// invisible until something actually uses that bay, which is why it lasted
	// as long as B went unexercised.
	// Every owned reference the dock holds — the two previews and the multiview
	// tiles — in one place, so the destructor and the "OBS is about to clear
	// scene data" path cannot drift apart. They did: the destructor released the
	// tiles and A's preview, the cleanup path released neither, and OBS named
	// what was left in a dialog on the way out.
	dropPreviewRefs();
}

// The Monitors key, applied. Hiding the two rows inside the pane was not
// enough: the pane is a QSplitter child, the splitter had already given it a
// share of the height, and an empty child keeps that share — so the table below
// stayed exactly as short as it was with a band of nothing above it, which is
// the opposite of what the key is for. A hidden splitter child contributes
// nothing and its handle goes with it, so the list takes the whole panel.
//
// The rows are hidden too, and deliberately: a hidden OBSQTDisplay does not
// create its display at all (see qt-display.hpp), which is the GPU cost this
// key exists to put down.
void MultiReplayDock::applyMonitorsVisible(bool on)
{
	monitorsOn_ = on;
	if (monitorsRow_)
		monitorsRow_->setVisible(on);
	if (monitorsStrip_)
		monitorsStrip_->setVisible(on);
	if (previewPane_)
		previewPane_->setVisible(on);
	applyMonitorsRoom();
	if (on) {
		// The divider he chose for this arrangement, put back. While the
		// pictures were down it described nothing, and in Short it was
		// deliberately overridden to give the list the whole width.
		if (splitter_ && splitChosen() &&
		    !savedSplit_[(int)panelMode_].isEmpty())
			splitter_->restoreState(savedSplit_[(int)panelMode_]);
		applyPreviewAspect();
	}
	obs_log(LOG_INFO, "[dock] monitors %s", on ? "shown" : "hidden");
}

// ── AND THE SPLITTER HAS TO BE TOLD, BECAUSE ITS CHILD IS NOT THE PANE ──────
//
// The rule above — hide the splitter's child, not the rows inside it — was
// right and stopped being true underneath itself. previewPane_ WAS that child;
// then the Short arrangement wrapped it in leftCol_, which in that shape also
// carries the key strip. From that commit on, putting the monitors down hid the
// pictures and left leftCol_ visible and EMPTY, still holding the height the
// splitter had given it — so the table did not grow by a pixel and a maximised
// panel showed a band of nothing where the pictures had been. That is the
// report, and it is the same defect the comment above describes, one level up.
//
// Two cases, because leftCol_ holds two different things:
//
//   - WIDE and TALL: the pictures are all it has, so it goes with them. A
//     hidden splitter child contributes nothing and its handle goes too, which
//     is exactly the height the list wanted.
//   - SHORT: the strip lives in that column, so the column must stay. There the
//     splitter divides WIDTH, and asking for zero is how a QSplitter is told
//     "as little as this child will accept" — it clamps to the child's own
//     minimum, so the keys keep their width and the list takes the rest.
//
// No minimumSizeHint() is asked for anywhere in here, deliberately: this runs
// from applyPanelMode, which runs from resizeEvent, and asking a widget for
// that inside the resize cascade does not read a number — it forces a pass, and
// the pass it forces is the one that stays on screen.
void MultiReplayDock::applyMonitorsRoom()
{
	if (!splitter_ || !leftCol_)
		return;
	// In Short the key strip has been moved into this column, so it is never
	// empty and must never be hidden.
	const bool colHasKeys = panelMode_ == PanelMode::Short;

	if (monitorsOn_) {
		if (!leftCol_->isVisible())
			leftCol_->setVisible(true);
		return;
	}

	// A cap written for the pictures outlives them, and on the way back in it
	// would hold the pane at whatever the last visible pass computed.
	if (previewPane_)
		previewPane_->setMaximumHeight(QWIDGETSIZE_MAX);

	if (!colHasKeys) {
		leftCol_->setVisible(false);
		return;
	}
	leftCol_->setVisible(true);
	const int total = splitter_->width();
	if (total > 0)
		splitter_->setSizes({0, total});
}

// ---------------------------------------------------------------------------
// THE THREE ARRANGEMENTS OF THE PANEL
//
// See PanelMode in dock-layout.hpp for the argument. In short: a replay panel
// is used from memory rather than read, so it gets a small number of designed
// arrangements instead of one that reflows into a different panel at every
// width. Nothing is dropped in any of them; what changes is rank.
//
// EVERY LINE HERE IS A RE-CELL OR A PROPERTY. Nothing is created, destroyed or
// re-parented, because under these boxes are OBSQTDisplay widgets whose native
// window a re-parent would destroy — see the note in buildPreview().
// ---------------------------------------------------------------------------

// HOW TALL THE MONITORING BLOCK MAY BE, and it must not be read off the block
// itself: that height is DERIVED from the arrangement this number chooses, so
// feeding it back in makes a pass decide from whatever the widget happened to be
// mid-settle - measured in the mockup, 100 px, which picked an arrangement of
// 78 px stamps. What the splitter is willing to give depends only on the panel
// and a constant, so it is the same on every pass.
//
// Once the OPERATOR has moved the divider it is his answer, and that is stable
// for the same reason: it stops being derived from anything.
int MultiReplayDock::monitorRoomH() const
{
	// ONE COPY OF THIS ARITHMETIC, in dock-layout, shared with the mockup.
	// There were two, and they disagreed about the one thing that mattered:
	// the mockup honoured the divider the operator had dragged between the
	// pictures and the list, and this did not. At 1090x811 on a two-camera
	// rig the mockup drew 357 px cameras filling the row and the panel drew
	// 237 px ones with a 240 px band beside them — and the mockup is what
	// every layout decision here is judged on.
	return monitorRoomFor({height(),
			       splitter_ ? splitter_->height() : height(),
			       previewPane_ ? previewPane_->height() : 0,
			       0,
			       kListPaneFloor,
			       splitChosen(),
			       panelMode_ == PanelMode::Wide});
}

int MultiReplayDock::tileColumns(int tileCount) const
{
	const int n = std::max(1, tileCount);
	// THE SAME COLUMN RULE IN EVERY ARRANGEMENT — one row up to three
	// cameras, ceil(n/2) columns beyond — so the grid the operator learned in
	// the Wide layout is the same grid down a side (Tall) or under the OBS
	// preview (Short). It used to be a one-row filmstrip in Tall on the
	// argument that a narrow column cannot carry a 4-wide grid at a size
	// worth looking at; the operator asked for the consistent shape instead,
	// small tiles and all.
	//
	// FROM THE SAME ARITHMETIC THE SIZES COME FROM (tileBlockFor): a column
	// count that disagrees with the measured tile size is a block with a hole
	// in it. tileBlockFor's cols does not depend on the width or the room, so
	// passing rough values here is fine.
	const int paneW =
		std::max(80, monitorSplit_ ? monitorSplit_->width() : width());
	const Config cfg = ReplayCore::instance().getConfig();
	const int bays = cfg.enableChannelB ? 2 : 1;
	return std::max(1, tileBlockFor(paneW, bays, n, 5, monitorRoomH()).cols);
}

void MultiReplayDock::applyPanelMode(PanelMode m, bool force)
{
	if (!monitorSplit_ || !splitter_)
		return;
	if (!force && m == panelMode_)
		return;
	const PanelMode was = panelMode_;
	panelMode_ = m;

	// THE MONITORING BLOCK IS ONE SPLITTER and every arrangement turns it
	// rather than rebuilding it: side by side the bays and the cameras divide
	// a WIDTH, in a column they divide a HEIGHT. setOrientation does not touch
	// the children, which is the whole reason this is a splitter — moving an
	// OBSQTDisplay between two layouts destroys its native window and strands
	// the obs_display presenting into it.
	//
	// A AND B STAY SIDE BY SIDE IN ALL THREE. They are two bays of one deck;
	// stacking them would make the pair read as a hierarchy, and it is the one
	// relationship on this panel that is exactly equal.
	monitorSplit_->setOrientation(m == PanelMode::Tall ? Qt::Vertical
							   : Qt::Horizontal);
	splitter_->setOrientation(m == PanelMode::Short ? Qt::Horizontal
						       : Qt::Vertical);

	// SHORT PUTS THE CONTROLS IN THE LEFT COLUMN. The panel is wide and
	// shallow, so the list goes down the right-hand half and everything else —
	// pictures, keys, the on-air band and the position bar — stacks on the
	// left. The strip carries no picture, so moving it between the two layouts
	// costs a relayout and nothing else.
	applyControlsColumn(m == PanelMode::Short);

	// THE DIVIDERS THE OPERATOR CHOSE FOR *THIS* ARRANGEMENT, put back. After
	// the orientations, never before: a saved state applied to a splitter that
	// is still dividing the other way is a split nobody asked for.
	if (was != m) {
		if (splitChosen())
			splitter_->restoreState(savedSplit_[(int)m]);
		if (monitorSplitChosen())
			monitorSplit_->restoreState(savedMonitorSplit_[(int)m]);
	}

	// THE STRIP STACKS WHENEVER IT IS IN A COLUMN, which is Tall AND Short:
	// in Short the keys move into the left-hand column beside the list, so
	// they have about half the panel's width. Left on the wide two-macro-row
	// shape there, the sections did not fit across that half and the strip
	// folded anyway - into something so deep that the panel's own floor rose
	// above the height at which Short is chosen, and the arrangement was
	// undone by the resize it had just caused. From the operator's chair
	// Short flashed on and vanished, at every width.
	//
	// Width alone cannot tell these apart — see setStacked.
	if (strip_)
		strip_->setStacked(m == PanelMode::Wide ? 0 : 1);
	// §6.3: Tall collapses bay/clips/speed behind "more" — see the note on
	// applyTallCollapse itself for why hiding them wholesale, rather than
	// stacking all six, is what actually buys the height back.
	applyTallCollapse(m == PanelMode::Tall);

	// The search box is the one control that can be asked to give width back:
	// in a column, its normal minimum is width the Live and Monitors keys do
	// not have. In em, like the box's own construction, so a larger OBS font
	// does not clip it back down to a fixed pixel count.
	if (search_) {
		const int em =
			search_->fontMetrics().horizontalAdvance(QLatin1Char('M'));
		search_->setMinimumWidth(m == PanelMode::Tall ? qMax(40, 4 * em)
							       : qMax(80, 7 * em));
	}

	// OUT COLUMN: the one column of the table that is inferable. IN and
	// DURATA together say where the clip is and how long it runs, so OUT is
	// arithmetic — and it is the column worth spending on a camera instead
	// when there are 330 px to divide.
	if (events_)
		events_->setColumnHidden(kColOut, m == PanelMode::Tall);

	rebuildMultiview();

	// A NOTE ON SETTLING, because it looks like a bug and is not one.
	// A mode change rewrites how short the panel may be, so a panel asked for
	// a size that only the NEW arrangement can hold takes two resize events to
	// get there: the first arrives while the old floor is still in force. The
	// mockup's --check measures it (a top-level asked for 1400x340 lands on
	// 404, then 340).
	//
	// `layout()->invalidate()` here was the obvious fix and was MEASURED TO DO
	// NOTHING — the same two events either way — so it is not in this file. In
	// OBS the panel is a child of a QDockWidget rather than a top-level, so
	// there is no window manager clamping the first size at all.

	// AFTER applyControlsColumn, never before: in Short that call moves the key
	// strip INTO leftCol_, and this decides whether leftCol_ may be hidden. Run
	// the other way round on a mode change with the monitors down, the strip
	// would be added to a column that is still hidden.
	applyMonitorsRoom();

	applyPreviewAspect();

	// THE FLOOR IS IN THE LOG, because a mode that cannot be reached looks
	// exactly like a mode that was not chosen. Short is picked below a height
	// the panel must also be ABLE to be, and the arrangement it switches to
	// rewrites that floor - so when the two disagree the panel flicks into
	// Short and is pushed straight back out by its own minimum. The numbers
	// that settle it are this line's.
	if (was != m) {
		obs_log(LOG_INFO,
			"[dock] layout %s -> %s (%dx%d, floor %dx%d)",
			panelModeName(was), panelModeName(m), width(), height(),
			minimumSizeHint().width(), minimumSizeHint().height());
		// ...AND WHO IS ASKING FOR IT. A floor is a sum, and a sum is
		// exactly the thing you cannot argue with until it is broken into
		// the parts that made it. Every zone that can put a number in.
		const auto floorOf = [](QWidget *w) {
			return w ? w->minimumSizeHint().height() : 0;
		};
		obs_log(LOG_INFO,
			"[dock] floor parts: pictures %d, list %d, keys %d "
			"(strip %d, status %d, band %d, bar %d)",
			floorOf(previewPane_), floorOf(events_),
			floorOf(bottomBar_), floorOf(strip_),
			floorOf(statusBar_), floorOf(clipBar_), floorOf(seek_));
	}
}

int MultiReplayDock::aspectHeight(int width)
{
	obs_video_info ovi{};
	// A canvas we cannot read is 16:9, because that is what a replay rig is
	// nine times in ten — and because a wrong ratio here costs a few pixels of
	// bar, not a picture.
	double ratio = 9.0 / 16.0;
	if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0)
		ratio = (double)ovi.base_height / (double)ovi.base_width;
	return std::max(1, (int)std::lround(width * ratio));
}

// ── TEMP DIAGNOSTIC ─────────────────────────────────────────────────────────
//
// Everything a deterministic read of the "resizing sizes the C1..C8 tiles wrong
// and leaves unused space" report needs: the panel and monitoring-pane sizes,
// the room the row may have (and what the last real pass computed it from), how
// many tiles are VISIBLE against how many cameras are CONFIGURED, the column and
// row split chosen, the TileBlock arithmetic re-run against the current
// geometry, the resulting bay/tile widths, the SLACK left on each axis, the live
// splitter sizes, and every tile box's on-screen geometry plus the maximum that
// was written onto it. Deduplicated on its payload, so a drag emits one line per
// distinct settled state instead of thirty a second.
void MultiReplayDock::queueMonitorDump(const char *why)
{
	// Verbose diagnostic only — Settings ▸ Advanced ▸ Verbose log, or the
	// OBS_MULTIREPLAY_DEBUG env var. Off, this costs nothing: no timer, no dump.
	if (!debugLoggingEnabled())
		return;
	// `why` is a string literal at every call site, so holding the pointer in
	// the closure is safe. Coalesced: many calls in one event-loop turn (a
	// resize fires applyPanelMode + applyPreviewAspect + the poll re-settle)
	// produce ONE dump, read after Qt has re-laid the children.
	if (monitorDiagQueued_)
		return;
	monitorDiagQueued_ = true;
	QTimer::singleShot(0, this, [this, why]() {
		monitorDiagQueued_ = false;
		dumpMonitorLayout(why);
	});
}

void MultiReplayDock::dumpMonitorLayout(const char *why)
{
	if (!debugLoggingEnabled())
		return;
	if (!monitorSplit_ || !multiviewBox_ || !previewPane_)
		return;

	const Config cfg = ReplayCore::instance().getConfig();
	const int bays = cfg.enableChannelB ? 2 : 1;
	int cfgCams = 0;
	for (int i = 0; i < kMaxCameras; i++)
		if (!cfg.cameras[i].sourceName.empty())
			cfgCams++;

	int visTiles = 0;
	QString visList;
	for (int i = 0; i < kMaxPreviewTiles; i++)
		if (tiles_[i].box && tiles_[i].box->isVisible()) {
			visTiles++;
			if (!visList.isEmpty())
				visList += ',';
			visList += QString::number(i);
		}

	const int paneW = std::max(80, monitorSplit_->width());
	const int paneH = monitorSplit_->height();
	const int roomH = monitorRoomH();
	const int gap = monitorSplit_->handleWidth();
	const int cols = visTiles > 0 ? std::max(1, tileColumns(visTiles)) : 0;
	const int rows = cols > 0 ? (visTiles + cols - 1) / cols : 0;
	const TileBlock tb =
		visTiles > 0
			? tileBlockFor(paneW, bays, visTiles, gap, roomH)
			: TileBlock{};

	QString split;
	for (int v : monitorSplit_->sizes()) {
		if (!split.isEmpty())
			split += ',';
		split += QString::number(v);
	}
	QString outerSplit;
	if (splitter_)
		for (int v : splitter_->sizes()) {
			if (!outerSplit.isEmpty())
				outerSplit += ',';
			outerSplit += QString::number(v);
		}

	const auto geo = [](QWidget *w) -> QString {
		if (!w)
			return QStringLiteral("null");
		const QRect r = w->geometry();
		return QString("%1,%2,%3x%4%5")
			.arg(r.x())
			.arg(r.y())
			.arg(r.width())
			.arg(r.height())
			.arg(w->isVisible() ? "" : "/HID");
	};
	const auto cap = [](int v) {
		return v >= QWIDGETSIZE_MAX ? QStringLiteral("-")
					   : QString::number(v);
	};

	QString tileGeo;
	for (int i = 0; i < kMaxPreviewTiles; i++) {
		const PreviewTile &t = tiles_[i];
		if (!t.box)
			continue;
		const QRect r = t.box->geometry();
		tileGeo += QString(" [%1 cam%2 %3 %4,%5,%6x%7 max=%8x%9 disp=%10x%11]")
				   .arg(i)
				   .arg(t.cam0)
				   .arg(t.box->isVisible() ? "V" : "h")
				   .arg(r.x())
				   .arg(r.y())
				   .arg(r.width())
				   .arg(r.height())
				   .arg(cap(t.box->maximumWidth()))
				   .arg(cap(t.box->maximumHeight()))
				   .arg(t.display ? t.display->width() : -1)
				   .arg(t.display ? t.display->height() : -1);
	}

	// What the wide-arrangement arithmetic in applyPreviewAspect would use.
	const int bayWfromTb = (visTiles > 0 && tb.bayW > 0)
				       ? tb.bayW
				       : aspectHeight(paneW / bays) + AspectBox::kTagH;
	const int usedW = bays * bayWfromTb + tb.blockW + gap * bays;
	const int slackW = paneW - usedW;
	const int bayH = aspectHeight((bayWfromTb - 3 * (bays - 1)) / bays) +
			 AspectBox::kTagH;
	const int wantH = std::min(std::max(bayH, tb.blockH), roomH);
	const int slackV = paneH - wantH;

	const QString payload =
		QString("mode=%1 panel=%2x%3 pane=%4x%5 room=%6 (lastPass paneW=%7 "
			"roomH=%8) bays=%9 gap=%10 | cfgCams=%11 visTiles=%12[%13] "
			"cols=%14 rows=%15 splitChosen=%16 | tb: tw=%17 th=%18 "
			"blockW=%19 blockH=%20 bayW=%21 rowH=%22 | bayH=%23 "
			"wantH=%24 usedW=%25 SLACK_W=%26 SLACK_V=%27 | mSplit=[%28] "
			"oSplit=[%29] mvBox=%30 bays=%31 aBox=%32 bBox=%33 "
			"prevPane=%34 maxH=%35 |%36")
			.arg(QString::fromUtf8(panelModeName(panelMode_)))
			.arg(width())
			.arg(height())
			.arg(paneW)
			.arg(paneH)
			.arg(roomH)
			.arg(aspectPaneW_)
			.arg(aspectRoomH_)
			.arg(bays)
			.arg(gap)
			.arg(cfgCams)
			.arg(visTiles)
			.arg(visList)
			.arg(cols)
			.arg(rows)
			.arg(monitorSplitChosen() ? 1 : 0)
			.arg(tb.tileW)
			.arg(tb.tileH)
			.arg(tb.blockW)
			.arg(tb.blockH)
			.arg(tb.bayW)
			.arg(tb.rowH)
			.arg(bayH)
			.arg(wantH)
			.arg(usedW)
			.arg(slackW)
			.arg(slackV)
			.arg(split)
			.arg(outerSplit)
			.arg(geo(multiviewBox_))
			.arg(geo(bays_))
			.arg(geo(aBox_))
			.arg(geo(bBox_))
			.arg(geo(previewPane_))
			.arg(cap(previewPane_->maximumHeight()))
			.arg(tileGeo);

	if (payload == lastMonitorDiag_)
		return;
	lastMonitorDiag_ = payload;
	obs_log(LOG_INFO, "[mondiag] (%s) %s", why,
		payload.toUtf8().constData());
}

// ── THE MONITORING BLOCK, in three steps ─────────────────────────────────
//
//  1. pick the camera block's shape for the room there is;
//  2. unless the OPERATOR has moved a divider, put the dividers where the
//     pictures want them;
//  3. nothing. Every box keeps the canvas's ratio by itself (AspectBox), so
//     there is no third step any more — which is most of what this function
//     used to be, and all of what it used to get wrong.
void MultiReplayDock::applyPreviewAspect()
{
	if (!previewPane_ || !monitorSplit_)
		return;
	// NOTHING IS SIZED WHILE THE MONITORS ARE DOWN. Every number below is a
	// maximum written onto a box, and a maximum written for a block nobody can
	// see is a maximum that is still there when it comes back — and, through
	// applyPreviewSplit, a share of the splitter handed to a pane the operator
	// has just switched off.
	if (!monitorsOn_)
		return;

	// WHAT THIS PASS WAS COMPUTED FROM, remembered so that poll() can notice
	// it has gone stale.
	//
	// The row's arithmetic reads two geometries — how wide the pane is and
	// how tall the row may be — and both of them are settling while a resize
	// is in flight. Read a beat early they are smaller than they will be, and
	// the answer STICKS: the tile ceiling is a MAXIMUM written onto the
	// boxes, so a pass that ran against a half-settled panel leaves the
	// cameras small and the room they should have had shows as empty panel.
	// That is "resizing generates unused space", and it is not any one
	// resize path — it is every path that ends without one more pass.
	//
	// So the inputs are recorded here and poll() re-runs this when they no
	// longer match. It converges by construction: a pass that agrees with the
	// geometry changes nothing, so there is nothing to re-trigger it.
	const int paneW = std::max(80, monitorSplit_->width());
	aspectPaneW_ = paneW;
	aspectRoomH_ = monitorRoomH();
	const Config cfg = ReplayCore::instance().getConfig();
	const int bays = cfg.enableChannelB ? 2 : 1;

	// B'S SHARE OF THE ROW GOES AWAY WITH B, and hiding the box is not what
	// does it. A grid column keeps the stretch it was given whether or not
	// anything visible is in it, so with one bay A was laid out in the LEFT
	// HALF of the picture row and centred there - which reads exactly like a
	// panel holding a space open for something that is switched off. Across
	// the bottom of a wide panel it also opened a gap between A and the first
	// camera that no drag could close.
	if (auto *bg = qobject_cast<QGridLayout *>(bays_->layout()))
		bg->setColumnStretch(1, cfg.enableChannelB ? 1 : 0);
	const int gap = monitorSplit_->handleWidth();
	int visibleTiles = 0;
	for (const PreviewTile &t : tiles_)
		if (t.box && t.box->isVisible())
			visibleTiles++;
	const bool haveTiles = visibleTiles > 0 && multiviewBox_ &&
			       multiviewBox_->isVisible();

	// THE CAMERA BLOCK IS SIZED FIRST and the bays take what is left, which is
	// the opposite of what a stretch factor does — and the reason a stretch
	// factor starved them: a stretch shares only what is left AFTER every
	// column has its minimum, and a tile's minimum is nothing. Eight cameras
	// came out as eight vertical slivers.
	const int cols = haveTiles ? std::max(1, tileColumns(visibleTiles)) : 1;
	tileCap_ = kTileMinWidth;
	const int rows = haveTiles ? (visibleTiles + cols - 1) / cols : 0;
	const int tagH = AspectBox::kTagH;

	int want = 0;
	if (panelMode_ == PanelMode::Tall) {
		// A COLUMN: the bays across the top (A full width, or A|B side by
		// side), then the SAME grid of cameras as the Wide layout under
		// them — ceil(n/2) columns, up to two rows.
		const int bayH =
			aspectHeight((paneW - 3 * (bays - 1)) / bays) + tagH;
		int stripH = 0;
		if (haveTiles) {
			// THE TILES FILL THE ROW. The kTileMaxShare ceiling is
			// there to stop ONE camera drawing itself as big as the
			// picture being watched; from two upwards the row is
			// already divided between the columns, so the ceiling is
			// not applied and the grid spans the pane.
			const int share = (paneW - 2 * (cols - 1)) / cols;
			const int tileW =
				cols >= 2 ? share
					  : std::min((int)(paneW * kTileMaxShare),
						     share);
			tileCap_ = tileW;
			stripH = rows * (aspectHeight(tileW) + tagH) +
				 (rows - 1) * 2;
		}
		want = bayH + (haveTiles ? gap + stripH : 0);
		if (haveTiles)
			monitorSplit_->setSizes({bayH, stripH});
	} else {
		// ONE PIECE OF ARITHMETIC, SHARED WITH THE MOCKUP (tileBlockFor,
		// in dock-layout). This used to be a second, simpler copy here -
		// a fixed 150 px ceiling and a flat share of the width - and the
		// two disagreed: beside an 840 px A the cameras came out as two
		// stamps with 270 px of empty panel under them, while the mockup,
		// which is what every layout decision was being judged on, drew
		// something else entirely.
		int tilesW = 0, blockH = 0;
		TileBlock tb0;
		if (haveTiles) {
			// THE HEIGHT THE BLOCK WILL ACTUALLY HAVE, in every wide
			// arrangement and not only the short one. Left unbounded the
			// calculation believes the bays can be as tall as their width
			// allows and picks the arrangement for a block that will
			// really be much shorter: A comes out height-bound and far
			// narrower than the space it was given, and the difference is
			// the black band beside it.
			const TileBlock tb = tileBlockFor(paneW, bays, visibleTiles,
							  gap, monitorRoomH());
			tb0 = tb;
			tilesW = tb.blockW;
			blockH = tb.blockH;
			tileCap_ = tb.tileW;
		}
		// THE PANES ARE SIZED FROM THE PICTURES, not the other way round:
		// tileBlockFor settled one height for the whole row, so a bay is
		// exactly as wide as that height allows and the cameras take the
		// rest. Sized from the leftover instead, A was height-bound and
		// floated in a pane hundreds of pixels wider than itself.
		const int bayW = haveTiles && tb0.bayW > 0
					 ? tb0.bayW
					 : aspectHeight(paneW / bays) + tagH;
		int baysW = std::max(60, bays * bayW + 3 * (bays - 1));
		if (haveTiles && baysW + tilesW + gap > paneW)
			baysW = std::max(60, paneW - tilesW - gap);
		// ── THE OPERATOR'S DIVIDER, HONOURED ON BOTH SIDES ───────────
		//
		// Once he has dragged it, the two widths are his and the algebra
		// above is only a starting point. What matters is that BOTH sides
		// then fill what they were given: the bays at 16:9 in their pane,
		// the cameras at 16:9 in theirs, each at its own height, and the
		// row as tall as the taller of the two.
		//
		// Honouring one side and not the other is what banded: the
		// cameras took the width they were given and the bays kept the
		// width the algebra had picked, so the difference came back as
		// empty panel down the middle of the row.
		const QList<int> have = monitorSplit_->sizes();
		if (haveTiles && monitorSplitChosen() && have.size() > 1 &&
		    have[0] > 0 && have[1] > 0) {
			baysW = std::max(60, have[0]);
			tilesW = std::max(kTileMinWidth, have[1]);
			tileCap_ = std::max(kTileMinWidth,
					    (tilesW - (cols - 1) * kTileGap) / cols);
			blockH = rows * (aspectHeight(tileCap_) + tagH) +
				 (rows - 1) * kTileGap;
		}
		const int bayH = aspectHeight((baysW - 3 * (bays - 1)) / bays) + tagH;
		// monitorRoomH() is the ONE authority on how tall this block may be,
		// and the tile arithmetic above was given the same number.
		want = std::min(std::max(bayH, blockH), monitorRoomH());
		if (haveTiles && !monitorSplitChosen()) {
			// Anything the row cannot fill is split between the two
			// panes, so each picture is centred in its own rather than
			// the whole block hugging one edge.
			const int slack = std::max(0, paneW - baysW - tilesW - gap);
			monitorSplit_->setSizes(
				{baysW + slack / 2, tilesW + slack - slack / 2});
		}
	}

	// A tile is a CONFIDENCE MONITOR, so it has a ceiling: left to fill the
	// row, a single configured camera drew itself as big as the picture being
	// watched — the same angle twice, with the event list paying for the
	// second copy.
	// ...AND A CEILING ON ITS HEIGHT, which is the same statement made
	// the other way round: beside a big A the camera column is as tall as
	// A is, and a box left free to fill it holds one small picture in the
	// middle of a tall empty rectangle. Capped, the pictures stack at the
	// top and the leftover goes to the spare row below them.
	//
	// IN A COLUMN THERE IS NO CEILING, and that is not a detail: in a
	// column the strip's height is exactly what the pictures need, so a
	// ceiling would be a second opinion on the same number - one derived
	// from the width the layout just produced, fed back into the layout
	// that produced it. Written that way this function span the panel until
	// it was killed. Beside a big A the ceiling is a CONSTANT.
	const int tileCapH = panelMode_ == PanelMode::Tall
				     ? QWIDGETSIZE_MAX
				     : aspectHeight(tileCap_) + tagH;
	for (const PreviewTile &t : tiles_)
		if (t.box) {
			// Only when it CHANGED. Setting a maximum invalidates
			// the layout, and this runs from the resize it would
			// then cause.
			if (t.box->maximumWidth() != tileCap_)
				t.box->setMaximumWidth(tileCap_);
			if (t.box->maximumHeight() != tileCapH)
				t.box->setMaximumHeight(tileCapH);
		}

	applyPreviewSplit(want);
	queueMonitorDump("applyPreviewAspect");
}

// ── WHERE THE KEYS LIVE ──────────────────────────────────────────────────
//
// Across the foot of the panel, or stacked in the left column beside the list.
// The strip carries no picture, so moving it between the two layouts costs a
// relayout and nothing else — which is exactly why the PICTURES are not the
// thing that moves: re-parenting an OBSQTDisplay destroys its native window and
// strands the obs_display presenting into it.
//
// A no-op unless the answer changed. It is called from applyPanelMode, which is
// called from every resize.
void MultiReplayDock::applyControlsColumn(bool inColumn)
{
	if (!bottomBar_ || !rootLayout_ || !leftColLayout_)
		return;
	if (inColumn == controlsInColumn_)
		return;
	controlsInColumn_ = inColumn;
	if (inColumn) {
		rootLayout_->removeWidget(bottomSep_);
		rootLayout_->removeWidget(bottomBar_);
		bottomSep_->hide();
		leftColLayout_->addWidget(bottomBar_);
	} else {
		leftColLayout_->removeWidget(bottomBar_);
		bottomBar_->setParent(this);
		rootLayout_->addWidget(bottomSep_);
		rootLayout_->addWidget(bottomBar_);
		bottomSep_->show();
	}
	bottomBar_->show();
}

// The divider between the pictures and the list. In Short it divides WIDTH, so
// a height means nothing to it and the pane simply takes the column it is in.
void MultiReplayDock::applyPreviewSplit(int want)
{
	if (panelMode_ == PanelMode::Short) {
		previewPane_->setMaximumHeight(QWIDGETSIZE_MAX);
		// shortSplitLeftWidth (dock-layout.hpp) — the width divider `want`
		// (a height) means nothing to. Only until he has dragged it, same
		// rule as the height case below.
		if (!splitChosen() && splitter_->width() > 0 && listPane_) {
			const int total = splitter_->width() - splitter_->handleWidth();
			const int rightWant = listPane_->sizeHint().width();
			const QList<int> now = splitter_->sizes();
			if (now.size() > 1 && now[1] < rightWant) {
				const int give = shortSplitLeftWidth(
					total, leftCol_->minimumSizeHint().width(),
					rightWant);
				splitter_->setSizes({give, total - give});
			}
		}
		return;
	}
	// A CAP, so the pane can never be GIVEN more room than its pictures can
	// fill — every pixel over is panel showing through where a picture was
	// expected, and the list wanted it.
	//
	// ...AND NO CAP AT ALL ONCE THE OPERATOR HAS DRAGGED THE DIVIDER, which
	// is not a nicety — it is what stops this from being a one-way handle.
	// From that moment the room the row may have IS the pane's height (see
	// monitorRoomFor), so `want` is computed from the pane and then written
	// back onto it as a maximum: the pane can be dragged shorter and never
	// taller again, and each pass can only ratchet it further down. His
	// divider is the answer; the pictures fill what it gives them.
	if (splitChosen()) {
		if (previewPane_->maximumHeight() != QWIDGETSIZE_MAX)
			previewPane_->setMaximumHeight(QWIDGETSIZE_MAX);
		return;
	}
	if (previewPane_->maximumHeight() != want)
		previewPane_->setMaximumHeight(want);
	// …AND THE SPLITTER HAS TO BE TOLD, which the cap alone does not do: a
	// QSplitter hands out height by stretch factor and capped at the top it
	// can still give the pane LESS than the pictures need.
	// (The operator's own choice returned above.)
	const int total = splitter_->height();
	// As much as the pictures need, or as much as is left once the list has
	// its floor — whichever is smaller. A perfect picture over two visible
	// rows of events is the wrong trade on a panel whose point is the list.
	const int give = std::min(want, total - kListPaneFloor);
	const QList<int> now = splitter_->sizes();
	if (give > 0 && (now.isEmpty() || std::abs(now[0] - give) > 2))
		splitter_->setSizes({give, total - give});
}


// THE WIDE ARRANGEMENT'S FLOOR IS SAMPLED AFTER THE PASS, NEVER DURING ONE.
//
// Asking a widget for its minimumSizeHint ACTIVATES its layout, and doing that
// anywhere inside the resize cascade - in here, or in applyPanelMode - does not
// merely read a number: the pass it forces is the one that stays on screen.
// Measured on the mockup, which has the same layer under it: the six speed
// presets came out 38x16 instead of 38x22, in every arrangement, and its own
// hit-target check is what caught it.
//
// One pass late costs nothing. The floor moves only with the WIDTH, and the
// hysteresis in panelModeFor covers the tick it takes to catch up.
void MultiReplayDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	applyPanelMode(panelModeFor(size(), panelMode_, wideFloorH_));
	QTimer::singleShot(0, this, [this]() {
		if (panelMode_ == PanelMode::Wide)
			wideFloorH_ = minimumSizeHint().height();
	});
	// AFTER THE LAYOUT PASS, not during it. A resizeEvent arrives before the
	// children have been re-laid, so the splitter still reports its OLD height
	// here — and a split computed from a stale total is then rescaled
	// proportionally by the layout that follows. Measured: asking for 286/110
	// produced 91/305, which is the squashed picture again by a different
	// route. A zero-delay timer runs once the layout has settled.
	QTimer::singleShot(0, this, [this]() {
		// With the monitors down the left column has to be told again:
		// applyPanelMode early-outs when the arrangement has not changed,
		// so nothing else re-asserts it, and a QSplitter rescales its
		// children proportionally on a resize — which would hand a hidden
		// or empty column a growing share of a growing panel.
		applyMonitorsRoom();
		applyPreviewAspect();
	});
}

// ---------------------------------------------------------------------------
// THE PANEL'S COLOURS
// ---------------------------------------------------------------------------

// Where the sub-control marks live, and what they are for this scheme.
//
// obs_module_config_path() is the plugin's own directory — the one config.json
// is already written to — so this adds no new place for the plugin to own and
// nothing outside it can be reached. A null path (no module context, which is
// the case in a unit test) is an empty directory and the sheet falls back.
// The size the event table is really drawing its items in.
//
// QFontInfo, not QFont: OBS states its base size in POINTS and scales it by the
// operator's font-scale setting, so QFont::pixelSize() on that font is -1 and
// the only way to get the number the painter will use is to ask what it
// resolved to. Before the table exists (the first pass, from the constructor)
// the application font is the same answer, because that is what the table will
// inherit.
int MultiReplayDock::rowFontPx() const
{
	const QFont f = events_ ? events_->font() : qApp->font();
	return std::max(8, QFontInfo(f).pixelSize());
}

SheetAssetPaths MultiReplayDock::sheetAssets() const
{
	char *dir = obs_module_config_path("");
	const QString where = dir ? QString::fromUtf8(dir) : QString();
	bfree(dir);
	return writeSheetAssets(where, sc());
}

void MultiReplayDock::restyleSearchIcon()
{
	if (!searchIcon_)
		return;
	searchIcon_->setPixmap(iconFor(Icon::Search, QColor(sc().textMuted), 13,
				       devicePixelRatioF())
				       .pixmap(13, 13));
}

void MultiReplayDock::applyTheme()
{
	const Config cfg = ReplayCore::instance().getConfig();
	const int choice = cfg.uiTheme;
	// qApp->palette() rather than this->palette(): OBS applies the theme's
	// palette to the APPLICATION, and a widget's own palette is a copy that
	// may have been resolved before the theme changed.
	sc() = schemeFor((ThemeChoice)choice, qApp->palette());
	// THE MARKS A SUB-CONTROL CAN ONLY BE HANDED AS A FILE — the tick in a
	// check box, the two arrows on a spin box, the one on a selector. They are
	// drawn by us like every other mark on this panel (see dock-assets.hpp for
	// why they cannot simply be a QIcon), written into the plugin's own config
	// directory, and re-drawn here so that they follow a theme change like the
	// key marks do. If the write fails the sheet says `image: none` and the
	// panel is plainer, not broken.
	const SheetAssetPaths marks = sheetAssets();
	setStyleSheet(dockStyle(sc(), cfg.tableDensity, marks, rowFontPx()));
	// ...AND THE ROW FONT IS ONLY KNOWN ONCE THE SHEET HAS BEEN APPLIED.
	// Qt writes a style sheet's font-size onto the widget during polish, and
	// the size the table draws its items in comes from OBS's own
	// `QWidget { font-size: … }` — so the number handed to the sheet a line
	// above was measured BEFORE that landed and can be a pass behind. Asked
	// again and re-applied once if it moved; it converges, because nothing in
	// our sheet sets a font on the table itself.
	if (const int settled = rowFontPx(); settled != rowFont_) {
		rowFont_ = settled;
		setStyleSheet(dockStyle(sc(), cfg.tableDensity, marks, settled));
	}
	applyTableDensity(cfg.tableDensity);

	// THE MARKS ARE PIXMAPS, so unlike every label on this panel they do not
	// follow a new style sheet: a theme that moved the key colour under them
	// would leave them the colour they were drawn in, which on a light panel
	// is a row of keys with invisible marks on them. Each one remembers which
	// mark it is (see dock-icons.hpp) and is redrawn from the new scheme.
	restyleIcons(this, tintsFor(sc()));
	restyleSearchIcon();
	// ...AND THE HEIGHTS THE SECTIONS PINNED. Applying a style sheet writes
	// its min-height onto every widget it matches, and this panel has a rule
	// that stands the pinned keys' min-height down to nothing on purpose — so
	// re-applying the sheet drops the two-row height off REC and the green
	// play key. Measured: 56 px at start-up, 46 px after a theme change. See
	// kPinnedHeightProperty in dock-layout.hpp.
	repinKeys(this);

	// The two painted widgets read sc() directly and are not restyled by the
	// sheet, so they have to be told to redraw. Everything else — projectLbl_
	// included, now that its colour comes from the mrProject property rule in
	// dock-style.hpp rather than a per-widget style sheet — Qt repolishes for
	// us when the style sheet is replaced.
	if (seek_)
		seek_->update();
	if (clipBar_)
		clipBar_->update();
	obs_log(LOG_INFO, "[dock] theme %d applied (panel %s, text %s)", choice,
		qUtf8Printable(sc().panel), qUtf8Printable(sc().text));
}

// How tight the event list is drawn. The style sheet has already been given the
// matching input metrics (see dockStyle); this is the half of the pair that
// lives on the widgets.
void MultiReplayDock::applyTableDensity(int level)
{
	if (!events_)
		return;
	const Density d = densityFor(level);

	// DOWN as well as up. refreshEvents() raises the row height to whatever
	// the tallest cell it built needs and never lowers it, which is right while
	// the metrics are fixed and wrong the moment they can change: without this
	// reset, picking "compact" would rebuild shorter cells inside rows that
	// were still 30 px tall, and nothing would look any denser.
	events_->verticalHeader()->setDefaultSectionSize(d.rowFloor);

	// The headings, which are not cells and so are not raised by anything.
	QHeaderView *hh = events_->horizontalHeader();
	// ...AND THE HEIGHT IS ASKED OF THE TYPE, not written down beside it. It
	// was the constant alone, and the constant knows nothing about the padding
	// and the border the style draws round the label: at the denser settings
	// the headings came out clipped along the top. Same lesson as the row
	// height, which is taken from the cell that was actually built.
	{
		QFont hf = hh->font();
		hf.setPixelSize(d.headerFont);
		hf.setBold(true);
		// 3 px of padding top and bottom plus the rule under it.
		const int need = QFontMetrics(hf).height() + 7;
		hh->setFixedHeight(std::max(d.headerH, need));
	}

	// The cells carry their own built-in size hints, so they have to be built
	// again to pick up the new metrics. version() has not moved, so ask
	// directly rather than waiting for a mark.
	eventsDirty_ = true;
}

void MultiReplayDock::restyleDock()
{
	if (g_dock)
		g_dock->applyTheme();
}

void MultiReplayDock::rebuildMultiview()
{
	if (!multiviewBox_ || !multiviewGrid_)
		return;

	const Config cfg = ReplayCore::instance().getConfig();

	// Which tiles belong on screen, and what they are called. Only configured
	// cameras: eight black rectangles on a two-camera rig is six tiles of
	// nothing between the operator and the two that matter — and six
	// obs_displays doing it.
	// NOT called `slots`: with Qt keywords enabled that is a macro expanding
	// to nothing, and the declaration silently becomes a no-op.
	std::vector<int> tileSlots;
	QStringList captions;
	for (int i = 0; i < kMaxCameras; i++) {
		if (cfg.cameras[i].sourceName.empty())
			continue;
		const std::string &dn = cfg.cameras[i].displayName;
		tileSlots.push_back(i);
		captions << QString("%1 %2").arg(i + 1).arg(
			dn.empty() ? QString("Cam %1").arg(i + 1)
				   : QString::fromStdString(dn));
	}
	// The multiview is CAMERAS only. It used to carry a "Replay" tile as
	// well, from the time there was one channel and the big preview followed
	// the live camera — so "what will go on air" had nowhere else to be
	// seen. There are two output boxes above it now, A and B, bigger,
	// labelled and always right: a third, smaller copy of the same picture
	// is a tile taken away from the cameras and one more display on the
	// shared graphics thread for nothing.

	const bool show = cfg.showMultiview;
	// THE COLUMN COUNT IS PART OF THE SIGNATURE. Without it the early-out
	// below is a re-layout that never happens: the panel switches to a column,
	// asks for a filmstrip, and gets the two-column block back because the
	// cameras and their names did not change.
	const int cols = tileColumns((int)tileSlots.size());
	QStringList sigParts;
	for (int s : tileSlots)
		sigParts << QString::number(s);
	// THE ARRANGEMENT IS PART OF IT TOO, and leaving it out was a hole the
	// column count happened to cover most of the time: the spare row and the
	// spare column are chosen by the arrangement, not by the cameras, so on a
	// rig whose column count is the same in two arrangements - two cameras
	// are two columns in both - turning the panel kept the other one's rule.
	const QString sig = QString::number(show ? 1 : 0) + '|' +
			    QString::number((int)panelMode_) + '|' +
			    QString::number(cols) + '|' + sigParts.join(',') +
			    '|' + captions.join(',');
	// TEMP DIAGNOSTIC — which slots become tiles, and whether this call is
	// about to do nothing. "C1..C8 shown on a two-camera rig" would surface
	// here as a tileSlots list with more than the configured cameras in it,
	// or as an early-out that never re-hides slots an earlier pass showed.
	MR_DLOG("[mondiag] rebuildMultiview: mode=%s show=%d cfgSlots=[%s] cols=%d "
		"earlyOut=%d sig=%s",
		panelModeName(panelMode_), show ? 1 : 0,
		sigParts.join(',').toUtf8().constData(), cols,
		(sig == multiviewSig_) ? 1 : 0, sig.toUtf8().constData());
	if (sig == multiviewSig_)
		return;
	multiviewSig_ = sig;

	// HOW MANY CAMERAS THERE ARE INVALIDATES THE OPERATOR'S DIVIDER, exactly
	// as changing the arrangement does, and for the same reason. "This much
	// for the bays, that much for the cameras" is an answer about a rig of
	// eight; on a rig of four the camera block needs a third of that width
	// and the bays should have the rest. Kept, the panel came back from
	// Settings with A still the size it was for eight and nothing on screen
	// explaining why. The split is re-derived once; he may drag it again.
	if ((int)tileSlots.size() != lastTileCount_) {
		lastTileCount_ = (int)tileSlots.size();
		for (bool &chosen : userMonitorSplit_)
			chosen = false;
	}

	multiviewBox_->setVisible(show);
	for (int i = 0; i < kMaxPreviewTiles; i++)
		if (tiles_[i].box)
			tiles_[i].box->setVisible(false);
	// PURGE EVERY EXISTING ITEM FIRST. QGridLayout::addWidget on a widget the
	// layout ALREADY tracks appends a second item for it rather than moving
	// it, so re-laying on each mode change silently accumulates stale items
	// at old cells. takeAt() removes the layout item without re-parenting the
	// widget, so the OBSQTDisplay under each tile — and the obs_display bound
	// to its native window — is untouched. The tiles are all re-added below;
	// nothing else lives in this grid.
	while (multiviewGrid_->count() > 0)
		delete multiviewGrid_->takeAt(0);
	for (size_t k = 0; k < tileSlots.size(); k++) {
		PreviewTile &t = tiles_[tileSlots[k]];
		if (!t.box)
			continue;
		t.caption->setText(captions[(int)k]);
		t.caption->setToolTip(
			t.cam0 >= 0 ? QString("%1 %2")
					      .arg(obs_module_text("Dock.Angle"))
					      .arg(t.cam0 + 1)
				    : QString(obs_module_text("Dock.ReplayTileHint")));
		// Moving a widget between cells of the grid it is ALREADY in does
		// not re-parent it (QLayout::addChildWidget only calls setParent
		// when the parent differs), so the native window — and the display
		// bound to it — survives.
		// CAPPED IN A COLUMN, free beside a big A. Without a cap the grid
		// hands a lone tile the whole row and it draws itself as big as
		// the picture being watched — the same angle, twice, and the event
		// list paying for the second copy.
		// applyPreviewAspect settles the real ceiling a moment later; this
		// only stops a freshly shown tile claiming the whole row for one
		// frame.
		t.box->setMaximumWidth(tileCap_ > 0 ? tileCap_ : QWIDGETSIZE_MAX);
		multiviewGrid_->addWidget(t.box, (int)k / cols, (int)k % cols);
		t.box->setVisible(show);
	}
	// THE COLUMNS IN USE SHARE THE ROW, and a box in this grid has no size
	// of its own to fall back on: AspectBox declares no floor and no hint
	// deliberately (a floor here becomes the panel's). A column left at
	// stretch 0 beside one that has it is therefore not "narrow", it is
	// ZERO — which is how the column arrangement came to show an empty box
	// where the cameras are. It was drawn correctly only by accident: the
	// early-out above kept the wide arrangement's stretches in place on a rig
	// whose column count did not change when the panel was turned, and the
	// column rule below was never actually reached.
	//
	// The leftover cannot land in the tiles either way, because each one is
	// capped at the width its own picture may have (see applyPreviewAspect):
	// it stays as trailing space, so a filmstrip of two on a rig of two still
	// starts at the left edge instead of floating in the middle of the row.
	// COVER EVERY COLUMN THAT HAS EVER EXISTED, not just 0..cols. A
	// QGridLayout's columnCount() never comes back down, so the filmstrip
	// arrangement (up to eight columns) leaves columns 5-7 stretched, and a
	// later Wide pass that only reset 0..4 would split the pane among seven
	// stretched columns instead of four — the tiles come back at half width.
	// This is the column twin of the usedRows/rowCount() guard just below.
	for (int c = 0; c < std::max(cols + 1, multiviewGrid_->columnCount());
	     c++)
		multiviewGrid_->setColumnStretch(c, c < cols ? 1 : 0);
	// THE ROWS IN USE SHARE THE BLOCK; the rest hold nothing. A
	// QGridLayout remembers the stretch of a row it no longer has anything
	// in and rowCount() never comes back down, so a rig that once wanted
	// four rows keeps them stretching after it wants one - and the pictures
	// get a quarter of the height the block was given. Rows past the last
	// one in use are also where the hidden tiles were left sitting.
	const int usedRows =
		((int)tileSlots.size() + cols - 1) / std::max(1, cols);
	// NO SPARE ROW: the camera block is now exactly as tall as the bays
	// beside it (tileBlockFor settles ONE height for the whole monitoring
	// row), so there is nothing left over to park. A spare row here is the
	// empty band that used to sit under C1 and C2.
	//
	// The bound is usedRows + 1, NOT rowCount(): setRowStretch on an index
	// past the end GROWS the grid, so a loop that walks to rowCount() adds a
	// row every time it runs - and this runs whenever the cameras change.
	for (int r = 0;
	     r < std::max(usedRows + 1, multiviewGrid_->rowCount()); r++)
		multiviewGrid_->setRowStretch(r, r < usedRows ? 1 : 0);
	tileTallyPvw_ = -2; // captions were just rewritten
	tileTallyPgm_ = -2;
	updateMultiviewTally();
	queueMonitorDump("rebuildMultiview");
	// A tile appeared or went away, so the pane needs a different height: its
	// cap is the sum of the pictures actually in it.
	applyPreviewAspect();
}

void MultiReplayDock::refreshTileSources()
{
	const Config cfg = ReplayCore::instance().getConfig();
	// Which slot's FEED a tile actually reads during review — see
	// ensureTileFeeds(): a duplicate slot (camera-dedup.hpp) has no feed of
	// its own, so its tile shows the canonical slot's, the same decoded
	// texture as every other tile naming that source.
	std::array<std::string, kMaxCameras> srcNames{};
	for (int i = 0; i < kMaxCameras; i++)
		srcNames[i] = cfg.cameras[i].sourceName;
	const auto canonical = canonicalCameraIndices(srcNames);
	// LIVE OR REVIEW — the same question the big preview asks, and the same
	// answer.
	//
	// Following live, a tile is a confidence monitor: that camera as it is
	// now. Out of follow-live — the operator has cued a row, played an event
	// or moved the bar — a tile still showing the camera as it is NOW is worse
	// than a black rectangle, because it reads as the footage of the moment
	// being reviewed and is not. So it shows that moment on its own lens,
	// which is what the boxes are for: choosing the angle before the replay
	// goes up.
	const bool live = ReplayCore::instance().followLive();
	tilesLive_ = live;
	for (int i = 0; i < kMaxPreviewTiles; i++) {
		PreviewTile &t = tiles_[i];
		obs_source_t *next = nullptr;
		// A hidden tile publishes nothing: its display is disabled anyway
		// (OBSQTDisplay disables on QEvent::Hide), and holding a reference
		// to a source nobody is drawing keeps it alive for no reason.
		if (t.box && t.box->isVisible()) {
			if (t.cam0 < 0) {
				if (previewHasContent_)
					next = chan()
						       .acquireSource();
			} else if (live) {
				const std::string &nm =
					cfg.cameras[t.cam0].sourceName;
				if (!nm.empty())
					next = obs_get_source_by_name(nm.c_str());
			} else if (t.cam0 < kMaxCameras) {
				// Redirected through the owning slot (see the note
				// above): a duplicate camera has no feed of its own.
				const int owner = canonical[t.cam0];
				if (owner < (int)tileFeed_.size() &&
				    tileFeed_[owner]) {
					// The STICKY flag, not the feed's hasPosition().
					// "Has this feed ever shown a picture" is the
					// question; hasPosition() answers "has THIS clip
					// pushed a frame yet", and play() zeroes the
					// stats at the start of every clip — so asking it
					// blacked the tile out at every cue and left it
					// black until the next 4 Hz beat. See
					// tileFeedHadPicture_.
					if (tileFeedHadPicture_[owner])
						next = tileFeed_[owner]
							       ->acquireSource();
				}
			}
		}
		obs_source_t *prev = nullptr;
		{
			std::lock_guard<std::mutex> lk(tileMutex_);
			prev = tileSource_[i];
			tileSource_[i] = next;
		}
		// Released outside the lock: the last release destroys the source,
		// which enters the graphics context — and the graphics thread is
		// what wants tileMutex_. See the note on previewSource_.
		if (prev)
			obs_source_release(prev);
	}
}

// ---------------------------------------------------------------------------
// The angle boxes during a review — a preview feed per camera
// ---------------------------------------------------------------------------

void MultiReplayDock::ensureTileFeeds()
{
	const Config cfg = ReplayCore::instance().getConfig();

	// Which slot owns the feed for its source (camera-dedup.hpp). Several
	// slots pointed at one physical source is the same picture, and
	// decoding it once per slot — eight independent ReplayChannel
	// instances, eight decode threads racing each other — is exactly what
	// made the tiles desynchronise: each caught up with a newly cued clip
	// at its own pace, so for a while some tiles still showed the
	// PREVIOUS event's last frame while others already showed the new
	// one. Only the canonical slot gets a feed; refreshTileSources() reads
	// a duplicate's picture from the canonical slot's feed instead, which
	// is not just the same footage but the very same decoded texture — so
	// duplicate tiles cannot show anything but each other, in lockstep.
	std::array<std::string, kMaxCameras> srcNames{};
	for (int i = 0; i < kMaxCameras; i++)
		srcNames[i] = cfg.cameras[i].sourceName;
	const auto canonical = canonicalCameraIndices(srcNames);

	// A feed exists for a camera the operator has configured AND whose
	// tile is on screen. Monitors off, the multiview switched off, an
	// unconfigured slot: no feed, no decoder, no source. That is the
	// whole cost control, and it is the same one the tiles themselves
	// already use.
	std::array<bool, kMaxCameras> wanted{};
	for (int cam = 0; cam < kMaxCameras; cam++)
		wanted[cam] = !cfg.cameras[cam].sourceName.empty() &&
			      cam < kMaxPreviewTiles && tiles_[cam].box &&
			      tiles_[cam].box->isVisible();

	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		if (cam < kMaxCameras && canonical[cam] != cam) {
			// A duplicate slot never gets a feed of its own.
			tileFeed_[cam].reset();
			tileFeedHadPicture_[cam] = false;
			continue;
		}

		// The canonical slot's feed is wanted if its OWN tile wants one,
		// or a duplicate slot sharing its source does — the lens is still
		// on screen even if it is the duplicate's box that shows it.
		bool feedWanted = cam < kMaxCameras && wanted[cam];
		if (!feedWanted && cam < kMaxCameras)
			for (int j = 0; j < kMaxCameras; j++)
				if (canonical[j] == cam && wanted[j]) {
					feedWanted = true;
					break;
				}

		if (feedWanted == (bool)tileFeed_[cam])
			continue;
		if (!feedWanted) {
			// Destroying it stops and joins its worker; the private
			// input goes with it.
			tileFeed_[cam].reset();
			tileFeedHadPicture_[cam] = false;
			continue;
		}
		tileFeed_[cam] = ReplayChannel::makePreview(
			"MultiReplay - Angle feed " + std::to_string(cam + 1));
		// A brand new feed really has nothing behind it, which is the one
		// moment the tile SHOULD be black.
		tileFeedHadPicture_[cam] = false;
	}
}

bool MultiReplayDock::pollTileFeedPictures()
{
	bool changed = false;
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		// Only the ones that have not answered yes yet, so once every feed
		// has a picture this loop is eight bool reads and nothing else. It
		// runs on EVERY tick on purpose: the first picture is what the
		// operator is waiting for, and making him wait for the 4 Hz beat
		// as well is precisely the quarter second this is fixing.
		if (tileFeedHadPicture_[cam] || !tileFeed_[cam])
			continue;
		if (tileFeed_[cam]->hasPosition()) {
			tileFeedHadPicture_[cam] = true;
			changed = true;
		}
	}
	return changed;
}

void MultiReplayDock::prefetchTiles(int64_t inNs, int64_t outNs, int speedPct)
{
	if (inNs == kNoInstant || outNs == kNoInstant || outNs <= inNs)
		return;
	// Capped exactly the way cueTiles() caps, or the two would ask for
	// different ranges and the cache key would never match — a prefetch that
	// cannot be hit is a file read for nothing.
	outNs = std::min(outNs, inNs + kTileReviewMaxNs);
	const int pct = std::clamp(speedPct > 0 ? speedPct : 100, 5, 400);
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		// SKIP THE ONES STILL READING. prefetch() joins the previous
		// worker on the calling thread, and this thread is the one that
		// draws the panel: with a feed per camera, an operator walking the
		// list faster than a cold read completes would stall the dock by
		// up to a file read PER ANGLE, and the stall would come and go
		// with how warm the cache happened to be. Skipping is free — a
		// prefetch that is not issued only means play() fetches for
		// itself, which is the arrangement everything here already
		// tolerates.
		if (!tileFeed_[cam] || tileFeed_[cam]->prefetchBusy())
			continue;
		ReplayChannel::PlayRequest req;
		req.camIndex = cam;
		req.inNs = inNs;
		req.outNs = outNs;
		req.speedPct = pct;
		tileFeed_[cam]->prefetch(req);
	}
}

void MultiReplayDock::cueTiles(int64_t inNs, int64_t outNs, int speedPct,
			       ReplayChannel::Direction dir, int maxFrames)
{
	if (inNs == kNoInstant || outNs == kNoInstant || outNs <= inNs)
		return;
	// Capped, because a feed materialises its clip's packets in RAM and a free
	// review can ask for a whole session. Past the cap the tiles simply hold
	// the last frame they were given, which is still the reviewed moment on
	// that lens.
	outNs = std::min(outNs, inNs + kTileReviewMaxNs);
	const int pct = std::clamp(speedPct > 0 ? speedPct : 100, 5, 400);
	// CALLED FROM poll(), thirty times a second. Re-issuing the same cue would
	// restart every feed on every tick — eight decoders started and joined
	// thirty times a second, which is not slow, it is a dock that never shows
	// a picture.
	if (inNs == tileCueInNs_ && outNs == tileCueOutNs_ &&
	    pct == tileCueSpeedPct_ && dir == tileCueDir_ &&
	    maxFrames == tileCueMaxFrames_)
		return;
	tileCueInNs_ = inNs;
	tileCueOutNs_ = outNs;
	tileCueSpeedPct_ = pct;
	tileCueDir_ = dir;
	tileCueMaxFrames_ = maxFrames;

	ensureTileFeeds();
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		if (!tileFeed_[cam])
			continue;
		ReplayChannel::PlayRequest req;
		req.camIndex = cam;
		req.inNs = inNs;
		req.outNs = outNs;
		req.speedPct = pct;
		req.direction = dir;
		req.maxFrames = maxFrames;
		std::string err;
		if (!tileFeed_[cam]->play(req, err))
			// NOT a notice, and not a warning either. An angle with
			// nothing behind it at that instant is ordinary — a camera
			// added mid-session, a slot whose ring has wrapped — and
			// the tile answering with black is exactly right. Shouting
			// about it on every cue would train the operator to ignore
			// the strip that carries the messages that matter.
			MR_DLOG("[dock] angle feed %d: %s", cam + 1, err.c_str());
	}
}

void MultiReplayDock::releaseTileFeeds()
{
	for (auto &f : tileFeed_)
		f.reset();
	tileFeedHadPicture_.fill(false);
	tileCueInNs_ = kNoInstant;
	tileCueOutNs_ = kNoInstant;
	tileCueSpeedPct_ = 0;
	tileCueDir_ = ReplayChannel::Direction::Forward;
}

MultiReplayDock::MultiviewState MultiReplayDock::multiviewState() const
{
	MultiviewState s;
	s.followingReview = !ReplayCore::instance().followLive();
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		if (!tileFeed_[cam])
			continue;
		s.feeds++;
		// The sticky flag, which is what the tile PUBLISH keys on — so the
		// gate asserts the property the operator can actually see, not a
		// per-clip counter that is false for the first tens of
		// milliseconds of every clip.
		if (tileFeedHadPicture_[cam])
			s.feedsWithPicture++;
		if (tileFeed_[cam]->hasPosition())
			s.feedsWithCurrentClip++;
	}
	s.cueInNs = tileCueInNs_;
	s.cueOutNs = tileCueOutNs_;
	{
		// The camera tiles only: the replay tile does not exist any more,
		// but the slot arithmetic still says so, and counting a slot that
		// is not a camera would make the number disagree with the strip.
		std::lock_guard<std::mutex> lk(tileMutex_);
		for (int i = 0; i < kMaxPreviewTiles; i++)
			if (tiles_[i].cam0 >= 0 && tileSource_[i])
				s.tilesPublished++;
	}
	return s;
}

std::vector<OBSQTDisplay *> MultiReplayDock::allDisplays() const
{
	std::vector<OBSQTDisplay *> out;
	out.reserve(2 + tiles_.size());
	if (displayA_)
		out.push_back(displayA_);
	if (displayB_)
		out.push_back(displayB_);
	for (const PreviewTile &t : tiles_)
		if (t.display)
			out.push_back(t.display);
	return out;
}

MultiReplayDock::PreviewStats MultiReplayDock::previewStats() const
{
	PreviewStats s;
	// Long enough that a dock still being laid out is not called starved,
	// short enough that a preview an operator is already looking at is. The
	// display itself forces its own creation at 3 s (see qt-display), so a
	// widget still empty at 5 s has failed for a reason no waiting will fix.
	constexpr int64_t kStarvedMs = 5000;
	const auto account = [&s](const OBSQTDisplay *d) {
		if (!d)
			return;
		s.tiles++;
		if (!d->isVisible())
			return;
		s.visible++;
		if (d->display()) {
			s.withDisplay++;
			return;
		}
		const int64_t waited = d->blockedMs();
		s.worstBlockedMs = std::max(s.worstBlockedMs, waited);
		if (waited >= kStarvedMs)
			s.starved++;
	};
	for (const OBSQTDisplay *d : allDisplays())
		account(d);
	return s;
}

// Where the zones ended up, in the dock's own coordinates. See LayoutProbe in
// the header for why the gate is allowed to ask.
int MultiReplayDock::heldSourceRefs() const
{
	int n = 0;
	{
		std::lock_guard<std::mutex> lk(previewMutex_);
		n += previewSource_ ? 1 : 0;
		n += previewSourceB_ ? 1 : 0;
	}
	// ...and the angle feeds. Their inputs are PRIVATE, so they are not part
	// of the operator's scene collection — but they are sources libobs is
	// holding on our behalf, and "released everything before OBS cleared its
	// data" is a claim that has to include them or it is not the claim.
	for (const auto &f : tileFeed_)
		n += f ? 1 : 0;
	std::lock_guard<std::mutex> lk(tileMutex_);
	for (obs_source_t *s : tileSource_)
		n += s ? 1 : 0;
	return n;
}

void MultiReplayDock::releasePreviewRefs()
{
	// The static half only finds the live dock; the work is the member below,
	// so the destructor can do it too WITHOUT going through the pointer it has
	// already cleared.
	if (g_dock)
		g_dock->dropPreviewRefs();
}

void MultiReplayDock::prepareForShutdown()
{
	if (!g_dock)
		return;
	// COME OUT OF FULL SCREEN BEFORE OBS WRITES ITS LAYOUT. OBS saves the dock
	// geometry on the way out and restores it on the way in, so a panel left
	// full screen here comes back next launch as a floating window the size of
	// a whole monitor with no way to tell it was ever anything else — and the
	// operator's own window size, the one this key was careful to remember, is
	// gone with it.
	g_dock->setPanelFullScreen(false);
	// STOP POLLING FIRST, then let go. Releasing alone was not enough: the poll
	// timer runs at 30 Hz and re-resolves these references on its slow beat, so
	// between "OBS says it is exiting" and "OBS clears scene data" a tick could
	// slip in and take a fresh reference — and OBS then named that one source in
	// the dialog. Measured: the take pass came out clean and the reopen pass,
	// where shutdown follows polling more closely, still reported one.
	if (g_dock->pollTimer_)
		g_dock->pollTimer_->stop();
	g_dock->dropPreviewRefs();
}

void MultiReplayDock::dropPreviewRefs()
{
	obs_source_t *a = nullptr;
	obs_source_t *b = nullptr;
	{
		// Swapped out under the lock, released outside it: the last release
		// destroys the source and enters the graphics context, which is the
		// thread that wants previewMutex_.
		std::lock_guard<std::mutex> lk(previewMutex_);
		a = previewSource_;
		b = previewSourceB_;
		previewSource_ = nullptr;
		previewSourceB_ = nullptr;
	}
	if (a)
		obs_source_release(a);
	if (b)
		obs_source_release(b);

	// ...and the MULTIVIEW tiles, which are the operator's own cameras. These
	// were the "- C1 / - C2" in the same OBS complaint: one owned reference per
	// tile, dropped only in the destructor, which runs long after scene data has
	// been cleared. Same swap-then-release discipline (see above) for the same
	// reason: the last release enters the graphics context.
	{
		std::array<obs_source_t *, kMaxPreviewTiles> dying{};
		{
			std::lock_guard<std::mutex> lk(tileMutex_);
			dying = tileSource_;
			tileSource_.fill(nullptr);
		}
		for (obs_source_t *s : dying)
			if (s)
				obs_source_release(s);
	}
	// ...and the angle FEEDS, which own private inputs of their own. Same
	// reason and the same deadline: this runs on the way out of a scene
	// collection and on exit, and anything still alive at that moment is
	// reported to the operator as a plugin that leaked. Destroying a feed
	// stops and joins its worker first, which is why it is safe here — the
	// worker takes no lock this thread can be holding.
	releaseTileFeeds();
	// So poll() resolves them all again rather than trusting its cache.
	previewCam0_ = -1;
}

MultiReplayDock::LayoutProbe MultiReplayDock::layoutProbe() const
{
	LayoutProbe lp;
	const auto topOf = [this](const QWidget *w) {
		return w ? w->mapTo(this, QPoint(0, 0)).y() : -1;
	};
	const auto bottomOf = [&topOf](const QWidget *w) {
		return w ? topOf(w) + w->height() : -1;
	};
	// The picture block is the big preview AND the tile grid beside it,
	// whichever reaches lower: "under the pictures" has to mean under all of
	// them.
	lp.previewBottomY =
		std::max(bottomOf(displayA_), bottomOf(multiviewBox_));
	lp.searchY = topOf(search_);
	lp.listTabsY = topOf(listTabs_);
	lp.tableY = topOf(events_);
	lp.clipBarY = topOf(clipBar_);
	lp.seekY = topOf(seek_);
	lp.channelBVisible = bBox_ && bBox_->isVisibleTo(this);
	if (seek_) {
		lp.seekHeight = seek_->height();
		lp.seekGraduations = seek_->graduations();
		lp.seekEnabled = seek_->hasTimeline();
	}
	return lp;
}

void MultiReplayDock::updateMultiviewTally()
{
	const auto ps = pc().playState();
	const int pvw = currentAngle1() - 1;
	const int pgm = (ps.active && ps.angle1 > 0) ? ps.angle1 - 1 : -1;
	if (pvw == tileTallyPvw_ && pgm == tileTallyPgm_)
		return;
	tileTallyPvw_ = pvw;
	tileTallyPgm_ = pgm;
	for (int i = 0; i < kMaxPreviewTiles; i++) {
		PreviewTile &t = tiles_[i];
		if (!t.caption)
			continue;
		QString tally;
		if (t.cam0 < 0)
			tally = QStringLiteral("replay");
		else if (t.cam0 == pgm)
			tally = QStringLiteral("pgm");
		else if (t.cam0 == pvw)
			tally = QStringLiteral("pvw");
		if (t.caption->property("tally").toString() == tally)
			continue;
		t.caption->setProperty("tally", tally);
		repolish(t.caption);
	}
}

void MultiReplayDock::applyChannelBVisibility()
{
	const bool on = ReplayCore::instance().getConfig().enableChannelB;
	if (channelBApplied_ == (on ? 1 : 0))
		return;
	channelBApplied_ = on ? 1 : 0;
	channelBEnabled_ = on;

	// The B box, B's angle row, the A|B/A/B selector and the swap key are all
	// one decision. With one bay they are not "disabled" — they are ABSENT: a
	// greyed-out selector on a single-channel rig is three keys of furniture
	// and a question the operator has to answer every time he looks at it.
	if (bBox_)
		bBox_->setVisible(on);
	// …and the picture grid has to be told, because B's share of the room is a
	// property of its ROW or COLUMN rather than of the box in it: hiding the
	// box alone leaves the space it had. Re-applying the arrangement re-reads
	// the flag and sets that share to zero.
	applyPanelMode(panelMode_, /*force*/ true);
	// The selector and the swap COLLAPSE rather than holding their place: a
	// bay the operator has switched off is not coming back until he switches
	// it on, and with them gone the section is empty and takes no room at all.
	for (QWidget *w : channelBWidgets_) {
		QSizePolicy sp = w->sizePolicy();
		sp.setRetainSizeWhenHidden(false);
		w->setSizePolicy(sp);
		w->setVisible(on);
	}
	// ...and its CAPTION, through the section itself rather than by poking
	// the label directly: apply() decides cap_'s visibility on every
	// relayout (setFlat(), setShapes(), the measure() a few lines below
	// triggers), so telling the section once and having it forgotten by the
	// next pass is exactly the bug this method exists to fix. See
	// KeyBlock::setSectionVisible().
	if (angleBlock_)
		angleBlock_->setSectionVisible(on);
	// The section is a row shorter or a row longer, which the strip only
	// learns when the block is re-measured.
	if (strip_ && angleBlock_)
		strip_->blockChanged(angleBlock_);

	if (on) {
		// TWO bays, so the resting state is BOTH: the reference controller's A|B is what makes a
		// two-bay panel one panel, and the operator who asked for a second
		// bay asked for it to be used. He can still pick A or B.
		setActiveChannel(Which::A, /*linked*/ true);
		if (QAbstractButton *ab = chanSel_ ? chanSel_->button(2) : nullptr)
			ab->setChecked(true);
		// The input only exists once he has asked for it.
		ReplayChannel::instance(Which::B).ensureSource();
	} else {
		// One bay: everything means A, and B is stopped rather than left
		// running behind a hidden box.
		setActiveChannel(Which::A, /*linked*/ false);
		if (QAbstractButton *ab = chanSel_ ? chanSel_->button(0) : nullptr)
			ab->setChecked(true);
		PlaybackCoordinator::instance(Which::B).stopEvents();
		ReplayChannel::instance(Which::B).reset();
	}
	obs_log(LOG_INFO, "[dock] channel B %s", on ? "enabled (A|B)" : "disabled");
}


// ---------------------------------------------------------------------------
// Table columns: the fixed ones plus one per configured camera (the reference controller)
// ---------------------------------------------------------------------------

void MultiReplayDock::rebuildEventColumns()
{
	if (!events_)
		return;

	// Which cameras deserve a column, and what they are called. Only the
	// configured ones: eight columns on a two-camera rig is six columns of
	// dashes between the operator and the two that matter.
	const Config cfg = ReplayCore::instance().getConfig();
	std::vector<int> cams;
	QStringList camLabels;
	for (int i = 0; i < kNCams; i++) {
		if (cfg.cameras[i].sourceName.empty())
			continue;
		const std::string &dn = cfg.cameras[i].displayName;
		cams.push_back(i);
		camLabels << QString("%1 %2").arg(i + 1).arg(
			dn.empty() ? QString("Cam %1").arg(i + 1)
				   : QString::fromStdString(dn));
	}

	// Rebuilding blows the table away, so only do it when the answer changed.
	// The labels are part of the answer: renaming a camera has to show.
	QStringList wanted;
	for (int c : cams)
		wanted << QString::number(c);
	static const char *kProp = "mrCamSig";
	const QString sig = wanted.join(',') + '|' + camLabels.join(',');
	if (events_->columnCount() > 0 &&
	    events_->property(kProp).toString() == sig)
		return;
	events_->setProperty(kProp, sig);

	camCols_ = cams;
	camHeaderHot_ = -1; // the highlight belongs to sections that just died

	const bool wasProgrammatic = itemsProgrammatic_;
	itemsProgrammatic_ = true; // setHorizontalHeaderItem must not read back as an edit
	events_->setRowCount(0);
	events_->setColumnCount(kColFirstCam +
			        (int)cams.size() * kColsPerCam);
	QStringList headers;
	headers << QStringLiteral("#") << obs_module_text("Dock.In")
		<< obs_module_text("Dock.Out") << obs_module_text("Dock.Duration")
		<< obs_module_text("Dock.Comment");
	// One heading per camera, and the camera cell now holds the two things
	// that really do differ per lens - does it play, and how fast. WHAT the
	// event is has a column of its own, once per row.
	for (size_t i = 0; i < cams.size(); i++)
		headers << camLabels[(int)i];
	events_->setHorizontalHeaderLabels(headers);
	// The camera headers carry their own label in UserRole: the "angle I am
	// watching" marker is a prefix on the text (see updateCamHeaderHighlight),
	// so the plain label has to survive somewhere.
	for (size_t i = 0; i < cams.size(); i++) {
		QTableWidgetItem *h = events_->horizontalHeaderItem(
			kColFirstCam + (int)i * kColsPerCam);
		if (h)
			h->setData(Qt::UserRole, camLabels[(int)i]);
	}
	{
		QHeaderView *hh = events_->horizontalHeader();
		hh->setHighlightSections(false);
		for (int c = 0; c < events_->columnCount(); c++)
			hh->setSectionResizeMode(c, QHeaderView::ResizeToContents);
		// The camera columns take the slack and share it equally: each
		// holds free text (the comment) and so has no natural width, and
		// four angles that are the same width are four angles the eye can
		// scan down without re-measuring.
		for (int c = kColFirstCam; c < events_->columnCount(); c++)
			hh->setSectionResizeMode(c, QHeaderView::Stretch);
		hh->setMinimumSectionSize(34);
	}
	// AND THE ARRANGEMENT AGAIN. setColumnCount() re-initialises the header's
	// sections, which takes the hidden state of OUT with it — so on a narrow
	// panel the column the column arrangement had put away came back the first
	// time a camera was added or renamed, and the table went back to needing a
	// horizontal scroll. The mode is the authority on which columns fit; it has
	// to be asked again whenever the columns are rebuilt.
	events_->setColumnHidden(kColOut, panelMode_ == PanelMode::Tall);

	itemsProgrammatic_ = wasProgrammatic;
	updateCamHeaderHighlight();
}

void MultiReplayDock::updateCamHeaderHighlight()
{
	if (!events_ || camCols_.empty())
		return;
	int hot = -1;
	for (size_t i = 0; i < camCols_.size(); i++)
		if (camCols_[i] == currentAngle1() - 1)
			hot = kColFirstCam + (int)i * kColsPerCam;
	if (hot == camHeaderHot_)
		return;
	const bool wasProgrammatic = itemsProgrammatic_;
	itemsProgrammatic_ = true;
	for (size_t i = 0; i < camCols_.size(); i++) {
		const int col = kColFirstCam + (int)i * kColsPerCam;
		QTableWidgetItem *h = events_->horizontalHeaderItem(col);
		if (!h)
			continue;
		// The header is the camera's NAME and nothing else. It used to
		// gain a ▶ on the angle being watched, which said the same thing
		// the angle keys already say, twice as far from where the operator
		// was looking — and it moved every column heading sideways by a
		// glyph as he switched angles.
		h->setText(h->data(Qt::UserRole).toString());
	}
	itemsProgrammatic_ = wasProgrammatic;
	camHeaderHot_ = hot;
}

// ---------------------------------------------------------------------------
// Engine interaction helpers
// ---------------------------------------------------------------------------

int64_t MultiReplayDock::markTimeNs() const
{
	// Live: the newest instant the tap actually captured on this angle. It is
	// measured off the encoder on the shared system clock, so the mark lands
	// on the frame that was on screen and means the same instant on every
	// other angle — no arm timestamp, no encoder-startup lag to subtract.
	if (EventStore::instance().liveMode()) {
		int64_t now = PacketTap::instance().newestNs(currentAngle1() - 1);
		if (now > 0) {
			MR_DLOG("[ev] markTime LIVE master=%lldms (cam %d)",
				(long long)(now / 1000000), currentAngle1());
			return now;
		}
	}
	// Recorded mode is the reference controller's "mark at the position of the bar", and the bar is
	// the dock's playhead — where the operator parked the timeline with a
	// scrub, a frame step or the end of a clip. ReplayChannel::positionNs() is
	// only the last frame it pushed, which stops agreeing with the bar the
	// moment a review runs out, and marking there marks somewhere he is not
	// looking. It stays as the fallback for a playhead that was never set.
	if (playheadNs_ != kNoInstant)
		return playheadNs_;
	return chan().positionNs();
}

bool MultiReplayDock::markable(int64_t tNs)
{
	// "No instant at all": the tap has captured nothing on this angle (not
	// recording, or the angle has no Branch Output filter running) and no clip
	// has played, so there is no playhead either. EventStore would happily
	// store that as a mark, producing an event that looks real in the list and
	// can never be played back, because no footage covers it. Refusing is the
	// honest answer, and saying so is better than a row nobody can explain.
	//
	// Both spellings of "none" are refused: the ring reports 0 when it is
	// empty, the dock's own playhead reports kNoInstant. Every other value is
	// a real instant — INCLUDING a negative one, which is what footage older
	// than the machine's last boot looks like.
	if (tNs != 0 && tNs != kNoInstant)
		return true;
	showNotice(obs_module_text("Dock.NothingToMark"));
	return false;
}

std::vector<int> MultiReplayDock::selectedEventIds() const
{
	std::vector<int> ids;
	auto rows = events_->selectionModel()->selectedRows();
	for (const auto &idx : rows) {
		QTableWidgetItem *it = events_->item(idx.row(), 0);
		if (!it)
			continue;
		// Skip non-event rows (e.g. session dividers carry no UserRole id,
		// so toInt() yields 0): never inject a spurious id 0 into play/edit.
		int id = it->data(Qt::UserRole).toInt();
		if (id > 0)
			ids.push_back(id);
	}
	return ids;
}

namespace {

// OBS fires hotkey callbacks on the hotkey thread. Everything the dock does
// touches widgets, so the work is posted to the GUI thread with the dock itself
// as the context object: if the dock is gone by then, ~QObject has already
// dropped the posted event and nothing runs on a dead pointer.
void onDockHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *ctx = static_cast<MultiReplayDock::HotkeyCtx *>(data);
	if (!ctx || !ctx->dock)
		return;
	QMetaObject::invokeMethod(
		ctx->dock, [ctx]() { ctx->fn(ctx->dock); },
		Qt::QueuedConnection);
}

} // namespace

void MultiReplayDock::registerDockHotkeys()
{
	struct Def {
		const char *name;
		const char *locale;
		void (*fn)(MultiReplayDock *);
	};

	static const Def kDefs[] = {
		// The play button, not "the last event": what an operator has
		// selected is what he means, and the table auto-selects the mark
		// he has just taken.
		{"ReplayPlaySelected", "Hotkey.PlaySelected",
		 [](MultiReplayDock *d) { d->playSelected(); }},
		{"ReplayStepForward", "Hotkey.StepForward",
		 [](MultiReplayDock *d) { d->stepFrameForward(); }},
		// Both frame keys, because finding the right frame means going
		// past it and coming back — and that is held-key work on a
		// Stream Deck, watching the picture rather than the panel.
		{"ReplayStepBackward", "Hotkey.StepBackward",
		 [](MultiReplayDock *d) { d->stepFrameBackward(); }},
		{"ReplayPlayReverse", "Hotkey.PlayReverse",
		 [](MultiReplayDock *d) { d->playSelectedReverse(); }},
		{"ReplayMarkCancel", "Hotkey.MarkCancel",
		 [](MultiReplayDock *d) {
			 EventStore::instance().markCancel();
			 d->refreshEvents();
		 }},
		// Mute / unmute the replay audio in the OBS mixer. Calls the key
		// of the same name, so a Stream Deck runs the same path as a
		// click — and the mute latches: nothing but this clears it.
		{"ReplayMuteAudio", "Hotkey.MuteAudio",
		 [](MultiReplayDock *d) {
			 if (d->muteBtn_)
				 d->muteBtn_->toggle();
		 }},
		// Speed presets: the same path as the chips, so they re-cue the
		// current clip at the new speed instead of only changing a number.
		{"ReplaySpeed25", "Hotkey.Speed25",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(25); }},
		{"ReplaySpeed50", "Hotkey.Speed50",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(50); }},
		{"ReplaySpeed75", "Hotkey.Speed75",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(75); }},
		{"ReplaySpeed100", "Hotkey.Speed100",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(100); }},
		{"ReplayFastForward", "Hotkey.FastForward",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(200); }},
		// 20 lists and no way to change list without the mouse was the
		// gap; two relative steps beat twenty absolute hotkeys.
		{"ReplayPrevList", "Hotkey.PrevList",
		 [](MultiReplayDock *d) { d->stepList(-1); }},
		{"ReplayNextList", "Hotkey.NextList",
		 [](MultiReplayDock *d) { d->stepList(+1); }},
		// Trimming a mark that was taken late, one frame at a time. On
		// keys rather than on the panel because this is Stream Deck work:
		// the operator holds a key and watches the picture, which is the
		// only way to find the right frame.
		{"ReplayTrimInBack", "Hotkey.TrimInBack",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(true, -1); }},
		{"ReplayTrimInForward", "Hotkey.TrimInForward",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(true, +1); }},
		{"ReplayTrimOutBack", "Hotkey.TrimOutBack",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(false, -1); }},
		{"ReplayTrimOutForward", "Hotkey.TrimOutForward",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(false, +1); }},
		// ...and the same two points straight to the position bar.
		{"ReplaySetInHere", "Hotkey.SetInHere",
		 [](MultiReplayDock *d) { d->setSelectedPoint(true); }},
		{"ReplaySetOutHere", "Hotkey.SetOutHere",
		 [](MultiReplayDock *d) { d->setSelectedPoint(false); }},
		// Whole SECONDS, which is how an operator describes the problem:
		// "the mark is two seconds late". A frame key pressed sixty times
		// is not an answer to that, and on a Stream Deck it is not even
		// possible.
		{"ReplayTrimInBack1s", "Hotkey.TrimInBack1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, -1'000'000'000LL);
		 }},
		{"ReplayTrimInFwd1s", "Hotkey.TrimInFwd1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, 1'000'000'000LL);
		 }},
		{"ReplayTrimOutBack1s", "Hotkey.TrimOutBack1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(false, -1'000'000'000LL);
		 }},
		{"ReplayTrimOutFwd1s", "Hotkey.TrimOutFwd1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(false, 1'000'000'000LL);
		 }},
		{"ReplayTrimInBack5s", "Hotkey.TrimInBack5s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, -5'000'000'000LL);
		 }},
		{"ReplayTrimInFwd5s", "Hotkey.TrimInFwd5s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, 5'000'000'000LL);
		 }},
	};

	for (const Def &def : kDefs) {
		auto ctx = std::make_unique<HotkeyCtx>();
		ctx->dock = this;
		ctx->fn = def.fn;
		hotkeys_.push_back(obs_hotkey_register_frontend(
			def.name, obs_module_text(def.locale), onDockHotkey,
			ctx.get()));
		hotkeyCtx_.push_back(std::move(ctx));
	}
}

void MultiReplayDock::stepList(int delta)
{
	if (!listTabs_)
		return;
	// Only over the lists that are actually SHOWN: stepping onto a hidden tab
	// would leave the table on a list with no tab lit.
	const int shown =
		std::clamp(ReplayCore::instance().getConfig().eventListCount, 1,
			   std::min(kEventLists, listTabs_->count()));
	const int next =
		std::clamp(listTabs_->currentIndex() + delta, 0, shown - 1);
	listTabs_->setCurrentIndex(next); // its signal selects the list + refreshes
}

void MultiReplayDock::moveSelectedEvent(int delta)
{
	const auto ids = selectedEventIds();
	if (ids.empty()) {
		showNotice(obs_module_text("Dock.SelectToReorder"));
		return;
	}

	// Manual order and the chronological auto-sort cannot both be in force:
	// with the sort on, the operator would move a row and watch it snap
	// straight back, with nothing to tell him why. So reordering by hand
	// TURNS THE SORT OFF, and says so — the alternative (refusing) leaves him
	// hunting through Settings for a switch he does not know exists.
	auto &core = ReplayCore::instance();
	Config cfg = core.getConfig();
	if (cfg.sortEventsByTime) {
		cfg.sortEventsByTime = false;
		core.setConfig(cfg);
		showNotice(obs_module_text("Dock.ManualOrderOn"));
	}

	// One at a time: moving a multi-selection by one place has no meaning the
	// operator could predict.
	if (!EventStore::instance().moveEvent(ids.front(), delta))
		showNotice(obs_module_text("Dock.CannotMoveFurther"));
	refreshEvents();
}

bool MultiReplayDock::playOnTargets(const std::vector<int> &ids,
				    ReplayChannel::Direction direction,
				    std::string &errorOut)
{
	// EVERY bay the selector points at, which under A|B is both — that is what
	// A|B promises and it was not being kept: play went to the active channel
	// only, so A|B behaved exactly like A and the second bay sat idle behind a
	// lit key.
	//
	// "To output" is the one thing that CANNOT go to both, because Program is
	// one scene. So it goes to the bay the operator nominated in Settings
	// (Config.abOutputUsesB) and the other bay plays without touching Program.
	const bool toOut = toOutputBtn_ && toOutputBtn_->isChecked();
	// Playing EVENTS is talking about events, so an unmarked stretch armed on
	// the bar stops being what the play keys mean. (playSelected() answers the
	// armed review before it ever gets here; this catches ◀ and the hotkeys,
	// which go straight to the events.)
	clearFreeReview();
	const bool useB = ReplayCore::instance().getConfig().abOutputUsesB;
	const auto targets = targetChannels();
	bool any = false;
	for (Which w : targets) {
		const bool thisOne =
			toOut && (targets.size() == 1 ||
				  (useB ? w == Which::B : w == Which::A));
		std::string err;
		if (PlaybackCoordinator::instance(w).playEvents(
			    ids, angle1_[(int)w] - 1, thisOne, err,
			    PlaybackCoordinator::AngleMode::AllEnabled,
			    direction))
			any = true;
		else if (errorOut.empty())
			errorOut = err;
	}
	return any;
}

// ---------------------------------------------------------------------------
// Unmarked footage: watching it, and then deciding to air it
// ---------------------------------------------------------------------------

void MultiReplayDock::clearFreeReview()
{
	freeReviewInNs_ = kNoInstant;
	freeReviewOnAir_ = false;
}

bool MultiReplayDock::playheadIsFreeFootage() const
{
	// Following live there is no "here" to play: the bar is at the front and
	// the footage under it is the instant that has just happened. ▶ keeps
	// meaning what it has always meant.
	if (ReplayCore::instance().followLive())
		return false;
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0)
		return false;

	// Asked of the SELECTION, not of the whole list. The table auto-selects
	// the newest mark, so "is there an event somewhere near here" would answer
	// yes for the whole match; the question that decides what ▶ does is
	// whether the bar is standing on the event ▶ would otherwise replay.
	std::vector<int> ids = selectedEventIds();
	if (ids.empty())
		ids = {EventStore::instance().lastEventId()};
	if (ids.empty() || ids.front() <= 0)
		return true; // nothing marked at all: it is all free footage
	ReplayEvent ev;
	if (!EventStore::instance().get(ids.front(), ev) ||
	    ev.tInNs == kNoInstant)
		return true;
	const int64_t outNs =
		(ev.tOutNs != kNoInstant && ev.tOutNs > ev.tInNs) ? ev.tOutNs
								  : ev.tInNs;
	// Half a second of slack at both ends. Cueing a row parks the bar ON the
	// in-point and the engine's own frame instants drift by a frame or two, so
	// an exact comparison would call the cued event "free footage" about as
	// often as not — and ▶ would then play something else than the row the
	// operator is looking at.
	constexpr int64_t kSlackNs = 500'000'000LL;
	return playheadNs_ < ev.tInNs - kSlackNs ||
	       playheadNs_ > outNs + kSlackNs;
}

bool MultiReplayDock::playFreeReview(bool toOutput)
{
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return false;
	}
	// From the bar to the end of the footage, cut at the seams and CHUNKED.
	//
	// One range would be wrong twice over. It would include the gaps between
	// takes, which are not footage and which the engine refuses outright (the
	// same reason a scrub review is split — see seekToFraction); and a single
	// range of "everything from here" would ask the engine to materialise a
	// whole session's packets in RAM. A queue of minute-long links is neither:
	// the coordinator chains them exactly as it chains the angles of an event,
	// and each link is about thirty megabytes.
	const int64_t inNs = playheadNs_;
	const int64_t edge = timelineStartNs_ + displayDurNs_;
	std::vector<std::pair<int64_t, int64_t>> ranges;
	const auto chop = [&](int64_t a, int64_t b) {
		for (int64_t t = a;
		     t < b && (int)ranges.size() < kFreeReviewMaxChunks;
		     t += kFreeReviewChunkNs)
			ranges.push_back({t, std::min(b, t + kFreeReviewChunkNs)});
	};
	if (timeline_.empty()) {
		if (edge > inNs)
			chop(inNs, edge);
	} else {
		for (const TimelineSpan &s : timeline_.spansFrom(
			     inNs, kFreeReviewChunkNs *
					   (int64_t)kFreeReviewMaxChunks))
			chop(s.startNs, s.endNs);
	}
	if (ranges.empty()) {
		showNotice(obs_module_text("Dock.NoFootageHere"));
		return false;
	}
	// §7.2.7 — SAID BEFORE IT HAPPENS, not discovered when the picture
	// just stops. Hitting the chunk cap exactly is indistinguishable here
	// from footage that happens to end on a chunk boundary, which is
	// close enough to never for continuously time-stamped packets that
	// treating "exactly the cap" as "capped" costs nothing in practice.
	// Queued (§7.3.8) behind the "reviewing" notice a few lines down, so
	// both reach the operator instead of the second erasing the first.
	const bool capped = (int)ranges.size() == kFreeReviewMaxChunks;

	auto &core = ReplayCore::instance();
	// Same discipline as a scrub: kill the queue first so its own finish
	// callback cannot cut in, and consume the transition so poll() does not
	// read the stop as "the sequence ended, go back to the live edge".
	pc().stopEvents();
	prevSequenceActive_ = false;
	core.setFollowLive(false);
	playheadNs_ = inNs;
	// ARMED, and it outlives the clip on purpose: the operator watches this
	// off air, presses Stop, decides, and then presses Play events — which has
	// to start from the instant he chose, not from wherever the review
	// happened to stop.
	freeReviewInNs_ = inNs;
	freeReviewOnAir_ = toOutput;

	std::string err;
	if (!pc().playRanges(ranges, currentAngle1() - 1, speedPct_, toOutput,
			     err)) {
		showNotice(QString("%1 — %2")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(QString::fromStdString(err)));
		obs_log(LOG_WARNING,
			"[dock] free review on angle %d at %lld ms refused: %s",
			currentAngle1(),
			(long long)((inNs - timelineStartNs_) / 1000000),
			err.c_str());
		return false;
	}
	obs_log(LOG_INFO,
		"[dock] free review: %zu chunk(s) from %lld ms on angle %d, %s%s",
		ranges.size(),
		(long long)((inNs - timelineStartNs_) / 1000000),
		currentAngle1(), toOutput ? "TO OUTPUT" : "off air",
		capped ? " (capped)" : "");
	// §7.2.7: said BEFORE the "reviewing" notice, so it is the one showing
	// while the operator's eye is still on the strip from the gesture he
	// just made — the "reviewing" notice queues behind it (§7.3.8) rather
	// than erasing it.
	if (capped) {
		const int64_t totalMs =
			kFreeReviewChunkNs * (int64_t)kFreeReviewMaxChunks /
			1'000'000LL;
		showNotice(QString(obs_module_text("Dock.FreeReviewCapped"))
				   .arg(totalMs / 60000));
	}
	showNotice(obs_module_text(toOutput ? "Dock.FreeReviewOnAir"
					    : "Dock.FreeReviewOffAir"));
	return true;
}

void MultiReplayDock::stopPlayback()
{
	// Every bay the selector points at, because that is what the selector
	// means everywhere else. stopEvents() puts the operator's previous scene
	// back in Program when the sequence had taken it.
	for (Which w : targetChannels())
		PlaybackCoordinator::instance(w).stopEvents();
}

void MultiReplayDock::playSelected()
{
	// THE SECOND FUNCTION OF THIS KEY.
	//
	// With a free review armed — the operator moved the bar onto footage
	// nobody marked and pressed ▶ — this key is what puts THAT on air. It is
	// the only way it can get there: ▶ deliberately plays it off air, so
	// reviewing an unmarked action can never reach Program by itself, and the
	// decision to air it stays a separate press.
	//
	// "In output" still governs, exactly as it does for an event: this key
	// does not have a private route to Program that the panel's own on-air
	// switch cannot see.
	if (freeReviewInNs_ != kNoInstant) {
		playheadNs_ = freeReviewInNs_;
		playFreeReview(toOutputBtn_ && toOutputBtn_->isChecked());
		return;
	}
	std::string err;
	if (!playOnTargets(selectedEventIds(),
			   ReplayChannel::Direction::Forward, err))
		showNotice(localizedError(err));
}

void MultiReplayDock::setAngle(int angle1Based)
{
	// The active channel's row. This is what the hotkeys and the numbered
	// keys mean by "the angle".
	setAngleOn(activeChannel_, angle1Based);
}

void MultiReplayDock::setAngleOn(Which which, int angle1Based)
{
	if (angle1Based < 1 || angle1Based > kNCams)
		return;
	angle1_[(int)which] = angle1Based;
	// Shared with the hotkeys, which have no way to reach the dock — and
	// only the ACTIVE channel's angle can be that shared one, because there
	// is one of it. Pressing B's camera 3 while the keys are on A sets B's
	// angle and leaves the hotkeys where they were, which is what the two
	// rows are for.
	if (which == activeChannel_)
		ReplayCore::instance().setCurrentAngle(angle1Based - 1);
	// AN ANGLE KEY CHANGES THE ANGLE. It does not start a replay.
	//
	// While something is on air it re-cues that clip on the chosen camera —
	// which IS "change the angle being watched", the same clip from the same
	// in-point on the other lens. But with nothing playing it only moves the
	// selection: pressing C2 to see what camera 2 is looking at used to put a
	// replay on air (and with "In output" on, on PROGRAM) that nobody had asked
	// for. Same rule the speed keys now follow.
	if (ReplayChannel::instance(which).playing() ||
	    PlaybackCoordinator::instance(which).queueActive())
		replayCurrentOn(which);
}

void MultiReplayDock::stepFrameForward()
{
	// the reference controller frame-by-frame forward.
	//
	// There is no playhead in the engine to advance: it plays RANGES. So a
	// step is the shortest range that can hold the next frame, played from one
	// frame past where the transport stands. The replay input keeps the last
	// frame it was handed, so when that tiny clip ends the stepped frame is
	// what stays on screen — which is exactly what a frame step is for.
	auto &core = ReplayCore::instance();
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return;
	}
	const int64_t edge = timelineStartNs_ + displayDurNs_;

	struct obs_video_info ovi = {};
	int64_t frameNs = 33333333; // 30 fps, if OBS will not say
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
		frameNs = (int64_t)((1000000000LL * (int64_t)ovi.fps_den) /
				    (int64_t)ovi.fps_num);

	const int64_t inNs = std::max(timelineStartNs_, playheadNs_ + frameNs);
	// Two frames wide, not one: the engine refuses a range it cannot serve
	// exactly, and a one-frame window that falls between two timestamps is
	// exactly that. Only the first frame is seen anyway.
	const int64_t outNs = inNs + 2 * frameNs;
	if (inNs >= edge) {
		// Already at the live edge — there is no next frame yet.
		showNotice(obs_module_text("Dock.AtLiveEdge"));
		return;
	}

	// Same discipline as a scrub: kill the queue first so its finish callback
	// cannot cut in, and consume the transition so poll() does not read the
	// stop as "the sequence ended, go back to live".
	pc().stopEvents();
	prevSequenceActive_ = false;
	core.setFollowLive(false);
	playheadNs_ = inNs;

	// The boxes step with it: a frame step is how an operator finds the exact
	// picture, and finding it on one lens while the others sit a second behind
	// defeats the comparison the strip is there for. Straight at the channel
	// again, so poll() has no queue to read this off.
	cueTiles(inNs, std::min(outNs, edge), 100,
		 ReplayChannel::Direction::Forward);

	std::string err;
	if (!chan().play(currentAngle1() - 1, inNs,
					    std::min(outNs, edge), 100, err))
		showNotice(QString("%1 — %2")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(QString::fromStdString(err)));
}

void MultiReplayDock::stepFrameBackward()
{
	// the reference controller frame-by-frame BACKWARDS, and it is not the forward step with the
	// sign flipped.
	//
	// A step is a range, because the engine plays ranges. Forwards, the range
	// [playhead + 1f, playhead + 3f] comes to rest on a frame past the bar, so
	// the picture moves on. Played FORWARDS, the range [playhead - 1f,
	// playhead + 1f] comes to rest on the frame at the playhead — the one
	// already on screen. The step would look broken while doing exactly what
	// it was told.
	//
	// So the step back is a REVERSE run: pictures come out newest-first, so
	// the run comes to rest on the OLDEST one it shows, and capping it at two
	// pictures makes that "one frame back" and nothing more. The window is
	// three frames wide because frame instants are not multiples of a frame
	// time — an encoder's own timestamps drift — and a window exactly one
	// frame wide can hold only the frame we are already looking at.
	auto &core = ReplayCore::instance();
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return;
	}

	struct obs_video_info ovi = {};
	int64_t frameNs = 33333333; // 30 fps, if OBS will not say
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
		frameNs = (int64_t)((1000000000LL * (int64_t)ovi.fps_den) /
				    (int64_t)ovi.fps_num);

	const int64_t outNs = playheadNs_;
	const int64_t inNs = outNs - 3 * frameNs;
	if (inNs < timelineStartNs_) {
		// The beginning of the footage: there is no earlier frame, and
		// clamping would silently show the first one again.
		showNotice(obs_module_text("Dock.AtTimelineStart"));
		return;
	}

	// Same discipline as the forward step: kill the queue first so its finish
	// callback cannot cut in, and consume the transition so poll() does not
	// read the stop as "the sequence ended, go back to live".
	pc().stopEvents();
	prevSequenceActive_ = false;
	core.setFollowLive(false);
	// An estimate, corrected by poll() from the frame the engine actually
	// pushes: the exact instant of "one frame back" belongs to the encoder's
	// timestamps, not to our arithmetic.
	playheadNs_ = outNs - frameNs;

	ReplayChannel::PlayRequest req;
	req.camIndex = currentAngle1() - 1;
	req.inNs = inNs;
	req.outNs = outNs;
	req.speedPct = 100;
	req.direction = ReplayChannel::Direction::Reverse;
	req.maxFrames = 2; // the frame on screen, then the one before it
	// The boxes step back with it (see stepFrameForward for why this is here
	// and not in poll()) — but FORWARDS over the same window, not backwards.
	// A tile is not the picture being trimmed: it only has to be on the moment
	// the operator is on, and playing this three-frame window forwards leaves
	// it there for the price of an ordinary decode. Reverse would be a GOP
	// decode per tile per key repeat, and this key is held down.
	cueTiles(inNs, outNs, 100, ReplayChannel::Direction::Forward);
	std::string err;
	if (!chan().play(req, err))
		showNotice(QString("%1 — %2")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(QString::fromStdString(err)));
}

void MultiReplayDock::playSelectedReverse()
{
	// the reference controller's ◀. The same queue, the same angles, the same "to output" — only
	// each clip runs from its OUT back to its IN. Going through the coordinator
	// rather than straight to the channel is what keeps that true: a reverse
	// that talked to the engine directly would be a second playback path, and
	// the first thing it would lose is the scene switch.
	std::vector<int> ids = selectedEventIds();
	if (ids.empty())
		ids = {EventStore::instance().lastEventId()};
	if (ids.empty() || ids.front() <= 0) {
		showNotice(obs_module_text("Dock.NothingSelected"));
		return;
	}
	std::string err;
	if (!playOnTargets(ids, ReplayChannel::Direction::Reverse, err))
		showNotice(localizedError(err));
}

void MultiReplayDock::applyReplaySpeed(int pct)
{
	// Default speed for every angle without an override — the coordinator
	// resolves it when it builds the queue, including for the hotkeys.
	speedPct_ = std::clamp(pct, 5, 200);
	for (Which w : targetChannels()) {
		PlaybackCoordinator::instance(w).setDefaultSpeedPct(speedPct_);
		// LIVE if something is playing: the clip carries on from the frame
		// on screen at the new spacing. It used to restart from the IN,
		// which threw away the very thing the operator was looking at — and
		// with the dial applying on every step of a drag, restarting would
		// have made the control unusable rather than merely annoying.
		PlaybackCoordinator::instance(w).setLiveSpeedPct(speedPct_);
	}
	// AND WHEN NOTHING IS PLAYING, IT ONLY SETS THE SPEED. Choosing 25% before
	// a replay is choosing how the next one will run, not asking for one now:
	// starting a clip off a preset key put footage on air (with "In output" on,
	// on PROGRAM) that nobody had asked to see. The number is armed and the
	// next Play uses it.
}

// ---------------------------------------------------------------------------
// The keyboard. ONE layer of input: every key calls the button of the same name.
// ---------------------------------------------------------------------------

bool MultiReplayDock::focusIsTextEntry()
{
	QWidget *fw = QApplication::focusWidget();
	if (!fw)
		return false;
	// An editable combo's field is a QLineEdit child, so this one cast covers
	// the per-angle comment cells as well as the search box — the same question
	// refreshEvents() asks before it rebuilds the table under an editor.
	return qobject_cast<QLineEdit *>(fw) ||
	       qobject_cast<QPlainTextEdit *>(fw) ||
	       qobject_cast<QAbstractSpinBox *>(fw);
}

bool MultiReplayDock::handleTransportKey(QKeyEvent *event)
{
	if (!event)
		return false;
	// Modified keys belong to OBS and to the operator's own shortcuts. Shift is
	// ours, and only as a MAGNITUDE: Shift+arrow is the same movement in a
	// bigger unit, never a different command.
	const Qt::KeyboardModifiers mods = event->modifiers();
	if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
		return false;
	// Typing wins, always. These are single-key commands sitting two pixels
	// from a search box and a column of comment cells.
	if (focusIsTextEntry())
		return false;

	const bool shift = mods.testFlag(Qt::ShiftModifier);
	switch (event->key()) {
	case Qt::Key_Left:
		if (shift)
			scrubBySeconds(-1.0);
		else
			stepFrameBackward();
		return true;
	case Qt::Key_Right:
		if (shift)
			scrubBySeconds(+1.0);
		else
			stepFrameForward();
		return true;
	case Qt::Key_Up:
		stepEventSelection(-1);
		return true;
	case Qt::Key_Down:
		stepEventSelection(+1);
		return true;
	// Both faces of each key: the one on the main row and the one on the
	// numeric pad, which is where a hand on a controller actually is.
	case Qt::Key_Plus:
	case Qt::Key_Equal:
		nudgeSpeed(+kSpeedKeyStepPct);
		return true;
	case Qt::Key_Minus:
	case Qt::Key_Underscore:
		nudgeSpeed(-kSpeedKeyStepPct);
		return true;
	// Esc leaves full screen, and ONLY that: while the panel is a window
	// inside OBS the key is not ours, and handing it back is what lets a
	// dialog or a popup keep it. A full-screen floating dock has no title bar
	// and therefore no close button, so this is the escape everyone reaches
	// for first — the ⛶ key on the row is the other one.
	case Qt::Key_Escape:
		if (!panelIsFullScreen())
			return false;
		setPanelFullScreen(false);
		refreshFullScreenKey();
		return true;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		playSelected();
		return true;
	// §7.3.11 — opt-in (Config.spacebarPlays): the eventFilter below
	// never forwards Space here while focus is on a button or the table
	// (it is the click / nothing there), so reaching this case at all
	// already means focus is on the bare panel — the one place Space did
	// nothing before this setting existed.
	case Qt::Key_Space:
		if (!ReplayCore::instance().getConfig().spacebarPlays)
			return false;
		playSelected();
		return true;
	default:
		return false;
	}
}

void MultiReplayDock::keyPressEvent(QKeyEvent *event)
{
	// Reached when the focused child ignored the key (a button, a label, the
	// panel itself). The table does NOT ignore arrows or Enter, so it is
	// filtered separately — see eventFilter.
	if (handleTransportKey(event)) {
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}

void MultiReplayDock::stepEventSelection(int delta)
{
	if (!events_ || events_->rowCount() == 0)
		return;
	const auto sel = events_->selectionModel()->selectedRows();
	const int rows = events_->rowCount();
	// With nothing selected, ↓ takes the first row and ↑ the last: the key says
	// which end of the list the operator is coming from.
	const int cur = sel.empty() ? (delta > 0 ? -1 : rows) : sel.first().row();
	const int next = std::clamp(cur + delta, 0, rows - 1);
	if (next == cur)
		return; // at the end of the list: refuse rather than wrap
	// This is the OPERATOR selecting, so it cues the event — which is the whole
	// point of walking the list with the keys (see cueSelected, and the
	// reselecting_ guard that keeps our own re-selection from cueing).
	events_->selectRow(next);
	if (QTableWidgetItem *it = events_->item(next, kColId))
		events_->scrollToItem(it);
}

void MultiReplayDock::scrubBySeconds(double seconds)
{
	// Along the FOOTAGE axis, not wall time: the bar is drawn over the recorded
	// spans joined end to end, so "one second earlier" has to mean one second of
	// material — a session with a pause in it would otherwise send the playhead
	// into a gap that no footage covers.
	if (timeline_.empty() || playheadNs_ == kNoInstant) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return;
	}
	const int64_t total = timeline_.totalNs();
	if (total <= 0)
		return;
	int64_t at = timeline_.footageBefore(playheadNs_) +
		     (int64_t)std::llround(seconds * 1e9);
	at = std::clamp(at, (int64_t)0, total);
	// Through seekToFraction, so a keyboard scrub is the same gesture as a
	// dragged one: it reviews from there, consumes the sequence transition and
	// says so when that instant holds no footage.
	seekToFraction((double)at / (double)total);
}

void MultiReplayDock::nudgeSpeed(int deltaPct)
{
	// The dial's own range, and applyReplaySpeed does the rest: it re-speeds the
	// clip on air without restarting it, and merely arms the number when nothing
	// is playing. poll() moves the slider to match, so the widget cannot end up
	// disagreeing with the speed.
	applyReplaySpeed(std::clamp(speedPct_ + deltaPct, 5, 200));
}

void MultiReplayDock::cueSelected()
{
	// Selecting a row SHOWS that event, on the channel the selector points at.
	// the reference controller loads the event into the selected bay the moment you pick it; here it
	// used to sit there until "Play events" was pressed, so the operator was
	// choosing clips blind.
	//
	// A cue is not a playback: two frames from the IN, which is the shortest
	// range the engine will serve, and it comes to rest on the first of them —
	// so the IN frame is what stays on screen. (There is no freeze-frame in the
	// engine: it plays ranges. A range of two is a still.)
	const auto ids = selectedEventIds();
	if (ids.empty() || ids.front() <= 0)
		return;
	ReplayEvent ev;
	if (!EventStore::instance().get(ids.front(), ev) || ev.tInNs == kNoInstant)
		return;

	struct obs_video_info ovi = {};
	int64_t frameNs = 33333333;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
		frameNs = (int64_t)((1000000000LL * (int64_t)ovi.fps_den) /
				    (int64_t)ovi.fps_num);

	auto &core = ReplayCore::instance();
	// Choosing a clip is reviewing, so the preview stops mirroring the camera —
	// otherwise the cue would land on a source nobody is looking at.
	core.setFollowLive(false);
	prevSequenceActive_ = false;
	playheadNs_ = ev.tInNs;
	// Picking a row is talking about THAT event, so whatever unmarked stretch
	// was armed on the bar is no longer what the play keys mean.
	clearFreeReview();

	// START THE FEEDS' FETCHES BEFORE THE BAY'S, AND LET THEM RUN UNDER IT.
	//
	// This is what makes the bay and the boxes arrive together, and the order
	// of these three steps IS the mechanism. A fetch that is not in the ring
	// is an open, a seek and a demux — about a tenth of a second — and play()
	// does it inline, on this thread. Three inline fetches in a row (bay, then
	// each feed) means somebody is a third of a second late by construction,
	// and which somebody depends only on who was called first: the boxes used
	// to be, so the BAY was the one that came in late.
	//
	// prefetch() is asynchronous and each feed has a thread of its own, so
	// asking them all first puts their reads in flight in PARALLEL; the bay
	// then does its own fetch inline, and by the time cueTiles() runs below
	// the feeds' clips are already in their caches and their play() starts at
	// once. Nothing waits on any of it — a prefetch that has not landed just
	// means play() fetches for itself, exactly as before.
	//
	// ONE RANGE FOR THE CUE AND THE PLAY, which is what makes this
	// predictable instead of merely fast on a good day. The cue asks for the
	// WHOLE event and stops on the first picture (PlayRequest::maxFrames), so
	// it leaves in the cache exactly the clip the play after it will ask for.
	// It used to ask for a two-frame range instead: a different range is a
	// different key, so the cue fetched, and then the play fetched the same
	// footage again — and whether that second read was a memcpy out of the
	// ring or an open-seek-demux off the disk is what made the wait feel
	// random.
	const int64_t cueOutNs =
		(ev.tOutNs != kNoInstant && ev.tOutNs > ev.tInNs)
			? ev.tOutNs
			: ev.tInNs + 2 * frameNs;
	ensureTileFeeds();
	prefetchTiles(ev.tInNs, cueOutNs, 100);

	for (Which w : targetChannels()) {
		auto &pcw = PlaybackCoordinator::instance(w);
		// A BAY THAT IS PLAYING IS LEFT ALONE. Selecting a row is
		// preparation for the next play, not a request to interrupt the
		// one on air — and interrupting it would mean stopEvents(), which
		// for a sequence started with "In output" puts the operator's
		// previous scene back in Program. A click in a list must never be
		// able to do that. (It is also how the reference controller behaves: you cue into the
		// bay that is not live, which is what two bays are for.)
		if (pcw.queueActive()) {
			obs_log(LOG_INFO,
				"[dock] cue %s: skipped, that bay is on air",
				channelLetter(w));
			continue;
		}
		ReplayChannel::PlayRequest req;
		req.camIndex = angle1_[(int)w] - 1;
		req.inNs = ev.tInNs;
		req.outNs = cueOutNs;
		req.speedPct = 100;
		// The still is a frame cap, not a short range — see cueOutNs. The
		// fetch this pays for is the one the play will want, so there is
		// no second prefetch here any more: it was a whole extra read of
		// the same footage, and the join it did on this thread was a UI
		// stall of up to a file read per bay.
		req.maxFrames = 2;
		std::string err;
		if (!ReplayChannel::instance(w).play(req, err))
			// Not a notice: a cue is a side effect of moving the
			// selection, and a row whose footage has gone is already
			// flagged with ⚠ in the table. Shouting on every arrow-key
			// press would be worse than silence.
			obs_log(LOG_INFO, "[dock] cue %s: event %d not playable: %s",
				channelLetter(w), ev.id, err.c_str());
	}

	// THE ANGLE BOXES SHOW THE CUE TOO — every lens, on the in-point.
	//
	// This is the gesture the multiview exists for: the operator picks a row
	// and looks along the strip to decide which camera saw it. Done here and
	// not from poll() because a cue never reaches the coordinator — it is two
	// frames played straight at the channel, so there is no queue for poll()
	// to read it off.
	//
	// AFTER the bay, on the clips the prefetch above has been reading while
	// the bay did its own: this play() is a cache hit, so the boxes light up
	// with it rather than a fetch apiece behind it. Same range, same frame
	// cap: one fetch per angle serves both this still and the replay that
	// follows it.
	cueTiles(ev.tInNs, cueOutNs, 100, ReplayChannel::Direction::Forward,
		 /*maxFrames*/ 2);
}

void MultiReplayDock::replayCurrent()
{
	replayCurrentOn(activeChannel_);
}

void MultiReplayDock::replayCurrentOn(Which which)
{
	// While following live the angle buttons only pick which camera the
	// preview mirrors; they must not start a replay. Once the operator plays
	// something (which clears follow-live) they re-cue it — including during
	// recording, which the ring makes possible and is the whole point.
	if (ReplayCore::instance().followLive())
		return;
	// A FREE REVIEW IS RE-CUED ON THE CHOSEN CAMERA, not abandoned for an
	// event. Changing the angle means "the same moment, on that lens" — which
	// is exactly as true of a stretch nobody marked as it is of a marked one,
	// and it is what the angle boxes have just been showing him. Jumping to
	// the selected event here would take the operator somewhere else on the
	// timeline for pressing a camera key.
	if (freeReviewInNs_ != kNoInstant) {
		// Once, on the active bay: a free review is a range, and playing
		// one range on both bays is two decoders showing one picture.
		if (which == activeChannel_) {
			playheadNs_ = freeReviewInNs_;
			playFreeReview(freeReviewOnAir_);
		}
		return;
	}
	auto &pc = PlaybackCoordinator::instance(which);
	std::string err;
	std::vector<int> ids = selectedEventIds();
	bool toOut = toOutputBtn_ && toOutputBtn_->isChecked();
	// THIS channel's angle, not the active one's: pressing B's camera 3
	// re-cues B on camera 3 even while the transport keys are on A.
	int a0 = angle1_[(int)which] - 1;
	if (ids.empty())
		ids = {EventStore::instance().lastEventId()};
	if (ids.empty() || ids.front() <= 0)
		return;
	// Single angle on purpose: this is the re-cue behind the angle buttons and
	// the speed slider, so it shows the camera the operator just picked. The
	// play buttons go through the default (every enabled angle in sequence).
	//
	// And when that camera cannot be played, SAY SO. It used to fall through to
	// some other angle without a word, so pressing "2" played camera 1 and the
	// operator was left deducing the angle model from what he heard.
	if (!pc.playEvents(ids, a0, toOut, err,
			   PlaybackCoordinator::AngleMode::Single))
		showNotice(localizedError(err));
}

void MultiReplayDock::setSearchText(const QString &text)
{
	if (search_)
		search_->setText(text); // textChanged does the rest
}

void MultiReplayDock::zoomWholeTimeline()
{
	if (seek_)
		seek_->setZoom(1.0, 0.5);
}

void MultiReplayDock::showZoomMenu()
{
	QMenu menu(this);
	// "The whole thing" first, because it is the way back and the way back has
	// to be the shortest gesture. Then spans, longest to shortest.
	struct Entry {
		const char *label;
		int64_t spanNs; // 0 = the whole timeline
	};
	static const Entry kEntries[] = {
		{"100%", 0},
		{"1 h", 3600'000'000'000LL},
		{"30 min", 1800'000'000'000LL},
		{"10 min", 600'000'000'000LL},
		{"5 min", 300'000'000'000LL},
		{"1 min", 60'000'000'000LL},
	};

	const int64_t total = displayDurNs_;
	for (const Entry &e : kEntries) {
		QAction *a = menu.addAction(QString::fromUtf8(e.label));
		a->setCheckable(true);
		// A span longer than the session is the whole session; offering it
		// as a separate choice that does nothing would be a lie about the
		// control.
		const double want = (e.spanNs <= 0 || total <= 0 ||
				     e.spanNs >= total)
					    ? 1.0
					    : (double)total / (double)e.spanNs;
		const bool usable = e.spanNs <= 0 || total > e.spanNs;
		a->setEnabled(usable);
		// Only a usable entry can be the one in force. A span longer than
		// the session collapses to "the whole timeline", so on a two-minute
		// project "5 min" computed the same factor as "100%" and came up
		// greyed out AND ticked — an entry claiming to be the current view
		// while refusing to be clicked.
		a->setChecked(usable && std::abs(want - seek_->zoom()) < 0.01);
		const int64_t span = e.spanNs;
		connect(a, &QAction::triggered, this, [this, want, span]() {
			if (span <= 0) {
				zoomWholeTimeline();
				return;
			}
			// Centred on the playhead, not on the middle of the
			// timeline: the operator asked to see five minutes, and the
			// five minutes he means are the ones around where he is.
			double centre = 0.5;
			if (!timeline_.empty() && playheadNs_ != kNoInstant &&
			    displayDurNs_ > 0)
				centre = std::clamp(
					(double)timeline_.footageBefore(playheadNs_) /
						(double)displayDurNs_,
					0.0, 1.0);
			seek_->setZoom(want, centre);
			obs_log(LOG_INFO,
				"[dock] seek zoom: %s → %.2f× (centre %.3f)",
				span > 0 ? "span" : "whole timeline", want, centre);
		});
	}
	// §6.6: anchored off the bar itself now, at the badge's own corner —
	// there is no zoomBtn_ any more to anchor off.
	menu.exec(seek_->mapToGlobal(QPoint(seek_->width(), seek_->height())));
}

// §7.3.10 — DELETE OVER A THRESHOLD ASKS FIRST. Only DeleteAll used to: a
// handful of events under one right click had no confirmation at all, and
// "a handful" turned out to mean whatever was selected, no matter how much
// of the marked footage that was. Two thresholds, either one enough to ask —
// a long list of short events and a short list of long ones are the same
// mistake from two different directions.
bool MultiReplayDock::confirmDelete(const std::vector<int> &ids)
{
	constexpr size_t kConfirmCount = 3;
	constexpr int64_t kConfirmTotalNs = 30'000'000'000LL; // 30 s
	int64_t totalNs = 0;
	auto &store = EventStore::instance();
	for (int id : ids) {
		ReplayEvent ev;
		if (store.get(id, ev) && ev.tOutNs > ev.tInNs)
			totalNs += ev.tOutNs - ev.tInNs;
	}
	if (ids.size() <= kConfirmCount && totalNs <= kConfirmTotalNs)
		return true;
	// The buttons are OURS, not Qt's: QMessageBox::Yes/No follow Qt's own
	// locale, not OBS's, same reason DeleteAllConfirm already builds its
	// own (see that connection, a few lines up in buildExportBlock).
	QMessageBox box(this);
	box.setWindowTitle("obs-multireplay");
	box.setText(QString(obs_module_text("Dock.DeleteConfirm"))
			    .arg(ids.size())
			    .arg(formatTc(totalNs)));
	QPushButton *yes =
		box.addButton(obs_module_text("Dock.Yes"), QMessageBox::YesRole);
	box.addButton(obs_module_text("Dock.No"), QMessageBox::NoRole);
	box.exec();
	return box.clickedButton() == yes;
}

void MultiReplayDock::showNotice(const QString &text)
{
	// Shown on the STATUS LINE, which owns it for a few seconds (see
	// updateChannelStrip). It runs the width of the panel, it is next to the
	// modes the sentence is usually about, and it is the one place left that
	// can hold a sentence at all now that the three-line channel band is gone.
	//
	// §7.3.8 — QUEUED, not overwritten, when the line is already showing
	// something: a mark rejected followed within the same tick by a skip
	// used to replace the rejection before the operator's eye had reached
	// the strip, so he never learned about it. updateChannelStrip()
	// (dock-poll.cpp) pops the next one once the current message's window
	// closes, in the order they happened. Capped (kNoticeQueueMax): past
	// that, a NEW notice is the one dropped, not one already waiting — a
	// bug that calls this in a loop must not grow the backlog forever, and
	// dropping the tail keeps what does get shown in the order it occurred
	// instead of reordering causes and effects.
	const int64_t now = (int64_t)os_gettime_ns();
	if (noticeUntilNs_ > 0 && now < noticeUntilNs_) {
		if (noticeQueue_.size() < kNoticeQueueMax)
			noticeQueue_.push_back(text);
		return;
	}
	noticeText_ = text;
	noticeUntilNs_ = now + kNoticeNs;
	updateChannelStrip();
}

void MultiReplayDock::showHealthDetails()
{
	const auto findings = HealthMonitor::instance().findings();
	if (findings.empty())
		return;
	// A message box, not a modal that blocks the take: the take is running,
	// the operator opened this on purpose, and closing it changes nothing.
	// Every finding, in full, with the numbers that produced it — the badge
	// only has room for a count.
	QMessageBox::information(
		this, obs_module_text("Dock.HealthTitle"),
		QString::fromStdString(
			findingsBlock(findings, health::Level::Info)));
}

// ---------------------------------------------------------------------------
// Trimming an event that has already been marked
// ---------------------------------------------------------------------------

void MultiReplayDock::setSelectedPoint(bool inPoint)
{
	const auto ids = selectedEventIds();
	if (ids.empty()) {
		showNotice(obs_module_text("Dock.TrimNoSelection"));
		return;
	}
	if (playheadNs_ == kNoInstant) {
		showNotice(obs_module_text("Dock.TrimNoPosition"));
		return;
	}
	ReplayEvent ev;
	if (!EventStore::instance().get(ids.front(), ev))
		return;
	const int64_t from = inPoint ? ev.tInNs : ev.tOutNs;
	if (from == kNoInstant) {
		// An open event has no OUT to move; marking one is Mark Out's
		// job, and doing it from here would hide which key did what.
		showNotice(obs_module_text("Dock.TrimNoPoint"));
		return;
	}
	// Expressed as a delta so the store's own clamping (an IN cannot pass
	// its OUT, and the other way round) is the one that decides — there is
	// exactly one copy of that rule and it is already unit-tested.
	if (!EventStore::instance().movePoint(ids.front(), inPoint,
					      playheadNs_ - from)) {
		showNotice(obs_module_text("Dock.TrimRefused"));
		return;
	}
	obs_log(LOG_INFO, "[dock] trim: event %d %s moved by %lld ms",
		ids.front(), inPoint ? "IN" : "OUT",
		(long long)((playheadNs_ - from) / 1'000'000));
	refreshEvents();
}

void MultiReplayDock::onMarkerDragged(int eventId, bool inPoint, double frac)
{
	if (timeline_.empty())
		return;
	ReplayEvent ev;
	if (!EventStore::instance().get(eventId, ev))
		return;
	const int64_t want = timeline_.instantAt(frac);
	const int64_t from = inPoint ? ev.tInNs : ev.tOutNs;
	if (from == kNoInstant)
		return;
	// A delta, like every other way of moving a point: the store owns the
	// rule that an IN cannot pass its OUT, and it is tested in one place.
	if (!EventStore::instance().movePoint(eventId, inPoint, want - from)) {
		showNotice(obs_module_text("Dock.TrimRefused"));
		return;
	}
	obs_log(LOG_INFO, "[dock] dragged event %d's %s by %lld ms", eventId,
		inPoint ? "IN" : "OUT", (long long)((want - from) / 1'000'000));
	refreshEvents();
}

void MultiReplayDock::nudgeSelectedPointNs(bool inPoint, int64_t deltaNs)
{
	const auto ids = selectedEventIds();
	if (ids.empty()) {
		showNotice(obs_module_text("Dock.TrimNoSelection"));
		return;
	}
	if (EventStore::instance().movePoint(ids.front(), inPoint, deltaNs))
		refreshEvents();
	else
		showNotice(obs_module_text("Dock.TrimRefused"));
}

void MultiReplayDock::nudgeSelectedPoint(bool inPoint, int frames)
{
	const auto ids = selectedEventIds();
	if (ids.empty())
		return;
	obs_video_info ovi{};
	const double fps = obs_get_video_info(&ovi) && ovi.fps_den
				   ? (double)ovi.fps_num / (double)ovi.fps_den
				   : 30.0;
	const int64_t frameNs = (int64_t)(1'000'000'000.0 / std::max(1.0, fps));
	if (EventStore::instance().movePoint(ids.front(), inPoint,
					     frameNs * frames))
		refreshEvents();
}

// ---------------------------------------------------------------------------
// Which channel the controls drive (the reference controller's A|B / A / B)
// ---------------------------------------------------------------------------

PlaybackCoordinator &MultiReplayDock::pc() const
{
	return PlaybackCoordinator::instance(activeChannel_);
}

ReplayChannel &MultiReplayDock::chan() const
{
	return ReplayChannel::instance(activeChannel_);
}

std::vector<Which> MultiReplayDock::targetChannels() const
{
	// A|B is not "A and also B sometimes": it is one command going to both,
	// which is the whole reason a two-channel panel is one panel.
	//
	// With B switched off in Settings there is one bay and it is A — checked
	// here rather than trusted to the selector being hidden, because a hotkey
	// or a Stream Deck reaches this too and neither of them can see a widget.
	if (!channelBEnabled_)
		return {Which::A};
	if (linkedAB_)
		return {Which::A, Which::B};
	return {activeChannel_};
}

void MultiReplayDock::setActiveChannel(Which which, bool linked)
{
	activeChannel_ = which;
	linkedAB_ = linked;
	// PUSH THIS CHANNEL'S OWN ANGLE INTO THE SHARED VALUE, before poll()
	// below (or the next tick) copies that shared value back onto
	// angle1_[activeChannel_] — see angle-channels.hpp. Without this, a
	// bare bay switch made the copy run backwards: switch to B, prepare
	// it on some angle with a hotkey, switch to A and touch its angle,
	// switch back to B, and the very next tick silently reset B's angle
	// to whatever A had just left in the shared value. Two bays exist so
	// one can be lined up while the other is on air; an angle that does
	// not survive being looked away from defeats that.
	ReplayCore::instance().setCurrentAngle(
		angle_channels::sharedAngleOnActivate(angle1_[(int)which]));
	// The preview, the badge and the transport all read these on the next
	// tick; nothing is stopped or started by CHOOSING a channel, because in
	// the reference controller the selector picks where the keys go, not what is on air.
	previewCam0_ = -1; // force the preview source to be re-resolved
	// The letters under the two boxes say which one the keys are driving —
	// both lit under A|B, because under A|B they are.
	if (labelA_ && labelB_) {
		labelA_->setProperty("active", linked || which == Which::A);
		labelB_->setProperty("active", linked || which == Which::B);
		repolish(labelA_);
		repolish(labelB_);
	}
	// (There was a badge here spelling out "A|B 3". The two letters under the
	// pictures already say which bay the keys drive, and they say it where
	// the operator is looking; the angle is said by the tally on the camera
	// whose picture is green.)
	poll();
}

void MultiReplayDock::swapChannels()
{
	// ⇄ — what A is playing goes to B and B's goes to A. Each channel is
	// asked to play the OTHER's clip from its IN: there is no way to hand a
	// half-played clip across, and starting from the top is what an operator
	// means by "put that on the other channel".
	auto &pa = PlaybackCoordinator::instance(Which::A);
	auto &pb = PlaybackCoordinator::instance(Which::B);
	const auto sa = pa.playState();
	const auto sb = pb.playState();
	if (!sa.active && !sb.active) {
		showNotice(obs_module_text("Dock.NothingQueued"));
		return;
	}
	pa.stopEvents();
	pb.stopEvents();
	std::string err;
	// B takes what A had, A takes what B had. Angles come across with the
	// clips: the point of the swap is to keep watching the same two things
	// the other way round.
	if (sa.active && sa.eventId > 0)
		pb.playEvents({sa.eventId}, sa.angle1 - 1, false, err,
			      PlaybackCoordinator::AngleMode::Single);
	if (sb.active && sb.eventId > 0)
		pa.playEvents({sb.eventId}, sb.angle1 - 1, false, err,
			      PlaybackCoordinator::AngleMode::Single);
	obs_log(LOG_INFO, "[dock] swap: A(event %d angle %d) <-> B(event %d angle %d)",
		sa.eventId, sa.angle1, sb.eventId, sb.angle1);
}

void MultiReplayDock::centreComboItems(QComboBox *cb)
{
	// Both dimensions, because they fail for different reasons. Horizontally,
	// item text is drawn by the view's DELEGATE and no stylesheet reaches it —
	// only the role does. Vertically, AlignVCenter is what stops the text
	// sitting at the bottom of a row that the stylesheet has made taller than
	// the glyphs.
	if (!cb)
		return;
	for (int i = 0; i < cb->count(); i++)
		cb->setItemData(i, (int)(Qt::AlignCenter),
				Qt::TextAlignmentRole);
}

// Write the three values into a cell that is ALREADY about this event and this
// angle, and say whether that was possible.
//
// This is the fast path of the event table, and it exists because the slow one
// was measured: rebuilding a row of two angle cells cost 50-180 ms, nearly all
// of it Qt resolving the dock's style sheet for the five widgets a cell is made
// of, and poll() did it on every bump of the store's version — every mark,
// every trim, every checkbox. The ordinary case never needs it: the rows are
// the same events in the same order and only their values moved.
//
// It refuses, rather than adapting, in the three cases where a cell is not
// interchangeable: a different event or angle (the signal handlers captured
// those), and a comment vocabulary newer than the one the cell was built with.
// Refusing costs a rebuild, which is exactly what used to happen every time.
//
// refreshing_ is true throughout, so the handlers below early-out and none of
// this reaches the store.

bool MultiReplayDock::updateAngleCell(QWidget *cell, int eventId, int cam0,
				      bool on, double speed)
{
	if (!cell)
		return false;
	// No vocabulary version here any more: an angle cell holds no words, so
	// a new tag cannot make it stale. That belongs to the comment cell.
	if (cell->property("mrEventId").toInt() != eventId ||
	    cell->property("mrCam").toInt() != cam0)
		return false;

	auto *box = cell->findChild<QCheckBox *>();
	auto *sp = cell->findChild<QPushButton *>(QStringLiteral("mrAngleSpeed"));
	if (!box || !sp)
		return false;

	if (box->isChecked() != on)
		box->setChecked(on);

	const int pct = speed >= 0 ? (int)std::lround(speed * 100.0) : -1;
	if (sp->property("mrPct").toInt() != pct) {
		sp->setProperty("mrPct", pct);
		sp->setText(pct > 0 ? QString("%1%").arg(pct)
				    : QStringLiteral("--"));
	}
	const bool noOverride = pct <= 0;
	if (sp->property("mrNoOverride").toBool() != noOverride) {
		sp->setProperty("mrNoOverride", noOverride);
		repolish(sp);
	}

	return true;
}

// `presets` is passed IN, and that is the whole of a 175 ms fix.
//
// This used to call ReplayCore::getConfig() for its comment list — once per
// cell, so once per event per camera. getConfig() copies the entire Config
// (eight camera slots, every string, the preset vector) and takes the core
// mutex to do it, and the core mutex is held by startRecording and setConfig.
// On a real 22-minute session the dock's own phase accounting put
// refreshEvents at 120-175 ms, every time, as the longest thing poll() did;
// with eight cameras and a match's worth of marks it is that multiplied by
// forty. The list is the same for every cell on the panel, so it is read once
// and handed down.
// ---------------------------------------------------------------------------
// The event's comment — one cell, one click, two ways in
// ---------------------------------------------------------------------------
//
// THIS IS THE FOURTH ARRANGEMENT OF THIS COLUMN, so the constraints are worth
// stating once, together, because each previous version satisfied some of them
// and broke another:
//
//   1. AT REST IT IS TEXT. Sixty rows of framed control say what sixty words
//      say, and set the row height for the whole list while they do it.
//   2. ONE CLICK. Both to type and to choose — a delegate on a double click
//      looked right and was WRONG here, because a double click on a row of this
//      table already means "put this event on air", and with "in output" on
//      that reaches the Program. Editing a comment must never be able to do
//      that, and a widget in the cell is what guarantees it: the table never
//      sees the click at all.
//   3. THE VOCABULARY IS VISIBLE. Hiding it behind the right button, as the
//      previous version did, is hiding it: nobody right-clicks a word to find
//      out that a list of words exists.
//
// So: a flat line edit that takes a caret on the first click, and beside it a
// chooser that opens the list on ITS first click. Neither has a frame until the
// pointer is over the cell, which is where a frame says something.
QWidget *MultiReplayDock::buildNoteCell(int eventId, const std::string &note,
					const std::vector<std::string> &presets)
{
	// PARENTLESS until setCellWidget takes it: the dock's style sheet is
	// resolved for a widget the moment it is given a parent inside the dock,
	// so building the pair detached polishes the subtree once.
	auto *w = new QWidget;
	w->setObjectName(QStringLiteral("mrNoteCell"));
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(2, 0, 0, 0);
	h->setSpacing(0);

	auto *cm = new QLineEdit(w);
	cm->setObjectName(QStringLiteral("mrAngleNote"));
	cm->setToolTip(obs_module_text("Dock.CamNoteHint"));
	cm->setPlaceholderText(kNoNote);
	cm->setAlignment(Qt::AlignCenter);
	cm->setFrame(false);
	// NO CONTEXT MENU. Cut/copy/paste on a one-word field is three entries
	// nobody came for, and the vocabulary - the only list worth offering here -
	// is on the chooser beside it where it can be seen.
	cm->setContextMenuPolicy(Qt::NoContextMenu);
	h->addWidget(cm, 1);

	// The chooser. A glyph rather than a word: it is 14 px wide and the words
	// it offers are in the list it opens.
	auto *pick = new QPushButton(w);
	pick->setObjectName(QStringLiteral("mrNotePick"));
	pick->setToolTip(obs_module_text("Dock.CamNoteHint"));
	pick->setCursor(Qt::PointingHandCursor);
	pick->setFocusPolicy(Qt::NoFocus);
	pick->setFixedWidth(14);
	setKeyIcon(pick, Icon::More, tintsFor(sc()), 10);
	h->addWidget(pick, 0);

	w->setProperty("mrEventId", eventId);
	// A cell records the vocabulary it was built with: a word typed on another
	// event makes this one stale, because its list would be missing it.
	w->setProperty("mrVocab", (qulonglong)commentVocabVersion_);

	connect(pick, &QPushButton::clicked, this, [this, cm, presets, pick]() {
		if (refreshing_)
			return;
		QMenu m(pick);
		QStringList words;
		for (const auto &p : presets)
			words << QString::fromStdString(p);
		for (const QString &s : sessionComments_)
			if (!words.contains(s))
				words << s;
		// "No comment" is a choice and not a gap: an operator who marked
		// the wrong thing needs a way back to blank that is not select-all
		// and delete.
		QAction *none = m.addAction(kNoNote);
		connect(none, &QAction::triggered, cm,
			[cm]() { cm->setText(QString()); });
		if (!words.isEmpty())
			m.addSeparator();
		for (const QString &t : words) {
			QAction *a = m.addAction(t);
			connect(a, &QAction::triggered, cm,
				[cm, t]() { cm->setText(t); });
		}
		m.exec(pick->mapToGlobal(QPoint(0, pick->height())));
	});

	// A TAG REACHES THE STORE THE MOMENT IT IS ON SCREEN, whichever way it got
	// there — typed or taken off the list. It fires per keystroke, and that is
	// affordable because the table refuses to rebuild while a line edit inside
	// it has focus (see refreshEvents). The comparison keeps even that honest:
	// re-writing the same text would bump the store's version for nothing.
	connect(cm, &QLineEdit::textChanged, this,
		[this, eventId](const QString &text) {
			if (refreshing_)
				return;
			const std::string want = text.trimmed().toStdString();
			auto &store = EventStore::instance();
			if (store.description(eventId) == want)
				return;
			store.setDescription(eventId, want);
		});
	// Finishing the edit is what promotes a word to this session's list: it is
	// the point at which the operator has decided on it. Doing it per keystroke
	// would offer "G", "Go" and "Gol" on every other row.
	connect(cm, &QLineEdit::editingFinished, this, [this, cm]() {
		if (refreshing_)
			return;
		rememberComment(cm->text().trimmed());
	});

	cm->setText(QString::fromStdString(note));
	return w;
}

// The fast path for a comment cell that already belongs to this event.
bool MultiReplayDock::updateNoteCell(QWidget *cell, int eventId,
				     const std::string &note)
{
	if (!cell || cell->property("mrEventId").toInt() != eventId ||
	    cell->property("mrVocab").toULongLong() != commentVocabVersion_)
		return false;
	auto *cm = cell->findChild<QLineEdit *>(QStringLiteral("mrAngleNote"));
	if (!cm)
		return false;
	const QString noteQ = QString::fromStdString(note);
	if (cm->text() != noteQ)
		cm->setText(noteQ);
	return true;
}

QWidget *MultiReplayDock::buildAngleCell(int eventId, int cam0, bool on,
					 double speed)
{
	// [☑] [speed ▾] - one widget, one angle, two answers: does it play,
	// and how fast. WHAT it is moved out to a column of its own (kColNote):
	// a goal is a goal on every lens that saw it, and asking once per camera
	// meant it was answered on one of them and blank on the rest.
	// Widgets rather than a delegate on purpose: the check has to toggle on
	// the FIRST click and the two menus have to open on the first click too.
	// With a delegate each of those is a click to select, then a click to
	// edit, and in a live gallery that second click is the one that does not
	// happen.
	// PARENTLESS until setCellWidget takes it. The dock carries 376 lines of
	// style sheet, and Qt resolves that sheet for a widget the moment it is
	// given a parent inside it — so building the five widgets of this cell
	// under `events_` polished each of them separately against the whole
	// sheet. Measured, one row of two cells: 62-123 ms. Built detached, the
	// subtree is polished ONCE, when the table adopts it.
	auto *w = new QWidget;
	auto *h = new QHBoxLayout(w);
	// THE PAIR IS CENTRED AND TIGHT. With the comment gone the cell holds two
	// small controls in a column the table stretches, and packed to the left
	// at the old spacing they read as a tick marooned a long way from the
	// number it belongs to - two things, not one answer about one angle.
	h->setContentsMargins(2, 0, 2, 0);
	h->setSpacing(2);
	h->addStretch(1);

	auto *box = new QCheckBox(w);
	box->setChecked(on);
	box->setToolTip(obs_module_text("Dock.AngleOnHint"));
	h->addWidget(box);

	// THE SPEED IS A LABEL - text, like the id and the duration beside it -
	// and a click on it opens the list. It has been a drop-down and then a
	// chip, and the operator asked for the label back both times: a frame
	// round a value says "this is a control", which is true of every cell in
	// this table and so worth saying on none of them, and what it costs is
	// the row height for the whole list.
	//
	// FIXED WIDTH so the column does not dance as "--" becomes "100%": eight
	// rows of angles are scanned down, and a value that moves sideways between
	// rows is read twice.
	auto *sp = new QPushButton(w);
	sp->setObjectName("mrAngleSpeed");
	sp->setToolTip(obs_module_text("Dock.AngleSpeedHint"));
	sp->setCursor(Qt::PointingHandCursor);
	sp->setFocusPolicy(Qt::NoFocus);
	sp->setFixedWidth(44);
	const int pct = speed >= 0 ? (int)std::lround(speed * 100.0) : -1;
	// "--", NOT "100%", for the default (Angelo, 2026-08-17). The value means
	// "no override; the slider decides", and printing it as a number lies
	// whenever the slider is not at 100: with the slider on 25 the cell read
	// 100% while the clip played at a quarter speed. A number the operator can
	// read is worth having, but not a number that can be wrong.
	sp->setText(pct > 0 ? QString("%1%").arg(pct) : QStringLiteral("--"));
	sp->setProperty("mrPct", pct);
	// Grey for "the slider decides", the panel's ordinary text for an override.
	// A PROPERTY, not a per-widget style sheet: setStyleSheet on a single widget
	// makes Qt build a style context of its own for it and re-polish its
	// subtree, once per angle cell, on a rebuild that was already the longest
	// thing the dock's poll did.
	sp->setProperty("mrNoOverride", pct <= 0);
	h->addWidget(sp);
	h->addStretch(1);

	const int a1c = cam0 + 1;
	connect(sp, &QPushButton::clicked, this, [this, sp, eventId, a1c]() {
		if (refreshing_)
			return;
		QMenu m(sp);
		// 100 IS one of the presets, and has to be: "--" is not a speed,
		// so without it there is no way to pin an angle to 1x while the
		// slider sits at 25.
		const QVector<int> pcts = {-1, 25, 33, 50, 75, 100, 200};
		const int now = sp->property("mrPct").toInt();
		for (int p : pcts) {
			QAction *a = m.addAction(
				p > 0 ? QString("%1%").arg(p)
				      : QStringLiteral("--"));
			a->setCheckable(true);
			a->setChecked(p == now);
			connect(a, &QAction::triggered, this,
				[this, eventId, a1c, p]() {
					EventStore::instance().setAngleSpeed(
						eventId, a1c,
						p > 0 ? p / 100.0 : -1.0);
				});
		}
		m.exec(sp->mapToGlobal(QPoint(0, sp->height())));
	});


	// WHAT THIS CELL IS ABOUT, so refreshEvents can tell whether it may be
	// updated in place instead of rebuilt. The connections below capture the
	// event and the angle by value, so a cell may only ever be reused for that
	// same pair — reusing it for another event would write the operator's next
	// edit onto the wrong one. The vocabulary version is here for the same
	// reason: a reused cell keeps the comment list it was built with.
	w->setProperty("mrEventId", eventId);
	w->setProperty("mrCam", cam0);

	const int a1 = cam0 + 1; // EventStore is 1-based

	connect(box, &QCheckBox::toggled, this, [this, eventId, a1](bool v) {
		if (refreshing_)
			return;
		EventStore::instance().setAngle(eventId, a1, v);
	});
	return w;
}

// THE TWO CELLS THAT ARE WIDGETS HAVE TO BE TOLD THE ROW IS SELECTED.
//
// A selected row is painted by the view — background and text — and that reaches
// the four plain columns and stops. The comment and the speed live in widgets,
// which the view draws round rather than through, so on a selected row they kept
// the panel's ordinary ink: dark text on the selection's fill, which on a light
// theme is unreadable. A property and one rule each; there is nowhere else the
// selection can arrive from.
void MultiReplayDock::tintSelectedCells()
{
	if (!events_)
		return;
	for (int r = 0; r < events_->rowCount(); r++) {
		const bool sel = events_->selectionModel() &&
				 events_->selectionModel()->isRowSelected(r);
		auto mark = [&](QWidget *w) {
			if (!w || w->property("sel").toBool() == sel)
				return;
			w->setProperty("sel", sel);
			repolish(w);
		};
		if (QWidget *nc = events_->cellWidget(r, kColNote)) {
			mark(nc);
			if (auto *le = nc->findChild<QLineEdit *>(
				    QStringLiteral("mrAngleNote")))
				repolish(le);
		}
		for (size_t i = 0; i < camCols_.size(); i++) {
			QWidget *cell = events_->cellWidget(
				r, kColFirstCam + (int)i * kColsPerCam);
			if (!cell)
				continue;
			mark(cell->findChild<QPushButton *>(
				QStringLiteral("mrAngleSpeed")));
		}
	}
}

void MultiReplayDock::onEventItemChanged(QTableWidgetItem *item)
{
	// ONE editable column: the event comment. In, out and duration are
	// read-only, and what changes per angle lives in the widget that cell
	// holds (see buildAngleCell).
	//
	// itemsProgrammatic_ is what keeps refreshEvents from writing its own
	// redraw back into the store as if the operator had typed it.
	if (!item || itemsProgrammatic_ || refreshing_)
		return;
	if (item->column() != kColNote || !events_)
		return;
	QTableWidgetItem *idIt = events_->item(item->row(), kColId);
	if (!idIt)
		return;
	const int id = idIt->data(Qt::UserRole).toInt();
	if (id <= 0)
		return;
	const std::string want = item->text().trimmed().toStdString();
	auto &store = EventStore::instance();
	if (store.description(id) == want)
		return;
	store.setDescription(id, want);
}

// ---------------------------------------------------------------------------
// Event filter — double-click on note labels to edit
// ---------------------------------------------------------------------------

bool MultiReplayDock::eventFilter(QObject *watched, QEvent *event)
{
	// A DOUBLE CLICK ON THE FLOATING PANEL'S TITLE BAR MAXIMISES IT, it does
	// not put it away. Qt's answer to one is _q_toggleTopLevel() — the panel
	// re-docks — and inside OBS that means it goes back to wherever the layout
	// last had it, which can be behind another dock's tab: from the operator's
	// side the panel he was working in simply vanished, and came back only when
	// something else made OBS re-lay-out. Reported from a real rig.
	//
	// SWALLOWING IS THE WHOLE FIX ON A NATIVE FRAME — and doing more than that
	// was the second report. A non-client message is delivered to Qt AND then
	// falls through to DefWindowProc (it has to: dragging, resizing and the
	// system menu are all non-client, and Qt returning "handled" would take
	// them away). So Windows already toggles maximise on a title-bar double
	// click, now that the window has a maximise box at all — and a showNormal()
	// of ours on top of it made the panel shrink and immediately grow back,
	// because two actors were toggling the same thing. Blocking Qt's handler
	// leaves exactly one.
	//
	// The Qt-DRAWN title bar (xcb, Wayland — where wmSupportsNativeWindowDeco()
	// is false) has no DefWindowProc behind it, so there the toggle IS ours.
	// That one arrives as an ordinary double click, and is only ours ABOVE the
	// panel — that is where the bar is; inside our own rectangle a double click
	// belongs to whatever was clicked.
	if (filteredHost_ && watched == filteredHost_ &&
	    filteredHost_->isFloating()) {
		const QEvent::Type t = event->type();
		if (t == QEvent::NonClientAreaMouseButtonDblClick)
			return true; // the platform does the rest
		if (t == QEvent::MouseButtonDblClick) {
			auto *me = static_cast<QMouseEvent *>(event);
			if (me->position().y() < geometry().top()) {
				// MAXIMISE, NOT FULL SCREEN — the difference is
				// the title bar. Full screen takes every piece
				// of chrome with it, which is right for the ⛶
				// key and wrong for a double click aimed at the
				// very bar the operator is asking to keep.
				if (filteredHost_->isMaximized())
					filteredHost_->showNormal();
				else
					filteredHost_->showMaximized();
				refreshFullScreenKey();
				return true;
			}
		}
	}
	// THE TABLE EATS THE KEYS THAT MATTER. A QTableWidget with focus takes
	// Enter to open an editor and ←/→ to walk across columns, and the table is
	// where the operator's focus is for most of a match — so without this the
	// transport keys existed everywhere except the one place he was.
	//
	// ↑/↓ are deliberately LEFT to the table: moving down a row is moving to the
	// next event, which is exactly what the key is for, and taking it over would
	// also leave the table's current cell behind the selection.
	if (event->type() == QEvent::KeyPress &&
	    (watched == events_ || qobject_cast<QAbstractButton *>(watched))) {
		auto *ke = static_cast<QKeyEvent *>(event);
		// Space is left alone: on a focused button it is the click, and taking
		// it away would break the one keyboard gesture Qt gives for free.
		const bool tableUp = watched == events_ &&
				     (ke->key() == Qt::Key_Up ||
				      ke->key() == Qt::Key_Down);
		if (ke->key() != Qt::Key_Space && !tableUp &&
		    handleTransportKey(ke))
			return true;
	}
	// Clicking a multiview tile (its picture or its caption) selects that
	// angle — the same thing the numbered angle button does, reached from the
	// picture the operator is already looking at. The replay tile is not an
	// angle, so it is inert.
	if (event->type() == QEvent::MouseButtonPress) {
		auto *me = static_cast<QMouseEvent *>(event);
		if (me->button() == Qt::LeftButton) {
			for (const PreviewTile &t : tiles_) {
				if (t.cam0 < 0)
					continue;
				if (watched != t.display && watched != t.caption)
					continue;
				setAngle(t.cam0 + 1);
				return true;
			}
			// The per-angle speed cell is a combo whose display is a
			// read-only line edit (so the text can be CENTRED — see
			// buildAngleCell). A read-only line edit eats the press
			// instead of dropping the list down, so the press is handed
			// back to the combo. This has to work on the FIRST click:
			// in a gallery the second one does not happen.
			if (auto *le = qobject_cast<QLineEdit *>(watched)) {
				if (auto *cb = qobject_cast<QComboBox *>(
					    le->parentWidget())) {
					cb->showPopup();
					return true;
				}
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}

void MultiReplayDock::rememberComment(const QString &text)
{
	// A comment typed on one event joins the list offered on every other one,
	// for the rest of the session.
	//
	// NOT written into Config.commentPresets, tempting as that is: setConfig()
	// re-points the segment index and re-creates the Branch Output filters, so
	// persisting a comment would mean a typed word could restart the recording
	// path mid-match. The preset list stays the operator's, edited in Settings;
	// this is the session's own memory of what he has been writing.
	const QString t = text.trimmed();
	if (t.isEmpty() || sessionComments_.contains(t))
		return;
	// Bounded: this is a convenience, not a log. The oldest goes when the list
	// is full, because what was typed most recently is what is about to be
	// typed again.
	sessionComments_.append(t);
	while (sessionComments_.size() > kMaxSessionComments)
		sessionComments_.removeFirst();
	// The cells are rebuilt from this list on the next refresh, and a new
	// comment has to reach the OTHER rows, so ask for one — and say that the
	// VOCABULARY moved, not just the table. Cells are reused in place now, and
	// a reused cell keeps the list it was built with: without this the word
	// just typed would reach every other row only when something else happened
	// to force those cells to be rebuilt, which is exactly the kind of "it
	// works, sometimes" the reuse must not introduce.
	commentVocabVersion_++;
	commentsDirty_ = true;
	obs_log(LOG_INFO, "[dock] comment '%s' added to this session's list (%d)",
		t.toUtf8().constData(), (int)sessionComments_.size());
}

} // namespace multireplay
