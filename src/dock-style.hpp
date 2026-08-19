// dock-style.hpp — the panel's style sheet, in one place.
//
// It lives in a header of its own so that the DOCK and the LAYOUT MOCKUP are
// styled by the same bytes. A mockup with its own copy of the sheet measures a
// panel that does not exist: every height rule here (see the 28 px arithmetic
// below) is part of the layout, not decoration, and a second copy would drift
// from this one the first time a padding changed.
#pragma once

namespace multireplay {

// is copied from the reference controller itself — these are hex values in our own stylesheet.
//
// Palette (sampled):
//   #1D3D74  table header blue      #176533  active-angle header green
//   #DB5026  selected row orange    #199847  position bar, played
//   #00121C  row odd (navy black)   #146433  position bar, remaining
//   #002A42  row even               #116B35  angle enabled (checkbox)
//   #A81C1C  Live / REC red         #0c0c0c  dock background
inline const char *const kDockStyle =
R"QSS(
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

/* ── generic buttons ───────────────────────────────────── */
/* ONE HEIGHT for every key in the control rows: 26 px. A row mixing 22, 24, 26
   and 28 px keys reads as several rows badly aligned — the eye follows the line
   of the bottom borders, and four lines are four groups where there is one.
   Widths stay free (they follow the label, which is what tells the keys apart),
   heights do not.
   Unified DOWNWARDS, to the middle of the old spread, because every pixel of
   chrome here is a pixel the splitter takes off the PREVIEW, and the preview is
   what the operator is actually looking at. */
#MultiReplayDock QPushButton {
	background: #1e1e1e; color: #b0b0b0;
	border: 1px solid #2c2c2c; border-radius: 4px;
	padding: 3px 9px; font-size: 11px; min-height: 20px;
}
/* ONE HEIGHT FOR EVERY KEY: 28 px, and it is arithmetic, not a wish.
   In Qt, a style sheet min-height is the CONTENT box — padding and border are
   added to it. So min-height 26 with 3 px of padding and a 1 px border is a
   34 px key, while the transport rule (min-height 26, padding 0) is a 28 px
   one, and the toggles (padding 2) are 32. Three heights on one row, from a
   number that looked identical in all three rules.
   Every rule below therefore states min-height = 28 - 2*padding - 2, and the
   inputs already land on the same 28 (min-height 20 + 3 + 3 + 1 + 1), so a
   combo in a row of keys lines up with them instead of nearly lining up.
   CHANGING A PADDING HERE MEANS CHANGING ITS min-height. */
#MultiReplayDock QPushButton:hover  { background: #282828; border-color: #424242; }
#MultiReplayDock QPushButton:pressed { background: #141414; }
#MultiReplayDock QPushButton:disabled { color: #303030; border-color: #1e1e1e; }

/* ── transport step / icon buttons ────────────────────── */
QPushButton#mrTransport {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 5px;
	color: #c8c8c8; font-size: 14px;
	min-width: 30px; min-height: 26px; padding: 0;
}
QPushButton#mrTransport:hover { background: #222222; border-color: #424242; color: #f0f0f0; }

/* >> lives ON the green band, which is 28 px tall with 2 px of margin, so it
   cannot ask for the 28 px of height a transport key asks for: a stylesheet
   min-height LARGER than the widget's own fixed height makes the style draw a
   taller frame than the widget owns, and the bottom border lands outside it.
   Hence its own role, with the height it can actually have. */
QPushButton#mrSkip {
	background: #0f3d22; border: 1px solid #2b7a45; border-radius: 4px;
	color: #d6f0e0; font-size: 11px; font-weight: 700;
	min-width: 30px; min-height: 0px; padding: 0px 4px;
}
QPushButton#mrSkip:hover { background: #14512c; border-color: #3fa060; color: #ffffff; }

/* play/pause */
QPushButton#mrPlay {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 5px;
	color: #c8c8c8; font-size: 16px;
	min-width: 38px; min-height: 26px; padding: 0;
}
QPushButton#mrPlay:hover { background: #222222; border-color: #424242; color: #f0f0f0; }
QPushButton#mrPlay[playing="true"] { background: #0c2212; border-color: #1c8a38; color: #28b050; }
QPushButton#mrPlay[playing="true"]:hover { background: #102818; border-color: #22a040; }

