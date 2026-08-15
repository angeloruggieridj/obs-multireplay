/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multireplay-dock.hpp"
#include "qt-display.hpp"
#include "replay-core.hpp"
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
#include <QAbstractButton>
#include <QDateTime>
#include <QPushButton>
#include <QTabBar>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
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
#include <QListWidget>
#include <QStackedWidget>
#include <QMessageBox>
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
#include <cstring>

namespace multireplay {

// ---------------------------------------------------------------------------
// Dock-wide stylesheet — the broadcast replay controller's palette.
//
// The colours are SAMPLED from the reference screenshots in "reference-gui/", not
// invented: an operator coming from the reference controller reads this panel by colour before he
// reads it by label, and "green means the angle I am on, orange means the row I
// have selected" is muscle memory worth more than any house style. Nothing here
// is copied from the reference controller itself — these are hex values in our own stylesheet.
//
// Palette (sampled):
//   #1D3D74  table header blue      #176533  active-angle header green
//   #DB5026  selected row orange    #199847  position bar, played
//   #00121C  row odd (navy black)   #146433  position bar, remaining
//   #002A42  row even               #116B35  angle enabled (checkbox)
//   #A81C1C  Live / REC red         #0c0c0c  dock background
static const char *kDockStyle = R"QSS(
/* ── base ─────────────────────────────────────────────── */
#MultiReplayDock { background: #0c0c0c; }
#MultiReplayDock QLabel { color: #d0d0d0; background: transparent; }

/* labels */
QLabel#mrMuted      { color: #787878; font-size: 10px; }
QLabel#mrTimecode   { color: #c8c8c8; font-size: 12px; font-weight: 700;
                      letter-spacing: 0.3px; }
QLabel#mrSectionLabel { color: #686868; font-size: 9px; font-weight: 700;
                        letter-spacing: 1.4px; text-transform: uppercase; }
/* wall clock over the "remaining" line, the reference controller-red while a take is running */
QLabel#mrClock      { color: #6a6a6a; font-size: 10px; }
QLabel#mrClock[rec="true"] { color: #e03030; font-weight: 700; }

/* ── generic buttons ───────────────────────────────────── */
#MultiReplayDock QPushButton {
	background: #1e1e1e; color: #b0b0b0;
	border: 1px solid #2c2c2c; border-radius: 4px;
	padding: 3px 9px; font-size: 11px; min-height: 24px;
}
#MultiReplayDock QPushButton:hover  { background: #282828; border-color: #424242; }
#MultiReplayDock QPushButton:pressed { background: #141414; }
#MultiReplayDock QPushButton:disabled { color: #303030; border-color: #1e1e1e; }

/* ── transport step / icon buttons ────────────────────── */
QPushButton#mrTransport {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 5px;
	color: #c8c8c8; font-size: 14px;
	min-width: 30px; min-height: 28px; padding: 0;
}
QPushButton#mrTransport:hover { background: #222222; border-color: #424242; color: #f0f0f0; }

/* play/pause */
QPushButton#mrPlay {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 5px;
	color: #c8c8c8; font-size: 16px;
	min-width: 38px; min-height: 28px; padding: 0;
}
QPushButton#mrPlay:hover { background: #222222; border-color: #424242; color: #f0f0f0; }
QPushButton#mrPlay[playing="true"] { background: #0c2212; border-color: #1c8a38; color: #28b050; }
QPushButton#mrPlay[playing="true"]:hover { background: #102818; border-color: #22a040; }

/* NOW / live-edge */
QPushButton#mrNow {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 5px;
	font-weight: 700; font-size: 10px; letter-spacing: 0.8px;
	min-height: 28px; min-width: 36px; color: #484848;
}
QPushButton#mrNow[live="true"] { background: #280808; border-color: #c02020; color: #e03030; }

/* ── the reference controller "Live" mode toggle — red when marks are taken as they happen ── */
QPushButton#mrLive {
	background: #181818; color: #6a6a6a;
	border: 1px solid #2c2c2c; border-radius: 3px;
	font-weight: 700; font-size: 11px; letter-spacing: 0.6px;
	min-height: 22px; min-width: 54px; padding: 2px 14px;
}
QPushButton#mrLive:hover { border-color: #424242; color: #9a9a9a; }
QPushButton#mrLive:checked {
	background: #A81C1C; color: #ffffff; border-color: #d03030;
}

/* ── latching toggles (Loop · music · to output) ─────────── */
QPushButton#mrToggle {
	background: #181818; color: #6a6a6a;
	border: 1px solid #2c2c2c; border-radius: 3px;
	font-size: 10px; min-height: 26px; padding: 2px 9px;
}
QPushButton#mrToggle:hover { border-color: #424242; color: #9a9a9a; }
QPushButton#mrToggle:checked {
	background: #176533; color: #ffffff; border-color: #22a04a;
}

/* ── list tabs (the reference controller: one tab per event list) ─────────────── */
QTabBar#mrListTabs { background: transparent; }
/* No font-size here on purpose: the tab font is set on the WIDGET (see
   buildToolbar). A size that lives only in the stylesheet is a size nothing
   outside the painter can measure, and "is this tab wide enough for its own
   name" is exactly the question the gate has to answer. */
QTabBar#mrListTabs::tab {
	background: #141414; color: #8a8a8a;
	border: 1px solid #232323; border-bottom: 0;
	padding: 3px 9px; margin-right: 1px; min-width: 16px;
}
QTabBar#mrListTabs::tab:hover { background: #1e1e1e; color: #c0c0c0; }
QTabBar#mrListTabs::tab:selected {
	background: #1D3D74; color: #ffffff; border-color: #2a5296;
}

/* ── settings dialog: side menu + pages ─────────────────────────────── */
QListWidget#mrSettingsNav {
	background: #121212; color: #9a9a9a;
	border: 0; border-right: 1px solid #232323;
	outline: 0; font-size: 11px;
}
QListWidget#mrSettingsNav::item { padding: 8px 12px; border: 0; }
QListWidget#mrSettingsNav::item:hover { background: #1c1c1c; color: #d0d0d0; }
QListWidget#mrSettingsNav::item:selected {
	background: #1D3D74; color: #ffffff;
}
QLabel#mrSettingsTitle {
	color: #e0e6ee; font-size: 14px; font-weight: 700;
}
QLabel#mrSettingsBlurb { color: #7a8490; font-size: 10px; }

/* ── multiview tiles (the reference controller's camera thumbnails beside the A output) ── */
QWidget#mrTile { background: #000000; }
/* the reference controller captions its thumbnails with a blue band; the angle being watched turns
   green and the one on air red, so the operator reads tally from the picture
   itself instead of correlating it with the angle buttons. */
QLabel#mrTileCap {
	background: #1D3D74; color: #dfe8f6;
	font-size: 9px; font-weight: 700; padding: 1px 4px;
}
QLabel#mrTileCap[tally="pvw"] { background: #176533; color: #ffffff; }
QLabel#mrTileCap[tally="pgm"] { background: #A81C1C; color: #ffffff; }
QLabel#mrTileCap[tally="replay"] { background: #3a2d10; color: #ffd07a; }

/* ── channel strip under the preview (the reference controller green info band) ─ */
QLabel#mrChanBadge {
	background: #0e4523; color: #ffffff;
	font-weight: 700; font-size: 11px; padding: 2px 7px;
}
/* The letter under each output box. the reference controller puts A on a green bar and B on a
   blue one, and that colour is how the operator tells the two boxes apart
   from across the room — faster than reading a letter. The one being driven
   by the keys is the bright one. */
QLabel#mrChanTag {
	background: #12161c; color: #6b7787;
	font-weight: 700; font-size: 10px; padding: 1px 0;
}
QLabel#mrChanTag[chan="A"][active="true"] { background: #146433; color: #ffffff; }
QLabel#mrChanTag[chan="B"][active="true"] { background: #1d3d74; color: #ffffff; }
QLabel#mrChanStrip {
	background: #146433; color: #dff3e2;
	font-size: 10px; padding: 2px 7px;
}

/* REC button */
QPushButton#mrRec {
	font-weight: 700; font-size: 12px; letter-spacing: 0.6px;
	border-radius: 4px; min-height: 28px; padding: 3px 14px;
}
QPushButton#mrRec[recording="false"] {
	background: #181010; color: #b03030; border: 1px solid #2c1818;
}
QPushButton#mrRec[recording="false"]:hover { background: #1e1010; border-color: #4a2020; }
QPushButton#mrRec[recording="true"] {
	background: #640808; color: #ffffff; border: 1px solid #c02020;
}
QPushButton#mrRec[recording="true"]:hover { background: #740e0e; }

/* Channel selector A|B / A / B and the swap. Small, square and always visible:
   it is the answer to "where is this key going", and an operator who has to
   look for it has already pressed something on the wrong channel. */
QPushButton#mrChanSel {
	background: #14161a; color: #7a879a; border: 1px solid #262b33;
	border-radius: 3px; padding: 2px 6px; font-weight: 700; font-size: 11px;
	min-height: 22px;
}
QPushButton#mrChanSel:hover { background: #1b1f26; color: #c8d2de; }
QPushButton#mrChanSel:checked {
	background: #1d3d74; color: #ffffff; border-color: #2f5da8;
}

/* M4 health badge: amber = degraded, red = this take is not usable. It sits
   beside REC and is hidden entirely while there is nothing to report. */
QPushButton#mrHealth {
	font-weight: 700; font-size: 12px; border-radius: 4px;
	min-height: 28px; padding: 3px 8px;
}
QPushButton#mrHealth[level="warn"] {
	background: #2a2008; color: #e0a020; border: 1px solid #6a5010;
}
QPushButton#mrHealth[level="bad"] {
	background: #3a0c0c; color: #ff6a4a; border: 1px solid #8a1c1c;
}

/* settings gear */
QToolButton#mrGear {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 4px;
	padding: 3px 7px; color: #484848; font-size: 14px;
}
QToolButton#mrGear:hover { background: #222222; color: #c0c0c0; border-color: #424242; }

/* ── angle selector — state drives color, not :checked ───── */
QPushButton#mrAngle {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 3px;
	color: #383838; font-weight: 700; font-size: 10px;
	min-width: 34px; min-height: 22px; padding: 0 4px;
}
QPushButton#mrAngle:hover { background: #202020; color: #585858; border-color: #424242; }
QPushButton#mrAngle[state="preview"] {
	background: #081a0e; border-color: #1c8a38; color: #28b050;
}
QPushButton#mrAngle[state="preview"]:hover { background: #0c2014; border-color: #22a040; }
QPushButton#mrAngle[state="program"] {
	background: #200808; border-color: #be2020; color: #de3838;
}
QPushButton#mrAngle[state="program"]:hover { background: #280c0c; border-color: #cc2828; }

/* ── speed preset chips ─────────────────────────────────── */
QPushButton#mrSpeedChip {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 3px;
	color: #484848; font-size: 9px; font-weight: 700;
	min-width: 28px; min-height: 20px; padding: 1px 4px;
}
QPushButton#mrSpeedChip:hover { background: #222222; color: #b0b0b0; border-color: #424242; }
QPushButton#mrSpeedChip:pressed { background: #0c2212; border-color: #1c8a38; color: #28b050; }
/* the reference controller fills the chip that matches the current speed (100% by default). */
QPushButton#mrSpeedChip[active="true"] {
	background: #176533; border-color: #22a04a; color: #ffffff;
}

/* ── section separator line ─────────────────────────────── */
QWidget#mrSepLine { background: #1c1c1c; }

/* ── accent / danger buttons ─────────────────────────────── */
QPushButton#mrAccent {
	background: #0c1a0e; border: 1px solid #1a5a30; color: #28904a;
}
QPushButton#mrAccent:hover { background: #0e2012; color: #32b058; border-color: #208040; }
QPushButton#mrDanger { color: #b03030; border-color: #2c1818; }
QPushButton#mrDanger:hover { background: #1e1010; border-color: #442020; }

/* ── checkboxes ──────────────────────────────────────────── */
#MultiReplayDock QCheckBox { color: #888888; spacing: 5px; font-size: 11px; }
#MultiReplayDock QCheckBox::indicator {
	width: 13px; height: 13px; border-radius: 3px;
	border: 1px solid #2c2c2c; background: #181818;
}
#MultiReplayDock QCheckBox::indicator:checked { background: #1c8a38; border-color: #22a040; }

/* ── inputs ──────────────────────────────────────────────── */
#MultiReplayDock QComboBox, #MultiReplayDock QLineEdit {
	background: #181818; color: #c0c0c0;
	border: 1px solid #2c2c2c; border-radius: 3px;
	padding: 3px 7px; min-height: 20px; font-size: 11px;
}
#MultiReplayDock QComboBox:hover, #MultiReplayDock QLineEdit:hover { border-color: #424242; }
#MultiReplayDock QComboBox::drop-down { border: 0; width: 16px; }
#MultiReplayDock QComboBox QAbstractItemView {
	background: #181818; color: #c0c0c0; border: 1px solid #2c2c2c;
	selection-background-color: #1a2e52; selection-color: #d0d8f0; outline: 0;
}

/* ── speed slider — steel blue ───────────────────────────── */
QSlider#mrSpeed::groove:horizontal {
	height: 3px; background: #1e1e1e; border-radius: 2px;
}
QSlider#mrSpeed::sub-page:horizontal { background: #365e8a; border-radius: 2px; }
QSlider#mrSpeed::handle:horizontal {
	width: 10px; height: 10px; margin: -4px 0;
	background: #7aabc8; border-radius: 5px; border: 1px solid #284860;
}
QSlider#mrSpeed::handle:horizontal:hover { background: #e0e0e0; }

/* ── event table (the reference controller: navy rows, orange selection) ─────── */
QTableWidget#mrEvents {
	background: #00121C; alternate-background-color: #002A42;
	gridline-color: transparent; border: 1px solid #10243a;
	border-radius: 0; color: #d6dde6; outline: 0;
}
QTableWidget#mrEvents::item { padding: 2px 5px; border: 0; }
QTableWidget#mrEvents::item:selected { background: #DB5026; color: #ffffff; }
/* The per-angle enable box, which in the reference controller IS the cell */
QTableWidget#mrEvents::indicator {
	width: 11px; height: 11px;
	border: 1px solid #9aa4ae; background: #05131c;
}
QTableWidget#mrEvents::indicator:checked {
	background: #116B35; border-color: #d8dde2;
}
/* The section BACKGROUND is not ours to set: OBS's own theme styles
   QHeaderView::section (Yami.obt: background-color: var(--button_bg)) and wins
   whatever we put here — measured on screen, #272A33 either way. That is why
   the "angle I am watching" cue is a ▶ in the header TEXT (see
   updateCamHeaderHighlight) and not the reference controller's green fill: a colour we cannot
   guarantee is worse than a glyph we can. */
QHeaderView::section {
	color: #cfd8e4; padding: 3px 5px;
	font-size: 9px; font-weight: 700; letter-spacing: 0.6px;
}

/* ── scrollbars ──────────────────────────────────────────── */
#MultiReplayDock QScrollBar:vertical {
	background: transparent; width: 6px; margin: 0;
}
#MultiReplayDock QScrollBar::handle:vertical {
	background: #2a2a2a; border-radius: 3px; min-height: 20px;
}
#MultiReplayDock QScrollBar::handle:vertical:hover { background: #424242; }
#MultiReplayDock QScrollBar::add-line, #MultiReplayDock QScrollBar::sub-line {
	height: 0; width: 0;
}

