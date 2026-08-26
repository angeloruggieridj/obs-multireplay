// dock-style.hpp — the panel's colours and its style sheet, in one place.
//
// It lives in a header of its own so that the DOCK and the LAYOUT MOCKUP are
// styled by the same bytes. A mockup with its own copy of the sheet measures a
// panel that does not exist: every height rule here (see the 28 px arithmetic
// below) is part of the layout, not decoration, and a second copy would drift
// from this one the first time a padding changed.
//
// ---------------------------------------------------------------------------
// TWO KINDS OF COLOUR, AND ONLY ONE OF THEM IS THE OPERATOR'S TO CHOOSE
// ---------------------------------------------------------------------------
//
// CHROME — the panel's background, its keys, borders, text, table rows, tabs,
// scrollbars. This is taste, and it belongs to whoever is looking at it. It is
// derived from the Qt palette OBS builds from the user's theme, so the panel
// sits inside Yami, Yami Grey, Rachni or a light theme without looking stuck on.
//
// SIGNAL — REC, the on-air band, the tally on an angle key, the health badge.
// This is NOT taste. Red means on air. An operator reads tally by colour, from
// across a gallery, without reading anything, and a theme whose accent happens
// to be green must not be able to redefine it. The signal hues are constants
// here; what the theme changes is only their LUMINANCE, so they stay legible on
// a light background as well as a dark one.
//
// That distinction is the whole reason this is a struct and a function rather
// than one string: there is no single "accent" for this panel.
//
// ---------------------------------------------------------------------------
// THE 26 px ARITHMETIC
// ---------------------------------------------------------------------------
//
// ONE HEIGHT FOR EVERY KEY: 26 px, and it is arithmetic, not a wish. In Qt a
// style sheet min-height is the CONTENT box — padding and border are added to
// it. So min-height 24 with 3 px of padding and a 1 px border is a 32 px key,
// while a rule with min-height 24 and no padding is a 26 px one. Three heights
// on one row, from a number that looked identical in all three rules.
//
// Every rule below therefore states min-height = 26 - 2*padding - 2*border.
// CHANGING A PADDING HERE MEANS CHANGING ITS min-height.
//
// IT WAS 28. The redesign took two pixels off every key row — with five or six
// of them between the two macro-rows and the folded stack that is 10-14 px of
// event list bought back — and 26 is still comfortably above the size at which
// a key stops being easy to hit in a hurry. The FOLDED key (kKeyFoldedH in
// dock-layout.hpp) went 24 → 22 with it.
#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

namespace multireplay {

// Which set of colours the panel wears. Offered in Settings ▸ Interfaccia.
enum class ThemeChoice {
	// Chrome from the OBS theme's Qt palette. The default: a plugin panel
	// should look like it belongs to the program it is docked in.
	FollowObs = 0,
	// The reference controller's own scheme, fixed, whatever OBS is wearing:
	// near-black chrome, navy list, orange selection. For the operator who
	// wants the panel to look like the panel.
	Broadcast = 1,
	// Same layout, harder edges and brighter text, for a gallery with the
	// lights up.
	HighContrast = 2,
	// A LIGHT panel, ours, and not merely "whatever OBS is wearing". The
	// derived scale has always worked on paper - every surface is a step from
	// the background TOWARDS the text, which on a light theme means darker -
	// but until now the only way to see it was to run OBS in a light theme.
	// An operator working in daylight should not have to re-theme OBS to get
	// a panel he can read.
	Light = 3,
};

// ---------------------------------------------------------------------------
// Scheme — every colour the sheet uses, and nothing else
// ---------------------------------------------------------------------------
//
// 129 distinct hex values used to be written into the sheet by hand. Most were
// near-duplicates of each other (#1e1e1e, #181818, #141414, #101010 …), which
// is what made the panel impossible to re-colour: there was no scale, only a
// list. These are a scale, derived from two colours the theme actually gives us.
struct Scheme {
	// chrome
	QString panel;      // the dock's own background
	QString raise1;     // a key at rest, an input
	QString raise2;     // hovered
	QString sink1;      // pressed, and the well behind the table
	QString sink2;      // the event table's own background
	QString sinkAlt;    // its alternating row
	QString border;
	QString borderHi;   // hovered
	QString text;       // a heading, a hovered key, a value
	QString textKey;    // a key's label at rest — see the note in schemeFor
	QString textMuted;  // captions, the clock, the "--" of no override
	QString textDim;    // disabled, and an empty slot
	QString accent;     // the theme's highlight: selection, current tab
	QString accentText;
	QString rowSel;     // the selected event row
	QString rowSelText;

	// signal — constants in hue, adjusted in luminance; never the theme's
	QString rec;        // REC armed, and the on-air tally
	QString recBg;      // the same, as a fill behind text
	QString pvw;        // the angle being watched, the play key while playing
	QString pvwBg;
	QString onAir;      // the clip band
	QString onAirDim;   // its unplayed remainder
	QString warn;
	QString warnBg;
	QString danger;     // Annulla, and the health badge at its worst
	QString action;     // THE ONE FILLED KEY: "Riproduci eventi"
	QString actionHi;

	// two structural colours that are neither chrome nor signal
	QString tabBar;     // the list tabs' selected fill
	QString seekBar;    // the position bar's played portion
};

namespace detail {

inline QString hex(const QColor &c)
{
	return c.name(QColor::HexRgb);
}

// t in [0,1]: 0 is a, 1 is b.
inline QColor mix(const QColor &a, const QColor &b, double t)
{
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
				a.greenF() + (b.greenF() - a.greenF()) * t,
				a.blueF() + (b.blueF() - a.blueF()) * t);
}

// A signal colour, kept at its own hue and saturation but pushed to a lightness
// that reads against this background. THIS IS THE WHOLE LIGHT-THEME STORY: the
// red of REC on a white panel is the same red, darker.
// `darkFloor` is how bright a signal is pushed to on a DARK panel. 120 for
// most of them: a signal wants to be brighter than the mid point or it reads as
// a dead key. The GREENS pass 0 and keep their own lightness instead, and that
// is the whole of "the green is too light" - #199847 is the colour the on-air
// band is painted, and lifting it to 120 made every other green on the panel a
// perceptibly paler version of the one it was being compared against. A hue
// that already separates from the background does not need lifting.
inline QColor signalOn(const QColor &hue, const QColor &bg, bool dark,
		       int darkFloor = 120)
{
	QColor c = hue.toHsl();
	const int l = c.lightness();
	// On a dark panel a signal wants to be brighter than the mid point; on a
	// light one it wants to be darker, or it disappears into the paper.
	const int want = dark ? qMax(l, darkFloor) : qMin(l, 110);
	c.setHsl(c.hslHue(), c.hslSaturation(), want, 255);
	// …and if it still does not separate from the background, push it away.
	//
	// NOT WHEN THE CALLER SAID THE HUE IS THE STATEMENT (darkFloor 0). The
	// greens ARE the colour the on-air band is painted, and under "follow the
	// OBS theme" - where the background is often mid-dark rather than near
	// black - this clause fired and lifted them to bg + 90, which is the pale
	// green that kept being reported. The band is legible at that hue on that
	// background because it IS that hue on that background.
	if (darkFloor > 0 && qAbs(c.lightness() - bg.lightness()) < 60)
		c.setHsl(c.hslHue(), c.hslSaturation(),
			 dark ? qMin(255, bg.lightness() + 90)
			      : qMax(0, bg.lightness() - 90),
			 255);
	return c;
}

} // namespace detail