/* NOW / live-edge */
QPushButton#mrNow {
	background: #181818; border: 1px solid #2c2c2c; border-radius: 5px;
	font-weight: 700; font-size: 10px; letter-spacing: 0.8px;
	min-height: 26px; min-width: 36px; padding: 0; color: #484848;
}
/* RED AT REST TOO. NOW is where the operator goes to get out of a replay and
   back on the live edge; drawn in the panel's ordinary grey it was the least
   visible key in the row that matters most. */
QPushButton#mrNow { color: #a03030; border-color: #4a1c1c; }
QPushButton#mrNow:hover { color: #e05050; border-color: #7a2a2a; }
QPushButton#mrNow[live="true"] { background: #280808; border-color: #c02020; color: #e03030; }

/* ── the reference controller "Live" mode toggle — red when marks are taken as they happen ── */
QPushButton#mrLive {
	background: #181818; color: #6a6a6a;
	border: 1px solid #2c2c2c; border-radius: 3px;
	font-weight: 700; font-size: 11px; letter-spacing: 0.6px;
	min-height: 22px; /* + 2px padding + 2px border = 28 */ min-width: 54px; padding: 2px 14px;
}
QPushButton#mrLive:hover { border-color: #424242; color: #9a9a9a; }
QPushButton#mrLive:checked {
	background: #A81C1C; color: #ffffff; border-color: #d03030;
}

/* ── latching toggles (Loop · music · to output) ─────────── */
QPushButton#mrToggle {
	background: #181818; color: #6a6a6a;
	border: 1px solid #2c2c2c; border-radius: 3px;
	font-size: 10px; min-height: 22px; padding: 2px 9px;
}
QPushButton#mrToggle:hover { border-color: #424242; color: #9a9a9a; }
/* A LIT TOGGLE IS A STATE, NOT AN INVITATION. It used to be the same filled
   green as the play key, so "Loop is on" and "press this to play" carried the
   same weight — and on a panel read at a glance under pressure, two meanings in
   one colour is one meaning too many. Lit but hollow: unmistakably on, clearly
   not the thing that starts a replay. */
QPushButton#mrToggle:checked {
	background: #14351f; color: #4fd07d; border-color: #22a04a;
	font-weight: 700;
}
QPushButton#mrToggle:checked:hover { background: #1a4227; color: #6fe098; }

/* ── list tabs (the reference controller: one tab per event list) ─────────────── */
QTabBar#mrListTabs { background: transparent; }
/* No font-size here on purpose: the tab font is set on the WIDGET (see
   buildToolbar). A size that lives only in the stylesheet is a size nothing
   outside the painter can measure, and "is this tab wide enough for its own
   name" is exactly the question the gate has to answer. */
QTabBar#mrListTabs::tab {
	background: #141414; color: #8a8a8a;
	border: 1px solid #232323; border-bottom: 0;
	padding: 3px 9px; margin-right: 1px; min-width: 16px;
}
QTabBar#mrListTabs::tab:hover { background: #1e1e1e; color: #c0c0c0; }
QTabBar#mrListTabs::tab:selected {
	background: #1D3D74; color: #ffffff; border-color: #2a5296;
}

/* ── settings dialog: side menu + pages ─────────────────────────────── */
QListWidget#mrSettingsNav {
	background: #121212; color: #9a9a9a;
	border: 0; border-right: 1px solid #232323;
	outline: 0; font-size: 11px;
}
QListWidget#mrSettingsNav::item { padding: 8px 12px; border: 0; }
QListWidget#mrSettingsNav::item:hover { background: #1c1c1c; color: #d0d0d0; }
QListWidget#mrSettingsNav::item:selected {
	background: #1D3D74; color: #ffffff;
}
QLabel#mrSettingsTitle {
	color: #e0e6ee; font-size: 14px; font-weight: 700;
}
QLabel#mrSettingsBlurb { color: #7a8490; font-size: 10px; }

/* The two numbers an operator opens Settings to read before kick-off: how much
   disk is left, and how much recording that is. A card each — as one line of
   text the second number had no caption at all, and "09:12" with no word in
   front of it reads as a clock. */
QFrame#mrStatCard {
	background: #121212; border: 1px solid #262626; border-radius: 3px;
}
QLabel#mrStatCaption {
	color: #7a8490; font-size: 10px; font-weight: 600;
	letter-spacing: 0.6px; text-transform: uppercase;
}
QLabel#mrStatValue { color: #e6ecf4; font-size: 19px; font-weight: 700; }
QLabel#mrStatUnit { color: #7a8490; font-size: 11px; padding-bottom: 3px; }

