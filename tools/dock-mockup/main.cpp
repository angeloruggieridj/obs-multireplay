// dock-mockup — the whole panel, alive, without OBS.
//
// WHY IT EXISTS. Every layout complaint about this dock ("the keys are
// scattered", "it will not resize", "the borders are cut") is about what the
// panel does at a size nobody built it at, and the only way to see that is to
// resize it. Doing that inside OBS costs a four-minute gate run per look, needs
// a rig with cameras on it, and leaves you reading a screenshot of a window
// somebody else's compositor drew.
//
// So this builds the SAME zones, with the SAME layout code (src/dock-layout),
// the SAME style sheet (src/dock-style) and the SAME marks (src/dock-icons),
// out of dummy keys — and then walks itself through a list of sizes, writing a
// PNG of each. Portrait, landscape, docked-narrow, floating-wide: one second,
// seven pictures.
//
// THE ARRANGEMENT IT IS CHECKING, top to bottom:
//
//   A | B | the camera filmstrip          the monitoring block, aspect-capped
//   A1 · list · IN+/OUT− · notice         ONE line (it was three)
//   project · search · Live · Monitors ⛶  the toolbar, UNDER the pictures
//   1 2 3 4 …                             the list tabs
//   the event table                       the elastic zone
//   IN OUT −5 −10 −20 │ ▶ ■ ◀ … │ …       the control strip, no captions
//   the on-air band                       what is playing, and how much is left
//   REC 09:52 ⚠ │ NOW LOOP ♫ OUT │ 1.00×  the status line, which OWNS the modes
//   the position bar                      the whole recorded timeline
//
// A MOCKUP THAT ARRANGES ITS KEYS DIFFERENTLY IS MEASURING A PANEL THAT DOES
// NOT EXIST. Whenever this file and multireplay-dock.cpp disagree about where
// something goes, this one is wrong.
#include "../../src/dock-icons.hpp"
#include "../../src/dock-layout.hpp"
#include "../../src/dock-style.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace multireplay;

namespace {

// The camera keys are square-ish and small, as on the reference panel: a matrix
// is read by position, not by label, and a wide key breaks the grid.
constexpr int kAngleKeyW = 46;
// …and the width they compress to on a narrow panel. The QSS floor for #mrAngle
// is 34, so anything below that is not reachable from here.
constexpr int kAngleKeyMinW = 34;
// Past this the compressed matrix still does not fit a side dock, and the
// second answer — wrapping at four — is worth its row per bay. 320 is a 340 px
// dock less the panel's own margins.
constexpr int kAngleWrapWidth = 320;
// Narrower than this a tile stops being a picture and becomes a smear; wider
// than this it stops being a confidence monitor and starts competing with the
// bay it is meant to be checked against.
constexpr int kTileMinWidth = 78;
constexpr int kTileMaxWidth = 150;
// The status line. Shorter than a key row because nothing in it is a target the
// hand goes to blind — the modes are pressed while looking at them.
constexpr int kStatusH = 22;
// The on-air band, and the position bar. The band was 28 and the bar 52 with a
// caption over each; the caption is gone (the band is green and the bar is
// graduated — neither has ever needed to be told apart by a heading) and the
// bar's ruler is tighter.
constexpr int kClipBarH = 24;
constexpr int kSeekH = 42;

// The RIG the mockup pretends to be driving. Defaults to the one that is
// hardest on the layout and happens to be the operator's own: both bays and
// eight cameras.
int g_cams = 8;
bool g_haveB = true;
ThemeChoice g_theme = ThemeChoice::Broadcast;
QPalette g_pal;
Scheme g_sc;
IconTints g_tints;

QPushButton *key(const QString &text, const char *role = "")
{
	auto *b = new QPushButton(text);
	if (role && *role)
		b->setObjectName(QString::fromLatin1(role));
	b->setMinimumHeight(kKeyH);
	b->setMaximumHeight(kKeyH);
	b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	return b;
}

// A key that is a MARK and nothing else. `id` is the stable identity the gate
// finds it by (see dock-icons.hpp): it survives a locale, a font and this
// redesign, which the literal text it replaced did not.
QPushButton *iconKey(Icon ic, const QString &id, const QString &tip,
		     const char *role = "mrTransport")
{
	auto *b = key(QString(), role);
	setKeyIcon(b, ic, g_tints);
	setKeyId(b, id);
	b->setToolTip(tip);
	return b;
}

// A mark AND a word: the two export keys and REC, where the label is doing work
// the mark cannot ("Esporta clip" vs "Sequenza" is one file or many).
QPushButton *iconTextKey(Icon ic, const QString &text, const QString &id,
			 const char *role = "")
{
	auto *b = key(text, role);
	setKeyIcon(b, ic, g_tints, 14);
	setKeyId(b, id);
	return b;
}

QPushButton *statusKey(const QString &text, const QString &id)
{
	auto *b = new QPushButton(text);
	b->setObjectName(QStringLiteral("mrStatKey"));
	b->setCheckable(true);
	b->setFixedHeight(kStatusH - 4);
	b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setKeyId(b, id);
	return b;
}

// ---------------------------------------------------------------------------
// THE CAMERA BLOCK BESIDE THE BAYS — how many columns, and how big a tile
// ---------------------------------------------------------------------------
//
// Two wrong answers were tried before this one, and both are worth knowing.
//
//   A GRID COLUMN WITH A STRETCH FACTOR. The obvious thing, and it collapsed:
//   at 1000 px the tiles got 50 px between them while A and B took 370 each,
//   because a stretch only shares what is left AFTER every column has its
//   minimum, and the tiles' minimum was nothing. Eight cameras came out as
//   eight vertical slivers.
//
//   ceil(sqrt(n)) COLUMNS. Square-ish, and wrong at exactly the count this
//   panel is most often asked for: eight cameras became 3×3 with an empty cell
//   in the corner, and the eye finds that hole every time it reads the block.
//
// What the block actually has to do is stand beside two 16:9 pictures and be
// ABOUT AS TALL AS THEY ARE — otherwise it is either a ribbon with a band of
// nothing under it or a column taller than the bays it is next to. So the
// arrangement is chosen from the bay height: for each column count that wastes
// no cell, work out how big a 16:9 tile would have to be to fill that height in
// the rows it implies, and keep the one whose block comes out nearest the width
// the block is meant to have.
struct TileBlock {
	int cols = 1;
	int tileW = kTileMinWidth;
	int tileH = 44;
	int blockW = 0;
	int blockH = 0;
};

// The share of the monitoring pane the camera block is aimed at. They are
// confidence monitors: the two bays are what is being watched.
constexpr double kTileShare = 0.22;
constexpr int kTileGap = 2;

// `maxH` is how much HEIGHT the block may actually have, which is a different
// question from how tall the bays are and the one that matters when the panel
// is docked under the OBS preview: there the pane is 750 px wide and 120 px
// tall, so an arrangement chosen from the width alone asks for four rows of
// tiles in a pane that has room for two — and the panel's floor goes up by the
// difference. 0 = no limit.
TileBlock tileBlockFor(int paneW, int bays, int n, int gap, int maxH = 0)
{
	TileBlock best;
	if (n <= 0 || paneW <= 0)
		return best;
	auto aspect = [](int w) { return std::max(1, w * 9 / 16); };
	const int target = (int)(paneW * kTileShare);
	// A first guess at the bay height, from the width the block is aimed at.
	// It only has to be close: it picks the arrangement, and the arrangement
	// is then measured properly.
	const int aw0 = std::max(40, (paneW - target - gap * bays) / bays);
	int bayH = aspect(aw0);
	if (maxH > 0)
		bayH = std::min(bayH, maxH);

	int bestScore = INT_MAX;
	for (int cols = 1; cols <= n; cols++) {
		const int rows = (n + cols - 1) / cols;
		// NO HOLES. A ragged last row is the thing the eye keeps
		// returning to; if nothing divides evenly the fallback below
		// takes the least ragged.
		if (cols * rows != n)
			continue;
		int th = (bayH - (rows - 1) * kTileGap) / rows;
		int tw = std::clamp(th * 16 / 9, kTileMinWidth, kTileMaxWidth);
		th = aspect(tw);
		const int bw = cols * tw + (cols - 1) * kTileGap;
		const int bh = rows * th + (rows - 1) * kTileGap;
		// Width is the preference; HEIGHT IS A WALL. A block that does not
		// fit the pane is not a slightly worse arrangement, it is a floor
		// the operator cannot get back, so overflow is weighted far above
		// being the wrong width.
		const int over = (maxH > 0) ? std::max(0, bh - maxH) : 0;
		const int score = std::abs(bw - target) + over * 4;
		if (score < bestScore) {
			bestScore = score;
			best = {cols, tw, th, bw, bh};
		}
	}
	if (bestScore == INT_MAX) {
		// Nothing divided evenly (a prime count of cameras). Take the
		// squarest arrangement and live with the one empty cell.
		const int cols = std::max(1, (int)std::ceil(std::sqrt((double)n)));
		const int rows = (n + cols - 1) / cols;
		int th = std::max(1, (bayH - (rows - 1) * kTileGap) / rows);
		int tw = std::clamp(th * 16 / 9, kTileMinWidth, kTileMaxWidth);
		th = aspect(tw);
		best = {cols, tw, th, cols * tw + (cols - 1) * kTileGap,
			rows * th + (rows - 1) * kTileGap};
	}
	return best;
}

QWidget *statusSep(QWidget *parent)
{
	auto *s = new QWidget(parent);
	s->setObjectName(QStringLiteral("mrStatSep"));
	s->setFixedWidth(1);
	return s;
}

class Mock : public QWidget {
public:
	// A stand-in for one of the panel's pictures. The real ones are
	// OBSQTDisplay widgets with a swap chain behind them; here they only have
	// to occupy the same room and prove the arrangement puts them somewhere
	// usable.
	QWidget *pic(const QString &text, int minH, bool tally)
	{
		auto *box = new QWidget(this);
		auto *v = new QVBoxLayout(box);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(0);
		auto *l = new QLabel(text, box);
		l->setAlignment(Qt::AlignCenter);
		l->setStyleSheet(QString("background:#000;color:%1;font-size:10px;")
					 .arg(g_sc.textDim));
		l->setMinimumHeight(minH);
		l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		v->addWidget(l, 1);
		if (tally) {
			// THE TALLY BAND STAYS. It is how the operator tells one
			// picture from another without reading — the one thing on
			// this panel that is read by colour alone. It is only
			// thinner: a tally has to be findable, not large.
			auto *tag = new QLabel(text, box);
			tag->setObjectName(QStringLiteral("mrChanTag"));
			tag->setProperty("chan", text);
			tag->setProperty("active", text == QStringLiteral("A"));
			tag->setAlignment(Qt::AlignCenter);
			v->addWidget(tag);
		}
		return box;
	}

