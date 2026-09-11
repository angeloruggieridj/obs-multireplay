#include "dock-layout.hpp"

#include <QGridLayout>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QAbstractButton>
#include <QStyle>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QPainter>
#include <QPen>
#include <QStringList>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMargins>
#include <QTabBar>

#include <algorithm>
#include <climits>
#include <cmath>

namespace multireplay {

// ---------------------------------------------------------------------------
// PanelMode
// ---------------------------------------------------------------------------

PanelMode panelModeFor(const QSize &size, PanelMode current, int wideFloorH)
{
	// Each threshold is widened in the direction that would UNDO the current
	// mode, so a panel sitting on a boundary keeps what it has until the drag
	// is meant. Coming out of Tall costs 40 px more width than going in did.
	const int wLimit = kTallMaxWidth +
			   (current == PanelMode::Tall ? kModeHysteresis : 0);
	if (size.width() < wLimit)
		return PanelMode::Tall;

	// SHORT IS "THE WIDE ARRANGEMENT NO LONGER FITS", and the honest way to
	// ask that is to compare against what it actually needs rather than
	// against a number written down once. A panel dragged as short as it will
	// go comes to rest exactly ON its floor, so the test has to fire AT that
	// height, not below it - the hysteresis is what gives it room to.
	const int need = std::max(kShortMaxHeight, wideFloorH + kModeHysteresis);
	const int hLimit = need + (current == PanelMode::Short ? kModeHysteresis
							      : 0);
	if (size.height() < hLimit)
		return PanelMode::Short;

	return PanelMode::Wide;
}

const char *panelModeName(PanelMode m)
{
	switch (m) {
	case PanelMode::Wide:
		return "wide";
	case PanelMode::Short:
		return "short";
	case PanelMode::Tall:
		return "tall";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// FlowLayout
// ---------------------------------------------------------------------------

FlowLayout::FlowLayout(QWidget *parent, int hSpacing, int vSpacing)
	: QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing)
{
	setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
	while (QLayoutItem *it = takeAt(0))
		delete it;
}

void FlowLayout::addItem(QLayoutItem *item)
{
	items_.append(item);
}

int FlowLayout::count() const
{
	return (int)items_.size();
}

QLayoutItem *FlowLayout::itemAt(int i) const
{
	return (i >= 0 && i < items_.size()) ? items_.at(i) : nullptr;
}

QLayoutItem *FlowLayout::takeAt(int i)
{
	return (i >= 0 && i < items_.size()) ? items_.takeAt(i) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
	return {};
}

bool FlowLayout::hasHeightForWidth() const
{
	return true;
}

int FlowLayout::heightForWidth(int w) const
{
	return doLayout(QRect(0, 0, w, 0), true);
}

void FlowLayout::setGeometry(const QRect &r)
{
	QLayout::setGeometry(r);
	doLayout(r, false);
}

QSize FlowLayout::sizeHint() const
{
	return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
	// The widest single item, never the sum: the sum is what makes a band
	// refuse to wrap and start squeezing again.
	QSize s(0, 0);
	for (const QLayoutItem *it : items_)
		s = s.expandedTo(it->minimumSize());
	const QMargins m = contentsMargins();
	return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

bool FlowLayout::grows(const QLayoutItem *it)
{
	const QWidget *w = it->widget();
	return w && (w->sizePolicy().horizontalPolicy() & QSizePolicy::ExpandFlag);
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	const QMargins m = contentsMargins();
	const QRect eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
	int y = eff.y();
	int i = 0;
	while (i < items_.size()) {
		// One line: how far it reaches, how tall it is, and how many of
		// its items are willing to be widened.
		int x = eff.x();
		const int first = i;
		int lineH = 0, right = eff.x(), growers = 0;
		while (i < items_.size()) {
			const QSize sz = items_[i]->sizeHint();
			const int next = x + sz.width();
			if (i > first && next - hSpace_ > eff.right() + 1)
				break;
			x = next + hSpace_;
			right = next;
			lineH = std::max(lineH, sz.height());
			if (grows(items_[i]))
				growers++;
			i++;
		}
		if (!testOnly) {
			const int extra = std::max(0, eff.right() + 1 - right);
			// A line with nothing that wants the room still has to
			// use it: the last section on the line takes it. A strip
			// whose first line stops two thirds of the way across
			// reads as a panel that gave up, and the alternative —
			// leaving the hole — is the thing that got reported as
			// "spazio libero non occupato".
			const int growersHere = growers > 0 ? growers : 1;
			const int lastIdx = i - 1;
			const int share = extra / growersHere;
			int px = eff.x();
			for (int k = first; k < i; k++) {
				QSize sz = items_[k]->sizeHint();
				const bool takesIt = growers > 0
							     ? grows(items_[k])
							     : (k == lastIdx);
				if (share > 0 && takesIt)
					sz.setWidth(sz.width() + share);
				// Every item on a line is given the LINE's height,
				// not its own: a two-row section beside a
				// three-row one otherwise sits at its own height
				// and their captions stop lining up. Each section
				// is fixed in height inside itself, so the spare
				// pixels land under its keys and nothing within
				// it stretches.
				sz.setHeight(lineH);
				items_[k]->setGeometry(QRect(QPoint(px, y), sz));
				px += sz.width() + hSpace_;
			}
		}
		y += lineH + vSpace_;
	}
	return (items_.isEmpty() ? 0 : y - vSpace_) - rect.y() + m.bottom();
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

QWidget *flowBand(QWidget *parent, const QList<QWidget *> &children, int hSpacing)
{
	auto *band = new QWidget(parent);
	auto *fl = new FlowLayout(band, hSpacing, kBandVGap + 2);
	for (QWidget *w : children)
		if (w)
			fl->addWidget(w);
	QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Minimum);
	sp.setHeightForWidth(true);
	band->setSizePolicy(sp);
	return band;
}

QWidget *stretchyZone(QWidget *zone)
{
	QSizePolicy sp = zone->sizePolicy();
	sp.setHorizontalPolicy(QSizePolicy::Expanding);
	zone->setSizePolicy(sp);
	return zone;
}

QGridLayout *bandGrid(QWidget *host)
{
	auto *g = new QGridLayout(host);
	g->setContentsMargins(0, 0, 0, 0);
	g->setHorizontalSpacing(4);
	g->setVerticalSpacing(kBandVGap);
	host->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	return g;
}

void equaliseKeyWidths(const QList<QPushButton *> &keys)
{
	int w = 0;
	for (QPushButton *b : keys) {
		if (!b)
			continue;
		b->ensurePolished();
		w = std::max(w, b->sizeHint().width());
	}
	// minimumWidth, not a fixed one: a grid cell can still come out wider
	// than this, and a key that refuses to fill its own cell leaves a hole
	// where the operator aims.
	for (QPushButton *b : keys)
		if (b)
			b->setMinimumWidth(w);
}

void useTextGlyph(QWidget *w, const QString &glyph)
{
	if (!w || glyph.isEmpty())
		return;
	// Monochrome symbol faces, most specific first. Only the FAMILY is set:
	// the style sheet owns the size (mrTransport asks for 14px), and a font
	// set here that also carried a size would silently win nothing — a style
	// sheet property beats a widget font — while a family it does not mention
	// is ours.
	static const char *const kFamilies[] = {
		"Segoe UI Symbol", // Windows: has U+23EE/U+23ED, monochrome
		"DejaVu Sans",     // most Linux desktops
		"Arial Unicode MS",
		"Apple Symbols",   // macOS
	};
	for (const char *family : kFamilies) {
		QFont f = w->font();
		f.setFamily(QString::fromLatin1(family));
		const QFontMetrics fm(f);
		bool all = true;
		for (const QChar &ch : glyph)
			if (!fm.inFont(ch)) {
				all = false;
				break;
			}
		if (!all)
			continue;
		w->setFont(f);
		return;
	}
}

// ---------------------------------------------------------------------------
// KeyBlock
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// AspectBox — see the note in the header
// ---------------------------------------------------------------------------

AspectBox::AspectBox(QWidget *parent) : QWidget(parent)
{
	// NO FLOOR. A minimum here becomes the panel's, and four rows of tiles
	// at a comfortable size put that past what a small floating window can
	// be. The box is happy at any size; what it will not do is be the wrong
	// shape at one.
	setMinimumSize(0, 0);
}

void AspectBox::setContents(QWidget *picture, QWidget *tag)
{
	pic_ = picture;
	tag_ = tag;
	relayout();
}

void AspectBox::setRatio(int w, int h)
{
	if (w <= 0 || h <= 0 || (w == rw_ && h == rh_))
		return;
	rw_ = w;
	rh_ = h;
	relayout();
}

void AspectBox::resizeEvent(QResizeEvent *)
{
	relayout();
}

void AspectBox::setTallyFrame(const QColor &c, int width)
{
	if (tallyC_ == c && tallyW_ == width)
		return;
	const bool insetChanged = (tallyW_ > 0) != (width > 0) || tallyW_ != width;
	tallyC_ = c;
	tallyW_ = width;
	// A CHANGED WIDTH CHANGES THE PICTURE'S GEOMETRY, not just the paint:
	// the ring the frame is drawn in is made by insetting the child.
	if (insetChanged)
		relayout();
	update();
}

void AspectBox::paintEvent(QPaintEvent *)
{
	// The tally is drawn as a frame INSIDE the picture rectangle — in the
	// letterbox margin when the box is not exactly the canvas ratio, and on
	// the picture's own edge when it is. No stylesheet border (a subclass
	// does not get one painted) and no background (the letterbox must stay
	// panel, like A/B's — see the tile rule in dock-style.hpp): the edge is
	// a 2px base frame in the tile-edge tint, promoted to green/red by the
	// tally, drawn here.
	// Rounded to the tile radius (artifact .box{border-radius:4px}): a square
	// frame on a rounded tile would poke its corners past the picture.
	if (!tallyC_.isValid() || tallyW_ <= 0 || picRect_.isEmpty())
		return;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(tallyC_, tallyW_));
	const qreal h = tallyW_ / 2.0;
	// OUTSIDE the picture: the ring was reserved for it in relayout(), and
	// drawing inside would put the line under a native child that paints
	// over its parent (OBSQTDisplay in the real dock) - which is why this
	// frame was invisible on the panel and fine everywhere else.
	p.drawRoundedRect(QRectF(picRect_).adjusted(-h, -h, h, h), kTileRadius,
			  kTileRadius);
}

void AspectBox::relayout()
{
	if (!pic_)
		return;
	// THE NAME IS A BADGE OVERLAID ON THE PICTURE NOW (spec §2: "cornice
	// colorata + badge piccolo in alto a sinistra, fondo semi-trasparente"),
	// not a band that used to cost the picture its own height. The picture
	// gets the WHOLE box — a 16:9 rectangle of the full available height —
	// and the badge sits inside its top-left corner, over the image.
	const int availH = height();
	if (availH < 2 || width() < 4) {
		picRect_ = QRect(0, 0, std::max(0, width()),
				 std::max(0, height()));
		pic_->setGeometry(picRect_);
		return;
	}
	// RESERVE THE RING FIRST. The tally is a ring AROUND the picture, so the
	// space it needs comes off the box before the 16:9 rectangle is fitted
	// - never off the picture afterwards. Insetting the picture instead
	// changes its ratio by the frame width (a constant inset on all four
	// sides is not proportional), and the mockup's "every picture is 16:9"
	// check caught exactly that: 172x95 where 96 was wanted.
	const int ring = std::max(0, tallyW_);
	const int availW2 = std::max(1, width() - 2 * ring);
	const int availH2 = std::max(1, availH - 2 * ring);
	const int w = std::max(1, std::min(availW2, availH2 * rw_ / rh_));
	const int h = std::max(1, w * rh_ / rw_);
	const int x = (width() - w) / 2;
	const int y = (availH - h) / 2;
	picRect_ = QRect(x, y, w, h);
	// THE PICTURE IS INSET BY THE TALLY, and it has to be. The frame is
	// painted by THIS widget, and in the real dock the picture is an
	// OBSQTDisplay - a native window, which paints over whatever its parent
	// drew underneath, whoever was raised last. So a frame drawn inside
	// picRect_ was invisible on the panel while looking perfectly right
	// anywhere the picture is an ordinary widget. Insetting leaves an
	// exposed ring no native child covers.
	//
	// The name badge hit the same wall and was fixed by giving IT a native
	// window too; a frame cannot take that route - there is nothing in the
	// middle of it to give a window to.
	pic_->setGeometry(picRect_);
	if (!tag_)
		return;
	// TOO SMALL A PICTURE HAS NO ROOM FOR A BADGE EITHER — the same floor
	// the band used to observe, just measured against the badge's own
	// height instead of subtracting it first.
	const bool room = h >= kTagH + 6 && w >= 24;
	tag_->setVisible(room);
	if (!room)
		return;
	// NATURAL WIDTH, not the picture's: a badge is small text on a small
	// chip, and stretching it to the tile's width would print "C1" in a
	// bar as wide as the image again — the very thing this replaces.
	const QSize ts = tag_->sizeHint();
	const int tw = std::min(std::max(1, w - 6), std::max(1, ts.width()));
	const int th = std::max(kTagH, ts.height());
	tag_->setGeometry(x + kBadgeX, y + kBadgeY, tw, th);
	// RAISED, AND NATIVE, because it overlaps a picture that IS a native
	// window in the real dock (OBSQTDisplay, qt-display.hpp): a plain
	// Qt-painted sibling composites into its own top-level's backing
	// store, which a separate native HWND simply paints over regardless of
	// which one was added or raised last. Giving the badge a native
	// window of its own puts it in the SAME stacking mechanism as the
	// picture, where raise() (SetWindowPos, not a Qt-internal reorder)
	// actually decides who is on top.
	if (!tag_->testAttribute(Qt::WA_NativeWindow))
		tag_->setAttribute(Qt::WA_NativeWindow);
	tag_->raise();
}


// ---------------------------------------------------------------------------
// The camera block — see the note in the header
// ---------------------------------------------------------------------------

TileBlock tileBlockFor(int paneW, int bays, int n, int gap, int maxH,
		       int forcedCols)
{
	TileBlock best;
	if (n <= 0 || paneW <= 0 || bays <= 0)
		return best;
	const auto aspect = [](int w) { return std::max(1, w * 9 / 16); };
	const int tagH = AspectBox::kTagH;
	// Proportional, not a constant: see kTileMaxShare.
	const int ceiling =
		std::max(kTileMinWidth, (int)(paneW * kTileMaxShare));

	// THE CAMERAS FILL THE WIDTH; the height is what cannot always be filled —
	// three 16:9 pictures do not tile a 3.7:1 rectangle — so the arrangement
	// that wastes least is the one chosen rather than one written down. Aiming
	// at a flat share of the pane instead (it was 22%) starved them: beside a
	// height-bound 16:9 A on a maximised panel that share was a narrow stacked
	// column with hundreds of px of black next to it.

	// ONE SLIM COLUMN TO THREE, TWO ROWS BEYOND — DECLARED, NOT SCORED.
	//
	//     1                one tile
	//     2, 3             one column, n rows (C1 over C2, C1 over C2 over C3)
	//     4                two rows, 2 columns
	//     5, 6             two rows, 3 columns
	//     7, 8             two rows, 4 columns
	//
	// which is one column to three, ceil(n/2) past it. NEVER MORE THAN TWO
	// ROWS past three: the cameras stand beside the bays, and the column
	// stays one tile wide while a third row would narrow every tile for
	// nothing. Declared, because "a slim column, then widen" is a decision
	// about how a rig is read and a score agrees with it only by accident.
	// THE GRID IS DECLARED, NOT SCORED (artifact «Blocco monitor», f9b56e12:
	// GRID={1:[1,1],2:[2,1],3:[3,1],4:[2,2],5:[2,3],6:[2,3],7:[2,4],8:[2,4]}
	// as [rows,cols]). Up to three cameras stand in ONE SLIM COLUMN beside
	// the bays — not one row of three: the column stays one tile wide while
	// a row of three spends the pane's width on thumbnails. Past three the
	// column grows to ceil(n/2) with two rows. "Three equal pictures across
	// the row" was the old intent and is gone with it.
	const int cols = forcedCols > 0 ? std::clamp(std::min(forcedCols, n), 1,
						    forcedCols)
					: (n <= 3) ? 1
						   : (n + 1) / 2;
	const int rows = (n + cols - 1) / cols;

	// THE WHOLE MONITORING ROW IS ONE HEIGHT, and that is the change that
	// took the empty band out from under the cameras.
	//
	// It used to work the other way round: the bays' height was settled first
	// and the cameras were then fitted INSIDE it, so a single row of tiles
	// came out half as tall as A and the rest of the block was a strip of
	// nothing. Cameras are 16:9 like the bays, so the honest statement is that
	// A, B and every tile row share one height h - and h is whatever makes the
	// row exactly as wide as the pane:
	//
	//     paneW = bays*aw(h) + block(h),  aw(h) = (h - tag) * 16/9
	//
	// One line of algebra rather than a search, and it fills BOTH dimensions:
	// two cameras beside A stand in one slim column as tall as A, eight
	// become four-by-two whose two rows together are exactly as tall as A.
	// Nothing is left over to park, which is why there is no spare row any
	// more.
	const double perBay = 16.0 / 9.0;
	const double perTile = cols * 16.0 / (9.0 * rows);
	const double k = bays * perBay + perTile;
	const double cst = gap * bays + (cols - 1) * kTileGap -
			   bays * perBay * tagH -
			   cols * (16.0 / 9.0) *
				   ((rows - 1) * kTileGap / (double)rows + tagH);
	int h = (k > 0.01) ? (int)((paneW - cst) / k) : maxH;
	// ...AND NEVER TALLER THAN THE ROOM. Clamped, the row simply stops short
	// of the pane's width — which is the one thing 16:9 cannot be argued out
	// of when the panel is wide and shallow.
	if (maxH > 0)
		h = std::min(h, maxH);
	h = std::max(h, kTileMinWidth * 9 / 16 + tagH);

	int th = std::max(1, (h - (rows - 1) * kTileGap) / rows - tagH);
	int tw = std::clamp(th * 16 / 9, kTileMinWidth, ceiling);
	th = aspect(tw);
	const int blockW = cols * tw + (cols - 1) * kTileGap;

	// THE ROW STILL SPANS THE PANE AFTER h HAS BEEN CLAMPED.
	//
	// h is solved so that bays*aw(h) + block(h) == paneW — the row is exactly
	// as wide as the pane. maxH (monitorRoomH: the list's floor, and the
	// half-panel rule) then caps h, and finalBayW/blockW below are taken from
	// the CAPPED h. At that shorter height a 16:9 row of this composition is
	// narrower than the pane, and the difference used to come back as dead
	// panel — the bays' AspectBox letterboxed it and the tile grid left it
	// trailing. The note under the clamp called this out ("the row simply
	// stops short of the pane's width") and accepted it; on a tall Wide panel
	// with two tile rows it is a band up to ~185 px wide (measured: eight
	// cameras at 1456).
	//
	// The tiles are confidence monitors and keep the size their height gives
	// them; the freed width goes to the BAYS, which are what is being watched.
	// Their AspectBox still centres a 16:9 picture, so A/B sit centred in a
	// slightly wider slot instead of the row falling short of the edge. Only
	// ever a widen: a bay narrower than aw(h) would letterbox vertically,
	// which is worse, and the caller already clamps an over-wide block.
	int finalBayW = std::max(40, (h - tagH) * 16 / 9);
	if (bays > 0) {
		const int filled = (paneW - blockW - gap * bays) / bays;
		if (filled > finalBayW)
			finalBayW = filled;
	}

	best = {cols,
		rows,
		tw,
		th,
		blockW,
		rows * (th + tagH) + (rows - 1) * kTileGap,
		finalBayW,
		h};
	return best;
}


KeyBlock::KeyBlock(const QString &caption, QWidget *parent)
	: QWidget(parent), caption_(caption)
{
	// A HANDLE FOR THE GATE. This class has no Q_OBJECT - giving it one would
	// put a moc'd type in a header the mockup also compiles - so a name is how
	// a section is found from outside, exactly as the camera tiles are.
	setObjectName(QStringLiteral("mrBlock"));
	// Qt honours a style-sheet background/border only on a widget that
	// carries this — it sets it for a plain QWidget but NOT for a subclass,
	// and KeyBlock is one. Without it the boxed-sub-section border in the
	// sheet draws nothing.
	setAttribute(Qt::WA_StyledBackground, true);
	auto *v = new QVBoxLayout(this);
	// A boxed sub-section (spec §4: "sotto-sezioni riquadrate, etichetta che
	// interrompe il bordo in alto a sinistra"). The border is drawn by
	// #mrBlock in the sheet; these margins keep the keys off it, with room
	// at the top for the legend to sit on the border line — but only when
	// there IS a legend. The header blocks pass an empty caption and want
	// the whole height for their key.
	// THE OUTER WIDGET NO LONGER DRAWS THE BOX. It carries nothing but the
	// inset that lets the legend straddle the frame's top line: a child
	// cannot be drawn above y=0 of its parent, so a caption can never sit ON
	// a border that this widget draws. The border moves to frame_, inset by
	// half a caption, and the caption is laid OVER it (see placeCaption).
	// The old attempt did this with `margin-top: -8px` in the sheet, which
	// cannot work: a style-sheet margin is applied INSIDE the geometry the
	// layout already assigned, so it never moved the label out of its cell.
	v->setContentsMargins(0, caption_.isEmpty() ? 0 : kCaptionH / 2, 0, 0);
	v->setSpacing(0);

	frame_ = new QWidget(this);
	frame_->setObjectName(QStringLiteral("mrBlockFrame"));
	frame_->setAttribute(Qt::WA_StyledBackground, true);
	auto *fv = new QVBoxLayout(frame_);
	// THE TWO TOP NUMBERS SUM TO WHAT blockHeight() DECLARES (kCaptionH + 2):
	// kCaptionH/2 on the outer, the remainder here. The strip measures with
	// blockHeight() and apply() draws with these; two ways of asking the same
	// question is two answers waiting to differ.
	fv->setContentsMargins(6,
			       caption_.isEmpty() ? 2
						  : kCaptionH + 2 - kCaptionH / 2,
			       6, 4);
	fv->setSpacing(2);
	v->addWidget(frame_);

	// The caption sits ABOVE the keys. It names the group instead of
	// competing with the first key for the same line, and it lets every
	// section start its keys at the same y.
	// AN EMPTY CAPTION MEANS NO CAPTION, and most sections use it. The
	// reference panel labels nothing but its mark keys: its groups are told
	// apart by the space between them, and the result is a dense strip that
	// reads at a glance instead of a column of headings with keys under them.
	// A caption line also costs ~16 px on a panel that is short of height.
	//
	// THE LEGEND IS AT THE TOP, interrupting the box border. Its negative
	// top margin (in the sheet) lifts it onto the border line; a panel-
	// coloured background clears the border behind the text.
	if (!caption_.isEmpty()) {
		cap_ = new QLabel(caption_.toUpper(), this);
		cap_->setObjectName(QStringLiteral("mrZoneTitle"));
		cap_->setWordWrap(false);
		cap_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		cap_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		// DELIBERATELY NOT ADDED TO A LAYOUT. It is placed by hand over the
		// frame's top border - that is the only way it can interrupt the line
		// rather than sit under it. See placeCaption().
	}

	body_ = new QWidget(frame_);
	grid_ = bandGrid(body_);
	// CENTRED IN THE LINE, not hung from the top of it.
	//
	// Every section on a line is given the LINE's height, which is the
	// height of the deepest section on it. A one-row group beside a
	// three-row one therefore has two rows of slack, and where that slack
	// goes is the whole difference between a strip that reads as a row of
	// groups and one that reads as things that fell to one side. It used to
	// all go UNDER the keys — so the bay selector sat on the top edge of its
	// line with sixty pixels of nothing beneath it, and the eye read the gap
	// as a missing row rather than as a shorter group.
	//
	// Split evenly, the group sits on the line's optical centre, which is
	// where a shorter group belongs beside a taller one.
	fv->addWidget(body_, 0);
	fv->addStretch(1);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void KeyBlock::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	placeCaption();
}

// THE LEGEND INTERRUPTS THE BORDER (the artifact's
// `.fbox > .flabel{position:absolute;top:-7px;left:12px}`): it is centred on
// the frame's top line, and its panel-coloured background clears the line
// behind the text. x is the artifact's 12 px.
void KeyBlock::placeCaption()
{
	if (!cap_ || !frame_)
		return;
	const int w = std::min(cap_->sizeHint().width(), std::max(0, width() - 16));
	cap_->setGeometry(12, 0, w, kCaptionH);
	// The frame is a sibling added after it, so without this the border is
	// painted OVER the text it is supposed to be interrupted by.
	cap_->raise();
}

void KeyBlock::setShapes(const BlockShape &tall, const BlockShape &flat)
{
	tall_ = tall;
	flat_ = flat;
	applied_ = false;
	apply();
}

void KeyBlock::setFlat(bool flat)
{
	// A section with one declared shape keeps it: the camera matrix's rows
	// are A, B and the selector, and there is no honest way to put those on
	// one line.
	const bool want = flat && !flat_.isEmpty();
	if (applied_ && want == flatActive_)
		return;
	flatActive_ = want;
	applied_ = false;
	apply();
}

void KeyBlock::setCompactShapes(const BlockShape &compact)
{
	compact_ = compact;
	applied_ = false;
	apply();
}

void KeyBlock::setCompact(bool compact)
{
	const bool want = compact && !compact_.isEmpty();
	if (applied_ && want == compactActive_)
		return;
	compactActive_ = want;
	applied_ = false;
	apply();
}

void KeyBlock::refresh()
{
	applied_ = false;
	apply();
}

int KeyBlock::rows() const
{
	const BlockShape &s = (compactActive_ && !compact_.isEmpty())
				      ? compact_
			      : (flatActive_ && !flat_.isEmpty()) ? flat_
								  : tall_;
	return (int)s.size();
}

int KeyBlock::shapeHeight(bool flat) const
{
	const BlockShape &s = (flat && !flat_.isEmpty()) ? flat_ : tall_;
	// The caption height is a CONSTANT, not the label's own sizeHint: this
	// function is what the strip measures with, and apply() is what draws it.
	// Two ways of asking the same question is two answers waiting to differ.
	const int capH = (cap_ && !flat) ? kCaptionH + 2 : 0;
	const int keyH = flat ? sectionKeyFoldedH() : sectionKeyH();
	// Rows with tall keys (mrKeyH) stand taller: measure them, not the pin.
	int total = capH;
	bool first = true;
	for (const QVector<Cell> &row : s) {
		int rh = keyH;
		if (!flat) {
			for (const Cell &cell : row) {
				if (const auto *btn =
					    qobject_cast<const QAbstractButton *>(
						    cell.w))
					rh = std::max(
						rh,
						btn->property(kKeyHeightProperty)
							.toInt());
			}
		}
		if (!first)
			total += kBandVGap;
		first = false;
		total += rh;
	}
	return total;
}

void KeyBlock::setStretchColumns(int firstCol, int lastCol)
{
	stretchFrom_ = firstCol;
	stretchTo_ = lastCol;
	applied_ = false;
	apply();
}

void KeyBlock::setOnShape(std::function<void(bool flat)> fn)
{
	onShape_ = std::move(fn);
	applied_ = false;
	apply();
}

void KeyBlock::setSectionVisible(bool visible)
{
	if (sectionHidden_ == !visible)
		return;
	sectionHidden_ = !visible;
	applied_ = false;
	apply();
}

void KeyBlock::apply()
{
	if (applied_)
		return;
	applied_ = true;

	// FIRST, before a single cell is placed: a section that sizes its own keys
	// per shape (the camera matrix) has to have done it by the time the grid
	// asks them how big they are. See setOnShape in the header.
	if (onShape_)
		onShape_(flatActive_);

	// THE BOX IS A WIDE-LAYOUT FEATURE. Side by side the rounded borders and
	// legends give the panel its broadcast-desk grid (spec §4). Stacked in a
	// narrow column they are eight borders and eight legends down a panel
	// whose scarce axis is height — which is exactly the "too fragmented"
	// the redesign set out to fix — and they add ~100 px the panel's Short
	// floor cannot spare. Folded, the block goes flat: no border, tight
	// margins, the legend as a plain caption line.
	if (auto *v = qobject_cast<QVBoxLayout *>(layout())) {
		const bool legend = cap_ && !caption_.isEmpty();
		v->setContentsMargins(6, flatActive_ ? 0 : (legend ? 8 : 2), 6,
				      flatActive_ ? 0 : 4);
	}
	if (property("folded").toBool() != flatActive_) {
		setProperty("folded", flatActive_);
		if (style()) {
			style()->unpolish(this);
			style()->polish(this);
		}
	}

	if (cap_) {
		// STACKED, THE CAPTION GOES. Side by side it is what tells six
		// groups apart in one glance across the strip, and it costs one
		// line for all of them. In a column it costs a line EACH — six
		// captions were 90 px of a 900 px panel, a fifth of the control
		// strip — and it is buying much less: a group standing on its own
		// above the next one is already divided from it, and every one of
		// these groups is named by its own keys (● REC, In/Out, C1/C2, the
		// transport glyphs, the percentages, Esporta clip).
		//
		// Six labelled boxes down a narrow panel is also what "too
		// fragmented" looks like from the operator's chair: the labels were
		// part of the fragmentation, not the cure for it.
		// !sectionHidden_ too: an outsider who has told this section it
		// has nothing to show right now (channel B off — see
		// setSectionVisible()) means it on every relayout, not just the
		// one where it asked. Folding still wins on its own terms when
		// the section is NOT hidden: a stacked panel drops captions to
		// save the line regardless of channel B.
		// The legend names the boxed sub-section (spec §4) in every
		// arrangement, not only the wide one — the boxes are stacked in
		// Short and behind a tab in Tall, and a box with no name there is
		// just a rectangle.
		cap_->setVisible(!sectionHidden_ && !compactActive_);
		cap_->setFixedHeight(kCaptionH);
	}

	// Take everything out first. Deleting the layout ITEMS leaves the widgets
	// alive and parented to body_, which is the point: the checked state, the
	// tally and every connection live on those widgets, so a shape change
	// re-places them and never rebuilds them.
	const BlockShape &s = (compactActive_ && !compact_.isEmpty())
				      ? compact_
			      : (flatActive_ && !flat_.isEmpty()) ? flat_
								  : tall_;
	while (QLayoutItem *it = grid_->takeAt(0))
		delete it;
	for (int c = 0; c < 16; c++)
		grid_->setColumnStretch(c, 0);

	int r = 0;
	int maxCol = 0;
	for (const QVector<Cell> &row : s) {
		int c = 0;
		for (const Cell &cell : row) {
			if (!cell.w) { // a hole: a deliberate gap in the grid
				c += cell.span;
				continue;
			}
			// VISIBILITY IS THE CALLER'S, and re-parenting takes it
			// away: setParent() hides a widget whatever it was
			// doing, so the intent has to be read first and put
			// back. Showing everything unconditionally is the
			// obvious version of this line, and it un-hides the
			// camera slots the panel deliberately keeps empty —
			// which is exactly what the mockup drew the first time
			// it ran.
			// THE KEY ITSELF SHRINKS WHEN THE SECTION FOLDS. Only
			// buttons: a slider, a two-line clock or the bay selector
			// are cells too, and they own their own heights.
			//
			// A KEY THAT SPANS ROWS IS AS TALL AS THE ROWS IT SPANS.
			// This line used to pin EVERY button to one key height,
			// which quietly cancelled every rowSpan a caller declared:
			// REC and the green play key were asked for two rows and
			// drawn at one, so the three first-function keys came out
			// the same size as a frame step — the exact thing the
			// spans were added to fix.
			//
			// AND THE STYLE SHEET HAS TO BE TOLD. A QSS min-height
			// is a CONTENT box: the rules here state 26 px worth of
			// content plus padding plus border, and a style asked for
			// more height than the widget owns draws the frame past
			// the bottom of it - which is what cut the underside off
			// every key in the folded shape, where the pin is 22.
			// The property says "this height is the layout's", and one
			// rule at the end of the sheet stands the min-height down.
			if (auto *btn = qobject_cast<QAbstractButton *>(cell.w)) {
				const int h = flatActive_ ? sectionKeyFoldedH()
							  : sectionKeyH();
				// A TALL KEY STANDS TALLER THAN THE PIN (transport 40,
				// clip 42 — artifact .tlg/.big): its height rides the
				// mrKeyH property instead. Folded shapes ignore it.
				const int ownH = flatActive_ ? 0
							     : btn->property(
								       kKeyHeightProperty)
								       .toInt();
				const int hh = (ownH > 0) ? ownH : h;
				const int pinned = cell.rowSpan * hh +
						   (cell.rowSpan - 1) * kBandVGap;
				// STAMPED AS WELL AS SET: a style sheet's
				// min-height is written onto the widget by Qt,
				// so the next re-polish drops this. See
				// kPinnedHeightProperty.
				btn->setProperty(kPinnedHeightProperty, pinned);
				if (!btn->property("mrPinned").toBool()) {
					btn->setProperty("mrPinned", true);
					if (btn->style()) {
						btn->style()->unpolish(btn);
						btn->style()->polish(btn);
					}
				}
				// AFTER the polish, never before: polishing is
				// one of the things that drops this.
				btn->setFixedHeight(pinned);
			}
			const bool wantVisible = !cell.w->isHidden();
			cell.w->setParent(body_);
			if (wantVisible)
				cell.w->show();
			grid_->addWidget(cell.w, r, c, cell.rowSpan, cell.span);
			if (cell.grow) {
				QSizePolicy sp = cell.w->sizePolicy();
				if (sp.horizontalPolicy() == QSizePolicy::Fixed)
					sp.setHorizontalPolicy(
						QSizePolicy::Preferred);
				cell.w->setSizePolicy(sp);
			}
			c += cell.span;
		}
		maxCol = std::max(maxCol, c);
		r++;
	}
	if (stretchFrom_ >= 0) {
		for (int c = stretchFrom_; c <= stretchTo_; c++)
			grid_->setColumnStretch(c, 1);
	} else if (maxCol > 0) {
		// A PHANTOM STRETCH COLUMN just past the last key. Any width the
		// block is given beyond what its keys ask for lands here, on the
		// right, instead of being shared out among the key columns —
		// which is what spread a row of three keys across a whole 40%
		// panel and gave the redesigned strip its sparse, un-broadcast
		// look. Keys stay at their natural width; the section reads as a
		// dense group. (No-op when the block is already sized to its
		// content, e.g. inside ControlStrip's lanes.)
		grid_->setColumnStretch(maxCol, 1);
	}
	body_->updateGeometry();
	updateGeometry();
}

// ---------------------------------------------------------------------------
// ControlStrip
// ---------------------------------------------------------------------------

void repinKeys(QWidget *root)
{
	if (!root)
		return;
	const auto restore = [](QWidget *w) {
		auto *b = qobject_cast<QAbstractButton *>(w);
		if (!b)
			return;
		const QVariant h = b->property(kPinnedHeightProperty);
		if (!h.isValid())
			return;
		const int px = h.toInt();
		// Only when it has actually been lost. setFixedHeight on a
		// widget that already has it is not free — it invalidates the
		// layout — and this runs over every key on the panel.
		if (px > 0 && (b->minimumHeight() != px || b->maximumHeight() != px))
			b->setFixedHeight(px);
	};
	// ROOT INCLUDED, because the two callers want different halves of that:
	// a theme change hands over the whole panel, and a property flip hands
	// over the one key whose property flipped.
	restore(root);
	for (QAbstractButton *b : root->findChildren<QAbstractButton *>())
		restore(b);
}

ControlStrip::ControlStrip(QWidget *parent) : QWidget(parent)
{
	// NO LAYOUT of its own, deliberately — see the note in the header: a
	// child that answers heightForWidth has already told its parent that the
	// height of its current shape is its floor, and that floor is what this
	// class exists to avoid. It places its sections itself.
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void ControlStrip::measure(Entry &e)
{
	// Both shapes, once, while nothing is on screen yet. Measuring means
	// applying, and applying re-parents widgets, so this is done here rather
	// than during a layout pass — a size hint that lays widgets out is a size
	// hint that recurses.
	// ACTIVATE BEFORE READING. A shape change moves widgets between cells and
	// now also changes their HEIGHT, and Qt invalidates a layout lazily: the
	// sizeHint read on the next line is the one from before the change unless
	// the layout is made to recompute. Measured: shortening the folded keys by
	// 4 px moved the strip's height by 4 px in total rather than by 4 px per
	// row, because eleven of the twelve rows were still being measured tall.
	const bool was = e.block->isFlat();
	auto measured = [](KeyBlock *b) {
		if (b->layout())
			b->layout()->activate();
		return b->sizeHint();
	};
	e.block->setFlat(true);
	e.flat = measured(e.block);
	e.block->setFlat(false);
	e.tall = measured(e.block);
	e.block->setFlat(was);
}

void ControlStrip::addBlock(KeyBlock *b, Lane lane, bool startsLine, int rank)
{
	b->setParent(this);
	Entry e;
	e.block = b;
	e.startsLine = startsLine;
	e.lane = lane;
	e.rank = rank;
	measure(e);
	blocks_ << e;
	updateGeometry();
}

void ControlStrip::blockChanged(KeyBlock *b)
{
	for (Entry &e : blocks_)
		if (e.block == b) {
			measure(e);
			break;
		}
	layoutLines(width(), flat_, true);
	updateGeometry();
}

void ControlStrip::refreshAllBlocks()
{
	// TWO THINGS, not one, and both are needed: refresh() forces
	// KeyBlock::apply() to run regardless of its own flat/tall guard, which
	// is what makes each key's OWN pinned height (setFixedHeight, from
	// sectionKeyH()/sectionKeyFoldedH()) actually change. measure() is a
	// SEPARATE cache — the Entry's .tall/.flat QSize that layoutLanes/
	// layoutStack/layoutPacked pack sections from — and refresh() alone
	// does not touch it: a block correctly drawn 32 px tall but still
	// PACKED as if it were 26 would overlap its neighbour on the line
	// below.
	for (Entry &e : blocks_) {
		e.block->refresh();
		measure(e);
	}
	layoutLines(width(), flat_, true);
	updateGeometry();
}

QSize ControlStrip::sizeHint() const
{
	// What it needs at the width it has, in the arrangement it will wear
	// there. Both arrangements are honest; there is nothing to prefer.
	const int w = width() > 0 ? width() : minimumSizeHint().width();
	return QSize(w, minHeightForWidth(w));
}

QSize ControlStrip::minimumSizeHint() const
{
	// Width: the widest single section, never the sum — the sum is what makes
	// a strip refuse to wrap and start squeezing its keys instead. Measured in
	// the TALL shape, which is the narrow one: a flat section is a long row,
	// and asking the panel to be as wide as one would be asking it to be as
	// wide as the whole strip.
	// The NARROWER of the two shapes, not the wide one. Measuring the floor
	// against the wide row is what pinned the panel at 560 px and made a
	// vertical dock impossible: the strip demanded the width of MARK's eight
	// keys in a row, at every size, including the sizes where it would have
	// worn the compact shape instead. A floor has to be measured in the shape
	// worn at the floor.
	int w = 0;
	for (const Entry &e : blocks_) {
		// A section a mode has hidden wholesale (§6.3's "more" menu in
		// Tall) sets no floor of its own — the whole point of collapsing
		// it was to stop it doing that.
		if (e.block->isHidden())
			continue;
		const int narrow = std::min(e.tall.width(), e.flat.width());
		w = std::max(w, std::max(narrow, kMinBlockWidth));
	}
	// Height: one line's worth. The real floor is width-dependent and is
	// answered by minHeightForWidth() through ControlStripItem; a widget's
	// minimumSizeHint cannot ask about width, so it must not pretend to.
	return QSize(w, blocks_.isEmpty() ? 0 : blocks_.first().flat.height());
}

QString ControlStrip::describeBlocks() const
{
	QStringList parts;
	for (int i = 0; i < blocks_.size(); i++) {
		const Entry &e = blocks_[i];
		parts << QString("#%1 tall %2x%3 flat %4x%5")
				 .arg(i)
				 .arg(e.tall.width())
				 .arg(e.tall.height())
				 .arg(e.flat.width())
				 .arg(e.flat.height());
	}
	return parts.join(QStringLiteral("; "));
}

bool ControlStrip::flatFits(int w) const
{
	// Does the WIDE arrangement fit? A wide section is one long row, and a row
	// wider than the strip does not wrap - it is CUT. The mockup drew it: at
	// 460 px the wide MARK ended after "-10s" and the rest of its keys were
	// simply not on the panel. So the wide arrangement is only ever worn when
	// every section of it fits.
	for (const Entry &e : blocks_)
		if (e.tall.width() > w)
			return false;
	return true;
}

int ControlStrip::minHeightForWidth(int w) const
{
	// THE HEIGHT OF THE ARRANGEMENT IT WILL ACTUALLY WEAR — the same decision
	// resizeEvent makes, asked in advance. It used to report the BETTER of the
	// two shapes, which was true while the strip was free to choose either;
	// now that the panel's mode can pin it, "better" is a shape it may not be
	// allowed to take. The mockup drew the consequence at 340x900: the floor
	// came back as the packed wide rows (~276 px), the strip was wearing the
	// stack (~450), and the last two sections — VELOCITA and EXPORT — were
	// simply cut off the bottom of the panel.
	bool stack = !flatFits(w);
	if (!stack && forcedStack_ >= 0)
		stack = forcedStack_ != 0;
	return layoutLines(w, stack, false);
}

int ControlStrip::tallHeightForWidth(int w) const
{
	// The floor and the preference are the SAME number: a control strip is
	// fixed in height by design (spare height belongs to the picture and the
	// list), so once the arrangement is decided there is nothing to prefer.
	return minHeightForWidth(w);
}

void ControlStrip::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	// WIDE WHEN IT FITS. The wide arrangement is one key-row per section -
	// the reference panel's own - and it is what an operator should see
	// whenever the dock is wide enough to carry it. When it is not, every
	// section folds into its compact shape rather than being cut off at the
	// right-hand edge, which is where a single arrangement always ends on a
	// narrow dock.
	//
	// WIDTH decides, not height: height is what the fold COSTS, and a rule
	// that read the height could never reach the arrangement that would have
	// freed it (see minimumSizeHint in the header).
	// A section that would be CUT OFF folds whatever anyone says — that is the
	// one case where the wide arrangement is not an arrangement at all. Short
	// of that, the panel's mode decides (see setStacked): a side dock stacks,
	// a floating window keeps its wide rows even at a width where the three
	// lanes no longer stand side by side.
	bool want = !flatFits(width());
	if (!want && forcedStack_ >= 0)
		want = forcedStack_ != 0;
	if (want != flat_)
		applyShape(want);
	layoutLines(width(), flat_, true);
	// A FOLDED SECTION CAN LAND ON A LINE THAT GIVES IT NO SLACK — every key
	// pinned to exactly its section's height (see kPinnedHeightProperty) —
	// and a resize that reaches that state without an intervening theme
	// change never called repinKeys(), whose only other caller is
	// applyTheme(). Measured: the speed chips came out 38x13 instead of
	// 38x22, on a plain resize with no theme touched at all. Cheap and a
	// no-op unless something was actually lost (see repinKeys itself), so
	// it belongs on every pass, not only the one that reapplies the sheet.
	repinKeys(this);

	// A WIDTH CHANGE CHANGES THE FLOOR, so the parent has to be told: how
	// short this strip may be depends on how wide it is - six sections side
	// by side need two lines, folded and stacked they need four.
	if (e->oldSize().width() != width())
		updateGeometry();
}

// ---------------------------------------------------------------------------
// The hairlines between sections — see the note in the header
// ---------------------------------------------------------------------------
//
// Read off the geometry the layout has ALREADY computed, so there is no second
// copy of where a section is. Two sections belong to the same line when their
// tops agree; the rule goes down the middle of the gap between them and is
// inset top and bottom so it reads as a divider rather than as a border on
// either neighbour.
void ControlStrip::paintEvent(QPaintEvent *e)
{
	QWidget::paintEvent(e);
	if (sepRects_.isEmpty())
		return;
	QPainter p(this);
	// From the palette rather than a constant: this file has no Scheme in it,
	// and the panel's own text colour at low opacity is the same hairline the
	// style sheet draws for @border@ on any theme, light or dark.
	QColor line = palette().color(QPalette::WindowText);
	line.setAlpha(46);
	for (const QRect &r : sepRects_)
		p.fillRect(r, line);
}

// A rule between two sections of one line. Collected during the layout that
// already knows where everything is, rather than derived from block geometry
// afterwards — see the note on sepRects_.
void ControlStrip::addSeparator(int x, int top, int height) const
{
	// Inset so it reads as a divider between two groups rather than as a
	// border belonging to one of them.
	const int inset = std::max(2, height / 6);
	sepRects_ << QRect(x, top + inset, 1, std::max(1, height - 2 * inset));
}

void ControlStrip::setStacked(int on)
{
	if (forcedStack_ == on)
		return;
	forcedStack_ = on;
	bool want = !flatFits(width());
	if (!want && forcedStack_ >= 0)
		want = forcedStack_ != 0;
	if (want != flat_)
		applyShape(want);
	layoutLines(width(), flat_, true);
	updateGeometry();
}

void ControlStrip::applyShape(bool flat)
{
	flat_ = flat;
	for (Entry &e : blocks_)
		e.block->setFlat(flat);
	updateGeometry();
}

QVector<int> ControlStrip::orderFor(bool flat) const
{
	QVector<int> idx;
	idx.reserve(blocks_.size());
	// isHidden(), NOT isVisible() — a section this strip has never touched
	// reports isVisible()==false too, for as long as the strip itself (or
	// any ancestor up to the top-level window) has not been shown yet, and
	// every one of these functions can run during construction, before
	// that ever happens. isHidden() answers a narrower, safer question:
	// did somebody call hide() on THIS widget specifically — which is
	// exactly what a mode collapsing a section (§6.3, Tall's "more" menu)
	// does, and exactly what channel B's own visibility toggle never has
	// (it hides the WIDGETS inside a section, not the section itself, so
	// this filter changes nothing for the channel-B case it sits beside).
	for (int i = 0; i < blocks_.size(); i++)
		if (!blocks_[i].block->isHidden())
			idx << i;
	if (!flat)
		return idx; // wide: the declared, left-to-right order
	// Folded: by rank, stably, so two sections of equal rank keep the order
	// they were declared in.
	std::stable_sort(idx.begin(), idx.end(), [this](int a, int b) {
		return blocks_[a].rank < blocks_[b].rank;
	});
	return idx;
}

int ControlStrip::layoutLines(int width, bool flat, bool apply) const
{
	if (blocks_.isEmpty())
		return 0;
	// FOLDED IS A STACK, WIDE IS LANES, and only the wide one can fail: the
	// three lanes need room to be told apart, and when they do not have it
	// packing them is more honest than pretending the alignment is there.
	if (apply)
		sepRects_.clear();
	if (flat)
		return layoutStack(width, apply);
	const int laned = layoutLanes(width, apply);
	if (laned >= 0)
		return laned;
	return layoutPacked(width, flat, apply);
}

// The wide arrangement: two macro-rows, three lanes, the lanes aligned across
// both rows. See the note on Lane in the header for why this is declared rather
// than flowed.
int ControlStrip::layoutLanes(int width, bool apply) const
{
	// The declared, left-to-right order, minus whatever a mode has hidden
	// wholesale (§6.3's "more" menu in Tall) — see orderFor's own note on
	// why that is isHidden(), not isVisible().
	const QVector<int> idx = orderFor(false);
	struct Line {
		int first = 0, last = 0; // [first, last) into idx, not blocks_
		int laneW[3] = {0, 0, 0};
		int height = 0;
	};
	QVector<Line> lines;

	// Gather the lines first, in the DECLARED order — left to right is the
	// reference panel's own reading order, and it is what an operator learned.
	int i = 0;
	while (i < idx.size()) {
		Line ln;
		ln.first = i;
		int used = 0;
		while (i < idx.size()) {
			const Entry &e = blocks_[idx[i]];
			const int w = e.tall.width();
			if (i > ln.first && (e.startsLine || used + zoneGap() + w > width))
				break;
			const int lane = (int)e.lane;
			ln.laneW[lane] += (ln.laneW[lane] ? zoneGap() : 0) + w;
			ln.height = std::max(ln.height, e.tall.height());
			used += (used ? zoneGap() : 0) + w;
			i++;
		}
		ln.last = i;
		lines << ln;
	}

	// ONE set of lane widths for every line — that is the whole mechanism.
	// Taking each line's own widths would put the middle group of row one at a
	// different x from the middle group of row two, which is the 48 px of
	// near-alignment this replaced.
	int LW = 0, CW = 0, RW = 0;
	for (const Line &ln : lines) {
		LW = std::max(LW, ln.laneW[(int)Lane::Left]);
		CW = std::max(CW, ln.laneW[(int)Lane::Centre]);
		RW = std::max(RW, ln.laneW[(int)Lane::Right]);
	}
	const int need = LW + CW + RW + (CW ? zoneGap() : 0) + (RW ? zoneGap() : 0);
	if (need > width)
		return -1; // no room to tell the lanes apart; pack instead

	// THE CENTRE LANE IS CENTRED IN THE PANEL, AND THE TWO GAPS MAY DIFFER.
	//
	// Centring the whole block instead puts the transport off the panel's
	// middle by exactly half the difference between the outer lanes — and
	// they are not the same width, marks and record being wider than the
	// exports and the speed dial. Fifteen pixels, every time, on the group
	// the operator's hand goes to first.
	//
	// So the transport is placed dead centre and its neighbours are hung off
	// it, each at most kLaneGapMax away. A side that will not fit gives up
	// its gap rather than pushing the middle off centre: the panel has one
	// middle, and this is the group that belongs in it.
	const int centreX = (width - CW) / 2;
	int leftX = centreX - laneGapMax() - LW;
	if (leftX < 0)
		leftX = 0;
	int rightX = centreX + CW + laneGapMax();
	if (rightX + RW > width)
		rightX = std::max(centreX + CW, width - RW);

	// A LANE USED ON ONE LINE ONLY GETS THE HEIGHT OF ALL OF THEM.
	//
	// The wide arrangement is two macro-rows, and each lane normally has a
	// section on both: marks over record, bays over transport, exports over
	// the speed dial. Switch the second bay off and the centre lane loses its
	// top section entirely - so the transport stayed on the lower row with the
	// whole of the upper one empty above it, which is a rectangle of nothing
	// in the middle of the panel and reads as a row that failed to draw.
	//
	// Given both rows the section centres itself in them (see KeyBlock), so
	// the keys sit on the strip's own middle instead of hanging under a void.
	// Nothing moves in the two-bay case, which is the point: this fires only
	// where a lane is genuinely half empty.
	int laneLines[3] = {0, 0, 0};
	for (const Line &ln : lines) {
		bool used[3] = {false, false, false};
		// A LANE COUNTS AS USED ONLY IF SOMETHING IS ACTUALLY IN IT.
		//
		// With one bay the bay-selector section still EXISTS - its keys are
		// hidden and it measures zero - so counting blocks rather than
		// widths made the centre lane look occupied on both macro-rows, and
		// the transport stayed on the lower one with the whole upper one
		// empty above it. The mockup could not show it: there the section is
		// not built at all when B is off, so the count was right there and
		// wrong in the panel.
		for (int k = ln.first; k < ln.last; k++)
			if (blocks_[idx[k]].tall.width() > 0 &&
			    blocks_[idx[k]].tall.height() > 0)
				used[(int)blocks_[idx[k]].lane] = true;
		for (int l = 0; l < 3; l++)
			laneLines[l] += used[l] ? 1 : 0;
	}
	int totalH = 0;
	for (const Line &ln : lines)
		totalH += ln.height;
	totalH += (lines.size() - 1) * (kBandVGap + 2);

	int y = 0;
	const int vgap = kBandVGap + 2;
	for (const Line &ln : lines) {
		if (apply) {
			int x[3];
			x[(int)Lane::Left] = leftX;
			// THE CENTRE LANE IS CENTRED IN ITSELF TOO. Its width is
			// the widest centre group across every line — the
			// transport — so a narrower one on another line (the bay
			// selector) was laid from the lane's LEFT EDGE and came
			// out sitting off to one side of the panel's middle,
			// which is the one place on this strip where being in
			// the middle is the whole point. Left and right lanes
			// keep their edges: those are read as edges.
			x[(int)Lane::Centre] =
				centreX + (CW - ln.laneW[(int)Lane::Centre]) / 2;
			// The right lane ENDS flush, so its sections' right edges
			// line up across the rows even when the lanes hold
			// different keys — which is what puts the speed dial
			// under the exports instead of near them.
			x[(int)Lane::Right] =
				rightX + RW - ln.laneW[(int)Lane::Right];
			// THE RULES SIT ON THE LANE BOUNDARIES, so the one between
			// marks and angles is at the same x as the one between
			// REC and the transport on the row below. Derived from
			// the block edges instead they staggered by 80 px, which
			// reads as a mistake rather than as a division.
			if (ln.laneW[(int)Lane::Centre] > 0 && LW > 0)
				addSeparator((leftX + LW + centreX) / 2, y,
					     ln.height);
			if (ln.laneW[(int)Lane::Right] > 0 && CW > 0)
				addSeparator((centreX + CW + rightX) / 2, y,
					     ln.height);
			for (int k = ln.first; k < ln.last; k++) {
				const Entry &e = blocks_[idx[k]];
				const int lane = (int)e.lane;
				const QSize sz = e.tall;
				// Every section on a line gets the LINE's height,
				// so their captions line up; each is fixed inside
				// itself, so the slack lands under its keys and
				// nothing within it stretches.
				const bool alone = lines.size() > 1 &&
						   laneLines[lane] == 1;
				e.block->setGeometry(x[lane], alone ? 0 : y,
						     sz.width(),
						     alone ? totalH : ln.height);
				x[lane] += sz.width() + zoneGap();
			}
		}
		y += ln.height + vgap;
	}
	return y - vgap;
}

// The folded arrangement. Sections in rank order, packed onto as few lines as
// the width allows, every line starting at x = 0.
int ControlStrip::layoutStack(int width, bool apply) const
{
	const QVector<int> idx = orderFor(true);
	// WIDER THAN THE GAP INSIDE A SECTION, and it is now the only thing
	// dividing one group from the next: the captions are gone in this shape
	// (see KeyBlock::apply). A gap the size of the gap between two key rows
	// would make six groups read as one long list of keys.
	const int vgap = zoneGap() - 2;
	// THE SPINE IS CENTRED, THE SECTIONS ARE NOT. Two different things, and
	// the difference is the whole reason this is two passes: a column of
	// sections each centred on its own width has no edge to be read down, and
	// that is what a stack of "scattered" keys actually is. One left edge for
	// all of them, placed so the block as a whole sits in the middle of the
	// panel, keeps the edge AND stops the keys hugging one side of a dock
	// that is wider than they are.
	int spine = 0;
	if (apply) {
		int widest = 0, w = 0, j = 0;
		while (j < idx.size()) {
			const int start = j;
			w = 0;
			while (j < idx.size()) {
				const QSize sz = blocks_[idx[j]].flat;
				const int next = w + (j > start ? zoneGap() : 0) +
						 sz.width();
				if (j > start && next > width)
					break;
				w = next;
				j++;
			}
			widest = std::max(widest, w);
		}
		spine = std::max(0, (width - widest) / 2);
	}
	int y = 0, i = 0;
	while (i < idx.size()) {
		int lineW = 0, lineH = 0;
		const int first = i;
		while (i < idx.size()) {
			const QSize sz = blocks_[idx[i]].flat;
			const int next = lineW + (i > first ? zoneGap() : 0) +
					 sz.width();
			if (i > first && next > width)
				break;
			lineW = next;
			lineH = std::max(lineH, sz.height());
			i++;
		}
		if (apply) {
			// ONE left edge for every line, at the centred spine
			// worked out above. Not one centring per section: that
			// leaves the column with no edge to be read down, which
			// is what a stack of "scattered" keys actually is.
			int x = spine;
			for (int k = first; k < i; k++) {
				const Entry &e = blocks_[idx[k]];
				if (k > first)
					addSeparator(x - zoneGap() / 2, y, lineH);
				e.block->setGeometry(x, y, e.flat.width(), lineH);
				x += e.flat.width() + zoneGap();
			}
		}
		y += lineH + vgap;
	}
	return y - vgap;
}

int ControlStrip::layoutPacked(int width, bool flat, bool apply) const
{
	const QVector<int> idx = orderFor(flat);
	const int vgap = kBandVGap + 2;
	int y = 0;
	int i = 0;
	while (i < idx.size()) {
		// Gather one line at the natural gap.
		int lineW = 0, lineH = 0, n = 0;
		const int first = i;
		while (i < idx.size()) {
			const Entry &e = blocks_[idx[i]];
			const QSize sz = flat ? e.flat : e.tall;
			const int next = lineW + (n ? zoneGap() : 0) + sz.width();
			// A declared break opens a new line whatever room is
			// left; it is how the wide arrangement keeps its two
			// macro-rows. Folded there is nothing to keep, so the
			// break is ignored and the stack simply flows.
			if (n > 0 && ((!flat && e.startsLine) || next > width))
				break;
			lineW = next;
			lineH = std::max(lineH, sz.height());
			n++;
			i++;
		}
		if (apply) {
			const int extra = std::max(0, width - lineW);
			// THE LEFTOVER GOES INTO THE GAPS, NEVER INTO THE KEYS.
			// Widening a section widens its columns: on the camera
			// matrix that pulled A|B, A and B apart and left the
			// eight slots swimming, and on the speed section it drew
			// six preset keys 250 px wide because the dock happened
			// to be 1000 px. A key that changes size with the window
			// is a key the hand has to find again every time.
			//
			// Spreading the SECTIONS instead reads as a toolbar that
			// fills its bar - which is what the reference panel does,
			// with its marks at one end and its exports at the other.
			//
			// A section ALONE on a line is CENTRED instead: there is
			// nothing to spread it against, and left-aligned in a
			// stack of centred lines it reads as the one that went
			// wrong.
			// CAPPED, same reasoning as kLaneGapMax in layoutLanes: past
			// kZoneGapMax the sections on this line stop reading as a
			// row of groups and start reading as scattered keys. Was
			// uncapped — a stacked line with few sections and a lot of
			// leftover width could spread them arbitrarily far apart.
			const int gap = n > 1 ? std::min(kZoneGapMax,
							  zoneGap() + extra / (n - 1))
					      : zoneGap();
			int x = n > 1 ? 0 : extra / 2;
			for (int k = first; k < i; k++) {
				const Entry &e = blocks_[idx[k]];
				QSize sz = flat ? e.flat : e.tall;
				// Every section on a line gets the LINE's height,
				// so their captions line up; each is fixed inside
				// itself, so the slack lands under its keys and
				// nothing within it stretches.
				e.block->setGeometry(x, y, sz.width(), lineH);
				x += sz.width() + gap;
			}
		}
		y += lineH + vgap;
	}
	return y - vgap;
}

// ---------------------------------------------------------------------------
// ControlStripItem — where the floor and the preference are told apart
// ---------------------------------------------------------------------------

ControlStripItem::ControlStripItem(ControlStrip *s) : QWidgetItem(s), strip_(s) {}

int ControlStripItem::heightForWidth(int w) const
{
	return strip_->tallHeightForWidth(w);
}

int ControlStripItem::minimumHeightForWidth(int w) const
{
	return strip_->minHeightForWidth(w);
}

QSize ControlStripItem::minimumSize() const
{
	return QSize(strip_->minimumSizeHint().width(),
		     strip_->minHeightForWidth(strip_->width() > 0
						       ? strip_->width()
						       : strip_->minimumSizeHint()
								 .width()));
}

QSize ControlStripItem::sizeHint() const
{
	return strip_->sizeHint();
}

void addStrip(QBoxLayout *parent, ControlStrip *s)
{
	s->setParent(parent->parentWidget());
	parent->addItem(new ControlStripItem(s));
}

// ---------------------------------------------------------------------------
// TwoPanelStrip — MARCA | REVIEW, as the redesign draws it
// ---------------------------------------------------------------------------
//
// Two titled panels. Each has a fixed-height HEADER row (the panel's name and,
// for MARCA, REC + clock; for REVIEW, the event id and IN OUTPUT) and a BODY
// of boxed sub-sections whose three rows line up across the divide:
//
//   MARCA (~40%)                     REVIEW (~60%)
//   ┌ Clip rapida ─────┐   row 0     ┌ Riproduzione ┐┌ Modi ──────┐
//   ┌ Clip manuale ────┐   row 1     ┌ Trasporto ───┐┌ Rifinitura ┐
//   ┌ Canali replay ───┐   row 2     ┌ Velocità ────────────────  ┐
//   [ health footer ]               [ green on-air band footer ]
//
// Rows 0 and 1 are taller than row 2 (stretch 2:2:1), the same in both
// panels, so the boxes stay aligned. Wide sets the two panels side by side;
// Short stacks them and REVIEW's grid collapses to one column; Tall swaps
// them behind a REVIEW / MARCA tab bar.

// ── MARCA'S FOOTER: badge + notice (S2, B2, D5) ──────────────────────────

namespace {
// Air either side of the sentence inside its capped box; the text is centred,
// so it is split evenly. Also what keeps a bold face measured before a repolish
// from clipping its last letter.
constexpr int kNoticeSlack = 6;
} // namespace

QLabel *buildMarcaFootRow(QWidget *foot, QWidget *badge)
{
	auto *mfl = new QHBoxLayout(foot);
	mfl->setContentsMargins(0, 0, 0, 0);
	mfl->setSpacing(6);
	// TAS .subfoot{justify-content:center}: «⚠ N» + the notice, centred.
	mfl->addStretch(1);
	if (badge)
		mfl->addWidget(badge, 0, Qt::AlignVCenter);
	auto *notice = new QLabel(foot);
	notice->setObjectName(QStringLiteral("mrNotice"));
	notice->setTextFormat(Qt::PlainText);
	notice->setAlignment(Qt::AlignCenter);
	// Ignored horizontally: what it says changes and its natural width
	// would otherwise be a floor under the whole panel. It still has to GET
	// a width — an Ignored item's hint is zero, so beside two spacers it
	// would get none — hence the stretch that out-bids them, capped by
	// setFooterNotice to the sentence it shows.
	notice->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	notice->setMaximumWidth(0);
	notice->hide();
	mfl->addWidget(notice, 1000, Qt::AlignVCenter);
	mfl->addStretch(1);
	return notice;
}

void setFooterNotice(QLabel *notice, const QWidget *badge, const QString &text)
{
	if (!notice)
		return;
	const bool lit = !text.isEmpty();
	// The sheet colours a lit notice; restyle on the CHANGE only — this is
	// called thirty times a second.
	if (notice->property("notice").toBool() != lit) {
		notice->setProperty("notice", lit);
		notice->style()->unpolish(notice);
		notice->style()->polish(notice);
	}
	notice->ensurePolished();
	const QFontMetrics fm = notice->fontMetrics();

	// What the footer has left beside the badge. A footer never laid out
	// (Tall, MARCA behind REVIEW since the start) has no width yet: the
	// sentence goes whole, and the next pass after it is shown elides it.
	QString shown = text;
	const QWidget *foot = notice->parentWidget();
	int avail = foot ? foot->contentsRect().width() : 0;
	if (lit && avail > 0) {
		if (badge && !badge->isHidden()) {
			const int bw = badge->isVisible() && badge->width() > 0
					       ? badge->width()
					       : badge->sizeHint().width();
			const int sp = foot->layout() ? foot->layout()->spacing() : 0;
			avail -= bw + sp;
		}
		shown = fm.elidedText(text, Qt::ElideRight,
				      std::max(0, avail - kNoticeSlack));
	}
	if (notice->text() != shown)
		notice->setText(shown);
	// The whole sentence, always, while lit: elided or not, the tooltip is
	// where the operator (and a check) reads what was said.
	const QString tip = lit ? text : QString();
	if (notice->toolTip() != tip)
		notice->setToolTip(tip);
	const int want = shown.isEmpty()
				 ? 0
				 : fm.horizontalAdvance(shown) + kNoticeSlack;
	if (notice->maximumWidth() != want)
		notice->setMaximumWidth(want);
	// Hidden when there is nothing to show: a zero-width label would still
	// be charged the row's spacing, and the lone badge would sit 3 px off
	// centre.
	const bool show = !shown.isEmpty();
	if (notice->isHidden() == show)
		notice->setVisible(show);
}

TwoPanelStrip::TwoPanelStrip(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("mrStrip"));
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(4);

	tabs_ = new QTabBar(this);
	tabs_->setObjectName(QStringLiteral("mrPanelTabs"));
	tabs_->setDrawBase(false);
	tabs_->setExpanding(false);
	tabs_->setFocusPolicy(Qt::NoFocus);
	tabs_->addTab(QStringLiteral("REVIEW"));
	tabs_->addTab(QStringLiteral("MARCA"));
	tabs_->hide();
	outer->addWidget(tabs_);

	marca_ = new QWidget(this);
	marca_->setObjectName(QStringLiteral("mrMarca"));
	marcaCol_ = new QVBoxLayout(marca_);
	marcaCol_->setContentsMargins(8, 8, 8, 8); // .sub{padding:8px}
	marcaCol_->setSpacing(14); // bodies rows gap (.livebody gap:14px)

	review_ = new QWidget(this);
	review_->setObjectName(QStringLiteral("mrReview"));
	reviewCol_ = new QVBoxLayout(review_);
	reviewCol_->setContentsMargins(8, 8, 8, 8);
	reviewCol_->setSpacing(14);

	// REVIEW's body is a 2-column grid in Wide (Riproduzione | Modi over
	// Trasporto | Rifinitura, Velocità spanning) and one column otherwise.
	reviewGrid_ = new QWidget(review_);
	reviewGrid_->setObjectName(QStringLiteral("mrReviewGrid"));
	grid_ = new QGridLayout(reviewGrid_);
	grid_->setContentsMargins(0, 0, 0, 0);
	grid_->setHorizontalSpacing(14); // .reviewbody gap:14px
	grid_->setVerticalSpacing(14);

	auto *body = new QWidget(this);
	row_ = new QHBoxLayout(body);
	row_->setContentsMargins(0, 0, 0, 0);
	row_->setSpacing(10); // .subs{gap:10px} between MARCA and REVIEW
	row_->addWidget(marca_, 2);
	row_->addWidget(review_, 3);
	outer->addWidget(body, 1);

	connect(tabs_, &QTabBar::currentChanged, this,
		[this](int) { relayout(); });
}

void TwoPanelStrip::setHeaders(KeyBlock *marcaHeader, KeyBlock *reviewHeader)
{
	marcaHeader_ = marcaHeader;
	reviewHeader_ = reviewHeader;
	// One MINIMUM height for both, so the line under the header sits at the
	// same y on MARCA and REVIEW even though only MARCA carries a key (REC)
	// up there (spec §4). Not fixed: the full-screen gallery view grows
	// every key, and a fixed header would clip REC there.
	for (KeyBlock *h : {marcaHeader, reviewHeader}) {
		if (!h)
			continue;
		h->setParent(h == marcaHeader ? marca_ : review_);
		h->setObjectName(QStringLiteral("mrPanelHeader"));
		h->setMinimumHeight(kHeaderH);
	}
	if (marcaHeader_)
		marcaCol_->insertWidget(0, marcaHeader_);
	if (reviewHeader_)
		reviewCol_->insertWidget(0, reviewHeader_);
}

void TwoPanelStrip::addToMarca(KeyBlock *b)
{
	if (!b)
		return;
	// After the header, in order; the footer (setFooters) comes last.
	// 12:12:7 — the bodies' 1.2fr/1.2fr/0.7fr, in Qt integers.
	const int i = (int)marcaBlocks_.size();
	marcaCol_->insertWidget(1 + i, b);
	marcaCol_->setStretch(1 + i, i < 2 ? 12 : 7);
	marcaBlocks_ << b;
}

void TwoPanelStrip::setReviewGrid(KeyBlock *playback, KeyBlock *modes,
				  KeyBlock *transport, KeyBlock *trim,
				  KeyBlock *speed)
{
	reviewBoxes_ = {playback, modes, transport, trim, speed};
	for (KeyBlock *b : reviewBoxes_)
		if (b) {
			b->setParent(reviewGrid_);
			reviewBlocks_ << b;
		}
	reviewCol_->insertWidget(1, reviewGrid_); // after the header
	reviewCol_->setStretch(1, 1);
	applyGrid();
}

void TwoPanelStrip::applyGrid()
{
	// Take everything out (widgets survive — they are parented to
	// reviewGrid_), then place by the current mode.
	while (QLayoutItem *it = grid_->takeAt(0))
		delete it;
	for (int c = 0; c < 4; c++)
		grid_->setColumnStretch(c, 0);
	for (int r = 0; r < 4; r++)
		grid_->setRowStretch(r, 0);

	const bool twoCol = mode_ == PanelMode::Wide;
	KeyBlock *pb = reviewBoxes_.value(0), *md = reviewBoxes_.value(1),
		 *tr = reviewBoxes_.value(2), *rf = reviewBoxes_.value(3),
		 *sp = reviewBoxes_.value(4);
	if (twoCol) {
		if (pb)
			grid_->addWidget(pb, 0, 0);
		if (md)
			grid_->addWidget(md, 0, 1);
		if (tr)
			grid_->addWidget(tr, 1, 0);
		if (rf)
			grid_->addWidget(rf, 1, 1);
		if (sp)
			grid_->addWidget(sp, 2, 0, 1, 2);
		// Playback column takes the slack, Modi keeps its drawn 152px:
		// .reviewbody{grid-template-columns:1fr auto}.
		grid_->setColumnStretch(0, 1);
		grid_->setColumnStretch(1, 0);
		grid_->setRowStretch(0, 12);
		grid_->setRowStretch(1, 12);
		grid_->setRowStretch(2, 7);
	} else {
		int r = 0;
		for (KeyBlock *b : {pb, md, tr, rf, sp})
			if (b)
				grid_->addWidget(b, r++, 0);
		grid_->setColumnStretch(0, 1);
	}
}

void TwoPanelStrip::setFooters(QWidget *marcaFoot, QWidget *reviewFoot)
{
	marcaFoot_ = marcaFoot;
	reviewFoot_ = reviewFoot;
	if (marcaFoot_) {
		marcaFoot_->setParent(marca_);
		marcaCol_->addWidget(marcaFoot_);
	}
	if (reviewFoot_) {
		reviewFoot_->setParent(review_);
		reviewCol_->addWidget(reviewFoot_);
	}
}

void TwoPanelStrip::setMarcaCentered(bool centred)
{
	// Column order is fixed — header, the three blocks in addToMarca order,
	// footer — so the spacers live at known indices and come back out
	// symmetrically. Stretches are ours too (12:12:7 from addToMarca):
	// centred they go quiet and the blocks stand at natural height.
	if (centred == marcaCentred_)
		return;
	marcaCentred_ = centred;
	if (centred) {
		marcaCol_->insertStretch(4, 1);
		marcaCol_->insertStretch(1, 1);
		marcaCol_->setStretch(2, 0);
		marcaCol_->setStretch(3, 0);
		marcaCol_->setStretch(4, 0);
	} else {
		delete marcaCol_->takeAt(5);
		delete marcaCol_->takeAt(1);
		marcaCol_->setStretch(1, 12);
		marcaCol_->setStretch(2, 12);
		marcaCol_->setStretch(3, 7);
	}
	marca_->updateGeometry();
}

void TwoPanelStrip::setMode(PanelMode m)
{
	mode_ = m;
	relayout();
}

void TwoPanelStrip::setMarcaAlert(bool on)
{
	if (on == marcaAlert_)
		return;
	marcaAlert_ = on;
	updateMarcaTab();
}

// TALL: MARCA sits behind REVIEW, so a notice or a health finding in its footer
// is out of sight. Flag the MARCA tab while there is one and MARCA is not the
// current tab: «MARCA •», and `alert` on the tab bar for the sheet's warn
// colour. Called from relayout(), which the tab bar's currentChanged and every
// setMode run — so becoming current clears it in the same pass.
void TwoPanelStrip::updateMarcaTab()
{
	const bool flag = marcaAlert_ && mode_ == PanelMode::Tall &&
			  tabs_->currentIndex() != 1;
	const QString text = flag ? QStringLiteral("MARCA •")
				  : QStringLiteral("MARCA");
	if (tabs_->tabText(1) != text)
		tabs_->setTabText(1, text);
	if (tabs_->property("alert").toBool() != flag) {
		tabs_->setProperty("alert", flag);
		// A property selector is matched at polish time.
		tabs_->style()->unpolish(tabs_);
		tabs_->style()->polish(tabs_);
		tabs_->update();
	}
}

void TwoPanelStrip::relayout()
{
	const bool tall = mode_ == PanelMode::Tall;
	const bool wide = mode_ == PanelMode::Wide;

	tabs_->setVisible(tall);
	row_->setDirection(wide ? QBoxLayout::LeftToRight
				: QBoxLayout::TopToBottom);
	row_->setStretch(0, wide ? 2 : 0);
	row_->setStretch(1, wide ? 3 : 0);

	if (tall) {
		const bool showReview = tabs_->currentIndex() != 1;
		marca_->setVisible(!showReview);
		review_->setVisible(showReview);
	} else {
		marca_->setVisible(true);
		review_->setVisible(true);
	}

	// Every sub-box wears its wide shape only in Wide; folded otherwise.
	// Short goes one further: sections that declared a compact packing
	// wear it (captions off, shared lines) — the ~210px column cannot
	// hold full boxes (artifact Short wireframe).
	for (KeyBlock *b : marcaBlocks_)
		b->setFlat(!wide);
	for (KeyBlock *b : reviewBlocks_)
		b->setFlat(!wide);
	const bool compact = mode_ == PanelMode::Short;
	for (KeyBlock *b : marcaBlocks_)
		b->setCompact(compact);
	for (KeyBlock *b : reviewBlocks_)
		b->setCompact(compact);
	// SHORT HIDES WHAT HOTKEYS AND CHIPS ALREADY COVER. Every trim nudge
	// has a hotkey (TrimIn/Out ×/±1s/±5s, SetIn/OutHere) and the chips say
	// 25–125, so the trim box and the slider row cost two lines the ~210px
	// column cannot spare. What has no other door stays: modes (LOOP and
	// music have no hotkeys), the band (skip + status) and the badge.
	if (KeyBlock *trim = reviewBoxes_.value(3, nullptr))
		trim->setVisible(mode_ != PanelMode::Short);
	if (auto *sl = findChild<QSlider *>(QStringLiteral("mrSpeed")))
		sl->setVisible(mode_ != PanelMode::Short);
	if (auto *tick = findChild<QWidget *>(QStringLiteral("mrSpeedTick")))
		tick->setVisible(mode_ != PanelMode::Short);

	applyGrid();
	updateMarcaTab();
	updateGeometry();
	update();
}

void TwoPanelStrip::refreshAllBlocks()
{
	for (KeyBlock *b : marcaBlocks_)
		b->refresh();
	for (KeyBlock *b : reviewBlocks_)
		b->refresh();
	// The header blocks too — REC's height is pinned by KeyBlock::apply()
	// and the full-screen gallery view relies on a refresh to grow it.
	if (marcaHeader_)
		marcaHeader_->refresh();
	if (reviewHeader_)
		reviewHeader_->refresh();
	updateGeometry();
}

void TwoPanelStrip::blockChanged(KeyBlock *b)
{
	if (b)
		b->refresh();
	updateGeometry();
	update();
}

int TwoPanelStrip::wantedHeight() const
{
	const int mH = marca_->layout()->sizeHint().height();
	const int rH = review_->layout()->sizeHint().height();
	int h = 0;
	if (mode_ == PanelMode::Wide)
		h = std::max(mH, rH);
	else if (mode_ == PanelMode::Short)
		h = mH + rH + (row_ ? row_->spacing() : 0);
	else
		h = tabs_->sizeHint().height() + 4 + std::max(mH, rH);
	const QMargins mg = layout()->contentsMargins();
	return h + mg.top() + mg.bottom();
}

QSize TwoPanelStrip::sizeHint() const
{
	return QSize(QWidget::sizeHint().width(), wantedHeight());
}

QSize TwoPanelStrip::minimumSizeHint() const
{
	return QSize(QWidget::minimumSizeHint().width(), wantedHeight());
}

void TwoPanelStrip::paintEvent(QPaintEvent *)
{
	if (mode_ != PanelMode::Wide || !marca_->isVisible())
		return;
	const int x = (marca_->geometry().right() + review_->geometry().left()) /
		      2;
	QPainter p(this);
	p.setPen(QColor(255, 255, 255, 28));
	p.drawLine(x, marca_->geometry().top() + 2, x,
		   marca_->geometry().bottom() - 2);
}

} // namespace multireplay
