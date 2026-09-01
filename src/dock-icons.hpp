// dock-icons.hpp — the panel's icons, DRAWN rather than shipped.
//
// WHY THEY ARE PAINTED AND NOT FILES. The panel needs about two dozen marks —
// play, stop, reverse, the two frame steps, mark in and out, trim, export. Every
// ordinary way of getting them costs something this project does not want to
// pay:
//
//   an icon FONT      one more binary blob in a GPL repo, with its own licence
//                     and its own hinting, drawn at whatever weight the family
//                     happens to have.
//   SVG files         needs the QtSvg module, which this plugin does not link,
//                     plus a .qrc and a build step for it.
//   QStyle pixmaps    follow the PLATFORM icon theme, so they come out dark on
//                     dark inside OBS's dark palette. This is already written
//                     down in multireplay-dock.cpp, next to the reason the
//                     transport keys were Unicode glyphs in the first place.
//   Unicode glyphs    what the panel used until now. Two of them (U+23EE,
//                     U+23ED) carry Emoji_Presentation=Yes, so Windows drew the
//                     two least consequential keys in the transport row in
//                     bright blue Segoe UI Emoji — the whole reason
//                     useTextGlyph() exists. And a glyph's weight, size and
//                     baseline are the font's opinion, not ours: ▶ and ■ do not
//                     come out the same optical size in any family.
//
// So they are QPainterPaths on a 24×24 design grid, stroked and filled by us.
// That buys four things at once: no dependency, one optical weight across every
// mark, a colour that comes from the panel's own Scheme (so the icons follow the
// OBS theme like everything else), and crispness at any DPI because the pixmap
// is rendered at the widget's devicePixelRatio rather than scaled up from 16 px.
//
// THE STATES MATTER AS MUCH AS THE SHAPES. A QSS rule can recolour a key's
// background when it is hovered, checked or disabled; it cannot recolour a
// pixmap. So every icon is built as a QIcon carrying all four of the modes Qt
// asks for, plus the On state a checkable key uses — otherwise a lit toggle is a
// bright key with a dim mark on it, which reads as disabled.
//
// NOTHING OBS-SPECIFIC IS IN HERE, on purpose: like dock-layout and dock-style,
// this file is driven by tools/dock-mockup, which is where an icon set is judged
// — twenty-two marks at 16 px are looked at together or not at all.
#pragma once

#include "dock-style.hpp"

#include <QColor>
#include <QIcon>
#include <QString>

class QAbstractButton;

