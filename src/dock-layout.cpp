#include "dock-layout.hpp"

#include <QGridLayout>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QAbstractButton>
#include <QStyle>
#include <QPushButton>
#include <QSizePolicy>
#include <QPainter>
#include <QStringList>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

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

void AspectBox::relayout()
{
	if (!pic_)
		return;
	// THE BAND GOES BEFORE THE SHAPE DOES. Squeezed under the OBS preview a
	// tile can end up shorter than its own naming band, and the first
	// version answered that by giving the picture the whole box — which is
	// the one thing this class exists to prevent. A 12 px band on an 11 px
	// box was not telling anybody anything; the ratio still is.
	int availH = height();
	const bool room = tag_ && availH >= kTagH + 10;
	if (room)
		availH -= kTagH;
	if (tag_)
		tag_->setVisible(room);
	if (availH < 2 || width() < 4) {
		pic_->setGeometry(0, 0, std::max(0, width()),
				  std::max(0, height()));
		return;
	}
	const int w = std::max(1, std::min(width(), availH * rw_ / rh_));
	const int h = std::max(1, w * rh_ / rw_);
	const int x = (width() - w) / 2;
	const int y = (availH - h) / 2;
	pic_->setGeometry(x, y, w, h);
	if (room)
		tag_->setGeometry(x, y + h, w, kTagH);
}


// ---------------------------------------------------------------------------
// The camera block — see the note in the header
// ---------------------------------------------------------------------------

TileBlock tileBlockFor(int paneW, int bays, int n, int gap, int maxH)
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

	// ONE ROW UP TO THREE, TWO ROWS BEYOND — DECLARED, NOT SCORED.
	//
	//     1..3 cameras   one row,  n columns    A | C1 | C2 | C3
	//     4              two rows, 2 columns
	//     5, 6           two rows, 3 columns
	//     7, 8           two rows, 4 columns
	//
	// which is ceil(n/2) columns past three. NEVER MORE THAN TWO ROWS: the
	// cameras stand beside the bays, and a third row makes each of them
	// smaller than the glance they exist for. Declared, because "three
	// across, then four" is a decision about how a rig is read and a score
	// agrees with it only by accident.
	const int cols = (n <= 3) ? n : (n + 1) / 2;
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
	// two cameras beside A become three equal pictures across the row, eight
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
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(2);

	// The caption sits ABOVE the keys. It names the group instead of
	// competing with the first key for the same line, and it lets every
	// section start its keys at the same y.
	// AN EMPTY CAPTION MEANS NO CAPTION, and most sections use it. The
	// reference panel labels nothing but its mark keys: its groups are told
	// apart by the space between them, and the result is a dense strip that
	// reads at a glance instead of a column of headings with keys under them.
	// A caption line also costs ~16 px on a panel that is short of height.
	//
	// THE STRETCH GOES ABOVE THE CAPTION, not between it and the keys. It
	// used to sit between, so on a deep line the caption stayed pinned to the
	// top edge while its keys drifted to the middle - and a heading a
	// centimetre away from what it names has stopped naming it.
	v->addStretch(1);
	if (!caption_.isEmpty()) {
		cap_ = new QLabel(caption_.toUpper(), this);
		cap_->setObjectName(QStringLiteral("mrZoneTitle"));
		cap_->setWordWrap(false);
		cap_->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
		cap_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		v->addWidget(cap_, 0);
	}

	body_ = new QWidget(this);
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
	v->addWidget(body_, 0);
	v->addStretch(1);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
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

void KeyBlock::refresh()
{
	applied_ = false;
	apply();
}

int KeyBlock::rows() const
{
	const BlockShape &s = (flatActive_ && !flat_.isEmpty()) ? flat_ : tall_;
	return (int)s.size();
}

int KeyBlock::shapeHeight(bool flat) const
{
	const BlockShape &s = (flat && !flat_.isEmpty()) ? flat_ : tall_;
	const int rows = std::max(1, (int)s.size());
	// The caption height is a CONSTANT, not the label's own sizeHint: this
	// function is what the strip measures with, and apply() is what draws it.
	// Two ways of asking the same question is two answers waiting to differ.
	const int capH = (cap_ && !flat) ? kCaptionH + 2 : 0;
	const int keyH = flat ? kKeyFoldedH : kKeyH;
	return capH + rows * keyH + (rows - 1) * kBandVGap;
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
		cap_->setVisible(!flatActive_ && !sectionHidden_);
		cap_->setFixedHeight(kCaptionH);
	}

	const BlockShape &s = (flatActive_ && !flat_.isEmpty()) ? flat_ : tall_;

	// Take everything out first. Deleting the layout ITEMS leaves the widgets
	// alive and parented to body_, which is the point: the checked state, the
	// tally and every connection live on those widgets, so a shape change
	// re-places them and never rebuilds them.
	while (QLayoutItem *it = grid_->takeAt(0))
		delete it;
	for (int c = 0; c < 16; c++)
		grid_->setColumnStretch(c, 0);

	int r = 0;
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
				const int h = flatActive_ ? kKeyFoldedH : kKeyH;
				const int pinned = cell.rowSpan * h +
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
		r++;
	}
	if (stretchFrom_ >= 0)
		for (int c = stretchFrom_; c <= stretchTo_; c++)
			grid_->setColumnStretch(c, 1);
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
	for (int i = 0; i < blocks_.size(); i++)
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
	struct Line {
		int first = 0, last = 0; // [first, last)
		int laneW[3] = {0, 0, 0};
		int height = 0;
	};
	QVector<Line> lines;

	// Gather the lines first, in the DECLARED order — left to right is the
	// reference panel's own reading order, and it is what an operator learned.
	int i = 0;
	while (i < blocks_.size()) {
		Line ln;
		ln.first = i;
		int used = 0;
		while (i < blocks_.size()) {
			const Entry &e = blocks_[i];
			const int w = e.tall.width();
			if (i > ln.first && (e.startsLine || used + kZoneGap + w > width))
				break;
			const int lane = (int)e.lane;
			ln.laneW[lane] += (ln.laneW[lane] ? kZoneGap : 0) + w;
			ln.height = std::max(ln.height, e.tall.height());
			used += (used ? kZoneGap : 0) + w;
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
	const int need = LW + CW + RW + (CW ? kZoneGap : 0) + (RW ? kZoneGap : 0);
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
	int leftX = centreX - kLaneGapMax - LW;
	if (leftX < 0)
		leftX = 0;
	int rightX = centreX + CW + kLaneGapMax;
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
			if (blocks_[k].tall.width() > 0 &&
			    blocks_[k].tall.height() > 0)
				used[(int)blocks_[k].lane] = true;
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
				const Entry &e = blocks_[k];
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
				x[lane] += sz.width() + kZoneGap;
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
	const int vgap = kZoneGap - 2;
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
				const int next = w + (j > start ? kZoneGap : 0) +
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
			const int next = lineW + (i > first ? kZoneGap : 0) +
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
					addSeparator(x - kZoneGap / 2, y, lineH);
				e.block->setGeometry(x, y, e.flat.width(), lineH);
				x += e.flat.width() + kZoneGap;
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
			const int next = lineW + (n ? kZoneGap : 0) + sz.width();
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
							  kZoneGap + extra / (n - 1))
					      : kZoneGap;
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

} // namespace multireplay
