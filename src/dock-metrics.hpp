// dock-metrics.hpp — the panel's sizes, worked out with no Qt in them.
//
// WHY THIS IS A FILE OF ITS OWN. Every arrangement question in dock-layout.hpp
// is really an arithmetic question — is the panel narrow enough to be a
// column, how much height is the monitoring row allowed, how big does a camera
// tile come out — and the answers are pure int/double work. They used to live
// in dock-layout.hpp/.cpp, which include Qt, so the standalone unit tests
// (compiled with nothing but a C++20 compiler) could not reach a single one:
// the only things that exercised them were a full panel and a second-renderer run, and
// neither fails a build. Moved here, they cost a header include and gain a
// CTest.
//
// ONE COPY: dock-layout.hpp includes this and re-exports the QSize overload of
// panelModeFor, so the panel and the tests read the same numbers.
#pragma once

#include <algorithm>

namespace multireplay {

// ---------------------------------------------------------------------------
// HOW TALL THE MONITORING ROW MAY BE — ONE COPY OF IT
// ---------------------------------------------------------------------------
//
// It was two: the panel had one and a second renderer had another, and they had drifted
// apart in a way that mattered — a second renderer honoured the divider the operator
// had dragged between the pictures and the list, and the panel did not. At the
// same size and on the same rig the two produced 357 px cameras and 237 px
// cameras. A second renderer is what every layout decision is judged on, so the panel
// was being judged against a panel that did not exist.
//
// IT MUST NOT BE READ OFF THE ROW ITSELF. The row's height is DERIVED from the
// arrangement this number chooses, so feeding it back makes a pass decide from
// whatever the widget happened to be mid-settle — measured once at 100 px, which
// picked an arrangement of 78 px stamps. What the splitter is willing to give
// depends on the panel and a constant, so it is the same answer on every pass.
//
// ...BUT ONCE THE OPERATOR HAS MOVED THE DIVIDER, that IS the answer, and it is
// stable for the same reason: it has stopped being derived from anything.
struct MonitorRoom {
	int panelH;      // the whole panel
	int splitterH;   // the body splitter: pictures + list
	int leftColH;    // what the picture side actually has
	int controlsH;   // the keys, when they are in that column (Short)
	int listFloor;   // how much list is kept whatever the pictures ask for
	bool bodyChosen; // has the operator dragged the pictures/list divider
	bool wide;       // the half-the-panel cap applies only to the wide shape
};

inline int monitorRoomFor(const MonitorRoom &m)
{
	if (m.bodyChosen && m.leftColH > 0)
		return std::max(40, m.leftColH - m.controlsH);
	int room = m.splitterH - m.listFloor - m.controlsH;
	// ...AND THE PICTURES MAY NOT HAVE MORE THAN HALF THE PANEL. Past that
	// the list stops being a list. The cap belongs HERE, with the rest of the
	// answer, because the tile arithmetic asks the same question and the two
	// answers have to be one — they were two, and the cameras came out sized
	// for a block a third taller than the one they were given.
	if (m.wide)
		room = std::min(room, m.panelH / 2);
	return std::max(40, room);
}

// ---------------------------------------------------------------------------
// SHORT'S OTHER DIVIDER — the one nobody was minding
// ---------------------------------------------------------------------------
//
// `monitorRoomFor` above exists because a HEIGHT split had two copies that
// drifted. This is the same lesson on the other axis: in Short the body
// splitter divides WIDTH, not height, and until this function existed NOTHING
// gave that divider an opinion — the height-management in applyPreviewSplit
// explicitly hands Short back with "a height means nothing here" and stops,
// leaving a QSplitter with only its stretch factors (3:2) to go on. Content
// changes on the LEFT side (a section gaining a row, a rank moving a block
// onto a different packed line) can shift where that stretch-driven default
// converges even though nothing on the right changed at all — measured: a
// height-only edit to the key strip moved this divider 28 px, enough to push
// "Live" in the toolbar down to its CSS floor (44 px) instead of its own
// word's width (85), rendering it clipped rather than merely tight.
//
// So this divider gets the same treatment as the height one: left alone once
// the operator has dragged it (that check happens at the call site, same as
// bodyChosen/splitChosen elsewhere), it otherwise guarantees the RIGHT pane
// (the toolbar + event list) its own preferred width before the left column
// gets whatever is left — floored at the left column's own minimum, since
// giving away more than exists is not "guaranteeing" anything.
inline int shortSplitLeftWidth(int totalW, int leftMinW, int rightWantW)
{
	return std::max(leftMinW, totalW - rightWantW);
}

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

// Which arrangement a panel of this size wants. `current` is what it is wearing
// now, and it is an argument rather than a fresh decision because a threshold
// crossed on the way in is not the same threshold on the way out.
// `wideFloorH` is the height the WIDE arrangement last reported as its own
// minimum, or 0 if it has never worn one. It is measured rather than assumed
// because it moves with the width (see kShortMaxHeight), and passing it in
// keeps this function pure — the panel measures, this decides.
inline PanelMode panelModeFor(int w, int h, PanelMode current,
			      int wideFloorH = 0)
{
	// Each threshold is widened in the direction that would UNDO the current
	// mode, so a panel sitting on a boundary keeps what it has until the drag
	// is meant. Coming out of Tall costs 40 px more width than going in did.
	const int wLimit =
		kTallMaxWidth + (current == PanelMode::Tall ? kModeHysteresis : 0);
	if (w < wLimit)
		return PanelMode::Tall;

	// SHORT IS "THE WIDE ARRANGEMENT NO LONGER FITS", and the honest way to
	// ask that is to compare against what it actually needs rather than
	// against a number written down once. A panel dragged as short as it will
	// go comes to rest exactly ON its floor, so the test has to fire AT that
	// height, not below it - the hysteresis is what gives it room to.
	const int need = std::max(kShortMaxHeight, wideFloorH + kModeHysteresis);
	const int hLimit = need + (current == PanelMode::Short ? kModeHysteresis
							       : 0);
	if (h < hLimit)
		return PanelMode::Short;

	return PanelMode::Wide;
}

inline const char *panelModeName(PanelMode m)
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
// The camera block beside the bays — how many columns, and how big a tile
// ---------------------------------------------------------------------------
//
// ONE COPY, and it lives here because it used to be two: the panel had its own
// arithmetic and a second renderer had this one, so a change that made a second renderer look
// right left the panel exactly as it was. That is not a tidiness point — it is
// the reason two rounds of "the cameras are still postage stamps" were answered
// with "it is fixed, look at a second renderer".
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
// THE CEILING IS A SHARE, which is the whole of "the cameras are
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
// The naming band under each picture — tileBlockFor has to leave room for it
// in the height every tile carries. It is the same band AspectBox draws under
// a monitor: dock-layout.hpp aliases its kTagH to this number, so the two can
// never drift apart.
inline constexpr int kTileTagH = 5;

// `paneW` is the whole monitoring pane, `bays` how many big pictures share it,
// `n` the configured cameras. `maxH` is how much HEIGHT the block may actually
// have, which is a different question from how tall the bays are and the one
// that matters when the panel is docked under the OBS preview: there the pane
// is wide and shallow, and an arrangement chosen from the width alone asks for
// four rows of tiles in a pane with room for two. 0 = no limit.
// `bayTagH` is the naming band under the BIG pictures, which is taller than
// the tiles' in the monitors' own dock (see MultiReplayDock::bayTagH), and
// `tileTagH` the band under each camera tile, which can differ from both.
inline TileBlock tileBlockFor(int paneW, int bays, int n, int gap, int maxH = 0,
			      int bayTagH = kTileTagH, int tileTagH = kTileTagH)
{
	TileBlock best;
	if (n <= 0 || paneW <= 0 || bays <= 0)
		return best;
	const auto aspect = [](int w) { return std::max(1, w * 9 / 16); };
	const int tagH = tileTagH;
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
			   bays * perBay * bayTagH -
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
	int finalBayW = std::max(40, (h - bayTagH) * 16 / 9);
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

} // namespace multireplay
