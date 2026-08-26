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
//   A | B ‖ the camera tiles         the monitoring block, a DRAGGABLE split
//   project · search · Live · Monitors · ⚙ · ⛶
//   1 2 3 4 …                        the list tabs
//   the event table                  the elastic zone
//   −5 −10 −20 / IN OUT ✕ / trims │ A↔B │ speeds / dial / export
//   ● REC clock                    │ transport + NOW + the green PLAY
//   LOOP ♫ IN OUTPUT                 the status line
//   the on-air band
//   the position bar
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
#include <QSet>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace multireplay;

namespace {

// The status line. Shorter than a key row because nothing in it is a target the
// hand goes to blind — the modes are pressed while being looked at.
constexpr int kStatusH = 22;
// The on-air band, and the position bar. The band was 28 and the bar 52 with a
// caption over each; the captions are gone (the band is green and the bar is
// graduated — neither has ever needed a heading to be told apart) and the bar's
// ruler is tighter.
constexpr int kClipBarH = 24;
constexpr int kSeekH = 42;
// Narrower than this a tile stops being a picture and becomes a smear; wider
// than this it stops being a confidence monitor and starts competing with the
// bay it is meant to be checked against.
constexpr int kTileMinWidth = 78;
constexpr int kTileMaxWidth = 150;


// The naming band under a picture. AspectBox owns the number; this is the same
// one, named locally so the block arithmetic below reads.
constexpr int kTagH = AspectBox::kTagH;

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

// A KEY THAT IS A MARK AND A WORD, sized so the word actually fits.
//
// Qt under-measures a stylesheet-styled QPushButton that carries both: the
// padding in the sheet is not folded into sizeHint(), and "Monitors" came out
// as "Monitor:" with its last letter cut off. So the width is worked out here
// from the two things that are actually in the key.
QPushButton *iconTextKey(Icon ic, const QString &text, const QString &id,
			 const char *role = "", int iconPx = 14)
{
	auto *b = key(text, role);
	setKeyIcon(b, ic, g_tints, iconPx);
	setKeyId(b, id);
	b->ensurePolished();
	// A LABELLED KEY IS ITS OWN SIZE, and this is the only way to say so that
	// actually holds. Two others were tried: a width computed from the font
	// metrics (20 px short — the metrics of a widget that has not been shown
	// are not the ones it is painted with, and the style sheet's padding is
	// in neither), and setMinimumWidth from the size hint, which the style
	// sheet's own minimum then overrode. Fixed leaves the layout no room to
	// squeeze it, which is how "Monitors" came to ship as "Monitor:".
	//
	// Inside a KeyBlock this is promoted back to Preferred for any cell that
	// declared grow (see KeyBlock::apply), so a key that is meant to fill its
	// row still does.
	b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	return b;
}

QWidget *statusSep(QWidget *parent)
{
	auto *s = new QWidget(parent);
	s->setObjectName(QStringLiteral("mrStatSep"));
	s->setFixedWidth(1);
	return s;
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
//   minimum, and the tiles' minimum was nothing.
//
//   ceil(sqrt(n)) COLUMNS. Square-ish, and wrong at exactly the count this
//   panel is most often asked for: eight cameras became 3×3 with an empty cell
//   in the corner, and the eye finds that hole every time it reads the block.
//
// What the block actually has to do is stand beside two 16:9 pictures and be
// ABOUT AS TALL AS THEY ARE. So the arrangement is chosen from the bay height:
// for each column count that wastes no cell, work out how big a 16:9 tile would
// have to be to fill that height in the rows it implies, and keep the one whose
// block comes out nearest the width the block is meant to have.
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
// Between two tiles. It was 2, and with a naming band under each picture that
// put one row's label hard against the next row's picture — the block read as
// one striped rectangle rather than as N monitors.
constexpr int kTileGap = 4;

// `maxH` is how much HEIGHT the block may actually have, which is a different
// question from how tall the bays are and the one that matters when the panel
// is docked under the OBS preview: there the pane is 750 px wide and 120 px
// tall, so an arrangement chosen from the width alone asks for four rows of
// tiles in a pane that has room for two. 0 = no limit.
TileBlock tileBlockFor(int paneW, int bays, int n, int gap, int maxH = 0)
{
	TileBlock best;
	if (n <= 0 || paneW <= 0)
		return best;
	auto aspect = [](int w) { return std::max(1, w * 9 / 16); };
	const int target = (int)(paneW * kTileShare);
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
		const int bh = rows * (th + kTagH) + (rows - 1) * kTileGap;
		// Width is the preference; HEIGHT IS A WALL. A block that does
		// not fit the pane is not a slightly worse arrangement, it is a
		// floor the operator cannot get back.
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
			rows * (th + kTagH) + (rows - 1) * kTileGap};
	}
	return best;
}

// A monitor box with a dummy picture in it. The SHAPE is AspectBox's job (it is
// in src/dock-layout so the panel and this run the same rule); all this adds is
// something black to look at and a band that can be tallied.
class PictureBox : public AspectBox {
public:
	PictureBox(const QString &text, const char *tagRole, const Scheme &sc,
		   QWidget *parent)
		: AspectBox(parent)
	{
		auto *pic = new QLabel(text, this);
		pic->setAlignment(Qt::AlignCenter);
		pic->setStyleSheet(
			QString("background:#000;color:%1;font-size:10px;")
				.arg(sc.textDim));
		tag_ = new QLabel(text, this);
		tag_->setObjectName(QString::fromLatin1(tagRole));
		tag_->setProperty("chan", text);
		tag_->setProperty("active", text == QStringLiteral("A"));
		tag_->setAlignment(Qt::AlignCenter);
		setContents(pic, tag_);
	}

	void setTally(const char *what)
	{
		tag_->setProperty("tally", QString::fromLatin1(what));
	}

private:
	QLabel *tag_ = nullptr;
};

class Mock : public QWidget {
public:
	// A stand-in for one of the panel's pictures, with the band that names
	// it underneath. The real ones are OBSQTDisplay widgets with a swap
	// chain behind them; here they only have to occupy the same room and
	// prove the arrangement puts them somewhere usable.
	//
	// EVERY picture gets a band, cameras included. It was only on A and B,
	// and a camera box with no name is a rectangle the operator has to
	// identify by remembering where it is.
	PictureBox *pic(const QString &text, const char *tagRole)
	{
		return new PictureBox(text, tagRole, g_sc, this);
	}