	Mock()
	{
		setObjectName(QStringLiteral("MultiReplayDock"));
		sc_ = g_sc;
		setStyleSheet(dockStyle(sc_));
		// The floor a VERTICAL DOCK needs. It was 420 here and 560 in the
		// panel, and both were measured against the wide arrangement — the
		// one shape the panel could never wear at that width.
		setMinimumWidth(300);

		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(4, 4, 4, 4);
		v->setSpacing(3);

		// ── the monitoring block: A, B and the camera tiles ───────────
		// A GRID, NOT A ROW, and the reason is not tidiness: the three
		// arrangements move these between cells, and moving a widget
		// between two LAYOUTS re-parents it. In the panel that destroys an
		// OBSQTDisplay's native window and strands its obs_display, which
		// is the one failure qt-display.hpp exists to prevent. Re-celling
		// one grid never re-parents anything.
		previewBox_ = new QWidget(this);
		auto *pv = new QVBoxLayout(previewBox_);
		pv->setContentsMargins(0, 0, 0, 0);
		pv->setSpacing(0);
		auto *picRow = new QWidget(previewBox_);
		previewGrid_ = new QGridLayout(picRow);
		previewGrid_->setContentsMargins(0, 0, 0, 0);
		previewGrid_->setSpacing(3);
		aPic_ = pic(QStringLiteral("A"), 60, true);
		bPic_ = pic(QStringLiteral("B"), 60, true);
		if (!g_haveB)
			bPic_->hide();
		tiles_ = new QWidget(picRow);
		tilesGrid_ = new QGridLayout(tiles_);
		tilesGrid_->setContentsMargins(0, 0, 0, 0);
		tilesGrid_->setSpacing(2);
		for (int i = 0; i < kTiles; i++) {
			// NO FLOOR ON A TILE. A minimum height here becomes the
			// panel's: four rows of confidence monitors at 24 px put
			// 96 px into a floor the operator cannot get back, and the
			// point of the whole redesign is that this panel can be
			// made short. What keeps them picture-shaped is the CAP
			// (setTileCaps), which is a limit and not a demand.
			tile_[i] = pic(QStringLiteral("C%1").arg(i + 1), 0, false);
			// Only the configured cameras get a tile, as in the panel.
			if (i >= g_cams)
				tile_[i]->hide();
		}
		pv->addWidget(picRow, 1);

		// ── the channel strip: ONE line ───────────────────────────────
		// It carried three: list / clip x of y / remaining, then the event
		// id with the two offsets, then timecode and speed — 44 px of the
		// panel restating what the on-air band and the position bar
		// already say. What is left is what is said nowhere else, plus the
		// place showNotice() answers a key press.
		auto *strip = new QWidget(previewBox_);
		auto *sh = new QHBoxLayout(strip);
		sh->setContentsMargins(0, 0, 0, 0);
		sh->setSpacing(0);
		auto *badge = new QLabel(QStringLiteral("A1"), strip);
		badge->setObjectName(QStringLiteral("mrChanBadge"));
		auto *chan = new QLabel(
			QStringLiteral("L01  ·  0003  IN +01.24  OUT −00.40"),
			strip);
		chan->setObjectName(QStringLiteral("mrChanStrip"));
		// Same as the on-air band: a placeholder's text is not a
		// constraint the panel actually has.
		chan->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		chan->setMinimumWidth(40);
		sh->addWidget(badge);
		sh->addWidget(chan, 1);
		pv->addWidget(strip);

		splitter_ = new QSplitter(Qt::Vertical, this);
		splitter_->setChildrenCollapsible(false);
		splitter_->setHandleWidth(5);
		splitter_->addWidget(previewBox_);

		// ── the list pane: the toolbar, the tabs, the table ───────────
		// THE TOOLBAR IS UNDER THE PICTURES, not above them. The pictures
		// lead — that is what an operator's eye goes to — and everything
		// that picks WHICH event (search, the tabs) belongs to the list it
		// filters, so it lives with the list.
		auto *listPane = new QWidget(this);
		auto *lv = new QVBoxLayout(listPane);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(2);
		lv->addWidget(buildToolbar(listPane));
		auto *tabs = new QLabel(
			QStringLiteral(" 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │ 10 "),
			listPane);
		tabs->setObjectName(QStringLiteral("mrMuted"));
		tabs->setFixedHeight(20);
		lv->addWidget(tabs);
		table_ = new QTableWidget(6, 6, listPane);
		table_->setObjectName(QStringLiteral("mrEvents"));
		table_->verticalHeader()->setVisible(false);
		table_->setMinimumHeight(50);
		table_->setSizePolicy(QSizePolicy::Expanding,
				      QSizePolicy::Expanding);
		lv->addWidget(table_, 1);
		splitter_->addWidget(listPane);
		splitter_->setStretchFactor(0, 3);
		splitter_->setStretchFactor(1, 2);
		v->addWidget(splitter_, 1);

		strip_ = new ControlStrip(this);
		// Two macro-rows in the wide arrangement; the numbers are the
		// FOLDED order — what an operator reaches for through a whole
		// match comes first, and the exports, which nobody touches while
		// the ball is in play, come last.
		buildMark(Lane::Left, /*rank*/ 1);
		buildAngles(Lane::Centre, /*rank*/ 2);
		buildExport(Lane::Right, /*rank*/ 5);
		buildRec(Lane::Left, /*startsLine*/ true, /*rank*/ 0);
		buildPlayback(Lane::Centre, /*rank*/ 3);
		buildSpeed(Lane::Right, /*rank*/ 4);
		addStrip(v, strip_);

		// ── the on-air band ───────────────────────────────────────────
		auto *clipRow = new QWidget(this);
		auto *ch = new QHBoxLayout(clipRow);
		ch->setContentsMargins(0, 0, 0, 0);
		ch->setSpacing(0);
		auto *clip = new QLabel(
			QStringLiteral("  0003 · C2 · 50%          −00:03.20   Σ 00:11"),
			clipRow);
		clip->setStyleSheet(
			QString("background:%1;color:#fff;font-weight:700;font-size:10px;")
				.arg(sc_.onAir));
		clip->setFixedHeight(kClipBarH);
		// THE PLACEHOLDER MUST NOT SET THE PANEL'S FLOOR. In the panel
		// this band is a custom-painted widget that elides its own text;
		// here it is a QLabel, and a QLabel's minimum is the width of its
		// text — so the words chosen for the mockup, which mean nothing,
		// were deciding how narrow the dock could be made (364 px of a
		// 372 px floor). Ignored horizontally is what the real widget
		// behaves like.
		clip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		clip->setMinimumWidth(60);
		auto *skip = iconKey(Icon::SkipNext, QStringLiteral("skipNext"),
				     QStringLiteral("Clip successiva"), "mrSkip");
		skip->setFixedSize(30, kClipBarH - 6);
		skip->setMinimumHeight(0);
		auto *cl = new QHBoxLayout(clip);
		cl->setContentsMargins(4, 3, 4, 3);
		cl->addStretch(1);
		cl->addWidget(skip, 0, Qt::AlignVCenter);
		ch->addWidget(clip, 1);
		v->addWidget(clipRow, 0);

		v->addWidget(buildStatusBar());

		auto *seekRow = new QWidget(this);
		auto *skh = new QHBoxLayout(seekRow);
		skh->setContentsMargins(0, 0, 0, 0);
		skh->setSpacing(4);
		auto *seek = new QLabel(seekRow);
		seek->setStyleSheet(QString("background:%1;border:1px solid %2;")
					    .arg(sc_.sink2, sc_.border));
		seek->setFixedHeight(kSeekH);
		auto *zoom = iconTextKey(Icon::Zoom, QStringLiteral("1×"),
					 QStringLiteral("zoom"), "mrChanSel");
		zoom->setFixedHeight(kKeyH);
		skh->addWidget(seek, 1);
		skh->addWidget(zoom, 0, Qt::AlignBottom);
		v->addWidget(seekRow, 0);

		applyPanelMode(PanelMode::Wide, /*force*/ true);
	}