// The scheme the panel should wear, given what the operator asked for and what
// OBS is currently themed with.
//
// `pal` is the application palette — OBS builds it from the theme's `palette_*`
// variables and calls setPalette() on the QApplication (OBSApp_Themes.cpp), and
// it is the ONLY route a plugin has to a theme's colours: the frontend API
// exposes obs_frontend_is_theme_dark() and nothing else. Parsing the .obt files
// was the alternative and was rejected — they need var() and calc() resolved,
// they live in two places, and a third-party theme is arbitrary.
//
// THERE IS NO `dark` PARAMETER, deliberately. There was, fed from
// obs_frontend_is_theme_dark(), and the mockup passed the wrong one on its very
// first run: the whole signal scale inverted and "Riproduci eventi" came out
// BLACK on a dark panel. Whether a background is dark is not something a caller
// should be trusted to assert when it is sitting right there in the colour. It
// is measured.
inline Scheme schemeFor(ThemeChoice choice, const QPalette &pal)
{
	using namespace detail;

	QColor bg, fg, hl, hlText, base;
	switch (choice) {
	case ThemeChoice::Broadcast:
		// The reference controller's own, sampled from its screenshots.
		bg = QColor("#0c0c0c");
		fg = QColor("#d0d0d0");
		hl = QColor("#1D3D74");
		hlText = QColor("#ffffff");
		base = QColor("#00121C");
		break;
	case ThemeChoice::Light:
		bg = QColor("#efefef");
		fg = QColor("#101010");
		hl = QColor("#2f6fd0");
		hlText = QColor("#ffffff");
		base = QColor("#ffffff");
		break;
	case ThemeChoice::HighContrast:
		bg = QColor("#000000");
		fg = QColor("#ffffff");
		hl = QColor("#2f6fd0");
		hlText = QColor("#ffffff");
		base = QColor("#000814");
		break;
	case ThemeChoice::FollowObs:
	default:
		bg = pal.color(QPalette::Window);
		fg = pal.color(QPalette::WindowText);
		hl = pal.color(QPalette::Highlight);
		hlText = pal.color(QPalette::HighlightedText);
		base = pal.color(QPalette::Base);
		// A theme that leaves Base and Window the same colour gives the
		// table no well to sit in; sink it a step rather than drawing a
		// flat panel with an invisible list on it.
		if (qAbs(base.lightness() - bg.lightness()) < 6)
			base = mix(bg, fg, 0.05);
		break;
	}

	const bool dark = bg.lightness() < 128;
	Scheme s;
	// ELEVATION, derived rather than listed. Every surface is a step from the
	// background towards the text colour, so the same numbers work on a light
	// theme — where "towards the text" happens to mean darker.
	s.panel = hex(bg);
	s.raise1 = hex(mix(bg, fg, 0.07));
	s.raise2 = hex(mix(bg, fg, 0.14));
	s.sink1 = hex(mix(bg, dark ? QColor(Qt::black) : QColor(Qt::white), 0.35));
	s.sink2 = hex(base);
	s.sinkAlt = hex(mix(base, fg, 0.06));
	s.border = hex(mix(bg, fg, 0.20));
	s.borderHi = hex(mix(bg, fg, 0.34));
	// FOUR STEPS OF TEXT, and the middle one is the one that matters: a KEY AT
	// REST is not a caption. Collapsing the two put every button label at the
	// weight of a section heading — legible in a screenshot, thin in a gallery
	// where the panel is two feet away and lit from the side.
	s.text = hex(mix(bg, fg, 0.95));
	s.textKey = hex(mix(bg, fg, 0.70));
	s.textMuted = hex(mix(bg, fg, 0.50));
	s.textDim = hex(mix(bg, fg, 0.26));
	s.accent = hex(hl);
	s.accentText = hex(hlText);
	s.tabBar = hex(hl);

	// SIGNAL. Fixed hues; only their lightness is answerable to the theme.
	const QColor recHue("#C0202A");
	// ONE GREEN. The angle being watched, the play key while it runs, a
	// ticked angle and the on-air band were three greens a shade apart -
	// near enough to read as one colour badly reproduced rather than as a
	// distinction, and the paler two looked washed out beside the band.
	// Green means REPLAY on this panel and red means the take and the
	// Program: that is two signals, so there are two hues.
	const QColor airHue("#199847");
	const QColor pvwHue = airHue;
	const QColor warnHue("#E0A020");
	const QColor actHue = airHue;

	const QColor rec = signalOn(recHue, bg, dark);
	const QColor pvw = signalOn(pvwHue, bg, dark, 0);
	// THE GREEN IS THE BAND'S, unlifted: see signalOn's darkFloor.
	const QColor air = signalOn(airHue, bg, dark, 0);
	const QColor wrn = signalOn(warnHue, bg, dark);
	const QColor act = signalOn(actHue, bg, dark, 0);

	// THE LIT-KEY WASH IS WEAKER ON A LIGHT PANEL, and it is not taste.
	// A lit toggle is drawn as its signal colour ON a wash of the same colour
	// (@pvw@ on @pvwBg@). Mixing the background towards the signal moves it
	// AWAY from the paper and TOWARDS the very colour being written on it — on
	// a dark panel that raises the background from near-black and the bright
	// signal still stands off it, but on a light one it drags the background
	// down towards the ink. Measured: 2.9:1 for a checked key's label on the
	// light theme, under the 3.0 a working panel needs, and the icon drawn on
	// that key had exactly the same problem.
	// 0.20 on a light panel, not 0.45: the greens were unified onto the band's
	// hue, which is a shade lighter than the one the lit keys used to carry, and
	// the mark drawn on a lit key fell to 2.9:1 - under the 3.0 a working panel
	// needs. Washing the background LESS keeps it nearer the paper, which is
	// what the dark ink on it has to stand off.
	const double wash = dark ? 1.0 : 0.20;
	s.rec = hex(rec);
	s.recBg = hex(mix(bg, rec, 0.30 * wash));
	s.pvw = hex(pvw);
	s.pvwBg = hex(mix(bg, pvw, 0.24 * wash));
	s.onAir = hex(air);
	// The on-air band is FILLED and carries white text, so it is not a wash and
	// does not follow the rule above: it has to stay a solid green on any theme.
	//
	// AND ON A LIGHT PANEL IT IS MIXED TOWARDS BLACK, not towards the
	// background. Mixing towards the background is what "dim" means on a dark
	// panel - it walks the green down to near-black - but from WHITE the same
	// sum walks it up to a pale mint, and the band came out washed out with
	// white text on it that could barely be read. Dim means darker; on paper
	// the background is the wrong direction to look for darker.
	s.onAirDim = hex(dark ? mix(bg, air, 0.45)
			      : mix(air, QColor(Qt::black), 0.22));
	s.warn = hex(wrn);
	s.warnBg = hex(mix(bg, wrn, 0.22 * wash));
	s.danger = hex(rec);
	s.action = hex(act);
	s.actionHi = hex(mix(act, QColor(Qt::white), 0.18));
	s.seekBar = hex(mix(hl, fg, 0.10));

	// The selected row. In Broadcast it is the reference controller's orange,
	// which is part of what that scheme IS; anywhere else it is the theme's own
	// highlight, because a row selected in one colour in the table and another
	// in every OBS list is a panel that looks bolted on.
	if (choice == ThemeChoice::Broadcast) {
		s.sink2 = "#00121C";
		s.sinkAlt = "#002A42";
		s.rowSel = "#DB5026";
		s.rowSelText = "#ffffff";
	} else {
		s.rowSel = hex(hl);
		s.rowSelText = hex(hlText);
	}
	return s;
}

