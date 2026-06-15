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
#include <QSizePolicy>
#include <QStyle>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QFontDatabase>

#include <algorithm>
#include <string>
#include <cstring>

namespace multireplay {

// ---------------------------------------------------------------------------
// Dock-wide stylesheet — a compact, modern dark theme that sits cleanly on top
// of OBS' own palette. Selectors are scoped by object name / Qt class so the
// dock styles only itself and never leaks into the rest of OBS.
// ---------------------------------------------------------------------------
// Broadcast amber theme.
// Reference: hardware waveform monitors, tape counters, intercom panels —
// amber/phosphor readouts on near-black panels. Every "active" state
// (playing, angle selected, camera on, seek fill, marks) uses the amber
// family; danger/recording stays red; everything else is dark and quiet.
//
// Palette:
//   #0f1118  deep bg          #e8a020  amber (primary active)
//   #161b24  surface          #f5c842  amber bright / hover
//   #1e2430  elevated         #3a2800  amber bg (checked state)
//   #252d3a  btn rest         #d4b868  timecode (warm phosphor cream)
//   #2f3a4a  btn hover        #7a6838  section label (muted amber)
//   #2a3545  border           #dc2626  recording / danger red
//   #3a4f65  border hover     #3a0a0a  red bg (live mode indicator)
//   #c8cdd8  text primary     #6b7688  text muted
static const char *kDockStyle = R"QSS(
#MultiReplayDock {
	background: #0f1118;
}
#MultiReplayDock QLabel {
	color: #c8cdd8;
	background: transparent;
}
QLabel#mrMuted { color: #6b7688; font-size: 11px; }
QLabel#mrTimecode {
	color: #d4b868;
	font-size: 13px;
	font-weight: 600;
	letter-spacing: 0.5px;
}
QLabel#mrSectionLabel {
	color: #7a6838;
	font-size: 10px;
	font-weight: 700;
	letter-spacing: 1.2px;
}

/* generic buttons */
#MultiReplayDock QPushButton {
	background: #252d3a;
	color: #c8cdd8;
	border: 1px solid #2a3545;
	border-radius: 5px;
	padding: 5px 11px;
	font-size: 12px;
}
#MultiReplayDock QPushButton:hover { background: #2f3a4a; border-color: #3a4f65; }
#MultiReplayDock QPushButton:pressed { background: #1e2b3a; }
#MultiReplayDock QPushButton:disabled { color: #4a5568; border-color: #1e2430; }

/* transport step buttons */
QPushButton#mrTransport {
	background: #1e2430;
	border: 1px solid #2a3545;
	border-radius: 6px;
	min-width: 34px;
	min-height: 32px;
	padding: 0;
}
QPushButton#mrTransport:hover { background: #252d3a; border-color: #3a4f65; }

/* play/pause — dark when paused, amber-tinted when playing */
QPushButton#mrPlay {
	background: #1e2430;
	border: 1px solid #2a3545;
	border-radius: 6px;
	min-width: 44px;
	min-height: 32px;
	padding: 0;
}
QPushButton#mrPlay:hover { background: #252d3a; border-color: #3a4f65; }
QPushButton#mrPlay[playing="true"] {
	background: #3a2800;
	border: 1px solid #e8a020;
}
QPushButton#mrPlay[playing="true"]:hover {
	background: #4a3400;
	border-color: #f5c842;
}

/* NOW / live-edge button */
QPushButton#mrNow {
	background: #1e2430;
	border: 1px solid #2a3545;
	border-radius: 6px;
	font-weight: 700;
	font-size: 11px;
	letter-spacing: 1px;
	min-height: 32px;
	color: #6b7688;
}
QPushButton#mrNow[live="true"] {
	background: #3a0a0a;
	border-color: #dc2626;
	color: #ef4444;
}

/* REC button */
QPushButton#mrRec {
	font-weight: 700;
	font-size: 12px;
	letter-spacing: 0.8px;
	border-radius: 5px;
	min-height: 32px;
	padding: 5px 14px;
}
QPushButton#mrRec[recording="false"] {
	background: #1e2430; color: #e05050; border: 1px solid #3a2020;
}
QPushButton#mrRec[recording="false"]:hover { background: #2a1a1a; border-color: #5a2525; }
QPushButton#mrRec[recording="true"] {
	background: #7a0a0a; color: #fff; border: 1px solid #dc2626;
}
QPushButton#mrRec[recording="true"]:hover { background: #901010; }