	ControlStrip *strip_ = nullptr;
	PanelMode mode_ = PanelMode::Wide;
	Scheme sc_;

	// THE ARRANGEMENT, applied. Everything here is a re-cell or a property
	// change; nothing is created, destroyed or re-parented, so the switch
	// costs a layout pass and not a native window.
	void applyPanelMode(PanelMode m, bool force = false)
	{
		if (!force && m == mode_)
			return;
		if (m != mode_)
			userSplit_ = false;
		mode_ = m;

		while (QLayoutItem *it = previewGrid_->takeAt(0))
			delete it;
		for (int c = 0; c < 4; c++) {
			previewGrid_->setColumnStretch(c, 0);
			// A column MINIMUM survives a re-cell, so the camera
			// block's reserved width would follow the panel into the
			// column arrangement, where that column is a bay.
			previewGrid_->setColumnMinimumWidth(c, 0);
		}
		for (int r = 0; r < 4; r++)
			previewGrid_->setRowStretch(r, 0);

		if (m == PanelMode::Tall) {
			// A COLUMN. A and B are PEERS — two bays, the same size —
			// so they share the top row, and the cameras run under
			// them as a filmstrip. Stacking all three full width does
			// not fit: at 340 px a 16:9 picture is 191 px tall, so
			// three ask for ~600 px of a pane that has about 330, and
			// what that produced was three squashed boxes with black
			// bars down the sides of each.
			previewGrid_->addWidget(aPic_, 0, 0);
			previewGrid_->addWidget(bPic_, 0, 1);
			previewGrid_->addWidget(tiles_, 1, 0, 1, 2);
			previewGrid_->setColumnStretch(0, 1);
			previewGrid_->setColumnStretch(1, g_haveB ? 1 : 0);
			splitter_->setOrientation(Qt::Vertical);
		} else if (m == PanelMode::Short) {
			// WIDE AND SHALLOW: the pictures cannot sit above the
			// list, so they sit beside it. Only the SPLITTER turns —
			// its children stay its children.
			previewGrid_->addWidget(aPic_, 0, 0);
			previewGrid_->addWidget(bPic_, 0, 1);
			previewGrid_->addWidget(tiles_, 0, 2);
			previewGrid_->setColumnStretch(0, 3);
			previewGrid_->setColumnStretch(1, g_haveB ? 3 : 0);
			previewGrid_->setColumnStretch(2, 2);
			previewGrid_->setRowStretch(0, 1);
			splitter_->setOrientation(Qt::Horizontal);
		} else {
			previewGrid_->addWidget(aPic_, 0, 0);
			previewGrid_->addWidget(bPic_, 0, 1);
			previewGrid_->addWidget(tiles_, 0, 2);
			previewGrid_->setColumnStretch(0, 3);
			previewGrid_->setColumnStretch(1, g_haveB ? 3 : 0);
			previewGrid_->setColumnStretch(2, 2);
			previewGrid_->setRowStretch(0, 1);
			splitter_->setOrientation(Qt::Vertical);
		}
		strip_->setStacked(m == PanelMode::Tall ? 1 : 0);
		applyCompactChrome(m == PanelMode::Tall);
		// The tile block is laid out from applyPreviewAspect, which is the
		// only place that knows how wide the pane really is. Doing it here
		// computed the column count from a pane that had not been given a
		// width yet — one column, eight rows, and a monitoring block
		// taller than the panel.
		tileCols_ = 0;
		applyPreviewAspect();
	}

	// ── WHEN THE PANEL IS A COLUMN, THE WORDS GO AND THE MARKS STAY ──────
	//
	// The status line and the toolbar are rows of LABELLED controls, and a row
	// of labels has a minimum width that is the sum of its words. With the
	// modes on the status line that came to 530 px — which is not "narrow", it
	// is wider than an OBS side dock, so the panel could not be put there at
	// all. Measured before this: 530. That is the same failure the control
	// strip has a fold for, one floor up, and it gets the same answer: every
	// one of these controls already carries its mark and its tooltip, so in a
	// column the word is the part that can go.
	//
	// NOTHING IS HIDDEN AND NOTHING MOVES. A key that was third from the left
	// is still third from the left, at the same size, doing the same thing —
	// which is the rule that separates this from "hide widgets when it gets
	// tight".
	void applyCompactChrome(bool compact)
	{
		if (compact == compactChrome_)
			return;
		compactChrome_ = compact;
		for (auto &wc : worded_) {
			wc.b->setText(compact ? QString() : wc.text);
			// A key with no label still needs a target: without this
			// the icon-only key collapses to its icon plus padding and
			// the row reads as a set of specks.
			wc.b->setMinimumWidth(compact ? 26 : 0);
		}
		if (statusDetail_)
			statusDetail_->setVisible(!compact);
		if (search_)
			search_->setFixedWidth(compact ? 90 : 150);
		if (projectLbl_)
			projectLbl_->setVisible(!compact);
	}

	// The tiles, laid into the block chosen for the room they were given. In
	// a column they are a FILMSTRIP across the panel; beside the bays they
	// are a block whose height matches theirs (see tileBlockFor).
	void relayTiles(int cols)
	{
		cols = std::max(1, cols);
		if (cols == tileCols_ && tilesGrid_->count() > 0)
			return;
		while (QLayoutItem *it = tilesGrid_->takeAt(0))
			delete it;
		// NO ALIGNMENT FLAG. An aligned layout item is given its
		// sizeHint, not its cell — so a tile whose minimum is nothing and
		// whose content is a caption would draw itself caption-sized and
		// the block would be a row of specks. Unaligned, it fills the cell
		// and its MAXIMUM is what holds it to a picture-shaped rectangle.
		for (int i = 0; i < kTiles; i++)
			tilesGrid_->addWidget(tile_[i], i / cols, i % cols);
		// EVERY CELL SHARES THE BLOCK, and the block is held to its size
		// from outside (tiles_ gets a maximum). The first version parked
		// the leftover in a spare column past the last tile — which was
		// right for a filmstrip that has room to spare and wrong
		// everywhere else: with the tiles' minimum at nothing, that column
		// took the whole block and eight cameras came out as eight 12 px
		// slivers with a wide empty strip beside them.
		const int rows = (kTiles + cols - 1) / cols;
		// EVERY row and column, not just the ones in use now. A
		// QGridLayout remembers the stretch of a row it no longer has
		// items in, and rowCount() never comes back down — so a block
		// that was once eight rows deep kept four EMPTY stretching rows
		// after it became four, and they took half the block's height.
		// Measured: 140×78 tiles asked for, 140×52 drawn.
		for (int c = 0; c < std::max(cols, tilesGrid_->columnCount()); c++)
			tilesGrid_->setColumnStretch(c, c < cols ? 1 : 0);
		for (int r = 0; r < std::max(rows, tilesGrid_->rowCount()); r++)
			tilesGrid_->setRowStretch(r, r < rows ? 1 : 0);
		tileCols_ = cols;
	}

	// Cap every tile, floor none of them. See the note in the else branch of
	// applyPreviewAspect for why the difference is the panel's floor.
	void setTileCaps(int w, int h)
	{
		w = std::max(1, w);
		h = std::max(1, h);
		for (int i = 0; i < kTiles; i++) {
			tile_[i]->setMinimumSize(0, 0);
			tile_[i]->setMaximumSize(w, h);
		}
		// …and the block itself, so it cannot be handed more room than
		// its tiles can use and spread them out inside it.
		const int rows = (kTiles + tileCols_ - 1) / std::max(1, tileCols_);
		tiles_->setMaximumWidth(tileCols_ * w + (tileCols_ - 1) * kTileGap);
		tiles_->setMaximumHeight(rows * h + (rows - 1) * kTileGap);
	}

