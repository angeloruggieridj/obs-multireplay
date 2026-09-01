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
#include "../../src/dock-assets.hpp"
#include "../../src/dock-icons.hpp"
#include "../../src/dock-layout.hpp"
#include "../../src/dock-style.hpp"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFontInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOption>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>

using namespace multireplay;

namespace {

// The status line. Shorter than a key row because nothing in it is a target the
// hand goes to blind — the modes are pressed while being looked at.
constexpr int kStatusH = 26;
// The on-air band, and the position bar. The band was 28 and the bar 52 with a
// caption over each; the captions are gone (the band is green and the bar is
// graduated — neither has ever needed a heading to be told apart) and the bar's
// ruler is tighter.
constexpr int kClipBarH = 28;
constexpr int kSeekH = 42;
// Narrower than this a tile stops being a picture and becomes a smear; wider
// than this it stops being a confidence monitor and starts competing with the
// bay it is meant to be checked against.


// The naming band under a picture. AspectBox owns the number; this is the same
// one, named locally so the block arithmetic below reads.
constexpr int kTagH = AspectBox::kTagH;

// The RIG the mockup pretends to be driving. Defaults to the one that is
// hardest on the layout and happens to be the operator's own: both bays and
// eight cameras.
int g_cams = 8;
bool g_haveB = true;
// Sizes named on the command line with --size=WxH. Empty means the built-in set.
QVector<QPair<int, int>> g_sizes;
ThemeChoice g_theme = ThemeChoice::Broadcast;
QPalette g_pal;
Scheme g_sc;
IconTints g_tints;
// The marks a sub-control can only be handed as a file, written into a temp
// directory. The dock writes them into its own config directory; both draw them
// from the same scheme with the same code (src/dock-assets.hpp).
SheetAssetPaths g_assets;

void refreshSheetAssets()
{
	g_assets = writeSheetAssets(
		QDir::temp().filePath(QStringLiteral("mr-mock-assets")), g_sc);
}

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
// THE THREE KEYS THAT OPEN A MENU ARE QToolButtons WITH A MENU, which is what
// the panel builds and what this tool used to fake with a plain key.
//
// Both halves of that matter, and both were measured here. Without a menu Qt
// never draws a menu arrow at all, so the mockup could not see the SECOND mark
// that was landing on the gear, the ▾ beside the play key and the ⋯ over the
// clip list. And the CLASS matters twice over: a QPushButton with a menu
// reserves 24 px for the arrow whatever the style sheet says (measured) while a
// QToolButton reserves none, and the two draw the arrow through different code
// — so a stand-in of the wrong class is measuring a key the panel does not have.
//
// The menu is popped on `clicked` rather than given to setMenu, exactly as the
// panel does it, because no style-sheet rule reaches that arrow. See the note
// in dock-style.hpp for the probe that established it.
QToolButton *menuKey(Icon ic, const QString &id, const QString &tip,
		     const char *role = "mrGear", int px = 14)
{
	auto *b = new QToolButton();
	b->setObjectName(QString::fromLatin1(role));
	setKeyIcon(b, ic, g_tints, px);
	setKeyId(b, id);
	b->setToolTip(tip);
	b->setMinimumHeight(kKeyH);
	b->setMaximumHeight(kKeyH);
	b->setPopupMode(QToolButton::InstantPopup);
	auto *m = new QMenu(b);
	m->addAction(QStringLiteral("Una voce"));
	m->addAction(QStringLiteral("Un'altra"));
	// A SEPARATOR AND A SUBMENU, because the gear's menu has both and each
	// is a sub-control OBS has an opinion about — the separator's colour and
	// the submenu's arrow, which OBS draws from a file picked for a dark
	// theme. A menu without them measures two thirds of a menu.
	m->addSeparator();
	QMenu *sub = m->addMenu(QStringLiteral("Un sottomenu"));
	sub->addAction(QStringLiteral("Dentro"));
	m->addSeparator();
	QAction *off = m->addAction(QStringLiteral("Non disponibile"));
	off->setEnabled(false);
	QObject::connect(b, &QToolButton::clicked, b, [b, m]() {
		m->popup(b->mapToGlobal(QPoint(0, b->height())));
	});
	return b;
}

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

// (The camera block's arithmetic - how many columns and how big a tile - used
// to be duplicated here. It lives in src/dock-layout now, so this tool and the
// panel cannot disagree about it: they did, and that is how two rounds of "the
// cameras are still postage stamps" were answered by looking at this window and
// declaring it fixed.)

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
	// Q_OBJECT so this is the same KIND of thing the dock is — a moc'd
	// QWidget subclass. It is not what made the mockup blind to the light
	// panel's dark band; that was measured and it was the containment (see
	// runHostChecks). Being honest about the class is worth a macro anyway.
	Q_OBJECT

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
		// THE SAME LINE THE DOCK NOW CARRIES, and for the same reason:
		// Qt honours a style sheet background only on a widget with this
		// attribute, and it does not set it for a subclass. Without it a
		// CHILD panel paints nothing and shows whatever OBS painted.
		setAttribute(Qt::WA_StyledBackground, true);
		sc_ = g_sc;
		setStyleSheet(dockStyle(sc_, 0, g_assets, rowFontPx()));
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
		table_->setAlternatingRowColors(true);
		// WITH ROWS IN IT, and a TICKED angle among them. An empty table
		// is the largest area of this panel rendering nothing, and the
		// tick in an angle cell is drawn from the same file the settings
		// dialog's check boxes use (dock-assets.hpp) — so an empty table
		// is one more thing this tool could not be asked about.
		table_->setHorizontalHeaderLabels(
			{QStringLiteral("#"), QStringLiteral("In"),
			 QStringLiteral("Out"), QStringLiteral("Durata"),
			 QStringLiteral("Commento"), QStringLiteral("1 C1")});
		for (int r = 0; r < table_->rowCount(); r++) {
			const QString id = QStringLiteral("%1").arg(r + 1, 4,
								   10,
								   QLatin1Char('0'));
			table_->setItem(r, 0, new QTableWidgetItem(id));
			table_->setItem(r, 1,
					new QTableWidgetItem(QStringLiteral(
						"00:%1.733").arg(11 + r * 7, 2,
								 10,
								 QLatin1Char('0'))));
			table_->setItem(r, 2,
					new QTableWidgetItem(QStringLiteral(
						"00:%1.733").arg(21 + r * 7, 2,
								 10,
								 QLatin1Char('0'))));
			table_->setItem(r, 3,
					new QTableWidgetItem(
						QStringLiteral("00:10.000")));
			// THE TWO CELLS THAT ARE WIDGETS, built the way the panel
			// builds them — a comment cell with a flat line edit and
			// a chooser beside it, and an angle cell with a tick and
			// the speed as a label. They are the reason the row's type
			// size can drift: the four columns above are items drawn
			// by the table and these are widgets drawn by the sheet.
			{
				auto *nc = new QWidget;
				nc->setObjectName(QStringLiteral("mrNoteCell"));
				auto *nh = new QHBoxLayout(nc);
				nh->setContentsMargins(2, 0, 2, 0);
				nh->setSpacing(2);
				auto *note = new QLineEdit(
					r % 2 ? QStringLiteral("Gol")
					      : QStringLiteral("Esultanza"),
					nc);
				note->setObjectName(QStringLiteral("mrAngleNote"));
				note->setFrame(false);
				nh->addWidget(note, 1);
				auto *pick = new QPushButton(nc);
				pick->setObjectName(QStringLiteral("mrNotePick"));
				setKeyIcon(pick, Icon::More, g_tints, 10);
				pick->setFixedWidth(16);
				nh->addWidget(pick);
				table_->setCellWidget(r, 4, nc);
			}
			{
				auto *cell = new QWidget;
				auto *ch = new QHBoxLayout(cell);
				ch->setContentsMargins(2, 0, 2, 0);
				ch->setSpacing(2);
				ch->addStretch(1);
				auto *box = new QCheckBox(cell);
				box->setChecked(r % 2 == 0);
				ch->addWidget(box);
				auto *sp = new QPushButton(cell);
				sp->setObjectName(QStringLiteral("mrAngleSpeed"));
				sp->setText(r % 3 ? QStringLiteral("--")
						  : QStringLiteral("50%"));
				sp->setProperty("mrNoOverride", r % 3 != 0);
				sp->setFixedWidth(44);
				ch->addWidget(sp);
				ch->addStretch(1);
				table_->setCellWidget(r, 5, cell);
			}
		}
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

