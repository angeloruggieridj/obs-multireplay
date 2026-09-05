/*
obs-multireplay — MultiReplayDock: building the toolbar, the transport and the event table
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Split out of multireplay-dock.cpp (pure move, no behaviour change): the
one-time widget construction for the toolbar, the preview pane, the
multiview tiles, the bay selector, the control-strip sections (marks, the
transport, REC, speed, export) and the event table used to sit in the same
10k+ line file as the poll loop and the Settings dialog. See CLAUDE.md's
§4.2 for why.
*/

#include "multireplay-dock.hpp"
#include "angle-channels.hpp"
#include "dock-internal.hpp"
#include "dock-layout.hpp"
#include "error-locale.hpp"
#include "dock-style.hpp"
#include "dock-assets.hpp"
#include "dock-icons.hpp"
#include "qt-display.hpp"
#include "branch-output-install.hpp"
#include "camera-dedup.hpp"
#include "replay-core.hpp"
#include "updater.hpp"
#include "event-store.hpp"
#include "health.hpp"
#include "packet-tap.hpp"
#include "playback-coordinator.hpp"
#include "export.hpp"
#include "replay-channel.hpp"
#include "segment-index.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h> // obs_frontend_get_scenes (output-scene picker)
#include <util/platform.h>    // os_gettime_ns (arm watchdog deadline)

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QFontInfo>
#include <QSplitterHandle>
#include <QAbstractButton>
#include <QDateTime>
#include <QPushButton>
#include <QTabBar>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QItemSelectionModel>
#include <QHeaderView>
#include <QButtonGroup>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QDir>
#include <QGroupBox>
#include <QFrame>
#include <QListWidget>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QMessageBox>
#include <QStandardPaths>
#include <QProgressBar>
#include <QDesktopServices>
#include <QUrl>
#include <QDockWidget>
#include <QSizePolicy>
#include <QStyle>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QInputDialog>
#include <QMenu>
#include <QAction>
#include <QClipboard>
#include <QApplication>

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdlib>
#include <cstring>

