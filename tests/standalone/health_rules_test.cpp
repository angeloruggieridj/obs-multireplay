/*
obs-multireplay — unit tests for the M4 health rules.
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, no disk, no cameras, so this runs in CI on every platform —
which is the only reason "the disk is nearly full" and "an angle died" are
testable at all. Both are states you cannot conveniently arrange on a laptop and
must never meet for the first time during a match.

The properties pinned here:

  - a take that cannot work is REFUSED (blocker), and one that merely runs
    degraded is NOT (warning) — that difference is the whole design;
  - a ring that does not fit in RAM is cut to what fits and the cut is
    reported, never granted silently;
  - unknown is reported as unknown, never as fine;
  - the runtime rules escalate on time (2 s stalled, 5 s dead) and name the
    camera they are about.
*/

#include "health-rules.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

using namespace multireplay::health;

static int g_fail = 0;

#define CHECK(cond)                                                         \
	do {                                                                \
		if (!(cond)) {                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, \
				    #cond);                                 \
			++g_fail;                                           \
		}                                                           \
	} while (0)

static constexpr int64_t kGB = 1024LL * 1024 * 1024;

// The numbers a finding carries. `id` is what the gate asserts on and what the
// locale key is built from; `detail` is what the operator reads, and for the
// overload rule it is the only place the difference between "a burst" and "this
// machine has been behind for ten seconds" is written down.
static std::string detailOf(const std::vector<Finding> &findings, const char *id)
{
	for (const auto &f : findings)
		if (f.id == id)
			return f.detail;
	return std::string();
}

// A rig that should sail through: Branch Output there, folder writable, two
// cameras present, ~25 Mbit/s of demand, a fast disk, 8 GB free RAM.
static PreflightInput healthyRig()
{
	PreflightInput in;
	in.branchOutputAvailable = true;
	in.sessionFolderSet = true;
	in.sessionFolderWritable = true;
	in.camerasConfigured = 2;
	in.camerasWithSource = 2;
	in.encoderAvailable = true;
	in.diskFreeBytes = 500 * kGB;
	in.diskWriteBytesPerSec = 400LL * 1024 * 1024; // 400 MB/s NVMe
	in.requiredBytesPerSec = 3LL * 1024 * 1024;    // ~25 Mbit/s
	in.ringBytesPerSecond = 3LL * 1024 * 1024;
	in.freeRamBytes = 8 * kGB;
	in.ringSecondsWanted = 90;
	return in;
}

static RuntimeInput healthyTake()
{
	RuntimeInput in;
	in.recording = true;
	in.takeElapsedMs = 60'000;
	in.targetRingSpanNs = 90'000'000'000LL;
	in.diskFreeBytes = 500 * kGB;
	in.requiredBytesPerSec = 3LL * 1024 * 1024;
	in.laggedFrames = 0;
	in.totalFrames = 300;
	in.rssBytes = 900LL * 1024 * 1024;
	in.rssBaselineBytes = 600LL * 1024 * 1024;
	in.ringBytesTotal = 200LL * 1024 * 1024;
	for (int i = 0; i < 2; i++) {
		in.angles[i].armed = true;
		in.angles[i].attached = true;
		in.angles[i].armedForMs = 60'000;
		in.angles[i].sinceLastPacketMs = 140; // the measured live edge
		in.angles[i].ringSpanNs = 88'000'000'000LL;
	}
	return in;
}

// --- pre-flight -------------------------------------------------------------

static void test_healthy_rig_is_silent()
{
	const PreflightResult r = preflight(healthyRig());
	CHECK(r.findings.empty());
	CHECK(r.ok());
	CHECK(r.worst() == Level::Ok);
	// Nothing was cut: it asked for 90 s and 90 s fit.
	CHECK(r.ringSeconds == 90);
}

static void test_missing_prerequisites_block()
{
	{
		PreflightInput in = healthyRig();
		in.branchOutputAvailable = false;
		const PreflightResult r = preflight(in);
		CHECK(!r.ok());
		CHECK(r.has("branch_output_missing"));
	}
	{
		PreflightInput in = healthyRig();
		in.sessionFolderSet = false;
		const PreflightResult r = preflight(in);
		CHECK(!r.ok());
		CHECK(r.has("session_folder_unset"));
		// One complaint about the folder, not two: "unset" and
		// "unwritable" said together read like two separate faults.
		CHECK(!r.has("session_folder_unwritable"));
	}
	{
		PreflightInput in = healthyRig();
		in.sessionFolderWritable = false;
		const PreflightResult r = preflight(in);
		CHECK(!r.ok());
		CHECK(r.has("session_folder_unwritable"));
	}
	{
		PreflightInput in = healthyRig();
		in.camerasConfigured = 0;
		in.camerasWithSource = 0;
		const PreflightResult r = preflight(in);
		CHECK(!r.ok());
		CHECK(r.has("no_camera_configured"));
	}
	{
		// Configured, but every source has gone (scene collection
		// changed, capture card unplugged).
		PreflightInput in = healthyRig();
		in.camerasWithSource = 0;
		const PreflightResult r = preflight(in);
		CHECK(!r.ok());
		CHECK(r.has("no_camera_source_found"));
	}
}

