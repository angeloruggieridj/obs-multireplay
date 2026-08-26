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
//  3. NOTHING STRETCHES BY ACCIDENT. A key is 26 px tall, always (the style
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

#include <functional>

class QGridLayout;
class QLabel;
class QPushButton;

namespace multireplay {

// ONE HEIGHT FOR EVERY KEY, and it is arithmetic rather than taste: the style
// sheet states each key's min-height as 26 - 2*padding - 2*border, so every rule
// lands on the same 26. A row pitch built on it lines the sections up with each
// other.
//
// IT WAS 28. The two pixels were bought back for the event list: five or six key
// rows stand between the two macro-rows and the folded stack, so this is 10-14
// px of table on every arrangement, and 26 is still well above the size at which
// a key stops being easy to hit in a hurry.
inline constexpr int kKeyH = 26;
// …and 22 STACKED. In a column the strip is six groups one under another, so
// every key row is charged to the event list — which on that panel is the thing
// the operator is actually reading. 22 px is still a comfortable target and
// across the eleven rows of the folded stack it is the difference between the
// pictures getting the height their aspect asks for and being squashed into
// black bars.
// SIDE BY SIDE THE KEYS STAY 26: there the strip is two lines and the height
// costs the list almost nothing, so there is nothing to buy.
inline constexpr int kKeyFoldedH = 22;
// Between two rows of one section.
inline constexpr int kBandVGap = 4;
// A SECTION'S CAPTION, and it has two heights because it is worth two different
// amounts. Side by side, a caption is how the eye tells six groups apart at a
// glance and it is cheap — one line across the whole strip. STACKED, there is
// one per group down a narrow column, and six of them are six times the cost
// for a job the gap between the groups is already doing most of. So it stays
// (the grouping is the point) and it stops taking a full line of text to say so.
inline constexpr int kCaptionH = 13;
inline constexpr int kCaptionFoldedH = 11;
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
// PanelMode — THREE DECLARED ARRANGEMENTS OF THE WHOLE PANEL
// ---------------------------------------------------------------------------
//
// The same argument as the two shapes of a KeyBlock, one floor up. A replay
// panel is not read, it is used from memory: the hand goes where the key was
// last time. A fluid layout is a panel with a different memory at every width,
// so the answer is not reflow — it is a small number of arrangements, each one
// designed, chosen by the shape of the room the panel was given.
//
//   Wide   undocked, full screen, or a big dock: pictures across the top, list
//          under them, the control strip in two macro-rows.
//   Short  docked UNDER the OBS preview — wide and shallow. Stacking pictures
//          on top of the list cannot afford both, so they go side by side.
//   Tall   docked down one SIDE — a single narrow column. The multiview becomes
//          a filmstrip and the control strip becomes a stack.
//
// Nothing is dropped in any of them; what changes is rank.
enum class PanelMode { Wide, Short, Tall };

// Narrower than this and the panel is a column, whatever its height.
inline constexpr int kTallMaxWidth = 760;
// Shorter than this (and wide enough not to be Tall) and the pictures cannot
// sit above the list.
//
// IT WAS 470, AND THAT BECAME A TRAP. The threshold has to be above the WIDE
// arrangement's own minimum height, or the panel can never get short enough to
// be told to change: it sits at its floor, still wide, refusing to shrink. When
// the mark keys went to three rows and the speed dial grew an export key under
// it, that floor went from 422 to 471 and crossed the line — measured, and the
// symptom was a 1400x340 dock that came back 1400x471 still in the wide shape.
//
// So this is not a taste number: it is "the wide arrangement no longer fits".
//
// AND A CONSTANT CANNOT SAY THAT, which is the second time this trap was
// sprung and the reason it is now measured instead. The wide arrangement's
// floor is not one number: it depends on the WIDTH, because below a certain
// one the strip's six sections stop fitting across three lanes and fold onto
// more lines. Measured on the panel this was written for: 553 px at 1920 wide,
// 651 px at 1400. A threshold of 540 is under BOTH, so a docked panel dragged
// as short as it will go stops at its floor, still wide, and the arrangement
// that would have fitted is never reached. From the operator's chair the short
// arrangement simply does not exist.
//
// So this constant is only the LOWER BOUND now — shorter than this and the
// panel is Short whatever else is true — and the real test is the floor the
// panel reports while it is wearing the wide arrangement. See panelModeFor.
inline constexpr int kShortMaxHeight = 540;
// HYSTERESIS, and it is not politeness. Dragging a dock edge across a bare
// threshold flips the mode back and forth, and every flip re-lays the
// OBSQTDisplay widgets — which on Windows means re-allocating a D3D swap chain
// on the graphics thread, several times a second, while a take is recording.
inline constexpr int kModeHysteresis = 40;
// HOW FAR APART THE THREE LANES MAY BE PUSHED.
//
// The wide arrangement justifies: marks at one end, the speed dial at the
// other, the transport in the middle. That is the reference panel's own shape
// and it reads well at the width it was designed for - but the leftover was
// going into the gaps WITHOUT LIMIT, so on a maximised 1920 px panel the three
// groups ended up with 400-500 px of nothing between them. Cramped keys with
// acres of empty panel around them is not justification, it is a strip that
// gave up.
//
// Past this the block stops spreading and is CENTRED instead, so the keys keep
// their own size (a key that changes size with the window is a key the hand has
// to find again) and the panel keeps its middle.
inline constexpr int kLaneGapMax = 140;

// Which arrangement a panel of this size wants. `current` is what it is wearing
// now, and it is an argument rather than a fresh decision because a threshold
// crossed on the way in is not the same threshold on the way out.
// `wideFloorH` is the height the WIDE arrangement last reported as its own
// minimum, or 0 if it has never worn one. It is measured rather than assumed
// because it moves with the width (see kShortMaxHeight), and passing it in
// keeps this function pure — the panel measures, this decides.
PanelMode panelModeFor(const QSize &size, PanelMode current,
		       int wideFloorH = 0);
const char *panelModeName(PanelMode m);

// ---------------------------------------------------------------------------
// Lane — WHERE A SECTION LIVES ON ITS LINE
// ---------------------------------------------------------------------------
//
// The wide arrangement is two macro-rows of three groups, and the whole point is
// that the groups are in the SAME PLACE on both rows: marks over the record key
// at the left, angles over the transport in the middle, exports over the speed
// dial at the right. Flowing them and spreading the leftover into the gaps put
// the two middle groups 48 px apart — near-alignment, which reads worse than
// either alignment or a deliberate offset, and is most of what "the keys are
// scattered" was.
//
// A lane is declared, so it cannot drift. Lane widths are taken across ALL the
// lines, so the left lane starts at the same x on every row, the right lane ends
// at the same x on every row, and the centre lane is centred once for all of
// them.
enum class Lane { Left, Centre, Right };

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
// useTextGlyph — draw a glyph as TEXT, not as a colour emoji
// ---------------------------------------------------------------------------
//
// U+23EE ⏮ and U+23ED ⏭ carry Emoji_Presentation=Yes, so a platform that has a
// colour emoji font draws them from it: on Windows the two frame-step keys came
// out in full-colour Segoe UI Emoji BLUE, on a panel where every other key is a
// grey glyph on a grey key. They were the loudest things in the transport row
// and they are the two least consequential keys in it. ▶ U+25B6, ◀ U+25C0,
// ■ U+25A0 and ↺ U+21BA default to text presentation, which is why only these
// two ever looked wrong.
//
// THE FIX IS THE FONT, NOT THE GLYPH. Appending U+FE0E (VARIATION SELECTOR-15)
// would be the tidy answer and it changes text(), and the gate finds Stop,
// reverse and both frame steps BY THEIR GLYPH (selftest.cpp). A font swap is
// invisible to that and to every other reader of the label.
//
// Falls back to the widget's current font when no candidate family has the
// glyph, which is the honest answer on a platform we have not thought about:
// a coloured key beats a missing one.
void useTextGlyph(QWidget *w, const QString &glyph);

// ---------------------------------------------------------------------------
// AspectBox — a monitor that is 16:9 whatever it is put in
// ---------------------------------------------------------------------------
//
// A box of any other shape draws the video letterboxed inside itself, and from
// the operator's chair a black edge is indistinguishable from the framing. So
// the shape is not negotiated with the layout: the box takes whatever cell it
// is given and places the LARGEST picture of the canvas's ratio that fits,
// centred, with its naming band tucked under it at the picture's own width.
//
// IT WAS DONE THE OTHER WAY FIRST — a maximum size computed from the parent's
// current width — and the panel's own check caught it on a two-camera rig: the
// maximum was worked out from a pane that had not been laid out yet, so a box
// was sized for one cell and then handed another. A widget that maintains its
// own geometry has nothing to be out of step with, and it settles in one pass
// rather than in however many the splitters take.
//
// IT NEVER RE-PARENTS ANYTHING. Its children are created with it as their
// parent and are only ever MOVED, because in the panel the picture is an
// OBSQTDisplay: Qt answers a re-parent by destroying the native window, which
// strands the obs_display presenting into it.
//
// The children are placed by hand rather than by a layout for the same reason
// the shape is not negotiated: a layout would negotiate, and there is nothing
// here to negotiate about.
class AspectBox : public QWidget {
public:
	explicit AspectBox(QWidget *parent = nullptr);

