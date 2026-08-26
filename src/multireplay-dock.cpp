/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multireplay-dock.hpp"
#include "dock-layout.hpp"
#include "dock-style.hpp"
#include "dock-icons.hpp"
#include "qt-display.hpp"
#include "branch-output-install.hpp"
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
// Dock-wide stylesheet — the broadcast replay controller's palette.
//
// The colours are SAMPLED from the reference screenshots in "reference-gui/", not
// invented: an operator coming from the reference controller reads this panel by colour before he
// reads it by label, and "green means the angle I am on, orange means the row I
// have selected" is muscle memory worth more than any house style. Nothing here

namespace {

// THE SCHEME, for the two widgets that PAINT rather than being styled.
//
// A style sheet cannot reach inside a QPainter, so the position bar and the
// on-air band read their colours from here. Both their paintEvents and the one
// thing that writes it (MultiReplayDock::applyTheme) run on the GUI thread, so
// a plain object needs no guard.
//
// A FUNCTION-LOCAL STATIC, not a file-scope one: schemeFor() builds QColors and
// touches a QPalette, and a file-scope object would do that during static
// initialisation — before QApplication exists.
Scheme &sc()
{
	static Scheme s = schemeFor(ThemeChoice::Broadcast, QPalette());
	return s;
}

// ── KEYS, MARKS AND IDENTITIES ───────────────────────────────────────────
//
// Three things every command key on this panel needs and one place that gives
// it all three: the mark (drawn, not a glyph from whatever font had one — see
// dock-icons.hpp), the tooltip that says what the mark means, and the STABLE ID
// the automated gate finds it by.
//
// The id is the part worth insisting on. The gate used to find twelve of these
// keys by their literal text — "⏭", "■", "-5s" — which made every one of them a
// tripwire under the wrong thing: a label is a translation and a glyph is a
// drawing, and neither is the key's identity. An icon-first panel would have
// broken all twelve at once while the keys themselves worked perfectly.
QPushButton *iconBtn(Icon ic, const char *id, const QString &tip,
		     QWidget *parent, const char *role = "mrTransport")
{
	auto *b = new QPushButton(parent);
	b->setObjectName(QString::fromLatin1(role));
	b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setKeyIcon(b, ic, tintsFor(sc()));
	setKeyId(b, QString::fromLatin1(id));
	return b;
}

// A key that is a mark AND a word — REC, the exports. Qt under-measures a
// stylesheet-styled button carrying both (the sheet's padding is not in
// sizeHint), so "Monitors" shipped with its last letter cut off; Fixed leaves
// the layout nothing to squeeze. Inside a section KeyBlock promotes that back
// to Preferred for any cell that declared grow, so a key meant to fill its row
// still does.
QPushButton *iconTextBtn(Icon ic, const QString &text, const char *id,
			 QWidget *parent, const char *role = "", int px = 14)
{
	auto *b = new QPushButton(text, parent);
	if (role && *role)
		b->setObjectName(QString::fromLatin1(role));
	b->setCursor(Qt::PointingHandCursor);
	setKeyIcon(b, ic, tintsFor(sc()), px);
	setKeyId(b, QString::fromLatin1(id));
	b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	return b;
}

// ── A MODE, ON THE STATUS LINE ───────────────────────────────────────────
//
// Loop, music and "in output" are the modes a replay runs UNDER, and the status
// line owns them rather than mirroring them: a bar that repeated four toggles
// which were also keys in the strip would be four states with two homes, which
// is exactly how a toggle ends up left in the wrong position.
//
// Shorter than a key in the strip because the line is shorter, and because
// nothing here is reached for blind — these are pressed while being looked at.
// Lit they are a STATE: hollow, not filled. The one filled key on this panel is
// the one that takes the Program.
QPushButton *statusToggle(Icon ic, const QString &text, const char *id,
			  const QString &tip, QWidget *parent)
{
	auto *b = new QPushButton(text, parent);
	b->setObjectName(QStringLiteral("mrStatKey"));
	b->setCheckable(true);
	b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setKeyIcon(b, ic, tintsFor(sc()), 12);
	setKeyId(b, QString::fromLatin1(id));
	return b;
}

// A scheme colour at partial opacity. The hairlines and the playhead used a
// hardcoded white with alpha, which is a line that vanishes on a light theme.
QColor tint(const QString &hex, int alpha)
{
	QColor c(hex);
	c.setAlpha(alpha);
	return c;
}

// The live dock, so the module's frontend-event handler can reach it when OBS is
// about to clear scene data (see releasePreviewRefs). One dock at a time: it is
// registered by id and OBS builds exactly one.
MultiReplayDock *g_dock = nullptr;

constexpr int kNCams = kMaxCameras; // 8
// multireplay-dock.hpp cannot see kMaxCameras (it does not include replay-core),
// so the angle-key array is spelled 8 there. If that ever diverges the keys
// would silently stop at the wrong camera.
static_assert(kMaxCameras == 8, "kMaxPreviewTiles in the header is sized for 8");

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

// this panel that are allowed to shout, the green band and the REC key.
QWidget *zoneBox(const QString &title, QWidget *content, QWidget *parent)
{
	auto *box = new QFrame(parent);
	box->setObjectName(QStringLiteral("mrZone"));
	box->setFrameShape(QFrame::NoFrame); // the stylesheet draws it
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(6, 2, 6, 3);
	v->setSpacing(2);

	auto *cap = new QLabel(title.toUpper(), box);
	cap->setObjectName(QStringLiteral("mrZoneTitle"));
	// NO width cap and NO wrapping. Capped at 34 px the captions were simply
	// cut ("IN ONDA", "TIMELINE" — a label that has to be guessed is worse
	// than no label), and wrapping cannot help a single word.
	cap->setWordWrap(false);
	cap->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
	cap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	v->addWidget(cap, 0);
	v->addWidget(content, 1);

	// The floor that keeps the panel from folding. Asked of the content
	// rather than assumed, so it stays true when a band re-flows onto a
	// second row or the operator's font is larger than ours.
	const int need = content->minimumSizeHint().height() > 0
				 ? content->minimumSizeHint().height()
				 : content->sizeHint().height();
	// Margins (2 + 3) + spacing (2) is 7, and 7 exactly is what clipped the
	// bottom border of whatever sat in the zone: a minimum with no slack in
	// it has to be right to the pixel on every font and every DPI, and it is
	// not. Two spare pixels cost nothing the splitter cannot give.
	box->setMinimumHeight(cap->sizeHint().height() + need + 9);
	box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	return box;
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
// 14 px strip left 9 px for a number and clipped the digits — the scale was
// there but unreadable, which is the same as not being there. 20 leaves 15,
// which the small font fits at any DPI.
constexpr int kSeekRulerH = 20;
// Track thickness. Wide enough to hold the event markers and the timecode the
// way it always did.
constexpr int kSeekTrackH = 20;

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

// The two repaint censuses (see RepaintCensus in the header).
RepaintCensus g_seekCensus;
RepaintCensus g_clipCensus;

namespace {

// How long a deferred repaint may wait before it is flushed. ONE FRAME.
//
// This was 100 ms for one build, on the reasoning that the graduations and the
// markers shift by a third of a pixel per tick while recording and that nobody
// could see the difference between redrawing them at 30 Hz and at 10 Hz. That
// reasoning was wrong in the only way that counts: the panel was watched, and
// the position bar and the green band's fill were reported as less smooth than
// before. Smoothness of the thing an operator stares at while a replay runs IS
// behaviour, not overhead, and this file does not get to trade it away.
//
// At 16 ms the delay is under a frame at 60 Hz, so nothing is visibly deferred.
// What it still buys is the only thing it was ever needed for: several changes
// arriving inside the SAME poll tick — the overlay timecode, the markers, a pan
// under the playhead — become one repaint instead of three. The storm this
// whole mechanism exists to stop was never the repaints that follow a real
// change; it was the thirty a second that followed no change at all, and those
// are stopped by the comparisons in the setters, not here.
constexpr int kCoalesceMs = 16;

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

	coalesceTimer_ = new QTimer(this);
	coalesceTimer_->setSingleShot(true);
	coalesceTimer_->setInterval(kCoalesceMs);
	connect(coalesceTimer_, &QTimer::timeout, this, [this] { flushPending(); });
}

QRect SeekBar::trackBand() const
{
	// The track plus the 1 px above it and the top row of the ruler, which
	// the playhead and the marker edges bleed into.
	return QRect(0, 0, width(), kSeekTrackH + 2);
}

void SeekBar::repaintNow(const QRect &r)
{
	if (r.isEmpty())
		return;
	// Anything already deferred is folded in and flushed with it: two
	// repaints of overlapping rectangles in the same millisecond are one
	// repaint, and leaving the deferred one behind would draw a bar that is
	// half new and half old.
	QRect u = r;
	if (!pendingRect_.isNull()) {
		u = u.united(pendingRect_);
		pendingRect_ = QRect();
		coalesceTimer_->stop();
	}
	g_seekCensus.requested++;
	update(u);
}

void SeekBar::repaintSoon(const QRect &r)
{
	if (r.isEmpty())
		return;
	if (pendingRect_.isNull()) {
		pendingRect_ = r;
		coalesceTimer_->start(); // restarts at kCoalesceMs
	} else {
		// Already waiting: widen the rect and let the timer that is
		// already running deliver it. Restarting it here would let a
		// change arriving every 33 ms postpone the repaint forever.
		pendingRect_ = pendingRect_.united(r);
		g_seekCensus.coalesced++;
	}
}

void SeekBar::flushPending()
{
	if (pendingRect_.isNull())
		return;
	const QRect r = pendingRect_;
	pendingRect_ = QRect();
	g_seekCensus.requested++;
	update(r);
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
	repaintSoon(allOfIt());
}

// THE PLAYHEAD. This is the one thing on the bar that has to answer at 30 Hz,
// and it is also the one that used to cost a full-width repaint to move by a
// pixel — because the setter simply called update() and had nothing to compare
// against. Two things fixed that, and both matter:
//
//  - the comparison is made in PIXELS, not in fractions. While recording, the
//    timeline grows every tick, so positionFrac is never twice the same number
//    and a comparison on it would suppress precisely nothing while the bar sat
//    still. What the operator sees is the pixel.
//  - the dirty rect is the band the hairline moved ACROSS, not the widget. At
//    30 Hz that is a handful of pixels, so what Qt re-composes and flushes out
//    of the OBS main window's backing store is a handful of pixels.
void SeekBar::setProgress(double positionFrac, double seekableFrac)
{
	const double p = std::clamp(positionFrac, 0.0, 1.0);
	const double s = std::clamp(seekableFrac, 0.0, 1.0);
	if (dragging_) {
		// The bar is following the hand, not the engine: mouseMoveEvent
		// owns the repaint while a gesture is in progress.
		positionFrac_ = p;
		seekableFrac_ = s;
		return;
	}

	const int oldX = lastDrawnPlayheadX_;
	const bool seekableMoved = std::abs(s - seekableFrac_) > 0.0005;
	positionFrac_ = p;
	seekableFrac_ = s;
	const int newX = xForFraction(p);

	if (newX == oldX && !seekableMoved) {
		g_seekCensus.suppressed++;
		return;
	}
	lastDrawnPlayheadX_ = newX;

	if (seekableMoved) {
		// The dark "you cannot go here" region changed: that is the whole
		// track, and it is not a per-tick event.
		repaintSoon(trackBand());
		return;
	}

	// The hairline is 2 px (3 while dragging) and the fill edge sits under
	// it; 4 px of slack each side covers both plus rounding. Full HEIGHT
	// because the playhead runs down through the ruler as well.
	const int lo = std::min(oldX == INT_MIN ? newX : oldX, newX) - 4;
	const int hi = std::max(oldX == INT_MIN ? newX : oldX, newX) + 4;
	repaintNow(QRect(lo, 0, hi - lo, height()));
}

void SeekBar::setEventMarkers(const std::vector<std::pair<double, double>> &markers)
{
	// Handed over on EVERY tick of a 30 Hz poll. Outside a take the answer is
	// always "the same as last time", which is the state the panel spends most
	// of a session in and the state it was repainting itself to death in.
	if (markers == markers_) {
		g_seekCensus.suppressed++;
		return;
	}
	markers_ = markers;
	// Markers can appear anywhere on the bar, so this one really is the whole
	// widget. Deferred only far enough to join the other changes arriving in
	// the same tick (see kCoalesceMs) — not far enough to be seen.
	repaintSoon(allOfIt());
}

void SeekBar::setOverlayText(const QString &text)
{
	if (overlay_ == text) {
		g_seekCensus.suppressed++;
		return;
	}
	overlay_ = text;
	// Hundredths of a second: this changes on nearly every tick while a clip
	// runs. It is drawn centred ON THE TRACK, so the ruler and its labels —
	// the expensive half of a repaint — are not involved.
	repaintSoon(trackBand());
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
	// The window changed: every graduation, every label and every marker
	// moves. Whole widget, and immediate — this one is a gesture, and a
	// zoom that lands a tenth of a second late feels broken.
	repaintNow(allOfIt());
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
	// Panning under a moving playhead: the whole widget changes, but this is
	// driven by poll() at 30 Hz, so it is deferred like everything else on
	// that beat. The playhead itself keeps its own immediate path.
	repaintSoon(allOfIt());
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
	repaintNow(allOfIt()); // a gesture: it has to land under the hand
	e->accept();
}

void SeekBar::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	// The pixel the playhead was last drawn at is the thing setProgress
	// compares against, and it means nothing once the bar is a different
	// width. Left stale, it would suppress the first real move after every
	// resize — a bar that comes back from a dock drag with a frozen playhead.
	lastDrawnPlayheadX_ = INT_MIN;
	pendingRect_ = QRect(); // Qt repaints the whole widget after a resize
	if (coalesceTimer_)
		coalesceTimer_->stop();
}

void SeekBar::paintEvent(QPaintEvent *)
{
	g_seekCensus.served++;
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
		p.setBrush(QColor(sc().sink1));
		p.drawRect(QRect(m, y, w, h + kSeekRulerH));
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(sc().border), 1));
		p.drawRect(QRect(m, y, w - 1, h + kSeekRulerH - 1));
		if (!emptyHint_.isEmpty()) {
			QFont f = p.font();
			f.setBold(true);
			p.setFont(f);
			p.setPen(QColor(sc().textMuted));
			p.drawText(QRect(m + 6, y, w - 12, h + kSeekRulerH),
				   Qt::AlignCenter, emptyHint_);
		}
		return;
	}

	// Track (the part of the timeline behind/ahead of the playhead)
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(sc().sink2));
	p.drawRect(QRectF(m, y, w, h));

	// Anything outside the seekable region is darker still: it is not a place
	// the operator can go, and painting it like the rest would say it is.
	if (seekableFrac_ < 1.0) {
		p.setBrush(QColor(sc().panel));
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
		p.setBrush(QColor(sc().seekBar));
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
		p.setBrush(QColor(sc().panel));
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
			p.setPen(QPen(tint(sc().text, 0x38), 1));
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
	p.setPen(QPen(tint(sc().text, dragging_ ? 0xff : 0xc0),
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
		// The drop shadow is the BAR's own colour, not black: this text is
		// drawn over the played band and the event markers, so what it
		// needs is separation from whatever is behind it — and on a light
		// theme a black shadow under dark text is a dark halo rather than
		// a shadow.
		p.setPen(tint(sc().sink2, 0xd0));
		p.drawText(tr.adjusted(1, 1, 1, 1), Qt::AlignCenter, overlay_);
		p.setPen(QColor(sc().text));
		p.drawText(tr, Qt::AlignCenter, overlay_);
	}
}

int SeekBar::xForFraction(double frac) const
{
	const int m = 2;
	const int w = std::max(1, width() - 2 * m);
	return m + (int)std::lround(w * std::clamp(toView(frac), 0.0, 1.0));
}

void SeekBar::setEventMarkerIds(const std::vector<int> &ids)
{
	// No repaint of its own — ids are not drawn, they are what makes an edge
	// draggable. The comparison is still worth making: this is handed over on
	// every tick, and reassigning a vector thirty times a second is an
	// allocation thirty times a second for a list that changes when the
	// operator marks something.
	if (ids != markerIds_)
		markerIds_ = ids;
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
		repaintNow(allOfIt());
		return;
	}
	dragMarker_ = -1;
	dragging_ = true;
	// Clamp to seekable region so the user can't drag into unindexed territory.
	dragFrac_ = std::min(fracAt(e->pos().x()), seekableFrac_);
	emit scrubStateChanged(true);
	emit scrubMoved(dragFrac_);
	repaintNow(allOfIt());
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
		repaintNow(allOfIt());
		return;
	}
	emit scrubMoved(dragFrac_);
	repaintNow(allOfIt());
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
		repaintNow(allOfIt());
		return;
	}
	positionFrac_ = dragFrac_;
	emit seekRequested(dragFrac_);
	emit scrubStateChanged(false);
	repaintNow(allOfIt());
}

// ---------------------------------------------------------------------------
// ClipBar — the green on-air band (see the header for what it is FOR)
// ---------------------------------------------------------------------------

ClipBar::ClipBar(QWidget *parent) : QWidget(parent)
{
	// 24, and the >> key on it is 18: a stylesheet min-height LARGER than the
	// widget's own height makes the style draw a taller frame than the widget
	// owns and the bottom border lands outside it.
	// 28, NOT 24. The skip key sits ON this band and is pinned to four less
	// than it; at 20 px the frame its style draws is 20 as well, so the bottom
	// border landed on the widget edge and any rounding put it past. Same
	// arithmetic as the status line above it, same four pixels of headroom.
	setFixedHeight(28);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	// Deliberately NOT a pointing-hand cursor and deliberately not clickable:
	// the bar directly under it IS clickable, and a bar that looks draggable
	// but is not is worse than one that looks inert.

	coalesceTimer_ = new QTimer(this);
	coalesceTimer_->setSingleShot(true);
	coalesceTimer_->setInterval(kCoalesceMs);
	connect(coalesceTimer_, &QTimer::timeout, this, [this] {
		if (!pending_)
			return;
		pending_ = false;
		g_clipCensus.requested++;
		update();
	});
}

void ClipBar::repaintSoon()
{
	if (pending_) {
		g_clipCensus.coalesced++;
		return;
	}
	pending_ = true;
	coalesceTimer_->start();
}

void ClipBar::setState(double progressFrac, const QString &text, bool onAir,
		       const std::vector<double> &clipJoins)
{
	const double p = std::clamp(progressFrac, 0.0, 1.0);
	if (std::abs(p - progress_) < 0.0005 && text == text_ &&
	    onAir == onAir_ && clipJoins == joins_) {
		g_clipCensus.suppressed++;
		return; // 30 times a second, most ticks change nothing
	}
	progress_ = p;
	text_ = text;
	onAir_ = onAir;
	joins_ = clipJoins;
	// Something really did change: the fill moved or the text did. It is
	// deferred by at most one frame, purely so that a tick which changes both
	// costs one repaint rather than two — the fill an operator watches cross
	// this band must not lose a single step of it.
	repaintSoon();
}

void ClipBar::paintEvent(QPaintEvent *)
{
	g_clipCensus.served++;
	QPainter p(this);
	const int m = 2;
	const int h = height() - 2;
	const int y = 1;
	const int w = width() - 2 * m;

	// Track, then the played part of THIS clip. Bright while it is on air,
	// muted while the bar is only describing what would play: "is something
	// on air" must be answerable without reading a word.
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(sc().onAirDim));
	p.drawRect(QRectF(m, y, w, h));
	if (progress_ > 0.0) {
		p.setBrush(onAir_ ? QColor(sc().onAir)
				  : QColor(sc().onAirDim));
		p.drawRect(QRectF(m, y, w * progress_, h));
	}

	// The joins between the clips of the sequence. White, thin, full height:
	// they are a scale, not another fill, and the operator has to be able to
	// count "three more angles to go" without reading anything.
	if (!joins_.empty()) {
		p.setPen(QPen(tint(sc().text, 0xcc), 1));
		for (double j : joins_) {
			if (j <= 0.0 || j >= 1.0)
				continue;
			const int x = m + (int)(w * j);
			p.drawLine(x, y, x, y + h - 1);
		}
		p.setPen(Qt::NoPen);
	}

	if (text_.isEmpty())
		return;
	QFont f = p.font();
	f.setBold(true);
	p.setFont(f);
	// The right end belongs to the >> key, which is a child of this widget:
	// centring the text across the whole width would run it under the button.
	int rightGap = 12;
	for (const QObject *o : children())
		if (auto *cw = qobject_cast<const QWidget *>(o))
			if (cw->isVisible())
				rightGap = std::max(rightGap, cw->width() + 12);
	const QRectF tr(m + 6, y, w - 6 - rightGap, h);
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
	// WIDTH is the only hard floor. A hard minimum HEIGHT is what let the
	// panel be dragged shorter than its own controls need: an explicit
	// minimumSize wins over the layout's computed one, so Qt happily handed
	// out 340 px and the QVBoxLayout paid for it by squeezing every child
	// towards a minimum of nothing — buttons a few pixels tall that cannot be
	// hit. With the height left to the layout (and the control bands pinned at
	// Minimum, see buildBottomBar) the panel simply refuses to go shorter than
	// the keys, and the picture above is what gives way instead.
	// THE FLOOR OF A VERTICAL DOCK. It was 560, and an OBS dock down one side
	// is 300-400 px wide: the panel simply could not be put there. The number
	// was never a design decision either — it was the width of the WIDE
	// arrangement of the widest section, demanded at every size, including the
	// sizes at which the strip would have folded instead (see
	// ControlStrip::minimumSizeHint).
	setMinimumWidth(300);
	// THE SHEET IS BUILT, NOT A CONSTANT. It carries the operator's theme choice
	// and the colours OBS is currently themed with; applyTheme() is called again
	// whenever either changes. A first pass here, before any child exists, so
	// every widget is polished into the right colours as it is created.
	{
		const Config &c0 = ReplayCore::instance().getConfig();
		sc() = schemeFor((ThemeChoice)c0.uiTheme, qApp->palette());
		setStyleSheet(dockStyle(sc(), c0.tableDensity));
	}

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
		// THE LEFT COLUMN. In the wide and column arrangements it holds
		// only the pictures and the splitter divides height. In the SHORT
		// one — docked under the OBS preview, wide and shallow — the
		// splitter divides WIDTH instead, the list takes the right-hand
		// half, and the keys, the on-air band and the position bar come
		// down here to stack under the pictures. It is the only shape in
		// which a 340 px tall dock still shows a usable table.
		leftCol_ = new QWidget(this);
		leftColLayout_ = new QVBoxLayout(leftCol_);
		leftColLayout_->setContentsMargins(0, 0, 0, 0);
		leftColLayout_->setSpacing(3);
		leftColLayout_->addWidget(buildPreview(), 1);
		splitter_->addWidget(leftCol_);

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

		connect(splitter_, &QSplitter::splitterMoved, this,
			[this](int, int) {
				userSplit_[(int)panelMode_] = true;
				savedSplit_[(int)panelMode_] =
					splitter_->saveState();
			});
		splitter_->setStretchFactor(0, 3);
		splitter_->setStretchFactor(1, 2);
		root->addWidget(splitter_, 1);
	}

	rootLayout_ = root;
	bottomSep_ = mkSep();
	root->addWidget(bottomSep_);
	bottomBar_ = buildBottomBar();
	root->addWidget(bottomBar_);

	// THE KEYS HAVE TO WORK WHEREVER THE FOCUS IS, and in this panel the focus
	// is nearly always on a button the operator has just pressed. A QPushButton
	// that belongs to a QButtonGroup — every angle key, every speed chip — takes
	// the arrows for focus navigation instead of ignoring them, so ←/→ would walk
	// the highlight along the camera row rather than step a frame. Filtering the
	// buttons keeps focus (and its ring) working while the keys stay uniform.
	//
	// The panel itself takes focus too, so clicking an empty patch of it does not
	// leave the keyboard pointing at whatever was focused before.
	setFocusPolicy(Qt::StrongFocus);
	for (QAbstractButton *b : findChildren<QAbstractButton *>())
		b->installEventFilter(this);

	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(33); // ~30 fps — smooth seekbar + responsive transport
	connect(pollTimer_, &QTimer::timeout, this, &MultiReplayDock::poll);
	pollTimer_->start();

	// After the widgets exist: every one of these acts on one of them.
	registerDockHotkeys();
	// Reachable from the module's frontend handler (see releasePreviewRefs).
	g_dock = this;

	// HOW MANY BAYS, and therefore what the panel even shows. One call sets the
	// flag, the badge, the two letters, the checked key and the visibility of
	// the whole B half together, so they cannot disagree: with B off everything
	// means A, with B on the resting state is A|B (the operator who asked for a
	// second bay asked for it to be used).
	applyChannelBVisibility();

	refreshAngles();
	refreshEvents();

	// THE ARRANGEMENT, once, now that every widget it places exists. Forced,
	// because panelMode_ already says Wide and the point of this call is to do
	// the celling rather than to agree with itself. resizeEvent takes over
	// from here — including the first real resize OBS gives the dock, which is
	// usually not Wide.
	applyTableDensity(ReplayCore::instance().getConfig().tableDensity);
	applyPanelMode(panelModeFor(size(), panelMode_, wideFloorH_),
		       /*force*/ true);

	poll();
}

