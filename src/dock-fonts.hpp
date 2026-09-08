#pragma once

// The panel's four typefaces, EMBEDDED IN THE BINARY.
//
// WHY EMBEDDED AND NOT THE HOST'S. The design artifacts specify this panel in
// Barlow Condensed / Barlow Semi Condensed / IBM Plex Sans / IBM Plex Mono. A
// family that is merely ASKED FOR resolves to whatever the machine happens to
// have, which is a different panel on every rig and a different panel in every
// screenshot — so "identical to the artifact" would be untestable by
// construction. Registering the faces makes the answer the same everywhere.
//
// PURE QT, NO OBS TYPES — the same rule as dock-style.hpp and dock-layout.
// tools/dock-mockup compiles this file, and the mockup cannot link libobs.
//
// A FAMILY THAT FAILS TO REGISTER FALLS BACK; IT NEVER RETURNS EMPTY. An empty
// family-name in a stylesheet is a rule Qt accepts and then ignores, so a
// failed load would look EXACTLY like a working one until somebody measured it.
// Same rule as the drawn marks, where a failed write becomes `image: none`.

#include <QString>

namespace multireplay::fonts {

// Registers every embedded face. Idempotent — calling it twice registers
// nothing the second time. Returns how many faces actually registered, so a
// caller can log "8 of 8" rather than assuming.
int registerEmbedded();

QString labelFamily();   // Barlow Condensed — section captions, LIVE, tabs
QString displayFamily(); // Barlow Semi Condensed — project name, titles
QString bodyFamily();    // IBM Plex Sans — prose, the search placeholder
QString monoFamily();    // IBM Plex Mono — keys, numbers, timecodes

// True when every face registered. The gate asserts this: see the note above
// about a failed load being invisible.
bool allEmbedded();

} // namespace multireplay::fonts
