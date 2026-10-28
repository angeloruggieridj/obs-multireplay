/*
obs-multireplay — unit tests for dock-metrics.hpp
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, no Qt. The panel's arrangement code (dock-layout.hpp)
includes Qt and cannot be reached from here, so this test exists only because
the arithmetic it is made of was moved into dock-metrics.hpp: the mode
thresholds and their hysteresis, the monitoring room, the short splitter's
left width, and the camera tile block.

The numbers are derived from the constants rather than copied: a threshold
that moves should move this test with it, and an invariant that stops holding
(a tile under kTileMinWidth, a block taller than the room it was handed) fails
here instead of in front of an operator.
*/

#include "dock-metrics.hpp"

#include <cstdio>
#include <cstring>

using namespace multireplay;

static int g_fail = 0;

#define CHECK(cond)                                                        \
	do {                                                                \
		if (!(cond)) {                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, \
				    #cond);                                 \
			++g_fail;                                           \
		}                                                           \
	} while (0)

namespace {

bool same_block(const TileBlock &a, const TileBlock &b)
{
	return a.cols == b.cols && a.tileW == b.tileW && a.tileH == b.tileH &&
	       a.blockW == b.blockW && a.blockH == b.blockH && a.bayW == b.bayW &&
	       a.rowH == b.rowH;
}

// The declared arrangement: 1..3 across in one row, ceil(n/2) columns in two
// rows beyond — see the table in dock-metrics.hpp. Written here so the test
// says what it expects rather than what the code happens to do.
int declared_cols(int n)
{
	return n <= 3 ? n : (n + 1) / 2;
}

int declared_rows(int n)
{
	const int cols = declared_cols(n);
	return (n + cols - 1) / cols;
}

} // namespace

static void test_panel_mode_thresholds()
{
	// Sizes the panel is really given: full screen, a side dock and a dock
	// squeezed under the OBS preview.
	CHECK(panelModeFor(1000, 700, PanelMode::Wide) == PanelMode::Wide);
	CHECK(panelModeFor(500, 700, PanelMode::Wide) == PanelMode::Tall);
	CHECK(panelModeFor(1200, 400, PanelMode::Wide) == PanelMode::Short);
	CHECK(panelModeFor(1000, 300, PanelMode::Wide) == PanelMode::Short);
	// Tall beats Short: a narrow column is a column whatever its height.
	CHECK(panelModeFor(500, 300, PanelMode::Wide) == PanelMode::Tall);
}

static void test_panel_mode_hysteresis()
{
	// WIDTH, in and out of Tall. A panel exactly on the threshold keeps what
	// it is wearing; coming out of Tall costs kModeHysteresis more.
	CHECK(panelModeFor(kTallMaxWidth, 900, PanelMode::Wide) !=
	      PanelMode::Tall);
	CHECK(panelModeFor(kTallMaxWidth - 1, 900, PanelMode::Wide) ==
	      PanelMode::Tall);
	CHECK(panelModeFor(kTallMaxWidth + 1, 900, PanelMode::Wide) ==
	      PanelMode::Wide);
	CHECK(panelModeFor(kTallMaxWidth + kModeHysteresis - 1, 900,
			   PanelMode::Tall) == PanelMode::Tall);
	CHECK(panelModeFor(kTallMaxWidth + kModeHysteresis, 900,
			   PanelMode::Tall) == PanelMode::Wide);

	// HEIGHT, the same both ways around kShortMaxHeight.
	CHECK(panelModeFor(1000, kShortMaxHeight - 1, PanelMode::Wide) ==
	      PanelMode::Short);
	CHECK(panelModeFor(1000, kShortMaxHeight, PanelMode::Wide) ==
	      PanelMode::Wide);
	CHECK(panelModeFor(1000, kShortMaxHeight + kModeHysteresis - 1,
			   PanelMode::Short) == PanelMode::Short);
	CHECK(panelModeFor(1000, kShortMaxHeight + kModeHysteresis,
			   PanelMode::Short) == PanelMode::Wide);
}

