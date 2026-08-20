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
#include <obs-frontend-api.h>

#include <QWidget> // the dock's parent: OBS's main window (see obs_module_post_load)

#include "plugin-support.h"
#include "event-store.hpp"
#include "export.hpp"
#include "health.hpp"
#include "multireplay-dock.hpp"
#include "replay-core.hpp"
#include "segment-index.hpp"
#include "packet-tap.hpp"
#include "playback-coordinator.hpp"
#include "replay-channel.hpp"
#include "selftest.hpp"
#include "updater.hpp"

namespace {
constexpr const char *kDockId = "obs-multireplay-dock";
}

namespace {
// Branch Output filters are persisted ENABLED in the scene collection and
// start recording as soon as their source becomes active. The scene
// collection loads AFTER obs_module_post_load, so the disarm must run on
// the frontend FINISHED_LOADING / SCENE_COLLECTION_CHANGED events. The same
// events are where the replay input must be (re)adopted for the just-loaded
// scene collection.
void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		if (!multireplay::ReplayCore::instance().isRecording())
			multireplay::ReplayCore::instance()
				.disarmPersistedFilters();
		// Both channels: A and B are two inputs of one type, and the
		// second has to be adopted (or created) on exactly the same beat
		// as the first.
		const bool wantB = multireplay::ReplayCore::instance()
					   .getConfig()
					   .enableChannelB;
		for (int i = 0; i < multireplay::kChannels; i++) {
			// ...but only the bays the operator asked for. Creating B's
			// input unconditionally put a source in his scene collection
			// he never wanted and would have to delete by hand, on a rig
			// where one bay is the whole job.
			if (i == 1 && !wantB)
				continue;
			auto &ch = multireplay::ReplayChannel::instance(
				(multireplay::Which)i);
			ch.ensureSource();
			// The scene collection has just been (re)loaded, so this
			// is where the operator's items that show a replay input
			// exist again - and where the "fit to canvas" setting has
			// to be seeded into the channel, which re-applies it on
			// every clip afterwards.
			ch.applyCanvasFit(multireplay::ReplayCore::instance()
						  .getConfig()
						  .fitReplayToCanvas);
			// A collection saved by an earlier build can carry our
			// dip filter, which then sits in the operator's filter
			// list for ever. It is removed at full opacity now, so
			// this is the one sweep that clears the old ones.
			multireplay::PlaybackCoordinator::instance(
				(multireplay::Which)i)
				.clearDip();
		}
		// No-op unless OBS_MULTIREPLAY_SELFTEST is set.
		multireplay::maybeRunSelfTest();
		return;
	}

	// --- LET GO OF THE SOURCES BEFORE OBS CLEARS THEM ---------------------
	// OBS clears scene data on the way out of a collection and on the way out
	// of the program, and anything still referenced then produces
	//   Not all sources were cleared when clearing scene data: ...
	// which OBS reports to the operator as a plugin that failed to release its
	// resources — a dialog on shutdown, which is exactly where nobody wants
	// one. We hold refs in two places (each channel's own input, and the two
	// preview pointers the dock publishes for the graphics thread), and both
	// have to be dropped here rather than in obs_module_unload: the module is
	// unloaded well AFTER the scene data has gone.
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP ||
	    event == OBS_FRONTEND_EVENT_EXIT) {
		// On EXIT the dock also stops polling: a collection CHANGE is
		// followed by more work (the new collection has to be adopted), but
		// an exit is not, and a tick between here and the clearing would take
		// a fresh reference to the very sources we have just let go of.
		if (event == OBS_FRONTEND_EVENT_EXIT)
			multireplay::MultiReplayDock::prepareForShutdown();
		else
			multireplay::MultiReplayDock::releasePreviewRefs();
		for (int i = 0; i < multireplay::kChannels; i++) {
			multireplay::ReplayChannel::instance((multireplay::Which)i)
				.releaseSource();
			// The music bed is a private source of ours sitting on a
			// mixer channel: same rule, same moment.
			multireplay::PlaybackCoordinator::instance(
				(multireplay::Which)i)
				.releaseMusic();
		}
	}
}
} // namespace

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

	// Register the packet-tap output types before anything can arm them,
	// and the replay source type before a scene collection can reference it.
	multireplay::PacketTap::instance().load();
	multireplay::ReplayChannel::instance().load();

	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

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

	// Register the native dock now that the Qt frontend is ready. OBS wraps
	// the widget in a dock and takes ownership of it (removed by id below).
	//
	// BUILT AS A CHILD OF THE MAIN WINDOW, and that is not tidiness. Built
	// parentless it is a TOP-LEVEL widget while its constructor runs, and the
	// constructor creates OBSQTDisplay children that ask for a native window
	// (WA_NativeWindow). Qt answers that by giving the top level of the tree a
	// real handle — i.e. the dock itself — and the WA_NativeWindow it sets
	// there sticks. A moment later obs_frontend_add_dock_by_id re-parents the
	// dock into a QDockWidget, and what is left is a NON-top-level widget with
	// a QWindow of its own: from then on every dialog and popup that computes
	// its transient parent from the nearest native ancestor finds that window,
	// and Qt refuses it —
	//   QWidgetWindow(0x..., name="MultiReplayDockWindow") must be a top level
	//   window.
	// which is the warning in the log. With the main window as the parent the
	// top level that gets the handle is the main window (which has one
	// already), the dock stays alien, and the transient parent every dialog
	// computes is a real top level.
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dock = new multireplay::MultiReplayDock(main);
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Dock.Title"),
					 dock)) {
		obs_log(LOG_ERROR, "Failed to register the obs-multireplay dock");
		delete dock;
	}
}

// EVERY SUBSYSTEM THAT OWNS A THREAD IS STOPPED HERE, IN THIS ORDER.
//
// The rule was already written down — "a std::thread still joinable at
// destruction is std::terminate", replay-channel.cpp — and three subsystems had
// simply never been told: ExportManager (whose reel ran DETACHED and writes
// gigabytes for minutes, i.e. executing code out of a DLL that has been
// unloaded), HealthMonitor (sampler plus a detached disk probe) and
// SegmentIndex (a watcher that demuxes files and calls into PacketTap).
//
// The order is not alphabetical. Producers first, then the things that read
// what they produce, and last the ones that only write to disk:
//   tap    — stop capturing before anything that reads the ring goes away
//   channels — their playback and prefetch threads read the ring and the files
//   export — its jobs read SegmentIndex
//   segment index — its watcher calls PacketTap::videoSamples()
//   health — its sampler calls PacketTap::stats()
//   events — flush the last events.json write
//   core   — hotkeys and recording state
//   updater — a network thread that touches none of the above
void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_frontend_remove_dock(kDockId);
	// Detach from Branch Output's encoders before anything else tears down.
	multireplay::PacketTap::instance().unload();
	for (int i = 0; i < multireplay::kChannels; i++)
		multireplay::ReplayChannel::instance((multireplay::Which)i)
			.unload();
	multireplay::ExportManager::instance().shutdown();
	multireplay::SegmentIndex::instance().stop();
	multireplay::HealthMonitor::instance().shutdown();
	multireplay::EventStore::instance().shutdown();
	multireplay::ReplayCore::instance().unload();
	// A check or a download still in flight is a thread still joinable, and a
	// joinable thread at static destruction is std::terminate — OBS vanishing
	// on the way out rather than closing. Bounded by one HTTP timeout.
	multireplay::Updater::instance().shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
