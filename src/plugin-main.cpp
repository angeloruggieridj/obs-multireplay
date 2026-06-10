/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <obs-module.h>

#include "plugin-support.h"
#include "preview.hpp"
#include "replay-core.hpp"
#include "replay-player.hpp"
#include "web-server.hpp"

namespace multireplay {
void registerReplaySources(); // replay-source.cpp
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "broadcast-style multicamera instant replay for OBS Studio. "
	       "Recording layer powered by the Branch Output plugin.";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)",
		PLUGIN_VERSION);

	auto &core = multireplay::ReplayCore::instance();
	core.load();

	multireplay::registerReplaySources();
	multireplay::ReplayEngine::instance().load();
	multireplay::PreviewManager::instance().start();

	multireplay::WebServer::instance().start(core.getConfig().port);

	return true;
}

void obs_module_post_load(void)
{
	// Module load order is alphabetical: obs-multireplay loads BEFORE
	// osi-branch-output, so the filter type only exists after all
	// modules finished loading — check it here, not in obs_module_load.
	if (multireplay::ReplayCore::instance().branchOutputAvailable()) {
		obs_log(LOG_INFO, "Branch Output plugin detected — recording "
				  "layer ready");
		// Filters saved in the scene collection are enabled and would
		// auto-record on startup (the reference controller records only on REC).
		multireplay::ReplayCore::instance().disarmPersistedFilters();
	} else {
		obs_log(LOG_WARNING,
			"Branch Output plugin not found. Install it from "
			"https://github.com/OPENSPHERE-Inc/branch-output — "
			"recording will be unavailable until then.");
	}
}

void obs_module_unload(void)
{
	// Stop the preview manager FIRST: open MJPEG connections only
	// terminate when it reports !running(), and WebServer::stop() joins
	// the listener thread which waits for all active handlers.
	multireplay::PreviewManager::instance().stop();
	multireplay::WebServer::instance().stop();
	multireplay::ReplayEngine::instance().unload();
	multireplay::ReplayCore::instance().unload();
	obs_log(LOG_INFO, "plugin unloaded");
}
