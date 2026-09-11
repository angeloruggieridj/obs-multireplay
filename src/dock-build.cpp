/*
obs-multireplay — MultiReplayDock: building the toolbar, the transport and the event table
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Split out of multireplay-dock.cpp (pure move, no behaviour change): the
one-time widget construction for the toolbar, the preview pane, the
multiview tiles, the bay selector, the control-strip sections (marks, the
transport, REC, speed, export) and the event table used to sit in the same
10k+ line file as the poll loop and the Settings dialog. Splitting them
into their own translation units keeps each concern reviewable on its own.
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
#include <QActionGroup>
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
	// .tbar{padding:7px 9px} and .tbar.tall{row-gap:7px}. Without the
	// horizontal pad the last key on the row - LIVE, deliberately at the far
	// right - sits with its border ON the panel edge and reads as cut off.
	v->setContentsMargins(9, 7, 9, 7);
	v->setSpacing(7);
	toolbarV_ = v;

	// A THIN VERTICAL RULE (spec §1: "tre zone separate da filetti"). A
	// plain QWidget, not a QFrame — this project's rule for a background
	// from a stylesheet (dock-style.hpp's own note on WA_StyledBackground):
	// Qt paints one for free on a bare QWidget, never on a subclass.
	const auto mkVSep = [this]() -> QWidget * {
		auto *s = new QWidget(this);
		s->setObjectName(QStringLiteral("mrSepLine"));
		s->setFixedWidth(1);
		// .tb-sep{stretch}: the rule runs the row's full height.
		s->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
		return s;
	};
	toolSepA_ = mkVSep();
	toolSepB_ = mkVSep();
	toolSepC_ = mkVSep();

	auto *topRow = new QWidget(box);
	// NAMED so the sheet can reach the whole bar at once. The artifact
	// declares the typeface on the BAR (.tbar{font-family:var(--ff-label)}),
	// not on each key; only the project name, LIVE and the search field step
	// out of it. Left unnamed, the bar inherited the BODY family, which is
	// wider than the condensed one the drawing uses - and the row overflowed,
	// clipping LIVE against the panel edge.
	topRow->setObjectName(QStringLiteral("mrToolbar"));
	toolRow1_ = topRow;
	auto *h = new QHBoxLayout(topRow);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(7); // .tbar{gap:7px}
	// TALL'S OWN SEARCH ROW (spec §5): built here so arrangeToolbar() only
	// ever moves widgets, never creates them. Hidden until Tall asks for it.
	toolRow2_ = new QWidget(box);
	toolRow2_->setObjectName(QStringLiteral("mrToolbar"));
	auto *h2 = new QHBoxLayout(toolRow2_);
	h2->setContentsMargins(0, 0, 0, 0);
	h2->setSpacing(5);
	toolRow2_->hide();

	// THE PROJECT SELECTOR (spec §1): a menu button, not a label — Nuovo,
	// Apri…, and the list of projects on disk. Wider than the other toolbar
	// items (min-width) so the whole name shows where there is room. Its
	// menu opens on clicked (popupOnClick), never setMenu.
	projectBtn_ = new QToolButton(box);
	projectBtn_->setObjectName(QStringLiteral("mrProjectSel"));
	projectBtn_->setProperty("mrProject", true);
	projectBtn_->setCursor(Qt::PointingHandCursor);
	projectBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);
	projectBtn_->setToolTip(obs_module_text("Dock.ProjectMenuHint"));
	projectBtn_->setMinimumWidth(kProjectSelMinW);
	projectBtn_->setMaximumWidth(280);
	projectBtn_->setFixedHeight(kKeyH);
	setKeyId(projectBtn_, QStringLiteral("project"));
	projectBtn_->hide();
	{
		auto *menu = new QMenu(projectBtn_);
		auto *actNew = menu->addAction(obs_module_text("Dock.NewProject"));
		actNew->setObjectName(QStringLiteral("mrActProjectNew"));
		auto *actOpen =
			menu->addAction(obs_module_text("Dock.OpenProject"));
		actOpen->setObjectName(QStringLiteral("mrActProjectOpen"));
		menu->addSeparator();
		auto *recent = menu->addMenu(obs_module_text("Dock.RecentProjects"));
		recent->setObjectName(QStringLiteral("mrActProjectRecent"));
		// The gate finds actions, and a submenu is a menu: name its
		// action too, so the four voices are found the same way.
		recent->menuAction()->setObjectName(
			QStringLiteral("mrActProjectRecent"));
		auto *actRename =
			menu->addAction(obs_module_text("Dock.RenameProject"));
		actRename->setObjectName(QStringLiteral("mrActProjectRename"));
		connect(actRename, &QAction::triggered, this,
			&MultiReplayDock::renameProjectDialog);
		connect(actNew, &QAction::triggered, this,
			&MultiReplayDock::newProjectDialog);
		connect(actOpen, &QAction::triggered, this,
			&MultiReplayDock::openProjectDialog);
		// Built on every open: a project created or deleted while the
		// panel is up must show up without a rebuild.
		connect(recent, &QMenu::aboutToShow, this, [this, recent]() {
			recent->clear();
			auto &core = ReplayCore::instance();
			const std::string cur = core.getConfig().currentProjectName;
			const auto all = core.listProjects();
			if (all.empty()) {
				QAction *none = recent->addAction(
					obs_module_text("Dock.NoProjects"));
				none->setEnabled(false);
				return;
			}
			for (const std::string &p : all) {
				QAction *a = recent->addAction(
					QString::fromStdString(p));
				a->setCheckable(true);
				a->setChecked(p == cur);
				connect(a, &QAction::triggered, this,
					[this, p]() {
						std::string err;
						if (!ReplayCore::instance()
							     .openProject(p, err))
							showNotice(
								localizedError(
									err));
						poll();
					});
			}
		});
		popupOnClick(projectBtn_, menu);
	}
	// projectBtn_, the search pair, the three panel keys and Live are all
	// placed by arrangeToolbar() at the end of this function, and again on
	// every mode change — not here. Which of h / h2 each rides in, and in
	// what order, is exactly the thing that differs between Wide/Short's one
	// row and Tall's three (spec §1/§5); building that placement twice would
	// be two copies of the same decision to keep in step.

	// A KEY, not a label: where the field hides (Short) the magnifier is
	// what opens it, and a QLabel with an event filter is not a thing an
	// operator can tell is pressable. Wears the tool-icon size in every
	// arrangement (set by arrangeToolbar, 26x25 — 23 in Tall); in Wide/Tall
	// the field is always out, so the key just hands it the focus.
	searchIcon_ = new QToolButton(box);
	// ITS OWN NAME (see mrGear, same trap): the look is shared in the sheet,
	// the identity is not. Carries the cluster's "search" id — the gate
	// reads the order off it, visible in all three arrangements, while the
	// field itself hides in Short.
	searchIcon_->setObjectName(QStringLiteral("mrSearchKey"));
	setKeyId(searchIcon_, QStringLiteral("search"));
	setKeyIcon(searchIcon_, Icon::Search, tintsFor(sc()), 13);
	searchIcon_->setCursor(Qt::PointingHandCursor);
	searchIcon_->setToolTip(obs_module_text("Dock.Search"));
	connect(searchIcon_, &QToolButton::clicked, this, [this]() {
		if (!search_)
			return;
		// In Short the field is a guest the key summoned: a press shows
		// it, a second press sends it back. Elsewhere it is always out,
		// so the key just hands it the focus.
		if (panelMode_ == PanelMode::Short && search_->isVisible()) {
			search_->setVisible(false);
			searchIcon_->setFocus(Qt::MouseFocusReason);
		} else {
			search_->setVisible(true);
			search_->setFocus(Qt::MouseFocusReason);
		}
	});
	search_ = new QLineEdit(box);
	// ITS OWN NAME (.tb-search): without a rule of its own the field was
	// drawn by OBS — a smudge on a light panel inside a dark OBS.
	search_->setObjectName(QStringLiteral("mrSearch"));
	search_->setPlaceholderText(obs_module_text("Dock.Search"));
	search_->setClearButtonEnabled(true);
	// FIXED WIDTHS FROM THE DRAWING, not ems: .tb-search{min-width:150px}
	// (.narrow: 112, applied by applyPanelMode). An em derivation used to
	// sit here for 150% font scales; the artifact states pixels and the
	// gate asserts them, so pixels win.
	search_->setMinimumWidth(kSearchMinW);
	search_->setProperty("searchEmpty", true);
	connect(search_, &QLineEdit::textChanged, this,
		[this](const QString &text) {
			// The placeholder reads italic (.tb-search, italic) while a
			// typed value does not: Qt has no placeholder-only font
			// state, so the widget carries it and flips when text
			// arrives. An unpolish/polish round trips the dynamic
			// property into the sheet rule below.
			search_->setProperty("searchEmpty", text.isEmpty());
			search_->style()->unpolish(search_);
			search_->style()->polish(search_);
			refreshEvents();
		});
	// SHORT: DONE MEANS GONE. Enter or focus-out ends the errand the key
	// summoned the field for, and it goes back to icon-only on its own —
	// otherwise the row wears a field nobody asked to keep.
	connect(search_, &QLineEdit::editingFinished, this, [this]() {
		if (panelMode_ == PanelMode::Short && search_)
			search_->setVisible(false);
	});

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
	// Live is added LAST, past the panel keys and a separator — see the far
	// end of this function.

	// the reference controller's Monitors key. It takes the whole
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

	// THE LAYOUT MENU. It was the ⛶ full-screen toggle; the redesign folded
	// the arrangement choice into it — one control instead of two. A
	// QToolButton with a menu (Automatico · Normale · Short · Tall ·
	// separator · Schermo intero), popped on click like the gear (no
	// setMenu — Qt draws its own arrow over our mark and no rule reaches it).
	//
	// ALWAYS VISIBLE now, docked or floating: the four shape presets are
	// useful either way. Only the "Schermo intero" action is disabled while
	// docked — a docked panel has no window of ours to grow — and lit while
	// the panel owns the screen. refreshFullScreenKey() keeps all five in
	// step on poll()'s slow beat.
	fullScreenBtn_ = new QToolButton(box);
	fullScreenBtn_->setObjectName("mrToggle");
	setKeyIcon(fullScreenBtn_, Icon::FullScreen, tintsFor(sc()), 14);
	setKeyId(fullScreenBtn_, QStringLiteral("layout"));
	fullScreenBtn_->setCursor(Qt::PointingHandCursor);
	fullScreenBtn_->setToolTip(obs_module_text("Dock.LayoutHint"));
	// .tb-ico{width:26px;height:25px} - icon-only in every arrangement, so
	// unlike Monitors this one takes the size at construction.
	fullScreenBtn_->setFixedSize(kToolIcoW, kToolIcoH);
	{
		auto *menu = new QMenu(fullScreenBtn_);
		auto *shapes = new QActionGroup(menu);
		shapes->setExclusive(true);
		const auto addShape = [&](const char *key, int preset,
					  const char *objName) {
			QAction *a = menu->addAction(obs_module_text(key));
			a->setObjectName(QString::fromLatin1(objName));
			a->setCheckable(true);
			shapes->addAction(a);
			connect(a, &QAction::triggered, this,
				[this, preset]() { setLayoutPreset(preset); });
			return a;
		};
		actLayoutAuto_ = addShape("Dock.LayoutAuto", 0, "mrActLayoutAuto");
		actLayoutWide_ = addShape("Dock.LayoutWide", 1, "mrActLayoutWide");
		actLayoutShort_ =
			addShape("Dock.LayoutShort", 2, "mrActLayoutShort");
		actLayoutTall_ = addShape("Dock.LayoutTall", 3, "mrActLayoutTall");
		menu->addSeparator();
		actFullScreen_ = menu->addAction(obs_module_text("Dock.FullScreen"));
		actFullScreen_->setObjectName("mrActFullScreen");
		actFullScreen_->setCheckable(true);
		connect(actFullScreen_, &QAction::triggered, this,
			[this](bool on) {
				setPanelFullScreen(on);
				// The window may have refused (re-docked between
				// the paint and the click), so the action is told
				// what happened rather than trusted to have made
				// it happen.
				refreshFullScreenKey();
			});
		popupOnClick(fullScreenBtn_, menu);
	}
	layoutPreset_ = ReplayCore::instance().getConfig().layoutPreset;
	// THE GEAR SITS WITH THE OTHER PANEL-WIDE KEYS. What it opens is the
	// configuration of the whole panel — the project, the cameras, the tags,
	// the theme — and inside the record section it read as part of arming a
	// take. It goes beside the full-screen key because those two are the pair
	// that are about the panel itself rather than about the replay.
	gearBtn_ = buildGearMenu();
	// TOOLS CLUSTER, THEN A GAP, THEN LIVE — in Wide/Short. Tall groups them
	// differently (spec §5); arrangeToolbar() below is what actually places
	// every one of these keys, in both h and h2.
	v->addWidget(topRow);
	v->addWidget(toolRow2_);

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
	// NO max-width on the strip: the drawing caps it at 300px, but the
	// operator overrode that on 2026-09-08 (recorded in the design spec §9)
	// — the slack between "+" and the tools is spent showing MORE of the
	// defined banks instead. arrangeToolbar() gives bankRow_ the row's
	// stretch, so it grows into that slack and only scrolls on overflow.
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
	// THE TAB'S OWN MENU (operator request, 2026-09-08): right-click a label
	// offers Rinomina · Elimina tutto (this list's events) · Elimina scheda,
	// in that order. Creation had the "+" for years with no way back.
	// Clearing keeps the name; deleting compacts the lists above down so
	// 1..count stays contiguous (EventStore::removeList) and shrinks the
	// count — the last remaining tab refuses, with a notice saying why.
	listTabs_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(listTabs_, &QTabBar::customContextMenuRequested, this,
		[this](const QPoint &pos) {
			const int idx = listTabs_->tabAt(pos);
			if (idx < 0)
				return;
			// Tabs past the count are HIDDEN, never removed
			// (refreshListNames), so a visible index is its own list.
			const int list = idx + 1;
			auto &store = EventStore::instance();
			const std::string nm = store.listName(list);
			const QString shown =
				nm.empty() ? QString::number(list)
					   : QString::fromStdString(nm);
			QMenu menu(listTabs_);
			QAction *actRename = menu.addAction(
				obs_module_text("Dock.RenameList"));
			QAction *actClear = menu.addAction(
				obs_module_text("Dock.DeleteAll"));
			QAction *actDelete = menu.addAction(
				obs_module_text("Dock.TabDelete"));
			QAction *picked = menu.exec(
				listTabs_->mapToGlobal(pos));
			if (!picked)
				return;
			if (picked == actRename) {
				renameListAt(list);
				return;
			}
			const auto confirm = [&](const char *key) {
				QMessageBox box(this);
				box.setWindowTitle("obs-multireplay");
				box.setText(QString(obs_module_text(key))
						    .arg(shown)
						    .arg(store.eventCount(list)));
				QPushButton *yes = box.addButton(
					obs_module_text("Dock.Yes"),
					QMessageBox::YesRole);
				box.addButton(obs_module_text("Dock.No"),
					      QMessageBox::NoRole);
				box.exec();
				return box.clickedButton() == yes;
			};
			if (picked == actClear) {
				if (store.eventCount(list) > 0 && !confirm("Dock.TabClearConfirm"))
					return;
				pc().stopEvents();
				store.clearList(list);
				poll();
				return;
			}
			// Elimina scheda.
			const int count = std::clamp(
				ReplayCore::instance().getConfig().eventListCount,
				1, kEventLists);
			if (count <= 1) {
				showNotice(obs_module_text("Dock.TabDeleteLast"));
				return;
			}
			if (!confirm("Dock.TabDeleteConfirm"))
				return;
			pc().stopEvents();
			if (!store.removeList(list, count))
				return;
			ReplayCore::instance().setEventListCount(count - 1);
			const int sel = store.selectedList();
			if (sel > list)
				store.selectList(sel - 1);
			else if (sel == list)
				store.selectList(std::max(1, list - 1));
			refreshListNames();
			poll();
		});
	// THE "+" KEY (spec §1): ~5 tabs show; + raises the count by one, up to
	// kEventLists, with the number as the name. Pinned to the right of the
	// strip so the tabs scroll under it, not past it.
	bankRow_ = new QWidget(box);
	bankRow_->setObjectName(QStringLiteral("mrToolbar"));
	auto *tr = new QHBoxLayout(bankRow_);
	tr->setContentsMargins(0, 0, 0, 0);
	tr->setSpacing(kBankTabGap); // .tb-tabs{gap:3px}
	tr->addWidget(listTabs_, 1);
	addBankBtn_ = new QToolButton(bankRow_);
	// ITS OWN NAME, not mrToggle: the + is not a state, it is a key that
	// creates, and the drawing gives it a square box and an ink of its own
	// (.tb-add). Under mrToggle no rule could tell it from a latch.
	addBankBtn_->setObjectName(QStringLiteral("mrAddBank"));
	addBankBtn_->setText(QStringLiteral("+"));
	addBankBtn_->setCursor(Qt::PointingHandCursor);
	addBankBtn_->setToolTip(obs_module_text("Dock.AddBankHint"));
	setKeyId(addBankBtn_, QStringLiteral("addBank"));
	addBankBtn_->setFixedSize(kAddBankSide, kAddBankSide); // .tb-add{25x25}
	connect(addBankBtn_, &QToolButton::clicked, this, [this]() {
		auto &core = ReplayCore::instance();
		const int n = core.getConfig().eventListCount;
		if (n >= kEventLists)
			return;
		core.setEventListCount(n + 1);
		// The new tab is the operator's doing: select it, don't strand him
		// on the old list while the new one sits out of view past the
		// scrollers (in Tall it always does). setCurrentIndex scrolls it
		// into view; refreshListNames only repaints names.
		EventStore::instance().selectList(n + 1);
		listTabs_->setCurrentIndex(n);
		refreshListNames();
		poll();
	});
	tr->addWidget(addBankBtn_, 0);

	// THE INITIAL ARRANGEMENT (spec §1/§5). panelMode_ already carries its
	// real default (Wide) at this point in construction; applyPanelMode's
	// own call to arrangeToolbar(), moments later, re-asserts whatever the
	// panel's actual starting size resolves to and is a no-op if it agrees.
	arrangeToolbar(panelMode_);

	return box;
}

// ---------------------------------------------------------------------------
// The toolbar's row count follows the panel mode (artifact «La toolbar»,
// dcd11c4d, and spec-unico §1/§7): ONE row in Wide — project · banks ·
// search+tools · Live — the SAME row in Short but with search and Monitors
// worn as icons (tabella responsive: "Short ~900px, 1 riga icone"), and
// THREE rows in Tall (project/Live/tools · search alone · banks). Every
// widget here already exists (buildToolbar built it once); this only ever
// MOVES them between h (toolRow1_), h2 (toolRow2_) and toolbarV_
// (bankRow_'s own row) — never a second copy of a button whose
// checked/current state could go stale against the first.
//
// THE CLUSTER ORDER IS THE SPEC'S, NOT THE CONCEPT'S (spec-unico §1 wins:
// "🔍 ▦Monitors ⛶ ⚙"). The row used to run search · Monitors · gear ·
// layout, which matched neither document — it was in one of the two
// positions by accident, and the gate (toolbar_tool_cluster_order) now
// asserts it instead.
// ---------------------------------------------------------------------------

void MultiReplayDock::arrangeToolbar(PanelMode m)
{
	if (!toolRow1_ || !toolRow2_ || !bankRow_ || !toolbarV_)
		return;
	// THREE arrangements: Wide keeps every word, Short keeps the one row
	// but iconises search+Monitors, Tall stacks three rows. Short used to
	// ride the Wide branch, leaving "Monitors" spelled out at 900 px — the
	// exact row the drawing shows icon-only.
	const int want =
		(m == PanelMode::Tall) ? 2 : (m == PanelMode::Short) ? 1 : 0;
	if (want == toolbarArrangement_)
		return;
	toolbarArrangement_ = want;

	auto *h1 = qobject_cast<QHBoxLayout *>(toolRow1_->layout());
	auto *h2 = qobject_cast<QHBoxLayout *>(toolRow2_->layout());
	if (!h1 || !h2)
		return;

	// CLEAR BOTH ROWS COMPLETELY. Every entry here is either a widget item
	// (taking it does not delete the widget — it only detaches it from this
	// layout) or a stretch/spacing item (which owns nothing else and must be
	// deleted itself, the same way Qt's own "clear a layout" recipe does).
	QLayoutItem *item;
	while ((item = h1->takeAt(0)) != nullptr)
		delete item;
	while ((item = h2->takeAt(0)) != nullptr)
		delete item;
	// bankRow_ is the one piece with a THIRD possible home — its own row in
	// toolbarV_ — rather than just h1 vs h2, so it needs its own detach.
	h1->removeWidget(bankRow_);
	toolbarV_->removeWidget(bankRow_);

	if (want == 0) {
		// WIDE (spec-unico §1): one row, three zones behind thin
		// rules, Live isolated past its own gap and rule. Full words on
		// both keys — there is room for them beside a whole extra zone
		// (the banks) that Tall's row does not carry at all.
		// Order: search · Monitors · ⛶▾(layout) · ⚙ — the spec's, with
		// the layout menu BEFORE the gear.
		liveBtn_->setText(QString::fromUtf8(obs_module_text("Dock.LiveMode"))
					  .toUpper());
		monitorsBtn_->setText(
			QString::fromUtf8(obs_module_text("Dock.Monitors")));
		// A word key again (see Tall below): the export key gets its label
		// back outside the column.
		if (exportBtn_)
			exportBtn_->setText(QString::fromUtf8(
				obs_module_text("Dock.ExportClips")));
		// A word key again: release the icon size Short/Tall pinned.
		monitorsBtn_->setMinimumSize(QSize(0, 0));
		monitorsBtn_->setMaximumSize(
			QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
		// Icon-only keys wear the drawing's size in every arrangement;
		// re-asserted here because Tall shrinks them (below) and a mode
		// change must not leave them shrunk.
		gearBtn_->setFixedSize(kToolIcoW, kToolIcoH);
		fullScreenBtn_->setFixedSize(kToolIcoW, kToolIcoH);
		searchIcon_->setFixedSize(kToolIcoW, kToolIcoH);
		// .tb-name{min-width:132px}, elided past 280.
		projectBtn_->setMinimumWidth(kProjectSelMinW);
		projectBtn_->setMaximumWidth(280);
		h1->addWidget(projectBtn_);
		h1->addWidget(toolSepA_);
		// bankRow_ takes the row's stretch (operator override, 2026-09-08:
		// no 300px cap — the defined banks use the slack, the tools keep
		// the right edge). No second free stretch: two shares of leftover
		// space let the strip swallow the row and strand the "+" an inch
		// past the last tab.
		h1->addWidget(bankRow_, 1);
		h1->addWidget(toolSepB_);
		h1->addWidget(searchIcon_);
		h1->addWidget(search_);
		h1->addWidget(monitorsBtn_);
		h1->addWidget(fullScreenBtn_);
		h1->addWidget(gearBtn_);
		h1->addSpacing(10);
		h1->addWidget(toolSepC_);
		h1->addWidget(liveBtn_);
		toolSepA_->show();
		toolSepB_->show();
		toolSepC_->show();
		toolRow2_->hide();
	} else if (want == 1) {
		// SHORT (tabella responsive): the Wide row, but search and
		// Monitors as icons — .tb-ico{26x25} (only Tall narrows to 23).
		// search_ itself hides until tapped (applyPanelMode's narrow
		// logic); LIVE keeps its word (wireframe Short).
		liveBtn_->setText(QString::fromUtf8(obs_module_text("Dock.LiveMode"))
					  .toUpper());
		monitorsBtn_->setText(QString());
		monitorsBtn_->setFixedSize(kToolIcoW, kToolIcoH);
		// Word back on (see Tall below): Short is wide, the bar fits.
		if (exportBtn_)
			exportBtn_->setText(QString::fromUtf8(
				obs_module_text("Dock.ExportClips")));
		gearBtn_->setFixedSize(kToolIcoW, kToolIcoH);
		fullScreenBtn_->setFixedSize(kToolIcoW, kToolIcoH);
		searchIcon_->setFixedSize(kToolIcoW, kToolIcoH);
		projectBtn_->setMinimumWidth(kProjectSelMinW);
		projectBtn_->setMaximumWidth(280);
		h1->addWidget(projectBtn_);
		h1->addWidget(toolSepA_);
		h1->addWidget(bankRow_, 1);
		h1->addWidget(toolSepB_);
		h1->addWidget(searchIcon_);
		h1->addWidget(search_);
		h1->addWidget(monitorsBtn_);
		h1->addWidget(fullScreenBtn_);
		h1->addWidget(gearBtn_);
		h1->addSpacing(10);
		h1->addWidget(toolSepC_);
		h1->addWidget(liveBtn_);
		toolSepA_->show();
		toolSepB_->show();
		toolSepC_->show();
		toolRow2_->hide();
	} else {
		// TALL (spec §5): three rows —
		//   1) [project ▾] … ● LIVE (fenced by rules) … Monitors ⛶▾ ⚙
		//   2) the search field, extended to the row's full width
		//   3) banks + "+" (bankRow_, in its own row of toolbarV_)
		//
		// MONITORS LOSES ITS WORD HERE, LIVE DOES NOT. Row 1 carries the
		// project selector AND both panel keys AND the layout/gear pair
		// in a column as narrow as 320 px; Monitors' icon and tooltip say
		// what it does, but LIVE stays whole (operator request, 2026-09-08
		// — the drawing shows it full on Tall's first row, fenced by rules
		// past the selector). The Tall selector cap (150) pays for it.
		liveBtn_->setText(QString::fromUtf8(obs_module_text("Dock.LiveMode"))
					  .toUpper());
		monitorsBtn_->setText(QString());
		// THE EXPORT KEY LOSES ITS WORD TOO. The tools bar overflows a
		// ~320 px column with it on ("Esporta clip" clipped to "aporta",
		// measured); icon + tooltip carry it, the way Monitors' do above.
		if (exportBtn_)
			exportBtn_->setText(QString());
		// .tbar.tall .tb-ico{width:23px} — height stays 25.
		monitorsBtn_->setFixedSize(kToolIcoWTall, kToolIcoH);
		gearBtn_->setFixedSize(kToolIcoWTall, kToolIcoH);
		fullScreenBtn_->setFixedSize(kToolIcoWTall, kToolIcoH);
		searchIcon_->setFixedSize(kToolIcoWTall, kToolIcoH);
		// .tbar.tall .tb-name{min-width:0}, .pn elided past 150.
		projectBtn_->setMinimumWidth(0);
		projectBtn_->setMaximumWidth(150);
		h1->addWidget(projectBtn_);
		h1->addStretch(1);
		h1->addWidget(toolSepA_);
		h1->addWidget(liveBtn_);
		h1->addWidget(toolSepB_);
		h1->addWidget(monitorsBtn_);
		h1->addWidget(fullScreenBtn_);
		h1->addWidget(gearBtn_);
		toolSepA_->show();
		toolSepB_->show();
		toolSepC_->hide();

		h2->addWidget(searchIcon_);
		h2->addWidget(search_, 1);
		toolRow2_->show();

		// After toolRow1_ (index 0) and toolRow2_ (index 1).
		toolbarV_->insertWidget(2, bankRow_);
	}
	bankRow_->show();
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
	bg->setSpacing(6); // artifact monitor: .mrow{gap:6px} between the bays

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
		// The drawing's own edge (.box{border:2px}), like every tile: A/B
		// are boxes too, and borderless ones read as unfinished beside
		// framed cameras. Recoloured with the theme (see applyTheme).
		aBox_->setTallyFrame(QColor(sc().tileEdge), kTileEdgeW);
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
		bBox_->setTallyFrame(QColor(sc().tileEdge), kTileEdgeW);
	}
	bg->addWidget(aBox_, 0, 0);
	bg->addWidget(bBox_, 0, 1);
	bg->setColumnStretch(0, 1);
	bg->setColumnStretch(1, 1);

	buildMultiview();

	monitorSplit_ = new QSplitter(Qt::Horizontal, box);
	// Named: the gate tells the two splitters apart by name (the body one
	// divides height in Wide and width in Short; this one divides the
	// bays from the camera tiles).
	monitorSplit_->setObjectName(QStringLiteral("mrMonitorSplit"));
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
	// Named: the sheet keeps it transparent so no theme paints bands
	// behind the tiles.
	multiviewBox_->setObjectName(QStringLiteral("mrTiles"));
	multiviewGrid_ = new QGridLayout(multiviewBox_);
	multiviewGrid_->setContentsMargins(0, 0, 0, 0);
	multiviewGrid_->setSpacing(4); // artifact monitor: .cgrid/.crow{gap:4px}

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
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneChannels"), this);
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
	// ONE segmented control (artifact .seg: shared border, dividers between
	// keys), not three loose keys: the question — which bay do these keys
	// drive — is one question. segPos tells the sheet which corners stay
	// square; ⇄ sits apart, as drawn.
	sel->setObjectName(QStringLiteral("mrChanSeg"));
	auto *h = new QHBoxLayout(sel);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(0);
	chanSel_ = new QButtonGroup(this);
	chanSel_->setExclusive(true);
	// A|B, as drawn — the spec's label wins over the old comment's reading.
	const std::pair<const char *, int> chanChoices[] = {
		{"A|B", 2}, {"A", 0}, {"B", 1}};
	const char *segPos[] = {"first", "mid", "last"};
	int si = 0;
	for (const auto &[label, code] : chanChoices) {
		auto *b = new QPushButton(QString::fromUtf8(label), sel);
		b->setObjectName("mrChanSel");
		b->setProperty("segPos", segPos[si++]);
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
	// 25-125% on the panel (spec §4): the dial is for slow motion and a
	// touch over. The 2× fast-forward is off the panel now — still a
	// hotkey, and the engine still takes 5-400%, so applyReplaySpeed() can
	// be handed any of that from a Stream Deck.
	speed_->setRange(25, 125);
	speed_->setValue(100);
	speed_->setMinimumWidth(kSpeedSliderMinW); // artifact: 120px
	speed_->setMinimumHeight(26);
	speed_->setTickPosition(QSlider::TicksBelow);
	speed_->setTickInterval(25);
	speed_->setToolTip(obs_module_text("Dock.SpeedSliderHint"));
	speed_->setCursor(Qt::PointingHandCursor);

	// THE 100 TICK (artifact .track.vel .tick{left:75%}): a single 2x13px
	// mark where the default sits — Qt's native ticks draw every interval
	// instead (and are off). Transparent to the mouse: the dial drags under
	// it. Repositioned on every resize (eventFilter).
	speedTick_ = new QLabel(speed_);
	speedTick_->setObjectName(QStringLiteral("mrSpeedTick"));
	speedTick_->setFixedSize(2, 13);
	speedTick_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	// The tick tracks the dial's size, so the filter watches the dial.
	speed_->installEventFilter(this);
	positionSpeedTick();

	speedLbl_ = new QLabel(QStringLiteral("100%"), this);
	speedLbl_->setObjectName("mrTimecode");
	speedLbl_->setFont(QFont(monoFamily()));
	speedLbl_->setMinimumWidth(42);
	speedLbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	setKeyId(speedLbl_, QStringLiteral("speedReadout"));

	// LIVE, while the thumb moves. The speed of a replay is judged by
	// watching the picture, so applying it only on release meant aiming —
	// and applyReplaySpeed() now re-speeds the clip on air instead of
	// restarting it, which is what makes a dragged dial usable at all.
	connect(speed_, &QSlider::valueChanged, this, [this](int val) {
		speedLbl_->setText(QString::number(val) + QStringLiteral("%"));
		applyReplaySpeed(val);
	});
}

void MultiReplayDock::positionSpeedTick()
{
	// 100 on a 25..125 dial. Asked of the style itself
	// (sliderPositionFromValue), not of a fraction: the tick must sit where
	// THIS style puts the thumb centre at 100, Yami or not, and only the
	// style knows its own groove insets. Centred: the 2px tick straddles
	// the 11px thumb's middle.
	if (!speed_ || !speedTick_)
		return;
	const int w = speed_->width();
	const int h = speed_->height();
	const int span = std::max(1, w - 11);
	int pos = (3 * span) / 4;
	if (QStyle *st = speed_->style())
		pos = st->sliderPositionFromValue(speed_->minimum(),
						  speed_->maximum(), 100, span);
	speedTick_->move(std::max(0, pos + 11 / 2 - 1),
			 std::max(0, (h - 13) / 2));
}

// ---------------------------------------------------------------------------
// REVIEW panel (spec §4) — header · playback · modes · transport · trim
// ---------------------------------------------------------------------------
//
// One KeyBlock per group, in the reference panel's order. Every widget and
// every connection was in the old one-block buildTransport(); this splits it
// into the groups the spec draws and drops the ▾ play-options menu (spec §8:
// its three entries have dedicated keys, its Angolo fallback is the CAM key).

KeyBlock *MultiReplayDock::buildReviewHeader()
{
	auto *blk = new KeyBlock(QString(), this);

	// ■ REVIEW — the panel's name, in its header row (spec §4), not a
	// separate label above the column.
	auto *name = new QLabel(QStringLiteral("\xE2\x96\xA0 REVIEW"), this);
	name->setObjectName(QStringLiteral("mrPanelTitle"));
	name->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

	// Which event ▶ is about, padded like the table's id column, centred,
	// kept up to date by updateChannelStrip().
	reviewEventLbl_ = new QLabel(QStringLiteral("\xE2\x80\x94"), this);
	reviewEventLbl_->setObjectName(QStringLiteral("mrReviewEvent"));
	reviewEventLbl_->setAlignment(Qt::AlignCenter);
	reviewEventLbl_->setFont(QFont(monoFamily()));

	// IN OUTPUT — the toggle that decides whether a replay takes the
	// Program, at the far right of the header. Seeded from Settings, never
	// written back: setConfig() re-points the segment index and re-creates
	// the Branch Output filters, so a key pressed mid-match must not reach
	// it.
	toOutputBtn_ = statusToggle(Icon::ToOutput,
				    obs_module_text("Dock.ToOutput"), "toOutput",
				    obs_module_text("Dock.ToOutput"), this);
	toOutputBtn_->setChecked(
		ReplayCore::instance().getConfig().toOutputOnPlay);
	toOutputBtn_->setFixedHeight(kKeyH);

	// event id centred and growing, IN OUTPUT far right. The title rides
	// the event's own width (artifact .sub.review .hd packs right with the
	// title centred over the row): event takes the slack, name and key hug
	// the right edge.
	blk->setShapes({{Cell(reviewEventLbl_, 4), Cell(name, 1, false),
			 Cell(toOutputBtn_, 2, false)}},
		       {{Cell(reviewEventLbl_, 4), Cell(name, 1, false),
			 Cell(toOutputBtn_, 2, false)}});
	return blk;
}

KeyBlock *MultiReplayDock::buildPlayback()
{
	auto *blk = new KeyBlock(obs_module_text("Dock.ZonePlayback"), this);

	// ▶ PLAY — the biggest key on the panel: the one that takes the
	// Program, so the one the eye should land on without reading anything.
	// A filled green rectangle two key-rows tall carrying only the play
	// mark. Plain QPushButton, no menu (setMenu swallows click(), and a
	// hotkey and the gate reach it that way).
	auto *playSel = iconBtn(Icon::Play, "playEvents",
			       obs_module_text("Dock.PlaySelected"), this,
			       "mrAccent");
	setKeyIconRole(playSel, Icon::Play, IconRole::OnSignal, tintsFor(sc()),
		       22);
	playSel->setMinimumWidth(64);
	playSel->setMaximumHeight(QWIDGETSIZE_MAX);
	// Two transport rows tall BY CONSTRUCTION, not by slack: the pin is
	// rowSpan * mrKeyH, so without its own mrKeyH PLAY stands one pin row
	// (56px) and the "spans two rows" check only passed when the REVIEW
	// grid happened to have slack to lend. Gated: play_key_spans_two_rows.
	playSel->setProperty(kKeyHeightProperty, kTransportKeyH);
	connect(playSel, &QPushButton::clicked, this,
		&MultiReplayDock::playSelected);

	// NOW — a destination, not a modifier: drop the replay and go back to the
	// live edge. Keeps the WORD, drawn big and red even at rest, the same
	// size as PLAY (spec §4).
	nowBtn_ = new QPushButton(QStringLiteral("NOW"), this);
	nowBtn_->setObjectName("mrNow");
	nowBtn_->setProperty("live", false);
	nowBtn_->setCursor(Qt::PointingHandCursor);
	nowBtn_->setToolTip(obs_module_text("Dock.JumpToNow"));
	setKeyId(nowBtn_, QStringLiteral("now"));
	nowBtn_->setMinimumWidth(64); // same taglia as PLAY (artifact: same class)
	nowBtn_->setMaximumHeight(QWIDGETSIZE_MAX);
	// Same two-row pin as PLAY (see above): same taglia, same check.
	nowBtn_->setProperty(kKeyHeightProperty, kTransportKeyH);
	connect(nowBtn_, &QPushButton::clicked, this, [this]() {
		// the reference controller NOW: drop the replay and watch the
		// live edge again. The stretch armed on the bar stops being what
		// the play keys are about.
		pc().stopEvents();
		ReplayCore::instance().setFollowLive(true);
		clearFreeReview();
	});

	// TWO real grid rows so PLAY and NOW stand two key-rows tall (spec §4,
	// "▶ PLAY (grande)" · "NOW (grande, stessa taglia di PLAY)"). A 1 px
	// spacer widget holds row 1 open for the row-span-2 cells to reach into
	// — a bare null cell is only a hole and rows() would report 1.
	auto *rowFill = new QWidget(this);
	rowFill->setObjectName(QStringLiteral("mrRowFill"));
	rowFill->setFixedHeight(1);
	blk->setShapes({{Cell(playSel, 3, true, 2), Cell(nowBtn_, 3, true, 2)},
			{Cell(rowFill, 6, false)}},
		       {{Cell(playSel, 3, true, 2), Cell(nowBtn_, 3, true, 2)},
			{Cell(rowFill, 6, false)}});
	return blk;
}

KeyBlock *MultiReplayDock::buildModes()
{
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneModes"), this);
	// Fixed-width stack (artifact .modstack{width:152px}): both rows keep
	// their length with or without CAM.
	blk->setObjectName(QStringLiteral("mrModesBox"));
	blk->setFixedWidth(kModStackW);

	// ↺ "instantly play last event" — a distinct mark from LOOP (Icon::
	// PlayLast, not Icon::Loop) so the two do not read as the same thing.
	auto *lastBtn = iconBtn(Icon::PlayLast, "playLast",
				obs_module_text("Dock.PlayLast"), this);
	connect(lastBtn, &QPushButton::clicked, this, [this]() {
		std::string err;
		if (!pc().playLastEvent(
			    currentAngle1() - 1,
			    toOutputBtn_ && toOutputBtn_->isChecked(), err))
			showNotice(localizedError(err));
	});

	loopBtn_ = statusToggle(Icon::Loop, obs_module_text("Dock.Loop"), "loop",
				obs_module_text("Dock.Loop"), this);
	connect(loopBtn_, &QPushButton::toggled, this,
		[this](bool on) { pc().setLoop(on); });

	// MUTE — the replay input(s) sit muted in the OBS mixer. It LATCHES: a
	// new replay does not clear it, NOW/Live do not, only the operator does.
	muteBtn_ = statusToggle(Icon::Mute, obs_module_text("Dock.Mute"),
				"muteAudio", obs_module_text("Dock.MuteHint"),
				this);
	muteBtn_->setChecked(
		ReplayCore::instance().getConfig().muteReplayAudio);
	connect(muteBtn_, &QPushButton::toggled, this, [this](bool on) {
		for (Which w : targetChannels())
			ReplayChannel::instance(w).setMuted(on);
	});

	musicBtn_ = statusToggle(Icon::Music, obs_module_text("Dock.Music"),
				 "music", obs_module_text("Dock.MusicHint"),
				 this);
	connect(musicBtn_, &QPushButton::toggled, this, [this](bool on) {
		for (Which w : targetChannels())
			PlaybackCoordinator::instance(w).setMusicEnabled(on);
		// SAY IT NOW, not after the replay: the two ways music produces
		// nothing are both invisible while the key is pressed.
		if (!on)
			return;
		const std::string why = pc().musicProblem();
		if (!why.empty())
			showNotice(localizedError(why));
	});

	// CAM — pick an angle with the mouse when there are no multiview tiles
	// to click (Monitors off). Carries the same lazy "Angolo" list the
	// play-options ▾ used to; poll()/applyMonitorsRoom hides it while the
	// tiles are on screen. Opened by popupOnClick, never setMenu.
	camBtn_ = new QToolButton(this);
	camBtn_->setObjectName("mrCam");
	camBtn_->setText(QStringLiteral("CAM"));
	camBtn_->setCursor(Qt::PointingHandCursor);
	camBtn_->setToolTip(obs_module_text("Dock.Angle"));
	setKeyId(camBtn_, QStringLiteral("cam"));
	camBtn_->setFixedHeight(kKeyH);
	camBtn_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	{
		auto *menu = new QMenu(camBtn_);
		connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
			menu->clear();
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
				QAction *act = menu->addAction(label);
				connect(act, &QAction::triggered, this,
					[this, i]() { setAngle(i + 1); });
			}
		});
		popupOnClick(camBtn_, menu);
	}

	for (QPushButton *b : {lastBtn, loopBtn_, muteBtn_, musicBtn_})
		b->setFixedHeight(kKeyH);

	// Fixed-width stack, equal rows with or without CAM (spec §4). Row 0:
	// ↺ · LOOP. Row 1: MUTE · ♪ · CAM.
	blk->setShapes({{Cell(lastBtn, 3), Cell(loopBtn_, 3)},
			{Cell(muteBtn_, 2), Cell(musicBtn_, 2), Cell(camBtn_, 2)}},
		       {{Cell(lastBtn, 3), Cell(loopBtn_, 3)},
			{Cell(muteBtn_, 2), Cell(musicBtn_, 2),
			 Cell(camBtn_, 2)}});
	// Compact (Short ~210px column): all five on one line. LOOP and music
	// have no hotkeys, so unlike trim and the slider this row cannot hide.
	blk->setCompactShapes({{Cell(lastBtn, 3), Cell(loopBtn_, 3),
				Cell(muteBtn_, 2), Cell(musicBtn_, 2),
				Cell(camBtn_, 2)}});
	return blk;
}

KeyBlock *MultiReplayDock::buildReviewTransport()
{
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneTransport"), this);

	// The two frame steps, side by side, in the order the timeline runs.
	// The step BACK is not the forward one with the sign changed — see
	// stepFrameBackward.
	auto *stepBackBtn = iconBtn(Icon::StepBack, "stepBack",
				    obs_module_text("Dock.StepBack"), this);
	connect(stepBackBtn, &QPushButton::clicked, this,
		[this]() { stepFrameBackward(); });
	auto *stepBtn = iconBtn(Icon::StepFwd, "stepFwd",
				obs_module_text("Dock.StepFwd"), this);
	connect(stepBtn, &QPushButton::clicked, this,
		[this]() { stepFrameForward(); });
	// HELD DOWN, not tapped. 150 ms, not Qt's 100: a step BACK decodes a
	// whole GOP (~100 ms on an iGPU).
	for (QPushButton *b : {stepBackBtn, stepBtn}) {
		b->setAutoRepeat(true);
		b->setAutoRepeatDelay(400);
		b->setAutoRepeatInterval(150);
	}

	// ◀ ▶ ■ — reverse, play/pause, stop.
	auto *revBtn = iconBtn(Icon::Reverse, "playReverse",
			       obs_module_text("Dock.PlayReverse"), this);
	connect(revBtn, &QPushButton::clicked, this,
		[this]() { playSelectedReverse(); });

	// ONE MARK FOR A KEY THAT IS BOTH: a play at rest, a pause while a clip
	// runs.
	playPauseBtn_ = iconBtn(Icon::Play, "playPause",
				obs_module_text("Dock.PlayPause"), this,
				"mrPlay");
	// Stop must be a key of its own: a free review runs until it is
	// stopped, and ▶ cannot be that key (while something plays it is a
	// PAUSE, which holds the picture instead of giving Program back).
	// EXACTLY ONE button in this dock carries this glyph — the gate finds
	// Stop by it.
	stopBtn_ = iconBtn(Icon::Stop, "stop", obs_module_text("Dock.Stop"),
			   this);

	// ENLARGED (artifact .key.tlg 40px tall, 42 wide): icon-only keys pressed
	// under time pressure. The height rides mrKeyH (KeyBlock::apply pins it),
	// not setFixedHeight — a fixed height here would fight the pin.
	for (QPushButton *b : {stepBackBtn, stepBtn, revBtn, playPauseBtn_,
			       stopBtn_}) {
		b->setProperty(kKeyHeightProperty, kTransportKeyH);
		b->setMinimumWidth(42);
	}

	connect(playPauseBtn_, &QPushButton::clicked, this, [this]() {
		// A REAL pause: the clip freezes on the frame it is showing and
		// the next press carries on from there.
		for (Which w : targetChannels()) {
			auto &pcw = PlaybackCoordinator::instance(w);
			auto &chw = ReplayChannel::instance(w);
			if (chw.playing()) {
				pcw.setPaused(!chw.paused());
				continue;
			}
			// NOTHING RUNNING, AND THE BAR IS ON FOOTAGE NOBODY
			// MARKED: play THAT, off air. It runs until Stop and
			// never touches Program. Once, for the bay the selector
			// is on: a free review is a range, and playing it on
			// both bays would be two decoders showing one picture.
			if (playheadIsFreeFootage()) {
				if (w == targetChannels().front())
					playFreeReview(/*toOutput*/ false);
				continue;
			}
			// Otherwise ▶ means "play what is selected".
			ReplayCore::instance().setFollowLive(false);
			replayCurrentOn(w);
		}
	});
	connect(stopBtn_, &QPushButton::clicked, this,
		[this]() { stopPlayback(); });

	// Six columns: the two frame steps, a margin, then ◀ ▶ ■ (spec §4).
	blk->setShapes({{Cell(stepBackBtn), Cell(stepBtn), Cell(nullptr),
			 Cell(revBtn), Cell(playPauseBtn_), Cell(stopBtn_)}},
		       {{Cell(stepBackBtn), Cell(stepBtn), Cell(nullptr),
			 Cell(revBtn), Cell(playPauseBtn_), Cell(stopBtn_)}});
	return blk;
}

