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

#include "session-clock.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace multireplay {

// Verbose [ev]/[live] tracing is gated behind the env var OBS_MULTIREPLAY_DEBUG
// (set to anything but empty/"0"). Off by default → no per-event log spam in
// production. Use MR_DLOG(...) exactly like obs_log's format args.
bool debugLoggingEnabled();
#define MR_DLOG(...)                                       \
	do {                                               \
		if (::multireplay::debugLoggingEnabled())  \
			obs_log(LOG_INFO, __VA_ARGS__);    \
	} while (0)

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
	// Legacy: the replay is a plugin-provided OBS input the operator places
	// himself, so nothing drives a Media Source any more. Kept so an existing
	// config.json round-trips unchanged.
	std::string replaySourceName;
	std::string musicSourceName; // OBS audio source unmuted during playback
	bool autoSwitchScene = true; // play-to-output switches the OBS scene;
				     // false = only feed the Replay source
	// Make the replay fill the canvas, aspect preserved, wherever the
	// operator put it (ReplayChannel::applyCanvasFit). On by default: a
	// 720p camera in a 1080p project is otherwise composited at 2/3 size,
	// and since the replay input changes size with the angle no fixed
	// transform can be right for a mixed set. Off = hands off his transform.
	bool fitReplayToCanvas = true;
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
	// Is Branch Output really recording, on at least one armed camera?
	//
	// isRecording() only says WE armed the filters. Branch Output starts (or
	// declines to start) on its own, from a global Interlock setting we cannot
	// read — so this is the difference between "the GUI says REC" and "frames
	// are being captured". The dock uses it to fail the take loudly instead of
	// letting the operator mark events over nothing (see poll()).
	bool branchOutputRecording() const;

	// --- Operator state shared by the dock, the hotkeys and the coordinator ---
	// The playback engine holds none of this any more: ReplayChannel is told an
	// explicit (camera, in, out, speed) and knows nothing about "the current
	// angle" or "following live". Both are plain UI state, but the hotkeys have
	// to reach them without going through the dock, so they live here.
	int currentAngle() const { return currentAngle_.load(); } // 0-based
	void setCurrentAngle(int angle0)
	{
		currentAngle_.store(angle0 < 0 ? 0
					       : (angle0 >= kMaxCameras
							  ? kMaxCameras - 1
							  : angle0));
	}
	// the reference controller NOW: the preview mirrors the live camera instead of the replay.
	bool followLive() const { return followLive_.load(); }
	void setFollowLive(bool follow) { followLive_.store(follow); }

	// --- The session epoch: monotonic time ↔ wall time (session-clock.hpp) ---
	// Seated ONCE per process, in load(), and never re-sampled: everything
	// persisted this run (event markers, file anchors) is converted through
	// this one pair, so re-seating it mid-session would silently shift values
	// already written against the old pair. It is deliberately NOT tied to REC
	// either — opening yesterday's project has to convert its wall-clock marks
	// with nothing recording.
	SessionEpoch sessionEpoch() const
	{
		return {epochMasterNs_.load(), epochWallNs_.load()};
	}

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

	void seatSessionEpoch(); // sample both clocks once; see sessionEpoch()
	// (Re)point SegmentIndex at `folder` so the anchors written by earlier
	// sessions are read back. Must be called WITHOUT mutex_ held.
	void restartSegmentIndex(const std::string &folder);

	void loadConfig();
	void saveConfig() const;
	// Called with mutex_ held — returns recordingFolder without re-acquiring.
	std::string recordingFolderLocked() const;
	// Update Branch Output filter path on every configured camera source.
	// Must be called WITHOUT mutex_ held (ensureFilter reads recordingFolder).
	void reapplyFilterSettings();

	void registerReplayHotkeys(); // full broadcast-style hotkey set

	mutable std::mutex mutex_;
	Config config_;
	bool recording_ = false;
	std::atomic<int> currentAngle_{0};   // 0-based, see currentAngle()
	std::atomic<bool> followLive_{true}; // see followLive()
	std::atomic<int64_t> epochMasterNs_{0}; // see sessionEpoch()
	std::atomic<int64_t> epochWallNs_{0};
	std::array<CameraStatus, kMaxCameras> cameraStatus_{};
	obs_hotkey_id startHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id stopHotkey_ = OBS_INVALID_HOTKEY_ID;
	std::vector<obs_hotkey_id> extraHotkeys_;
};

} // namespace multireplay
