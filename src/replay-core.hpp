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
*/

#pragma once

#include <obs-module.h>

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace multireplay {

constexpr int kMaxCameras = 8; // M5: full reference parity (was 4 in v1)
constexpr int kDefaultPort = 8456;
constexpr int kDefaultSplitMinutes = 20;
constexpr int kDefaultVideoBitrateKbps = 12000;
constexpr int kDefaultAudioBitrateKbps = 256;

struct CameraConfig {
	std::string sourceName; // OBS source acting as this camera ("" = unused)
};

struct Config {
	std::string sessionFolder;
	int port = kDefaultPort;
	int splitMinutes = kDefaultSplitMinutes;
	int videoBitrateKbps = kDefaultVideoBitrateKbps;
	int audioBitrateKbps = kDefaultAudioBitrateKbps;
	std::string videoEncoderId; // "" = auto-detect best hardware encoder
	std::string recFormat = "hybrid_mp4";
	std::string outputSceneName; // scene switched to program on "to output"
	std::string musicSourceName; // OBS audio source unmuted during playback
	std::array<CameraConfig, kMaxCameras> cameras;
};

struct CameraStatus {
	int index = 0;
	std::string sourceName;
	bool configured = false;
	bool sourceFound = false;
	bool filterPresent = false;
	bool recording = false;
	uint64_t startTimestampNs = 0; // os_gettime_ns() at enable, for alignment
};

class ReplayCore {
public:
	static ReplayCore &instance();

	void load();   // called from obs_module_load
	void unload(); // called from obs_module_unload

	// --- Recording control (API: recording.start / recording.stop) ---
	bool startRecording(std::string &errorOut);
	bool stopRecording();
	bool isRecording() const { return recording_; }

	// the reference controller "Delete All": wipe recordings + events in the session folder,
	// keep all settings. Refuses while recording.
	bool deleteAllSession(std::string &errorOut);

	// Branch Output filters are persisted (enabled) in the scene
	// collection and would auto-record on OBS startup: disable them all
	// until the operator presses REC. Called from obs_module_post_load.
	void disarmPersistedFilters();

	// --- Config ---
	Config getConfig() const;
	void setConfig(const Config &cfg);

	// --- Introspection for the web UI ---
	std::string statusJson() const;
	std::string sourcesJson() const;      // video sources usable as cameras
	std::string encodersJson() const;     // available video encoder ids
	bool branchOutputAvailable() const;

	// Best available encoder: hardware first, x264 fallback.
	std::string pickVideoEncoder() const;

	// Master-timeline "now" while recording (ns since the earliest camera
	// start of this recording run), -1 when not recording. Used by the
	// event system in Live mode (the reference controller "as it happens" marking).
	int64_t masterNowNs() const;

	// Disk space remaining in session folder (bytes), -1 if unknown.
	int64_t diskFreeBytes() const;
	// Estimated remaining recording minutes at current aggregate bitrate.
	int64_t estimatedMinutesRemaining() const;

private:
	ReplayCore() = default;

	void loadConfig();
	void saveConfig() const;
	void writeSessionManifest() const;

	mutable std::mutex mutex_;
	Config config_;
	bool recording_ = false;
	uint64_t sessionStartMinNs_ = 0; // earliest camera start (monotonic)
	std::array<CameraStatus, kMaxCameras> cameraStatus_{};
	obs_hotkey_id startHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id stopHotkey_ = OBS_INVALID_HOTKEY_ID;
};

} // namespace multireplay
