/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "branch-output-control.hpp"
#include "replay-core.hpp"
#include "path-utf8.hpp"
#include "plugin-support.h"

#include <filesystem>
#include <string>
#include <vector>

namespace multireplay {
namespace branch_output {

bool available()
{
	// Returns the translated display name if a source type with this id
	// is registered, nullptr otherwise.
	return obs_source_get_display_name(kFilterId) != nullptr;
}

obs_data_t *buildSettings(int camIndex, const Config &cfg)
{
	obs_data_t *settings = obs_data_create();

	// --- Recording only: no stream server configured ---
	obs_data_set_bool(settings, "stream_recording", true);
	obs_data_set_bool(settings, "recording_output_enabled", true);
	obs_data_set_bool(settings, "replay_buffer", false);

	// --- Destination: session folder, one file series per camera ---
	obs_data_set_bool(settings, "use_profile_recording_path", false);
	// Compute recording path from cfg directly — do NOT call
	// ReplayCore::instance().recordingFolder() here because buildSettings()
	// is called from ensureFilter() which is called from startRecording()
	// while holding ReplayCore::mutex_, causing a recursive-lock deadlock.
	// A3: UTF-8. This string is handed to Branch Output, which hands it to
	// the muxer; path::string() on MSVC would narrow it through the ANSI code
	// page and a project folder with an accent in it would not be found.
	std::string recPath = cfg.currentProjectName.empty()
				      ? cfg.sessionFolder
				      : joinUtf8(cfg.sessionFolder,
						 cfg.currentProjectName);
	obs_data_set_string(settings, "path", recPath.c_str());
	std::string nameFormat = "cam" + std::to_string(camIndex + 1) +
				 "_%CCYY-%MM-%DD_%hh-%mm-%ss";
	obs_data_set_string(settings, "filename_formatting", nameFormat.c_str());
	obs_data_set_bool(settings, "no_space_filename", true);

	// --- Container: Hybrid MP4 (crash-safe, chapter markers) ---
	obs_data_set_string(settings, "rec_format", cfg.recFormat.c_str());

	// --- Automatic split by time, or not at all -------------------------
	// 0 = one continuous file, and that is what an ISO normally wants: every
	// split is a seam an event can straddle, and the export path still cannot
	// cross one.
	//
	// Branch Output enables splitting on a non-empty string
	// (plugin-stream-recording.cpp: `splitRecordingEnabled = strlen(splitFile)
	// > 0`, then by_time/by_size decide max_time_sec/max_size_mb), and "" is
	// the value its own "No split" list item carries. It MUST be written
	// explicitly rather than left out: Branch Output defaults this key from
	// the OBS profile's own recording-split settings, so an unset key can
	// still split.
	if (cfg.splitMinutes > 0) {
		obs_data_set_string(settings, "split_file", "by_time");
		obs_data_set_int(settings, "split_file_time_mins",
				 cfg.splitMinutes);
	} else {
		obs_data_set_string(settings, "split_file", "");
	}

	// --- Video encoder. Encoder-specific settings (e.g. bitrate) live in
	// the same settings object: Branch Output passes it verbatim to
	// obs_video_encoder_create(). ---
	std::string encoderId = cfg.videoEncoderId.empty()
					? ReplayCore::instance().pickVideoEncoder()
					: cfg.videoEncoderId;
	obs_data_set_string(settings, "video_encoder", encoderId.c_str());
	obs_data_set_int(settings, "bitrate", cfg.videoBitrateKbps);
	// Short GOP (1s) keeps future frame-accurate seeking cheap (M2).
	obs_data_set_int(settings, "keyint_sec", 1);

	// --- Low-latency profile for the live packet tap --------------------
	// The replay live edge IS the encoder's output (see packet-tap.hpp), so
	// encoder pipeline latency directly delays how fresh the newest
	// replayable frame is. The stock profile measured 368 ms on QSV.
	//
	// QSV (obs-qsv11.c:595-608): latency="normal" means AsyncDepth 4 plus a
	// 60-frame look-ahead under CBR; "ultra-low" means AsyncDepth 1 and no
	// look-ahead at all. B-frames add reordering delay on top, and removing
	// them also makes replay seeking cheaper (no reordered decode).
	//
	// Keys not understood by the selected encoder are simply ignored, so
	// this stays safe across QSV / NVENC / AMF / x264.
	obs_data_set_string(settings, "latency", "ultra-low"); // QSV
	obs_data_set_int(settings, "bframes", 0);              // QSV
	obs_data_set_int(settings, "bf", 0);                   // NVENC / x264 / AMF
	obs_data_set_bool(settings, "lookahead", false);       // NVENC

	// --- Audio: filter audio (the camera's own embedded audio) ---
	obs_data_set_bool(settings, "custom_audio_source", false);
	obs_data_set_string(settings, "audio_encoder", "ffmpeg_aac");
	obs_data_set_int(settings, "audio_bitrate", cfg.audioBitrateKbps);

	// --- Keep source resolution/fps (session format = camera format) ---
	obs_data_set_string(settings, "resolution", "");
	obs_data_set_int(settings, "fps_divider", 1);

	return settings;
}

obs_source_t *ensureFilter(obs_source_t *target, int camIndex, const Config &cfg)
{
	std::string filterName = std::string(kFilterNamePrefix) +
				 std::to_string(camIndex + 1);

	obs_source_t *filter =
		obs_source_get_filter_by_name(target, filterName.c_str());

	obs_data_t *settings = buildSettings(camIndex, cfg);

	if (!filter) {
		filter = obs_source_create_private(kFilterId, filterName.c_str(),
						   settings);
		if (filter) {
			// DISARM BEFORE THE FILTER IS EVEN ATTACHED.
			//
			// A source is born ENABLED, and an enabled Branch Output
			// filter is a running recording as soon as BO's own 1 s
			// timer looks at it (with Interlock on "Always ON" nothing
			// else gates it). So creating a project - which is only
			// meant to point the filters at a new folder - started the
			// take by itself: the operator typed a name, pressed Enter,
			// and BO wrote 'Starting recording output succeeded' 1.3 s
			// later, fifteen seconds before he pressed REC.
			//
			// That is not merely an early file: the tap only attaches
			// on REC, so those fifteen seconds are recorded with no
			// packets in the ring to anchor the file's opening against,
			// and the whole take ends "nothing anchored this run" -
			// unreplayable after the ring wraps.
			//
			// Doing it before obs_source_filter_add() leaves no window
			// at all for BO's timer to see it armed.
			obs_source_set_enabled(filter, false);
			obs_source_filter_add(target, filter);
			obs_log(LOG_INFO,
				"Added Branch Output filter '%s' to '%s' (disarmed — "
				"recording starts only on REC)",
				filterName.c_str(), obs_source_get_name(target));
		} else {
			obs_log(LOG_ERROR,
				"Failed to create Branch Output filter '%s'",
				filterName.c_str());
		}
	} else {
		obs_source_update(filter, settings);
		// Same rule for a RE-configured filter (New/Open Project, Settings):
		// this function is never called while recording (startRecording only
		// calls it for a filter that does not exist yet, and
		// reapplyFilterSettings refuses mid-take), so "configured" must never
		// mean "armed". Only startRecording() enables.
		obs_source_set_enabled(filter, false);
	}

	obs_data_release(settings);
	return filter; // caller releases
}

int pruneFilters(const Config &cfg)
{
	// EVERY FILTER WE OWN IS ACCOUNTED FOR, or the rig lies about itself.
	//
	// A filter is named after its SLOT ("MultiReplay cam3") and lives on the
	// source that slot pointed at when it was created. Nothing removed it when
	// the slot changed: open a project with three cameras, then a project with
	// two, and cam3's filter is still sitting on yesterday's source — armed by
	// nobody, invisible on this panel, and counted by anyone (the operator, the
	// health rules, Branch Output's own dock) who asks how many angles this
	// session records. Three declared, two armed.
	//
	// So a filter of ours is kept only when the slot it is named for is
	// configured AND names the very source it is attached to; anything else is
	// removed. Two references are taken during the enumeration and the removal
	// happens after it: obs_enum_sources holds libobs' source list while it
	// calls back, and taking a filter off a source inside that callback is a
	// re-entrancy nobody has to risk for a loop this short.
	struct Doomed {
		obs_source_t *target;
		obs_source_t *filter;
		std::string name;
	};
	struct Ctx {
		const Config *cfg;
		std::vector<Doomed> doomed;
	} ctx{&cfg, {}};

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *c = static_cast<Ctx *>(param);
			const char *sn = obs_source_get_name(source);
			for (int i = 0; i < kMaxCameras; i++) {
				const std::string name =
					std::string(kFilterNamePrefix) +
					std::to_string(i + 1);
				obs_source_t *f = obs_source_get_filter_by_name(
					source, name.c_str());
				if (!f)
					continue;
				const std::string &want = c->cfg->cameras[i].sourceName;
				const bool belongs = !want.empty() && sn && want == sn;
				if (belongs) {
					obs_source_release(f);
					continue;
				}
				c->doomed.push_back(
					{obs_source_get_ref(source), f, name});
			}
			return true;
		},
		&ctx);