MultiReplayDock::~MultiReplayDock()
{
	// FIRST of all: nothing may reach a dock that is being taken apart.
	if (g_dock == this)
		g_dock = nullptr;
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
	// Every one of them — the two bays and every multiview tile: each has a
	// draw callback pointing at this object, and B's used to be left attached
	// (see allDisplays).
	for (OBSQTDisplay *d : allDisplays())
		d->setRenderCallback(nullptr, nullptr);
	if (pollTimer_)
		pollTimer_->stop();
	// BOTH preview refs. B's was missed, and it was the only source OBS ever
	// complained about: "Not all sources were cleared when clearing scene data:
	// MultiReplay - Replay B", which OBS shows the operator as a plugin that
	// failed to release its resources — a dialog on the way out. A held ref is
	// invisible until something actually uses that bay, which is why it lasted
	// as long as B went unexercised.
	// Every owned reference the dock holds — the two previews and the multiview
	// tiles — in one place, so the destructor and the "OBS is about to clear
	// scene data" path cannot drift apart. They did: the destructor released the
	// tiles and A's preview, the cleanup path released neither, and OBS named
	// what was left in a dialog on the way out.
	dropPreviewRefs();
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
	projectLbl_->setStyleSheet(
		QString("color: %1; font-size: 9px; padding: 0 4px;")
			.arg(sc().accent));
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
	auto *mag = new QLabel(box);
	mag->setPixmap(iconFor(Icon::Search, QColor(sc().textMuted), 13,
			       devicePixelRatioF())
			       .pixmap(13, 13));
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
	liveBtn_ = iconTextBtn(Icon::Live, obs_module_text("Dock.LiveMode"),
			       "live", box, "mrLive", 12);
	liveBtn_->setObjectName("mrLive");
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
	connect(monitorSplit_, &QSplitter::splitterMoved, this, [this](int, int) {
		userMonitorSplit_[(int)panelMode_] = true;
		savedMonitorSplit_[(int)panelMode_] = monitorSplit_->saveState();
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

	previewPane_ = box; // the splitter child the Monitors key gives back
	return box;
}

// The Monitors key, applied. Hiding the two rows inside the pane was not
// enough: the pane is a QSplitter child, the splitter had already given it a
// share of the height, and an empty child keeps that share — so the table below
// stayed exactly as short as it was with a band of nothing above it, which is
// the opposite of what the key is for. A hidden splitter child contributes
// nothing and its handle goes with it, so the list takes the whole panel.
//
// The rows are hidden too, and deliberately: a hidden OBSQTDisplay does not
// create its display at all (see qt-display.hpp), which is the GPU cost this
// key exists to put down.
void MultiReplayDock::applyMonitorsVisible(bool on)
{
	if (monitorsRow_)
		monitorsRow_->setVisible(on);
	if (monitorsStrip_)
		monitorsStrip_->setVisible(on);
	if (previewPane_)
		previewPane_->setVisible(on);
	if (on)
		applyPreviewAspect();
	obs_log(LOG_INFO, "[dock] monitors %s", on ? "shown" : "hidden");
}

// ---------------------------------------------------------------------------
// THE THREE ARRANGEMENTS OF THE PANEL
//
// See PanelMode in dock-layout.hpp for the argument. In short: a replay panel
// is used from memory rather than read, so it gets a small number of designed
// arrangements instead of one that reflows into a different panel at every
// width. Nothing is dropped in any of them; what changes is rank.
//
// EVERY LINE HERE IS A RE-CELL OR A PROPERTY. Nothing is created, destroyed or
// re-parented, because under these boxes are OBSQTDisplay widgets whose native
// window a re-parent would destroy — see the note in buildPreview().
// ---------------------------------------------------------------------------

// HOW TALL THE MONITORING BLOCK MAY BE, and it must not be read off the block
// itself: that height is DERIVED from the arrangement this number chooses, so
// feeding it back in makes a pass decide from whatever the widget happened to be
// mid-settle - measured in the mockup, 100 px, which picked an arrangement of
// 78 px stamps. What the splitter is willing to give depends only on the panel
// and a constant, so it is the same on every pass.
//
// Once the OPERATOR has moved the divider it is his answer, and that is stable
// for the same reason: it stops being derived from anything.
int MultiReplayDock::monitorRoomH() const
{
	// IT MUST NOT BE READ OFF THE PANE, not even once the operator has moved
	// the divider. It used to be, and that is why maximising the window left
	// the pictures the size they were: the ceiling on the picture pane is set
	// FROM this number, so taking it from the pane made the ceiling equal to
	// the height the pane already had - a pane that could never grow, however
	// big the window got. What the operator chose is WHERE the divider sits,
	// and applyPreviewSplit still leaves that alone; how tall the block may be
	// is a question about the panel.
	const int total = splitter_ ? splitter_->height() : height();
	int room = total - kListPaneFloor;
	// ...AND THE PICTURES MAY NOT HAVE MORE THAN HALF THE PANEL. Past that the
	// list stops being a list. The cap belongs HERE, with the rest of the
	// answer, because the tile arithmetic asks the same question and the two
	// answers have to be the same one - they were two, and the cameras came out
	// sized for a block a third taller than the one they were given.
	if (panelMode_ == PanelMode::Wide)
		room = std::min(room, height() / 2);
	return std::max(40, room);
}

int MultiReplayDock::tileColumns(int tileCount) const
{
	const int n = std::max(1, tileCount);
	// Beside a big A output the tiles are a tall narrow strip: two columns up
	// to four tiles, three beyond. That was tuned against the picture next to
	// it and stays.
	if (panelMode_ != PanelMode::Tall) {
		// FROM THE SAME ARITHMETIC THE SIZES COME FROM. Hardcoded as
		// "two up to four, three beyond", it could disagree with the
		// block that was actually measured - and a column count that
		// disagrees with the tile size is a block with a hole in it.
		const int paneW = std::max(80, monitorSplit_ ? monitorSplit_->width()
							     : width());
		const Config cfg = ReplayCore::instance().getConfig();
		const int bays = cfg.enableChannelB ? 2 : 1;
		const int roomH = monitorRoomH();
		return std::max(1, tileBlockFor(paneW, bays, n, 5, roomH).cols);
	}
	// In a COLUMN the same two columns build a block as tall as the picture
	// above them, and the picture is what the operator is looking at. So the
	// tiles go as wide as the panel can carry them at a size still worth
	// looking at — a filmstrip rather than a grid.
	//
	// std::min(2, n) rather than 2: on a ONE-CAMERA rig the bounds would be
	// clamp(x, 2, 1), whose behaviour is undefined when lo > hi — and one
	// camera is the configuration this plugin is most often installed on.
	const int avail = std::max(1, width() - 16);
	return std::clamp(avail / kTileMinWidth, std::min(2, n), n);
}

void MultiReplayDock::applyPanelMode(PanelMode m, bool force)
{
	if (!monitorSplit_ || !splitter_)
		return;
	if (!force && m == panelMode_)
		return;
	const PanelMode was = panelMode_;
	panelMode_ = m;

	// THE MONITORING BLOCK IS ONE SPLITTER and every arrangement turns it
	// rather than rebuilding it: side by side the bays and the cameras divide
	// a WIDTH, in a column they divide a HEIGHT. setOrientation does not touch
	// the children, which is the whole reason this is a splitter — moving an
	// OBSQTDisplay between two layouts destroys its native window and strands
	// the obs_display presenting into it.
	//
	// A AND B STAY SIDE BY SIDE IN ALL THREE. They are two bays of one deck;
	// stacking them would make the pair read as a hierarchy, and it is the one
	// relationship on this panel that is exactly equal.
	monitorSplit_->setOrientation(m == PanelMode::Tall ? Qt::Vertical
							   : Qt::Horizontal);
	splitter_->setOrientation(m == PanelMode::Short ? Qt::Horizontal
						       : Qt::Vertical);

	// SHORT PUTS THE CONTROLS IN THE LEFT COLUMN. The panel is wide and
	// shallow, so the list goes down the right-hand half and everything else —
	// pictures, keys, the on-air band and the position bar — stacks on the
	// left. The strip carries no picture, so moving it between the two layouts
	// costs a relayout and nothing else.
	applyControlsColumn(m == PanelMode::Short);

	// THE DIVIDERS THE OPERATOR CHOSE FOR *THIS* ARRANGEMENT, put back. After
	// the orientations, never before: a saved state applied to a splitter that
	// is still dividing the other way is a split nobody asked for.
	if (was != m) {
		if (splitChosen())
			splitter_->restoreState(savedSplit_[(int)m]);
		if (monitorSplitChosen())
			monitorSplit_->restoreState(savedMonitorSplit_[(int)m]);
	}

	// THE STRIP STACKS WHENEVER IT IS IN A COLUMN, which is Tall AND Short:
	// in Short the keys move into the left-hand column beside the list, so
	// they have about half the panel's width. Left on the wide two-macro-row
	// shape there, the sections did not fit across that half and the strip
	// folded anyway - into something so deep that the panel's own floor rose
	// above the height at which Short is chosen, and the arrangement was
	// undone by the resize it had just caused. From the operator's chair
	// Short flashed on and vanished, at every width.
	//
	// Width alone cannot tell these apart — see setStacked.
	if (strip_)
		strip_->setStacked(m == PanelMode::Wide ? 0 : 1);

	// The search box is the one control that can be asked to give width back:
	// in a column, 90 px of it is 90 px the Live and Monitors keys do not have.
	if (search_)
		search_->setMinimumWidth(m == PanelMode::Tall ? 54 : 90);

	// OUT COLUMN: the one column of the table that is inferable. IN and
	// DURATA together say where the clip is and how long it runs, so OUT is
	// arithmetic — and it is the column worth spending on a camera instead
	// when there are 330 px to divide.
	if (events_)
		events_->setColumnHidden(kColOut, m == PanelMode::Tall);

	rebuildMultiview();

	// A NOTE ON SETTLING, because it looks like a bug and is not one.
	// A mode change rewrites how short the panel may be, so a panel asked for
	// a size that only the NEW arrangement can hold takes two resize events to
	// get there: the first arrives while the old floor is still in force. The
	// mockup's --check measures it (a top-level asked for 1400x340 lands on
	// 404, then 340).
	//
	// `layout()->invalidate()` here was the obvious fix and was MEASURED TO DO
	// NOTHING — the same two events either way — so it is not in this file. In
	// OBS the panel is a child of a QDockWidget rather than a top-level, so
	// there is no window manager clamping the first size at all.

	applyPreviewAspect();

	// THE FLOOR IS IN THE LOG, because a mode that cannot be reached looks
	// exactly like a mode that was not chosen. Short is picked below a height
	// the panel must also be ABLE to be, and the arrangement it switches to
	// rewrites that floor - so when the two disagree the panel flicks into
	// Short and is pushed straight back out by its own minimum. The numbers
	// that settle it are this line's.
	if (was != m) {
		obs_log(LOG_INFO,
			"[dock] layout %s -> %s (%dx%d, floor %dx%d)",
			panelModeName(was), panelModeName(m), width(), height(),
			minimumSizeHint().width(), minimumSizeHint().height());
		// ...AND WHO IS ASKING FOR IT. A floor is a sum, and a sum is
		// exactly the thing you cannot argue with until it is broken into
		// the parts that made it. Every zone that can put a number in.
		const auto floorOf = [](QWidget *w) {
			return w ? w->minimumSizeHint().height() : 0;
		};
		obs_log(LOG_INFO,
			"[dock] floor parts: pictures %d, list %d, keys %d "
			"(strip %d, status %d, band %d, bar %d)",
			floorOf(previewPane_), floorOf(events_),
			floorOf(bottomBar_), floorOf(strip_),
			floorOf(statusBar_), floorOf(clipBar_), floorOf(seek_));
	}
}

int MultiReplayDock::aspectHeight(int width)
{
	obs_video_info ovi{};
	// A canvas we cannot read is 16:9, because that is what a replay rig is
	// nine times in ten — and because a wrong ratio here costs a few pixels of
	// bar, not a picture.
	double ratio = 9.0 / 16.0;
	if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0)
		ratio = (double)ovi.base_height / (double)ovi.base_width;
	return std::max(1, (int)std::lround(width * ratio));
}

// ── THE MONITORING BLOCK, in three steps ─────────────────────────────────
//
//  1. pick the camera block's shape for the room there is;
//  2. unless the OPERATOR has moved a divider, put the dividers where the
//     pictures want them;
//  3. nothing. Every box keeps the canvas's ratio by itself (AspectBox), so
//     there is no third step any more — which is most of what this function
//     used to be, and all of what it used to get wrong.
void MultiReplayDock::applyPreviewAspect()
{
	if (!previewPane_ || !monitorSplit_)
		return;

	const int paneW = std::max(80, monitorSplit_->width());
	const Config cfg = ReplayCore::instance().getConfig();
	const int bays = cfg.enableChannelB ? 2 : 1;

	// B'S SHARE OF THE ROW GOES AWAY WITH B, and hiding the box is not what
	// does it. A grid column keeps the stretch it was given whether or not
	// anything visible is in it, so with one bay A was laid out in the LEFT
	// HALF of the picture row and centred there - which reads exactly like a
	// panel holding a space open for something that is switched off. Across
	// the bottom of a wide panel it also opened a gap between A and the first
	// camera that no drag could close.
	if (auto *bg = qobject_cast<QGridLayout *>(bays_->layout()))
		bg->setColumnStretch(1, cfg.enableChannelB ? 1 : 0);
	const int gap = monitorSplit_->handleWidth();
	int visibleTiles = 0;
	for (const PreviewTile &t : tiles_)
		if (t.box && t.box->isVisible())
			visibleTiles++;
	const bool haveTiles = visibleTiles > 0 && multiviewBox_ &&
			       multiviewBox_->isVisible();

	// THE CAMERA BLOCK IS SIZED FIRST and the bays take what is left, which is
	// the opposite of what a stretch factor does — and the reason a stretch
	// factor starved them: a stretch shares only what is left AFTER every
	// column has its minimum, and a tile's minimum is nothing. Eight cameras
	// came out as eight vertical slivers.
	const int cols = haveTiles ? std::max(1, tileColumns(visibleTiles)) : 1;
	tileCap_ = kTileMinWidth;
	const int rows = haveTiles ? (visibleTiles + cols - 1) / cols : 0;
	const int tagH = AspectBox::kTagH;

	int want = 0;
	if (panelMode_ == PanelMode::Tall) {
		// A COLUMN: the bays across the top, the cameras under them.
		const int bayH =
			aspectHeight((paneW - 3 * (bays - 1)) / bays) + tagH;
		int stripH = 0;
		if (haveTiles) {
			// THE STRIP FILLS THE ROW. The 150 px ceiling is there
			// to stop ONE camera drawing itself as big as the
			// picture being watched; from two upwards the row is
			// already divided, and holding the ceiling there just
			// left a band of empty panel down the right-hand side of
			// every filmstrip.
			const int share = (paneW - 2 * (cols - 1)) / cols;
			const int tileW =
				cols >= 2 ? share
					  : std::min((int)(paneW * kTileMaxShare),
						     share);
			tileCap_ = tileW;
			stripH = rows * (aspectHeight(tileW) + tagH) +
				 (rows - 1) * 2;
		}
		want = bayH + (haveTiles ? gap + stripH : 0);
		if (!monitorSplitChosen() && haveTiles)
			monitorSplit_->setSizes({bayH, stripH});
	} else {
		// ONE PIECE OF ARITHMETIC, SHARED WITH THE MOCKUP (tileBlockFor,
		// in dock-layout). This used to be a second, simpler copy here -
		// a fixed 150 px ceiling and a flat share of the width - and the
		// two disagreed: beside an 840 px A the cameras came out as two
		// stamps with 270 px of empty panel under them, while the mockup,
		// which is what every layout decision was being judged on, drew
		// something else entirely.
		int tilesW = 0, blockH = 0;
		TileBlock tb0;
		if (haveTiles) {
			// THE HEIGHT THE BLOCK WILL ACTUALLY HAVE, in every wide
			// arrangement and not only the short one. Left unbounded the
			// calculation believes the bays can be as tall as their width
			// allows and picks the arrangement for a block that will
			// really be much shorter: A comes out height-bound and far
			// narrower than the space it was given, and the difference is
			// the black band beside it.
			const TileBlock tb = tileBlockFor(paneW, bays, visibleTiles,
							  gap, monitorRoomH());
			tb0 = tb;
			tilesW = tb.blockW;
			blockH = tb.blockH;
			tileCap_ = tb.tileW;
			// THE DIVIDER IS THE OPERATOR'S ANSWER once he has moved
			// it: the block above is what the panel would choose, and
			// holding it after he has dragged the handle leaves the
			// cameras small while the room he just gave them shows as
			// empty panel.
			const QList<int> have = monitorSplit_->sizes();
			if (monitorSplitChosen() && have.size() > 1 &&
			    have[1] > 0) {
				tilesW = have[1];
				tileCap_ = std::max(
					kTileMinWidth,
					(tilesW - (cols - 1) * kTileGap) / cols);
				blockH = rows * (aspectHeight(tileCap_) + tagH) +
					 (rows - 1) * kTileGap;
			}
		}
		// THE PANES ARE SIZED FROM THE PICTURES, not the other way round:
		// tileBlockFor settled one height for the whole row, so a bay is
		// exactly as wide as that height allows and the cameras take the
		// rest. Sized from the leftover instead, A was height-bound and
		// floated in a pane hundreds of pixels wider than itself.
		const int bayW = haveTiles && tb0.bayW > 0
					 ? tb0.bayW
					 : aspectHeight(paneW / bays) + tagH;
		int baysW = std::max(60, bays * bayW + 3 * (bays - 1));
		if (haveTiles && baysW + tilesW + gap > paneW)
			baysW = std::max(60, paneW - tilesW - gap);
		const int bayH = aspectHeight((baysW - 3 * (bays - 1)) / bays) + tagH;
		// monitorRoomH() is the ONE authority on how tall this block may be,
		// and the tile arithmetic above was given the same number.
		want = std::min(std::max(bayH, blockH), monitorRoomH());
		if (!monitorSplitChosen() && haveTiles) {
			// Anything the row cannot fill is split between the two
			// panes, so each picture is centred in its own rather than
			// the whole block hugging one edge.
			const int slack = std::max(0, paneW - baysW - tilesW - gap);
			monitorSplit_->setSizes(
				{baysW + slack / 2, tilesW + slack - slack / 2});
		}
	}

	// A tile is a CONFIDENCE MONITOR, so it has a ceiling: left to fill the
	// row, a single configured camera drew itself as big as the picture being
	// watched — the same angle twice, with the event list paying for the
	// second copy.
	// ...AND A CEILING ON ITS HEIGHT, which is the same statement made
	// the other way round: beside a big A the camera column is as tall as
	// A is, and a box left free to fill it holds one small picture in the
	// middle of a tall empty rectangle. Capped, the pictures stack at the
	// top and the leftover goes to the spare row below them.
	//
	// IN A COLUMN THERE IS NO CEILING, and that is not a detail: in a
	// column the strip's height is exactly what the pictures need, so a
	// ceiling would be a second opinion on the same number - one derived
	// from the width the layout just produced, fed back into the layout
	// that produced it. Written that way this function span the panel until
	// it was killed. Beside a big A the ceiling is a CONSTANT.
	const int tileCapH = panelMode_ == PanelMode::Tall
				     ? QWIDGETSIZE_MAX
				     : aspectHeight(tileCap_) + tagH;
	for (const PreviewTile &t : tiles_)
		if (t.box) {
			// Only when it CHANGED. Setting a maximum invalidates
			// the layout, and this runs from the resize it would
			// then cause.
			if (t.box->maximumWidth() != tileCap_)
				t.box->setMaximumWidth(tileCap_);
			if (t.box->maximumHeight() != tileCapH)
				t.box->setMaximumHeight(tileCapH);
		}

	applyPreviewSplit(want);
}

// ── WHERE THE KEYS LIVE ──────────────────────────────────────────────────
//
// Across the foot of the panel, or stacked in the left column beside the list.
// The strip carries no picture, so moving it between the two layouts costs a
// relayout and nothing else — which is exactly why the PICTURES are not the
// thing that moves: re-parenting an OBSQTDisplay destroys its native window and
// strands the obs_display presenting into it.
//
// A no-op unless the answer changed. It is called from applyPanelMode, which is
// called from every resize.
void MultiReplayDock::applyControlsColumn(bool inColumn)
{
	if (!bottomBar_ || !rootLayout_ || !leftColLayout_)
		return;
	if (inColumn == controlsInColumn_)
		return;
	controlsInColumn_ = inColumn;
	if (inColumn) {
		rootLayout_->removeWidget(bottomSep_);
		rootLayout_->removeWidget(bottomBar_);
		bottomSep_->hide();
		leftColLayout_->addWidget(bottomBar_);
	} else {
		leftColLayout_->removeWidget(bottomBar_);
		bottomBar_->setParent(this);
		rootLayout_->addWidget(bottomSep_);
		rootLayout_->addWidget(bottomBar_);
		bottomSep_->show();
	}
	bottomBar_->show();
}

// The divider between the pictures and the list. In Short it divides WIDTH, so
// a height means nothing to it and the pane simply takes the column it is in.
void MultiReplayDock::applyPreviewSplit(int want)
{
	if (panelMode_ == PanelMode::Short) {
		previewPane_->setMaximumHeight(QWIDGETSIZE_MAX);
		return;
	}
	// A CAP, so the pane can never be GIVEN more room than its pictures can
	// fill — every pixel over is panel showing through where a picture was
	// expected, and the list wanted it.
	if (previewPane_->maximumHeight() != want)
		previewPane_->setMaximumHeight(want);
	// …AND THE SPLITTER HAS TO BE TOLD, which the cap alone does not do: a
	// QSplitter hands out height by stretch factor and capped at the top it
	// can still give the pane LESS than the pictures need.
	//
	// UNTIL THE OPERATOR SAYS OTHERWISE. The moment he drags the handle he has
	// stated a preference, and it is not overruled until the arrangement
	// changes under him.
	if (splitChosen())
		return;
	const int total = splitter_->height();
	// As much as the pictures need, or as much as is left once the list has
	// its floor — whichever is smaller. A perfect picture over two visible
	// rows of events is the wrong trade on a panel whose point is the list.
	const int give = std::min(want, total - kListPaneFloor);
	const QList<int> now = splitter_->sizes();
	if (give > 0 && (now.isEmpty() || std::abs(now[0] - give) > 2))
		splitter_->setSizes({give, total - give});
}


// THE WIDE ARRANGEMENT'S FLOOR IS SAMPLED AFTER THE PASS, NEVER DURING ONE.
//
// Asking a widget for its minimumSizeHint ACTIVATES its layout, and doing that
// anywhere inside the resize cascade - in here, or in applyPanelMode - does not
// merely read a number: the pass it forces is the one that stays on screen.
// Measured on the mockup, which has the same layer under it: the six speed
// presets came out 38x16 instead of 38x22, in every arrangement, and its own
// hit-target check is what caught it.
//
// One pass late costs nothing. The floor moves only with the WIDTH, and the
// hysteresis in panelModeFor covers the tick it takes to catch up.
void MultiReplayDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	applyPanelMode(panelModeFor(size(), panelMode_, wideFloorH_));
	QTimer::singleShot(0, this, [this]() {
		if (panelMode_ == PanelMode::Wide)
			wideFloorH_ = minimumSizeHint().height();
	});
	// AFTER THE LAYOUT PASS, not during it. A resizeEvent arrives before the
	// children have been re-laid, so the splitter still reports its OLD height
	// here — and a split computed from a stale total is then rescaled
	// proportionally by the layout that follows. Measured: asking for 286/110
	// produced 91/305, which is the squashed picture again by a different
	// route. A zero-delay timer runs once the layout has settled.
	QTimer::singleShot(0, this, [this]() { applyPreviewAspect(); });
}