	// A 16:9 picture in a box of another shape is drawn letterboxed, and the
	// difference comes out as BLACK BARS — which is what the panel showed in a
	// column, on both boxes. Every bar is a pixel the event list asked for, so
	// the monitoring pane is capped at the height its pictures can actually
	// fill and the splitter hands the rest to the list.
	static int aspectHeight(int w) { return std::max(1, w * 9 / 16); }

	void applyPreviewAspect()
	{
		const int paneW = std::max(80, previewBox_->width());
		// The channel strip rides with the pictures and is not one.
		const int stripH = 16;
		const int bays = g_haveB ? 2 : 1;
		int aW = paneW;
		TileBlock tb;
		if (mode_ != PanelMode::Tall) {
			// THE CAMERA BLOCK IS SIZED FIRST and the bays take what
			// is left, which is the opposite of what a stretch factor
			// does and the reason a stretch factor starved them.
			// In Short the pane is wide and SHALLOW — it stands beside
			// the list rather than above it — so how much height the
			// block may have is the binding constraint, not how much
			// width it would like.
			const int roomH =
				mode_ == PanelMode::Short
					? std::max(40, previewBox_->height() -
								 stripH - kTagH)
					: 0;
			tb = tileBlockFor(paneW, bays, g_cams, 3, roomH);
			relayTiles(tb.cols);
			tileCols_ = tb.cols;
			aW = std::max(60, (paneW - tb.blockW - 3 * bays) / bays);
		} else {
			const int avail = std::max(1, paneW);
			const int cols = std::clamp(avail / kTileMinWidth, 1,
						    std::max(1, g_cams));
			relayTiles(cols);
			tileCols_ = cols;
		}
		int want = aspectHeight(aW) + kTagH + stripH;
		if (mode_ != PanelMode::Tall)
			want = std::min(want, height() / 2);
		if (mode_ == PanelMode::Tall) {
			// Row 0 is A and B side by side; row 1 is the filmstrip.
			const int bayW = g_haveB ? (paneW - 3) / 2 : paneW;
			const int bayH = aspectHeight(bayW) + kTagH;
			const int cols = tileCols_;
			const int rows = (g_cams + cols - 1) / cols;
			const int tileW = std::min(kTileMaxWidth,
						   (paneW - 2 * (cols - 1)) / cols);
			const int stripRowH =
				rows * aspectHeight(tileW) + (rows - 1) * 2;
			// Each box capped too, not just the pane: capping the pane
			// alone leaves the grid free to stretch what is inside it.
			aPic_->setMaximumHeight(bayH);
			if (g_haveB)
				bPic_->setMaximumHeight(bayH);
			// …and let go of the width cap the wide arrangement puts
			// on them: a leftover maximum from another arrangement is
			// a picture pinned small the moment the dock changes shape.
			for (QWidget *w : {aPic_, bPic_})
				w->setMaximumWidth(QWIDGETSIZE_MAX);
			// setTileCaps sizes the block from the tiles it holds.
			setTileCaps(tileW, aspectHeight(tileW));
			want = bayH + 3 + stripRowH + stripH;
		} else {
			// The bays fill the row; the camera block gets a RESERVED
			// column of the grid and its tiles a maximum size inside
			// it. Left to a stretch factor the tiles got what was left
			// after the bays had their minimum — which is nothing, and
			// eight cameras came out as eight vertical slivers.
			//
			// A MAXIMUM, NEVER A FIXED SIZE. Fixed, the block's own
			// minimum height became the panel's: four rows of 78 px
			// put the floor at 668 and the panel stopped being able
			// to be made short at all, which is the thing this whole
			// redesign is for.
			// WHEN THE HEIGHT CAP BITES, THE BAY GETS NARROWER TOO.
			// The pictures may not have more than half the panel, and
			// on a wide single-bay rig a 16:9 A across the whole pane
			// wants three quarters of it — so the box ends up wider
			// than its picture and renderSourceFitted draws the
			// difference as BLACK BARS down either side. Capping the
			// width instead leaves the same space in the PANEL's own
			// colour, which is space rather than a picture with
			// something wrong with it.
			const int fitW = (want - kTagH - stripH) * 16 / 9;
			for (QWidget *w : {aPic_, bPic_}) {
				w->setMaximumHeight(QWIDGETSIZE_MAX);
				w->setMaximumWidth(fitW < aW ? fitW
							     : QWIDGETSIZE_MAX);
			}
			// setTileCaps sizes the block from the tiles it holds.
			setTileCaps(tb.tileW, tb.tileH);
			previewGrid_->setColumnStretch(2, 0);
			previewGrid_->setColumnMinimumWidth(2, tb.blockW);
		}

		// A cap AND the split: capped alone, the splitter can still hand
		// the pane LESS than the pictures need, and short is a black bar
		// too — down the sides instead of along the top.
		previewBox_->setMaximumHeight(want);
		// ONLY when the splitter divides HEIGHT. In Short it divides
		// WIDTH, so `want` — a height — means nothing to it.
		if (!userSplit_ && splitter_->orientation() == Qt::Vertical) {
			const int total = splitter_->height();
			const int give = std::min(want, total - kListFloor);
			const QList<int> now = splitter_->sizes();
			if (give > 0 && (now.isEmpty() || std::abs(now[0] - give) > 2))
				splitter_->setSizes({give, total - give});
		}
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		applyPanelMode(panelModeFor(size(), mode_));
		// AFTER the layout pass: a resizeEvent arrives before the children
		// are re-laid, so the splitter still reports its OLD height and a
		// split computed from it gets rescaled by the layout that follows.
		QTimer::singleShot(0, this, [this]() { applyPreviewAspect(); });
	}

private:
	static constexpr int kTiles = 8; // kMaxCameras
	// The tally band under a picture, at the height the style sheet gives it.
	static constexpr int kTagH = 12;
	// How much list is kept whatever the pictures ask for.
	static constexpr int kListFloor = 110;

public:
	QSplitter *splitter_ = nullptr;
	QWidget *previewBox_ = nullptr;
	int tileCols_ = 2;
	// What the camera block actually came out as. Reported by the render pass
	// because "the tiles look wrong" is answered with two numbers, not by
	// squinting at a PNG.
	QSize tileSize() const { return tile_[0] ? tile_[0]->size() : QSize(); }
	QSize tileBlockSize() const { return tiles_ ? tiles_->size() : QSize(); }

private:
	QGridLayout *previewGrid_ = nullptr;
	QWidget *aPic_ = nullptr, *bPic_ = nullptr;
	QWidget *tiles_ = nullptr;
	QGridLayout *tilesGrid_ = nullptr;
	QWidget *tile_[kTiles] = {};
	QTableWidget *table_ = nullptr;
	bool userSplit_ = false;

	// The controls whose LABEL is optional — see applyCompactChrome. The text
	// is kept here rather than read back off the button, because a button that
	// has already been stripped once has nothing to read back.
	struct Worded {
		QPushButton *b;
		QString text;
	};
	QVector<Worded> worded_;
	QWidget *statusDetail_ = nullptr; // "rim. 01:10:24"
	QLabel *search_ = nullptr;
	QLabel *projectLbl_ = nullptr;
	bool compactChrome_ = false;

	void remember(QPushButton *b) { worded_.push_back({b, b->text()}); }

	// ── the toolbar: what is GLOBAL to the panel, plus the search ───────
	QWidget *buildToolbar(QWidget *parent)
	{
		auto *box = new QWidget(parent);
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(5);
		auto *proj = new QLabel(QStringLiteral("Partita"), box);
		proj->setObjectName(QStringLiteral("mrMuted"));
		projectLbl_ = proj;
		h->addWidget(proj);
		h->addStretch(1);

		auto *mag = new QLabel(box);
		mag->setPixmap(iconFor(Icon::Search, QColor(sc_.textMuted), 13,
				       devicePixelRatioF())
				       .pixmap(13, 13));
		h->addWidget(mag);
		auto *search = new QLabel(QStringLiteral("Cerca…"), box);
		search->setObjectName(QStringLiteral("mrMuted"));
		search->setStyleSheet(
			QString("background:%1;border:1px solid %2;border-radius:3px;"
				"padding:2px 7px;color:%3;")
				.arg(sc_.raise1, sc_.border, sc_.textDim));
		search->setFixedWidth(150);
		search_ = search;
		h->addWidget(search);

		auto *live = key(QStringLiteral("Live"), "mrLive");
		live->setCheckable(true);
		live->setMinimumWidth(0);
		setKeyIcon(live, Icon::Live, g_tints, 12);
		setKeyId(live, QStringLiteral("live"));
		remember(live);
		h->addWidget(live);
		// A STATE, NOT A SIGNAL. Monitors used to be drawn with the Live
		// key's role, so "the pictures are on" — which is the resting
		// state of the panel — lit up in the same red as REC and the
		// on-air tally. Red has one meaning here and this is not it.
		auto *mon = key(QStringLiteral("Monitors"), "mrToggle");
		mon->setCheckable(true);
		mon->setChecked(true);
		mon->setMinimumWidth(0);
		setKeyIcon(mon, Icon::Monitors, g_tints, 12);
		setKeyId(mon, QStringLiteral("monitors"));
		remember(mon);
		h->addWidget(mon);
		auto *full = iconKey(Icon::FullScreen, QStringLiteral("fullscreen"),
				     QStringLiteral("Schermo intero"), "mrToggle");
		full->setCheckable(true);
		h->addWidget(full);
		h->addStretch(1);
		return box;
	}