// The sheet itself. Written with @tokens@ rather than %1 placeholders: a
// stylesheet is read far more often than it is edited, and forty positional
// arguments is a file nobody can check by eye.
//
// SPLIT INTO TWO LITERALS, and it is the compiler's rule rather than a section
// boundary: MSVC refuses a single string literal over 16380 bytes (C2026,
// "trailing characters will be truncated" — a stylesheet silently missing its
// second half). Adjacent literals are concatenated at translation, so the sheet
// is still one string; it just arrives in two pieces.
inline const char *const kDockStyleTemplate =
R"QSS(
/* ── base ─────────────────────────────────────────────── */
#MultiReplayDock { background: @panel@; }
/* EVERY SURFACE THE PANEL OWNS, and BY ID so OBS's own theme cannot out-rank
   it. These were left to the application palette, which is invisible while the
   panel and OBS are both dark and is a set of black patches the moment they are
   not: a dialog, a scroll area's viewport, a menu, a group box. */
#MultiReplayDock QDialog, #MultiReplayDock QStackedWidget,
#MultiReplayDock QScrollArea, #MultiReplayDock QAbstractScrollArea {
	background: @panel@; color: @text@;
}
#MultiReplayDock QMenu {
	background: @raise1@; color: @text@;
	border: 1px solid @border@;
}
#MultiReplayDock QMenu::item:selected {
	background: @rowSel@; color: @rowSelText@;
}
#MultiReplayDock QGroupBox { color: @text@; }
#MultiReplayDock QToolTip {
	background: @raise2@; color: @text@; border: 1px solid @border@;
}
#MultiReplayDock QLabel { color: @text@; background: transparent; }

/* labels */
QLabel#mrMuted      { color: @textMuted@; font-size: 10px; }
QLabel#mrTimecode   { color: @text@; font-size: 12px; font-weight: 700;
                      letter-spacing: 0.3px; }
QLabel#mrSectionLabel { color: @textMuted@; font-size: 9px; font-weight: 700;
                        letter-spacing: 1.4px; text-transform: uppercase; }
/* wall clock over the "remaining" line, signal-red while a take is running */
QLabel#mrClock      { color: @textMuted@; font-size: 10px; }
QLabel#mrClock[rec="true"] { color: @rec@; font-weight: 700; }

/* ── THE KEY TAXONOMY ──────────────────────────────────────
   THREE CLASSES, not fifteen treatments. The panel had one visual role per
   kind of key — mrPlay, mrTransport, mrNow, mrLive, mrToggle, mrRec, mrHealth,
   mrGear, mrAngle, mrSpeedChip, mrChanSel, mrAccent, mrDanger and the default —
   which is fifteen cases to learn rather than a system to read. They are now
   three, and the axis is CONSEQUENCE rather than importance:

     COMMAND   outlined, neutral. Marks, trims, transport, exports, angle keys,
               speed presets. It does something; it does not reach Program.
     ACTION    FILLED. Exactly two on the whole panel: "Riproduci eventi" and
               REC once armed. These are the keys that take the Program.
     STATE     lit but HOLLOW. Loop, music, In output, Live, the current angle,
               the active speed chip. Unmistakably on, and clearly not the
               thing that starts a replay.

   The rule that made this necessary: In and Out were drawn in the same filled
   green as "Riproduci eventi", so the loudest keys on the panel were two that
   mark a point and do not put anything on air. */
#MultiReplayDock QPushButton {
	background: @raise1@; color: @textKey@;
	border: 1px solid @border@; border-radius: 4px;
	padding: 3px 9px; font-size: 11px; min-height: 18px;
}
#MultiReplayDock QPushButton:hover  { background: @raise2@; border-color: @borderHi@; color: @text@; }
#MultiReplayDock QPushButton:pressed { background: @sink1@; }
#MultiReplayDock QPushButton:disabled { color: @textDim@; border-color: @raise1@; }

/* ── transport step / icon buttons ────────────────────── */
QPushButton#mrTransport {
	background: @raise1@; border: 1px solid @border@; border-radius: 5px;
	color: @text@; font-size: 14px;
	min-width: 30px; min-height: 24px; padding: 0;
}
QPushButton#mrTransport:hover { background: @raise2@; border-color: @borderHi@; }

/* >> lives ON the green band, which is 28 px tall with 2 px of margin, so it
   cannot ask for the 28 px of height a transport key asks for: a stylesheet
   min-height LARGER than the widget's own fixed height makes the style draw a
   taller frame than the widget owns, and the bottom border lands outside it.
   Hence its own role, with the height it can actually have. */
QPushButton#mrSkip {
	background: @onAirDim@; border: 1px solid @onAir@; border-radius: 4px;
	color: #ffffff; font-size: 11px; font-weight: 700;
	min-width: 30px; min-height: 0px; padding: 0px 4px;
}
QPushButton#mrSkip:hover { background: @onAir@; color: #ffffff; }

/* play/pause — a COMMAND at rest, a STATE while it runs */
QPushButton#mrPlay {
	background: @raise1@; border: 1px solid @border@; border-radius: 5px;
	color: @text@; font-size: 16px;
	min-width: 38px; min-height: 24px; padding: 0;
}
QPushButton#mrPlay:hover { background: @raise2@; border-color: @borderHi@; }
QPushButton#mrPlay[playing="true"] { background: @pvwBg@; border-color: @pvw@; color: @pvw@; }
QPushButton#mrPlay[playing="true"]:hover { background: @pvwBg@; border-color: @pvw@; }

/* NOW / live-edge. RED AT REST TOO: NOW is where the operator goes to get out
   of a replay and back on the live edge, and drawn in the panel's ordinary grey
   it was the least visible key in the row that matters most. */
QPushButton#mrNow {
	background: @raise1@; border: 1px solid @border@; border-radius: 5px;
	font-weight: 700; font-size: 10px; letter-spacing: 0.8px;
	min-height: 24px; min-width: 36px; padding: 0;
	color: @rec@; border-color: @recBg@;
}
QPushButton#mrNow:hover { color: @rec@; border-color: @rec@; background: @recBg@; }
QPushButton#mrNow[live="true"] { background: @recBg@; border-color: @rec@; color: @rec@; }