/* settings gear */
QToolButton#mrGear {
	background: #1e2430;
	border: 1px solid #2a3545;
	border-radius: 5px;
	padding: 4px 8px;
	color: #6b7688;
	font-size: 15px;
}
QToolButton#mrGear:hover { background: #252d3a; color: #c8cdd8; border-color: #3a4f65; }

/* angle segmented control — amber when active */
QPushButton#mrAngle {
	background: #161b24;
	border: 1px solid #2a3545;
	border-radius: 4px;
	color: #4a5568;
	font-weight: 700;
	font-size: 11px;
	min-width: 28px;
	min-height: 26px;
	padding: 0;
}
QPushButton#mrAngle:hover { background: #1e2430; color: #7a8898; border-color: #3a4f65; }
QPushButton#mrAngle:checked {
	background: #3a2800;
	border-color: #e8a020;
	color: #f5c842;
}

/* per-event camera toggle chips inside the table — amber when on */
QToolButton#mrCamToggle {
	background: #161b24;
	border: 1px solid #2a3545;
	border-radius: 3px;
	color: #4a5568;
	font-size: 10px;
	font-weight: 700;
	min-width: 20px;
	max-width: 22px;
	min-height: 20px;
	padding: 0;
}
QToolButton#mrCamToggle:hover { border-color: #3a4f65; color: #7a8898; }
QToolButton#mrCamToggle:checked {
	background: #3a2800; border-color: #e8a020; color: #f5c842;
}

/* Mark In/Out and accent buttons */
QPushButton#mrAccent {
	background: #1c1e10; border: 1px solid #e8a020; color: #e8a020;
}
QPushButton#mrAccent:hover { background: #28280e; color: #f5c842; border-color: #f5c842; }
QPushButton#mrDanger { color: #e05050; border-color: #3a2020; }
QPushButton#mrDanger:hover { background: #2a1a1a; border-color: #5a2525; }

/* checkboxes — amber tick */
#MultiReplayDock QCheckBox { color: #b0b8c4; spacing: 6px; font-size: 12px; }
#MultiReplayDock QCheckBox::indicator {
	width: 14px; height: 14px; border-radius: 3px;
	border: 1px solid #2a3545; background: #161b24;
}
#MultiReplayDock QCheckBox::indicator:checked {
	background: #e8a020; border-color: #f5c842;
}

/* inputs */
#MultiReplayDock QComboBox, #MultiReplayDock QLineEdit {
	background: #161b24; color: #c8cdd8;
	border: 1px solid #2a3545; border-radius: 4px;
	padding: 4px 8px; min-height: 22px;
}
#MultiReplayDock QComboBox:hover, #MultiReplayDock QLineEdit:hover {
	border-color: #3a4f65;
}
#MultiReplayDock QComboBox::drop-down { border: 0; width: 18px; }
#MultiReplayDock QComboBox QAbstractItemView {
	background: #161b24; color: #c8cdd8;
	border: 1px solid #2a3545;
	selection-background-color: #3a2800; selection-color: #f5c842;
	outline: 0;
}

/* speed slider — amber fill + handle */
QSlider#mrSpeed::groove:horizontal {
	height: 4px; background: #1e2430; border-radius: 2px;
}
QSlider#mrSpeed::sub-page:horizontal { background: #e8a020; border-radius: 2px; }
QSlider#mrSpeed::handle:horizontal {
	width: 12px; height: 12px; margin: -4px 0;
	background: #f5c842; border-radius: 6px; border: 1px solid #a06014;
}
QSlider#mrSpeed::handle:horizontal:hover { background: #fff; }

