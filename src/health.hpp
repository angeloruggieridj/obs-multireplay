/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

HealthMonitor (M4) — the OBS-facing half of hardening: it collects the numbers,
health-rules.hpp judges them, and nobody acts on them but the operator.

Two jobs:

  preflight(cfg)  Before REC. Everything that can be known without recording:
                  Branch Output present, session folder writable, camera sources
                  really there, disk space and (if it was measured) disk write
                  bandwidth against the bitrate about to be demanded, and how
                  much packet ring actually fits in free RAM. Blockers refuse
                  the take inside ReplayCore::startRecording, so a REC from a
                  hotkey or the API is refused exactly like one from the button.
                  Warnings never refuse anything — they are shown and logged.

  sample()        During REC, once a second from the dock's poll. Packet flow per
                  angle, malformed packets and discontinuities, ring occupancy,
                  OBS frame drops, disk space, resident memory.

WHAT IT WILL NOT DO: touch the Program. It stops nothing, switches nothing and
restarts nothing — the only outputs are findings, a log line and a badge in the
dock. A rig that pulls itself off air because a disk got slow has converted a
recoverable problem into a broadcast one, and the operator, who is sitting right
there, was never asked.

The disk-bandwidth probe is the one thing here that writes: 8 MiB into the
session folder, flushed to the device (not just to the page cache, or it would
measure RAM and report a happy number), timed, deleted. It runs on a background
thread, never while recording, and caches its answer per folder.

THE SAMPLING THREAD IS NOT OPTIONAL (this froze OBS solid, once).
-----------------------------------------------------------------
Sampling reads PacketTap::stats(), which takes the tap's BIG lock — and the tap
holds that same lock across detach, where obs_output_stop() can block until
Branch Output has finished tearing its own output down, on the UI thread. So a
sample taken FROM the UI thread, at the moment an angle goes away, is a
three-way deadlock: UI waits for the tap lock, the tap waits for Branch Output,
Branch Output waits for the UI. Measured, from the very check that kills a
camera mid-take: OBS stopped dead with no shutdown lines in its log at all.

Hence: one sampler thread of our own, and it never holds this class's mutex
while it is inside the tap. What the dock does is read findings() — a copy,
under a lock nobody holds for longer than that copy. For the same reason the
sampler must not call into ReplayCore either: stopRecording() holds the core
lock while it joins this thread, so the folder it needs is cached at
takeStarted() instead of asked for every second.
*/

#pragma once

#include <obs-module.h>

#include "health-rules.hpp"
#include "replay-core.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace multireplay {

// A finding as the operator should read it: the locale string for `Health.<id>`
// (the id itself if the key is missing), with the numbers appended.
std::string findingText(const health::Finding &f);
// The findings at or above `min`, one per line, bulleted — what goes into the
// REC refusal box and the badge tooltip.
std::string findingsBlock(const std::vector<health::Finding> &findings,
			  health::Level min);

class HealthMonitor {
public:
	static HealthMonitor &instance();

	// --- pre-flight -----------------------------------------------------
	// Collect and judge. Read-only: it arms nothing and creates nothing.
	// `ringSecondsWanted` is the budget the take would like (RingBudget).
	health::PreflightResult preflight(const Config &cfg,
					  int ringSecondsWanted) const;
	// The last preflight, kept so the dock can show it after REC started.
	health::PreflightResult lastPreflight() const;
	void rememberPreflight(const health::PreflightResult &r);

	// --- runtime --------------------------------------------------------
	// Seat the baselines: arm time per camera, resident memory, frame
	// counters, the ring window the take was actually granted.
	// `sessionFolder` is handed in rather than read back from ReplayCore:
	// this is called from inside startRecording, which holds the core lock,
	// so asking for the config here is a self-deadlock (it froze OBS).
	// `canonical` is camera-dedup.hpp's canonicalCameraIndices(): a slot that
	// duplicates an earlier slot's source shares that slot's PacketTap
	// channel entirely (see packet-tap.hpp), so its ring bytes must be
	// counted once, under the canonical index, not once per duplicate —
	// otherwise the same physical ring would be added into the memory-growth
	// budget several times over and the "is this actually leaking" check
	// would be looking at a number nobody's RAM usage ever produces.
	void takeStarted(const std::array<bool, kMaxCameras> &armed,
			 const std::array<int, kMaxCameras> &canonical,
			 int ringSecondsGranted, int64_t requiredBytesPerSec,
			 const std::string &sessionFolder);
	void takeStopped();

