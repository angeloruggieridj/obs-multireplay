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
#include <QPushButton>
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
#include <string>
#include <cstring>

namespace multireplay {

// ---------------------------------------------------------------------------
// Dock-wide stylesheet — professional broadcast control room palette.
// Reference: Grass Valley, Ross Video, Viz master control panels —
// near-black surface, neutral grey buttons, PVW green / PGM red for
// camera state, steel blue for the seekbar progress. No amber.
//
// Palette:
//   #0c0c0c  deep bg          #1c8a38  PVW green (preview/selected)
//   #141414  surface          #be2020  PGM red  (program/playing)
//   #1c1c1c  elevated         #081a0e  green bg dark
//   #222222  btn rest         #200808  red bg dark
//   #2c2c2c  border           #28b050  green text
//   #424242  border hover     #de3838  red text
//   #c0c0c0  text primary     #365e8a  steel-blue (seekbar)
//   #484848  text muted       #c02020  danger/rec red
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

/* ── camera toggle chips in event table ─────────────────── */
QToolButton#mrCamToggle {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 3px;
	color: #383838; font-size: 9px; font-weight: 700;
	min-width: 30px; max-width: 44px; min-height: 22px;
	padding: 1px 4px; text-align: center;
}
QToolButton#mrCamToggle:hover  { border-color: #424242; color: #585858; }
QToolButton#mrCamToggle[state="preview"] {
	background: #081a0e; border-color: #1c8a38; color: #28b050;
}
QToolButton#mrCamToggle[state="preview"]:hover { background: #0c2014; border-color: #22a040; }
QToolButton#mrCamToggle[state="program"] {
	background: #200808; border-color: #be2020; color: #de3838;
}
QToolButton#mrCamToggle[state="program"]:hover { background: #280c0c; border-color: #cc2828; }

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

