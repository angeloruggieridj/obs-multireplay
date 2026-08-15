/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multireplay {

// Exports events to standalone MP4 files (the reference controller "Export Clips" /
// ReplayExportLastEvent). Stream-copy remux: packets are copied from the
// recording segments without re-encoding, so exports are nearly instant and
// can run while recording. The clip starts at the keyframe at/before the
// event In point (1s GOP -> max 1s of pre-roll).
class ExportManager {
public:
	static ExportManager &instance();

	// Queue one event+angle for export. Empty folder = "<session>/export".
	bool exportEvent(int eventId, int angle1Based,
			 const std::string &customFolder,
			 std::string &errorOut);
	bool exportLastEvent(const std::string &customFolder,
			     std::string &errorOut);

	std::string statusJson() const;

private:
	ExportManager() = default;

	struct Job {
		int eventId;
		int angle;            // 0-based
		int64_t tInNs, tOutNs; // master timeline
		// The speed THIS angle is marked at (1.0 = untouched). A replay
		// the operator set to 50% is a 50% replay wherever it goes: an
		// export that quietly ran at 100% handed him a different clip
		// from the one he had just watched.
		double speed = 1.0;
		std::string outPath;
		std::string state = "queued"; // queued|running|done|failed
		std::string detail;
	};

	void worker();
	bool runJob(Job &job);

	mutable std::mutex mutex_;
	std::vector<Job> jobs_;
	std::thread thread_;
	std::atomic<bool> workerRunning_{false};
};

} // namespace multireplay
