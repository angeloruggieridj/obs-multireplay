// dock-layout.hpp — how the panel's keys are arranged, with no OBS in it.
//
// WHY THIS IS A FILE OF ITS OWN. Every arrangement bug this panel has had was a
// bug about SPACE — a band that stretched when the dock grew, a matrix that
// would not shrink when it had to, a section that fitted at one size and fell
// apart at another — and none of them are visible from the code that builds the
// keys. They are visible when you resize the thing. So the arrangement lives
// here, free of libobs, and a mockup (tools/dock-mockup) drives the SAME code
// through every size in a second, instead of a four-minute run of OBS per look.
//
// THE MODEL, in three ideas:
//
//  1. A SECTION (KeyBlock) is a captioned group of keys that knows TWO shapes of
//     itself: TALL, several rows deep, and FLAT, one or two rows. Both are
//     declared by hand at the call site, because they are designs, not
//     arithmetic: "IN / OUT / Annulla over -5s / -10s / -20s" is a sentence about
//     marking, and a reflow algorithm would only ever get it right by accident.
//
//  2. THE SHAPE FOLLOWS THE HEIGHT, THE WRAPPING FOLLOWS THE WIDTH. A strip of
//     sections in a tall dock (portrait, or docked down one side) uses TALL
//     sections and lets them wrap onto several lines — height is what it has. In
//     a short wide dock (landscape, docked under the preview) it uses FLAT
//     sections, which is the only honest answer when there are 60 px of height
//     to put a control strip in.
//
//  3. NOTHING STRETCHES BY ACCIDENT. A key is 28 px tall, always (the style
//     sheet's rules add up to it, deliberately). A section is fixed in height,
//     so spare height goes to the picture and the event list, which can use it.
//     Spare WIDTH goes to the one section per line that says it wants it, so the
//     strip ends flush instead of trailing off into dead space.
#pragma once

#include <QBoxLayout>
#include <QLayout>
#include <QLayoutItem>
#include <QList>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QLabel;
class QPushButton;

namespace multireplay {

// ONE HEIGHT FOR EVERY KEY, and it is arithmetic rather than taste: the style
// sheet states each key's min-height as 28 - 2*padding - 2*border, so every rule
// lands on the same 28. A row pitch built on it lines the sections up with each
// other.
inline constexpr int kKeyH = 28;
// Between two rows of one section.
inline constexpr int kBandVGap = 4;
// Between two sections, against the 4-6 px between two keys inside one. A gap
// the size of the gap inside a group makes two groups look like one group, and
// then the caption is the only thing saying otherwise — a label doing work the
// layout should have done.
inline constexpr int kZoneGap = 14;
// …and how far apart they may be pushed when a wide dock has width to spare.
// Past this the sections stop reading as a row of groups and start reading as
// scattered keys.
inline constexpr int kZoneGapMax = 56;
// No section is allowed to demand more width than this of the panel: a section
// wider than the dock is a dock that cannot be narrowed. Sections that hold more
// than this (the camera matrix, eight slots wide) shrink their keys instead.
inline constexpr int kMinBlockWidth = 220;

// ---------------------------------------------------------------------------
// FlowLayout — a row of controls that WRAPS instead of squeezing
// ---------------------------------------------------------------------------
//
// A QHBoxLayout answers "not enough width" by taking it off the widgets until
// the glyphs are unreadable and the hit target is smaller than a fingertip,
// which is precisely how a transport key stops being pressable in the middle of
// a match. The honest answer to a narrow panel is another line.
//
// The leftover width of each line is GIVEN AWAY rather than left at the right
// edge: sections that declare Expanding share it, the rest keep the width they
// asked for. A band that stops short of the edge reads as a panel that is not
// finished, and the space is real — the speed dial is dragged, the camera matrix
// has eight slots to show.
class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent, int hSpacing = 6, int vSpacing = 4);
	~FlowLayout() override;

	void addItem(QLayoutItem *item) override;
	int count() const override;
	QLayoutItem *itemAt(int i) const override;
	QLayoutItem *takeAt(int i) override;
	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int w) const override;
	void setGeometry(const QRect &r) override;
	QSize sizeHint() const override;
	QSize minimumSize() const override;

