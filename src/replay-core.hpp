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
#include <cstdint>

namespace multireplay {

constexpr int kMaxCameras = 8; // M5: full reference parity (was 4 in v1)
constexpr int kDefaultPort = 8456;
constexpr int kDefaultSplitMinutes = 20;
constexpr int kDefaultVideoBitrateKbps = 12000;
constexpr int kDefaultAudioBitrateKbps = 256;

struct CameraConfig {
	std::string sourceName;  // OBS source acting as this camera ("" = unused)
	std::string displayName; // user-defined label shown on angle chips/buttons
};

struct Config {
	std::string sessionFolder;
	std::string currentProjectName; // "" = write directly to sessionFolder
	int port = kDefaultPort;
	int splitMinutes = kDefaultSplitMinutes;
	int videoBitrateKbps = kDefaultVideoBitrateKbps;
	int audioBitrateKbps = kDefaultAudioBitrateKbps;
	std::string videoEncoderId; // "" = auto-detect best hardware encoder
	std::string recFormat = "hybrid_mp4";
	std::string outputSceneName; // scene switched to program on "to output"
	// OBS Media Source the plugin drives for replay. Put one in the output
	// scene and pick it here, so the replay actually feeds that scene.
	// Empty = the plugin's own managed "MultiReplay — Replay A" source.
	std::string replaySourceName;
	std::string musicSourceName; // OBS audio source unmuted during playback
	// Encoder-startup latency (ms): the file's first frame is written this
	// long AFTER the camera is armed, so the recording file lags the master
	// (wall-clock) timeline by this much. Live event marks subtract it so the
	// replayed in/out match the moment the operator saw. Auto-learned at each
	// stopRecording() from (wall elapsed − finalized file duration).
	int replayOffsetMs = 0;
	bool autoSwitchScene = true; // play-to-output switches the OBS scene;
				     // false = only feed the Replay source
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
	// Monotonic ns of the earliest camera arm in the current session.
	int64_t sessionMonoStartNs() const { return sessionMonoStartNs_; }
	// Auto-calibrate replayOffsetMs from the just-finished session: given the
	// finalized indexed footage length, the offset ≈ wall elapsed − footage.
	// Called after STOP once the index has been refreshed. Persists config.
	void learnReplayOffset(int64_t indexedFootageNs);
	int replayOffsetMs() const;
	// Cumulative footage ns at the START of the current session (0 if first).
	// Add to live elapsed time to get a cumulative master-timeline position.
	int64_t sessionBaseNs() const { return sessionBaseNs_; }

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

	// --- Project management ---
	// Returns sessionFolder/currentProjectName, or sessionFolder if no project.
	std::string recordingFolder() const;
	// Create a new project subfolder under sessionFolder; sets it as current.
	bool newProject(const std::string &title, std::string &errorOut);
	// Switch to an existing project subfolder; reloads events + session.
	bool openProject(const std::string &folderName, std::string &errorOut);
	// List non-hidden subdirectories of sessionFolder (potential projects).
	std::vector<std::string> listProjects() const;

	// --- Introspection for the web UI ---
	std::string statusJson() const;
	std::string sourcesJson() const;      // video sources usable as cameras
	std::string encodersJson() const;     // available video encoder ids
	bool branchOutputAvailable() const;

	// Best available encoder: hardware first, x264 fallback.
	std::string pickVideoEncoder() const;

	// Disk space remaining in session folder (bytes), -1 if unknown.
	int64_t diskFreeBytes() const;
	// Estimated remaining recording minutes at current aggregate bitrate.
	int64_t estimatedMinutesRemaining() const;

private:
	ReplayCore() = default;

	void loadConfig();
	void saveConfig() const;
	void writeSessionManifest() const;
	// Called with mutex_ held — returns recordingFolder without re-acquiring.
	std::string recordingFolderLocked() const;
	// Update Branch Output filter path on every configured camera source.
	// Must be called WITHOUT mutex_ held (ensureFilter reads recordingFolder).
	void reapplyFilterSettings();

	void registerReplayHotkeys(); // full broadcast-style hotkey set

	mutable std::mutex mutex_;
	Config config_;
	bool recording_ = false;
	// Wall-clock Unix time (seconds) of the last startRecording() call.
	// Written into session.json so SessionIndex can filter out stale segment
	// files from previous recording sessions that share the same folder.
	int64_t sessionWallStartSec_ = 0;
	// Monotonic (os_gettime_ns) timestamp of the earliest camera arm in the
	// current recording session. Aligns with SessionIndex minStart.
	int64_t sessionMonoStartNs_ = 0;
	// Cumulative master-timeline offset at the START of the current session:
	// sum of all previous sessions' footage within this project (0 for first).
	// Captured from footageDurationNs() just before startRecording() clears.
	int64_t sessionBaseNs_ = 0;
	// Wall-clock ns elapsed in the current session at the moment STOP was
	// pressed (os_gettime_ns − sessionMonoStartNs_); used to learn the offset.
	int64_t stopElapsedNs_ = 0;
	std::array<CameraStatus, kMaxCameras> cameraStatus_{};
	obs_hotkey_id startHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id stopHotkey_ = OBS_INVALID_HOTKEY_ID;
	std::vector<obs_hotkey_id> extraHotkeys_;
};

} // namespace multireplay