namespace multireplay {
// ---------------------------------------------------------------------------
// Toolbar: search · Live, then the list tabs  (the controller's list header)
//
// TWO rows, in this order, because that is the order the reference panel has:
// the search box and the Live key on one line, the twenty list tabs on the line
// under it, and the table under those. The tabs are the widest thing here and
// they scroll; sharing a line with them is what squeezed the search box down to
// a slot too narrow to read what had been typed into it.
//
// The whole block sits UNDER the pictures (see the constructor).
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildToolbar()
{
	auto *box = new QWidget(this);
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(2);

	auto *topRow = new QWidget(box);
	auto *h = new QHBoxLayout(topRow);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(5);

	projectLbl_ = new QLabel(box);
	projectLbl_->setObjectName("mrMuted");
	// A PROPERTY, not a per-widget style sheet: see the mrProject rule in
	// dock-style.hpp. setStyleSheet() on one widget opens a separate style
	// context and forces a re-polish of its subtree — measured elsewhere on
	// this panel at >100 ms — and it does not follow a theme change on its
	// own, which is why this used to be set twice, here and in applyTheme().
	projectLbl_->setProperty("mrProject", true);
	projectLbl_->hide();
	h->addWidget(projectLbl_);
	// Search and Live sit in the MIDDLE of their row, as they do on the
	// reference panel: the operator's eye comes down off the picture into the
	// centre of the panel, not into a corner.
	h->addStretch(1);

	// A DRAWN MAGNIFIER, not the emoji. U+1F50D carries
	// Emoji_Presentation=Yes, so Windows painted it in full colour from Segoe
	// UI Emoji — a bright blue blob beside a grey search box, on a panel
	// whose every other mark is a grey line.
	// A LABEL, so restyleIcons cannot reach it — it only walks buttons. Kept
	// so applyTheme can redraw it: it is the one mark on this panel that is
	// not on a key, and it was the one that stayed the old grey after a theme
	// change.
	searchIcon_ = new QLabel(box);
	h->addWidget(searchIcon_);
	restyleSearchIcon();
	search_ = new QLineEdit(box);
	search_->setPlaceholderText(obs_module_text("Dock.Search"));
	search_->setClearButtonEnabled(true);
	// IN EM, not fixed pixels: 190/90 px was sized for OBS's default font.
	// At Settings > Appearance font-scale 125-150% the same box stayed 190
	// px while the placeholder and the clear button grew, so the text was
	// clipped and the button sat outside the visible field. The clock
	// label's kClockW already derives its width from fontMetrics for the
	// same reason; the search box did not.
	const int searchEm = fontMetrics().horizontalAdvance(QLatin1Char('M'));
	search_->setMaximumWidth(qMax(140, 13 * searchEm));
	search_->setMinimumWidth(qMax(80, 7 * searchEm));
	connect(search_, &QLineEdit::textChanged, this,
		[this](const QString &) { refreshEvents(); });
	h->addWidget(search_, 0);

	// the reference controller's Live button, in the reference controller's place and the reference controller's colour: red means the
	// marks land where the action is happening, off means they land where the
	// position bar is parked.
	liveBtn_ = iconTextBtn(Icon::Live, obs_module_text("Dock.LiveMode"),
			       "live", box, "mrLive", 12);
	liveBtn_->setObjectName("mrLive");
	// UPPER CASE, in code: Qt style sheets have no text-transform, and the
	// letter-spacing this key carries made the mixed-case word read as "LIve".
	liveBtn_->setText(QString::fromUtf8(obs_module_text("Dock.LiveMode"))
				  .toUpper());
	// ...AND ITS MARK STAYS WHITE WHEN THE KEY IS LIT. The lit tint is the
	// panel's green, which on a red key is a green dot inside a red rectangle:
	// two signals arguing in one control. White is what the word beside it is.
	setKeyIconRole(liveBtn_, Icon::Live, IconRole::LitWhite, tintsFor(sc()),
		       12);
	liveBtn_->setCheckable(true);
	liveBtn_->setCursor(Qt::PointingHandCursor);
	liveBtn_->setToolTip(obs_module_text("Dock.LiveModeHint"));
	liveBtn_->setChecked(EventStore::instance().liveMode());
	connect(liveBtn_, &QPushButton::toggled, this, [this](bool on) {
		EventStore::instance().setLiveMode(on);
		if (!on)
			return;
		// LIVE IS THE MODE THE WHOLE PANEL IS IN, so pressing it puts the
		// whole panel back on the live edge — the replay is dropped, the
		// transport follows the front again, and the angle boxes go back
		// to mirroring their cameras in real time. It used to change only
		// where a mark lands, which left the one key labelled "Live"
		// unable to get the operator back to live.
		//
		// Turning it OFF does not do the opposite: marking at the bar is
		// a choice about marking, and it must not stop a replay.
		pc().stopEvents();
		ReplayCore::instance().setFollowLive(true);
		clearFreeReview();
	});
	h->addWidget(liveBtn_);

	// the reference controller's Monitors key, in the reference controller's place: right of Live. It takes the whole
	// monitoring block away — the two replay decks, the camera previews AND the
	// green strip under them — because that block is what costs GPU, and an
	// operator working from the list on a thin machine should be able to put it
	// down. One key for the whole block: hiding the pictures and leaving the
	// strip floating under nothing would read as a bug.
	// A STATE, NOT A SIGNAL. It wore the Live key's role, so "the pictures are
	// on" — the resting state of the panel — lit up in the same red as REC.
	// Red has one meaning here and this is not it.
	monitorsBtn_ = iconTextBtn(Icon::Monitors, obs_module_text("Dock.Monitors"),
				   "monitors", box, "mrToggle", 12);
	monitorsBtn_->setCheckable(true);
	monitorsBtn_->setChecked(true);
	monitorsBtn_->setCursor(Qt::PointingHandCursor);
	monitorsBtn_->setToolTip(obs_module_text("Dock.MonitorsHint"));
	connect(monitorsBtn_, &QPushButton::toggled, this,
		[this](bool on) { applyMonitorsVisible(on); });

	// ⛶ — the panel to the whole screen. ONLY WHEN IT FLOATS, and hidden (not
	// disabled) otherwise: see fullScreenBtn_ in the header for why the key is
	// absent rather than dead, and why this makes a window state change instead
	// of a new window. refreshFullScreenKey() decides whether it is on screen;
	// it starts hidden because a dock is docked until somebody pulls it out.
	fullScreenBtn_ = iconBtn(Icon::FullScreen, "fullscreen",
				 obs_module_text("Dock.FullScreenHint"), box,
				 "mrToggle");
	fullScreenBtn_->setCheckable(true);
	fullScreenBtn_->setCursor(Qt::PointingHandCursor);
	fullScreenBtn_->setToolTip(obs_module_text("Dock.FullScreenHint"));
	fullScreenBtn_->hide();
	connect(fullScreenBtn_, &QPushButton::toggled, this, [this](bool on) {
		setPanelFullScreen(on);
		// The window may have refused (it was re-docked between the paint
		// and the click), so the key is told what happened rather than
		// trusted to have made it happen.
		refreshFullScreenKey();
	});
	// THE GEAR SITS WITH THE OTHER PANEL-WIDE KEYS. What it opens is the
	// configuration of the whole panel — the project, the cameras, the tags,
	// the theme — and inside the record section it read as part of arming a
	// take. It goes beside the full-screen key because those two are the pair
	// that are about the panel itself rather than about the replay.
	// ...AND THE THREE OF THEM SIT AT THE FAR END. Monitors, the gear and
	// full screen are about the PANEL; search and Live are about the take.
	// Packed together in the middle the six keys read as one group and the
	// operator had to remember which three were which - so the stretch goes
	// between them, and the panel keys end flush with the panel's edge.
	h->addStretch(1);
	h->addWidget(monitorsBtn_);
	h->addWidget(buildGearMenu());
	h->addWidget(fullScreenBtn_);
	v->addWidget(topRow);

	// The 20 lists as TABS, not a dropdown. the reference controller shows them all at once and
	// the operator jumps between them mid-match without opening anything; a
	// combo hides nineteen of them behind a click. Named lists show the name.
	listTabs_ = new QTabBar(box);
	listTabs_->setObjectName("mrListTabs");
	listTabs_->setDrawBase(false);
	listTabs_->setExpanding(false);
	// SCROLL, never elide. A named list is named so it can be read: "PAR…" is
	// the number it replaced, minus the information. With elision off every tab
	// is drawn at its natural width and the bar scrolls when they do not all
	// fit — and an operator who does not want to scroll reduces the number of
	// lists (Config.eventListCount), which is the setting that actually gives
	// each name room.
	listTabs_->setUsesScrollButtons(true);
	listTabs_->setElideMode(Qt::ElideNone);
	listTabs_->setFocusPolicy(Qt::NoFocus);
	// Slightly smaller than the dock's font, and set on the WIDGET rather
	// than in the stylesheet: this is the font the tabs are measured AND
	// painted with, so "the tab is at least as wide as its own name" is a
	// question that can be answered from outside (the gate asks it).
	{
		QFont tf = listTabs_->font();
		if (tf.pointSizeF() > 0)
			tf.setPointSizeF(std::max(7.0, tf.pointSizeF() * 0.9));
		else if (tf.pixelSize() > 0)
			tf.setPixelSize(std::max(9, (int)(tf.pixelSize() * 0.9)));
		listTabs_->setFont(tf);
	}
	for (int i = 1; i <= kEventLists; i++)
		listTabs_->addTab(QString::number(i));
	refreshListNames();
	listTabs_->setCurrentIndex(EventStore::instance().selectedList() - 1);
	connect(listTabs_, &QTabBar::currentChanged, this, [this](int idx) {
		if (idx < 0)
			return;
		EventStore::instance().selectList(idx + 1);
		refreshEvents();
	});
	// The tab bar owns its own row now, so it gets the whole width: with the
	// names on, the scroll buttons only appear when twenty NAMED lists really
	// do not fit, instead of as soon as the search box took its share.
	v->addWidget(listTabs_);

	return box;
}

// ---------------------------------------------------------------------------
// Single A preview + its green channel strip
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildPreview()
{
	auto *box = new QWidget(this);
	box->setMinimumHeight(96);
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(0);

	// the reference controller: the replay outputs big, every camera small
	// beside them. The operator watches the big ones and keeps the others in
	// the corner of his eye, which is the whole reason the strip is here
	// rather than behind a button.
	//
	// A SPLITTER, not a fixed division, because how much room the cameras are
	// worth against the bays is the operator's call and nobody else's. In a
	// wide panel that divider is a WIDTH — and since every box keeps the
	// canvas's ratio (see AspectBox), the same drag changes their HEIGHT too,
	// which is the only control over the size of the pictures there has ever
	// been. Where he leaves it is remembered per arrangement.
	//
	// NOTHING IN HERE IS EVER RE-PARENTED. QSplitter::setOrientation does not
	// touch its children, and A and B stay in one grid whichever way the panel
	// is turned — because moving a widget from one LAYOUT to another destroys
	// the native window of every OBSQTDisplay underneath it and strands the
	// obs_display presenting into it.
	bays_ = new QWidget(box);
	auto *bg = new QGridLayout(bays_);
	bg->setContentsMargins(0, 0, 0, 0);
	bg->setSpacing(3);

	// TWO outputs side by side, in every arrangement. They are two bays of one
	// deck: stacking them would make the pair read as a hierarchy, and it is
	// the one relationship on this panel that is exactly equal. One box for
	// both would mean the operator has to remember which he is looking at —
	// and the point of a second bay is having the next replay ready while the
	// first is on air, which cannot be done if only one can be seen.
	aBox_ = new AspectBox(bays_);
	{
		displayA_ = new OBSQTDisplay(aBox_);
		displayA_->setRenderCallback(&MultiReplayDock::drawChannelA, this);
		labelA_ = new QLabel(QStringLiteral("A"), aBox_);
		labelA_->setObjectName("mrChanTag");
		labelA_->setProperty("chan", QStringLiteral("A"));
		labelA_->setProperty("active", true); // A is where the panel starts
		labelA_->setAlignment(Qt::AlignCenter);
		aBox_->setContents(displayA_, labelA_);
	}
	bBox_ = new AspectBox(bays_);
	{
		displayB_ = new OBSQTDisplay(bBox_);
		displayB_->setRenderCallback(&MultiReplayDock::drawChannelB, this);
		labelB_ = new QLabel(QStringLiteral("B"), bBox_);
		labelB_->setObjectName("mrChanTag");
		labelB_->setProperty("chan", QStringLiteral("B"));
		labelB_->setProperty("active", false);
		labelB_->setAlignment(Qt::AlignCenter);
		bBox_->setContents(displayB_, labelB_);
	}
	bg->addWidget(aBox_, 0, 0);
	bg->addWidget(bBox_, 0, 1);
	bg->setColumnStretch(0, 1);
	bg->setColumnStretch(1, 1);

	buildMultiview();

	monitorSplit_ = new QSplitter(Qt::Horizontal, box);
	monitorSplit_->setChildrenCollapsible(false);
	monitorSplit_->setHandleWidth(5);
	monitorSplit_->addWidget(bays_);
	monitorSplit_->addWidget(multiviewBox_);
	// ── DRAGGING THIS DIVIDER IS THE OPERATOR'S, AND IT HAS TO FILL ──────
	//
	// It was briefly disabled, on the argument that the row is one piece of
	// algebra — A, B and every tile row 16:9 at ONE height, solved so the row
	// is exactly as wide as the pane — and that any other split can only
	// letterbox. The argument is right about the ALGEBRA and wrong about the
	// panel: deciding how much of the row goes to the bays and how much to
	// the cameras is a real thing to want, and taking the handle away is not
	// an answer to it.
	//
	// What makes a drag fill instead of band is honouring it on BOTH sides:
	// the bays take the width they were given and the cameras take theirs,
	// each at 16:9 and each at its own height, and the row is as tall as the
	// taller of the two. That is what applyPreviewAspect does below when
	// monitorSplitChosen() — and it is what was missing, because the width
	// was honoured for the cameras and not for the bays.
	connect(monitorSplit_, &QSplitter::splitterMoved, this, [this](int, int) {
		userMonitorSplit_[(int)panelMode_] = true;
		savedMonitorSplit_[(int)panelMode_] = monitorSplit_->saveState();
		applyPreviewAspect();
	});
	monitorsRow_ = monitorSplit_; // the Monitors key hides this whole block
	v->addWidget(monitorSplit_, 1);

	// (The green channel strip that used to sit here is gone. It said which
	// list, which clip of how many, how much was left, the event id, the two
	// offsets, the timecode and the speed — three lines and 44 px of panel,
	// nearly all of it a second copy of what the on-air band and the position
	// bar already say. What was only said there — the notice answering a key
	// the operator just pressed — is on the status line now.)
	monitorsStrip_ = nullptr;

	box->setObjectName(QStringLiteral("mrPreviewPane"));
	previewPane_ = box; // the splitter child the Monitors key gives back
	return box;
}

// ---------------------------------------------------------------------------
// Multiview — one small preview per configured angle, plus the replay
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildMultiview()
{
	static_assert(kMaxPreviewTiles == kMaxCameras + 1,
		      "one tile per camera plus the replay tile");

	multiviewBox_ = new QWidget(this);
	multiviewGrid_ = new QGridLayout(multiviewBox_);
	multiviewGrid_->setContentsMargins(0, 0, 0, 0);
	multiviewGrid_->setSpacing(2);

	// Every tile is built ONCE, here, and afterwards only shown, hidden and
	// moved between cells of this same grid. A tile is never re-parented: Qt
	// answers a re-parent by destroying the widget's native window, which
	// strands the obs_display bound to it (see qt-display.hpp) — the one
	// failure mode this whole file has to avoid.
	for (int i = 0; i < kMaxPreviewTiles; i++) {
		PreviewTile &t = tiles_[i];
		t.cam0 = (i < kMaxCameras) ? i : -1;

		t.box = new AspectBox(multiviewBox_);
		t.box->setObjectName(QStringLiteral("mrTile"));
		// CLICKING THE PICTURE IS HOW AN ANGLE IS CHOSEN, and since the
		// camera key matrix was taken off the panel it is the ONLY way
		// with a mouse. It is also the shortest path there is from "that
		// camera has it" to "put that camera up": the key said "C5", the
		// picture says what C5 is pointing at. The cursor is the only
		// thing that advertises it, so it is not optional.
		t.box->setCursor(Qt::PointingHandCursor);

		t.display = new OBSQTDisplay(t.box);
		// Small on purpose: a tile costs a present() of its own on the
		// shared graphics thread, and the operator is checking framing
		// here, not focus.
		t.display->setMinimumSize(60, 34);
		tileCtx_[i].dock = this;
		tileCtx_[i].slot = i;
		t.display->setRenderCallback(&MultiReplayDock::drawTile,
					     &tileCtx_[i]);
		t.display->installEventFilter(this);

		t.caption = new QLabel(t.box);
		t.caption->setObjectName(QStringLiteral("mrTileCap"));
		t.caption->setTextFormat(Qt::PlainText);
		t.caption->setAlignment(Qt::AlignCenter);
		t.caption->installEventFilter(this);
		// The box keeps the canvas's ratio and places these two itself;
		// there is no layout to negotiate with (see AspectBox).
		t.box->setContents(t.display, t.caption);

		t.box->setVisible(false); // rebuildMultiview() decides
		multiviewGrid_->addWidget(t.box, i / 2, i % 2);
	}

	rebuildMultiview();
	return multiviewBox_;
}

// ---------------------------------------------------------------------------
// WHICH BAY the keys drive. That is all this section is now.
// ---------------------------------------------------------------------------
//
// IT USED TO CARRY THE CAMERA MATRIX AS WELL — two rows of eight keys, sixteen
// in all, the widest thing on the panel by a long way and the single reason it
// could not be docked down the side of an OBS window. Measured: taking it off
// brought the panel's minimum width from 336 px to 268.
//
// The angles are chosen by CLICKING THE PICTURE now. That is not a substitute,
// it is the better control: the key said "C5", the picture says what C5 is
// pointing at — and it is where the operator is already looking. The tally that
// used to be on the keys is on the pictures, which is where the reference
// controller has always put it.
//
// With one bay this section is not built at all: not disabled, absent. Three
// greyed keys on a single-bay rig are three keys the eye has to rule out every
// time it reads the strip.
KeyBlock *MultiReplayDock::buildAngleMatrix()
{
	// NAMED, because the three keys in it do not name themselves. Every other
	// section is told apart by its own keys - a record dot, In and Out, the
	// transport glyphs, the percentages - but "A|B  A  B" beside an arrow is
	// a question the operator has to answer from memory: which of the two
	// bays do the keys drive. The caption is the answer.
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneOutput"), this);
	channelBWidgets_.clear();
	// The CAPTION collapses with channel B through KeyBlock::
	// setSectionVisible() (see applyChannelBVisibility()), NOT through
	// channelBWidgets_ like the keys below it: a caption hidden by poking
	// captionLabel()->setVisible(false) from outside only stayed hidden
	// until the next time anything remeasured this section — which
	// applyChannelBVisibility() itself triggers a few lines after hiding
	// it — because KeyBlock::apply() used to decide the caption's
	// visibility unconditionally on every pass. setSectionVisible() is
	// what apply() actually consults now.
	QWidget *sel = buildChannelRow();
	QVector<Cell> row;
	row << Cell(sel, 1, false);
	// The gap before ⇄ is deliberate — see buildChannelRow() for why it
	// sits apart from the selector instead of the fourth button of one.
	row << Cell(nullptr, 1) << Cell(swapBtn_, 1, false);
	blk->setShapes({row}, {row});
	angleBlock_ = blk;
	return blk;
}

// The A|B / A / B selector and the swap.
//
// They are drawn as one segmented control rather than as three loose keys: the
// question they answer — which bay do these keys drive — is one question. The
// swap skips a column: ⇄ is not a fourth
// mode, and pressed by mistake it puts the wrong clip on air.
QWidget *MultiReplayDock::buildChannelRow()
{
	auto *sel = new QWidget(this);
	auto *h = new QHBoxLayout(sel);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(3);

	chanSel_ = new QButtonGroup(this);
	chanSel_->setExclusive(true);
	// A↔B, not A|B: it says what the mode DOES — a command goes to both bays
	// — rather than naming two things with a bar between them.
	const std::pair<const char *, int> chanChoices[] = {
		{"A↔B", 2}, {"A", 0}, {"B", 1}};
	for (const auto &[label, code] : chanChoices) {
		auto *b = new QPushButton(QString::fromUtf8(label), sel);
		b->setObjectName("mrChanSel");
		b->setCheckable(true);
		b->setChecked(code == 0); // A, as it has always been
		b->setFixedSize(kChanKeyWidth, kKeyH);
		b->setCursor(Qt::PointingHandCursor);
		setKeyId(b, QString("bay%1").arg(code));
		chanSel_->addButton(b, code);
		h->addWidget(b);
	}
	connect(chanSel_, &QButtonGroup::idClicked, this, [this](int code) {
		setActiveChannel(code == 1 ? Which::B : Which::A, code == 2);
	});
	// sectionKeyH(), not kKeyH: `sel` is a plain QWidget, not a button, so
	// KeyBlock::apply()'s per-button pin (which reads the gallery-scaled
	// height already) never touches it — this call is the only place its
	// height comes from.
	sel->setFixedHeight(sectionKeyH());
	channelBWidgets_ << sel;

	swapBtn_ = iconBtn(Icon::Swap, "swapBays",
			   obs_module_text("Dock.SwapChannels"), this,
			   "mrChanSel");
	swapBtn_->setFixedSize(kChanKeyWidth, kKeyH);
	swapBtn_->setCursor(Qt::PointingHandCursor);
	connect(swapBtn_, &QPushButton::clicked, this,
		&MultiReplayDock::swapChannels);
	channelBWidgets_ << swapBtn_;
	return sel;
}

void MultiReplayDock::buildSpeedDial()
{
	// The dial and its readout. WIDE and under the transport keys (placed by
	// buildTransport): this is the control an operator reaches for most often
	// during a match, and it used to be the smallest thing on the panel —
	// 70 px wedged between the presets and the edge.
	speed_ = new QSlider(Qt::Horizontal, this);
	speed_->setObjectName("mrSpeed");
	// Up to 2×: the reference controller's variable speed is 0-100%, and its fast forward is the
	// same control pushed past 1×.
	speed_->setRange(5, 200);
	speed_->setValue(100);
	speed_->setMinimumWidth(220);
	speed_->setMinimumHeight(26);
	speed_->setTickPosition(QSlider::TicksBelow);
	speed_->setTickInterval(25);
	speed_->setToolTip(obs_module_text("Dock.SpeedSliderHint"));
	speed_->setCursor(Qt::PointingHandCursor);

	speedLbl_ = new QLabel(QStringLiteral("1.00\xc3\x97"), this);
	speedLbl_->setObjectName("mrTimecode");
	speedLbl_->setFont(QFont(monoFamily()));
	speedLbl_->setMinimumWidth(42);
	speedLbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	// LIVE, while the thumb moves. The speed of a replay is judged by
	// watching the picture, so applying it only on release meant aiming —
	// and applyReplaySpeed() now re-speeds the clip on air instead of
	// restarting it, which is what makes a dragged dial usable at all.
	connect(speed_, &QSlider::valueChanged, this, [this](int val) {
		speedLbl_->setText(QString::asprintf("%.2f\xc3\x97", val / 100.0));
		applyReplaySpeed(val);
	});
}

// ---------------------------------------------------------------------------
// Transport — the reference controller's centre group: ⏸ ◀ ↺ [Play Events ▾] NOW ⏮ ⏭ Loop ♫
// ---------------------------------------------------------------------------

KeyBlock *MultiReplayDock::buildTransport()
{
	// ONE ROW, in the reference panel's order:
	//
	//   play  reverse  last  [Riproduci eventi] menu  step-  step+  NOW
	//   Loop  music  In output
	//
	// It was three rows of its own for a while, which made this section twice
	// as tall as the ones beside it and pushed the whole strip down the panel.
	// The reference keeps its transport on one line, and so does this.
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneTransport"), this);

	// THREE ROWS, ordered by what an operator reaches for first:
	//
	//   Riproduci eventi ▾   NOW      ← put a replay on air / come back to live
	//   ⏸  ◀  ↺  ⏮  ⏭                 ← drive the clip that is on air
	//   Loop  ♫  In output            ← the modes the two rows above run under
	//
	// One row of eleven keys made the two that matter most — play the events,
	// and get back to the live edge — the same size and the same weight as a
	// frame step, so the eye had to read the whole strip to find them. Rows
	// give them a place: the top one is where the hand goes without looking.
	//
	// The grid is five columns wide, and the wide keys SPAN it, so every key
	// stands on a column and the group reads as one block rather than three
	// rows of unrelated lengths.

	// ▶ U+25B6
	// ONE MARK FOR A KEY THAT IS BOTH. This is a play at rest and a pause
	// while a clip runs, and drawing it as one or the other made it look like
	// two different keys depending on when you glanced at it.
	playPauseBtn_ = iconBtn(Icon::Play, "playPause",
				obs_module_text("Dock.PlayPause"), this, "mrPlay");

	// Stop. It used to live two clicks deep in the ▾ menu, which was
	// survivable while every replay ended by itself at its OUT. A free review
	// does not: it runs until it is stopped, so the way to stop it has to be a
	// key. And ▶ cannot be that key — while something plays it is a PAUSE,
	// which holds the picture instead of giving Program back.
	stopBtn_ = iconBtn(Icon::Stop, "stop", obs_module_text("Dock.Stop"), this);

	// The reverse-play key, and it works (v1.3). Nothing decodes backwards, so
	// this is a GOP cache shown newest-first; see reverse-plan.hpp.
	auto *revBtn = iconBtn(Icon::Reverse, "playReverse",
			       obs_module_text("Dock.PlayReverse"), this);
	connect(revBtn, &QPushButton::clicked, this,
		[this]() { playSelectedReverse(); });

	// "Instantly play last event".
	auto *lastBtn = iconBtn(Icon::PlayLast, "playLast",
				obs_module_text("Dock.PlayLast"), this);
	connect(lastBtn, &QPushButton::clicked, this, [this]() {
		std::string err;
		if (!pc().playLastEvent(
			    currentAngle1() - 1,
			    toOutputBtn_ && toOutputBtn_->isChecked(), err))
			showNotice(localizedError(err));
	});

	// ── PLAY: the biggest key on the panel ───────────────────────────────
	//
	// It is the one that takes the Program, so it is the one the eye should
	// land on without reading anything. It was a text button the same weight
	// as a frame step, in a row of eight, and the operator had to read the
	// strip to find it. Now it is a filled green rectangle two key-rows tall
	// carrying nothing but the play mark — and there are exactly two filled
	// keys on this panel, this and REC once it is armed.
	//
	// It must stay a plain QPushButton with no menu on it: QPushButton::setMenu
	// swallows click(), and both a hotkey and the automated gate reach this
	// key that way. The options live on the ▾ beside it, which is where the
	// reference controller keeps them too.
	auto *playSel = iconBtn(Icon::Play, "playEvents",
				obs_module_text("Dock.PlaySelected"), this,
				"mrAccent");
	// WHITE, because the key is a filled green rectangle. #mrAccent already
	// says `color: #ffffff` for the label; the mark is the rest of it, and a
	// style sheet cannot reach a pixmap — so on a light panel this was a dark
	// grey ▶ sitting on the one key that takes the Program.
	setKeyIconRole(playSel, Icon::Play, IconRole::OnSignal, tintsFor(sc()), 22);
	playSel->setMinimumWidth(64);
	connect(playSel, &QPushButton::clicked, this,
		&MultiReplayDock::playSelected);

	auto *more = new QToolButton(this);
	more->setObjectName("mrGear");
	// setKeyIcon, NOT setIcon: a mark handed straight to the widget carries no
	// record of which mark it is, so restyleIcons cannot find it and the three
	// menu keys kept the ink of the theme they were BUILT in for the rest of
	// the session. Invisible until the operator changes theme, which is the one
	// moment it is guaranteed to be looked at.
	setKeyIcon(more, Icon::Menu, tintsFor(sc()), 14);
	more->setCursor(Qt::PointingHandCursor);
	more->setToolTip(obs_module_text("Dock.PlayOptions"));
	setKeyId(more, QStringLiteral("playOptions"));
	{
		auto *menu = new QMenu(more);
		auto *actOut = menu->addAction(obs_module_text("Dock.PlayToOutput"));
		auto *actLast = menu->addAction(obs_module_text("Dock.PlayLast"));
		menu->addSeparator();
		auto *actStop = menu->addAction(obs_module_text("Dock.Stop"));
		popupOnClick(more, menu);
		connect(actOut, &QAction::triggered, this, [this]() {
			// Explicitly the EVENT, so an unmarked stretch armed on
			// the bar stops being what the play keys are about. This
			// entry is also the way back to the selected row when a
			// free review is armed and the main key is answering it.
			clearFreeReview();
			std::string err;
			if (!pc().playEvents(
				    selectedEventIds(), currentAngle1() - 1,
				    /*toOutput*/ true, err))
				showNotice(localizedError(err));
		});
		connect(actLast, &QAction::triggered, this, [this]() {
			std::string err;
			if (!pc().playLastEvent(
				    currentAngle1() - 1,
				    toOutputBtn_ && toOutputBtn_->isChecked(),
				    err))
				showNotice(localizedError(err));
		});
		connect(actStop, &QAction::triggered, this,
			[this]() { pc().stopEvents(); });

		// ALWAYS-PRESENT FALLBACK: the per-camera angle matrix is gone —
		// the mouse picks an angle by clicking a multiview tile — and
		// with Monitors off, showMultiview off, or a filmstrip too
		// narrow to show every configured camera, clicking a picture is
		// not an option at all. This menu is reachable regardless of
		// any of that, so it is where "change the angle with the
		// mouse" always has an answer. Rebuilt on every open
		// (aboutToShow), not once at construction, because a camera can
		// be renamed or added in Settings without this dock being
		// rebuilt.
		menu->addSeparator();
		auto *angleMenu = menu->addMenu(obs_module_text("Dock.Angle"));
		connect(angleMenu, &QMenu::aboutToShow, this, [this, angleMenu]() {
			angleMenu->clear();
			const Config cfg = ReplayCore::instance().getConfig();
			for (int i = 0; i < kNCams; i++) {
				if (cfg.cameras[i].sourceName.empty())
					continue;
				const std::string &dn =
					cfg.cameras[i].displayName;
				const QString label =
					QString("%1  %2")
						.arg(i + 1)
						.arg(dn.empty()
							     ? QString("C%1").arg(
								       i + 1)
							     : QString::fromStdString(
								       dn));
				QAction *act = angleMenu->addAction(label);
				connect(act, &QAction::triggered, this,
					[this, i]() { setAngle(i + 1); });
			}
		});
	}

	// NOW IS A DESTINATION, not a modifier: it drops the replay and puts the
	// operator back on the live edge, which during a match is the most
	// consequential key on this panel after REC and PLAY. It keeps the WORD —
	// there is no mark for "back to now" that anybody would read — and it gets
	// the whole width of the transport row under the keys, which is what
	// "first function" looks like on a key that is not a rectangle of colour.
	nowBtn_ = new QPushButton(QStringLiteral("NOW"), this);
	nowBtn_->setObjectName("mrNow");
	nowBtn_->setProperty("live", false);
	nowBtn_->setCursor(Qt::PointingHandCursor);
	nowBtn_->setToolTip(obs_module_text("Dock.JumpToNow"));
	setKeyId(nowBtn_, QStringLiteral("now"));
	nowBtn_->setMinimumWidth(38);

	// One frame back, one frame forward. The step BACK is not the forward one
	// with the sign changed — see stepFrameBackward.
	auto *stepBackBtn = iconBtn(Icon::StepBack, "stepBack",
				    obs_module_text("Dock.StepBack"), this);
	connect(stepBackBtn, &QPushButton::clicked, this,
		[this]() { stepFrameBackward(); });

	auto *stepBtn = iconBtn(Icon::StepFwd, "stepFwd",
				obs_module_text("Dock.StepFwd"), this);
	connect(stepBtn, &QPushButton::clicked, this,
		[this]() { stepFrameForward(); });

	// HELD DOWN, not tapped. Finding the right frame means passing it and coming
	// back, and that is work done with a key held while watching the picture —
	// so these two repeat. Nothing else in this row does: a repeating Play or a
	// repeating REC is an accident waiting for a heavy hand.
	//
	// The interval is 150 ms and not Qt's default 100: a step BACK decodes a
	// whole GOP (that is what makes reverse possible at all), which is ~100 ms on
	// an iGPU, and asking for the next one before the last has been served just
	// queues work the machine is already behind on.
	// (These two used to be Unicode glyphs, and U+23EE / U+23ED carry
	// Emoji_Presentation=Yes — so Windows painted the two least consequential
	// keys in the row in bright Segoe UI Emoji blue, on a panel where every
	// other key is a grey mark. useTextGlyph existed to force the font. They
	// are drawn now, at the one weight every other mark is drawn at, and the
	// problem cannot come back.)
	for (QPushButton *b : {stepBackBtn, stepBtn}) {
		b->setAutoRepeat(true);
		b->setAutoRepeatDelay(400);
		b->setAutoRepeatInterval(150);
	}

	loopBtn_ = statusToggle(Icon::Loop, obs_module_text("Dock.Loop"), "loop",
				obs_module_text("Dock.Loop"), this);
	connect(loopBtn_, &QPushButton::toggled, this,
		[this](bool on) { pc().setLoop(on); });

	musicBtn_ = statusToggle(Icon::Music, obs_module_text("Dock.Music"),
				 "music",
				 obs_module_text("Dock.MusicHint"), this);
	connect(musicBtn_, &QPushButton::toggled, this, [this](bool on) {
		for (Which w : targetChannels())
			PlaybackCoordinator::instance(w).setMusicEnabled(on);
		// SAY IT NOW, not after the replay. The two ways music produces
		// nothing — a file that is not there, a source that is in no
		// active scene — are both invisible while the key is being
		// pressed and both silent while the replay runs.
		if (!on)
			return;
		const std::string why = pc().musicProblem();
		if (!why.empty())
			showNotice(localizedError(why));
	});

	// MUTE — the replay input(s) sit muted in the OBS mixer, so every replay
	// plays silent. It LATCHES: a new replay does not clear it, NOW/Live do
	// not, only the operator does (this key, its hotkey, or the Settings
	// checkbox that seeds it). Follows the A|B / A / B selector, exactly like
	// music. Music on + mute on = the clips play under the music track alone.
	muteBtn_ = statusToggle(Icon::Mute, obs_module_text("Dock.Mute"), "muteAudio",
				obs_module_text("Dock.MuteHint"), this);
	muteBtn_->setChecked(ReplayCore::instance().getConfig().muteReplayAudio);
	connect(muteBtn_, &QPushButton::toggled, this, [this](bool on) {
		for (Which w : targetChannels())
			ReplayChannel::instance(w).setMuted(on);
	});

	toOutputBtn_ = statusToggle(Icon::ToOutput,
				    obs_module_text("Dock.ToOutput"), "toOutput",
				    obs_module_text("Dock.ToOutput"), this);
	// ONE state, two places to see it: the key starts where Settings says, and
	// every play path reads the key (see playOnTargets). A setting and a button
	// that each hold their own copy of "does this take Program" is a button left
	// in the wrong position.
	toOutputBtn_->setChecked(ReplayCore::instance().getConfig().toOutputOnPlay);
	// The key is NOT written back to the config. setConfig() re-points the
	// segment index and re-creates the Branch Output filters, so a key the
	// operator presses mid-match must not reach it — the same rule that keeps a
	// typed comment out of the config. Settings seeds the key at start-up; from
	// then on the key is the live state and Settings is where the default lives.
	// TWO ROWS, AND THE TWO KEYS THAT MATTER MOST ARE DRAWN LIKE IT:
	//
	//   play/pause  stop  reverse  last  step-  step+  ⋮   drive the clip
	//   [           NOW           ]                        back to the live edge
	//   [ PLAY ]  two rows tall, filled                     events on air
	//
	// PLAY is the biggest key on the panel because it is the one that takes
	// Program; NOW is the widest because it is the way back. They were a text
	// button and a small key in a row of eight, the same weight as a frame
	// step, so the eye had to read the whole strip to find either.
	//
	// Stop stands next to Play, in that order, because that is the pair: one
	// starts the picture and the other gives Program back.
	//
	// The three MODES that used to end this row — loop, music, in output — are
	// on the status line now. They are not things you do to the clip, they are
	// the conditions the next one runs under, and mixed in here they read as
	// four more transport keys.
	nowBtn_->setMinimumWidth(56);
	more->setFixedSize(22, kKeyH);
	for (QPushButton *b : {playPauseBtn_, stopBtn_, revBtn, lastBtn,
			       stepBackBtn, stepBtn})
		b->setFixedHeight(kKeyH);
	nowBtn_->setFixedHeight(kKeyH);
	playSel->setMaximumHeight(QWIDGETSIZE_MAX);
	blk->setShapes({{Cell(playPauseBtn_), Cell(stopBtn_), Cell(revBtn),
			 Cell(lastBtn), Cell(stepBackBtn), Cell(stepBtn),
			 Cell(more), Cell(playSel, 1, true, 2)},
			{Cell(nowBtn_, 7)}},
		       {{Cell(playPauseBtn_), Cell(stopBtn_), Cell(revBtn),
			 Cell(lastBtn), Cell(stepBackBtn), Cell(stepBtn),
			 Cell(more)},
			{Cell(nowBtn_, 4), Cell(playSel, 3)}});

	// There WAS a big "position / length" readout under these keys. It is gone:
	// the position bar prints the same two numbers on itself (setOverlayText),
	// where the operator is already looking while he scrubs, and two copies of a
	// timecode a frame apart is two things to reconcile at exactly the moment
	// there is no time to.

	// wire transport actions
	connect(playPauseBtn_, &QPushButton::clicked, this, [this]() {
		// A REAL pause: the clip freezes on the frame it is showing and the
		// next press carries on from there. It used to stop the queue, so
		// the second press re-cued the event and played it again from the
		// IN — the operator paused on the moment he wanted, pressed play,
		// and lost it.
		for (Which w : targetChannels()) {
			auto &pcw = PlaybackCoordinator::instance(w);
			auto &chw = ReplayChannel::instance(w);
			if (chw.playing()) {
				pcw.setPaused(!chw.paused());
				continue;
			}
			// NOTHING RUNNING, AND THE BAR IS ON FOOTAGE NOBODY
			// MARKED: play THAT, off air.
			//
			// This is what the key was missing. Parked on an unmarked
			// stretch it used to replay the selected event instead —
			// something else entirely, from somewhere else on the
			// timeline — so the only way to look at an action that had
			// not been marked was to mark it, which is precisely what
			// the operator was trying not to do. It runs until Stop,
			// and it never touches Program: putting it on air is a
			// second, deliberate press of "Play events".
			//
			// Once, for the bay the selector is on: a free review is a
			// range, not an event, and playing the same range on both
			// bays would be two decoders showing one picture.
			if (playheadIsFreeFootage()) {
				if (w == targetChannels().front())
					playFreeReview(/*toOutput*/ false);
				continue;
			}
			// Otherwise ▶ means "play what is selected", which is what
			// it has always meant.
			ReplayCore::instance().setFollowLive(false);
			replayCurrentOn(w);
		}
	});
	connect(stopBtn_, &QPushButton::clicked, this,
		[this]() { stopPlayback(); });
	connect(nowBtn_, &QPushButton::clicked, this, [this]() {
		// the reference controller NOW: drop the replay and watch the live edge again.
		pc().stopEvents();
		ReplayCore::instance().setFollowLive(true);
		// Back at the front, so the stretch that was armed on the bar is
		// no longer what the play keys are about.
		clearFreeReview();
	});

	return blk;
}

// ---------------------------------------------------------------------------
// Bottom bar — the reference controller's two control rows plus the full-width position bar
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The status line
// ---------------------------------------------------------------------------
//
// One row. On the left the health badge and the sentence — which list and which
// event the transport keys are about, where the playhead is, and the answer to
// a key the operator just pressed; on the right the three modes and the speed
// the next replay will run at.
//
// THE MODES SIT AGAINST THE RIGHT EDGE, not in the middle: a group centred on a
// bar whose width changes with the dock is a group that moves every time the
// panel is resized, and against an edge the hand finds it the same way twice.
QWidget *MultiReplayDock::buildStatusBar(QWidget *parent)
{
	statusBar_ = new QWidget(parent);
	statusBar_->setObjectName(QStringLiteral("mrStatusBar"));
	statusBar_->setFixedHeight(kStatusBarH);
	auto *h = new QHBoxLayout(statusBar_);
	h->setContentsMargins(6, 2, 6, 2);
	h->setSpacing(6);

	// The health badge moved here from beside REC. What it reports is a
	// READING — how the take is going — and the record section is where the
	// take is ARMED. It is hidden unless there is something to say: a badge
	// that is always there is furniture, and furniture is what nobody looks
	// at the day it finally turns red.
	//
	// DENSE, so the style sheet does not ask for a taller frame than the
	// widget owns — which put its bottom border outside it and read as a box
	// somebody forgot to close. Same trap as the skip key on the on-air band.
	healthBtn_->setParent(statusBar_);
	healthBtn_->setProperty("dense", true);
	healthBtn_->setMinimumHeight(0);
	healthBtn_->setFixedHeight(kStatusBarH - 6);
	// AMBER, like the border and the number beside it. The badge is only ever
	// on screen when it has something to report (it is hidden outright when it
	// has not), so its mark has no resting chrome state to be drawn in.
	setKeyIconRole(healthBtn_, Icon::Health, IconRole::Warn, tintsFor(sc()),
		       11);
	setKeyId(healthBtn_, QStringLiteral("health"));
	h->addWidget(healthBtn_);

	statusNotice_ = new QLabel(statusBar_);
	statusNotice_->setObjectName(QStringLiteral("mrChanStrip"));
	statusNotice_->setTextFormat(Qt::PlainText);
	// Ignored horizontally: what it says changes every tick and its natural
	// width would otherwise be a floor under the whole panel.
	statusNotice_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	h->addWidget(statusNotice_, 1);

	auto *sep = new QWidget(statusBar_);
	sep->setObjectName(QStringLiteral("mrStatSep"));
	sep->setFixedWidth(1);
	h->addWidget(sep);
	for (QPushButton *b : {loopBtn_, musicBtn_, muteBtn_, toOutputBtn_}) {
		b->setParent(statusBar_);
		b->setFixedHeight(kStatusBarH - 4);
		// THE LAYOUT OWNS THE HEIGHT, so the style sheet must not also have
		// an opinion about it: the rule for these asks for exactly the 18 px
		// they are given, and exactly is not a margin - any rounding, at any
		// display scale, puts the frame one pixel past the widget and the
		// bottom border outside it. Unlit that is invisible (the border is
		// transparent); lit it is a box somebody forgot to close, which is
		// why only "in output" was ever reported.
		b->setProperty("mrPinned", true);
		h->addWidget(b);
	}
	auto *sep2 = new QWidget(statusBar_);
	sep2->setObjectName(QStringLiteral("mrStatSep"));
	sep2->setFixedWidth(1);
	h->addWidget(sep2);

	statusSpeed_ = new QLabel(QStringLiteral("1.00\xc3\x97"), statusBar_);
	statusSpeed_->setObjectName(QStringLiteral("mrStatusValue"));
	h->addWidget(statusSpeed_);
	return statusBar_;
}

QWidget *MultiReplayDock::buildBottomBar()
{
	auto *box = new QWidget(this);
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(3);
	// The control bands must never be the thing that gives height back: with a
	// Preferred policy a QVBoxLayout that is short of room shrinks every child
	// towards its minimum, and the minimum of a row of buttons is a row of
	// buttons nobody can hit. Minimum vertically = "sizeHint is the floor" —
	// the splitter above (stretch 1, and a picture that is happy at any size)
	// is what absorbs a short dock.
	box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

	// -- THE CONTROL STRIP --------------------------------------------
	// Two macro-rows:
	//
	//   MARK (3 rows)     A↔B A B ⇄        speeds / dial / Export
	//   REC + clock       transport + NOW + PLAY      ▲ ▼ ⋯
	//
	// The first line is what you do to the FOOTAGE, the second is the take
	// and the transport. Lanes put the same group in the same place on both
	// rows, so the left lane starts at one x on each and the right lane ends
	// at one x on each.
	//
	// Every section declares that wide arrangement AND a compact fold of the
	// same keys; the strip wears the wide one whenever the dock is wide
	// enough to carry it and folds otherwise (dock-layout.hpp). Nothing is
	// ever clipped and nothing is ever hidden.
	//
	// addStrip(), not addWidget(): the strip tells its parent two different
	// heights — the one it can live with and the one it would like — and only
	// a layout item of its own can carry both. Added as a plain widget its
	// floor becomes its preference, which is what pinned the panel at 680 px
	// and made it look unresizable.
	//
	// The rank is the order when the strip FOLDS on a narrow dock: what an
	// operator reaches for through a whole match comes first, and the running
	// order and the exports — which nobody touches while the ball is in play —
	// come last.
	strip_ = new ControlStrip(box);
	strip_->setObjectName(QStringLiteral("mrStrip"));
	// REARRANGED (§6.2): row 1 is PREPARE (arm, mark, pile the clip up), row
	// 2 is SEND IT LIVE (which bay, drive it, how fast). REC anchors row 1;
	// MARK moves to the centre lane so it is the first thing the eye lands
	// on after the picture, where the bay selector used to sit centred in a
	// lane that went empty the moment channel B was off. The ranks are the
	// order the strip folds into on a narrow dock: REC, mark, bay,
	// transport, speed, clips — export (inside clips now) stays last there,
	// "the one thing nobody touches while the ball is in play".
	//
	// buildAngleMatrix carries startsLine for row 2 unconditionally: with
	// one bay it is not omitted, only collapsed to zero width by
	// applyChannelBVisibility (see its own comment), so it stays the
	// correct place for a new line to start whether or not it is drawn.
	strip_->addBlock(buildRecBlock(), Lane::Left, /*startsLine*/ false, 0);
	strip_->addBlock(buildMarkers(), Lane::Centre, false, 1);
	clipsBlock_ = buildExportBlock();
	strip_->addBlock(clipsBlock_, Lane::Right, false, 5);
	strip_->addBlock(buildAngleMatrix(), Lane::Left, /*startsLine*/ true, 2);
	strip_->addBlock(buildTransport(), Lane::Centre, false, 3);
	speedBlock_ = buildSpeedBlock();
	strip_->addBlock(speedBlock_, Lane::Right, false, 4);
	// §6.3: built last, ranked last (a stack has a top, and this is the one
	// thing on it nobody reaches for during a match), visible only in
	// Tall — applyTallCollapse hides/shows it opposite the three sections
	// it stands in for.
	moreBlock_ = buildMoreBlock();
	strip_->addBlock(moreBlock_, Lane::Right, false, 6);
	addStrip(v, strip_);

	// ── THE STATUS LINE, ABOVE THE GREEN BAND ────────────────────────
	// The band says what is ON AIR; this line says what the next replay will
	// run under. Below it, the modes read as a footnote to a clip that is
	// already playing.
	//
	// It OWNS the modes rather than mirroring them: loop, music and "in
	// output" are buttons here and nowhere else. A status bar repeating three
	// toggles that were also keys in the strip would be three states with two
	// homes, which is exactly how a toggle ends up left in the wrong position.
	v->addWidget(buildStatusBar(box));

	// ── Row 3: the green ON-AIR band, and the key that skips past it ──
	// What is playing, on which angle, how much is left, at what speed —
	// with the fill as its progress. The >> beside it drops the clip and
	// takes the next item of the queue, which may be another angle of the
	// same event or the next event: the operator who has seen enough of a
	// replay should not have to sit through the rest of it, and Stop is a
	// different thing (it kills the sequence).
	{
		auto *h = new QHBoxLayout();
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(3);
		clipBar_ = new ClipBar(this);
		// FULL WIDTH, and the one key that belongs to it sits ON it, at the
		// right end. >> means "I have seen enough of THIS clip, take the
		// next one", so it is about the band and nothing else — putting it
		// in a row of unrelated keys made the operator hunt for it, and the
		// band is where his eye already is.
		// Its own role (mrSkip), not a transport key: a transport key's
		// Its own role (mrSkip), not a transport key: a transport key's
		// stylesheet asks for a height this band does not have in total, and
		// a style that draws taller than the widget owns puts the bottom
		// border outside it.
		//
		// CHEVRONS, deliberately not the solid triangles of a transport key.
		// It does not start anything: it drops the clip on air and takes the
		// next of the queue, and a mark that looked like play would be read
		// as one under pressure.
		nextClipBtn_ = iconBtn(Icon::SkipNext, "skipNext",
				       obs_module_text("Dock.NextClip"), clipBar_,
				       "mrSkip");
		// ...AND ITS MARK IS WHITE, because this key never sits on chrome:
		// it lives on the green band, at every theme and in every state.
		// #mrSkip already writes its label in #ffffff; drawn in the panel's
		// resting grey the chevrons were a grey-on-green smudge.
		setKeyIconRole(nextClipBtn_, Icon::SkipNext, IconRole::OnSignal,
			       tintsFor(sc()));
		// Four less than the band, which is 28: the key needs room for its
		// own bottom border inside it.
		nextClipBtn_->setFixedSize(30, 22);
		nextClipBtn_->setMinimumHeight(0);
		connect(nextClipBtn_, &QPushButton::clicked, this, [this]() {
			// Logged both ways: "I pressed >> and nothing happened"
			// is otherwise indistinguishable from "the press never
			// arrived", and one of those is a bug in the dock.
			const bool moved = pc().skipToNext();
			const auto ps = pc().playState();
			obs_log(LOG_INFO,
				"[dock] >> skip: %s (queue %d/%d, angle %d)",
				moved ? "advanced" : "nothing queued", ps.queuePos,
				ps.queued, ps.angle1);
			if (!moved)
				showNotice(obs_module_text("Dock.NothingQueued"));
		});
		auto *bl = new QHBoxLayout(clipBar_);
		bl->setContentsMargins(4, 2, 4, 2);
		bl->addStretch(1);
		bl->addWidget(nextClipBtn_, 0, Qt::AlignVCenter);

		auto *wrap = new QWidget(this);
		auto *wl = new QHBoxLayout(wrap);
		wl->setContentsMargins(0, 0, 0, 0);
		wl->addWidget(clipBar_, 1);
		// NO CAPTION OVER IT. The band is green, full width and the only
		// thing on the panel that fills with colour as a clip runs; a
		// heading saying "ON AIR" above it was a line of height spent
		// telling the operator what he could already see.
		h->addWidget(wrap, 1);
		v->addLayout(h);
	}

	// (The channel selector A|B / A / B and the swap key used to be a row of
	// their own here, under the green band. They are part of the camera
	// matrix now — see buildChannelRow: they say which bay the angle keys
	// drive, and that question belongs beside the keys it is about.)

	// ── Row 4: the position bar over the whole recorded timeline ──────
	// Graduated, and the widest thing on the panel: it is the only control
	// that reaches the whole project, and the operator finds it by its scale.
	seek_ = new SeekBar(this);
	seek_->setToolTip(obs_module_text("Dock.SeekHint"));
	// §6.6 — THE ZOOM FACTOR IS DRAWN INTO THE BAR NOW (SeekBar::
	// zoomHitRect/paintEvent), not a separate key beside it: the bar
	// already prints "4:12 / 1:03:20" over itself while scrubbing, so
	// drawing "8.4×" at its own right edge is the same idea applied to the
	// other number this control owns. Left click on the badge resets to
	// the whole timeline; right click opens the same spans menu the old
	// key did — the bar cannot build that menu itself (its entries depend
	// on this dock's displayDurNs_ and playhead), so it only asks.
	connect(seek_, &SeekBar::zoomMenuRequested, this,
		&MultiReplayDock::showZoomMenu);
	connect(seek_, &SeekBar::scrubStateChanged, this,
		[this](bool dragging) { seekDragging_ = dragging; });
	// Where the drag is, printed ON the bar — the only place it is printed now,
	// and the place the operator's eye is while he drags.
	connect(seek_, &SeekBar::scrubMoved, this, [this](double frac) {
		seek_->setOverlayText(
			formatTc((int64_t)(frac * (double)displayDurNs_)) +
			"  /  " + formatTc(displayDurNs_));
	});
	connect(seek_, &SeekBar::seekRequested, this,
		[this](double frac) { seekToFraction(frac); });
	connect(seek_, &SeekBar::markerDragged, this,
		&MultiReplayDock::onMarkerDragged);
	// The bar has to see the mouse before a button is pressed, or the cursor
	// could never say "this edge can be grabbed".
	seek_->setMouseTracking(true);
	// §6.6: no more barBox wrapper — that HBoxLayout existed only to hold
	// the bar and zoomBtn_ side by side, and the zoom factor lives inside
	// the bar's own ruler now. Recovers the 34px zoomBtn_ took plus the
	// 4px of spacing beside it, and the bar's right edge lines up with
	// every other control's, which a key floating past it never did.
	//
	// ...and no caption over the position bar either: it is graduated,
	// which is how an operator recognises a scrubber, and a caption above
	// it was naming the one control on the panel that names itself.
	v->addWidget(seek_);

	return box;
}

// ---------------------------------------------------------------------------


QToolButton *MultiReplayDock::buildGearMenu()
{
	// SETTINGS BELONGS WITH THE PANEL-WIDE KEYS, not inside the record
	// section. What it opens is the configuration of the whole panel — the
	// project, the cameras, the tags, the theme — and sitting beside REC it
	// read as part of arming a take.
	auto *gear = new QToolButton(this);
	gear->setObjectName("mrGear");
	setKeyIcon(gear, Icon::Gear, tintsFor(sc()), 15);
	setKeyId(gear, QStringLiteral("settings"));
	gear->setCursor(Qt::PointingHandCursor);
	gear->setToolTip(obs_module_text("Dock.Settings"));
	gear->setFixedHeight(kKeyH);
	{
		auto *menu = new QMenu(gear);
		auto *actNew = menu->addAction(obs_module_text("Dock.NewProject"));
		auto *actOpen = menu->addAction(obs_module_text("Dock.OpenProject"));
		menu->addSeparator();
		auto *actSetup = menu->addAction(obs_module_text("Setup.MenuItem"));
		auto *actSettings = menu->addAction(obs_module_text("Dock.Settings"));
		auto *actRename = menu->addAction(obs_module_text("Dock.RenameList"));
		// TAGS: the words this operator marks with. Worth carrying
		// between machines - a club's vocabulary is written once, not
		// once per laptop.
		auto *tags = menu->addMenu(obs_module_text("Dock.Tags"));
		auto *actTagsImport = tags->addAction(obs_module_text("Dock.TagsImport"));
		auto *actTagsExport = tags->addAction(obs_module_text("Dock.TagsExport"));
		menu->addSeparator();
		auto *actChapters =
			menu->addAction(obs_module_text("Dock.YouTubeChapters"));
		popupOnClick(gear, menu);
		connect(actTagsImport, &QAction::triggered, this,
			[this]() { importTags(); });
		connect(actTagsExport, &QAction::triggered, this,
			[this]() { exportTags(); });
		connect(actSetup, &QAction::triggered, this,
			&MultiReplayDock::runSetupWizard);
		connect(actRename, &QAction::triggered, this,
			&MultiReplayDock::renameListDialog);
		connect(actNew, &QAction::triggered, this,
			&MultiReplayDock::newProjectDialog);
		connect(actOpen, &QAction::triggered, this,
			&MultiReplayDock::openProjectDialog);
		connect(actSettings, &QAction::triggered, this,
			&MultiReplayDock::openSettings);
		connect(actChapters, &QAction::triggered, this,
			&MultiReplayDock::copyYouTubeChapters);
	}
	return gear;
}

// ---------------------------------------------------------------------------
// REC — the take, and every number about it
// ---------------------------------------------------------------------------
//
// ONE SECTION, as it should always have been: arming the take, how long it has
// been running, how much room is left, and the wall clock. They were spread
// between here and the status line, so "how long have we been recording" was
// answered in a different place from "are we recording".
//
// REC SPANS BOTH ROWS. It is one of the three first-function keys on this panel
// (REC, PLAY, NOW) and it is drawn bigger than the rest — exactly the height of
// the section, so nothing beside it is left misaligned.
KeyBlock *MultiReplayDock::buildRecBlock()
{
	auto *blk = new KeyBlock(QString(), this);

	// A MARK AND A WORD. The record dot is not the Live dot: one arms a take,
	// the other says where a mark lands, and they were the same drawing.
	recBtn_ = iconTextBtn(Icon::Rec, QStringLiteral("REC"), "rec", this,
			      "mrRec", 13);
	// ...AND THE DOT IS RED, like the word beside it. It was the panel's
	// resting grey, so the key that arms a take showed a grey dot next to the
	// letters REC written in the signal colour — the two halves of one key
	// disagreeing about what the key is.
	setKeyIconRole(recBtn_, Icon::Rec, IconRole::Rec, tintsFor(sc()), 13);
	recBtn_->setProperty("recording", false);
	recBtn_->setMinimumWidth(78);
	connect(recBtn_, &QPushButton::clicked, this, [this]() {
		auto &core = ReplayCore::instance();
		if (core.isRecording()) {
			core.stopRecording();
		} else {
			// Stop any event playing BEFORE arming: a new take must
			// not start while a clip is still being paced into the
			// replay input.
			pc().stopEvents();
			std::string err;
			if (!core.startRecording(err))
				QMessageBox::warning(this, "obs-multireplay",
						     QString::fromStdString(err));
		}
		poll();
	});

	// M4: the health badge lives next to the record key because that is
	// where the eye already goes when a take starts, and because what it
	// reports is always about the take. Hidden unless there is something to
	// say (see poll()).
	healthBtn_ = new QPushButton(this);
	healthBtn_->setObjectName("mrHealth");
	healthBtn_->setCursor(Qt::PointingHandCursor);
	healthBtn_->setFlat(true);
	healthBtn_->setFixedHeight(kKeyH);
	healthBtn_->hide();
	connect(healthBtn_, &QPushButton::clicked, this,
		&MultiReplayDock::showHealthDetails);

	// HOW LONG THE TAKE HAS BEEN RUNNING is the number the operator looks for,
	// so it is the big one and it is red while it runs. The wall clock and the
	// room left are the small print under it.
	//
	// FIXED WIDTH, and it is not cosmetic: these change four times a second and
	// their text changes LENGTH with it. A width change re-flows the strip,
	// which changes its height, which makes the panel redistribute height — and
	// what gives it up is the pictures, whose resize re-allocates a swap chain
	// on the graphics thread. A label that cannot change width cannot start
	// that chain.
	clockLbl_ = new QLabel(this);
	clockLbl_->setObjectName("mrClock");
	clockLbl_->setFont(QFont(monoFamily()));
	statusLbl_ = new QLabel(this);
	statusLbl_->setObjectName("mrMuted");
	const int kClockW = clockLbl_->fontMetrics().horizontalAdvance(
				    QStringLiteral("0000-00-00 00:00:00")) +
			    8;
	clockLbl_->setFixedWidth(kClockW);
	statusLbl_->setFixedWidth(kClockW);

	blk->setShapes({{Cell(recBtn_, 1, true, 2), Cell(clockLbl_, 1, false)},
			{Cell(nullptr, 1), Cell(statusLbl_, 1, false)}},
		       {{Cell(recBtn_, 1, true, 2), Cell(clockLbl_, 1, false)},
			{Cell(nullptr, 1), Cell(statusLbl_, 1, false)}});
	return blk;
}

// ---------------------------------------------------------------------------
// VELOCITA - the presets and the dial
// ---------------------------------------------------------------------------

KeyBlock *MultiReplayDock::buildSpeedBlock()
{
	// The dial is BUILT first because the shapes place it, and a widget
	// cannot be placed before it exists.
	buildSpeedDial();

	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneSpeed"), this);
	QList<QPushButton *> chips;
	speedChips_ = new QButtonGroup(this);
	speedChips_->setExclusive(false);
	// The reference set (25/33/50/75/100) plus the 2x that is its fast
	// forward - the engine takes any speed, since a speed is only the
	// spacing between frames.
	const std::pair<int, const char *> speedPresets[] = {
		{25, "25%"}, {33, "33%"},   {50, "50%"},
		{75, "75%"}, {100, "100%"}, {200, "2\xc3\x97"}};
	for (const auto &[pct, lbl] : speedPresets) {
		int p = pct; // copy: capturing a structured binding is
			     // non-portable
		auto *b = compactBtn(QString::fromUtf8(lbl), this, "mrSpeedChip");
		setKeyId(b, QString("speed%1").arg(p));
		// §7.3.12 — the slider next to these already has its own hint
		// (Dock.SpeedSliderHint); the six chips never did.
		b->setToolTip(obs_module_text("Dock.SpeedChipHint"));
		speedChips_->addButton(b, p);
		connect(b, &QPushButton::clicked, this, [this, p]() {
			QSignalBlocker block(speed_);
			speed_->setValue(p);
			speedLbl_->setText(
				QString::asprintf("%.2f\xc3\x97", p / 100.0));
			applyReplaySpeed(p);
		});
		b->setFixedHeight(kKeyH);
		chips << b;
	}
	// ONE WIDTH for the six of them: "2x" is two characters and "100%" is
	// four, so left to their labels they came out a ragged row of six
	// different keys - six sizes for six values of one setting, with the
	// widest reading as the most important.
	equaliseKeyWidths(chips);

	speed_->setMinimumWidth(110);
	// sectionKeyH(): the dial is a QSlider, not a button, so it never goes
	// through KeyBlock::apply()'s per-button pin — this is its only source
	// of height, gallery scale included.
	speed_->setFixedHeight(sectionKeyH());
	// THE DIAL SITS UNDER THE PRESETS, in both arrangements: they are one
	// control at two resolutions, and side by side the dial is a strip of
	// nothing between two groups of keys.
	//
	// EXPORT MOVED TO THE CLIPS SECTION (§6.2): "what do I do with a clip
	// once it is marked" used to have two answers in two corners of the
	// panel — reorder it here under the speed dial, export it one section
	// over. One section answers it now (buildExportBlock).
	blk->setShapes({{Cell(chips[0]), Cell(chips[1]), Cell(chips[2]),
			 Cell(chips[3]), Cell(chips[4]), Cell(chips[5])},
			{Cell(speed_, 5), Cell(speedLbl_, 1, false)}},
		       // The six presets stay on ONE row folded as well. They are
		       // the narrowest keys on the panel and splitting them across
		       // two rows bought nothing but a line — and it broke the run
		       // of values an operator reads left to right.
		       {{Cell(chips[0]), Cell(chips[1]), Cell(chips[2]),
			 Cell(chips[3]), Cell(chips[4]), Cell(chips[5])},
			{Cell(speed_, 5), Cell(speedLbl_, 1, false)}});
	return blk;
}

// ---------------------------------------------------------------------------
// THE RUNNING ORDER, and the actions that have no key of their own
// ---------------------------------------------------------------------------

KeyBlock *MultiReplayDock::buildExportBlock()
{
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneClips"), this);

	// The running order is the operator's. Two keys rather than
	// drag-and-drop: a drag inside a table whose cells are all editable is a
	// click away from starting an edit instead, and during a match that is
	// the wrong thing to risk.
	int orderKeyW = 0;
	QVector<QPushButton *> order;
	for (const auto &mv : {std::pair<Icon, int>{Icon::MoveUp, -1},
			       std::pair<Icon, int>{Icon::MoveDown, +1}}) {
		const int delta = mv.second;
		auto *b = iconBtn(mv.first, delta < 0 ? "moveUp" : "moveDown",
				  obs_module_text(delta < 0 ? "Dock.MoveUp"
							    : "Dock.MoveDown"),
				  this);
		connect(b, &QPushButton::clicked, this,
			[this, delta]() { moveSelectedEvent(delta); });
		b->ensurePolished();
		b->setFixedHeight(kKeyH);
		orderKeyW = std::max(orderKeyW, b->sizeHint().width());
		order << b;
	}

	// Duplicate / delete / delete-all have no place of their own on the
	// reference panel (they live in its context menu), and three more
	// buttons here would be three more things to read past. They are one
	// click away, and on the table's right-click menu as well.
	auto *edit = new QToolButton(this);
	edit->setObjectName("mrGear");
	setKeyIcon(edit, Icon::More, tintsFor(sc()), 14);
	edit->setCursor(Qt::PointingHandCursor);
	edit->setToolTip(obs_module_text("Dock.ClipActions"));
	setKeyId(edit, QStringLiteral("clipActions"));
	{
		auto *menu = new QMenu(edit);
		auto *actDup = menu->addAction(obs_module_text("Dock.Duplicate"));
		auto *actDel = menu->addAction(obs_module_text("Dock.Delete"));
		menu->addSeparator();
		auto *actAll = menu->addAction(obs_module_text("Dock.DeleteAll"));
		popupOnClick(edit, menu);
		connect(actDup, &QAction::triggered, this, [this]() {
			for (int id : selectedEventIds())
				EventStore::instance().duplicate(id);
			refreshEvents();
		});
		connect(actDel, &QAction::triggered, this, [this]() {
			const auto ids = selectedEventIds();
			if (!confirmDelete(ids))
				return;
			for (int id : ids)
				EventStore::instance().remove(id);
			refreshEvents();
		});
		connect(actAll, &QAction::triggered, this, [this]() {
			// The buttons are OURS, not Qt's: QMessageBox::Yes/No
			// follow Qt's own locale, not OBS's, so a panel in
			// Italian showed English "Yes/No" on the one confirm
			// this panel still has.
			QMessageBox box(this);
			box.setWindowTitle("obs-multireplay");
			box.setText(obs_module_text("Dock.DeleteAllConfirm"));
			QPushButton *yes = box.addButton(
				obs_module_text("Dock.Yes"),
				QMessageBox::YesRole);
			box.addButton(obs_module_text("Dock.No"),
				      QMessageBox::NoRole);
			box.exec();
			if (box.clickedButton() != yes)
				return;
			pc().stopEvents();
			EventStore::instance().clearAll();
		});
	}
	// A tool button sizes itself around its glyph plus a menu arrow, so left
	// alone it comes out narrower and shorter than the keys it stands with.
	edit->setFixedHeight(kKeyH);
	edit->setMinimumWidth(orderKeyW);
	edit->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	// EXPORT LIVES HERE NOW (§6.2): everything done to an event AFTER it is
	// marked, in one section instead of two. It used to sit under the speed
	// dial, which made "what do I do with a clip once it is marked" a
	// question with two different answers in two different corners of the
	// panel.
	auto *exportKey = buildExportKey();
	// TALL: order/actions, then export on its own line so it reads as the
	// section's second question rather than a fourth key crowded onto the
	// first row's end. FLAT: one row, all four side by side — this section
	// is short enough that folding it costs a line for no reason.
	blk->setShapes({{Cell(order[0]), Cell(order[1]), Cell(edit)},
			{Cell(exportKey, 3)}},
		       {{Cell(order[0]), Cell(order[1]), Cell(edit), Cell(exportKey)}});
	// EXPORT DROPS ITS WORD WHEN THE STRIP FOLDS, joining its three
	// neighbours as an icon the tooltip still names. Folded is exactly
	// where this section sits closest to the panel's own edge — a stacked
	// side dock, a Short arrangement's left column — and a labelled key
	// there is the one thing in the row asking for more width than the
	// other three combined (measured on the mockup: it moved the toolbar's
	// own "Live" key down to its CSS floor, clipped, at 1400x340).
	const QString exportLabel = exportKey->text();
	blk->setOnShape([exportKey, exportLabel](bool flat) {
		exportKey->setText(flat ? QString() : exportLabel);
	});
	return blk;
}

// ---------------------------------------------------------------------------
// §6.3 — "⋯ ALTRO": WHERE BAY, CLIPS AND SPEED GO IN A COLUMN
// ---------------------------------------------------------------------------
//
// Six sections in Tall's narrow width cost six lines — there is no room for
// even two of them to share one, which is what packs Short's own stack down
// to two or three (§6.4). REC, MARK and TRANSPORT stay visible: they are the
// 90% of what gets pressed during a match. The rest collapses behind one
// key, on the same menu-popup pattern the gear and the clip-actions key
// already use.
//
// EVERY ENTRY HERE CLICKS THE REAL (HIDDEN) BUTTON rather than
// reimplementing its slot. The alternative — a QAction wired to its own copy
// of the logic — is a second place a bay selector's checked state, or which
// speed is current, can go stale against the first: exactly the trap this
// project's own notes call out for the channel selector specifically. A
// hidden QAbstractButton still answers click() exactly as if the operator
// had pressed it, checked state included, so this menu is a second DOOR
// onto the same room, never a second room.
//
// Rebuilt on every aboutToShow, not once at construction: a hidden button's
// own state (which bay is current, which speed) can change while the panel
// sits collapsed, and a menu built once would show whatever was true the
// day it was opened.
KeyBlock *MultiReplayDock::buildMoreBlock()
{
	auto *blk = new KeyBlock(QString(), this);
	auto *btn = new QToolButton(this);
	btn->setObjectName("mrGear");
	setKeyIcon(btn, Icon::More, tintsFor(sc()), 14);
	btn->setCursor(Qt::PointingHandCursor);
	btn->setToolTip(obs_module_text("Dock.ZoneMoreHint"));
	setKeyId(btn, QStringLiteral("moreCollapsed"));
	auto *menu = new QMenu(btn);
	connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
		menu->clear();
		const Config cfg = ReplayCore::instance().getConfig();
		if (cfg.enableChannelB && chanSel_) {
			for (QAbstractButton *cb : chanSel_->buttons()) {
				QAction *a = menu->addAction(cb->text());
				a->setCheckable(true);
				a->setChecked(cb->isChecked());
				connect(a, &QAction::triggered, cb,
					[cb]() { cb->click(); });
			}
			if (swapBtn_) {
				QAction *a = menu->addAction(
					obs_module_text("Dock.SwapChannels"));
				connect(a, &QAction::triggered, swapBtn_,
					[this]() { swapBtn_->click(); });
			}
			menu->addSeparator();
		}
		if (QAbstractButton *up = findKeyButton(this, "moveUp")) {
			QAction *a =
				menu->addAction(obs_module_text("Dock.MoveUp"));
			connect(a, &QAction::triggered, up,
				[up]() { up->click(); });
		}
		if (QAbstractButton *dn = findKeyButton(this, "moveDown")) {
			QAction *a = menu->addAction(
				obs_module_text("Dock.MoveDown"));
			connect(a, &QAction::triggered, dn,
				[dn]() { dn->click(); });
		}
		// The SAME menu clipActions already builds (Duplica/Elimina/
		// Elimina tutto), added as a submenu rather than copied: one
		// list of destructive actions to keep meaning the same thing.
		if (QAbstractButton *edit = findKeyButton(this, "clipActions")) {
			if (QMenu *editMenu = edit->findChild<QMenu *>())
				menu->addMenu(editMenu);
		}
		menu->addSeparator();
		if (QAbstractButton *exp = findKeyButton(this, "export")) {
			QAction *a = menu->addAction(
				exp->text().isEmpty()
					? QString::fromUtf8(obs_module_text(
						  "Dock.ExportClips"))
					: exp->text());
			connect(a, &QAction::triggered, exp,
				[exp]() { exp->click(); });
		}
		if (speedChips_ && !speedChips_->buttons().isEmpty()) {
			menu->addSeparator();
			QMenu *speedMenu =
				menu->addMenu(obs_module_text("Dock.ZoneSpeed"));
			for (QAbstractButton *chip : speedChips_->buttons()) {
				QAction *a = speedMenu->addAction(chip->text());
				connect(a, &QAction::triggered, chip,
					[chip]() { chip->click(); });
			}
		}
	});
	popupOnClick(btn, menu);
	btn->setFixedHeight(kKeyH);
	const BlockShape shape{{Cell(btn)}};
	blk->setShapes(shape, shape);
	// HIDDEN FROM THE START: the panel opens Wide, and applyTallCollapse's
	// own guard only acts on a CHANGE — it would never think to hide a
	// block that came into the world already visible, which is what every
	// KeyBlock does by default.
	blk->setVisible(false);
	return blk;
}