	// ── the status line: it OWNS the modes ──────────────────────────────
	QWidget *buildStatusBar()
	{
		auto *box = new QWidget(this);
		box->setObjectName(QStringLiteral("mrStatusBar"));
		box->setFixedHeight(kStatusH);
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(6, 2, 6, 2);
		h->setSpacing(6);

		auto *elapsed = new QLabel(QStringLiteral("09:52:20"), box);
		elapsed->setObjectName(QStringLiteral("mrStatusValue"));
		elapsed->setProperty("rec", true);
		h->addWidget(elapsed);
		auto *left = new QLabel(QStringLiteral("rim. 01:10:24"), box);
		left->setObjectName(QStringLiteral("mrStatusText"));
		statusDetail_ = left;
		h->addWidget(left);
		auto *health = key(QStringLiteral("1"), "mrHealth");
		health->setProperty("level", QStringLiteral("warn"));
		health->setFixedHeight(kStatusH - 4);
		health->setMinimumHeight(0);
		setKeyIcon(health, Icon::Health, g_tints, 12);
		setKeyId(health, QStringLiteral("health"));
		h->addWidget(health);

		// THE MODES SIT AGAINST THE RIGHT EDGE, not in the middle. A group
		// centred on a bar whose width changes with the dock is a group
		// that moves every time the panel is resized; against an edge the
		// hand finds it the same way twice.
		h->addStretch(1);
		h->addWidget(statusSep(box));
		auto *now = statusKey(QStringLiteral("NOW"), QStringLiteral("now"));
		now->setCheckable(false);
		now->setProperty("now", true);
		setKeyIcon(now, Icon::Now, g_tints, 12);
		now->setToolTip(QStringLiteral("Torna al fronte live"));
		remember(now);
		h->addWidget(now);
		auto *loop = statusKey(QStringLiteral("LOOP"), QStringLiteral("loop"));
		setKeyIcon(loop, Icon::Loop, g_tints, 12);
		loop->setToolTip(QStringLiteral("Ripeti la clip"));
		remember(loop);
		h->addWidget(loop);
		auto *music = statusKey(QStringLiteral("MUSICA"),
					QStringLiteral("music"));
		setKeyIcon(music, Icon::Music, g_tints, 12);
		music->setToolTip(QStringLiteral("Musica sotto il replay"));
		remember(music);
		h->addWidget(music);
		auto *out = statusKey(QStringLiteral("IN OUTPUT"),
				      QStringLiteral("toOutput"));
		out->setChecked(true);
		setKeyIcon(out, Icon::ToOutput, g_tints, 12);
		out->setToolTip(QStringLiteral("Il replay prende il Program"));
		remember(out);
		h->addWidget(out);
		h->addWidget(statusSep(box));

		auto *speed = new QLabel(QStringLiteral("1.00×"), box);
		speed->setObjectName(QStringLiteral("mrStatusValue"));
		h->addWidget(speed);
		return box;
	}

	// MARK — one row, as on the reference panel. The keys keep their natural
	// widths: forcing them all to the widest ("Annulla") puts five short
	// labels in five wide keys, and the row stops reading as a row of marks
	// and starts reading as a form.
	//
	// IN AND OUT STAY WORDS. The brief asked for icons wherever an action is
	// universally recognisable, and these are the counter-example it names
	// itself: every mark on a timeline is a bracket of some kind, and a panel
	// whose two most-pressed keys are two brackets is a panel you have to
	// hover to use.
	void buildMark(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		auto *in = key(QStringLiteral("IN"));
		setKeyId(in, QStringLiteral("markIn"));
		auto *out = key(QStringLiteral("OUT"));
		setKeyId(out, QStringLiteral("markOut"));
		auto *m5 = key(QStringLiteral("−5s"));
		setKeyId(m5, QStringLiteral("mark5"));
		auto *m10 = key(QStringLiteral("−10s"));
		setKeyId(m10, QStringLiteral("mark10"));
		auto *m20 = key(QStringLiteral("−20s"));
		setKeyId(m20, QStringLiteral("mark20"));
		auto *tin = iconKey(Icon::TrimIn, QStringLiteral("trimIn"),
				    QStringLiteral("Porta l'IN qui"));
		auto *tout = iconKey(Icon::TrimOut, QStringLiteral("trimOut"),
				     QStringLiteral("Porta l'OUT qui"));
		auto *cancel = iconKey(Icon::Cancel, QStringLiteral("markCancel"),
				       QStringLiteral("Annulla la marcatura"),
				       "mrDanger");
		blk->setShapes(
			{{Cell(in), Cell(out), Cell(m5), Cell(m10), Cell(m20),
			  Cell(tin), Cell(tout), Cell(cancel)}},
			{{Cell(in, 2), Cell(out, 2), Cell(tin, 2), Cell(tout, 2)},
			 {Cell(m5, 2), Cell(m10, 2), Cell(m20, 2),
			  Cell(cancel, 2)}});
		strip_->addBlock(blk, lane, false, rank);
	}

