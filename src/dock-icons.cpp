/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

The marks on the panel's keys. See dock-icons.hpp for why they are painted here
rather than shipped as files or borrowed from a font.

TWO RULES HOLD THE SET TOGETHER, and they are the whole difference between an
icon set and twenty-two drawings:

 1. ONE DESIGN GRID. Every path is written on 24×24 and scaled to whatever the
    key asks for, so a triangle and a chevron next to each other are the same
    optical size. This is exactly what a font could not give: ▶ and ■ are sized
    by the family's own metrics, and in every family this panel was tried in they
    did not match.

 2. ONE WEIGHT. Outlined marks are stroked at kStroke on that grid, round cap and
    join. Filled marks (play, stop, the steps) are solid. Nothing is half-toned
    and nothing has a second colour — a mark on a 16 px key is read by its
    silhouette, and detail below the stroke width is noise that disappears at
    100% and smears at 150%.
*/

#include "dock-icons.hpp"

#include <QAbstractButton>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTransform>
#include <QVariant>

namespace multireplay {

namespace {

// The grid every path below is written on.
constexpr qreal kGrid = 24.0;
// The stroke of an outlined mark, on that grid. 2.0 at 24 is 1.33 px on a 16 px
// key, which is the lightest weight that still reads on the panel's greys.
constexpr qreal kStroke = 2.0;

// An arrowhead as a filled triangle: tip at (tx,ty), pointing along (dx,dy),
// `len` long and `half` wide. Written once because six marks need one and six
// hand-written triangles is six chances to get the proportion different.
void arrowHead(QPainterPath &p, qreal tx, qreal ty, qreal dx, qreal dy,
	       qreal len, qreal half)
{
	// The perpendicular, for the base corners.
	const qreal px = -dy, py = dx;
	const qreal bx = tx - dx * len, by = ty - dy * len;
	p.moveTo(tx, ty);
	p.lineTo(bx + px * half, by + py * half);
	p.lineTo(bx - px * half, by - py * half);
	p.closeSubpath();
}

// A right-pointing solid triangle inscribed in [x, x+w] × [y, y+h].
void triangleRight(QPainterPath &p, qreal x, qreal y, qreal w, qreal h)
{
	p.moveTo(x, y);
	p.lineTo(x + w, y + h / 2);
	p.lineTo(x, y + h);
	p.closeSubpath();
}

void triangleLeft(QPainterPath &p, qreal x, qreal y, qreal w, qreal h)
{
	p.moveTo(x + w, y);
	p.lineTo(x, y + h / 2);
	p.lineTo(x + w, y + h);
	p.closeSubpath();
}

// A solid triangle with SOFTENED CORNERS, added to both paths so that the
// round-join stroke following its outline is what rounds it. Written once
// because the play key and the reverse key sit next to each other, and two
// hand-drawn triangles is two chances to give them different weights - which is
// exactly how the reverse one came out looking blunt beside the play one.
//
// Inset by half the stroke, so softening the shape does not also grow it.
void triangleSoft(QPainterPath &fill, QPainterPath &line, qreal x, qreal y,
		  qreal w, qreal h, bool pointsLeft)
{
	const qreal i = kStroke / 2.0;
	const qreal X = x + i, Y = y + i, W = w - 2 * i, H = h - 2 * i;
	if (pointsLeft) {
		triangleLeft(fill, X, Y, W, H);
		triangleLeft(line, X, Y, W, H);
	} else {
		triangleRight(fill, X, Y, W, H);
		triangleRight(line, X, Y, W, H);
	}
}

// A chevron (an arrow head drawn as two strokes, not filled): the mark for
// "next / previous" where a solid triangle would read as "play".
void chevronRight(QPainterPath &p, qreal x, qreal y, qreal w, qreal h)
{
	p.moveTo(x, y);
	p.lineTo(x + w, y + h / 2);
	p.lineTo(x, y + h);
}

void chevronDown(QPainterPath &p, qreal x, qreal y, qreal w, qreal h)
{
	p.moveTo(x, y);
	p.lineTo(x + w / 2, y + h);
	p.lineTo(x + w, y);
}

// ---------------------------------------------------------------------------
// The set. `fill` collects solid shapes, `line` collects stroked ones.
// ---------------------------------------------------------------------------
//
// Two paths rather than one because a mark is usually BOTH — a frame step is a
// solid triangle against a solid bar, a trim is a solid bar with a stroked
// arrow at it — and QPainter cannot fill and stroke one path differently.
void buildPaths(Icon id, QPainterPath &fill, QPainterPath &line)
{
	switch (id) {
	case Icon::Play:
		triangleSoft(fill, line, 7, 4.5, 12, 15, false);
		break;

	case Icon::Pause:
		fill.addRoundedRect(QRectF(7.5, 5, 3.5, 14), 1.2, 1.2);
		fill.addRoundedRect(QRectF(13, 5, 3.5, 14), 1.2, 1.2);
		break;

	case Icon::Stop:
		fill.addRoundedRect(QRectF(6.5, 6.5, 11, 11), 1.6, 1.6);
		break;

	case Icon::Reverse:
		// THE EXACT MIRROR OF Play, drawn by the same helper.
		triangleSoft(fill, line, 5, 4.5, 12, 15, true);
		break;

	case Icon::PlayLast:
		// ↺ — three quarters of a circle with a head on the free end.
		// The gap is at the top left and the head points down into it,
		// which is the direction that reads as "again" rather than as
		// "refresh in progress".
		line.arcMoveTo(QRectF(5, 5, 14, 14), 110);
		line.arcTo(QRectF(5, 5, 14, 14), 110, -290);
		arrowHead(fill, 7.4, 5.2, -0.35, 0.94, 5.0, 2.4);
		break;

	case Icon::StepBack:
		// A bar and a triangle INTO it: the picture goes back one frame
		// and stops. The bar is on the side the picture moves towards,
		// which is the convention every deck uses.
		fill.addRoundedRect(QRectF(5.5, 5, 2.6, 14), 1.0, 1.0);
		triangleLeft(fill, 9.5, 5.5, 9, 13);
		break;

	case Icon::StepFwd:
		triangleRight(fill, 5.5, 5.5, 9, 13);
		fill.addRoundedRect(QRectF(15.9, 5, 2.6, 14), 1.0, 1.0);
		break;

	case Icon::SkipNext:
		// ≫ — CHEVRONS, deliberately not the solid triangles of a
		// transport key. It does not start anything: it drops the clip
		// on air and takes the next of the queue, and a mark that looks
		// like play would be read as one under pressure.
		chevronRight(line, 6.5, 6.5, 5.5, 11);
		chevronRight(line, 13, 6.5, 5.5, 11);
		break;

	case Icon::Now:
		// Back to the live edge: run to the end and stop against it.
		triangleRight(fill, 5.5, 5.5, 10, 13);
		fill.addRoundedRect(QRectF(16.5, 5, 2.6, 14), 1.0, 1.0);
		break;

	case Icon::MarkIn:
		// A post, and the footage that starts at it.
		fill.addRoundedRect(QRectF(5.5, 4, 2.6, 16), 1.0, 1.0);
		fill.addRoundedRect(QRectF(10, 8.5, 8.5, 7), 1.4, 1.4);
		break;

	case Icon::MarkOut:
		fill.addRoundedRect(QRectF(5.5, 8.5, 8.5, 7), 1.4, 1.4);
		fill.addRoundedRect(QRectF(15.9, 4, 2.6, 16), 1.0, 1.0);
		break;

	case Icon::TrimIn:
		// ⇤ — the post stays, and the point is DRAGGED to it. Same post
		// as MarkIn on purpose: it is the same edge of the same event,
		// and the arrow is the whole difference between setting it and
		// moving it.
		fill.addRoundedRect(QRectF(5.5, 4, 2.6, 16), 1.0, 1.0);
		line.moveTo(19, 12);
		line.lineTo(11.5, 12);
		arrowHead(fill, 10, 12, -1, 0, 4.2, 2.6);
		break;

	case Icon::TrimOut:
		fill.addRoundedRect(QRectF(15.9, 4, 2.6, 16), 1.0, 1.0);
		line.moveTo(5, 12);
		line.lineTo(12.5, 12);
		arrowHead(fill, 14, 12, 1, 0, 4.2, 2.6);
		break;

	case Icon::Cancel:
		line.moveTo(7, 7);
		line.lineTo(17, 17);
		line.moveTo(17, 7);
		line.lineTo(7, 17);
		break;

	case Icon::MoveUp:
		line.moveTo(6.5, 14.5);
		line.lineTo(12, 9);
		line.lineTo(17.5, 14.5);
		break;

	case Icon::MoveDown:
		chevronDown(line, 6.5, 9.5, 11, 5.5);
		break;

	case Icon::More:
		for (qreal x : {6.5, 12.0, 17.5})
			fill.addEllipse(QPointF(x, 12), 1.5, 1.5);
		break;

	case Icon::ExportClip:
		// Down into a tray. Not scissors: at 16 px a pair of scissors is
		// four strokes crossing inside three pixels, and what this key
		// does is write a file.
		line.moveTo(12, 4.5);
		line.lineTo(12, 13);
		arrowHead(fill, 12, 15, 0, 1, 4.4, 2.8);
		line.moveTo(5.5, 17.5);
		line.lineTo(5.5, 19.5);
		line.lineTo(18.5, 19.5);
		line.lineTo(18.5, 17.5);
		break;

	case Icon::ExportReel:
		// The same tray, under a STRIP of clips: the difference between
		// the two export keys is one file or many, so it is drawn as one
		// block or several.
		for (qreal x : {4.5, 9.7, 14.9})
			fill.addRoundedRect(QRectF(x, 4.5, 4.4, 7), 1.0, 1.0);
		line.moveTo(12, 12.5);
		line.lineTo(12, 15.5);
		arrowHead(fill, 12, 17.5, 0, 1, 4.0, 2.6);
		break;

	case Icon::Swap:
		// ⇄ — two lanes going opposite ways. The heads are on opposite
		// ends, which is the only part of this mark that carries meaning.
		line.moveTo(5.5, 9);
		line.lineTo(16, 9);
		arrowHead(fill, 18.5, 9, 1, 0, 4.0, 2.6);
		line.moveTo(18.5, 15);
		line.lineTo(8, 15);
		arrowHead(fill, 5.5, 15, -1, 0, 4.0, 2.6);
		break;

	case Icon::Gear: {
		// Eight teeth and a hole. Drawn from a transform rather than by
		// hand: eight hand-placed rectangles is eight chances for one of
		// them to sit a degree out, and at 16 px that is the only thing
		// anyone would see.
		QPainterPath tooth;
		tooth.addRoundedRect(QRectF(-1.7, -11.0, 3.4, 4.6), 1.0, 1.0);
		for (int i = 0; i < 8; i++) {
			QTransform t;
			t.translate(12, 12);
			t.rotate(i * 45.0);
			fill.addPath(t.map(tooth));
		}
		fill.addEllipse(QPointF(12, 12), 6.6, 6.6);
		QPainterPath hole;
		hole.addEllipse(QPointF(12, 12), 2.9, 2.9);
		fill = fill.subtracted(hole);
		break;
	}

	case Icon::Search:
		line.addEllipse(QPointF(10.5, 10.5), 5.2, 5.2);
		line.moveTo(14.4, 14.4);
		line.lineTo(19, 19);
		break;

	case Icon::Zoom:
		line.addEllipse(QPointF(10.5, 10.5), 5.2, 5.2);
		line.moveTo(14.4, 14.4);
		line.lineTo(19, 19);
		line.moveTo(8, 10.5);
		line.lineTo(13, 10.5);
		break;

	case Icon::FullScreen:
		// Four corners, no box: the box is the panel and the corners are
		// what it is being pushed out to.
		line.moveTo(4.5, 9);
		line.lineTo(4.5, 4.5);
		line.lineTo(9, 4.5);
		line.moveTo(15, 4.5);
		line.lineTo(19.5, 4.5);
		line.lineTo(19.5, 9);
		line.moveTo(19.5, 15);
		line.lineTo(19.5, 19.5);
		line.lineTo(15, 19.5);
		line.moveTo(9, 19.5);
		line.lineTo(4.5, 19.5);
		line.lineTo(4.5, 15);
		break;

	case Icon::Monitors:
		// Two screens, one behind the other: this key takes the whole
		// monitoring block away, not one picture.
		line.addRoundedRect(QRectF(4.5, 5.5, 12, 9), 1.4, 1.4);
		line.moveTo(19.5, 8.5);
		line.lineTo(19.5, 18.5);
		line.lineTo(8.5, 18.5);
		break;

	case Icon::Rec:
		// The record dot: solid, nothing else. It is not the Live mark
		// (that one has the tally's ring round it) and the two must not
		// be mistaken for one another — one arms a take, the other says
		// where a mark lands.
		fill.addEllipse(QPointF(12, 12), 6.0, 6.0);
		break;

	case Icon::Live:
		// The on-air dot with its ring. Solid centre so it survives being
		// tinted red on a red key.
		fill.addEllipse(QPointF(12, 12), 3.4, 3.4);
		line.addEllipse(QPointF(12, 12), 7.2, 7.2);
		break;

	case Icon::Loop:
		// A RECTANGULAR CIRCUIT with a head on each straight, which is the
		// mark every player uses for repeat. The first version was a
		// figure drawn from two arcs and a straight, and at 16 px it came
		// out as an ambiguous squiggle that could not be told from
		// PlayLast — two keys in the same row reading as the same key.
		//
		// Top rail runs right and turns down; bottom rail runs left and
		// turns up. The gap in each rail is where its arrowhead goes, so
		// the two heads sit on opposite corners and the direction is
		// readable at a glance.
		line.moveTo(8.5, 7.0);
		line.lineTo(16.5, 7.0);
		line.arcTo(QRectF(13.5, 7.0, 6, 6), 90, -90);
		line.lineTo(19.5, 13.5);
		arrowHead(fill, 19.5, 16.0, 0, 1, 3.6, 2.4);
		line.moveTo(15.5, 17.0);
		line.lineTo(7.5, 17.0);
		line.arcTo(QRectF(4.5, 11.0, 6, 6), 270, -90);
		line.lineTo(4.5, 10.5);
		arrowHead(fill, 4.5, 8.0, 0, -1, 3.6, 2.4);
		break;

	case Icon::Music:
		// One note, stem and head. Two notes at 16 px is a smudge.
		line.moveTo(10, 17);
		line.lineTo(10, 5.5);
		line.lineTo(18, 7.6);
		line.lineTo(18, 14.6);
		fill.addEllipse(QPointF(7.6, 17), 3.0, 2.5);
		fill.addEllipse(QPointF(15.6, 14.6), 3.0, 2.5);
		break;

	case Icon::ToOutput:
		// Out of the panel and into the programme: a screen with the
		// picture leaving it.
		line.moveTo(11, 5.5);
		line.lineTo(5.5, 5.5);
		line.lineTo(5.5, 18.5);
		line.lineTo(18.5, 18.5);
		line.lineTo(18.5, 13);
		line.moveTo(12.5, 11.5);
		line.lineTo(18, 6);
		arrowHead(fill, 19.5, 4.5, 0.707, -0.707, 4.4, 2.8);
		break;

	case Icon::Health:
		// The warning triangle. It is the one mark on the panel that is
		// allowed to be alarming, and it is hidden unless there is
		// something to say.
		line.moveTo(12, 4.8);
		line.lineTo(20, 18.8);
		line.lineTo(4, 18.8);
		line.closeSubpath();
		line.moveTo(12, 10);
		line.lineTo(12, 14);
		fill.addEllipse(QPointF(12, 16.6), 1.2, 1.2);
		break;

	case Icon::Menu:
		chevronDown(line, 7.5, 10, 9, 4.5);
		break;
	}
}

QPixmap renderIcon(Icon id, const QColor &tint, int px, qreal dpr)
{
	const int side = std::max(8, px);
	QPixmap pm(QSize((int)(side * dpr), (int)(side * dpr)));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);