	// Both must already be children of this box. `tag` may be null.
	void setContents(QWidget *picture, QWidget *tag);
	QWidget *picture() const { return pic_; }

	// The canvas's own ratio. A vertical canvas is a real thing an operator
	// streams, so this is not hardcoded to 16:9 — the panel reads it from
	// obs_get_video_info and the mockup from its own default.
	void setRatio(int w, int h);

	// How tall the naming band is. It is a TALLY as much as a label — which
	// box is which is read by colour before it is read by letter — so it is
	// small but never nothing.
	static constexpr int kTagH = 12;

protected:
	void resizeEvent(QResizeEvent *) override;

private:
	void relayout();
	QWidget *pic_ = nullptr;
	QWidget *tag_ = nullptr;
	int rw_ = 16, rh_ = 9;
};


// ---------------------------------------------------------------------------
// The camera block beside the bays — how many columns, and how big a tile
// ---------------------------------------------------------------------------
//
// ONE COPY, and it lives here because it used to be two: the panel had its own
// arithmetic and the mockup had this one, so a change that made the mockup look
// right left the panel exactly as it was. That is not a tidiness point — it is
// the reason two rounds of "the cameras are still postage stamps" were answered
// with "it is fixed, look at the mockup".
//
// Two wrong answers were tried before this one, and both are worth knowing.
//
//   A GRID COLUMN WITH A STRETCH FACTOR. The obvious thing, and it collapsed:
//   a stretch only shares what is left AFTER every column has its minimum, and
//   a tile's minimum is nothing.
//
//   ceil(sqrt(n)) COLUMNS. Square-ish, and wrong at exactly the count this
//   panel is most often asked for: eight cameras became 3x3 with an empty cell
//   in the corner, and the eye finds that hole every time it reads the block.
//
// What the block has to do is stand beside the bays and be ABOUT AS TALL AS
// THEY ARE. So the arrangement is chosen from the bay height: for each column
// count that wastes no cell, work out how big a 16:9 tile would have to be to
// fill that height in the rows it implies, and keep the one whose block comes
// out nearest the width the block is meant to have.
struct TileBlock {
	int cols = 1;
	int tileW = 0;
	int tileH = 0;
	int blockW = 0;
	int blockH = 0;
	// What ONE bay must be to stand the same height as the camera rows.
	// The caller sizes the panes from this rather than from what is left.
	int bayW = 0;
	int rowH = 0;
};

// A tile below this is not a confidence monitor any more.
inline constexpr int kTileMinWidth = 78;
// The share of the monitoring pane the camera block is aimed at. They are
// confidence monitors: the bays are what is being watched.
inline constexpr double kTileShare = 0.22;
// ...AND THE CEILING IS A SHARE TOO, which is the whole of "the cameras are
// still much smaller than A". It was a constant 150 px while every other number
// in this block was proportional, so on a 1000 px panel beside an 840 px A the
// cameras came out as two stamps with 270 px of empty panel under them. A
// ceiling has to exist — left free, ONE camera draws itself as big as the
// picture being watched — but it has to be the same kind of number as the
// thing it is limiting.
inline constexpr double kTileMaxShare = 0.34;
// Between two tiles. It was 2, and with a naming band under each picture that
// put one row's label hard against the next row's picture.
inline constexpr int kTileGap = 4;

// `paneW` is the whole monitoring pane, `bays` how many big pictures share it,
// `n` the configured cameras. `maxH` is how much HEIGHT the block may actually
// have, which is a different question from how tall the bays are and the one
// that matters when the panel is docked under the OBS preview: there the pane
// is wide and shallow, and an arrangement chosen from the width alone asks for
// four rows of tiles in a pane with room for two. 0 = no limit.
TileBlock tileBlockFor(int paneW, int bays, int n, int gap, int maxH = 0);

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