static void test_panel_mode_wide_floor()
{
	// A short dock whose wide arrangement last reported a 650 px floor: the
	// test is against the MEASURED floor plus hysteresis, not the constant,
	// so 600 px of height is Short even though it clears 540.
	CHECK(panelModeFor(1000, 600, PanelMode::Short, 650) ==
	      PanelMode::Short);
	CHECK(panelModeFor(1000, 600, PanelMode::Wide, 650) ==
	      PanelMode::Short);
	// A Wide panel stops being Wide at floor + hysteresis (690)...
	CHECK(panelModeFor(1000, 650 + kModeHysteresis - 1, PanelMode::Wide,
			   650) == PanelMode::Short);
	CHECK(panelModeFor(1000, 650 + kModeHysteresis, PanelMode::Wide,
			   650) == PanelMode::Wide);
	// ...and coming back out of Short costs one hysteresis more (730).
	CHECK(panelModeFor(1000, 650 + 2 * kModeHysteresis - 1,
			   PanelMode::Short, 650) == PanelMode::Short);
	CHECK(panelModeFor(1000, 650 + 2 * kModeHysteresis, PanelMode::Short,
			   650) == PanelMode::Wide);
	// The constant is still the lower bound: a reported floor under it
	// changes nothing.
	CHECK(panelModeFor(1000, kShortMaxHeight - 1, PanelMode::Short, 300) ==
	      PanelMode::Short);
}

static void test_panel_mode_names()
{
	CHECK(std::strcmp(panelModeName(PanelMode::Wide), "wide") == 0);
	CHECK(std::strcmp(panelModeName(PanelMode::Short), "short") == 0);
	CHECK(std::strcmp(panelModeName(PanelMode::Tall), "tall") == 0);
}

static void test_monitor_room()
{
	// Derived: the splitter, less the list's floor and the control column,
	// capped at half the panel while the wide shape is worn.
	CHECK(monitorRoomFor({600, 900, 0, 100, 200, false, true}) == 300);
	// Not wide: no cap, the list's floor is the only other claim.
	CHECK(monitorRoomFor({600, 900, 0, 100, 200, false, false}) == 600);
	// The operator has dragged the divider: that IS the answer, and the cap
	// does not apply to it.
	CHECK(monitorRoomFor({400, 900, 500, 100, 200, true, true}) == 400);
	// A row with nothing left in it still keeps 40 px of picture.
	CHECK(monitorRoomFor({600, 100, 0, 100, 200, false, true}) == 40);
}

static void test_short_split_left_width()
{
	// 500 - 400 leaves the left column 100, below its own minimum: the
	// minimum wins rather than the right pane's preference.
	CHECK(shortSplitLeftWidth(500, 300, 400) == 300);
	// When the preference leaves more than the minimum, the left column
	// takes the whole remainder.
	CHECK(shortSplitLeftWidth(900, 300, 400) == 500);
	// Exactly the minimum is not below it.
	CHECK(shortSplitLeftWidth(700, 300, 400) == 300);
}

static void test_tile_block_empty()
{
	// No cameras, no pane, no bays: the block comes back with no size at
	// all. `cols` keeps the struct's declared default of 1 — nobody should
	// read it with nothing in the block.
	const TileBlock none = tileBlockFor(1000, 1, 0, 3, 200);
	CHECK(none.tileW == 0 && none.tileH == 0);
	CHECK(none.blockW == 0 && none.blockH == 0);
	const TileBlock noPane = tileBlockFor(0, 1, 4, 3, 200);
	CHECK(noPane.blockW == 0 && noPane.blockH == 0);
}

static void test_tile_block_columns()
{
	// 1..3 cameras share one row; 4 goes two-by-two; 7 and 8 are four
	// across. Never a third row.
	const int counts[] = {1, 2, 3, 4, 5, 6, 7, 8};
	for (int n : counts) {
		const TileBlock tb = tileBlockFor(1000, 1, n, 3, 0);
		CHECK(tb.cols == declared_cols(n));
		CHECK(declared_rows(n) <= 2);
		CHECK(tb.cols * declared_rows(n) >= n);
	}
}

