/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multireplay-dock.hpp"
#include "qt-display.hpp"
#include "replay-core.hpp"
#include "media-replay.hpp"
#include "event-store.hpp"
#include "playback-coordinator.hpp"
#include "export.hpp"
#include "session-index.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/platform.h>

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
#include <QHeaderView>
#include <QButtonGroup>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QMessageBox>
#include <QDockWidget>
#include <QSizePolicy>
#include <QStyle>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QFontDatabase>
#include <QInputDialog>

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
QLabel#mrCamNote    { color: #909090; font-size: 9px; }

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

constexpr int kNCams = kIndexMaxCameras; // 8

// Event table column layout.
// Note: per-camera descriptions are edited via right-click on camera chips.
enum EventCol {
	kColId = 0,
	kColIn,
	kColOut,
	kColDur,
	kColSpeed,
	kColCams,
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
		: d(obs_data_create_from_json(json.c_str()))
	{
	}
	~Data()
	{
		if (d)
			obs_data_release(d);
	}
	operator obs_data_t *() const { return d; }
};

bool ensureSession()
{
	auto &engine = MediaReplay::instance();
	if (engine.sessionLoaded())
		return true;
	std::string err;
	return engine.loadSession(ReplayCore::instance().getConfig().sessionFolder,
				  err);
}

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
	dragFrac_ = fracAt(e->pos().x());
	emit scrubStateChanged(true);
	emit scrubMoved(dragFrac_);
	update();
}

void SeekBar::mouseMoveEvent(QMouseEvent *e)
{
	if (!dragging_)
		return;
	dragFrac_ = fracAt(e->pos().x());
	emit scrubMoved(dragFrac_);
	update();
}

void SeekBar::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !dragging_)
		return;
	dragging_ = false;
	dragFrac_ = fracAt(e->pos().x());
	positionFrac_ = dragFrac_;
	emit seekRequested(dragFrac_);
	emit scrubStateChanged(false);
	update();
}

// ---------------------------------------------------------------------------
// Preview render callback (runs on the OBS graphics thread)
// ---------------------------------------------------------------------------