	Mock()
	{
		setObjectName(QStringLiteral("MultiReplayDock"));
		sc_ = g_sc;
		setStyleSheet(dockStyle(sc_));
		setMinimumWidth(300);

		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(4, 4, 4, 4);
		v->setSpacing(3);
		root_ = v;

		buildMonitors();

		// ── the list pane: the toolbar, the tabs, the table ───────────
		// THE TOOLBAR IS UNDER THE PICTURES, not above them. The pictures
		// lead — that is what an operator's eye goes to — and everything
		// that picks WHICH event (search, the tabs) belongs to the list it
		// filters, so it lives with the list.
		listPane_ = new QWidget(this);
		auto *lv = new QVBoxLayout(listPane_);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(2);
		lv->addWidget(buildToolbar(listPane_));
		auto *tabs = new QLabel(
			QStringLiteral(" 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │ 10 "),
			listPane_);
		tabs->setObjectName(QStringLiteral("mrMuted"));
		tabs->setFixedHeight(20);
		tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		lv->addWidget(tabs);
		table_ = new QTableWidget(6, 6, listPane_);
		table_->setObjectName(QStringLiteral("mrEvents"));
		table_->verticalHeader()->setVisible(false);
		table_->setMinimumHeight(50);
		table_->setSizePolicy(QSizePolicy::Expanding,
				      QSizePolicy::Expanding);
		lv->addWidget(table_, 1);

		// THE BODY IS ONE SPLITTER of two panes, and which way it divides
		// is the arrangement. Vertical: pictures over list. Horizontal
		// (the shallow dock under the OBS preview): everything on the
		// left, the LIST on the right — which is the only shape in which
		// a 340 px tall panel can still show a usable table.
		bodySplit_ = new QSplitter(Qt::Vertical, this);
		bodySplit_->setChildrenCollapsible(false);
		bodySplit_->setHandleWidth(5);
		bodySplit_->addWidget(leftCol_);
		bodySplit_->addWidget(listPane_);
		bodySplit_->setStretchFactor(0, 3);
		bodySplit_->setStretchFactor(1, 2);
		connect(bodySplit_, &QSplitter::splitterMoved, this,
			[this](int, int) {
				userSplit_[modeIdx()] = true;
				savedBody_[modeIdx()] = bodySplit_->saveState();
			});
		v->addWidget(bodySplit_, 1);

		buildControls();
		v->addWidget(controls_);

		applyPanelMode(PanelMode::Wide, /*force*/ true);
	}

	ControlStrip *strip_ = nullptr;
	PanelMode mode_ = PanelMode::Wide;
	// What the wide arrangement asks for, measured while it is worn: Short is
	// chosen when the panel cannot be that tall, and that moves with the width.
	int wideFloorH_ = 0;
	Scheme sc_;

	// THE ARRANGEMENT, applied. Everything here is a re-cell, a splitter
	// orientation or a property change; NOTHING that holds a picture is ever
	// re-parented, because in the panel that destroys an OBSQTDisplay's
	// native window and strands its obs_display.
	void applyPanelMode(PanelMode m, bool force = false)
	{
		if (!force && m == mode_)
			return;
		const PanelMode was = mode_;
		mode_ = m;

		// The monitoring block: bays beside the tiles, or bays above them
		// in a column. setOrientation does not touch the children, which
		// is the whole reason this is a splitter and not two layouts.
		monitorSplit_->setOrientation(m == PanelMode::Tall ? Qt::Vertical
								  : Qt::Horizontal);
		bBox_->setVisible(g_haveB);

		// SHORT PUTS THE CONTROLS IN THE LEFT COLUMN. The panel is wide
		// and shallow, so the list goes down the right-hand half and
		// everything else — pictures, keys, the on-air band and the
		// position bar — stacks on the left. The strip carries no
		// picture, so moving it between the two layouts is free.
		const bool sideBySide = m == PanelMode::Short;
		if (sideBySide != controlsInColumn_) {
			controlsInColumn_ = sideBySide;
			if (sideBySide) {
				root_->removeWidget(controls_);
				leftColLayout_->addWidget(controls_);
			} else {
				leftColLayout_->removeWidget(controls_);
				controls_->setParent(this);
				root_->addWidget(controls_);
			}
			controls_->show();
		}
		bodySplit_->setOrientation(sideBySide ? Qt::Horizontal
						      : Qt::Vertical);

		strip_->setStacked(m == PanelMode::Wide ? 0 : 1);
		applyCompactChrome(m == PanelMode::Tall);

		// THE DIVIDERS THE OPERATOR CHOSE FOR *THIS* ARRANGEMENT, put
		// back. A restore has to happen after the orientations are set,
		// or the saved state is applied to a splitter that is still
		// dividing the other way round.
		if (was != m) {
			if (bodyChosen())
				bodySplit_->restoreState(savedBody_[modeIdx()]);
			if (monitorChosen())
				monitorSplit_->restoreState(
					savedMonitor_[modeIdx()]);
		}
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
	// The toolbar and the status line are rows of LABELLED controls, and a
	// row of labels has a minimum width that is the sum of its words. Every
	// one of these controls already carries its mark and its tooltip, so in
	// a column the word is the part that can go.
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
			wc.b->setMinimumWidth(compact ? 26 : wc.wide);
		}
		if (statusDetail_)
			statusDetail_->setVisible(!compact);
		if (search_)
			search_->setFixedWidth(compact ? 90 : 150);
		if (projectLbl_)
			projectLbl_->setVisible(!compact);
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		applyPanelMode(panelModeFor(size(), mode_, wideFloorH_));
		// THE FLOOR IS SAMPLED AFTER THE PASS, never during one. Asking a
		// widget for its minimumSizeHint ACTIVATES its layout, and doing
		// that anywhere inside the resize cascade does not merely read a
		// number - the pass it forces is the one that stays on screen.
		// Measured: the six speed presets came out 38x16 instead of 38x22.
		// One pass late is harmless; the floor only moves with the width,
		// and the hysteresis covers the tick it takes to catch up.
		QTimer::singleShot(0, this, [this]() {
			if (mode_ == PanelMode::Wide)
				wideFloorH_ = minimumSizeHint().height();
		});
		// AFTER the layout pass: a resizeEvent arrives before the children
		// are re-laid, so the splitter still reports its OLD height and a
		// split computed from it gets rescaled by the layout that follows.
		QTimer::singleShot(0, this, [this]() { applyPreviewAspect(); });
	}

private:
	static constexpr int kTiles = 8; // kMaxCameras
	// How much list is kept whatever the pictures ask for.
	static constexpr int kListFloor = 110;

public:
	QSplitter *bodySplit_ = nullptr;
	QWidget *leftCol_ = nullptr;
	int tileCols_ = 2;
	QSize tileSize() const { return tile_[0] ? tile_[0]->size() : QSize(); }
	QSize tileBlockSize() const { return tiles_ ? tiles_->size() : QSize(); }
	QSize monitorSize() const
	{
		return monitorSplit_ ? monitorSplit_->size() : QSize();
	}
	// The picture boxes, for the aspect check. Published rather than found by
	// class from outside: a QWidget with a black QLabel in it is not
	// identifiable, and guessing would make the check pass by finding nothing.
	// THE PICTURES THEMSELVES, not the boxes that hold them: the box takes
	// whatever cell the layout gives it and the picture is the 16:9 rectangle
	// centred inside. Asking the box would be asking the wrong widget and the
	// check would report a failure on every panel that is not exactly the
	// right shape — which is all of them.
	QVector<const QWidget *> pictureBoxes() const
	{
		QVector<const QWidget *> v{aBox_->picture(), bBox_->picture()};
		for (int i = 0; i < kTiles; i++)
			if (tile_[i]->isVisible())
				v << tile_[i]->picture();
		return v;
	}
	static int tagHeight() { return 0; }