/* event table */
QTableWidget#mrEvents {
	background: #0f1118;
	alternate-background-color: #131720;
	gridline-color: transparent;
	border: 1px solid #1e2430;
	border-radius: 6px;
	color: #c8cdd8;
	outline: 0;
}
QTableWidget#mrEvents::item { padding: 4px 6px; border: 0; }
QTableWidget#mrEvents::item:selected {
	background: #2a1e05; color: #f5c842;
}
QHeaderView::section {
	background: #161b24;
	color: #7a6838;
	padding: 5px 6px;
	border: 0;
	border-bottom: 1px solid #2a3545;
	font-size: 10px;
	font-weight: 700;
	letter-spacing: 0.8px;
}
QTableCornerButton::section { background: #161b24; border: 0; }

/* scrollbars */
#MultiReplayDock QScrollBar:vertical {
	background: transparent; width: 8px; margin: 0;
}
#MultiReplayDock QScrollBar::handle:vertical {
	background: #2a3545; border-radius: 4px; min-height: 24px;
}
#MultiReplayDock QScrollBar::handle:vertical:hover { background: #3a4f65; }
#MultiReplayDock QScrollBar::add-line, #MultiReplayDock QScrollBar::sub-line {
	height: 0; width: 0;
}

QSplitter::handle { background: transparent; }
)QSS";

namespace {

constexpr int kNCams = kIndexMaxCameras; // 8

// Event table column layout.
enum EventCol {
	kColId = 0,
	kColIn,
	kColOut,
	kColDur,
	kColSpeed,
	kColCams,
	kColDesc,
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

// transport icon button backed by a real QStyle pixmap (no font glyphs, so it
// always renders regardless of the platform's emoji/symbol fonts).
QPushButton *iconBtn(QStyle::StandardPixmap sp, QWidget *parent,
		     const QString &tip, const char *role = "mrTransport")
{
	auto *b = new QPushButton(parent);
	b->setObjectName(QString::fromLatin1(role));
	b->setIcon(parent->style()->standardIcon(sp));
	b->setIconSize(QSize(16, 16));
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
	setFixedHeight(20);
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

	const int m = 2;
	const int h = 6;                       // track thickness
	const int y = (height() - h) / 2;
	const int w = width() - 2 * m;
	const double pos = dragging_ ? dragFrac_ : positionFrac_;

	// track (deep surface)
	QRectF track(m, y, w, h);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x1e, 0x24, 0x30));
	p.drawRoundedRect(track, 3, 3);

	// seekable band — slightly lighter, shows indexed footage extent
	if (seekableFrac_ > 0.0) {
		QRectF s(m, y, w * seekableFrac_, h);
		p.setBrush(QColor(0x2a, 0x35, 0x45));
		p.drawRoundedRect(s, 3, 3);
	}

	// played-up-to-position fill — amber gradient (dark → bright, left → right)
	if (pos > 0.0) {
		QRectF f(m, y, w * pos, h);
		QLinearGradient grad(f.left(), 0, f.right(), 0);
		grad.setColorAt(0.0, QColor(0xa0, 0x60, 0x14));
		grad.setColorAt(1.0, QColor(0xe8, 0xa0, 0x20));
		p.setBrush(grad);
		p.drawRoundedRect(f, 3, 3);
	}

