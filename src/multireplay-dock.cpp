/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multireplay-dock.hpp"
#include "qt-display.hpp"
#include "replay-core.hpp"
#include "event-store.hpp"
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
#include <QMessageBox>
#include <QDockWidget>
#include <QSizePolicy>
#include <QStyle>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
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
/* the collapsible per-angle speed panel under the table */
QGroupBox#mrInspector {
	border: 1px solid #1c2a3c; border-radius: 3px;
	margin-top: 6px; color: #8a97a6; font-size: 10px;
}
QGroupBox#mrInspector::title {
	subcontrol-origin: margin; left: 7px; padding: 0 3px;
}
/* mrCamNoteEdit: styled inline via setStyleSheet() on the widget */

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
QTabBar#mrListTabs::tab {
	background: #141414; color: #8a8a8a;
	border: 1px solid #232323; border-bottom: 0;
	padding: 3px 9px; margin-right: 1px; min-width: 16px;
	font-size: 10px;
}
QTabBar#mrListTabs::tab:hover { background: #1e1e1e; color: #c0c0c0; }
QTabBar#mrListTabs::tab:selected {
	background: #1D3D74; color: #ffffff; border-color: #2a5296;
}

/* ── channel strip under the preview (the reference controller green info band) ─ */
QLabel#mrChanBadge {
	background: #0e4523; color: #ffffff;
	font-weight: 700; font-size: 11px; padding: 2px 7px;
}
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

// Event table column layout — the reference controller's: the fixed information columns, then ONE
// COLUMN PER CAMERA, each holding that angle's enable box and its comment.
//
// kColId must stay column 0 and kColIn column 1: the automated gate reads the
// padded id off item(row, 0) and fires a double-click on column 1 to prove it
// takes program. Camera columns start after kColSpeed and their count follows
// the camera configuration (see rebuildEventColumns / camCols_).
enum EventCol {
	kColId = 0,
	kColIn,
	kColOut,
	kColDur,
	kColSpeed,   // event speed, "--" = inherited (the reference controller), editable in place
	kColFirstCam // first per-camera column
};

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

} // namespace

// ---------------------------------------------------------------------------
// SeekBar
// ---------------------------------------------------------------------------

SeekBar::SeekBar(QWidget *parent) : QWidget(parent)
{
	setFixedHeight(28);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setCursor(Qt::PointingHandCursor);
	setMouseTracking(false);
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
	return std::clamp((double)(x - m) / w, 0.0, 1.0);
}

void SeekBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	// the reference controller's position bar is a full-height green band, not a bead on a rail:
	// it is the widest, brightest thing on the panel because it is the control
	// the operator's hand lives on. Same here — the whole widget IS the bar.
	const int m = 2;                       // horizontal margin
	const int h = height() - 2;            // track thickness (nearly full)
	const int y = 1;
	const int w = width() - 2 * m;
	const double pos = dragging_ ? dragFrac_ : positionFrac_;

	// Track (the remaining part of the timeline): the reference controller dark green
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x14, 0x64, 0x33));
	p.drawRect(QRectF(m, y, w, h));

	// Anything outside the seekable region is NOT green: it is not a place the
	// operator can go, and painting it like the rest would say it is.
	if (seekableFrac_ < 1.0) {
		p.setBrush(QColor(0x10, 0x18, 0x14));
		p.drawRect(QRectF(m + w * seekableFrac_, y,
				  w * (1.0 - seekableFrac_), h));
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
		double x0 = m + w * mk.first;
		double x1 = m + w * mk.second;
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

	// Played-up-to-here — the reference controller's bright green, drawn over the markers
	// (they show through: the fill is opaque, so the markers ahead of the
	// playhead are the ones that matter and those stay visible).
	if (pos > 0.0) {
		p.setBrush(QColor(0x19, 0x98, 0x47));
		p.drawRect(QRectF(m, y, w * pos, h));
	}

	// Playhead: a thin bright line, no bead. the reference controller draws a hairline, and a
	// bead on a 24 px band hides the very frame it is pointing at.
	const double hx = m + w * pos;
	p.setPen(QPen(QColor(0xff, 0xff, 0xff, dragging_ ? 0xff : 0xc0),
		      dragging_ ? 3.0 : 2.0, Qt::SolidLine, Qt::FlatCap));
	p.drawLine(QPointF(hx, y), QPointF(hx, y + h));

	// the reference controller prints the transport state ON the bar. Centred, with a dark halo
	// so it stays readable over both greens.
	if (!overlay_.isEmpty()) {
		QFont f = p.font();
		f.setPointSizeF(f.pointSizeF() * 1.05);
		f.setBold(true);
		p.setFont(f);
		const QRectF tr(m, y, w, h);
		p.setPen(QColor(0x00, 0x20, 0x0c, 0xb0));
		p.drawText(tr.adjusted(1, 1, 1, 1), Qt::AlignCenter, overlay_);
		p.setPen(QColor(0xff, 0xff, 0xff));
		p.drawText(tr, Qt::AlignCenter, overlay_);
	}
}

void SeekBar::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	dragging_ = true;
	// Clamp to seekable region so the user can't drag into unindexed territory.
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	emit scrubStateChanged(true);
	emit scrubMoved(dragFrac_);
	update();
}

