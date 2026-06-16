/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "branch-output-control.hpp"
#include "replay-core.hpp"
#include "plugin-support.h"

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
	obs_data_set_string(settings, "path",
			    ReplayCore::instance().recordingFolder().c_str());
	std::string nameFormat = "cam" + std::to_string(camIndex + 1) +
				 "_%CCYY-%MM-%DD_%hh-%mm-%ss";
	obs_data_set_string(settings, "filename_formatting", nameFormat.c_str());
	obs_data_set_bool(settings, "no_space_filename", true);

	// --- Container: Hybrid MP4 (crash-safe, chapter markers) ---
	obs_data_set_string(settings, "rec_format", cfg.recFormat.c_str());

	// --- Automatic split by time (broadcast replay splits every 20 minutes) ---
	obs_data_set_string(settings, "split_file", "by_time");
	obs_data_set_int(settings, "split_file_time_mins", cfg.splitMinutes);

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
			obs_source_filter_add(target, filter);
			obs_log(LOG_INFO, "Added Branch Output filter '%s' to '%s'",
				filterName.c_str(), obs_source_get_name(target));
		} else {
			obs_log(LOG_ERROR,
				"Failed to create Branch Output filter '%s'",
				filterName.c_str());
		}
	} else {
		obs_source_update(filter, settings);
	}

	obs_data_release(settings);
	return filter; // caller releases
}

void setEnabled(obs_source_t *filter, bool enabled)
{
	if (filter)
		obs_source_set_enabled(filter, enabled);
}

} // namespace branch_output
} // namespace multireplay