// ---------------------------------------------------------------------------
// THE PANEL'S COLOURS
// ---------------------------------------------------------------------------

void MultiReplayDock::applyTheme()
{
	const Config cfg = ReplayCore::instance().getConfig();
	const int choice = cfg.uiTheme;
	// qApp->palette() rather than this->palette(): OBS applies the theme's
	// palette to the APPLICATION, and a widget's own palette is a copy that
	// may have been resolved before the theme changed.
	sc() = schemeFor((ThemeChoice)choice, qApp->palette());
	setStyleSheet(dockStyle(sc(), cfg.tableDensity));
	applyTableDensity(cfg.tableDensity);

	// THE MARKS ARE PIXMAPS, so unlike every label on this panel they do not
	// follow a new style sheet: a theme that moved the key colour under them
	// would leave them the colour they were drawn in, which on a light panel
	// is a row of keys with invisible marks on them. Each one remembers which
	// mark it is (see dock-icons.hpp) and is redrawn from the new scheme.
	restyleIcons(this, tintsFor(sc()));

	// The two painted widgets read sc() directly and are not restyled by the
	// sheet, so they have to be told to redraw. Everything else Qt repolishes
	// for us when the style sheet is replaced.
	if (seek_)
		seek_->update();
	if (clipBar_)
		clipBar_->update();
	if (projectLbl_)
		projectLbl_->setStyleSheet(
			QString("color: %1; font-size: 9px; padding: 0 4px;")
				.arg(sc().accent));
	obs_log(LOG_INFO, "[dock] theme %d applied (panel %s, text %s)", choice,
		qUtf8Printable(sc().panel), qUtf8Printable(sc().text));
}

// How tight the event list is drawn. The style sheet has already been given the
// matching input metrics (see dockStyle); this is the half of the pair that
// lives on the widgets.
void MultiReplayDock::applyTableDensity(int level)
{
	if (!events_)
		return;
	const Density d = densityFor(level);

	// DOWN as well as up. refreshEvents() raises the row height to whatever
	// the tallest cell it built needs and never lowers it, which is right while
	// the metrics are fixed and wrong the moment they can change: without this
	// reset, picking "compact" would rebuild shorter cells inside rows that
	// were still 30 px tall, and nothing would look any denser.
	events_->verticalHeader()->setDefaultSectionSize(d.rowFloor);

	// The headings, which are not cells and so are not raised by anything.
	QHeaderView *hh = events_->horizontalHeader();
	// ...AND THE HEIGHT IS ASKED OF THE TYPE, not written down beside it. It
	// was the constant alone, and the constant knows nothing about the padding
	// and the border the style draws round the label: at the denser settings
	// the headings came out clipped along the top. Same lesson as the row
	// height, which is taken from the cell that was actually built.
	{
		QFont hf = hh->font();
		hf.setPixelSize(d.headerFont);
		hf.setBold(true);
		// 3 px of padding top and bottom plus the rule under it.
		const int need = QFontMetrics(hf).height() + 7;
		hh->setFixedHeight(std::max(d.headerH, need));
	}

	// The cells carry their own built-in size hints, so they have to be built
	// again to pick up the new metrics. version() has not moved, so ask
	// directly rather than waiting for a mark.
	eventsDirty_ = true;
}

void MultiReplayDock::restyleDock()
{
	if (g_dock)
		g_dock->applyTheme();
}

// ---------------------------------------------------------------------------
// ⛶  Full screen — only while the panel is floating
//
// The gesture this exists for: the operator pulls the dock out of OBS onto a
// second monitor and wants it to fill that monitor. Doing that by hand means
// dragging four edges to four screen edges, and doing it again every time a
// restored layout comes back with slightly different numbers.
//
// Three decisions worth keeping:
//
//  1. IT IS THE QDockWidget THAT GOES FULL SCREEN, not a window of our own.
//     Re-parenting this widget into a new top level would destroy the native
//     window of every OBSQTDisplay under it — both bays and every multiview
//     tile — and strand the obs_display bound to each (qt-display.hpp: the one
//     failure mode that file exists to avoid). A window STATE change touches no
//     child handle at all.
//  2. THE HOST IS RESOLVED EVERY TIME, never cached. OBS owns that dock: it
//     creates it after our constructor has run, and a layout restore can hand
//     the panel to a different one. Walking three parents is two pointer reads.
//  3. THE KEY IS POLLED, not wired to QDockWidget::topLevelChanged, for the
//     same reason — there is no single dock object whose lifetime we can pin a
//     connection to. Floating and full screen are both deliberate, rare
//     gestures; 4 Hz is far faster than either can be repeated.
// ---------------------------------------------------------------------------

QDockWidget *MultiReplayDock::hostDock() const
{
	for (QWidget *w = parentWidget(); w; w = w->parentWidget())
		if (auto *d = qobject_cast<QDockWidget *>(w))
			return d;
	return nullptr;
}

bool MultiReplayDock::panelIsFullScreen() const
{
	const QDockWidget *host = hostDock();
	return host && host->isFloating() && host->isFullScreen();
}

void MultiReplayDock::setPanelFullScreen(bool on)
{
	QDockWidget *host = hostDock();
	// Docked, there is nothing here to make full screen: the window belongs to
	// OBS. The key is hidden in that state, so this is the race (a re-dock
	// between the paint and the click), not the ordinary path.
	if (!host || !host->isFloating())
		return;
	if (on == host->isFullScreen())
		return;
	if (on) {
		// MAXIMISED IS A STATE, NOT A RECTANGLE. Saving the geometry of a
		// maximised window and setting it back gives a window that merely
		// looks maximised: it is not snapped to anything, and the first
		// thing that moves it proves the difference.
		preFullScreenMaximized_ = host->isMaximized();
		if (!preFullScreenMaximized_)
			preFullScreenGeom_ = host->geometry();
		host->showFullScreen();
	} else if (preFullScreenMaximized_) {
		host->showMaximized();
	} else {
		host->showNormal();
		// Qt keeps a pre-full-screen geometry of its own, but a dock that
		// has been floated, docked and floated again has had that memory
		// rewritten under it more than once — and coming back to a window
		// the size of a postage stamp in the corner of a monitor is worse
		// than not having the key.
		if (preFullScreenGeom_.isValid())
			host->setGeometry(preFullScreenGeom_);
	}
	obs_log(LOG_INFO, "[dock] panel %s",
		on ? "full screen" : "back to a window");
}


// ---------------------------------------------------------------------------
// The floating window gets a Minimize and a Maximize
//
// Qt floats a dock with a title bar it asked for by name — a title and a close
// box and nothing else — so the operator's right click on it finds Minimize and
// Maximize greyed out, and the two things a window on its own monitor most
// obviously wants are the two it cannot do. Reported from a real rig.
//
// The type matters as much as the hints. Qt floats a dock as a Qt::Tool, and a
// tool window on Windows is drawn with the small caption that has NO minimise
// box at all — the hint alone would change nothing there. Made a plain window it
// gets both boxes, and two things the operator also wanted anyway: a place in
// the taskbar and a stop in Alt+Tab, which is what a panel living on a second
// monitor for a whole match should have.
//
// Reapplied on every float because Qt rewrites the flags each time it makes one
// (QDockWidgetPrivate::setWindowState), so this cannot be done once at startup.
// ---------------------------------------------------------------------------

void MultiReplayDock::equipFloatingWindow(QDockWidget *host)
{
	if (!host || !host->isFloating() || host->isFullScreen())
		return;

	// RESCUE: A WINDOW THE PREVIOUS BUILD COULD PUT BEYOND REACH.
	// That build offered a minimise box, and pressing it was a one-way door
	// (see below): the panel went away and the taskbar had nothing to bring it
	// back with. The box is gone now, so a minimised floating dock can only be
	// that leftover — nothing in this build can produce one — and leaving it
	// there would keep the panel unreachable until OBS was restarted.
	//
	// Guarded on the MAIN WINDOW, because "Show desktop" minimises the owner
	// and everything it owns: bringing only this one back would drop a replay
	// panel onto a desktop the operator had just cleared. The state is cleared
	// rather than showNormal()'d so a window that was maximised comes back
	// maximised.
	if (host->isMinimized()) {
		auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
		if (main && !main->isMinimized()) {
			host->setWindowState(host->windowState() &
					     ~Qt::WindowMinimized);
			host->raise();
			if (!minimizeRescueLogged_) {
				minimizeRescueLogged_ = true;
				obs_log(LOG_INFO,
					"[dock] floating panel was left minimised "
					"by an older build — brought back");
			}
		}
	} else {
		minimizeRescueLogged_ = false;
	}
	const Qt::WindowFlags flags = host->windowFlags();
	// MID-DRAG, HANDS OFF. While the dock is being pulled out of OBS, Qt
	// floats it frameless with the mouse grabbed; setWindowFlags there
	// rebuilds the native window underneath the drag and the panel is dropped
	// on the floor. Two independent tells, because either one alone has a
	// moment where it is wrong: the frameless flag is Qt's unplugged state,
	// and a held button is the operator still holding it.
	if (flags.testFlag(Qt::FramelessWindowHint))
		return;
	if (QApplication::mouseButtons() != Qt::NoButton)
		return;

	// MAXIMISE, AND DELIBERATELY NOT MINIMISE. Reported from a real rig: with
	// the minimise box on, one press put the panel away and there was no way
	// back — the taskbar showed only the OBS main window and the log window.
	//
	// That is not a bug to chase, it is what this window IS. An OBS dock is
	// OWNED by the OBS main window (its QWidget parent, and on Windows its
	// owner HWND), and Windows gives an owned window no taskbar button and no
	// Alt+Tab stop of its own — it shows the owner instead. So a minimise box
	// here is a one-way door: the window goes somewhere with no handle on it,
	// and on a live rig that is the worst thing a panel can do. Getting a real
	// taskbar button would mean the dock stopping being OBS's dock, which is
	// also the thing that makes it dockable, save with the layout and come back
	// tomorrow. (An earlier version of this comment claimed the taskbar and
	// Alt+Tab as benefits of the type change below. They were never true.)
	//
	// The MAXIMISE box is real and works, and it is the one the request was
	// actually about: the window grown to the whole screen WITH its title bar,
	// which is a different thing from the ⛶ key's full screen.
	Qt::WindowFlags want = flags | Qt::WindowMaximizeButtonHint;
	want &= ~Qt::WindowFlags(Qt::WindowMinimizeButtonHint);
	// THE TYPE MATTERS AS MUCH AS THE HINT. Qt floats a dock as a Qt::Tool
	// (measured: 0x0a00300b), and a tool window on Windows is drawn with the
	// small caption that has no maximise box at all — the hint alone would
	// change nothing there.
	if ((flags & Qt::WindowType_Mask) == Qt::Tool)
		want = (want & ~Qt::WindowFlags(Qt::WindowType_Mask)) |
		       Qt::Window;
	if (want == flags)
		return;

	// setWindowFlags hides the widget, so the show() is not optional.
	//
	// THE HANDLES ARE LOGGED BECAUSE THE CLAIM THAT USED TO BE HERE WAS NEVER
	// MEASURED — and when it finally was, it was wrong in BOTH directions. It
	// read: "setWindowFlags ... rebuilds its native handle ... every
	// OBSQTDisplay underneath loses its window with it. That is survivable and
	// already handled (recheckWindow on the next tick rebuilds each display)."
	// Neither half happens. No display is ever destroyed or rebuilt after this
	// line in any session on record, and the handle itself does not change:
	// measured 0x23094c -> 0x23094c, because Qt applies these flags with
	// SetWindowLong/SetWindowPos rather than making a new window.
	//
	// The two integers stay printed. This was a candidate cause of the
	// black-screen reports — a swap chain left presenting into a window
	// rebuilt beneath it — and it was killed by printing them (with the
	// ancestor check in qt-display.cpp, which reports 0). Anyone who suspects
	// it again should read the log line rather than this comment: that is the
	// whole reason the line exists.
	//
	// internalWinId(), never winId(): the latter CREATES a native window when
	// there is none, which is the whole hazard this plugin has already paid
	// for once (qt-display.cpp).
	const QRect keep = host->geometry();
	const unsigned long long beforeWin =
		(unsigned long long)host->internalWinId();
	host->setWindowFlags(want);
	host->setGeometry(keep);
	host->show();
	obs_log(LOG_INFO,
		"[dock] floating window equipped: flags 0x%08x -> 0x%08x, "
		"top-level window 0x%llx -> 0x%llx",
		(unsigned)flags.toInt(), (unsigned)host->windowFlags().toInt(),
		beforeWin, (unsigned long long)host->internalWinId());
}

void MultiReplayDock::refreshFullScreenKey()
{
	if (!fullScreenBtn_)
		return;
	QDockWidget *host = hostDock();
	// THE FILTER GOES ON THE DOCK, NOT ON US. The double click the operator
	// aims at the title bar never reaches this widget: with a native frame it
	// is a non-client event delivered to the window, and Qt's own handler on
	// the QDockWidget answers it by re-docking. Filtering the dock is the only
	// place upstream of that. Re-hooked rather than hooked once, because OBS
	// owns that object and a layout restore can hand the panel to another.
	if (host && filteredHost_ != host) {
		if (filteredHost_)
			filteredHost_->removeEventFilter(this);
		host->installEventFilter(this);
		filteredHost_ = host;
	}
	const bool floating = host && host->isFloating();
	if (floating)
		equipFloatingWindow(host);
	const int want = floating ? 1 : 0;
	if (fullScreenKeyShown_ != want) {
		fullScreenKeyShown_ = want;
		fullScreenBtn_->setVisible(floating);
	}
	// Lit means "this panel owns the screen". Read off the WINDOW, not off the
	// last click: OBS can put the dock back into the main window under us, and
	// a key still lit after that is a key that lies about where you are.
	const bool full = floating && host->isFullScreen();
	if (fullScreenBtn_->isChecked() != full) {
		QSignalBlocker block(fullScreenBtn_);
		fullScreenBtn_->setChecked(full);
	}
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
	// THE COLUMN COUNT IS PART OF THE SIGNATURE. Without it the early-out
	// below is a re-layout that never happens: the panel switches to a column,
	// asks for a filmstrip, and gets the two-column block back because the
	// cameras and their names did not change.
	const int cols = tileColumns((int)tileSlots.size());
	QStringList sigParts;
	for (int s : tileSlots)
		sigParts << QString::number(s);
	// THE ARRANGEMENT IS PART OF IT TOO, and leaving it out was a hole the
	// column count happened to cover most of the time: the spare row and the
	// spare column are chosen by the arrangement, not by the cameras, so on a
	// rig whose column count is the same in two arrangements - two cameras
	// are two columns in both - turning the panel kept the other one's rule.
	const QString sig = QString::number(show ? 1 : 0) + '|' +
			    QString::number((int)panelMode_) + '|' +
			    QString::number(cols) + '|' + sigParts.join(',') +
			    '|' + captions.join(',');
	if (sig == multiviewSig_)
		return;
	multiviewSig_ = sig;

	// HOW MANY CAMERAS THERE ARE INVALIDATES THE OPERATOR'S DIVIDER, exactly
	// as changing the arrangement does, and for the same reason. "This much
	// for the bays, that much for the cameras" is an answer about a rig of
	// eight; on a rig of four the camera block needs a third of that width
	// and the bays should have the rest. Kept, the panel came back from
	// Settings with A still the size it was for eight and nothing on screen
	// explaining why. The split is re-derived once; he may drag it again.
	if ((int)tileSlots.size() != lastTileCount_) {
		lastTileCount_ = (int)tileSlots.size();
		for (bool &chosen : userMonitorSplit_)
			chosen = false;
	}

	multiviewBox_->setVisible(show);
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
		// CAPPED IN A COLUMN, free beside a big A. Without a cap the grid
		// hands a lone tile the whole row and it draws itself as big as
		// the picture being watched — the same angle, twice, and the event
		// list paying for the second copy.
		// applyPreviewAspect settles the real ceiling a moment later; this
		// only stops a freshly shown tile claiming the whole row for one
		// frame.
		t.box->setMaximumWidth(tileCap_ > 0 ? tileCap_ : QWIDGETSIZE_MAX);
		multiviewGrid_->addWidget(t.box, (int)k / cols, (int)k % cols);
		t.box->setVisible(show);
	}
	// THE COLUMNS IN USE SHARE THE ROW, and a box in this grid has no size
	// of its own to fall back on: AspectBox declares no floor and no hint
	// deliberately (a floor here becomes the panel's). A column left at
	// stretch 0 beside one that has it is therefore not "narrow", it is
	// ZERO — which is how the column arrangement came to show an empty box
	// where the cameras are. It was drawn correctly only by accident: the
	// early-out above kept the wide arrangement's stretches in place on a rig
	// whose column count did not change when the panel was turned, and the
	// column rule below was never actually reached.
	//
	// The leftover cannot land in the tiles either way, because each one is
	// capped at the width its own picture may have (see applyPreviewAspect):
	// it stays as trailing space, so a filmstrip of two on a rig of two still
	// starts at the left edge instead of floating in the middle of the row.
	for (int c = 0; c <= cols; c++)
		multiviewGrid_->setColumnStretch(c, c < cols ? 1 : 0);
	// THE ROWS IN USE SHARE THE BLOCK; the rest hold nothing. A
	// QGridLayout remembers the stretch of a row it no longer has anything
	// in and rowCount() never comes back down, so a rig that once wanted
	// four rows keeps them stretching after it wants one - and the pictures
	// get a quarter of the height the block was given. Rows past the last
	// one in use are also where the hidden tiles were left sitting.
	const int usedRows =
		((int)tileSlots.size() + cols - 1) / std::max(1, cols);
	// NO SPARE ROW: the camera block is now exactly as tall as the bays
	// beside it (tileBlockFor settles ONE height for the whole monitoring
	// row), so there is nothing left over to park. A spare row here is the
	// empty band that used to sit under C1 and C2.
	//
	// The bound is usedRows + 1, NOT rowCount(): setRowStretch on an index
	// past the end GROWS the grid, so a loop that walks to rowCount() adds a
	// row every time it runs - and this runs whenever the cameras change.
	for (int r = 0;
	     r < std::max(usedRows + 1, multiviewGrid_->rowCount()); r++)
		multiviewGrid_->setRowStretch(r, r < usedRows ? 1 : 0);
	tileTallyPvw_ = -2; // captions were just rewritten
	tileTallyPgm_ = -2;
	updateMultiviewTally();
	// A tile appeared or went away, so the pane needs a different height: its
	// cap is the sum of the pictures actually in it.
	applyPreviewAspect();
}

void MultiReplayDock::refreshTileSources()
{
	const Config cfg = ReplayCore::instance().getConfig();
	// LIVE OR REVIEW — the same question the big preview asks, and the same
	// answer.
	//
	// Following live, a tile is a confidence monitor: that camera as it is
	// now. Out of follow-live — the operator has cued a row, played an event
	// or moved the bar — a tile still showing the camera as it is NOW is worse
	// than a black rectangle, because it reads as the footage of the moment
	// being reviewed and is not. So it shows that moment on its own lens,
	// which is what the boxes are for: choosing the angle before the replay
	// goes up.
	const bool live = ReplayCore::instance().followLive();
	tilesLive_ = live;
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
			} else if (live) {
				const std::string &nm =
					cfg.cameras[t.cam0].sourceName;
				if (!nm.empty())
					next = obs_get_source_by_name(nm.c_str());
			} else if (t.cam0 < (int)tileFeed_.size() &&
				   tileFeed_[t.cam0]) {
				// The STICKY flag, not the feed's hasPosition().
				// "Has this feed ever shown a picture" is the
				// question; hasPosition() answers "has THIS clip
				// pushed a frame yet", and play() zeroes the
				// stats at the start of every clip — so asking it
				// blacked the tile out at every cue and left it
				// black until the next 4 Hz beat. See
				// tileFeedHadPicture_.
				if (tileFeedHadPicture_[t.cam0])
					next = tileFeed_[t.cam0]->acquireSource();
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

// ---------------------------------------------------------------------------
// The angle boxes during a review — a preview feed per camera
// ---------------------------------------------------------------------------

void MultiReplayDock::ensureTileFeeds()
{
	const Config cfg = ReplayCore::instance().getConfig();
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		// A feed exists for a camera the operator has configured AND whose
		// tile is on screen. Monitors off, the multiview switched off, an
		// unconfigured slot: no feed, no decoder, no source. That is the
		// whole cost control, and it is the same one the tiles themselves
		// already use.
		const bool wanted =
			!cfg.cameras[cam].sourceName.empty() &&
			cam < kMaxPreviewTiles && tiles_[cam].box &&
			tiles_[cam].box->isVisible();
		if (wanted == (bool)tileFeed_[cam])
			continue;
		if (!wanted) {
			// Destroying it stops and joins its worker; the private
			// input goes with it.
			tileFeed_[cam].reset();
			tileFeedHadPicture_[cam] = false;
			continue;
		}
		tileFeed_[cam] = ReplayChannel::makePreview(
			"MultiReplay - Angle feed " + std::to_string(cam + 1));
		// A brand new feed really has nothing behind it, which is the one
		// moment the tile SHOULD be black.
		tileFeedHadPicture_[cam] = false;
	}
}

bool MultiReplayDock::pollTileFeedPictures()
{
	bool changed = false;
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		// Only the ones that have not answered yes yet, so once every feed
		// has a picture this loop is eight bool reads and nothing else. It
		// runs on EVERY tick on purpose: the first picture is what the
		// operator is waiting for, and making him wait for the 4 Hz beat
		// as well is precisely the quarter second this is fixing.
		if (tileFeedHadPicture_[cam] || !tileFeed_[cam])
			continue;
		if (tileFeed_[cam]->hasPosition()) {
			tileFeedHadPicture_[cam] = true;
			changed = true;
		}
	}
	return changed;
}

