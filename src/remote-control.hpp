/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

External control over obs-websocket: the "multireplay" vendor and its requests,
for a hardware controller bridge (see tools/hardware-bridge).

Optional by construction. obs-websocket ships with OBS, but an operator can
disable it, and without it registerRequests() logs one line and does nothing —
the panel, the hotkeys and Stream Deck do not depend on it.
*/

#pragma once

namespace multireplay::remote_control {

// From obs_module_post_load only: obs-websocket creates its proc handler in its
// own obs_module_load, and modules load alphabetically — after ours.
void registerRequests();

// From OBS_FRONTEND_EVENT_EXIT. Every request after this is answered with an
// error instead of being queued to a UI thread that is about to stop turning.
void stopAccepting();

// From obs_module_unload: stops accepting and unregisters every request.
void shutdown();

} // namespace multireplay::remote_control