/* ── multiview tiles (the reference controller's camera thumbnails beside the A output) ── */
QWidget#mrTile { background: #000000; }
/* the reference controller captions its thumbnails with a blue band; the angle being watched turns
   green and the one on air red, so the operator reads tally from the picture
   itself instead of correlating it with the angle buttons. */
QLabel#mrTileCap {
	background: #1D3D74; color: #dfe8f6;
	font-size: 9px; font-weight: 700; padding: 1px 4px;
}
QLabel#mrTileCap[tally="pvw"] { background: #176533; color: #ffffff; }
QLabel#mrTileCap[tally="pgm"] { background: #A81C1C; color: #ffffff; }
QLabel#mrTileCap[tally="replay"] { background: #3a2d10; color: #ffd07a; }

/* ── channel strip under the preview (the reference controller green info band) ─ */
QLabel#mrChanBadge {
	background: #0e4523; color: #ffffff;
	font-weight: 700; font-size: 11px; padding: 2px 7px;
}
/* The letter under each output box. the reference controller puts A on a green bar and B on a
   blue one, and that colour is how the operator tells the two boxes apart
   from across the room — faster than reading a letter. The one being driven
   by the keys is the bright one. */
QLabel#mrChanTag {
	background: #12161c; color: #6b7787;
	font-weight: 700; font-size: 10px; padding: 1px 0;
}
QLabel#mrChanTag[chan="A"][active="true"] { background: #146433; color: #ffffff; }
QLabel#mrChanTag[chan="B"][active="true"] { background: #1d3d74; color: #ffffff; }
QLabel#mrChanStrip {
	background: #146433; color: #dff3e2;
	font-size: 10px; padding: 2px 7px;
}

/* REC button */
QPushButton#mrRec {
	font-weight: 700; font-size: 12px; letter-spacing: 0.6px;
	border-radius: 4px; min-height: 20px; padding: 3px 14px;
}
QPushButton#mrRec[recording="false"] {
	background: #181010; color: #b03030; border: 1px solid #2c1818;
}
QPushButton#mrRec[recording="false"]:hover { background: #1e1010; border-color: #4a2020; }
QPushButton#mrRec[recording="true"] {
	background: #640808; color: #ffffff; border: 1px solid #c02020;
}
QPushButton#mrRec[recording="true"]:hover { background: #740e0e; }

/* Channel selector A|B / A / B and the swap. Small, square and always visible:
   it is the answer to "where is this key going", and an operator who has to
   look for it has already pressed something on the wrong channel. */
QPushButton#mrChanSel {
	background: #14161a; color: #7a879a; border: 1px solid #262b33;
	border-radius: 3px; padding: 2px 6px; font-weight: 700; font-size: 11px;
	min-height: 22px; /* + 2px padding + 2px border = 28 */
}
QPushButton#mrChanSel:hover { background: #1b1f26; color: #c8d2de; }
QPushButton#mrChanSel:checked {
	background: #1d3d74; color: #ffffff; border-color: #2f5da8;
}

/* M4 health badge: amber = degraded, red = this take is not usable. It sits
   beside REC and is hidden entirely while there is nothing to report. */
QPushButton#mrHealth {
	font-weight: 700; font-size: 12px; border-radius: 4px;
	min-height: 20px; padding: 3px 8px;
}
QPushButton#mrHealth[level="warn"] {
	background: #2a2008; color: #e0a020; border: 1px solid #6a5010;
}
QPushButton#mrHealth[level="bad"] {
	background: #3a0c0c; color: #ff6a4a; border: 1px solid #8a1c1c;
}

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
	min-width: 34px; min-height: 26px; padding: 0 4px;
}
QPushButton#mrAngle:hover { background: #202020; color: #585858; border-color: #424242; }
/* AN EMPTY SLOT IS DRAWN, NOT LEFT AS A HOLE. The matrix reserves all eight
   places so that angle 5 is at the same x on every rig; reserving them by
   leaving nothing there made the section read as a gap somebody forgot to
   fill. A sunken outline says "a camera could go here" and costs no attention:
   it is darker than the panel, and it has nothing to read. */
