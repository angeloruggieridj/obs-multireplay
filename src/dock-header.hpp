// dock-header.hpp — the "viewer header" strip across the top of the control dock.
//
// Clock · IN ( LIVE | -00:00:30:00 ) OUT · rate.
//
// Drawn by hand, nothing but text and circles: no image files, so there is no
// artwork to license and nothing to scale per display ratio. It carries no state
// of its own — the dock's poll pushes a State into it and it repaints when that
// changed. The two round keys only forward a click: they press the very same
// IN / OUT keys the mark block owns, so there is one code path for a mark.
#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPalette>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <algorithm>
#include <functional>

class ReplayHeader : public QWidget {
public:
	struct State {
		QString clock = QStringLiteral("--:--:--:--");
		QString delay = QStringLiteral("LIVE"); // "-00:00:30:00" when behind
		bool live = true;
		bool markOpen = false; // an IN without its OUT: the IN key is lit
		QString rate = QStringLiteral("100%");
		bool operator==(const State &o) const
		{
			return clock == o.clock && delay == o.delay &&
			       live == o.live && markOpen == o.markOpen &&
			       rate == o.rate;
		}
	};

	static constexpr int kHeight = 44;

	std::function<void()> onIn;
	std::function<void()> onOut;

	explicit ReplayHeader(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("mrHeader"));
		setFixedHeight(kHeight);
		// Never a floor under the dock's width.
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		setMinimumWidth(0);
	}

	void setState(const State &s)
	{
		if (s == st_)
			return;
		st_ = s;
		update();
	}

	QSize sizeHint() const override { return QSize(300, kHeight); }
	QSize minimumSizeHint() const override { return QSize(0, kHeight); }

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		const QPointF p = e->position();
		if (inRect().contains(p) && onIn)
			onIn();
		else if (outRect().contains(p) && onOut)
			onOut();
		else
			QWidget::mousePressEvent(e);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		const QColor fg = palette().color(QPalette::WindowText);
		QColor dim = fg;
		dim.setAlphaF(0.55);
		QColor bg = palette().color(QPalette::Window);
		const bool dark = bg.lightness() < 128;
		p.fillRect(rect(), dark ? bg.lighter(112) : bg.darker(104));

		const int w = width();
		const int h = height();
		QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
		QFont cap = font();
		cap.setPixelSize(10);
		cap.setCapitalization(QFont::AllUppercase);
		cap.setLetterSpacing(QFont::AbsoluteSpacing, 1);

		// Left: the wall clock. Dropped when the dock is too narrow for it,
		// the centre group is the part that matters.
		if (w >= 520) {
			p.setFont(cap);
			p.setPen(dim);
			p.drawText(QRectF(12, 4, 150, 12), Qt::AlignLeft,
				   QStringLiteral("Time"));
			mono.setPixelSize(22);
			mono.setBold(true);
			p.setFont(mono);
			p.setPen(fg);
			p.drawText(QRectF(12, 14, 200, h - 16),
				   Qt::AlignLeft | Qt::AlignVCenter, st_.clock);
		}
		// Right: the playback rate.
		if (w >= 360) {
			p.setFont(cap);
			p.setPen(dim);
			p.drawText(QRectF(w - 112, 4, 100, 12), Qt::AlignRight,
				   QStringLiteral("Rate"));
			mono.setPixelSize(22);
			mono.setBold(true);
			p.setFont(mono);
			p.setPen(fg);
			p.drawText(QRectF(w - 112, 14, 100, h - 16),
				   Qt::AlignRight | Qt::AlignVCenter, st_.rate);
		}

		// Centre: IN ( delay ) OUT.
		const QRectF in = inRect(), out = outRect();
		drawRound(p, in, QStringLiteral("IN"), st_.markOpen, fg, bg);
		drawRound(p, out, QStringLiteral("OUT"), false, fg, bg);

		const QRectF mid(in.right() + 8, 6, out.left() - in.right() - 16,
				 h - 12);
		if (st_.live) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xD3, 0x2F, 0x2F));
			p.drawRoundedRect(mid, 5, 5);
			QFont f = font();
			f.setPixelSize(18);
			f.setBold(true);
			f.setLetterSpacing(QFont::AbsoluteSpacing, 2);
			p.setFont(f);
			p.setPen(Qt::white);
			p.drawText(mid, Qt::AlignCenter, st_.delay);
		} else {
			mono.setPixelSize(20);
			mono.setBold(true);
			p.setFont(mono);
			p.setPen(QColor(0xF5, 0x9E, 0x0B));
			p.drawText(mid, Qt::AlignCenter, st_.delay);
		}
	}

private:
	QRectF centreBox() const
	{
		// Centred in the dock; the wall clock and the rate flank it.
		const qreal bw = std::min<qreal>(width() - 8, 300);
		return QRectF((width() - bw) / 2, 0, bw, height());
	}
	QRectF inRect() const
	{
		const QRectF c = centreBox();
		return QRectF(c.left(), (height() - 32) / 2.0, 32, 32);
	}
	QRectF outRect() const
	{
		const QRectF c = centreBox();
		return QRectF(c.right() - 32, (height() - 32) / 2.0, 32, 32);
	}
	static void drawRound(QPainter &p, const QRectF &r, const QString &t,
			      bool lit, const QColor &fg, const QColor &bg)
	{
		p.setBrush(lit ? fg : Qt::NoBrush);
		p.setPen(QPen(fg, 2));
		p.drawEllipse(r.adjusted(1, 1, -1, -1));
		QFont f = p.font();
		f.setPixelSize(t.size() > 2 ? 9 : 11);
		f.setBold(true);
		p.setFont(f);
		p.setPen(lit ? bg : fg);
		p.drawText(r, Qt::AlignCenter, t);
	}

	State st_;
};
