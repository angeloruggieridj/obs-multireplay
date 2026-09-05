/*
obs-multireplay — MultiReplayDock: shared implementation detail, not a public API
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Split out of multireplay-dock.cpp's anonymous namespaces (pure move, no
behaviour change) when the dock's implementation moved into several .cpp
files: a helper with internal linkage in one TU is invisible to another, so
the small set of free functions and constants several of those files call —
building a key, the obs_data RAII wrapper, the localized-error headline —
needed a home every one of them can include. `inline` on every constant here
is load-bearing, not decoration: this project builds with
-Wunused-const-variable as an error (see the kColFirstCam/kColsPerCam note
below, carried over from where it was written), and a plain file-scope
constexpr unused in some particular .cpp that includes this header would
fail that .cpp's build even though the constant is used elsewhere.

Not a public header: nothing outside the dock's own .cpp files has business
including this.
*/

#pragma once

#include "dock-icons.hpp"
#include "dock-layout.hpp"
#include "dock-style.hpp"
#include "error-locale.hpp"
#include "multireplay-dock.hpp"
#include "replay-core.hpp"

#include <obs-module.h>

#include <QButtonGroup>
#include <QColor>
#include <QFontDatabase>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <string>

namespace multireplay {

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
inline Scheme &sc()
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
inline QPushButton *iconBtn(Icon ic, const char *id, const QString &tip,
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
inline QPushButton *iconTextBtn(Icon ic, const QString &text, const char *id,
				QWidget *parent, const char *role = "",
				int px = 14)
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

// ── A KEY THAT OPENS A MENU, WITHOUT WEARING THE PLATFORM'S ARROW ────────
//
// `QToolButton::setMenu` tells Qt the button HAS a menu, and Qt then draws its
// own arrow for it — bottom right, larger than the mark we drew, and clipped by
// the key's own edge. Two marks in one key, one of them half drawn: that is the
// gear with a triangle through it, and the same on the ▾ beside the play key
// and the ⋯ over the clip list.
//
// NO STYLE SHEET REACHES IT, and that was measured rather than assumed. Every
// sub-control Qt could plausibly be asking for was probed with a solid red rule
// and none of them was ever painted; the arrow comes out of the base style's
// PE_IndicatorArrowDown (see the note in dock-style.hpp). So the menu is popped
// on `clicked` instead — same place and same gesture as InstantPopup, keyboard
// included, because Space and Enter on a focused button emit clicked() too.
inline void popupOnClick(QToolButton *b, QMenu *menu)
{
	b->setPopupMode(QToolButton::InstantPopup);
	QObject::connect(b, &QToolButton::clicked, b, [b, menu]() {
		menu->popup(b->mapToGlobal(QPoint(0, b->height())));
	});
}

// A LOCALIZED HEADLINE IN FRONT OF THE ENGINE'S OWN ENGLISH, not a
// translation of it: error-locale.hpp only says WHICH known failure this is,
// and the detail behind it (a camera number, a file path, a source name)
// stays exactly as the engine wrote it. An error this cannot classify comes
// back unchanged — the raw English is still better than nothing, which is
// what showNotice(QString::fromStdString(err)) showed everywhere before this.
inline QString localizedError(const std::string &err)
{
	using namespace error_locale;
	const char *key = nullptr;
	switch (classify(err)) {
	case Class::NoFootageForCamera:
		key = "Dock.ErrNoFootageForCamera";
		break;
	case Class::NoPlayableEvents:
		key = "Dock.ErrNoPlayableEvents";
		break;
	case Class::NoFootageInRange:
		key = "Dock.ErrNoFootageInRange";
		break;
	case Class::NoCompletedEvents:
		key = "Dock.ErrNoCompletedEvents";
		break;
	case Class::MusicFileMissing:
		key = "Dock.ErrMusicFileMissing";
		break;
	case Class::MusicNotConfigured:
		key = "Dock.ErrMusicNotConfigured";
		break;
	case Class::MusicSourceMissing:
		key = "Dock.ErrMusicSourceMissing";
		break;
	case Class::MusicSourceNotActive:
		key = "Dock.ErrMusicSourceNotActive";
		break;
	case Class::Unknown:
		break;
	}
	if (!key)
		return QString::fromStdString(err);
	return QString("%1 — %2")
		.arg(obs_module_text(key), QString::fromStdString(err));
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
inline QPushButton *statusToggle(Icon ic, const QString &text, const char *id,
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
inline QColor tint(const QString &hex, int alpha)
{
	QColor c(hex);
	c.setAlpha(alpha);
	return c;
}

// The live dock, so the module's frontend-event handler can reach it when OBS is
// about to clear scene data (see releasePreviewRefs). One dock at a time: it is
// registered by id and OBS builds exactly one.
inline MultiReplayDock *g_dock = nullptr;

inline constexpr int kNCams = kMaxCameras; // 8
// multireplay-dock.hpp cannot see kMaxCameras (it does not include replay-core),
// so the angle-key array is spelled 8 there. If that ever diverges the keys
// would silently stop at the wrong camera.
static_assert(kMaxCameras == 8, "kMaxPreviewTiles in the header is sized for 8");

// Scrubbing means "review from here": the engine plays a range, it has no
// playhead to park. The window is capped because play() materialises every
// packet of the range in RAM, and an uncapped "from here to now" would be a
// multi-gigabyte copy on a long session.
inline constexpr int64_t kScrubReviewNs = 10'000'000'000LL; // 10 s

// How long a one-line notice keeps the status area (see showNotice).
inline constexpr int64_t kNoticeNs = 5'000'000'000LL; // 5 s

// A sequence is several clips, and the engine goes idle between them: the queue
// advance crosses the playback thread's finish callback and the OBS UI task
// queue, which takes a few tens of milliseconds. Anything inside this window
// still counts as "the replay is on screen", so the preview does not flash the
// live camera between two angles of the same event. Anything beyond it is a
// queue that is not coming back, and the preview must be free to move on.
inline constexpr int64_t kSequenceGapGraceNs = 1'500'000'000LL; // 1.5 s

// How long a take gets to prove Branch Output really started it. Branch Output
// re-evaluates its start conditions once a second (plugin-main.cpp:
// TASK_INTERVAL_MS = 1000), so this is four of its ticks — enough for encoder
// creation on a slow adapter, short enough that the operator learns NOW rather
// than when the first replay comes up empty. See poll().
inline constexpr int64_t kArmWatchNs = 4'000'000'000LL; // 4 s

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
inline constexpr int kColFirstCam = MultiReplayDock::kColFirstCam;
inline constexpr int kColsPerCam = MultiReplayDock::kColsPerCam;

// Which camera a table column belongs to. -1 = not a camera column at all.
// Kept as one function so the callers cannot drift.
inline int camPairIndex(int column)
{
	return column < kColFirstCam ? -1 : (column - kColFirstCam) / kColsPerCam;
}

// A camera cell with no comment reads "-" in the reference controller, not blank: an empty cell is
// ambiguous next to a checkbox, a dash is not. It is display only — it never
// reaches the store (see camNoteFromCell).
inline const QString kNoNote = QStringLiteral("-");

inline QString monoFamily()
{
	// Prefer a real fixed-pitch family for timecodes; fall back gracefully.
	QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	return f.family();
}

inline QString formatTc(int64_t ns)
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
inline QPushButton *compactBtn(const QString &text, QWidget *parent,
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
inline QPushButton *transportBtn(const QString &text, QWidget *parent,
				 const QString &tip,
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
inline void repolish(QWidget *w)
{
	w->style()->unpolish(w);
	w->style()->polish(w);
	// ...AND PUT THE SECTION'S HEIGHT BACK. Polishing applies the sheet's
	// min-height by WRITING setMinimumSize on the widget, so a key that its
	// section pinned to two rows — REC once it is armed, the green play key —
	// loses that pin every time one of its properties flips. See
	// kPinnedHeightProperty in dock-layout.hpp.
	repinKeys(w);
	w->update();
}

// A latching control drawn as a button, which is what the reference controller uses for Live, Loop
// and the music toggle — a checkbox reads as a form field, a lit button reads
// as a state, and this panel is read at a glance.
inline QPushButton *toggleBtn(const QString &text, QWidget *parent,
			      const QString &tip,
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
inline QLabel *sectionLabel(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text.toUpper(), parent);
	l->setObjectName(QStringLiteral("mrSectionLabel"));
	return l;
}

// this panel that are allowed to shout, the green band and the REC key.
inline QWidget *zoneBox(const QString &title, QWidget *content, QWidget *parent)
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
inline void renderSourceFitted(obs_source_t *src, uint32_t cx, uint32_t cy)
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

// ---------------------------------------------------------------------------
// SeekBar/ClipBar repaint bookkeeping
// ---------------------------------------------------------------------------

// The ruler strip under the track: where the graduation ticks and their labels
// are drawn. Small, but it is the whole difference between a scale and a
// coloured rectangle, so the bar is made tall enough to afford it rather than
// the labels being squeezed over the track and fighting the timecode there.
// 14 was not enough: the labels are drawn into (kSeekRulerH - 5) pixels, so a
// 14 px strip left 9 px for a number and clipped the digits — the scale was
// there but unreadable, which is the same as not being there. 20 leaves 15,
// which the small font fits at any DPI.
inline constexpr int kSeekRulerH = 20;
// Track thickness. Wide enough to hold the event markers and the timecode the
// way it always did.
inline constexpr int kSeekTrackH = 20;

// Label for a graduation. Minutes and seconds up to an hour, then hours: a
// position bar over a two-hour recording that counts to "137:00" is arithmetic
// the operator should not be doing.
inline QString tickLabel(int64_t ns)
{
	const int64_t s = ns / 1'000'000'000LL;
	if (s < 3600)
		return QString::asprintf("%lld:%02lld", (long long)(s / 60),
					 (long long)(s % 60));
	return QString::asprintf("%lld:%02lld:%02lld", (long long)(s / 3600),
				 (long long)((s % 3600) / 60), (long long)(s % 60));
}

// The two repaint censuses (see RepaintCensus in the header). One shared
// instance across every .cpp that includes this — an `inline` variable, not a
// `static` one, precisely so the seek bar and the poll loop (different .cpp
// files) count the same census rather than each keeping its own.
inline RepaintCensus g_seekCensus;
inline RepaintCensus g_clipCensus;

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
inline constexpr int kCoalesceMs = 16;

} // namespace multireplay
