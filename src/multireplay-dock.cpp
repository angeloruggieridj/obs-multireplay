/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multireplay-dock.hpp"
#include "qt-display.hpp"
#include "replay-core.hpp"
#include "replay-player.hpp"
#include "event-store.hpp"
#include "playback-coordinator.hpp"
#include "export.hpp"
#include "session-index.hpp"
#include "plugin-support.h"

#include <obs-module.h>

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

#include <algorithm>
#include <string>

namespace multireplay {

namespace {

constexpr int kNCams = kIndexMaxCameras; // 8

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
	auto &engine = ReplayEngine::instance();
	if (engine.sessionLoaded())
		return true;
	std::string err;
	return engine.loadSession(ReplayCore::instance().getConfig().sessionFolder,
				  err);
}

// small compact transport/marker button
QPushButton *compactBtn(const QString &text, QWidget *parent, int w = 0)
{
	auto *b = new QPushButton(text, parent);
	b->setMaximumHeight(26);
	if (w > 0)
		b->setFixedWidth(w);
	b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	return b;
}

} // namespace

// ---------------------------------------------------------------------------
// Preview render callback (runs on the OBS graphics thread)
// ---------------------------------------------------------------------------

void MultiReplayDock::drawChannelA(void *, uint32_t cx, uint32_t cy)
{
	ReplayPlayer *p = ReplayEngine::instance().channel('A');
	obs_source_t *src = p ? p->acquireSource() : nullptr;
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
	setMinimumWidth(280);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);

	// --- top toolbar: REC + status + settings ---
	{
		auto *bar = new QHBoxLayout();
		bar->setSpacing(4);
		recBtn_ = new QPushButton(this);
		recBtn_->setMinimumWidth(84);
		recBtn_->setMaximumHeight(28);
		bar->addWidget(recBtn_);

		statusLbl_ = new QLabel(this);
		statusLbl_->setStyleSheet("color:#9aa;");
		bar->addWidget(statusLbl_, 1);

		auto *gear = new QToolButton(this);
		gear->setText(QStringLiteral("⚙"));
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
				ReplayEngine::instance().loadSession(
					core.getConfig().sessionFolder, err);
			} else {
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
	auto *splitter = new QSplitter(Qt::Vertical, this);
	splitter->setChildrenCollapsible(false);
	splitter->setHandleWidth(6);

	auto *controls = new QWidget(splitter);
	auto *cv = new QVBoxLayout(controls);
	cv->setContentsMargins(0, 0, 0, 0);
	cv->setSpacing(4);
	cv->addWidget(buildPreview(), 1);
	cv->addWidget(buildTransport());
	cv->addWidget(buildMarkers());

	splitter->addWidget(controls);
	splitter->addWidget(buildEvents());
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);
	root->addWidget(splitter, 1);

	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(66); // ~15 fps transport refresh
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
	v->setSpacing(3);

	displayA_ = new OBSQTDisplay(this);
	displayA_->setRenderCallback(&MultiReplayDock::drawChannelA, this);
	displayA_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	displayA_->setMinimumHeight(150);
	v->addWidget(displayA_, 1);

	// angle selector row (cam 1..N)
	auto *row = new QWidget(box);
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(2);
	h->addWidget(new QLabel(obs_module_text("Dock.Angle"), row));
	anglesA_ = new QButtonGroup(this);
	anglesA_->setExclusive(true);
	for (int i = 1; i <= kNCams; i++) {
		auto *b = new QPushButton(QString::number(i), row);
		b->setCheckable(true);
		b->setFixedWidth(28);
		b->setMaximumHeight(26);
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
	v->setSpacing(3);

	seek_ = new QSlider(Qt::Horizontal, this);
	seek_->setRange(0, 10000);
	connect(seek_, &QSlider::sliderPressed, this,
		[this]() { seekDragging_ = true; });
	connect(seek_, &QSlider::sliderReleased, this, [this]() {
		seekDragging_ = false;
		seekToFraction(seek_->value() / 10000.0);
	});
	connect(seek_, &QSlider::sliderMoved, this, [this](int val) {
		tcLbl_->setText(formatTc((int64_t)(val / 10000.0 *
						   (double)durationNs_)) +
				" / " + formatTc(durationNs_));
	});
	v->addWidget(seek_);

	auto *h = new QHBoxLayout();
	h->setSpacing(3);

	tcLbl_ = new QLabel("00:00.000 / 00:00.000", this);
	tcLbl_->setStyleSheet("font-family:monospace;");
	h->addWidget(tcLbl_);
	h->addStretch(1);

	auto *stepBack = compactBtn(QStringLiteral("⏮"), this, 32);
	reverseBtn_ = compactBtn(QStringLiteral("◀"), this, 32);
	playPauseBtn_ = compactBtn(QStringLiteral("▶"), this, 36);
	auto *stepFwd = compactBtn(QStringLiteral("⏭"), this, 32);
	nowBtn_ = compactBtn(QStringLiteral("NOW"), this, 44);
	for (auto *b : {stepBack, reverseBtn_, playPauseBtn_, stepFwd, nowBtn_})
		h->addWidget(b);

	connect(stepBack, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		ReplayEngine::instance().setFollowLive(false);
		ReplayEngine::instance().applyTransport(
			'A', [](ReplayPlayer &p) { p.stepFrames(-1); });
	});
	connect(stepFwd, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		ReplayEngine::instance().setFollowLive(false);
		ReplayEngine::instance().applyTransport(
			'A', [](ReplayPlayer &p) { p.stepFrames(1); });
	});
	connect(reverseBtn_, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		ReplayEngine::instance().applyTransport(
			'A', [](ReplayPlayer &p) { p.changeDirection(); });
	});
	connect(playPauseBtn_, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		auto &engine = ReplayEngine::instance();
		bool playing = engine.channelA().playing();
		if (!playing)
			engine.setFollowLive(false);
		engine.applyTransport('A', [playing](ReplayPlayer &p) {
			p.setPlaying(!playing);
		});
	});
	connect(nowBtn_, &QPushButton::clicked, this, []() {
		if (!ensureSession())
			return;
		auto &engine = ReplayEngine::instance();
		engine.setFollowLive(true);
		engine.applyTransport('A',
				      [](ReplayPlayer &p) { p.jumpToEnd(); });
	});

	v->addLayout(h);

	// speed row
	auto *sh = new QHBoxLayout();
	sh->setSpacing(3);
	sh->addWidget(new QLabel(obs_module_text("Dock.Speed"), this));
	speed_ = new QSlider(Qt::Horizontal, this);
	speed_->setRange(5, 100); // 0.05x .. 1.00x
	speed_->setValue(100);
	connect(speed_, &QSlider::valueChanged, this, [](int val) {
		double sp = val / 100.0;
		ReplayEngine::instance().applyTransport(
			'A', [sp](ReplayPlayer &p) { p.setSpeed(sp); });
	});
	sh->addWidget(speed_, 1);
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
	h->setSpacing(3);

	liveChk_ = new QCheckBox(obs_module_text("Dock.LiveMode"), this);
	liveChk_->setChecked(EventStore::instance().liveMode());
	connect(liveChk_, &QCheckBox::toggled, this,
		[](bool on) { EventStore::instance().setLiveMode(on); });
	h->addWidget(liveChk_);
	h->addStretch(1);

	auto *in = compactBtn(obs_module_text("Dock.MarkIn"), this);
	auto *out = compactBtn(obs_module_text("Dock.MarkOut"), this);
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
		auto *b = compactBtn(QString("-%1s").arg(sec), this, 40);
		connect(b, &QPushButton::clicked, this, [this, sec]() {
			EventStore::instance().markInOut(markTimeNs(), sec);
			refreshEvents();
		});
		h->addWidget(b);
	}

	auto *cancel = compactBtn(obs_module_text("Dock.Cancel"), this);
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
	top->setSpacing(3);
	top->addWidget(new QLabel(obs_module_text("Dock.List"), this));
	listCombo_ = new QComboBox(this);
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
	events_->setColumnCount(6);
	events_->setHorizontalHeaderLabels(
		{"ID", obs_module_text("Dock.In"), obs_module_text("Dock.Out"),
		 obs_module_text("Dock.Duration"), obs_module_text("Dock.Angles"),
		 obs_module_text("Dock.Note")});
	events_->horizontalHeader()->setStretchLastSection(true);
	events_->setSelectionBehavior(QAbstractItemView::SelectRows);
	events_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	events_->verticalHeader()->setVisible(false);
	events_->setAlternatingRowColors(true);
	connect(events_, &QTableWidget::itemDoubleClicked, this,
		[this](QTableWidgetItem *) {
			std::string err;
			if (ensureSession())
				PlaybackCoordinator::instance().playEvents(
					selectedEventIds(),
					toOutputChk_->isChecked(), err);
		});
	v->addWidget(events_, 1);

	// playback controls
	auto *pb = new QHBoxLayout();
	pb->setSpacing(3);
	auto *playSel = compactBtn(obs_module_text("Dock.PlaySelected"), this);
	auto *playLast = compactBtn(obs_module_text("Dock.PlayLast"), this);
	auto *stop = compactBtn(obs_module_text("Dock.Stop"), this);
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
	auto *del = compactBtn(obs_module_text("Dock.Delete"), this);
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
	eb->addWidget(dup);
	eb->addWidget(del);
	eb->addWidget(exp);
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
		int64_t now = ReplayCore::instance().masterNowNs();
		if (now >= 0)
			return now;
	}
	return ReplayEngine::instance().channelA().position();
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
	ReplayPlayer *p = ReplayEngine::instance().channel('A');
	if (p && angle1Based >= 1 && angle1Based <= kIndexMaxCameras)
		p->setAngle(angle1Based - 1);
}