	QPainterPath fill, line;
	buildPaths(id, fill, line);

	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	// The whole design grid maps onto the pixmap, so a path written at 24 is
	// drawn at `side` and the stroke scales with it.
	p.scale(side / kGrid, side / kGrid);

	if (!line.isEmpty()) {
		QPen pen(tint);
		pen.setWidthF(kStroke);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(line);
	}
	if (!fill.isEmpty()) {
		p.setPen(Qt::NoPen);
		p.setBrush(tint);
		p.drawPath(fill);
	}
	return pm;
}

// One rendered pixmap per (mark, colour, size, dpr). The panel rebuilds its
// style on every theme change and poll() touches these keys thirty times a
// second; re-rasterising twenty-two paths for that would be work nobody asked
// for.
QHash<QString, QPixmap> &pixCache()
{
	static QHash<QString, QPixmap> c;
	return c;
}

QPixmap cachedPixmap(Icon id, const QColor &tint, int px, qreal dpr)
{
	const QString key = QString::number((int)id) + '|' + tint.name(QColor::HexArgb) +
			    '|' + QString::number(px) + '|' +
			    QString::number(dpr, 'f', 2);
	auto it = pixCache().constFind(key);
	if (it != pixCache().constEnd())
		return *it;
	QPixmap pm = renderIcon(id, tint, px, dpr);
	pixCache().insert(key, pm);
	return pm;
}

} // namespace

