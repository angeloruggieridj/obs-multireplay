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

void obs_module_unload(void)
{
	multireplay::WebServer::instance().stop();
	multireplay::PreviewManager::instance().stop();
	multireplay::ReplayEngine::instance().unload();
	multireplay::ReplayCore::instance().unload();
	obs_log(LOG_INFO, "plugin unloaded");
}