	// ── A SECTION MAY SIZE ITS OWN KEYS PER SHAPE ────────────────────────
	// Called at the top of every apply(), with the shape about to be worn.
	//
	// It exists for the camera matrix and it is not a special case: eight
	// slots at their comfortable 46 px is 400 px of section, which on its own
	// put the panel's floor past the width of an OBS side dock. The keys have
	// to be narrower in the folded shape — and a maximumWidth cannot do it,
	// because what the strip MEASURES with is sizeHint(), and a maximum does
	// not move that. Only a fixed width does, and a fixed width has to change
	// when the shape does.
	//
	// The hook rather than a width argument, because "what my keys look like
	// in each of my two shapes" is the section's business — the same reason
	// the two shapes are declared by hand at the call site instead of being
	// reflowed by an algorithm. ControlStrip::measure() flips the shape twice
	// to read both sizes, so a section wired up this way is measured correctly
	// in both without anybody sequencing it.
	void setOnShape(std::function<void(bool flat)> fn);

private:
	void apply();

	QString caption_;
	QLabel *cap_ = nullptr;
	QWidget *body_ = nullptr;
	QGridLayout *grid_ = nullptr;
	BlockShape tall_, flat_;
	std::function<void(bool)> onShape_;
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
	void addBlock(KeyBlock *b, Lane lane, bool startsLine = false,
		      int rank = 0);
	bool isFlat() const { return flat_; }