	// ANGOLI — the bay selector, then the matrix, then the swap. Two rows,
	// and the two controls that are ABOUT both rows stand beside them
	// instead of under them.
	void buildAngles(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		QVector<Cell> rowA, rowB;
		QVector<QPushButton *> camKeys;
		for (int ch = 0; ch < 2; ch++) {
			auto *letter = new QLabel(ch ? "B" : "A", this);
			letter->setObjectName(QStringLiteral("mrSectionLabel"));
			letter->setFixedWidth(10);
			letter->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			// A widget that belongs to no shape is never re-parented
			// into the section, so it would sit at the panel's origin
			// drawn over the pictures. With one bay, B's row is not in
			// any shape.
			if (ch && !g_haveB)
				letter->hide();
			(ch ? rowB : rowA) << Cell(letter, 1, false);
			for (int i = 0; i < 8; i++) {
				auto *b = key(QStringLiteral("C%1").arg(i + 1),
					      "mrAngle");
				b->setCheckable(true);
				// The width is set per SHAPE, below — see the
				// setOnShape hook. Eight slots pinned at 46 px is
				// 400 px of matrix, and that alone put the
				// panel's floor past the width of an OBS side
				// dock.
				camKeys << b;
				setKeyId(b, QStringLiteral("angle%1%2")
						     .arg(ch ? "B" : "A")
						     .arg(i + 1));
				if (ch && !g_haveB) {
					b->hide();
				} else if (i >= g_cams) {
					// AN UNCONFIGURED SLOT IS DRAWN, NOT
					// HIDDEN — a sunken outline saying "a
					// camera could go here". It keeps angle 5
					// at the same x on every rig, so
					// configuring camera 3 does not slide the
					// row out from under the operator's
					// fingers mid-match, and it has nothing
					// to read so it costs no attention.
					//
					// It was hidden-with-retained-size here,
					// and that was not just a different look:
					// Qt measures a retained hidden widget at
					// the size it would have WITHOUT the width
					// this section pins on it, so six empty
					// slots pushed the section from 314 px to
					// 374 — and the panel could not be docked
					// down a side on a TWO-CAMERA rig, which
					// is the commonest one there is.
					b->setObjectName(
						QStringLiteral("mrAngleSlot"));
					b->setText(QString());
					b->setCheckable(false);
					b->setFocusPolicy(Qt::NoFocus);
				}
				(ch ? rowB : rowA) << Cell(b);
			}
		}

		auto *sel = new QWidget(this);
		auto *sl = new QHBoxLayout(sel);
		sl->setContentsMargins(0, 0, 0, 0);
		sl->setSpacing(3);
		// A↔B, not A|B: the brief's own suggestion, and it says what the
		// mode does (a command goes to both bays) rather than naming two
		// things with a bar between them.
		for (const char *l : {"A↔B", "A", "B"}) {
			auto *b = key(QString::fromUtf8(l), "mrChanSel");
			b->setCheckable(true);
			b->setFixedWidth(38);
			setKeyId(b, QStringLiteral("bay%1").arg(QString::fromUtf8(l)));
			sl->addWidget(b);
		}
		sel->setFixedHeight(kKeyH);

		auto *swap = iconKey(Icon::Swap, QStringLiteral("swapBays"),
				     QStringLiteral("Scambia A e B"), "mrChanSel");
		swap->setFixedWidth(38);

		// WITH ONE BAY THEY ARE ABSENT, NOT DISABLED — the B row, the
		// A↔B / A / B selector and the swap. That is what the panel does
		// (see applyChannelBVisibility), and modelling it matters here
		// rather than being a detail: on a single-bay rig the selector
		// spanning three of the matrix's columns pushed the section to
		// 374 px, so the SIMPLEST configuration was the one that could not
		// be docked down a side.
		if (!g_haveB) {
			sel->hide();
			swap->hide();
			BlockShape one{rowA};
			QVector<QVector<Cell>> compactOne;
			for (int i = 1; i < rowA.size(); i += 8) {
				QVector<Cell> line;
				line << (i == 1 ? rowA[0] : Cell(nullptr, 1));
				for (int k = i; k < rowA.size() && k < i + 8; k++)
					line << rowA[k];
				compactOne << line;
			}
			blk->setShapes(one, compactOne);
			blk->setOnShape([camKeys](bool flat) {
				for (QPushButton *b : camKeys)
					b->setFixedWidth(flat ? kAngleKeyMinW
							      : kAngleKeyW);
			});
			strip_->addBlock(blk, lane, false, rank);
			return;
		}

		QVector<Cell> top;
		top << Cell(sel, 1, false, 2);
		top += rowA;
		top << Cell(swap, 1, false, 2);
		QVector<Cell> bottom;
		bottom << Cell(nullptr, 1); // the selector's column, already taken
		bottom += rowB;

		// ── THE COMPACT SHAPE WRAPS THE MATRIX AT FOUR ─────────────────
		// Eight slots in one row is the widest thing on the panel, and it
		// is what stopped a side dock from being possible at all: with
		// eight cameras configured the panel demanded 530 px, and an OBS
		// dock down one side is 300-400. Wrapping at four takes that to
		// four columns.
		//
		// It costs a ROW PER BAY, which in the stack is 44 px of event
		// list, so it is the second answer and not the first: the keys
		// are given a width RANGE (see above) and compress instead.
		// Wrapping is what is left when even that does not fit, and with
		// the 34 px floor eight slots come to 310 px — inside a 340 px
		// side dock — so on this rig it never has to.
		//
		// POSITION STILL MEANS THE ANGLE. Wrapping row-major keeps C5
		// under C1, so the hand learns a block instead of a line — what it
		// must never do is RENUMBER, and nothing here does.
		const int slotsWide = 1 + g_cams * (kAngleKeyMinW + 4);
		const int perRow = slotsWide > kAngleWrapWidth ? 4 : 8;
		QVector<QVector<Cell>> compact;
		for (int ch = 0; ch < 2; ch++) {
			const QVector<Cell> &src = ch ? rowB : rowA;
			// src[0] is the bay letter; the keys follow.
			for (int i = 1; i < src.size(); i += perRow) {
				QVector<Cell> line;
				line << (i == 1 ? src[0] : Cell(nullptr, 1));
				for (int k = i; k < src.size() && k < i + perRow;
				     k++)
					line << src[k];
				compact << line;
			}
		}
		QVector<Cell> cSel;
		cSel << Cell(nullptr, 1) << Cell(sel, 3, false)
		     << Cell(nullptr, std::max(1, perRow - 4))
		     << Cell(swap, 1, false);
		compact << cSel;
		blk->setShapes({top, bottom}, compact);
		// THE KEYS ARE NARROWER IN THE FOLDED SHAPE, and this is the only
		// way to say so: the strip measures a section with sizeHint(), and
		// a maximumWidth does not move that — only a fixed width does. The
		// hook is called on every shape change, including the two flips
		// ControlStrip::measure() makes to read both sizes, so the wide
		// shape is measured at 46 and the folded one at 34 without anybody
		// having to sequence it.
		blk->setOnShape([camKeys](bool flat) {
			for (QPushButton *b : camKeys)
				b->setFixedWidth(flat ? kAngleKeyMinW
						      : kAngleKeyW);
		});
		strip_->addBlock(blk, lane, false, rank);
	}

	// EXPORT — the reference keeps it in the far corner of the first row.
	void buildExport(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		auto *up = iconKey(Icon::MoveUp, QStringLiteral("moveUp"),
				   QStringLiteral("Sposta su"));
		auto *dn = iconKey(Icon::MoveDown, QStringLiteral("moveDown"),
				   QStringLiteral("Sposta giù"));
		auto *more = iconKey(Icon::More, QStringLiteral("clipActions"),
				     QStringLiteral("Duplica · Elimina"));
		auto *exp = iconTextKey(Icon::ExportClip,
					QStringLiteral("Esporta"),
					QStringLiteral("exportClips"));
		exp->setToolTip(QStringLiteral("Esporta le clip selezionate"));
		auto *reel = iconTextKey(Icon::ExportReel,
					 QStringLiteral("Sequenza"),
					 QStringLiteral("exportReel"));
		reel->setToolTip(QStringLiteral("Esporta la selezione in un file solo"));
		remember(exp);
		remember(reel);
		// ONE ROW IN BOTH SHAPES. It was two folded — the three ordering
		// keys with one export, the reel underneath — which cost a line of
		// the stack for the section nobody touches while the ball is in
		// play. Folded, the two exports lose their words and keep their
		// marks (one block, or a strip of them), so five keys come to
		// ~180 px and the line is back.
		blk->setShapes({{Cell(up), Cell(dn), Cell(more), Cell(exp),
				 Cell(reel)}},
			       {{Cell(up), Cell(dn), Cell(more), Cell(exp),
				 Cell(reel)}});
		strip_->addBlock(blk, lane, false, rank);
	}

	// REC — the take. Gear, the record key, and the wall clock beside them.
	// The elapsed time and the health badge moved to the status line: they
	// are readings, not commands, and this section is where the take is
	// ARMED.
	void buildRec(Lane lane, bool startsLine, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		auto *gear = iconKey(Icon::Gear, QStringLiteral("settings"),
				     QStringLiteral("Impostazioni"), "mrGear");
		gear->setFixedWidth(30);
		auto *rec = key(QStringLiteral("REC"), "mrRec");
		rec->setProperty("recording", false);
		rec->setMinimumWidth(74);
		setKeyIcon(rec, Icon::Live, g_tints, 12);
		setKeyId(rec, QStringLiteral("rec"));

		auto *clock = new QLabel(QStringLiteral("17:22:06"), this);
		clock->setObjectName(QStringLiteral("mrClock"));
		clock->setFixedWidth(58);

		blk->setShapes({{Cell(gear), Cell(rec), Cell(clock, 1, false)}},
			       {{Cell(gear), Cell(rec, 2)},
				{Cell(clock, 3, false)}});
		strip_->addBlock(blk, lane, startsLine, rank);
	}

	// RIPRODUZIONE — the reference's centre group, in its order. Four keys
	// lighter than it was: NOW, Loop, music and "in output" live on the
	// status line now, so what is left here is what DRIVES the picture.
	void buildPlayback(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		auto *pp = iconKey(Icon::Play, QStringLiteral("playPause"),
				   QStringLiteral("Riproduci / pausa"), "mrPlay");
		auto *stop = iconKey(Icon::Stop, QStringLiteral("stop"),
				     QStringLiteral("Stop"));
		auto *rev = iconKey(Icon::Reverse, QStringLiteral("playReverse"),
				    QStringLiteral("Riproduci all'indietro"));
		auto *last = iconKey(Icon::PlayLast, QStringLiteral("playLast"),
				     QStringLiteral("Riproduci l'ultimo evento"));
		auto *sb = iconKey(Icon::StepBack, QStringLiteral("stepBack"),
				   QStringLiteral("Un fotogramma indietro"));
		auto *sf = iconKey(Icon::StepFwd, QStringLiteral("stepFwd"),
				   QStringLiteral("Un fotogramma avanti"));
		auto *play = key(QStringLiteral("Riproduci eventi"), "mrAccent");
		setKeyId(play, QStringLiteral("playEvents"));
		auto *more = iconKey(Icon::Menu, QStringLiteral("playOptions"),
				     QStringLiteral("Altre opzioni"), "mrGear");
		more->setFixedWidth(22);
		blk->setShapes(
			{{Cell(pp), Cell(stop), Cell(rev), Cell(last), Cell(sb),
			  Cell(sf), Cell(play), Cell(more)}},
			{{Cell(play, 5), Cell(more)},
			 {Cell(pp), Cell(stop), Cell(rev), Cell(last), Cell(sb),
			  Cell(sf)}});
		strip_->addBlock(blk, lane, false, rank);
	}

	// VELOCITA — presets then dial, as on the reference panel.
	void buildSpeed(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		QVector<Cell> row;
		QList<QPushButton *> chips;
		for (const char *l : {"25%", "33%", "50%", "75%", "100%", "2×"}) {
			auto *b = key(QString::fromUtf8(l), "mrSpeedChip");
			setKeyId(b, QStringLiteral("speed%1").arg(
					    QString::fromUtf8(l)));
			chips << b;
			row << Cell(b);
		}
		equaliseKeyWidths(chips);
		auto *dial = new QSlider(Qt::Horizontal, this);
		dial->setObjectName(QStringLiteral("mrSpeed"));
		dial->setRange(5, 200);
		dial->setValue(100);
		dial->setMinimumWidth(110);
		dial->setFixedHeight(kKeyH);
		blk->setShapes({row, {Cell(dial, 6)}},
			       {{Cell(chips[0]), Cell(chips[1]), Cell(chips[2]),
				 Cell(chips[3]), Cell(chips[4]), Cell(chips[5])},
				{Cell(dial, 6)}});
		strip_->addBlock(blk, lane, false, rank);
	}
};

} // namespace