// §6.3's other half: bay/clips/speed disappear from the strip's own
// arithmetic in Tall (see orderFor's isHidden() check, dock-layout.cpp),
// and the "more" key takes their place. angleBlock_ is never null here —
// unlike the mockup's bay_, this dock always builds it (§2.10 collapses
// its CONTENT when B is off, via applyChannelBVisibility, not the section
// itself) — but hiding it wholesale for Tall is independent of that and
// composes with it fine: whichever reason hid it, orderFor stops reserving
// its room, and the more menu's own aboutToShow decides on cfg.enableChannelB
// whether to offer the channel items at all.
void MultiReplayDock::applyTallCollapse(bool tall)
{
	if (tall == tallCollapsed_)
		return;
	tallCollapsed_ = tall;
	if (angleBlock_)
		angleBlock_->setVisible(!tall);
	if (clipsBlock_)
		clipsBlock_->setVisible(!tall);
	if (speedBlock_)
		speedBlock_->setVisible(!tall);
	const bool anyCollapsed = angleBlock_ || clipsBlock_ || speedBlock_;
	if (moreBlock_)
		moreBlock_->setVisible(tall && anyCollapsed);
	if (strip_)
		for (KeyBlock *b :
		     {angleBlock_, clipsBlock_, speedBlock_, moreBlock_})
			if (b)
				strip_->blockChanged(b);
}

