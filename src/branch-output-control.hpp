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
obs_source_t *ensureFilter(obs_source_t *target, int camIndex, const Config &cfg);

// Enable/disable the filter (this is what starts/stops the Branch Output
// recording when stream_recording=true and no stream server is configured).
void setEnabled(obs_source_t *filter, bool enabled);

// Build the obs_data settings for one camera filter.
// Exposed for unit testing.
obs_data_t *buildSettings(int camIndex, const Config &cfg);

} // namespace branch_output
} // namespace multireplay