QIcon iconFor(Icon id, const QColor &tint, int px, qreal dpr)
{
	QIcon ic;
	ic.addPixmap(cachedPixmap(id, tint, px, dpr));
	return ic;
}

void setKeyIcon(QAbstractButton *b, Icon id, const IconTints &tints, int px)
{
	if (!b)
		return;
	b->setProperty("mrIcon", (int)id);
	b->setProperty("mrIconPx", px);
	// A KEY MAY ASK FOR A WHITE MARK WHEN IT IS LIT. The lit tint is the
	// panel's preview colour, which is right on a neutral key and wrong on one
	// that is itself a signal: a green dot inside the red LIVE key is two
	// signals arguing in one control.
	IconTints t = tints;
	if (b->property("mrIconOnWhite").toBool())
		t.on = QColor(Qt::white);

	const qreal dpr = b->devicePixelRatioF() > 0 ? b->devicePixelRatioF() : 1.0;
	QIcon ic;
	// ALL FOUR STATES, and this is the part a style sheet cannot do. A QSS
	// rule recolours a key's background when it is hovered, lit or disabled;
	// the mark on it is a pixmap and would stay one colour through all of it —
	// so a lit toggle would be a bright key wearing a dim mark, which reads as
	// disabled.
	ic.addPixmap(cachedPixmap(id, t.rest, px, dpr), QIcon::Normal,
		     QIcon::Off);
	ic.addPixmap(cachedPixmap(id, t.hover, px, dpr), QIcon::Active,
		     QIcon::Off);
	ic.addPixmap(cachedPixmap(id, t.disabled, px, dpr), QIcon::Disabled,
		     QIcon::Off);
	ic.addPixmap(cachedPixmap(id, t.on, px, dpr), QIcon::Normal,
		     QIcon::On);
	ic.addPixmap(cachedPixmap(id, t.on, px, dpr), QIcon::Active,
		     QIcon::On);
	ic.addPixmap(cachedPixmap(id, t.disabled, px, dpr), QIcon::Disabled,
		     QIcon::On);
	b->setIcon(ic);
	b->setIconSize(QSize(px, px));
}

void restyleIcons(QObject *root, const IconTints &tints)
{
	if (!root)
		return;
	for (QAbstractButton *b : root->findChildren<QAbstractButton *>()) {
		const QVariant v = b->property("mrIcon");
		if (!v.isValid())
			continue;
		setKeyIcon(b, (Icon)v.toInt(), tints,
			   b->property("mrIconPx").toInt() > 0
				   ? b->property("mrIconPx").toInt()
				   : 16);
	}
}

void setKeyId(QAbstractButton *b, const QString &id)
{
	if (b)
		b->setProperty(kKeyProperty, id);
}

} // namespace multireplay