	// THE PANEL'S MODE DRIVES THE STRIP, because width alone cannot tell the
	// two narrow cases apart: 520 px of a side dock wants a stack, and 1000 px
	// of a floating window wants the wide rows even though its three lanes no
	// longer fit side by side. Left alone (-1) the strip decides for itself
	// from its width, which is what the mockup's strip-only sizes rely on.
	// A section that would be CUT OFF still folds whatever this says.
	void setStacked(int on); // -1 auto, 0 lanes, 1 stack

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

	// WHICH SECTION IS SETTING THE FLOOR, in both of its shapes. "The panel
	// will not go below 374 px" is a number with nowhere to go: six sections
	// have an opinion about it and five of them are innocent, and the one that
	// is not is usually innocent in the shape you are looking at. Used by the
	// mockup's --check and --report, which is where a floor gets argued about.
	QString describeBlocks() const;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	// ── THE SEPARATORS THAT REPLACED THE CAPTIONS ────────────────────
	// Six sections used to be told apart by six headings: MARK, ANGOLI,
	// RIPRODUZIONE… Side by side that is one line of text for all six and it
	// is cheap; STACKED it is one line EACH, down a narrow column, which is
	// six lines of a panel whose scarce axis is height — and each of those
	// groups is already named by its own keys (● REC, In/Out, C1/C2, the
	// transport marks, the percentages).
	//
	// So the grouping is drawn instead of written: a hairline down the
	// middle of the gap between two adjacent sections of the same line. It
	// says the same thing in one pixel of WIDTH, which the panel has, rather
	// than a line of HEIGHT, which it does not.
	//
	// Painted rather than built out of widgets: the gaps move on every
	// relayout (lanes are re-centred, sections wrap), and a widget per gap
	// would be a set of children to keep in step with a geometry that is
	// already computed here.
	void paintEvent(QPaintEvent *e) override;

private:
	// WHERE THE RULES GO, collected BY the layout rather than deduced from
	// block geometry afterwards. The first version read the blocks' rects and
	// put a rule in the middle of each gap, and in the lane arrangement that
	// came out STAGGERED: the left lane's width is the widest left section
	// across every line, so a narrower one (REC, under the wider MARK) ends
	// early and its gap starts somewhere else. Two rules 80 px apart on two
	// stacked rows read as a mistake, which is the opposite of what a divider
	// is for.
	//
	// The layout knows the lane boundaries — that is the whole point of lanes
	// — so it says where the rules are and this only draws them.
	mutable QVector<QRect> sepRects_;
	void addSeparator(int x, int top, int height) const;

	struct Entry {
		KeyBlock *block = nullptr;
		bool startsLine = false;
		Lane lane = Lane::Left;
		int rank = 0;     // order when folded; see addBlock
		QSize tall, flat; // measured once, per shape
	};

	// The blocks in the order the current arrangement wants them.
	QVector<int> orderFor(bool flat) const;

	// Runs the same wrapping in both roles: `apply` false only measures.
	int layoutLines(int width, bool flat, bool apply) const;
	// The wide arrangement: lines of three lanes, the lanes aligned across
	// every line. Returns -1 when the lanes cannot be told apart at this
	// width, which is the caller's cue to pack instead.
	int layoutLanes(int width, bool apply) const;
	// The folded arrangement: a stack, in rank order, on a LEFT SPINE. A
	// column of sections each centred on its own width is a column with no
	// edge to read down, which is the narrow-dock version of "scattered".
	int layoutStack(int width, bool apply) const;
	// What the old flow did: gather, then spread the leftover into the gaps.
	// Still the honest answer when the lanes do not fit.
	int layoutPacked(int width, bool flat, bool apply) const;
	void measure(Entry &e);
	void applyShape(bool flat);

	mutable QVector<Entry> blocks_;
	bool flat_ = false;
	int forcedStack_ = -1;
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