/* ── "Live" mode toggle — red when marks are taken as they happen ── */
QPushButton#mrLive {
	background: @raise1@; color: @textKey@;
	border: 1px solid @border@; border-radius: 3px;
	font-weight: 700; font-size: 11px; letter-spacing: 0.6px;
	/* min-width is the ICON-ONLY floor, not the labelled one: in a column
	   these two lose their words and keep their marks (see
	   applyCompactChrome), and a 54 px floor stated here would have kept the
	   width the word needed long after the word was gone. */
	min-height: 20px; /* + 2px padding + 2px border = 26 */ min-width: 22px; padding: 2px 10px;
}
QPushButton#mrLive:hover { border-color: @borderHi@; color: @text@; }
QPushButton#mrLive:checked {
	background: @rec@; color: #ffffff; border-color: @rec@;
}

/* ── latching toggles (Loop · music · to output) = STATE ────
   A LIT TOGGLE IS A STATE, NOT AN INVITATION. It used to be the same filled
   green as the play key, so "Loop is on" and "press this to play" carried the
   same weight — and on a panel read at a glance under pressure, two meanings in
   one colour is one meaning too many. */
QPushButton#mrToggle {
	background: @raise1@; color: @textKey@;
	border: 1px solid @border@; border-radius: 3px;
	font-size: 10px; min-height: 20px; padding: 2px 9px;
}
QPushButton#mrToggle:hover { border-color: @borderHi@; color: @text@; }
QPushButton#mrToggle:checked {
	background: @pvwBg@; color: @pvw@; border-color: @pvw@;
	font-weight: 700;
}
QPushButton#mrToggle:checked:hover { background: @pvwBg@; color: @pvw@; }

/* ── list tabs (one tab per event list) ─────────────────────── */
QTabBar#mrListTabs { background: transparent; }
/* No font-size here on purpose: the tab font is set on the WIDGET (see
   buildToolbar). A size that lives only in the stylesheet is a size nothing
   outside the painter can measure, and "is this tab wide enough for its own
   name" is exactly the question the gate has to answer. */
QTabBar#mrListTabs::tab {
	background: @raise1@; color: @textMuted@;
	border: 1px solid @border@; border-bottom: 0;
	padding: 3px 9px; margin-right: 1px; min-width: 16px;
}
QTabBar#mrListTabs::tab:hover { background: @raise2@; color: @text@; }
QTabBar#mrListTabs::tab:selected {
	background: @tabBar@; color: @accentText@; border-color: @tabBar@;
}

/* ── settings dialog: side menu + pages ─────────────────────────────── */
QListWidget#mrSettingsNav {
	background: @sink1@; color: @textMuted@;
	border: 0; border-right: 1px solid @border@;
	outline: 0; font-size: 11px;
}
QListWidget#mrSettingsNav::item { padding: 8px 12px; border: 0; }
QListWidget#mrSettingsNav::item:hover { background: @raise2@; color: @text@; }
QListWidget#mrSettingsNav::item:selected {
	background: @accent@; color: @accentText@;
}
QLabel#mrSettingsTitle {
	color: @text@; font-size: 14px; font-weight: 700;
}
QLabel#mrSettingsBlurb { color: @textMuted@; font-size: 10px; }

/* The two numbers an operator opens Settings to read before kick-off: how much
   disk is left, and how much recording that is. A card each — as one line of
   text the second number had no caption at all, and "09:12" with no word in
   front of it reads as a clock. */
QFrame#mrStatCard {
	background: @raise1@; border: 1px solid @border@; border-radius: 3px;
}
QLabel#mrStatCaption {
	color: @textMuted@; font-size: 10px; font-weight: 600;
	letter-spacing: 0.6px; text-transform: uppercase;
}
QLabel#mrStatValue { color: @text@; font-size: 19px; font-weight: 700; }
QLabel#mrStatUnit { color: @textMuted@; font-size: 11px; padding-bottom: 3px; }

/* ── multiview tiles ────────────────────────────────────────────────
   The caption band IS the tally: green = the angle being watched, red = the
   angle on air, so the operator reads it off the picture instead of
   correlating it with the angle keys. Signal, therefore not the theme's. */
/* NO BACKGROUND OF ITS OWN. A tile keeps the canvas ratio, so whenever the
   block is not exactly that shape the difference shows as a band above and
   below the picture - and painted black here that band was BLACK while the
   same band on A and B, which carry no such rule, was the panel grey: two
   letterboxes of two colours in one row of monitors. The picture itself is an
   OBS display and black by nature; the band around it is panel. */
/* QUIET UNTIL IT HAS SOMETHING TO SAY, and the same band A and B wear. It was
   filled with the theme's accent, so on an eight-camera rig six blue bars
   shouted from the corner of the panel about nothing at all — and the two that
   were carrying a tally could not be picked out of them. */
QLabel#mrTileCap {
	background: @raise1@; color: @textMuted@;
	font-size: 9px; font-weight: 700; padding: 0px 4px;
}
QLabel#mrTileCap[tally="pvw"] { background: @pvw@; color: #ffffff; }
QLabel#mrTileCap[tally="pgm"] { background: @rec@; color: #ffffff; }
QLabel#mrTileCap[tally="replay"] { background: @warnBg@; color: @warn@; }

/* ── channel strip under the preview ─────────────────────── */
QLabel#mrChanBadge {
	background: @onAirDim@; color: #ffffff;
	font-weight: 700; font-size: 11px; padding: 2px 7px;
}
/* The letter under each output box: A on a green bar, B on a blue one, and that
   colour is how the operator tells the two boxes apart from across the room —
   faster than reading a letter. The one being driven by the keys is the bright
   one.

   THE BAND IS THINNER THAN IT WAS. It is a tally, not a caption: it has to be
   findable in the corner of the eye and read in one glance, and neither of
   those improves past a few pixels. 10 px of coloured band under a picture is
   10 px the event list does not get, on every box, on every rig. */
QLabel#mrChanTag {
	background: @raise1@; color: @textMuted@;
	font-weight: 700; font-size: 9px; padding: 0px 0px;
}
QLabel#mrChanTag[chan="A"][active="true"] { background: @onAirDim@; color: #ffffff; }
QLabel#mrChanTag[chan="B"][active="true"] { background: @accent@; color: @accentText@; }
/* ONE LINE, not three. It used to stack list / clip / remaining, then id and
   the two offsets, then timecode and speed — 44 px under the pictures, most of
   which the on-air band and the position bar were already saying. What is left
   here is what is said NOWHERE else: which list, how far the playhead is past
   IN and short of OUT, and whatever showNotice() has to tell the operator about
   the key he just pressed.

   AND IT IS NO LONGER GREEN. It was a full-width green band directly above the
   on-air band, which is also a full-width green band — so the panel had two of
   them, one saying what is playing and one saying what the playhead is near,
   and telling them apart meant reading both. Green is reserved for on air. This
   is a reading, so it is drawn like one; the BADGE keeps its colour, because
   that is the channel's identity and it matches the tally under the picture. */