void SeekBar::mouseMoveEvent(QMouseEvent *e)
{
	if (!dragging_)
		return;
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	emit scrubMoved(dragFrac_);
	update();
}

void SeekBar::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !dragging_)
		return;
	dragging_ = false;
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	positionFrac_ = dragFrac_;
	emit seekRequested(dragFrac_);
	emit scrubStateChanged(false);
	update();
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
	uint32_t sw = obs_source_get_width(src);
	uint32_t sh = obs_source_get_height(src);
	if (sw == 0 || sh == 0) {
		obs_source_release(src);
		return;
	}
	// Letterbox: fit source inside widget preserving aspect ratio.
	// Bars (black) appear on whichever axis has excess space.
	float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
	int dw = (int)((float)sw * scale);
	int dh = (int)((float)sh * scale);
	int x = ((int)cx - dw) / 2;
	int y = ((int)cy - dh) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	gs_set_viewport(x, y, dw, dh);
	obs_source_video_render(src);
	gs_projection_pop();
	gs_viewport_pop();

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
	// 1. list tabs · search · Live
	// 2. channel A preview + its green status strip
	// 3. the event list, one column per camera
	// 4. mark keys · angle row · export
	// 5. record + transport + slow-motion speed
	// 6. the full-width position bar
	//
	// An operator who has used the reference controller finds every control where his hand
	// already goes, which is the entire point of this layout.
	root->addWidget(buildToolbar());

	{
		// Preview above, list below: the reference controller stacks them, and the previous
		// side-by-side split had no equivalent there. Draggable, because an
		// OBS dock can be a wide strip or a tall column.
		splitter_ = new QSplitter(Qt::Vertical, this);
		splitter_->setChildrenCollapsible(false);
		splitter_->setHandleWidth(5);
		splitter_->addWidget(buildPreview());
		splitter_->addWidget(buildEvents());
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
	if (pollTimer_)
		pollTimer_->stop();
	if (previewSource_) {
		obs_source_release(previewSource_);
		previewSource_ = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Toolbar: list tabs · search · Live  (the reference controller's top strip)
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildToolbar()
{
	auto *box = new QWidget(this);
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(5);

	projectLbl_ = new QLabel(box);
	projectLbl_->setObjectName("mrMuted");
	projectLbl_->setStyleSheet("color: #487898; font-size: 9px; padding: 0 4px;");
	projectLbl_->hide();
	h->addWidget(projectLbl_);

	// The 20 lists as TABS, not a dropdown. the reference controller shows them all at once and
	// the operator jumps between them mid-match without opening anything; a
	// combo hides nineteen of them behind a click. Named lists show the name.
	listTabs_ = new QTabBar(box);
	listTabs_->setObjectName("mrListTabs");
	listTabs_->setDrawBase(false);
	listTabs_->setExpanding(false);
	listTabs_->setUsesScrollButtons(true);
	listTabs_->setElideMode(Qt::ElideRight);
	listTabs_->setFocusPolicy(Qt::NoFocus);
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
	h->addWidget(listTabs_, 0);
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

	displayA_ = new OBSQTDisplay(this);
	displayA_->setRenderCallback(&MultiReplayDock::drawChannelA, this);
	displayA_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	displayA_->setMinimumHeight(40);
	v->addWidget(displayA_, 1);

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
		if (!PlaybackCoordinator::instance().playLastEvent(
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
			if (!PlaybackCoordinator::instance().playEvents(
				    selectedEventIds(), currentAngle1_ - 1,
				    /*toOutput*/ true, err))
				showNotice(QString::fromStdString(err));
		});
		connect(actLast, &QAction::triggered, this, [this]() {
			std::string err;
			if (!PlaybackCoordinator::instance().playLastEvent(
				    currentAngle1_ - 1,
				    toOutputBtn_ && toOutputBtn_->isChecked(),
				    err))
				showNotice(QString::fromStdString(err));
		});
		connect(actStop, &QAction::triggered, this,
			[]() { PlaybackCoordinator::instance().stopEvents(); });
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
		[](bool on) { PlaybackCoordinator::instance().setLoop(on); });

	musicBtn_ = toggleBtn(QStringLiteral("♫"), this,
			      obs_module_text("Dock.Music"));
	connect(musicBtn_, &QPushButton::toggled, this, [](bool on) {
		PlaybackCoordinator::instance().setMusicEnabled(on);
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
		if (ReplayChannel::instance().playing()) {
			PlaybackCoordinator::instance().stopEvents();
			return;
		}
		ReplayCore::instance().setFollowLive(false);
		replayCurrent();
	});
	connect(nowBtn_, &QPushButton::clicked, this, []() {
		// the reference controller NOW: drop the replay and watch the live edge again.
		PlaybackCoordinator::instance().stopEvents();
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
				PlaybackCoordinator::instance().stopEvents();
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
				PlaybackCoordinator::instance().stopEvents();
				std::string err;
				if (!core.startRecording(err))
					QMessageBox::warning(
						this, "obs-multireplay",
						QString::fromStdString(err));
			}
			poll();
		});
		h->addWidget(recBtn_);

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

	// ── Row 3: the position bar, full width (the reference controller's green band) ───────
	seek_ = new SeekBar(this);
	connect(seek_, &SeekBar::scrubStateChanged, this,
		[this](bool dragging) { seekDragging_ = dragging; });
	connect(seek_, &SeekBar::scrubMoved, this, [this](double frac) {
		tcLbl_->setText(formatTc((int64_t)(frac * (double)displayDurNs_)) +
				" / " + formatTc(displayDurNs_));
	});
	connect(seek_, &SeekBar::seekRequested, this,
		[this](double frac) { seekToFraction(frac); });
	v->addWidget(seek_);

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
	// keeps his hand on the mouse. Two exemptions, both because a double-click
	// is ALSO how that cell is edited: the speed column and the per-camera
	// columns. Taking program because someone wanted to type a comment would
	// be the worst kind of surprise.
	connect(events_, &QTableWidget::cellDoubleClicked, this,
		[this](int row, int column) {
			if (column == kColSpeed || column >= kColFirstCam)
				return;
			QTableWidgetItem *it = events_->item(row, kColId);
			if (!it)
				return;
			const int id = it->data(Qt::UserRole).toInt();
			if (id <= 0)
				return;
			std::string err;
			if (!PlaybackCoordinator::instance().playEvents(
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

	// Inspector panel: the per-angle SPEED override for the selected event
	// (the enable toggle and the comment are in the table now, where the reference controller puts
	// them). Collapsed by default — it is the rarest of the three edits and
	// eight camera rows would cost more height than the table it sits under.
	inspector_ = new QGroupBox(obs_module_text("Dock.AngleSpeeds"), this);
	inspector_->setObjectName("mrInspector");
	inspector_->setCheckable(true);
	inspector_->setChecked(false);
	inspector_->setToolTip(obs_module_text("Dock.AngleSpeedsHint"));
	{
		auto *outer = new QVBoxLayout(inspector_);
		outer->setContentsMargins(6, 2, 6, 2);
		outer->setSpacing(0);
		inspectorBody_ = new QWidget(inspector_);
		inspectorLayout_ = new QVBoxLayout(inspectorBody_);
		inspectorLayout_->setContentsMargins(0, 2, 0, 2);
		inspectorLayout_->setSpacing(2);
		outer->addWidget(inspectorBody_);
		inspectorBody_->setVisible(false);
		// A checkable QGroupBox only DISABLES its children when unchecked,
		// and an empty frame still claims its minimum height — measured at
		// ~65 px of nothing between the list and the mark keys. Hiding the
		// body AND capping the frame is what actually gives the height back.
		inspector_->setMaximumHeight(26);
		connect(inspector_, &QGroupBox::toggled, this, [this](bool on) {
			if (inspectorBody_)
				inspectorBody_->setVisible(on);
			inspector_->setMaximumHeight(on ? QWIDGETSIZE_MAX : 26);
		});
	}
	v->addWidget(inspector_, 0);
	connect(events_->selectionModel(),
		&QItemSelectionModel::selectionChanged, this, [this]() {
			auto ids = selectedEventIds();
			populateInspector(ids.empty() ? 0 : ids.front());
		});

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
	events_->setColumnCount(kColFirstCam + (int)cams.size());
	QStringList headers;
	headers << QStringLiteral("#") << obs_module_text("Dock.In")
		<< obs_module_text("Dock.Out") << obs_module_text("Dock.Duration")
		<< obs_module_text("Dock.Speed");
	headers += camLabels;
	events_->setHorizontalHeaderLabels(headers);
	// The camera headers carry their own label in UserRole: the "angle I am
	// watching" marker is a prefix on the text (see updateCamHeaderHighlight),
	// so the plain label has to survive somewhere.
	for (size_t i = 0; i < cams.size(); i++) {
		QTableWidgetItem *h =
			events_->horizontalHeaderItem(kColFirstCam + (int)i);
		if (h)
			h->setData(Qt::UserRole, camLabels[(int)i]);
	}
	{
		QHeaderView *hh = events_->horizontalHeader();
		hh->setHighlightSections(false);
		for (int c = 0; c < events_->columnCount(); c++)
			hh->setSectionResizeMode(c, QHeaderView::ResizeToContents);
		// The camera columns take the slack, as in the reference controller, where they are
		// the wide half of the list: their cells hold free text.
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
			hot = kColFirstCam + (int)i;
	if (hot == camHeaderHot_)
		return;
	const bool wasRefreshing = refreshing_;
	refreshing_ = true;
	for (size_t i = 0; i < camCols_.size(); i++) {
		const int col = kColFirstCam + (int)i;
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
	if (playheadNs_ > 0)
		return playheadNs_;
	return ReplayChannel::instance().positionNs();
}

bool MultiReplayDock::markable(int64_t tNs)
{
	// Master time is os_gettime_ns(), which is never 0 on a running machine:
	// a 0 here means "no instant at all" — the tap has captured nothing on
	// this angle (not recording, or the angle has no Branch Output filter
	// running) and no clip has played, so there is no playhead either.
	// EventStore would happily store that as a mark at master 0, producing an
	// event that looks real in the list and can never be played back, because
	// no footage will ever cover instant zero. Refusing is the honest answer,
	// and saying so is better than a row nobody can explain.
	if (tNs > 0)
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
	const int next = std::clamp(listTabs_->currentIndex() + delta, 0,
				    listTabs_->count() - 1);
	listTabs_->setCurrentIndex(next); // its signal selects the list + refreshes
}

void MultiReplayDock::playSelected()
{
	std::string err;
	if (!PlaybackCoordinator::instance().playEvents(
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
	const int64_t edge = timelineStartNs_ + displayDurNs_;
	if (playheadNs_ <= 0 || displayDurNs_ <= 0) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return;
	}

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
	PlaybackCoordinator::instance().stopEvents();
	prevSequenceActive_ = false;
	core.setFollowLive(false);
	playheadNs_ = inNs;

	std::string err;
	if (!ReplayChannel::instance().play(currentAngle1_ - 1, inNs,
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
	PlaybackCoordinator::instance().setDefaultSpeedPct(speedPct_);
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
	auto &pc = PlaybackCoordinator::instance();
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
	const auto ps = PlaybackCoordinator::instance().playState();

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
		const int64_t hi = ev.tOutNs > 0 ? ev.tOutNs : ev.tInNs;
		clipPos = std::clamp(playheadNs_, ev.tInNs, hi);
	}

	QString l1 = listText;
	if (ps.active && ps.queued > 0)
		l1 += QString("   %1/%2 %3")
			      .arg(ps.queuePos, 2, 10, QLatin1Char('0'))
			      .arg(ps.queued, 2, 10, QLatin1Char('0'))
			      .arg(obs_module_text("Dock.Clips"));
	if (haveEv && ev.tOutNs > 0) {
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
		if (ev.tOutNs > 0)
			l2 += QString("  OUT %1")
				      .arg(signedTc(clipPos - ev.tOutNs));
	} else {
		l2 = obs_module_text("Dock.NoEvent");
	}

	QString l3;
	if (noticeUntilNs_ > 0 && (int64_t)os_gettime_ns() < noticeUntilNs_) {
		l3 = QStringLiteral("⚠ ") + noticeText_;
	} else {
		const int64_t rel = (playheadNs_ > eventOriginNs_ &&
				     eventOriginNs_ > 0)
					    ? playheadNs_ - eventOriginNs_
					    : 0;
		l3 = QString("TC %1   %2%")
			     .arg(eventOriginNs_ > 0 ? shortTc(rel)
						     : QStringLiteral("--:--.--"))
			     .arg(speedPct_);
	}

	chanStrip_->setText(l1 + "\n" + l2 + "\n" + l3);
	if (chanBadge_)
		chanBadge_->setText(QString("A%1").arg(currentAngle1_));

	// The same information the reference controller prints ON the position bar: which event, where
	// the playhead is, at what speed.
	if (seek_) {
		const int64_t rel = (playheadNs_ > timelineStartNs_ &&
				     timelineStartNs_ > 0)
					    ? playheadNs_ - timelineStartNs_
					    : 0;
		QString ov;
		if (haveEv)
			ov = QString("%1 - ").arg(evId, idDigits, 10,
						  QLatin1Char('0'));
		ov += shortTc(rel) + QString("   %1%").arg(speedPct_);
		seek_->setOverlayText(ov);
	}
}

void MultiReplayDock::seekToFraction(double frac)
{
	if (timelineStartNs_ <= 0 || displayDurNs_ <= 0)
		return;
	frac = std::clamp(frac, 0.0, 1.0);
	const int64_t inNs =
		timelineStartNs_ + (int64_t)(frac * (double)displayDurNs_);
	const int64_t edge = timelineStartNs_ + displayDurNs_;
	const int64_t outNs = std::min(edge, inNs + kScrubReviewNs);
	if (outNs <= inNs)
		return;

	// Scrubbing is "review from here" (see kScrubReviewNs): the engine has no
	// playhead to park, it plays ranges. Stop the queue first so its own
	// finish callback cannot cut in over the review clip.
	PlaybackCoordinator::instance().stopEvents();
	// ...and consume that stop ourselves. poll() sends a finished SEQUENCE back
	// to the live edge, which is right when a replay ends and wrong here: it
	// would drag the operator off the very instant he just chose.
	prevSequenceActive_ = false;
	ReplayCore::instance().setFollowLive(false);
	// Where the timeline now stands, whatever the engine can serve: the bar
	// stays under the operator's finger instead of snapping back.
	playheadNs_ = inNs;

	std::string err;
	if (!ReplayChannel::instance().play(currentAngle1_ - 1, inNs, outNs,
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
	auto &chan = ReplayChannel::instance();
	auto &tap = PacketTap::instance();

	// The preview's obs_display is bound to a native window handle, and OBS
	// re-parents its docks whenever the layout is restored, floated, tabbed or
	// re-docked — which destroys that handle. Qt does not reliably tell the
	// widget, so this is the only place that can notice a display left
	// presenting into a window that no longer exists. Two integer compares.
	if (displayA_)
		displayA_->recheckWindow();

	// The hotkeys change the angle without going through the dock.
	const int hotAngle1 = core.currentAngle() + 1;
	if (hotAngle1 >= 1 && hotAngle1 <= kNCams)
		currentAngle1_ = hotAngle1;
	const int cam0 = currentAngle1_ - 1;

	const bool rec = core.isRecording();
	const bool followLive = core.followLive();
	const bool playing = chan.playing();
	const auto playSt = PlaybackCoordinator::instance().playState();
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
	const int64_t liveEdgeNs = tap.newestNs(cam0);
	int64_t startNs = SegmentIndex::instance().oldestNs(cam0);
	if (startNs <= 0)
		startNs = tap.oldestReplayableNs(cam0);
	timelineStartNs_ = startNs;
	displayDurNs_ = (startNs > 0 && liveEdgeNs > startNs)
				? liveEdgeNs - startNs
				: 0;
	// Anchored footage counts as content even with a dead live edge: that is
	// exactly a project reopened in a later OBS run (nothing captured yet, but
	// yesterday's files are on the timeline). Without startNs the preview would
	// stay black while the replay input was actually producing frames.
	previewHasContent_ = liveEdgeNs > 0 || startNs > 0;

	// Event times belong to the PROJECT, not to the angle being watched: the
	// earliest anchored recording on ANY camera is 0:00 for the table and for
	// the YouTube chapters. Reading it off the selected angle renumbered every
	// row when the operator pressed another camera button, and gave nothing at
	// all for an angle with no anchor — which is how a reopened project ended
	// up printing marks as raw monotonic time. The ring is the fallback for a
	// session that has not written a file yet.
	int64_t eventOrigin = SegmentIndex::instance().projectOriginNs();
	if (eventOrigin <= 0)
		eventOrigin = startNs;
	eventOriginNs_ = eventOrigin;

	// The event columns are drawn relative to that origin, so a moved origin
	// has to redraw them — it moves once for real, when the first anchored
	// recording replaces the ring's (constantly evicted) oldest instant. The
	// 1 s of slack is what keeps the ring's drift from rebuilding the table
	// thirty times a second.
	if (eventOriginNs_ > 0 &&
	    std::abs(eventOriginNs_ - tableOriginNs_) > 1'000'000'000LL) {
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
	const int64_t relPosNs = (playheadNs_ > startNs && startNs > 0)
					 ? playheadNs_ - startNs
					 : 0;

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
			PlaybackCoordinator::instance().defaultSpeedPct();
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
		mf.reserve(markerNs_.size());
		for (const auto &[inNs, outNs] : markerNs_) {
			double inf = std::clamp((double)(inNs - timelineStartNs_) /
							(double)displayDurNs_,
						0.0, 1.0);
			double outf = std::clamp((double)(outNs - timelineStartNs_) /
							 (double)displayDurNs_,
						 0.0, 1.0);
			if (outf > inf)
				mf.push_back({inf, outf});
		}
		seek_->setEventMarkers(std::move(mf));
	} else if (seek_) {
		seek_->setEventMarkers({});
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
	// selection and selectRow() below re-sets it, each of which would otherwise
	// emit selectionChanged → populateInspector(). We rebuild the inspector once,
	// explicitly, at the end instead (avoids a 2-3× teardown storm per refresh).
	QSignalBlocker selBlock(events_->selectionModel());
	events_->setRowCount(0);
	obs_data_array_t *arr = obs_data_get_array(d, "events");
	if (!arr) {
		refreshing_ = false;
		return;
	}
	const Qt::Alignment mid = Qt::AlignVCenter | Qt::AlignHCenter;
	std::vector<std::pair<int64_t, int64_t>> rawMarkers;

	// Marks are absolute instants on a monotonic clock that started with OBS,
	// so a column only means something relative to where this project's
	// footage begins. With NO footage there is no such origin, and printing
	// the raw instant produced the five-digit minute counts a reopened project
	// showed ("5648:09.557" for a mark taken four minutes into a take). A mark
	// we cannot place is shown as unplaceable.
	const int64_t originNs = eventOriginNs_;
	const bool haveOrigin = originNs > 0;
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
		double ownSpeed = -1.0;      // what this event sets, <0 = "--"
		double resolvedSpeed = -1.0; // inherited from the previous event
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
		if (tin >= 0 && tout > tin)
			rawMarkers.push_back({tin, tout});

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
		r.ownSpeed = obs_data_get_double(e, "speed");
		r.resolvedSpeed = obs_data_get_double(e, "resolvedSpeed");
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

		QString dur = r.tout >= 0
				      ? formatTc(r.tout - r.tin)
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
		const bool degenerate = r.tout >= 0 && r.tout - r.tin < 1'000'000;
		const bool playable =
			r.tin > 0 && !degenerate && footageExists(r.tin);
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
				 roItem(r.tout >= 0 ? relTc(r.tout)
						    : QStringLiteral("--"),
					mid));
		events_->setItem(row, kColDur, roItem(dur, mid));

		// Speed: what this event sets, or in parentheses what it inherited
		// from the event before it (the reference controller), or "--" when nobody set one and
		// the slider decides. The parentheses are the whole point — an
		// operator has to be able to see at a glance whether 50% is HIS or
		// something he is dragging along from three marks ago.
		QString speedText = QStringLiteral("--");
		if (r.ownSpeed >= 0)
			speedText = QString("%1%").arg((int)std::lround(
				r.ownSpeed * 100.0));
		else if (r.resolvedSpeed >= 0)
			speedText = QString("(%1%)").arg((int)std::lround(
				r.resolvedSpeed * 100.0));
		auto *speedItem = new QTableWidgetItem(speedText);
		speedItem->setTextAlignment(mid);
		speedItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled |
				    Qt::ItemIsEditable);
		speedItem->setToolTip(obs_module_text("Dock.SpeedHint"));
		speedItem->setData(Qt::UserRole, r.id);
		if (r.ownSpeed < 0)
			speedItem->setForeground(QBrush(QColor("#707070")));
		events_->setItem(row, kColSpeed, speedItem);

		// One cell per camera, exactly as the reference controller draws it: a tick box for
		// "play this angle" and the comment for that angle beside it,
		// both editable right there. This is the half of the event the
		// operator actually works on during a match — it used to live in
		// a panel under the table, which meant every angle change was a
		// click away from the row it belonged to.
		for (size_t ci = 0; ci < camCols_.size(); ci++) {
			const int cam = camCols_[ci];
			const QString note =
				r.camNotes[cam].empty()
					? kNoNote
					: QString::fromStdString(r.camNotes[cam]);
			auto *it = new QTableWidgetItem(note);
			it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled |
				     Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
			it->setCheckState(r.camOn[cam] ? Qt::Checked
						       : Qt::Unchecked);
			it->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
			it->setData(Qt::UserRole, r.id);
			// A per-angle speed has no column in the reference controller. Rather than
			// stuff it into the comment — where it would be typed
			// back into the note on the next edit — the cell is
			// tinted and says the number in its tooltip; the panel
			// under the table is where it is set.
			if (r.camSpeeds[cam] >= 0) {
				it->setToolTip(
					QString("%1 · %2%")
						.arg(obs_module_text(
							"Dock.AngleSpeedHint"))
						.arg((int)(r.camSpeeds[cam] * 100)));
				it->setForeground(QBrush(QColor("#ffd07a")));
			}
			events_->setItem(row, kColFirstCam + (int)ci, it);
		}
	}
	refreshing_ = false;
	markerNs_ = std::move(rawMarkers);
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
	// Rebuild the inspector for the selected event. Done explicitly because
	// selectRow() above does not emit selectionChanged when the same row was
	// already selected (e.g. after an inspector edit bumped the version).
	populateInspector(target);
}

void MultiReplayDock::populateInspector(int eventId)
{
	if (!inspector_ || !inspectorLayout_)
		return;

	// Don't tear the panel down (and steal focus) on a SAME-event refresh while
	// the user is editing one of its fields: their own commit bumps the store
	// version, which triggers refreshEvents() ~33ms later → a rebuild would
	// destroy the field they just tabbed into. Values are already persisted, so
	// skipping the rebuild is safe. A real selection change (different id) still
	// rebuilds (focus moves anyway).
	if (eventId == inspectorEventId_) {
		QWidget *fw = QApplication::focusWidget();
		if (fw && inspector_->isAncestorOf(fw))
			return;
	}
	inspectorEventId_ = eventId;

	// Tear down the previous rows.
	QLayoutItem *child;
	while ((child = inspectorLayout_->takeAt(0)) != nullptr) {
		if (child->widget())
			child->widget()->deleteLater();
		delete child;
	}

	auto addHint = [this](const char *key) {
		auto *hint = new QLabel(obs_module_text(key), inspector_);
		hint->setStyleSheet("color: #707070; font-style: italic;");
		inspectorLayout_->addWidget(hint);
	};

	ReplayEvent ev;
	if (eventId <= 0 || !EventStore::instance().get(eventId, ev)) {
		inspector_->setTitle(obs_module_text("Dock.AngleSpeeds"));
		addHint("Dock.SelectEvent");
		return;
	}
	// The toggle and the comment are also in the table (the reference controller), and they are
	// the same store fields — editing either writes the same value. What is
	// only here is the per-angle speed.
	inspector_->setTitle(QString("%1 — #%2")
				     .arg(obs_module_text("Dock.AngleSpeeds"))
				     .arg(eventId));

	Config cfg = ReplayCore::instance().getConfig();
	bool any = false;
	for (int i = 0; i < kEventAngles; i++) {
		// Only show rows for cameras that have a source configured.
		bool configured = (i < kMaxCameras) &&
				  !cfg.cameras[i].sourceName.empty();
		if (!configured)
			continue;
		any = true;
		int a1 = i + 1;

		const std::string &dn = cfg.cameras[i].displayName;
		QString label = dn.empty() ? QString("Cam %1").arg(a1)
					   : QString::fromStdString(dn);

		auto *rowW = new QWidget(inspector_);
		auto *row = new QHBoxLayout(rowW);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(6);

		// Enable toggle (the camera name doubles as the label).
		auto *chk = new QCheckBox(label, rowW);
		chk->setMinimumWidth(96);
		chk->setChecked(ev.angles[i].enabled);
		if (!dn.empty())
			chk->setToolTip(QString::fromStdString(dn));
		connect(chk, &QCheckBox::toggled, this, [eventId, a1](bool on) {
			EventStore::instance().setAngle(eventId, a1, on);
		});

		// Comment (free text). Commit on focus-out / Enter.
		auto *note = new QLineEdit(
			QString::fromStdString(ev.angles[i].note), rowW);
		note->setPlaceholderText(QStringLiteral("commento"));
		note->setToolTip(obs_module_text("Dock.CamNoteHint"));
		connect(note, &QLineEdit::editingFinished, this,
			[eventId, a1, note]() {
				EventStore::instance().setAngleNote(
					eventId, a1,
					note->text().trimmed().toStdString());
			});

		// Per-angle speed override (percent); empty = use default speed.
		auto *sp = new QLineEdit(rowW);
		sp->setPlaceholderText(QStringLiteral("vel%"));
		sp->setToolTip(obs_module_text("Dock.AngleSpeedHint"));
		sp->setFixedWidth(56);
		sp->setMaxLength(4);
		sp->setAlignment(Qt::AlignCenter);
		if (ev.angles[i].speed >= 0)
			sp->setText(QString::number(
				(int)(ev.angles[i].speed * 100)));
		connect(sp, &QLineEdit::editingFinished, this,
			[eventId, a1, sp]() {
				QString t = sp->text().trimmed();
				t.remove('%');
				bool ok = false;
				int v = t.toInt(&ok);
				if (!ok || t.isEmpty()) {
					EventStore::instance().setAngleSpeed(
						eventId, a1, -1.0);
					sp->clear();
				} else {
					v = std::clamp(v, 1, 100);
					EventStore::instance().setAngleSpeed(
						eventId, a1, v / 100.0);
					sp->setText(QString::number(v));
				}
			});

		row->addWidget(chk, 0);
		row->addWidget(note, 1);
		row->addWidget(sp, 0);
		inspectorLayout_->addWidget(rowW);
	}
	if (!any)
		addHint("Dock.NoCameras");
}

void MultiReplayDock::onEventItemChanged(QTableWidgetItem *item)
{
	// In/out/duration are read-only; what an operator can change is the event
	// speed and, in the per-camera columns, whether that angle plays and what
	// it is called. Everything else reaching here is a rebuild writing its own
	// cells, which must not be read back as an operator edit.
	if (refreshing_ || !item)
		return;
	const int col = item->column();
	if (col != kColSpeed && col < kColFirstCam)
		return;
	const int id = item->data(Qt::UserRole).toInt();
	if (id <= 0)
		return;

	if (col >= kColFirstCam) {
		const size_t ci = (size_t)(col - kColFirstCam);
		if (ci >= camCols_.size())
			return;
		const int a1 = camCols_[ci] + 1; // EventStore is 1-based
		// Both halves of the cell are written back on any change: the two
		// are one edit as far as the operator is concerned, and applying
		// both is idempotent — far cheaper than tracking which of the two
		// Qt actually moved.
		EventStore::instance().setAngle(id, a1,
						item->checkState() == Qt::Checked);
		QString note = item->text().trimmed();
		if (note == kNoNote) // the placeholder is not a comment
			note.clear();
		EventStore::instance().setAngleNote(id, a1, note.toStdString());
		// No refresh from here — see the note at the end of this function.
		return;
	}

	// Accept what an operator actually types: "50", "50%", " 50 ", and blank
	// or "--" for "no speed of my own" — which is what makes the event fall
	// back to the previous one's, exactly like the reference controller.
	QString t = item->text().trimmed();
	t.remove(QLatin1Char('%'));
	t.remove(QLatin1Char('('));
	t.remove(QLatin1Char(')'));
	bool ok = false;
	const int v = t.toInt(&ok);
	if (!ok || t.isEmpty() || t == QStringLiteral("--"))
		EventStore::instance().setSpeed(id, -1.0);
	else
		EventStore::instance().setSpeed(id, std::clamp(v, 1, 100) / 100.0);
	// No refresh from here: we are inside the model's own setData, and tearing
	// the rows down under it would free the item the view is still finishing
	// with. The store's version counter has just moved, so poll() rebuilds on
	// its next tick (~33 ms) on a clean stack — which is also what normalises
	// the text and re-parenthesises every event that inherits from this one.
}

// ---------------------------------------------------------------------------
// Event filter — double-click on note labels to edit
// ---------------------------------------------------------------------------

bool MultiReplayDock::eventFilter(QObject *watched, QEvent *event)
{
	// The inspector panel uses always-editable fields (commit on focus-out),
	// so no per-widget event filtering is needed anymore. Kept as a thin
	// override hook for future use.
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
	auto *form = new QFormLayout(&dlg);

	// session folder
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
	form->addRow(obs_module_text("Dock.SessionFolder"), folderRow);

	auto *split = new QSpinBox(&dlg);
	// 0 = never split: an ISO is normally one continuous file (see
	// branch_output::buildSettings). specialValueText replaces the number at
	// the minimum, so 0 reads as words instead of a meaningless "0 min".
	split->setRange(0, 240);
	split->setValue(cfg.splitMinutes);
	split->setSuffix(" min");
	split->setSpecialValueText(obs_module_text("Dock.SplitNever"));
	split->setToolTip(obs_module_text("Dock.SplitMinutesHint"));
	form->addRow(obs_module_text("Dock.SplitMinutes"), split);

	auto *vbr = new QSpinBox(&dlg);
	vbr->setRange(1000, 200000);
	vbr->setSingleStep(1000);
	vbr->setValue(cfg.videoBitrateKbps);
	vbr->setSuffix(" kbps");
	form->addRow(obs_module_text("Dock.VideoBitrate"), vbr);

	auto *abr = new QSpinBox(&dlg);
	abr->setRange(64, 1024);
	abr->setValue(cfg.audioBitrateKbps);
	abr->setSuffix(" kbps");
	form->addRow(obs_module_text("Dock.AudioBitrate"), abr);

	// --- the reference controller event options -----------------------------------------------
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
	form->addRow(obs_module_text("Dock.PreRoll"), preRoll);

	auto *postRoll = new QDoubleSpinBox(&dlg);
	postRoll->setRange(0.0, 30.0);
	postRoll->setSingleStep(0.5);
	postRoll->setDecimals(1);
	postRoll->setSuffix(" s");
	postRoll->setValue(cfg.postRollMs / 1000.0);
	postRoll->setToolTip(obs_module_text("Dock.PostRollHint"));
	form->addRow(obs_module_text("Dock.PostRoll"), postRoll);

	auto *sortByTime = new QCheckBox(&dlg);
	sortByTime->setChecked(cfg.sortEventsByTime);
	sortByTime->setToolTip(obs_module_text("Dock.SortByTimeHint"));
	form->addRow(obs_module_text("Dock.SortByTime"), sortByTime);

	auto *idDigits = new QSpinBox(&dlg);
	idDigits->setRange(1, 8);
	idDigits->setValue(cfg.eventIdDigits);
	idDigits->setToolTip(obs_module_text("Dock.IdDigitsHint"));
	form->addRow(obs_module_text("Dock.IdDigits"), idDigits);

	// Clip crossfade is gone with the A/B ffmpeg_source pair it belonged to:
	// there is a single replay input now, and a transition between clips is
	// the operator's own (OBS transitions on the scene that holds it).

	// encoder combo
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
				enc->addItem(
					QString::fromUtf8(obs_data_get_string(
						it, "name")),
					QString::fromUtf8(obs_data_get_string(
						it, "id")));
				obs_data_release(it);
			}
			obs_data_array_release(arr);
		}
	}
	{
		int idx = enc->findData(
			QString::fromStdString(cfg.videoEncoderId));
		if (idx >= 0)
			enc->setCurrentIndex(idx);
	}
	form->addRow(obs_module_text("Dock.Encoder"), enc);

	// gather source names once
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

	// No replay-source selector: "MultiReplay - Replay A" is a plugin-provided
	// OBS input the operator drops into whatever scene he likes, exactly like
	// a capture card. cfg.replaySourceName survives only for back-compat.
	//
	// The OUTPUT SCENE selector, however, is needed: PlaybackCoordinator only
	// takes program on "to output" when cfg.outputSceneName names a scene, and
	// with the picker gone (removed together with the plugin-managed scene) the
	// field stayed empty forever — so "to output" silently did nothing. The
	// operator has to tell us which of HIS scenes holds the replay input.
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
	form->addRow(obs_module_text("Dock.OutputScene"), outScene);

	auto *music = makeSourceCombo(cfg.musicSourceName);
	form->addRow(obs_module_text("Dock.MusicSource"), music);

	auto *autoSwitch = new QCheckBox(&dlg);
	autoSwitch->setChecked(cfg.autoSwitchScene);
	form->addRow(obs_module_text("Dock.AutoSwitch"), autoSwitch);

	// Scale the replay to the canvas. This is a scene-item transform, not a
	// picture change: the frames stay at the camera's own resolution and the
	// GPU scales them while compositing (see ReplayChannel::applyCanvasFit).
	auto *fitCanvas = new QCheckBox(&dlg);
	fitCanvas->setChecked(cfg.fitReplayToCanvas);
	fitCanvas->setToolTip(obs_module_text("Dock.FitCanvasHint"));
	form->addRow(obs_module_text("Dock.FitCanvas"), fitCanvas);

	// camera assignments: source + display name
	auto *camBox = new QGroupBox(obs_module_text("Dock.Cameras"), &dlg);
	auto *camForm = new QFormLayout(camBox);
	std::vector<QComboBox *> camCombos;
	std::vector<QLineEdit *> camNameEdits;
	for (int i = 0; i < kMaxCameras; i++) {
		auto *row = new QHBoxLayout();
		auto *c = makeSourceCombo(cfg.cameras[i].sourceName);
		auto *nameEdit = new QLineEdit(
			QString::fromStdString(cfg.cameras[i].displayName), &dlg);
		nameEdit->setPlaceholderText(
			QString(obs_module_text("Dock.CameraName"))
				.arg(i + 1));
		nameEdit->setFixedWidth(110);
		row->addWidget(c, 1);
		row->addWidget(nameEdit);
		camCombos.push_back(c);
		camNameEdits.push_back(nameEdit);
		camForm->addRow(QString("Cam %1").arg(i + 1), row);
	}
	form->addRow(camBox);

	auto *buttons = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	form->addRow(buttons);

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
	cfg.videoEncoderId = enc->currentData().toString().toStdString();
	cfg.outputSceneName = outScene->currentData().toString().toStdString();
	cfg.musicSourceName = music->currentData().toString().toStdString();
	cfg.autoSwitchScene = autoSwitch->isChecked();
	cfg.fitReplayToCanvas = fitCanvas->isChecked();
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