static void test_branch_output_schema_mismatch_warns()
{
	// §9.4(b) — a settings key ensureFilter() writes that Branch Output no
	// longer declares a default for: degrade, do not block. A rig that
	// cannot see Branch Output at all is a different, harder fault
	// (branch_output_missing) and must not ALSO report this one.
	PreflightInput in = healthyRig();
	in.branchOutputSchemaCompatible = false;
	in.branchOutputMissingKeys = "rec_format";
	const PreflightResult r = preflight(in);
	CHECK(r.ok());
	CHECK(r.worst() == Level::Warning);
	CHECK(r.has("branch_output_schema_mismatch"));

	in = healthyRig();
	in.branchOutputAvailable = false;
	in.branchOutputSchemaCompatible = false; // as health.cpp leaves it: unset
	const PreflightResult r2 = preflight(in);
	CHECK(r2.has("branch_output_missing"));
	CHECK(!r2.has("branch_output_schema_mismatch"));
}

static void test_partial_rig_warns_but_records()
{
	// One of two cameras missing is still a take worth having: the reference controller records
	// the angle that is there, and refusing would be worse than the fault.
	PreflightInput in = healthyRig();
	in.camerasWithSource = 1;
	const PreflightResult r = preflight(in);
	CHECK(r.ok());
	CHECK(r.worst() == Level::Warning);
	CHECK(r.has("camera_source_missing"));

	in = healthyRig();
	in.duplicateSourceSlots = 1;
	const PreflightResult dup = preflight(in);
	CHECK(dup.ok());
	CHECK(dup.has("duplicate_camera_source"));

	in = healthyRig();
	in.encoderAvailable = false;
	const PreflightResult enc = preflight(in);
	CHECK(enc.ok());
	CHECK(enc.has("encoder_unavailable"));
}

static void test_disk_space_thresholds()
{
	PreflightInput in = healthyRig();
	// One minute of headroom: refuse.
	in.diskFreeBytes = in.requiredBytesPerSec * 60;
	const PreflightResult crit = preflight(in);
	CHECK(!crit.ok());
	CHECK(crit.has("disk_space_critical"));
	// The numbers that made it fire travel with it.
	for (const auto &f : crit.findings)
		if (f.id == "disk_space_critical")
			CHECK(!f.detail.empty());

	// Ten minutes: it runs, and it says so.
	in.diskFreeBytes = in.requiredBytesPerSec * 600;
	const PreflightResult low = preflight(in);
	CHECK(low.ok());
	CHECK(low.has("disk_space_low"));

	// An hour: nothing to say.
	in.diskFreeBytes = in.requiredBytesPerSec * 3600;
	CHECK(preflight(in).findings.empty());

	// Unknown is reported as unknown, and does not block.
	in = healthyRig();
	in.diskFreeBytes = -1;
	const PreflightResult unknown = preflight(in);
	CHECK(unknown.ok());
	CHECK(unknown.has("disk_space_unknown"));
	CHECK(unknown.worst() == Level::Info);
}

static void test_disk_bandwidth_thresholds()
{
	PreflightInput in = healthyRig();
	// A disk that can just about write what we demand cannot also write the
	// keyframes, the muxer's flushes and whatever else that machine runs.
	in.diskWriteBytesPerSec = in.requiredBytesPerSec; // 1.0×
	CHECK(preflight(in).has("disk_too_slow"));
	CHECK(!preflight(in).ok());

	in.diskWriteBytesPerSec = in.requiredBytesPerSec * 3 / 2; // 1.5×
	const PreflightResult thin = preflight(in);
	CHECK(thin.ok());
	CHECK(thin.has("disk_margin_thin"));

	in.diskWriteBytesPerSec = in.requiredBytesPerSec * 4; // 4×
	CHECK(preflight(in).findings.empty());

	// Never measured: no finding at all. An Info on every single REC is
	// noise, and noise is how the findings that matter stop being read.
	in.diskWriteBytesPerSec = -1;
	CHECK(preflight(in).findings.empty());
}

