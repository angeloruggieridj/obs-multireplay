/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "display-overlay.hpp"

#include <graphics/graphics.h>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>

namespace multireplay {

namespace {

constexpr int kColonNum = 6; // a colon steps 6/10 of a digit cell
constexpr int kColonDen = 10;

QFont overlayFont(int px)
{
	QFont f;
	f.setPixelSize(px);
	f.setBold(true);
	return f;
}

int padFor(int px)
{
	return std::max(2, px / 10);
}

// White text with a dark outline: legible on any picture, no band needed.
void paintOutlined(QPainter &p, const QPainterPath &path, int pad)
{
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(0, 0, 0, 225), pad * 1.1, Qt::SolidLine,
		      Qt::RoundCap, Qt::RoundJoin));
	p.drawPath(path);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::white);
	p.drawPath(path);
}

QImage renderText(const QString &s, int px)
{
	if (s.isEmpty())
		return QImage();
	const QFont f = overlayFont(px);
	const QFontMetrics fm(f);
	const int pad = padFor(px);
	const int w = fm.horizontalAdvance(s) + 2 * pad;
	const int h = fm.height() + 2 * pad;
	QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	QPainterPath path;
	path.addText(pad, pad + fm.ascent(), f, s);
	paintOutlined(p, path, pad);
	p.end();
	return img;
}

// The red recording dot, the same height as the text.
QImage renderDot(int px)
{
	const int pad = padFor(px);
	const int d = std::max(8, (int)std::lround(px * 0.85));
	const int h = d + 2 * pad;
	QImage img(h, h, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(0, 0, 0, 225), pad * 0.8));
	p.setBrush(QColor(0xe5, 0x1c, 0x23));
	p.drawEllipse(QRectF(pad, pad, d, d));
	p.end();
	return img;
}

// "0123456789:" in equal cells, so the time can be composed without Qt.
QImage renderAtlas(int px, int *cellW, int *cellH)
{
	static const QString kChars = QStringLiteral("0123456789:");
	const QFont f = overlayFont(px);
	const QFontMetrics fm(f);
	const int pad = padFor(px);
	int adv = 0;
	for (int i = 0; i < 10; i++)
		adv = std::max(adv, fm.horizontalAdvance(kChars[i]));
	const int cw = adv + 2 * pad;
	const int ch = fm.height() + 2 * pad;
	QImage img(cw * (int)kChars.size(), ch,
		   QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	for (int i = 0; i < (int)kChars.size(); i++) {
		const QChar c = kChars[i];
		QPainterPath path;
		const int gx = i * cw + pad + (adv - fm.horizontalAdvance(c)) / 2;
		path.addText(gx, pad + fm.ascent(), f, QString(c));
		paintOutlined(p, path, pad);
	}
	p.end();
	*cellW = cw;
	*cellH = ch;
	return img;
}

} // namespace

DisplayOverlay::~DisplayOverlay()
{
	freeTextures();
}

void DisplayOverlay::freeTextures()
{
	bool any = false;
	for (const Tex &t : t_)
		any = any || t.tex;
	if (!any)
		return;
	// Textures belong to the graphics context. If libobs is already gone
	// there is nothing left to free them with (and nothing to leak into).
	if (!obs_initialized())
		return;
	obs_enter_graphics();
	for (Tex &t : t_) {
		if (t.tex)
			gs_texture_destroy(t.tex);
		t.tex = nullptr;
	}
	obs_leave_graphics();
}

void DisplayOverlay::setImage(Slot s, const QImage &img)
{
	t_[s].pending = img;
	t_[s].dirty = true;
}