QSplitter::handle:vertical {
	background: #1e1e1e; height: 5px;
}
QSplitter::handle:vertical:hover { background: #2e2e2e; }
QSplitter::handle:horizontal { background: #1e1e1e; width: 5px; }
)QSS";

namespace {

constexpr int kNCams = kMaxCameras; // 8

// Scrubbing means "review from here": the engine plays a range, it has no
// playhead to park. The window is capped because play() materialises every
// packet of the range in RAM, and an uncapped "from here to now" would be a
// multi-gigabyte copy on a long session.
constexpr int64_t kScrubReviewNs = 10'000'000'000LL; // 10 s

// How long a one-line notice keeps the status area (see showNotice).
constexpr int64_t kNoticeNs = 5'000'000'000LL; // 5 s

// A sequence is several clips, and the engine goes idle between them: the queue
// advance crosses the playback thread's finish callback and the OBS UI task
// queue, which takes a few tens of milliseconds. Anything inside this window
// still counts as "the replay is on screen", so the preview does not flash the
// live camera between two angles of the same event. Anything beyond it is a
// queue that is not coming back, and the preview must be free to move on.
constexpr int64_t kSequenceGapGraceNs = 1'500'000'000LL; // 1.5 s

// How long a take gets to prove Branch Output really started it. Branch Output
// re-evaluates its start conditions once a second (plugin-main.cpp:
// TASK_INTERVAL_MS = 1000), so this is four of its ticks — enough for encoder
// creation on a slow adapter, short enough that the operator learns NOW rather
// than when the first replay comes up empty. See poll().
constexpr int64_t kArmWatchNs = 4'000'000'000LL; // 4 s

// Event table column layout — the fixed information columns, then TWO COLUMNS
// PER CONFIGURED CAMERA:
//
//     …  |  N Nome  |  Nota  |  N+1 Nome  |  Nota  | …
//            ☑ 50%     free text
//
// which is the operator's whole per-angle edit — IS THIS ANGLE IN, AT WHAT
// SPEED, WITH WHAT COMMENT — laid out left to right, one click for the box, one
// double-click for either text, and no dialog anywhere. During a live match
// that is the difference between an edit he makes and an edit he skips.
//
// There is deliberately NO per-event speed column. The velocities are per-angle
// and default, full stop (see ReplayEvent) — the M3 "Vel" column encoded a third
// idea that belongs to neither.
//
// The column numbers themselves live in the HEADER (MultiReplayDock::kColId …
// kColFirstCam, kColsPerCam): the gate edits real cells and must read the
// layout from the same place the dock builds it from.
// Only the two the free functions below need. The fixed columns are NOT
// mirrored here: inside a member function unqualified `kColId` resolves to the
// class enumerator, so a file-scope copy would never be read - and clang says
// so with -Wunused-const-variable, which this project treats as an error.
constexpr int kColFirstCam = MultiReplayDock::kColFirstCam;
constexpr int kColsPerCam = MultiReplayDock::kColsPerCam;

// Which camera a table column belongs to. -1 = not a camera column at all.
// Kept as one function so the callers cannot drift.
inline int camPairIndex(int column)
{
	return column < kColFirstCam ? -1 : (column - kColFirstCam) / kColsPerCam;
}

// A camera cell with no comment reads "-" in the reference controller, not blank: an empty cell is
// ambiguous next to a checkbox, a dash is not. It is display only — it never
// reaches the store (see camNoteFromCell).
const QString kNoNote = QStringLiteral("-");

QString monoFamily()
{
	// Prefer a real fixed-pitch family for timecodes; fall back gracefully.
	QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	return f.family();
}

QString formatTc(int64_t ns)
{
	if (ns < 0)
		ns = 0;
	int64_t totalMs = ns / 1000000;
	int ms = (int)(totalMs % 1000);
	int64_t totalS = totalMs / 1000;
	int s = (int)(totalS % 60);
	int m = (int)(totalS / 60);
	return QString::asprintf("%02d:%02d.%03d", m, s, ms);
}

// obs_data RAII helper
struct Data {
	obs_data_t *d;
	explicit Data(const std::string &json)
		// An empty string is a normal answer - no session, no cameras
		// configured - not a parse failure. Handing it to the parser
		// anyway made obs-data.c log "'[' or '{' expected" about twenty
		// times a second from the poll timer, which buried real errors
		// in the log and did the work for nothing.
		: d(json.empty() ? nullptr
				 : obs_data_create_from_json(json.c_str()))
	{
	}
	~Data()
	{
		if (d)
			obs_data_release(d);
	}
	operator obs_data_t *() const { return d; }
};

// small compact marker/action button. `role` maps to a QSS object name so the
// stylesheet can theme it ("" = default, "mrAccent", "mrDanger").
QPushButton *compactBtn(const QString &text, QWidget *parent,
			const char *role = "")
{
	auto *b = new QPushButton(text, parent);
	b->setMinimumHeight(28);
	if (role && *role)
		b->setObjectName(QString::fromLatin1(role));
	b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

// Transport button using Unicode glyphs — visible on dark backgrounds without
// relying on QStyle pixmaps (which follow the platform icon theme and render
// dark on dark in OBS's dark palette).
QPushButton *transportBtn(const QString &text, QWidget *parent, const QString &tip,
			   const char *role = "mrTransport")
{
	auto *b = new QPushButton(text, parent);
	b->setObjectName(QString::fromLatin1(role));
	b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	return b;
}

// Re-evaluate a widget's stylesheet after a dynamic property changes (QSS
// property selectors like [recording="true"] don't restyle on their own).
void repolish(QWidget *w)
{
	w->style()->unpolish(w);
	w->style()->polish(w);
	w->update();
}

// A latching control drawn as a button, which is what the reference controller uses for Live, Loop
// and the music toggle — a checkbox reads as a form field, a lit button reads
// as a state, and this panel is read at a glance.
QPushButton *toggleBtn(const QString &text, QWidget *parent, const QString &tip,
		       const char *role = "mrToggle")
{
	auto *b = new QPushButton(text, parent);
	b->setObjectName(QString::fromLatin1(role));
	b->setCheckable(true);
	b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	return b;
}

// A small uppercase caption, the reference controller's "Mark" / "A" row prefixes.
QLabel *sectionLabel(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text.toUpper(), parent);
	l->setObjectName(QStringLiteral("mrSectionLabel"));
	return l;
}

// Render `src` letterboxed inside a cx*cy display. Shared by the big preview
// and by every multiview tile: they differ in WHICH source they were handed,
// never in how it is drawn, and one copy of this arithmetic is one place where
// an aspect-ratio bug can live.
//
// Runs on the OBS graphics thread. It must not look anything up.
void renderSourceFitted(obs_source_t *src, uint32_t cx, uint32_t cy)
{
	const uint32_t sw = obs_source_get_width(src);
	const uint32_t sh = obs_source_get_height(src);
	if (sw == 0 || sh == 0)
		return;
	// Fit inside the widget preserving aspect ratio; bars (black) appear on
	// whichever axis has excess space.
	const float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
	const int dw = (int)((float)sw * scale);
	const int dh = (int)((float)sh * scale);
	const int x = ((int)cx - dw) / 2;
	const int y = ((int)cy - dh) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	gs_set_viewport(x, y, dw, dh);
	obs_source_video_render(src);
	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace

// ---------------------------------------------------------------------------
// SeekBar
// ---------------------------------------------------------------------------

namespace {

// The ruler strip under the track: where the graduation ticks and their labels
// are drawn. Small, but it is the whole difference between a scale and a
// coloured rectangle, so the bar is made tall enough to afford it rather than
// the labels being squeezed over the track and fighting the timecode there.
// 14 was not enough: the labels are drawn into (kSeekRulerH - 5) pixels, so a
// 14 px strip left 9 px for a number and clipped the digits - the scale was
// there but unreadable, which is the same as not being there. 24 leaves 19 px,
// comfortable for the small font at any DPI.
constexpr int kSeekRulerH = 24;
// Track thickness. Wide enough to hold the event markers and the timecode the
// way it always did.
constexpr int kSeekTrackH = 26;

// Label for a graduation. Minutes and seconds up to an hour, then hours: a
// position bar over a two-hour recording that counts to "137:00" is arithmetic
// the operator should not be doing.
QString tickLabel(int64_t ns)
{
	const int64_t s = ns / 1'000'000'000LL;
	if (s < 3600)
		return QString::asprintf("%lld:%02lld", (long long)(s / 60),
					 (long long)(s % 60));
	return QString::asprintf("%lld:%02lld:%02lld", (long long)(s / 3600),
				 (long long)((s % 3600) / 60), (long long)(s % 60));
}

} // namespace

SeekBar::SeekBar(QWidget *parent) : QWidget(parent)
{
	// Track + ruler + the 1 px of margin each side. The bar used to be 28 px
	// of pure track: enough to click, not enough to be recognised as the
	// control over the whole recording.
	setFixedHeight(kSeekTrackH + kSeekRulerH + 2);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setCursor(Qt::PointingHandCursor);
	setMouseTracking(false);
}

int SeekBar::trackWidth() const
{
	return std::max(1, width() - 4); // 2 px margin each side
}

int64_t SeekBar::tickStepNs() const
{
	if (durationNs_ <= 0)
		return 0;
	// Only round steps an operator counts in: seconds, then the quarter and
	// half minute, then minutes. A "every 3.7 s" scale is worse than none.
	// Zoomed in, the same list gives finer marks by itself — which is the
	// point of zooming: the scale has to get more precise, not just wider.
	static const int64_t kStepsSec[] = {1,   2,   5,   10,   15,   30,  60,
					    120, 300, 600, 900, 1800, 3600, 7200,
					    10800};
	// Two labels 58 px apart still have air between them at this font; any
	// closer and the scale turns into a smear of digits.
	const double kMinPx = 58.0;
	const double w = (double)trackWidth();
	const double visibleNs = (double)durationNs_ * viewSpan_;
	for (int64_t sec : kStepsSec) {
		const int64_t step = sec * 1'000'000'000LL;
		if (w * (double)step / visibleNs >= kMinPx)
			return step;
	}
	// Longer than the coarsest step: keep the coarsest rather than dropping
	// the scale — sparse marks still say which way time runs.
	return kStepsSec[sizeof(kStepsSec) / sizeof(kStepsSec[0]) - 1] *
	       1'000'000'000LL;
}

int SeekBar::graduations() const
{
	const int64_t step = tickStepNs();
	if (step <= 0)
		return 0;
	// Marks at step, 2*step, … up to the end (0 is the left edge and needs
	// no mark of its own).
	return (int)(durationNs_ / step);
}

void SeekBar::setTimeline(int64_t durationNs, const QString &emptyHint)
{
	if (durationNs < 0)
		durationNs = 0;
	if (durationNs == durationNs_ && emptyHint == emptyHint_)
		return; // called thirty times a second; most ticks change nothing
	const bool had = durationNs_ > 0;
	durationNs_ = durationNs;
	emptyHint_ = emptyHint;
	if (had != (durationNs_ > 0)) {
		// A bar that cannot be scrubbed must not offer the hand cursor:
		// the cursor is the first promise the widget makes.
		setCursor(durationNs_ > 0 ? Qt::PointingHandCursor
					  : Qt::ArrowCursor);
		if (dragging_) {
			dragging_ = false;
			emit scrubStateChanged(false);
		}
	}
	update();
}

void SeekBar::setProgress(double positionFrac, double seekableFrac)
{
	positionFrac_ = std::clamp(positionFrac, 0.0, 1.0);
	seekableFrac_ = std::clamp(seekableFrac, 0.0, 1.0);
	if (!dragging_)
		update();
}

void SeekBar::setEventMarkers(std::vector<std::pair<double, double>> markers)
{
	markers_ = std::move(markers);
	update();
}

void SeekBar::setOverlayText(const QString &text)
{
	if (overlay_ == text)
		return;
	overlay_ = text;
	update();
}

double SeekBar::fracAt(int x) const
{
	const int m = 2;
	double w = std::max(1, width() - 2 * m);
	// Pixels are window coordinates; everything this class hands out is a
	// fraction of the WHOLE timeline, so the window is undone right here and
	// nowhere else.
	const double v = std::clamp((double)(x - m) / w, 0.0, 1.0);
	return std::clamp(fromView(v), 0.0, 1.0);
}

void SeekBar::setZoom(double zoom, double centreFrac)
{
	// 1× is the whole timeline; past 512× a 1080p frame of a three-hour
	// session is still several pixels wide, and there is nothing finer to
	// aim at.
	zoom_ = std::clamp(zoom, 1.0, 512.0);
	viewSpan_ = 1.0 / zoom_;
	// Clamped to the ends: a window hanging off the edge would draw empty
	// space as if the timeline had it.
	viewStart_ = std::clamp(centreFrac - viewSpan_ / 2.0, 0.0,
				1.0 - viewSpan_);
	emit zoomChanged(zoom_);
	update();
}

void SeekBar::ensureVisible(double frac)
{
	if (viewSpan_ >= 1.0)
		return;
	// A tenth of the window of air on the side it left by: scrolling it to
	// the very edge means it leaves again on the next frame.
	const double pad = viewSpan_ * 0.1;
	double start = viewStart_;
	if (frac < viewStart_ + pad)
		start = frac - pad;
	else if (frac > viewStart_ + viewSpan_ - pad)
		start = frac - viewSpan_ + pad;
	start = std::clamp(start, 0.0, 1.0 - viewSpan_);
	if (std::abs(start - viewStart_) < 1e-9)
		return;
	viewStart_ = start;
	update();
}

void SeekBar::wheelEvent(QWheelEvent *e)
{
	if (durationNs_ <= 0) {
		e->ignore();
		return;
	}
	const int steps = e->angleDelta().y();
	if (steps == 0) {
		e->ignore();
		return;
	}
	// Zoom about the POINTER, not the centre: the operator puts the cursor
	// on the moment he cares about and scrolls, and that moment stays under
	// the cursor while everything else spreads out around it.
	const double at = fracAt((int)e->position().x());
	const double factor = steps > 0 ? 1.25 : 1.0 / 1.25;
	const double next = std::clamp(zoom_ * factor, 1.0, 512.0);
	// Keep `at` where it is on screen: the window shrinks around it rather
	// than around its own middle.
	const double vAt = toView(at); // where it sits in the window now
	const double span = 1.0 / next;
	viewStart_ = std::clamp(at - vAt * span, 0.0, 1.0 - span);
	zoom_ = next;
	viewSpan_ = span;
	emit zoomChanged(zoom_);
	update();
	e->accept();
}

void SeekBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	// Antialiasing OFF: everything here is axis-aligned, and a 1 px
	// graduation drawn with AA is a 2 px grey smudge — which is how a scale
	// stops being readable at the exact size where it matters.
	p.setRenderHint(QPainter::Antialiasing, false);

	// The whole widget IS the bar (no bead on a rail): it is the control the
	// operator's hand lives on, so it is as wide and as tall as the panel can
	// afford.
	//
	// SLATE BLUE, not green. The green band is the ClipBar directly above,
	// and that one is a STATUS: what is on air, how much of it is left. This
	// one is a CONTROL over the whole recorded timeline. Two stacked bars in
	// the same colour would be two bars the operator has to tell apart by
	// reading, every time, under pressure.
	const int m = 2;             // horizontal margin
	const int h = kSeekTrackH;   // track thickness
	const int y = 1;
	const int rulerY = y + h;    // graduations live under the track
	const int w = width() - 2 * m;
	// Everything below draws in WINDOW coordinates: a fraction of the whole
	// timeline becomes a fraction of what is on screen through toView(). At
	// 1× the two are the same and none of this costs anything.
	const double pos = toView(dragging_ ? dragFrac_ : positionFrac_);

	// --- no timeline: say so, and mean it ---------------------------------
	// This is not a defensive branch, it is a state the operator reaches by
	// opening yesterday's project: the files are on disk but nothing is
	// feeding a live edge, so there is no span to scrub. Drawing the usual
	// empty track there gave him a scrubber that swallowed every click.
	if (durationNs_ <= 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x11, 0x13, 0x17));
		p.drawRect(QRect(m, y, w, h + kSeekRulerH));
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(0x24, 0x2a, 0x33), 1));
		p.drawRect(QRect(m, y, w - 1, h + kSeekRulerH - 1));
		if (!emptyHint_.isEmpty()) {
			QFont f = p.font();
			f.setBold(true);
			p.setFont(f);
			p.setPen(QColor(0x70, 0x7e, 0x8e));
			p.drawText(QRect(m + 6, y, w - 12, h + kSeekRulerH),
				   Qt::AlignCenter, emptyHint_);
		}
		return;
	}

	// Track (the part of the timeline behind/ahead of the playhead)
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x16, 0x1c, 0x24));
	p.drawRect(QRectF(m, y, w, h));

	// Anything outside the seekable region is darker still: it is not a place
	// the operator can go, and painting it like the rest would say it is.
	if (seekableFrac_ < 1.0) {
		p.setBrush(QColor(0x0c, 0x0e, 0x12));
		p.drawRect(QRectF(m + w * seekableFrac_, y,
				  w * (1.0 - seekableFrac_), h));
	}

	// Behind the playhead — steel blue, and it goes down FIRST, under the
	// markers. The events are the content of this bar; the progress is where
	// the operator happens to be in it, and a position that hides the marks
	// it has already passed is a bar that forgets what is on it. The marker
	// fill is semi-transparent, so on the played side both read at once:
	// orange event over blue ground.
	if (pos > 0.0) {
		p.setBrush(QColor(0x2a, 0x4a, 0x72));
		p.drawRect(QRectF(m, y, w * std::clamp(pos, 0.0, 1.0), h));
	}

	// Event markers — orange, the colour the reference controller gives an event everywhere else
	// (the selected row). Semi-transparent fill + bright edges.
	static const QColor kFill[2] = {
		QColor(0xdb, 0x50, 0x26, 0x80), // orange
		QColor(0xe0, 0x80, 0x20, 0x80), // amber (alternating, so two
						// adjacent events stay legible)
	};
	static const QColor kEdge[2] = {
		QColor(0xff, 0x76, 0x40),
		QColor(0xff, 0xa8, 0x40),
	};
	for (int mi = 0; mi < (int)markers_.size(); mi++) {
		const auto &mk = markers_[mi];
		if (mk.second <= mk.first)
			continue;
		const int ci = mi % 2;
		const double v0 = toView(mk.first);
		const double v1 = toView(mk.second);
		if (v1 < 0.0 || v0 > 1.0)
			continue; // outside the window entirely
		double x0 = m + w * std::clamp(v0, 0.0, 1.0);
		double x1 = m + w * std::clamp(v1, 0.0, 1.0);
		double mw = std::max(x1 - x0, 2.0);

		// Fill
		p.setPen(Qt::NoPen);
		p.setBrush(kFill[ci]);
		p.drawRect(QRectF(x0, y, mw, h));

		// Left edge (bright, 2px)
		p.setPen(QPen(kEdge[ci], 2.0, Qt::SolidLine, Qt::FlatCap));
		p.drawLine(QPointF(x0, y), QPointF(x0, y + h));

		// Right edge (dimmer, 1px, only when wide enough)
		if (x1 - x0 > 4) {
			p.setPen(QPen(kEdge[ci].darker(150), 1.0,
				      Qt::SolidLine, Qt::FlatCap));
			p.drawLine(QPointF(x1, y), QPointF(x1, y + h));
		}
	}
	p.setPen(Qt::NoPen);

	// --- the graduations --------------------------------------------------
	// The scale, on its own strip under the track plus a matching tick INTO
	// the track. This is what tells the operator that the bar spans forty
	// seconds and not two hours — the number in the middle only says where he
	// is, never how far the ends are.
	//
	// Two densities: labelled marks at tickStepNs(), and four unlabelled ones
	// between each pair (the fifths of the step) as long as they are far
	// enough apart to still be separate lines. The minor marks are what makes
	// the strip read as a ruler at a glance, before any digit is read.
	{
		p.setBrush(Qt::NoBrush);
		// Ruler ground, a shade darker than the track: the strip is part
		// of the control, not the panel behind it.
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x0d, 0x10, 0x15));
		p.drawRect(QRect(m, rulerY, w, kSeekRulerH));

		const int64_t step = tickStepNs();
		// Pixels per nanosecond OF WHAT IS ON SCREEN, and where the
		// window starts in time: the marks are absolute timecodes drawn
		// at their own place in the window, so zooming changes the
		// spacing and never the labels.
		const int64_t viewStartNs =
			(int64_t)((double)durationNs_ * viewStart_);
		const int64_t viewEndNs =
			(int64_t)((double)durationNs_ * (viewStart_ + viewSpan_));
		const double pxPerNs =
			(double)w / std::max(1.0, (double)(viewEndNs - viewStartNs));
		const double minorPx = pxPerNs * (double)step / 5.0;
		const bool drawMinor = minorPx >= 7.0;

		QFont lf = p.font();
		if (lf.pointSizeF() > 0)
			lf.setPointSizeF(std::max(6.5, lf.pointSizeF() * 0.78));
		p.setFont(lf);
		const QFontMetrics fm(lf);

		const QColor kMajor(0x8e, 0xa4, 0xbc);
		const QColor kMinor(0x46, 0x54, 0x66);
		// Where the last label ended, so two of them can never overlap
		// even at the clamped edges.
		int lastLabelRight = -10000;

		// Start at the first whole step inside the window, not at zero:
		// zoomed into the fiftieth minute, counting from 0 would walk
		// three thousand invisible marks before drawing one.
		const int64_t first = (viewStartNs / step) * step;
		for (int64_t t = first; t <= viewEndNs; t += step) {
			if (t < 0)
				continue;
			const double x =
				(double)m + pxPerNs * (double)(t - viewStartNs);

			// Minor marks between this graduation and the next.
			if (drawMinor) {
				p.setPen(QPen(kMinor, 1));
				for (int k = 1; k < 5; k++) {
					const double mx =
						x + pxPerNs * (double)step *
							    ((double)k / 5.0);
					if (mx > (double)(m + w))
						break;
					const int mxi = (int)mx;
					p.drawLine(mxi, rulerY + 1, mxi,
						   rulerY + 4);
				}
			}

			const int xi = (int)x;
			// The mark itself: a tick into the bottom of the track so
			// the scale is tied to the thing it measures, and a taller
			// one on the ruler.
			p.setPen(QPen(QColor(0xff, 0xff, 0xff, 0x38), 1));
			p.drawLine(xi, y + h - 5, xi, y + h - 1);
			p.setPen(QPen(kMajor, 1));
			p.drawLine(xi, rulerY + 1, xi, rulerY + 6);

			// ...and its label, when there is room for it. The first
			// and last are pulled inside the bar instead of being cut
			// in half by its edge.
			const QString lbl = tickLabel(t);
			const int lw = fm.horizontalAdvance(lbl);
			int lx = xi - lw / 2;
			lx = std::clamp(lx, m + 1, m + w - lw - 1);
			if (lx > lastLabelRight + 6) {
				p.setPen(kMajor);
				p.drawText(QRect(lx, rulerY + 5, lw, kSeekRulerH - 5),
					   Qt::AlignHCenter | Qt::AlignTop, lbl);
				lastLabelRight = lx + lw;
			}
		}
	}

	// Playhead: a thin bright line, no bead. the reference controller draws a hairline, and a
	// bead on a 24 px band hides the very frame it is pointing at. It runs
	// through the ruler too — a scale is only useful if the position can be
	// read against it.
	const double hx = m + w * pos;
	p.setPen(QPen(QColor(0xff, 0xff, 0xff, dragging_ ? 0xff : 0xc0),
		      dragging_ ? 3.0 : 2.0, Qt::SolidLine, Qt::FlatCap));
	p.drawLine(QPointF(hx, y), QPointF(hx, y + h + kSeekRulerH));

	// Where the timeline stands, printed ON the bar (the reference controller). NOT the clip
	// state — that is the green band above; this is position and length of
	// the recorded timeline, which is what a scrubber is about.
	if (!overlay_.isEmpty()) {
		QFont f = p.font();
		f.setPointSizeF(f.pointSizeF() * 1.05);
		f.setBold(true);
		p.setFont(f);
		const QRectF tr(m, y, w, h);
		p.setPen(QColor(0x00, 0x00, 0x00, 0xb0));
		p.drawText(tr.adjusted(1, 1, 1, 1), Qt::AlignCenter, overlay_);
		p.setPen(QColor(0xd8, 0xe4, 0xf2));
		p.drawText(tr, Qt::AlignCenter, overlay_);
	}
}

int SeekBar::xForFraction(double frac) const
{
	const int m = 2;
	const int w = std::max(1, width() - 2 * m);
	return m + (int)std::lround(w * std::clamp(toView(frac), 0.0, 1.0));
}

void SeekBar::setEventMarkerIds(std::vector<int> ids)
{
	markerIds_ = std::move(ids);
}

bool SeekBar::findMarkerEdge(int x, int &marker, bool &inPoint) const
{
	// Six pixels either side. Wider and a click meaning "scrub here" starts
	// grabbing edges the operator was not aiming at; narrower and the edge
	// cannot be hit at all on a bar this short. Zoom is what makes the aim
	// easy, and that is the other half of this feature.
	constexpr int kGrabPx = 6;
	int best = -1;
	int bestDist = kGrabPx + 1;
	bool bestIn = true;
	for (size_t i = 0; i < markers_.size() && i < markerIds_.size(); i++) {
		const int xin = xForFraction(markers_[i].first);
		const int xout = xForFraction(markers_[i].second);
		if (std::abs(x - xin) < bestDist) {
			bestDist = std::abs(x - xin);
			best = (int)i;
			bestIn = true;
		}
		if (std::abs(x - xout) < bestDist) {
			bestDist = std::abs(x - xout);
			best = (int)i;
			bestIn = false;
		}
	}
	if (best < 0)
		return false;
	marker = best;
	inPoint = bestIn;
	return true;
}

void SeekBar::mousePressEvent(QMouseEvent *e)
{
	// No timeline, no scrub. The host would refuse the seek anyway
	// (seekToFraction), but silently: the click has to be refused HERE, so
	// the playhead does not jump to where the finger landed on a bar that
	// spans nothing.
	if (e->button() != Qt::LeftButton || durationNs_ <= 0)
		return;

	// An edge under the finger means "move this point", not "go there". A
	// mark taken live is late, and the fastest fix is to take hold of the
	// mark itself: no selection, no keys, no dialog.
	int marker = -1;
	bool inPoint = true;
	if (findMarkerEdge(e->pos().x(), marker, inPoint)) {
		dragMarker_ = marker;
		dragMarkerIn_ = inPoint;
		dragging_ = true;
		dragFrac_ = fracAt(e->pos().x());
		emit scrubStateChanged(true);
		update();
		return;
	}
	dragMarker_ = -1;
	dragging_ = true;
	// Clamp to seekable region so the user can't drag into unindexed territory.
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	emit scrubStateChanged(true);
	emit scrubMoved(dragFrac_);
	update();
}

void SeekBar::mouseMoveEvent(QMouseEvent *e)
{
	if (!dragging_) {
		// Hovering an edge says so with the cursor, which is the only
		// hint there is that a marker can be taken hold of at all.
		int m = -1;
		bool in = true;
		setCursor(findMarkerEdge(e->pos().x(), m, in) ? Qt::SizeHorCursor
							      : Qt::ArrowCursor);
		return;
	}
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	if (dragMarker_ >= 0) {
		// Live feedback on the marker itself, so the operator sees the
		// clip's edge follow his hand rather than a number changing
		// after he lets go.
		auto &mk = markers_[(size_t)dragMarker_];
		if (dragMarkerIn_)
			mk.first = std::min(dragFrac_, mk.second);
		else
			mk.second = std::max(dragFrac_, mk.first);
		update();
		return;
	}
	emit scrubMoved(dragFrac_);
	update();
}

void SeekBar::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !dragging_)
		return;
	dragging_ = false;
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	if (dragMarker_ >= 0) {
		// The bar knows nothing about events: it reports the gesture and
		// the host decides what it means (and whether the store will
		// even accept it — an IN cannot pass its OUT).
		const int id = (size_t)dragMarker_ < markerIds_.size()
				       ? markerIds_[(size_t)dragMarker_]
				       : 0;
		dragMarker_ = -1;
		emit scrubStateChanged(false);
		if (id > 0)
			emit markerDragged(id, dragMarkerIn_, dragFrac_);
		update();
		return;
	}
	positionFrac_ = dragFrac_;
	emit seekRequested(dragFrac_);
	emit scrubStateChanged(false);
	update();
}

// ---------------------------------------------------------------------------
// ClipBar — the green on-air band (see the header for what it is FOR)
// ---------------------------------------------------------------------------

ClipBar::ClipBar(QWidget *parent) : QWidget(parent)
{
	setFixedHeight(24);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	// Deliberately NOT a pointing-hand cursor and deliberately not clickable:
	// the bar directly under it IS clickable, and a bar that looks draggable
	// but is not is worse than one that looks inert.
}

void ClipBar::setState(double progressFrac, const QString &text, bool onAir)
{
	const double p = std::clamp(progressFrac, 0.0, 1.0);
	if (std::abs(p - progress_) < 0.0005 && text == text_ && onAir == onAir_)
		return; // 30 times a second, most ticks change nothing
	progress_ = p;
	text_ = text;
	onAir_ = onAir;
	update();
}

void ClipBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const int m = 2;
	const int h = height() - 2;
	const int y = 1;
	const int w = width() - 2 * m;

	// Track, then the played part of THIS clip. Bright while it is on air,
	// muted while the bar is only describing what would play: "is something
	// on air" must be answerable without reading a word.
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x14, 0x64, 0x33));
	p.drawRect(QRectF(m, y, w, h));
	if (progress_ > 0.0) {
		p.setBrush(onAir_ ? QColor(0x19, 0x98, 0x47)
				  : QColor(0x17, 0x65, 0x33));
		p.drawRect(QRectF(m, y, w * progress_, h));
	}

	if (text_.isEmpty())
		return;
	QFont f = p.font();
	f.setBold(true);
	p.setFont(f);
	const QRectF tr(m + 6, y, w - 12, h);
	// Dark halo first: the text crosses both greens and has to stay legible
	// over either.
	p.setPen(QColor(0x00, 0x20, 0x0c, 0xb0));
	p.drawText(tr.adjusted(1, 1, 1, 1), Qt::AlignCenter, text_);
	p.setPen(onAir_ ? QColor(0xff, 0xff, 0xff) : QColor(0xa8, 0xc8, 0xb0));
	p.drawText(tr, Qt::AlignCenter, text_);
}

// ---------------------------------------------------------------------------
// Preview render callback (runs on the OBS graphics thread)
// ---------------------------------------------------------------------------

void MultiReplayDock::drawChannelA(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiReplayDock *>(data);
	if (!self || cx == 0 || cy == 0)
		return;

	// This runs on the ONE graphics thread that renders every obs_display in
	// OBS, so it does the least work that can possibly show a picture: copy
	// the source poll() already resolved and take a reference to it. That
	// reference is an atomic increment on the source's own control block — no
	// name lookup, no libobs global source mutex, no engine lock. A draw
	// callback that blocks on any of those blocks the whole interface, which
	// is what turned the OBS window black while its taskbar thumbnail (drawn
	// from an already-composed surface) stayed correct. See previewSource_.
	obs_source_t *src = nullptr;
	{
		std::lock_guard<std::mutex> lk(self->previewMutex_);
		if (self->previewSource_)
			src = obs_source_get_ref(self->previewSource_);
	}
	if (!src)
		return;
	renderSourceFitted(src, cx, cy);
	obs_source_release(src);
}

// Channel B's box. Same contract as drawChannelA, and deliberately simpler:
// B has no live-camera mirror. A is where the operator watches the match
// (follow-live puts the selected camera in it); B is a replay bay — what it
// shows is whatever B last played, and black when B has played nothing. Giving
// it a live mirror too would put the same picture in both boxes for most of a
// match, which is two boxes saying one thing.
void MultiReplayDock::drawChannelB(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiReplayDock *>(data);
	if (!self || cx == 0 || cy == 0)
		return;
	obs_source_t *src = nullptr;
	{
		std::lock_guard<std::mutex> lk(self->previewMutex_);
		if (self->previewSourceB_)
			src = obs_source_get_ref(self->previewSourceB_);
	}
	if (!src)
		return;
	renderSourceFitted(src, cx, cy);
	obs_source_release(src);
}

