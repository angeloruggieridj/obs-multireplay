/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "branch-output-control.hpp"
#include "replay-core.hpp"
#include "plugin-support.h"

#include <filesystem>
#include <string>

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
	std::string recPath = cfg.currentProjectName.empty()
				      ? cfg.sessionFolder
				      : (std::filesystem::path(cfg.sessionFolder) /
					 cfg.currentProjectName)
					        .string();
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