void MultiReplayDock::prefetchTiles(int64_t inNs, int64_t outNs, int speedPct)
{
	if (inNs == kNoInstant || outNs == kNoInstant || outNs <= inNs)
		return;
	// Capped exactly the way cueTiles() caps, or the two would ask for
	// different ranges and the cache key would never match — a prefetch that
	// cannot be hit is a file read for nothing.
	outNs = std::min(outNs, inNs + kTileReviewMaxNs);
	const int pct = std::clamp(speedPct > 0 ? speedPct : 100, 5, 400);
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		// SKIP THE ONES STILL READING. prefetch() joins the previous
		// worker on the calling thread, and this thread is the one that
		// draws the panel: with a feed per camera, an operator walking the
		// list faster than a cold read completes would stall the dock by
		// up to a file read PER ANGLE, and the stall would come and go
		// with how warm the cache happened to be. Skipping is free — a
		// prefetch that is not issued only means play() fetches for
		// itself, which is the arrangement everything here already
		// tolerates.
		if (!tileFeed_[cam] || tileFeed_[cam]->prefetchBusy())
			continue;
		ReplayChannel::PlayRequest req;
		req.camIndex = cam;
		req.inNs = inNs;
		req.outNs = outNs;
		req.speedPct = pct;
		tileFeed_[cam]->prefetch(req);
	}
}

void MultiReplayDock::cueTiles(int64_t inNs, int64_t outNs, int speedPct,
			       ReplayChannel::Direction dir, int maxFrames)
{
	if (inNs == kNoInstant || outNs == kNoInstant || outNs <= inNs)
		return;
	// Capped, because a feed materialises its clip's packets in RAM and a free
	// review can ask for a whole session. Past the cap the tiles simply hold
	// the last frame they were given, which is still the reviewed moment on
	// that lens.
	outNs = std::min(outNs, inNs + kTileReviewMaxNs);
	const int pct = std::clamp(speedPct > 0 ? speedPct : 100, 5, 400);
	// CALLED FROM poll(), thirty times a second. Re-issuing the same cue would
	// restart every feed on every tick — eight decoders started and joined
	// thirty times a second, which is not slow, it is a dock that never shows
	// a picture.
	if (inNs == tileCueInNs_ && outNs == tileCueOutNs_ &&
	    pct == tileCueSpeedPct_ && dir == tileCueDir_ &&
	    maxFrames == tileCueMaxFrames_)
		return;
	tileCueInNs_ = inNs;
	tileCueOutNs_ = outNs;
	tileCueSpeedPct_ = pct;
	tileCueDir_ = dir;
	tileCueMaxFrames_ = maxFrames;

	ensureTileFeeds();
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		if (!tileFeed_[cam])
			continue;
		ReplayChannel::PlayRequest req;
		req.camIndex = cam;
		req.inNs = inNs;
		req.outNs = outNs;
		req.speedPct = pct;
		req.direction = dir;
		req.maxFrames = maxFrames;
		std::string err;
		if (!tileFeed_[cam]->play(req, err))
			// NOT a notice, and not a warning either. An angle with
			// nothing behind it at that instant is ordinary — a camera
			// added mid-session, a slot whose ring has wrapped — and
			// the tile answering with black is exactly right. Shouting
			// about it on every cue would train the operator to ignore
			// the strip that carries the messages that matter.
			MR_DLOG("[dock] angle feed %d: %s", cam + 1, err.c_str());
	}
}

void MultiReplayDock::releaseTileFeeds()
{
	for (auto &f : tileFeed_)
		f.reset();
	tileFeedHadPicture_.fill(false);
	tileCueInNs_ = kNoInstant;
	tileCueOutNs_ = kNoInstant;
	tileCueSpeedPct_ = 0;
	tileCueDir_ = ReplayChannel::Direction::Forward;
}

MultiReplayDock::MultiviewState MultiReplayDock::multiviewState() const
{
	MultiviewState s;
	s.followingReview = !ReplayCore::instance().followLive();
	for (int cam = 0; cam < (int)tileFeed_.size(); cam++) {
		if (!tileFeed_[cam])
			continue;
		s.feeds++;
		// The sticky flag, which is what the tile PUBLISH keys on — so the
		// gate asserts the property the operator can actually see, not a
		// per-clip counter that is false for the first tens of
		// milliseconds of every clip.
		if (tileFeedHadPicture_[cam])
			s.feedsWithPicture++;
		if (tileFeed_[cam]->hasPosition())
			s.feedsWithCurrentClip++;
	}
	s.cueInNs = tileCueInNs_;
	s.cueOutNs = tileCueOutNs_;
	{
		// The camera tiles only: the replay tile does not exist any more,
		// but the slot arithmetic still says so, and counting a slot that
		// is not a camera would make the number disagree with the strip.
		std::lock_guard<std::mutex> lk(tileMutex_);
		for (int i = 0; i < kMaxPreviewTiles; i++)
			if (tiles_[i].cam0 >= 0 && tileSource_[i])
				s.tilesPublished++;
	}
	return s;
}

std::vector<OBSQTDisplay *> MultiReplayDock::allDisplays() const
{
	std::vector<OBSQTDisplay *> out;
	out.reserve(2 + tiles_.size());
	if (displayA_)
		out.push_back(displayA_);
	if (displayB_)
		out.push_back(displayB_);
	for (const PreviewTile &t : tiles_)
		if (t.display)
			out.push_back(t.display);
	return out;
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
	for (const OBSQTDisplay *d : allDisplays())
		account(d);
	return s;
}

// Where the zones ended up, in the dock's own coordinates. See LayoutProbe in
// the header for why the gate is allowed to ask.
int MultiReplayDock::heldSourceRefs() const
{
	int n = 0;
	{
		std::lock_guard<std::mutex> lk(previewMutex_);
		n += previewSource_ ? 1 : 0;
		n += previewSourceB_ ? 1 : 0;
	}
	// ...and the angle feeds. Their inputs are PRIVATE, so they are not part
	// of the operator's scene collection — but they are sources libobs is
	// holding on our behalf, and "released everything before OBS cleared its
	// data" is a claim that has to include them or it is not the claim.
	for (const auto &f : tileFeed_)
		n += f ? 1 : 0;
	std::lock_guard<std::mutex> lk(tileMutex_);
	for (obs_source_t *s : tileSource_)
		n += s ? 1 : 0;
	return n;
}

void MultiReplayDock::releasePreviewRefs()
{
	// The static half only finds the live dock; the work is the member below,
	// so the destructor can do it too WITHOUT going through the pointer it has
	// already cleared.
	if (g_dock)
		g_dock->dropPreviewRefs();
}

void MultiReplayDock::prepareForShutdown()
{
	if (!g_dock)
		return;
	// COME OUT OF FULL SCREEN BEFORE OBS WRITES ITS LAYOUT. OBS saves the dock
	// geometry on the way out and restores it on the way in, so a panel left
	// full screen here comes back next launch as a floating window the size of
	// a whole monitor with no way to tell it was ever anything else — and the
	// operator's own window size, the one this key was careful to remember, is
	// gone with it.
	g_dock->setPanelFullScreen(false);
	// STOP POLLING FIRST, then let go. Releasing alone was not enough: the poll
	// timer runs at 30 Hz and re-resolves these references on its slow beat, so
	// between "OBS says it is exiting" and "OBS clears scene data" a tick could
	// slip in and take a fresh reference — and OBS then named that one source in
	// the dialog. Measured: the take pass came out clean and the reopen pass,
	// where shutdown follows polling more closely, still reported one.
	if (g_dock->pollTimer_)
		g_dock->pollTimer_->stop();
	g_dock->dropPreviewRefs();
}

void MultiReplayDock::dropPreviewRefs()
{
	obs_source_t *a = nullptr;
	obs_source_t *b = nullptr;
	{
		// Swapped out under the lock, released outside it: the last release
		// destroys the source and enters the graphics context, which is the
		// thread that wants previewMutex_.
		std::lock_guard<std::mutex> lk(previewMutex_);
		a = previewSource_;
		b = previewSourceB_;
		previewSource_ = nullptr;
		previewSourceB_ = nullptr;
	}
	if (a)
		obs_source_release(a);
	if (b)
		obs_source_release(b);

	// ...and the MULTIVIEW tiles, which are the operator's own cameras. These
	// were the "- C1 / - C2" in the same OBS complaint: one owned reference per
	// tile, dropped only in the destructor, which runs long after scene data has
	// been cleared. Same swap-then-release discipline (see above) for the same
	// reason: the last release enters the graphics context.
	{
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
	// ...and the angle FEEDS, which own private inputs of their own. Same
	// reason and the same deadline: this runs on the way out of a scene
	// collection and on exit, and anything still alive at that moment is
	// reported to the operator as a plugin that leaked. Destroying a feed
	// stops and joins its worker first, which is why it is safe here — the
	// worker takes no lock this thread can be holding.
	releaseTileFeeds();
	// So poll() resolves them all again rather than trusting its cache.
	previewCam0_ = -1;
}

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
	lp.channelBVisible = bBox_ && bBox_->isVisibleTo(this);
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
	const int pvw = currentAngle1() - 1;
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
	QWidget *sel = buildChannelRow();
	QVector<Cell> row;
	row << Cell(sel, 1, false);
	// The swap skips a column: ⇄ is not a fourth mode, and pressed by mistake
	// it puts the wrong clip on air.
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
	sel->setFixedHeight(kKeyH);
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

void MultiReplayDock::applyChannelBVisibility()
{
	const bool on = ReplayCore::instance().getConfig().enableChannelB;
	if (channelBApplied_ == (on ? 1 : 0))
		return;
	channelBApplied_ = on ? 1 : 0;
	channelBEnabled_ = on;

	// The B box, B's angle row, the A|B/A/B selector and the swap key are all
	// one decision. With one bay they are not "disabled" — they are ABSENT: a
	// greyed-out selector on a single-channel rig is three keys of furniture
	// and a question the operator has to answer every time he looks at it.
	if (bBox_)
		bBox_->setVisible(on);
	// …and the picture grid has to be told, because B's share of the room is a
	// property of its ROW or COLUMN rather than of the box in it: hiding the
	// box alone leaves the space it had. Re-applying the arrangement re-reads
	// the flag and sets that share to zero.
	applyPanelMode(panelMode_, /*force*/ true);
	// The selector and the swap COLLAPSE rather than holding their place: a
	// bay the operator has switched off is not coming back until he switches
	// it on, and with them gone the section is empty and takes no room at all.
	for (QWidget *w : channelBWidgets_) {
		QSizePolicy sp = w->sizePolicy();
		sp.setRetainSizeWhenHidden(false);
		w->setSizePolicy(sp);
		w->setVisible(on);
	}
	// The section is a row shorter or a row longer, which the strip only
	// learns when the block is re-measured.
	if (strip_ && angleBlock_)
		strip_->blockChanged(angleBlock_);

	if (on) {
		// TWO bays, so the resting state is BOTH: the reference controller's A|B is what makes a
		// two-bay panel one panel, and the operator who asked for a second
		// bay asked for it to be used. He can still pick A or B.
		setActiveChannel(Which::A, /*linked*/ true);
		if (QAbstractButton *ab = chanSel_ ? chanSel_->button(2) : nullptr)
			ab->setChecked(true);
		// The input only exists once he has asked for it.
		ReplayChannel::instance(Which::B).ensureSource();
	} else {
		// One bay: everything means A, and B is stopped rather than left
		// running behind a hidden box.
		setActiveChannel(Which::A, /*linked*/ false);
		if (QAbstractButton *ab = chanSel_ ? chanSel_->button(0) : nullptr)
			ab->setChecked(true);
		PlaybackCoordinator::instance(Which::B).stopEvents();
		ReplayChannel::instance(Which::B).reset();
	}
	obs_log(LOG_INFO, "[dock] channel B %s", on ? "enabled (A|B)" : "disabled");
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
			QMessageBox::warning(this, "obs-multireplay",
					     QString::fromStdString(err));
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
	setKeyIcon(playSel, Icon::Play, tintsFor(sc()), 22);
	playSel->setMinimumWidth(64);
	connect(playSel, &QPushButton::clicked, this,
		&MultiReplayDock::playSelected);

	auto *more = new QToolButton(this);
	more->setObjectName("mrGear");
	more->setIcon(iconFor(Icon::Menu, QColor(sc().textKey), 14,
			      devicePixelRatioF()));
	more->setIconSize(QSize(14, 14));
	more->setCursor(Qt::PointingHandCursor);
	more->setToolTip(obs_module_text("Dock.PlayOptions"));
	setKeyId(more, QStringLiteral("playOptions"));
	more->setPopupMode(QToolButton::InstantPopup);
	{
		auto *menu = new QMenu(more);
		auto *actOut = menu->addAction(obs_module_text("Dock.PlayToOutput"));
		auto *actLast = menu->addAction(obs_module_text("Dock.PlayLast"));
		menu->addSeparator();
		auto *actStop = menu->addAction(obs_module_text("Dock.Stop"));
		more->setMenu(menu);
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
				showNotice(QString::fromStdString(err));
		});
		connect(actLast, &QAction::triggered, this, [this]() {
			std::string err;
			if (!pc().playLastEvent(
				    currentAngle1() - 1,
				    toOutputBtn_ && toOutputBtn_->isChecked(),
				    err))
				showNotice(QString::fromStdString(err));
		});
		connect(actStop, &QAction::triggered, this,
			[this]() { pc().stopEvents(); });
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
			showNotice(QString::fromStdString(why));
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
	setKeyIcon(healthBtn_, Icon::Health, tintsFor(sc()), 11);
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
	for (QPushButton *b : {loopBtn_, musicBtn_, toOutputBtn_}) {
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
	strip_->addBlock(buildMarkers(), Lane::Left, false, 1);
	strip_->addBlock(buildAngleMatrix(), Lane::Centre, false, 3);
	strip_->addBlock(buildSpeedBlock(), Lane::Right, false, 5);
	strip_->addBlock(buildRecBlock(), Lane::Left, /*startsLine*/ true, 0);
	strip_->addBlock(buildTransport(), Lane::Centre, false, 2);
	strip_->addBlock(buildExportBlock(), Lane::Right, false, 4);
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
	// The zoom key: it shows the factor because a bar that is showing four
	// seconds of an hour looks exactly like a bar over a four-second
	// session, and the operator has to be able to tell those apart at a
	// glance. Clicking it goes back to the whole timeline.
	zoomBtn_ = new QPushButton(QStringLiteral("1×"), this);
	zoomBtn_->setObjectName("mrChanSel");
	zoomBtn_->setToolTip(obs_module_text("Dock.ZoomFit"));
	setKeyId(zoomBtn_, QStringLiteral("zoom"));
	zoomBtn_->setMinimumWidth(34);
	// AS TALL AS THE BAR IT BELONGS TO. At a key's height it stood against the
	// bar's top edge with a notch of panel under it, which reads as a control
	// that has come loose from the thing it controls.
	zoomBtn_->setFixedHeight(kSeekTrackH + kSeekRulerH + 2);
	zoomBtn_->setCursor(Qt::PointingHandCursor);
	// A MENU of spans, not a reset. The wheel is how you zoom by feel, but "show
	// me the last five minutes" is a thing an operator wants exactly and cannot
	// reach by rolling a wheel over an hour of footage — and the factor that
	// gets there depends on how long the session is, which is arithmetic he
	// should not be doing. The entries are durations for that reason; the factor
	// is computed from the timeline as it stands when the entry is picked.
	connect(zoomBtn_, &QPushButton::clicked, this,
		&MultiReplayDock::showZoomMenu);
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
	{
		// The zoom key sits at the right end OF THE BAR, not in a
		// toolbar: it is about this control and nothing else, and it is
		// also where the eye lands after reading the scale.
		auto *barBox = new QWidget(this);
		auto *row = new QHBoxLayout(barBox);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(4);
		row->addWidget(seek_, 1);
		row->addWidget(zoomBtn_, 0);
		// …and none over the position bar either: it is graduated, which is
		// how an operator recognises a scrubber, and a caption above it was
		// naming the one control on the panel that names itself.
		v->addWidget(barBox);
	}

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
	gear->setIcon(iconFor(Icon::Gear, QColor(sc().textKey), 15,
			      devicePixelRatioF()));
	gear->setIconSize(QSize(15, 15));
	setKeyId(gear, QStringLiteral("settings"));
	gear->setCursor(Qt::PointingHandCursor);
	gear->setToolTip(obs_module_text("Dock.Settings"));
	gear->setPopupMode(QToolButton::InstantPopup);
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
		gear->setMenu(menu);
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
	speed_->setFixedHeight(kKeyH);
	// THE DIAL SITS UNDER THE PRESETS, in both arrangements: they are one
	// control at two resolutions, and side by side the dial is a strip of
	// nothing between two groups of keys.
	//
	// AND THE EXPORT KEY UNDER BOTH. There were two of them in a section of
	// their own — one clip, or the whole selection as one file — and that is a
	// QUESTION, not two keys. One key that asks it takes half the room and
	// stops the operator having to know the difference before he has decided
	// he wants to export at all.
	auto *exportKey = buildExportKey();
	blk->setShapes({{Cell(chips[0]), Cell(chips[1]), Cell(chips[2]),
			 Cell(chips[3]), Cell(chips[4]), Cell(chips[5])},
			{Cell(speed_, 5), Cell(speedLbl_, 1, false)},
			{Cell(exportKey, 6)}},
		       // The six presets stay on ONE row folded as well. They are
		       // the narrowest keys on the panel and splitting them across
		       // two rows bought nothing but a line — and it broke the run
		       // of values an operator reads left to right.
		       {{Cell(chips[0]), Cell(chips[1]), Cell(chips[2]),
			 Cell(chips[3]), Cell(chips[4]), Cell(chips[5])},
			{Cell(speed_, 5), Cell(speedLbl_, 1, false)},
			{Cell(exportKey, 6)}});
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
	edit->setIcon(iconFor(Icon::More, QColor(sc().textKey), 14,
			      devicePixelRatioF()));
	edit->setIconSize(QSize(14, 14));
	edit->setCursor(Qt::PointingHandCursor);
	edit->setToolTip(obs_module_text("Dock.ClipActions"));
	setKeyId(edit, QStringLiteral("clipActions"));
	edit->setPopupMode(QToolButton::InstantPopup);
	{
		auto *menu = new QMenu(edit);
		auto *actDup = menu->addAction(obs_module_text("Dock.Duplicate"));
		auto *actDel = menu->addAction(obs_module_text("Dock.Delete"));
		menu->addSeparator();
		auto *actAll = menu->addAction(obs_module_text("Dock.DeleteAll"));
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
				    obs_module_text("Dock.DeleteAllConfirm"),
				    QMessageBox::Yes | QMessageBox::No,
				    QMessageBox::No) != QMessageBox::Yes)
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

	blk->setShapes({{Cell(order[0]), Cell(order[1]), Cell(edit)}},
		       {{Cell(order[0]), Cell(order[1]), Cell(edit)}});
	return blk;
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
		const QString folder = QFileDialog::getExistingDirectory(
			this, obs_module_text("Dock.ExportFolder"));
		if (folder.isEmpty())
			return;
		std::string err;
		if (picked == one) {
			for (int id : ids)
				ExportManager::instance().exportEvent(
					id, 0, folder.toStdString(), err);
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
			showNotice(QString::fromStdString(err));
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
			QMessageBox::information(
				this, "obs-multireplay",
				obs_module_text("Dock.NoOpenEvent"));
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
				showNotice(QString::fromStdString(err));
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

	const bool wasProgrammatic = itemsProgrammatic_;
	itemsProgrammatic_ = true; // setHorizontalHeaderItem must not read back as an edit
	events_->setRowCount(0);
	events_->setColumnCount(kColFirstCam +
			        (int)cams.size() * kColsPerCam);
	QStringList headers;
	headers << QStringLiteral("#") << obs_module_text("Dock.In")
		<< obs_module_text("Dock.Out") << obs_module_text("Dock.Duration")
		<< obs_module_text("Dock.Comment");
	// One heading per camera, and the camera cell now holds the two things
	// that really do differ per lens - does it play, and how fast. WHAT the
	// event is has a column of its own, once per row.
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
	// AND THE ARRANGEMENT AGAIN. setColumnCount() re-initialises the header's
	// sections, which takes the hidden state of OUT with it — so on a narrow
	// panel the column the column arrangement had put away came back the first
	// time a camera was added or renamed, and the table went back to needing a
	// horizontal scroll. The mode is the authority on which columns fit; it has
	// to be asked again whenever the columns are rebuilt.
	events_->setColumnHidden(kColOut, panelMode_ == PanelMode::Tall);

	itemsProgrammatic_ = wasProgrammatic;
	updateCamHeaderHighlight();
}

void MultiReplayDock::updateCamHeaderHighlight()
{
	if (!events_ || camCols_.empty())
		return;
	int hot = -1;
	for (size_t i = 0; i < camCols_.size(); i++)
		if (camCols_[i] == currentAngle1() - 1)
			hot = kColFirstCam + (int)i * kColsPerCam;
	if (hot == camHeaderHot_)
		return;
	const bool wasProgrammatic = itemsProgrammatic_;
	itemsProgrammatic_ = true;
	for (size_t i = 0; i < camCols_.size(); i++) {
		const int col = kColFirstCam + (int)i * kColsPerCam;
		QTableWidgetItem *h = events_->horizontalHeaderItem(col);
		if (!h)
			continue;
		// The header is the camera's NAME and nothing else. It used to
		// gain a ▶ on the angle being watched, which said the same thing
		// the angle keys already say, twice as far from where the operator
		// was looking — and it moved every column heading sideways by a
		// glyph as he switched angles.
		h->setText(h->data(Qt::UserRole).toString());
	}
	itemsProgrammatic_ = wasProgrammatic;
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
		int64_t now = PacketTap::instance().newestNs(currentAngle1() - 1);
		if (now > 0) {
			MR_DLOG("[ev] markTime LIVE master=%lldms (cam %d)",
				(long long)(now / 1000000), currentAngle1());
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
		// Both frame keys, because finding the right frame means going
		// past it and coming back — and that is held-key work on a
		// Stream Deck, watching the picture rather than the panel.
		{"ReplayStepBackward", "Hotkey.StepBackward",
		 [](MultiReplayDock *d) { d->stepFrameBackward(); }},
		{"ReplayPlayReverse", "Hotkey.PlayReverse",
		 [](MultiReplayDock *d) { d->playSelectedReverse(); }},
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

bool MultiReplayDock::playOnTargets(const std::vector<int> &ids,
				    ReplayChannel::Direction direction,
				    std::string &errorOut)
{
	// EVERY bay the selector points at, which under A|B is both — that is what
	// A|B promises and it was not being kept: play went to the active channel
	// only, so A|B behaved exactly like A and the second bay sat idle behind a
	// lit key.
	//
	// "To output" is the one thing that CANNOT go to both, because Program is
	// one scene. So it goes to the bay the operator nominated in Settings
	// (Config.abOutputUsesB) and the other bay plays without touching Program.
	const bool toOut = toOutputBtn_ && toOutputBtn_->isChecked();
	// Playing EVENTS is talking about events, so an unmarked stretch armed on
	// the bar stops being what the play keys mean. (playSelected() answers the
	// armed review before it ever gets here; this catches ◀ and the hotkeys,
	// which go straight to the events.)
	clearFreeReview();
	const bool useB = ReplayCore::instance().getConfig().abOutputUsesB;
	const auto targets = targetChannels();
	bool any = false;
	for (Which w : targets) {
		const bool thisOne =
			toOut && (targets.size() == 1 ||
				  (useB ? w == Which::B : w == Which::A));
		std::string err;
		if (PlaybackCoordinator::instance(w).playEvents(
			    ids, angle1_[(int)w] - 1, thisOne, err,
			    PlaybackCoordinator::AngleMode::AllEnabled,
			    direction))
			any = true;
		else if (errorOut.empty())
			errorOut = err;
	}
	return any;
}

// ---------------------------------------------------------------------------
// Unmarked footage: watching it, and then deciding to air it
// ---------------------------------------------------------------------------

void MultiReplayDock::clearFreeReview()
{
	freeReviewInNs_ = kNoInstant;
	freeReviewOnAir_ = false;
}

bool MultiReplayDock::playheadIsFreeFootage() const
{
	// Following live there is no "here" to play: the bar is at the front and
	// the footage under it is the instant that has just happened. ▶ keeps
	// meaning what it has always meant.
	if (ReplayCore::instance().followLive())
		return false;
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0)
		return false;

	// Asked of the SELECTION, not of the whole list. The table auto-selects
	// the newest mark, so "is there an event somewhere near here" would answer
	// yes for the whole match; the question that decides what ▶ does is
	// whether the bar is standing on the event ▶ would otherwise replay.
	std::vector<int> ids = selectedEventIds();
	if (ids.empty())
		ids = {EventStore::instance().lastEventId()};
	if (ids.empty() || ids.front() <= 0)
		return true; // nothing marked at all: it is all free footage
	ReplayEvent ev;
	if (!EventStore::instance().get(ids.front(), ev) ||
	    ev.tInNs == kNoInstant)
		return true;
	const int64_t outNs =
		(ev.tOutNs != kNoInstant && ev.tOutNs > ev.tInNs) ? ev.tOutNs
								  : ev.tInNs;
	// Half a second of slack at both ends. Cueing a row parks the bar ON the
	// in-point and the engine's own frame instants drift by a frame or two, so
	// an exact comparison would call the cued event "free footage" about as
	// often as not — and ▶ would then play something else than the row the
	// operator is looking at.
	constexpr int64_t kSlackNs = 500'000'000LL;
	return playheadNs_ < ev.tInNs - kSlackNs ||
	       playheadNs_ > outNs + kSlackNs;
}

bool MultiReplayDock::playFreeReview(bool toOutput)
{
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return false;
	}
	// From the bar to the end of the footage, cut at the seams and CHUNKED.
	//
	// One range would be wrong twice over. It would include the gaps between
	// takes, which are not footage and which the engine refuses outright (the
	// same reason a scrub review is split — see seekToFraction); and a single
	// range of "everything from here" would ask the engine to materialise a
	// whole session's packets in RAM. A queue of minute-long links is neither:
	// the coordinator chains them exactly as it chains the angles of an event,
	// and each link is about thirty megabytes.
	const int64_t inNs = playheadNs_;
	const int64_t edge = timelineStartNs_ + displayDurNs_;
	std::vector<std::pair<int64_t, int64_t>> ranges;
	const auto chop = [&](int64_t a, int64_t b) {
		for (int64_t t = a;
		     t < b && (int)ranges.size() < kFreeReviewMaxChunks;
		     t += kFreeReviewChunkNs)
			ranges.push_back({t, std::min(b, t + kFreeReviewChunkNs)});
	};
	if (timeline_.empty()) {
		if (edge > inNs)
			chop(inNs, edge);
	} else {
		for (const TimelineSpan &s : timeline_.spansFrom(
			     inNs, kFreeReviewChunkNs *
					   (int64_t)kFreeReviewMaxChunks))
			chop(s.startNs, s.endNs);
	}
	if (ranges.empty()) {
		showNotice(obs_module_text("Dock.NoFootageHere"));
		return false;
	}

	auto &core = ReplayCore::instance();
	// Same discipline as a scrub: kill the queue first so its own finish
	// callback cannot cut in, and consume the transition so poll() does not
	// read the stop as "the sequence ended, go back to the live edge".
	pc().stopEvents();
	prevSequenceActive_ = false;
	core.setFollowLive(false);
	playheadNs_ = inNs;
	// ARMED, and it outlives the clip on purpose: the operator watches this
	// off air, presses Stop, decides, and then presses Play events — which has
	// to start from the instant he chose, not from wherever the review
	// happened to stop.
	freeReviewInNs_ = inNs;
	freeReviewOnAir_ = toOutput;

	std::string err;
	if (!pc().playRanges(ranges, currentAngle1() - 1, speedPct_, toOutput,
			     err)) {
		showNotice(QString("%1 — %2")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(QString::fromStdString(err)));
		obs_log(LOG_WARNING,
			"[dock] free review on angle %d at %lld ms refused: %s",
			currentAngle1(),
			(long long)((inNs - timelineStartNs_) / 1000000),
			err.c_str());
		return false;
	}
	obs_log(LOG_INFO,
		"[dock] free review: %zu chunk(s) from %lld ms on angle %d, %s",
		ranges.size(),
		(long long)((inNs - timelineStartNs_) / 1000000),
		currentAngle1(), toOutput ? "TO OUTPUT" : "off air");
	showNotice(obs_module_text(toOutput ? "Dock.FreeReviewOnAir"
					    : "Dock.FreeReviewOffAir"));
	return true;
}

void MultiReplayDock::stopPlayback()
{
	// Every bay the selector points at, because that is what the selector
	// means everywhere else. stopEvents() puts the operator's previous scene
	// back in Program when the sequence had taken it.
	for (Which w : targetChannels())
		PlaybackCoordinator::instance(w).stopEvents();
}

void MultiReplayDock::playSelected()
{
	// THE SECOND FUNCTION OF THIS KEY.
	//
	// With a free review armed — the operator moved the bar onto footage
	// nobody marked and pressed ▶ — this key is what puts THAT on air. It is
	// the only way it can get there: ▶ deliberately plays it off air, so
	// reviewing an unmarked action can never reach Program by itself, and the
	// decision to air it stays a separate press.
	//
	// "In output" still governs, exactly as it does for an event: this key
	// does not have a private route to Program that the panel's own on-air
	// switch cannot see.
	if (freeReviewInNs_ != kNoInstant) {
		playheadNs_ = freeReviewInNs_;
		playFreeReview(toOutputBtn_ && toOutputBtn_->isChecked());
		return;
	}
	std::string err;
	if (!playOnTargets(selectedEventIds(),
			   ReplayChannel::Direction::Forward, err))
		QMessageBox::warning(this, "obs-multireplay",
				     QString::fromStdString(err));
}

void MultiReplayDock::setAngle(int angle1Based)
{
	// The active channel's row. This is what the hotkeys and the numbered
	// keys mean by "the angle".
	setAngleOn(activeChannel_, angle1Based);
}

void MultiReplayDock::setAngleOn(Which which, int angle1Based)
{
	if (angle1Based < 1 || angle1Based > kNCams)
		return;
	angle1_[(int)which] = angle1Based;
	// Shared with the hotkeys, which have no way to reach the dock — and
	// only the ACTIVE channel's angle can be that shared one, because there
	// is one of it. Pressing B's camera 3 while the keys are on A sets B's
	// angle and leaves the hotkeys where they were, which is what the two
	// rows are for.
	if (which == activeChannel_)
		ReplayCore::instance().setCurrentAngle(angle1Based - 1);
	// AN ANGLE KEY CHANGES THE ANGLE. It does not start a replay.
	//
	// While something is on air it re-cues that clip on the chosen camera —
	// which IS "change the angle being watched", the same clip from the same
	// in-point on the other lens. But with nothing playing it only moves the
	// selection: pressing C2 to see what camera 2 is looking at used to put a
	// replay on air (and with "In output" on, on PROGRAM) that nobody had asked
	// for. Same rule the speed keys now follow.
	if (ReplayChannel::instance(which).playing() ||
	    PlaybackCoordinator::instance(which).queueActive())
		replayCurrentOn(which);
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

	// The boxes step with it: a frame step is how an operator finds the exact
	// picture, and finding it on one lens while the others sit a second behind
	// defeats the comparison the strip is there for. Straight at the channel
	// again, so poll() has no queue to read this off.
	cueTiles(inNs, std::min(outNs, edge), 100,
		 ReplayChannel::Direction::Forward);

	std::string err;
	if (!chan().play(currentAngle1() - 1, inNs,
					    std::min(outNs, edge), 100, err))
		showNotice(QString("%1 — %2")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(QString::fromStdString(err)));
}

void MultiReplayDock::stepFrameBackward()
{
	// the reference controller frame-by-frame BACKWARDS, and it is not the forward step with the
	// sign flipped.
	//
	// A step is a range, because the engine plays ranges. Forwards, the range
	// [playhead + 1f, playhead + 3f] comes to rest on a frame past the bar, so
	// the picture moves on. Played FORWARDS, the range [playhead - 1f,
	// playhead + 1f] comes to rest on the frame at the playhead — the one
	// already on screen. The step would look broken while doing exactly what
	// it was told.
	//
	// So the step back is a REVERSE run: pictures come out newest-first, so
	// the run comes to rest on the OLDEST one it shows, and capping it at two
	// pictures makes that "one frame back" and nothing more. The window is
	// three frames wide because frame instants are not multiples of a frame
	// time — an encoder's own timestamps drift — and a window exactly one
	// frame wide can hold only the frame we are already looking at.
	auto &core = ReplayCore::instance();
	if (playheadNs_ == kNoInstant || timelineStartNs_ == kNoInstant ||
	    displayDurNs_ <= 0) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return;
	}

	struct obs_video_info ovi = {};
	int64_t frameNs = 33333333; // 30 fps, if OBS will not say
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
		frameNs = (int64_t)((1000000000LL * (int64_t)ovi.fps_den) /
				    (int64_t)ovi.fps_num);

	const int64_t outNs = playheadNs_;
	const int64_t inNs = outNs - 3 * frameNs;
	if (inNs < timelineStartNs_) {
		// The beginning of the footage: there is no earlier frame, and
		// clamping would silently show the first one again.
		showNotice(obs_module_text("Dock.AtTimelineStart"));
		return;
	}

	// Same discipline as the forward step: kill the queue first so its finish
	// callback cannot cut in, and consume the transition so poll() does not
	// read the stop as "the sequence ended, go back to live".
	pc().stopEvents();
	prevSequenceActive_ = false;
	core.setFollowLive(false);
	// An estimate, corrected by poll() from the frame the engine actually
	// pushes: the exact instant of "one frame back" belongs to the encoder's
	// timestamps, not to our arithmetic.
	playheadNs_ = outNs - frameNs;

	ReplayChannel::PlayRequest req;
	req.camIndex = currentAngle1() - 1;
	req.inNs = inNs;
	req.outNs = outNs;
	req.speedPct = 100;
	req.direction = ReplayChannel::Direction::Reverse;
	req.maxFrames = 2; // the frame on screen, then the one before it
	// The boxes step back with it (see stepFrameForward for why this is here
	// and not in poll()) — but FORWARDS over the same window, not backwards.
	// A tile is not the picture being trimmed: it only has to be on the moment
	// the operator is on, and playing this three-frame window forwards leaves
	// it there for the price of an ordinary decode. Reverse would be a GOP
	// decode per tile per key repeat, and this key is held down.
	cueTiles(inNs, outNs, 100, ReplayChannel::Direction::Forward);
	std::string err;
	if (!chan().play(req, err))
		showNotice(QString("%1 — %2")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(QString::fromStdString(err)));
}