QLabel#mrChanStrip {
	background: @sink1@; color: @textMuted@;
	font-size: 10px; padding: 1px 7px;
}
/* A notice owns the line for a few seconds — it is the answer to a key the
   operator just pressed, so it is allowed to be brighter than the reading it
   replaces. */
QLabel#mrChanStrip[notice="true"] { color: @warn@; font-weight: 700; }

)QSS"
/* MSVC caps a single string literal at 16380 bytes, so the sheet is written in
   chunks and the compiler concatenates them. Adding to it means watching for
   C2026 and breaking here rather than making the last chunk longer. */
R"QSS(
/* ── THE STATUS LINE ────────────────────────────────────────────────
   One row, and it OWNS the modes rather than mirroring them: Loop, music, "in
   output" and the return to the live edge are buttons here and nowhere else.
   That is the whole reason it can exist at all — a status bar that repeated
   four toggles which are also keys in the strip would be four states with two
   homes, which is exactly how a toggle ends up left in the wrong position.
   Beside them it carries the numbers about the take that are pure readings:
   how long it has been running, what the health monitor found, what speed the
   next replay will run at. */
QWidget#mrStatusBar { background: @sink1@; border-top: 1px solid @border@; }
QLabel#mrStatusText  { color: @textMuted@; font-size: 10px; }
QLabel#mrStatusValue { color: @text@; font-size: 10px; font-weight: 700;
                       letter-spacing: 0.3px; }
QLabel#mrStatusValue[rec="true"] { color: @rec@; }
/* A vertical hairline between two groups of the status line, and between two
   sections of the control strip. It is what replaced the six captions: a rule
   costs one pixel of width and says the same thing a heading said in a whole
   line of height. */
QWidget#mrStatSep { background: @border@; }
/* The toggles that live on this line. Shorter than a key in the strip because
   the line is shorter; still a STATE when lit — hollow, not filled. */
QPushButton#mrStatKey {
	background: transparent; color: @textMuted@;
	border: 1px solid transparent; border-radius: 3px;
	font-size: 10px; font-weight: 700; letter-spacing: 0.4px;
	/* NO VERTICAL PADDING, and two pixels of headroom is the point. The key
	   is pinned to 18 px by the layout; with 1 px of padding top and bottom
	   plus a 1 px border the style asks for exactly 18 as well, and EXACTLY
	   is not a margin - any rounding, at any display scale, puts the frame a
	   pixel past the widget and the bottom border outside it. Invisible while
	   the border is transparent; lit, it is a box somebody forgot to close,
	   which is why of three identical keys only "in output" was reported. */
	min-height: 0px; padding: 0px 7px;
}
QPushButton#mrStatKey:hover { color: @text@; border-color: @borderHi@; }
QPushButton#mrStatKey:checked {
	background: @pvwBg@; color: @pvw@; border-color: @pvw@;
}
QPushButton#mrStatKey:checked:hover { background: @pvwBg@; color: @pvw@; }
/* NOW is not a mode and is not drawn as one: it is where the operator goes to
   get out of a replay, so it keeps the red it has at rest in the strip. */
QPushButton#mrStatKey[now="true"] {
	color: @rec@; border-color: @recBg@;
}
QPushButton#mrStatKey[now="true"]:hover { background: @recBg@; border-color: @rec@; }

/* REC — one of the two keys allowed to be FILLED, and only once it is armed.
   At rest it is a command like any other, in the danger colour. */
QPushButton#mrRec {
	font-weight: 700; font-size: 12px; letter-spacing: 0.6px;
	border-radius: 4px; min-height: 18px; padding: 3px 14px;
}
QPushButton#mrRec[recording="false"] {
	background: @raise1@; color: @rec@; border: 1px solid @recBg@;
}
QPushButton#mrRec[recording="false"]:hover { background: @recBg@; border-color: @rec@; }
QPushButton#mrRec[recording="true"] {
	background: @rec@; color: #ffffff; border: 1px solid @rec@;
}
QPushButton#mrRec[recording="true"]:hover { background: @rec@; }

/* Channel selector A|B / A / B and the swap. Small, square and always visible:
   it is the answer to "where is this key going", and an operator who has to
   look for it has already pressed something on the wrong channel. */
QPushButton#mrChanSel {
	background: @raise1@; color: @textKey@; border: 1px solid @border@;
	border-radius: 3px; padding: 2px 6px; font-weight: 700; font-size: 11px;
	min-height: 20px; /* + 2px padding + 2px border = 26 */
}
QPushButton#mrChanSel:hover { background: @raise2@; color: @text@; }
QPushButton#mrChanSel:checked {
	background: @accent@; color: @accentText@; border-color: @accent@;
}

/* M4 health badge: amber = degraded, red = this take is not usable. It sits
   beside REC and is hidden entirely while there is nothing to report. */
QPushButton#mrHealth {
	font-weight: 700; font-size: 12px; border-radius: 4px;
	min-height: 18px; padding: 3px 8px;
}
QPushButton#mrHealth[level="warn"] {
	background: @warnBg@; color: @warn@; border: 1px solid @warn@;
}
QPushButton#mrHealth[level="bad"] {
	background: @recBg@; color: @rec@; border: 1px solid @rec@;
}
/* ON THE STATUS LINE, where the row is shorter than a key. A style sheet
   min-height LARGER than the widget's own height makes the style draw a taller
   frame than the widget owns and the BOTTOM BORDER lands outside it — which on
   a badge with a border all the way round reads as a box someone forgot to
   close. Same trap, same fix, as #mrSkip on the on-air band. */
QPushButton#mrHealth[dense="true"] {
	min-height: 0px; padding: 0px 6px; font-size: 10px;
}

/* settings gear and the two ⋯ / ▾ menu keys.
   READABLE. They were drawn at #484848 on #181818 — the faintest things on the
   panel — and behind them are Stop, Play-to-output, Duplicate and Delete. A
   menu nobody can see is a menu nobody opens. */
QToolButton#mrGear {
	background: @raise1@; border: 1px solid @border@; border-radius: 4px;
	padding: 3px 7px; color: @textKey@; font-size: 14px;
}
QToolButton#mrGear:hover { background: @raise2@; color: @text@; border-color: @borderHi@; }

/* ── angle selector — STATE drives colour, not :checked ───── */
QPushButton#mrAngle {
	background: @raise1@; border: 1px solid @border@; border-radius: 3px;
	color: @textKey@; font-weight: 700; font-size: 10px;
	min-width: 34px; min-height: 24px; padding: 0 4px;
}
QPushButton#mrAngle:hover { background: @raise2@; color: @text@; border-color: @borderHi@; }
/* AN EMPTY SLOT IS DRAWN, NOT LEFT AS A HOLE. The matrix reserves the places up
   to the last configured camera so that angle 5 is at the same x on every rig;
   reserving them by leaving nothing there made the section read as a gap
   somebody forgot to fill. A sunken outline says "a camera could go here" and
   costs no attention: it is darker than the panel, and it has nothing to read. */