// One multiview tile. Same contract as drawChannelA — and the same reason for
// it: this runs on the ONE graphics thread that renders every obs_display in
// OBS, and with up to nine tiles a single blocking lookup in here would be nine
// chances per frame to freeze the whole interface. All it may touch is the
// pointer poll() already resolved for this slot.
void MultiReplayDock::drawTile(void *data, uint32_t cx, uint32_t cy)
{
	auto *ctx = static_cast<TileCtx *>(data);
	if (!ctx || !ctx->dock || cx == 0 || cy == 0)
		return;
	MultiReplayDock *self = ctx->dock;
	if (ctx->slot < 0 || ctx->slot >= kMaxPreviewTiles)
		return;

	obs_source_t *src = nullptr;
	{
		std::lock_guard<std::mutex> lk(self->tileMutex_);
		if (self->tileSource_[ctx->slot])
			src = obs_source_get_ref(self->tileSource_[ctx->slot]);
	}
	if (!src)
		return;
	renderSourceFitted(src, cx, cy);
	obs_source_release(src);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MultiReplayDock::MultiReplayDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("MultiReplayDock");
	// Taller than before: the reference controller zoning is vertical (preview, list, two rows
	// of controls, position bar) where the old layout was two columns.
	setMinimumSize(700, 340);
	setStyleSheet(QString::fromUtf8(kDockStyle));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(3);

	// Helper: 1px separator line (each call returns a fresh widget).
	auto mkSep = [this]() -> QWidget * {
		auto *s = new QWidget(this);
		s->setObjectName("mrSepLine");
		s->setFixedHeight(1);
		return s;
	};

	// ── The broadcast replay controller's zoning, top to bottom ────────────
	// 1. the pictures: channel A big, the multiview beside it, green strip
	// 2. search · Live
	// 3. the list tabs
	// 4. the event list, one column per camera
	// 5. mark keys · angle row · export
	// 6. record + transport + slow-motion speed
	// 7. the green on-air band
	// 8. the full-width position bar
	//
	// An operator who has used the reference controller finds every control where his hand
	// already goes, which is the entire point of this layout.
	//
	// THE PICTURES COME FIRST. They did not, for a while: the tabs, the
	// search box and Live sat above them, which put a row of small text
	// where the operator's eye goes for the picture and pushed the picture
	// down. In the controller this panel is modelled on, everything that
	// SELECTS an event (which list, which words, live or parked) belongs to
	// the list — so it lives with the list, directly under the pictures and
	// directly above the table it filters.
	{
		// Preview above, list below: the reference controller stacks them, and the previous
		// side-by-side split had no equivalent there. Draggable, because an
		// OBS dock can be a wide strip or a tall column.
		splitter_ = new QSplitter(Qt::Vertical, this);
		splitter_->setChildrenCollapsible(false);
		splitter_->setHandleWidth(5);
		splitter_->addWidget(buildPreview());

		// The list pane: what picks the events, then the events. One
		// widget so the splitter treats them as the single zone they are
		// — dragging the handle must not be able to leave the search row
		// stranded away from its table.
		auto *listPane = new QWidget(this);
		auto *lv = new QVBoxLayout(listPane);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(2);
		lv->addWidget(buildToolbar());
		lv->addWidget(buildEvents(), 1);
		splitter_->addWidget(listPane);

		splitter_->setStretchFactor(0, 3);
		splitter_->setStretchFactor(1, 2);
		root->addWidget(splitter_, 1);
	}

	root->addWidget(mkSep());
	root->addWidget(buildBottomBar());

	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(33); // ~30 fps — smooth seekbar + responsive transport
	connect(pollTimer_, &QTimer::timeout, this, &MultiReplayDock::poll);
	pollTimer_->start();

	// After the widgets exist: every one of these acts on one of them.
	registerDockHotkeys();

	refreshAngles();
	refreshEvents();
	poll();
}

MultiReplayDock::~MultiReplayDock()
{
	// FIRST, before anything else is torn down: a hotkey firing from the
	// hotkey thread must not find a half-destroyed dock. obs_hotkey_unregister
	// takes libobs' hotkey mutex, so it returns only once no callback is in
	// flight; the contexts it was pointing at die with the vector below.
	for (obs_hotkey_id id : hotkeys_)
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);
	hotkeys_.clear();
	hotkeyCtx_.clear();

	// Order matters here. The display is a CHILD widget, so ~QWidget takes it
	// down only after this body has returned — leaving a window in which the
	// graphics thread can still call drawChannelA() on a half-destroyed dock.
	// Detaching the callback first closes it: obs_display_remove_draw_callback
	// takes the display's callback mutex, so it returns only once the draw in
	// flight has finished.
	if (displayA_)
		displayA_->setRenderCallback(nullptr, nullptr);
	// Same for every multiview tile, and for the same reason: each one has a
	// draw callback pointing at a TileCtx that dies with this object.
	for (PreviewTile &t : tiles_)
		if (t.display)
			t.display->setRenderCallback(nullptr, nullptr);
	if (pollTimer_)
		pollTimer_->stop();
	if (previewSource_) {
		obs_source_release(previewSource_);
		previewSource_ = nullptr;
	}
	{
		// Swapped out under the lock, released outside it: the last release
		// destroys the source and enters the graphics context, and that is
		// the thread that wants tileMutex_.
		std::array<obs_source_t *, kMaxPreviewTiles> dying{};
		{
			std::lock_guard<std::mutex> lk(tileMutex_);
			dying = tileSource_;
			tileSource_.fill(nullptr);
		}
		for (obs_source_t *s : dying)
			if (s)
				obs_source_release(s);
	}
}

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
	projectLbl_->setStyleSheet("color: #487898; font-size: 9px; padding: 0 4px;");
	projectLbl_->hide();
	h->addWidget(projectLbl_);
	// Search and Live sit in the MIDDLE of their row, as they do on the
	// reference panel: the operator's eye comes down off the picture into the
	// centre of the panel, not into a corner.
	h->addStretch(1);

	auto *mag = new QLabel(QStringLiteral("🔍"), box);
	mag->setObjectName("mrMuted");
	h->addWidget(mag);
	search_ = new QLineEdit(box);
	search_->setPlaceholderText(obs_module_text("Dock.Search"));
	search_->setClearButtonEnabled(true);
	search_->setMaximumWidth(190);
	search_->setMinimumWidth(90);
	connect(search_, &QLineEdit::textChanged, this,
		[this](const QString &) { refreshEvents(); });
	h->addWidget(search_, 0);

	// the reference controller's Live button, in the reference controller's place and the reference controller's colour: red means the
	// marks land where the action is happening, off means they land where the
	// position bar is parked.
	liveBtn_ = new QPushButton(obs_module_text("Dock.LiveMode"), box);
	liveBtn_->setObjectName("mrLive");
	liveBtn_->setCheckable(true);
	liveBtn_->setCursor(Qt::PointingHandCursor);
	liveBtn_->setToolTip(obs_module_text("Dock.LiveModeHint"));
	liveBtn_->setChecked(EventStore::instance().liveMode());
	connect(liveBtn_, &QPushButton::toggled, this,
		[](bool on) { EventStore::instance().setLiveMode(on); });
	h->addWidget(liveBtn_);
	h->addStretch(1);
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

	// the reference controller: the A output big on the left, every camera small on the right.
	// One row, two stretches — the operator watches the big one and keeps the
	// others in the corner of his eye, which is the whole reason the strip is
	// here rather than behind a button.
	auto *row = new QWidget(box);
	auto *rh = new QHBoxLayout(row);
	rh->setContentsMargins(0, 0, 0, 0);
	rh->setSpacing(3);

	// TWO outputs side by side, as in the reference controller: A on the left, B on the right,
	// each under its own letter. One box for two channels would mean the
	// operator has to remember which one he is looking at — and the point of
	// a second channel is having the next replay ready while the first is on
	// air, which cannot be done if only one of them can be seen.
	auto *aBox = new QWidget(row);
	{
		auto *av = new QVBoxLayout(aBox);
		av->setContentsMargins(0, 0, 0, 0);
		av->setSpacing(1);
		displayA_ = new OBSQTDisplay(aBox);
		displayA_->setRenderCallback(&MultiReplayDock::drawChannelA, this);
		displayA_->setSizePolicy(QSizePolicy::Expanding,
					 QSizePolicy::Expanding);
		displayA_->setMinimumHeight(40);
		av->addWidget(displayA_, 1);
		labelA_ = new QLabel(QStringLiteral("A"), aBox);
		labelA_->setObjectName("mrChanTag");
		labelA_->setProperty("chan", QStringLiteral("A"));
		labelA_->setProperty("active", true); // A is where the panel starts
		labelA_->setAlignment(Qt::AlignCenter);
		av->addWidget(labelA_);
	}
	auto *bBox = new QWidget(row);
	{
		auto *bv = new QVBoxLayout(bBox);
		bv->setContentsMargins(0, 0, 0, 0);
		bv->setSpacing(1);
		displayB_ = new OBSQTDisplay(bBox);
		displayB_->setRenderCallback(&MultiReplayDock::drawChannelB, this);
		displayB_->setSizePolicy(QSizePolicy::Expanding,
					 QSizePolicy::Expanding);
		displayB_->setMinimumHeight(40);
		bv->addWidget(displayB_, 1);
		labelB_ = new QLabel(QStringLiteral("B"), bBox);
		labelB_->setObjectName("mrChanTag");
		labelB_->setProperty("chan", QStringLiteral("B"));
		labelB_->setProperty("active", false);
		labelB_->setAlignment(Qt::AlignCenter);
		bv->addWidget(labelB_);
	}
	rh->addWidget(aBox, 3);
	rh->addWidget(bBox, 3);
	rh->addWidget(buildMultiview(), 2);
	v->addWidget(row, 1);

	// The green strip the reference controller puts directly under the A output: which list, which
	// clip of how many, how much of it is left, the event id, how far the
	// playhead is past IN and short of OUT, the timecode and the speed. All of
	// it is state we already had and were making the operator infer.
	auto *strip = new QWidget(box);
	auto *sh = new QHBoxLayout(strip);
	sh->setContentsMargins(0, 0, 0, 0);
	sh->setSpacing(0);
	chanBadge_ = new QLabel(QStringLiteral("A1"), strip);
	chanBadge_->setObjectName("mrChanBadge");
	chanBadge_->setAlignment(Qt::AlignCenter);
	chanStrip_ = new QLabel(strip);
	chanStrip_->setObjectName("mrChanStrip");
	chanStrip_->setFont(QFont(monoFamily()));
	chanStrip_->setTextFormat(Qt::PlainText);
	sh->addWidget(chanBadge_);
	sh->addWidget(chanStrip_, 1);
	v->addWidget(strip);

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

		t.box = new QWidget(multiviewBox_);
		t.box->setObjectName(QStringLiteral("mrTile"));
		auto *tv = new QVBoxLayout(t.box);
		tv->setContentsMargins(0, 0, 0, 0);
		tv->setSpacing(0);

		t.display = new OBSQTDisplay(t.box);
		t.display->setSizePolicy(QSizePolicy::Expanding,
					 QSizePolicy::Expanding);
		// Small on purpose: a tile costs a present() of its own on the
		// shared graphics thread, and the operator is checking framing
		// here, not focus.
		t.display->setMinimumSize(80, 45);
		tileCtx_[i].dock = this;
		tileCtx_[i].slot = i;
		t.display->setRenderCallback(&MultiReplayDock::drawTile,
					     &tileCtx_[i]);
		// Clicking a tile selects that angle: it is the shortest path there
		// is from "that camera has it" to "put that camera up", and it is
		// what an operator tries first. Handled in eventFilter().
		t.display->installEventFilter(this);
		tv->addWidget(t.display, 1);

		t.caption = new QLabel(t.box);
		t.caption->setObjectName(QStringLiteral("mrTileCap"));
		t.caption->setTextFormat(Qt::PlainText);
		t.caption->installEventFilter(this);
		tv->addWidget(t.caption, 0);

		t.box->setVisible(false); // rebuildMultiview() decides
		multiviewGrid_->addWidget(t.box, i / 2, i % 2);
	}

	rebuildMultiview();
	return multiviewBox_;
}

void MultiReplayDock::rebuildMultiview()
{
	if (!multiviewBox_ || !multiviewGrid_)
		return;

	const Config cfg = ReplayCore::instance().getConfig();

	// Which tiles belong on screen, and what they are called. Only configured
	// cameras: eight black rectangles on a two-camera rig is six tiles of
	// nothing between the operator and the two that matter — and six
	// obs_displays doing it.
	// NOT called `slots`: with Qt keywords enabled that is a macro expanding
	// to nothing, and the declaration silently becomes a no-op.
	std::vector<int> tileSlots;
	QStringList captions;
	for (int i = 0; i < kMaxCameras; i++) {
		if (cfg.cameras[i].sourceName.empty())
			continue;
		const std::string &dn = cfg.cameras[i].displayName;
		tileSlots.push_back(i);
		captions << QString("%1 %2").arg(i + 1).arg(
			dn.empty() ? QString("Cam %1").arg(i + 1)
				   : QString::fromStdString(dn));
	}
	// The multiview is CAMERAS only. It used to carry a "Replay" tile as
	// well, from the time there was one channel and the big preview followed
	// the live camera — so "what will go on air" had nowhere else to be
	// seen. There are two output boxes above it now, A and B, bigger,
	// labelled and always right: a third, smaller copy of the same picture
	// is a tile taken away from the cameras and one more display on the
	// shared graphics thread for nothing.

	const bool show = cfg.showMultiview;
	QStringList sigParts;
	for (int s : tileSlots)
		sigParts << QString::number(s);
	const QString sig = QString::number(show ? 1 : 0) + '|' +
			    sigParts.join(',') + '|' + captions.join(',');
	if (sig == multiviewSig_)
		return;
	multiviewSig_ = sig;

	multiviewBox_->setVisible(show);

	// Columns: two up to four tiles, three beyond. A tile narrower than about
	// a sixth of the dock stops being a picture and becomes a smear.
	const int cols = tileSlots.size() <= 4 ? 2 : 3;
	for (int i = 0; i < kMaxPreviewTiles; i++)
		if (tiles_[i].box)
			tiles_[i].box->setVisible(false);
	for (size_t k = 0; k < tileSlots.size(); k++) {
		PreviewTile &t = tiles_[tileSlots[k]];
		if (!t.box)
			continue;
		t.caption->setText(captions[(int)k]);
		t.caption->setToolTip(
			t.cam0 >= 0 ? QString("%1 %2")
					      .arg(obs_module_text("Dock.Angle"))
					      .arg(t.cam0 + 1)
				    : QString(obs_module_text("Dock.ReplayTileHint")));
		// Moving a widget between cells of the grid it is ALREADY in does
		// not re-parent it (QLayout::addChildWidget only calls setParent
		// when the parent differs), so the native window — and the display
		// bound to it — survives.
		multiviewGrid_->addWidget(t.box, (int)k / cols, (int)k % cols);
		t.box->setVisible(show);
	}
	tileTallyPvw_ = -2; // captions were just rewritten
	tileTallyPgm_ = -2;
	updateMultiviewTally();
}

void MultiReplayDock::refreshTileSources()
{
	const Config cfg = ReplayCore::instance().getConfig();
	for (int i = 0; i < kMaxPreviewTiles; i++) {
		PreviewTile &t = tiles_[i];
		obs_source_t *next = nullptr;
		// A hidden tile publishes nothing: its display is disabled anyway
		// (OBSQTDisplay disables on QEvent::Hide), and holding a reference
		// to a source nobody is drawing keeps it alive for no reason.
		if (t.box && t.box->isVisible()) {
			if (t.cam0 < 0) {
				if (previewHasContent_)
					next = chan()
						       .acquireSource();
			} else {
				const std::string &nm =
					cfg.cameras[t.cam0].sourceName;
				if (!nm.empty())
					next = obs_get_source_by_name(nm.c_str());
			}
		}
		obs_source_t *prev = nullptr;
		{
			std::lock_guard<std::mutex> lk(tileMutex_);
			prev = tileSource_[i];
			tileSource_[i] = next;
		}
		// Released outside the lock: the last release destroys the source,
		// which enters the graphics context — and the graphics thread is
		// what wants tileMutex_. See the note on previewSource_.
		if (prev)
			obs_source_release(prev);
	}
}

MultiReplayDock::PreviewStats MultiReplayDock::previewStats() const
{
	PreviewStats s;
	// Long enough that a dock still being laid out is not called starved,
	// short enough that a preview an operator is already looking at is. The
	// display itself forces its own creation at 3 s (see qt-display), so a
	// widget still empty at 5 s has failed for a reason no waiting will fix.
	constexpr int64_t kStarvedMs = 5000;
	const auto account = [&s](const OBSQTDisplay *d) {
		if (!d)
			return;
		s.tiles++;
		if (!d->isVisible())
			return;
		s.visible++;
		if (d->display()) {
			s.withDisplay++;
			return;
		}
		const int64_t waited = d->blockedMs();
		s.worstBlockedMs = std::max(s.worstBlockedMs, waited);
		if (waited >= kStarvedMs)
			s.starved++;
	};
	account(displayA_);
	for (const PreviewTile &t : tiles_)
		account(t.display);
	return s;
}

// Where the zones ended up, in the dock's own coordinates. See LayoutProbe in
// the header for why the gate is allowed to ask.
MultiReplayDock::LayoutProbe MultiReplayDock::layoutProbe() const
{
	LayoutProbe lp;
	const auto topOf = [this](const QWidget *w) {
		return w ? w->mapTo(this, QPoint(0, 0)).y() : -1;
	};
	const auto bottomOf = [&topOf](const QWidget *w) {
		return w ? topOf(w) + w->height() : -1;
	};
	// The picture block is the big preview AND the tile grid beside it,
	// whichever reaches lower: "under the pictures" has to mean under all of
	// them.
	lp.previewBottomY =
		std::max(bottomOf(displayA_), bottomOf(multiviewBox_));
	lp.searchY = topOf(search_);
	lp.listTabsY = topOf(listTabs_);
	lp.tableY = topOf(events_);
	lp.clipBarY = topOf(clipBar_);
	lp.seekY = topOf(seek_);
	if (seek_) {
		lp.seekHeight = seek_->height();
		lp.seekGraduations = seek_->graduations();
		lp.seekEnabled = seek_->hasTimeline();
	}
	return lp;
}

void MultiReplayDock::updateMultiviewTally()
{
	const auto ps = pc().playState();
	const int pvw = currentAngle1_ - 1;
	const int pgm = (ps.active && ps.angle1 > 0) ? ps.angle1 - 1 : -1;
	if (pvw == tileTallyPvw_ && pgm == tileTallyPgm_)
		return;
	tileTallyPvw_ = pvw;
	tileTallyPgm_ = pgm;
	for (int i = 0; i < kMaxPreviewTiles; i++) {
		PreviewTile &t = tiles_[i];
		if (!t.caption)
			continue;
		QString tally;
		if (t.cam0 < 0)
			tally = QStringLiteral("replay");
		else if (t.cam0 == pgm)
			tally = QStringLiteral("pgm");
		else if (t.cam0 == pvw)
			tally = QStringLiteral("pvw");
		if (t.caption->property("tally").toString() == tally)
			continue;
		t.caption->setProperty("tally", tally);
		repolish(t.caption);
	}
}

// ---------------------------------------------------------------------------
// Angle row — the reference controller's "A [1..8]" camera matrix
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildAngleMatrix()
{
	auto *row = new QWidget(this);
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(3);

	// the reference controller has an A row and a B row here. There is one replay channel in this
	// plugin, so there is one row, and it keeps the "A" prefix: the operator
	// reads it as the A row he knows, not as an unlabelled strip of numbers.
	h->addWidget(sectionLabel(QStringLiteral("A"), row));

	anglesA_ = new QButtonGroup(this);
	anglesA_->setExclusive(true);
	for (int i = 1; i <= kNCams; i++) {
		auto *b = new QPushButton(QString::number(i), row);
		b->setObjectName("mrAngle");
		b->setCheckable(true);
		b->setCursor(Qt::PointingHandCursor);
		b->setToolTip(QString("%1 %2")
				      .arg(obs_module_text("Dock.Angle"))
				      .arg(i));
		anglesA_->addButton(b, i);
		h->addWidget(b);
	}
	connect(anglesA_, &QButtonGroup::idClicked, this,
		[this](int id) { setAngle(id); });

	return row;
}

// ---------------------------------------------------------------------------
// Transport — the reference controller's centre group: ⏸ ◀ ↺ [Play Events ▾] NOW ⏭ Loop ♫
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildTransport()
{
	auto *box = new QWidget(this);
	// Two lines, as on the reference panel: the keys, and the big timecode centred
	// underneath them.
	auto *col = new QVBoxLayout(box);
	col->setContentsMargins(0, 0, 0, 0);
	col->setSpacing(1);
	auto *tr = new QHBoxLayout();
	tr->setContentsMargins(0, 0, 0, 0);
	tr->setSpacing(3);

	// ▶ U+25B6
	playPauseBtn_ = transportBtn(QStringLiteral("▶"), this,
				     obs_module_text("Dock.PlayPause"), "mrPlay");

	// the reference controller has a reverse-play key right here. This engine decodes forward
	// only, so the key keeps its place and is DISABLED with a tooltip that
	// says so: an operator hunting for it finds it greyed out in one glance
	// instead of concluding the panel is missing controls.
	auto *revBtn = transportBtn(QStringLiteral("◀"), this,
				    obs_module_text("Dock.ReverseUnavailable"));
	revBtn->setEnabled(false);

	// ↺ the reference controller "instantly play last event".
	auto *lastBtn = transportBtn(QStringLiteral("↺"), this,
				     obs_module_text("Dock.PlayLast"));
	connect(lastBtn, &QPushButton::clicked, this, [this]() {
		std::string err;
		if (!pc().playLastEvent(
			    currentAngle1_ - 1,
			    toOutputBtn_ && toOutputBtn_->isChecked(), err))
			QMessageBox::warning(this, "obs-multireplay",
					     QString::fromStdString(err));
	});

	// the reference controller's "Play Events". The gate finds this button by its module text, so
	// the LABEL may move with the locale but the widget must stay a plain
	// QPushButton whose text is exactly obs_module_text("Dock.PlaySelected") —
	// no menu on it (a QPushButton with a menu swallows click()), hence the
	// separate ▾ beside it, which is where the reference controller keeps the same options.
	auto *playSel = compactBtn(obs_module_text("Dock.PlaySelected"), this,
				   "mrAccent");
	connect(playSel, &QPushButton::clicked, this,
		&MultiReplayDock::playSelected);

	auto *more = new QToolButton(this);
	more->setObjectName("mrGear");
	more->setText(QStringLiteral("▾"));
	// A QToolButton defaults to icon-only, and with no icon that is a blank
	// key with a menu arrow. These are glyph buttons, so say text-only.
	more->setToolButtonStyle(Qt::ToolButtonTextOnly);
	more->setCursor(Qt::PointingHandCursor);
	more->setToolTip(obs_module_text("Dock.PlayOptions"));
	more->setPopupMode(QToolButton::InstantPopup);
	{
		auto *menu = new QMenu(more);
		auto *actOut = menu->addAction(obs_module_text("Dock.PlayToOutput"));
		auto *actLast = menu->addAction(obs_module_text("Dock.PlayLast"));
		menu->addSeparator();
		auto *actStop = menu->addAction(obs_module_text("Dock.Stop"));
		more->setMenu(menu);
		connect(actOut, &QAction::triggered, this, [this]() {
			std::string err;
			if (!pc().playEvents(
				    selectedEventIds(), currentAngle1_ - 1,
				    /*toOutput*/ true, err))
				showNotice(QString::fromStdString(err));
		});
		connect(actLast, &QAction::triggered, this, [this]() {
			std::string err;
			if (!pc().playLastEvent(
				    currentAngle1_ - 1,
				    toOutputBtn_ && toOutputBtn_->isChecked(),
				    err))
				showNotice(QString::fromStdString(err));
		});
		connect(actStop, &QAction::triggered, this,
			[this]() { pc().stopEvents(); });
	}

	nowBtn_ = new QPushButton(QStringLiteral("NOW"), this);
	nowBtn_->setObjectName("mrNow");
	nowBtn_->setProperty("live", false);
	nowBtn_->setCursor(Qt::PointingHandCursor);
	nowBtn_->setToolTip(obs_module_text("Dock.JumpToNow"));
	nowBtn_->setMinimumWidth(38);

	// ⏭ U+23ED — one frame forward (the reference controller frame-by-frame). Forward only: the
	// engine decodes forward, and a backward step is not a v1 feature.
	// EXACTLY ONE button in this dock may carry this glyph: the gate finds the
	// frame step by it.
	auto *stepBtn = transportBtn(QStringLiteral("⏭"), this,
				     obs_module_text("Dock.StepFwd"));
	connect(stepBtn, &QPushButton::clicked, this,
		[this]() { stepFrameForward(); });

	loopBtn_ = toggleBtn(obs_module_text("Dock.Loop"), this,
			     obs_module_text("Dock.Loop"));
	connect(loopBtn_, &QPushButton::toggled, this,
		[this](bool on) { pc().setLoop(on); });

	musicBtn_ = toggleBtn(QStringLiteral("♫"), this,
			      obs_module_text("Dock.Music"));
	connect(musicBtn_, &QPushButton::toggled, this, [this](bool on) {
		pc().setMusicEnabled(on);
	});

	toOutputBtn_ = toggleBtn(obs_module_text("Dock.ToOutput"), this,
				 obs_module_text("Dock.ToOutput"));

	tr->addWidget(playPauseBtn_);
	tr->addWidget(revBtn);
	tr->addWidget(lastBtn);
	tr->addWidget(playSel);
	tr->addWidget(more);
	tr->addWidget(nowBtn_);
	tr->addWidget(stepBtn);
	tr->addSpacing(6);
	tr->addWidget(loopBtn_);
	tr->addWidget(musicBtn_);
	tr->addWidget(toOutputBtn_);
	col->addLayout(tr);

	tcLbl_ = new QLabel(QStringLiteral("00:00.000 / 00:00.000"), box);
	tcLbl_->setObjectName("mrTimecode");
	tcLbl_->setFont(QFont(monoFamily()));
	tcLbl_->setAlignment(Qt::AlignCenter);
	col->addWidget(tcLbl_);

	// wire transport actions
	connect(playPauseBtn_, &QPushButton::clicked, this, [this]() {
		// There is no free-running playhead to pause any more: the engine
		// plays a clip. ▶ re-cues the selected event, ⏸ stops it.
		if (chan().playing()) {
			pc().stopEvents();
			return;
		}
		ReplayCore::instance().setFollowLive(false);
		replayCurrent();
	});
	connect(nowBtn_, &QPushButton::clicked, this, [this]() {
		// the reference controller NOW: drop the replay and watch the live edge again.
		pc().stopEvents();
		ReplayCore::instance().setFollowLive(true);
	});

	return box;
}