// ---------------------------------------------------------------------------
// ONE EXPORT KEY, which asks WHICH
// ---------------------------------------------------------------------------
//
// There were two, side by side in a section of their own: one clip each, or the
// whole selection as a single file. That is a QUESTION, not two keys — and
// asking it on the press takes half the room and stops the operator having to
// know the difference before he has decided he wants to export at all.
//
// It lives under the speed dial, with the rest of what is done to a clip once
// it is marked.
QPushButton *MultiReplayDock::buildExportKey()
{
	auto *exp = iconTextBtn(Icon::ExportClip,
				obs_module_text("Dock.ExportClips"), "export", this);
	exp->setToolTip(obs_module_text("Dock.ExportReelHint"));
	exp->setFixedHeight(kKeyH);
	connect(exp, &QPushButton::clicked, this, [this, exp]() {
		const auto ids = selectedEventIds();
		if (ids.empty()) {
			showNotice(obs_module_text("Dock.SelectToReorder"));
			return;
		}
		QMenu menu(this);
		QAction *one = menu.addAction(obs_module_text("Dock.ExportClips"));
		QAction *reel = menu.addAction(obs_module_text("Dock.ExportReel"));
		QAction *picked = menu.exec(
			exp->mapToGlobal(QPoint(0, exp->height())));
		if (!picked)
			return;
		// §7.3.9: opens on the last folder THIS project exported to,
		// rather than starting the picker over from zero every time.
		const QString folder = QFileDialog::getExistingDirectory(
			this, obs_module_text("Dock.ExportFolder"),
			lastExportFolder());
		if (folder.isEmpty())
			return;
		setLastExportFolder(folder);
		std::string err;
		if (picked == one) {
			// exportEvent's return value used to be thrown away here: a
			// rejection on one clip out of a multi-selection (a bad
			// folder mid-run, an event whose OUT never got marked)
			// never reached the operator, and only the LAST error of
			// the batch would have survived even if it had been kept.
			int failed = 0;
			std::string lastErr;
			for (int id : ids) {
				std::string e;
				if (!ExportManager::instance().exportEvent(
					    id, kAllAngles, folder.toStdString(),
					    e)) {
					failed++;
					lastErr = e;
				}
			}
			if (failed > 0)
				showNotice(
					QString("%1/%2 %3: %4")
						.arg(failed)
						.arg(ids.size())
						.arg(obs_module_text(
							"Dock.ExportClipsFailed"))
						.arg(QString::fromStdString(
							lastErr)));
			return;
		}
		if (picked != reel)
			return;
		// ...and the whole selection as ONE file: the highlights reel.
		// Same events, same order, same angles, same speeds — one clip
		// after another, with the operator's music over it if the music
		// key is down.
		const bool music = musicBtn_ && musicBtn_->isChecked();
		if (!ExportManager::instance().exportSequence(
			    ids, music, folder.toStdString(), err))
			showNotice(localizedError(err));
		else
			showNotice(obs_module_text("Dock.ExportReelStarted"));
	});
	return exp;
}