void MultiReplayDock::seekToFraction(double frac)
{
	if (!ensureSession())
		return;
	frac = std::clamp(frac, 0.0, 1.0);
	int64_t pos = (int64_t)(frac * (double)durationNs_);
	if (seekableNs_ > 0 && pos > seekableNs_)
		pos = seekableNs_;
	auto &engine = ReplayEngine::instance();
	engine.setFollowLive(false);
	engine.applyTransport('A',
			      [pos](ReplayPlayer &p) { p.seekMaster(pos); });
}

// ---------------------------------------------------------------------------
// Periodic refresh
// ---------------------------------------------------------------------------

void MultiReplayDock::poll()
{
	auto &core = ReplayCore::instance();
	auto &engine = ReplayEngine::instance();

	// Keep the index fresh: pick up completed segments while recording, or
	// lazily load the session the first time a folder is configured. Done
	// ~every 2s (30 ticks at 66 ms) so the seekbar grows during a take.
	static int tick = 0;
	if (++tick % 30 == 0) {
		if (engine.sessionLoaded()) {
			if (core.isRecording())
				engine.refreshSession();
		} else if (!core.getConfig().sessionFolder.empty()) {
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
			int v = durationNs_ > 0
					? (int)(10000.0 * (double)posA /
						(double)durationNs_)
					: 0;
			seek_->blockSignals(true);
			seek_->setValue(std::clamp(v, 0, 10000));
			seek_->blockSignals(false);
			tcLbl_->setText(formatTc(posA) + " / " +
					formatTc(durationNs_));
		}

		playPauseBtn_->setText(playingA ? QStringLiteral("⏸")
						: QStringLiteral("▶"));
		nowBtn_->setStyleSheet(followLive ? "color:#e33;font-weight:bold"
						  : "");
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
	recBtn_->setText(rec ? QStringLiteral("■ STOP")
			     : QStringLiteral("● REC"));
	recBtn_->setStyleSheet(
		rec ? "background:#c0392b;color:white;font-weight:bold"
		    : "font-weight:bold");

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
}

void MultiReplayDock::refreshEvents()
{
	int list = EventStore::instance().selectedList();
	Data d(EventStore::instance().listJson(list));
	if (!d)
		return;
	QString needle = search_ ? search_->text().trimmed().toLower() : QString();

	events_->setRowCount(0);
	obs_data_array_t *arr = obs_data_get_array(d, "events");
	if (!arr)
		return;
	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(arr, i);
		int id = (int)obs_data_get_int(e, "id");
		int64_t tin = obs_data_get_int(e, "tInNs");
		int64_t tout = obs_data_get_int(e, "tOutNs");

		QString angles, notes;
		obs_data_array_t *aArr = obs_data_get_array(e, "angles");
		if (aArr) {
			size_t na = obs_data_array_count(aArr);
			for (size_t k = 0; k < na; k++) {
				obs_data_t *ad = obs_data_array_item(aArr, k);
				if (obs_data_get_bool(ad, "enabled"))
					angles += QString::number(k + 1) + " ";
				const char *nt = obs_data_get_string(ad, "note");
				if (nt && *nt) {
					if (!notes.isEmpty())
						notes += "; ";
					notes += QString::fromUtf8(nt);
				}
				obs_data_release(ad);
			}
			obs_data_array_release(aArr);
		}

		QString dur = tout >= 0 ? formatTc(tout - tin)
					: obs_module_text("Dock.Open");

		// search filter (id / notes / angles)
		if (!needle.isEmpty()) {
			QString hay = QString::number(id) + " " +
				      notes.toLower() + " " + angles;
			if (!hay.contains(needle)) {
				obs_data_release(e);
				continue;
			}
		}

		int row = events_->rowCount();
		events_->insertRow(row);
		auto set = [this, row](int col, const QString &txt) {
			events_->setItem(row, col, new QTableWidgetItem(txt));
		};
		auto *idItem = new QTableWidgetItem(QString::number(id));
		idItem->setData(Qt::UserRole, id);
		events_->setItem(row, 0, idItem);
		set(1, formatTc(tin));
		set(2, tout >= 0 ? formatTc(tout) : "--");
		set(3, dur);
		set(4, angles.trimmed());
		set(5, notes);

		obs_data_release(e);
	}
	obs_data_array_release(arr);
	events_->resizeColumnsToContents();
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
	cfg.musicSourceName = music->currentData().toString().toStdString();
	cfg.autoSwitchScene = autoSwitch->isChecked();
	for (int i = 0; i < kMaxCameras; i++)
		cfg.cameras[i].sourceName =
			camCombos[i]->currentData().toString().toStdString();
	core.setConfig(cfg);
	EventStore::instance().setSessionFolder(cfg.sessionFolder);
	refreshEvents();
}

} // namespace multireplay