void MultiReplayDock::drawChannelA(void *, uint32_t cx, uint32_t cy)
{
	obs_source_t *src = MediaReplay::instance().acquireSource();
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

		auto *gear = new QToolButton(this);
		gear->setObjectName("mrGear");
		gear->setText(QStringLiteral("⚙"));
		gear->setCursor(Qt::PointingHandCursor);
		gear->setToolTip(obs_module_text("Dock.Settings"));
		bar->addWidget(gear);
		connect(gear, &QToolButton::clicked, this,
			&MultiReplayDock::openSettings);
		root->addLayout(bar);

		connect(recBtn_, &QPushButton::clicked, this, [this]() {
			auto &core = ReplayCore::instance();
			if (core.isRecording()) {
				core.stopRecording();
				std::string err;
				MediaReplay::instance().loadSession(
					core.getConfig().sessionFolder, err);
			} else {
				// Stop any event playing BEFORE starting a new
				// recording. startRecording() calls clearSession()
				// which resets eventActive_/onDone but does not
				// call stopEvents() — that would deadlock because
				// startRecording holds ReplayCore::mutex_ while
				// stopEvents would acquire MediaReplay::mutex_ in
				// an order that conflicts with the monitor thread.
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
		tcLbl_->setText(formatTc((int64_t)(frac * (double)durationNs_)) +
				" / " + formatTc(durationNs_));
	});
	connect(seek_, &SeekBar::seekRequested, this,
		[this](double frac) { seekToFraction(frac); });
	v->addWidget(seek_);

	// Row 2: [◀◀][▶][▶▶] [NOW]  ──stretch──  timecode
	// Timecode is right-justified, transport buttons left-justified.
	// One row instead of two (saves ~18px of height).
	auto *tr = new QHBoxLayout();
	tr->setSpacing(4);

	// ⏮ U+23EE  ▶ U+25B6  ⏭ U+23ED
	auto *stepBack = transportBtn(QStringLiteral("⏮"), this,
				      obs_module_text("Dock.StepBack"));
	playPauseBtn_ = transportBtn(QStringLiteral("▶"), this,
				     obs_module_text("Dock.PlayPause"), "mrPlay");
	auto *stepFwd = transportBtn(QStringLiteral("⏭"), this,
				     obs_module_text("Dock.StepFwd"));
	nowBtn_ = new QPushButton(QStringLiteral("NOW"), this);
	nowBtn_->setObjectName("mrNow");
	nowBtn_->setProperty("live", false);
	nowBtn_->setCursor(Qt::PointingHandCursor);
	nowBtn_->setToolTip(obs_module_text("Dock.JumpToNow"));
	nowBtn_->setMinimumWidth(38);

	tr->addWidget(stepBack);
	tr->addWidget(playPauseBtn_);
	tr->addWidget(stepFwd);
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
		auto *b = compactBtn(QString::fromUtf8(lbl), this, "mrSpeedChip");
		connect(b, &QPushButton::clicked, this, [this, pct]() {
			speed_->blockSignals(true);
			speed_->setValue(pct);
			speed_->blockSignals(false);
			speedLbl_->setText(
				QString::asprintf("%.2f\xc3\x97", pct / 100.0));
			MediaReplay::instance().setSpeed(pct / 100.0);
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
	connect(speed_, &QSlider::sliderReleased, this, [this]() {
		MediaReplay::instance().setSpeed(speed_->value() / 100.0);
	});

	sh->addWidget(speed_, 1);
	sh->addWidget(speedLbl_);
	v->addLayout(sh);

	// wire transport actions
	connect(stepBack, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		MediaReplay::instance().setFollowLive(false);
		MediaReplay::instance().stepFrames(-1);
	});
	connect(stepFwd, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		MediaReplay::instance().setFollowLive(false);
		MediaReplay::instance().stepFrames(1);
	});
	connect(playPauseBtn_, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		auto &engine = MediaReplay::instance();
		if (!engine.playing())
			engine.setFollowLive(false);
		engine.togglePlay();
	});
	connect(nowBtn_, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		MediaReplay::instance().jumpToEnd();
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
		EventStore::instance().markIn(markTimeNs());
		refreshEvents();
	});
	connect(out, &QPushButton::clicked, this, [this]() {
		if (!EventStore::instance().markOut(markTimeNs()))
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
			EventStore::instance().markInOut(markTimeNs(), sec);
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
		 obs_module_text("Dock.Duration"), obs_module_text("Dock.Speed"),
		 obs_module_text("Dock.Cameras")});
	events_->setSelectionBehavior(QAbstractItemView::SelectRows);
	events_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// Speed cell (4) is edited in place; camera cell (5) is an inline
	// widget. Right-click on a camera chip edits that angle's description.
	events_->setEditTriggers(QAbstractItemView::DoubleClicked |
				 QAbstractItemView::EditKeyPressed);
	events_->verticalHeader()->setVisible(false);
	events_->verticalHeader()->setDefaultSectionSize(38);
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

	// playback controls
	auto *pb = new QHBoxLayout();
	pb->setSpacing(3);
	auto *playSel = compactBtn(obs_module_text("Dock.PlaySelected"), this,
				   "mrAccent");
	auto *playLast = compactBtn(obs_module_text("Dock.PlayLast"), this);
	auto *stop = compactBtn(obs_module_text("Dock.Stop"), this, "mrDanger");
	connect(playSel, &QPushButton::clicked, this, [this]() {
		std::string err;
		if (ensureSession() &&
		    !PlaybackCoordinator::instance().playEvents(
			    selectedEventIds(), toOutputChk_->isChecked(), err))
			QMessageBox::warning(this, "obs-multireplay",
					     QString::fromStdString(err));
	});
	connect(playLast, &QPushButton::clicked, this, [this]() {
		std::string err;
		if (ensureSession() &&
		    !PlaybackCoordinator::instance().playLastEvent(
			    toOutputChk_->isChecked(), err))
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
	auto &store = EventStore::instance();
	if (store.liveMode()) {
		auto &core = ReplayCore::instance();
		if (core.isRecording() && core.sessionMonoStartNs() > 0) {
			// Files still open: use elapsed monotonic time from REC
			// start. SessionIndex origins align with this value once
			// files are flushed (both use os_gettime_ns).
			int64_t elapsed = (int64_t)os_gettime_ns() -
					  core.sessionMonoStartNs();
			return std::max<int64_t>(0, elapsed);
		}
		// Not recording (scrub/review mode): use indexed footage length.
		int64_t edge = MediaReplay::instance().footageDurationNs();
		if (edge > 0)
			return edge;
	}
	return MediaReplay::instance().position();
}

std::vector<int> MultiReplayDock::selectedEventIds() const
{
	std::vector<int> ids;
	auto rows = events_->selectionModel()->selectedRows();
	for (const auto &idx : rows) {
		QTableWidgetItem *it = events_->item(idx.row(), 0);
		if (it)
			ids.push_back(it->data(Qt::UserRole).toInt());
	}
	return ids;
}

void MultiReplayDock::setAngle(int angle1Based)
{
	if (angle1Based >= 1 && angle1Based <= kIndexMaxCameras)
		MediaReplay::instance().setAngle(angle1Based - 1);
}

void MultiReplayDock::seekToFraction(double frac)
{
	if (!ensureSession())
		return;
	frac = std::clamp(frac, 0.0, 1.0);
	int64_t pos = (int64_t)(frac * (double)durationNs_);
	if (seekableNs_ > 0 && pos > seekableNs_)
		pos = seekableNs_;
	auto &engine = MediaReplay::instance();
	engine.setFollowLive(false);
	engine.seekMaster(pos);
}

// ---------------------------------------------------------------------------
// Periodic refresh
// ---------------------------------------------------------------------------

void MultiReplayDock::poll()
{
	auto &core = ReplayCore::instance();
	auto &engine = MediaReplay::instance();

	// Keep the index fresh: pick up completed segments while recording, or
	// lazily load the session the first time a folder is configured. Done
	// ~every 2s (60 ticks at 33ms) so the seekbar grows during a take.
	if (++pollTick_ % 60 == 0) {
		if (engine.sessionLoaded()) {
			if (core.isRecording())
				engine.refreshSession();
		} else if (!core.isRecording() &&
			   !core.getConfig().sessionFolder.empty() &&
			   pollTick_ >= 30) {
			// Brief startup delay (~1s at 33ms/tick) lets OBS finish
			// FINISHED_LOADING and ensureSource() before we drive the
			// media source. No longer needs 5s: hw_decode=false removes
			// the D3D11VA contention that required the long guard.
			std::string err;
			engine.loadSession(core.getConfig().sessionFolder, err);
		}
	}

	// --- transport ---
	auto ts = engine.transportState();
	seekableNs_ = ts.seekableNs;
	durationNs_ = ts.durationNs;

	// During live recording the session index isn't flushed yet, so
	// durationNs == 0. Compute wall-clock elapsed from session start so
	// the timecode and seekbar grow from the moment REC is pressed.
	int64_t liveElapsedNs = 0;
	if (ts.recording) {
		int64_t t0 = core.sessionMonoStartNs();
		if (t0 > 0)
			liveElapsedNs = std::max<int64_t>(
				0, (int64_t)os_gettime_ns() - t0);
	}
	// Display duration: prefer indexed footage; fall back to elapsed.
	int64_t displayDurNs = std::max(ts.durationNs, liveElapsedNs);

	if (!seekDragging_) {
		// During recording keep the playhead at the live edge (right).
		double posFrac = ts.recording
					 ? 1.0
					 : (displayDurNs > 0
						    ? (double)ts.positionNs /
							      (double)displayDurNs
						    : 0.0);
		double seekFrac = (displayDurNs > 0 && ts.seekableNs > 0)
					  ? (double)ts.seekableNs /
						    (double)displayDurNs
					  : (ts.recording ? 0.0 : 1.0);
		seek_->setProgress(posFrac, seekFrac);
		if (ts.recording)
			tcLbl_->setText(QStringLiteral("● ") +
					formatTc(liveElapsedNs));
		else
			tcLbl_->setText(formatTc(ts.positionNs) + " / " +
					formatTc(displayDurNs));
	}

	// ⏸ U+23F8  ▶ U+25B6
	playPauseBtn_->setText(ts.playing ? QStringLiteral("⏸")
					  : QStringLiteral("▶"));
	if (playPauseBtn_->property("playing").toBool() != ts.playing) {
		playPauseBtn_->setProperty("playing", ts.playing);
		repolish(playPauseBtn_);
	}
	if (nowBtn_->property("live").toBool() != ts.followLive) {
		nowBtn_->setProperty("live", ts.followLive);
		repolish(nowBtn_);
	}
	if (!speed_->isSliderDown()) {
		int sv = std::clamp((int)(ts.speed * 100.0), 5, 100);
		speed_->blockSignals(true);
		speed_->setValue(sv);
		speed_->blockSignals(false);
		speedLbl_->setText(QString::asprintf("%.2f\xc3\x97", sv / 100.0));
	}
	// Angle buttons: PVW green = selected, PGM red = event playing on it.
	// Visual state is driven by the "state" property + QSS, not :checked.
	if (anglesA_) {
		bool ep = ts.eventActive && ts.playing;
		for (int i = 1; i <= kNCams; i++) {
			auto *b = qobject_cast<QPushButton *>(
				anglesA_->button(i));
			if (!b || !b->isVisible())
				continue;
			QString st = (ep && i == ts.angle)
					     ? QStringLiteral("program")
				   : (i == ts.angle)
					     ? QStringLiteral("preview")
					     : QString();
			if (b->property("state").toString() != st) {
				b->setProperty("state", st);
				repolish(b);
			}
		}
		// Keep exclusive selection in sync for click handling
		if (anglesA_->button(ts.angle))
			anglesA_->button(ts.angle)->setChecked(true);
	}

	// Event camera chips: PVW green = enabled, PGM red = currently playing.
	{
		auto ps = PlaybackCoordinator::instance().playState();
		for (int row = 0; row < events_->rowCount(); row++) {
			QTableWidgetItem *idItem = events_->item(row, kColId);
			if (!idItem)
				continue;
			int rowEv = idItem->data(Qt::UserRole).toInt();
			bool isActive = ps.active && (ps.eventId == rowEv);
			QWidget *cell = events_->cellWidget(row, kColCams);
			if (!cell)
				continue;
			const auto btns =
				cell->findChildren<QToolButton *>();
			for (QToolButton *btn : btns) {
				int ci = btn->property("camIndex").toInt();
				bool checked = btn->isChecked();
				QString st;
				if (isActive && ci == ps.angle1)
					st = QStringLiteral("program");
				else if (checked)
					st = QStringLiteral("preview");
				if (btn->property("state").toString() != st) {
					btn->setProperty("state", st);
					repolish(btn);
				}
			}
		}
	}

	// --- recording status ---
	bool rec = core.isRecording();
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

	Data st(core.statusJson());
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

	// --- auto-refresh event list on any external mutation (hotkeys, etc.) ---
	uint64_t ev = EventStore::instance().version();
	if (ev != lastEventVersion_) {
		lastEventVersion_ = ev;
		refreshAngles();
		refreshEvents();
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

	refreshing_ = true;
	events_->setRowCount(0);
	obs_data_array_t *arr = obs_data_get_array(d, "events");
	if (!arr) {
		refreshing_ = false;
		return;
	}
	const Qt::Alignment mid = Qt::AlignVCenter | Qt::AlignHCenter;
	std::vector<std::pair<double, double>> markers;

	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(arr, i);
		int id = (int)obs_data_get_int(e, "id");
		int64_t tin = obs_data_get_int(e, "tInNs");
		int64_t tout = obs_data_get_int(e, "tOutNs");
		double speed = obs_data_get_double(e, "speed");

		bool camOn[kEventAngles] = {};
		std::string camNotes[kEventAngles];
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
				obs_data_release(ad);
			}
			obs_data_array_release(aArr);
		}

		// Collect timeline marker for ALL events (regardless of search
		// filter) so the seekbar shows full density even when filtered.
		if (tin >= 0 && tout >= tin && durationNs_ > 0) {
			double inFrac = std::clamp(
				(double)tin / (double)durationNs_, 0.0, 1.0);
			double outFrac = std::clamp(
				(double)tout / (double)durationNs_, 0.0, 1.0);
			if (outFrac > inFrac)
				markers.push_back({inFrac, outFrac});
		}

		QString dur = tout >= 0 ? formatTc(tout - tin)
					: QString::fromUtf8(
						  obs_module_text("Dock.Open"));
		QString spStr = speed >= 0
					? QString::number((int)(speed * 100)) + "%"
					: QStringLiteral("--");

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
		events_->setItem(row, kColIn, roItem(formatTc(tin), mid));
		events_->setItem(row, kColOut,
				 roItem(tout >= 0 ? formatTc(tout)
						  : QStringLiteral("--"),
					mid));
		events_->setItem(row, kColDur, roItem(dur, mid));

		// editable speed (type "50", "50%" or blank/-- to inherit)
		auto *spItem = new QTableWidgetItem(spStr);
		spItem->setTextAlignment(mid);
		spItem->setData(Qt::UserRole, id);
		spItem->setToolTip(obs_module_text("Dock.SpeedHint"));
		events_->setItem(row, kColSpeed, spItem);

		// inline 1..8 camera toggle chips with per-angle description
		events_->setCellWidget(row, kColCams,
				       makeCameraCell(id, camOn, camNotes));

		obs_data_release(e);
	}
	obs_data_array_release(arr);
	refreshing_ = false;
	if (seek_)
		seek_->setEventMarkers(std::move(markers));
}