// ---------------------------------------------------------------------------
// Markers: Live/Recorded + IN/OUT + presets
// ---------------------------------------------------------------------------

KeyBlock *MultiReplayDock::buildMarkers()
{
	// THREE ROWS, and they are the three questions this group answers, in the
	// order an operator asks them:
	//
	//   IN    OUT   Annulla      <- take a point / close it / throw it away
	//   -5s   -10s  -20s         <- take the last N seconds whole
	//   TRIM IN     TRIM OUT     <- move a point of the event already marked
	//
	// As one long row these eight keys were a strip with no internal
	// structure: "-10s" and "OUT" are not the same kind of act, and putting
	// them side by side said they were.
	//
	// SIX COLUMNS, not three, so that the row of two fills the section just as
	// the rows of three do - on three columns the trim row left one empty cell
	// in the corner, and the eye finds that hole every time it reads the block.
	auto *blk = new KeyBlock(obs_module_text("Dock.Mark"), this);

	// IN AND OUT STAY WORDS. The brief asks for icons wherever an action is
	// universally recognisable, and these are the counter-example it names
	// itself: every mark on a timeline is a bracket of some kind, and a panel
	// whose two most-pressed keys are two brackets is a panel you have to
	// hover to use.
	//
	// AND THEY ARE COMMANDS, NOT ACTIONS. They were drawn in the same filled
	// green as "play the events" — so the loudest keys on the panel were two
	// that mark a point and put nothing on air.
	auto *in = compactBtn(obs_module_text("Dock.MarkIn"), this);
	setKeyId(in, QStringLiteral("markIn"));
	auto *out = compactBtn(obs_module_text("Dock.MarkOut"), this);
	setKeyId(out, QStringLiteral("markOut"));
	connect(in, &QPushButton::clicked, this, [this]() {
		const int64_t t = markTimeNs();
		if (!markable(t))
			return;
		// Inherit the currently selected camera angle (0-based).
		EventStore::instance().markIn(t, currentAngle1() - 1);
		refreshEvents();
	});
	connect(out, &QPushButton::clicked, this, [this]() {
		const int64_t t = markTimeNs();
		if (!markable(t))
			return;
		if (!EventStore::instance().markOut(t))
			showNotice(obs_module_text("Dock.NoOpenEvent"));
		refreshEvents();
	});

	QList<QPushButton *> keys{in, out};
	QVector<QPushButton *> presets;
	for (int sec : {5, 10, 20}) {
		// A MINUS SIGN, not a hyphen: these read as durations before an
		// instant, and U+2212 is the character that says so at the size
		// the keys are drawn.
		auto *b = compactBtn(QString("−%1s").arg(sec), this);
		setKeyId(b, QString("mark%1").arg(sec));
		connect(b, &QPushButton::clicked, this, [this, sec]() {
			const int64_t t = markTimeNs();
			if (!markable(t))
				return;
			EventStore::instance().markInOut(t, sec,
							 currentAngle1() - 1);
			refreshEvents();
		});
		presets << b;
		keys << b;
	}

	// Trim: move the SELECTED event's in or out point to where the position
	// bar stands. A mark taken live is taken late by definition - the operator
	// saw the action first - and until now the only way to fix one was to
	// delete it and mark again from a scrub, which is two ways of saying the
	// same thing and one of them loses the angles and the comments. Zoom the
	// bar, put the playhead on the frame, press.
	//
	// Frame nudges are hotkeys rather than four more keys on a full row (see
	// registerDockHotkeys): a Stream Deck is where this kind of work actually
	// happens, and the panel is already dense.
	auto *trimIn = iconBtn(Icon::TrimIn, "trimIn",
			       obs_module_text("Dock.TrimInHint"), this);
	connect(trimIn, &QPushButton::clicked, this,
		[this]() { setSelectedPoint(true); });
	auto *trimOut = iconBtn(Icon::TrimOut, "trimOut",
				obs_module_text("Dock.TrimOutHint"), this);
	connect(trimOut, &QPushButton::clicked, this,
		[this]() { setSelectedPoint(false); });
	keys << trimIn << trimOut;

	// Cancel ends the FIRST row, beside the two keys it undoes, and it is the
	// only key of this group in the danger colour. On the old single row it sat
	// at the far end, five keys away from the thing it cancels.
	auto *cancel = iconBtn(Icon::Cancel, "markCancel",
			       obs_module_text("Dock.Cancel"), this, "mrDanger");
	// #mrDanger colours the LABEL, and this key has no label — only the ✕. So
	// the whole of "this one destroys something" lived in a property that
	// reached nothing, and the key was drawn exactly as neutral as the two it
	// undoes.
	setKeyIconRole(cancel, Icon::Cancel, IconRole::Danger, tintsFor(sc()));
	connect(cancel, &QPushButton::clicked, this, [this]() {
		EventStore::instance().markCancel();
		refreshEvents();
	});
	keys << cancel;

	// THREE ROWS, because it answers three questions, in the order an operator
	// reaches for them during a match:
	//
	//   −5s −10s −20s    take the last N seconds whole   (first function)
	//   IN  OUT  ✕       take a point, close it, undo it (second)
	//   ⇤IN OUT⇥         move a point already taken      (third)
	//
	// It was one row of eight, and one row said those three were the same kind
	// of act: "-10s" and "OUT" are not, and putting them side by side claimed
	// they were.
	//
	// SIX COLUMNS so the row of two divides as evenly as the rows of three: on
	// three columns the trim row left a hole in the corner, and the eye finds
	// that hole every time it reads the block.
	//
	// FOLDED IT IS TWO ROWS, and that is a deliberate trade rather than a
	// compromise. In a column every key row is charged to the event list, and a
	// third row here put the panel's floor past what a small floating window
	// can be. The hierarchy stands where there is room to draw it; where there
	// is not, the durations keep their own row and the points and the trims
	// share the next one.
	blk->setShapes({{Cell(presets[0], 2), Cell(presets[1], 2),
			 Cell(presets[2], 2)},
			{Cell(in, 2), Cell(out, 2), Cell(cancel, 2)},
			{Cell(trimIn, 3), Cell(trimOut, 3)}},
		       {{Cell(presets[0]), Cell(presets[1]), Cell(presets[2]),
			 Cell(cancel)},
			{Cell(in), Cell(out), Cell(trimIn), Cell(trimOut)}});
	return blk;
}