private:
	QVBoxLayout *root_ = nullptr;
	QVBoxLayout *leftColLayout_ = nullptr;
	QSplitter *monitorSplit_ = nullptr;
	QWidget *bays_ = nullptr;
	QGridLayout *baysGrid_ = nullptr;
	PictureBox *aBox_ = nullptr, *bBox_ = nullptr;
	QWidget *tiles_ = nullptr;
	QGridLayout *tilesGrid_ = nullptr;
	PictureBox *tile_[kTiles] = {};
	QWidget *listPane_ = nullptr;
	QWidget *controls_ = nullptr;
	QTableWidget *table_ = nullptr;
	// ── WHERE THE OPERATOR PUT THE DIVIDERS, PER ARRANGEMENT ─────────────
	//
	// A drag is a decision and it has to survive; but it is a decision about
	// ONE arrangement. The split that is right for a wide floating window
	// means nothing in a narrow column where the same divider runs the other
	// way, so carrying it across would hand the operator back a layout he
	// never chose. One remembered state per mode: choose once in each shape,
	// and each shape keeps its answer.
	//
	// (In the panel these are written to the project's settings. Here they
	// live as long as the process, which is as long as anything else does.)
	QByteArray savedBody_[3], savedMonitor_[3];
	bool userSplit_[3] = {false, false, false};
	bool userMonitorSplit_[3] = {false, false, false};
	bool controlsInColumn_ = false;

	int modeIdx() const { return (int)mode_; }
	bool bodyChosen() const { return userSplit_[modeIdx()]; }
	bool monitorChosen() const { return userMonitorSplit_[modeIdx()]; }

	// The controls whose LABEL is optional — see applyCompactChrome. The text
	// and the laid-out width are kept here rather than read back off the
	// button, because a button that has been stripped once has nothing to
	// read back.
	struct Worded {
		QPushButton *b;
		QString text;
		int wide;
	};
	QVector<Worded> worded_;
	QWidget *statusDetail_ = nullptr;
	QLabel *search_ = nullptr;
	QLabel *projectLbl_ = nullptr;
	bool compactChrome_ = false;

	void remember(QPushButton *b)
	{
		worded_.push_back({b, b->text(), b->minimumWidth()});
	}

	// ── the monitoring block ─────────────────────────────────────────────
	//
	// A SPLITTER, so the operator can move the divider between the bays and
	// the cameras. In a wide panel that division is a WIDTH, and because both
	// sides keep their 16:9 the same drag changes their HEIGHT — which is the
	// only control the operator has ever had over how big the pictures are.
	void buildMonitors()
	{
		leftCol_ = new QWidget(this);
		leftColLayout_ = new QVBoxLayout(leftCol_);
		leftColLayout_->setContentsMargins(0, 0, 0, 0);
		leftColLayout_->setSpacing(3);

		bays_ = new QWidget(leftCol_);
		baysGrid_ = new QGridLayout(bays_);
		baysGrid_->setContentsMargins(0, 0, 0, 0);
		baysGrid_->setSpacing(3);
		aBox_ = pic(QStringLiteral("A"), "mrChanTag");
		bBox_ = pic(QStringLiteral("B"), "mrChanTag");
		// A AND B ARE ALWAYS SIDE BY SIDE, in every arrangement. They are
		// two bays of one deck: stacking them in a column would make the
		// pair read as a hierarchy, and it is the one relationship on this
		// panel that is exactly equal.
		baysGrid_->addWidget(aBox_, 0, 0);
		baysGrid_->addWidget(bBox_, 0, 1);
		baysGrid_->setColumnStretch(0, 1);
		// B'S HALF OF THE ROW GOES AWAY WITH B. A grid column keeps the
		// stretch it was given whether or not anything visible is in it,
		// so with one bay A was drawn in the LEFT HALF and centred there.
		baysGrid_->setColumnStretch(1, g_haveB ? 1 : 0);

		tiles_ = new QWidget(leftCol_);
		tilesGrid_ = new QGridLayout(tiles_);
		tilesGrid_->setContentsMargins(0, 0, 0, 0);
		tilesGrid_->setSpacing(kTileGap);
		for (int i = 0; i < kTiles; i++) {
			// NO FLOOR ON A TILE. A minimum height here becomes the
			// panel's, and the point of the redesign is that this
			// panel can be made short. What keeps them
			// picture-shaped is the CAP, which is a limit and not a
			// demand.
			tile_[i] = pic(QStringLiteral("C%1").arg(i + 1),
				       "mrTileCap");
			// THE PICTURE IS THE ANGLE KEY NOW. The camera matrix is
			// gone — sixteen keys of it — because clicking the box is
			// what the operator was already looking at: the key says
			// "C5", the picture says what C5 is pointing at. The
			// cursor is the only thing that advertises it, so it is
			// not optional.
			tile_[i]->setCursor(Qt::PointingHandCursor);
			tile_[i]->setToolTip(
				QStringLiteral("Guarda l'angolo %1").arg(i + 1));
			if (i >= g_cams)
				tile_[i]->hide();
		}
		// The tally, so the block shows what it is for: green is the angle
		// being watched, red the one on air.
		if (g_cams > 0)
			tileTally(0, "pvw");
		if (g_cams > 1)
			tileTally(1, "pgm");

		monitorSplit_ = new QSplitter(Qt::Horizontal, leftCol_);
		monitorSplit_->setChildrenCollapsible(false);
		monitorSplit_->setHandleWidth(5);
		monitorSplit_->addWidget(bays_);
		monitorSplit_->addWidget(tiles_);
		connect(monitorSplit_, &QSplitter::splitterMoved, this,
			[this](int, int) {
				userMonitorSplit_[modeIdx()] = true;
				savedMonitor_[modeIdx()] =
					monitorSplit_->saveState();
			});
		leftColLayout_->addWidget(monitorSplit_, 1);
	}

	void tileTally(int i, const char *what) { tile_[i]->setTally(what); }

	// ── the toolbar: what is GLOBAL to the panel ─────────────────────────
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

		auto *live = iconTextKey(Icon::Live, QStringLiteral("Live"),
					 QStringLiteral("live"), "mrLive", 12);
		live->setCheckable(true);
		live->setToolTip(QStringLiteral("Le marcature cadono sul fronte live"));
		remember(live);
		h->addWidget(live);
		// A STATE, NOT A SIGNAL. Monitors used to be drawn with the Live
		// key's role, so "the pictures are on" — the resting state of the
		// panel — lit up in the same red as REC. Red has one meaning here
		// and this is not it.
		auto *mon = iconTextKey(Icon::Monitors, QStringLiteral("Monitors"),
					QStringLiteral("monitors"), "mrToggle", 12);
		mon->setCheckable(true);
		mon->setChecked(true);
		mon->setToolTip(QStringLiteral("Mostra o nascondi le immagini"));
		remember(mon);
		h->addWidget(mon);
		// THE GEAR LIVES WITH THE OTHER PANEL-WIDE KEYS, not down in the
		// record section. What it opens is Settings for the whole panel;
		// beside REC it read as part of arming a take.
		auto *gear = iconKey(Icon::Gear, QStringLiteral("settings"),
				     QStringLiteral("Impostazioni"), "mrToggle");
		h->addWidget(gear);
		auto *full = iconKey(Icon::FullScreen, QStringLiteral("fullscreen"),
				     QStringLiteral("Schermo intero"), "mrToggle");
		full->setCheckable(true);
		h->addWidget(full);
		h->addStretch(1);
		return box;
	}

	// ── everything below the list ────────────────────────────────────────
	void buildControls()
	{
		controls_ = new QWidget(this);
		auto *v = new QVBoxLayout(controls_);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(3);
		controls_->setSizePolicy(QSizePolicy::Preferred,
					 QSizePolicy::Minimum);

		strip_ = new ControlStrip(controls_);
		// Two macro-rows. The first is what you do to the FOOTAGE, the
		// second is the take and the transport; the numbers are the order
		// when the strip folds into a stack on a narrow dock, which is
		// what an operator reaches for through a whole match.
		buildMark(Lane::Left, /*rank*/ 1);
		if (g_haveB)
			buildBaySelector(Lane::Centre, /*rank*/ 3);
		buildSpeed(Lane::Right, /*rank*/ 5);
		buildRec(Lane::Left, /*startsLine*/ true, /*rank*/ 0);
		buildPlayback(Lane::Centre, /*rank*/ 2);
		buildClips(Lane::Right, /*rank*/ 4);
		addStrip(v, strip_);

		// THE STATUS LINE SITS ABOVE THE GREEN BAND. The band says what is
		// on air; the line says what the next replay will run under. Below
		// it, the modes read as a footnote to a clip that is already
		// playing.
		v->addWidget(buildStatusBar());

		// ── the on-air band ──────────────────────────────────────────
		auto *clip = new QLabel(
			QStringLiteral("  0003 · C2 · 50%          −00:03.20   Σ 00:11"),
			controls_);
		clip->setStyleSheet(
			QString("background:%1;color:#fff;font-weight:700;font-size:10px;")
				.arg(sc_.onAir));
		clip->setFixedHeight(kClipBarH);
		// THE PLACEHOLDER MUST NOT SET THE PANEL'S FLOOR. In the panel
		// this band is a custom-painted widget that elides its own text;
		// here it is a QLabel, and a QLabel's minimum is the width of its
		// text — so words chosen for a mockup were deciding how narrow the
		// dock could be made.
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
		v->addWidget(clip);

		// ── the position bar ─────────────────────────────────────────
		auto *seekRow = new QWidget(controls_);
		auto *skh = new QHBoxLayout(seekRow);
		skh->setContentsMargins(0, 0, 0, 0);
		skh->setSpacing(4);
		auto *seek = new QLabel(seekRow);
		seek->setStyleSheet(QString("background:%1;border:1px solid %2;")
					    .arg(sc_.sink2, sc_.border));
		seek->setFixedHeight(kSeekH);
		auto *zoom = key(QStringLiteral("1×"), "mrChanSel");
		setKeyId(zoom, QStringLiteral("zoom"));
		zoom->setToolTip(QStringLiteral("Quanta timeline è in vista"));
		// THE SAME HEIGHT AS THE BAR IT BELONGS TO. At a key's height it
		// sat against the bar's top edge with a notch of panel under it,
		// which reads as a control that has come loose from the thing it
		// controls.
		zoom->setMinimumHeight(kSeekH);
		zoom->setMaximumHeight(kSeekH);
		zoom->setMinimumWidth(36);
		skh->addWidget(seek, 1);
		skh->addWidget(zoom, 0);
		v->addWidget(seekRow);
	}

	// ── the status line: it OWNS the modes ───────────────────────────────
	QWidget *buildStatusBar()
	{
		auto *box = new QWidget(controls_);
		box->setObjectName(QStringLiteral("mrStatusBar"));
		box->setFixedHeight(kStatusH);
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(6, 2, 6, 2);
		h->setSpacing(6);

		auto *health = key(QStringLiteral("1"), "mrHealth");
		health->setProperty("level", QStringLiteral("warn"));
		// DENSE, so the style sheet does not ask for a taller frame than
		// the widget owns — which put the badge's bottom border outside
		// it and read as a box nobody closed.
		health->setProperty("dense", true);
		health->setMinimumHeight(0);
		health->setFixedHeight(kStatusH - 6);
		setKeyIcon(health, Icon::Health, g_tints, 11);
		setKeyId(health, QStringLiteral("health"));
		h->addWidget(health);
		auto *notice = new QLabel(QStringLiteral("Lista 01 · evento 0003"),
					  box);
		notice->setObjectName(QStringLiteral("mrStatusText"));
		notice->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		statusDetail_ = notice;
		h->addWidget(notice, 1);

		// THE MODES SIT AGAINST THE RIGHT EDGE, not in the middle. A group
		// centred on a bar whose width changes with the dock is a group
		// that moves every time the panel is resized; against an edge the
		// hand finds it the same way twice.
		h->addWidget(statusSep(box));
		auto *loop = statusKey(Icon::Loop, QStringLiteral("LOOP"),
				       QStringLiteral("loop"),
				       QStringLiteral("Ripeti la clip"));
		h->addWidget(loop);
		auto *music = statusKey(Icon::Music, QStringLiteral("MUSICA"),
					QStringLiteral("music"),
					QStringLiteral("Musica sotto il replay"));
		h->addWidget(music);
		auto *out = statusKey(Icon::ToOutput, QStringLiteral("IN OUTPUT"),
				      QStringLiteral("toOutput"),
				      QStringLiteral("Il replay prende il Program"));
		out->setChecked(true);
		h->addWidget(out);
		h->addWidget(statusSep(box));

		auto *speed = new QLabel(QStringLiteral("1.00×"), box);
		speed->setObjectName(QStringLiteral("mrStatusValue"));
		h->addWidget(speed);
		return box;
	}

	QPushButton *statusKey(Icon ic, const QString &text, const QString &id,
			       const QString &tip)
	{
		auto *b = new QPushButton(text);
		b->setObjectName(QStringLiteral("mrStatKey"));
		b->setCheckable(true);
		b->setFixedHeight(kStatusH - 4);
		b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		setKeyIcon(b, ic, g_tints, 12);
		setKeyId(b, id);
		b->setToolTip(tip);
		b->ensurePolished();
		b->setMinimumWidth(b->sizeHint().width() + 4);
		remember(b);
		return b;
	}

	// ── MARK: three rows, because it answers three questions ─────────────
	//
	//   −5s −10s −20s    take the last N seconds whole   (first function)
	//   IN  OUT  ✕       take a point, close it, undo it (second)
	//   ⇤IN OUT⇥         move a point already taken      (third)
	//
	// It was one row of eight, and one row said those three were the same
	// kind of act. The order is how often an operator reaches for them
	// during a match, top first.
	void buildMark(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		auto *m5 = key(QStringLiteral("−5s"));
		setKeyId(m5, QStringLiteral("mark5"));
		auto *m10 = key(QStringLiteral("−10s"));
		setKeyId(m10, QStringLiteral("mark10"));
		auto *m20 = key(QStringLiteral("−20s"));
		setKeyId(m20, QStringLiteral("mark20"));
		// IN AND OUT STAY WORDS. The brief asks for icons wherever an
		// action is universally recognisable, and these are the
		// counter-example it names itself: every mark on a timeline is a
		// bracket of some kind, and a panel whose most-pressed keys are
		// two brackets is a panel you have to hover to use.
		auto *in = key(QStringLiteral("IN"));
		setKeyId(in, QStringLiteral("markIn"));
		auto *out = key(QStringLiteral("OUT"));
		setKeyId(out, QStringLiteral("markOut"));
		auto *cancel = iconKey(Icon::Cancel, QStringLiteral("markCancel"),
				       QStringLiteral("Annulla la marcatura"),
				       "mrDanger");
		auto *tin = iconKey(Icon::TrimIn, QStringLiteral("trimIn"),
				    QStringLiteral("Porta l'IN qui"));
		auto *tout = iconKey(Icon::TrimOut, QStringLiteral("trimOut"),
				     QStringLiteral("Porta l'OUT qui"));
		// SIX COLUMNS so the row of two divides as evenly as the rows of
		// three: on three columns the trim row left a hole in the corner,
		// and the eye finds that hole every time it reads the block.
		//
		// FOLDED IT IS TWO ROWS, and that is a deliberate trade rather
		// than a compromise. In a column every key row is charged to the
		// event list, and a third row here put the panel's floor at 668 px
		// — which is fine in an OBS side dock (they are as tall as the
		// screen) and impossible in a small floating window. The hierarchy
		// stands where there is room to draw it; where there is not, the
		// durations keep their own row and the points and the trims share
		// the next one.
		blk->setShapes({{Cell(m5, 2), Cell(m10, 2), Cell(m20, 2)},
				{Cell(in, 2), Cell(out, 2), Cell(cancel, 2)},
				{Cell(tin, 3), Cell(tout, 3)}},
			       {{Cell(m5), Cell(m10), Cell(m20), Cell(cancel)},
				{Cell(in), Cell(out), Cell(tin), Cell(tout)}});
		strip_->addBlock(blk, lane, false, rank);
	}

	// ── WHICH BAY the keys drive. That is all this section is now ────────
	//
	// It used to carry the camera matrix as well — two rows of eight keys,
	// the widest thing on the panel by a long way, and the reason a
	// side dock could not hold it. The angles are chosen by CLICKING THE
	// PICTURE, which is where the operator is looking anyway and which no
	// row of keys can do better: the key says "C5", the picture says what
	// C5 is pointing at.
	//
	// With one bay the section is not here at all — not disabled, absent.
	void buildBaySelector(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		QVector<Cell> row;
		// A↔B, not A|B: it says what the mode DOES (a command goes to
		// both bays) rather than naming two things with a bar between.
		for (const char *l : {"A↔B", "A", "B"}) {
			auto *b = key(QString::fromUtf8(l), "mrChanSel");
			b->setCheckable(true);
			b->setChecked(QString::fromUtf8(l) == QStringLiteral("A"));
			b->setMinimumWidth(38);
			setKeyId(b, QStringLiteral("bay%1").arg(QString::fromUtf8(l)));
			row << Cell(b);
		}
		auto *swap = iconKey(Icon::Swap, QStringLiteral("swapBays"),
				     QStringLiteral("Scambia A e B"), "mrChanSel");
		swap->setMinimumWidth(38);
		// The swap skips a column: ⇄ is not a fourth mode, and pressed by
		// mistake it puts the wrong clip on air.
		row << Cell(nullptr, 1) << Cell(swap);
		blk->setShapes({row}, {row});
		strip_->addBlock(blk, lane, false, rank);
	}

	// ── REC: the take, and every number about it ─────────────────────────
	//
	// One section, as it should always have been: arming the take, how long
	// it has been running, how much room is left, and the wall clock. They
	// were spread between here and the status line, so "how long have we
	// been recording" was answered in a different place from "are we
	// recording".
	//
	// REC SPANS BOTH ROWS. It is one of the three first-function keys on the
	// panel (REC, PLAY, NOW) and it is drawn bigger than the rest — the same
	// height as the section, so nothing beside it is left misaligned.
	void buildRec(Lane lane, bool startsLine, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		auto *rec = iconTextKey(Icon::Rec, QStringLiteral("REC"),
					QStringLiteral("rec"), "mrRec", 13);
		rec->setMaximumHeight(QWIDGETSIZE_MAX);
		rec->setMinimumWidth(78);

		// HOW LONG THE TAKE HAS BEEN RUNNING is the number the operator
		// looks for, so it is the big one and it is red while it runs.
		// The wall clock and the room left are the small print under it.
		auto *elapsed = new QLabel(QStringLiteral("09:52:20"), this);
		elapsed->setObjectName(QStringLiteral("mrStatusValue"));
		elapsed->setProperty("rec", true);
		auto *sub = new QLabel(QStringLiteral("17:22:06 · rim. 01:10:24"),
				       this);
		sub->setObjectName(QStringLiteral("mrMuted"));
		// FIXED WIDTH, and it is not cosmetic: these change four times a
		// second and their text changes LENGTH with it. A width change
		// re-flows the strip, which changes its height, which makes the
		// panel redistribute height — and what gives it up is the
		// pictures, whose resize re-allocates a swap chain on the
		// graphics thread. A label that cannot change width cannot start
		// that chain.
		elapsed->setFixedWidth(132);
		sub->setFixedWidth(132);

		const BlockShape shape{{Cell(rec, 1, true, 2), Cell(elapsed, 1, false)},
				       {Cell(nullptr, 1), Cell(sub, 1, false)}};
		blk->setShapes(shape, shape);
		strip_->addBlock(blk, lane, startsLine, rank);
	}

	// ── the transport, and the two keys that matter most in it ───────────
	//
	//   ▶⏸ ■ ◀ ↺ ⏮ ⏭ ▾   drive the clip that is showing
	//   [      NOW      ]  come back to the live edge
	//   [  PLAY  ]         put the selected events on air — two rows tall
	//
	// PLAY IS THE BIGGEST KEY ON THE PANEL because it is the one that takes
	// Program, and NOW is the widest because it is the way back. They were a
	// text button and a small key in a row of eight, the same weight as a
	// frame step, so the eye had to read the whole strip to find either.
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
		auto *more = iconKey(Icon::Menu, QStringLiteral("playOptions"),
				     QStringLiteral("Altre opzioni"), "mrGear");
		more->setMaximumWidth(22);

		auto *now = key(QStringLiteral("NOW"), "mrNow");
		setKeyId(now, QStringLiteral("now"));
		now->setToolTip(QStringLiteral("Torna al fronte live"));

		auto *play = iconKey(Icon::Play, QStringLiteral("playEvents"),
				     QStringLiteral("Riproduci gli eventi selezionati"),
				     "mrAccent");
		play->setMaximumHeight(QWIDGETSIZE_MAX);
		// WIDE AS WELL AS TALL. Two rows of height alone made it a green
		// stripe; the key that takes Program should be the one rectangle
		// the eye lands on without reading anything.
		play->setMinimumWidth(96);
		setKeyIcon(play, Icon::Play, g_tints, 22);

		const BlockShape wide{
			{Cell(pp), Cell(stop), Cell(rev), Cell(last), Cell(sb),
			 Cell(sf), Cell(more), Cell(play, 1, true, 2)},
			{Cell(now, 7)}};
		const BlockShape flat{
			{Cell(pp), Cell(stop), Cell(rev), Cell(last), Cell(sb),
			 Cell(sf), Cell(more)},
			{Cell(now, 4), Cell(play, 3)}};
		blk->setShapes(wide, flat);
		strip_->addBlock(blk, lane, false, rank);
	}

	// ── THE RUNNING ORDER, and the actions that have no key of their own ─
	//
	// ▲ ▼ move the selected event in its list's running order — which is the
	// order the sequence export writes and the order a queue plays, so it is
	// not a tidying-up gesture, it is the edit. They were lost in the first
	// pass of this redesign, which is exactly the kind of thing a mockup is
	// for.
	//
	// TWO KEYS RATHER THAN DRAG AND DROP: a drag inside a table whose cells
	// are all editable is one slip away from starting an edit instead, and
	// during a match that is the wrong thing to risk.
	//
	// They sit in the RIGHT lane of the second macro-row, under the speed
	// dial and the export key — which is where the rest of "what is done with
	// the clips once they are marked" already lives, and which fills the one
	// lane the arrangement had left empty.
	void buildClips(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		auto *up = iconKey(Icon::MoveUp, QStringLiteral("moveUp"),
				   QStringLiteral("Sposta l'evento su"));
		auto *dn = iconKey(Icon::MoveDown, QStringLiteral("moveDown"),
				   QStringLiteral("Sposta l'evento giù"));
		auto *more = iconKey(Icon::More, QStringLiteral("clipActions"),
				     QStringLiteral("Duplica · Elimina"), "mrGear");
		const BlockShape shape{{Cell(up), Cell(dn), Cell(more)}};
		blk->setShapes(shape, shape);
		strip_->addBlock(blk, lane, false, rank);
	}

	// ── VELOCITA, and what is done with the clips once they are marked ───
	//
	//   25 33 50 75 100 2×
	//   [ the dial ]
	//   [ Export ▾ ]
	//
	// The export keys were a section of their own in the far corner. There
	// were two of them — one clip, or the whole selection as one file — and
	// that is a QUESTION, not two keys: one key that asks it takes half the
	// room and stops the operator having to know the difference before he
	// has decided he wants to export at all.
	void buildSpeed(Lane lane, int rank)
	{
		auto *blk = new KeyBlock(QString(), this);
		QVector<Cell> row;
		QList<QPushButton *> chips;
		for (const char *l : {"25%", "33%", "50%", "75%", "100%", "2×"}) {
			auto *b = key(QString::fromUtf8(l), "mrSpeedChip");
			b->setChecked(QString::fromUtf8(l) == QStringLiteral("100%"));
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
		auto *exp = iconTextKey(Icon::ExportClip, QStringLiteral("Export"),
					QStringLiteral("export"));
		exp->setToolTip(QStringLiteral("Esporta la clip o l'intera sequenza"));
		remember(exp);

		const BlockShape shape{row,
				       {Cell(dial, 6)},
				       {Cell(exp, 6)}};
		blk->setShapes(shape, shape);
		strip_->addBlock(blk, lane, false, rank);
	}

	// The tiles, laid into the block chosen for the room they were given.
	void relayTiles(int cols)
	{
		cols = std::max(1, cols);
		if (cols == tileCols_ && tilesGrid_->count() > 0)
			return;
		while (QLayoutItem *it = tilesGrid_->takeAt(0))
			delete it;
		// NO ALIGNMENT FLAG. An aligned layout item is given its
		// sizeHint, not its cell — so a tile whose minimum is nothing and
		// whose content is a caption would draw itself caption-sized.
		// Unaligned, it fills the cell and its MAXIMUM is what holds it to
		// a picture-shaped rectangle.
		for (int i = 0; i < kTiles; i++)
			tilesGrid_->addWidget(tile_[i], i / cols, i % cols);
		// FROM THE TILES THAT ARE ON SCREEN, not from the eight that
		// exist. Counted from kTiles, a two-camera rig stretched four
		// rows to hold one row of pictures and each got a quarter of the
		// block: 164 px wide and 26 px tall, which is a strip of
		// letterboxing where two confidence monitors should be.
		const int rows = (std::max(1, g_cams) + cols - 1) / cols;
		// EVERY row and column, not just the ones in use now. A
		// QGridLayout remembers the stretch of a row it no longer has
		// items in, and rowCount() never comes back down — so a block
		// that was once eight rows deep kept four EMPTY stretching rows
		// after it became four, and they took half the block's height.
		for (int c = 0; c < std::max(cols, tilesGrid_->columnCount()); c++)
			tilesGrid_->setColumnStretch(c, c < cols ? 1 : 0);
		// The row PAST the last one stretches too: each tile is capped at
		// its own picture height, so the rows in use stop growing and a
		// block taller than the pictures keeps them at the top of it.
		// The bound is rows + 1, NOT rowCount(): setRowStretch on an index
		// past the end GROWS the grid, so a loop that walks to rowCount()
		// adds one row every time it runs - and this runs on every resize.
		for (int r = 0; r < std::max(rows + 1, tilesGrid_->rowCount()); r++)
			tilesGrid_->setRowStretch(
				r, r < rows ? 1
					    : (r == rows &&
					       mode_ != PanelMode::Tall)
						      ? 1
						      : 0);
		tileCols_ = cols;
	}


	// A 16:9 picture in a box of another shape is drawn letterboxed, and the
	// difference comes out as BLACK BARS. Every bar is a pixel the event list
	// asked for, so the monitoring pane is capped at the height its pictures
	// can actually fill and the splitter hands the rest to the list.
	static int aspectHeight(int w) { return std::max(1, w * 9 / 16); }

	// How many columns the camera block wears in the arrangement it is in.
	int tileColsFor(int paneW, int bays, int paneH) const
	{
		if (mode_ == PanelMode::Tall)
			return std::clamp(paneW / kTileMinWidth, 1,
					  std::max(1, g_cams));
		const int roomH = mode_ == PanelMode::Short ? std::max(40, paneH)
							    : 0;
		return tileBlockFor(paneW, bays, g_cams, 3, roomH).cols;
	}

	int controlsHeight() const
	{
		return controlsInColumn_ && controls_ ? controls_->height() + 3 : 0;
	}

	// ── THE MONITORING BLOCK, in three steps ─────────────────────────────
	//
	//  1. pick the camera block's shape for the room there is;
	//  2. unless the OPERATOR has moved a divider, put the dividers where
	//     the pictures want them;
	//  3. fit every box to the cell it actually got, at 16:9.
	//
	// Step 3 is last on purpose: it reads the geometry the splitters ended
	// up with rather than the geometry they were asked for, so a divider the
	// operator dragged is honoured by the pictures instead of being argued
	// with on the next tick.
	void applyPreviewAspect()
	{
		const int paneW = std::max(80, leftCol_->width());
		const int bays = g_haveB ? 2 : 1;
		const int gap = monitorSplit_->handleWidth();

		const int cols = tileColsFor(paneW, bays, leftCol_->height() -
							       controlsHeight());
		relayTiles(cols);
		const int rows = (std::max(1, g_cams) + cols - 1) / cols;

		// --- 1. what the two halves would like -----------------------
		int baysW, tilesW, want;
		int tileCap = kTileMaxWidth;
		if (mode_ == PanelMode::Tall) {
			// A COLUMN: the bays across the top, the cameras under
			// them. Both halves have the whole width; the divider
			// between them is a HEIGHT.
			baysW = tilesW = paneW;
			const int bayH = aspectHeight((paneW - 3 * (bays - 1)) /
						      bays) +
					 kTagH;
			// THE STRIP FILLS THE ROW. The ceiling stops ONE camera
			// drawing itself as big as the picture being watched; from
			// two upwards the row is already divided between them.
			const int share = (paneW - kTileGap * (cols - 1)) / cols;
			const int tileW =
				cols >= 2 ? share
					  : std::min(kTileMaxWidth, share);
			tileCap = tileW;
			const int stripH = rows * (aspectHeight(tileW) + kTagH) +
					   (rows - 1) * kTileGap;
			want = bayH + gap + stripH;
			if (!monitorChosen())
				monitorSplit_->setSizes({bayH, stripH});
		} else {
			const TileBlock tb = tileBlockFor(
				paneW, bays, g_cams, 3,
				mode_ == PanelMode::Short
					? std::max(40, leftCol_->height() -
								 controlsHeight())
					: 0);
			tilesW = tb.blockW;
			baysW = std::max(60, paneW - tilesW - gap);
			const int bayH =
				aspectHeight((baysW - 3 * (bays - 1)) / bays) +
				kTagH;
			want = std::max(bayH, tb.blockH);
			if (mode_ == PanelMode::Wide)
				// The pictures may not have more than half the
				// panel. Past that the list stops being a list.
				want = std::min(want, height() / 2);
			if (!monitorChosen())
				monitorSplit_->setSizes({baysW, tilesW});
		}

		// --- 2. the divider between the pictures and the list --------
		// In Short that divider is a WIDTH and `want` — a height — means
		// nothing to it.
		if (mode_ != PanelMode::Short) {
			leftCol_->setMaximumHeight(want);
			if (!bodyChosen()) {
				const int total = bodySplit_->height();
				const int give = std::min(want, total - kListFloor);
				const QList<int> now = bodySplit_->sizes();
				if (give > 0 &&
				    (now.isEmpty() || std::abs(now[0] - give) > 2))
					bodySplit_->setSizes({give, total - give});
			}
		} else {
			leftCol_->setMaximumHeight(QWIDGETSIZE_MAX);
		}

		// --- 3. the only size the boxes are told ---------------------
		// A TILE IS A CONFIDENCE MONITOR, so it has a ceiling: left to
		// fill the row, a single configured camera drew itself as big as
		// the picture being watched — the same angle twice, with the
		// event list paying for the second copy. Everything else about
		// their shape the boxes work out themselves (see PictureBox).
		// IN A COLUMN THERE IS NO CEILING ON THE HEIGHT: there the
		// strip's height is already exactly what the pictures need, and a
		// ceiling derived from the width this layout just produced, fed
		// back into it, spins.
		const int capH = mode_ == PanelMode::Tall
					 ? QWIDGETSIZE_MAX
					 : aspectHeight(kTileMaxWidth) + kTagH;
		for (int i = 0; i < kTiles; i++) {
			if (tile_[i]->maximumWidth() != tileCap)
				tile_[i]->setMaximumWidth(tileCap);
			if (tile_[i]->maximumHeight() != capH)
				tile_[i]->setMaximumHeight(capH);
		}
		(void)rows;
	}
};

} // namespace