	std::vector<health::Finding> findings() const;
	health::Level worst() const;
	// Evidence for the gate and the soak run.
	uint64_t samples() const { return samples_.load(); }
	uint64_t raised() const { return raised_.load(); }

	// --- disk bandwidth -------------------------------------------------
	// Measure once per folder, on a background thread, never while
	// recording. -1 until an answer exists.
	void probeDiskAsync(const std::string &folder);
	// Stop the sampler and JOIN the disk probe. Called from
	// obs_module_unload: the probe used to be a detached thread nobody ever
	// waited for, writing 8 MiB and then touching this object's state, and
	// this object is a function-local singleton in a DLL that is about to be
	// unloaded. It is bounded by one 8 MiB write.
	void shutdown();
	int64_t diskWriteBytesPerSec() const { return diskWriteBps_.load(); }
	std::string probedFolder() const;

private:
	HealthMonitor() = default;
	// Same backstop as EventStore: the sampler and the disk probe are members,
	// and a joinable std::thread at destruction is std::terminate.
	~HealthMonitor();
	HealthMonitor(const HealthMonitor &) = delete;
	HealthMonitor &operator=(const HealthMonitor &) = delete;

	// The sampler thread: one observation a second for as long as the take
	// runs. See the deadlock note at the top of this file for why it is a
	// thread and not a call from the dock's timer.
	void samplerLoop();
	void sampleOnce();
	// Log what appeared and what cleared, once each, never per tick.
	// Called with mutex_ held.
	void logDeltaLocked(const std::vector<health::Finding> &now);

	std::thread sampler_;
	std::atomic<bool> samplerRun_{false};

	mutable std::mutex mutex_;

	health::PreflightResult lastPreflight_;
	std::vector<health::Finding> findings_;
	std::vector<std::string> lastIds_;

	std::array<int64_t, kMaxCameras> armedAtNs_{};
	std::array<bool, kMaxCameras> armed_{};
	std::array<int, kMaxCameras> canonical_ = [] {
		std::array<int, kMaxCameras> id{};
		for (int i = 0; i < kMaxCameras; i++)
			id[i] = i;
		return id;
	}();
	bool recording_ = false;
	int64_t takeStartNs_ = 0;
	int64_t rssBaselineBytes_ = -1;
	int64_t requiredBytesPerSec_ = 0;
	int64_t targetRingSpanNs_ = 0;

	uint32_t lastLagged_ = 0;
	uint32_t lastTotal_ = 0;
	// The last kLaggedWindowSamples one-second deltas, oldest first, and the
	// count of consecutive windows that have been over the blocking ratio.
	// The rules keep nothing (that is what makes them testable), so the
	// window has to be kept here. See the note on kLaggedWarnPct for why one
	// second is not a window: at 30 fps its ratio is quantised to 3.3% and
	// the badge it drove flipped thirty-two times in two minutes on a run
	// whose real figure was 0.86%.
	std::deque<std::pair<uint32_t, uint32_t>> lagWindow_; // (lagged, total)
	int laggedWindowsOverBlock_ = 0;
	int64_t lastDiskCheckNs_ = 0;
	int64_t diskFreeBytes_ = -1;
	// Cached at takeStarted: asking ReplayCore for it every second would put
	// the sampler behind the core lock that stopRecording() holds while it
	// joins this very thread.
	std::string sessionFolder_;

	std::atomic<uint64_t> samples_{0};
	std::atomic<uint64_t> raised_{0};

	std::atomic<int64_t> diskWriteBps_{-1};
	std::string probedFolder_;
	std::atomic<bool> probeRunning_{false};
	// OWNED, not detached (see shutdown).
	std::thread probe_;
};

} // namespace multireplay