// ---------------------------------------------------------------------------
// Bottom bar — the reference controller's two control rows plus the full-width position bar
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildBottomBar()
{
	auto *box = new QWidget(this);
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(3);

	// ── Row 1: mark keys · angle row · clip actions ───────────────────
	{
		auto *h = new QHBoxLayout();
		h->setSpacing(4);
		h->addWidget(buildMarkers());
		h->addStretch(1);
		h->addWidget(buildAngleMatrix());
		h->addStretch(1);

		// the reference controller's "Export Clips", in the reference controller's corner.
		auto *exp = compactBtn(obs_module_text("Dock.ExportClips"), this);
		connect(exp, &QPushButton::clicked, this, [this]() {
			auto ids = selectedEventIds();
			if (ids.empty())
				return;
			QString folder = QFileDialog::getExistingDirectory(
				this, obs_module_text("Dock.ExportFolder"));
			if (folder.isEmpty())
				return;
			std::string err;
			for (int id : ids)
				ExportManager::instance().exportEvent(
					id, 0, folder.toStdString(), err);
		});
		h->addWidget(exp);

		// ...and the whole selection as ONE file: the highlights reel.
		// Same events, same order, same angles, same speeds — one clip
		// after another, with the operator's music over it if the ♫ key
		// is down. The music key doubles as the choice here rather than
		// asking in a dialog: it is already the panel's answer to "do I
		// want music with my replays".
		auto *reel = compactBtn(obs_module_text("Dock.ExportReel"), this);
		reel->setToolTip(obs_module_text("Dock.ExportReelHint"));
		connect(reel, &QPushButton::clicked, this, [this]() {
			auto ids = selectedEventIds();
			if (ids.empty()) {
				showNotice(obs_module_text("Dock.SelectToReorder"));
				return;
			}
			QString folder = QFileDialog::getExistingDirectory(
				this, obs_module_text("Dock.ExportFolder"));
			if (folder.isEmpty())
				return;
			const bool music = musicBtn_ && musicBtn_->isChecked();
			std::string err;
			if (!ExportManager::instance().exportSequence(
				    ids, music, folder.toStdString(), err))
				showNotice(QString::fromStdString(err));
			else
				showNotice(obs_module_text("Dock.ExportReelStarted"));
		});
		h->addWidget(reel);

		// The running order is the operator's. the reference controller sorts by time or by
		// the order marks were taken; neither is the order a highlights
		// reel goes out in, and the only way to get that one is by hand.
		// Two keys rather than drag-and-drop: a drag inside a table whose
		// cells are all editable is a click away from starting an edit
		// instead, and during a match that is the wrong thing to risk.
		for (const auto &mv : {std::pair<const char *, int>{"▲", -1},
				       std::pair<const char *, int>{"▼", +1}}) {
			const int delta = mv.second;
			auto *b = transportBtn(QString::fromUtf8(mv.first), this,
					       obs_module_text(delta < 0
								       ? "Dock.MoveUp"
								       : "Dock.MoveDown"));
			connect(b, &QPushButton::clicked, this,
				[this, delta]() { moveSelectedEvent(delta); });
			h->addWidget(b);
		}

		// Duplicate / delete / delete-all have no place of their own on the
		// reference panel (they live in its context menu), and four more buttons
		// on this row would be four more things to read past. They are here,
		// one click away, and on the table's right-click menu as well.
		auto *edit = new QToolButton(this);
		edit->setObjectName("mrGear");
		edit->setText(QStringLiteral("⋯"));
		edit->setToolButtonStyle(Qt::ToolButtonTextOnly);
		edit->setCursor(Qt::PointingHandCursor);
		edit->setToolTip(obs_module_text("Dock.ClipActions"));
		edit->setPopupMode(QToolButton::InstantPopup);
		{
			auto *menu = new QMenu(edit);
			auto *actDup =
				menu->addAction(obs_module_text("Dock.Duplicate"));
			auto *actDel =
				menu->addAction(obs_module_text("Dock.Delete"));
			menu->addSeparator();
			auto *actAll =
				menu->addAction(obs_module_text("Dock.DeleteAll"));
			edit->setMenu(menu);
			connect(actDup, &QAction::triggered, this, [this]() {
				for (int id : selectedEventIds())
					EventStore::instance().duplicate(id);
				refreshEvents();
			});
			connect(actDel, &QAction::triggered, this, [this]() {
				for (int id : selectedEventIds())
					EventStore::instance().remove(id);
				refreshEvents();
			});
			connect(actAll, &QAction::triggered, this, [this]() {
				if (QMessageBox::question(
					    this, "obs-multireplay",
					    obs_module_text(
						    "Dock.DeleteAllConfirm"),
					    QMessageBox::Yes | QMessageBox::No,
					    QMessageBox::No) != QMessageBox::Yes)
					return;
				pc().stopEvents();
				EventStore::instance().clearAll();
			});
		}
		h->addWidget(edit);
		v->addLayout(h);
	}

	// ── Row 2: ⚙ · REC · clock  |  transport  |  slow-motion speed ────
	{
		auto *h = new QHBoxLayout();
		h->setSpacing(4);

		auto *gear = new QToolButton(this);
		gear->setObjectName("mrGear");
		gear->setText(QStringLiteral("⚙"));
		gear->setToolButtonStyle(Qt::ToolButtonTextOnly);
		gear->setCursor(Qt::PointingHandCursor);
		gear->setToolTip(obs_module_text("Dock.Settings"));
		gear->setPopupMode(QToolButton::InstantPopup);
		{
			auto *menu = new QMenu(gear);
			auto *actNew =
				menu->addAction(obs_module_text("Dock.NewProject"));
			auto *actOpen = menu->addAction(
				obs_module_text("Dock.OpenProject"));
			menu->addSeparator();
			auto *actSettings =
				menu->addAction(obs_module_text("Dock.Settings"));
			auto *actRename =
				menu->addAction(obs_module_text("Dock.RenameList"));
			menu->addSeparator();
			auto *actChapters = menu->addAction(
				obs_module_text("Dock.YouTubeChapters"));
			gear->setMenu(menu);
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
		h->addWidget(gear);

		recBtn_ = new QPushButton(QStringLiteral("●  REC"), this);
		recBtn_->setObjectName("mrRec");
		recBtn_->setProperty("recording", false);
		recBtn_->setMinimumWidth(84);
		recBtn_->setCursor(Qt::PointingHandCursor);
		connect(recBtn_, &QPushButton::clicked, this, [this]() {
			auto &core = ReplayCore::instance();
			if (core.isRecording()) {
				core.stopRecording();
			} else {
				// Stop any event playing BEFORE arming: a new take
				// must not start while a clip is still being paced
				// into the replay input.
				pc().stopEvents();
				std::string err;
				if (!core.startRecording(err))
					QMessageBox::warning(
						this, "obs-multireplay",
						QString::fromStdString(err));
			}
			poll();
		});
		h->addWidget(recBtn_);

		// M4: the health badge lives next to the record key because that
		// is where the eye already goes when a take starts, and because
		// what it reports is always about the take. Hidden unless there
		// is something to say (see poll()).
		healthBtn_ = new QPushButton(this);
		healthBtn_->setObjectName("mrHealth");
		healthBtn_->setCursor(Qt::PointingHandCursor);
		healthBtn_->setFlat(true);
		healthBtn_->hide();
		connect(healthBtn_, &QPushButton::clicked, this,
			&MultiReplayDock::showHealthDetails);
		h->addWidget(healthBtn_);

		// the reference controller stacks the wall clock over the remaining recording time,
		// right of the record key, in red. Same two lines, same place.
		auto *clockBox = new QWidget(this);
		auto *cv = new QVBoxLayout(clockBox);
		cv->setContentsMargins(2, 0, 2, 0);
		cv->setSpacing(0);
		clockLbl_ = new QLabel(clockBox);
		clockLbl_->setObjectName("mrClock");
		clockLbl_->setFont(QFont(monoFamily()));
		statusLbl_ = new QLabel(clockBox);
		statusLbl_->setObjectName("mrMuted");
		cv->addWidget(clockLbl_);
		cv->addWidget(statusLbl_);
		h->addWidget(clockBox);

		h->addStretch(1);
		h->addWidget(buildTransport());
		h->addStretch(1);

		// Slow-motion presets, the reference controller's set (25/33/50/75/100) plus the 2×
		// that is its fast forward — the engine takes any speed, since a
		// speed is only the spacing between frames.
		speedChips_ = new QButtonGroup(this);
		speedChips_->setExclusive(false);
		const std::pair<int, const char *> speedPresets[] = {
			{25, "25%"},  {33, "33%"},  {50, "50%"},
			{75, "75%"},  {100, "100%"}, {200, "2\xc3\x97"}};
		for (const auto &[pct, lbl] : speedPresets) {
			int p = pct; // copy: capturing a structured binding is
				     // non-portable
			auto *b = compactBtn(QString::fromUtf8(lbl), this,
					     "mrSpeedChip");
			speedChips_->addButton(b, p);
			connect(b, &QPushButton::clicked, this, [this, p]() {
				QSignalBlocker block(speed_);
				speed_->setValue(p);
				speedLbl_->setText(QString::asprintf(
					"%.2f\xc3\x97", p / 100.0));
				applyReplaySpeed(p);
			});
			h->addWidget(b);
		}

		speed_ = new QSlider(Qt::Horizontal, this);
		speed_->setObjectName("mrSpeed");
		// Up to 2×: the reference controller's variable speed is 0-100%, and its fast forward is
		// the same control pushed past 1×.
		speed_->setRange(5, 200);
		speed_->setValue(100);
		speed_->setMinimumWidth(70);
		speed_->setMaximumWidth(150);
		speed_->setTickPosition(QSlider::TicksBelow);
		speed_->setTickInterval(25);
		speed_->setToolTip(obs_module_text("Dock.SpeedSliderHint"));
		speed_->setCursor(Qt::PointingHandCursor);

		speedLbl_ = new QLabel(QStringLiteral("1.00\xc3\x97"), this);
		speedLbl_->setObjectName("mrTimecode");
		speedLbl_->setFont(QFont(monoFamily()));
		speedLbl_->setMinimumWidth(42);
		speedLbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		connect(speed_, &QSlider::valueChanged, this, [this](int val) {
			speedLbl_->setText(
				QString::asprintf("%.2f\xc3\x97", val / 100.0));
		});
		connect(speed_, &QSlider::sliderReleased, this,
			[this]() { applyReplaySpeed(speed_->value()); });

		h->addWidget(speed_, 0);
		h->addWidget(speedLbl_);
		v->addLayout(h);
	}

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
		// FULL WIDTH and nothing beside it, as in the reference controller. The green band is
		// the one thing on this panel that has to be readable from across
		// the room; keys parked at its end shorten it and, worse, read as
		// belonging to it. They now live in the rows above, with the
		// other keys of their own kind.
		h->addWidget(clipBar_, 1);
		v->addLayout(h);
	}

	// The keys that used to sit beside the green band, in the rows where
	// they belong (the reference controller's own arrangement, see reference-gui/GUI.png).
	{
		auto *h = new QHBoxLayout();
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(3);

		// the reference controller's channel selector, right where the reference controller puts it: A|B / A / B,
		// then the swap. It says where the transport keys, the marks and
		// the play buttons go — not what is on air, which is whatever the
		// operator's scenes are showing.
		chanSel_ = new QButtonGroup(this);
		chanSel_->setExclusive(true);
		const std::pair<const char *, int> chanChoices[] = {
			{"A|B", 2}, {"A", 0}, {"B", 1}};
		for (const auto &[label, code] : chanChoices) {
			auto *b = new QPushButton(QString::fromUtf8(label), this);
			b->setObjectName("mrChanSel");
			b->setCheckable(true);
			b->setChecked(code == 0); // A, as it has always been
			b->setMinimumWidth(30);
			b->setCursor(Qt::PointingHandCursor);
			chanSel_->addButton(b, code);
			h->addWidget(b, 0);
		}
		connect(chanSel_, &QButtonGroup::idClicked, this, [this](int code) {
			setActiveChannel(code == 1 ? Which::B : Which::A,
					 code == 2);
		});

		swapBtn_ = new QPushButton(QStringLiteral("⇄"), this);
		swapBtn_->setObjectName("mrChanSel");
		swapBtn_->setToolTip(obs_module_text("Dock.SwapChannels"));
		swapBtn_->setMinimumWidth(28);
		swapBtn_->setCursor(Qt::PointingHandCursor);
		connect(swapBtn_, &QPushButton::clicked, this,
			&MultiReplayDock::swapChannels);
		h->addWidget(swapBtn_, 0);

		// ">>", never the ⏭ glyph: EXACTLY ONE button in this dock may
		// carry that one (the frame step), and the gate finds it by it.
		nextClipBtn_ = transportBtn(QStringLiteral(">>"), this,
					    obs_module_text("Dock.NextClip"));
		nextClipBtn_->setMinimumHeight(24);
		connect(nextClipBtn_, &QPushButton::clicked, this, [this]() {
			// Logged both ways: "I pressed >> and nothing happened"
			// is otherwise indistinguishable from "the press never
			// arrived", and one of those is a bug in the dock.
			const bool moved =
				pc().skipToNext();
			const auto ps = pc().playState();
			obs_log(LOG_INFO,
				"[dock] >> skip: %s (queue %d/%d, angle %d)",
				moved ? "advanced" : "nothing queued", ps.queuePos,
				ps.queued, ps.angle1);
			if (!moved)
				showNotice(obs_module_text("Dock.NothingQueued"));
		});
		h->addWidget(nextClipBtn_, 0);
		h->addStretch(1);
		// This row is read left to right as "which channel, then what to
		// do to it", so it ends where the operator's eye is going next:
		// the bar underneath.
		v->insertLayout(v->count() - 1, h);
	}

	// ── Row 4: the position bar over the whole recorded timeline ──────
	// Graduated, and the widest thing on the panel: it is the only control
	// that reaches the whole project, and the operator finds it by its scale.
	seek_ = new SeekBar(this);
	seek_->setToolTip(obs_module_text("Dock.SeekHint"));
	// The zoom key: it shows the factor because a bar that is showing four
	// seconds of an hour looks exactly like a bar over a four-second
	// session, and the operator has to be able to tell those apart at a
	// glance. Clicking it goes back to the whole timeline.
	zoomBtn_ = new QPushButton(QStringLiteral("1×"), this);
	zoomBtn_->setObjectName("mrChanSel");
	zoomBtn_->setToolTip(obs_module_text("Dock.ZoomFit"));
	zoomBtn_->setMinimumWidth(34);
	zoomBtn_->setCursor(Qt::PointingHandCursor);
	connect(zoomBtn_, &QPushButton::clicked, this,
		[this]() { seek_->setZoom(1.0, 0.5); });
	connect(seek_, &SeekBar::zoomChanged, this, [this](double z) {
		zoomBtn_->setText(z <= 1.001 ? QStringLiteral("1×")
					     : QString("%1×").arg(z, 0, 'f',
								  z < 10 ? 1 : 0));
		zoomBtn_->setProperty("level", z > 1.001 ? QStringLiteral("warn")
							 : QString());
		repolish(zoomBtn_);
	});
	connect(seek_, &SeekBar::scrubStateChanged, this,
		[this](bool dragging) { seekDragging_ = dragging; });
	connect(seek_, &SeekBar::scrubMoved, this, [this](double frac) {
		tcLbl_->setText(formatTc((int64_t)(frac * (double)displayDurNs_)) +
				" / " + formatTc(displayDurNs_));
	});
	connect(seek_, &SeekBar::seekRequested, this,
		[this](double frac) { seekToFraction(frac); });
	connect(seek_, &SeekBar::markerDragged, this,
		&MultiReplayDock::onMarkerDragged);
	// The bar has to see the mouse before a button is pressed, or the cursor
	// could never say "this edge can be grabbed".
	seek_->setMouseTracking(true);
	{
		// The zoom key sits at the right end OF THE BAR, not in a
		// toolbar: it is about this control and nothing else, and it is
		// also where the eye lands after reading the scale.
		auto *row = new QHBoxLayout();
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(4);
		row->addWidget(seek_, 1);
		row->addWidget(zoomBtn_, 0, Qt::AlignBottom);
		v->addLayout(row);
	}

	return box;
}

// ---------------------------------------------------------------------------
// Markers: Live/Recorded + IN/OUT + presets
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildMarkers()
{
	auto *box = new QWidget(this);
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(3);

	// the reference controller labels this group "Mark" and then the keys are bare: In, Out,
	// - 5, - 10, - 20. The caption carries the meaning, so the keys stay
	// short enough to hit without reading.
	h->addWidget(sectionLabel(obs_module_text("Dock.Mark"), box));

	auto *in = compactBtn(obs_module_text("Dock.MarkIn"), this, "mrAccent");
	auto *out = compactBtn(obs_module_text("Dock.MarkOut"), this, "mrAccent");
	connect(in, &QPushButton::clicked, this, [this]() {
		const int64_t t = markTimeNs();
		if (!markable(t))
			return;
		// Inherit the currently selected camera angle (0-based).
		EventStore::instance().markIn(t, currentAngle1_ - 1);
		refreshEvents();
	});
	connect(out, &QPushButton::clicked, this, [this]() {
		const int64_t t = markTimeNs();
		if (!markable(t))
			return;
		if (!EventStore::instance().markOut(t))
			QMessageBox::information(
				this, "obs-multireplay",
				obs_module_text("Dock.NoOpenEvent"));
		refreshEvents();
	});
	h->addWidget(in);
	h->addWidget(out);

	for (int sec : {5, 10, 20}) {
		auto *b = compactBtn(QString("-%1s").arg(sec), this);
		connect(b, &QPushButton::clicked, this, [this, sec]() {
			const int64_t t = markTimeNs();
			if (!markable(t))
				return;
			EventStore::instance().markInOut(t, sec,
							 currentAngle1_ - 1);
			refreshEvents();
		});
		h->addWidget(b);
	}

	// Trim: move the SELECTED event's in or out point to where the position
	// bar stands. A mark taken live is taken late by definition — the
	// operator saw the action first — and until now the only way to fix one
	// was to delete it and mark again from a scrub, which is two ways of
	// saying the same thing and one of them loses the angles and the
	// comments. Zoom the bar, put the playhead on the frame, press.
	//
	// Frame nudges are hotkeys rather than four more keys on a full row (see
	// registerDockHotkeys): a Stream Deck is where this kind of work
	// actually happens, and the panel is already dense.
	auto *trimIn = compactBtn(QStringLiteral("⇤IN"), this);
	trimIn->setToolTip(obs_module_text("Dock.TrimInHint"));
	connect(trimIn, &QPushButton::clicked, this,
		[this]() { setSelectedPoint(true); });
	auto *trimOut = compactBtn(QStringLiteral("OUT⇥"), this);
	trimOut->setToolTip(obs_module_text("Dock.TrimOutHint"));
	connect(trimOut, &QPushButton::clicked, this,
		[this]() { setSelectedPoint(false); });
	h->addWidget(trimIn);
	h->addWidget(trimOut);

	auto *cancel = compactBtn(obs_module_text("Dock.Cancel"), this, "mrDanger");
	connect(cancel, &QPushButton::clicked, this, [this]() {
		EventStore::instance().markCancel();
		refreshEvents();
	});
	h->addWidget(cancel);

	return box;
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
	events_->verticalHeader()->setDefaultSectionSize(22);
	events_->setAlternatingRowColors(true);
	events_->setShowGrid(false);
	events_->setWordWrap(false);
	events_->setFrameShape(QFrame::NoFrame);
	events_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	events_->setContextMenuPolicy(Qt::CustomContextMenu);
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
			QTableWidgetItem *it = events_->item(row, kColId);
			if (!it)
				return;
			const int id = it->data(Qt::UserRole).toInt();
			if (id <= 0)
				return;
			std::string err;
			if (!pc().playEvents(
				    {id}, currentAngle1_ - 1, /*toOutput*/ true,
				    err))
				showNotice(QString::fromStdString(err));
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
				for (int id : selectedEventIds())
					EventStore::instance().remove(id);
				refreshEvents();
			} else if (chosen == actExp) {
				auto ids = selectedEventIds();
				if (ids.empty())
					return;
				QString folder = QFileDialog::getExistingDirectory(
					this, obs_module_text("Dock.ExportFolder"));
				if (folder.isEmpty())
					return;
				std::string err;
				for (int id : ids)
					ExportManager::instance().exportEvent(
						id, 0, folder.toStdString(), err);
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

// ---------------------------------------------------------------------------
// Table columns: the fixed ones plus one per configured camera (the reference controller)
// ---------------------------------------------------------------------------

void MultiReplayDock::rebuildEventColumns()
{
	if (!events_)
		return;

	// Which cameras deserve a column, and what they are called. Only the
	// configured ones: eight columns on a two-camera rig is six columns of
	// dashes between the operator and the two that matter.
	const Config cfg = ReplayCore::instance().getConfig();
	std::vector<int> cams;
	QStringList camLabels;
	for (int i = 0; i < kNCams; i++) {
		if (cfg.cameras[i].sourceName.empty())
			continue;
		const std::string &dn = cfg.cameras[i].displayName;
		cams.push_back(i);
		camLabels << QString("%1 %2").arg(i + 1).arg(
			dn.empty() ? QString("Cam %1").arg(i + 1)
				   : QString::fromStdString(dn));
	}

	// Rebuilding blows the table away, so only do it when the answer changed.
	// The labels are part of the answer: renaming a camera has to show.
	QStringList wanted;
	for (int c : cams)
		wanted << QString::number(c);
	static const char *kProp = "mrCamSig";
	const QString sig = wanted.join(',') + '|' + camLabels.join(',');
	if (events_->columnCount() > 0 &&
	    events_->property(kProp).toString() == sig)
		return;
	events_->setProperty(kProp, sig);

	camCols_ = cams;
	camHeaderHot_ = -1; // the highlight belongs to sections that just died

	const bool wasRefreshing = refreshing_;
	refreshing_ = true; // setHorizontalHeaderItem must not read back as an edit
	events_->setRowCount(0);
	events_->setColumnCount(kColFirstCam +
			        (int)cams.size() * kColsPerCam);
	QStringList headers;
	headers << QStringLiteral("#") << obs_module_text("Dock.In")
		<< obs_module_text("Dock.Out") << obs_module_text("Dock.Duration");
	// One heading per camera now, because there is one column per camera:
	// the check, the speed and the comment are three answers about the same
	// angle, and they used to be split across two headings the eye had to
	// pair up again on every row.
	for (size_t i = 0; i < cams.size(); i++)
		headers << camLabels[(int)i];
	events_->setHorizontalHeaderLabels(headers);
	// The camera headers carry their own label in UserRole: the "angle I am
	// watching" marker is a prefix on the text (see updateCamHeaderHighlight),
	// so the plain label has to survive somewhere.
	for (size_t i = 0; i < cams.size(); i++) {
		QTableWidgetItem *h = events_->horizontalHeaderItem(
			kColFirstCam + (int)i * kColsPerCam);
		if (h)
			h->setData(Qt::UserRole, camLabels[(int)i]);
	}
	{
		QHeaderView *hh = events_->horizontalHeader();
		hh->setHighlightSections(false);
		for (int c = 0; c < events_->columnCount(); c++)
			hh->setSectionResizeMode(c, QHeaderView::ResizeToContents);
		// The camera columns take the slack and share it equally: each
		// holds free text (the comment) and so has no natural width, and
		// four angles that are the same width are four angles the eye can
		// scan down without re-measuring.
		for (int c = kColFirstCam; c < events_->columnCount(); c++)
			hh->setSectionResizeMode(c, QHeaderView::Stretch);
		hh->setMinimumSectionSize(34);
	}
	refreshing_ = wasRefreshing;
	updateCamHeaderHighlight();
}

void MultiReplayDock::updateCamHeaderHighlight()
{
	if (!events_ || camCols_.empty())
		return;
	int hot = -1;
	for (size_t i = 0; i < camCols_.size(); i++)
		if (camCols_[i] == currentAngle1_ - 1)
			hot = kColFirstCam + (int)i * kColsPerCam;
	if (hot == camHeaderHot_)
		return;
	const bool wasRefreshing = refreshing_;
	refreshing_ = true;
	for (size_t i = 0; i < camCols_.size(); i++) {
		const int col = kColFirstCam + (int)i * kColsPerCam;
		QTableWidgetItem *h = events_->horizontalHeaderItem(col);
		if (!h)
			continue;
		// the reference controller fills this header green. We cannot: OBS's theme owns the
		// section background (see kDockStyle). A ▶ on the label says the
		// same thing and no theme can take it away.
		const QString base = h->data(Qt::UserRole).toString();
		h->setText(col == hot ? QStringLiteral("▶ ") + base : base);
	}
	refreshing_ = wasRefreshing;
	camHeaderHot_ = hot;
}

// ---------------------------------------------------------------------------
// Engine interaction helpers
// ---------------------------------------------------------------------------

int64_t MultiReplayDock::markTimeNs() const
{
	// Live: the newest instant the tap actually captured on this angle. It is
	// measured off the encoder on the shared system clock, so the mark lands
	// on the frame that was on screen and means the same instant on every
	// other angle — no arm timestamp, no encoder-startup lag to subtract.
	if (EventStore::instance().liveMode()) {
		int64_t now = PacketTap::instance().newestNs(currentAngle1_ - 1);
		if (now > 0) {
			MR_DLOG("[ev] markTime LIVE master=%lldms (cam %d)",
				(long long)(now / 1000000), currentAngle1_);
			return now;
		}
	}
	// Recorded mode is the reference controller's "mark at the position of the bar", and the bar is
	// the dock's playhead — where the operator parked the timeline with a
	// scrub, a frame step or the end of a clip. ReplayChannel::positionNs() is
	// only the last frame it pushed, which stops agreeing with the bar the
	// moment a review runs out, and marking there marks somewhere he is not
	// looking. It stays as the fallback for a playhead that was never set.
	if (playheadNs_ != kNoInstant)
		return playheadNs_;
	return chan().positionNs();
}

bool MultiReplayDock::markable(int64_t tNs)
{
	// "No instant at all": the tap has captured nothing on this angle (not
	// recording, or the angle has no Branch Output filter running) and no clip
	// has played, so there is no playhead either. EventStore would happily
	// store that as a mark, producing an event that looks real in the list and
	// can never be played back, because no footage covers it. Refusing is the
	// honest answer, and saying so is better than a row nobody can explain.
	//
	// Both spellings of "none" are refused: the ring reports 0 when it is
	// empty, the dock's own playhead reports kNoInstant. Every other value is
	// a real instant — INCLUDING a negative one, which is what footage older
	// than the machine's last boot looks like.
	if (tNs != 0 && tNs != kNoInstant)
		return true;
	QMessageBox::information(this, "obs-multireplay",
				 obs_module_text("Dock.NothingToMark"));
	return false;
}

std::vector<int> MultiReplayDock::selectedEventIds() const
{
	std::vector<int> ids;
	auto rows = events_->selectionModel()->selectedRows();
	for (const auto &idx : rows) {
		QTableWidgetItem *it = events_->item(idx.row(), 0);
		if (!it)
			continue;
		// Skip non-event rows (e.g. session dividers carry no UserRole id,
		// so toInt() yields 0): never inject a spurious id 0 into play/edit.
		int id = it->data(Qt::UserRole).toInt();
		if (id > 0)
			ids.push_back(id);
	}
	return ids;
}

namespace {

// OBS fires hotkey callbacks on the hotkey thread. Everything the dock does
// touches widgets, so the work is posted to the GUI thread with the dock itself
// as the context object: if the dock is gone by then, ~QObject has already
// dropped the posted event and nothing runs on a dead pointer.
void onDockHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *ctx = static_cast<MultiReplayDock::HotkeyCtx *>(data);
	if (!ctx || !ctx->dock)
		return;
	QMetaObject::invokeMethod(
		ctx->dock, [ctx]() { ctx->fn(ctx->dock); },
		Qt::QueuedConnection);
}

} // namespace