static void test_ring_is_cut_to_fit_and_says_so()
{
	// 3 MB/s of packets, 90 s wanted = 270 MB. With 512 MB free, 40% is
	// ~205 MB, so about 68 s fit: shorten, warn, run.
	PreflightInput in = healthyRig();
	in.freeRamBytes = 512LL * 1024 * 1024;
	const PreflightResult tight = preflight(in);
	CHECK(tight.ok());
	CHECK(tight.has("ring_ram_tight"));
	CHECK(tight.ringSeconds > 0);
	CHECK(tight.ringSeconds < in.ringSecondsWanted);
	CHECK(tight.ringSeconds >= kRingSecondsFloor);

	// 128 MB free: ~51 MB of budget, ~17 s — under the floor, so the −20 s
	// preset the product is built on could not be served. Refuse.
	in.freeRamBytes = 128LL * 1024 * 1024;
	const PreflightResult starved = preflight(in);
	CHECK(!starved.ok());
	CHECK(starved.has("ring_ram_insufficient"));

	// Unknown RAM leaves the wanted value alone rather than guessing down.
	in.freeRamBytes = -1;
	const PreflightResult unknown = preflight(in);
	CHECK(unknown.ringSeconds == in.ringSecondsWanted);
	CHECK(!unknown.has("ring_ram_tight"));

	CHECK(ringSecondsThatFit(90, 0, 8 * kGB) == 90);
	CHECK(ringSecondsThatFit(90, 3LL * 1024 * 1024, 0) == 90);
	CHECK(ringSecondsThatFit(90, 3LL * 1024 * 1024, 8 * kGB) == 90);
}

static void test_worst_comes_first()
{
	// A blocker and two warnings at once: the head of the list is what a
	// one-line badge shows, so it has to be the blocker.
	PreflightInput in = healthyRig();
	in.camerasWithSource = 1;                       // warning
	in.encoderAvailable = false;                    // warning
	in.diskFreeBytes = in.requiredBytesPerSec * 30; // blocker
	const PreflightResult r = preflight(in);
	CHECK(!r.ok());
	CHECK(r.findings.size() >= 3);
	CHECK(r.findings.front().level == Level::Blocker);
	CHECK(r.worst() == Level::Blocker);
}

// --- runtime ----------------------------------------------------------------

static void test_healthy_take_is_silent()
{
	CHECK(runtime(healthyTake()).empty());
}

static void test_not_recording_says_nothing()
{
	RuntimeInput in = healthyTake();
	in.recording = false;
	in.angles[0].sinceLastPacketMs = 60'000; // dead, but there is no take
	CHECK(runtime(in).empty());
}

static void test_angle_stall_escalates_and_names_the_camera()
{
	RuntimeInput in = healthyTake();
	in.angles[1].sinceLastPacketMs = 2500;
	auto f = runtime(in);
	CHECK(hasFinding(f, "angle_stalled"));
	CHECK(worstOf(f) == Level::Warning);
	CHECK(f.front().detail.rfind("CAM2", 0) == 0);

	in.angles[1].sinceLastPacketMs = 8000;
	f = runtime(in);
	CHECK(hasFinding(f, "angle_dead"));
	CHECK(!hasFinding(f, "angle_stalled"));
	CHECK(worstOf(f) == Level::Blocker);
	CHECK(f.front().detail.rfind("CAM2", 0) == 0);

	// ...and CAM1, which is fine, is not mentioned: a warning that names
	// every angle is a warning that names none.
	CHECK(f.size() == 1);
}

static void test_a_dead_angle_is_not_a_dead_take()
{
	// The rules can say "this angle is gone" and nothing else: there is no
	// field in the result that could stop, switch or restart anything. This
	// test exists to fail loudly if that ever changes.
	RuntimeInput in = healthyTake();
	in.angles[0].sinceLastPacketMs = 30'000;
	const auto f = runtime(in);
	CHECK(hasFinding(f, "angle_dead"));
	// The other angle keeps its silence, i.e. it keeps recording.
	CHECK(f.size() == 1);
	static_assert(std::is_same_v<decltype(runtime(in)), std::vector<Finding>>,
		      "runtime() must return findings and nothing else");
}

static void test_attach_grace_then_complaint()
{
	RuntimeInput in = healthyTake();
	in.angles[1].attached = false;
	in.angles[1].armedForMs = 3000; // Branch Output is still coming up
	CHECK(runtime(in).empty());

	in.angles[1].armedForMs = 20'000;
	const auto f = runtime(in);
	CHECK(hasFinding(f, "angle_not_tapped"));

	// Attached but never a single packet is a different fault, and reads
	// differently: the encoder is there, the frames are not.
	RuntimeInput noPackets = healthyTake();
	noPackets.angles[0].sinceLastPacketMs = -1;
	noPackets.angles[0].armedForMs = 20'000;
	CHECK(hasFinding(runtime(noPackets), "angle_no_packets"));
}

