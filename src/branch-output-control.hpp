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
// CALL THIS ONLY FOR A SLOT'S CANONICAL CAMERA (camera-dedup.hpp:
// canonicalCameraIndices — the lowest-numbered slot naming a given source).
// Two slots pointed at the same OBS source are the same picture, and asking
// Branch Output to encode it twice is not two angles, it is one angle
// encoded twice — two filters, two files, and on a GPU with a limited number
// of concurrent hardware encode sessions, enough of them that the losing
// slots' outputs never go active at all (the packet tap can then never
// attach to them — see packet-tap.hpp — which is the shape of "only the
// first camera's preview ever shows a picture"). A duplicate slot owns no
// filter of its own; everything that would read one from it (PacketTap,
// SegmentIndex) redirects to the canonical slot instead.
//
// The returned filter is ALWAYS DISARMED, whether it was just created or only
// reconfigured. Configuring is not arming: a filter left enabled records the
// moment Branch Output's timer notices it, which is how creating a project used
// to start a take nobody asked for. setEnabled(true) is the only way in, and
// ReplayCore::startRecording() is the only caller allowed to use it.
obs_source_t *ensureFilter(obs_source_t *target, int camIndex, const Config &cfg);

// Remove every filter of ours that this configuration does not claim, and
// return how many went. A filter is ours by name ("MultiReplay camN") — this
// function NEVER enumerates or touches a Branch Output filter under any other
// name, so a recording the operator built by hand with Branch Output's own UI,
// on a source we do not manage, is invisible to it and cannot be removed or
// reconfigured by it. One of ours is kept only when slot N is CONFIGURED,
// names the source it is attached to, AND is that source's CANONICAL slot
// (camera-dedup.hpp) — a filter under the name of a slot that merely
// duplicates an earlier one's source is pruned even though the source name
// still matches, because no such filter is ever created any more.
//
// Without this the count of filters and the count of angles drift apart the
// first time a project is opened with fewer cameras than the last one: cam3's
// filter stays on yesterday's source, so the rig declares three angles and arms
// two. Must run on the UI thread, like every other create/destroy of a Branch
// Output filter (its start conditions are evaluated from a QTimer).
int pruneFilters(const Config &cfg);

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