void DisplayOverlay::setContent(const Content &c, int pictureHeightPx)
{
	if (!c.visible) {
		std::lock_guard<std::mutex> lk(mu_);
		cur_.visible = false;
		cur_.time.clear();
		return;
	}

	const int px = std::clamp((int)std::lround(pictureHeightPx * 0.055), 8, 40);

	Content old;
	int oldPx;
	{
		std::lock_guard<std::mutex> lk(mu_);
		old = cur_;
		oldPx = px_;
	}
	const bool sized = px != oldPx;

	// All QPainter work happens here, outside the lock.
	const bool rTl = sized || c.topLeft != old.topLeft;
	const bool rName = sized || c.name != old.name;
	const bool rRate = sized || c.rate != old.rate;
	QImage iTl, iName, iRate, iDot, iAtlas;
	std::array<QImage, 11> iGlyph;
	int cw = 0, ch = 0;
	if (rTl)
		iTl = renderText(c.topLeft, px);
	if (rName)
		iName = renderText(c.name, px);
	if (rRate)
		iRate = renderText(c.rate, px);
	if (sized) {
		iDot = renderDot(px);
		iAtlas = renderAtlas(px, &cw, &ch);
		// One texture per character: gs_draw_sprite_subregion sizes the quad
		// by the whole texture, so an atlas cannot be drawn cell by cell.
		const int step = cw * kColonNum / kColonDen;
		for (int i = 0; i < 10; i++)
			iGlyph[i] = iAtlas.copy(i * cw, 0, cw, ch);
		iGlyph[10] = iAtlas.copy(10 * cw + (cw - step) / 2, 0, step, ch);
	}

	std::lock_guard<std::mutex> lk(mu_);
	if (rTl)
		setImage(TopLeft, iTl);
	if (rName)
		setImage(Name, iName);
	if (rRate)
		setImage(Rate, iRate);
	if (sized) {
		setImage(Dot, iDot);
		for (int i = 0; i < 11; i++)
			setImage((Slot)(Glyph0 + i), iGlyph[i]);
		cellW_ = cw;
		cellH_ = ch;
		px_ = px;
	}
	cur_ = c;
}

void DisplayOverlay::draw(const FitRect &r)
{
	if (!r.valid())
		return;
	std::lock_guard<std::mutex> lk(mu_);
	if (!cur_.visible)
		return;

	// Upload what changed. Rare: the time, which changes every frame, is
	// composed from the atlas and never comes through here.
	for (Tex &t : t_) {
		if (!t.dirty)
			continue;
		t.dirty = false;
		if (t.tex) {
			gs_texture_destroy(t.tex);
			t.tex = nullptr;
		}
		t.w = t.h = 0;
		if (t.pending.isNull())
			continue;
		const QImage img = t.pending.convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
		const uint8_t *data = img.constBits();
		t.tex = gs_texture_create((uint32_t)img.width(),
					  (uint32_t)img.height(), GS_BGRA, 1,
					  &data, 0);
		t.w = img.width();
		t.h = img.height();
		t.pending = QImage();
	}

	gs_effect_t *fx = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!fx)
		return;
	gs_eparam_t *image = gs_effect_get_param_by_name(fx, "image");
	if (!image)
		return;

	const int margin = std::max(4, px_ / 2);
	const int bottom = r.y + r.h - margin;
	const int left = r.x + margin;
	const int right = r.x + r.w - margin;
	const int top = r.y + margin;

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	auto sprite = [&](gs_texture_t *tex, int x, int y, int w, int h) {
		gs_effect_set_texture(image, tex);
		while (gs_effect_loop(fx, "Draw")) {
			gs_matrix_push();
			gs_matrix_translate3f((float)x, (float)y, 0.0f);
			gs_draw_sprite(tex, 0, (uint32_t)w, (uint32_t)h);
			gs_matrix_pop();
		}
	};
	auto whole = [&](const Tex &t, int x, int y) {
		if (t.tex)
			sprite(t.tex, x, y, t.w, t.h);
	};

	// Top left: event id / camera number.
	whole(t_[TopLeft], left - padFor(px_), top - padFor(px_));
	// Top right: the recording dot.
	if (cur_.rec && t_[Dot].tex)
		whole(t_[Dot], right - t_[Dot].w + padFor(px_),
		      top - padFor(px_));

	// Bottom line: time of day | camera name | speed.
	if (t_[Name].tex)
		whole(t_[Name], r.x + (r.w - t_[Name].w) / 2,
		      bottom - t_[Name].h + padFor(px_));
	if (t_[Rate].tex)
		whole(t_[Rate], right - t_[Rate].w + padFor(px_),
		      bottom - t_[Rate].h + padFor(px_));

	// The time, composed from the glyph textures: digits step one cell, a
	// colon steps 6/10 of one.
	if (!cur_.time.isEmpty() && t_[Glyph0].tex && cellW_ > 0) {
		int x = left - padFor(px_);
		const int y = bottom - cellH_ + padFor(px_);
		for (const QChar c : cur_.time) {
			int idx = -1;
			if (c == QLatin1Char(':'))
				idx = 10;
			else if (c.unicode() >= '0' && c.unicode() <= '9')
				idx = (int)(c.unicode() - '0');
			if (idx < 0)
				continue;
			const Tex &g = t_[Glyph0 + idx];
			if (!g.tex)
				continue;
			whole(g, x, y);
			x += g.w;
		}
	}

	gs_blend_state_pop();
}

} // namespace multireplay