// ---------------------------------------------------------------------------
// --check — the part of this that is a GATE rather than a look
// ---------------------------------------------------------------------------
//
// Rendering seven PNGs answers "does it look right", which needs a person. These
// answer "is anything off the panel, is anything too small to hit, does the
// arrangement change when it is supposed to" — which does not, and therefore
// should not wait for one. Exit code 0 = every check true.
namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
	if (!ok)
		g_fail++;
	std::printf("%-52s %s%s%s\n", qUtf8Printable(what), ok ? "OK" : "FAIL",
		    detail.isEmpty() ? "" : "  ", qUtf8Printable(detail));
}

// WCAG relative luminance, so "can this be read" is a number rather than an
// opinion. The panel is a working tool in a room with the lights up.
double luminance(const QColor &c)
{
	auto ch = [](double v) {
		return v <= 0.03928 ? v / 12.92
				    : std::pow((v + 0.055) / 1.055, 2.4);
	};
	return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) +
	       0.0722 * ch(c.blueF());
}

double contrast(const QColor &a, const QColor &b)
{
	const double la = luminance(a), lb = luminance(b);
	return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Everything visible has to be ON the panel. This is the check that would have
// caught VELOCITA and EXPORT being cut off the bottom of the 340x900 column —
// which a size hint reported correctly and the layout then ignored.
void checkNothingClipped(Mock *w, const QString &label)
{
	const QRect panel(QPoint(0, 0), w->size());
	QString worst;
	int off = 0;
	for (QWidget *c : w->findChildren<QWidget *>()) {
		// isVisible(), NOT !isHidden(): a widget inside a hidden parent
		// reports isHidden() false — its own flag was never touched — so
		// the first version of this check was reading the geometry of
		// things that are not on the screen at all.
		if (!c->isVisible() || c->width() <= 0 || c->height() <= 0)
			continue;
		// Only leaves: a container reporting a rect its children do not
		// occupy is not a clipped control.
		if (!c->findChildren<QWidget *>().isEmpty())
			continue;
		const QRect r(c->mapTo(w, QPoint(0, 0)), c->size());
		if (panel.contains(r))
			continue;
		off++;
		if (worst.isEmpty())
			worst = QString("%1 %2,%3 %4x%5")
					.arg(c->objectName().isEmpty()
						     ? QString(c->metaObject()
								       ->className())
						     : c->objectName())
					.arg(r.x())
					.arg(r.y())
					.arg(r.width())
					.arg(r.height());
	}
	check(off == 0, label + ": nothing clipped",
	      off ? QString("%1 off-panel, first %2").arg(off).arg(worst)
		  : QString());
}

// A key smaller than this is a key that cannot be hit under pressure. It is the
// failure mode a QHBoxLayout produces when it runs out of width, and the reason
// FlowLayout exists. The floor came down with kKeyH (28 → 26, folded 24 → 22),
// so it is stated against kKeyFoldedH rather than as a loose number: if the key
// height is ever cut again, this check has to be a deliberate part of it.
void checkHitTargets(Mock *w, const QString &label)
{
	int small = 0;
	QString worst;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		if (!b->isVisible())
			continue;
		// The status line's own keys are shorter by design — they are
		// pressed while being looked at, not reached for blind.
		const int floor = b->objectName() == QStringLiteral("mrStatKey")
					  ? 16
					  : kKeyFoldedH - 4;
		if (b->width() >= 20 && b->height() >= floor)
			continue;
		small++;
		if (worst.isEmpty())
			worst = QString("%1 %2 %3x%4")
					.arg(b->objectName(),
					     b->property(kKeyProperty).toString())
					.arg(b->width())
					.arg(b->height());
	}
	check(small == 0, label + ": every key is hittable",
	      small ? QString("%1 too small, first %2").arg(small).arg(worst)
		    : QString());
}

// EVERY COMMAND KEY CARRIES ITS IDENTITY. This is the check that makes the
// icon-first panel safe to change again: the automated gate used to find twelve
// keys by their literal text ("⏭", "■", "-5s"), which made a redrawing or a
// translation look exactly like a broken key. If a key here has no mrKey, the
// gate cannot find it and nothing else will say so.
void checkKeyIds(Mock *w)
{
	int missing = 0, ids = 0;
	QString worst;
	QStringList seen;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		// Qt builds buttons of its own inside its widgets (a table view's
		// corner button, a scroll area's). They are not this panel's keys
		// and nothing outside Qt ever presses them.
		if (b->objectName().startsWith(QStringLiteral("qt_")))
			continue;
		const QString id = b->property(kKeyProperty).toString();
		if (id.isEmpty()) {
			missing++;
			if (worst.isEmpty())
				worst = b->objectName() + " '" + b->text() + "'";
			continue;
		}
		ids++;
		if (seen.contains(id) && worst.isEmpty())
			worst = "duplicate " + id;
		seen << id;
	}
	check(missing == 0, "every key carries an mrKey id",
	      missing ? QString("%1 without, first %2").arg(missing).arg(worst)
		      : QString("%1 keys").arg(ids));
	check(seen.size() == QSet<QString>(seen.begin(), seen.end()).size(),
	      "no two keys share an id", worst);
}

// AN ICON HAS TO BE VISIBLE ON THE KEY IT IS DRAWN ON. The marks are pixmaps,
// so unlike every label on this panel they do not follow the style sheet — a
// theme that moved the key colour under them would leave them where they were.
void checkIconContrast(const Scheme &s, const QString &label)
{
	const IconTints t = tintsFor(s);
	const QColor keyBg(s.raise1);
	check(contrast(t.rest, keyBg) >= 3.0,
	      label + ": a mark at rest reads on its key",
	      QString("%1:1").arg(contrast(t.rest, keyBg), 0, 'f', 1));
	check(contrast(t.on, QColor(s.pvwBg)) >= 3.0,
	      label + ": a mark on a LIT key reads",
	      QString("%1:1").arg(contrast(t.on, QColor(s.pvwBg)), 0, 'f', 1));
}

// WHICH CHILD IS SETTING THE FLOOR. "min width 530" is a number nobody can act
// on: six widgets have an opinion about it and five of them are innocent. This
// names the widest one, which is the only thing that turns the failure into a
// piece of work.
QString widestMinimum(Mock *w)
{
	// THE TOP THREE, with their parents. One name was not enough: the widest
	// child is usually a plain QWidget that some section put its keys in, and
	// "QWidget (374)" is a number with nowhere to go. The chain says which
	// section.
	QVector<QPair<int, QString>> found;
	for (QWidget *c : w->findChildren<QWidget *>()) {
		if (!c->isVisible())
			continue;
		QString name = c->objectName().isEmpty()
				       ? QString(c->metaObject()->className())
				       : c->objectName();
		for (QWidget *p = c->parentWidget(); p && p != w;
		     p = p->parentWidget())
			name += "<" + (p->objectName().isEmpty()
					       ? QString(p->metaObject()
								 ->className())
					       : p->objectName());
		found << qMakePair(c->minimumSizeHint().width(), name);
	}
	std::sort(found.begin(), found.end(),
		  [](const auto &a, const auto &b) { return a.first > b.first; });
	QStringList top;
	for (int i = 0; i < found.size() && i < 3; i++)
		top << QString("%1 (%2)").arg(found[i].second).arg(found[i].first);
	return QString("min width %1, widest: %2")
		.arg(w->minimumSizeHint().width())
		.arg(top.join(", "));
}