/* ── event table ─────────────────────────────────────────── */
QTableWidget#mrEvents {
	background: #0c0c0c; alternate-background-color: #101010;
	gridline-color: transparent; border: 1px solid #1c1c1c;
	border-radius: 4px; color: #c0c0c0; outline: 0;
}
QTableWidget#mrEvents::item { padding: 3px 5px; border: 0; }
QTableWidget#mrEvents::item:selected { background: #1a2e52; color: #d0d8f0; }
QHeaderView::section {
	background: #141414; color: #909090;
	padding: 4px 5px; border: 0;
	border-bottom: 1px solid #2c2c2c;
	font-size: 9px; font-weight: 700; letter-spacing: 0.8px;
}
QTableCornerButton::section { background: #141414; border: 0; }

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

QSplitter::handle:horizontal {
	background: #1e1e1e; width: 5px;
}
QSplitter::handle:horizontal:hover { background: #2e2e2e; }
QSplitter::handle:vertical { background: transparent; }
)QSS";

namespace {

constexpr int kNCams = kMaxCameras; // 8

// Scrubbing means "review from here": the engine plays a range, it has no
// playhead to park. The window is capped because play() materialises every
// packet of the range in RAM, and an uncapped "from here to now" would be a
// multi-gigabyte copy on a long session.
constexpr int64_t kScrubReviewNs = 10'000'000'000LL; // 10 s

// How long a take gets to prove Branch Output really started it. Branch Output
// re-evaluates its start conditions once a second (plugin-main.cpp:
// TASK_INTERVAL_MS = 1000), so this is four of its ticks — enough for encoder
// creation on a slow adapter, short enough that the operator learns NOW rather
// than when the first replay comes up empty. See poll().
constexpr int64_t kArmWatchNs = 4'000'000'000LL; // 4 s

// Event table column layout.
// Note: per-camera descriptions are edited via right-click on camera chips.
enum EventCol {
	kColId = 0,
	kColIn,
	kColOut,
	kColDur,
	kColCams, // per-angle: [cam toggle] [comment] [speed%]
	kColCount
};

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

	const int m = 4;                       // horizontal margin
	const int h = 14;                      // track thickness
	const int y = (height() - h) / 2;
	const int w = width() - 2 * m;
	const double pos = dragging_ ? dragFrac_ : positionFrac_;

	// Track (dark base)
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x16, 0x16, 0x16));
	p.drawRoundedRect(QRectF(m, y, w, h), 2, 2);

	// Seekable band (slightly lighter — indexed footage extent)
	if (seekableFrac_ > 0.0) {
		p.setBrush(QColor(0x22, 0x22, 0x22));
		p.drawRoundedRect(QRectF(m, y, w * seekableFrac_, h), 2, 2);
	}

	// Event markers — alternating green / cyan.
	// Each marker: semi-transparent fill + bright left-edge line + dim right edge.
	static const QColor kFill[2] = {
		QColor(0x1c, 0x8a, 0x38, 0x70), // green
		QColor(0x1a, 0x72, 0x98, 0x70), // cyan
	};
	static const QColor kEdge[2] = {
		QColor(0x28, 0xb0, 0x50),        // bright green
		QColor(0x22, 0x9a, 0xc0),        // bright cyan
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

	// Position fill — steel blue gradient (drawn on top of markers,
	// blends because of partial alpha)
	if (pos > 0.0) {
		QRectF f(m, y, w * pos, h);
		QLinearGradient grad(f.left(), 0, f.right(), 0);
		grad.setColorAt(0.0, QColor(0x1c, 0x3c, 0x5c, 0xb0));
		grad.setColorAt(1.0, QColor(0x2a, 0x5e, 0x8a, 0xb0));
		p.setBrush(grad);
		p.drawRoundedRect(f, 2, 2);
	}

	// Playhead: vertical line through full track + circle handle
	const double hx = m + w * pos;
	const double r = (dragging_ || underMouse()) ? 7.5 : 6.0;
	p.setPen(QPen(QColor(0xe0, 0xe0, 0xe0, 0xc0), 1.5,
		      Qt::SolidLine, Qt::FlatCap));
	p.drawLine(QPointF(hx, y), QPointF(hx, y + h));
	p.setPen(QPen(QColor(0x28, 0x48, 0x60), 1.0));
	p.setBrush(QColor(0xe0, 0xe0, 0xe0));
	p.drawEllipse(QPointF(hx, height() / 2.0), r, r);
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
	// Live mirror: unless a clip is playing, render the live camera source of
	// the selected angle. That is what the reference controller shows, it is the only zero-latency
	// picture available (the replay input carries frames only while a clip is
	// being paced into it), and it is what the operator needs BEFORE the take
	// to check the angles are the right ones. poll() decides which of the two
	// this is and publishes the source name; here we only resolve it.
	obs_source_t *src = nullptr;
	auto *self = static_cast<MultiReplayDock *>(data);
	if (self && self->previewLive_.load()) {
		std::string name;
		{
			std::lock_guard<std::mutex> lk(self->previewMutex_);
			name = self->liveSourceName_;
		}
		// An unconfigured angle leaves the name empty, and a stale one
		// resolves to nothing: both fall through, they never reach the
		// render below with a dangling pointer.
		if (!name.empty())
			src = obs_get_source_by_name(name.c_str()); // add-ref'd
	}
	if (!src) {
		// Nothing captured yet: render black rather than whatever the
		// replay input last held.
		if (!self || !self->previewHasContent_.load())
			return;
		src = ReplayChannel::instance().acquireSource();
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
	setMinimumSize(700, 300);
	setStyleSheet(QString::fromUtf8(kDockStyle));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(5, 5, 5, 5);
	root->setSpacing(4);

	// Helper: 1px separator line (each call returns a fresh widget).
	auto mkSep = [this]() -> QWidget * {
		auto *s = new QWidget(this);
		s->setObjectName("mrSepLine");
		s->setFixedHeight(1);
		return s;
	};

	// ── HEADER: REC · status · ⚙ ─────────────────────────────────────
	{
		auto *bar = new QHBoxLayout();
		bar->setSpacing(6);
		recBtn_ = new QPushButton(QStringLiteral("●  REC"), this);
		recBtn_->setObjectName("mrRec");
		recBtn_->setProperty("recording", false);
		recBtn_->setMinimumWidth(92);
		recBtn_->setCursor(Qt::PointingHandCursor);
		bar->addWidget(recBtn_);

		statusLbl_ = new QLabel(this);
		statusLbl_->setObjectName("mrMuted");
		bar->addWidget(statusLbl_, 1);

		projectLbl_ = new QLabel(this);
		projectLbl_->setObjectName("mrMuted");
		projectLbl_->setStyleSheet(
			"color: #487898; font-size: 9px; padding: 0 4px;");
		projectLbl_->hide();
		bar->addWidget(projectLbl_);

		auto *gear = new QToolButton(this);
		gear->setObjectName("mrGear");
		gear->setText(QStringLiteral("⚙"));
		gear->setCursor(Qt::PointingHandCursor);
		gear->setToolTip(obs_module_text("Dock.Settings"));
		gear->setPopupMode(QToolButton::InstantPopup);
		{
			auto *menu = new QMenu(gear);
			auto *actNew = menu->addAction(
				obs_module_text("Dock.NewProject"));
			auto *actOpen = menu->addAction(
				obs_module_text("Dock.OpenProject"));
			menu->addSeparator();
			auto *actSettings = menu->addAction(
				obs_module_text("Dock.Settings"));
			menu->addSeparator();
			auto *actChapters = menu->addAction(
				obs_module_text("Dock.YouTubeChapters"));
			gear->setMenu(menu);
			connect(actNew, &QAction::triggered, this,
				&MultiReplayDock::newProjectDialog);
			connect(actOpen, &QAction::triggered, this,
				&MultiReplayDock::openProjectDialog);
			connect(actSettings, &QAction::triggered, this,
				&MultiReplayDock::openSettings);
			connect(actChapters, &QAction::triggered, this,
				&MultiReplayDock::copyYouTubeChapters);
		}
		bar->addWidget(gear);
		root->addLayout(bar);

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
	}
	root->addWidget(mkSep());

	// ── MAIN: resizable horizontal splitter ──────────────────────────
	{
		splitter_ = new QSplitter(Qt::Horizontal, this);
		splitter_->setChildrenCollapsible(false);
		splitter_->setHandleWidth(5);

		// Left panel: preview + angle selector + seekbar + transport
		auto *left = new QWidget(splitter_);
		left->setMinimumWidth(180);
		auto *lv = new QVBoxLayout(left);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(0);
		lv->addWidget(buildPreview(), 1);
		lv->addWidget(mkSep());
		lv->addWidget(buildTransport());
		splitter_->addWidget(left);

		// Right panel: event list (list selector, search, table, playback)
		splitter_->addWidget(buildEvents());
		splitter_->setStretchFactor(0, 1);
		splitter_->setStretchFactor(1, 2);
		root->addWidget(splitter_, 1);
	}

	// ── FOOTER: markers (left) + edit controls (right) ─────────────────
	{
		root->addWidget(mkSep());
		auto *footerBox = new QWidget(this);
		auto *fh = new QHBoxLayout(footerBox);
		fh->setContentsMargins(0, 0, 0, 0);
		fh->setSpacing(4);

		// Marker section (Live + IN/OUT + presets + Cancel)
		fh->addWidget(buildMarkers());
		fh->addStretch(1);

		// Edit controls (secondary — delete, duplicate, export)
		auto *dup = compactBtn(obs_module_text("Dock.Duplicate"), this);
		auto *del = compactBtn(obs_module_text("Dock.Delete"), this,
				       "mrDanger");
		auto *exp = compactBtn(obs_module_text("Dock.Export"), this);
		auto *delAll = compactBtn(obs_module_text("Dock.DeleteAll"), this,
					  "mrDanger");
		connect(dup, &QPushButton::clicked, this, [this]() {
			for (int id : selectedEventIds())
				EventStore::instance().duplicate(id);
			refreshEvents();
		});
		connect(del, &QPushButton::clicked, this, [this]() {
			for (int id : selectedEventIds())
				EventStore::instance().remove(id);
			refreshEvents();
		});
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
		connect(delAll, &QPushButton::clicked, this, [this]() {
			if (QMessageBox::question(
				    this, "obs-multireplay",
				    obs_module_text("Dock.DeleteAllConfirm"),
				    QMessageBox::Yes | QMessageBox::No,
				    QMessageBox::No) != QMessageBox::Yes)
				return;
			PlaybackCoordinator::instance().stopEvents();
			EventStore::instance().clearAll();
		});
		fh->addWidget(dup);
		fh->addWidget(del);
		fh->addWidget(exp);
		fh->addWidget(delAll);
		root->addWidget(footerBox);
	}

	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(33); // ~30 fps — smooth seekbar + responsive transport
	connect(pollTimer_, &QTimer::timeout, this, &MultiReplayDock::poll);
	pollTimer_->start();

	refreshAngles();
	refreshEvents();
	poll();
}

MultiReplayDock::~MultiReplayDock() = default;

// ---------------------------------------------------------------------------
// Single A preview + angle selector
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildPreview()
{
	auto *box = new QWidget(this);
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(3);

	displayA_ = new OBSQTDisplay(this);
	displayA_->setRenderCallback(&MultiReplayDock::drawChannelA, this);
	displayA_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	displayA_->setMinimumHeight(40);
	v->addWidget(displayA_, 1);

	// angle selector row (cam 1..N) — segmented control
	auto *row = new QWidget(box);
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(4);
	auto *lbl = new QLabel(
		QString::fromUtf8(obs_module_text("Dock.Angle")).toUpper(), row);
	lbl->setObjectName("mrSectionLabel");
	h->addWidget(lbl);
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
	h->addStretch(1);
	connect(anglesA_, &QButtonGroup::idClicked, this,
		[this](int id) { setAngle(id); });
	v->addWidget(row);

	return box;
}

// ---------------------------------------------------------------------------
// Transport: seekbar + timecode + buttons + speed
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildTransport()
{
	auto *box = new QWidget(this);
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(4, 4, 4, 4);
	v->setSpacing(4);

	// Row 1: seekbar (full width)
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

	// Row 2: [◀◀][▶][▶▶] [NOW]  ──stretch──  timecode
	// Timecode is right-justified, transport buttons left-justified.
	// One row instead of two (saves ~18px of height).
	auto *tr = new QHBoxLayout();
	tr->setSpacing(4);

	// ▶ U+25B6
	playPauseBtn_ = transportBtn(QStringLiteral("▶"), this,
				     obs_module_text("Dock.PlayPause"), "mrPlay");
	nowBtn_ = new QPushButton(QStringLiteral("NOW"), this);
	nowBtn_->setObjectName("mrNow");
	nowBtn_->setProperty("live", false);
	nowBtn_->setCursor(Qt::PointingHandCursor);
	nowBtn_->setToolTip(obs_module_text("Dock.JumpToNow"));
	nowBtn_->setMinimumWidth(38);

	tr->addWidget(playPauseBtn_);
	tr->addWidget(nowBtn_);
	tr->addStretch(1);

	tcLbl_ = new QLabel(QStringLiteral("00:00.000 / 00:00.000"), this);
	tcLbl_->setObjectName("mrTimecode");
	tcLbl_->setFont(QFont(monoFamily()));
	tcLbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	tr->addWidget(tcLbl_);
	v->addLayout(tr);

	// Row 3: speed chips [25%][50%][75%][1×] + slider + value label
	// HTML-style discrete quick-access buttons for common replay speeds.
	auto *sh = new QHBoxLayout();
	sh->setSpacing(3);

	const std::pair<int, const char *> speedPresets[] = {
		{25, "25%"}, {50, "50%"}, {75, "75%"}, {100, "1\xc3\x97"}};
	for (const auto &[pct, lbl] : speedPresets) {
		int p = pct; // copy: capturing a structured binding is non-portable
		auto *b = compactBtn(QString::fromUtf8(lbl), this, "mrSpeedChip");
		connect(b, &QPushButton::clicked, this, [this, p]() {
			speed_->blockSignals(true);
			speed_->setValue(p);
			speed_->blockSignals(false);
			speedLbl_->setText(
				QString::asprintf("%.2f\xc3\x97", p / 100.0));
			applyReplaySpeed(p);
		});
		sh->addWidget(b);
	}

	speed_ = new QSlider(Qt::Horizontal, this);
	speed_->setObjectName("mrSpeed");
	speed_->setRange(5, 100);
	speed_->setValue(100);
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

	sh->addWidget(speed_, 1);
	sh->addWidget(speedLbl_);
	v->addLayout(sh);

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
// Markers: Live/Recorded + IN/OUT + presets
// ---------------------------------------------------------------------------

QWidget *MultiReplayDock::buildMarkers()
{
	auto *box = new QWidget(this);
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(4, 3, 4, 3);
	h->setSpacing(4);

	liveChk_ = new QCheckBox(obs_module_text("Dock.LiveMode"), this);
	liveChk_->setChecked(EventStore::instance().liveMode());
	liveChk_->setCursor(Qt::PointingHandCursor);
	connect(liveChk_, &QCheckBox::toggled, this,
		[](bool on) { EventStore::instance().setLiveMode(on); });
	h->addWidget(liveChk_);

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
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 4, 0, 0);
	v->setSpacing(3);

	auto *top = new QHBoxLayout();
	top->setSpacing(5);
	auto *listLbl = new QLabel(
		QString::fromUtf8(obs_module_text("Dock.List")).toUpper(), this);
	listLbl->setObjectName("mrSectionLabel");
	top->addWidget(listLbl);
	listCombo_ = new QComboBox(this);
	listCombo_->setFixedWidth(56);
	for (int i = 1; i <= kEventLists; i++)
		listCombo_->addItem(QString::number(i));
	listCombo_->setCurrentIndex(EventStore::instance().selectedList() - 1);
	connect(listCombo_, &QComboBox::currentIndexChanged, this,
		[this](int idx) {
			EventStore::instance().selectList(idx + 1);
			refreshEvents();
		});
	top->addWidget(listCombo_);

	search_ = new QLineEdit(this);
	search_->setPlaceholderText(obs_module_text("Dock.Search"));
	search_->setClearButtonEnabled(true);
	connect(search_, &QLineEdit::textChanged, this,
		[this](const QString &) { refreshEvents(); });
	top->addWidget(search_, 1);
	v->addLayout(top);

	events_ = new QTableWidget(this);
	events_->setObjectName("mrEvents");
	events_->setColumnCount(kColCount);
	events_->setHorizontalHeaderLabels(
		{"#", obs_module_text("Dock.In"), obs_module_text("Dock.Out"),
		 obs_module_text("Dock.Duration"),
		 obs_module_text("Dock.AnglesHeader")});
	events_->setSelectionBehavior(QAbstractItemView::SelectRows);
	events_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// Speed cell (4) is edited in place; camera cell (5) is an inline
	// widget. Right-click on a camera chip edits that angle's description.
	events_->setEditTriggers(QAbstractItemView::DoubleClicked |
				 QAbstractItemView::EditKeyPressed);
	events_->verticalHeader()->setVisible(false);
	events_->verticalHeader()->setDefaultSectionSize(24);
	events_->setAlternatingRowColors(true);
	events_->setShowGrid(false);
	events_->setWordWrap(false);
	events_->setFrameShape(QFrame::NoFrame);
	events_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	{
		QHeaderView *hh = events_->horizontalHeader();
		hh->setHighlightSections(false);
		for (int c = 0; c < kColCount; c++)
			hh->setSectionResizeMode(c,
						 QHeaderView::ResizeToContents);
		hh->setSectionResizeMode(kColCams, QHeaderView::Stretch);
		hh->setMinimumSectionSize(34);
	}
	connect(events_, &QTableWidget::itemChanged, this,
		&MultiReplayDock::onEventItemChanged);
	v->addWidget(events_, 1);

	// Inspector panel: per-angle toggle · comment · vel% for the selected event.
	// Rebuilt on every selection change (and after a refresh) by
	// populateInspector(). Keeps the table itself compact (it only shows an
	// enabled-angles summary now).
	inspector_ = new QGroupBox(obs_module_text("Dock.AnglesHeader"), this);
	inspector_->setObjectName("mrInspector");
	inspectorLayout_ = new QVBoxLayout(inspector_);
	inspectorLayout_->setContentsMargins(6, 4, 6, 4);
	inspectorLayout_->setSpacing(2);
	v->addWidget(inspector_, 0);
	connect(events_->selectionModel(),
		&QItemSelectionModel::selectionChanged, this, [this]() {
			auto ids = selectedEventIds();
			populateInspector(ids.empty() ? 0 : ids.front());
		});

	// playback controls
	auto *pb = new QHBoxLayout();
	pb->setSpacing(3);
	auto *playSel = compactBtn(obs_module_text("Dock.PlaySelected"), this,
				   "mrAccent");
	auto *playLast = compactBtn(obs_module_text("Dock.PlayLast"), this);
	auto *stop = compactBtn(obs_module_text("Dock.Stop"), this, "mrDanger");
	connect(playSel, &QPushButton::clicked, this, [this]() {
		std::string err;
		if (!PlaybackCoordinator::instance().playEvents(
			    selectedEventIds(), currentAngle1_ - 1,
			    toOutputChk_->isChecked(), err))
			QMessageBox::warning(this, "obs-multireplay",
					     QString::fromStdString(err));
	});
	connect(playLast, &QPushButton::clicked, this, [this]() {
		std::string err;
		if (!PlaybackCoordinator::instance().playLastEvent(
			    currentAngle1_ - 1, toOutputChk_->isChecked(), err))
			QMessageBox::warning(this, "obs-multireplay",
					     QString::fromStdString(err));
	});
	connect(stop, &QPushButton::clicked, this,
		[]() { PlaybackCoordinator::instance().stopEvents(); });
	pb->addWidget(playSel);
	pb->addWidget(playLast);
	pb->addWidget(stop);
	pb->addStretch(1);

	toOutputChk_ = new QCheckBox(obs_module_text("Dock.ToOutput"), this);
	loopChk_ = new QCheckBox(obs_module_text("Dock.Loop"), this);
	musicChk_ = new QCheckBox(obs_module_text("Dock.Music"), this);
	connect(loopChk_, &QCheckBox::toggled, this,
		[](bool on) { PlaybackCoordinator::instance().setLoop(on); });
	connect(musicChk_, &QCheckBox::toggled, this, [](bool on) {
		PlaybackCoordinator::instance().setMusicEnabled(on);
	});
	pb->addWidget(toOutputChk_);
	pb->addWidget(loopChk_);
	pb->addWidget(musicChk_);
	v->addLayout(pb);

	return box;
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
	// Reviewing: the last frame the replay actually put on screen.
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

void MultiReplayDock::applyReplaySpeed(int pct)
{
	// Default speed for every angle without an override — the coordinator
	// resolves it when it builds the queue, including for the hotkeys.
	speedPct_ = std::clamp(pct, 5, 100);
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
	bool toOut = toOutputChk_ && toOutputChk_->isChecked();
	int a0 = currentAngle1_ - 1;
	if (!ids.empty())
		pc.playEvents(ids, a0, toOut, err);
	else
		pc.playLastEvent(a0, toOut, err);
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
	ReplayCore::instance().setFollowLive(false);
	std::string err;
	if (!ReplayChannel::instance().play(currentAngle1_ - 1, inNs, outNs,
					    speedPct_, err))
		MR_DLOG("[dock] scrub review unavailable: %s", err.c_str());
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

	// The hotkeys change the angle without going through the dock.
	const int hotAngle1 = core.currentAngle() + 1;
	if (hotAngle1 >= 1 && hotAngle1 <= kNCams)
		currentAngle1_ = hotAngle1;
	const int cam0 = currentAngle1_ - 1;

	const bool rec = core.isRecording();
	const bool followLive = core.followLive();
	const bool playing = chan.playing();
	const bool eventActive =
		PlaybackCoordinator::instance().playState().active;

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
	previewHasContent_.store(liveEdgeNs > 0 || startNs > 0);

	// The event columns are drawn relative to that origin, so a moved origin
	// has to redraw them — it moves once for real, when the first anchored
	// recording replaces the ring's (constantly evicted) oldest instant. The
	// 1 s of slack is what keeps the ring's drift from rebuilding the table
	// thirty times a second.
	if (timelineStartNs_ > 0 &&
	    std::abs(timelineStartNs_ - tableOriginNs_) > 1'000'000'000LL) {
		tableOriginNs_ = timelineStartNs_;
		refreshEvents();
	}

	// Everything inside that window is playable (ring or files), so unlike the
	// file-tailing engine there is no trailing "not yet flushed" region.
	const int64_t posNs = chan.positionNs();
	const int64_t relPosNs =
		(posNs > startNs && startNs > 0) ? posNs - startNs : 0;

	if (!seekDragging_) {
		if (rec && followLive) {
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
	// The speed slider is the dock's own state (the engine has none), so it is
	// never written back here — only read when a clip is queued.

	// Angle buttons: PVW green = selected, PGM red = event playing on it.
	// Visual state is driven by the "state" property + QSS, not :checked.
	if (anglesA_) {
		bool ep = eventActive && playing;
		for (int i = 1; i <= kNCams; i++) {
			auto *b = qobject_cast<QPushButton *>(
				anglesA_->button(i));
			if (!b || !b->isVisible())
				continue;
			QString st = (ep && i == currentAngle1_)
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

	// Highlight the angles-summary cell of the event currently playing (PGM
	// red); others use the default colour. Per-angle editing/state now lives
	// in the inspector panel, so the table only needs this row-level cue.
	{
		auto ps = PlaybackCoordinator::instance().playState();
		for (int row = 0; row < events_->rowCount(); row++) {
			QTableWidgetItem *idItem = events_->item(row, kColId);
			QTableWidgetItem *camItem = events_->item(row, kColCams);
			if (!idItem || !camItem)
				continue;
			int rowEv = idItem->data(Qt::UserRole).toInt();
			bool isActive = ps.active && (ps.eventId == rowEv);
			if (camItem->data(Qt::UserRole + 1).toBool() != isActive) {
				camItem->setData(Qt::UserRole + 1, isActive);
				camItem->setForeground(
					isActive ? QBrush(QColor("#e0604a"))
						 : QBrush());
			}
		}
	}

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
	// cameras up BEFORE the take, so it mirrors the live source whenever
	// nothing else is on it — recording or not, ever started or not. The
	// replay input only carries pictures while a clip is being paced into it,
	// so it wins for exactly as long as one is playing.
	//
	// Resolving the source NAME happens here, on the UI thread; the graphics
	// thread only looks it up and releases its own reference (drawChannelA).
	bool live = !playing;
	std::string liveName;
	if (live)
		liveName = core.getConfig().cameras[cam0].sourceName;
	{
		std::lock_guard<std::mutex> lk(previewMutex_);
		liveSourceName_ = liveName;
	}
	previewLive_.store(live && !liveName.empty());

	recBtn_->setText(rec ? QStringLiteral("◼  STOP")
			     : QStringLiteral("●  REC"));
	if (recBtn_->property("recording").toBool() != rec) {
		recBtn_->setProperty("recording", rec);
		repolish(recBtn_);
	}

	// Sync live checkbox with engine state (startRecording sets liveMode
	// internally without going through the checkbox).
	bool lm = EventStore::instance().liveMode();
	if (liveChk_ && liveChk_->isChecked() != lm) {
		liveChk_->blockSignals(true);
		liveChk_->setChecked(lm);
		liveChk_->blockSignals(false);
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
	constexpr int kStatusEveryNTicks = 8; // ~264 ms at 33 ms/tick
	const bool refreshStatus = (statusTick_++ % kStatusEveryNTicks) == 0;

	Data st(refreshStatus ? core.statusJson() : std::string());
	if (st) {
		QString ver = obs_data_get_string(st, "version");
		int64_t mins = obs_data_get_int(st, "estimatedMinutesRemaining");
		bool boOk = obs_data_get_bool(st, "branchOutputAvailable");
		QString s = QString("v%1 • %2")
				    .arg(ver)
				    .arg(rec ? obs_module_text("Dock.Recording")
					     : obs_module_text("Dock.Idle"));
		if (!boOk)
			s += QStringLiteral("  ⚠ Branch Output");
		else if (mins >= 0)
			s += QString("  • ~%1 min").arg(mins);
		statusLbl_->setText(s);
	}

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

	// Marks are absolute master-timeline instants; the columns show them
	// relative to the same origin the seekbar uses, so the two agree.
	const int64_t originNs = timelineStartNs_;
	auto relTc = [originNs](int64_t ns) {
		return formatTc(ns > originNs ? ns - originNs : 0);
	};

	size_t n = obs_data_array_count(arr);
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

		QString dur = tout >= 0 ? formatTc(tout - tin)
					: QString::fromUtf8(
						  obs_module_text("Dock.Open"));

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

		int row = events_->rowCount();
		events_->insertRow(row);

		auto roItem = [](const QString &txt, Qt::Alignment al) {
			auto *it = new QTableWidgetItem(txt);
			it->setTextAlignment(al);
			it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
			return it;
		};

		auto *idItem = roItem(QString::number(id), Qt::AlignCenter);
		idItem->setData(Qt::UserRole, id);
		events_->setItem(row, kColId, idItem);
		events_->setItem(row, kColIn, roItem(relTc(tin), mid));
		events_->setItem(row, kColOut,
				 roItem(tout >= 0 ? relTc(tout)
						  : QStringLiteral("--"),
					mid));
		events_->setItem(row, kColDur, roItem(dur, mid));

		// Compact summary of enabled angles (full editing is in the
		// inspector panel below): "1  3·50%  5✎". A trailing ✎ marks a
		// per-angle comment; ·NN% marks a per-angle speed override.
		QString summary;
		for (int k = 0; k < kEventAngles; k++) {
			if (!camOn[k])
				continue;
			QString tok = QString::number(k + 1);
			if (camSpeeds[k] >= 0)
				tok += QString("·%1%").arg(
					(int)(camSpeeds[k] * 100));
			if (!camNotes[k].empty())
				tok += QStringLiteral("✎");
			if (!summary.isEmpty())
				summary += QStringLiteral("  ");
			summary += tok;
		}
		auto *camItem = roItem(summary, Qt::AlignVCenter | Qt::AlignLeft);
		events_->setItem(row, kColCams, camItem);

		obs_data_release(e);
	}
	obs_data_array_release(arr);
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
		inspector_->setTitle(obs_module_text("Dock.AnglesHeader"));
		addHint("Dock.SelectEvent");
		return;
	}
	inspector_->setTitle(QString("%1 — #%2")
				     .arg(obs_module_text("Dock.AnglesHeader"))
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
	// No editable text columns remain (speed is per-angle in the camera cell,
	// in/out/duration are read-only). Kept as a no-op hook for future columns.
	(void)item;
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
	split->setRange(1, 240);
	split->setValue(cfg.splitMinutes);
	split->setSuffix(" min");
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
	cfg.videoEncoderId = enc->currentData().toString().toStdString();
	cfg.outputSceneName = outScene->currentData().toString().toStdString();
	cfg.musicSourceName = music->currentData().toString().toStdString();
	cfg.autoSwitchScene = autoSwitch->isChecked();
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
	refreshEvents();
	refreshAngles();
	poll();
}

void MultiReplayDock::copyYouTubeChapters()
{
	int list = EventStore::instance().selectedList();
	// Chapter 0:00 is the start of the timeline the dock is showing, which is
	// the oldest instant still replayable — the same origin as the seekbar.
	std::string text =
		EventStore::instance().chaptersText(list, timelineStartNs_);
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