KeyBlock *MultiReplayDock::buildTrim()
{
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneTrim"), this);

	// Move the SELECTED event's in or out point to where the position bar
	// stands. A mark taken live is late by definition; until this the only
	// fix was delete-and-remark from a scrub, which loses the angles and
	// the comments. Frame nudges are hotkeys (registerDockHotkeys).
	auto *trimIn = iconBtn(Icon::TrimIn, "trimIn",
			       obs_module_text("Dock.TrimInHint"), this);
	connect(trimIn, &QPushButton::clicked, this,
		[this]() { setSelectedPoint(true); });
	auto *trimOut = iconBtn(Icon::TrimOut, "trimOut",
				obs_module_text("Dock.TrimOutHint"), this);
	connect(trimOut, &QPushButton::clicked, this,
		[this]() { setSelectedPoint(false); });
	for (QPushButton *b : {trimIn, trimOut}) {
		// 60px wide fixed and equal, 40 tall like transport (.key.tlg).
		b->setProperty(kKeyHeightProperty, kTransportKeyH);
		b->setMinimumWidth(kTrimKeyW);
	}

	// Same fixed width, side by side (spec §4).
	blk->setShapes({{Cell(trimIn), Cell(trimOut)}},
		       {{Cell(trimIn), Cell(trimOut)}});
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

	// (The health badge used to sit here. Spec §8 moves it to the footer of
	// the MARCA panel — set up in buildRecBlock, placed by
	// TwoPanelStrip::setFooters.)

	statusNotice_ = new QLabel(statusBar_);
	statusNotice_->setObjectName(QStringLiteral("mrChanStrip"));
	statusNotice_->setTextFormat(Qt::PlainText);
	// Ignored horizontally: what it says changes every tick and its natural
	// width would otherwise be a floor under the whole panel.
	statusNotice_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	h->addWidget(statusNotice_, 1);

	// (loop · music · mute · in output used to sit here. Spec §4 makes them
	// REVIEW's "modes" — loop/mute/music in buildModes(), IN OUTPUT in the
	// REVIEW header. This line is now only the notice and the speed
	// read-out.)
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

	// -- THE COMMAND PANEL: MARCA | REVIEW ---------------------------
	// TWO PANELS, not six folding sections (spec §4). MARCA is what you do to
	// the live feed — arm, mark, pick a bay; REVIEW is what you do to the
	// replay — drive it, pick a mode, set a speed. Wide puts them side by
	// side (2:3), Short stacks them, Tall swaps them behind a tab bar. The
	// blocks themselves are the ones the strip already had — this changes how
	// they are grouped and arranged, not what is in them.
	strip_ = new TwoPanelStrip(box);

	// HEADERS — equal fixed height on both panels (spec §4). buildRecBlock()
	// carries the MARCA name + REC + clock and also creates healthBtn_ for
	// the MARCA footer; buildReviewHeader() carries ■ REVIEW + event id +
	// IN OUTPUT.
	strip_->setHeaders(buildRecBlock(), buildReviewHeader());

	// MARCA body — three boxed sub-sections that line up with REVIEW's grid
	// rows: Clip rapida (row 0), Clip manuale (row 1), Canali replay (row 2).
	strip_->addToMarca(buildQuickClip());
	strip_->addToMarca(buildManualClip());
	strip_->addToMarca(buildAngleMatrix()); // sets angleBlock_

	// REVIEW body — a 2x3 grid: [playback | modes] / [transport | trim] /
	// [speed spanning].
	speedBlock_ = buildSpeedBlock();
	strip_->setReviewGrid(buildPlayback(), buildModes(),
			      buildReviewTransport(), buildTrim(), speedBlock_);
	// (Export / reorder / delete-all are a toolbar over the event table now,
	// buildTableTools() — spec §3. Off the strip is also what keeps the
	// panel's floor under what a floating window restores to.)

	// Tall used to collapse bay/clips/speed behind a "more" menu; the tab bar
	// does that job now, so the block is not built.
	moreBlock_ = nullptr;

	// ── THE GREEN ON-AIR BAND: it is REVIEW's footer (spec §0/§4) ─────
	// What is playing, on which angle, how much is left, at what speed —
	// with the fill as its progress. The >> beside it drops the clip and
	// takes the next item of the queue, which may be another angle of the
	// same event or the next event: the operator who has seen enough of a
	// replay should not have to sit through the rest of it, and Stop is a
	// different thing (it kills the sequence).
	QWidget *reviewFoot = nullptr;
	{
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

		auto *wrap = new QWidget(box);
		wrap->setObjectName(QStringLiteral("mrReviewFoot"));
		auto *wl = new QHBoxLayout(wrap);
		wl->setContentsMargins(0, 0, 0, 0);
		wl->addWidget(clipBar_, 1);
		// NO CAPTION OVER IT. The band is green, full width and the only
		// thing on the panel that fills with colour as a clip runs; a
		// heading saying "ON AIR" above it was a line of height spent
		// telling the operator what he could already see.
		reviewFoot = wrap;
	}

	// FOOTERS: the health badge under MARCA (spec §8), the on-air band under
	// REVIEW (spec §0). The band is a real footer of the panel now, not a
	// full-width row below it. The health badge is hidden until there is
	// something to say, so it rides in a fixed-height carrier — the row is
	// reserved either way and MARCA's body lines up with REVIEW's (spec §4:
	// "in REVIEW il footer è riservato ma invisibile", the same the other
	// way round).
	auto *marcaFoot = new QWidget(box);
	marcaFoot->setObjectName(QStringLiteral("mrMarcaFoot"));
	// 26px of badge + the sheet's 6px padding-top above the edge: 32 total
	// (artifact .subfoot{min-height:26px;padding-top:6px}). A fixed 26 with
	// padding would clip the 26px badge it carries.
	marcaFoot->setFixedHeight(kKeyH + 6);
	auto *mfl = new QHBoxLayout(marcaFoot);
	mfl->setContentsMargins(0, 0, 0, 0);
	// Centred (artifact .subfoot{justify-content:center}): a lone badge
	// hugging the left edge reads as a layout that gave up halfway.
	mfl->addStretch(1);
	mfl->addWidget(healthBtn_, 0, Qt::AlignVCenter);
	mfl->addStretch(1);
	strip_->setFooters(marcaFoot, reviewFoot);

	v->addWidget(strip_);

	// ── THE STATUS LINE ─────────────────────────────────────────────
	// What the NEXT replay will run under: which list and event the
	// transport is about, where the playhead is, the answer to a key just
	// pressed. It OWNS the modes (loop, music, mute, in output) rather than
	// mirroring keys that live elsewhere.
	v->addWidget(buildStatusBar(box));

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
	// .tb-ico{width:26px;height:25px}. This REPLACES a setFixedHeight(kKeyH)
	// that used to sit a few lines below and silently won, because it ran
	// last: the key was set to 25 and measured 26, and the gate check said so.
	gear->setFixedSize(kToolIcoW, kToolIcoH);
	setKeyIcon(gear, Icon::Gear, tintsFor(sc()), 15);
	setKeyId(gear, QStringLiteral("settings"));
	gear->setCursor(Qt::PointingHandCursor);
	gear->setToolTip(obs_module_text("Dock.Settings"));
	{
		// (Nuovo / Apri… moved to the project selector button, spec §1.
		// The gear keeps the configuration of the whole panel — setup,
		// settings, tags, chapters.)
		auto *menu = new QMenu(gear);
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

	// M4: the health badge. It reports how the take is going, so it sits in
	// the footer of MARCA — the panel where the take is armed
	// (TwoPanelStrip::setFooters puts it there). Hidden unless there is
	// something to say (see poll()).
	healthBtn_ = new QPushButton(this);
	healthBtn_->setObjectName("mrHealth");
	healthBtn_->setCursor(Qt::PointingHandCursor);
	healthBtn_->setFlat(true);
	healthBtn_->setProperty("dense", true);
	healthBtn_->setMinimumHeight(0);
	healthBtn_->setFixedHeight(kKeyH);
	healthBtn_->hide();
	// AMBER, like the border and the number beside it. The badge is only ever
	// on screen when it has something to report, so its mark has no resting
	// chrome state to be drawn in.
	setKeyIconRole(healthBtn_, Icon::Health, IconRole::Warn, tintsFor(sc()),
		       11);
	setKeyId(healthBtn_, QStringLiteral("health"));
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

	// MARCA — the panel names itself in its header row (spec §4), beside a
	// COMPACT record key and the clock + room-left, all on ONE line so the
	// header is the same fixed height as REVIEW's.
	auto *name = new QLabel(QStringLiteral("MARCA"), this);
	name->setObjectName(QStringLiteral("mrPanelTitle"));
	name->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	// REC's height is pinned by KeyBlock::apply() (which follows gallery
	// scale) — no setFixedHeight here, or it would stop growing in the
	// full-screen gallery view the gate checks.
	statusLbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	blk->setShapes({{Cell(name, 1, false), Cell(recBtn_, 1, false),
			 Cell(clockLbl_, 1, false), Cell(statusLbl_, 1)}},
		       {{Cell(name, 1, false), Cell(recBtn_, 1, false),
			 Cell(clockLbl_, 1, false), Cell(statusLbl_, 1)}});
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
	// 25 / 50 / 75 / 100 / 125 (spec §4). Slow motion and a touch over;
	// the 2× is a hotkey now, not a chip.
	const std::pair<int, const char *> speedPresets[] = {
		{25, "25%"}, {50, "50%"}, {75, "75%"}, {100, "100%"}, {125, "125%"}};
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
			speedLbl_->setText(QString::number(p) +
					   QStringLiteral("%"));
			applyReplaySpeed(p);
		});
		b->setFixedHeight(kKeyH);
		chips << b;
	}
	// ONE WIDTH for the five of them: "25%" is three characters and "100%"
	// is four, so left to their labels they came out a ragged row — five
	// sizes for five values of one setting, with the widest reading as the
	// most important. 34px floor (artifact .seg .key).
	for (QPushButton *b : chips)
		b->setMinimumWidth(34);
	equaliseKeyWidths(chips);

	speed_->setMinimumWidth(kSpeedSliderMinW);
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
			 Cell(chips[3]), Cell(chips[4])},
			{Cell(speed_, 4), Cell(speedLbl_, 1, false)}},
		       // The five presets stay on ONE row folded as well. They are
		       // the narrowest keys on the panel and splitting them across
		       // two rows bought nothing but a line — and it broke the run
		       // of values an operator reads left to right.
		       {{Cell(chips[0]), Cell(chips[1]), Cell(chips[2]),
			 Cell(chips[3]), Cell(chips[4])},
			{Cell(speed_, 4), Cell(speedLbl_, 1, false)}});
	return blk;
}