	// ── CHANGING THE THEME WHILE THE PANEL IS UP ─────────────────────────
	//
	// The same sequence MultiReplayDock::applyTheme() runs, in the same
	// order, and this tool did not have it — every picture it has ever taken
	// was of a panel whose colours were set ONCE, in the constructor, before
	// a single child existed. Three faults were reported from the other path:
	// the marks a sub-control is handed as a file were missing until the
	// operator switched theme, a menu came out white on white, and the green
	// play key stopped being two rows tall. None of them can happen on the
	// path this tool was exercising, which is why none of them was ever seen
	// here.
	void retheme(ThemeChoice choice, const QPalette &pal)
	{
		g_theme = choice;
		g_sc = schemeFor(choice, pal);
		g_tints = tintsFor(g_sc);
		refreshSheetAssets();
		sc_ = g_sc;
		setStyleSheet(dockStyle(sc_, 0, g_assets, rowFontPx()));
		restyleIcons(this, g_tints);
		// The heights the sections pinned, which applying a sheet drops:
		// see kPinnedHeightProperty. Measured here first — the green play
		// key went 56 px to 46 px on the second line above.
		repinKeys(this);
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
		// (Only the body divider: the monitor one is derived, never chosen.)
		if (was != m && bodyChosen())
			bodySplit_->restoreState(savedBody_[modeIdx()]);
		// The tile block is laid out from applyPreviewAspect, which is the
		// only place that knows how wide the pane really is. Doing it here
		// computed the column count from a pane that had not been given a
		// width yet — one column, eight rows, and a monitoring block
		// taller than the panel.
		tileCols_ = 0;
		// AFTER the controls have been moved, never before: in Short they
		// live in leftCol_, and this decides whether leftCol_ may be
		// hidden at all.
		applyMonitorsRoom();
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
		// With the monitors down the left column has to be told again:
		// applyPanelMode early-outs when the arrangement has not changed,
		// and a QSplitter rescales its children proportionally on a resize.
		QTimer::singleShot(0, this, [this]() {
			applyMonitorsRoom();
			applyPreviewAspect();
		});
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
	// The event list, so a check can read the size its rows are drawn at.
	QTableWidget *eventTable() const { return table_; }
	QSize tileBlockSize() const { return tiles_ ? tiles_->size() : QSize(); }
	QSize monitorSize() const
	{
		return monitorSplit_ ? monitorSplit_->size() : QSize();
	}
	// The pane the whole monitoring row lives in, so a check can ask how much
	// of it the pictures actually cover.
	const QWidget *monitorPane() const { return monitorSplit_; }

	// The size the event table really draws its items in — the same question
	// the panel asks, and the same answer, so the two cells that are widgets
	// can be given it. QFontInfo because a point size resolves to -1 pixels.
	int rowFontPx() const
	{
		const QFont f = table_ ? table_->font() : qApp->font();
		return std::max(8, QFontInfo(f).pixelSize());
	}

	// THE OPERATOR DRAGS THE DIVIDERS, and until now nothing in this tool ever
	// did — every measurement it has ever taken was of a panel whose dividers
	// were still where the arithmetic had put them. That is not the panel an
	// operator has after five minutes, and it is not the one the report came
	// from.
	void simulateMonitorDrag(int bayShare)
	{
		if (!monitorSplit_)
			return;
		const int total = monitorSplit_->width();
		monitorSplit_->setSizes({bayShare, qMax(40, total - bayShare)});
		userMonitorSplit_[modeIdx()] = true;
		savedMonitor_[modeIdx()] = monitorSplit_->saveState();
		applyPreviewAspect();
	}

	// ...and the OTHER divider, the one between the pictures and the list.
	// Dragging it is how the cameras are made bigger, and it is the state the
	// reported panel was in.
	void simulateBodyDrag(int picturesH)
	{
		if (!bodySplit_)
			return;
		const int total = bodySplit_->height();
		bodySplit_->setSizes({picturesH, qMax(40, total - picturesH)});
		userSplit_[modeIdx()] = true;
		savedBody_[modeIdx()] = bodySplit_->saveState();
		applyPreviewAspect();
	}

	// The divider inside the monitoring row is the operator's to drag, so a
	// check can assert it is reachable rather than trust the line that builds
	// it: it was taken away once, and taking it away was the wrong answer.
	bool monitorHandleUsable() const
	{
		if (!monitorSplit_)
			return false;
		for (int i = 1; i < monitorSplit_->count(); i++)
			if (QSplitterHandle *h = monitorSplit_->handle(i))
				if (h->isEnabled())
					return true;
		return false;
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

	// The camera block and the pictures in it, on their own: A filling the row
	// while the cameras beside it do not is exactly what was reported, and a
	// measure that unions all five pictures cannot see it.
	const QWidget *tilePane() const { return tiles_; }
	// THE BOXES, not the pictures inside them: a tile is its picture AND the
	// band that names it, and the band is 12 px of every tile. Measured on
	// the pictures, every arrangement looks 13 px short on the height for a
	// reason that is not a fault.
	QVector<const QWidget *> tileBoxes() const
	{
		QVector<const QWidget *> v;
		for (int i = 0; i < kTiles; i++)
			if (tile_[i]->isVisible())
				v << tile_[i];
		return v;
	}

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
	bool userMonitorSplit_[3] = {false, false, false};
	bool monitorChosen() const { return userMonitorSplit_[modeIdx()]; }
	bool userSplit_[3] = {false, false, false};
	bool controlsInColumn_ = false;
	// Whether the pictures are up. The Monitors key writes it.
	bool monitorsOn_ = true;

	int modeIdx() const { return (int)mode_; }
	bool bodyChosen() const { return userSplit_[modeIdx()]; }
	// ONLY WHILE THE PANE IS THE ONE IT WAS DRAGGED IN. The row's widths are
	// a function of its height (tileBlockFor solves for the height that makes
	// the row fill the pane), so a remembered divider is a width that answers
	// a question about one pane size and no other. Measured here: after a drag
	// and a resize to 1920x1040, 220 px of the row was not a picture.
	// Same rule as the panel — see MultiReplayDock::monitorSplitChosen.

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
		// DRAGGABLE, like the panel: how much of the row goes to the bays
		// and how much to the cameras is a real thing to want. What makes
		// a drag FILL rather than band is honouring it on both sides -
		// see applyPreviewAspect.
		connect(monitorSplit_, &QSplitter::splitterMoved, this,
			[this](int, int) {
				userMonitorSplit_[modeIdx()] = true;
				savedMonitor_[modeIdx()] =
					monitorSplit_->saveState();
				applyPreviewAspect();
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
		// White once lit: the key goes solid red and a green dot inside it
		// would be two signals arguing in one control.
		setKeyIconRole(live, Icon::Live, IconRole::LitWhite, g_tints, 12);
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
		mon->setToolTip(QStringLiteral(
			"Mostra o nasconde baie e anteprime camera"));
		// WIRED, and it was not. A key drawn and connected to nothing is a
		// key this tool cannot judge — which is why the fault it hides
		// (the pictures go away, the room does not) was only ever found in
		// the real panel.
		connect(mon, &QAbstractButton::toggled, this,
			[this](bool on) { setMonitorsVisible(on); });
		remember(mon);
		h->addWidget(mon);
		// THE GEAR LIVES WITH THE OTHER PANEL-WIDE KEYS, not down in the
		// record section. What it opens is Settings for the whole panel;
		// beside REC it read as part of arming a take.
		auto *gear = menuKey(Icon::Gear, QStringLiteral("settings"),
				     QStringLiteral("Impostazioni"), "mrGear", 15);
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
		// It lives ON the green band, always: its mark is the other half
		// of a label the sheet already writes in white.
		setKeyIconRole(skip, Icon::SkipNext, IconRole::OnSignal, g_tints);
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
		setKeyIconRole(health, Icon::Health, IconRole::Warn, g_tints, 11);
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
		auto *mute = statusKey(Icon::Mute, QStringLiteral("MUTO"),
				       QStringLiteral("muteAudio"),
				       QStringLiteral("Replay mutato nel mixer"));
		h->addWidget(mute);
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
		// #mrDanger colours a LABEL and this key has none — only the ✕ —
		// so without a role the one destructive key on the row was drawn
		// exactly as neutral as the two it undoes.
		setKeyIconRole(cancel, Icon::Cancel, IconRole::Danger, g_tints);
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
		// The dot is red, like the word beside it. At rest the key is
		// chrome with a red label; armed it is filled red and the mark
		// turns white with it (see poll() in the dock).
		setKeyIconRole(rec, Icon::Rec, IconRole::Rec, g_tints, 13);
		// The property the sheet keys its two states off. Unset, NEITHER
		// #mrRec[recording="false"] nor ["true"] matched and the key fell
		// back to the ordinary key colour — so the mockup was drawing a
		// REC key in grey and calling it drawn.
		rec->setProperty("recording", false);
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
		auto *more = menuKey(Icon::Menu, QStringLiteral("playOptions"),
				     QStringLiteral("Altre opzioni"));
		// NO WIDTH CAP. The panel's ▾ has none — its section sizes it,
		// which comes to the mark plus the key's own padding. Capped at
		// 22 here it had SIX pixels of content for a 14 px chevron, so
		// this tool was rendering a smudge on a key the panel draws
		// properly. A stand-in narrower than the real key measures a
		// mark that was never clipped.

		auto *now = key(QStringLiteral("NOW"), "mrNow");
		setKeyId(now, QStringLiteral("now"));
		now->setToolTip(QStringLiteral("Torna al fronte live"));

		auto *play = iconKey(Icon::Play, QStringLiteral("playEvents"),
				     QStringLiteral("Riproduci gli eventi selezionati"),
				     "mrAccent");
		// The one filled key on the panel: white mark on solid green,
		// which is what the sheet already says about its label.
		setKeyIconRole(play, Icon::Play, IconRole::OnSignal, g_tints, 22);
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
		auto *more = menuKey(Icon::More, QStringLiteral("clipActions"),
				     QStringLiteral("Duplica · Elimina"));
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
		// NO SPARE ROW: one here took a THIRD of the block, because with
		// every row at stretch 1 and the ceiling never reached the grid
		// simply divided the height three ways. The tiles own ceiling is
		// what keeps them at the top of a block taller than they are.
		// The bound is rows + 1, NOT rowCount(): setRowStretch on an index
		// past the end GROWS the grid, so a loop that walks to rowCount()
		// adds one row every time it runs - and this runs on every resize.
		for (int r = 0; r < std::max(rows + 1, tilesGrid_->rowCount()); r++)
			// NO SPARE ROW: the block is now exactly as tall as the bays.
			tilesGrid_->setRowStretch(r, r < rows ? 1 : 0);
		tileCols_ = cols;
	}


	// A 16:9 picture in a box of another shape is drawn letterboxed, and the
	// difference comes out as BLACK BARS. Every bar is a pixel the event list
	// asked for, so the monitoring pane is capped at the height its pictures
	// can actually fill and the splitter hands the rest to the list.
	static int aspectHeight(int w) { return std::max(1, w * 9 / 16); }

	// How many columns the camera block wears — the SAME rule in every
	// arrangement now (ceil(n/2) past three), so the grid down a side matches
	// the grid in the Wide layout. It used to be a one-row filmstrip in Tall.
	int tileColsFor(int paneW, int bays) const
	{
		return std::max(1, tileBlockFor(paneW, bays, g_cams, 3, roomH()).cols);
	}

	// HOW TALL THE MONITORING BLOCK MAY BE, and it must not be read off the
	// block itself: that height is DERIVED from the arrangement this number
	// chooses, so feeding it back in makes the first pass decide from whatever
	// the widget happened to be mid-settle - measured, 100 px, which picked an
	// arrangement of 78 px stamps. What the splitter is willing to give depends
	// only on the panel and a constant, so it is the same on every pass.
	// ONE COPY, shared with the panel (dock-layout). It was two, and the two
	// disagreed about the divider the operator had dragged - which is how the
	// panel came to draw 237 px cameras where this drew 357 px ones.
	int roomH() const
	{
		return monitorRoomFor({height(),
				       bodySplit_ ? bodySplit_->height() : height(),
				       leftCol_ ? leftCol_->height() : 0,
				       controlsHeight(),
				       kListFloor,
				       bodyChosen(),
				       mode_ == PanelMode::Wide});
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
	// ── THE MONITORS KEY, AND WHERE THE ROOM GOES ────────────────────────
	//
	// This tool was blind to the whole thing until now: the key was drawn and
	// wired to nothing, so "the pictures go away" was never even exercised,
	// let alone "the list gets the room". In the panel it did the first and
	// not the second — bodySplit_'s child is leftCol_, not the picture row, so
	// hiding the row left an empty visible child holding exactly the height
	// the key was pressed to reclaim. Same shape here, same rule.
	//
	// Two cases, because leftCol_ holds two different things: in Wide and Tall
	// the pictures are all it has, so it goes with them; in Short the key
	// strip is down here too, so the column stays and the splitter is asked
	// for zero — which a QSplitter reads as "as little as this child accepts".
	void applyMonitorsRoom()
	{
		if (!bodySplit_ || !leftCol_)
			return;
		if (monitorsOn_) {
			leftCol_->setVisible(true);
			return;
		}
		leftCol_->setMaximumHeight(QWIDGETSIZE_MAX);
		if (!controlsInColumn_) {
			leftCol_->setVisible(false);
			return;
		}
		leftCol_->setVisible(true);
		if (bodySplit_->width() > 0)
			bodySplit_->setSizes({0, bodySplit_->width()});
	}

	void setMonitorsVisible(bool on)
	{
		monitorsOn_ = on;
		if (monitorSplit_)
			monitorSplit_->setVisible(on);
		applyMonitorsRoom();
		if (on) {
			if (bodyChosen() && !savedBody_[modeIdx()].isEmpty())
				bodySplit_->restoreState(savedBody_[modeIdx()]);
			applyPreviewAspect();
		}
	}

	void applyPreviewAspect()
	{
		// Nothing is sized while the monitors are down: every number below
		// is a maximum written onto a box, and one written for a block
		// nobody can see is still there when it comes back.
		if (!monitorsOn_)
			return;
		const int paneW = std::max(80, leftCol_->width());
		const int bays = g_haveB ? 2 : 1;
		const int gap = monitorSplit_->handleWidth();

		const int cols = tileColsFor(paneW, bays);
		relayTiles(cols);
		const int rows = (std::max(1, g_cams) + cols - 1) / cols;

		// --- 1. what the two halves would like -----------------------
		int baysW, tilesW, want;
		int tileCap = kTileMinWidth;
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
					  : std::min((int)(paneW * kTileMaxShare),
						     share);
			tileCap = tileW;
			const int stripH = rows * (aspectHeight(tileW) + kTagH) +
					   (rows - 1) * kTileGap;
			want = bayH + gap + stripH;
			monitorSplit_->setSizes({bayH, stripH});
		} else {
			// THE HEIGHT THE BLOCK WILL ACTUALLY HAVE, in every wide
			// arrangement and not only the short one. Left at 0 the
			// calculation believes the bays can be as tall as their
			// width allows - 840 px on a maximised panel - and picks
			// the arrangement for a block that will really be 517: A
			// comes out height-bound and 600 px narrower than the
			// space it was given, which is the black band beside it.
			const TileBlock tb =
				tileBlockFor(paneW, bays, g_cams, 3, roomH());
			tilesW = tb.blockW;
			tileCap = tb.tileW;
			// SIZED FROM THE PICTURES: tileBlockFor settled one height
			// for the whole row, so a bay is exactly as wide as that
			// height allows and the cameras take the rest.
			baysW = std::max(60, bays * tb.bayW + 3 * (bays - 1));
			if (baysW + tilesW + gap > paneW)
				baysW = std::max(60, paneW - tilesW - gap);
			int blockH = tb.blockH;
			// THE OPERATOR'S DIVIDER, HONOURED ON BOTH SIDES — the
			// same as the panel, and this is what this tool did not
			// have: it skipped setSizes after a drag but went on
			// using the DERIVED widths for the pictures, so the
			// cameras stayed the size the algebra wanted inside a
			// pane the operator had made bigger. That is the 162 px
			// band this check used to measure.
			const QList<int> have = monitorSplit_->sizes();
			if (monitorChosen() && have.size() > 1 && have[0] > 0 &&
			    have[1] > 0) {
				baysW = std::max(60, have[0]);
				tilesW = std::max(kTileMinWidth, have[1]);
				tileCap = std::max(kTileMinWidth,
						   (tilesW - (cols - 1) * kTileGap) /
							   cols);
				blockH = rows * (aspectHeight(tileCap) + kTagH) +
					 (rows - 1) * kTileGap;
			}
			const int bayH =
				aspectHeight((baysW - 3 * (bays - 1)) / bays) +
				kTagH;
			// roomH() is the ONE authority on how tall this block may
			// be, and the tile arithmetic above was given the same
			// number.
			want = std::min(std::max(bayH, blockH), roomH());
			if (!monitorChosen()) {
				const int slack =
					std::max(0, paneW - baysW - tilesW - gap);
				monitorSplit_->setSizes({baysW + slack / 2,
							 tilesW + slack - slack / 2});
			}
		}

		// --- 2. the divider between the pictures and the list --------
		// In Short that divider is a WIDTH and `want` — a height — means
		// nothing to it.
		if (mode_ != PanelMode::Short) {
			// NO CAP ONCE HE HAS DRAGGED IT, and that is what keeps
			// the handle two-way: from that moment the room the row
			// may have IS this pane's height, so a cap computed from
			// the pane and written back onto it can only ratchet the
			// pane down — dragged shorter and never taller again.
			// Same rule as the panel (applyPreviewSplit).
			leftCol_->setMaximumHeight(bodyChosen() ? QWIDGETSIZE_MAX
								: want);
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
					 : aspectHeight(tileCap) + kTagH;
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

// Returns what it was told, so a check that is a PRECONDITION for the next one
// can be written as one: measuring the mark on a key that is not on the panel
// produces a number, and the number is meaningless.
bool check(bool ok, const QString &what, const QString &detail = QString())
{
	if (!ok)
		g_fail++;
	std::printf("%-52s %s%s%s\n", qUtf8Printable(what), ok ? "OK" : "FAIL",
		    detail.isEmpty() ? "" : "  ", qUtf8Printable(detail));
	return ok;
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
// ── A CELL OF THE EVENT LIST IS NOT A KEY OF THIS PANEL ──────────────────
//
// The four checks below are about the control strip and the toolbar: is every
// key big enough to hit, does every icon-only key say what it is, does every
// key carry its id, is anything drawn off the panel. None of that is true of
// the widgets inside the event table, and none of it should be: the chooser
// beside a comment is 16 px wide because it sits in a table row, a cell has no
// mrKey because the gate finds cells by column, and a row below the fold IS
// outside the panel — that is what a scrolling list is.
//
// They never came up before because this tool's table was EMPTY. The panel's
// has always had them.
bool inEventList(const QWidget *c)
{
	for (const QWidget *p = c; p; p = p->parentWidget())
		if (p->objectName() == QStringLiteral("mrEvents"))
			return true;
	return false;
}

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
		if (inEventList(c))
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
		if (!b->isVisible() || inEventList(b))
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
		if (b->objectName().startsWith(QStringLiteral("qt_")) ||
		    inEventList(b))
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
// ── HOW MUCH OF THE MONITORING ROW IS NOT A PICTURE ──────────────────────
//
// The row is A (and B) plus the camera tiles, all 16:9 and all one height, and
// the arithmetic in tileBlockFor solves for the height that makes it exactly
// as wide as the pane. When something clamps that height — the room the
// splitter can give, the per-tile ceiling — the row comes out SHORT and what
// is left over is drawn as panel: a band down the right of the pictures, which
// is the "the angles stop filling the space, they leave borders" report.
//
// Measured as the gap between the rightmost picture edge and the pane, in
// pixels and as a share, because "there is a border" and "there are four
// pixels of border" are different bugs and only the number tells them apart.
// ONE AXIS HAS TO BE FULL, and that is the whole invariant — not "the row fills
// its width", which is not always possible and was the wrong thing to assert.
//
// The pictures are 16:9 and the pane is whatever shape the operator's two
// dividers make it. There is exactly ONE pane height at which pictures of that
// aspect fill both axes; give the row more and the slack is vertical, give it
// less and the slack is horizontal. So a band on one axis is geometry and is
// the direct consequence of a drag.
//
// A band on BOTH axes is not geometry. It means the pictures were sized for a
// pane other than the one they got — which is exactly what was reported: a 1082
// x 224 row whose cameras were drawn 237 x 133, short on the width AND short on
// the height, because the arithmetic had read a half-settled panel and the
// ceiling it wrote is a maximum that sticks.
struct RowFit {
	int paneW = 0, coveredW = 0, gapW = 0;
	int paneH = 0, coveredH = 0, gapH = 0;
	// ...and the same question asked of the CAMERA block on its own, which is
	// the half the report was about: A can fill the row while the cameras
	// beside it do not.
	int tilePaneW = 0, tileCoveredW = 0, tileGapW = 0;
	int tilePaneH = 0, tileCoveredH = 0, tileGapH = 0;
};

RowFit rowFit(Mock *w)
{
	RowFit f;
	const QWidget *pane = w->monitorPane();
	if (!pane)
		return f;
	f.paneW = pane->width();
	f.paneH = pane->height();
	int right = 0, bottom = 0, left = INT_MAX, top = INT_MAX;
	for (const QWidget *box : w->pictureBoxes()) {
		if (!box->isVisible())
			continue;
		const QPoint at = box->mapTo(pane, QPoint(0, 0));
		left = qMin(left, at.x());
		top = qMin(top, at.y());
		right = qMax(right, at.x() + box->width());
		bottom = qMax(bottom, at.y() + box->height());
	}
	if (left == INT_MAX)
		return f;
	f.coveredW = right - left;
	f.coveredH = bottom - top;
	f.gapW = f.paneW - f.coveredW;
	f.gapH = f.paneH - f.coveredH;

	if (const QWidget *tp = w->tilePane(); tp && tp->isVisible()) {
		f.tilePaneW = tp->width();
		f.tilePaneH = tp->height();
		int r = 0, b = 0, l = INT_MAX, t = INT_MAX;
		for (const QWidget *box : w->tileBoxes()) {
			if (!box->isVisible())
				continue;
			const QPoint at = box->mapTo(tp, QPoint(0, 0));
			l = qMin(l, at.x());
			t = qMin(t, at.y());
			r = qMax(r, at.x() + box->width());
			b = qMax(b, at.y() + box->height());
		}
		if (l != INT_MAX) {
			f.tileCoveredW = r - l;
			f.tileCoveredH = b - t;
			f.tileGapW = f.tilePaneW - f.tileCoveredW;
			f.tileGapH = f.tilePaneH - f.tileCoveredH;
		}
	}
	return f;
}

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
		// view's corner button), and the event list's cells are cells
		// rather than keys. Neither is one of this panel's keys.
		if (b->objectName().startsWith(QStringLiteral("qt_")) ||
		    inEventList(b))
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


// ---------------------------------------------------------------------------
// THE HOST UNDERNEATH — what OBS is painting while the panel sits in it
// ---------------------------------------------------------------------------
//
// THIS IS THE THIRD THING THIS TOOL LIED ABOUT BY OMISSION, and it is the one
// that cost the most. It omitted a section the panel hides, then the tile
// arithmetic, and then an entire APPLICATION STYLE SHEET: a widget kind the
// panel uses and has no rule for is not unstyled, it is styled by OBS. With no
// application sheet at all, every one of those rendered correctly here and
// always had — so a panel that came out perfect in six PNGs came out with a
// dark band across it in the operator's OBS.
//
// So `--host=obs` puts the opponent on the field: OBS's own Qt palette (Yami's
// palette_* block, which is the only route a plugin has to a theme's colours)
// and the rules from Yami.obt that actually contend with ours. It is an
// EXCERPT and says so — the whole sheet needs var() and calc() resolved, which
// is why parsing .obt was rejected in the first place — but every rule in it
// was copied out of that file rather than remembered.
//
// The marks OBS prints (a menu arrow, a tick, a spin arrow) are `url(theme:…)`
// SVGs and this tool links no SVG support, so they are redrawn here as PNGs in
// the same near-white a dark theme draws them in. That is the part that
// matters: the bug is a foreign LIGHT mark landing on our light surface.
QString drawHostAsset(const QString &dir, const QString &name,
		      const std::function<void(QPainter &, QSize)> &paint)
{
	const QString path = QDir(dir).filePath(name + QStringLiteral(".png"));
	QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	{
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing, true);
		paint(p, img.size());
	}
	img.save(path);
	return path;
}

void installObsHost(QApplication &app, const QString &assetDir)
{
	QDir().mkpath(assetDir);
	// Yami's near-white: the colour every one of these marks is drawn in for
	// a dark theme, which is exactly why it is invisible on ours.
	const QColor ink(QStringLiteral("#FFFFFF"));
	const auto triangle = [&](bool pointingDown) {
		return [&, pointingDown](QPainter &p, QSize s) {
			p.setBrush(ink);
			p.setPen(Qt::NoPen);
			const double a = pointingDown ? 0.34 : 0.66;
			const double b = pointingDown ? 0.70 : 0.30;
			const QPointF tri[3] = {
				QPointF(3, s.height() * a),
				QPointF(s.width() - 3, s.height() * a),
				QPointF(s.width() / 2.0, s.height() * b)};
			p.drawPolygon(tri, 3);
		};
	};
	const QString collapse =
		drawHostAsset(assetDir, QStringLiteral("collapse"), triangle(true));
	const QString up =
		drawHostAsset(assetDir, QStringLiteral("up"), triangle(false));
	const QString down =
		drawHostAsset(assetDir, QStringLiteral("down"), triangle(true));
	const QString right = drawHostAsset(
		assetDir, QStringLiteral("right"), [&](QPainter &p, QSize s) {
			p.setBrush(ink);
			p.setPen(Qt::NoPen);
			const QPointF tri[3] = {QPointF(s.width() * 0.36, 3),
						QPointF(s.width() * 0.36,
							s.height() - 3.0),
						QPointF(s.width() * 0.70,
							s.height() / 2.0)};
			p.drawPolygon(tri, 3);
		});
	const QString tick = drawHostAsset(
		assetDir, QStringLiteral("tick"), [&](QPainter &p, QSize s) {
			QPen pen(ink);
			pen.setWidthF(2.0);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.drawLine(QPointF(3, s.height() * 0.55),
				   QPointF(s.width() * 0.42, s.height() - 4.0));
			p.drawLine(QPointF(s.width() * 0.42, s.height() - 4.0),
				   QPointF(s.width() - 3.0, 4.0));
		});
	const QString box = drawHostAsset(
		assetDir, QStringLiteral("box"), [&](QPainter &p, QSize s) {
			p.setPen(QPen(ink, 1.0));
			p.setBrush(Qt::NoBrush);
			p.drawRect(QRectF(2.5, 2.5, s.width() - 5.0,
					  s.height() - 5.0));
		});

	// Yami's palette_* block, resolved. Window is grey7 #1D1F26 — the exact
	// dark that showed through the panel in the report.
	QPalette pal = app.palette();
	pal.setColor(QPalette::Window, QColor(QStringLiteral("#1D1F26")));
	pal.setColor(QPalette::WindowText, QColor(QStringLiteral("#FFFFFF")));
	pal.setColor(QPalette::Base, QColor(QStringLiteral("#272A33")));
	pal.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#272A33")));
	pal.setColor(QPalette::Text, QColor(QStringLiteral("#FFFFFF")));
	pal.setColor(QPalette::Button, QColor(QStringLiteral("#3C404D")));
	pal.setColor(QPalette::ButtonText, QColor(QStringLiteral("#FFFFFF")));
	pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#284CB8")));
	pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#FFFFFF")));
	pal.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#272A33")));
	pal.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#FFFFFF")));
	app.setPalette(pal);

	// The excerpt. Selectors verbatim from Yami.obt — a selector we paraphrase
	// is a fight we are not actually having.
	const QString sheet =
		QStringLiteral(
			"QWidget { alternate-background-color: #272A33; color: #FFFFFF;"
			" selection-background-color: #284CB8; selection-color: #FFFFFF; }\n"
			"QWidget:disabled { color: #8A8F9E; }\n"
			"QDialog, QMainWindow { background-color: #1D1F26; }\n"
			"QPushButton { background-color: #3C404D; color: #FFFFFF;"
			" border: 1px solid #5B6273; border-radius: 4px; padding: 3px 9px; }\n"
			"QPushButton::menu-indicator { image: url(%1);"
			" subcontrol-position: right; subcontrol-origin: content;"
			" margin-left: 8px; right: -2px; }\n"
			"QToolButton { border: 1px solid #5B6273; background-color: #3C404D; }\n"
			"QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit {"
			" background-color: #3C404D; color: #FFFFFF;"
			" border: 1px solid #5B6273; border-radius: 4px; padding: 3px 6px; }\n"
			"QSpinBox::up-button, QDoubleSpinBox::up-button {"
			" subcontrol-origin: padding; subcontrol-position: top right;"
			" width: 26px; height: 12px; border-left: 1px solid #272A33; }\n"
			"QSpinBox::down-button, QDoubleSpinBox::down-button {"
			" subcontrol-origin: padding; subcontrol-position: bottom right;"
			" width: 26px; height: 12px; border-left: 1px solid #272A33;"
			" border-top: 1px solid #272A33; }\n"
			"QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {"
			" image: url(%2); width: 100%; margin: 2px; }\n"
			"QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
			" image: url(%3); width: 100%; padding: 2px; }\n"
			"QCheckBox::indicator, QGroupBox::indicator, QTableView::indicator {"
			" width: 16px; height: 16px; margin-right: 8px; }\n"
			"QCheckBox::indicator:unchecked, QGroupBox::indicator:unchecked,"
			"QTableView::indicator:unchecked { image: url(%4); }\n"
			"QCheckBox::indicator:checked, QGroupBox::indicator:checked,"
			"QTableView::indicator:checked { image: url(%5); }\n"
			"QHeaderView::section { background-color: #3C404D; color: #FFFFFF;"
			" border: none; border-left: 1px solid #1D1F26;"
			" border-right: 1px solid #1D1F26; padding: 3px 0px;"
			" margin-bottom: 2px; }\n"
			"QTabWidget::pane { border-top: 4px solid #272A33; }\n"
			"QListView, QTreeView, QTableView { background-color: #272A33;"
			" color: #FFFFFF; }\n"
			"QMenu { background-color: #1D1F26; }\n"
			// THE ITEM RULE, and leaving it out is what let this tool
			// call a white-on-white menu readable. Yami colours the
			// ITEM, not the menu — so a rule of ours on QMenu alone
			// never wins for the text drawn on it, however specific
			// it is. Same rule, same line, for a list's items.
			"QMenu::item, QMenu > QWidget, QListView::item,"
			"QListWidget::item { color: #FFFFFF;"
			" border: 1px solid transparent; padding: 4px 8px; }\n"
			"QMenu::item:disabled { color: #8A8F9E; }\n"
			"QMenu::separator { background: #3C404D; height: 1px; }\n"
			// A submenu arrow, drawn white for a dark theme.
			"QMenu::right-arrow { image: url(%6); }\n"
			"QSplitter::handle { background-color: #1D1F26; }\n"
			"QScrollBar { background-color: #272A33; }\n")
			.arg(collapse, up, down, box, tick).arg(right);
	app.setStyleSheet(sheet);
}

// ---------------------------------------------------------------------------
// THE SETTINGS DIALOG, as a set of WIDGET KINDS
// ---------------------------------------------------------------------------
//
// Not the fields — the KINDS. Everything that has ever gone wrong in that
// dialog went wrong because a kind of widget had no rule of ours and got OBS's
// instead: a spin box with two dark hairlines down it and a white arrow on
// white paper, a tick drawn for a dark theme landing on our green, a navigation
// list with no colour at all. So this builds one of each, in the same
// containers and with the same object names as openSettings(), and the picture
// is judged on whether anything in it belongs to a different panel.
//
// IT IS A STAND-IN AND CANNOT BE ANYTHING ELSE: the real dialog needs a
// ReplayCore, a scene list and an updater. What it CAN be is complete about the
// kinds, and that list is short enough to keep true by reading openSettings.
QDialog *buildSettingsMock(QWidget *parent)
{
	auto *dlg = new QDialog(parent);
	dlg->setWindowTitle(QStringLiteral("Impostazioni"));
	dlg->setMinimumSize(760, 480);

	auto *root = new QVBoxLayout(dlg);
	auto *body = new QHBoxLayout();
	body->setSpacing(0);

	auto *nav = new QListWidget(dlg);
	nav->setObjectName(QStringLiteral("mrSettingsNav"));
	nav->setFixedWidth(172);
	nav->setFocusPolicy(Qt::NoFocus);
	auto *pages = new QStackedWidget(dlg);
	body->addWidget(nav, 0);
	body->addWidget(pages, 1);
	root->addLayout(body, 1);

	const auto addPage = [&](const QString &title,
				 const QString &blurb) -> QFormLayout * {
		auto *page = new QWidget(pages);
		auto *v = new QVBoxLayout(page);
		v->setContentsMargins(14, 12, 14, 12);
		v->setSpacing(2);
		auto *t = new QLabel(title, page);
		t->setObjectName(QStringLiteral("mrSettingsTitle"));
		v->addWidget(t);
		auto *b = new QLabel(blurb, page);
		b->setObjectName(QStringLiteral("mrSettingsBlurb"));
		b->setWordWrap(true);
		v->addWidget(b);
		auto *form = new QFormLayout();
		form->setContentsMargins(0, 10, 0, 0);
		form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
		form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
		v->addLayout(form);
		v->addStretch(1);
		pages->addWidget(page);
		nav->addItem(title);
		return form;
	};
	QObject::connect(nav, &QListWidget::currentRowChanged, pages,
			 &QStackedWidget::setCurrentIndex);

	// Session: the two cards, a path with a browse key, three spin boxes.
	QFormLayout *rec = addPage(QStringLiteral("Session"),
				   QStringLiteral("Dove finisce il girato, e con che qualita."));
	{
		const auto card = [&](const QString &cap, const QString &val,
				      const QString &unit) -> QWidget * {
			auto *f = new QFrame(dlg);
			f->setObjectName(QStringLiteral("mrStatCard"));
			f->setFrameShape(QFrame::NoFrame);
			auto *cv = new QVBoxLayout(f);
			cv->setContentsMargins(12, 8, 12, 8);
			cv->setSpacing(1);
			auto *c = new QLabel(cap, f);
			c->setObjectName(QStringLiteral("mrStatCaption"));
			cv->addWidget(c);
			auto *row = new QHBoxLayout();
			row->setContentsMargins(0, 0, 0, 0);
			row->setSpacing(4);
			auto *value = new QLabel(val, f);
			value->setObjectName(QStringLiteral("mrStatValue"));
			row->addWidget(value, 0, Qt::AlignBottom);
			auto *u = new QLabel(unit, f);
			u->setObjectName(QStringLiteral("mrStatUnit"));
			row->addWidget(u, 0, Qt::AlignBottom);
			row->addStretch(1);
			cv->addLayout(row);
			return f;
		};
		auto *cards = new QWidget(dlg);
		auto *ch = new QHBoxLayout(cards);
		ch->setContentsMargins(0, 0, 0, 6);
		ch->setSpacing(8);
		ch->addWidget(card(QStringLiteral("SPAZIO LIBERO"),
				   QStringLiteral("743.2"), QStringLiteral("GiB")),
			      1);
		ch->addWidget(card(QStringLiteral("GIRATO"), QStringLiteral("9:12"),
				   QStringLiteral("ore")),
			      1);
		rec->addRow(cards);
	}
	{
		auto *row = new QHBoxLayout();
		auto *edit = new QLineEdit(QStringLiteral("D:/Partite"), dlg);
		auto *browse = new QPushButton(QStringLiteral("..."), dlg);
		browse->setFixedWidth(34);
		row->addWidget(edit, 1);
		row->addWidget(browse);
		rec->addRow(QStringLiteral("Cartella di sessione"), row);
	}
	{
		const char *labels[] = {"Split", "Bitrate video", "Bitrate audio"};
		const char *suffixes[] = {" min", " kbps", " kbps"};
		const int values[] = {20, 12000, 160};
		for (int i = 0; i < 3; i++) {
			auto *s = new QSpinBox(dlg);
			s->setRange(0, 200000);
			s->setSuffix(QString::fromLatin1(suffixes[i]));
			s->setValue(values[i]);
			rec->addRow(QString::fromLatin1(labels[i]), s);
		}
	}

	// Cameras: the 4x2 grid of source picker + name.
	QFormLayout *cams = addPage(QStringLiteral("Telecamere"),
				    QStringLiteral("Quale sorgente OBS e quale angolo."));
	{
		auto *grid = new QGridLayout();
		grid->setHorizontalSpacing(14);
		grid->setVerticalSpacing(4);
		for (int i = 0; i < 8; i++) {
			auto *c = new QComboBox(dlg);
			c->addItem(QStringLiteral("(nessuna)"));
			c->addItem(QStringLiteral("Media"));
			c->addItem(QStringLiteral("C2"));
			c->setCurrentIndex(i < 2 ? i + 1 : 0);
			c->setMinimumWidth(120);
			auto *name = new QLineEdit(dlg);
			name->setPlaceholderText(
				QStringLiteral("CAM %1").arg(i + 1));
			if (i < 2)
				name->setText(QStringLiteral("C%1").arg(i + 1));
			name->setFixedWidth(96);
			auto *cell = new QHBoxLayout();
			cell->setContentsMargins(0, 0, 0, 0);
			cell->addWidget(new QLabel(QString::number(i + 1), dlg));
			cell->addWidget(c, 1);
			cell->addWidget(name);
			grid->addLayout(cell, i % 4, i / 4);
		}
		cams->addRow(grid);
	}

	// Replay: combos, check boxes, a spin box, and a DISABLED pair — the
	// second bay's fields, which is the only place in this dialog where a
	// disabled colour is on screen.
	QFormLayout *out = addPage(QStringLiteral("Replay / Messa in onda"),
				   QStringLiteral("Come il replay prende il Program."));
	{
		auto *useB = new QCheckBox(dlg);
		out->addRow(QStringLiteral("Seconda baia"), useB);
		auto *sceneA = new QComboBox(dlg);
		sceneA->addItem(QStringLiteral("(nessuna)"));
		sceneA->addItem(QStringLiteral("REPLAY A"));
		sceneA->setCurrentIndex(1);
		out->addRow(QStringLiteral("Scena di output"), sceneA);
		auto *sceneB = new QComboBox(dlg);
		sceneB->addItem(QStringLiteral("(nessuna)"));
		sceneB->setEnabled(false);
		out->addRow(QStringLiteral("Scena di output B"), sceneB);
		auto *fit = new QCheckBox(dlg);
		fit->setChecked(true);
		out->addRow(QStringLiteral("Adatta alla canvas"), fit);
		auto *ms = new QSpinBox(dlg);
		ms->setRange(0, 20000);
		ms->setSuffix(QStringLiteral(" ms"));
		ms->setValue(300);
		out->addRow(QStringLiteral("Durata transizione"), ms);
		auto *note = new QLabel(
			QStringLiteral("La sequenza esportata non usa queste "
				       "transizioni: e una stream copy."),
			dlg);
		note->setObjectName(QStringLiteral("mrSettingsBlurb"));
		note->setWordWrap(true);
		out->addRow(QString(), note);
	}

	// Events: the double spin boxes, more check boxes, the tag box.
	QFormLayout *ev = addPage(QStringLiteral("Eventi"),
				  QStringLiteral("Come cade una marcatura, e cosa si scrive sopra."));
	{
		const char *labels[] = {"Pre-roll", "Post-roll",
					"Continua oltre l'OUT"};
		const double values[] = {3.0, 1.5, 0.0};
		for (int i = 0; i < 3; i++) {
			auto *s = new QDoubleSpinBox(dlg);
			s->setRange(0.0, 60.0);
			s->setDecimals(1);
			s->setSuffix(QStringLiteral(" s"));
			s->setValue(values[i]);
			ev->addRow(QString::fromUtf8(labels[i]), s);
		}
		auto *sortBy = new QCheckBox(dlg);
		sortBy->setChecked(true);
		ev->addRow(QStringLiteral("Ordina per tempo"), sortBy);
		auto *dbl = new QCheckBox(dlg);
		dbl->setChecked(true);
		ev->addRow(QStringLiteral("Doppio click riproduce"), dbl);
		auto *digits = new QSpinBox(dlg);
		digits->setRange(1, 8);
		digits->setValue(4);
		ev->addRow(QStringLiteral("Cifre ID"), digits);
		auto *tags = new QPlainTextEdit(dlg);
		tags->setPlainText(QStringLiteral("Gol\nFallo\nEsultanza\n"));
		tags->setMaximumHeight(96);
		ev->addRow(QStringLiteral("Tag"), tags);
	}

	// Interface: where the theme itself is chosen.
	QFormLayout *ui = addPage(QStringLiteral("Interfaccia"),
				  QStringLiteral("Che aspetto ha il pannello."));
	{
		auto *mv = new QCheckBox(dlg);
		mv->setChecked(true);
		ui->addRow(QStringLiteral("Mostra multiview"), mv);
		auto *theme = new QComboBox(dlg);
		theme->addItem(QStringLiteral("Segui il tema di OBS"));
		theme->addItem(QStringLiteral("Broadcast scuro"));
		theme->addItem(QStringLiteral("Alto contrasto"));
		theme->addItem(QStringLiteral("Chiaro"));
		theme->setCurrentIndex(3);
		ui->addRow(QStringLiteral("Tema"), theme);
		auto *rows = new QComboBox(dlg);
		rows->addItem(QStringLiteral("Comode"));
		rows->addItem(QStringLiteral("Compatte"));
		rows->addItem(QStringLiteral("Dense"));
		ui->addRow(QStringLiteral("Righe"), rows);
	}

	// Updates: the read-only changelog and the three keys under it.
	QFormLayout *upd = addPage(QStringLiteral("Aggiornamenti"),
				   QStringLiteral("Fra una partita e l'altra."));
	{
		upd->addRow(QStringLiteral("Versione installata"),
			    new QLabel(QStringLiteral("1.0.0-beta6"), dlg));
		auto *chan = new QComboBox(dlg);
		chan->addItem(QStringLiteral("Stabile"));
		chan->addItem(QStringLiteral("Beta"));
		upd->addRow(QStringLiteral("Canale"), chan);
		auto *btnRow = new QWidget(dlg);
		auto *bl = new QHBoxLayout(btnRow);
		bl->setContentsMargins(0, 0, 0, 0);
		bl->addWidget(new QPushButton(QStringLiteral("Controlla"), dlg));
		auto *get = new QPushButton(QStringLiteral("Scarica"), dlg);
		get->setEnabled(false);
		bl->addWidget(get);
		auto *inst = new QPushButton(QStringLiteral("Installa"), dlg);
		inst->setEnabled(false);
		bl->addWidget(inst);
		bl->addStretch(1);
		upd->addRow(QString(), btnRow);
		auto *notes = new QPlainTextEdit(dlg);
		notes->setReadOnly(true);
		notes->setMinimumHeight(120);
		notes->setPlainText(QStringLiteral(
			"- il pannello chiaro dipinge il proprio fondo\n"
			"- niente piu freccia di OBS sopra i nostri marchi\n"));
		upd->addRow(QStringLiteral("Novita"), notes);
	}

	auto *buttons = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel, dlg);
	root->addWidget(buttons, 0);
	nav->setCurrentRow(0);
	return dlg;
}

// ---------------------------------------------------------------------------
// THE CHECKS THAT NEEDED A HOST — measured in PIXELS, not in colour arithmetic
// ---------------------------------------------------------------------------
//
// Everything above this line reasons about the SCHEME: is this hex readable on
// that hex. That catches a colour chosen badly and is blind to a colour that
// never reaches the screen — which is what every fault in the light-panel
// report turned out to be. The panel's own background rule was a no-op for as
// long as the panel has existed; the mark on the filled play key was drawn in
// chrome grey by a pixmap no style sheet can reach; OBS printed its own arrow
// over three of our marks and its own tick over our tick.
//
// So these render the panel and READ IT BACK. With `--host=obs` underneath —
// OBS's dark palette and the rules from Yami.obt that contend with ours — a
// light panel that fails to paint something comes back dark, and that is a
// number.

// The most common colour in a region: the fill, whatever it happens to be.
QColor dominant(const QImage &img, const QRect &r)
{
	QHash<QRgb, int> counts;
	for (int y = r.top(); y <= r.bottom(); y++)
		for (int x = r.left(); x <= r.right(); x++)
			counts[img.pixel(x, y)]++;
	QRgb best = 0;
	int most = -1;
	for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
		if (it.value() > most) {
			most = it.value();
			best = it.key();
		}
	return QColor::fromRgb(best);
}

// The pixel in `r` that stands furthest off `from`: the mark, if there is one.
QColor boldest(const QImage &img, const QRect &r, const QColor &from)
{
	QColor best = from;
	double worst = 0;
	for (int y = r.top(); y <= r.bottom(); y++)
		for (int x = r.left(); x <= r.right(); x++) {
			const QColor c = QColor::fromRgb(img.pixel(x, y));
			const double d = contrast(c, from);
			if (d > worst) {
				worst = d;
				best = c;
			}
		}
	return best;
}

bool sameColour(const QColor &a, const QColor &b, int tol = 6)
{
	return qAbs(a.red() - b.red()) <= tol && qAbs(a.green() - b.green()) <= tol &&
	       qAbs(a.blue() - b.blue()) <= tol;
}

QAbstractButton *keyById(QWidget *root, const QString &id)
{
	for (QAbstractButton *b : root->findChildren<QAbstractButton *>())
		if (b->property(kKeyProperty).toString() == id)
			return b;
	return nullptr;
}

// PUTTING THE MONITORS DOWN HAS TO GIVE THE ROOM TO THE LIST.
//
// "The pictures went away" and "the room came back" are two claims, and for a
// while only the first was true: the splitter's child is the left COLUMN, not
// the picture row, so hiding the row left an empty visible child holding
// exactly the height the key was pressed to reclaim. On a maximised panel that
// is a band of nothing where the pictures were, and a table no taller than it
// was — which is the report this check exists for.
//
// Asserted on the GROWTH against the room that was there, not on a threshold:
// "the table got bigger" passes on a panel that hands back a third of it.
// Measured on the ELASTIC axis of the arrangement, because Short divides width
// and the other two divide height.
void checkMonitorsGiveRoom(Mock *w, const QString &label)
{
	QAbstractButton *mon = keyById(w, QStringLiteral("monitors"));
	if (!check(mon && mon->isChecked(),
		   label + ": the Monitors key is up to press"))
		return;
	const bool wide = w->mode_ != PanelMode::Short;
	const auto listSize = [&]() {
		QTableWidget *t = w->eventTable();
		return t ? (wide ? t->height() : t->width()) : 0;
	};
	const auto pictureRoom = [&]() {
		if (!w->leftCol_ || !w->leftCol_->isVisible())
			return 0;
		return (wide ? w->leftCol_->height() : w->leftCol_->width()) +
		       w->bodySplit_->handleWidth();
	};
	const auto settle = [&]() {
		for (int i = 0; i < 6; i++) {
			QApplication::processEvents();
			QApplication::sendPostedEvents();
		}
	};

	const int before = listSize();
	const int roomBefore = pictureRoom();
	mon->click();
	settle();
	const int after = listSize();
	const int roomAfter = pictureRoom();
	const int gaveUp = roomBefore - roomAfter;
	// EVERYTHING THE COLUMN GAVE UP LANDED ON THE LIST. Not "the table got
	// bigger", which passes on a panel that hands back a third of it, and not
	// "the list got the whole column" either — in Short the key strip lives
	// down there too and the column keeps the width the keys need. Sixteen
	// pixels of slack: layout spacing and a splitter handle, not a share.
	check(roomBefore > 0 && gaveUp > 0 && after - before >= gaveUp - 16,
	      label + ": Monitors down gives the room to the list",
	      QString("list %1 -> %2, pictures %3 -> %4")
		      .arg(before)
		      .arg(after)
		      .arg(roomBefore)
		      .arg(roomAfter));
	// ...AND WHERE THE PICTURES WERE ALL IT HELD, THE COLUMN GOES WITH THEM.
	// This is the half that shipped broken: an empty but VISIBLE splitter
	// child keeps the share it was given, so the pictures vanished and the
	// band of panel where they had been did not.
	if (wide)
		check(roomAfter == 0,
		      label + ": the empty column goes with the pictures",
		      QString("%1 px still held").arg(roomAfter));
	mon->click(); // put them back: everything after this looks at pictures
	settle();
	check(w->leftCol_ && w->leftCol_->isVisible() && listSize() > 0,
	      label + ": Monitors up brings the pictures back",
	      QString("list back to %1 px").arg(listSize()));
}

// 1. THE PANEL PAINTS ITS OWN BACKGROUND — the fault that produced the report.
//
// Not everywhere: the parts of the panel inside the splitter were always
// right, because a QSplitter is a QFrame and Qt styles those. It is the band
// under the control strip and the four margins — everything the dock itself is
// behind — that came back as OBS's window colour.
void checkPanelSurface(Mock *w, const QImage &shot, const QString &label)
{
	const QColor want(g_sc.panel);
	struct Spot {
		const char *what;
		QPoint at;
	};
	// Two margins and the gap between two sections of the strip. Sampled
	// from the panel's own geometry rather than from numbers typed here, so
	// the check follows the layout instead of pinning it.
	const int stripY = w->strip_->mapTo(w, QPoint(0, 0)).y() +
			   w->strip_->height() / 2;
	const Spot spots[] = {
		{"left margin", QPoint(1, w->height() / 2)},
		{"right margin", QPoint(w->width() - 2, w->height() / 2)},
		{"under the strip", QPoint(1, stripY)},
		{"bottom margin", QPoint(w->width() / 2, w->height() - 2)},
	};
	for (const Spot &s : spots) {
		if (!shot.rect().contains(s.at))
			continue;
		const QColor got = QColor::fromRgb(shot.pixel(s.at));
		check(sameColour(got, want),
		      label + QString(": the panel owns its %1").arg(s.what),
		      QString("%1, wanted %2").arg(got.name(), want.name()));
	}
}

// 2. A MARK ON A SIGNAL KEY IS DRAWN IN THAT KEY'S INK.
//
// The play key is a filled green rectangle carrying nothing but a ▶, and the
// ▶ came out the panel's resting grey — a style sheet says `color: #ffffff`
// for the label and cannot reach a pixmap. Read off the picture: whatever the
// key is filled with, the mark on it has to stand off it.
void checkMarkOnKey(Mock *w, const QString &id, const QString &what)
{
	QAbstractButton *b = keyById(w, id);
	if (!check(b && b->isVisible(), what + ": the key is on the panel"))
		return;
	const QImage shot = b->grab().toImage().convertToFormat(
		QImage::Format_ARGB32);
	// Inside the border, so the frame is not mistaken for the mark.
	const QRect inner = shot.rect().adjusted(3, 3, -3, -3);
	if (inner.width() < 4 || inner.height() < 4)
		return;
	const QColor fill = dominant(shot, inner);
	const QColor mark = boldest(shot, inner, fill);
	check(contrast(mark, fill) >= 3.0, what + ": its mark reads on it",
	      QString("%1 on %2 = %3:1")
		      .arg(mark.name(), fill.name())
		      .arg(contrast(mark, fill), 0, 'f', 1));
}

// 2b. ...AND IT IS THE RIGHT INK, not merely a legible one.
//
// Legibility alone passes REC with a grey dot next to the word REC written in
// red, and passes Annulla drawn exactly as neutral as the two keys it undoes.
// Those are the faults; both keys read perfectly well. What was wrong is that
// the mark and the label of one key disagreed about what the key is.
void checkMarkInk(Mock *w, const QString &id, const QColor &want,
		  const QString &what)
{
	QAbstractButton *b = keyById(w, id);
	if (!check(b && b->isVisible(), what + ": the key is on the panel"))
		return;
	const QImage shot = b->grab().toImage().convertToFormat(
		QImage::Format_ARGB32);
	const QRect inner = shot.rect().adjusted(3, 3, -3, -3);
	if (inner.width() < 4 || inner.height() < 4)
		return;
	const QColor fill = dominant(shot, inner);
	const QColor mark = boldest(shot, inner, fill);
	// Generous: the mark is antialiased, so even its boldest pixel is a few
	// steps short of the ink it was drawn with.
	check(sameColour(mark, want, 40), what + ": its mark is the key's ink",
	      QString("%1, wanted %2").arg(mark.name(), want.name()));
}

// 3. NOBODY ELSE'S MARK IS ON OUR KEYS.
//
// OBS prints a menu arrow over any button that has a menu, bottom right and
// clipped by the key's own edge, so the gear wore a gear AND a triangle. There
// is no way to ask Qt "did another sheet draw here", but there is a property
// that tells the two apart: our marks are CENTRED, and a second one hanging off
// the right edge moves the centre of everything drawn on the key.
void checkOneMarkOnly(Mock *w, const QString &id, const QString &what,
		      const QString &outDir = QString())
{
	QAbstractButton *b = keyById(w, id);
	if (!check(b && b->isVisible(), what + ": the key is on the panel"))
		return;
	const QImage shot = b->grab().toImage().convertToFormat(
		QImage::Format_ARGB32);
	// Kept when a folder was named: a mark that is off centre by three
	// pixels is answered by looking at it, not by reading the number again.
	if (!outDir.isEmpty())
		shot.scaled(shot.width() * 8, shot.height() * 8,
			    Qt::IgnoreAspectRatio, Qt::FastTransformation)
			.save(QDir(outDir).filePath(
				QString("mock-key-%1.png").arg(id)));
	const QRect inner = shot.rect().adjusted(3, 3, -3, -3);
	if (inner.width() < 6 || inner.height() < 6)
		return;
	const QColor fill = dominant(shot, inner);
	int lo = INT_MAX, hi = INT_MIN;
	for (int y = inner.top(); y <= inner.bottom(); y++)
		for (int x = inner.left(); x <= inner.right(); x++)
			if (contrast(QColor::fromRgb(shot.pixel(x, y)), fill) >=
			    1.6) {
				lo = qMin(lo, x);
				hi = qMax(hi, x);
			}
	{
		QStyleOptionButton so;
		so.initFrom(b);
		std::printf("   [%s] key %dx%d  menu indicator %d px\n",
			    qUtf8Printable(what), b->width(), b->height(),
			    b->style()->pixelMetric(QStyle::PM_MenuButtonIndicator,
						    &so, b));
	}
	// SIX PIXELS OF INK, not one. "Is anything drawn" passes on a mark that
	// has been clipped by its own key down to a smudge — which is exactly
	// what the ▾ was, and what nobody noticed for as long as the platform
	// was printing a second, bigger arrow over the top of it.
	if (!check(hi - lo >= 5, what + ": its mark is actually drawn",
		   QString("%1 px of ink").arg(hi >= lo ? hi - lo + 1 : 0)))
		return;
	const double centre = (lo + hi) / 2.0;
	const double want = (inner.left() + inner.right()) / 2.0;
	check(qAbs(centre - want) <= 3.0, what + ": one mark, centred",
	      QString("ink spans %1..%2, centre %3, key centre %4")
		      .arg(lo)
		      .arg(hi)
		      .arg(centre, 0, 'f', 1)
		      .arg(want, 0, 'f', 1));
}

// 3b. A POPUP IS A WINDOW OF ITS OWN, AND NOBODY HAD EVER RENDERED ONE.
//
// A drop-down list and a menu are top-level windows parented to the widget that
// opened them — so the panel's rules DO reach them, and so does everything of
// OBS's that we have not out-ranked. Nothing in this tool ever opened one, so
// "the menus are white on white" was invisible to it while being the first
// thing an operator sees.
//
// Read as a pair, not as one colour: what is wrong with a menu is never the
// background on its own, it is the background AGAINST the text on it.
//
// AND ROW BY ROW, not over the whole popup. The first version took the boldest
// pixel anywhere inside it, which is the MOST readable thing on screen — so a
// menu of white-on-white entries passed on the strength of its one greyed-out
// row. What has to be true is that every row is readable, so every row is
// measured, and the answer is the worst of them.
void checkPopupReadable(QWidget *popup, const QVector<QPair<QRect, bool>> &rows,
			const QString &what, const QString &outDir,
			const QString &name)
{
	if (!check(popup != nullptr, what + ": it opened"))
		return;
	const QImage shot =
		popup->grab().toImage().convertToFormat(QImage::Format_ARGB32);
	if (!outDir.isEmpty())
		shot.save(QDir(outDir).filePath(
			QString("mock-popup-%1.png").arg(name)));
	if (!check(!rows.isEmpty(), what + ": it has rows to read"))
		return;

	double worst = 1e9;   // the least readable row, whatever its bar
	QString detail;
	bool allPass = true;
	for (const auto &[rect, enabled] : rows) {
		const QRect r = rect.intersected(shot.rect()).adjusted(1, 1, -1, -1);
		if (r.width() < 4 || r.height() < 4)
			continue;
		const QColor paper = dominant(shot, r);
		const QColor ink = boldest(shot, r, paper);
		const double c = contrast(ink, paper);
		// A DISABLED ROW IS MEANT TO BE DIM, so it answers a lower bar —
		// but it answers one: "greyed out" and "not there" are two
		// different things to an operator.
		const double want = enabled ? 4.0 : 2.0;
		if (c < worst) {
			worst = c;
			detail = QString("%1 on %2 = %3:1")
					 .arg(ink.name(), paper.name())
					 .arg(c, 0, 'f', 1);
		}
		if (c < want)
			allPass = false;
	}
	check(allPass, what + ": every row reads on it",
	      QString("worst row %1").arg(detail));
}

// The rows of an open menu, with whether each one is meant to be readable.
QVector<QPair<QRect, bool>> menuRows(QMenu *m)
{
	QVector<QPair<QRect, bool>> rows;
	for (QAction *a : m->actions()) {
		if (a->isSeparator())
			continue;
		rows << qMakePair(m->actionGeometry(a), a->isEnabled());
	}
	return rows;
}

// 3c. THE ROW IS ONE SIZE OF TYPE.
//
// Four of the columns are plain table items and two of them are widgets — the
// comment and the per-angle speed — so they are drawn by two different things
// and had drifted apart: the widgets were carrying a size written into the
// sheet while the items used the table's own font, which is OBS's base size in
// POINTS. The comment and the speed came out smaller than the id and the
// in-point on their own row.
//
// Measured off the widgets, not off the rules: what a rule asks for and what
// the painter uses are the same thing only when nothing else is talking, and
// something else was.
void checkRowTypeIsOneSize(Mock *w, const QString &label)
{
	QTableWidget *t = w->eventTable();
	if (!check(t && t->rowCount() > 0 && t->columnCount() > 4,
		   label + ": there is a row to read"))
		return;
	const int itemPx = QFontInfo(t->font()).pixelSize();
	int worst = 0;
	QString detail;
	const auto compare = [&](QWidget *cell, const char *what) {
		if (!cell)
			return;
		const int px = QFontInfo(cell->font()).pixelSize();
		if (std::abs(px - itemPx) > worst) {
			worst = std::abs(px - itemPx);
			detail = QString("%1 %2 px against the row's %3")
					 .arg(QString::fromLatin1(what))
					 .arg(px)
					 .arg(itemPx);
		}
	};
	compare(t->findChild<QWidget *>(QStringLiteral("mrAngleNote")), "comment");
	compare(t->findChild<QWidget *>(QStringLiteral("mrAngleSpeed")), "speed");
	check(worst == 0, label + ": the comment and the speed are the row's size",
	      detail.isEmpty() ? QString("both %1 px").arg(itemPx) : detail);
}

// 4. THE SETTINGS DIALOG IS THE SAME PANEL.
//
// It is a separate top-level window and every widget kind in it is a kind the
// strip does not use — spin boxes, check boxes, a navigation list, a stacked
// pane. Each of those was OBS's until it was named here. Rendered and read
// back: the dialog's own surface, the arrow on a spin box, and a ticked box
// that is our green rather than somebody else's tick.
void checkSettingsDialog(Mock *w, const QString &outDir)
{
	QDialog *dlg = buildSettingsMock(w);
	dlg->resize(820, 560);
	dlg->show();

	auto *nav = dlg->findChild<QListWidget *>(QStringLiteral("mrSettingsNav"));
	if (!check(nav != nullptr, "settings: the dialog has its side menu"))
		return;

	// EVERY PAGE, because only one is on screen at a time and the kinds are
	// spread across them: the spin boxes are on the first, the check boxes on
	// the third. Looking at one page and calling the dialog checked is the
	// same omission the whole tool has just been fixed for.
	QSpinBox *spinSeen = nullptr;
	QCheckBox *tickSeen = nullptr;
	QCheckBox *clearSeen = nullptr;
	QImage spinShot, tickShot;
	QColor paper;

	for (int page = 0; page < nav->count(); page++) {
		nav->setCurrentRow(page);
		for (int i = 0; i < 4; i++) {
			QApplication::processEvents();
			QApplication::sendPostedEvents();
		}
		const QImage shot = dlg->grab().toImage().convertToFormat(
			QImage::Format_ARGB32);
		if (!outDir.isEmpty())
			shot.save(QDir(outDir).filePath(
				QString("mock-settings-%1.png").arg(page)));
		// The dialog's own paper, sampled under the buttons where nothing
		// else is drawn.
		if (page == 0)
			paper = QColor::fromRgb(
				shot.pixel(shot.width() / 2, shot.height() - 3));
		for (QSpinBox *s : dlg->findChildren<QSpinBox *>())
			if (s->isVisible() && !spinSeen) {
				spinSeen = s;
				spinShot = shot;
			}
		for (QCheckBox *c : dlg->findChildren<QCheckBox *>()) {
			if (!c->isVisible())
				continue;
			if (c->isChecked() && !tickSeen) {
				tickSeen = c;
				tickShot = shot;
			}
			if (!c->isChecked() && !clearSeen)
				clearSeen = c;
		}
	}

	check(sameColour(paper, QColor(g_sc.panel), 10),
	      "settings: the dialog is the panel's colour",
	      QString("%1, wanted %2").arg(paper.name(), g_sc.panel));

	// A SPIN BOX, arrows and all. OBS draws them from a file chosen for a
	// dark theme — a white arrow, on our white field.
	if (check(spinSeen != nullptr, "settings: there is a spin box to look at")) {
		const QPoint at = spinSeen->mapTo(dlg, QPoint(0, 0));
		const QRect r(at, spinSeen->size());
		if (spinShot.rect().contains(r)) {
			const QColor field =
				dominant(spinShot, r.adjusted(2, 2, -2, -2));
			// The right-hand third, where the buttons live.
			const QRect btns(r.left() + r.width() * 2 / 3, r.top() + 2,
					 r.width() / 3 - 2, r.height() - 4);
			const QColor mark = boldest(spinShot, btns, field);
			check(contrast(mark, field) >= 3.0,
			      "settings: a spin box's arrows are visible",
			      QString("%1 on %2 = %3:1")
				      .arg(mark.name(), field.name())
				      .arg(contrast(mark, field), 0, 'f', 1));
		}
	}

	// A TICKED BOX IS OUR GREEN. Not "has a tick in it": the tick was OBS's
	// asset and there is no way to hand a QSS sub-control a mark we drew, so
	// the fill carries the state and what is checked is that the fill is ours.
	if (check(tickSeen && clearSeen,
		  "settings: there is a ticked box and a clear one")) {
		const auto boxOf = [&](QCheckBox *c) {
			const QPoint at = c->mapTo(dlg, QPoint(0, 0));
			return QRect(at.x() + 1, at.y() + (c->height() - 13) / 2,
				     13, 13);
		};
		const QColor on = dominant(tickShot, boxOf(tickSeen));
		check(sameColour(on, QColor(g_sc.pvw), 30),
		      "settings: a ticked box is the panel's green",
		      QString("%1, wanted %2").arg(on.name(), g_sc.pvw));
	}
	dlg->hide();
	delete dlg;
}

// ── THE ROW, SWEPT ───────────────────────────────────────────────────────
//
// One size is an anecdote. The report is about RESIZING — the row fills at some
// sizes and not at others — so it is swept, and the worst is what is checked.
enum class Drag { None, Monitor, Body };

void checkMonitorRowFills(const QString &label, Drag drag = Drag::None)
{
	struct S {
		int w, h;
	};
	const S sizes[] = {{1479, 894}, {1400, 900}, {1280, 800}, {1100, 700},
			   {1000, 980}, {900, 620},  {1600, 1000}, {1200, 1040},
			   {1920, 1040}, {820, 560}};
	int worstGap = -1;
	QString worstAt;
	for (const S &s : sizes) {
		auto *host = new QWidget();
		auto *hl = new QVBoxLayout(host);
		hl->setContentsMargins(0, 0, 0, 0);
		auto *w = new Mock();
		hl->addWidget(w);
		// Three passes: a mode change rewrites the floor, and the split
		// settles a pass behind the resize that caused it.
		const auto settle = [&](int cw, int ch) {
			for (int pass = 0; pass < 3; pass++) {
				host->resize(cw, ch);
				host->show();
				for (int i = 0; i < 3; i++) {
					QApplication::processEvents();
					QApplication::sendPostedEvents();
				}
			}
		};
		// AT ONE SIZE, DRAGGED, THEN RESIZED — the order an operator does
		// it in, and the state the reported panel was in. 1357x881 is
		// deliberately not one of the swept sizes, so every measurement
		// below follows a real resize.
		//
		// BOTH DIVIDERS, because they fail differently: the one between
		// the pictures and the list decides how much room the row has
		// (and the panel used to ignore it, which is why it drew 237 px
		// cameras where this drew 357), and the one inside the row
		// decides how that room is shared.
		if (drag != Drag::None) {
			settle(1357, 881);
			if (drag == Drag::Monitor) {
				check(w->monitorHandleUsable(),
				      label + ": the divider can be dragged");
				w->simulateMonitorDrag(
					w->monitorPane()->width() / 3);
			} else {
				w->simulateBodyDrag(200);
			}
			for (int i = 0; i < 3; i++) {
				QApplication::processEvents();
				QApplication::sendPostedEvents();
			}
		}
		settle(s.w, s.h);
		const RowFit f = rowFit(w);
		std::printf("   fit %4dx%-4d pane %4dx%-4d pictures %4dx%-4d"
			    "  gap %3d x %3d   tiles %3dx%-3d in %3dx%-3d\n",
			    s.w, s.h, f.paneW, f.paneH, f.coveredW, f.coveredH,
			    f.gapW, f.gapH, w->tileSize().width(),
			    w->tileSize().height(), w->tileBlockSize().width(),
			    w->tileBlockSize().height());
		// THE SMALLER OF THE TWO AXES, because only one of them can be
		// full: see the note on RowFit. Short on BOTH is the fault.
		// Asked of the CAMERA block, which is the half that was reported
		// — A can fill the row while the cameras beside it do not.
		if (f.tilePaneW > 0) {
			const int bound = qMin(f.tileGapW, f.tileGapH);
			if (bound > worstGap) {
				worstGap = bound;
				worstAt = QString("%1x%2: cameras %3x%4 in a "
						  "%5x%6 pane (short %7 x %8)")
						  .arg(s.w)
						  .arg(s.h)
						  .arg(f.tileCoveredW)
						  .arg(f.tileCoveredH)
						  .arg(f.tilePaneW)
						  .arg(f.tilePaneH)
						  .arg(f.tileGapW)
						  .arg(f.tileGapH);
			}
		}
		host->hide();
		delete host;
	}
	// TWELVE PIXELS, derived rather than chosen: the block is integer 16:9
	// arithmetic over up to four columns, so each column can lose a pixel to
	// the division, and the gaps between them are integers too. A BAND — what
	// the report was about — is nothing like it: the reported panel drew its
	// cameras 237x133 in a pane 722x224, short 485 px on one axis and 91 on
	// the other at the same time.
	check(worstGap <= 12, label + ": the cameras fill one axis of their pane",
	      worstAt.isEmpty() ? QStringLiteral("no camera block") : worstAt);
}

// ── A DIVIDER HAS TO GO BOTH WAYS ────────────────────────────────────────
//
// Once the operator has dragged the pictures/list divider, the room the
// monitoring row may have IS that pane's height — and the row's arithmetic
// writes a maximum back onto the same pane. Left that way the handle is
// one-way: the pane can be dragged shorter and never taller again, and each
// pass can only ratchet it further down. Nearly shipped; caught by asking the
// obvious question of it.
void checkDividerGoesBothWays(const QString &label)
{
	auto *host = new QWidget();
	auto *hl = new QVBoxLayout(host);
	hl->setContentsMargins(0, 0, 0, 0);
	auto *w = new Mock();
	hl->addWidget(w);
	const auto settle = [&]() {
		for (int i = 0; i < 6; i++) {
			QApplication::processEvents();
			QApplication::sendPostedEvents();
		}
	};
	for (int pass = 0; pass < 3; pass++) {
		host->resize(1400, 900);
		host->show();
		settle();
	}
	w->simulateBodyDrag(200);
	settle();
	const int shrunk = w->monitorPane()->height();
	w->simulateBodyDrag(420);
	settle();
	const int grown = w->monitorPane()->height();
	check(grown > shrunk + 100,
	      label + ": the pictures/list divider goes both ways",
	      QString("dragged to 200 gave %1, dragged back to 420 gave %2")
		      .arg(shrunk)
		      .arg(grown));
	host->hide();
	delete host;
}

// The whole host pass: a LIGHT panel inside a DARK OBS, which is the
// configuration every one of these faults was reported from.
void runHostChecks(QApplication &app, const QString &outDir)
{
	installObsHost(app, QDir::temp().filePath(QStringLiteral("mr-mock-host")));
	g_theme = ThemeChoice::Light;
	g_sc = schemeFor(g_theme, app.palette());
	refreshSheetAssets();
	g_tints = tintsFor(g_sc);

	// INSIDE A WINDOW, and this is the whole reason the mockup could not see
	// the fault. A top-level widget with no WA_StyledBackground still paints
	// its palette's Window brush — and QStyleSheetStyle copies a matched
	// background rule INTO that palette, so the panel came out the right
	// colour by a route the real dock does not have. The dock is a CHILD of
	// OBS's QDockWidget: a child that paints nothing simply shows its parent,
	// and its parent is OBS.
	//
	// Measured on the mockup before this window existed: `styled background:
	// no, palette window #efefef` — not painting, and passing anyway.
	auto *host = new QWidget();
	host->setAutoFillBackground(true);
	{
		QPalette hp = host->palette();
		hp.setColor(QPalette::Window, QColor(QStringLiteral("#1D1F26")));
		host->setPalette(hp);
	}
	auto *hl = new QVBoxLayout(host);
	hl->setContentsMargins(0, 0, 0, 0);
	auto *w = new Mock();
	hl->addWidget(w);
	for (int pass = 0; pass < 3; pass++) {
		host->resize(1500, 900);
		host->show();
		for (int i = 0; i < 3; i++) {
			QApplication::processEvents();
			QApplication::sendPostedEvents();
		}
	}
	const QImage full =
		host->grab().toImage().convertToFormat(QImage::Format_ARGB32);
	const QImage shot = full.copy(w->geometry());
	if (!outDir.isEmpty())
		shot.save(QDir(outDir).filePath(QStringLiteral("mock-host.png")));

	// Reported as a fact rather than inferred from the picture: "the panel is
	// light" and "the panel is painting" are two different claims, and for
	// three years only the first one was being checked.
	std::printf("   styled background: %s, palette window %s\n",
		    w->testAttribute(Qt::WA_StyledBackground) ? "yes" : "no",
		    qUtf8Printable(
			    w->palette().color(QPalette::Window).name()));
	checkPanelSurface(w, shot, QStringLiteral("host"));
	// The two keys whose mark sits on a signal fill, and the one that is a
	// mark and a word in the signal colour.
	checkMarkOnKey(w, QStringLiteral("playEvents"), QStringLiteral("play key"));
	checkMarkOnKey(w, QStringLiteral("skipNext"), QStringLiteral("skip key"));
	// These two are chrome-coloured keys with a SIGNAL label, so legibility
	// was never the problem — the mark simply belonged to a different key.
	checkMarkInk(w, QStringLiteral("rec"), QColor(g_sc.rec),
		     QStringLiteral("rec key"));
	checkMarkInk(w, QStringLiteral("markCancel"), QColor(g_sc.danger),
		     QStringLiteral("cancel key"));
	// The three that open a menu, which is where OBS printed a second one.
	checkOneMarkOnly(w, QStringLiteral("settings"), QStringLiteral("gear"),
			 outDir);
	checkOneMarkOnly(w, QStringLiteral("playOptions"),
			 QStringLiteral("play options"), outDir);
	checkOneMarkOnly(w, QStringLiteral("clipActions"),
			 QStringLiteral("clip actions"), outDir);

	checkRowTypeIsOneSize(w, QStringLiteral("list"));
	checkSettingsDialog(w, outDir);

	// ── AND NOW THE SAME PANEL AFTER A THEME CHANGE ──────────────────────
	//
	// Not a repeat: it is a DIFFERENT code path, and it is the one three
	// reports came from. Everything above measured a panel whose colours
	// were set once, in the constructor, before a child existed. This
	// measures one that was up and running when they changed.
	//
	// Light → contrast → light, because the fault reported was "it comes
	// back after a round trip": a state that only settles on the second
	// application is a state the first application did not produce.
	const auto measurePlayKey = [&]() {
		QAbstractButton *play = keyById(w, QStringLiteral("playEvents"));
		QAbstractButton *step = keyById(w, QStringLiteral("stepFwd"));
		return play && step ? QPair<int, int>(play->height(), step->height())
				    : QPair<int, int>(0, 0);
	};
	const QPair<int, int> before = measurePlayKey();
	check(before.first >= before.second * 2,
	      "theme: the play key is two rows before",
	      QString("%1 px against %2").arg(before.first).arg(before.second));

	w->retheme(ThemeChoice::HighContrast, app.palette());
	w->retheme(ThemeChoice::Light, app.palette());
	for (int i = 0; i < 4; i++) {
		QApplication::processEvents();
		QApplication::sendPostedEvents();
	}

	const QPair<int, int> after = measurePlayKey();
	// THE ONE THE OPERATOR REPORTED: it is two rows at startup and one after
	// a theme change. A fixed height is not supposed to move, so the fault is
	// in something that re-runs the section's layout with the folded shape.
	check(after.first >= after.second * 2,
	      "theme: ...and still two rows after",
	      QString("%1 px against %2 (was %3)")
		      .arg(after.first)
		      .arg(after.second)
		      .arg(before.first));

	const QImage reshot =
		host->grab().toImage().convertToFormat(QImage::Format_ARGB32);
	if (!outDir.isEmpty())
		reshot.copy(w->geometry())
			.save(QDir(outDir).filePath(
				QStringLiteral("mock-host-rethemed.png")));
	checkPanelSurface(w, reshot.copy(w->geometry()),
			  QStringLiteral("theme"));
	checkMarkOnKey(w, QStringLiteral("playEvents"),
		       QStringLiteral("theme: play key"));
	checkRowTypeIsOneSize(w, QStringLiteral("theme"));

	// The two popups, opened for real. A combo's list and a menu are the two
	// kinds of window this panel puts on top of itself, and neither had ever
	// been rendered by anything.
	{
		QDialog *dlg = buildSettingsMock(w);
		dlg->resize(820, 560);
		dlg->show();
		auto *nav = dlg->findChild<QListWidget *>(
			QStringLiteral("mrSettingsNav"));
		if (nav)
			nav->setCurrentRow(nav->count() - 2); // Interfaccia
		for (int i = 0; i < 4; i++) {
			QApplication::processEvents();
			QApplication::sendPostedEvents();
		}
		QComboBox *combo = nullptr;
		for (QComboBox *c : dlg->findChildren<QComboBox *>())
			if (c->isVisible() && !combo)
				combo = c;
		if (combo) {
			combo->showPopup();
			for (int i = 0; i < 4; i++) {
				QApplication::processEvents();
				QApplication::sendPostedEvents();
			}
			QVector<QPair<QRect, bool>> rows;
			for (int i = 0; i < combo->count(); i++)
				rows << qMakePair(
					combo->view()->visualRect(
						combo->model()->index(i, 0)),
					true);
			checkPopupReadable(combo->view(), rows,
					   QStringLiteral("combo list"), outDir,
					   QStringLiteral("combo"));
			combo->hidePopup();
		} else {
			check(false, "combo list: there is one to open");
		}
		dlg->hide();
		delete dlg;
	}
	{
		QAbstractButton *gear = keyById(w, QStringLiteral("settings"));
		QMenu *menu = gear ? gear->findChild<QMenu *>() : nullptr;
		if (menu) {
			menu->popup(QPoint(80, 80));
			for (int i = 0; i < 4; i++) {
				QApplication::processEvents();
				QApplication::sendPostedEvents();
			}
			checkPopupReadable(menu, menuRows(menu),
					   QStringLiteral("menu"), outDir,
					   QStringLiteral("menu"));
			menu->hide();
		} else {
			check(false, "menu: there is one to open");
		}
	}

	host->hide();
	delete host;
}

int runChecks(QPalette pal, QApplication &app, const QString &outDir)
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
		checkMonitorsGiveRoom(w, label);
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
	checkMonitorRowFills(QStringLiteral("monitors"));
	checkMonitorRowFills(QStringLiteral("monitors, row divider dragged"),
			     Drag::Monitor);
	checkMonitorRowFills(QStringLiteral("monitors, list divider dragged"),
			     Drag::Body);
	checkDividerGoesBothWays(QStringLiteral("monitors"));

	// LAST, because it replaces the application palette and style sheet for
	// the rest of the process: from here on the panel is a LIGHT one sitting
	// in a DARK OBS, which is where every fault in the report was seen.
	runHostChecks(app, outDir);

	std::printf("\n%s  (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
	return g_fail ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QStringList args = app.arguments();
	// THE HOST FIRST, because it replaces the application palette and the
	// panel's scheme is derived from it. Done after g_sc was computed it
	// would style the panel against a palette nothing on screen is wearing.
	if (args.contains(QStringLiteral("--host=obs")))
		installObsHost(app, QDir::temp().filePath(
					    QStringLiteral("mr-mock-host")));
	for (const QString &a : args) {
		if (a.startsWith(QStringLiteral("--cams=")))
			g_cams = qBound(1, a.mid(7).toInt(), 8);
		if (a == QStringLiteral("--nob"))
			g_haveB = false;
		// --size=WxH, repeatable: the sizes an operator's dock actually
		// has, rather than the seven this file guessed at. A flag and not
		// a positional argument, so it composes with --cams and --nob -
		// which is how the panel is judged at the CONFIGURATION it will be
		// used in instead of at the one that happens to be the default.
		if (a.startsWith(QStringLiteral("--size="))) {
			const QStringList wh =
				a.mid(7).split(QLatin1Char('x'));
			if (wh.size() == 2 && wh[0].toInt() > 0 &&
			    wh[1].toInt() > 0)
				g_sizes << QPair<int, int>(wh[0].toInt(),
							   wh[1].toInt());
		}
	}
	if (args.contains(QStringLiteral("--theme=obs")))
		g_theme = ThemeChoice::FollowObs;
	else if (args.contains(QStringLiteral("--theme=contrast")))
		g_theme = ThemeChoice::HighContrast;
	// OUR light theme, which is a different thing from "OBS is light" below:
	// this is the one the operator picks in Settings ▸ Interfaccia, and with
	// --host=obs it is the reported configuration exactly — a light panel in
	// a dark OBS.
	else if (args.contains(QStringLiteral("--theme=ourlight")))
		g_theme = ThemeChoice::Light;
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
	refreshSheetAssets();
	g_tints = tintsFor(g_sc);

	if (args.contains(QStringLiteral("--check"))) {
		QPalette lp = app.palette();
		lp.setColor(QPalette::Window, QColor("#efefef"));
		lp.setColor(QPalette::WindowText, QColor("#101010"));
		lp.setColor(QPalette::Base, QColor("#ffffff"));
		lp.setColor(QPalette::Highlight, QColor("#2f6fd0"));
		lp.setColor(QPalette::HighlightedText, QColor("#ffffff"));
		// The pictures the host pass reads back are worth keeping when a
		// folder was named: a failed pixel check is answered by looking.
		const QString shots = args.size() > 1 && !args[1].startsWith("--")
					      ? args[1]
					      : QString();
		if (!shots.isEmpty())
			QDir().mkpath(shots);
		return runChecks(lp, app, shots);
	}

	// The settings dialog on its own, which is the half of the panel no
	// resize sweep has ever rendered.
	if (args.contains(QStringLiteral("--settings"))) {
		auto *host = new Mock();
		QDialog *dlg = buildSettingsMock(host);
		dlg->resize(820, 560);
		dlg->show();
		if (args.contains(QStringLiteral("--show")))
			return app.exec();
		for (int i = 0; i < 6; i++) {
			QApplication::processEvents();
			QApplication::sendPostedEvents();
		}
		const QString outDir = args.size() > 1 && !args[1].startsWith("--")
					       ? args[1]
					       : QStringLiteral(".");
		QDir().mkpath(outDir);
		dlg->grab().save(
			QDir(outDir).filePath(QStringLiteral("mock-settings.png")));
		std::printf("wrote mock-settings.png\n");
		return 0;
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
	static QVector<QByteArray> names;
	if (!g_sizes.isEmpty()) {
		sizes.clear();
		for (const auto &s : g_sizes) {
			names << QString("at-%1x%2").arg(s.first).arg(s.second)
					 .toUtf8();
			sizes << Size{names.last().constData(), s.first,
				      s.second};
		}
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

// A Q_OBJECT in a single-file program: moc writes main.moc and it is included
// here. See the macro on Mock for why that class needs one at all.
#include "main.moc"