private:
	static bool grows(const QLayoutItem *it);
	int doLayout(const QRect &rect, bool testOnly) const;

	QList<QLayoutItem *> items_;
	int hSpace_;
	int vSpace_;
};

// A widget whose only job is to hold a FlowLayout and report its own height as a
// function of its width. Without the height-for-width policy the parent asks for
// sizeHint() once, at a width the widget does not end up with, and the last
// wrapped line is drawn outside it.
QWidget *flowBand(QWidget *parent, const QList<QWidget *> &children,
		  int hSpacing = kZoneGap);

// Mark the one section per line that absorbs the width nobody claimed.
QWidget *stretchyZone(QWidget *zone);

// A grid for one section's keys: no margins, the shared row pitch, and FIXED IN
// HEIGHT — a grid handed more height than it needs shares it out among its rows,
// which is how a dock dragged taller pulled a block of keys apart.
QGridLayout *bandGrid(QWidget *host);

// ONE WIDTH for a set of keys, taken from the widest of them, so the columns of
// a section line up instead of nearly lining up. Measured after ensurePolished()
// because the font comes from the style sheet.
void equaliseKeyWidths(const QList<QPushButton *> &keys);

// ---------------------------------------------------------------------------
// KeyBlock — one captioned section, in two declared shapes
// ---------------------------------------------------------------------------

struct Cell {
	QWidget *w = nullptr;
	int span = 1;      // columns
	int rowSpan = 1;   // rows: a group that stands beside several of them
	bool grow = true;  // fills its cell rather than sitting at its own size

	Cell() = default;
	Cell(QWidget *widget, int columns = 1, bool fill = true, int rows = 1)
		: w(widget), span(columns), rowSpan(rows), grow(fill)
	{
	}
};

// One arrangement of a section: rows of cells, in reading order.
using BlockShape = QVector<QVector<Cell>>;

class KeyBlock : public QWidget {
public:
	KeyBlock(const QString &caption, QWidget *parent);

	// Both arrangements are declared by hand — see the note at the top.
	// `flat` may be empty, which means "this section has only one shape".
	void setShapes(const BlockShape &tall, const BlockShape &flat);
	// Re-lay the keys. Cheap and idempotent: it does nothing unless the shape
	// actually changed.
	void setFlat(bool flat);
	bool isFlat() const { return flatActive_; }
	// Rows of the shape currently applied.
	int rows() const;
	// What this section needs in one shape or the other — caption, rows, and
	// the gutters between them. The strip asks for the FLAT one to work out
	// how short the panel may be made.
	int shapeHeight(bool flat) const;
	// Give these columns of the current shape the leftover width, in equal
	// parts (the camera matrix uses it so its eight slots share the room).
	void setStretchColumns(int firstCol, int lastCol);
	// Re-apply the current shape: call after showing or hiding cells, since a
	// hidden cell that is not retained changes the row structure.
	void refresh();

private:
	void apply();

	QString caption_;
	QLabel *cap_ = nullptr;
	QWidget *body_ = nullptr;
	QGridLayout *grid_ = nullptr;
	BlockShape tall_, flat_;
	bool flatActive_ = false;
	bool applied_ = false;
	int stretchFrom_ = -1, stretchTo_ = -1;
};

