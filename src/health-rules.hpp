/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Health rules (M4) — the two questions hardening has to answer, as pure
arithmetic over numbers somebody else collected.

    preflight()  before REC: can this take even work?
    runtime()    during REC: is it still working?

Both return FINDINGS, never actions. That is the M4 rule and it is enforced
here by having no other vocabulary: there is no "stop", no "switch", no
"restart" in this file, so no amount of degradation can reach the Program. A
replay rig that takes itself off air because a disk got slow has turned a
recoverable problem into a broadcast one. The operator is sitting right there;
tell him, loudly, and let him decide.

Why the split from health.cpp: everything here is integers and thresholds, so
it runs in `ctest` on any machine, with no OBS, no disk and no cameras — which
is the only way the thresholds ever get tested at all. The OBS side collects
(disk space, RAM, tap counters, frame counters) and formats for the UI.

A Finding carries a STABLE id, not a sentence. The id is what the gate asserts
on and what the locale file keys off (`Health.<id>`); `detail` is the numbers
that made it fire, in a form that means the same thing in every language.
*/

#pragma once

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace multireplay::health {

// Kept in step with ReplayCore::kMaxCameras by a static_assert in health.cpp
// (this header stays free of OBS headers, so it cannot see that constant).
inline constexpr int kMaxHealthAngles = 8;

enum class Level {
	Ok = 0,
	Info = 1,    // worth recording, not worth interrupting anyone for
	Warning = 2, // the take works, but something is degraded
	Blocker = 3, // before REC: refuse. During REC: this take is not usable.
};

struct Finding {
	Level level = Level::Info;
	std::string id;     // stable key: locale lookup + gate assertions
	std::string detail; // the numbers behind it, language-independent
};

// --- thresholds -------------------------------------------------------------
// Every number here is a judgement call, so each one says what it is protecting
// against. They are deliberately generous: a gate that cries wolf gets ignored,
// and this one talks to an operator during a live match.

// Disk: a take with two minutes of headroom is a take that dies mid-action.
inline constexpr int64_t kDiskCriticalSeconds = 120;
inline constexpr int64_t kDiskLowSeconds = 15 * 60;
// ...and once the take is running, "low" means something more urgent than it
// did at the door: there is no longer time to go and free 200 GB.
inline constexpr int64_t kDiskLowSecondsRunning = 5 * 60;
// Write bandwidth measured against what Branch Output is about to demand. Under
// 1.2× the rig cannot keep up at all; under 2× it survives a steady bitrate and
// nothing else (keyframes, a second application, an antivirus scan).
inline constexpr double kDiskBandwidthBlockRatio = 1.2;
inline constexpr double kDiskBandwidthWarnRatio = 2.0;
// RAM for the packet ring. 40% of what is free is a lot to ask for; it is also
// the whole product (the ring IS the live replay), so the honest move is to
// take it, cap there, and shrink the window rather than the machine.
inline constexpr int64_t kRingRamSharePct = 40;
// Below this the ring stops being a replay buffer and starts being a rumour:
// the −5/−10/−20 presets are the product, and −20 has to land inside it.
inline constexpr int kRingSecondsFloor = 25;
// Runtime: an angle whose packets stop for this long is not "jittery".
inline constexpr int64_t kAngleStallWarnMs = 2000;
inline constexpr int64_t kAngleDeadMs = 5000;
// Branch Output builds its infrastructure asynchronously; the tap retries.
inline constexpr int64_t kAttachGraceMs = 10000;
// OBS dropping frames is OBS's problem, but a replay rig is often what pushed
// it over, so it is reported here too.
//
// READ THIS BEFORE CHANGING THE NUMBERS. These are judged over a SLIDING
// WINDOW (health.cpp keeps the last kLaggedWindowSamples one-second samples and
// sums them), and the window is the point of the whole rule. Judged over a
// single second at 30 fps the ratio is quantised to 3.3% — one lagged frame is
// 3.3%, two are 6.7% — so no reading can ever land near a threshold: it is
// either zero or well past it. On a real session that produced a badge flipping
// blocker→clear THIRTY-TWO TIMES IN TWO MINUTES on a run whose true figure was
// 0.86%, and a badge that flickers is a badge nobody looks at on the day it
// means something. Five seconds of frames is 150 of them, i.e. 0.7% per frame,
// which is a scale a threshold can actually sit on.
//
// And it is a WARNING, not a blocker. A blocker means "this take cannot run";
// OBS shedding a few render frames means the machine is working hard, which is
// what a replay rig does. The escalation that matters is duration, not severity:
// sustained is what the operator needs told.
inline constexpr double kLaggedWarnPct = 1.0;
inline constexpr double kLaggedBlockPct = 5.0;
// How many one-second runtime samples the lag ratio is judged over.
inline constexpr int kLaggedWindowSamples = 5;
// ...and how many consecutive windows have to stay over kLaggedBlockPct before
// it is called sustained. Two windows = the machine has been behind for ten
// seconds, which no single burst can fake.
inline constexpr int kLaggedSustainedWindows = 2;
// Resident memory above ring + this much is a leak, not a buffer.
inline constexpr int64_t kMemorySlackBytes = 768LL * 1024 * 1024;
// A ring holding much less than it was budgeted for is being trimmed by the
// byte cap — the operator reaches back 30 s and finds 12.
inline constexpr int64_t kRingShortPct = 60;

// --- small helpers ----------------------------------------------------------

inline const char *levelName(Level l)
{
	switch (l) {
	case Level::Ok:
		return "ok";
	case Level::Info:
		return "info";
	case Level::Warning:
		return "warning";
	case Level::Blocker:
		return "blocker";
	}
	return "?";
}

inline Level worstOf(const std::vector<Finding> &findings)
{
	Level worst = Level::Ok;
	for (const auto &f : findings)
		if (f.level > worst)
			worst = f.level;
	return worst;
}

inline bool hasFinding(const std::vector<Finding> &findings, const char *id)
{
	for (const auto &f : findings)
		if (f.id == id)
			return true;
	return false;
}

// Worst first, so a UI with room for one line shows the line that matters.
// Stable within a level: the collector emits them in camera order.
inline void sortWorstFirst(std::vector<Finding> &findings)
{
	std::stable_sort(findings.begin(), findings.end(),
			 [](const Finding &a, const Finding &b) {
				 return a.level > b.level;
			 });
}

namespace detail {

inline std::string fmt(const char *format, ...)
{
	char buf[256];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	return std::string(buf);
}

inline std::string gb(int64_t bytes)
{
	return fmt("%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

inline std::string mbps(int64_t bytesPerSec)
{
	return fmt("%.0f Mb/s", (double)bytesPerSec * 8.0 / 1'000'000.0);
}

inline std::string mins(int64_t seconds)
{
	return fmt("%lld min", (long long)(seconds / 60));
}

} // namespace detail

// --- pre-flight -------------------------------------------------------------

struct PreflightInput {
	bool branchOutputAvailable = false;
	// §9.4(b) — only meaningful when branchOutputAvailable is true (see
	// branch-output-control.hpp: schemaCompatible()). Left true when the
	// check was never run, so an old health.cpp caller that never sets
	// this field reports nothing, not a false alarm.
	bool branchOutputSchemaCompatible = true;
	std::string branchOutputMissingKeys; // comma-joined, for the detail
	bool sessionFolderSet = false;
	bool sessionFolderWritable = false;
	int camerasConfigured = 0;    // slots with a source name
	int camerasWithSource = 0;    // ...whose source actually exists right now
	int duplicateSourceSlots = 0; // slots sharing one source with another slot
	bool encoderAvailable = true; // the configured encoder id is registered
	// -1 anywhere below means "not measured": unknown is reported as unknown,
	// never as fine.
	int64_t diskFreeBytes = -1;
	int64_t diskWriteBytesPerSec = -1;
	int64_t requiredBytesPerSec = 0; // what Branch Output is about to write
	int64_t freeRamBytes = -1;
	int ringSecondsWanted = 0;
	int64_t ringBytesPerSecond = 0; // aggregate, all armed cameras
};

struct PreflightResult {
	std::vector<Finding> findings;
	// What the ring should actually be given, once RAM is taken into
	// account. Equal to ringSecondsWanted unless it had to be cut.
	int ringSeconds = 0;

	Level worst() const { return worstOf(findings); }
	bool ok() const { return worst() < Level::Blocker; }
	bool has(const char *id) const { return hasFinding(findings, id); }
};

// How many seconds of ring fit in RAM. Unknown RAM or bitrate = leave the
// wanted value alone: guessing downward is its own kind of lie.
inline int ringSecondsThatFit(int wantSeconds, int64_t bytesPerSecond,
			      int64_t freeRamBytes)
{
	if (wantSeconds <= 0 || bytesPerSecond <= 0 || freeRamBytes <= 0)
		return wantSeconds;
	const int64_t budget = freeRamBytes / 100 * kRingRamSharePct;
	const int64_t fits = budget / bytesPerSecond;
	if (fits >= (int64_t)wantSeconds)
		return wantSeconds;
	return (int)std::max<int64_t>(fits, 0);
}

inline PreflightResult preflight(const PreflightInput &in)
{
	PreflightResult r;
	r.ringSeconds = in.ringSecondsWanted;
	auto add = [&r](Level l, const char *id, std::string det = {}) {
		r.findings.push_back({l, id, std::move(det)});
	};

	if (!in.branchOutputAvailable)
		add(Level::Blocker, "branch_output_missing");
	// §9.4(b) — Warning, not Blocker: a missing key means the field it
	// would have carried silently does nothing, which can still leave a
	// usable (if degraded) recording — very different from no recording
	// at all. Only judged when Branch Output IS present, or this would
	// double-report the same absence branch_output_missing already names.
	else if (!in.branchOutputSchemaCompatible)
		add(Level::Warning, "branch_output_schema_mismatch",
		    in.branchOutputMissingKeys);

	if (!in.sessionFolderSet)
		add(Level::Blocker, "session_folder_unset");
	else if (!in.sessionFolderWritable)
		add(Level::Blocker, "session_folder_unwritable");

	if (in.camerasConfigured <= 0)
		add(Level::Blocker, "no_camera_configured");
	else if (in.camerasWithSource <= 0)
		add(Level::Blocker, "no_camera_source_found");
	else if (in.camerasWithSource < in.camerasConfigured)
		add(Level::Warning, "camera_source_missing",
		    detail::fmt("%d/%d",
				in.camerasConfigured - in.camerasWithSource,
				in.camerasConfigured));

	// Info, not Warning: a later slot naming an earlier one's source shares
	// that slot's filter and encoder rather than building a second one
	// (camera-dedup.hpp / branch-output-control.hpp), so this is no longer a
	// resource cost — it is only worth telling the operator that two angle
	// buttons will show the same picture.
	if (in.duplicateSourceSlots > 0)
		add(Level::Info, "duplicate_camera_source",
		    detail::fmt("%d", in.duplicateSourceSlots));

	if (!in.encoderAvailable)
		add(Level::Warning, "encoder_unavailable");

	// --- disk space ---
	if (in.diskFreeBytes < 0) {
		add(Level::Info, "disk_space_unknown");
	} else if (in.requiredBytesPerSec > 0) {
		const int64_t secs = in.diskFreeBytes / in.requiredBytesPerSec;
		if (secs < kDiskCriticalSeconds)
			add(Level::Blocker, "disk_space_critical",
			    detail::gb(in.diskFreeBytes) + ", " +
				    detail::mins(secs));
		else if (secs < kDiskLowSeconds)
			add(Level::Warning, "disk_space_low",
			    detail::gb(in.diskFreeBytes) + ", " +
				    detail::mins(secs));
	}

	// --- disk bandwidth ---
	// Only judged when it was actually measured. An unmeasured disk gets no
	// finding at all: an Info on every single REC is noise, and noise is how
	// the findings that matter stop being read.
	if (in.diskWriteBytesPerSec > 0 && in.requiredBytesPerSec > 0) {
		const double ratio = (double)in.diskWriteBytesPerSec /
				     (double)in.requiredBytesPerSec;
		const std::string d = detail::mbps(in.diskWriteBytesPerSec) +
				      " vs " +
				      detail::mbps(in.requiredBytesPerSec);
		if (ratio < kDiskBandwidthBlockRatio)
			add(Level::Blocker, "disk_too_slow", d);
		else if (ratio < kDiskBandwidthWarnRatio)
			add(Level::Warning, "disk_margin_thin", d);
	}

	// --- RAM for the ring ---
	if (in.ringSecondsWanted > 0 && in.ringBytesPerSecond > 0 &&
	    in.freeRamBytes > 0) {
		const int fits = ringSecondsThatFit(in.ringSecondsWanted,
						    in.ringBytesPerSecond,
						    in.freeRamBytes);
		if (fits < kRingSecondsFloor) {
			// Even the floor does not fit: a ring this short cannot
			// serve the −20 s preset the product is built on, so this
			// is a refusal, not a degradation.
			add(Level::Blocker, "ring_ram_insufficient",
			    detail::gb(in.freeRamBytes) + " free, " +
				    detail::fmt("%d s wanted",
						in.ringSecondsWanted));
			r.ringSeconds = std::max(fits, 0);
		} else if (fits < in.ringSecondsWanted) {
			// Visible degradation: the take runs, with a shorter
			// memory, and the operator is told how much shorter.
			add(Level::Warning, "ring_ram_tight",
			    detail::fmt("%d s → %d s, ", in.ringSecondsWanted,
					fits) +
				    detail::gb(in.freeRamBytes) + " free");
			r.ringSeconds = fits;
		}
	}

	sortWorstFirst(r.findings);
	return r;
}

// --- runtime ----------------------------------------------------------------

struct AngleSample {
	bool armed = false;    // this take asked for it
	bool attached = false; // ...and the tap is on its encoder
	int64_t armedForMs = 0;
	// Since the newest packet arrived. -1 = none has ever arrived.
	int64_t sinceLastPacketMs = -1;
	uint64_t malformedPackets = 0;
	uint64_t discontinuities = 0;
	int64_t ringSpanNs = 0;
};

struct RuntimeInput {
	bool recording = false;
	int64_t takeElapsedMs = 0;
	std::array<AngleSample, kMaxHealthAngles> angles{};

	int64_t diskFreeBytes = -1;
	int64_t requiredBytesPerSec = 0;

	// Frames OBS lagged and frames it drew, summed over the caller's SLIDING
	// WINDOW (kLaggedWindowSamples seconds), not over the last second and not
	// over the take. See the note on kLaggedWarnPct for why the window has to
	// be wider than one second: at 30 fps a one-second ratio can only take the
	// values 0%, 3.3%, 6.7%… and a threshold on a scale like that is a switch,
	// not a measurement.
	uint32_t laggedFrames = 0;
	uint32_t totalFrames = 0;
	// How many consecutive windows have now been over kLaggedBlockPct,
	// counting this one. The caller keeps the count because these rules keep
	// nothing; 0 or 1 = a burst, kLaggedSustainedWindows or more = the machine
	// has genuinely been behind for that long and the wording says so.
	int laggedWindowsOverBlock = 0;

	int64_t rssBytes = -1;         // -1 = not measured
	int64_t rssBaselineBytes = -1; // resident size before the take
	int64_t ringBytesTotal = 0;
	int64_t targetRingSpanNs = 0;
};

// The state of the take, in findings. Called once a second while recording;
// idempotent, keeps nothing, decides nothing.
inline std::vector<Finding> runtime(const RuntimeInput &in)
{
	std::vector<Finding> out;
	if (!in.recording)
		return out;
	auto add = [&out](Level l, const char *id, std::string det = {}) {
		out.push_back({l, id, std::move(det)});
	};

	uint64_t malformed = 0, discontinuities = 0;
	for (int i = 0; i < kMaxHealthAngles; i++) {
		const AngleSample &a = in.angles[i];
		if (!a.armed)
			continue;
		const std::string cam = detail::fmt("CAM%d", i + 1);

		if (!a.attached) {
			// Retrying is normal for the first seconds; forever is not.
			if (a.armedForMs > kAttachGraceMs)
				add(Level::Warning, "angle_not_tapped", cam);
			continue;
		}
		if (a.sinceLastPacketMs < 0) {
			if (a.armedForMs > kAttachGraceMs)
				add(Level::Warning, "angle_no_packets", cam);
		} else if (a.sinceLastPacketMs >= kAngleDeadMs) {
			// This angle has stopped. Everything else keeps running:
			// one dead camera is not a dead take, and the operator
			// needs to know WHICH one before he cuts to it.
			add(Level::Blocker, "angle_dead",
			    cam + detail::fmt(", %lld s",
					      (long long)(a.sinceLastPacketMs /
							  1000)));
		} else if (a.sinceLastPacketMs >= kAngleStallWarnMs) {
			add(Level::Warning, "angle_stalled",
			    cam + detail::fmt(", %lld ms",
					      (long long)a.sinceLastPacketMs));
		}

		malformed += a.malformedPackets;
		discontinuities += a.discontinuities;

		// A ring far below its budget means the byte cap is evicting: the
		// operator's −20 s preset may reach past the end of memory. Only
		// once the take has run longer than the window it is compared to,
		// or every take would report it for its first half-minute.
		if (in.targetRingSpanNs > 0 && a.ringSpanNs > 0 &&
		    in.takeElapsedMs * 1'000'000LL >
			    in.targetRingSpanNs + 2'000'000'000LL &&
		    a.ringSpanNs * 100 < in.targetRingSpanNs * kRingShortPct)
			add(Level::Info, "ring_short",
			    cam + detail::fmt(", %lld s",
					      (long long)(a.ringSpanNs /
							  1'000'000'000LL)));
	}

	if (malformed > 0 || discontinuities > 0)
		add(Level::Warning, "timeline_broken",
		    detail::fmt("%llu malformed, %llu discontinuities",
				(unsigned long long)malformed,
				(unsigned long long)discontinuities));

	if (in.totalFrames > 0) {
		const double pct = 100.0 * (double)in.laggedFrames /
				   (double)in.totalFrames;
		if (pct >= kLaggedBlockPct) {
			// Warning, not blocker: nothing here can stop a take, and
			// a rig that sheds a few render frames is a rig under
			// load, not a rig that must not run. What is worth saying
			// is how LONG it has been like this — the detail carries
			// the ratio, the window it was measured over, and, once it
			// has held, that it is sustained rather than a burst.
			const bool sustained = in.laggedWindowsOverBlock >=
					       kLaggedSustainedWindows;
			add(Level::Warning, "obs_overloaded",
			    detail::fmt("%.1f%% over %ds%s", pct,
					kLaggedWindowSamples,
					sustained ? ", sustained" : ""));
		} else if (pct >= kLaggedWarnPct) {
			add(Level::Warning, "obs_dropping_frames",
			    detail::fmt("%.1f%% over %ds", pct,
					kLaggedWindowSamples));
		}
	}

	if (in.diskFreeBytes >= 0 && in.requiredBytesPerSec > 0) {
		const int64_t secs = in.diskFreeBytes / in.requiredBytesPerSec;
		if (secs < kDiskCriticalSeconds)
			add(Level::Blocker, "disk_space_critical",
			    detail::gb(in.diskFreeBytes) + ", " +
				    detail::mins(secs));
		else if (secs < kDiskLowSecondsRunning)
			add(Level::Warning, "disk_space_low",
			    detail::gb(in.diskFreeBytes) + ", " +
				    detail::mins(secs));
	}

	if (in.rssBytes > 0 && in.rssBaselineBytes > 0) {
		const int64_t growth = in.rssBytes - in.rssBaselineBytes;
		if (growth > in.ringBytesTotal + kMemorySlackBytes)
			add(Level::Warning, "memory_growth",
			    detail::gb(growth) + " over " +
				    detail::gb(in.ringBytesTotal) + " of ring");
	}

	sortWorstFirst(out);
	return out;
}

} // namespace multireplay::health