QPushButton#mrAngleSlot {
	background: #101010; border: 1px solid #191919; border-radius: 3px;
	color: transparent; min-width: 34px; min-height: 26px; padding: 0 4px;
}
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
	min-width: 28px; min-height: 24px; padding: 1px 4px;
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
/* THE ONE FILLED KEY IN THE PANEL. "Play the selected events" is the action the
   operator reaches for more than any other, and it was an outlined key with a
   green tint — the same weight as Mark, as Export, as ⋯, so the eye had to READ
   the row to find it. Filled, it is found without reading, and nothing else on
   the panel is filled except the two states that are allowed to shout (the REC
   key when armed, the on-air band). An action and a state look different here
   on purpose: this one asks to be pressed, those two report. */
QPushButton#mrAccent {
	background: #1b8a44; border: 1px solid #22a352; color: #ffffff;
	font-weight: 700;
}
QPushButton#mrAccent:hover { background: #21a151; border-color: #2cbb63; }
QPushButton#mrAccent:pressed { background: #146633; }
QPushButton#mrAccent:disabled {
	background: #16281c; border-color: #1e3a26; color: #4a5a50;
}
QPushButton#mrDanger { color: #b03030; border-color: #2c1818; }
QPushButton#mrDanger:hover { background: #1e1010; border-color: #442020; }

/* ── checkboxes ──────────────────────────────────────────── */
#MultiReplayDock QCheckBox { color: #888888; spacing: 5px; font-size: 11px; }
#MultiReplayDock QCheckBox::indicator {
	width: 13px; height: 13px; border-radius: 3px;
	border: 1px solid #2c2c2c; background: #181818;
}
#MultiReplayDock QCheckBox::indicator:checked { background: #1c8a38; border-color: #22a040; }

)QSS"
// SPLIT HERE, and it is the compiler's rule rather than a section boundary:
// MSVC refuses a single string literal over 16380 bytes (C2026, "trailing
// characters will be truncated" — a stylesheet silently missing its second
// half). Adjacent literals are concatenated at translation, so the sheet is
// still one string; it just arrives in two pieces. Any section boundary will do
// when the next rule pushes it over again.
R"QSS(
/* ── inputs ──────────────────────────────────────────────── */
#MultiReplayDock QComboBox, #MultiReplayDock QLineEdit {
	background: #181818; color: #c0c0c0;
	border: 1px solid #2c2c2c; border-radius: 3px;
	padding: 3px 7px; min-height: 20px; font-size: 11px;
}
#MultiReplayDock QComboBox:hover, #MultiReplayDock QLineEdit:hover { border-color: #424242; }
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
/* "--" in an angle cell: no per-angle speed, the slider decides. Grey, because
   it is an absence and not a choice. It lives here rather than in a
   setStyleSheet() on the combo itself: a style sheet set on one widget makes Qt
   build a separate style context for it and re-polish its subtree, and that was
   a measurable part of a table rebuild that took over a tenth of a second. */
#MultiReplayDock QComboBox[mrNoOverride="true"],
#MultiReplayDock QComboBox[mrNoOverride="true"] QLineEdit {
	color: #707070;
}
#MultiReplayDock QComboBox QAbstractItemView {
	background: #181818; color: #c0c0c0; border: 1px solid #2c2c2c;
	selection-background-color: #1a2e52; selection-color: #d0d8f0; outline: 0;
}
/* The rows of an open list. Without an explicit item rule the view inherits the
   padding of the CLOSED box above (3px 7px plus its min-height), which leaves
   each row taller than its text and the text sitting at the bottom of it —
   which is exactly how a centred list stops looking centred. The height is
   stated here and the horizontal centring is set per item, in code, because
   Qt draws item text through the delegate and no stylesheet reaches it. */
#MultiReplayDock QComboBox QAbstractItemView::item {
	min-height: 20px; padding: 0px; border: 0;
}

/* ── zones: a captioned frame round each group of controls ───────
   Quiet on purpose. The border is there to say "these keys belong together",
   which needs one pixel and no colour — a loud box would compete with the
   green band and the red REC key, which are the two things on this panel that
   are allowed to shout. */
/* A caption over a hairline, not a box. Five bordered groups drew five frames
   competing for attention with the green band and the REC key; a rule under a
   title says "this group" just as well and disappears when it is not being
   looked for. */
QFrame#mrZone {
	border: 0; border-top: 1px solid #22262c; background: transparent;
}
QLabel#mrZoneTitle {
	color: #6a7686; font-size: 9px; font-weight: 700; letter-spacing: 1.1px;
	padding: 1px 0px 0px 1px; margin: 0px;
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

} // namespace multireplay