// ---------------------------------------------------------------------------
// --check — the part of this that is a GATE rather than a look
// ---------------------------------------------------------------------------
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

// Everything visible has to be ON the panel.
void checkNothingClipped(Mock *w, const QString &label)
{
	const QRect panel(QPoint(0, 0), w->size());
	QString worst;
	int off = 0;
	for (QWidget *c : w->findChildren<QWidget *>()) {
		// isVisible(), NOT !isHidden(): a widget inside a hidden parent
		// reports isHidden() false — its own flag was never touched.
		if (!c->isVisible() || c->width() <= 0 || c->height() <= 0)
			continue;
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

// A key smaller than this is a key that cannot be hit under pressure.
void checkHitTargets(Mock *w, const QString &label)
{
	int small = 0;
	QString worst;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		if (!b->isVisible())
			continue;
		// The status line's own keys are shorter by design — they are
		// pressed while being looked at, not reached for blind.
		const int floor = b->objectName() == QStringLiteral("mrStatKey") ||
						  b->objectName() ==
							  QStringLiteral("mrHealth")
					  ? 15
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

// A KEY MUST BE WIDE ENOUGH FOR ITS OWN LABEL. Qt under-measures a
// stylesheet-styled button carrying both an icon and a word, and "Monitors"
// shipped as "Monitor:" — the kind of thing that is obvious in a screenshot and
// invisible in a size hint.
void checkLabelsFit(Mock *w, const QString &label)
{
	int cut = 0;
	QString worst;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		if (!b->isVisible() || b->text().isEmpty())
			continue;
		const int need = b->fontMetrics().horizontalAdvance(b->text()) +
				 (b->icon().isNull() ? 0 : b->iconSize().width() + 4);
		if (b->width() >= need + 8)
			continue;
		cut++;
		if (worst.isEmpty())
			worst = QString("'%1' has %2 needs %3 (min %4, hint %5)")
					.arg(b->text())
					.arg(b->width())
					.arg(need + 8)
					.arg(b->minimumWidth())
					.arg(b->sizeHint().width());
	}
	check(cut == 0, label + ": every label fits its key",
	      cut ? QString("%1 cut, first %2").arg(cut).arg(worst) : QString());
}

// A KEY WITH NO WORD ON IT MUST SAY WHAT IT IS SOMEHOW.
//
// This is the one rule an icon-first panel cannot be allowed to break, and it
// is the rule that is easiest to break by accident: a mark is obvious to
// whoever drew it and to nobody else. The brief asks for it in so many words,
// and a tooltip is the only place the answer can live once the label is gone.
void checkTooltips(Mock *w, const QString &label)
{
	int mute = 0;
	QString worst;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		if (b->objectName().startsWith(QStringLiteral("qt_")))
			continue;
		// A key with a word on it says what it is by saying it.
		if (!b->text().isEmpty() || !b->toolTip().isEmpty())
			continue;
		mute++;
		if (worst.isEmpty())
			worst = b->property(kKeyProperty).toString() + " (" +
				b->objectName() + ")";
	}
	check(mute == 0, label + ": every icon-only key has a tooltip",
	      mute ? QString("%1 mute, first %2").arg(mute).arg(worst)
		   : QString());
}

// THE PICTURES ARE 16:9, TO THE PIXEL.
//
// Not "roughly", and not "capped so it does not letterbox too badly": a box of
// any other shape draws the video letterboxed inside itself, and the operator
// cannot tell whether the black edge is the framing or the panel. It is also
// the thing that silently comes back — every arrangement change is a chance for
// one box to be given a cell of the wrong shape.
void checkAspect(Mock *w, const QString &label)
{
	int bad = 0;
	QString worst;
	for (const QWidget *box : w->pictureBoxes()) {
		if (!box->isVisible())
			continue;
		const int picH = box->height() - w->tagHeight();
		if (picH <= 0 || box->width() <= 0)
			continue;
		const double want = box->width() * 9.0 / 16.0;
		// One pixel of slack: the fit is integer arithmetic and a cell
		// an odd number of pixels tall cannot be halved exactly.
		if (std::abs(picH - want) <= 1.5)
			continue;
		bad++;
		if (worst.isEmpty())
			worst = QString("%1x%2 wants %3 tall")
					.arg(box->width())
					.arg(picH)
					.arg((int)want);
	}
	check(bad == 0, label + ": every picture is 16:9",
	      bad ? QString("%1 wrong, first %2").arg(bad).arg(worst) : QString());
}

// EVERY COMMAND KEY CARRIES ITS IDENTITY. The automated gate used to find
// twelve of this panel's keys by their literal text, which made a redrawing or
// a translation look exactly like a broken key.
void checkKeyIds(Mock *w)
{
	int missing = 0, ids = 0;
	QString worst;
	QStringList seen;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		// Qt builds buttons of its own inside its widgets (a table
		// view's corner button). They are not this panel's keys.
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
// so unlike every label on this panel they do not follow the style sheet.
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

// WHICH CHILD IS SETTING THE FLOOR, with its parents. One name was not enough:
// the widest child is usually a plain QWidget that some section put its keys
// in, and "QWidget (374)" is a number with nowhere to go.
QString widestMinimum(Mock *w)
{
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
		// where it means something: in the COLUMN arrangement. Asking it
		// of the wide one would be asking the wrong question — a panel
		// 1500 px wide is wearing the shape for 1500 px, and dragging it
		// narrow changes the shape first.
		if (t.mode == PanelMode::Tall) {
			const bool fits = w->minimumSizeHint().width() <= 340;
			check(fits, label + ": fits a side dock", widestMinimum(w));
			if (!fits)
				std::printf("      sections: %s\n",
					    qUtf8Printable(
						    w->strip_->describeBlocks()));
		}
		// THE LIST IS THE ELASTIC ZONE. Whatever else the arrangement
		// does, the table has to come out usable — the panel exists to
		// pick events off it.
		check(w->bodySplit_->sizes().value(1) >= 90,
		      label + ": the event list has room",
		      QString("%1 px").arg(w->bodySplit_->sizes().value(1)));
		checkNothingClipped(w, label);
		checkHitTargets(w, label);
		checkLabelsFit(w, label);
		checkTooltips(w, label);
		checkAspect(w, label);
		if (label == QStringLiteral("wide"))
			checkKeyIds(w);
		w->hide();
		delete w;
	}

	// HYSTERESIS, both directions. Without it a dock edge dragged across the
	// boundary flips the arrangement back and forth, and every flip re-lays
	// the panel's displays — a swap chain re-allocated on the graphics
	// thread, several times a second, while a take is recording.
	const QSize edge(kTallMaxWidth + 10, 900);
	check(panelModeFor(edge, PanelMode::Tall) == PanelMode::Tall,
	      "hysteresis: a column does not flip out early");
	check(panelModeFor(edge, PanelMode::Wide) == PanelMode::Wide,
	      "hysteresis: a wide panel stays wide");
	check(panelModeFor(QSize(kTallMaxWidth + kModeHysteresis + 10, 900),
			   PanelMode::Tall) == PanelMode::Wide,
	      "hysteresis: past the far edge it does flip");

	// The scheme, on a LIGHT palette — the case that never existed before the
	// panel could follow a theme, and the one where a scheme built for a
	// near-black panel disappears.
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
	// actually gets: a maximised window on a second monitor, a dock under
	// the OBS preview, a dock down one side. The rest are the corners that
	// used to break.
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
		std::printf("   tiles %dx%d block %dx%d, monitors %dx%d\n",
			    w->tileSize().width(), w->tileSize().height(),
			    w->tileBlockSize().width(), w->tileBlockSize().height(),
			    w->monitorSize().width(), w->monitorSize().height());
		std::printf("%-16s asked %4dx%4d  got %4dx%4d  min %4dx%4d  %-5s strip=%s split %d/%d\n",
			    s.name, s.w, s.h, w->width(), w->height(),
			    w->minimumSizeHint().width(),
			    w->minimumSizeHint().height(),
			    panelModeName(w->mode_),
			    w->strip_->isFlat() ? "stack" : "lanes",
			    w->bodySplit_->sizes().value(0),
			    w->bodySplit_->sizes().value(1));
	}
	return 0;
}