int runChecks(QPalette pal)
{
	struct Want {
		const char *name;
		int w, h;
		PanelMode mode;
	};
	const Want targets[] = {
		{"wide", 1500, 900, PanelMode::Wide},
		{"short", 1400, 340, PanelMode::Short},
		{"tall", 340, 900, PanelMode::Tall},
	};

	for (const Want &t : targets) {
		auto *w = new Mock();
		// THREE PASSES, and the number is measured rather than
		// superstitious: a mode change rewrites the panel's floor, so a
		// size only the NEW arrangement can hold takes a second event to
		// reach. The third is slack.
		for (int pass = 0; pass < 3; pass++) {
			w->resize(t.w, t.h);
			w->show();
			for (int i = 0; i < 3; i++) {
				QApplication::processEvents();
				QApplication::sendPostedEvents();
			}
		}
		std::printf("   [%s] size %dx%d  min %dx%d  mode %s  strip %d\n",
			    t.name, w->width(), w->height(),
			    w->minimumSizeHint().width(),
			    w->minimumSizeHint().height(),
			    panelModeName(w->mode_),
			    w->strip_->minHeightForWidth(w->strip_->width()));
		const QString label = QString(t.name);
		check(w->mode_ == t.mode, label + ": arrangement chosen",
		      QString("got %1").arg(panelModeName(w->mode_)));
		check(w->height() <= t.h, label + ": fits the height it was given",
		      QString("asked %1, got %2").arg(t.h).arg(w->height()));
		// THE PANEL HAS TO FIT A REAL OBS DOCK, and the claim is made
		// where it means something: in the COLUMN arrangement. A vertical
		// dock in OBS is 300-400 px wide, and with eight cameras
		// configured the panel demanded 411 before this redesign and 530
		// half way through it — which is not "narrow", it is "cannot be
		// docked there at all", on the rig this is most often run on.
		//
		// Asking it of the wide arrangement would be asking the wrong
		// question: a panel that is 1500 px wide is wearing the shape for
		// 1500 px, and dragging it narrow changes the shape first (see
		// panelModeFor). What must be true is that the shape it lands in
		// fits.
		if (t.mode == PanelMode::Tall) {
			const bool fits = w->minimumSizeHint().width() <= 340;
			check(fits, label + ": fits a side dock",
			      widestMinimum(w));
			// WHICH SECTION, in both shapes. The floor is the widest
			// section's NARROW shape, and knowing that it is #1 at
			// 374 px flat is the difference between a number and a
			// piece of work.
			if (!fits)
				std::printf("      sections: %s\n",
					    qUtf8Printable(
						    w->strip_->describeBlocks()));
		}
		checkNothingClipped(w, label);
		checkHitTargets(w, label);
		if (label == QStringLiteral("wide"))
			checkKeyIds(w);
		w->hide();
		delete w;
	}

	// HYSTERESIS, both directions. Without it a dock edge dragged across the
	// boundary flips the arrangement back and forth, and every flip re-lays
	// the panel's displays — a swap chain re-allocated on the graphics thread,
	// several times a second, while a take is recording.
	const QSize edge(kTallMaxWidth + 10, 900);
	check(panelModeFor(edge, PanelMode::Tall) == PanelMode::Tall,
	      "hysteresis: a column does not flip out early");
	check(panelModeFor(edge, PanelMode::Wide) == PanelMode::Wide,
	      "hysteresis: a wide panel stays wide");
	check(panelModeFor(QSize(kTallMaxWidth + kModeHysteresis + 10, 900),
			   PanelMode::Tall) == PanelMode::Wide,
	      "hysteresis: past the far edge it does flip");

	// The scheme, on a LIGHT palette — the case that never existed before and
	// the one where a hardcoded near-black panel would simply vanish.
	const Scheme light = schemeFor(ThemeChoice::FollowObs, pal);
	const QColor bg(light.panel);
	check(contrast(QColor(light.text), bg) >= 4.5,
	      "light: body text is readable",
	      QString("%1:1").arg(contrast(QColor(light.text), bg), 0, 'f', 1));
	check(contrast(QColor(light.textKey), bg) >= 3.0,
	      "light: a key label is readable",
	      QString("%1:1").arg(contrast(QColor(light.textKey), bg), 0, 'f',
				  1));
	// SIGNAL SURVIVES THE THEME. This is the check the whole two-kinds-of-
	// colour split exists for: on a white panel the same red has to still be
	// findable, or "red means on air" has quietly stopped being true.
	// NOT called `signals`: with Qt keywords enabled that is a macro expanding
	// to `public:`, and the declaration becomes a syntax error several lines
	// later.
	const std::pair<const char *, QString> sigColours[] = {
		{"rec", light.rec},
		{"pvw", light.pvw},
		{"onAir", light.onAir},
		{"action", light.action},
	};
	for (const auto &[name, colour] : sigColours)
		check(contrast(QColor(colour), bg) >= 3.0,
		      QString("light: %1 separates from the panel").arg(name),
		      QString("%1:1").arg(contrast(QColor(colour), bg), 0, 'f',
					  1));
	checkIconContrast(light, "light");
	checkIconContrast(schemeFor(ThemeChoice::Broadcast, pal), "dark");

	std::printf("\n%s  (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
	return g_fail ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QStringList args = app.arguments();
	for (const QString &a : args) {
		if (a.startsWith(QStringLiteral("--cams=")))
			g_cams = qBound(1, a.mid(7).toInt(), 8);
		if (a == QStringLiteral("--nob"))
			g_haveB = false;
	}
	if (args.contains(QStringLiteral("--theme=obs")))
		g_theme = ThemeChoice::FollowObs;
	else if (args.contains(QStringLiteral("--theme=contrast")))
		g_theme = ThemeChoice::HighContrast;
	g_pal = app.palette();
	if (args.contains(QStringLiteral("--theme=light"))) {
		// A LIGHT theme by hand, because the only way to know the derived
		// scale and the signal colours survive one is to look at one.
		g_theme = ThemeChoice::FollowObs;
		g_pal.setColor(QPalette::Window, QColor("#efefef"));
		g_pal.setColor(QPalette::WindowText, QColor("#101010"));
		g_pal.setColor(QPalette::Base, QColor("#ffffff"));
		g_pal.setColor(QPalette::Highlight, QColor("#2f6fd0"));
		g_pal.setColor(QPalette::HighlightedText, QColor("#ffffff"));
	}
	g_sc = schemeFor(g_theme, g_pal);
	g_tints = tintsFor(g_sc);

	if (args.contains(QStringLiteral("--check"))) {
		// A LIGHT palette on purpose, whatever this machine is themed
		// with: the light path is the one that never existed before the
		// panel could follow a theme, and it is the one where a scheme
		// built for a near-black panel disappears.
		QPalette lp = app.palette();
		lp.setColor(QPalette::Window, QColor("#efefef"));
		lp.setColor(QPalette::WindowText, QColor("#101010"));
		lp.setColor(QPalette::Base, QColor("#ffffff"));
		lp.setColor(QPalette::Highlight, QColor("#2f6fd0"));
		lp.setColor(QPalette::HighlightedText, QColor("#ffffff"));
		return runChecks(lp);
	}

	auto *w = new Mock();

	if (args.contains(QStringLiteral("--show"))) {
		w->resize(1100, 800);
		w->show();
		return app.exec();
	}

	const QString outDir = args.size() > 1 ? args[1] : QStringLiteral(".");
	QDir().mkpath(outDir);

	struct Size {
		const char *name;
		int w, h;
	};
	// THE THREE TARGETS FIRST, because they are the shapes an operator
	// actually gets: a maximised window on a second monitor, a dock under the
	// OBS preview, a dock down one side. The rest are the corners that used to
	// break.
	QVector<Size> sizes = {{"target-wide", 1500, 900},
			       {"target-short", 1400, 340},
			       {"target-tall", 340, 900},
			       {"portrait-narrow", 520, 980},
			       {"landscape-short", 1500, 560},
			       {"tiny", 460, 420},
			       {"floating", 1000, 760}};
	if (args.size() > 2) {
		const QStringList wh = args[2].split(QLatin1Char('x'));
		if (wh.size() == 2)
			sizes = {{"custom", wh[0].toInt(), wh[1].toInt()}};
	}

	for (const Size &s : sizes) {
		// TWICE, and it is not superstition: the first resize can change
		// the panel's MODE, and a mode change rewrites the minimums the
		// layout is about to be measured against.
		for (int pass = 0; pass < 2; pass++) {
			w->resize(s.w, s.h);
			w->show();
			for (int i = 0; i < 3; i++) {
				QApplication::processEvents();
				QApplication::sendPostedEvents();
			}
		}
		const QString path =
			QDir(outDir).filePath(QString("mock-%1.png").arg(s.name));
		w->grab().save(path);
		// The strip's own height is printed because it is the number that
		// decides whether the pictures and the list can both have what
		// they need: on a 340x900 column it is the single biggest item on
		// the panel, and "the keys take too much room" is a claim that
		// should be answered with it rather than by eye.
		std::printf("   tiles %dx%d block %dx%d\n",
			    w->tileSize().width(), w->tileSize().height(),
			    w->tileBlockSize().width(), w->tileBlockSize().height());
		std::printf("   strip %d px, preview cap %d px, split %d/%d, pane %d, tile cols %d\n",
			    w->strip_->minHeightForWidth(w->strip_->width()),
			    w->previewBox_->maximumHeight(),
			    w->splitter_->sizes().value(0),
			    w->splitter_->sizes().value(1),
			    w->previewBox_->height(), w->tileCols_);
		std::printf("%-16s asked %4dx%4d  got %4dx%4d  min %4dx%4d  %-5s strip=%s\n",
			    s.name, s.w, s.h, w->width(), w->height(),
			    w->minimumSizeHint().width(),
			    w->minimumSizeHint().height(),
			    panelModeName(w->mode_),
			    w->strip_->isFlat() ? "stack" : "lanes");
	}
	return 0;
}