// ---------------------------------------------------------------------------
// Event list (searchable) + playback controls
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildEvents()
{
	auto *box = new QWidget(this);
	box->setMinimumHeight(84);
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(2);

	events_ = new QTableWidget(this);
	events_->setObjectName("mrEvents");
	events_->setSelectionBehavior(QAbstractItemView::SelectRows);
	events_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// The speed cell and the per-camera comments are edited in place; the
	// per-camera enable box is a click on its indicator.
	events_->setEditTriggers(QAbstractItemView::DoubleClicked |
				 QAbstractItemView::EditKeyPressed);
	events_->verticalHeader()->setVisible(false);
	// 30, and the number is arithmetic rather than taste: an angle cell holds
	// combo boxes, and the dock's style sheet gives every QComboBox a
	// min-height of 20 with 3px of padding above and below and a 1px border —
	// 28 px before anything is drawn in it. At the 22 it used to be, every row
	// clipped its own contents, which is what "the text looks cut" was.
	// Whoever changes the input rule in the dock style sheet has to change this
	// with it (see kDockStyleTemplate in dock-style.hpp).
	events_->verticalHeader()->setDefaultSectionSize(30);
	events_->setAlternatingRowColors(true);
	events_->setShowGrid(false);
	events_->setWordWrap(false);
	events_->setFrameShape(QFrame::NoFrame);
	events_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	events_->setContextMenuPolicy(Qt::CustomContextMenu);
	// The transport keys have to work from HERE, which is where the operator's
	// focus lives for most of a match. The table would otherwise swallow Enter
	// (open an editor) and ←/→ (walk across columns) — see eventFilter.
	events_->installEventFilter(this);
	rebuildEventColumns();
	connect(events_, &QTableWidget::itemChanged, this,
		&MultiReplayDock::onEventItemChanged);
	// the reference controller: double-clicking an event plays it TO OUTPUT. It is the fastest
	// path there is from "that one" to "on air", and the reason the operator
	// keeps his hand on the mouse. The per-camera columns are exempt, because
	// a double-click is ALSO how those cells are edited: taking program
	// because someone wanted to type a comment would be the worst kind of
	// surprise.
	connect(events_, &QTableWidget::cellDoubleClicked, this,
		[this](int row, int column) {
			if (column >= kColFirstCam)
				return;
			// Switchable, and ON by default: it is the fastest path
			// from "that one" to Program, and it is also two pixels
			// from the cells an operator edits all match long.
			if (!ReplayCore::instance().getConfig().doubleClickPlays)
				return;
			QTableWidgetItem *it = events_->item(row, kColId);
			if (!it)
				return;
			const int id = it->data(Qt::UserRole).toInt();
			if (id <= 0)
				return;
			std::string err;
			// The SAME to-output state as the panel key — not the
			// hard-coded true this used to pass. Two ways of putting
			// one event on air that disagree about taking Program is
			// the surprise nobody wants mid-match.
			//
			// Deliberately NOT playOnTargets(): a double-click is one
			// gesture on one row and it drives the channel the keys
			// drive, not both bays. Fanning it out made it a different
			// action from the one the operator made.
			if (!pc().playEvents({id}, currentAngle1() - 1,
					     toOutputBtn_ &&
						     toOutputBtn_->isChecked(),
					     err))
				showNotice(localizedError(err));
		});
	// Choosing a row LOADS it, on whichever channel the A|B selector points
	// at. In the reference controller picking an event puts it in the selected bay straight away;
	// here it used to sit there doing nothing until "Play events" was pressed,
	// so the operator picked his clip without seeing it.
	//
	// Guarded by refreshing_ AND by reselecting_, because refreshEvents()
	// re-selects a row on every rebuild (auto-selecting the newest mark). A cue
	// fired from there would drag the preview off the live camera every time a
	// mark was taken during a match, which is the opposite of what an operator
	// watching the game wants.
	connect(events_, &QTableWidget::itemSelectionChanged, this, [this]() {
		// The two cells that are widgets are drawn AROUND by the view, so
		// they never hear about the selection. Told on every change, and
		// before the early-out: a programmatic re-select still moves the
		// highlight, and the ink has to follow it.
		tintSelectedCells();
		if (refreshing_ || itemsProgrammatic_ || reselecting_)
			return;
		cueSelected();
	});
	// Right-click menu: the reference controller keeps the clip housekeeping off the panel, and
	// so do we — same actions as the ⋯ button on the bottom row.
	connect(events_, &QTableWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) {
			QMenu menu(this);
			QAction *actDup =
				menu.addAction(obs_module_text("Dock.Duplicate"));
			QAction *actDel =
				menu.addAction(obs_module_text("Dock.Delete"));
			menu.addSeparator();
			QAction *actExp =
				menu.addAction(obs_module_text("Dock.ExportClips"));
			QAction *chosen =
				menu.exec(events_->viewport()->mapToGlobal(pos));
			if (!chosen)
				return;
			if (chosen == actDup) {
				for (int id : selectedEventIds())
					EventStore::instance().duplicate(id);
				refreshEvents();
			} else if (chosen == actDel) {
				const auto delIds = selectedEventIds();
				if (!confirmDelete(delIds))
					return;
				for (int id : delIds)
					EventStore::instance().remove(id);
				refreshEvents();
			} else if (chosen == actExp) {
				auto ids = selectedEventIds();
				if (ids.empty())
					return;
				// §7.3.9: same remembered folder as the other
				// export entry point — one habit, not two.
				QString folder = QFileDialog::getExistingDirectory(
					this, obs_module_text("Dock.ExportFolder"),
					lastExportFolder());
				if (folder.isEmpty())
					return;
				setLastExportFolder(folder);
				// §2.5 — same accounting as the other export entry
				// point (buildExportKey): a rejection on one clip
				// out of a multi-selection must reach the operator,
				// not vanish into a discarded return value.
				int failed = 0;
				std::string lastErr;
				for (int id : ids) {
					std::string e;
					if (!ExportManager::instance().exportEvent(
						    id, kAllAngles,
						    folder.toStdString(), e)) {
						failed++;
						lastErr = e;
					}
				}
				if (failed > 0)
					showNotice(
						QString("%1/%2 %3: %4")
							.arg(failed)
							.arg(ids.size())
							.arg(obs_module_text(
								"Dock.ExportClipsFailed"))
							.arg(QString::fromStdString(
								lastErr)));
			}
		});
	v->addWidget(events_, 1);

	// There is no inspector panel any more. It existed for the one edit the
	// table could not hold — the per-angle speed — and that now has its own
	// half-column next to the enable box of the camera it belongs to. A panel
	// that duplicates the table costs height, costs a rebuild per selection
	// change, and asks the operator to look somewhere other than at the row.

	return box;
}

} // namespace multireplay