QPushButton#mrAngleSlot {
	background: @sink1@; border: 1px solid @panel@; border-radius: 3px;
	color: transparent; min-width: 34px; min-height: 24px; padding: 0 4px;
}
QPushButton#mrAngle[state="preview"] {
	background: @pvwBg@; border-color: @pvw@; color: @pvw@;
}
QPushButton#mrAngle[state="preview"]:hover { background: @pvwBg@; border-color: @pvw@; }
QPushButton#mrAngle[state="program"] {
	background: @recBg@; border-color: @rec@; color: @rec@;
}
QPushButton#mrAngle[state="program"]:hover { background: @recBg@; border-color: @rec@; }

/* ── speed preset chips ─────────────────────────────────── */
QPushButton#mrSpeedChip {
	background: @raise1@; border: 1px solid @border@; border-radius: 3px;
	color: @textKey@; font-size: 9px; font-weight: 700;
	min-width: 28px; min-height: 22px; padding: 1px 4px;
}
QPushButton#mrSpeedChip:hover { background: @raise2@; color: @text@; border-color: @borderHi@; }
QPushButton#mrSpeedChip:pressed { background: @sink1@; }
/* The chip that matches the current speed is a STATE: lit, hollow. */
QPushButton#mrSpeedChip[active="true"] {
	background: @pvwBg@; border-color: @pvw@; color: @pvw@;
}

/* ── section separator line ─────────────────────────────── */
QWidget#mrSepLine { background: @border@; }

/* ── ACTION: the one filled key in the panel ──────────────────
   "Play the selected events" is the action the operator reaches for more than
   any other, and it was an outlined key with a green tint — the same weight as
   Mark, as Export, as ⋯, so the eye had to READ the row to find it. Filled, it
   is found without reading, and nothing else on the panel is filled except REC
   when armed and the on-air band. An action and a state look different here on
   purpose: this one asks to be pressed, those two report. */
QPushButton#mrAccent {
	background: @action@; border: 1px solid @action@; color: #ffffff;
	font-weight: 700;
}
QPushButton#mrAccent:hover { background: @actionHi@; border-color: @actionHi@; }
QPushButton#mrAccent:pressed { background: @action@; }
QPushButton#mrAccent:disabled {
	background: @raise1@; border-color: @border@; color: @textDim@;
}
/* Annulla. A BORDER, not just red text: with the panel's own background and a
   border a shade off it, the one destructive key on the mark row was drawn as a
   hyperlink. */
QPushButton#mrDanger { color: @danger@; border-color: @recBg@; }
QPushButton#mrDanger:hover { background: @recBg@; border-color: @danger@; color: @danger@; }

/* ── checkboxes ──────────────────────────────────────────── */
#MultiReplayDock QCheckBox { color: @textMuted@; spacing: 5px; font-size: 11px; }
#MultiReplayDock QCheckBox::indicator {
	width: 13px; height: 13px; border-radius: 3px;
	border: 1px solid @border@; background: @raise1@;
}
#MultiReplayDock QCheckBox::indicator:checked { background: @pvw@; border-color: @pvw@; }

)QSS"
R"QSS(
/* ── inputs ──────────────────────────────────────────────── */
#MultiReplayDock QComboBox, #MultiReplayDock QLineEdit {
	background: @raise1@; color: @text@;
	border: 1px solid @border@; border-radius: 3px;
	padding: 3px 7px; min-height: 18px; font-size: 11px;
}
#MultiReplayDock QComboBox:hover, #MultiReplayDock QLineEdit:hover { border-color: @borderHi@; }
#MultiReplayDock QComboBox::drop-down { border: 0; width: 16px; }
/* AN EDITABLE COMBO IS A COMBO WITH A LINE EDIT INSIDE IT, and the rule above
   matches BOTH. So the per-angle speed and tag cells were paying for the box
   twice: the combo's own 3px/7px padding, its border and its min-height, and
   then the child line edit's again inside them. The text ended up pushed down
   and right of the centre it had been told to sit in — which is what "the tag
   and the custom percentages are not vertically centred" looks like. The child
   contributes nothing of its own; the combo around it already draws the frame. */
#MultiReplayDock QComboBox QLineEdit {
	background: transparent; border: 0; padding: 0; min-height: 0;
}
/* "--" in an angle cell: no per-angle speed, the slider decides. Muted, because
   it is an absence and not a choice. It lives here rather than in a
   setStyleSheet() on the combo itself: a style sheet set on one widget makes Qt
   build a separate style context for it and re-polish its subtree, and that was
   a measurable part of a table rebuild that took over a tenth of a second. */
#MultiReplayDock QComboBox[mrNoOverride="true"],
#MultiReplayDock QComboBox[mrNoOverride="true"] QLineEdit {
	color: @textMuted@;
}
#MultiReplayDock QComboBox QAbstractItemView {
	background: @raise1@; color: @text@; border: 1px solid @border@;
	selection-background-color: @accent@; selection-color: @accentText@; outline: 0;
}
/* The rows of an open list. Without an explicit item rule the view inherits the
   padding of the CLOSED box above (3px 7px plus its min-height), which leaves
   each row taller than its text and the text sitting at the bottom of it —
   which is exactly how a centred list stops looking centred. The height is
   stated here and the horizontal centring is set per item, in code, because
   Qt draws item text through the delegate and no stylesheet reaches it. */
#MultiReplayDock QComboBox QAbstractItemView::item {
	min-height: 18px; padding: 0px; border: 0;
}

/* ── HOW TALL A ROW OF THE EVENT LIST IS ───────────────────────────
   The row is sized from the CELL the table actually built (see refreshEvents),
   and an angle cell is two combo boxes — so the row height is decided here,
   not by a number next to setDefaultSectionSize. Those two used to be set
   independently and drifted apart, which is what "the text looks cut" was.
   Scoped to #mrEvents so the Settings dialog keeps inputs at a size a mouse
   can hit: a dense LIST is a reading surface, a dense FORM is a hazard. */
/* ...AND IN THE TABLE THEY ARE DRAWN AS TEXT, not as controls.

   A framed combo with a drop-down arrow is three chrome elements around one
   word: the frame and its padding set the row height for the whole list, the
   arrow takes width the comment needed, and the type had to shrink to fit
   inside what was left. Sixty rows of that is a list that is harder to read
   than the same words plainly written.

   So in this table a combo has no frame, no arrow and no padding of its own -
   it looks like the cell's text and behaves exactly as before: one click opens
   the list, typing goes straight into the comment. The frame appears on hover
   and focus, which is where it says something ("this is editable") instead of
   saying it sixty times at once. */