void MultiReplayDock::registerDockHotkeys()
{
	struct Def {
		const char *name;
		const char *locale;
		void (*fn)(MultiReplayDock *);
	};

	static const Def kDefs[] = {
		// The play button, not "the last event": what an operator has
		// selected is what he means, and the table auto-selects the mark
		// he has just taken.
		{"ReplayPlaySelected", "Hotkey.PlaySelected",
		 [](MultiReplayDock *d) { d->playSelected(); }},
		{"ReplayStepForward", "Hotkey.StepForward",
		 [](MultiReplayDock *d) { d->stepFrameForward(); }},
		{"ReplayMarkCancel", "Hotkey.MarkCancel",
		 [](MultiReplayDock *d) {
			 EventStore::instance().markCancel();
			 d->refreshEvents();
		 }},
		// Speed presets: the same path as the chips, so they re-cue the
		// current clip at the new speed instead of only changing a number.
		{"ReplaySpeed25", "Hotkey.Speed25",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(25); }},
		{"ReplaySpeed50", "Hotkey.Speed50",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(50); }},
		{"ReplaySpeed75", "Hotkey.Speed75",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(75); }},
		{"ReplaySpeed100", "Hotkey.Speed100",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(100); }},
		{"ReplayFastForward", "Hotkey.FastForward",
		 [](MultiReplayDock *d) { d->applyReplaySpeed(200); }},
		// 20 lists and no way to change list without the mouse was the
		// gap; two relative steps beat twenty absolute hotkeys.
		{"ReplayPrevList", "Hotkey.PrevList",
		 [](MultiReplayDock *d) { d->stepList(-1); }},
		{"ReplayNextList", "Hotkey.NextList",
		 [](MultiReplayDock *d) { d->stepList(+1); }},
		// Trimming a mark that was taken late, one frame at a time. On
		// keys rather than on the panel because this is Stream Deck work:
		// the operator holds a key and watches the picture, which is the
		// only way to find the right frame.
		{"ReplayTrimInBack", "Hotkey.TrimInBack",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(true, -1); }},
		{"ReplayTrimInForward", "Hotkey.TrimInForward",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(true, +1); }},
		{"ReplayTrimOutBack", "Hotkey.TrimOutBack",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(false, -1); }},
		{"ReplayTrimOutForward", "Hotkey.TrimOutForward",
		 [](MultiReplayDock *d) { d->nudgeSelectedPoint(false, +1); }},
		// ...and the same two points straight to the position bar.
		{"ReplaySetInHere", "Hotkey.SetInHere",
		 [](MultiReplayDock *d) { d->setSelectedPoint(true); }},
		{"ReplaySetOutHere", "Hotkey.SetOutHere",
		 [](MultiReplayDock *d) { d->setSelectedPoint(false); }},
		// Whole SECONDS, which is how an operator describes the problem:
		// "the mark is two seconds late". A frame key pressed sixty times
		// is not an answer to that, and on a Stream Deck it is not even
		// possible.
		{"ReplayTrimInBack1s", "Hotkey.TrimInBack1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, -1'000'000'000LL);
		 }},
		{"ReplayTrimInFwd1s", "Hotkey.TrimInFwd1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, 1'000'000'000LL);
		 }},
		{"ReplayTrimOutBack1s", "Hotkey.TrimOutBack1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(false, -1'000'000'000LL);
		 }},
		{"ReplayTrimOutFwd1s", "Hotkey.TrimOutFwd1s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(false, 1'000'000'000LL);
		 }},
		{"ReplayTrimInBack5s", "Hotkey.TrimInBack5s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, -5'000'000'000LL);
		 }},
		{"ReplayTrimInFwd5s", "Hotkey.TrimInFwd5s",
		 [](MultiReplayDock *d) {
			 d->nudgeSelectedPointNs(true, 5'000'000'000LL);
		 }},
	};

	for (const Def &def : kDefs) {
		auto ctx = std::make_unique<HotkeyCtx>();
		ctx->dock = this;
		ctx->fn = def.fn;
		hotkeys_.push_back(obs_hotkey_register_frontend(
			def.name, obs_module_text(def.locale), onDockHotkey,
			ctx.get()));
		hotkeyCtx_.push_back(std::move(ctx));
	}
}

void MultiReplayDock::stepList(int delta)
{
	if (!listTabs_)
		return;
	// Only over the lists that are actually SHOWN: stepping onto a hidden tab
	// would leave the table on a list with no tab lit.
	const int shown =
		std::clamp(ReplayCore::instance().getConfig().eventListCount, 1,
			   std::min(kEventLists, listTabs_->count()));
	const int next =
		std::clamp(listTabs_->currentIndex() + delta, 0, shown - 1);
	listTabs_->setCurrentIndex(next); // its signal selects the list + refreshes
}

void MultiReplayDock::moveSelectedEvent(int delta)
{
	const auto ids = selectedEventIds();
	if (ids.empty()) {
		showNotice(obs_module_text("Dock.SelectToReorder"));
		return;
	}

	// Manual order and the chronological auto-sort cannot both be in force:
	// with the sort on, the operator would move a row and watch it snap
	// straight back, with nothing to tell him why. So reordering by hand
	// TURNS THE SORT OFF, and says so — the alternative (refusing) leaves him
	// hunting through Settings for a switch he does not know exists.
	auto &core = ReplayCore::instance();
	Config cfg = core.getConfig();
	if (cfg.sortEventsByTime) {
		cfg.sortEventsByTime = false;
		core.setConfig(cfg);
		showNotice(obs_module_text("Dock.ManualOrderOn"));
	}

	// One at a time: moving a multi-selection by one place has no meaning the
	// operator could predict.
	if (!EventStore::instance().moveEvent(ids.front(), delta))
		showNotice(obs_module_text("Dock.CannotMoveFurther"));
	refreshEvents();
}

void MultiReplayDock::playSelected()
{
	std::string err;
	if (!pc().playEvents(
		    selectedEventIds(), currentAngle1_ - 1,
		    toOutputBtn_ && toOutputBtn_->isChecked(), err))
		QMessageBox::warning(this, "obs-multireplay",
				     QString::fromStdString(err));
}

void MultiReplayDock::setAngle(int angle1Based)
{
	if (angle1Based < 1 || angle1Based > kNCams)
		return;
	currentAngle1_ = angle1Based;
	// Shared with the hotkeys, which have no way to reach the dock.
	ReplayCore::instance().setCurrentAngle(angle1Based - 1);
	// Re-cue the current clip on the chosen angle: re-play the selected (or
	// last) completed event from its IN on this angle, at the angle's resolved
	// speed. So switching angle during a replay shows the SAME clip from the
	// same in-point on the new camera.
	replayCurrent();
}

void MultiReplayDock::stepFrameForward()
{
	// the reference controller frame-by-frame forward.
	//
	// There is no playhead in the engine to advance: it plays RANGES. So a
	// step is the shortest range that can hold the next frame, played from one
	// frame past where the transport stands. The replay input keeps the last
	// frame it was handed, so when that tiny clip ends the stepped frame is
	// what stays on screen — which is exactly what a frame step is for.
	auto &core = ReplayCore::instance();
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return;
	}
	const int64_t edge = timelineStartNs_ + displayDurNs_;

	struct obs_video_info ovi = {};
	int64_t frameNs = 33333333; // 30 fps, if OBS will not say
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
		frameNs = (int64_t)((1000000000LL * (int64_t)ovi.fps_den) /
				    (int64_t)ovi.fps_num);

	const int64_t inNs = std::max(timelineStartNs_, playheadNs_ + frameNs);
	// Two frames wide, not one: the engine refuses a range it cannot serve
	// exactly, and a one-frame window that falls between two timestamps is
	// exactly that. Only the first frame is seen anyway.
	const int64_t outNs = inNs + 2 * frameNs;
	if (inNs >= edge) {
		// Already at the live edge — there is no next frame yet.
		showNotice(obs_module_text("Dock.AtLiveEdge"));
		return;
	}

	// Same discipline as a scrub: kill the queue first so its finish callback
	// cannot cut in, and consume the transition so poll() does not read the
	// stop as "the sequence ended, go back to live".
	pc().stopEvents();
	prevSequenceActive_ = false;
	core.setFollowLive(false);
	playheadNs_ = inNs;

	std::string err;
	if (!chan().play(currentAngle1_ - 1, inNs,
					    std::min(outNs, edge), 100, err))
		showNotice(QString("%1 — %2")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(QString::fromStdString(err)));
}

void MultiReplayDock::applyReplaySpeed(int pct)
{
	// Default speed for every angle without an override — the coordinator
	// resolves it when it builds the queue, including for the hotkeys.
	speedPct_ = std::clamp(pct, 5, 200);
	pc().setDefaultSpeedPct(speedPct_);
	// Always restart from the in-point so the saved IN is respected (the reference controller).
	replayCurrent();
}

void MultiReplayDock::replayCurrent()
{
	// While following live the angle buttons only pick which camera the
	// preview mirrors; they must not start a replay. Once the operator plays
	// something (which clears follow-live) they re-cue it — including during
	// recording, which the ring makes possible and is the whole point.
	if (ReplayCore::instance().followLive())
		return;
	auto &pc = this->pc();
	std::string err;
	std::vector<int> ids = selectedEventIds();
	bool toOut = toOutputBtn_ && toOutputBtn_->isChecked();
	int a0 = currentAngle1_ - 1;
	if (ids.empty())
		ids = {EventStore::instance().lastEventId()};
	if (ids.empty() || ids.front() <= 0)
		return;
	// Single angle on purpose: this is the re-cue behind the angle buttons and
	// the speed slider, so it shows the camera the operator just picked. The
	// play buttons go through the default (every enabled angle in sequence).
	//
	// And when that camera cannot be played, SAY SO. It used to fall through to
	// some other angle without a word, so pressing "2" played camera 1 and the
	// operator was left deducing the angle model from what he heard.
	if (!pc.playEvents(ids, a0, toOut, err,
			   PlaybackCoordinator::AngleMode::Single))
		showNotice(QString::fromStdString(err));
}

void MultiReplayDock::showNotice(const QString &text)
{
	// Shown on the green channel strip (see updateChannelStrip): it is wide,
	// it is directly under the picture the operator is looking at, and it is
	// where the reference controller keeps the state of the channel. The corner status line is
	// two inches wide and would swallow half the sentence.
	noticeText_ = text;
	noticeUntilNs_ = (int64_t)os_gettime_ns() + kNoticeNs;
	if (chanStrip_)
		chanStrip_->setToolTip(text);
	updateChannelStrip();
}

void MultiReplayDock::showHealthDetails()
{
	const auto findings = HealthMonitor::instance().findings();
	if (findings.empty())
		return;
	// A message box, not a modal that blocks the take: the take is running,
	// the operator opened this on purpose, and closing it changes nothing.
	// Every finding, in full, with the numbers that produced it — the badge
	// only has room for a count.
	QMessageBox::information(
		this, obs_module_text("Dock.HealthTitle"),
		QString::fromStdString(
			findingsBlock(findings, health::Level::Info)));
}

// ---------------------------------------------------------------------------
// The green channel strip (the reference controller's information band under the A output)
// ---------------------------------------------------------------------------

void MultiReplayDock::updateChannelStrip()
{
	// Both are built before the first poll(), but showNotice() can be reached
	// from anywhere and a half-built dock must not be a crash.
	if (!chanStrip_ || !events_)
		return;

	// mm:ss.cc — the reference controller's precision on this strip. Hundredths, because a
	// thousandth is a digit nobody reads while the clip is running.
	auto shortTc = [](int64_t ns) {
		if (ns < 0)
			ns = 0;
		const int64_t cs = ns / 10000000; // centiseconds
		return QString::asprintf("%02d:%02d.%02d", (int)(cs / 6000),
					 (int)((cs / 100) % 60), (int)(cs % 100));
	};
	auto signedTc = [&shortTc](int64_t ns) {
		return (ns < 0 ? QStringLiteral("-") : QStringLiteral("+")) +
		       shortTc(ns < 0 ? -ns : ns);
	};

	auto &store = EventStore::instance();
	const auto ps = pc().playState();

	// Which event this strip is about: the one on air, else the one selected,
	// else the last mark taken — the same order the transport keys use, so
	// the strip always describes what ▶ would play.
	int evId = ps.active ? ps.eventId : 0;
	if (evId <= 0) {
		const auto sel = selectedEventIds();
		evId = sel.empty() ? store.lastEventId() : sel.front();
	}
	ReplayEvent ev;
	const bool haveEv = evId > 0 && store.get(evId, ev);

	const int list = store.selectedList();
	const std::string listNm = store.listName(list);
	const QString listText = listNm.empty()
					 ? QString("%1 %2")
						   .arg(obs_module_text("Dock.List"))
						   .arg(list, 2, 10,
							QLatin1Char('0'))
					 : QString::fromStdString(listNm);

	const int idDigits =
		std::clamp(ReplayCore::instance().getConfig().eventIdDigits, 1, 8);

	// Where the playhead is INSIDE this clip. Clamped on purpose: parked at
	// the live edge (or at 0, with a project reopened and nothing recording)
	// the raw difference is the distance between two unrelated instants, and
	// it printed as "IN -5879:34.56" — a number with no meaning that makes the
	// whole strip look broken. Clamped, an un-cued clip reads exactly as the reference controller
	// shows a cued one: IN +00:00.00, OUT -<duration>, REM = duration.
	int64_t clipPos = playheadNs_;
	if (haveEv) {
		const int64_t hi =
			ev.tOutNs != kNoInstant ? ev.tOutNs : ev.tInNs;
		clipPos = std::clamp(playheadNs_, ev.tInNs, hi);
	}

	QString l1 = listText;
	if (ps.active && ps.queued > 0)
		l1 += QString("   %1/%2 %3")
			      .arg(ps.queuePos, 2, 10, QLatin1Char('0'))
			      .arg(ps.queued, 2, 10, QLatin1Char('0'))
			      .arg(obs_module_text("Dock.Clips"));
	if (haveEv && ev.tOutNs != kNoInstant) {
		// Remaining WALL time, so it counts down at the rate the operator
		// is watching: at 50% a 4 s clip has 8 s left, not 4.
		const int64_t remNs = ev.tOutNs > clipPos ? ev.tOutNs - clipPos : 0;
		const int pct = speedPct_ > 0 ? speedPct_ : 100;
		l1 += QString("   REM %1").arg(shortTc(remNs * 100 / pct));
	}

	QString l2;
	if (haveEv) {
		l2 = QString("%1").arg(evId, idDigits, 10, QLatin1Char('0'));
		l2 += QString("  IN %1").arg(signedTc(clipPos - ev.tInNs));
		if (ev.tOutNs != kNoInstant)
			l2 += QString("  OUT %1")
				      .arg(signedTc(clipPos - ev.tOutNs));
	} else {
		l2 = obs_module_text("Dock.NoEvent");
	}

	QString l3;
	if (noticeUntilNs_ > 0 && (int64_t)os_gettime_ns() < noticeUntilNs_) {
		l3 = QStringLiteral("⚠ ") + noticeText_;
	} else {
		const bool haveTc = eventOriginNs_ != kNoInstant &&
				    playheadNs_ != kNoInstant &&
				    playheadNs_ > eventOriginNs_;
		const int64_t rel = haveTc ? playheadNs_ - eventOriginNs_ : 0;
		l3 = QString("TC %1   %2%")
			     .arg(eventOriginNs_ != kNoInstant
					  ? shortTc(rel)
					  : QStringLiteral("--:--.--"))
			     .arg(speedPct_);
	}

	chanStrip_->setText(l1 + "\n" + l2 + "\n" + l3);
	if (chanBadge_)
		chanBadge_->setText(QString("A%1").arg(currentAngle1_));

	// --- the green band: the state of the ANGLE that is on air ------------
	// id · angle · time left · speed, with the fill as the progress through
	// that clip. The speed is the CLIP's (the angle's override when it has
	// one), not the slider's: what has to be readable there is the speed of
	// the picture in front of the operator.
	if (clipBar_) {
		const bool onAir = ps.active && ps.eventId > 0;
		const int barPct = onAir ? ps.speedPct
					 : (speedPct_ > 0 ? speedPct_ : 100);
		double frac = 0.0;
		QString text;
		if (haveEv && ev.tOutNs != kNoInstant &&
		    ev.tOutNs > ev.tInNs) {
			const int64_t dur = ev.tOutNs - ev.tInNs;
			frac = (double)(clipPos - ev.tInNs) / (double)dur;
			const int64_t remNs =
				ev.tOutNs > clipPos ? ev.tOutNs - clipPos : 0;
			// Remaining in WALL time: at 50% a 4 s clip has 8 s left,
			// and 8 s is how long the operator will be looking at it.
			text = QString("%1   A%2   %3   %4%")
				       .arg(evId, idDigits, 10, QLatin1Char('0'))
				       .arg(onAir ? ps.angle1 : currentAngle1_)
				       .arg(shortTc(remNs * 100 / (barPct > 0
									   ? barPct
									   : 100)))
				       .arg(barPct);
			if (onAir && ps.queued > 1)
				text += QString("   %1/%2")
						.arg(ps.queuePos)
						.arg(ps.queued);
		} else {
			text = obs_module_text("Dock.NoEvent");
		}
		clipBar_->setState(frac, text, onAir);
	}
	// >> stays ENABLED, always. It used to follow ps.active, which is read
	// here at 30 Hz — so for the first frames of a sequence the key was still
	// disabled and a press landed on nothing. During a match that is a lost
	// press with no explanation, and "the button was grey for 40 ms" is not
	// something anybody can see. The handler already refuses honestly when
	// there is nothing queued (it says so on the strip and in the log), which
	// is the better place for that answer.
	//
	// It cost the gate three runs to find, because a disabled button swallows
	// click() in silence: the check saw a queue that never advanced and blamed
	// the coordinator.

	// --- the position bar: where the TIMELINE stands ----------------------
	// Not the clip state (that is the band above). Position and length of the
	// recorded timeline, which is the thing this bar actually controls.
	if (seek_) {
		// "How far in" means how much FOOTAGE is behind the playhead, so
		// the number on the bar and the length beside it are the same
		// kind of quantity even when the session has pauses in it.
		const int64_t rel =
			!timeline_.empty() && playheadNs_ != kNoInstant
				? timeline_.footageBefore(playheadNs_)
				: ((timelineStartNs_ != kNoInstant &&
				    playheadNs_ != kNoInstant &&
				    playheadNs_ > timelineStartNs_)
					   ? playheadNs_ - timelineStartNs_
					   : 0);
		seek_->setOverlayText(shortTc(rel) + "  /  " +
				      shortTc(displayDurNs_));
		// The bar needs the LENGTH, not just the fraction: the graduations
		// are computed from it, and a fraction cannot say whether the bar
		// spans forty seconds or two hours.
		//
		// ...and when there is no length, why. Two different nothings,
		// and they are no longer the same two: a reopened project now
		// HAS a timeline (the footage on disk), so the only way to reach
		// this branch with footage anchored is a recording whose own
		// length cannot be read back from it. That is rare, real, and
		// worth naming — the operator is looking at events that play
		// perfectly while the bar under them refuses to move.
		seek_->setTimeline(
			displayDurNs_,
			obs_module_text(timelineStartNs_ != kNoInstant
						? "Dock.SeekLengthUnknown"
						: "Dock.SeekNoTimeline"));
	}
}

void MultiReplayDock::seekToFraction(double frac)
{
	if (timelineStartNs_ == kNoInstant || displayDurNs_ <= 0)
		return;
	frac = std::clamp(frac, 0.0, 1.0);
	// Through the map: the bar's axis is footage, so 50% is halfway through
	// what was RECORDED, not halfway through the afternoon. Without it, a
	// session with a pause in it had a stretch of bar that resolved to an
	// instant no camera covers — a click that could only ever answer "no
	// footage here".
	const int64_t inNs = timeline_.empty()
				     ? timelineStartNs_ +
					       (int64_t)(frac * (double)displayDurNs_)
				     : timeline_.instantAt(frac);
	const int64_t edge = timeline_.empty() ? timelineStartNs_ + displayDurNs_
					       : timeline_.lastNs();
	const int64_t outNs = std::min(edge, inNs + kScrubReviewNs);
	if (outNs <= inNs)
		return;

	// Scrubbing is "review from here" (see kScrubReviewNs): the engine has no
	// playhead to park, it plays ranges. Stop the queue first so its own
	// finish callback cannot cut in over the review clip.
	pc().stopEvents();
	// ...and consume that stop ourselves. poll() sends a finished SEQUENCE back
	// to the live edge, which is right when a replay ends and wrong here: it
	// would drag the operator off the very instant he just chose.
	prevSequenceActive_ = false;
	ReplayCore::instance().setFollowLive(false);
	// Where the timeline now stands, whatever the engine can serve: the bar
	// stays under the operator's finger instead of snapping back.
	playheadNs_ = inNs;

	std::string err;
	if (!chan().play(currentAngle1_ - 1, inNs, outNs,
					    speedPct_, err)) {
		// Nothing covers that instant on this angle — the ring has evicted
		// it and no anchored file holds it. Saying so is the point: the
		// alternative was the preview quietly showing the live camera, which
		// reads as "this is what was recorded there" and is not.
		const int64_t relMs = (inNs - timelineStartNs_) / 1000000;
		showNotice(QString("%1 (cam %2 @ %3) — %4")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(currentAngle1_)
				   .arg(formatTc(relMs * 1000000))
				   .arg(QString::fromStdString(err)));
		obs_log(LOG_WARNING,
			"[dock] no footage on angle %d at %lld ms into the "
			"timeline: %s",
			currentAngle1_, (long long)relMs, err.c_str());
	}
}