// ---------------------------------------------------------------------------
// THE RUNNING ORDER, and the actions that have no key of their own
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The event-table toolbar (spec §3): ⇅ Tempo · ▲ ▼ · Elimina tutto · ⤓ Esporta
// ---------------------------------------------------------------------------
//
// This was a KeyBlock in the command panel ("clips"). Spec §3 puts it over
// the table, with the tabs — it is about the list, not about a clip on air —
// and dropping it from the strip is also what brought the panel's floor back
// under what a floating window restores to. The ⋯ (Duplica / Elimina) menu
// is gone: those two live on the table's own right-click menu.
QWidget *MultiReplayDock::buildTableTools()
{
	auto *box = new QWidget(this);
	box->setObjectName(QStringLiteral("mrTableTools"));
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(8); // artifact tabella: .tbar2{gap:8px}
	// Event counter (artifact tabella D1: "7 / 24" — events in this list /
	// events everywhere). The list tabs live in the toolbar (decided,
	// gated); the count rides the tools bar's left end, where D1 parks it.
	// Refreshed with the rows (updateEventCount, on every refreshEvents).
	eventCount_ = new QLabel(box);
	eventCount_->setObjectName(QStringLiteral("mrEventCount"));
	setKeyId(eventCount_, QStringLiteral("eventCount"));
	eventCount_->setToolTip(obs_module_text("Dock.EventCountHint"));
	h->addWidget(eventCount_);
	const auto mkSep = [box]() -> QWidget * {
		auto *s = new QWidget(box);
		s->setObjectName(QStringLiteral("mrSepLine"));
		s->setFixedWidth(1);
		s->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
		return s;
	};
	h->addWidget(mkSep());

	// ⇅ Tempo — chronological auto-sort. Compact label (artifact D1), full
	// sentence stays in the tooltip: a 22-character word in a .tk key is a
	// key twice the width of its neighbours.
	auto *sortBtn = new QPushButton(obs_module_text("Dock.SortTimeShort"), box);
	sortBtn->setObjectName(QStringLiteral("mrToggle"));
	sortBtn->setCheckable(true);
	sortBtn->setCursor(Qt::PointingHandCursor);
	sortBtn->setToolTip(obs_module_text("Dock.SortByTimeHint"));
	setKeyId(sortBtn, QStringLiteral("sortTime"));
	sortBtn->setChecked(
		ReplayCore::instance().getConfig().sortEventsByTime);
	sortBtn->setFixedHeight(kKeyH);
	useTextGlyph(sortBtn, QStringLiteral("⇅"));
	connect(sortBtn, &QPushButton::toggled, this, [this](bool on) {
		auto &core = ReplayCore::instance();
		Config cfg = core.getConfig();
		if (cfg.sortEventsByTime == on)
			return;
		cfg.sortEventsByTime = on;
		core.setConfig(cfg);
		refreshEvents();
	});
	h->addWidget(sortBtn);

	// ▲ ▼ — the running order is the operator's. Keys rather than a drag: a
	// drag inside a table whose cells are all editable is one slip from
	// starting an edit instead.
	for (const auto &mv : {std::pair<Icon, int>{Icon::MoveUp, -1},
			       std::pair<Icon, int>{Icon::MoveDown, +1}}) {
		const int delta = mv.second;
		auto *b = iconBtn(mv.first, delta < 0 ? "moveUp" : "moveDown",
				  obs_module_text(delta < 0 ? "Dock.MoveUp"
							    : "Dock.MoveDown"),
				  box);
		connect(b, &QPushButton::clicked, this,
			[this, delta]() { moveSelectedEvent(delta); });
		b->setFixedHeight(kKeyH);
		h->addWidget(b);
	}
	h->addWidget(mkSep());

	h->addStretch(1);

	// Elimina tutto — its own key now (the ⋯ menu it used to hide in is
	// gone). Amber, and it confirms; the buttons are OURS so the panel's
	// locale wins over Qt's.
	auto *delAll = new QPushButton(obs_module_text("Dock.DeleteAll"), box);
	delAll->setObjectName(QStringLiteral("mrDanger"));
	delAll->setCursor(Qt::PointingHandCursor);
	setKeyId(delAll, QStringLiteral("deleteAll"));
	delAll->setFixedHeight(kKeyH);
	connect(delAll, &QPushButton::clicked, this, [this]() {
		QMessageBox box(this);
		box.setWindowTitle("obs-multireplay");
		box.setText(obs_module_text("Dock.DeleteAllConfirm"));
		QPushButton *yes = box.addButton(obs_module_text("Dock.Yes"),
						 QMessageBox::YesRole);
		box.addButton(obs_module_text("Dock.No"), QMessageBox::NoRole);
		box.exec();
		if (box.clickedButton() != yes)
			return;
		pc().stopEvents();
		EventStore::instance().clearAll();
	});
	h->addWidget(delAll);

	// ⤓ Esporta — asks on the press whether it is one clip or the whole
	// selection as one file.
	exportBtn_ = buildExportKey();
	h->addWidget(exportBtn_);
	return box;
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
	// ITS OWN NAME. It borrowed "mrGear" for the look, and that made TWO
	// widgets answer to one name: findChild returns whichever Qt walks into
	// first, so the gate measuring "the gear" was measuring this button and
	// reporting 26x26 for a key that had been set to 26x25. A shared
	// objectName is not an identifier. The look is shared in the sheet
	// instead, where sharing a look is what a selector list is for.
	btn->setObjectName("mrMore");
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

// TALL USED TO COLLAPSE bay/clips/speed behind a "more" menu, because six
// folding sections in a narrow column cost six lines. The MARCA | REVIEW
// panel (spec §4) puts REVIEW behind a tab bar in Tall instead — all of its
// blocks are on the one tab, none hidden — so there is nothing to collapse.
// Kept as a guarded no-op: moreBlock_ is null now, and the three blocks stay
// visible in every mode.
void MultiReplayDock::applyTallCollapse(bool tall)
{
	if (tall == tallCollapsed_)
		return;
	tallCollapsed_ = tall;
	if (moreBlock_) // not built any more; here in case it comes back
		moreBlock_->setVisible(tall);
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

// MARCA body box 0 — Clip rapida: the last N seconds, whole. Big keys, the
// function colour (spec §4).
KeyBlock *MultiReplayDock::buildQuickClip()
{
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneQuickClip"), this);
	QVector<QPushButton *> presets;
	for (int sec : {5, 10, 20}) {
		// A MINUS SIGN, not a hyphen: these read as durations before an
		// instant, and U+2212 is the character that says so.
		auto *b = compactBtn(QString("\xE2\x88\x92%1s").arg(sec), this,
				     "mrFn");
		setKeyId(b, QString("mark%1").arg(sec));
		// Big keys (artifact .key.big 42x78): height rides mrKeyH, width
		// is a minimum (labels differ per locale).
		b->setProperty(kKeyHeightProperty, kClipKeyH);
		b->setMinimumWidth(78);
		connect(b, &QPushButton::clicked, this, [this, sec]() {
			const int64_t t = markTimeNs();
			if (!markable(t))
				return;
			EventStore::instance().markInOut(t, sec,
							 currentAngle1() - 1);
			refreshEvents();
		});
		presets << b;
	}
	blk->setShapes({{Cell(presets[0]), Cell(presets[1]), Cell(presets[2])}},
		       {{Cell(presets[0]), Cell(presets[1]), Cell(presets[2])}});
	return blk;
}

// MARCA body box 1 — Clip manuale: take a point, close it, throw it away.
KeyBlock *MultiReplayDock::buildManualClip()
{
	auto *blk = new KeyBlock(obs_module_text("Dock.ZoneManualClip"), this);

	// IN AND OUT STAY WORDS — every mark on a timeline is a bracket, and a
	// panel whose two most-pressed keys are two brackets is one you hover to
	// use. They are COMMANDS, not actions: neutral, not the filled green of
	// "play the events".
	auto *in = compactBtn(obs_module_text("Dock.MarkIn"), this);
	setKeyId(in, QStringLiteral("markIn"));
	in->setProperty(kKeyHeightProperty, kClipKeyH);
	in->setMinimumWidth(78);
	auto *out = compactBtn(obs_module_text("Dock.MarkOut"), this);
	setKeyId(out, QStringLiteral("markOut"));
	out->setProperty(kKeyHeightProperty, kClipKeyH);
	out->setMinimumWidth(78);
	connect(in, &QPushButton::clicked, this, [this]() {
		const int64_t t = markTimeNs();
		if (!markable(t))
			return;
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

	// ✕ Annulla — the one destructive key of the group, AMBER (artifact
	// .key.xcancel / spec §4): clearing a mark destroys work but takes
	// nothing on air, and red has one meaning here. Small (.key.sm 24x32).
	auto *cancel = iconBtn(Icon::Cancel, "markCancel",
			       obs_module_text("Dock.Cancel"), this, "mrWarn");
	setKeyIconRole(cancel, Icon::Cancel, IconRole::Warn, tintsFor(sc()));
	cancel->setProperty(kKeyHeightProperty, 24);
	cancel->setMinimumWidth(32);
	connect(cancel, &QPushButton::clicked, this, [this]() {
		EventStore::instance().markCancel();
		refreshEvents();
	});

	// Heights ride mrKeyH (set above), pinned by KeyBlock::apply().
	blk->setShapes({{Cell(in), Cell(out), Cell(cancel)}},
		       {{Cell(in), Cell(out), Cell(cancel)}});
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
	v->setSpacing(0); // barretta + tabella joined: one frame, no gap

	events_ = new QTableWidget(this);
	events_->setObjectName("mrEvents");
	// The table's own mono 11px (artifact tabella .etbl/.68rem), stated on
	// the WIDGET rather than only in the sheet: items inherit it at paint,
	// and a font on the widget is a question the gate can answer from
	// outside (the mockup's one-size check reads it). Same number as the
	// ::item rule below — the two say one thing, twice, on purpose.
	{
		QFont tf(monoFamily());
		tf.setPixelSize(11);
		events_->setFont(tf);
	}
	events_->setSelectionBehavior(QAbstractItemView::SelectRows);
	events_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// The speed cell and the per-camera comments are edited in place; the
	// per-camera enable box is a click on its indicator.
	events_->setEditTriggers(QAbstractItemView::DoubleClicked |
				 QAbstractItemView::EditKeyPressed);
	events_->verticalHeader()->setVisible(false);
	// 24, the Normale floor: applyTableDensity() re-asserts the floor for
	// the configured density right after construction, so this is only the
	// starting value — but it has to be a floor, not the old historic 30,
	// or the first paint shows 30px rows until the first theme pass.
	events_->verticalHeader()->setDefaultSectionSize(24);
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
	// The left-edge stripe on the id of whatever event is in PGM (spec §3) —
	// see OnAirIdDelegate's own note (dock-internal.hpp) for why it needs a
	// delegate and not just an item property.
	events_->setItemDelegateForColumn(kColId, new OnAirIdDelegate(events_));
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
	v->addWidget(buildTableTools());
	v->addWidget(events_, 1);

	// There is no inspector panel any more. It existed for the one edit the
	// table could not hold — the per-angle speed — and that now has its own
	// half-column next to the enable box of the camera it belongs to. A panel
	// that duplicates the table costs height, costs a rebuild per selection
	// change, and asks the operator to look somewhere other than at the row.

	return box;
}

} // namespace multireplay