QTableWidget#mrEvents QComboBox, QTableWidget#mrEvents QLineEdit {
	background: transparent; border: 1px solid transparent;
	min-height: @cellInput@px; padding: 0px 4px;
	font-size: @cellFont@px;
}
QTableWidget#mrEvents QComboBox::drop-down { width: 0px; border: 0; }
QTableWidget#mrEvents QComboBox:hover, QTableWidget#mrEvents QLineEdit:hover {
	background: @raise1@; border-color: @borderHi@;
}
QTableWidget#mrEvents QComboBox:focus, QTableWidget#mrEvents QLineEdit:focus {
	background: @raise1@; border-color: @accent@;
}
/* THE COMMENT CELL. Text at rest and a chooser beside it, and both take the
   FIRST click - which is not a nicety here: a double click on a row of this
   table means "put this event on air", so an editor that opened on one could
   reach the Program from a cell whose whole job is a word.

   The frame belongs to the CELL and appears when the pointer is over it, so a
   list of sixty comments is sixty words rather than sixty boxes. */
QTableWidget#mrEvents QWidget#mrNoteCell { background: transparent; }
QTableWidget#mrEvents QWidget#mrNoteCell:hover {
	background: @raise1@;
}
QTableWidget#mrEvents QLineEdit#mrAngleNote {
	background: transparent; border: 0; padding: 0px 2px;
	min-height: @cellInput@px; font-size: @cellFont@px;
	color: @text@;
}
QTableWidget#mrEvents QPushButton#mrNotePick {
	background: transparent; border: 0; padding: 0;
	min-height: @cellInput@px;
}
QTableWidget#mrEvents QPushButton#mrNotePick:hover {
	background: @raise2@; border-radius: 2px;
}

/* THE SPEED IS A LABEL, which is what it was before it became a drop-down and
   then a chip - and the operator has now asked for it back twice over.

   A value in a list of values should be drawn the way the id, the in-point and
   the duration beside it are drawn: as text. A frame around it says "this is a
   control", which is true of every cell in this table and therefore worth
   saying on none of them; what it costs is the row height for the whole list.

   What is NOT given up is the gesture: one click on the text opens the list.
   Grey where the slider decides, the panel's ordinary text where the angle
   overrides it. The only chrome is a tint under the pointer, which says
   "this one" at the moment the question is being asked. */
QTableWidget#mrEvents QPushButton#mrAngleSpeed {
	background: transparent; border: 1px solid transparent;
	color: @text@; padding: 0px 2px;
	min-height: @cellInput@px; font-size: @cellFont@px;
	text-align: center;
}
QTableWidget#mrEvents QPushButton#mrAngleSpeed:hover {
	background: @raise1@; border-color: @borderHi@;
}
QTableWidget#mrEvents QPushButton#mrAngleSpeed[mrNoOverride="true"] {
	color: @textDim@;
}

QTableWidget#mrEvents QComboBox QAbstractItemView::item {
	min-height: @cellInput@px;
}

/* ── zones: a caption over a hairline, not a box ───────
   Quiet on purpose. Five bordered groups drew five frames competing for
   attention with the green band and the REC key; a rule under a title says
   "this group" just as well and disappears when it is not being looked for. */
QFrame#mrZone {
	border: 0; border-top: 1px solid @border@; background: transparent;
}
QLabel#mrZoneTitle {
	color: @textMuted@; font-size: 9px; font-weight: 700; letter-spacing: 1.1px;
	padding: 1px 0px 0px 1px; margin: 0px;
}
/* FOLDED: the same caption in a narrow column, where there is one per group
   down the panel instead of one per group across it. Smaller type, tighter
   tracking - it still names the group, it just stops taking a full line of
   text to do it. Stated HERE and not on the widget: a style-sheet font-size
   beats a widget font, so a font set in code would simply be ignored. */
QLabel#mrZoneTitle[folded="true"] {
	font-size: 8px; letter-spacing: 0.8px;
}

/* ── speed slider ────────────────────────────────────────── */
QSlider#mrSpeed::groove:horizontal {
	height: 3px; background: @raise2@; border-radius: 2px;
}
QSlider#mrSpeed::sub-page:horizontal { background: @seekBar@; border-radius: 2px; }
QSlider#mrSpeed::handle:horizontal {
	width: 10px; height: 10px; margin: -4px 0;
	background: @text@; border-radius: 5px; border: 1px solid @border@;
}
QSlider#mrSpeed::handle:horizontal:hover { background: @accentText@; }

/* ── event table ───────────────────────────────────────────── */
QTableWidget#mrEvents {
	background: @sink2@; alternate-background-color: @sinkAlt@;
	gridline-color: transparent; border: 1px solid @border@;
	border-radius: 0; color: @text@; outline: 0;
}
QTableWidget#mrEvents::item { padding: 2px 5px; border: 0; }
QTableWidget#mrEvents::item:selected { background: @rowSel@; color: @rowSelText@; }
/* The per-angle enable box, which in the reference controller IS the cell */
QTableWidget#mrEvents::indicator {
	width: 11px; height: 11px;
	border: 1px solid @textMuted@; background: @sink2@;
}
QTableWidget#mrEvents::indicator:checked {
	background: @pvw@; border-color: @text@;
}
/* The section BACKGROUND is not ours to set: OBS's own theme styles
   QHeaderView::section (Yami.obt: background-color: var(--button_bg)) and wins
   whatever we put here — measured on screen, #272A33 either way. That is why
   the "angle I am watching" cue is a ▶ in the header TEXT (see
   updateCamHeaderHighlight) and not a green fill: a colour we cannot guarantee
   is worse than a glyph we can. */
#MultiReplayDock QHeaderView::section {
	background: @raise1@; color: @text@; padding: 3px 5px;
	border: 0; border-bottom: 1px solid @border@;
	font-size: @headerFont@px; font-weight: 700; letter-spacing: 0.6px;
}
#MultiReplayDock QHeaderView { background: @panel@; }
#MultiReplayDock QTableCornerButton::section {
	background: @raise1@; border: 0;
}

/* ── scrollbars ──────────────────────────────────────────── */
#MultiReplayDock QScrollBar:vertical {
	background: transparent; width: 6px; margin: 0;
}
#MultiReplayDock QScrollBar::handle:vertical {
	background: @border@; border-radius: 3px; min-height: 20px;
}
#MultiReplayDock QScrollBar::handle:vertical:hover { background: @borderHi@; }
#MultiReplayDock QScrollBar::add-line, #MultiReplayDock QScrollBar::sub-line {
	height: 0; width: 0;
}
#MultiReplayDock QScrollBar:horizontal {
	background: transparent; height: 6px; margin: 0;
}
#MultiReplayDock QScrollBar::handle:horizontal {
	background: @border@; border-radius: 3px; min-width: 20px;
}
#MultiReplayDock QScrollBar::handle:horizontal:hover { background: @borderHi@; }

#MultiReplayDock QSplitter { background: @panel@; }
#MultiReplayDock QSplitter::handle:vertical {
	background: @raise1@; height: 5px;
}
#MultiReplayDock QSplitter::handle:vertical:hover { background: @raise2@; }
#MultiReplayDock QSplitter::handle:horizontal { background: @raise1@; width: 5px; }
#MultiReplayDock QSplitter::handle:horizontal:hover { background: @raise2@; }

)QSS"
			   R"QSS(