// ---------------------------------------------------------------------------
// The take that never started
// ---------------------------------------------------------------------------

void MultiReplayDock::cancelDeadRecording()
{
	auto &core = ReplayCore::instance();

	obs_log(LOG_ERROR,
		"REC armed the Branch Output filters but no recording output "
		"started within %lld s — cancelling the take. Branch Output's "
		"Interlock setting must be 'Always ON'.",
		(long long)(kArmWatchNs / 1'000'000'000LL));

	// Disarm through the same path as the STOP button: the filters go back
	// off, the tap stops retrying and the GUI stops claiming to record. This
	// runs on the GUI thread, so it must not wait on anything — it does not:
	// stopRecording() only flips filter enables and detaches the tap.
	core.stopRecording();

	// The message box is DEFERRED. Showing it from inside poll() would open a
	// nested event loop on top of a running poll tick, re-entering the whole
	// refresh (and its table rebuild) from within itself. Queued, it opens on
	// a clean stack, with `this` as context so a closed dock cancels it.
	QTimer::singleShot(0, this, [this]() {
		QMessageBox::warning(
			this, obs_module_text("Dock.RecNotStartedTitle"),
			obs_module_text("Dock.RecNotStarted"));
	});
}

// ---------------------------------------------------------------------------
// Periodic refresh
// ---------------------------------------------------------------------------

void MultiReplayDock::poll()
{
	auto &core = ReplayCore::instance();
	auto &chan = this->chan();
	auto &tap = PacketTap::instance();

	// The preview's obs_display is bound to a native window handle, and OBS
	// re-parents its docks whenever the layout is restored, floated, tabbed or
	// re-docked — which destroys that handle. Qt does not reliably tell the
	// widget, so this is the only place that can notice a display left
	// presenting into a window that no longer exists. Two integer compares.
	if (displayA_)
		displayA_->recheckWindow();
	if (displayB_)
		displayB_->recheckWindow();
	// ...and every multiview tile, for exactly the same reason: they are
	// re-parented by the same dock moves. Two integer compares each, and a
	// hidden tile early-outs on isVisible().
	for (PreviewTile &t : tiles_)
		if (t.display)
			t.display->recheckWindow();

	// The hotkeys change the angle without going through the dock.
	const int hotAngle1 = core.currentAngle() + 1;
	if (hotAngle1 >= 1 && hotAngle1 <= kNCams)
		currentAngle1_ = hotAngle1;
	const int cam0 = currentAngle1_ - 1;

	const bool rec = core.isRecording();
	const bool followLive = core.followLive();
	const bool playing = chan.playing();
	const auto playSt = pc().playState();
	const bool eventActive = playSt.active;

	// --- is the REPLAY on screen? (the sequence, not the clip) -----------
	// ReplayChannel::playing() goes false for a moment between the clips of a
	// multi-angle sequence — the finish callback has to travel from the playback
	// thread through the OBS UI queue before the next clip starts — and asking
	// it alone made the dock show the live camera in that gap: a visible flash,
	// and with "to output" on, a flash on air. The queue knows better: it is
	// still active. The grace window keeps a queue that died badly (nothing
	// playing, nothing coming) from pinning the preview forever.
	const int64_t nowNs = (int64_t)os_gettime_ns();
	if (playing || (eventActive && !prevSequenceActive_))
		lastPlayingNs_ = nowNs;
	const bool sequenceOnAir =
		playing || (eventActive && lastPlayingNs_ > 0 &&
			    nowNs - lastPlayingNs_ < kSequenceGapGraceNs);

	// Counts poll() ticks so the work nobody reads thirty times a second — the
	// status line, and re-resolving the preview source — runs at a fraction of
	// the transport rate. See the status block below for why that matters.
	constexpr int kStatusEveryNTicks = 8; // ~264 ms at 33 ms/tick
	const bool refreshStatus = (statusTick_++ % kStatusEveryNTicks) == 0;

	// --- timeline window ---
	// Nothing is "indexed" any more: the live edge is the newest packet the
	// tap captured, and the timeline starts at the oldest instant that can
	// still be replayed. The recorded files reach further back than the RAM
	// ring, so they win when they have anything.
	//
	// Both ends are INSTANTS, and an instant may be negative: master time is
	// read off a clock that starts at boot, so a project recorded before the
	// last reboot sits at negative master values. Nothing here may test one
	// for > 0 — see kNoInstant in segment-index.hpp. The ring, by contrast,
	// only ever holds this session's packets, so its 0 really does mean
	// "empty".
	const int64_t liveEdgeNs = tap.newestNs(cam0);
	int64_t startNs = SegmentIndex::instance().oldestNs(cam0);
	if (startNs == kNoInstant) {
		const int64_t ringOldest = tap.oldestReplayableNs(cam0);
		startNs = ringOldest > 0 ? ringOldest : kNoInstant;
	}
	timelineStartNs_ = startNs;
	// The timeline ends where the FOOTAGE ends, which is the live edge only
	// while a take is running. Measuring it from the live edge alone is what
	// made a reopened project draw a bar of length zero over hours of usable
	// footage: outside REC nothing feeds the front, so the whole recorded
	// project collapsed to a rectangle that said "no live timeline" and
	// swallowed every click. The end of the last recording is not something
	// the anchors can say (an anchor is a beginning), so SegmentIndex demuxes
	// it from the file itself — and says so when the file does not, which
	// leaves the bar honestly empty instead of drawing a guess.
	//
	// Pressing REC then EXTENDS this: the new file anchors onto the same
	// project origin and the live edge overtakes the disk end, so the bar
	// grows to the right instead of starting over.
	const int64_t diskEndNs = SegmentIndex::instance().newestNs(cam0);
	const int64_t endNs =
		std::max(liveEdgeNs > 0 ? liveEdgeNs : kNoInstant, diskEndNs);

	// --- the axis is FOOTAGE, not wall time -------------------------------
	// Stop after two minutes, talk to somebody for one, press REC again: on a
	// wall-time axis the bar grows to three minutes and one of them scrubs to
	// nothing — a hole the operator has to know is there and step over. So
	// the bar is drawn over the recorded spans only, joined end to end (see
	// timeline-map.hpp): the second take continues the first at 2:00, every
	// position on the bar is an instant footage covers, and the total is how
	// much material there is rather than how long the session lasted.
	{
		// The spans on DISK are read on the slow beat only. They change
		// when a file is anchored or its length measured — a few times a
		// minute — and reading them takes SegmentIndex's lock, which its
		// watcher holds while it demuxes a file. Asking thirty times a
		// second put a disk read in the way of the GUI thread, and the
		// dock went unresponsive for exactly as long as the measurement
		// took (the gate caught it: a >> press that never got serviced).
		if (refreshStatus || diskSpans_.empty()) {
			diskSpans_.clear();
			for (const auto &[s, e] :
			     SegmentIndex::instance().recordedSpans())
				diskSpans_.push_back({s, e});
		}
		// The take in progress has no measured length on disk yet, so it
		// comes from what the tap holds — the only span that grows, and
		// it merges with the file it belongs to. This one IS cheap
		// (the ring's own lock, held for a compare).
		std::vector<TimelineSpan> spans = diskSpans_;
		const int64_t liveStart = tap.oldestReplayableNs(cam0);
		if (liveEdgeNs > 0 && liveStart > 0 && liveEdgeNs > liveStart)
			spans.push_back({liveStart, liveEdgeNs});
		timeline_.setSpans(std::move(spans));
	}
	// Everything below still speaks in master instants; only the conversion
	// to a position on the bar goes through the map.
	displayDurNs_ = timeline_.totalNs();
	if (displayDurNs_ <= 0)
		displayDurNs_ = (startNs != kNoInstant && endNs != kNoInstant &&
				 endNs > startNs)
					? endNs - startNs
					: 0;
	// Anchored footage counts as content even with a dead live edge: that is
	// exactly a project reopened in a later OBS run (nothing captured yet, but
	// yesterday's files are on the timeline). Without startNs the preview would
	// stay black while the replay input was actually producing frames.
	previewHasContent_ = liveEdgeNs > 0 || startNs != kNoInstant;

	// Event times belong to the PROJECT, not to the angle being watched: the
	// earliest anchored recording on ANY camera is 0:00 for the table and for
	// the YouTube chapters. Reading it off the selected angle renumbered every
	// row when the operator pressed another camera button, and gave nothing at
	// all for an angle with no anchor — which is how a reopened project ended
	// up printing marks as raw monotonic time. The ring is the fallback for a
	// session that has not written a file yet.
	int64_t eventOrigin = SegmentIndex::instance().projectOriginNs();
	if (eventOrigin == kNoInstant)
		eventOrigin = startNs;
	eventOriginNs_ = eventOrigin;

	// The event columns are drawn relative to that origin, so a moved origin
	// has to redraw them — it moves once for real, when the first anchored
	// recording replaces the ring's (constantly evicted) oldest instant. The
	// 1 s of slack is what keeps the ring's drift from rebuilding the table
	// thirty times a second.
	// The subtraction is guarded, not just the result: kNoInstant is INT64_MIN
	// and subtracting it from a real instant overflows.
	if (eventOriginNs_ != kNoInstant &&
	    (tableOriginNs_ == kNoInstant ||
	     std::abs(eventOriginNs_ - tableOriginNs_) > 1'000'000'000LL)) {
		tableOriginNs_ = eventOriginNs_;
		refreshEvents();
	}

	// Is the live front still being FED? Not "did we press REC": after STOP the
	// tap is detached and the newest instant stops moving, and that is exactly
	// when jumping "back to live" would be a lie — there is no live to go to,
	// only the last frame of a take that is over.
	const bool liveFrontFed = liveEdgeNs > 0 && tap.anyAttached();

	// --- the sequence just ended: give the transport back to the live edge ---
	// The operator should not have to press NOW after every replay. This fires
	// on the QUEUE ending, natural or by Stop, not on each clip — a two-angle
	// event must not snap back to live halfway through.
	if (prevSequenceActive_ && !eventActive) {
		if (liveFrontFed) {
			// Back to the front, exactly like NOW.
			core.setFollowLive(true);
			playheadNs_ = liveEdgeNs;
		} else {
			// The take is over, so there is nothing to follow: park the
			// playhead on the last instant of footage and STAY in review,
			// which keeps the replay's picture on the preview instead of
			// swapping in a live camera that has nothing to do with the
			// moment being reviewed.
			if (liveEdgeNs > 0)
				playheadNs_ = liveEdgeNs;
		}
	}
	prevSequenceActive_ = eventActive;

	// Everything inside that window is playable (ring or files), so unlike the
	// file-tailing engine there is no trailing "not yet flushed" region.
	//
	// The playhead is the dock's, not the engine's: ReplayChannel reports the
	// last frame it pushed forever after, so a finished clip left the bar
	// wherever it stopped. While a clip plays it IS that frame; otherwise it is
	// where the operator parked the timeline (scrub, NOW, end of sequence).
	const int64_t posNs = chan.positionNs();
	if (playing && posNs > 0)
		playheadNs_ = posNs;
	else if (followLive && liveEdgeNs > 0 && !sequenceOnAir)
		playheadNs_ = liveEdgeNs;
	// Footage behind the playhead, not wall time behind it — the same axis
	// the bar is drawn on (see timeline-map.hpp).
	const int64_t relPosNs =
		!timeline_.empty() && playheadNs_ != kNoInstant
			? timeline_.footageBefore(playheadNs_)
			: ((startNs != kNoInstant && playheadNs_ != kNoInstant &&
			    playheadNs_ > startNs)
				   ? playheadNs_ - startNs
				   : 0);

	if (!seekDragging_) {
		if (followLive && liveFrontFed) {
			// Watching the live edge: the playhead IS the edge.
			seek_->setProgress(1.0, 1.0);
			tcLbl_->setText(QStringLiteral("● ") +
					formatTc(displayDurNs_));
		} else {
			double posFrac = displayDurNs_ > 0
						 ? std::min(1.0,
							    (double)relPosNs /
								    (double)displayDurNs_)
						 : 0.0;
			seek_->setProgress(posFrac, 1.0);
			// Zoomed in, the window follows the picture: a bar
			// showing four seconds of an hour is useless the moment
			// the playhead walks out of it.
			seek_->ensureVisible(posFrac);
			if (rec)
				tcLbl_->setText(formatTc(relPosNs) +
						QStringLiteral(" / ● ") +
						formatTc(displayDurNs_));
			else
				tcLbl_->setText(formatTc(relPosNs) + " / " +
						formatTc(displayDurNs_));
		}
	}

	// ⏸ U+23F8  ▶ U+25B6
	playPauseBtn_->setText(playing ? QStringLiteral("⏸")
				       : QStringLiteral("▶"));
	if (playPauseBtn_->property("playing").toBool() != playing) {
		playPauseBtn_->setProperty("playing", playing);
		repolish(playPauseBtn_);
	}
	if (nowBtn_->property("live").toBool() != followLive) {
		nowBtn_->setProperty("live", followLive);
		repolish(nowBtn_);
	}
	// The speed slider is the dock's own state (the engine has none), with one
	// exception: the speed HOTKEYS set the default without ever reaching a
	// widget, and a Stream Deck operator pressing 50% must not be left looking
	// at a slider that still says 1×. Only when the two disagree, and never
	// while his finger is on it.
	{
		const int coordPct =
			pc().defaultSpeedPct();
		if (speed_ && coordPct != speedPct_ && !speed_->isSliderDown()) {
			speedPct_ = coordPct;
			QSignalBlocker block(speed_);
			speed_->setValue(coordPct);
			speedLbl_->setText(QString::asprintf(
				"%.2f\xc3\x97", coordPct / 100.0));
		}
		// the reference controller fills the preset that matches the speed in force. The
		// property drives the QSS, so it is only repolished when it moves.
		if (speedChips_) {
			for (QAbstractButton *ab : speedChips_->buttons()) {
				const bool on = speedChips_->id(ab) == speedPct_;
				if (ab->property("active").toBool() == on)
					continue;
				ab->setProperty("active", on);
				repolish(ab);
			}
		}
	}

	// Angle buttons: PVW green = selected, PGM red = event playing on it.
	// Visual state is driven by the "state" property + QSS, not :checked.
	if (anglesA_) {
		// PGM follows the angle actually on air, which is not the selected
		// one any more: a two-angle event plays C1 then C2 while the dock
		// still points at whichever the operator picked. Driven by the
		// SEQUENCE so the tally does not blink off in the gap between clips.
		bool ep = sequenceOnAir && playSt.angle1 > 0;
		for (int i = 1; i <= kNCams; i++) {
			auto *b = qobject_cast<QPushButton *>(
				anglesA_->button(i));
			if (!b || !b->isVisible())
				continue;
			QString st = (ep && i == playSt.angle1)
					     ? QStringLiteral("program")
				   : (i == currentAngle1_)
					     ? QStringLiteral("preview")
					     : QString();
			if (b->property("state").toString() != st) {
				b->setProperty("state", st);
				repolish(b);
			}
		}
		// Keep exclusive selection in sync for click handling
		if (anglesA_->button(currentAngle1_))
			anglesA_->button(currentAngle1_)->setChecked(true);
	}

	// The row on air gets a PGM cue on its id cell. the reference controller colours the whole
	// row, but our row colour is the selection (orange), and repainting a
	// selected row would make "playing" and "selected" indistinguishable —
	// which is the one thing that must never be ambiguous during a match.
	{
		const auto &ps = playSt;
		const bool wasRefreshing = refreshing_;
		refreshing_ = true; // colouring is not an operator edit
		for (int row = 0; row < events_->rowCount(); row++) {
			QTableWidgetItem *idItem = events_->item(row, kColId);
			if (!idItem)
				continue;
			int rowEv = idItem->data(Qt::UserRole).toInt();
			bool isActive = ps.active && (ps.eventId == rowEv);
			if (idItem->data(Qt::UserRole + 1).toBool() != isActive) {
				idItem->setData(Qt::UserRole + 1, isActive);
				idItem->setForeground(
					isActive ? QBrush(QColor("#ff5a3c"))
						 : QBrush());
			}
		}
		refreshing_ = wasRefreshing;
	}
	// the reference controller paints the header of the camera being watched green.
	updateCamHeaderHighlight();

	// --- recording status ---
	// Auto-follow the live edge when recording starts so the preview tracks
	// the new take instead of sitting on the last clip that played.
	if (rec && !prevRecording_) {
		core.setFollowLive(true);
		// Start the watchdog on every take, however it was started (the
		// REC button, a hotkey, the API): the failure it catches is not
		// the dock's, it is Branch Output declining.
		armWatchDeadlineNs_ = (int64_t)os_gettime_ns() + kArmWatchNs;
	}
	prevRecording_ = rec;

	// --- did Branch Output actually START? -------------------------------
	// Our REC only enables the filters. Branch Output re-evaluates its own
	// start conditions on a 1 s timer, and the Interlock setting that gates
	// them is global, lives in ITS dock and is exposed by no API — so on
	// anything but "Always ON" the filters stay armed and nothing records.
	// The old symptom was the worst kind: a red REC button, a running clock,
	// and marks landing on footage that does not exist, discovered only when
	// a replay came up empty (and, before that, only as a log warning 40 s
	// later, from the tap giving up). A take that has not started in a few
	// seconds never will, so it is cancelled here rather than faked.
	if (armWatchDeadlineNs_ > 0) {
		if (!rec || core.branchOutputRecording()) {
			armWatchDeadlineNs_ = 0; // running, or already stopped
		} else if ((int64_t)os_gettime_ns() >= armWatchDeadlineNs_) {
			// Cleared BEFORE cancelling: cancelDeadRecording() queues a
			// modal, whose nested event loop keeps this timer ticking.
			armWatchDeadlineNs_ = 0;
			cancelDeadRecording();
			return;
		}
	}

	// Live-mirror preview state (see drawChannelA). The preview is a
	// confidence monitor for the selected angle: the operator lines the
	// cameras up BEFORE the take, so it mirrors the live source — recording or
	// not, ever started or not.
	//
	// But ONLY while following live. That is the whole meaning of follow-live,
	// and the two bugs it fixes are the same bug: asking "is a clip playing
	// right now" showed the live camera in the gap between two clips of a
	// sequence, and showed it again the moment a scrub review ran out — so
	// dragging the seekbar to a point in the recorded timeline ended up
	// displaying the camera as it is NOW, presented as the footage of THEN.
	// Once the operator has scrubbed or played something he is reviewing the
	// timeline, and the preview stays on the footage (the replay input holds
	// the last frame it was handed) until he asks for the live edge back.
	//
	// The name→source lookup and the reference counting happen HERE, on the UI
	// thread. The graphics thread gets a ready-made reference (previewSource_).
	{
		const bool live = followLive && !sequenceOnAir;
		previewShowsReplay_.store(!live);
		// Re-resolving costs a lookup under libobs' global source mutex, so
		// only do it when the answer can have changed: a different kind of
		// picture, a different angle, or the 4 Hz revalidation that catches a
		// source renamed, deleted or replaced by a scene-collection change.
		if (live != previewLive_ || cam0 != previewCam0_ || refreshStatus) {
			obs_source_t *next = nullptr;
			if (live) {
				const std::string name =
					core.getConfig().cameras[cam0].sourceName;
				// An unconfigured angle, or a name nothing answers
				// to, publishes nothing: the preview goes black
				// instead of rendering a dangling pointer.
				if (!name.empty())
					next = obs_get_source_by_name(name.c_str());
			} else if (previewHasContent_) {
				// Nothing captured yet: render black rather than
				// whatever the replay input last held.
				next = chan.acquireSource();
			}

			obs_source_t *prev = nullptr;
			{
				std::lock_guard<std::mutex> lk(previewMutex_);
				prev = previewSource_;
				previewSource_ = next;
			}
			// Released OUTSIDE the lock, on purpose: the last release
			// destroys the source, which enters the graphics context —
			// holding previewMutex_ there would be the UI thread waiting
			// on the graphics thread while the graphics thread waits on
			// previewMutex_, which is the deadlock this whole change is
			// about.
			if (prev)
				obs_source_release(prev);
			previewLive_ = live;
			previewCam0_ = cam0;
		}
		// Channel B's box, on the same 4 Hz beat and by the same rules: a
		// lookup here, an owned reference published for the graphics
		// thread. B is a replay bay with no live mirror, so this is
		// simply B's input — and nothing at all until B has played
		// something, which draws black rather than a stale picture.
		if (refreshStatus) {
			obs_source_t *nextB =
				PlaybackCoordinator::instance(Which::B)
							.playState()
							.active ||
						ReplayChannel::instance(Which::B)
							.positionNs() > 0
					? ReplayChannel::instance(Which::B)
						  .acquireSource()
					: nullptr;
			obs_source_t *prevB = nullptr;
			{
				std::lock_guard<std::mutex> lk(previewMutex_);
				prevB = previewSourceB_;
				previewSourceB_ = nextB;
			}
			if (prevB)
				obs_source_release(prevB);
		}
		// The tiles are resolved on the same 4 Hz beat and by the same
		// rules: a lookup on the UI thread, an owned ref published for the
		// graphics thread. Re-running it periodically is also what catches
		// a camera source renamed, deleted or swapped by a scene-collection
		// change — the tile goes black instead of holding a stale pointer.
		if (refreshStatus)
			refreshTileSources();
	}
	// Cheap (two compares in the common case) and it has to follow the angle
	// buttons and the queue, both of which move outside refreshStatus.
	updateMultiviewTally();

	// --- M4: how is the take actually going? -----------------------------
	// READ ONLY. The monitor samples on a thread of its own, and it has to:
	// sampling reads PacketTap::stats(), whose lock the tap holds across a
	// detach that blocks on Branch Output, which in turn needs this thread —
	// so a sample taken here froze OBS outright the first time a camera went
	// away mid-take. See the note at the top of health.hpp. findings() is a
	// copy under a lock held for exactly as long as the copy.
	{
		auto &monitor = HealthMonitor::instance();
		if (healthBtn_) {
			const auto findings = monitor.findings();
			const health::Level worst = health::worstOf(findings);
			if (findings.empty()) {
				healthBtn_->hide();
			} else {
				const bool bad = worst >= health::Level::Blocker;
				healthBtn_->setText(
					QString("%1 %2")
						.arg(bad ? QStringLiteral("⛔")
							 : QStringLiteral("⚠"))
						.arg(findings.size()));
				healthBtn_->setToolTip(QString::fromStdString(
					findingsBlock(findings,
						      health::Level::Info)));
				const QString state = bad
							      ? QStringLiteral("bad")
							      : QStringLiteral("warn");
				if (healthBtn_->property("level").toString() !=
				    state) {
					healthBtn_->setProperty("level", state);
					repolish(healthBtn_);
				}
				healthBtn_->show();
			}
		}
	}

	recBtn_->setText(rec ? QStringLiteral("◼  STOP")
			     : QStringLiteral("●  REC"));
	if (recBtn_->property("recording").toBool() != rec) {
		recBtn_->setProperty("recording", rec);
		repolish(recBtn_);
	}

	// Sync the Live button with engine state (startRecording sets liveMode
	// internally without going through the button).
	bool lm = EventStore::instance().liveMode();
	if (liveBtn_ && liveBtn_->isChecked() != lm) {
		QSignalBlocker block(liveBtn_);
		liveBtn_->setChecked(lm);
	}

	// The status line is the only thing in this timer that can block, and it
	// is also the one nobody reads thirty times a second: statusJson() stats
	// the session folder (std::filesystem::space), picks the encoder by
	// enumerating the registered types while holding the core lock, then
	// builds a JSON document we immediately parse back.
	//
	// On a local disk that measures as free — the self-test saw the same tick
	// cadence with and without this throttle — so this is not a speed-up, it
	// is about WHERE that syscall runs: a session folder on a NAS (a normal
	// setup for a replay rig) turns space() into a network round trip, and one
	// that hits an unreachable share blocks for the SMB timeout. Thirty of
	// those a second on the GUI thread is a frozen OBS, not a slow dock. Four
	// a second is still faster than any number in that line can change.
	// The rest of poll() — seekbar, playhead, transport state — is cheap and
	// stays at full rate, because that IS what has to look smooth.
	// (refreshStatus is computed at the top: the preview resolution uses it
	// too.)
	// A live notice owns the status line until it expires: it is the answer to
	// something the operator just did, and overwriting it 250 ms later with the
	// idle line is how "the dock ignored me" happens.
	Data st((refreshStatus && nowNs >= noticeUntilNs_) ? core.statusJson()
							  : std::string());
	if (st) {
		QString ver = obs_data_get_string(st, "version");
		int64_t mins = obs_data_get_int(st, "estimatedMinutesRemaining");
		bool boOk = obs_data_get_bool(st, "branchOutputAvailable");
		// the reference controller's second line: how much recording time is left, in
		// hours:minutes, not a bare minute count nobody converts under
		// pressure.
		QString s;
		if (!boOk)
			s = QStringLiteral("⚠ Branch Output");
		else if (mins >= 0)
			s = QString("%1 %2")
				    .arg(QString::asprintf("%02lld:%02lld:00",
							   (long long)(mins / 60),
							   (long long)(mins % 60)))
				    .arg(obs_module_text("Dock.Remaining"));
		else
			s = QString("v%1 • %2")
				    .arg(ver)
				    .arg(rec ? obs_module_text("Dock.Recording")
					     : obs_module_text("Dock.Idle"));
		statusLbl_->setText(s);
	}

	// The wall clock above it, red while the take runs (the reference controller). Same 4 Hz as
	// the rest of the status block — a clock that ticks 30 times a second
	// costs a restyle 30 times a second and reads no better.
	if (refreshStatus && clockLbl_) {
		clockLbl_->setText(QDateTime::currentDateTime().toString(
			QStringLiteral("yyyy-MM-dd HH:mm:ss")));
		if (clockLbl_->property("rec").toBool() != rec) {
			clockLbl_->setProperty("rec", rec);
			repolish(clockLbl_);
		}
	}

	// The green channel strip and the text printed on the position bar.
	updateChannelStrip();

	// --- project label ---
	// Same rate as the status line: it only changes when the operator opens
	// or creates a project, and reading it copies the whole Config.
	if (refreshStatus) {
		std::string proj = core.getConfig().currentProjectName;
		if (projectLbl_) {
			if (proj.empty()) {
				projectLbl_->hide();
			} else {
				projectLbl_->setText(
					QString::fromStdString("[" + proj +
							       "]"));
				projectLbl_->show();
			}
		}
	}

	// --- auto-refresh event list on any external mutation (hotkeys, etc.) ---
	uint64_t ev = EventStore::instance().version();
	if (ev != lastEventVersion_) {
		lastEventVersion_ = ev;
		refreshAngles();
		refreshEvents(); // rebuilds markerNs_ (raw ns pairs)
	}

	// Recompute seekbar marker fractions every tick: the window slides (the
	// live edge grows, the ring drops its oldest), so markers move even when
	// the events themselves do not change.
	if (seek_ && displayDurNs_ > 0) {
		std::vector<std::pair<double, double>> mf;
		std::vector<int> ids;
		mf.reserve(markerNs_.size());
		for (size_t i = 0; i < markerNs_.size(); i++) {
			// Through the same map the bar is drawn with, so a mark
			// sits over its own footage however many takes there
			// have been (and an event in a gap that no longer exists
			// collapses onto the join rather than smearing over it).
			const auto &[inNs, outNs] = markerNs_[i];
			double inf = 0.0, outf = 0.0;
			if (timeline_.rangeFraction(inNs, outNs, inf, outf)) {
				mf.push_back({inf, outf});
				// The two lists are read in lockstep by the
				// bar, so a marker that is not drawn must not
				// leave its id behind either.
				ids.push_back(i < markerIds_.size()
						      ? markerIds_[i]
						      : 0);
			}
		}
		seek_->setEventMarkers(std::move(mf));
		seek_->setEventMarkerIds(std::move(ids));
	} else if (seek_) {
		seek_->setEventMarkers({});
		seek_->setEventMarkerIds({});
	}
}

void MultiReplayDock::refreshListNames()
{
	if (!listTabs_)
		return;
	auto &store = EventStore::instance();
	// Tab text only: changing it does not move the current tab, but blocking
	// the signals keeps a rebuild from ever looking like an operator switching
	// list.
	QSignalBlocker block(listTabs_);

	// How many lists the operator asked to see. The tabs beyond it are HIDDEN,
	// not removed: what is in those lists is still there, still saved, and
	// comes back the moment he raises the number.
	const int shown =
		std::clamp(ReplayCore::instance().getConfig().eventListCount, 1,
			   kEventLists);
	for (int i = 1; i <= kEventLists && i <= listTabs_->count(); i++)
		listTabs_->setTabVisible(i - 1, i <= shown);
	// A hidden tab must not stay the current one: the table would be showing a
	// list with no tab lit, which reads as "no list at all".
	if (listTabs_->currentIndex() >= shown) {
		listTabs_->setCurrentIndex(shown - 1);
		store.selectList(shown);
	}

	for (int i = 1; i <= kEventLists && i <= listTabs_->count(); i++) {
		const std::string nm = store.listName(i);
		// the reference controller labels the tabs "Events 1"; a named list replaces the
		// number with the name, which is what the name is for.
		listTabs_->setTabText(i - 1,
				      nm.empty()
					      ? QString::number(i)
					      : QString::fromStdString(nm));
		listTabs_->setTabToolTip(
			i - 1, nm.empty()
				       ? QString("%1 %2")
						 .arg(obs_module_text("Dock.List"))
						 .arg(i)
				       : QString("%1 · %2").arg(i).arg(
						 QString::fromStdString(nm)));
	}
}

void MultiReplayDock::renameListDialog()
{
	auto &store = EventStore::instance();
	const int list = store.selectedList();
	bool ok = false;
	const QString cur = QString::fromStdString(store.listName(list));
	const QString name = QInputDialog::getText(
		this, obs_module_text("Dock.RenameList"),
		QString(obs_module_text("Dock.RenameListLabel")).arg(list),
		QLineEdit::Normal, cur, &ok);
	if (!ok)
		return;
	// An empty name is how a list goes back to being just a number.
	store.setListName(list, name.trimmed().toStdString());
	refreshListNames();
}

void MultiReplayDock::refreshAngles()
{
	// The multiview follows the same configuration as the angle buttons, and
	// this is the one function every path that can change it calls. A no-op
	// unless the set of configured cameras (or a name) really moved.
	rebuildMultiview();
	if (!anglesA_)
		return;
	Config cfg = ReplayCore::instance().getConfig();
	for (int i = 0; i < kNCams; i++) {
		auto *b = qobject_cast<QPushButton *>(anglesA_->button(i + 1));
		if (!b)
			continue;
		// Show button only for cameras that have a source assigned.
		bool configured = !cfg.cameras[i].sourceName.empty();
		b->setVisible(configured);
		if (!configured)
			continue;
		const std::string &dn = cfg.cameras[i].displayName;
		if (dn.empty()) {
			b->setText(QString::number(i + 1));
			b->setToolTip(QString("%1 %2")
					      .arg(obs_module_text("Dock.Angle"))
					      .arg(i + 1));
		} else {
			b->setText(QString::fromStdString(dn).left(5));
			b->setToolTip(QString::fromStdString(dn));
		}
	}
}

void MultiReplayDock::refreshEvents()
{
	// Here rather than only where a name is edited: opening another project
	// loads that project's list names without ever bumping the version
	// counter, and this is the one function every one of those paths calls.
	refreshListNames();
	// Same reason: the camera columns follow the camera configuration, and a
	// no-op unless that really changed (it clears the table).
	rebuildEventColumns();
	int list = EventStore::instance().selectedList();
	Data d(EventStore::instance().listJson(list));
	if (!d)
		return;
	QString needle = search_ ? search_->text().trimmed().toLower() : QString();

	// Remember the user's current selection so it survives the rebuild.
	int prevSel = 0;
	{
		auto sel = events_->selectionModel()->selectedRows();
		if (!sel.empty()) {
			QTableWidgetItem *it = events_->item(sel.first().row(),
							     kColId);
			if (it)
				prevSel = it->data(Qt::UserRole).toInt();
		}
	}

	refreshing_ = true;
	// Block selection signals during the rebuild: setRowCount(0) clears the
	// selection and selectRow() below re-sets it, and nothing downstream needs
	// to hear about a selection that is only being restored.
	QSignalBlocker selBlock(events_->selectionModel());
	events_->setRowCount(0);
	obs_data_array_t *arr = obs_data_get_array(d, "events");
	if (!arr) {
		refreshing_ = false;
		return;
	}
	const Qt::Alignment mid = Qt::AlignVCenter | Qt::AlignHCenter;
	std::vector<std::pair<int64_t, int64_t>> rawMarkers;
	// ...and which event each of them belongs to, so its edge can be taken
	// hold of on the bar (SeekBar::markerDragged).
	std::vector<int> rawMarkerIds;

	// Marks are absolute instants on a monotonic clock that started with OBS,
	// so a column only means something relative to where this project's
	// footage begins. With NO footage there is no such origin, and printing
	// the raw instant produced the five-digit minute counts a reopened project
	// showed ("5648:09.557" for a mark taken four minutes into a take). A mark
	// we cannot place is shown as unplaceable.
	const int64_t originNs = eventOriginNs_;
	const bool haveOrigin = originNs != kNoInstant;
	auto relTc = [originNs, haveOrigin](int64_t ns) {
		if (!haveOrigin)
			return QStringLiteral("--:--.---");
		return formatTc(ns > originNs ? ns - originNs : 0);
	};

	// Can this mark still be played? The ring holds the last minutes; anything
	// older needs an anchored recording. An event of a session whose files were
	// never anchored can never play again, and saying so in the list beats a
	// row that does nothing when clicked.
	auto &tapRef = PacketTap::instance();
	const Config rowCfg = ReplayCore::instance().getConfig();
	auto footageExists = [&](int64_t ns) {
		if (SegmentIndex::instance().coversAnyCamera(ns))
			return true;
		for (int cam = 0; cam < kNCams; cam++) {
			if (rowCfg.cameras[cam].sourceName.empty())
				continue;
			const int64_t oldest = tapRef.oldestReplayableNs(cam);
			const int64_t newest = tapRef.newestNs(cam);
			if (oldest > 0 && ns >= oldest && ns <= newest)
				return true;
		}
		return false;
	};

	// The rows are COLLECTED first and rendered after, so the list can be drawn
	// in chronological order (the reference controller "sort events by time") rather than in the
	// order the marks were taken. The two orders diverge the moment a −20s
	// preset follows a −5s one, and during a match a list that is not in the
	// order things happened is read wrong, not slowly.
	struct Row {
		int id = 0;
		int64_t tin = 0;
		int64_t tout = -1;
		bool camOn[kEventAngles] = {};
		double camSpeeds[kEventAngles] = {};
		std::string camNotes[kEventAngles];
	};
	std::vector<Row> rows;

	size_t n = obs_data_array_count(arr);
	rows.reserve(n);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(arr, i);
		int id = (int)obs_data_get_int(e, "id");
		int64_t tin = obs_data_get_int(e, "tInNs");
		int64_t tout = obs_data_get_int(e, "tOutNs");

		bool camOn[kEventAngles] = {};
		std::string camNotes[kEventAngles];
		double camSpeeds[kEventAngles];
		for (int k = 0; k < kEventAngles; k++)
			camSpeeds[k] = -1.0;
		QString anglesStr;
		obs_data_array_t *aArr = obs_data_get_array(e, "angles");
		if (aArr) {
			size_t na = obs_data_array_count(aArr);
			for (size_t k = 0; k < na && k < (size_t)kEventAngles; k++) {
				obs_data_t *ad = obs_data_array_item(aArr, k);
				if (obs_data_get_bool(ad, "enabled")) {
					camOn[k] = true;
					anglesStr += QString::number(k + 1) + " ";
				}
				const char *nt = obs_data_get_string(ad, "note");
				camNotes[k] = nt ? nt : "";
				camSpeeds[k] = obs_data_has_user_value(ad, "speed")
						       ? obs_data_get_double(ad,
									     "speed")
						       : -1.0;
				obs_data_release(ad);
			}
			obs_data_array_release(aArr);
		}

		// Collect timeline marker for ALL events (regardless of search
		// filter) so the seekbar shows full density even when filtered.
		// Store raw ns — fractions computed each poll() tick using
		// displayDurNs_ so markers shift left as recording time grows.
		if (tin != kNoInstant && tout != kNoInstant && tout > tin)
			rawMarkers.push_back({tin, tout});
		if (tin != kNoInstant && tout != kNoInstant && tout > tin)
			rawMarkerIds.push_back(id);

		// search filter (id / per-angle notes / angles)
		if (!needle.isEmpty()) {
			QString hay = QString::number(id) + " " + anglesStr;
			for (int k = 0; k < kEventAngles; k++)
				if (!camNotes[k].empty())
					hay += " " + QString::fromStdString(
							       camNotes[k])
							       .toLower();
			if (!hay.contains(needle)) {
				obs_data_release(e);
				continue;
			}
		}

		Row r;
		r.id = id;
		r.tin = tin;
		r.tout = tout;
		for (int k = 0; k < kEventAngles; k++) {
			r.camOn[k] = camOn[k];
			r.camSpeeds[k] = camSpeeds[k];
			r.camNotes[k] = camNotes[k];
		}
		rows.push_back(std::move(r));

		obs_data_release(e);
	}
	obs_data_array_release(arr);

	if (rowCfg.sortEventsByTime)
		// Stable: two marks at the same instant keep the order they were
		// taken in, which is the only tie-break that means anything.
		std::stable_sort(rows.begin(), rows.end(),
				 [](const Row &a, const Row &b) {
					 return a.tin < b.tin;
				 });
	const int idDigits = std::clamp(rowCfg.eventIdDigits, 1, 8);

	auto roItem = [](const QString &txt, Qt::Alignment al) {
		auto *it = new QTableWidgetItem(txt);
		it->setTextAlignment(al);
		it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
		return it;
	};

	for (const Row &r : rows) {
		const int row = events_->rowCount();
		events_->insertRow(row);

		const bool closed = r.tout != kNoInstant;
		QString dur = closed ? formatTc(r.tout - r.tin)
				     : QString::fromUtf8(
					       obs_module_text("Dock.Open"));

		// Nothing behind the mark: flag it here rather than letting the
		// operator find out by pressing play during a match. A closed
		// event shorter than a millisecond is the other unplayable kind:
		// Mark Out taken at the same instant as Mark In, which markOut
		// widens to exactly 1 ns so the event is "closed". No frame can
		// sit in that range - the last session's log has one, event 4,
		// failing with "no video frame at or after the requested
		// in-point" five times in a row.
		const bool degenerate = closed && r.tout - r.tin < 1'000'000;
		const bool playable = r.tin != kNoInstant && !degenerate &&
				      footageExists(r.tin);
		// the reference controller pads ids to a fixed width so they stay the same length for
		// the whole match and can be called out loud.
		const QString idText =
			QString("%1").arg(r.id, idDigits, 10, QLatin1Char('0'));
		auto *idItem = roItem(playable ? idText
					       : QStringLiteral("⚠ ") + idText,
				      Qt::AlignCenter);
		idItem->setData(Qt::UserRole, r.id);
		if (!playable)
			idItem->setToolTip(
				obs_module_text("Dock.EventNoFootage"));
		events_->setItem(row, kColId, idItem);
		events_->setItem(row, kColIn, roItem(relTc(r.tin), mid));
		events_->setItem(row, kColOut,
				 roItem(closed ? relTc(r.tout)
					       : QStringLiteral("--"),
					mid));
		events_->setItem(row, kColDur, roItem(dur, mid));

		// ONE cell per camera, holding the three things an operator says
		// about that angle: does it play, how fast, and what is it. They
		// were two columns and it read as two subjects; with four cameras
		// on screen the eye had to pair them up again every time. Now the
		// unit on screen is the unit in the operator's head — the angle —
		// and none of it is behind a dialog, because during a match an
		// edit that costs a dialog is an edit that does not get made.
		for (size_t ci = 0; ci < camCols_.size(); ci++) {
			const int cam = camCols_[ci];
			const int col = kColFirstCam + (int)ci * kColsPerCam;
			events_->setCellWidget(
				row, col, buildAngleCell(r.id, cam, r.camOn[cam],
							 r.camSpeeds[cam],
							 r.camNotes[cam]));
			// The cell still carries an item underneath: the gate and
			// the selection model both address rows through items, and
			// a cell that is only a widget is a hole in that.
			auto *slot = new QTableWidgetItem(QString());
			slot->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
			slot->setData(Qt::UserRole, r.id);
			events_->setItem(row, col, slot);
		}
	}
	refreshing_ = false;
	markerNs_ = std::move(rawMarkers);
	markerIds_ = std::move(rawMarkerIds);
	// Fraction conversion happens in poll() each tick via displayDurNs_.

	// Keep exactly one event selected: when a newer event appears (a fresh
	// mark), auto-select it so "Riproduci selezionati" is one click; otherwise
	// preserve the user's selection across the rebuild.
	int maxId = 0;
	for (int r = 0; r < events_->rowCount(); r++) {
		QTableWidgetItem *it = events_->item(r, kColId);
		if (it)
			maxId = std::max(maxId, it->data(Qt::UserRole).toInt());
	}
	int target = (maxId > lastMaxEventId_) ? maxId
		     : (prevSel > 0)           ? prevSel
					       : maxId;
	lastMaxEventId_ = maxId;
	if (target > 0) {
		for (int r = 0; r < events_->rowCount(); r++) {
			QTableWidgetItem *it = events_->item(r, kColId);
			if (it && it->data(Qt::UserRole).toInt() == target) {
				events_->selectRow(r);
				break;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Trimming an event that has already been marked
// ---------------------------------------------------------------------------

void MultiReplayDock::setSelectedPoint(bool inPoint)
{
	const auto ids = selectedEventIds();
	if (ids.empty()) {
		showNotice(obs_module_text("Dock.TrimNoSelection"));
		return;
	}
	if (playheadNs_ == kNoInstant) {
		showNotice(obs_module_text("Dock.TrimNoPosition"));
		return;
	}
	ReplayEvent ev;
	if (!EventStore::instance().get(ids.front(), ev))
		return;
	const int64_t from = inPoint ? ev.tInNs : ev.tOutNs;
	if (from == kNoInstant) {
		// An open event has no OUT to move; marking one is Mark Out's
		// job, and doing it from here would hide which key did what.
		showNotice(obs_module_text("Dock.TrimNoPoint"));
		return;
	}
	// Expressed as a delta so the store's own clamping (an IN cannot pass
	// its OUT, and the other way round) is the one that decides — there is
	// exactly one copy of that rule and it is already unit-tested.
	if (!EventStore::instance().movePoint(ids.front(), inPoint,
					      playheadNs_ - from)) {
		showNotice(obs_module_text("Dock.TrimRefused"));
		return;
	}
	obs_log(LOG_INFO, "[dock] trim: event %d %s moved by %lld ms",
		ids.front(), inPoint ? "IN" : "OUT",
		(long long)((playheadNs_ - from) / 1'000'000));
	refreshEvents();
}

void MultiReplayDock::onMarkerDragged(int eventId, bool inPoint, double frac)
{
	if (timeline_.empty())
		return;
	ReplayEvent ev;
	if (!EventStore::instance().get(eventId, ev))
		return;
	const int64_t want = timeline_.instantAt(frac);
	const int64_t from = inPoint ? ev.tInNs : ev.tOutNs;
	if (from == kNoInstant)
		return;
	// A delta, like every other way of moving a point: the store owns the
	// rule that an IN cannot pass its OUT, and it is tested in one place.
	if (!EventStore::instance().movePoint(eventId, inPoint, want - from)) {
		showNotice(obs_module_text("Dock.TrimRefused"));
		return;
	}
	obs_log(LOG_INFO, "[dock] dragged event %d's %s by %lld ms", eventId,
		inPoint ? "IN" : "OUT", (long long)((want - from) / 1'000'000));
	refreshEvents();
}

void MultiReplayDock::nudgeSelectedPointNs(bool inPoint, int64_t deltaNs)
{
	const auto ids = selectedEventIds();
	if (ids.empty()) {
		showNotice(obs_module_text("Dock.TrimNoSelection"));
		return;
	}
	if (EventStore::instance().movePoint(ids.front(), inPoint, deltaNs))
		refreshEvents();
	else
		showNotice(obs_module_text("Dock.TrimRefused"));
}

void MultiReplayDock::nudgeSelectedPoint(bool inPoint, int frames)
{
	const auto ids = selectedEventIds();
	if (ids.empty())
		return;
	obs_video_info ovi{};
	const double fps = obs_get_video_info(&ovi) && ovi.fps_den
				   ? (double)ovi.fps_num / (double)ovi.fps_den
				   : 30.0;
	const int64_t frameNs = (int64_t)(1'000'000'000.0 / std::max(1.0, fps));
	if (EventStore::instance().movePoint(ids.front(), inPoint,
					     frameNs * frames))
		refreshEvents();
}

// ---------------------------------------------------------------------------
// Which channel the controls drive (the reference controller's A|B / A / B)
// ---------------------------------------------------------------------------

PlaybackCoordinator &MultiReplayDock::pc() const
{
	return PlaybackCoordinator::instance(activeChannel_);
}

ReplayChannel &MultiReplayDock::chan() const
{
	return ReplayChannel::instance(activeChannel_);
}

std::vector<Which> MultiReplayDock::targetChannels() const
{
	// A|B is not "A and also B sometimes": it is one command going to both,
	// which is the whole reason a two-channel panel is one panel.
	if (linkedAB_)
		return {Which::A, Which::B};
	return {activeChannel_};
}

void MultiReplayDock::setActiveChannel(Which which, bool linked)
{
	activeChannel_ = which;
	linkedAB_ = linked;
	// The preview, the badge and the transport all read these on the next
	// tick; nothing is stopped or started by CHOOSING a channel, because in
	// the reference controller the selector picks where the keys go, not what is on air.
	previewCam0_ = -1; // force the preview source to be re-resolved
	// The letters under the two boxes say which one the keys are driving —
	// both lit under A|B, because under A|B they are.
	if (labelA_ && labelB_) {
		labelA_->setProperty("active", linked || which == Which::A);
		labelB_->setProperty("active", linked || which == Which::B);
		repolish(labelA_);
		repolish(labelB_);
	}
	if (chanBadge_)
		chanBadge_->setText(QString("%1%2")
					    .arg(linked ? QStringLiteral("A|B")
							: QString(channelLetter(
								  which)))
					    .arg(currentAngle1_));
	poll();
}

void MultiReplayDock::swapChannels()
{
	// ⇄ — what A is playing goes to B and B's goes to A. Each channel is
	// asked to play the OTHER's clip from its IN: there is no way to hand a
	// half-played clip across, and starting from the top is what an operator
	// means by "put that on the other channel".
	auto &pa = PlaybackCoordinator::instance(Which::A);
	auto &pb = PlaybackCoordinator::instance(Which::B);
	const auto sa = pa.playState();
	const auto sb = pb.playState();
	if (!sa.active && !sb.active) {
		showNotice(obs_module_text("Dock.NothingQueued"));
		return;
	}
	pa.stopEvents();
	pb.stopEvents();
	std::string err;
	// B takes what A had, A takes what B had. Angles come across with the
	// clips: the point of the swap is to keep watching the same two things
	// the other way round.
	if (sa.active && sa.eventId > 0)
		pb.playEvents({sa.eventId}, sa.angle1 - 1, false, err,
			      PlaybackCoordinator::AngleMode::Single);
	if (sb.active && sb.eventId > 0)
		pa.playEvents({sb.eventId}, sb.angle1 - 1, false, err,
			      PlaybackCoordinator::AngleMode::Single);
	obs_log(LOG_INFO, "[dock] swap: A(event %d angle %d) <-> B(event %d angle %d)",
		sa.eventId, sa.angle1, sb.eventId, sb.angle1);
}

QWidget *MultiReplayDock::buildAngleCell(int eventId, int cam0, bool on,
					 double speed, const std::string &note)
{
	// [☑] [speed ▾] [comment ▾] — one widget, one angle, three answers.
	// Widgets rather than a delegate on purpose: the check has to toggle on
	// the FIRST click and the two menus have to open on the first click too.
	// With a delegate each of those is a click to select, then a click to
	// edit, and in a live gallery that second click is the one that does not
	// happen.
	auto *w = new QWidget(events_);
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(4, 0, 4, 0);
	h->setSpacing(4);

	auto *box = new QCheckBox(w);
	box->setChecked(on);
	box->setToolTip(obs_module_text("Dock.AngleOnHint"));
	h->addWidget(box);

	auto *sp = new QComboBox(w);
	sp->setObjectName("mrAngleSpeed");
	sp->setToolTip(obs_module_text("Dock.AngleSpeedHint"));
	// "--" first: it is the common case (the slider decides) and it must be
	// one click away from any override.
	sp->addItem(QStringLiteral("--"), -1);
	for (int pct : {25, 33, 50, 75, 100, 200})
		sp->addItem(QString("%1%").arg(pct), pct);
	const int pct = speed >= 0 ? (int)std::lround(speed * 100.0) : -1;
	int idx = sp->findData(pct);
	if (idx < 0 && pct > 0) { // a speed typed elsewhere (an older project)
		sp->addItem(QString("%1%").arg(pct), pct);
		idx = sp->count() - 1;
	}
	sp->setCurrentIndex(idx < 0 ? 0 : idx);
	// Amber for an override, grey for "the slider decides": which of the two
	// it is has to be readable without stopping to read it.
	sp->setStyleSheet(pct > 0 ? "color:#ffd07a;" : "color:#707070;");
	h->addWidget(sp);

	auto *cm = new QComboBox(w);
	cm->setObjectName("mrAngleNote");
	cm->setEditable(true); // free text stays free text
	cm->setInsertPolicy(QComboBox::NoInsert);
	cm->setToolTip(obs_module_text("Dock.CamNoteHint"));
	cm->addItem(QString()); // "no comment" is the first choice, not a gap
	for (const auto &p : ReplayCore::instance().getConfig().commentPresets)
		cm->addItem(QString::fromStdString(p));
	cm->setCurrentText(QString::fromStdString(note));
	cm->lineEdit()->setPlaceholderText(kNoNote);
	h->addWidget(cm, 1);

	const int a1 = cam0 + 1; // EventStore is 1-based

	connect(box, &QCheckBox::toggled, this, [this, eventId, a1](bool v) {
		if (refreshing_)
			return;
		EventStore::instance().setAngle(eventId, a1, v);
	});
	connect(sp, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this, sp, eventId, a1](int) {
			if (refreshing_)
				return;
			const int v = sp->currentData().toInt();
			EventStore::instance().setAngleSpeed(
				eventId, a1, v > 0 ? v / 100.0 : -1.0);
		});
	// Both ways a comment can arrive: picked from the operator's own list, or
	// typed. editingFinished rather than textChanged — writing to the store
	// on every keystroke would bump its version and have poll() rebuild the
	// table out from under the cursor.
	connect(cm->lineEdit(), &QLineEdit::editingFinished, this,
		[this, cm, eventId, a1]() {
			if (refreshing_)
				return;
			EventStore::instance().setAngleNote(
				eventId, a1, cm->currentText().trimmed().toStdString());
		});
	connect(cm, QOverload<int>::of(&QComboBox::activated), this,
		[this, cm, eventId, a1](int) {
			if (refreshing_)
				return;
			EventStore::instance().setAngleNote(
				eventId, a1, cm->currentText().trimmed().toStdString());
		});
	return w;
}

void MultiReplayDock::onEventItemChanged(QTableWidgetItem *item)
{
	// Nothing in this table is edited through its ITEMS any more: in, out and
	// duration are read-only, and everything an operator changes about an
	// angle lives in the widget that cell holds (see buildAngleCell). The
	// connection stays so that a future editable column cannot arrive
	// silently unhandled.
	(void)item;
}

// ---------------------------------------------------------------------------
// Event filter — double-click on note labels to edit
// ---------------------------------------------------------------------------

bool MultiReplayDock::eventFilter(QObject *watched, QEvent *event)
{
	// Clicking a multiview tile (its picture or its caption) selects that
	// angle — the same thing the numbered angle button does, reached from the
	// picture the operator is already looking at. The replay tile is not an
	// angle, so it is inert.
	if (event->type() == QEvent::MouseButtonPress) {
		auto *me = static_cast<QMouseEvent *>(event);
		if (me->button() == Qt::LeftButton) {
			for (const PreviewTile &t : tiles_) {
				if (t.cam0 < 0)
					continue;
				if (watched != t.display && watched != t.caption)
					continue;
				setAngle(t.cam0 + 1);
				return true;
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------

void MultiReplayDock::openSettings()
{
	auto &core = ReplayCore::instance();
	Config cfg = core.getConfig();

	QDialog dlg(this);
	dlg.setWindowTitle(obs_module_text("Dock.Settings"));
	dlg.setMinimumSize(760, 480);

	// A SIDE MENU AND PAGES, not one flat column of a dozen unrelated fields.
	// The old dialog put the session folder, the audio bitrate, the pre-roll,
	// the output scene and eight camera slots in one list, so finding the one
	// setting you came for meant reading all of them — and it gave no clue
	// which of them belong together. Grouped by what the operator is trying to
	// do: record, wire the cameras, play out, mark events, arrange the panel.
	auto *root = new QVBoxLayout(&dlg);
	auto *body = new QHBoxLayout();
	body->setSpacing(0);

	auto *nav = new QListWidget(&dlg);
	nav->setObjectName("mrSettingsNav");
	nav->setFixedWidth(172);
	nav->setFocusPolicy(Qt::NoFocus);
	nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *pages = new QStackedWidget(&dlg);
	body->addWidget(nav, 0);
	body->addWidget(pages, 1);
	root->addLayout(body, 1);

	// One page per group: a heading, one line saying what the group is FOR
	// (an operator who has to guess reads every field anyway), then the
	// fields themselves.
	const auto addPage = [&](const char *titleKey,
				 const char *blurbKey) -> QFormLayout * {
		auto *page = new QWidget(pages);
		auto *v = new QVBoxLayout(page);
		v->setContentsMargins(14, 12, 14, 12);
		v->setSpacing(2);

		auto *title = new QLabel(obs_module_text(titleKey), page);
		title->setObjectName("mrSettingsTitle");
		v->addWidget(title);

		auto *blurb = new QLabel(obs_module_text(blurbKey), page);
		blurb->setObjectName("mrSettingsBlurb");
		blurb->setWordWrap(true);
		v->addWidget(blurb);

		auto *form = new QFormLayout();
		form->setContentsMargins(0, 10, 0, 0);
		form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
		form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
		v->addLayout(form);
		v->addStretch(1);

		pages->addWidget(page);
		nav->addItem(obs_module_text(titleKey));
		return form;
	};
	connect(nav, &QListWidget::currentRowChanged, pages,
		&QStackedWidget::setCurrentIndex);

	// ── Recording ─────────────────────────────────────────────────────
	QFormLayout *recPage = addPage("Dock.SetRecording", "Dock.SetRecordingBlurb");

	auto *folderRow = new QHBoxLayout();
	auto *folderEdit =
		new QLineEdit(QString::fromStdString(cfg.sessionFolder), &dlg);
	auto *browse = new QPushButton("...", &dlg);
	browse->setFixedWidth(34);
	folderRow->addWidget(folderEdit, 1);
	folderRow->addWidget(browse);
	connect(browse, &QPushButton::clicked, &dlg, [&]() {
		QString f = QFileDialog::getExistingDirectory(
			&dlg, obs_module_text("Dock.SessionFolder"),
			folderEdit->text());
		if (!f.isEmpty())
			folderEdit->setText(f);
	});
	recPage->addRow(obs_module_text("Dock.SessionFolder"), folderRow);

	auto *split = new QSpinBox(&dlg);
	// 0 = never split: an ISO is normally one continuous file (see
	// branch_output::buildSettings). specialValueText replaces the number at
	// the minimum, so 0 reads as words instead of a meaningless "0 min".
	split->setRange(0, 240);
	split->setValue(cfg.splitMinutes);
	split->setSuffix(" min");
	split->setSpecialValueText(obs_module_text("Dock.SplitNever"));
	split->setToolTip(obs_module_text("Dock.SplitMinutesHint"));
	recPage->addRow(obs_module_text("Dock.SplitMinutes"), split);

	auto *vbr = new QSpinBox(&dlg);
	vbr->setRange(1000, 200000);
	vbr->setSingleStep(1000);
	vbr->setValue(cfg.videoBitrateKbps);
	vbr->setSuffix(" kbps");
	recPage->addRow(obs_module_text("Dock.VideoBitrate"), vbr);

	auto *abr = new QSpinBox(&dlg);
	abr->setRange(64, 1024);
	abr->setValue(cfg.audioBitrateKbps);
	abr->setSuffix(" kbps");
	recPage->addRow(obs_module_text("Dock.AudioBitrate"), abr);

	// ── Cameras ───────────────────────────────────────────────────────
	QFormLayout *camPage = addPage("Dock.SetCameras", "Dock.SetCamerasBlurb");

	// Gathered once and shared by every source picker on every page.
	QStringList sourceNames;
	{
		Data sd(core.sourcesJson());
		obs_data_array_t *arr =
			sd ? obs_data_get_array(sd, "sources") : nullptr;
		if (arr) {
			size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; i++) {
				obs_data_t *it = obs_data_array_item(arr, i);
				sourceNames << QString::fromUtf8(
					obs_data_get_string(it, "name"));
				obs_data_release(it);
			}
			obs_data_array_release(arr);
		}
	}
	auto makeSourceCombo = [&](const std::string &cur) {
		auto *c = new QComboBox(&dlg);
		c->addItem(obs_module_text("Dock.None"), "");
		for (const auto &nm : sourceNames)
			c->addItem(nm, nm);
		int idx = c->findData(QString::fromStdString(cur));
		if (idx >= 0)
			c->setCurrentIndex(idx);
		return c;
	};

	std::vector<QComboBox *> camCombos;
	std::vector<QLineEdit *> camNameEdits;
	for (int i = 0; i < kMaxCameras; i++) {
		auto *row = new QHBoxLayout();
		auto *c = makeSourceCombo(cfg.cameras[i].sourceName);
		auto *nameEdit = new QLineEdit(
			QString::fromStdString(cfg.cameras[i].displayName), &dlg);
		nameEdit->setPlaceholderText(
			QString(obs_module_text("Dock.CameraName")).arg(i + 1));
		nameEdit->setFixedWidth(130);
		row->addWidget(c, 1);
		row->addWidget(nameEdit);
		camCombos.push_back(c);
		camNameEdits.push_back(nameEdit);
		camPage->addRow(QString("Cam %1").arg(i + 1), row);
	}

	// ── Replay / playout ──────────────────────────────────────────────
	QFormLayout *outPage = addPage("Dock.SetReplay", "Dock.SetReplayBlurb");

	// No replay-source selector: "MultiReplay - Replay A" is a plugin-provided
	// OBS input the operator drops into whatever scene he likes, exactly like
	// a capture card. cfg.replaySourceName survives only for back-compat.
	//
	// The OUTPUT SCENE selector, however, is needed: PlaybackCoordinator only
	// takes program on "to output" when cfg.outputSceneName names a scene.
	auto *outScene = new QComboBox(&dlg);
	outScene->setToolTip(obs_module_text("Dock.OutputSceneHint"));
	outScene->addItem(obs_module_text("Dock.None"), "");
	{
		// obs_frontend_get_scenes returns the scene sources in the order
		// shown in the Scenes dock; names are what the coordinator resolves
		// with obs_get_source_by_name, so store the name as the item data.
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			const char *nm =
				obs_source_get_name(scenes.sources.array[i]);
			if (nm && *nm)
				outScene->addItem(QString::fromUtf8(nm),
						  QString::fromUtf8(nm));
		}
		obs_frontend_source_list_free(&scenes);
	}
	{
		// A scene configured earlier may have been renamed or deleted; keep
		// it in the list rather than silently resetting the setting.
		const QString cur = QString::fromStdString(cfg.outputSceneName);
		int idx = outScene->findData(cur);
		if (idx < 0 && !cur.isEmpty()) {
			outScene->addItem(cur, cur);
			idx = outScene->count() - 1;
		}
		if (idx >= 0)
			outScene->setCurrentIndex(idx);
	}
	outPage->addRow(obs_module_text("Dock.OutputScene"), outScene);

	// ...and B's own. Two channels are two OBS inputs, so they live in two
	// scenes: with one scene for both, playing on B switched program to the
	// scene that holds A and the operator watched A while B was the thing
	// that was playing. Same list, same "(none)" meaning "do not touch
	// program".
	auto *outSceneB = new QComboBox(&dlg);
	outSceneB->setToolTip(obs_module_text("Dock.OutputSceneBHint"));
	for (int i = 0; i < outScene->count(); i++)
		outSceneB->addItem(outScene->itemText(i), outScene->itemData(i));
	{
		const QString cur = QString::fromStdString(cfg.outputSceneNameB);
		int idx = outSceneB->findData(cur);
		if (idx < 0 && !cur.isEmpty()) {
			outSceneB->addItem(cur, cur);
			idx = outSceneB->count() - 1;
		}
		if (idx >= 0)
			outSceneB->setCurrentIndex(idx);
	}
	outPage->addRow(obs_module_text("Dock.OutputSceneB"), outSceneB);

	auto *autoSwitch = new QCheckBox(&dlg);
	autoSwitch->setChecked(cfg.autoSwitchScene);
	outPage->addRow(obs_module_text("Dock.AutoSwitch"), autoSwitch);

	// Scale the replay to the canvas. This is a scene-item transform, not a
	// picture change: the frames stay at the camera's own resolution and the
	// GPU scales them while compositing (see ReplayChannel::applyCanvasFit).
	auto *fitCanvas = new QCheckBox(&dlg);
	fitCanvas->setChecked(cfg.fitReplayToCanvas);
	fitCanvas->setToolTip(obs_module_text("Dock.FitCanvasHint"));
	outPage->addRow(obs_module_text("Dock.FitCanvas"), fitCanvas);

	auto *music = makeSourceCombo(cfg.musicSourceName);
	outPage->addRow(obs_module_text("Dock.MusicSource"), music);

	// ── Events ────────────────────────────────────────────────────────
	QFormLayout *evPage = addPage("Dock.SetEvents", "Dock.SetEventsBlurb");

	// Pre/post roll: the operator marks after he has seen the action, so the
	// event has to start before his finger did. Whole seconds like the reference controller, with
	// tenths available because a football replay and a snooker replay do not
	// want the same padding.
	auto *preRoll = new QDoubleSpinBox(&dlg);
	preRoll->setRange(0.0, 30.0);
	preRoll->setSingleStep(0.5);
	preRoll->setDecimals(1);
	preRoll->setSuffix(" s");
	preRoll->setValue(cfg.preRollMs / 1000.0);
	preRoll->setToolTip(obs_module_text("Dock.PreRollHint"));
	evPage->addRow(obs_module_text("Dock.PreRoll"), preRoll);

	auto *postRoll = new QDoubleSpinBox(&dlg);
	postRoll->setRange(0.0, 30.0);
	postRoll->setSingleStep(0.5);
	postRoll->setDecimals(1);
	postRoll->setSuffix(" s");
	postRoll->setValue(cfg.postRollMs / 1000.0);
	postRoll->setToolTip(obs_module_text("Dock.PostRollHint"));
	evPage->addRow(obs_module_text("Dock.PostRoll"), postRoll);

	auto *sortByTime = new QCheckBox(&dlg);
	sortByTime->setChecked(cfg.sortEventsByTime);
	sortByTime->setToolTip(obs_module_text("Dock.SortByTimeHint"));
	evPage->addRow(obs_module_text("Dock.SortByTime"), sortByTime);

	auto *idDigits = new QSpinBox(&dlg);
	idDigits->setRange(1, 8);
	idDigits->setValue(cfg.eventIdDigits);
	idDigits->setToolTip(obs_module_text("Dock.IdDigitsHint"));
	evPage->addRow(obs_module_text("Dock.IdDigits"), idDigits);

	// How many of the 20 lists to show. Fewer lists = wider tabs = readable
	// names, which is the whole reason this setting exists.
	auto *listCount = new QSpinBox(&dlg);
	listCount->setRange(1, kEventLists);
	listCount->setValue(cfg.eventListCount);
	listCount->setToolTip(obs_module_text("Dock.ListCountHint"));
	evPage->addRow(obs_module_text("Dock.ListCount"), listCount);

	// The comments this operator writes over and over. One per line, and the
	// order is the order of the drop-down on every angle cell — so the ones
	// he reaches for during a match go at the top. Free text is never taken
	// away: this is a shortcut, not a vocabulary.
	auto *presets = new QPlainTextEdit(&dlg);
	{
		QString joined;
		for (const auto &p : cfg.commentPresets)
			joined += QString::fromStdString(p) + "\n";
		presets->setPlainText(joined);
	}
	presets->setPlaceholderText(obs_module_text("Dock.CommentPresetsHint"));
	presets->setToolTip(obs_module_text("Dock.CommentPresetsHint"));
	presets->setMaximumHeight(96);
	evPage->addRow(obs_module_text("Dock.CommentPresets"), presets);

	// ── Interface ─────────────────────────────────────────────────────
	QFormLayout *uiPage = addPage("Dock.SetInterface", "Dock.SetInterfaceBlurb");

	// The multiview strip. Each tile is an obs_display rendered by the same
	// graphics thread as the OBS program preview, so a rig that is short of
	// GPU gets a switch rather than a slow dock nobody can explain.
	auto *multiview = new QCheckBox(&dlg);
	multiview->setChecked(cfg.showMultiview);
	multiview->setToolTip(obs_module_text("Dock.ShowMultiviewHint"));
	uiPage->addRow(obs_module_text("Dock.ShowMultiview"), multiview);

	// ── Advanced ──────────────────────────────────────────────────────
	QFormLayout *advPage = addPage("Dock.SetAdvanced", "Dock.SetAdvancedBlurb");

	auto *enc = new QComboBox(&dlg);
	enc->addItem(obs_module_text("Dock.AutoEncoder"), "");
	{
		Data ed(core.encodersJson());
		obs_data_array_t *arr =
			ed ? obs_data_get_array(ed, "encoders") : nullptr;
		if (arr) {
			size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; i++) {
				obs_data_t *it = obs_data_array_item(arr, i);
				enc->addItem(QString::fromUtf8(obs_data_get_string(
						     it, "name")),
					     QString::fromUtf8(obs_data_get_string(
						     it, "id")));
				obs_data_release(it);
			}
			obs_data_array_release(arr);
		}
	}
	{
		int idx = enc->findData(QString::fromStdString(cfg.videoEncoderId));
		if (idx >= 0)
			enc->setCurrentIndex(idx);
	}
	advPage->addRow(obs_module_text("Dock.Encoder"), enc);

	nav->setCurrentRow(0);

	auto *buttons = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	root->addWidget(buttons, 0);

	if (dlg.exec() != QDialog::Accepted)
		return;

	cfg.sessionFolder = folderEdit->text().toStdString();
	cfg.splitMinutes = split->value();
	cfg.videoBitrateKbps = vbr->value();
	cfg.audioBitrateKbps = abr->value();
	cfg.preRollMs = (int)std::lround(preRoll->value() * 1000.0);
	cfg.postRollMs = (int)std::lround(postRoll->value() * 1000.0);
	cfg.sortEventsByTime = sortByTime->isChecked();
	cfg.eventIdDigits = idDigits->value();
	cfg.eventListCount = listCount->value();
	cfg.commentPresets.clear();
	for (const QString &line :
	     presets->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
		const QString t = line.trimmed();
		if (!t.isEmpty())
			cfg.commentPresets.push_back(t.toStdString());
	}
	cfg.videoEncoderId = enc->currentData().toString().toStdString();
	cfg.outputSceneName = outScene->currentData().toString().toStdString();
	cfg.outputSceneNameB = outSceneB->currentData().toString().toStdString();
	cfg.musicSourceName = music->currentData().toString().toStdString();
	cfg.autoSwitchScene = autoSwitch->isChecked();
	cfg.fitReplayToCanvas = fitCanvas->isChecked();
	cfg.showMultiview = multiview->isChecked();
	for (int i = 0; i < kMaxCameras; i++) {
		cfg.cameras[i].sourceName =
			camCombos[i]->currentData().toString().toStdString();
		cfg.cameras[i].displayName =
			camNameEdits[i]->text().trimmed().toStdString();
	}
	core.setConfig(cfg);
	// Use recordingFolder() so EventStore points to the project subfolder
	// (if one is active) rather than the raw session folder.
	EventStore::instance().setSessionFolder(core.recordingFolder());
	// Cheap safety net: recreate the replay input if the operator deleted it.
	ReplayChannel::instance().ensureSource();
	// Applies immediately, so the operator sees the answer to the checkbox he
	// just ticked without waiting for the next replay.
	ReplayChannel::instance().applyCanvasFit(cfg.fitReplayToCanvas);
	refreshAngles();
	refreshEvents();
}

void MultiReplayDock::newProjectDialog()
{
	if (ReplayCore::instance().isRecording()) {
		QMessageBox::warning(this, "obs-multireplay",
				     obs_module_text("Dock.StopRecFirst"));
		return;
	}
	bool ok;
	QString title = QInputDialog::getText(
		this, obs_module_text("Dock.NewProject"),
		obs_module_text("Dock.ProjectNameLabel"), QLineEdit::Normal,
		"", &ok);
	if (!ok || title.trimmed().isEmpty())
		return;
	std::string err;
	if (!ReplayCore::instance().newProject(title.trimmed().toStdString(),
					       err)) {
		QMessageBox::warning(this, "obs-multireplay",
				     QString::fromStdString(err));
		return;
	}
	refreshEvents();
	poll();
}

void MultiReplayDock::openProjectDialog()
{
	if (ReplayCore::instance().isRecording()) {
		QMessageBox::warning(this, "obs-multireplay",
				     obs_module_text("Dock.StopRecFirst"));
		return;
	}
	auto projects = ReplayCore::instance().listProjects();
	if (projects.empty()) {
		QMessageBox::information(
			this, "obs-multireplay",
			obs_module_text("Dock.NoProjectsFound"));
		return;
	}
	QStringList items;
	items.reserve((int)projects.size());
	for (const auto &p : projects)
		items << QString::fromStdString(p);
	bool ok;
	QString sel = QInputDialog::getItem(
		this, obs_module_text("Dock.OpenProject"),
		obs_module_text("Dock.SelectProject"), items, 0, false, &ok);
	if (!ok || sel.isEmpty())
		return;
	std::string err;
	if (!ReplayCore::instance().openProject(sel.toStdString(), err)) {
		QMessageBox::warning(this, "obs-multireplay",
				     QString::fromStdString(err));
		return;
	}
	// poll() FIRST: it reads the newly loaded anchors and seats the project
	// origin the table is drawn against. Rebuilding the table before that
	// rendered the just-loaded marks against an origin of 0 — raw monotonic
	// time — until some later tick happened to move the origin by a second.
	poll();
	refreshEvents();
	refreshAngles();
}

void MultiReplayDock::copyYouTubeChapters()
{
	int list = EventStore::instance().selectedList();
	// Chapter 0:00 is the start of the timeline the dock is showing, which is
	// the oldest instant still replayable — the same origin as the seekbar.
	// With no origin at all there is nothing to measure a chapter from, and
	// chaptersText would be handed kNoInstant to subtract.
	if (eventOriginNs_ == kNoInstant) {
		QMessageBox::information(this, "obs-multireplay",
					 obs_module_text("Dock.NoChapters"));
		return;
	}
	std::string text =
		// The project's footage begins the chapter list, not the angle the
		// operator is on (see eventOriginNs_).
		EventStore::instance().chaptersText(list, eventOriginNs_);
	if (text.empty()) {
		QMessageBox::information(
			this, "obs-multireplay",
			obs_module_text("Dock.NoChapters"));
		return;
	}
	QApplication::clipboard()->setText(QString::fromStdString(text));

	// Also write a physical file in the project folder so the chapter list is
	// persisted next to the recordings (not only on the clipboard).
	QString folder = QString::fromStdString(
		ReplayCore::instance().recordingFolder());
	QString fpath = folder.isEmpty()
				? QString()
				: QDir(folder).filePath("youtube-chapters.txt");
	bool wrote = false;
	if (!fpath.isEmpty()) {
		QFile f(fpath);
		if (f.open(QIODevice::WriteOnly | QIODevice::Text |
			   QIODevice::Truncate)) {
			f.write(text.c_str());
			f.close();
			wrote = true;
		}
	}
	QMessageBox::information(
		this, "obs-multireplay",
		wrote ? (QString(obs_module_text("Dock.ChaptersCopied")) +
			 "\n" + fpath)
		      : QString(obs_module_text("Dock.ChaptersCopied")));
}

} // namespace multireplay