QWidget *MultiReplayDock::makeCameraCell(int id, const bool *enabled,
					  const std::string *notes)
{
	Config cfg = ReplayCore::instance().getConfig();
	auto *w = new QWidget(events_);
	auto *l = new QHBoxLayout(w);
	l->setContentsMargins(2, 1, 2, 1);
	l->setSpacing(4);

	for (int i = 0; i < kEventAngles; i++) {
		// Only show chips for cameras that have a source configured.
		bool configured = (i < kMaxCameras) &&
				  !cfg.cameras[i].sourceName.empty();
		if (!configured)
			continue;

		const std::string &dn = cfg.cameras[i].displayName;
		QString label = dn.empty() ? QString::number(i + 1)
					   : QString::fromStdString(dn).left(5);

		auto *b = new QToolButton(w);
		b->setObjectName("mrCamToggle");
		b->setText(label);
		b->setProperty("camIndex", i + 1); // 1-based; used in poll()
		if (!dn.empty())
			b->setToolTip(QString::fromStdString(dn));
		b->setCheckable(true);
		b->setChecked(enabled[i]);
		b->setCursor(Qt::PointingHandCursor);

		// Note label beside the chip (double-click to edit)
		const std::string &note = notes[i];
		QString noteText = note.empty()
					   ? QStringLiteral("--")
					   : QString::fromStdString(note).left(10);
		auto *noteLbl = new QLabel(noteText, w);
		noteLbl->setObjectName("mrCamNote");
		noteLbl->setToolTip(obs_module_text("Dock.CamNoteHint"));

		// Store context on the label so eventFilter can open the dialog.
		int a1 = i + 1;
		noteLbl->setProperty("eventId", id);
		noteLbl->setProperty("angle1", a1);
		QString camLabel = dn.empty() ? QString("Cam %1").arg(i + 1)
					      : QString::fromStdString(dn);
		noteLbl->setProperty("camLabel", camLabel);
		noteLbl->installEventFilter(this);

		connect(b, &QToolButton::toggled, this, [id, a1](bool on) {
			EventStore::instance().setAngle(id, a1, on);
		});

		l->addWidget(b);
		l->addSpacing(2);
		l->addWidget(noteLbl);
		l->addSpacing(6); // gap between camera pairs
	}
	l->addStretch(1);
	return w;
}