/* ── WHAT OBS WOULD OTHERWISE PAINT FOR US ─────────────────

   A widget kind this panel uses and does NOT have a rule for is not unstyled:
   it is styled by OBS, whose sheet is the whole application's and whose colours
   are its theme's. While the panel and OBS are both dark that is invisible and
   even convenient; the day the panel is LIGHT and OBS is not, every one of them
   is a dark patch in a white panel — a spin box in Settings, the pane behind a
   tab, the well of a list.

   THE MOCKUP CANNOT SHOW THIS. It has no application style sheet at all, so it
   renders these correctly and always has. Same class as the sections it simply
   does not build: the tool lies by omission, and only the panel can be asked.

   Every rule below is scoped by id, so ours out-ranks a bare type selector
   wherever the panel is the ancestor — the settings dialog included, a QDialog
   parented to the dock being a descendant in the object tree. */
#MultiReplayDock QSpinBox, #MultiReplayDock QDoubleSpinBox,
#MultiReplayDock QPlainTextEdit, #MultiReplayDock QTextEdit {
	background: @sink2@; color: @text@;
	border: 1px solid @border@; border-radius: 3px;
	padding: 3px 6px; min-height: 18px;
}
#MultiReplayDock QSpinBox:focus, #MultiReplayDock QDoubleSpinBox:focus,
#MultiReplayDock QPlainTextEdit:focus, #MultiReplayDock QTextEdit:focus {
	border-color: @accent@;
}
#MultiReplayDock QCheckBox, #MultiReplayDock QRadioButton,
#MultiReplayDock QGroupBox {
	color: @text@; background: transparent;
}
#MultiReplayDock QTabWidget::pane {
	background: @panel@; border: 1px solid @border@;
}
#MultiReplayDock QListView, #MultiReplayDock QTreeView,
#MultiReplayDock QAbstractItemView {
	background: @sink2@; color: @text@;
	border: 1px solid @border@;
	selection-background-color: @rowSel@;
	selection-color: @rowSelText@;
}

/* A KEY WHOSE HEIGHT BELONGS TO THE LAYOUT.
   Every key inside a section of the control strip is pinned by KeyBlock::apply
   to the height of the rows it spans - 26 px in the wide shape, 22 in the
   folded one. A min-height here is a CONTENT box with the padding and the
   border added on top of it, so the rules above all state 26 px worth; asked
   for 26 on a widget that owns 22, the style draws the frame past the bottom
   edge and the underside of the key is cut off. That is the whole of "In, Out
   and the two trims have no bottom border" in a column.

   So the pinned keys stand their min-height down and let the layout's number be
   the only one. Nothing else changes: a fixed height already overrides the
   hint, and min-height never touched the width.

   NOT last in the sheet - the focus ring is, and has to be. */
#MultiReplayDock QPushButton[mrPinned="true"] { min-height: 0px; }

/* ── WHERE THE KEYBOARD IS ──────────────────────────────────────────
   LAST IN THE SHEET, and that is the whole point of where it sits: Qt follows
   CSS specificity, and every role's own :hover and :checked rule scores the
   same as this one — so anywhere earlier and a lit toggle or a hovered key
   would quietly drop the focus ring, which is exactly when an operator is most
   likely to want it.

   This panel has a whole keyboard layer over it: arrows step frames, Enter
   plays, +/- move the speed. The focus can be on a key, on the table or in the
   search box, and the same keypress does different things in each — so which
   one has it is not decoration.

   The ACCENT, not a signal colour: having focus is a state of the panel, not a
   state of the take, and red and green already mean something here. */
#MultiReplayDock QAbstractButton:focus { border-color: @accent@; }
#MultiReplayDock QLineEdit:focus, #MultiReplayDock QComboBox:focus {
	border-color: @accent@;
}
)QSS";

// How tight the event list is drawn. Three steps rather than a free number:
// each one is a set of metrics that agree with each other, and a spin box of
// pixels would let an operator pick a row shorter than the text in it.
struct Density {
	int cellInput; // a table combo's content height
	int cellPad;   // its vertical padding
	int cellFont;  // and its type size
	int headerH;   // the column headings above it, at least
	int headerFont; // ...and the type they are drawn in, which decides it
	int rowFloor;  // where the row height starts before the cells raise it
};

inline Density densityFor(int level)
{
	switch (level) {
	case 1: // compact — about a fifth off, still comfortably clickable
		return {14, 2, 10, 18, 9, 22};
	case 2: // dense — for an operator working from the list on a big screen
		return {10, 1, 9, 15, 9, 18};
	default: // comfortable, and the historic 30 px row
		return {20, 3, 11, 22, 10, 28};
	}
}

// The sheet with this scheme's colours and this density's metrics in it.
inline QString dockStyle(const Scheme &s, int densityLevel = 0)
{
	const Density d = densityFor(densityLevel);
	QString out = QString::fromUtf8(kDockStyleTemplate);
	out.replace(QLatin1String("@cellInput@"), QString::number(d.cellInput));
	out.replace(QLatin1String("@cellPad@"), QString::number(d.cellPad));
	out.replace(QLatin1String("@headerFont@"), QString::number(d.headerFont));
	out.replace(QLatin1String("@cellFont@"), QString::number(d.cellFont));
	const std::pair<const char *, const QString *> tokens[] = {
		{"@panel@", &s.panel},         {"@raise1@", &s.raise1},
		{"@raise2@", &s.raise2},       {"@sink1@", &s.sink1},
		{"@sink2@", &s.sink2},         {"@sinkAlt@", &s.sinkAlt},
		{"@borderHi@", &s.borderHi},   {"@border@", &s.border},
		{"@textMuted@", &s.textMuted}, {"@textDim@", &s.textDim},
		{"@textKey@", &s.textKey},
		{"@text@", &s.text},           {"@accentText@", &s.accentText},
		{"@accent@", &s.accent},       {"@rowSelText@", &s.rowSelText},
		{"@rowSel@", &s.rowSel},       {"@recBg@", &s.recBg},
		{"@rec@", &s.rec},             {"@pvwBg@", &s.pvwBg},
		{"@pvw@", &s.pvw},             {"@onAirDim@", &s.onAirDim},
		{"@onAir@", &s.onAir},         {"@warnBg@", &s.warnBg},
		{"@warn@", &s.warn},           {"@danger@", &s.danger},
		{"@actionHi@", &s.actionHi},   {"@action@", &s.action},
		{"@tabBar@", &s.tabBar},       {"@seekBar@", &s.seekBar},
	};
	// LONGEST PREFIX FIRST, which is why @borderHi@ is listed above @border@
	// and @text@ below @textMuted@: these are delimited by @ at both ends, so
	// the order only matters if a token is a prefix of another AND the closing
	// @ is forgotten. Keeping the order right anyway costs nothing and means
	// the next token added cannot introduce a silent mis-substitution.
	for (const auto &[name, value] : tokens)
		out.replace(QLatin1String(name), *value);
	return out;
}

} // namespace multireplay