// ---------------------------------------------------------------------------
// ControlStrip — the sections, and the one decision they share
// ---------------------------------------------------------------------------
//
// It owns no keys. It holds the sections, wraps them by width (FlowLayout), and
// switches every one of them between TALL and FLAT together — together, because
// sections of different depths standing side by side is exactly the "shifted"
// look this whole file exists to remove.
// THE SIZE HINTS ARE THE WHOLE DESIGN, so they are worth stating plainly:
//
//   minimumSizeHint = the FLAT shape   → "you may always make me this short"
//   sizeHint        = the TALL shape   → "give me this much and I will use it"
//
// and the strip wears whichever shape fits the height it was actually handed.
//
// Getting this wrong is what pinned the panel. The first version reported the
// height of the shape it was CURRENTLY wearing — so a tall strip told the dock
// it could never be shorter than a tall strip, the dock could never reach the
// height at which the strip would have gone flat, and it therefore never did.
// The chicken and the egg, in a size hint. (And the minimum leaks through
// heightForWidth, not through minimumSizeHint: QLayoutItem::minimumHeightForWidth
// falls back to heightForWidth, so a widget that answers height-for-width has
// already told the layout its floor. This one does its own geometry instead.)
class ControlStrip : public QWidget {
public:
	explicit ControlStrip(QWidget *parent);

	// `startsLine` opens a new line whatever room is left on the current one.
	// It is how the panel keeps the arrangement an operator learned: REC at
	// the left of its line, playback in the middle of it, speed at the right.
	// Pure flow would put those three wherever they happened to fit, and on a
	// wide dock that is the fourth, fifth and sixth thing in a row of six.
	// `rank` is the section's place when the strip is FOLDED, where the
	// sections become a stack and a stack has a top. Lower is higher up. In
	// the wide arrangement the declared order is used instead, because there
	// the sections read left to right and that order is the reference
	// panel's own.
	void addBlock(KeyBlock *b, bool startsLine = false, int rank = 0);
	bool isFlat() const { return flat_; }

	// The two numbers a parent layout needs, and they are DIFFERENT numbers
	// at the same width — which is the reason this class exists and the reason
	// it is added to its parent through ControlStripItem below:
	//
	//   minHeightForWidth  the shorter of the two shapes  → "I can live here"
	//   tallHeightForWidth the tall shape                 → "give me this if you can"
	//
	// A widget cannot say both through the normal channel: Qt derives a
	// widget's minimum-height-for-width FROM its height-for-width, so a plain
	// child answering "112" at 1500 px also promises never to be shorter, and
	// a child answering "350" at 400 px demands 350 at every width. The first
	// version of this strip did the second thing, and the panel showed 350 px
	// of black between the keys and the event list on a wide dock.
	// True when every section fits the width in its flat shape. A flat row is
	// one long line and does not wrap, so choosing flat without asking cuts
	// keys off the right-hand edge.
	bool flatFits(int w) const;
	int minHeightForWidth(int w) const;
	int tallHeightForWidth(int w) const;
	// Re-measure a section that changed inside (a camera appeared, the second
	// bay was switched off) and lay the strip out again.
	void blockChanged(KeyBlock *b);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	struct Entry {
		KeyBlock *block = nullptr;
		bool startsLine = false;
		int rank = 0;     // order when folded; see addBlock
		QSize tall, flat; // measured once, per shape
	};

	// The blocks in the order the current arrangement wants them.
	QVector<int> orderFor(bool flat) const;

	// Runs the same wrapping in both roles: `apply` false only measures.
	int layoutLines(int width, bool flat, bool apply) const;
	void measure(Entry &e);
	void applyShape(bool flat);

	mutable QVector<Entry> blocks_;
	bool flat_ = false;
};

// The layout item that carries a ControlStrip into a parent layout, and the
// only place the two heights above are told apart. Use addStrip() rather than
// addWidget() — a strip added as a plain widget is a strip whose floor is its
// preference, which is the bug this whole arrangement is built around.
class ControlStripItem : public QWidgetItem {
public:
	explicit ControlStripItem(ControlStrip *s);
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override;
	int minimumHeightForWidth(int w) const override;
	QSize minimumSize() const override;
	QSize sizeHint() const override;

private:
	ControlStrip *strip_;
};

void addStrip(QBoxLayout *parent, ControlStrip *s);

} // namespace multireplay
