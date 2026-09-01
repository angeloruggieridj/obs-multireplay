// dock-assets.hpp — the three marks a STYLE SHEET has to be handed as files.
//
// WHY THIS EXISTS AT ALL. Every mark on this panel is drawn (dock-icons.hpp):
// no icon font, no SVG module, no binary blobs in a GPL repo. That works
// because a key is a widget and a widget takes a QIcon. It does NOT work for a
// SUB-CONTROL — a check box's indicator, a spin box's two arrows, a combo's
// drop-down — because those are painted by the style sheet and a style sheet
// can only be given `image: url(...)`. There is no way to hand it a pixmap.
//
// THE TRICK THAT WAS THERE BEFORE DID NOT WORK, and it looked like it did. The
// sheet drew these as "CSS triangles" — a zero-sized box with two transparent
// side borders and one solid one, which is how a browser makes a triangle. Qt
// does not mitre a sub-control's border like that: it paints the box. Rendered
// and looked at, every drop-down arrow and every spin arrow on this panel was a
// small solid grey RECTANGLE, and had been since the day that rule was written.
// The comment above it said "a CSS triangle is a shape we own and can colour",
// which was half right: we owned it, and it was not a triangle.
//
// So the marks are still ours and still drawn by us — they are simply written
// out as small PNGs first, and the sheet is given their paths. Two properties
// make that safe rather than clever:
//
//   * THEY ARE REGENERATED FROM THE SCHEME. A tint baked into a file at install
//     time would be the one thing on this panel that a theme change could not
//     follow, which is exactly the fault the icons on the three menu keys had.
//   * A FAILED WRITE IS NOT A FAILED PANEL. `write()` returns what it managed
//     to produce; whatever is missing comes back empty and the sheet falls back
//     to `image: none` — a filled box for a tick, no arrow for a spinner. Ugly,
//     and still a working panel.
//
// Pure Qt, no OBS and no FFmpeg, like dock-style and dock-layout: the dock
// hands it obs_module_config_path(), the mockup hands it a temp directory, and
// both are drawing the same marks.
#pragma once

#include "dock-style.hpp"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QString>

#include <functional>

namespace multireplay {

namespace detail {

// 3× the size the sheet asks for. The sheet states the logical width and
// height, so Qt scales this down — which is smooth, and stays sharp on a
// display running at 150% or 200%. Writing an @2x sibling would be the tidier
// Qt idiom and is two more files to keep in step for a mark eleven pixels wide.
constexpr int kAssetScale = 3;

inline bool saveMark(const QString &path, int w, int h,
		     const std::function<void(QPainter &, QSizeF)> &paint)
{
	QImage img(w * kAssetScale, h * kAssetScale,
		   QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	{
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing, true);
		p.scale(kAssetScale, kAssetScale);
		paint(p, QSizeF(w, h));
	}
	return img.save(path, "PNG");
}

} // namespace detail

// Draw this scheme's sub-control marks into `dir` and return their paths.
//
// The names carry the colour, so two themes do not fight over one file and a
// switch back to a theme already used costs no redraw. They are small and
// there are at most a handful; nothing here deletes them, because a stale mark
// is a few hundred bytes and a deleted one that is still referenced by a live
// style sheet is a missing arrow.
inline SheetAssetPaths writeSheetAssets(const QString &dir, const Scheme &s)
{
	SheetAssetPaths a;
	if (dir.isEmpty() || !QDir().mkpath(dir))
		return a;

	const QString key = QString(s.textKey).mid(1);
	const QColor ink(s.textKey);

	// A TICK, white, because it is only ever drawn on the signal-green fill
	// that a ticked indicator carries. Two strokes, round caps, the same
	// weight as the marks on the keys.
	const QString tick = QDir(dir).filePath(QStringLiteral("mr-tick.png"));
	if (detail::saveMark(tick, 11, 11, [](QPainter &p, QSizeF sz) {
		    QPen pen(Qt::white);
		    pen.setWidthF(1.7);
		    pen.setCapStyle(Qt::RoundCap);
		    pen.setJoinStyle(Qt::RoundJoin);
		    p.setPen(pen);
		    QPainterPath path;
		    path.moveTo(sz.width() * 0.20, sz.height() * 0.52);
		    path.lineTo(sz.width() * 0.42, sz.height() * 0.76);
		    path.lineTo(sz.width() * 0.82, sz.height() * 0.26);
		    p.drawPath(path);
	    }))
		a.tick = tick;

	// THE TWO ARROWS, in the resting key ink so a spinner and the label of
	// the key beside it are the same grey.
	const auto triangle = [&](bool up) {
		return [up, ink](QPainter &p, QSizeF sz) {
			p.setPen(Qt::NoPen);
			p.setBrush(ink);
			const double top = up ? sz.height() * 0.18 : sz.height() * 0.82;
			const double base = up ? sz.height() * 0.82 : sz.height() * 0.18;
			const QPointF tri[3] = {
				QPointF(sz.width() * 0.06, base),
				QPointF(sz.width() * 0.94, base),
				QPointF(sz.width() * 0.50, top)};
			p.drawPolygon(tri, 3);
		};
	};
	const QString up =
		QDir(dir).filePath(QStringLiteral("mr-up-%1.png").arg(key));
	if (detail::saveMark(up, 9, 6, triangle(true)))
		a.arrowUp = up;
	const QString down =
		QDir(dir).filePath(QStringLiteral("mr-down-%1.png").arg(key));
	if (detail::saveMark(down, 9, 6, triangle(false)))
		a.arrowDown = down;

	// AND THE ONE THAT POINTS RIGHT: a submenu. OBS draws it from a file
	// chosen for a dark theme too, so on a light menu it was a white
	// triangle on paper — the same fault as the other three, one sub-control
	// further in.
	const QString right =
		QDir(dir).filePath(QStringLiteral("mr-right-%1.png").arg(key));
	if (detail::saveMark(right, 6, 9, [ink](QPainter &p, QSizeF sz) {
		    p.setPen(Qt::NoPen);
		    p.setBrush(ink);
		    const QPointF tri[3] = {
			    QPointF(sz.width() * 0.18, sz.height() * 0.06),
			    QPointF(sz.width() * 0.18, sz.height() * 0.94),
			    QPointF(sz.width() * 0.82, sz.height() * 0.50)};
		    p.drawPolygon(tri, 3);
	    }))
		a.arrowRight = right;
	return a;
}

} // namespace multireplay