namespace multireplay {

// ---------------------------------------------------------------------------
// Icon — the marks, named after what they DO, not what they look like
// ---------------------------------------------------------------------------
//
// Named for the action so the call site reads as the panel does. `MarkIn` and
// `TrimIn` are drawn from the same bracket and would be one name if this list
// were about shapes; they are two commands, so they are two entries.
enum class Icon {
	// transport
	Play,
	Pause,
	// ▶ and ⏸ IN ONE MARK, for the key that is both. The transport play key
	// is a play at rest and a pause while a clip runs, and drawing it as one
	// or the other made it look like two different keys depending on when you
	// glanced at it.
	Stop,
	Reverse,
	PlayLast, // ↺ replay the last event
	StepBack,
	StepFwd,
	SkipNext, // ≫ drop this clip, take the next of the queue
	Now,      // back to the live edge
	// marking
	MarkIn,
	MarkOut,
	TrimIn,
	TrimOut,
	Cancel,
	// list / clips
	MoveUp,
	MoveDown,
	More,       // ⋯ the actions that have no key of their own
	ExportClip,
	ExportReel,
	// bays
	Swap,
	// panel
	// THE RECORD DOT IS NOT THE LIVE DOT. They were the same mark for a
	// while and they are not the same thing: Live is a MODE the panel is in
	// (marks land at the front), REC arms a take. A bare filled circle is
	// what every deck ever built has used for record; the ring belongs to
	// the tally.
	Rec,
	Gear,
	Search,
	FullScreen,
	Monitors,
	Live,
	Loop,
	Music,
	Mute, // speaker with a slash — the replay audio sits muted in the mixer
	ToOutput,
	Zoom,
	Health,
	Menu, // ▾ the options beside a key
};

// The icon, tinted for one colour, at one logical size.
//
// `px` is the LOGICAL size (the size the layout reasons in); the pixmap inside
// is rendered at `dpr` times that, which is what keeps a 16 px mark sharp on a
// 150% display. Results are cached: a key rebuilt on every theme change would
// otherwise re-render its mark on every call, and poll() touches these.
QIcon iconFor(Icon id, const QColor &tint, int px, qreal dpr = 1.0);

// ---------------------------------------------------------------------------
// The four colours an icon is drawn in, and why a key needs all four
// ---------------------------------------------------------------------------
//
// `rest`      the key sitting there            → QIcon::Normal, state Off
// `hover`     the pointer is on it             → QIcon::Active
// `disabled`  it cannot be pressed             → QIcon::Disabled
// `on`        a checkable key that is LIT      → state On
//
// Stated as a struct rather than four arguments because every call site wants
// the same four, and they come from the Scheme in one place (see iconColours in
// dock-style.hpp).
struct IconTints {
	QColor rest;
	QColor hover;
	QColor disabled;
	QColor on;
	// ...AND THE SIGNAL INKS, for the keys whose mark is not chrome. See
	// IconRole below: these are here rather than at the call sites so that a
	// theme change re-derives them, which a QColor stamped on a widget when
	// it was built cannot do.
	QColor rec;    // the mark IS the take
	QColor warn;   // the health badge, degraded
};

// ---------------------------------------------------------------------------
// IconRole — WHICH INK A MARK IS DRAWN IN
// ---------------------------------------------------------------------------
//
// The four tints above are CHROME, and chrome is right for the keys that are
// chrome. It is wrong, and was wrong on five keys at once, for a key that is
// itself a signal:
//
//   the filled green play key wore a dark grey ▶ (the label rule beside it
//   says #ffffff, but a style sheet cannot reach a pixmap);
//   ≫ on the on-air band was a grey mark on a green fill;
//   REC showed a GREY dot next to the word REC written in red;
//   Annulla, the one destructive key on the mark row, was drawn as neutral as
//   the two keys beside it;
//   and the health badge's mark stayed grey while its border and its number
//   went amber.
//
// So a key states what its mark MEANS and the ink is looked up from the scheme
// — never written down at the call site, or a theme change would leave it the
// colour it was born in. Stamped as a dynamic property so `restyleIcons` reads
// it back on every rebuild.
enum class IconRole {
	Chrome = 0, // the default: a key at rest is grey
	LitWhite,   // white ONCE LIT — a red LIVE key wants a white mark, not
	            // the panel's green: two signals arguing in one control
	OnSignal,   // white ALWAYS — the key is a filled signal colour
	Rec,        // red at rest: the mark is the take
	Danger,     // red: this key destroys something
	Warn,       // amber: the badge that says the take is degraded
};

inline const char *kIconRoleProperty = "mrIconRole";

// The four colours, taken from the panel's own Scheme. One place, so a mark and
// the label beside it cannot end up different greys — which is precisely what
// happened while the marks were glyphs painted by a font and the labels were
// painted by the style sheet.
inline IconTints tintsFor(const Scheme &s)
{
	// `on` is @pvw@ because that is what the style sheet lights a checked
	// key with (see QPushButton#mrToggle:checked and #mrAngle[state=…]).
	// A mark left at the resting grey on a lit key reads as disabled.
	return IconTints{QColor(s.textKey), QColor(s.text), QColor(s.textDim),
			 QColor(s.pvw),      QColor(s.rec),  QColor(s.warn)};
}

// The four states, for one role. Kept beside tintsFor so the mapping is read
// next to the colours it maps, and shared by setKeyIcon and the mockup.
inline IconTints tintsForRole(const IconTints &t, IconRole role)
{
	IconTints r = t;
	switch (role) {
	case IconRole::LitWhite:
		r.on = QColor(Qt::white);
		break;
	case IconRole::OnSignal:
		// The key is a solid signal colour and its label is #ffffff; the
		// mark is the other half of the same label.
		r.rest = r.hover = r.on = QColor(Qt::white);
		break;
	case IconRole::Rec:
		r.rest = r.hover = t.rec;
		r.on = QColor(Qt::white);
		break;
	case IconRole::Danger:
		r.rest = r.hover = t.rec;
		break;
	case IconRole::Warn:
		r.rest = r.hover = r.on = t.warn;
		break;
	case IconRole::Chrome:
	default:
		break;
	}
	return r;
}

// Give a button an icon, and remember which one, so a theme change can redraw
// it. The id is stored as a dynamic property; `restyleIcons` reads it back.
//
// It does NOT clear the button's text: several keys are a mark AND a word
// ("● REC"), and a caller that wants icon-only simply passes no text.
void setKeyIcon(QAbstractButton *b, Icon id, const IconTints &tints,
		int px = 16);

// Stamp the role and (re)draw the mark in it. Two lines everywhere it is used,
// and the property has to be on the widget before the icon is built — which is
// the whole reason this exists rather than an extra argument: `restyleIcons`
// rebuilds every mark on a theme change and reads the role back off the widget.
void setKeyIconRole(QAbstractButton *b, Icon id, IconRole role,
		    const IconTints &tints, int px = 16);

// Re-tint every icon under `root`. Called when the panel's colours are rebuilt:
// the marks are pixmaps, so unlike everything else on the panel they do not
// follow a new style sheet by themselves.
void restyleIcons(QObject *root, const IconTints &tints);

// ---------------------------------------------------------------------------
// mrKey — WHAT A KEY IS, independent of what it says or how it is drawn
// ---------------------------------------------------------------------------
//
// The automated gate used to find twelve of this panel's keys by their literal
// text: "⏭", "■", "-5s", ">>", "⇄". That made every one of them a tripwire
// under the WRONG thing — the label is a translation and the glyph is a
// drawing, and neither is the key's identity. An icon-first panel would have
// broken all twelve at once while the keys themselves still worked perfectly.
//
// So every command key carries a stable id in a dynamic property, and the gate
// looks for that. It cannot be changed by a locale, a font, or a redesign.
inline const char *kKeyProperty = "mrKey";

// Stamp the id on a button. Separate from setKeyIcon because keys that stay
// TEXT ("-5s", "IN") need an identity too.
void setKeyId(QAbstractButton *b, const QString &id);

} // namespace multireplay