void MultiReplayDock::playSelectedReverse()
{
	// the reference controller's ◀. The same queue, the same angles, the same "to output" — only
	// each clip runs from its OUT back to its IN. Going through the coordinator
	// rather than straight to the channel is what keeps that true: a reverse
	// that talked to the engine directly would be a second playback path, and
	// the first thing it would lose is the scene switch.
	std::vector<int> ids = selectedEventIds();
	if (ids.empty())
		ids = {EventStore::instance().lastEventId()};
	if (ids.empty() || ids.front() <= 0) {
		showNotice(obs_module_text("Dock.NothingSelected"));
		return;
	}
	std::string err;
	if (!playOnTargets(ids, ReplayChannel::Direction::Reverse, err))
		showNotice(QString::fromStdString(err));
}

void MultiReplayDock::applyReplaySpeed(int pct)
{
	// Default speed for every angle without an override — the coordinator
	// resolves it when it builds the queue, including for the hotkeys.
	speedPct_ = std::clamp(pct, 5, 200);
	for (Which w : targetChannels()) {
		PlaybackCoordinator::instance(w).setDefaultSpeedPct(speedPct_);
		// LIVE if something is playing: the clip carries on from the frame
		// on screen at the new spacing. It used to restart from the IN,
		// which threw away the very thing the operator was looking at — and
		// with the dial applying on every step of a drag, restarting would
		// have made the control unusable rather than merely annoying.
		PlaybackCoordinator::instance(w).setLiveSpeedPct(speedPct_);
	}
	// AND WHEN NOTHING IS PLAYING, IT ONLY SETS THE SPEED. Choosing 25% before
	// a replay is choosing how the next one will run, not asking for one now:
	// starting a clip off a preset key put footage on air (with "In output" on,
	// on PROGRAM) that nobody had asked to see. The number is armed and the
	// next Play uses it.
}

// ---------------------------------------------------------------------------
// The keyboard. ONE layer of input: every key calls the button of the same name.
// ---------------------------------------------------------------------------

bool MultiReplayDock::focusIsTextEntry()
{
	QWidget *fw = QApplication::focusWidget();
	if (!fw)
		return false;
	// An editable combo's field is a QLineEdit child, so this one cast covers
	// the per-angle comment cells as well as the search box — the same question
	// refreshEvents() asks before it rebuilds the table under an editor.
	return qobject_cast<QLineEdit *>(fw) ||
	       qobject_cast<QPlainTextEdit *>(fw) ||
	       qobject_cast<QAbstractSpinBox *>(fw);
}

bool MultiReplayDock::handleTransportKey(QKeyEvent *event)
{
	if (!event)
		return false;
	// Modified keys belong to OBS and to the operator's own shortcuts. Shift is
	// ours, and only as a MAGNITUDE: Shift+arrow is the same movement in a
	// bigger unit, never a different command.
	const Qt::KeyboardModifiers mods = event->modifiers();
	if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
		return false;
	// Typing wins, always. These are single-key commands sitting two pixels
	// from a search box and a column of comment cells.
	if (focusIsTextEntry())
		return false;

	const bool shift = mods.testFlag(Qt::ShiftModifier);
	switch (event->key()) {
	case Qt::Key_Left:
		if (shift)
			scrubBySeconds(-1.0);
		else
			stepFrameBackward();
		return true;
	case Qt::Key_Right:
		if (shift)
			scrubBySeconds(+1.0);
		else
			stepFrameForward();
		return true;
	case Qt::Key_Up:
		stepEventSelection(-1);
		return true;
	case Qt::Key_Down:
		stepEventSelection(+1);
		return true;
	// Both faces of each key: the one on the main row and the one on the
	// numeric pad, which is where a hand on a controller actually is.
	case Qt::Key_Plus:
	case Qt::Key_Equal:
		nudgeSpeed(+kSpeedKeyStepPct);
		return true;
	case Qt::Key_Minus:
	case Qt::Key_Underscore:
		nudgeSpeed(-kSpeedKeyStepPct);
		return true;
	// Esc leaves full screen, and ONLY that: while the panel is a window
	// inside OBS the key is not ours, and handing it back is what lets a
	// dialog or a popup keep it. A full-screen floating dock has no title bar
	// and therefore no close button, so this is the escape everyone reaches
	// for first — the ⛶ key on the row is the other one.
	case Qt::Key_Escape:
		if (!panelIsFullScreen())
			return false;
		setPanelFullScreen(false);
		refreshFullScreenKey();
		return true;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		playSelected();
		return true;
	default:
		return false;
	}
}

