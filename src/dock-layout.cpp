#include "dock-layout.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace multireplay {

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

// ---------------------------------------------------------------------------
// KeyBlock
// ---------------------------------------------------------------------------

KeyBlock::KeyBlock(const QString &caption, QWidget *parent)
	: QWidget(parent), caption_(caption)
{
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
	v->addWidget(body_, 0);
	// The keys take the height their rows need and not a pixel more; if the
	// line this section sits on is taller (a neighbour has more rows), the
	// slack goes here, under the keys, instead of between them.
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
	const int capH = cap_ ? cap_->sizeHint().height() + 2 : 0;
	return capH + rows * kKeyH + (rows - 1) * kBandVGap;
}

void KeyBlock::setStretchColumns(int firstCol, int lastCol)
{
	stretchFrom_ = firstCol;
	stretchTo_ = lastCol;
	applied_ = false;
	apply();
}

void KeyBlock::apply()
{
	if (applied_)
		return;
	applied_ = true;

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
	const bool was = e.block->isFlat();
	e.block->setFlat(true);
	e.flat = e.block->sizeHint();
	e.block->setFlat(false);
	e.tall = e.block->sizeHint();
	e.block->setFlat(was);
}

void ControlStrip::addBlock(KeyBlock *b, bool startsLine, int rank)
{
	b->setParent(this);
	Entry e;
	e.block = b;
	e.startsLine = startsLine;
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
	int w = 0;
	for (const Entry &e : blocks_)
		w = std::max(w, std::max(e.tall.width(), kMinBlockWidth));
	// Height: one line's worth. The real floor is width-dependent and is
	// answered by minHeightForWidth() through ControlStripItem; a widget's
	// minimumSizeHint cannot ask about width, so it must not pretend to.
	return QSize(w, blocks_.isEmpty() ? 0 : blocks_.first().flat.height());
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
	// The BETTER of the two shapes at this width, because the strip is free
	// to wear either. Flat is the short one under a wide dock, where its
	// sections sit on one line; under a narrow dock its long rows wrap into
	// more lines than the compact blocks do, and there it is the taller of the
	// two. Assuming flat is always the shorter one cost the panel 90 px of
	// floor it did not need — the mockup measured it.
	const int tall = layoutLines(w, false, false);
	return flatFits(w) ? std::min(layoutLines(w, true, false), tall) : tall;
}

int ControlStrip::tallHeightForWidth(int w) const
{
	return layoutLines(w, false, false);
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
	const bool want = !flatFits(width());
	if (want != flat_)
		applyShape(want);
	layoutLines(width(), flat_, true);

	// A WIDTH CHANGE CHANGES THE FLOOR, so the parent has to be told: how
	// short this strip may be depends on how wide it is - six sections side
	// by side need two lines, folded and stacked they need four.
	if (e->oldSize().width() != width())
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
			const int gap = n > 1 ? kZoneGap + extra / (n - 1)
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