static void test_tile_block_geometry()
{
	const int panes[] = {300, 520, 800, 1000, 1456};
	const int counts[] = {1, 2, 4, 8};
	for (int paneW : panes) {
		for (int n : counts) {
			const TileBlock tb = tileBlockFor(paneW, 1, n, 3, 200);
			CHECK(tb.cols == declared_cols(n));
			// A tile never drops below the confidence-monitor minimum:
			// the clamp holds it there even in a narrow pane.
			CHECK(tb.tileW >= kTileMinWidth);
			// 16:9 within the rounding of integer division.
			CHECK(tb.tileH * 16 <= tb.tileW * 9);
			CHECK(tb.tileW * 9 - tb.tileH * 16 < 16);
			// The block is its own rows and gaps, and it fits the room it
			// was handed.
			const int rows = declared_rows(n);
			CHECK(tb.blockW ==
			      tb.cols * tb.tileW + (tb.cols - 1) * kTileGap);
			CHECK(tb.blockH == rows * (tb.tileH + kTileTagH) +
					   (rows - 1) * kTileGap);
			CHECK(tb.blockH <= 200);
		}
	}
}

static void test_tile_block_max_height()
{
	// The docked-under-the-preview shape: wide and shallow. The block is
	// arranged for the height it has, not the height the width implies.
	const int counts[] = {1, 2, 4, 8};
	for (int n : counts) {
		CHECK(tileBlockFor(1200, 2, n, 3, 200).blockH <= 200);
		CHECK(tileBlockFor(1200, 1, n, 3, 150).blockH <= 150);
	}
	// Below what a minimum-width tile needs, the kTileMinWidth floor wins
	// over the clamp — a stamp is worse than a slightly tall block. The
	// block comes back taller than the 60 px it was offered, deliberately;
	// documented here rather than pretended away.
	const TileBlock floored = tileBlockFor(1000, 1, 8, 3, 60);
	CHECK(floored.tileW == kTileMinWidth);
	CHECK(floored.blockH == 100);
}

static void test_tile_block_exact_values()
{
	// Eight cameras in a 1000 px pane: four by two, the block exactly as
	// wide as four tiles and their gaps. Pinned so a change to any of the
	// constants shows up here rather than on a rig.
	const TileBlock wide = tileBlockFor(1000, 1, 8, 3, 200);
	CHECK(wide.cols == 4);
	CHECK(wide.tileW == 160 && wide.tileH == 90);
	CHECK(wide.blockW == 652 && wide.blockH == 194);
	CHECK(wide.bayW == 345 && wide.rowH == 195);

	// The narrow pane: one camera on its own, sized by the width there is.
	const TileBlock narrow = tileBlockFor(300, 1, 1, 3, 200);
	CHECK(narrow.cols == 1);
	CHECK(narrow.tileW == 102 && narrow.tileH == 57);
	CHECK(narrow.blockW == 102 && narrow.blockH == 62);
	CHECK(narrow.bayW == 195 && narrow.rowH == 88);
}

static void test_deterministic()
{
	// The same question twice is the same answer: the panel asks on every
	// resize pass, on both sides of a mode change.
	const int counts[] = {1, 2, 4, 8};
	for (int n : counts) {
		CHECK(same_block(tileBlockFor(1000, 1, n, 3, 200),
				 tileBlockFor(1000, 1, n, 3, 200)));
	}
	const int widths[] = {300, 760, 800, 1000};
	for (int w : widths) {
		CHECK(panelModeFor(w, 600, PanelMode::Wide, 650) ==
		      panelModeFor(w, 600, PanelMode::Wide, 650));
	}
	CHECK(monitorRoomFor({600, 900, 0, 100, 200, false, true}) ==
	      monitorRoomFor({600, 900, 0, 100, 200, false, true}));
	CHECK(shortSplitLeftWidth(500, 300, 400) ==
	      shortSplitLeftWidth(500, 300, 400));
}

int main()
{
	test_panel_mode_thresholds();
	test_panel_mode_hysteresis();
	test_panel_mode_wide_floor();
	test_panel_mode_names();
	test_monitor_room();
	test_short_split_left_width();
	test_tile_block_empty();
	test_tile_block_columns();
	test_tile_block_geometry();
	test_tile_block_max_height();
	test_tile_block_exact_values();
	test_deterministic();

	if (g_fail) {
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("dock_metrics: all checks passed\n");
	return 0;
}