static void test_timeline_and_frame_drops()
{
	RuntimeInput in = healthyTake();
	in.angles[0].malformedPackets = 3;
	CHECK(hasFinding(runtime(in), "timeline_broken"));

	in = healthyTake();
	in.angles[1].discontinuities = 1;
	CHECK(hasFinding(runtime(in), "timeline_broken"));

	in = healthyTake();
	in.laggedFrames = 6; // 2% of 300
	CHECK(hasFinding(runtime(in), "obs_dropping_frames"));
	in.laggedFrames = 30; // 10%
	CHECK(hasFinding(runtime(in), "obs_overloaded"));
	// A WARNING, not a blocker, and this assertion is the point. A blocker
	// means "this take cannot run"; OBS shedding render frames means the
	// machine is working hard, which is what a replay rig does. Nothing in
	// these rules acts, so a blocker here bought nothing but a red badge over
	// a take that was recording perfectly.
	CHECK(worstOf(runtime(in)) == Level::Warning);

	// The counts handed in are the SUM OF THE WINDOW, and the wording has to
	// tell a burst apart from a machine that has been behind for ten seconds.
	// Same ratio, different thing to do about it.
	in.laggedWindowsOverBlock = 1;
	CHECK(detailOf(runtime(in), "obs_overloaded").find("sustained") ==
	      std::string::npos);
	in.laggedWindowsOverBlock = kLaggedSustainedWindows;
	CHECK(detailOf(runtime(in), "obs_overloaded").find("sustained") !=
	      std::string::npos);
}

static void test_ring_short_only_once_the_window_has_passed()
{
	RuntimeInput in = healthyTake();
	in.angles[0].ringSpanNs = 20'000'000'000LL; // 20 s of a 90 s budget
	// 60 s into the take the ring cannot be full yet: saying "short" here
	// would fire on every take, for its first minute and a half.
	CHECK(!hasFinding(runtime(in), "ring_short"));

	in.takeElapsedMs = 200'000; // well past the window
	const auto f = runtime(in);
	CHECK(hasFinding(f, "ring_short"));
	CHECK(worstOf(f) == Level::Info);
}

static void test_memory_growth_is_measured_against_the_ring()
{
	RuntimeInput in = healthyTake();
	// The ring itself is not a leak, however big it gets.
	in.ringBytesTotal = 2LL * kGB;
	in.rssBytes = in.rssBaselineBytes + 2LL * kGB;
	CHECK(!hasFinding(runtime(in), "memory_growth"));

	// A gigabyte nobody can account for is.
	in.rssBytes = in.rssBaselineBytes + 2LL * kGB + kMemorySlackBytes + kGB;
	CHECK(hasFinding(runtime(in), "memory_growth"));

	// Unmeasured resident size says nothing at all.
	in.rssBytes = -1;
	CHECK(!hasFinding(runtime(in), "memory_growth"));
}

static void test_disk_fills_up_during_the_take()
{
	RuntimeInput in = healthyTake();
	in.diskFreeBytes = in.requiredBytesPerSec * 240; // 4 minutes left
	CHECK(hasFinding(runtime(in), "disk_space_low"));
	in.diskFreeBytes = in.requiredBytesPerSec * 60; // one minute
	CHECK(hasFinding(runtime(in), "disk_space_critical"));
	CHECK(worstOf(runtime(in)) == Level::Blocker);
}

int main()
{
	test_healthy_rig_is_silent();
	test_missing_prerequisites_block();
	test_branch_output_schema_mismatch_warns();
	test_partial_rig_warns_but_records();
	test_disk_space_thresholds();
	test_disk_bandwidth_thresholds();
	test_ring_is_cut_to_fit_and_says_so();
	test_worst_comes_first();

	test_healthy_take_is_silent();
	test_not_recording_says_nothing();
	test_angle_stall_escalates_and_names_the_camera();
	test_a_dead_angle_is_not_a_dead_take();
	test_attach_grace_then_complaint();
	test_timeline_and_frame_drops();
	test_ring_short_only_once_the_window_has_passed();
	test_memory_growth_is_measured_against_the_ring();
	test_disk_fills_up_during_the_take();

	if (g_fail == 0)
		std::printf("OK: all health-rule tests passed\n");
	return g_fail == 0 ? 0 : 1;
}