void MultiReplayDock::onEventItemChanged(QTableWidgetItem *item)
{
	if (refreshing_ || !item)
		return;
	int id = item->data(Qt::UserRole).toInt();
	if (id <= 0)
		return;
	auto &store = EventStore::instance();

	if (item->column() == kColSpeed) {
		QString t = item->text().trimmed();
		t.remove('%');
		if (t.isEmpty() || t == "--") {
			store.setSpeed(id, -1.0);
		} else {
			bool ok = false;
			double pct = t.toDouble(&ok);
			if (ok)
				store.setSpeed(id, std::clamp(pct, 1.0, 100.0) /
							    100.0);
		}
		refreshEvents();
	}
}

// ---------------------------------------------------------------------------
// Event filter — double-click on note labels to edit
// ---------------------------------------------------------------------------

bool MultiReplayDock::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::MouseButtonDblClick) {
		auto *lbl = qobject_cast<QLabel *>(watched);
		if (lbl && lbl->objectName() == QLatin1String("mrCamNote")) {
			int id = lbl->property("eventId").toInt();
			int a1 = lbl->property("angle1").toInt();
			QString camLabel = lbl->property("camLabel").toString();
			QString curNote = lbl->text() == QStringLiteral("--")
						  ? QString()
						  : lbl->text();
			bool ok;
			QString text = QInputDialog::getText(
				this,
				QString("Descrizione — %1").arg(camLabel),
				QStringLiteral("Descrizione:"),
				QLineEdit::Normal, curNote, &ok);
			if (ok) {
				curNote = text.trimmed();
				lbl->setText(curNote.isEmpty()
						     ? QStringLiteral("--")
						     : curNote.left(10));
				EventStore::instance().setAngleNote(
					id, a1, curNote.toStdString());
				refreshEvents();
			}
			return true;
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

	auto *outScene = makeSourceCombo(cfg.outputSceneName);
	form->addRow(obs_module_text("Dock.OutputScene"), outScene);

	// Replay Media Source: list the Media Sources present, plus "(auto)".
	auto *replaySrc = new QComboBox(&dlg);
	replaySrc->addItem(obs_module_text("Dock.AutoReplaySource"), "");
	{
		Data md(core.sourcesJson());
		obs_data_array_t *arr =
			md ? obs_data_get_array(md, "sources") : nullptr;
		if (arr) {
			size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; i++) {
				obs_data_t *it = obs_data_array_item(arr, i);
				const char *id = obs_data_get_string(it, "id");
				const char *nm = obs_data_get_string(it, "name");
				// Only Media-type sources can play a recording.
				if (id &&
				    (strcmp(id, "ffmpeg_source") == 0 ||
				     strcmp(id, "vlc_source") == 0))
					replaySrc->addItem(QString::fromUtf8(nm),
							   QString::fromUtf8(nm));
				obs_data_release(it);
			}
			obs_data_array_release(arr);
		}
	}
	{
		int idx = replaySrc->findData(
			QString::fromStdString(cfg.replaySourceName));
		if (idx >= 0)
			replaySrc->setCurrentIndex(idx);
	}
	replaySrc->setToolTip(obs_module_text("Dock.ReplaySourceHint"));
	form->addRow(obs_module_text("Dock.ReplaySource"), replaySrc);

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
	cfg.replaySourceName =
		replaySrc->currentData().toString().toStdString();
	cfg.musicSourceName = music->currentData().toString().toStdString();
	cfg.autoSwitchScene = autoSwitch->isChecked();
	for (int i = 0; i < kMaxCameras; i++) {
		cfg.cameras[i].sourceName =
			camCombos[i]->currentData().toString().toStdString();
		cfg.cameras[i].displayName =
			camNameEdits[i]->text().trimmed().toStdString();
	}
	core.setConfig(cfg);
	EventStore::instance().setSessionFolder(cfg.sessionFolder);
	// Re-bind the engine to the (possibly changed) replay Media Source.
	MediaReplay::instance().ensureSource();
	refreshAngles();
	refreshEvents();
}

} // namespace multireplay