	int removed = 0;
	for (Doomed &d : ctx.doomed) {
		// Disarm first: removing a filter Branch Output still considers
		// armed asks it to tear a running output down inside our call.
		obs_source_set_enabled(d.filter, false);
		if (d.target)
			obs_source_filter_remove(d.target, d.filter);
		obs_log(LOG_INFO,
			"Removed stale Branch Output filter '%s' from '%s' — that "
			"angle is not part of this project",
			d.name.c_str(),
			d.target ? obs_source_get_name(d.target) : "?");
		obs_source_release(d.filter);
		if (d.target)
			obs_source_release(d.target);
		removed++;
	}
	return removed;
}

void setEnabled(obs_source_t *filter, bool enabled)
{
	if (filter)
		obs_source_set_enabled(filter, enabled);
}

bool recordingOutputActive(int camIndex)
{
	// Same name the tap looks up: Branch Output creates its recording output
	// with the filter's name (plugin-stream-recording.cpp:
	// obs_output_create(outputId, qUtf8Printable(name), ...)), and its
	// streaming outputs carry a " (index)" suffix, so this cannot collide.
	const std::string name =
		std::string(kFilterNamePrefix) + std::to_string(camIndex + 1);

	obs_output_t *out = obs_get_output_by_name(name.c_str()); // add-ref'd
	if (!out)
		return false; // not created yet, or never will be
	const bool active = obs_output_active(out);
	obs_output_release(out);
	return active;
}

} // namespace branch_output
} // namespace multireplay