	// handle — amber bright; grows on hover/drag
	double hx = m + w * pos;
	double r = (dragging_ || underMouse()) ? 7.0 : 5.5;
	p.setBrush(QColor(0xf5, 0xc8, 0x42));
	p.setPen(QPen(QColor(0x60, 0x3c, 0x08), 1.0));
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
	setMinimumWidth(300);
	setStyleSheet(QString::fromUtf8(kDockStyle));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);

	// --- top toolbar: REC + status + settings ---
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

	// --- vertical splitter: controls on top, event list below ---
	// The event list is the working area, so it gets the larger share and a
	// generous minimum; the preview/transport block stays compact and the
	// user can drag the handle to trade space either way.
	auto *splitter = new QSplitter(Qt::Vertical, this);
	splitter->setChildrenCollapsible(false);
	splitter->setHandleWidth(8);

	auto *controls = new QWidget(splitter);
	auto *cv = new QVBoxLayout(controls);
	cv->setContentsMargins(0, 0, 0, 0);
	cv->setSpacing(8);
	cv->addWidget(buildPreview(), 1);
	cv->addWidget(buildTransport());
	cv->addWidget(buildMarkers());

	auto *eventsPanel = buildEvents();
	eventsPanel->setMinimumHeight(220);
	splitter->addWidget(controls);
	splitter->addWidget(eventsPanel);
	splitter->setStretchFactor(0, 2);
	splitter->setStretchFactor(1, 5);
	splitter->setSizes({260, 420});
	root->addWidget(splitter, 1);

	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(33); // ~30 fps — smooth seekbar + responsive transport
	connect(pollTimer_, &QTimer::timeout, this, &MultiReplayDock::poll);
	pollTimer_->start();

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
	v->setSpacing(6);

	displayA_ = new OBSQTDisplay(this);
	displayA_->setRenderCallback(&MultiReplayDock::drawChannelA, this);
	displayA_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	displayA_->setMinimumHeight(120);
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
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(6);

	seek_ = new SeekBar(this);
	connect(seek_, &SeekBar::scrubStateChanged, this,
		[this](bool dragging) { seekDragging_ = dragging; });
	connect(seek_, &SeekBar::scrubMoved, this, [this](double frac) {
		tcLbl_->setText(formatTc((int64_t)(frac * (double)durationNs_)) +
				"  /  " + formatTc(durationNs_));
	});
	connect(seek_, &SeekBar::seekRequested, this,
		[this](double frac) { seekToFraction(frac); });
	v->addWidget(seek_);

	tcLbl_ = new QLabel("00:00.000  /  00:00.000", this);
	tcLbl_->setObjectName("mrTimecode");
	tcLbl_->setFont(QFont(monoFamily()));
	tcLbl_->setAlignment(Qt::AlignHCenter);
	v->addWidget(tcLbl_);

	auto *h = new QHBoxLayout();
	h->setSpacing(6);
	h->addStretch(1);

	// Media Source has no reverse playback, so the transport is
	// step-back / play-pause / step-fwd / NOW. Stepping is a seek.
	auto *stepBack = iconBtn(QStyle::SP_MediaSkipBackward, this,
				 obs_module_text("Dock.StepBack"));
	playPauseBtn_ = iconBtn(QStyle::SP_MediaPlay, this,
				obs_module_text("Dock.PlayPause"), "mrPlay");
	auto *stepFwd = iconBtn(QStyle::SP_MediaSkipForward, this,
				obs_module_text("Dock.StepFwd"));
	nowBtn_ = new QPushButton(QStringLiteral("NOW"), this);
	nowBtn_->setObjectName("mrNow");
	nowBtn_->setProperty("live", false);
	nowBtn_->setCursor(Qt::PointingHandCursor);
	nowBtn_->setToolTip(obs_module_text("Dock.JumpToNow"));
	nowBtn_->setMinimumWidth(46);
	for (auto *b : {stepBack, playPauseBtn_, stepFwd})
		h->addWidget(b);
	h->addWidget(nowBtn_);
	h->addStretch(1);

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

	v->addLayout(h);

	// speed row
	auto *sh = new QHBoxLayout();
	sh->setSpacing(8);
	auto *spLbl = new QLabel(
		QString::fromUtf8(obs_module_text("Dock.Speed")).toUpper(), this);
	spLbl->setObjectName("mrSectionLabel");
	sh->addWidget(spLbl);
	speed_ = new QSlider(Qt::Horizontal, this);
	speed_->setObjectName("mrSpeed");
	speed_->setRange(5, 100); // 0.05x .. 1.00x
	speed_->setValue(100);
	speed_->setCursor(Qt::PointingHandCursor);
	speedLbl_ = new QLabel("1.00x", this);
	speedLbl_->setObjectName("mrTimecode");
	speedLbl_->setFont(QFont(monoFamily()));
	speedLbl_->setMinimumWidth(46);
	speedLbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	// Live label on drag; commit the speed on release (each change reloads
	// the Media Source, so don't thrash it while dragging).
	connect(speed_, &QSlider::valueChanged, this, [this](int val) {
		speedLbl_->setText(QString::asprintf("%.2fx", val / 100.0));
	});
	connect(speed_, &QSlider::sliderReleased, this, [this]() {
		MediaReplay::instance().setSpeed(speed_->value() / 100.0);
	});
	sh->addWidget(speed_, 1);
	sh->addWidget(speedLbl_);
	v->addLayout(sh);

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
	h->setSpacing(5);

	liveChk_ = new QCheckBox(obs_module_text("Dock.LiveMode"), this);
	liveChk_->setChecked(EventStore::instance().liveMode());
	liveChk_->setCursor(Qt::PointingHandCursor);
	connect(liveChk_, &QCheckBox::toggled, this,
		[](bool on) { EventStore::instance().setLiveMode(on); });
	h->addWidget(liveChk_);
	h->addStretch(1);

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
	v->setContentsMargins(0, 0, 0, 0);
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
		 obs_module_text("Dock.Cameras"),
		 obs_module_text("Dock.Description")});
	events_->setSelectionBehavior(QAbstractItemView::SelectRows);
	events_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// Speed (4) and Description (6) cells are edited in place; the camera
	// cell (5) is an inline widget. Double-click edits a cell — it no longer
	// triggers playback (use the Play buttons instead).
	events_->setEditTriggers(QAbstractItemView::DoubleClicked |
				 QAbstractItemView::EditKeyPressed);
	events_->verticalHeader()->setVisible(false);
	events_->verticalHeader()->setDefaultSectionSize(30);
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
		hh->setSectionResizeMode(kColDesc, QHeaderView::Stretch);
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

	// edit controls
	auto *eb = new QHBoxLayout();
	eb->setSpacing(3);
	auto *dup = compactBtn(obs_module_text("Dock.Duplicate"), this);
	auto *del = compactBtn(obs_module_text("Dock.Delete"), this, "mrDanger");
	auto *exp = compactBtn(obs_module_text("Dock.Export"), this);
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
	auto *delAll = compactBtn(obs_module_text("Dock.DeleteAll"), this,
				  "mrDanger");
	connect(delAll, &QPushButton::clicked, this, [this]() {
		if (QMessageBox::question(
			    this, "obs-multireplay",
			    obs_module_text("Dock.DeleteAllConfirm"),
			    QMessageBox::Yes | QMessageBox::No,
			    QMessageBox::No) != QMessageBox::Yes)
			return;
		PlaybackCoordinator::instance().stopEvents();
		EventStore::instance().clearAll();
		// version counter change will trigger auto-refresh in poll()
	});
	eb->addWidget(dup);
	eb->addWidget(del);
	eb->addWidget(exp);
	eb->addWidget(delAll);
	eb->addStretch(1);
	v->addLayout(eb);

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
			   !core.getConfig().sessionFolder.empty()) {
			// Don't load during recording: files are still open and
			// not yet fully flushed; loadSession would call jumpToEnd
			// → obs_source_media_restart which causes a visible flash
			// of playback in the preview every 2 seconds.
			std::string err;
			engine.loadSession(core.getConfig().sessionFolder, err);
		}
	}

	// --- transport ---
	Data t(engine.transportJson());
	if (t) {
		seekableNs_ = obs_data_get_int(t, "seekableNs");
		durationNs_ = obs_data_get_int(t, "durationNs");
		bool followLive = obs_data_get_bool(t, "followLive");

		obs_data_t *a = obs_data_get_obj(t, "A");
		int64_t posA = a ? obs_data_get_int(a, "positionNs") : 0;
		bool playingA = a ? obs_data_get_bool(a, "playing") : false;
		int angleA = a ? (int)obs_data_get_int(a, "angle") : 1;
		double spA = a ? obs_data_get_double(a, "speed") : 1.0;
		if (a)
			obs_data_release(a);

		if (!seekDragging_) {
			double posFrac = durationNs_ > 0
						 ? (double)posA / (double)durationNs_
						 : 0.0;
			double seekFrac = durationNs_ > 0
						  ? (double)seekableNs_ /
							    (double)durationNs_
						  : 1.0;
			seek_->setProgress(posFrac, seekFrac);
			tcLbl_->setText(formatTc(posA) + "  /  " +
					formatTc(durationNs_));
		}

		QStyle::StandardPixmap sp = playingA ? QStyle::SP_MediaPause
						     : QStyle::SP_MediaPlay;
		playPauseBtn_->setIcon(style()->standardIcon(sp));
		if (playPauseBtn_->property("playing").toBool() != playingA) {
			playPauseBtn_->setProperty("playing", playingA);
			repolish(playPauseBtn_);
		}
		if (nowBtn_->property("live").toBool() != followLive) {
			nowBtn_->setProperty("live", followLive);
			repolish(nowBtn_);
		}
		if (!speed_->isSliderDown()) {
			speed_->blockSignals(true);
			speed_->setValue(std::clamp((int)(spA * 100.0), 5, 100));
			speed_->blockSignals(false);
		}

		if (anglesA_ && anglesA_->button(angleA))
			anglesA_->button(angleA)->setChecked(true);
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
		refreshEvents();
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

	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(arr, i);
		int id = (int)obs_data_get_int(e, "id");
		int64_t tin = obs_data_get_int(e, "tInNs");
		int64_t tout = obs_data_get_int(e, "tOutNs");
		double speed = obs_data_get_double(e, "speed");

		bool camOn[kEventAngles] = {};
		QString anglesStr, notes;
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
				if (nt && *nt && notes.isEmpty())
					notes = QString::fromUtf8(nt);
				obs_data_release(ad);
			}
			obs_data_array_release(aArr);
		}

		QString dur = tout >= 0 ? formatTc(tout - tin)
					: QString::fromUtf8(
						  obs_module_text("Dock.Open"));
		QString spStr = speed >= 0
					? QString::number((int)(speed * 100)) + "%"
					: QStringLiteral("--");

		// search filter (id / notes / angles)
		if (!needle.isEmpty()) {
			QString hay = QString::number(id) + " " +
				      notes.toLower() + " " + anglesStr;
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

		// inline 1..8 camera toggles
		events_->setCellWidget(row, kColCams, makeCameraCell(id, camOn));

		// editable description (applies to the whole event)
		auto *descItem = new QTableWidgetItem(notes);
		descItem->setData(Qt::UserRole, id);
		descItem->setToolTip(obs_module_text("Dock.DescriptionHint"));
		events_->setItem(row, kColDesc, descItem);

		obs_data_release(e);
	}
	obs_data_array_release(arr);
	refreshing_ = false;
}

