# Contributing to obs-multireplay

This file exists so a rule already enforced in the code — sometimes the hard
way, after it broke something — doesn't have to be rediscovered by reading
10,000 lines of comments first. If you're about to touch the dock, the
updater, or the layout engine, read the section that applies before you
start.

## Before you open a PR

- **Build and run the test suite.** `ctest --test-dir build_x64 -C
  RelWithDebInfo` for the unit tests; they need no OBS, no Qt and no FFmpeg
  and take well under a minute. If your change touches `src/dock-layout.cpp`
  or `src/dock-style.hpp`, also build and run `tools/dock-mockup` (see
  below) — it's the same code the plugin uses, without the four-minute OBS
  launch cycle.
- **Small, reviewable commits.** One concern per commit, a message that says
  *why*, not just *what* (the diff already says what).
- **Don't reformat what you didn't touch.** There is no `.clang-format` in
  this repo on purpose: the codebase has never been run through one, and
  adding a config file would make the *next* CI run flag the whole tree at
  once. If you want to propose one, it's its own PR, with the resulting
  reformat as the only change in it.

## Invariants — things that will not build, or will build and be wrong

These come from the code's own comments, several of them written right after
a real regression. They're enforced by the gate script and the mockup's
`--check` mode where that's possible; where it isn't, this is the only place
they're written down.

- **Never re-parent an `OBSQTDisplay`.** Qt destroys the native window
  underneath a widget on re-parent, which strands the `obs_display` bound to
  it — the one failure mode `qt-display.hpp` exists to avoid. Reorganizing
  layout means moving grid *cells* (`QGridLayout::addWidget`/`takeAt`) or
  flipping a `QSplitter`'s orientation, never moving the display widget
  itself to a new parent.
- **No modal dialog (`QMessageBox`, blocking `QDialog::exec()`) reachable
  from a control-strip key while `ReplayCore::isRecording()` is true.** A
  modal seizes keyboard and mouse input mid-take. Use `showNotice()` — the
  status-line channel every rejection on the live path already goes through.
  Destructive actions with an explicit confirmation (Delete All) and
  anything reached only from Settings/wizard dialogs are the accepted
  exceptions.
- **A GUI layout decision is judged in `tools/dock-mockup`, not by eyeballing
  OBS.** It shares `dock-layout.cpp` and `dock-style.hpp` with the real dock
  — same arithmetic, same stylesheet — and resizes through the sizes that
  matter in about a second instead of a four-minute OBS relaunch per look.
  Run `dock-mockup.exe --check` (add `--host=obs` to test against OBS's own
  theme, `--font-scale=125` for a larger OBS font) before changing anything
  under `dock-layout.*` or `dock-style.hpp`.
- **`ControlStrip` reports two heights, not one** (`ControlStripItem`: a
  floor for the flat/folded shape, a preference for the wide one). A widget
  normally can't say both, and collapsing them back into one is exactly the
  bug that once pinned the panel's minimum height at 680px regardless of
  width. Add a section with `addStrip()`, never `addWidget()`.
- **A pixel value shared between a stylesheet and layout code is computed in
  exactly one place** (the `kKeyH` / `@rowFont@` pattern). Two numbers that
  are supposed to agree and are typed twice will eventually stop agreeing.
- **Never compare a master-clock instant with `> 0`.** Footage recorded
  before the machine's last reboot maps to *negative* instants — the
  ordinary case for a reopened project, not an edge case. Use `kNoInstant`
  (session-clock.hpp) / `hasPosition()`. Durations may still use `0`.
- **No mutex held across a call that can block** — a socket, a graphics-API
  call, a file demux. Resolve OBS source references on the UI thread and
  release them outside the lock.
- **`obs_module_unload` joins every thread this plugin started.** A
  `std::thread` still joinable at destruction is `std::terminate`. If you add
  a thread, add its join, in the shutdown order documented at the top of
  `plugin-main.cpp` (producers before the things that read what they
  produce).
- **A path is UTF-8 everywhere in this codebase.** `std::filesystem::path`'s
  `.string()` narrows through the ANSI code page on MSVC — silently correct
  on Linux/macOS and silently wrong on Windows for any path with an accented
  character. Use `pathToUtf8`/`utf8ToPath`/`joinUtf8` (`path-utf8.hpp`);
  never call `.string()` on a path in `src/`.
- **Locale files stay in sync.** `data/locale/en-US.ini` and `it-IT.ini` must
  carry the same keys — `cut -d= -f1 file.ini | sort` on both and diff. A
  user-facing string always goes through `obs_module_text`.
- **A core error string reaching the UI should try `error-locale.hpp`
  first.** It classifies a handful of known, frequent engine rejections and
  puts a localized headline in front of the (still-English) detail; anything
  it doesn't recognise should still reach the operator via `showNotice`,
  just without the headline — never silently dropped.
- **New pure logic goes in a header with zero OBS/Qt/FFmpeg types**, the way
  `master-timeline.hpp`, `health-rules.hpp`, `reverse-plan.hpp` and
  `angle-channels.hpp` do, with unit tests in `tests/standalone/` registered
  in `tests/CMakeLists.txt`. That's what lets `ctest` run on every platform
  in CI without needing obs-deps, Qt or FFmpeg — and what makes a fault like
  "an unbounded UI gap" or "a bay switch loses the other bay's angle"
  reproducible in milliseconds instead of by driving real OBS.

## Comments: the *why*, not the *what*

Identifiers already say what the code does; a comment earns its place by
saying why it's shaped this way — a constraint, an invariant, a bug it was
written to close. A comment that only restates the next line is worth
deleting.

**Lead with what must stay true, not with the story of how it got that
way.** A long comment block often does two different jobs at once — *here
is the rule* and *here is the bug that taught it to us* — and a reader
skimming for the rule has to read the whole story to find it. Open with a
short, declarative sentence (or an ALL-CAPS clause, which is already this
codebase's own habit: `NOTHING HERE MAY RE-PARENT AN OBSQTDisplay`) stating
the invariant; the history, the measurement, the exact freeze that shipped
can follow it.

**A history longer than ~10 lines moves to `docs/adr/`, not into the code.**
`docs/adr/NNN-slug.md` (see `docs/adr/README.md` for the format) keeps the
full story — what was tried, what broke, what was measured — with a
one-line pointer left where the code used to carry all of it:
`// See docs/adr/007-pinned-key-height.md`. `docs/` is gitignored on
purpose (this project's development history stays out of the published
repo — see the top of the internal project notes), so an ADR is for this
tree's own future sessions, not a public changelog.

**Before writing a long explanatory comment, check it isn't already
written somewhere else.** `tools/find-duplicate-comments.sh` greps `src/`
for comment lines that repeat verbatim — the kind of drift where an
explanation gets copied to a second call site and then only one copy is
updated when the reason changes. Advisory, not wired into CI, for the same
reason `find-orphan-constants.sh` isn't: it can't tell a stale copy from
two places that are still legitimately true for the same reason, so read
every hit before touching either copy.

## Running the mockup

```sh
cmake -S tools/dock-mockup -B build_mockup -DCMAKE_PREFIX_PATH=<path-to-qt6>
cmake --build build_mockup --config RelWithDebInfo
build_mockup/RelWithDebInfo/dock-mockup --check      # the gate: exit 0 = pass
build_mockup/RelWithDebInfo/dock-mockup --show        # a live window to drag
build_mockup/RelWithDebInfo/dock-mockup <folder>      # writes PNGs at six sizes
```

## Licence

By contributing you agree your changes are licensed under GPL-2.0-or-later,
matching the rest of the project (see [LICENSE](LICENSE)).