void MultiReplayDock::keyPressEvent(QKeyEvent *event)
{
	// Reached when the focused child ignored the key (a button, a label, the
	// panel itself). The table does NOT ignore arrows or Enter, so it is
	// filtered separately — see eventFilter.
	if (handleTransportKey(event)) {
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}

void MultiReplayDock::stepEventSelection(int delta)
{
	if (!events_ || events_->rowCount() == 0)
		return;
	const auto sel = events_->selectionModel()->selectedRows();
	const int rows = events_->rowCount();
	// With nothing selected, ↓ takes the first row and ↑ the last: the key says
	// which end of the list the operator is coming from.
	const int cur = sel.empty() ? (delta > 0 ? -1 : rows) : sel.first().row();
	const int next = std::clamp(cur + delta, 0, rows - 1);
	if (next == cur)
		return; // at the end of the list: refuse rather than wrap
	// This is the OPERATOR selecting, so it cues the event — which is the whole
	// point of walking the list with the keys (see cueSelected, and the
	// reselecting_ guard that keeps our own re-selection from cueing).
	events_->selectRow(next);
	if (QTableWidgetItem *it = events_->item(next, kColId))
		events_->scrollToItem(it);
}

void MultiReplayDock::scrubBySeconds(double seconds)
{
	// Along the FOOTAGE axis, not wall time: the bar is drawn over the recorded
	// spans joined end to end, so "one second earlier" has to mean one second of
	// material — a session with a pause in it would otherwise send the playhead
	// into a gap that no footage covers.
	if (timeline_.empty() || playheadNs_ == kNoInstant) {
		showNotice(obs_module_text("Dock.NothingToStep"));
		return;
	}
	const int64_t total = timeline_.totalNs();
	if (total <= 0)
		return;
	int64_t at = timeline_.footageBefore(playheadNs_) +
		     (int64_t)std::llround(seconds * 1e9);
	at = std::clamp(at, (int64_t)0, total);
	// Through seekToFraction, so a keyboard scrub is the same gesture as a
	// dragged one: it reviews from there, consumes the sequence transition and
	// says so when that instant holds no footage.
	seekToFraction((double)at / (double)total);
}

void MultiReplayDock::nudgeSpeed(int deltaPct)
{
	// The dial's own range, and applyReplaySpeed does the rest: it re-speeds the
	// clip on air without restarting it, and merely arms the number when nothing
	// is playing. poll() moves the slider to match, so the widget cannot end up
	// disagreeing with the speed.
	applyReplaySpeed(std::clamp(speedPct_ + deltaPct, 5, 200));
}

void MultiReplayDock::cueSelected()
{
	// Selecting a row SHOWS that event, on the channel the selector points at.
	// the reference controller loads the event into the selected bay the moment you pick it; here it
	// used to sit there until "Play events" was pressed, so the operator was
	// choosing clips blind.
	//
	// A cue is not a playback: two frames from the IN, which is the shortest
	// range the engine will serve, and it comes to rest on the first of them —
	// so the IN frame is what stays on screen. (There is no freeze-frame in the
	// engine: it plays ranges. A range of two is a still.)
	const auto ids = selectedEventIds();
	if (ids.empty() || ids.front() <= 0)
		return;
	ReplayEvent ev;
	if (!EventStore::instance().get(ids.front(), ev) || ev.tInNs == kNoInstant)
		return;

	struct obs_video_info ovi = {};
	int64_t frameNs = 33333333;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
		frameNs = (int64_t)((1000000000LL * (int64_t)ovi.fps_den) /
				    (int64_t)ovi.fps_num);

	auto &core = ReplayCore::instance();
	// Choosing a clip is reviewing, so the preview stops mirroring the camera —
	// otherwise the cue would land on a source nobody is looking at.
	core.setFollowLive(false);
	prevSequenceActive_ = false;
	playheadNs_ = ev.tInNs;
	// Picking a row is talking about THAT event, so whatever unmarked stretch
	// was armed on the bar is no longer what the play keys mean.
	clearFreeReview();

	// START THE FEEDS' FETCHES BEFORE THE BAY'S, AND LET THEM RUN UNDER IT.
	//
	// This is what makes the bay and the boxes arrive together, and the order
	// of these three steps IS the mechanism. A fetch that is not in the ring
	// is an open, a seek and a demux — about a tenth of a second — and play()
	// does it inline, on this thread. Three inline fetches in a row (bay, then
	// each feed) means somebody is a third of a second late by construction,
	// and which somebody depends only on who was called first: the boxes used
	// to be, so the BAY was the one that came in late.
	//
	// prefetch() is asynchronous and each feed has a thread of its own, so
	// asking them all first puts their reads in flight in PARALLEL; the bay
	// then does its own fetch inline, and by the time cueTiles() runs below
	// the feeds' clips are already in their caches and their play() starts at
	// once. Nothing waits on any of it — a prefetch that has not landed just
	// means play() fetches for itself, exactly as before.
	//
	// ONE RANGE FOR THE CUE AND THE PLAY, which is what makes this
	// predictable instead of merely fast on a good day. The cue asks for the
	// WHOLE event and stops on the first picture (PlayRequest::maxFrames), so
	// it leaves in the cache exactly the clip the play after it will ask for.
	// It used to ask for a two-frame range instead: a different range is a
	// different key, so the cue fetched, and then the play fetched the same
	// footage again — and whether that second read was a memcpy out of the
	// ring or an open-seek-demux off the disk is what made the wait feel
	// random.
	const int64_t cueOutNs =
		(ev.tOutNs != kNoInstant && ev.tOutNs > ev.tInNs)
			? ev.tOutNs
			: ev.tInNs + 2 * frameNs;
	ensureTileFeeds();
	prefetchTiles(ev.tInNs, cueOutNs, 100);

	for (Which w : targetChannels()) {
		auto &pcw = PlaybackCoordinator::instance(w);
		// A BAY THAT IS PLAYING IS LEFT ALONE. Selecting a row is
		// preparation for the next play, not a request to interrupt the
		// one on air — and interrupting it would mean stopEvents(), which
		// for a sequence started with "In output" puts the operator's
		// previous scene back in Program. A click in a list must never be
		// able to do that. (It is also how the reference controller behaves: you cue into the
		// bay that is not live, which is what two bays are for.)
		if (pcw.queueActive()) {
			obs_log(LOG_INFO,
				"[dock] cue %s: skipped, that bay is on air",
				channelLetter(w));
			continue;
		}
		ReplayChannel::PlayRequest req;
		req.camIndex = angle1_[(int)w] - 1;
		req.inNs = ev.tInNs;
		req.outNs = cueOutNs;
		req.speedPct = 100;
		// The still is a frame cap, not a short range — see cueOutNs. The
		// fetch this pays for is the one the play will want, so there is
		// no second prefetch here any more: it was a whole extra read of
		// the same footage, and the join it did on this thread was a UI
		// stall of up to a file read per bay.
		req.maxFrames = 2;
		std::string err;
		if (!ReplayChannel::instance(w).play(req, err))
			// Not a notice: a cue is a side effect of moving the
			// selection, and a row whose footage has gone is already
			// flagged with ⚠ in the table. Shouting on every arrow-key
			// press would be worse than silence.
			obs_log(LOG_INFO, "[dock] cue %s: event %d not playable: %s",
				channelLetter(w), ev.id, err.c_str());
	}

	// THE ANGLE BOXES SHOW THE CUE TOO — every lens, on the in-point.
	//
	// This is the gesture the multiview exists for: the operator picks a row
	// and looks along the strip to decide which camera saw it. Done here and
	// not from poll() because a cue never reaches the coordinator — it is two
	// frames played straight at the channel, so there is no queue for poll()
	// to read it off.
	//
	// AFTER the bay, on the clips the prefetch above has been reading while
	// the bay did its own: this play() is a cache hit, so the boxes light up
	// with it rather than a fetch apiece behind it. Same range, same frame
	// cap: one fetch per angle serves both this still and the replay that
	// follows it.
	cueTiles(ev.tInNs, cueOutNs, 100, ReplayChannel::Direction::Forward,
		 /*maxFrames*/ 2);
}

void MultiReplayDock::replayCurrent()
{
	replayCurrentOn(activeChannel_);
}

void MultiReplayDock::replayCurrentOn(Which which)
{
	// While following live the angle buttons only pick which camera the
	// preview mirrors; they must not start a replay. Once the operator plays
	// something (which clears follow-live) they re-cue it — including during
	// recording, which the ring makes possible and is the whole point.
	if (ReplayCore::instance().followLive())
		return;
	// A FREE REVIEW IS RE-CUED ON THE CHOSEN CAMERA, not abandoned for an
	// event. Changing the angle means "the same moment, on that lens" — which
	// is exactly as true of a stretch nobody marked as it is of a marked one,
	// and it is what the angle boxes have just been showing him. Jumping to
	// the selected event here would take the operator somewhere else on the
	// timeline for pressing a camera key.
	if (freeReviewInNs_ != kNoInstant) {
		// Once, on the active bay: a free review is a range, and playing
		// one range on both bays is two decoders showing one picture.
		if (which == activeChannel_) {
			playheadNs_ = freeReviewInNs_;
			playFreeReview(freeReviewOnAir_);
		}
		return;
	}
	auto &pc = PlaybackCoordinator::instance(which);
	std::string err;
	std::vector<int> ids = selectedEventIds();
	bool toOut = toOutputBtn_ && toOutputBtn_->isChecked();
	// THIS channel's angle, not the active one's: pressing B's camera 3
	// re-cues B on camera 3 even while the transport keys are on A.
	int a0 = angle1_[(int)which] - 1;
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

void MultiReplayDock::setSearchText(const QString &text)
{
	if (search_)
		search_->setText(text); // textChanged does the rest
}

void MultiReplayDock::zoomWholeTimeline()
{
	if (seek_)
		seek_->setZoom(1.0, 0.5);
}

void MultiReplayDock::showZoomMenu()
{
	QMenu menu(this);
	// "The whole thing" first, because it is the way back and the way back has
	// to be the shortest gesture. Then spans, longest to shortest.
	struct Entry {
		const char *label;
		int64_t spanNs; // 0 = the whole timeline
	};
	static const Entry kEntries[] = {
		{"100%", 0},
		{"1 h", 3600'000'000'000LL},
		{"30 min", 1800'000'000'000LL},
		{"10 min", 600'000'000'000LL},
		{"5 min", 300'000'000'000LL},
		{"1 min", 60'000'000'000LL},
	};

	const int64_t total = displayDurNs_;
	for (const Entry &e : kEntries) {
		QAction *a = menu.addAction(QString::fromUtf8(e.label));
		a->setCheckable(true);
		// A span longer than the session is the whole session; offering it
		// as a separate choice that does nothing would be a lie about the
		// control.
		const double want = (e.spanNs <= 0 || total <= 0 ||
				     e.spanNs >= total)
					    ? 1.0
					    : (double)total / (double)e.spanNs;
		const bool usable = e.spanNs <= 0 || total > e.spanNs;
		a->setEnabled(usable);
		// Only a usable entry can be the one in force. A span longer than
		// the session collapses to "the whole timeline", so on a two-minute
		// project "5 min" computed the same factor as "100%" and came up
		// greyed out AND ticked — an entry claiming to be the current view
		// while refusing to be clicked.
		a->setChecked(usable && std::abs(want - seek_->zoom()) < 0.01);
		const int64_t span = e.spanNs;
		connect(a, &QAction::triggered, this, [this, want, span]() {
			if (span <= 0) {
				zoomWholeTimeline();
				return;
			}
			// Centred on the playhead, not on the middle of the
			// timeline: the operator asked to see five minutes, and the
			// five minutes he means are the ones around where he is.
			double centre = 0.5;
			if (!timeline_.empty() && playheadNs_ != kNoInstant &&
			    displayDurNs_ > 0)
				centre = std::clamp(
					(double)timeline_.footageBefore(playheadNs_) /
						(double)displayDurNs_,
					0.0, 1.0);
			seek_->setZoom(want, centre);
			obs_log(LOG_INFO,
				"[dock] seek zoom: %s → %.2f× (centre %.3f)",
				span > 0 ? "span" : "whole timeline", want, centre);
		});
	}
	menu.exec(zoomBtn_->mapToGlobal(QPoint(0, zoomBtn_->height())));
}

void MultiReplayDock::showNotice(const QString &text)
{
	// Shown on the STATUS LINE, which owns it for a few seconds (see
	// updateChannelStrip). It runs the width of the panel, it is next to the
	// modes the sentence is usually about, and it is the one place left that
	// can hold a sentence at all now that the three-line channel band is gone.
	noticeText_ = text;
	noticeUntilNs_ = (int64_t)os_gettime_ns() + kNoticeNs;
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
	if (!events_)
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

	// ── ONE LINE, ON THE STATUS BAR ──────────────────────────────────────
	//
	// This was a three-line green band under the pictures — list / clip x of
	// y / remaining, then the event id with the two offsets, then timecode and
	// speed. Forty-four pixels of panel, and nearly all of it a second copy of
	// what the on-air band and the position bar were already saying, in a
	// second green that the eye had to tell apart from the first.
	//
	// What is left here is the one thing that is said NOWHERE else: the answer
	// to a key the operator just pressed. A notice OWNS the line while it
	// lasts, because a refusal nobody sees is a panel that ignored him.
	const bool reverseOnAir = ps.active && ps.reverse;
	QString line;
	const bool notice = noticeUntilNs_ > 0 &&
			    (int64_t)os_gettime_ns() < noticeUntilNs_;
	if (notice) {
		line = noticeText_;
	}
	// AND NOTHING ELSE. The line used to carry the list, the event id, the
	// clip counter and the playhead when there was no notice - and every one
	// of those is said somewhere the eye is already going: the list and the
	// id on the on-air band right under it, the counter there too, the
	// playhead on the position bar in the panel's own timecode. Four values
	// repeated one line apart are four glances that answer nothing, and they
	// were what made a notice hard to notice.
	//
	// So the line is the notice's, and empty the rest of the time. That is
	// not waste: it is the difference between a message and a caption.
	if (statusNotice_) {
		statusNotice_->setText(line);
		statusNotice_->setProperty("notice", notice);
		if (statusNoticeLit_ != notice) {
			statusNoticeLit_ = notice;
			repolish(statusNotice_);
		}
		statusNotice_->setToolTip(notice ? noticeText_ : QString());
	}
	if (statusSpeed_)
		statusSpeed_->setText(
			QString::asprintf("%.2f\xc3\x97", speedPct_ / 100.0));
	// clipPos and signedTc are still what the on-air band below is built from.
	(void)clipPos;
	(void)signedTc;

	// --- the green band: the state of the ANGLE that is on air ------------
	// id · angle · time left · speed, with the fill as the progress through
	// that clip. The speed is the CLIP's (the angle's override when it has
	// one), not the slider's: what has to be readable there is the speed of
	// the picture in front of the operator.
	if (clipBar_) {
		const bool onAir = ps.active && ps.eventId > 0;
		const int barPct = onAir ? ps.speedPct
					 : (speedPct_ > 0 ? speedPct_ : 100);
		// --- the whole SEQUENCE, when there is more than one clip in it ---
		// The fill used to be the progress through the clip on air, so a
		// three-angle replay filled the band up and started again three
		// times: nothing on the panel said when the replay would be over,
		// which is the one thing the band is for. Now the fill spans the
		// queue, the joins between the clips are drawn on it, and the text
		// carries the sequence's own remaining time beside the clip's.
		// --- the join, and why the band used to spike ---------------------
		// See clipBarJoinPos_ in the header: between two clips of a sequence
		// the queue has already moved on while positionNs() still reports the
		// last frame of the clip that ended, which reads as "this clip is
		// 100% done" — and since the sequence fill is built from it, the band
		// jumped to full at every white join before falling back.
		if (!onAir) {
			clipBarQueuePos_ = 0;
			clipBarAtJoin_ = false;
		} else {
			if (ps.queuePos != clipBarQueuePos_) {
				// Not the first clip of the sequence: that one has no
				// previous clip to be showing the tail of.
				if (clipBarQueuePos_ != 0) {
					clipBarJoinPos_ = clipPos;
					clipBarAtJoin_ = true;
				}
				clipBarQueuePos_ = ps.queuePos;
			}
			if (clipBarAtJoin_ && clipPos != clipBarJoinPos_)
				clipBarAtJoin_ = false;
		}
		const bool atJoin = clipBarAtJoin_;

		std::vector<double> joins;
		int64_t seqTotalNs = 0, seqDoneNs = 0;
		if (onAir && ps.queued > 1 &&
		    (int)ps.queuedWallNs.size() == ps.queued) {
			for (int64_t d : ps.queuedWallNs)
				seqTotalNs += d;
			for (int i = 0; i < ps.queuePos - 1 &&
					i < (int)ps.queuedWallNs.size();
			     i++)
				seqDoneNs += ps.queuedWallNs[i];
			if (seqTotalNs > 0) {
				int64_t acc = 0;
				for (size_t i = 0; i + 1 < ps.queuedWallNs.size();
				     i++) {
					acc += ps.queuedWallNs[i];
					joins.push_back((double)acc /
							(double)seqTotalNs);
				}
			}
		}
		double frac = 0.0;
		QString text;
		if (haveEv && ev.tOutNs != kNoInstant &&
		    ev.tOutNs > ev.tInNs) {
			const int64_t dur = ev.tOutNs - ev.tInNs;
			// Backwards the fill DRAINS, which needs no special case:
			// the fill is the playhead's place inside the clip, and in
			// reverse that place walks back towards the IN. An emptying
			// band is what "this replay is running out" looks like from
			// across a gallery, in either direction.
			frac = atJoin ? 0.0
				      : (double)(clipPos - ev.tInNs) / (double)dur;
			// At a join the whole clip is still to come, whichever
			// direction it runs: the position we can see belongs to the
			// clip that just ended.
			const int64_t remNs =
				atJoin ? dur
				       : (reverseOnAir
						  ? (clipPos > ev.tInNs
							     ? clipPos - ev.tInNs
							     : 0)
						  : (ev.tOutNs > clipPos
							     ? ev.tOutNs - clipPos
							     : 0));
			// Remaining in WALL time: at 50% a 4 s clip has 8 s left,
			// and 8 s is how long the operator will be looking at it.
			// The ◀ is not decoration: backwards at 50% and forwards at
			// 50% are the same three numbers and not the same picture.
			// THE LIST, THEN THE EVENT. The band used to open with
			// "A<angle>", which on a rig worked from one camera is the
			// same two characters all match - and the angle is already
			// on the pictures, in colour, as the tally. Which LIST the
			// clip came from was said on the line above instead, where
			// it was one of four things competing for a glance.
			text = QString("%1   %2   %3   %4%5%")
				       .arg(listText)
				       .arg(evId, idDigits, 10, QLatin1Char('0'))
				       .arg(shortTc(remNs * 100 / (barPct > 0
									   ? barPct
									   : 100)))
				       .arg(reverseOnAir ? QStringLiteral("◀ ")
							 : QString())
				       .arg(barPct);
			if (onAir && ps.queued > 1) {
				text += QString("   %1/%2")
						.arg(ps.queuePos)
						.arg(ps.queued);
				// The whole sequence: how far through it the fill
				// is, and how much of it is left. Without the total
				// the operator can see the end of THIS angle coming
				// and has no idea whether two more follow it.
				if (seqTotalNs > 0) {
					const int64_t clipWall =
						remNs * 100 /
						(barPct > 0 ? barPct : 100);
					const int64_t doneInClip =
						ps.queuedWallNs[ps.queuePos - 1] >
								clipWall
							? ps.queuedWallNs[ps.queuePos -
									  1] -
								  clipWall
							: 0;
					const int64_t elapsed = seqDoneNs + doneInClip;
					frac = std::clamp((double)elapsed /
								  (double)seqTotalNs,
							  0.0, 1.0);
					text += QString("   Σ %1").arg(shortTc(
						seqTotalNs > elapsed
							? seqTotalNs - elapsed
							: 0));
				}
			}
		} else {
			text = obs_module_text("Dock.NoEvent");
		}
		clipBar_->setState(frac, text, onAir, joins);
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
	// FOOTAGE from here, not wall time from here — and cut at the seams.
	//
	// Stop after five minutes and start again: the bar draws the two takes
	// joined, but in master time they are minutes apart. Asking for one range
	// [inNs, inNs + 10 s] therefore asks for the gap as well, the gap is not
	// footage, and the engine refuses a range it cannot serve exactly — so the
	// review stopped dead where one file ended and the next began. Split into
	// per-span pieces (TimelineMap::spansFrom) it is two ranges the engine can
	// serve, and the queue chains them the way it chains the angles of an event.
	std::vector<std::pair<int64_t, int64_t>> ranges;
	if (timeline_.empty()) {
		const int64_t edge = timelineStartNs_ + displayDurNs_;
		const int64_t outNs = std::min(edge, inNs + kScrubReviewNs);
		if (outNs > inNs)
			ranges.push_back({inNs, outNs});
	} else {
		for (const TimelineSpan &s :
		     timeline_.spansFrom(inNs, kScrubReviewNs))
			ranges.push_back({s.startNs, s.endNs});
	}
	if (ranges.empty())
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

	// DID HE JUST LAND ON FOOTAGE NOBODY MARKED? Then that stretch is what
	// the play keys are about from here on: ▶ watches it off air, and Play
	// events puts it up. Landing inside the selected event instead changes
	// nothing — Play events still replays the event, which is what a scrub
	// through a marked action has always meant.
	if (playheadIsFreeFootage())
		freeReviewInNs_ = inNs;
	else
		clearFreeReview();

	std::string err;
	// Through the QUEUE, not straight at the channel: the queue is what plays
	// one range after another, so a review that crosses a seam carries on into
	// the next take instead of ending there. (stopEvents() above already killed
	// whatever was queued, so this replaces it rather than fighting it.)
	if (!pc().playRanges(ranges, currentAngle1() - 1, speedPct_,
			     /*toOutput*/ false, err)) {
		// Nothing covers that instant on this angle — the ring has evicted
		// it and no anchored file holds it. Saying so is the point: the
		// alternative was the preview quietly showing the live camera, which
		// reads as "this is what was recorded there" and is not.
		const int64_t relMs = (inNs - timelineStartNs_) / 1000000;
		showNotice(QString("%1 (cam %2 @ %3) — %4")
				   .arg(obs_module_text("Dock.NoFootageHere"))
				   .arg(currentAngle1())
				   .arg(formatTc(relMs * 1000000))
				   .arg(QString::fromStdString(err)));
		obs_log(LOG_WARNING,
			"[dock] no footage on angle %d at %lld ms into the "
			"timeline: %s",
			currentAngle1(), (long long)relMs, err.c_str());
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


// Is a self-test driving this OBS? Both first-run dialogs are MODAL, and a
// modal dialog on the gate's OBS is not a slow run, it is a dead one: exec()
// parks the UI thread and every runOnUi() the checking thread posts after that
// waits forever. The take goes on recording, no report is ever written, and the
// script times out — which is exactly how a check that called playSelected()
// once cost minutes of a run (see the notes in selftest.cpp).
//
// Read from the environment rather than asked of the self-test module, because
// this must be true before anything in that module has run — poll() reaches
// here on the fifth second, and by then the harness has long since set it.
static bool selfTestIsDriving()
{
	const char *v = getenv("OBS_MULTIREPLAY_SELFTEST");
	return v && *v && std::strcmp(v, "0") != 0;
}
void MultiReplayDock::poll()
{
	// Taken FIRST and handed to accountUiTick last, so what is measured is the
	// whole tick and the gap since the previous one. See accountUiTick.
	const uint64_t tickStartNs = (uint64_t)os_gettime_ns();
	auto &core = ReplayCore::instance();
	auto &chan = this->chan();
	auto &tap = PacketTap::instance();

	// The preview's obs_display is bound to a native window handle, and OBS
	// re-parents its docks whenever the layout is restored, floated, tabbed or
	// re-docked — which destroys that handle. Qt does not reliably tell the
	// widget, so this is the only place that can notice a display left
	// presenting into a window that no longer exists. Two integer compares.
	// Every one, the multiview tiles included: they are re-parented by the
	// same dock moves. Two integer compares each, and a hidden tile
	// early-outs on isVisible().
	for (OBSQTDisplay *d : allDisplays())
		d->recheckWindow();


	// --- is Branch Output even installed? --------------------------------
	// Once per launch, and not on the first tick: OBS emits FINISHED_LOADING
	// from inside OBSBasic::OBSInit, which then spins a NESTED event loop of
	// its own (the YouTube dock) — and a modal dialog dropped into that is the
	// same hazard that crashes Branch Output's status dock. Four seconds of
	// polling is well past it, and past any module still registering.
	if (!branchOutputAsked_ && statusTick_ > 120 && !selfTestIsDriving()) {
		branchOutputAsked_ = true;
		if (!core.branchOutputAvailable())
			QTimer::singleShot(0, this, [this]() {
				promptForBranchOutput();
			});
	}
	// ...and, once that is settled, whether anything is configured at all.
	// AFTER Branch Output, deliberately: two modal dialogs racing each other
	// on a fresh machine is worse than either, and the recording layer is the
	// one that decides whether the rest is worth filling in.
	if (!setupAsked_ && branchOutputAsked_ && !modalOpen_ &&
	    statusTick_ > 150 && !selfTestIsDriving()) {
		setupAsked_ = true;
		if (needsSetup())
			QTimer::singleShot(0, this,
					   [this]() { runSetupWizard(); });
	}
	// The hotkeys change the angle without going through the dock.
	const int hotAngle1 = core.currentAngle() + 1;
	if (hotAngle1 >= 1 && hotAngle1 <= kNCams)
		angle1_[(int)activeChannel_] = hotAngle1;
	const int cam0 = currentAngle1() - 1;

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
			// Prime suspect for a poll() of most of a second: the
			// watcher holds this lock WHILE IT DEMUXES a file.
			Phase _ph(this, "diskSpans");
			diskSpans_.clear();
			for (const auto &[s, e] :
			     SegmentIndex::instance().recordedSpans())
				diskSpans_.push_back({s, e});
			// WHERE THE TAKE IN PROGRESS BEGAN. This is what made the
			// total wrong, and it got worse the longer the take ran.
			//
			// recordedSpans() cannot report the file being written: its
			// length is not measurable until the muxer has finished with
			// it, and inventing one is not that function's business. So
			// the only thing left describing the current take was the
			// RING — the last ~20 s. Everything between the take's start
			// and the ring's oldest instant was missing from the total,
			// so after five minutes of recording the bar declared about
			// twenty seconds, the graduations were drawn for twenty
			// seconds, and every position on it meant something other
			// than what it said.
			//
			// That footage is not missing: the ring serves the recent
			// part and the file on disk serves the older part (that is
			// what Source::Auto does). So the span of the take is
			// [its anchor, the live edge] — the anchor being the newest
			// one on this camera, i.e. the file currently being written.
			// Read here, on the slow beat, because it takes the segment
			// index's lock and does not move for the whole take.
			takeAnchorNs_ = kNoInstant;
			for (const auto &s : SegmentIndex::instance().segments(cam0))
				if (s.anchored && (takeAnchorNs_ == kNoInstant ||
						   s.anchorMasterNs > takeAnchorNs_))
					takeAnchorNs_ = s.anchorMasterNs;
		}
		// The take in progress: from where it started to the live edge. The
		// live edge itself IS cheap to read (the ring's own lock, held for
		// a compare), so it stays on the fast beat and the bar grows
		// smoothly.
		std::vector<TimelineSpan> spans = diskSpans_;
		const int64_t ringOldest = tap.oldestReplayableNs(cam0);
		int64_t liveStart = ringOldest;
		// Only reach back to the anchor if it really is behind the ring and
		// really is this take's: an anchor AFTER the live edge belongs to
		// nothing we can play, and one from a take that has already been
		// measured is in diskSpans_ already (where it does no harm — the
		// map merges touching spans).
		if (takeAnchorNs_ != kNoInstant && liveEdgeNs > 0 &&
		    takeAnchorNs_ < liveEdgeNs &&
		    (liveStart <= 0 || takeAnchorNs_ < liveStart))
			liveStart = takeAnchorNs_;
		if (liveEdgeNs > 0 && liveStart != kNoInstant &&
		    liveEdgeNs > liveStart)
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
		Phase _ph(this, "refreshEvents/origin");
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
		if (freeReviewInNs_ != kNoInstant) {
			// A FREE REVIEW DOES NOT HAND THE PANEL BACK TO LIVE.
			//
			// The operator stopped it on purpose — that is how it
			// ends — and what he does next is decide whether to air
			// it. Snapping to the live edge here would throw away the
			// stretch he just chose, and with it the second function
			// of the Play key, which replays exactly that stretch.
			// The armed in-point is left alone: it is the beginning of
			// what he watched, not wherever he pressed Stop.
		} else if (liveFrontFed) {
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
	// hasPosition(), not `posNs > 0`: an instant of 0 is a legitimate one and
	// footage recorded before the machine's last boot maps onto NEGATIVE
	// instants (session-clock.hpp, kNoInstant) — the ordinary state of a
	// reopened project. Asking "is it positive?" pinned the bar to the live
	// edge for exactly the footage a step back is for.
	const int64_t posNs = chan.positionNs();
	if (playing && chan.hasPosition())
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
		}
	}

	// ONE MARK, and it says what the key will DO. The key carried a drawn
	// mark AND a glyph in its text - a triangle, two bars and a third
	// character on one 30 px key, the last of them turned cyan by the
	// "playing" rule because a style sheet colours text and cannot reach a
	// pixmap. Three marks on the key that gets pressed most.
	if (playPauseIcon_ != (playing ? 1 : 0)) {
		playPauseIcon_ = playing ? 1 : 0;
		setKeyIcon(playPauseBtn_, playing ? Icon::Pause : Icon::Play,
			   tintsFor(sc()));
	}
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


	// The row on air gets a PGM cue on its id cell. the reference controller colours the whole
	// row, but our row colour is the selection (orange), and repainting a
	// selected row would make "playing" and "selected" indistinguishable —
	// which is the one thing that must never be ambiguous during a match.
	{
		const auto &ps = playSt;
		const bool wasProgrammatic = itemsProgrammatic_;
		itemsProgrammatic_ = true; // colouring is not an operator edit
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
		itemsProgrammatic_ = wasProgrammatic;
	}
	// the reference controller paints the header of the camera being watched green.
	updateCamHeaderHighlight();

	// --- recording status ---
	// Auto-follow the live edge when recording starts so the preview tracks
	// the new take instead of sitting on the last clip that played.
	if (rec && !prevRecording_) {
		core.setFollowLive(true);
		// A new take: whatever stretch of the last one was armed on the
		// bar is not what the play keys are about any more.
		clearFreeReview();
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
			// Still account for the tick: this branch opens a modal,
			// which is precisely a tick that took a very long time,
			// and losing it would hide the one sample worth having.
			accountUiTick(tickStartNs, (uint64_t)os_gettime_ns());
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
			// Fourth suspect: a name lookup takes libobs' global source
			// mutex, and the coordinator holds it across a scene switch —
			// which is exactly what was happening in the window where a
			// tick was measured at 964 ms.
			Phase _ph(this, "previewSource");
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
			// hasPosition(), not positionNs() > 0: instants can be zero
			// or negative (kNoInstant), and "has this bay ever shown a
			// frame" is the actual question.
			obs_source_t *nextB =
				channelBEnabled_ &&
						(PlaybackCoordinator::instance(
							 Which::B)
							 .playState()
							 .active ||
						 ReplayChannel::instance(Which::B)
							 .hasPosition())
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
		// --- the angle boxes follow the REVIEW -----------------------
		// The tiles have to show the moment on air on every other lens,
		// and the only thing that knows which moment that is — through a
		// multi-angle sequence, a review split across a junction, and the
		// chunks of a free run — is the QUEUE. Driving them from here, off
		// playState(), is one hook instead of a cueTiles() call sprinkled
		// over every play path, and the one that would be forgotten is
		// always the path that has no event behind it.
		//
		// cueTiles() is a no-op unless the range really moved, which on an
		// ordinary tick it has not.
		if (!followLive && playSt.active &&
		    playSt.clipInNs != kNoInstant) {
			Phase _ph(this, "cueTiles");
			cueTiles(playSt.clipInNs, playSt.clipOutNs,
				 playSt.speedPct,
				 playSt.reverse
					 ? ReplayChannel::Direction::Reverse
					 : ReplayChannel::Direction::Forward);
		}
		// Back on the live edge: the feeds are decoders and private
		// sources, and keeping eight of them alive to hold a still nobody
		// is looking at is the one way this feature could cost something
		// when it is not in use.
		if (followLive && tileCueInNs_ != kNoInstant)
			releaseTileFeeds();

		// The tiles are resolved on the same 4 Hz beat and by the same
		// rules: a lookup on the UI thread, an owned ref published for the
		// graphics thread. Re-running it periodically is also what catches
		// a camera source renamed, deleted or swapped by a scene-collection
		// change — the tile goes black instead of holding a stale pointer.
		//
		// ...and IMMEDIATELY when live/review flips, not on the next 4 Hz
		// beat: a quarter of a second of eight boxes showing the wrong
		// kind of picture is exactly the sort of thing an operator sees
		// and cannot name.
		//
		// ...and immediately when a feed shows its FIRST picture, which is
		// the other half of the same problem. The flip above fires
		// microseconds after the feeds were started — before any of them
		// has decoded anything — so on its own it published nothing and
		// left the boxes waiting for the slow beat anyway. This is the
		// tick the picture actually exists on.
		const bool tilePictureArrived = pollTileFeedPictures();
		if (refreshStatus || tilesLive_ != followLive || tilePictureArrived)
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

	// A MARK AND A WORD, not a mark and a word with a second mark inside it.
	// The text used to carry its own bullet, so the key showed the drawn
	// record dot and then another one printed beside it.
	recBtn_->setText(rec ? QStringLiteral("STOP") : QStringLiteral("REC"));
	if (recIcon_ != (rec ? 1 : 0)) {
		recIcon_ = rec ? 1 : 0;
		setKeyIcon(recBtn_, rec ? Icon::Stop : Icon::Rec, tintsFor(sc()),
			   13);
	}
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
	// On the same slow beat: whether there are two bays at all. It changes
	// when Settings is saved — a few times an evening, not thirty times a
	// second — and it early-outs otherwise, which keeps the getConfig() lock
	// off the fast path.
	if (refreshStatus) {
		applyChannelBVisibility();
		// ...and whether the panel is floating, which is the only state
		// in which there is a screen for it to take. Same beat, same
		// reason: pulling a dock out of OBS is a deliberate gesture, and
		// this early-outs unless the answer changed.
		refreshFullScreenKey();
	}

	// The other prime suspect: statusJson() calls std::filesystem::space() on
	// the session folder, and on a network share that is a round trip.
	std::unique_ptr<Phase> _statusPh;
	if (refreshStatus && nowNs >= noticeUntilNs_)
		_statusPh = std::make_unique<Phase>(this, "statusJson");
	Data st((refreshStatus && nowNs >= noticeUntilNs_) ? core.statusJson()
							  : std::string());
	_statusPh.reset();
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
	{
		Phase _ph(this, "channelStrip");
		updateChannelStrip();
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
		// TWO phases, not one. They were timed together and the total was
		// read as the table's — but the table's own breakdown never showed
		// a rebuild anywhere near the 567 ms this phase reported, which
		// means the time was in the other half or in neither. A phase that
		// covers two things names neither.
		{
			Phase _ph(this, "refreshAngles");
			refreshAngles();
		}
		Phase _ph(this, "refreshEvents/version");
		refreshEvents(); // rebuilds markerNs_ (raw ns pairs)
	} else if (eventsDirty_ || commentsDirty_) {
		// A rebuild the store did not ask for: one that was deferred past an
		// open editor, or a comment that has to reach the other rows. Both
		// resolve themselves within a tick or two of the operator finishing.
		commentsDirty_ = false;
		refreshEvents();
	}

	// Recompute seekbar marker fractions every tick: the window slides (the
	// live edge grows, the ring drops its oldest), so markers move even when
	// the events themselves do not change.
	//
	// Built into MEMBER scratch buffers, not into two fresh vectors: this ran
	// thirty times a second and allocated twice on every one of them, then
	// handed the result to a setter that took it BY VALUE and repainted
	// unconditionally — so a panel with nothing happening in it was asking Qt
	// to redraw the full-width bar 30 times a second, forever. The setters
	// compare now, which is only possible because the buffers survive the
	// call.
	if (seek_ && displayDurNs_ > 0) {
		markerFracScratch_.clear();
		markerIdScratch_.clear();
		markerFracScratch_.reserve(markerNs_.size());
		markerIdScratch_.reserve(markerNs_.size());
		for (size_t i = 0; i < markerNs_.size(); i++) {
			// Through the same map the bar is drawn with, so a mark
			// sits over its own footage however many takes there
			// have been (and an event in a gap that no longer exists
			// collapses onto the join rather than smearing over it).
			const auto &[inNs, outNs] = markerNs_[i];
			double inf = 0.0, outf = 0.0;
			if (timeline_.rangeFraction(inNs, outNs, inf, outf)) {
				markerFracScratch_.push_back({inf, outf});
				// The two lists are read in lockstep by the
				// bar, so a marker that is not drawn must not
				// leave its id behind either.
				markerIdScratch_.push_back(
					i < markerIds_.size() ? markerIds_[i]
							      : 0);
			}
		}
		seek_->setEventMarkers(markerFracScratch_);
		seek_->setEventMarkerIds(markerIdScratch_);
	} else if (seek_) {
		markerFracScratch_.clear();
		markerIdScratch_.clear();
		seek_->setEventMarkers(markerFracScratch_);
		seek_->setEventMarkerIds(markerIdScratch_);
	}

	// Last thing in the tick, so it measures the whole of it.
	accountUiTick(tickStartNs, (uint64_t)os_gettime_ns());
}

// How the panel is being SERVICED, written to the OBS log.
//
// The black-window failure this exists for is not a failure of the plugin's own
// logic: everything keeps running, the previews keep drawing, the recording
// keeps recording. What stops is Qt being able to put pixels on the OBS window.
// Two numbers say whether that was happening, and neither of them can be
// recovered after the fact from anything else in the log:
//
//   late   how long after its due time the 33 ms tick actually fired. The poll
//          timer is delivered by the same event loop as paint events, so a tick
//          that arrives 900 ms late IS a window that went 900 ms without a
//          repaint. This is the direct measurement of the symptom.
//   cost   how long poll() itself took. That is the part of `late` we own; if
//          cost is small and late is large, the UI thread was being held by
//          something outside this plugin.
//
// ...plus the repaint census, as a RATE. A run where the panel is idle and the
// bars are still asking for tens of repaints a second means the guards have a
// hole in them and this document's diagnosis was incomplete.
// The worst single phase of the window, kept by cost. Not a sum per phase: what
// has to come off this thread is the thing that blocked ONCE for most of a
// second, and an average over ten seconds of 33 ms ticks buries it completely.
void MultiReplayDock::notePhase(const char *name, int64_t ns)
{
	if (ns > uiWorstPhase_.ns) {
		uiWorstPhase_.ns = ns;
		uiWorstPhase_.name = name;
	}
}

void MultiReplayDock::accountUiTick(uint64_t tickStartNs, uint64_t tickEndNs)
{
	constexpr int64_t kExpectedTickNs = 33'000'000;
	// A tick this late is not jitter, it is a stall: at 30 Hz it means eight
	// frames the panel could not have been redrawn in. Logged as it happens
	// (throttled), because the aggregate below would average it away.
	constexpr int64_t kStallNs = 250'000'000;
	constexpr int64_t kStallLogEveryNs = 5'000'000'000;
	constexpr int64_t kReportEveryNs = 10'000'000'000;

	if (lastTickNs_ == 0) {
		lastTickNs_ = tickStartNs;
		uiWindowStartNs_ = tickStartNs;
		uiSeekReqAtWindow_ = g_seekCensus.requested;
		uiSeekSupAtWindow_ = g_seekCensus.suppressed;
		uiSeekServedAtWindow_ = g_seekCensus.served;
		uiClipReqAtWindow_ = g_clipCensus.requested;
		uiClipServedAtWindow_ = g_clipCensus.served;
		return;
	}

	const int64_t late =
		(int64_t)(tickStartNs - lastTickNs_) - kExpectedTickNs;
	lastTickNs_ = tickStartNs;
	const int64_t cost = (int64_t)(tickEndNs - tickStartNs);

	uiTicks_++;
	if (late > 0) {
		uiLateSumNs_ += late;
		uiLateMaxNs_ = std::max(uiLateMaxNs_, late);
	}
	uiCostSumNs_ += cost;
	uiCostMaxNs_ = std::max(uiCostMaxNs_, cost);

	if (late >= kStallNs &&
	    (uiLastStallLogNs_ == 0 ||
	     (int64_t)(tickStartNs - uiLastStallLogNs_) >= kStallLogEveryNs)) {
		uiLastStallLogNs_ = tickStartNs;
		obs_log(LOG_WARNING,
			"[ui] UI thread stalled %lld ms — the panel could not be "
			"repainted for that long (poll itself took %lld ms)",
			(long long)(late / 1'000'000),
			(long long)(cost / 1'000'000));
	}

	if ((int64_t)(tickStartNs - uiWindowStartNs_) < kReportEveryNs)
		return;

	const double secs = (double)(tickStartNs - uiWindowStartNs_) / 1e9;
	const uint64_t seekReq = g_seekCensus.requested - uiSeekReqAtWindow_;
	const uint64_t seekSup = g_seekCensus.suppressed - uiSeekSupAtWindow_;
	const uint64_t seekSrv = g_seekCensus.served - uiSeekServedAtWindow_;
	const uint64_t clipReq = g_clipCensus.requested - uiClipReqAtWindow_;
	const uint64_t clipSrv = g_clipCensus.served - uiClipServedAtWindow_;

	obs_log(LOG_INFO,
		"[ui] %.1f tick/s | late avg %lld ms max %lld ms | poll avg %.1f ms "
		"max %lld ms (worst phase: %s %lld ms) | repaints/s seek %.1f "
		"(served %.1f, suppressed %.1f) clip %.1f (served %.1f)",
		(double)uiTicks_ / secs,
		(long long)(uiTicks_ ? uiLateSumNs_ / uiTicks_ / 1'000'000 : 0),
		(long long)(uiLateMaxNs_ / 1'000'000),
		uiTicks_ ? (double)uiCostSumNs_ / (double)uiTicks_ / 1e6 : 0.0,
		(long long)(uiCostMaxNs_ / 1'000'000),
		uiWorstPhase_.name[0] ? uiWorstPhase_.name : "-",
		(long long)(uiWorstPhase_.ns / 1'000'000), (double)seekReq / secs,
		(double)seekSrv / secs, (double)seekSup / secs,
		(double)clipReq / secs, (double)clipSrv / secs);

	uiWindowStartNs_ = tickStartNs;
	uiTicks_ = 0;
	uiLateSumNs_ = 0;
	uiLateMaxNs_ = 0;
	uiCostSumNs_ = 0;
	uiCostMaxNs_ = 0;
	uiWorstPhase_ = PhaseCost{};
	uiSeekReqAtWindow_ = g_seekCensus.requested;
	uiSeekSupAtWindow_ = g_seekCensus.suppressed;
	uiSeekServedAtWindow_ = g_seekCensus.served;
	uiClipReqAtWindow_ = g_clipCensus.requested;
	uiClipServedAtWindow_ = g_clipCensus.served;
}

void MultiReplayDock::refreshListNames()
{
	if (!listTabs_)
		return;
	auto &store = EventStore::instance();

	// NOTHING TO DO IS THE ORDINARY CASE, and this is where 143 ms went.
	//
	// refreshEvents calls this on every bump of the store's version — every
	// mark, every trim, every ticked checkbox — and it then wrote text,
	// visibility and a tooltip onto all twenty tabs. Each of those makes
	// QTabBar re-lay-out the whole row and recompute its size hints, so twenty
	// writes are twenty relayouts of a widget nothing has changed. Measured
	// from the dock's own phase accounting: a refresh that reused every angle
	// cell and rebuilt none still took 143 ms, all of it here and in
	// rebuildEventColumns, before any row work began.
	//
	// List names change when the operator renames one. The count changes when
	// he changes it in Settings. Both are things he does between matches.
	const int shown =
		std::clamp(ReplayCore::instance().getConfig().eventListCount, 1,
			   kEventLists);
	const int tabs = std::min(kEventLists, listTabs_->count());
	bool same = shown == lastShownTabs_ &&
		    (int)lastListNames_.size() == tabs;
	if (same) {
		for (int i = 1; i <= tabs; i++) {
			if (lastListNames_[(size_t)(i - 1)] != store.listName(i)) {
				same = false;
				break;
			}
		}
	}
	if (same)
		return;

	lastShownTabs_ = shown;
	lastListNames_.resize((size_t)tabs);
	for (int i = 1; i <= tabs; i++)
		lastListNames_[(size_t)(i - 1)] = store.listName(i);
	// Tab text only: changing it does not move the current tab, but blocking
	// the signals keeps a rebuild from ever looking like an operator switching
	// list.
	QSignalBlocker block(listTabs_);

	// How many lists the operator asked to see (computed above, with the
	// early-out). The tabs beyond it are HIDDEN, not removed: what is in those
	// lists is still there, still saved, and comes back the moment he raises
	// the number.
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
		// "1 PARTITA" — the NUMBER STAYS. A named list used to replace the
		// number with the name, and that threw away the one label that is
		// stable: the hotkeys, the log lines and the operator's own "go to
		// three" all mean the number, so a tab that only says PARTITA is a
		// tab he has to count along the row to identify. The name is what it
		// is FOR; the number is how it is addressed.
		listTabs_->setTabText(i - 1,
				      nm.empty() ? QString::number(i)
						 : QString("%1 %2").arg(i).arg(
							   QString::fromStdString(
								   nm)));
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
	// The cameras are chosen by clicking their PICTURES now, so the only thing
	// left that follows the camera configuration is the multiview itself. This
	// stayed a function of its own because every path that can change that
	// configuration already calls it, and it is a no-op unless the set of
	// cameras (or one of their names) really moved.
	rebuildMultiview();
}

void MultiReplayDock::refreshEvents()
{
	// NEVER rebuild the table out from under an open editor.
	//
	// Every cell of a camera column is a real widget (a checkbox and two
	// combos), and rebuilding the table deletes them. poll() calls this
	// whenever the store's version moves — a mark, a trim, another angle
	// ticked — so a comment list dropped down during a match was being
	// destroyed a few tens of milliseconds later, with the popup vanishing
	// before the operator could pick anything out of it. That is the whole of
	// the "the dropdown closes too fast" bug: the list was never closing, its
	// combo was being deleted.
	//
	// So: an open popup, or a half-typed comment, defers the rebuild. poll()
	// comes back every 33 ms and picks it up the moment the editor is done.
	if (QApplication::activePopupWidget()) {
		eventsDirty_ = true;
		return;
	}
	if (events_) {
		QWidget *fw = QApplication::focusWidget();
		if (fw && qobject_cast<QLineEdit *>(fw) &&
		    events_->isAncestorOf(fw)) {
			eventsDirty_ = true;
			return;
		}
	}
	eventsDirty_ = false;

	// Here rather than only where a name is edited: opening another project
	// loads that project's list names without ever bumping the version
	// counter, and this is the one function every one of those paths calls.
	// From the TOP, not from where the rows are built. Timed only around the
	// row building, this function reported 62 ms while poll()'s phase around
	// the whole of it reported 307 — so a quarter of a second was going
	// somewhere upstream of the timer, and a breakdown that cannot see it
	// points at the wrong half. listJson() below serialises every event of the
	// list, with all its angles, under the store's lock, and it is called on
	// every refresh.
	const uint64_t refreshStartNs = os_gettime_ns();
	refreshListNames();
	// Same reason: the camera columns follow the camera configuration, and a
	// no-op unless that really changed (it clears the table).
	rebuildEventColumns();
	int list = EventStore::instance().selectedList();
	const uint64_t jsonStartNs = os_gettime_ns();
	Data d(EventStore::instance().listJson(list));
	const int64_t jsonNs = (int64_t)(os_gettime_ns() - jsonStartNs);
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
	// Block selection signals during the rebuild: the row count changes below
	// and selectRow() re-sets the selection, and nothing downstream needs to
	// hear about a selection that is only being restored.
	QSignalBlocker selBlock(events_->selectionModel());
	// NOTE: the table is NOT emptied here any more. It used to be
	// setRowCount(0), which destroys every item and every angle-cell widget,
	// and the rows were then created from nothing — on every bump of the
	// store's version, so on every mark, every trim, every tick of a checkbox.
	// Measured: one row of two angle cells, 50-180 ms, almost all of it Qt
	// resolving this dock's 376 lines of style sheet for the five widgets a
	// cell is made of. Rows are reused in place below; see reuseCount.
	obs_data_array_t *arr = obs_data_get_array(d, "events");
	if (!arr) {
		refreshing_ = false;
		return;
	}
	const Qt::Alignment mid = Qt::AlignVCenter | Qt::AlignHCenter;
	// ONCE for the whole table, not once per cell. getConfig() copies the
	// entire Config under the core mutex; doing it per event per camera is
	// what made this function the longest thing in poll(). See buildAngleCell.
	const std::vector<std::string> commentPresets =
		ReplayCore::instance().getConfig().commentPresets;
	// The operator's vocabulary can also change in Settings, which does not go
	// through rememberComment. Same consequence for a reused cell, so it counts
	// as a move of the same version.
	if (commentPresets != lastCommentPresets_) {
		lastCommentPresets_ = commentPresets;
		commentVocabVersion_++;
	}
	// Where a slow rebuild goes. This function is the longest thing the dock's
	// poll does, and knowing THAT is not enough to fix it: hoisting a
	// whole-Config copy out of the per-cell path — the obvious culprit from
	// reading the code — moved the number not at all. So it is split the same
	// way poll() was, and for the same reason.
	const uint64_t rebuildStartNs = os_gettime_ns();
	int64_t cellBuildNs = 0, cellInsertNs = 0;
	int cellCount = 0, cellReused = 0;
	// The tallest angle cell built this pass; the rows are sized from it below.
	int tallestCell = 0;
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
		std::string note;
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
		const char *nt = obs_data_get_string(e, "note");
		const std::string note = nt ? nt : "";
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

		// search filter (id / the event comment / angles)
		if (!needle.isEmpty()) {
			QString hay = QString::number(id) + " " + anglesStr;
			if (!note.empty())
				hay += " " + QString::fromStdString(note).toLower();
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
		}
		r.note = note;
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

	// Set the text of a read-only cell, REUSING the item that is already
	// there. Creating a QTableWidgetItem is cheap; the row it hangs off is
	// not, and neither is the widget in the camera cell beside it.
	auto setRoCell = [&](int row, int col, const QString &txt,
			     Qt::Alignment al) -> QTableWidgetItem * {
		QTableWidgetItem *it = events_->item(row, col);
		if (!it) {
			it = new QTableWidgetItem(txt);
			it->setTextAlignment(al);
			it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
			events_->setItem(row, col, it);
			return it;
		}
		if (it->text() != txt)
			it->setText(txt);
		return it;
	};

	// Grow or shrink to fit; the rows that survive keep their items and their
	// angle cells. Shrinking destroys the surplus, which is what should happen
	// to a row that no longer exists.
	events_->setRowCount((int)rows.size());

	for (size_t ri = 0; ri < rows.size(); ri++) {
		const Row &r = rows[ri];
		const int row = (int)ri;

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
		QTableWidgetItem *idItem = setRoCell(
			row, kColId,
			playable ? idText : QStringLiteral("⚠ ") + idText,
			Qt::AlignCenter);
		idItem->setData(Qt::UserRole, r.id);
		// Reused rows carry the previous occupant's tooltip, so the
		// "no footage" mark has to be cleared as well as set.
		idItem->setToolTip(playable
					   ? QString()
					   : obs_module_text(
						     "Dock.EventNoFootage"));
		setRoCell(row, kColIn, relTc(r.tin), mid);
		setRoCell(row, kColOut,
			  closed ? relTc(r.tout) : QStringLiteral("--"), mid);
		setRoCell(row, kColDur, dur, mid);

		// THE COMMENT, once, in a column of its own. A WIDGET and not an
		// item: a double click on a row of this table already means "put
		// this event on air", so an editor that opens on one would reach
		// the Program from a cell whose whole job is a word.
		{
			QWidget *nc = events_->cellWidget(row, kColNote);
			if (!updateNoteCell(nc, r.id, r.note)) {
				nc = buildNoteCell(r.id, r.note, commentPresets);
				events_->setCellWidget(row, kColNote, nc);
				const int want = nc->sizeHint().height() + 2;
				if (want > tallestCell)
					tallestCell = want;
			}
		}

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

			// REUSE FIRST. The widgets in this cell are connected to
			// an event and an angle; while those two are unchanged
			// the connections stay right and only the three values
			// have to be written. That is the ordinary case by a long
			// way — a mark appended, a point trimmed, an angle
			// ticked — and it is the difference between touching
			// three widgets and building five against a 376-line
			// style sheet.
			QWidget *cell = events_->cellWidget(row, col);
			if (updateAngleCell(cell, r.id, cam, r.camOn[cam],
					    r.camSpeeds[cam])) {
				cellReused++;
			} else {
				const uint64_t t0 = os_gettime_ns();
				cell = buildAngleCell(r.id, cam, r.camOn[cam],
						      r.camSpeeds[cam]);

				const uint64_t t1 = os_gettime_ns();
				// Replaces and deletes whatever was there.
				events_->setCellWidget(row, col, cell);
				// THE ROW IS SIZED FROM THE CELL, not from a
				// constant. It was 22 px against 28 px of
				// content, then 30 px chosen by adding up the
				// style sheet by hand — and it was still
				// clipping the bottom border, because a
				// stylesheet minimum is not a size hint: the
				// font metrics and the frame the combo actually
				// draws are bigger than the numbers in the
				// rule. Asking the built widget is the only
				// version of this that stays true when the
				// sheet or the font changes.
				const int want = cell->sizeHint().height() + 2;
				if (want > tallestCell)
					tallestCell = want;
				cellBuildNs += (int64_t)(t1 - t0);
				cellInsertNs += (int64_t)(os_gettime_ns() - t1);
				cellCount++;
			}
			// The cell still carries an item underneath: the gate and
			// the selection model both address rows through items, and
			// a cell that is only a widget is a hole in that.
			QTableWidgetItem *slot = events_->item(row, col);
			if (!slot) {
				slot = new QTableWidgetItem(QString());
				slot->setFlags(Qt::ItemIsSelectable |
					       Qt::ItemIsEnabled);
				events_->setItem(row, col, slot);
			}
			slot->setData(Qt::UserRole, r.id);
		}
	}
	// Grow the rows to whatever the cells turned out to need, once, and never
	// shrink them back: a row that changes height between refreshes makes the
	// whole list jump under the operator's eyes while he is reading it.
	if (tallestCell > events_->verticalHeader()->defaultSectionSize()) {
		events_->verticalHeader()->setDefaultSectionSize(tallestCell);
		obs_log(LOG_INFO, "[dock] event rows grown to %d px to fit their cells",
			tallestCell);
	}

	refreshing_ = false;
	// Only when it actually hurt, and at most once every few seconds: a
	// rebuild is a normal thing that happens on every mark.
	const int64_t rebuildNs = (int64_t)(os_gettime_ns() - refreshStartNs);
	const int64_t rowsNs = (int64_t)(os_gettime_ns() - rebuildStartNs);
	if (rebuildNs > 50'000'000LL &&
	    (lastRebuildLogNs_ == 0 ||
	     (int64_t)(os_gettime_ns() - lastRebuildLogNs_) > 5'000'000'000LL)) {
		lastRebuildLogNs_ = os_gettime_ns();
		obs_log(LOG_INFO,
			"[ui] event table refreshed in %lld ms: %d row(s), %d angle "
			"cell(s) reused, %d rebuilt — listJson %lld ms, rows %lld ms "
			"(building cells %lld, inserting them %lld), the rest %lld ms",
			(long long)(rebuildNs / 1'000'000),
			events_->rowCount(), cellReused, cellCount,
			(long long)(jsonNs / 1'000'000),
			(long long)(rowsNs / 1'000'000),
			(long long)(cellBuildNs / 1'000'000),
			(long long)(cellInsertNs / 1'000'000),
			(long long)((rebuildNs - jsonNs - rowsNs) / 1'000'000));
	}
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
		// This selection is OURS, not the operator's, so it must not cue:
		// a cue here would pull the preview off the live camera every time a
		// mark was taken. Only a row he picks himself loads a clip.
		reselecting_ = true;
		for (int r = 0; r < events_->rowCount(); r++) {
			QTableWidgetItem *it = events_->item(r, kColId);
			if (it && it->data(Qt::UserRole).toInt() == target) {
				events_->selectRow(r);
				// ...AND BRING IT INTO VIEW. Selecting a row off
				// the bottom of a scrolled list leaves the panel
				// showing the first marks of the match while the
				// one just taken is somewhere below the edge —
				// so the operator marks a goal and the table
				// does not move. Qt does not scroll for a
				// programmatic selection; it has to be asked.
				events_->scrollToItem(
					it, QAbstractItemView::EnsureVisible);
				break;
			}
		}
		reselecting_ = false;
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
	//
	// With B switched off in Settings there is one bay and it is A — checked
	// here rather than trusted to the selector being hidden, because a hotkey
	// or a Stream Deck reaches this too and neither of them can see a widget.
	if (!channelBEnabled_)
		return {Which::A};
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
	// (There was a badge here spelling out "A|B 3". The two letters under the
	// pictures already say which bay the keys drive, and they say it where
	// the operator is looking; the angle is said by the tally on the camera
	// whose picture is green.)
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

void MultiReplayDock::centreComboItems(QComboBox *cb)
{
	// Both dimensions, because they fail for different reasons. Horizontally,
	// item text is drawn by the view's DELEGATE and no stylesheet reaches it —
	// only the role does. Vertically, AlignVCenter is what stops the text
	// sitting at the bottom of a row that the stylesheet has made taller than
	// the glyphs.
	if (!cb)
		return;
	for (int i = 0; i < cb->count(); i++)
		cb->setItemData(i, (int)(Qt::AlignCenter),
				Qt::TextAlignmentRole);
}

// Write the three values into a cell that is ALREADY about this event and this
// angle, and say whether that was possible.
//
// This is the fast path of the event table, and it exists because the slow one
// was measured: rebuilding a row of two angle cells cost 50-180 ms, nearly all
// of it Qt resolving the dock's style sheet for the five widgets a cell is made
// of, and poll() did it on every bump of the store's version — every mark,
// every trim, every checkbox. The ordinary case never needs it: the rows are
// the same events in the same order and only their values moved.
//
// It refuses, rather than adapting, in the three cases where a cell is not
// interchangeable: a different event or angle (the signal handlers captured
// those), and a comment vocabulary newer than the one the cell was built with.
// Refusing costs a rebuild, which is exactly what used to happen every time.
//
// refreshing_ is true throughout, so the handlers below early-out and none of
// this reaches the store.

bool MultiReplayDock::updateAngleCell(QWidget *cell, int eventId, int cam0,
				      bool on, double speed)
{
	if (!cell)
		return false;
	// No vocabulary version here any more: an angle cell holds no words, so
	// a new tag cannot make it stale. That belongs to the comment cell.
	if (cell->property("mrEventId").toInt() != eventId ||
	    cell->property("mrCam").toInt() != cam0)
		return false;

	auto *box = cell->findChild<QCheckBox *>();
	auto *sp = cell->findChild<QPushButton *>(QStringLiteral("mrAngleSpeed"));
	if (!box || !sp)
		return false;

	if (box->isChecked() != on)
		box->setChecked(on);

	const int pct = speed >= 0 ? (int)std::lround(speed * 100.0) : -1;
	if (sp->property("mrPct").toInt() != pct) {
		sp->setProperty("mrPct", pct);
		sp->setText(pct > 0 ? QString("%1%").arg(pct)
				    : QStringLiteral("--"));
	}
	const bool noOverride = pct <= 0;
	if (sp->property("mrNoOverride").toBool() != noOverride) {
		sp->setProperty("mrNoOverride", noOverride);
		repolish(sp);
	}

	return true;
}

// `presets` is passed IN, and that is the whole of a 175 ms fix.
//
// This used to call ReplayCore::getConfig() for its comment list — once per
// cell, so once per event per camera. getConfig() copies the entire Config
// (eight camera slots, every string, the preset vector) and takes the core
// mutex to do it, and the core mutex is held by startRecording and setConfig.
// On a real 22-minute session the dock's own phase accounting put
// refreshEvents at 120-175 ms, every time, as the longest thing poll() did;
// with eight cameras and a match's worth of marks it is that multiplied by
// forty. The list is the same for every cell on the panel, so it is read once
// and handed down.
// ---------------------------------------------------------------------------
// The event's comment — one cell, one click, two ways in
// ---------------------------------------------------------------------------
//
// THIS IS THE FOURTH ARRANGEMENT OF THIS COLUMN, so the constraints are worth
// stating once, together, because each previous version satisfied some of them
// and broke another:
//
//   1. AT REST IT IS TEXT. Sixty rows of framed control say what sixty words
//      say, and set the row height for the whole list while they do it.
//   2. ONE CLICK. Both to type and to choose — a delegate on a double click
//      looked right and was WRONG here, because a double click on a row of this
//      table already means "put this event on air", and with "in output" on
//      that reaches the Program. Editing a comment must never be able to do
//      that, and a widget in the cell is what guarantees it: the table never
//      sees the click at all.
//   3. THE VOCABULARY IS VISIBLE. Hiding it behind the right button, as the
//      previous version did, is hiding it: nobody right-clicks a word to find
//      out that a list of words exists.
//
// So: a flat line edit that takes a caret on the first click, and beside it a
// chooser that opens the list on ITS first click. Neither has a frame until the
// pointer is over the cell, which is where a frame says something.
QWidget *MultiReplayDock::buildNoteCell(int eventId, const std::string &note,
					const std::vector<std::string> &presets)
{
	// PARENTLESS until setCellWidget takes it: the dock's style sheet is
	// resolved for a widget the moment it is given a parent inside the dock,
	// so building the pair detached polishes the subtree once.
	auto *w = new QWidget;
	w->setObjectName(QStringLiteral("mrNoteCell"));
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(2, 0, 0, 0);
	h->setSpacing(0);

	auto *cm = new QLineEdit(w);
	cm->setObjectName(QStringLiteral("mrAngleNote"));
	cm->setToolTip(obs_module_text("Dock.CamNoteHint"));
	cm->setPlaceholderText(kNoNote);
	cm->setAlignment(Qt::AlignCenter);
	cm->setFrame(false);
	h->addWidget(cm, 1);

	// The chooser. A glyph rather than a word: it is 14 px wide and the words
	// it offers are in the list it opens.
	auto *pick = new QPushButton(w);
	pick->setObjectName(QStringLiteral("mrNotePick"));
	pick->setToolTip(obs_module_text("Dock.CamNoteHint"));
	pick->setCursor(Qt::PointingHandCursor);
	pick->setFocusPolicy(Qt::NoFocus);
	pick->setFixedWidth(14);
	setKeyIcon(pick, Icon::More, tintsFor(sc()), 10);
	h->addWidget(pick, 0);

	w->setProperty("mrEventId", eventId);
	// A cell records the vocabulary it was built with: a word typed on another
	// event makes this one stale, because its list would be missing it.
	w->setProperty("mrVocab", (qulonglong)commentVocabVersion_);

	connect(pick, &QPushButton::clicked, this, [this, cm, presets, pick]() {
		if (refreshing_)
			return;
		QMenu m(pick);
		QStringList words;
		for (const auto &p : presets)
			words << QString::fromStdString(p);
		for (const QString &s : sessionComments_)
			if (!words.contains(s))
				words << s;
		// "No comment" is a choice and not a gap: an operator who marked
		// the wrong thing needs a way back to blank that is not select-all
		// and delete.
		QAction *none = m.addAction(kNoNote);
		connect(none, &QAction::triggered, cm,
			[cm]() { cm->setText(QString()); });
		if (!words.isEmpty())
			m.addSeparator();
		for (const QString &t : words) {
			QAction *a = m.addAction(t);
			connect(a, &QAction::triggered, cm,
				[cm, t]() { cm->setText(t); });
		}
		m.exec(pick->mapToGlobal(QPoint(0, pick->height())));
	});

	// A TAG REACHES THE STORE THE MOMENT IT IS ON SCREEN, whichever way it got
	// there — typed or taken off the list. It fires per keystroke, and that is
	// affordable because the table refuses to rebuild while a line edit inside
	// it has focus (see refreshEvents). The comparison keeps even that honest:
	// re-writing the same text would bump the store's version for nothing.
	connect(cm, &QLineEdit::textChanged, this,
		[this, eventId](const QString &text) {
			if (refreshing_)
				return;
			const std::string want = text.trimmed().toStdString();
			auto &store = EventStore::instance();
			if (store.description(eventId) == want)
				return;
			store.setDescription(eventId, want);
		});
	// Finishing the edit is what promotes a word to this session's list: it is
	// the point at which the operator has decided on it. Doing it per keystroke
	// would offer "G", "Go" and "Gol" on every other row.
	connect(cm, &QLineEdit::editingFinished, this, [this, cm]() {
		if (refreshing_)
			return;
		rememberComment(cm->text().trimmed());
	});

	cm->setText(QString::fromStdString(note));
	return w;
}

// The fast path for a comment cell that already belongs to this event.
bool MultiReplayDock::updateNoteCell(QWidget *cell, int eventId,
				     const std::string &note)
{
	if (!cell || cell->property("mrEventId").toInt() != eventId ||
	    cell->property("mrVocab").toULongLong() != commentVocabVersion_)
		return false;
	auto *cm = cell->findChild<QLineEdit *>(QStringLiteral("mrAngleNote"));
	if (!cm)
		return false;
	const QString noteQ = QString::fromStdString(note);
	if (cm->text() != noteQ)
		cm->setText(noteQ);
	return true;
}

QWidget *MultiReplayDock::buildAngleCell(int eventId, int cam0, bool on,
					 double speed)
{
	// [☑] [speed ▾] - one widget, one angle, two answers: does it play,
	// and how fast. WHAT it is moved out to a column of its own (kColNote):
	// a goal is a goal on every lens that saw it, and asking once per camera
	// meant it was answered on one of them and blank on the rest.
	// Widgets rather than a delegate on purpose: the check has to toggle on
	// the FIRST click and the two menus have to open on the first click too.
	// With a delegate each of those is a click to select, then a click to
	// edit, and in a live gallery that second click is the one that does not
	// happen.
	// PARENTLESS until setCellWidget takes it. The dock carries 376 lines of
	// style sheet, and Qt resolves that sheet for a widget the moment it is
	// given a parent inside it — so building the five widgets of this cell
	// under `events_` polished each of them separately against the whole
	// sheet. Measured, one row of two cells: 62-123 ms. Built detached, the
	// subtree is polished ONCE, when the table adopts it.
	auto *w = new QWidget;
	auto *h = new QHBoxLayout(w);
	// THE PAIR IS CENTRED AND TIGHT. With the comment gone the cell holds two
	// small controls in a column the table stretches, and packed to the left
	// at the old spacing they read as a tick marooned a long way from the
	// number it belongs to - two things, not one answer about one angle.
	h->setContentsMargins(2, 0, 2, 0);
	h->setSpacing(2);
	h->addStretch(1);

	auto *box = new QCheckBox(w);
	box->setChecked(on);
	box->setToolTip(obs_module_text("Dock.AngleOnHint"));
	h->addWidget(box);

	// THE SPEED IS A LABEL - text, like the id and the duration beside it -
	// and a click on it opens the list. It has been a drop-down and then a
	// chip, and the operator asked for the label back both times: a frame
	// round a value says "this is a control", which is true of every cell in
	// this table and so worth saying on none of them, and what it costs is
	// the row height for the whole list.
	//
	// FIXED WIDTH so the column does not dance as "--" becomes "100%": eight
	// rows of angles are scanned down, and a value that moves sideways between
	// rows is read twice.
	auto *sp = new QPushButton(w);
	sp->setObjectName("mrAngleSpeed");
	sp->setToolTip(obs_module_text("Dock.AngleSpeedHint"));
	sp->setCursor(Qt::PointingHandCursor);
	sp->setFocusPolicy(Qt::NoFocus);
	sp->setFixedWidth(44);
	const int pct = speed >= 0 ? (int)std::lround(speed * 100.0) : -1;
	// "--", NOT "100%", for the default (Angelo, 2026-08-17). The value means
	// "no override; the slider decides", and printing it as a number lies
	// whenever the slider is not at 100: with the slider on 25 the cell read
	// 100% while the clip played at a quarter speed. A number the operator can
	// read is worth having, but not a number that can be wrong.
	sp->setText(pct > 0 ? QString("%1%").arg(pct) : QStringLiteral("--"));
	sp->setProperty("mrPct", pct);
	// Grey for "the slider decides", the panel's ordinary text for an override.
	// A PROPERTY, not a per-widget style sheet: setStyleSheet on a single widget
	// makes Qt build a style context of its own for it and re-polish its
	// subtree, once per angle cell, on a rebuild that was already the longest
	// thing the dock's poll did.
	sp->setProperty("mrNoOverride", pct <= 0);
	h->addWidget(sp);
	h->addStretch(1);

	const int a1c = cam0 + 1;
	connect(sp, &QPushButton::clicked, this, [this, sp, eventId, a1c]() {
		if (refreshing_)
			return;
		QMenu m(sp);
		// 100 IS one of the presets, and has to be: "--" is not a speed,
		// so without it there is no way to pin an angle to 1x while the
		// slider sits at 25.
		const QVector<int> pcts = {-1, 25, 33, 50, 75, 100, 200};
		const int now = sp->property("mrPct").toInt();
		for (int p : pcts) {
			QAction *a = m.addAction(
				p > 0 ? QString("%1%").arg(p)
				      : QStringLiteral("--"));
			a->setCheckable(true);
			a->setChecked(p == now);
			connect(a, &QAction::triggered, this,
				[this, eventId, a1c, p]() {
					EventStore::instance().setAngleSpeed(
						eventId, a1c,
						p > 0 ? p / 100.0 : -1.0);
				});
		}
		m.exec(sp->mapToGlobal(QPoint(0, sp->height())));
	});


	// WHAT THIS CELL IS ABOUT, so refreshEvents can tell whether it may be
	// updated in place instead of rebuilt. The connections below capture the
	// event and the angle by value, so a cell may only ever be reused for that
	// same pair — reusing it for another event would write the operator's next
	// edit onto the wrong one. The vocabulary version is here for the same
	// reason: a reused cell keeps the comment list it was built with.
	w->setProperty("mrEventId", eventId);
	w->setProperty("mrCam", cam0);

	const int a1 = cam0 + 1; // EventStore is 1-based

	connect(box, &QCheckBox::toggled, this, [this, eventId, a1](bool v) {
		if (refreshing_)
			return;
		EventStore::instance().setAngle(eventId, a1, v);
	});
	return w;
}

void MultiReplayDock::onEventItemChanged(QTableWidgetItem *item)
{
	// ONE editable column: the event comment. In, out and duration are
	// read-only, and what changes per angle lives in the widget that cell
	// holds (see buildAngleCell).
	//
	// itemsProgrammatic_ is what keeps refreshEvents from writing its own
	// redraw back into the store as if the operator had typed it.
	if (!item || itemsProgrammatic_ || refreshing_)
		return;
	if (item->column() != kColNote || !events_)
		return;
	QTableWidgetItem *idIt = events_->item(item->row(), kColId);
	if (!idIt)
		return;
	const int id = idIt->data(Qt::UserRole).toInt();
	if (id <= 0)
		return;
	const std::string want = item->text().trimmed().toStdString();
	auto &store = EventStore::instance();
	if (store.description(id) == want)
		return;
	store.setDescription(id, want);
}

// ---------------------------------------------------------------------------
// Event filter — double-click on note labels to edit
// ---------------------------------------------------------------------------

bool MultiReplayDock::eventFilter(QObject *watched, QEvent *event)
{
	// A DOUBLE CLICK ON THE FLOATING PANEL'S TITLE BAR MAXIMISES IT, it does
	// not put it away. Qt's answer to one is _q_toggleTopLevel() — the panel
	// re-docks — and inside OBS that means it goes back to wherever the layout
	// last had it, which can be behind another dock's tab: from the operator's
	// side the panel he was working in simply vanished, and came back only when
	// something else made OBS re-lay-out. Reported from a real rig.
	//
	// SWALLOWING IS THE WHOLE FIX ON A NATIVE FRAME — and doing more than that
	// was the second report. A non-client message is delivered to Qt AND then
	// falls through to DefWindowProc (it has to: dragging, resizing and the
	// system menu are all non-client, and Qt returning "handled" would take
	// them away). So Windows already toggles maximise on a title-bar double
	// click, now that the window has a maximise box at all — and a showNormal()
	// of ours on top of it made the panel shrink and immediately grow back,
	// because two actors were toggling the same thing. Blocking Qt's handler
	// leaves exactly one.
	//
	// The Qt-DRAWN title bar (xcb, Wayland — where wmSupportsNativeWindowDeco()
	// is false) has no DefWindowProc behind it, so there the toggle IS ours.
	// That one arrives as an ordinary double click, and is only ours ABOVE the
	// panel — that is where the bar is; inside our own rectangle a double click
	// belongs to whatever was clicked.
	if (filteredHost_ && watched == filteredHost_ &&
	    filteredHost_->isFloating()) {
		const QEvent::Type t = event->type();
		if (t == QEvent::NonClientAreaMouseButtonDblClick)
			return true; // the platform does the rest
		if (t == QEvent::MouseButtonDblClick) {
			auto *me = static_cast<QMouseEvent *>(event);
			if (me->position().y() < geometry().top()) {
				// MAXIMISE, NOT FULL SCREEN — the difference is
				// the title bar. Full screen takes every piece
				// of chrome with it, which is right for the ⛶
				// key and wrong for a double click aimed at the
				// very bar the operator is asking to keep.
				if (filteredHost_->isMaximized())
					filteredHost_->showNormal();
				else
					filteredHost_->showMaximized();
				refreshFullScreenKey();
				return true;
			}
		}
	}
	// THE TABLE EATS THE KEYS THAT MATTER. A QTableWidget with focus takes
	// Enter to open an editor and ←/→ to walk across columns, and the table is
	// where the operator's focus is for most of a match — so without this the
	// transport keys existed everywhere except the one place he was.
	//
	// ↑/↓ are deliberately LEFT to the table: moving down a row is moving to the
	// next event, which is exactly what the key is for, and taking it over would
	// also leave the table's current cell behind the selection.
	if (event->type() == QEvent::KeyPress &&
	    (watched == events_ || qobject_cast<QAbstractButton *>(watched))) {
		auto *ke = static_cast<QKeyEvent *>(event);
		// Space is left alone: on a focused button it is the click, and taking
		// it away would break the one keyboard gesture Qt gives for free.
		const bool tableUp = watched == events_ &&
				     (ke->key() == Qt::Key_Up ||
				      ke->key() == Qt::Key_Down);
		if (ke->key() != Qt::Key_Space && !tableUp &&
		    handleTransportKey(ke))
			return true;
	}
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
			// The per-angle speed cell is a combo whose display is a
			// read-only line edit (so the text can be CENTRED — see
			// buildAngleCell). A read-only line edit eats the press
			// instead of dropping the list down, so the press is handed
			// back to the combo. This has to work on the FIRST click:
			// in a gallery the second one does not happen.
			if (auto *le = qobject_cast<QLineEdit *>(watched)) {
				if (auto *cb = qobject_cast<QComboBox *>(
					    le->parentWidget())) {
					cb->showPopup();
					return true;
				}
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}

void MultiReplayDock::rememberComment(const QString &text)
{
	// A comment typed on one event joins the list offered on every other one,
	// for the rest of the session.
	//
	// NOT written into Config.commentPresets, tempting as that is: setConfig()
	// re-points the segment index and re-creates the Branch Output filters, so
	// persisting a comment would mean a typed word could restart the recording
	// path mid-match. The preset list stays the operator's, edited in Settings;
	// this is the session's own memory of what he has been writing.
	const QString t = text.trimmed();
	if (t.isEmpty() || sessionComments_.contains(t))
		return;
	// Bounded: this is a convenience, not a log. The oldest goes when the list
	// is full, because what was typed most recently is what is about to be
	// typed again.
	sessionComments_.append(t);
	while (sessionComments_.size() > kMaxSessionComments)
		sessionComments_.removeFirst();
	// The cells are rebuilt from this list on the next refresh, and a new
	// comment has to reach the OTHER rows, so ask for one — and say that the
	// VOCABULARY moved, not just the table. Cells are reused in place now, and
	// a reused cell keeps the list it was built with: without this the word
	// just typed would reach every other row only when something else happened
	// to force those cells to be rebuilt, which is exactly the kind of "it
	// works, sometimes" the reuse must not introduce.
	commentVocabVersion_++;
	commentsDirty_ = true;
	obs_log(LOG_INFO, "[dock] comment '%s' added to this session's list (%d)",
		t.toUtf8().constData(), (int)sessionComments_.size());
}

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
		if (QMessageBox::question(
			    &dlg, "obs-multireplay",
			    obs_module_text("Dock.UpdateInstallConfirm")) !=
		    QMessageBox::Yes)
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
	cfg.updateChannel = chan->currentData().toString().toStdString();
	cfg.outputSceneName = outScene->currentData().toString().toStdString();
	cfg.outputSceneNameB = outSceneB->currentData().toString().toStdString();
	cfg.enableChannelB = useB->isChecked();
	cfg.abOutputUsesB = abOut->currentData().toBool();
	cfg.musicSourceName = music->currentData().toString().toStdString();
	cfg.musicFilePath = musicFile->text().trimmed().toStdString();
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

} // namespace multireplay
