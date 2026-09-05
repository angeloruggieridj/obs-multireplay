/*
obs-multireplay — MultiReplayDock: the Settings dialog, the first-run wizard and the project dialogs
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Split out of multireplay-dock.cpp (pure move, no behaviour change): everything
reached through the gear menu or the project menu -- Settings itself, the
guided first-run setup, the "Branch Output is missing" prompt, tag
import/export and the new/open/rename project dialogs -- used to sit in the
same 10k+ line file as the widget assembly and the poll loop. See CLAUDE.md's
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
// Settings dialog
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// "Branch Output is not installed" — asked, not logged
//
// This plugin cannot record one frame without it: every camera's recording is a
// Branch Output filter and the tap attaches to the encoders it runs. Until now
// the only thing that said so on a fresh machine was a LOG WARNING, i.e. a
// sentence in a file nobody opens before a match — and the panel came up
// looking complete and refused at REC.
//
// So it asks, every launch, until the plugin is there. And it offers to do it,
// because "find the right file for your platform" is the step where an operator
// gives up: the download is fetched and CHECKSUM-VERIFIED by
// branch-output-install.cpp, and then handed to the desktop with one call that
// does the right, different thing on each platform — the signed installer runs
// with its own elevation prompt on Windows, Installer.app opens the .pkg on
// macOS, xdg-open hands the .deb to whatever the distribution installed for
// them. We never place a byte ourselves.
//
// openUrl() ANSWERING FALSE IS A REAL CASE, not a formality: a minimal Linux
// desktop with nothing registered for .deb is exactly that, and the honest
// answer there is the path and the command, not a dialog that closes as if
// something had happened.
// ---------------------------------------------------------------------------

void MultiReplayDock::promptForBranchOutput()
{
	auto &bo = BranchOutputInstall::instance();

	QDialog dlg(this);
	dlg.setWindowTitle(obs_module_text("BO.Title"));
	dlg.setMinimumWidth(600);

	auto *root = new QVBoxLayout(&dlg);
	root->setSpacing(10);

	auto *head = new QLabel(obs_module_text("BO.Blurb"), &dlg);
	head->setWordWrap(true);
	root->addWidget(head);

	auto *state = new QLabel(&dlg);
	state->setObjectName("mrMuted");
	state->setWordWrap(true);
	state->setTextInteractionFlags(Qt::TextSelectableByMouse);
	root->addWidget(state);

	auto *bar = new QProgressBar(&dlg);
	bar->setRange(0, 100);
	bar->hide();
	root->addWidget(bar);

	auto *row = new QHBoxLayout();
	auto *install = new QPushButton(obs_module_text("BO.Install"), &dlg);
	install->setObjectName("mrAccent");
	install->setDefault(true);
	auto *page = new QPushButton(obs_module_text("BO.OpenPage"), &dlg);
	auto *later = new QPushButton(obs_module_text("BO.Later"), &dlg);
	row->addWidget(install);
	row->addWidget(page);
	row->addStretch(1);
	row->addWidget(later);
	root->addLayout(row);

	connect(page, &QPushButton::clicked, &dlg, []() {
		QDesktopServices::openUrl(QUrl(kBranchOutputReleasesPage));
	});
	connect(later, &QPushButton::clicked, &dlg, &QDialog::reject);

	// Watching a worker, not driving one. 200 ms is four times faster than a
	// percentage can be read and slow enough to cost nothing.
	QTimer watch(&dlg);
	bool handedOver = false;
	connect(&watch, &QTimer::timeout, &dlg, [&]() {
		const auto st = bo.status();
		switch (st.phase) {
		case BranchOutputInstall::Phase::Asking:
			state->setText(obs_module_text("BO.Asking"));
			break;
		case BranchOutputInstall::Phase::Downloading:
			bar->setValue(st.percent);
			state->setText(
				QString(obs_module_text("BO.Downloading"))
					.arg(QString::fromStdString(st.assetName))
					.arg(QString::fromStdString(st.version)));
			break;
		case BranchOutputInstall::Phase::Verifying:
			bar->setValue(100);
			state->setText(obs_module_text("BO.Verifying"));
			break;
		case BranchOutputInstall::Phase::Ready: {
			if (handedOver)
				break;
			handedOver = true;
			watch.stop();
			bar->hide();
			const QString path =
				QString::fromStdString(st.filePath);
			const bool opened = QDesktopServices::openUrl(
				QUrl::fromLocalFile(path));
			state->setText(
				opened ? QString(obs_module_text("BO.Handed"))
				       : QString(obs_module_text("BO.NotHanded"))
						 .arg(path));
			obs_log(LOG_INFO,
				"[dock] Branch Output installer %s: %s",
				opened ? "handed to the desktop"
				       : "COULD NOT be opened",
				st.filePath.c_str());
			install->hide();
			page->hide();
			later->setText(obs_module_text("BO.Close"));
			later->setDefault(true);
			break;
		}
		case BranchOutputInstall::Phase::Failed:
			watch.stop();
			bar->hide();
			state->setText(QString(obs_module_text("BO.Failed"))
					       .arg(QString::fromStdString(
						       st.message)));
			install->setEnabled(true);
			break;
		default:
			break;
		}
	});

	connect(install, &QPushButton::clicked, &dlg, [&]() {
		install->setEnabled(false);
		bar->setValue(0);
		bar->show();
		state->setText(obs_module_text("BO.Asking"));
		bo.startAsync();
		watch.start(200);
	});

	modalOpen_ = true;
	dlg.exec();
	modalOpen_ = false;
}

// ---------------------------------------------------------------------------
// First run: the five answers, in one place
//
// A fresh install has no session folder, no cameras, no output scene and the
// stock recording settings, and every one of those lives on a DIFFERENT page of
// the Settings dialog. Reported from a new machine: the panel comes up looking
// finished and the operator has to go and find five things before it can do
// anything, without being told which five.
//
// ONE DIALOG, NOT A MULTI-PAGE WIZARD. Everything here fits on a screen, and
// pages would add clicks and a sense of ceremony to what is really a short
// form. It is also deliberately NOT the whole of Settings: these are the
// answers without which nothing works, and the line at the bottom says where
// the rest lives, so this never grows into a second Settings dialog that has to
// be kept in step with the first.
//
// Offered, never forced — "Later" is a real button — and reachable again from
// the gear menu, because the operator who dismisses it on a Tuesday needs a way
// back to it on the Saturday.
// ---------------------------------------------------------------------------

bool MultiReplayDock::needsSetup()
{
	const Config cfg = ReplayCore::instance().getConfig();
	if (cfg.sessionFolder.empty())
		return true;
	for (int i = 0; i < kMaxCameras; i++)
		if (!cfg.cameras[i].sourceName.empty())
			return false;
	return true; // a folder but not one camera: nothing to record
}

void MultiReplayDock::runSetupWizard()
{
	auto &core = ReplayCore::instance();
	if (core.isRecording()) {
		QMessageBox::warning(this, "obs-multireplay",
				     obs_module_text("Dock.StopRecFirst"));
		return;
	}
	Config cfg = core.getConfig();

	QDialog dlg(this);
	dlg.setWindowTitle(obs_module_text("Setup.Title"));
	dlg.setMinimumWidth(660);

	auto *root = new QVBoxLayout(&dlg);
	root->setSpacing(10);
	auto *blurb = new QLabel(obs_module_text("Setup.Blurb"), &dlg);
	blurb->setWordWrap(true);
	root->addWidget(blurb);

	auto *form = new QFormLayout();
	form->setLabelAlignment(Qt::AlignLeft);
	root->addLayout(form);

	// --- 1. where the footage goes ----------------------------------------
	// Pre-filled rather than blank: an empty path is a question, a suggested
	// one is a decision the operator can accept in a second. Movies/ is where
	// every other recorder on the machine already puts things.
	auto *folderRow = new QHBoxLayout();
	auto *folder = new QLineEdit(&dlg);
	folder->setText(cfg.sessionFolder.empty()
				? QDir::toNativeSeparators(
					  QStandardPaths::writableLocation(
						  QStandardPaths::MoviesLocation) +
					  "/MultiReplay")
				: QString::fromStdString(cfg.sessionFolder));
	auto *browse = new QPushButton(obs_module_text("Setup.Browse"), &dlg);
	folderRow->addWidget(folder, 1);
	folderRow->addWidget(browse);
	form->addRow(obs_module_text("Setup.Folder"), folderRow);
	connect(browse, &QPushButton::clicked, &dlg, [&]() {
		const QString p = QFileDialog::getExistingDirectory(
			&dlg, obs_module_text("Setup.Folder"), folder->text());
		if (!p.isEmpty())
			folder->setText(QDir::toNativeSeparators(p));
	});

	// --- 2. the project ----------------------------------------------------
	auto *project = new QLineEdit(&dlg);
	project->setText(QDateTime::currentDateTime().toString("yyyyMMdd_HHmm"));
	form->addRow(obs_module_text("Setup.Project"), project);

	// --- 3. the cameras ----------------------------------------------------
	// FOUR, not eight. This is the dialog that gets somebody recording; a rig
	// with five cameras has an operator who will find Settings.
	QStringList sourceNames;
	{
		Data sd(core.sourcesJson());
		obs_data_array_t *arr =
			sd ? obs_data_get_array(sd, "sources") : nullptr;
		if (arr) {
			const size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; i++) {
				obs_data_t *it = obs_data_array_item(arr, i);
				sourceNames << QString::fromUtf8(
					obs_data_get_string(it, "name"));
				obs_data_release(it);
			}
			obs_data_array_release(arr);
		}
	}
	constexpr int kWizardCams = 4;
	std::vector<QComboBox *> camCombos;
	std::vector<QLineEdit *> camNames;
	{
		auto *grid = new QGridLayout();
		grid->setHorizontalSpacing(10);
		for (int i = 0; i < kWizardCams; i++) {
			auto *c = new QComboBox(&dlg);
			c->addItem(obs_module_text("Dock.None"), "");
			for (const auto &nm : sourceNames)
				c->addItem(nm, nm);
			const int idx = c->findData(QString::fromStdString(
				cfg.cameras[i].sourceName));
			if (idx >= 0)
				c->setCurrentIndex(idx);
			c->setMinimumWidth(150);

			auto *nm = new QLineEdit(&dlg);
			nm->setText(cfg.cameras[i].displayName.empty()
					    ? QString("C%1").arg(i + 1)
					    : QString::fromStdString(
						      cfg.cameras[i].displayName));
			nm->setFixedWidth(90);

			grid->addWidget(new QLabel(QString::number(i + 1), &dlg),
					i, 0);
			grid->addWidget(c, i, 1);
			grid->addWidget(nm, i, 2);
			camCombos.push_back(c);
			camNames.push_back(nm);
		}
		grid->setColumnStretch(1, 1);
		form->addRow(obs_module_text("Setup.Cameras"), grid);
	}

	// --- 4. where the replay goes on air -----------------------------------
	auto *outScene = new QComboBox(&dlg);
	outScene->addItem(obs_module_text("Dock.None"), "");
	{
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
		const int idx = outScene->findData(
			QString::fromStdString(cfg.outputSceneName));
		if (idx >= 0)
			outScene->setCurrentIndex(idx);
	}
	form->addRow(obs_module_text("Setup.OutputScene"), outScene);

	// --- 5. how Branch Output records --------------------------------------
	auto *recRow = new QHBoxLayout();
	auto *split = new QSpinBox(&dlg);
	split->setRange(0, 120);
	split->setValue(cfg.splitMinutes);
	split->setSuffix(obs_module_text("Setup.MinutesSuffix"));
	split->setSpecialValueText(obs_module_text("Setup.NoSplit"));
	auto *bitrate = new QSpinBox(&dlg);
	bitrate->setRange(500, 100000);
	bitrate->setSingleStep(500);
	bitrate->setValue(cfg.videoBitrateKbps);
	bitrate->setSuffix(" kbps");
	recRow->addWidget(new QLabel(obs_module_text("Setup.Split"), &dlg));
	recRow->addWidget(split);
	recRow->addSpacing(14);
	recRow->addWidget(new QLabel(obs_module_text("Setup.Bitrate"), &dlg));
	recRow->addWidget(bitrate);
	recRow->addStretch(1);
	form->addRow(obs_module_text("Setup.Recording"), recRow);

	auto *rest = new QLabel(obs_module_text("Setup.TheRest"), &dlg);
	rest->setObjectName("mrMuted");
	rest->setWordWrap(true);
	root->addWidget(rest);

	auto *bb = new QDialogButtonBox(&dlg);
	auto *save = bb->addButton(obs_module_text("Setup.Save"),
				   QDialogButtonBox::AcceptRole);
	save->setObjectName("mrAccent");
	bb->addButton(obs_module_text("BO.Later"), QDialogButtonBox::RejectRole);
	root->addWidget(bb);
	connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec() != QDialog::Accepted)
		return;

	// --- apply --------------------------------------------------------------
	cfg.sessionFolder = folder->text().trimmed().toStdString();
	for (int i = 0; i < kWizardCams; i++) {
		cfg.cameras[i].sourceName =
			camCombos[i]->currentData().toString().toStdString();
		cfg.cameras[i].displayName =
			camNames[i]->text().trimmed().toStdString();
	}
	cfg.outputSceneName = outScene->currentData().toString().toStdString();
	cfg.splitMinutes = split->value();
	cfg.videoBitrateKbps = bitrate->value();
	core.setConfig(cfg);

	// THE FOLDER FIRST, THE PROJECT SECOND. newProject() creates the project
	// UNDER the session folder and re-points the segment index at it, so it has
	// to run after setConfig has been told where that folder is — the other way
	// round puts the project under the previous one, or under nothing.
	const QString title = project->text().trimmed();
	if (!title.isEmpty()) {
		std::string err;
		if (!core.newProject(title.toStdString(), err))
			QMessageBox::warning(this, "obs-multireplay",
					     QString::fromStdString(err));
	}
	EventStore::instance().setSessionFolder(core.recordingFolder());
	ReplayChannel::instance().ensureSource();
	clearBothBays();
	refreshAngles();
	refreshEvents();
	poll();
	obs_log(LOG_INFO,
		"[dock] guided setup applied: folder %s, project %s, output scene %s",
		cfg.sessionFolder.c_str(), title.toUtf8().constData(),
		cfg.outputSceneName.c_str());
}
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

	// ── Session ───────────────────────────────────────────────────────
	// Was "Recording", and it holds the same fields plus the two numbers an
	// operator opens this dialog to check before kick-off: how much disk is left
	// and how long that is in recording time. They belong on the page that names
	// the folder they are about — reading them off the status line means waiting
	// for the status line to be showing them.
	QFormLayout *recPage = addPage("Dock.SetSession", "Dock.SetSessionBlurb");

	// Taken ONCE, here, from the same statusJson() the status line reads.
	// std::filesystem::space() on a NAS is a network round trip and that is why
	// poll() only asks four times a second — a dialog that opens a few times an
	// evening may ask, and must not grow a second copy of the syscall.
	{
		// TWO NUMBERS, TWO BOXES. They were one string — "743.2 GiB • 09:12"
		// against a right-aligned form label — and the second half had no name
		// at all: a bullet, then four digits and a colon, which reads as a
		// clock as easily as it reads as a duration. They are also not the same
		// kind of fact (one is the disk's, one is this session's at this
		// bitrate), so they get a card each, with the unit under the number and
		// the caption over it.
		QString space = obs_module_text("Dock.SessionSpaceUnknown");
		QString spaceUnit;
		QString rec = obs_module_text("Dock.SessionSpaceUnknown");
		QString recUnit;
		Data st(core.statusJson());
		if (st) {
			const int64_t freeBytes =
				obs_data_get_int(st, "diskFreeBytes");
			const int64_t mins =
				obs_data_get_int(st, "estimatedMinutesRemaining");
			if (freeBytes > 0) {
				const double gib =
					(double)freeBytes / (1024.0 * 1024 * 1024);
				space = QString::number(gib, 'f', 1);
				spaceUnit = QStringLiteral("GiB");
			}
			if (mins >= 0) {
				rec = QString::asprintf("%lld:%02lld",
							(long long)(mins / 60),
							(long long)(mins % 60));
				recUnit = obs_module_text("Dock.SessionHoursUnit");
			}
		}

		// One card: caption, big value, unit. A QFrame so the stylesheet has
		// something to draw a border on.
		const auto card = [&](const char *capKey, const QString &value,
				      const QString &unit,
				      const char *tipKey) -> QWidget * {
			auto *f = new QFrame(&dlg);
			f->setObjectName(QStringLiteral("mrStatCard"));
			f->setFrameShape(QFrame::NoFrame); // the stylesheet draws it
			f->setToolTip(obs_module_text(tipKey));
			auto *cv = new QVBoxLayout(f);
			cv->setContentsMargins(12, 8, 12, 8);
			cv->setSpacing(1);
			auto *cap = new QLabel(obs_module_text(capKey), f);
			cap->setObjectName(QStringLiteral("mrStatCaption"));
			cv->addWidget(cap);
			auto *row = new QHBoxLayout();
			row->setContentsMargins(0, 0, 0, 0);
			row->setSpacing(4);
			auto *val = new QLabel(value, f);
			val->setObjectName(QStringLiteral("mrStatValue"));
			val->setFont(QFont(monoFamily()));
			row->addWidget(val, 0, Qt::AlignBottom);
			if (!unit.isEmpty()) {
				auto *u = new QLabel(unit, f);
				u->setObjectName(QStringLiteral("mrStatUnit"));
				row->addWidget(u, 0, Qt::AlignBottom);
			}
			row->addStretch(1);
			cv->addLayout(row);
			return f;
		};

		auto *cards = new QWidget(&dlg);
		auto *ch = new QHBoxLayout(cards);
		ch->setContentsMargins(0, 0, 0, 6);
		ch->setSpacing(8);
		ch->addWidget(card("Dock.SessionSpace", space, spaceUnit,
				   "Dock.SessionSpaceHint"),
			      1);
		ch->addWidget(card("Dock.SessionHours", rec, recUnit,
				   "Dock.SessionHoursHint"),
			      1);
		// Spanning row: these are a header for the page, not a field with a
		// label on its left.
		recPage->addRow(cards);
	}

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

	// A GRID, 4 rows × 2 columns, the way the reference controller lays its inputs out — not eight
	// stacked rows. Eight rows put the eighth camera below the fold on a laptop,
	// and a rig is read as a rig: "the left column is 1-4, the right is 5-8" is
	// something the eye learns once. Each cell is the pair that describes one
	// camera: which OBS source it is, and what the operator calls it (the name
	// that ends up on his angle keys and in the table headers).
	std::vector<QComboBox *> camCombos;
	std::vector<QLineEdit *> camNameEdits;
	{
		auto *grid = new QGridLayout();
		grid->setHorizontalSpacing(14);
		grid->setVerticalSpacing(4);
		for (int i = 0; i < kMaxCameras; i++) {
			auto *c = makeSourceCombo(cfg.cameras[i].sourceName);
			c->setMinimumWidth(120);
			auto *nameEdit = new QLineEdit(
				QString::fromStdString(cfg.cameras[i].displayName),
				&dlg);
			nameEdit->setPlaceholderText(
				QString(obs_module_text("Dock.CameraName"))
					.arg(i + 1));
			nameEdit->setFixedWidth(96);
			camCombos.push_back(c);
			camNameEdits.push_back(nameEdit);

			auto *cell = new QHBoxLayout();
			cell->setContentsMargins(0, 0, 0, 0);
			cell->addWidget(new QLabel(QString("%1").arg(i + 1), &dlg));
			cell->addWidget(c, 1);
			cell->addWidget(nameEdit);
			// Down the columns, not across the rows: cameras 1-4 on the
			// left, 5-8 on the right, so the numbers read in order in the
			// direction the eye goes.
			grid->addLayout(cell, i % 4, i / 4);
		}
		camPage->addRow(grid);
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

	// IS THERE A SECOND BAY? The first thing on this page, because everything
	// below it about B is meaningless when the answer is no — and because with
	// one bay the panel has no B box, no selector and no swap key at all.
	auto *useB = new QCheckBox(&dlg);
	useB->setChecked(cfg.enableChannelB);
	useB->setToolTip(obs_module_text("Dock.EnableChannelBHint"));
	outPage->insertRow(0, obs_module_text("Dock.EnableChannelB"), useB);

	// Under A|B both bays play the same event, but Program is ONE scene, so
	// somebody has to say which bay's scene goes on air. Left to a guess it
	// would be A forever, and an operator whose B scene is the one with the
	// graphics on it has no way to say so.
	auto *abOut = new QComboBox(&dlg);
	abOut->addItem(obs_module_text("Dock.ABOutputA"), false);
	abOut->addItem(obs_module_text("Dock.ABOutputB"), true);
	abOut->setCurrentIndex(cfg.abOutputUsesB ? 1 : 0);
	abOut->setToolTip(obs_module_text("Dock.ABOutputHint"));
	outPage->addRow(obs_module_text("Dock.ABOutput"), abOut);
	// Only worth answering when there are two bays.
	abOut->setEnabled(cfg.enableChannelB);
	outSceneB->setEnabled(cfg.enableChannelB);
	connect(useB, &QCheckBox::toggled, &dlg, [abOut, outSceneB](bool on) {
		abOut->setEnabled(on);
		outSceneB->setEnabled(on);
	});

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

	// ── the event transition (the reference controller) ───────────────────────────────────
	// TWO choices and one duration, because going to the replay and coming back
	// are two moments. A stinger needs no special case: OBS transitions are
	// listed by name, and a stinger the operator built is one of the names.
	auto makeTransitionCombo = [&](const std::string &cur) {
		auto *c = new QComboBox(&dlg);
		// "(as OBS)" first: leaving the operator's own transition alone is the
		// default, and it is what this plugin did before there was a setting.
		c->addItem(obs_module_text("Dock.TransitionAsObs"), "");
		struct obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		for (size_t i = 0; i < list.sources.num; i++) {
			const char *nm =
				obs_source_get_name(list.sources.array[i]);
			if (nm && *nm)
				c->addItem(QString::fromUtf8(nm),
					   QString::fromUtf8(nm));
		}
		obs_frontend_source_list_free(&list);
		// A transition configured earlier may have been renamed or removed;
		// keep it in the list rather than silently resetting the setting (the
		// coordinator says so in the log and falls back to OBS's own).
		const QString want = QString::fromStdString(cur);
		int idx = c->findData(want);
		if (idx < 0 && !want.isEmpty()) {
			c->addItem(want, want);
			idx = c->count() - 1;
		}
		if (idx >= 0)
			c->setCurrentIndex(idx);
		return c;
	};
	auto *transIn = makeTransitionCombo(cfg.transitionInName);
	transIn->setToolTip(obs_module_text("Dock.TransitionInHint"));
	outPage->addRow(obs_module_text("Dock.TransitionIn"), transIn);

	auto *transOut = makeTransitionCombo(cfg.transitionOutName);
	transOut->setToolTip(obs_module_text("Dock.TransitionOutHint"));
	outPage->addRow(obs_module_text("Dock.TransitionOut"), transOut);

	auto *transMs = new QSpinBox(&dlg);
	transMs->setRange(0, 20000);
	transMs->setSingleStep(50);
	transMs->setSuffix(" ms");
	transMs->setValue(cfg.transitionMs);
	transMs->setToolTip(obs_module_text("Dock.TransitionMsHint"));
	outPage->addRow(obs_module_text("Dock.TransitionMs"), transMs);

	// BETWEEN TWO EVENTS of one sequence: cut, or dip through black. One
	// control rather than a mode plus a duration, the way the split length and
	// "continue past the OUT" are already written here: zero IS the cut, and it
	// says so in words at the bottom of the range instead of leaving a
	// duration that means nothing next to a switch that turned it off.
	auto *evFade = new QSpinBox(&dlg);
	evFade->setRange(0, 4000);
	evFade->setSingleStep(50);
	evFade->setSuffix(" ms");
	evFade->setSpecialValueText(obs_module_text("Dock.EventFadeCut"));
	evFade->setValue(cfg.eventFadeMs);
	evFade->setToolTip(obs_module_text("Dock.EventFadeHint"));
	outPage->addRow(obs_module_text("Dock.EventFade"), evFade);

	// SAID OUT LOUD, not discovered later: these transitions are how the replay
	// goes ON AIR. The exported highlights reel does NOT use them, and cannot
	// without re-encoding every clip — it is a stream copy, which is why the
	// export is instant and lossless. A cut between clips in the file is the
	// price of that, and the operator gets to know it here rather than after
	// exporting twenty minutes of football.
	{
		auto *note = new QLabel(obs_module_text("Dock.TransitionReelNote"),
					&dlg);
		note->setObjectName("mrSettingsBlurb");
		note->setWordWrap(true);
		outPage->addRow(QString(), note);
	}

	auto *music = makeSourceCombo(cfg.musicSourceName);
	music->setToolTip(obs_module_text("Dock.MusicSourceHint"));
	outPage->addRow(obs_module_text("Dock.MusicSource"), music);

	// ...and the FILE, which is a different job for the same word: the source is
	// what gets unmuted live, this is what the exported reel reads. A path always
	// has a file behind it, and a music source that is not a media source (a
	// browser, an audio device) has none to give.
	auto *musicRow = new QHBoxLayout();
	auto *musicFile =
		new QLineEdit(QString::fromStdString(cfg.musicFilePath), &dlg);
	musicFile->setPlaceholderText(obs_module_text("Dock.MusicFilePlaceholder"));
	musicFile->setToolTip(obs_module_text("Dock.MusicFileHint"));
	auto *musicBrowse = new QPushButton("...", &dlg);
	musicBrowse->setFixedWidth(34);
	musicRow->addWidget(musicFile, 1);
	musicRow->addWidget(musicBrowse);
	connect(musicBrowse, &QPushButton::clicked, &dlg, [&dlg, musicFile]() {
		const QString f = QFileDialog::getOpenFileName(
			&dlg, obs_module_text("Dock.MusicFile"), musicFile->text(),
			obs_module_text("Dock.MusicFileFilter"));
		if (!f.isEmpty())
			musicFile->setText(f);
	});
	outPage->addRow(obs_module_text("Dock.MusicFile"), musicRow);

	// The starting position of the panel's Mute key. On, and every replay
	// plays silent until the operator lifts it — the key never lifts itself.
	// Global, like the theme: a habit of the operator's, not of the match.
	auto *muteAudio = new QCheckBox(&dlg);
	muteAudio->setChecked(cfg.muteReplayAudio);
	muteAudio->setToolTip(obs_module_text("Dock.MuteReplayAudioHint"));
	outPage->addRow(obs_module_text("Dock.MuteReplayAudio"), muteAudio);

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

	// Keep playing past the OUT. A LENGTH, not a switch: "carry on to the end
	// of the recording" during a match means carrying on to NOW, and a goal
	// marked five minutes ago would replay five minutes of football to catch
	// up. 0 = off, and off reads as a word rather than as "0.0 s" — the same
	// specialValueText trick as the split length above.
	auto *pastOut = new QDoubleSpinBox(&dlg);
	pastOut->setRange(0.0, 60.0);
	pastOut->setSingleStep(0.5);
	pastOut->setDecimals(1);
	pastOut->setSuffix(" s");
	pastOut->setSpecialValueText(obs_module_text("Dock.ContinuePastOutOff"));
	pastOut->setValue(cfg.continuePastOutMs / 1000.0);
	pastOut->setToolTip(obs_module_text("Dock.ContinuePastOutHint"));
	evPage->addRow(obs_module_text("Dock.ContinuePastOut"), pastOut);

	auto *sortByTime = new QCheckBox(&dlg);
	sortByTime->setChecked(cfg.sortEventsByTime);
	sortByTime->setToolTip(obs_module_text("Dock.SortByTimeHint"));
	evPage->addRow(obs_module_text("Dock.SortByTime"), sortByTime);

	// Two gestures the operator may not want, both ON by default because both
	// are why they exist. Double-click is the fastest way onto Program and sits
	// two pixels from the cells he edits; "to output" is what makes a replay a
	// broadcast rather than a preview.
	auto *dblPlay = new QCheckBox(&dlg);
	dblPlay->setChecked(cfg.doubleClickPlays);
	dblPlay->setToolTip(obs_module_text("Dock.DoubleClickPlaysHint"));
	evPage->addRow(obs_module_text("Dock.DoubleClickPlays"), dblPlay);

	auto *toOut = new QCheckBox(&dlg);
	toOut->setChecked(cfg.toOutputOnPlay);
	toOut->setToolTip(obs_module_text("Dock.ToOutputOnPlayHint"));
	evPage->addRow(obs_module_text("Dock.ToOutputOnPlay"), toOut);

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

	// WHICH COLOURS THE PANEL WEARS. "Follow OBS" is the default because a
	// plugin panel should look like it belongs to the program it is docked in
	// — and because OBS's theme is already the operator's stated preference,
	// asked once.
	//
	// Only the CHROME follows. REC, the on-air band and the tally on an angle
	// key keep their own hues under every option here: an operator reads tally
	// by colour, from across a gallery, and a theme whose accent happens to be
	// green must not be able to say that green now means something else.
	auto *theme = new QComboBox(&dlg);
	theme->addItem(obs_module_text("Dock.ThemeFollowObs"), 0);
	theme->addItem(obs_module_text("Dock.ThemeBroadcast"), 1);
	theme->addItem(obs_module_text("Dock.ThemeContrast"), 2);
	theme->addItem(obs_module_text("Dock.ThemeLight"), 3);
	{
		const int idx = theme->findData(cfg.uiTheme);
		theme->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	theme->setToolTip(obs_module_text("Dock.ThemeHint"));
	uiPage->addRow(obs_module_text("Dock.Theme"), theme);

	// HOW MUCH LIST FITS ON THE SCREEN. On a panel docked down one side the
	// event list is the thing the operator reads, and at the comfortable row
	// height it showed about eight events. Three steps rather than a pixel box:
	// the row is sized from the cells it holds, so each step is a set of
	// metrics that agree with each other rather than a number that can be set
	// shorter than the text in it.
	auto *density = new QComboBox(&dlg);
	density->addItem(obs_module_text("Dock.RowsComfortable"), 0);
	density->addItem(obs_module_text("Dock.RowsCompact"), 1);
	density->addItem(obs_module_text("Dock.RowsDense"), 2);
	{
		const int idx = density->findData(cfg.tableDensity);
		density->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	density->setToolTip(obs_module_text("Dock.RowsHint"));
	uiPage->addRow(obs_module_text("Dock.Rows"), density);

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

	// VERBOSE DIAGNOSTIC LOG. Off in normal use; turned on to capture the
	// deterministic layout/resize traces when something needs reporting.
	auto *verbose = new QCheckBox(obs_module_text("Dock.VerboseLog"), &dlg);
	verbose->setChecked(cfg.verboseLog);
	verbose->setToolTip(obs_module_text("Dock.VerboseLogHint"));
	advPage->addRow(QString(), verbose);

	// ── Updates ───────────────────────────────────────────────────────
	// Deliberately the LAST page, and deliberately a page rather than a
	// button somewhere: an operator comes here on purpose, between matches,
	// which is the only time updating a recording tool is a sensible idea.
	QFormLayout *updPage = addPage("Dock.SetUpdates", "Dock.SetUpdatesBlurb");

	auto *verLbl = new QLabel(QString::fromStdString(Updater::currentVersion()),
				  &dlg);
	updPage->addRow(obs_module_text("Dock.UpdateInstalled"), verLbl);

	auto *chan = new QComboBox(&dlg);
	chan->addItem(obs_module_text("Dock.ChannelStable"), "stable");
	chan->addItem(obs_module_text("Dock.ChannelBeta"), "beta");
	{
		const int idx = chan->findData(
			QString::fromStdString(cfg.updateChannel));
		chan->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	updPage->addRow(obs_module_text("Dock.UpdateChannel"), chan);

	// THE WARNING IS PART OF THE CONTROL, not a footnote elsewhere. It only
	// appears when beta is chosen, because a caution shown permanently is a
	// caution nobody reads on the day it applies.
	auto *betaWarn = new QLabel(obs_module_text("Dock.BetaWarning"), &dlg);
	betaWarn->setObjectName("mrSettingsBlurb");
	betaWarn->setWordWrap(true);
	betaWarn->setVisible(cfg.updateChannel == "beta");
	updPage->addRow(QString(), betaWarn);
	connect(chan, &QComboBox::currentIndexChanged, betaWarn,
		[chan, betaWarn](int) {
			betaWarn->setVisible(chan->currentData().toString() ==
					     "beta");
		});

	auto *updStatus = new QLabel(&dlg);
	updStatus->setWordWrap(true);
	auto *notes = new QPlainTextEdit(&dlg);
	notes->setReadOnly(true);
	notes->setMinimumHeight(140);
	notes->setPlaceholderText(obs_module_text("Dock.UpdateNoNotes"));
	auto *checkBtn = new QPushButton(obs_module_text("Dock.UpdateCheck"), &dlg);
	auto *getBtn = new QPushButton(obs_module_text("Dock.UpdateDownload"), &dlg);
	auto *installBtn =
		new QPushButton(obs_module_text("Dock.UpdateInstall"), &dlg);
	getBtn->setEnabled(false);
	installBtn->setEnabled(false);

	auto *btnRow = new QWidget(&dlg);
	auto *btnLay = new QHBoxLayout(btnRow);
	btnLay->setContentsMargins(0, 0, 0, 0);
	btnLay->addWidget(checkBtn);
	btnLay->addWidget(getBtn);
	btnLay->addWidget(installBtn);
	btnLay->addStretch(1);
	updPage->addRow(QString(), btnRow);
	updPage->addRow(obs_module_text("Dock.UpdateStatus"), updStatus);
	updPage->addRow(obs_module_text("Dock.UpdateChangelog"), notes);

	// The dialog polls the updater rather than the updater calling back into
	// the dialog: the worker lives on its own thread and the dialog can be
	// closed at any moment, and a callback into a dead widget is the kind of
	// crash that only happens in front of someone.
	auto *updTimer = new QTimer(&dlg);
	updTimer->setInterval(250);
	connect(updTimer, &QTimer::timeout, &dlg, [=]() {
		const Updater::Status st = Updater::instance().status();
		QString text;
		switch (st.phase) {
		case Updater::Phase::Idle:
			text = obs_module_text("Dock.UpdateIdle");
			break;
		case Updater::Phase::Checking:
			text = obs_module_text("Dock.UpdateChecking");
			break;
		case Updater::Phase::UpToDate:
			text = obs_module_text("Dock.UpdateUpToDate");
			break;
		case Updater::Phase::Available:
			text = QString(obs_module_text("Dock.UpdateAvailable"))
				       .arg(QString::fromStdString(
					       st.release.version));
			break;
		case Updater::Phase::Downloading:
			text = QString(obs_module_text("Dock.UpdateDownloading"))
				       .arg(st.percent);
			break;
		case Updater::Phase::Staged:
			text = QString(obs_module_text("Dock.UpdateStaged"))
				       .arg(QString::fromStdString(
					       st.release.version));
			break;
		case Updater::Phase::Failed:
			text = QString(obs_module_text("Dock.UpdateFailed"))
				       .arg(QString::fromStdString(st.message));
			break;
		}
		if (updStatus->text() != text)
			updStatus->setText(text);
		const QString body = QString::fromStdString(st.release.notes);
		if (notes->toPlainText() != body)
			notes->setPlainText(body);
		checkBtn->setEnabled(st.phase != Updater::Phase::Checking &&
				     st.phase != Updater::Phase::Downloading);
		getBtn->setEnabled(st.phase == Updater::Phase::Available);
		// A2: only where there IS a helper. An enabled "Install" that
		// returns false is how the platforms without one used to report
		// themselves — after the download, in a dialog.
		installBtn->setEnabled(st.phase == Updater::Phase::Staged &&
				       Updater::canInstallHere());
	});
	updTimer->start();

	connect(checkBtn, &QPushButton::clicked, &dlg, [chan]() {
		Updater::instance().checkAsync(updateChannelFromString(
			chan->currentData().toString().toStdString()));
	});
	connect(getBtn, &QPushButton::clicked, &dlg,
		[]() { Updater::instance().downloadAsync(); });
	connect(installBtn, &QPushButton::clicked, &dlg, [this, &dlg]() {
		// SAID OUT LOUD BEFORE IT HAPPENS. This arms something that
		// replaces the plugin the moment OBS closes, and an operator who
		// did not expect that would find a different build running at the
		// next match.
		QMessageBox box(&dlg);
		box.setWindowTitle("obs-multireplay");
		box.setText(obs_module_text("Dock.UpdateInstallConfirm"));
		QPushButton *yes = box.addButton(obs_module_text("Dock.Yes"),
						  QMessageBox::YesRole);
		box.addButton(obs_module_text("Dock.No"), QMessageBox::NoRole);
		box.exec();
		if (box.clickedButton() != yes)
			return;
		std::string err;
		if (Updater::instance().installStaged(err)) {
			QMessageBox::information(
				&dlg, "obs-multireplay",
				obs_module_text("Dock.UpdateInstallArmed"));
			return;
		}
		// Nothing to apologise for on the platforms that have no helper:
		// the archive is downloaded and where it is, is the answer.
		const Updater::Status st = Updater::instance().status();
		QMessageBox::information(
			&dlg, "obs-multireplay",
			err.empty()
				? QString(obs_module_text("Dock.UpdateManual"))
					  .arg(QString::fromStdString(
						  st.stagedPath))
				: QString::fromStdString(err));
	});

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
	cfg.continuePastOutMs = (int)std::lround(pastOut->value() * 1000.0);
	cfg.doubleClickPlays = dblPlay->isChecked();
	cfg.toOutputOnPlay = toOut->isChecked();
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
	cfg.verboseLog = verbose->isChecked();
	cfg.updateChannel = chan->currentData().toString().toStdString();
	cfg.outputSceneName = outScene->currentData().toString().toStdString();
	cfg.outputSceneNameB = outSceneB->currentData().toString().toStdString();
	cfg.enableChannelB = useB->isChecked();
	cfg.abOutputUsesB = abOut->currentData().toBool();
	cfg.musicSourceName = music->currentData().toString().toStdString();
	cfg.musicFilePath = musicFile->text().trimmed().toStdString();
	cfg.muteReplayAudio = muteAudio->isChecked();
	cfg.transitionInName = transIn->currentData().toString().toStdString();
	cfg.transitionOutName = transOut->currentData().toString().toStdString();
	cfg.transitionMs = transMs->value();
	cfg.eventFadeMs = evFade->value();
	cfg.autoSwitchScene = autoSwitch->isChecked();
	cfg.fitReplayToCanvas = fitCanvas->isChecked();
	cfg.showMultiview = multiview->isChecked();
	cfg.uiTheme = theme->currentData().toInt();
	cfg.tableDensity = density->currentData().toInt();
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
	// Same: move the panel's Mute key to the new default now. Its toggled
	// handler only calls setMuted(), never setConfig(), so there is no loop.
	if (muteBtn_ && muteBtn_->isChecked() != cfg.muteReplayAudio)
		muteBtn_->setChecked(cfg.muteReplayAudio);
	// The colours, immediately: a theme picked in a dialog and applied on the
	// next restart is a setting the operator cannot judge.
	applyTheme();
	refreshAngles();
	refreshEvents();
}

void MultiReplayDock::importTags()
{
	// Refused during a take, and the message says why rather than leaving the
	// menu item dead: this writes the config, and setConfig() re-points the
	// SegmentIndex and re-creates the Branch Output filters.
	if (ReplayCore::instance().isRecording()) {
		QMessageBox::warning(this, "obs-multireplay",
				     obs_module_text("Dock.StopRecFirst"));
		return;
	}
	const QString path = QFileDialog::getOpenFileName(
		this, obs_module_text("Dock.TagsImport"), QString(),
		obs_module_text("Dock.TagsFileFilter"));
	if (path.isEmpty())
		return;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QMessageBox::warning(this, "obs-multireplay",
				     obs_module_text("Dock.TagsReadFailed"));
		return;
	}
	// One tag per line, blank lines dropped, order kept: the order IS the order
	// of the drop-down, so the tags an operator reaches for during a match stay
	// where he put them.
	std::vector<std::string> tags;
	while (!f.atEnd()) {
		const QString line = QString::fromUtf8(f.readLine()).trimmed();
		if (!line.isEmpty())
			tags.push_back(line.toStdString());
	}
	f.close();
	if (tags.empty()) {
		showNotice(obs_module_text("Dock.TagsEmptyFile"));
		return;
	}

	auto &core = ReplayCore::instance();
	Config cfg = core.getConfig();
	// REPLACES rather than merges. A tag list is a vocabulary, and merging two
	// of them leaves the operator with a drop-down he did not write and cannot
	// tell apart; exporting first is one menu item away.
	cfg.commentPresets = std::move(tags);
	core.setConfig(cfg);
	refreshEvents();
	showNotice(QString(obs_module_text("Dock.TagsImported"))
			   .arg((int)cfg.commentPresets.size()));
	obs_log(LOG_INFO, "[dock] imported %d tag(s) from %s",
		(int)cfg.commentPresets.size(), path.toUtf8().constData());
}

void MultiReplayDock::exportTags()
{
	const Config cfg = ReplayCore::instance().getConfig();
	if (cfg.commentPresets.empty()) {
		showNotice(obs_module_text("Dock.TagsNoneToExport"));
		return;
	}
	QString path = QFileDialog::getSaveFileName(
		this, obs_module_text("Dock.TagsExport"),
		QStringLiteral("tags.txt"),
		obs_module_text("Dock.TagsFileFilter"));
	if (path.isEmpty())
		return;
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		QMessageBox::warning(this, "obs-multireplay",
				     obs_module_text("Dock.TagsWriteFailed"));
		return;
	}
	for (const auto &t : cfg.commentPresets) {
		f.write(QString::fromStdString(t).toUtf8());
		f.write("\n");
	}
	f.close();
	showNotice(QString(obs_module_text("Dock.TagsExported"))
			   .arg((int)cfg.commentPresets.size()));
	obs_log(LOG_INFO, "[dock] exported %d tag(s) to %s",
		(int)cfg.commentPresets.size(), path.toUtf8().constData());
}

void MultiReplayDock::newProjectDialog()
{
	if (ReplayCore::instance().isRecording()) {
		QMessageBox::warning(this, "obs-multireplay",
				     obs_module_text("Dock.StopRecFirst"));
		return;
	}
	// PRE-FILLED WITH THE MOMENT IT IS BEING CREATED. A project needs a name
	// before it can exist, and on a match day the honest one is when it was
	// recorded — typed by hand it is one more thing to do while the teams are
	// warming up, and left to the operator's imagination it produces "Test",
	// "Test2", "Provola". Sortable by name because the format is
	// year-month-day, which is the order they will be looked for in. It is a
	// starting point, not a rule: the field is selected so a real name simply
	// replaces it.
	bool ok;
	const QString suggested =
		QDateTime::currentDateTime().toString("yyyyMMdd_HHmm");
	QString title = QInputDialog::getText(
		this, obs_module_text("Dock.NewProject"),
		obs_module_text("Dock.ProjectNameLabel"), QLineEdit::Normal,
		suggested, &ok);
	if (!ok || title.trimmed().isEmpty())
		return;
	std::string err;
	if (!ReplayCore::instance().newProject(title.trimmed().toStdString(),
					       err)) {
		QMessageBox::warning(this, "obs-multireplay",
				     QString::fromStdString(err));
		return;
	}
	clearBothBays();
	refreshEvents();
	poll();
}

void MultiReplayDock::clearBothBays()
{
	// A NEW PROJECT IS EMPTY ON BOTH BAYS. A only looked empty by accident —
	// the big preview refuses to draw the replay input until something has
	// been captured — while B's box asked the channel whether it had ever
	// pushed a frame, and the channel still remembered the previous project's
	// clip. So the operator created a project and B carried on showing footage
	// that no longer belonged to anything.
	for (int i = 0; i < kChannels; i++) {
		PlaybackCoordinator::instance((Which)i).stopEvents();
		ReplayChannel::instance((Which)i).reset();
	}
	playheadNs_ = kNoInstant;
	prevSequenceActive_ = false;
	takeAnchorNs_ = kNoInstant;
	diskSpans_.clear();
	timeline_.setSpans({});
	displayDurNs_ = 0;
	ReplayCore::instance().setFollowLive(true);
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
	// Both bays first: whatever they were showing belonged to the project being
	// left, and it must not survive into this one.
	clearBothBays();
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

// Moved here with the rest of the project/list dialogs — it lived between
// refreshListNames() and refreshAngles() in the pre-split file, isolated
// from the rest of this group by everything now in dock-poll.cpp.
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

} // namespace multireplay
