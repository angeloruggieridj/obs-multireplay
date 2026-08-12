/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Automated M0 gate. Runs the whole "Branch Output records → we tap its encoder"
path against synthetic sources and writes a machine-readable verdict, so the
gate can be executed from a script instead of by hand.

Driven entirely by environment variables (absent = never runs, so a normal OBS
session is untouched):

  OBS_MULTIREPLAY_SELFTEST       non-empty and != "0" → run on FINISHED_LOADING
  OBS_MULTIREPLAY_SELFTEST_OUT   JSON report path (default: %TEMP%/obs-multireplay-selftest.json)
  OBS_MULTIREPLAY_SELFTEST_SECS  measurement window in seconds (default 25)
  OBS_MULTIREPLAY_SELFTEST_CAMS  synthetic cameras to create, 1..8 (default 2)
  OBS_MULTIREPLAY_SELFTEST_SOURCES
                                 comma-separated names of EXISTING OBS sources
                                 to tap instead of synthetic ones — this is how
                                 the same automated gate runs against the real
                                 capture cards without any manual measurement

libobs exposes no frontend quit call, so the runner script closes OBS once the
report file appears; writing that file is the "run finished" signal.

It deliberately does NOT go through ReplayCore's persisted configuration: it
builds a throwaway Config and drives branch_output::ensureFilter directly, so
running it can never overwrite the operator's real settings, session folder or
camera assignment.
*/

#pragma once

namespace multireplay {

// No-op unless OBS_MULTIREPLAY_SELFTEST is set. Safe to call more than once;
// only the first invocation starts a run.
void maybeRunSelfTest();

} // namespace multireplay
