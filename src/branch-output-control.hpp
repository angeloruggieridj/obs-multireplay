/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Programmatic controller for the Branch Output plugin
(https://github.com/OPENSPHERE-Inc/branch-output, GPLv2).
Filter id and settings keys verified against branch-output master (1.0.9).
*/

#pragma once

#include <obs-module.h>
#include <string>

namespace multireplay {

struct Config; // replay-core.hpp

namespace branch_output {

// Source id of the Branch Output filter (src/plugin-main.cpp: FILTER_ID).
constexpr const char *kFilterId = "osi_branch_output";
// Name prefix for filters owned by obs-multireplay.
constexpr const char *kFilterNamePrefix = "MultiReplay cam";

// True if the Branch Output plugin is loaded in this OBS instance.
bool available();

// Find-or-create our Branch Output filter on `target` for camera `camIndex`
// (0-based) and apply recording settings derived from `cfg`.
// Returns the filter source (caller must obs_source_release) or nullptr.
//
// The returned filter is ALWAYS DISARMED, whether it was just created or only
// reconfigured. Configuring is not arming: a filter left enabled records the
// moment Branch Output's timer notices it, which is how creating a project used
// to start a take nobody asked for. setEnabled(true) is the only way in, and
// ReplayCore::startRecording() is the only caller allowed to use it.
obs_source_t *ensureFilter(obs_source_t *target, int camIndex, const Config &cfg);

// Enable/disable the filter (this is what starts/stops the Branch Output
// recording when stream_recording=true and no stream server is configured).
void setEnabled(obs_source_t *filter, bool enabled);

// True when Branch Output has a RUNNING recording output for this camera.
//
// Enabling the filter is a request, not a start: Branch Output re-evaluates its
// own conditions on a 1 s timer (plugin-main.cpp: TASK_INTERVAL_MS) and the
// Interlock setting that gates them is GLOBAL, lives in Branch Output's own
// dock, and is not exposed by any API — so the only honest way to know whether
// our REC actually started anything is to ask whether the output it would have
// created is running. It is named after our filter (see packet-tap.hpp), so the
// lookup is exact.
bool recordingOutputActive(int camIndex);

// Build the obs_data settings for one camera filter.
// Exposed for unit testing.
obs_data_t *buildSettings(int camIndex, const Config &cfg);

} // namespace branch_output
} // namespace multireplay