QWidget *MultiReplayDock::makeCameraCell(int id, const bool *enabled)
{
	auto *w = new QWidget(events_);
	auto *l = new QHBoxLayout(w);
	l->setContentsMargins(2, 1, 2, 1);
	l->setSpacing(2);
	for (int i = 0; i < kEventAngles; i++) {
		auto *b = new QToolButton(w);
		b->setObjectName("mrCamToggle");
		b->setText(QString::number(i + 1));
		b->setCheckable(true);
		b->setChecked(enabled[i]);
		b->setCursor(Qt::PointingHandCursor);
		b->setToolTip(QString("%1 %2")
				      .arg(obs_module_text("Dock.Angle"))
				      .arg(i + 1));
		int a1 = i + 1;
		connect(b, &QToolButton::toggled, this, [id, a1](bool on) {
			EventStore::instance().setAngle(id, a1, on);
		});
		l->addWidget(b);
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
	} else if (item->column() == kColDesc) {
		store.setDescription(id, item->text().trimmed().toStdString());
	}
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

	// camera assignments
	auto *camBox = new QGroupBox(obs_module_text("Dock.Cameras"), &dlg);
	auto *camForm = new QFormLayout(camBox);
	std::vector<QComboBox *> camCombos;
	for (int i = 0; i < kMaxCameras; i++) {
		auto *c = makeSourceCombo(cfg.cameras[i].sourceName);
		camCombos.push_back(c);
		camForm->addRow(QString("Cam %1").arg(i + 1), c);
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
	for (int i = 0; i < kMaxCameras; i++)
		cfg.cameras[i].sourceName =
			camCombos[i]->currentData().toString().toStdString();
	core.setConfig(cfg);
	EventStore::instance().setSessionFolder(cfg.sessionFolder);
	// Re-bind the engine to the (possibly changed) replay Media Source.
	MediaReplay::instance().ensureSource();
	refreshEvents();
}

} // namespace multireplay
