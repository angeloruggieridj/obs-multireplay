/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "replay-core.hpp"
#include "branch-output-control.hpp"
#include "camera-dedup.hpp"
#include "event-store.hpp"
#include "health.hpp"
#include "playback-coordinator.hpp"
#include "packet-tap.hpp"
#include "path-utf8.hpp"
#include "plugin-support.h"
#include "project-name.hpp"
#include "replay-channel.hpp"
#include "segment-index.hpp"
#include "version-compare.hpp"

#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace multireplay {

namespace {
std::atomic<bool> g_verboseLog{false};
}

bool debugLoggingEnabled()
{
	static const bool env = []() {
		const char *v = getenv("OBS_MULTIREPLAY_DEBUG");
		return v && *v && *v != '0';
	}();
	return env || g_verboseLog.load(std::memory_order_relaxed);
}

void setVerboseLogging(bool on)
{
	g_verboseLog.store(on, std::memory_order_relaxed);
}

namespace {

constexpr const char *kConfigFile = "config.json";
constexpr const char *kProjectSettingsFile = "settings.json";

// Hardware encoders in order of preference, then software fallback.
// Ids as registered by OBS Studio encoder plugins.
constexpr const char *kEncoderPreference[] = {
	"jim_nvenc",          // NVIDIA NVENC H.264 (new)
	"ffmpeg_nvenc",       // NVIDIA NVENC H.264 (legacy)
	"obs_qsv11_v2",       // Intel QuickSync H.264
	"obs_qsv11",          // Intel QuickSync H.264 (legacy)
	"h264_texture_amf",   // AMD AMF H.264
	"amd_amf_h264",       // AMD AMF H.264 (legacy)
	"ffmpeg_vaapi",       // VAAPI (Linux iGPU)
	"com.apple.videotoolbox.videoencoder.ave.avc", // Apple VideoToolbox
	"obs_x264",           // software fallback
};

bool encoderExists(const char *id)
{
	const char *existingId = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &existingId); i++) {
		if (existingId && strcmp(existingId, id) == 0)
			return true;
	}
	return false;
}

struct SourceEnumCtx {
	obs_data_array_t *array;
};

bool enumVideoSources(void *param, obs_source_t *source)
{
	auto *ctx = static_cast<SourceEnumCtx *>(param);
	uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_VIDEO) == 0)
		return true;

	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "name", obs_source_get_name(source));
	obs_data_set_string(item, "id", obs_source_get_id(source));
	obs_data_set_string(item, "uuid", obs_source_get_uuid(source));
	obs_data_array_push_back(ctx->array, item);
	obs_data_release(item);
	return true;
}

void onStartHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	std::string err;
	ReplayCore::instance().startRecording(err);
}

void onStopHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	ReplayCore::instance().stopRecording();
}

} // namespace

ReplayCore &ReplayCore::instance()
{
	static ReplayCore core;
	return core;
}

void ReplayCore::seatSessionEpoch()
{
	// Both clocks read back to back: the pair is only as good as how close
	// the two samples are, and everything persisted this run is converted
	// through it. See session-clock.hpp.
	const int64_t master = (int64_t)os_gettime_ns();
	const int64_t wall =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count();
	epochMasterNs_.store(master);
	epochWallNs_.store(wall);
}

void ReplayCore::restartSegmentIndex(const std::string &folder)
{
	if (folder.empty())
		return;
	// Watch every configured camera slot: a project recorded earlier may have
	// used a different set of sources, and this is the same filter REC applies
	// (cam<N>_*.mp4 for a slot we know about). Files whose anchor is not in
	// anchors.json stay unresolvable — the ring is empty outside REC, so there
	// is nothing to re-derive an anchor from, and inventing one is exactly what
	// this engine exists not to do.
	std::array<bool, kMaxSegmentCameras> segCams{};
	std::array<int, kMaxSegmentCameras> segCanonical{};
	for (int i = 0; i < kMaxSegmentCameras; i++)
		segCanonical[i] = i;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (int i = 0; i < kMaxCameras && i < kMaxSegmentCameras; i++)
			segCams[i] = !config_.cameras[i].sourceName.empty();
		// A slot that only duplicates an earlier slot's source never had a
		// file series of its own on disk (see startRecording): redirect its
		// reads to the slot that does, the same way PacketTap's live ring
		// does for the recent past.
		std::array<std::string, kMaxCameras> srcNames{};
		for (int i = 0; i < kMaxCameras; i++)
			srcNames[i] = config_.cameras[i].sourceName;
		const std::array<int, kMaxCameras> canonicalCam =
			canonicalCameraIndices(srcNames);
		for (int i = 0; i < kMaxCameras && i < kMaxSegmentCameras; i++)
			segCanonical[i] = canonicalCam[i];
	}
	const SessionEpoch epoch = sessionEpoch();
	SegmentIndex::instance().start(folder, segCams, segCanonical,
				       epoch.masterNs, epoch.wallNs);

	// The folder just changed, so whatever disk bandwidth was measured for
	// the previous one means nothing here. Measuring is I/O, so it happens
	// now — between takes, on a background thread — and never at REC, where
	// it would both delay the take and compete with it (see probeDiskAsync).
	//
	// The SESSION folder, not this project's: write speed belongs to the
	// disk, and a probe file inside the project is a file in a directory the
	// operator (or the gate) may be about to delete.
	HealthMonitor::instance().probeDiskAsync(
		ReplayCore::instance().getConfig().sessionFolder);
}

void ReplayCore::load()
{
	// BEFORE loadConfig(): that call restores the last project's events, and
	// EventStore stores its marks in wall-clock time, so it needs the epoch to
	// map them back onto this session's monotonic clock.
	seatSessionEpoch();
	EventStore::instance().setSessionEpoch(sessionEpoch());

	loadConfig();
	// ...and, if a project was open when OBS last closed, ITS settings on top.
	// Without this the plugin comes back with the global config, which is the
	// last thing SAVED and not necessarily this project's rig.
	loadProjectSettings();

	// The events of the last project were just restored; without this its
	// footage would not be, and the operator would see marks with nothing
	// behind them until he re-opened the project by hand.
	restartSegmentIndex(recordingFolder());

	startHotkey_ = obs_hotkey_register_frontend(
		"MultiReplayStartRecording",
		obs_module_text("Hotkey.StartRecording"), onStartHotkey, nullptr);
	stopHotkey_ = obs_hotkey_register_frontend(
		"MultiReplayStopRecording",
		obs_module_text("Hotkey.StopRecording"), onStopHotkey, nullptr);

	registerReplayHotkeys();

	// NOTE: Branch Output availability is checked in obs_module_post_load
	// (plugin-main.cpp): at this point Branch Output may not be loaded
	// yet (alphabetical module load order).
}

// ---------------------------------------------------------------------------
// Full broadcast-style hotkey set: visible in OBS Settings -> Hotkeys, hence
// directly mappable from a Stream Deck via its native OBS integration.
// ---------------------------------------------------------------------------
namespace {

int64_t hotkeyMarkTimeNs()
{
	// The live edge is MEASURED now: the newest packet the tap captured off
	// the encoder, on the shared system clock. No arm timestamp, no
	// encoder-startup guess to subtract — a mark lands on the frame that was
	// on screen, and the same value means the same instant on every angle.
	if (EventStore::instance().liveMode()) {
		int64_t now = PacketTap::instance().newestNs(
			ReplayCore::instance().currentAngle());
		if (now > 0)
			return now;
	}
	return ReplayChannel::instance().positionNs();
}

// A mark is only real if there is an instant behind it. Both spellings of "no
// instant" are refused: the ring reports 0 when it has captured nothing on this
// angle, and ReplayChannel reports kNoInstant when nothing has played. Storing
// either would produce an event visible in the list and impossible to play.
//
// What is NOT refused is a negative instant: master time counts from the
// machine's boot, so footage older than that sits at negative values and marks
// on it are perfectly good. See kNoInstant in session-clock.hpp.
bool hotkeyMarkable(int64_t tNs)
{
	if (tNs != 0 && tNs != kNoInstant)
		return true;
	obs_log(LOG_WARNING,
		"mark ignored: nothing captured on angle %d and no replay "
		"playing",
		ReplayCore::instance().currentAngle() + 1);
	return false;
}

// The angle the operator is on, which is the one he is looking at when he hits
// the key. Marking always enabled CAM1 instead, so a mark taken from a Stream
// Deck while watching angle 3 produced an event enabled on angle 1 only — and
// playback, which refuses to play a disabled angle, then silently put the wrong
// camera on air. The dock has always passed its selected angle; the hotkeys,
// which are the ones actually used during a match, did not.
int hotkeyAngle0()
{
	return ReplayCore::instance().currentAngle();
}

using SimpleFn = void (*)();

// EVERY hotkey runs on the GUI thread, and this is not tidiness (B5).
//
// OBS calls hotkey callbacks from its own hotkey thread. These ones go
// straight into PlaybackCoordinator, which takes its mutex_ and — inside
// switchToReplayScene() — used to dispatch to the UI thread and WAIT. Meanwhile
// the operator pressing Stop enters stopEvents() on the UI thread and blocks on
// that same mutex_. Deck play and a Stop half a second apart is then a complete
// AB-BA: OBS frozen, with a take recording. The dock has always marshalled its
// own hotkeys; these were the ones that did not, and they are the ones a Stream
// Deck actually uses.
//
// The dispatch carries no allocation: `data` is a function pointer, not an
// owned object, so nothing has to survive the queue.
void onSimpleHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	if (obs_in_task_thread(OBS_TASK_UI)) {
		reinterpret_cast<SimpleFn>(data)();
		return;
	}
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) { reinterpret_cast<SimpleFn>(p)(); }, data, false);
}

struct HotkeyDef {
	const char *name;
	const char *locale;
	SimpleFn fn;
};

const HotkeyDef kReplayHotkeys[] = {
	{"ReplayMarkIn", "Hotkey.MarkIn",
	 []() {
		 const int64_t t = hotkeyMarkTimeNs();
		 if (hotkeyMarkable(t))
			 EventStore::instance().markIn(t, hotkeyAngle0());
	 }},
	{"ReplayMarkOut", "Hotkey.MarkOut",
	 []() {
		 const int64_t t = hotkeyMarkTimeNs();
		 if (hotkeyMarkable(t))
			 EventStore::instance().markOut(t);
	 }},
	{"ReplayMarkInOut5", "Hotkey.Mark5",
	 []() {
		 const int64_t t = hotkeyMarkTimeNs();
		 if (hotkeyMarkable(t))
			 EventStore::instance().markInOut(t, 5, hotkeyAngle0());
	 }},
	{"ReplayMarkInOut10", "Hotkey.Mark10",
	 []() {
		 const int64_t t = hotkeyMarkTimeNs();
		 if (hotkeyMarkable(t))
			 EventStore::instance().markInOut(t, 10, hotkeyAngle0());
	 }},
	{"ReplayMarkInOut20", "Hotkey.Mark20",
	 []() {
		 const int64_t t = hotkeyMarkTimeNs();
		 if (hotkeyMarkable(t))
			 EventStore::instance().markInOut(t, 20, hotkeyAngle0());
	 }},
	{"ReplayPlayLastEventToOutput", "Hotkey.PlayLast",
	 []() {
		 std::string err;
		 PlaybackCoordinator::instance().playLastEvent(
			 ReplayCore::instance().currentAngle(),
			 ReplayCore::instance().getConfig().autoSwitchScene,
			 err);
	 }},
	// There is no free-running playhead to pause any more: the engine plays a
	// clip. So this stops what is playing, or re-cues the last event.
	{"ReplayPlayPause", "Hotkey.PlayPause",
	 []() {
		 if (ReplayChannel::instance().playing()) {
			 PlaybackCoordinator::instance().stopEvents();
			 return;
		 }
		 std::string err;
		 PlaybackCoordinator::instance().playLastEvent(
			 ReplayCore::instance().currentAngle(),
			 ReplayCore::instance().getConfig().autoSwitchScene,
			 err);
	 }},
	// the reference controller NOW: drop the replay and go back to watching the live edge.
	{"ReplayJumpToNow", "Hotkey.JumpToNow",
	 []() {
		 PlaybackCoordinator::instance().stopEvents();
		 ReplayCore::instance().setFollowLive(true);
	 }},
	{"ReplayStopEvents", "Hotkey.StopEvents",
	 []() { PlaybackCoordinator::instance().stopEvents(); }},
	{"ReplayLiveToggle", "Hotkey.LiveToggle",
	 []() {
		 auto &store = EventStore::instance();
		 store.setLiveMode(!store.liveMode());
	 }},
};

// Angle hotkeys need an index: lambdas can't capture, so use a template.
template<int N> void setAngleA()
{
	ReplayCore::instance().setCurrentAngle(N);
}

const SimpleFn kAngleFns[kMaxCameras] = {
	setAngleA<0>, setAngleA<1>, setAngleA<2>, setAngleA<3>,
	setAngleA<4>, setAngleA<5>, setAngleA<6>, setAngleA<7>,
};

} // namespace

void ReplayCore::registerReplayHotkeys()
{
	for (const auto &def : kReplayHotkeys) {
		extraHotkeys_.push_back(obs_hotkey_register_frontend(
			def.name, obs_module_text(def.locale), onSimpleHotkey,
			reinterpret_cast<void *>(def.fn)));
	}
	for (int i = 0; i < kMaxCameras; i++) {
		std::string name = "ReplayACamera" + std::to_string(i + 1);
		std::string desc =
			std::string(obs_module_text("Hotkey.AngleA")) + " " +
			std::to_string(i + 1);
		extraHotkeys_.push_back(obs_hotkey_register_frontend(
			name.c_str(), desc.c_str(), onSimpleHotkey,
			reinterpret_cast<void *>(kAngleFns[i])));
	}
}

void ReplayCore::unload()
{
	if (recording_)
		stopRecording();
	if (startHotkey_ != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(startHotkey_);
	if (stopHotkey_ != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(stopHotkey_);
	for (obs_hotkey_id id : extraHotkeys_)
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);
	extraHotkeys_.clear();
}

bool ReplayCore::startRecording(std::string &errorOut)
{
	// M5: THE PRE-FLIGHT DOES NOT HOLD THE CORE LOCK.
	//
	// It asks the filesystem how much space there is and WRITES a probe file
	// to prove the folder is writable — on a session folder that lives on a
	// NAS, two network round trips. Held under mutex_, which the dock's poll
	// takes four times a second, pressing REC froze the interface for as long
	// as the answer took. Nothing in the pre-flight changes state, so a copy
	// of the configuration is all it needs.
	Config cfg;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (recording_) {
			errorOut = "already recording";
			return false;
		}
		cfg = config_;
	}

	// --- M4 pre-flight ----------------------------------------------------
	// Here and not in the dock: a take started from a hotkey, a Stream Deck
	// or the self-test has to be refused by exactly the same rules as one
	// started from the button, and refused BEFORE a single filter is armed.
	// It replaces the two hand-rolled checks that used to live here (Branch
	// Output installed, session folder set) and adds the ones that used to be
	// discovered halfway through a match: a source that is gone, a disk with
	// four minutes on it, a disk too slow for the bitrate, a ring that does
	// not fit in RAM.
	//
	// Blockers refuse. Warnings never do — they are logged, kept for the
	// dock's badge, and the take runs.
	RingBudget budget;
	budget.kbpsPerCamera = cfg.videoBitrateKbps + cfg.audioBitrateKbps;
	auto &monitor = HealthMonitor::instance();
	const health::PreflightResult preflight =
		monitor.preflight(cfg, budget.seconds);
	monitor.rememberPreflight(preflight);
	for (const auto &f : preflight.findings)
		obs_log(f.level >= health::Level::Blocker ? LOG_ERROR
							  : LOG_WARNING,
			"[health] preflight %s: %s%s%s",
			health::levelName(f.level), f.id.c_str(),
			f.detail.empty() ? "" : " - ", f.detail.c_str());
	if (!preflight.ok()) {
		errorOut = findingsBlock(preflight.findings,
					 health::Level::Blocker);
		return false;
	}
	// Visible degradation, the M4 way: a ring that does not fit is made
	// smaller and said out loud (ring_ram_tight), never silently granted.
	if (preflight.ringSeconds > 0)
		budget.seconds = preflight.ringSeconds;

	// ARMING, and only arming, under the lock.
	int started = 0;
	std::string recFolder;
	std::array<bool, kMaxSegmentCameras> segCams{};
	// Identity until the lock below fills it in from the live config — a
	// duplicate slot's file/ring reads redirect here (see PacketTap::owner
	// and SegmentIndex, both fed this same mapping).
	std::array<int, kMaxSegmentCameras> segCanonical{};
	for (int i = 0; i < kMaxSegmentCameras; i++)
		segCanonical[i] = i;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		// Between the snapshot above and here, another entry point may have
		// started a take: the button, a hotkey and the gate all come through
		// this function and only one of them can be first.
		if (recording_) {
			errorOut = "already recording";
			return false;
		}

		std::error_code ec;
		recFolder = recordingFolderLocked();
		std::filesystem::create_directories(utf8ToPath(recFolder), ec);

		// WHICH SLOT OWNS THE FILTER/ENCODER for its source (camera-dedup.hpp).
		// Two or more slots naming the same OBS source are the same picture:
		// only the lowest-numbered one — the CANONICAL slot — ever gets a
		// Branch Output filter. Asking Branch Output to encode the same
		// source a second time for nothing is how eight configured slots
		// became eight hardware encode sessions racing for a GPU that may
		// only have room for a few, and the losing ones never came up at all
		// — which is what "only the first camera's preview ever shows a
		// picture" looks like from the panel.
		std::array<std::string, kMaxCameras> srcNames{};
		for (int i = 0; i < kMaxCameras; i++)
			srcNames[i] = config_.cameras[i].sourceName;
		const std::array<int, kMaxCameras> canonicalCam =
			canonicalCameraIndices(srcNames);
		for (int i = 0; i < kMaxCameras && i < kMaxSegmentCameras; i++)
			segCanonical[i] = canonicalCam[i];

		for (int i = 0; i < kMaxCameras; i++) {
			auto &st = cameraStatus_[i];
			st = CameraStatus{};
			st.index = i;
			st.sourceName = config_.cameras[i].sourceName;
			st.configured = !st.sourceName.empty();
			if (!st.configured)
				continue;

			if (canonicalCam[i] != i) {
				// A DUPLICATE of an earlier slot's source: it rides
				// that slot's already-armed filter and encoder rather
				// than asking for a second one. Processed in order
				// 0..7, so the slot it mirrors was already handled.
				const CameraStatus &owner =
					cameraStatus_[canonicalCam[i]];
				st.sourceFound = owner.sourceFound;
				st.filterPresent = false;
				st.recording = owner.recording;
				st.startTimestampNs = owner.startTimestampNs;
				if (st.recording)
					started++;
				continue;
			}

			obs_source_t *target =
				obs_get_source_by_name(st.sourceName.c_str());
			if (!target) {
				obs_log(LOG_WARNING, "Camera %d: source '%s' not found",
					i + 1, st.sourceName.c_str());
				continue;
			}
			st.sourceFound = true;

			// REC arms; it does not configure.
			//
			// Writing settings here is what broke the take. obs_source_update()
			// on an existing filter makes Branch Output log "Settings change
			// detected, Attempting restart" and rebuild its pipeline - so the
			// encoder the tap had just attached to is destroyed, the tap backs
			// off and re-attaches seconds later, and by then the opening of the
			// file it must anchor against has already left the ring. Result:
			// "still unmatched ... (240 file, 17 ring)", no anchor, nothing
			// replayable. The same restart, caught mid-attach, is what crashed
			// gpu_encode_thread earlier.
			//
			// So an existing filter is only switched on. Settings belong to the
			// Settings dialog, which applies them while nothing is recording.
			const std::string filterName =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			obs_source_t *filter =
				obs_source_get_filter_by_name(target, filterName.c_str());
			if (!filter)
				filter = branch_output::ensureFilter(target, i, config_);
			if (filter) {
				st.filterPresent = true;
				branch_output::setEnabled(filter, true);
				st.startTimestampNs = os_gettime_ns();
				st.recording = true;
				started++;
				obs_source_release(filter);
			}
			obs_source_release(target);
		}

		if (started == 0) {
			errorOut = "no camera could be started (check sources and "
				   "Branch Output installation)";
			return false;
		}

		recording_ = true;

		// M0: attach the live packet tap to the encoders Branch Output just
		// started. Branch Output builds its infrastructure asynchronously, so
		// this only arms a retry loop; it never blocks REC and never fails it
		// (fail-SOFT in M0, fail-CLOSED from M1 once the ring is authoritative).
		{
			std::array<bool, kMaxTapChannels> wantTap{};
			std::array<int, kMaxTapChannels> tapCanonical{};
			std::array<bool, kMaxCameras> armedCams{};
			for (int i = 0; i < kMaxCameras; i++) {
				armedCams[i] = cameraStatus_[i].recording;
				if (i < kMaxTapChannels) {
					tapCanonical[i] = canonicalCam[i];
					// Only the canonical slot has a filter to
					// attach to; a duplicate's reads redirect to
					// it instead of arming a channel of its own.
					wantTap[i] = cameraStatus_[i].recording &&
						     canonicalCam[i] == i;
				}
			}
			// budget was filled in (and possibly cut to fit RAM) by pre-flight.
			PacketTap::instance().armAsync(wantTap, tapCanonical, budget);

			// The monitor's baselines belong to this take: resident memory
			// before it, the frame counters at its start, and the ring window
			// it was actually granted rather than the one it asked for.
			//
			// The disk-bandwidth estimate counts DISTINCT encoders only: a
			// duplicate slot writes no file of its own, so folding it into
			// this count would ask the disk-speed check for bandwidth
			// nothing is actually asking the disk for.
			int armedCount = 0;
			for (int i = 0; i < kMaxCameras; i++)
				if (armedCams[i] && canonicalCam[i] == i)
					armedCount++;
			monitor.takeStarted(armedCams, canonicalCam, budget.seconds,
					    (int64_t)budget.kbpsPerCamera * 1000 / 8 *
						    armedCount,
					    cfg.sessionFolder);

			// Watch the files Branch Output writes so replay can reach back
			// past the RAM window. The epoch pair ties this session's
			// monotonic clock to wall time, which is the only thing that
			// still means anything once OBS restarts.
			for (int i = 0; i < kMaxCameras && i < kMaxSegmentCameras; i++)
				segCams[i] = cameraStatus_[i].recording;
		}
	} // the core lock ends here

	// AND SEGMENTINDEX::START() IS OUTSIDE IT (M5). It joins its watcher
	// thread, parses anchors.json and stats every file named in it — seconds,
	// on a network folder — and it touches no state of ours, so there was
	// never a reason for the interface to wait behind it.
	//
	// The session epoch, not a fresh sample: the anchors already on disk for
	// this folder are about to be read back through it, and so are the event
	// marks. One pair per process (see sessionEpoch).
	{
		const SessionEpoch epoch = sessionEpoch();
		SegmentIndex::instance().start(recFolder, segCams, segCanonical,
					       epoch.masterNs, epoch.wallNs);
	}

	// The encoder-startup latency detector is gone with the file-based engine:
	// packets carry sys_dts_usec, so the first captured instant IS the first
	// encoded frame. Nothing left to measure or subtract.
	EventStore::instance().setSessionFolder(recFolder);
	EventStore::instance().setLiveMode(true); // the reference controller: recording => Live
	followLive_.store(true);                  // a new take starts at the live edge
	obs_log(LOG_INFO, "Recording started on %d camera(s)", started);
	return true;
}

bool ReplayCore::branchOutputRecording() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!recording_)
		return false;
	for (const auto &st : cameraStatus_) {
		// Only the cameras this take actually armed: an unconfigured or
		// missing source never had an output to begin with, and counting
		// it would make the check unfailable.
		if (!st.recording)
			continue;
		if (branch_output::recordingOutputActive(st.index))
			return true;
	}
	return false;
}

bool ReplayCore::stopRecording()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!recording_)
		return false;

	// Log the M0 evidence while the tap is still attached, then detach
	// BEFORE the filters are disabled: Branch Output frees its encoder in
	// releaseInfrastructureIfIdle() once its own outputs go idle, and we
	// must not be holding it (let alone still running it) at that point.
	if (PacketTap::instance().anyAttached())
		obs_log(LOG_INFO, "%s", PacketTap::instance().report().c_str());
	PacketTap::instance().detachAll();
	SegmentIndex::instance().stop();
	// The take is over, so its findings are over with it: they described a
	// recording that no longer exists (see takeStopped).
	HealthMonitor::instance().takeStopped();

	for (int i = 0; i < kMaxCameras; i++) {
		auto &st = cameraStatus_[i];
		if (!st.recording)
			continue;

		obs_source_t *target =
			obs_get_source_by_name(st.sourceName.c_str());
		if (!target)
			continue;
		std::string filterName =
			std::string(branch_output::kFilterNamePrefix) +
			std::to_string(i + 1);
		obs_source_t *filter = obs_source_get_filter_by_name(
			target, filterName.c_str());
		if (filter) {
			branch_output::setEnabled(filter, false);
			obs_source_release(filter);
		}
		obs_source_release(target);
		st.recording = false;
	}

	recording_ = false;
	obs_log(LOG_INFO, "Recording stopped");
	return true;
}

void ReplayCore::disarmPersistedFilters()
{
	struct Ctx {
		int disarmed = 0;
	} ctx;

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *c = static_cast<Ctx *>(param);
			for (int i = 1; i <= kMaxCameras; i++) {
				std::string name =
					std::string(branch_output::
							    kFilterNamePrefix) +
					std::to_string(i);
				obs_source_t *filter =
					obs_source_get_filter_by_name(
						source, name.c_str());
				if (filter) {
					if (obs_source_enabled(filter)) {
						obs_source_set_enabled(filter,
								       false);
						c->disarmed++;
					}
					obs_source_release(filter);
				}
			}
			return true;
		},
		&ctx);

	if (ctx.disarmed > 0)
		obs_log(LOG_INFO,
			"disarmed %d persisted recording filter(s) — "
			"recording starts only via REC",
			ctx.disarmed);

	// PRUNING STALE FILTERS IS NOT DONE HERE ON PURPOSE.
	//
	// The scene collection has just been (re)loaded, which is the moment the
	// filters it persisted come back — including the ones belonging to a
	// project that is no longer open, or the ones a duplicate camera slot
	// (camera-dedup.hpp) no longer gets. Disarming them (above) is enough for
	// safety; pruning is destructive, and this function is called from
	// FINISHED_LOADING/SCENE_COLLECTION_CHANGED while OBSInit() is still one
	// long synchronous C++ call with no Qt event loop of its own running.
	// Deserialising the scene collection just handed every persisted Branch
	// Output filter its own queued "filter added" notification on BO's
	// status dock (Qt::QueuedConnection, holding a raw source pointer), and
	// that queue is NOT drained by anything here — the first thing in
	// OBSInit() that actually spins a Qt event loop is NewYouTubeAppDock,
	// further down. Pruning now — even posted through obs_queue_task, which
	// measured no later than a couple of milliseconds after this call, not
	// "the next real event-loop turn" — destroys some of those just-loaded
	// filters before their own "added" notification is ever processed, and
	// OBS crashes minutes later inside NewYouTubeAppDock's nested loop,
	// calling obs_source_get_name() on a filter that is long gone. Measured
	// on a real project with 8 camera slots on one source, where
	// pruneFilters now correctly removes 7 of them (camera-dedup.hpp,
	// previously 0 — this crash could not happen before that fix):
	//   obs.dll!obs_source_get_name
	//   osi-branch-output.dll!OutputTableRow::OutputTableRow
	//   osi-branch-output.dll!BranchOutputStatusDock::addFilter/addRow
	//   obs64.exe!OBSBasic::NewYouTubeAppDock -> ...OBSInit
	//
	// The caller (plugin-main.cpp, which has Qt available and this file
	// deliberately does not) schedules the prune with QTimer::singleShot —
	// a real Qt timer only ever fires from inside an actual event-loop
	// iteration, so it lands in the SAME queue as Branch Output's own
	// connections and after them, in post order, instead of racing ahead of
	// them through some other, earlier drain path.
}

bool ReplayCore::deleteAllSession(std::string &errorOut)
{
	std::string folder;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (recording_) {
			errorOut = "stop recording first";
			return false;
		}
		if (config_.sessionFolder.empty()) {
			errorOut = "no session folder configured";
			return false;
		}
		folder = recordingFolderLocked();
	}

	// The index now watches the folder outside REC too, and it writes its
	// anchors back on the way down — stop it BEFORE deleting, or it would
	// resurrect anchors.json pointing at files that no longer exist.
	SegmentIndex::instance().stop();

	namespace fs = std::filesystem;
	std::error_code ec;
	int removed = 0;
	for (const auto &entry :
	     fs::directory_iterator(utf8ToPath(folder), ec)) {
		if (!entry.is_regular_file())
			continue;
		std::string name = pathToUtf8(entry.path().filename());
		std::string ext = pathToUtf8(entry.path().extension());
		bool isRecording = name.rfind("cam", 0) == 0 &&
				   (ext == ".mp4" || ext == ".mov");
		// anchors.json belongs to the recordings being deleted; the
		// session*.json manifests are the legacy engine's and are only
		// matched here so Delete All still cleans them up.
		int n = 0;
		bool isMeta =
			name == "anchors.json" || name == "session.json" ||
			name == "events.json" ||
			(std::sscanf(name.c_str(), "session_%d.json", &n) == 1 &&
			 n > 0);
		if (isRecording || isMeta) {
			fs::remove(entry.path(), ec);
			if (!ec)
				removed++;
		}
	}
	obs_log(LOG_INFO, "Delete All: removed %d file(s)", removed);
	restartSegmentIndex(folder); // fresh, empty index over the wiped folder
	return true;
}

Config ReplayCore::getConfig() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return config_;
}

void ReplayCore::setConfig(const Config &cfg)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		config_ = cfg;
		config_.eventIdDigits = std::clamp(config_.eventIdDigits, 1, 8);
		config_.eventListCount =
			std::clamp(config_.eventListCount, 1, kEventLists);
		config_.preRollMs = std::max(0, config_.preRollMs);
		config_.postRollMs = std::max(0, config_.postRollMs);
	}
	// Takes effect at once — the operator flips it in Settings precisely to
	// catch the next resize.
	setVerboseLogging(cfg.verboseLog);
	// Marking is a store rule and the hotkeys never pass through the dock, so
	// the rolls go where both paths read them.
	EventStore::instance().setRollNs((int64_t)cfg.preRollMs * 1000000,
					 (int64_t)cfg.postRollMs * 1000000);
	saveConfig();
	// Filters must know the new path even before the next REC press.
	if (!recording_) {
		reapplyFilterSettings();
		// The session folder (and the camera slots the index filters on)
		// may just have changed; re-point it so it reads the anchors of
		// whatever folder is now current.
		restartSegmentIndex(recordingFolder());
	}
}

std::string ReplayCore::pickVideoEncoder() const
{
	for (const char *id : kEncoderPreference) {
		if (encoderExists(id))
			return id;
	}
	return "obs_x264";
}

int64_t ReplayCore::diskFreeBytes() const
{
	if (config_.sessionFolder.empty())
		return -1;
	std::error_code ec;
	auto info = std::filesystem::space(config_.sessionFolder, ec);
	if (ec)
		return -1;
	return static_cast<int64_t>(info.available);
}

int64_t ReplayCore::estimatedMinutesRemaining() const
{
	int64_t freeBytes = diskFreeBytes();
	if (freeBytes < 0)
		return -1;
	int activeCams = 0;
	for (const auto &cam : config_.cameras)
		if (!cam.sourceName.empty())
			activeCams++;
	if (activeCams == 0)
		return -1;
	// kbps -> bytes per minute, video + audio per camera
	int64_t bytesPerMin =
		static_cast<int64_t>(config_.videoBitrateKbps +
				     config_.audioBitrateKbps) *
		1000 / 8 * 60 * activeCams;
	return bytesPerMin > 0 ? freeBytes / bytesPerMin : -1;
}

std::string ReplayCore::statusJson() const
{
	// Snapshot all config/status fields under a brief lock (no I/O inside).
	bool rec;
	std::string sessionFolder;
	int videoBitrateKbps, audioBitrateKbps;
	std::string videoEncoderStr;
	std::array<CameraStatus, kMaxCameras> camSnap;
	int activeCams = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		rec = recording_;
		sessionFolder = config_.sessionFolder;
		videoBitrateKbps = config_.videoBitrateKbps;
		audioBitrateKbps = config_.audioBitrateKbps;
		videoEncoderStr = config_.videoEncoderId.empty()
					  ? pickVideoEncoder()
					  : config_.videoEncoderId;
		camSnap = cameraStatus_;
		for (const auto &cam : config_.cameras)
			if (!cam.sourceName.empty())
				activeCams++;
	}

	// Disk I/O outside any lock: filesystem::space() can block (network drives).
	int64_t freeBytes = -1;
	int64_t minsRemaining = -1;
	if (!sessionFolder.empty()) {
		std::error_code ec;
		auto spaceInfo = std::filesystem::space(sessionFolder, ec);
		if (!ec) {
			freeBytes = static_cast<int64_t>(spaceInfo.available);
			if (activeCams > 0) {
				int64_t bytesPerMin =
					static_cast<int64_t>(videoBitrateKbps +
							     audioBitrateKbps) *
					1000 / 8 * 60 * activeCams;
				minsRemaining =
					bytesPerMin > 0 ? freeBytes / bytesPerMin
							: -1;
			}
		}
	}

	// Build JSON from local snapshots — no lock needed.
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "version", PLUGIN_VERSION);
	obs_data_set_bool(root, "recording", rec);
	obs_data_set_bool(root, "branchOutputAvailable",
			  branch_output::available());
	obs_data_set_string(root, "sessionFolder", sessionFolder.c_str());
	obs_data_set_int(root, "diskFreeBytes", freeBytes);
	obs_data_set_int(root, "estimatedMinutesRemaining", minsRemaining);
	obs_data_set_string(root, "videoEncoder", videoEncoderStr.c_str());

	obs_data_array_t *cams = obs_data_array_create();
	for (const auto &st : camSnap) {
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "index", st.index);
		obs_data_set_string(item, "sourceName", st.sourceName.c_str());
		obs_data_set_bool(item, "configured", st.configured);
		obs_data_set_bool(item, "sourceFound", st.sourceFound);
		obs_data_set_bool(item, "recording", st.recording);
		obs_data_set_int(item, "startTimestampNs",
				 static_cast<int64_t>(st.startTimestampNs));
		obs_data_array_push_back(cams, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "cameras", cams);
	obs_data_array_release(cams);

	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

std::string ReplayCore::sourcesJson() const
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *array = obs_data_array_create();
	SourceEnumCtx ctx{array};
	obs_enum_sources(enumVideoSources, &ctx);
	obs_data_set_array(root, "sources", array);
	obs_data_array_release(array);
	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

std::string ReplayCore::encodersJson() const
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *array = obs_data_array_create();
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", id);
		const char *name = obs_encoder_get_display_name(id);
		obs_data_set_string(item, "name", name ? name : id);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "encoders", array);
	obs_data_array_release(array);
	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

bool ReplayCore::branchOutputAvailable() const
{
	return branch_output::available();
}

void ReplayCore::reapplyFilterSettings()
{
	// Never while recording. ensureFilter() calls obs_source_update(), and a
	// live Branch Output filter reacts to that by restarting its pipeline -
	// destroying the obs_view behind the encoder the tap is attached to, and
	// taking libobs' gpu_encode_thread down with it. Saving Settings mid-take
	// did exactly this and crashed OBS.
	//
	// Refusing is also the right product behaviour: these settings describe
	// how the session is being recorded, and changing that under a take in
	// progress would split the footage and leave the earlier files anchored
	// to a configuration that no longer exists. REC owns the lifecycle.
	if (recording_) {
		obs_log(LOG_INFO,
			"Recording settings left untouched while REC is active - "
			"they apply from the next recording");
		return;
	}

	// Snapshot camera config without holding mutex_ (ensureFilter calls
	// recordingFolder() which would re-acquire and deadlock).
	Config cfg;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		cfg = config_;
	}
	// TAKE THE OLD ONES OFF FIRST. This function used to only ADD: it walked
	// the configured cameras and made sure each had its filter, so a slot that
	// stopped being configured — or was pointed at a different source — kept
	// the filter it had. Open a three-camera project, then a two-camera one,
	// and the scene collection still carries three of our filters while REC
	// arms two: the rig declares more angles than it records, which is the one
	// thing an operator cannot be asked to notice mid-match.
	const int pruned = branch_output::pruneFilters(cfg);

	// ...AND THEN LET THE FRONTEND BREATHE BEFORE PUTTING THE NEW ONES ON.
	//
	// Branch Output has a status dock that keeps a row per filter, and it
	// builds those rows from QUEUED signal handlers holding raw source
	// pointers. Removing filters and adding others in the same turn of the
	// event loop leaves it building a row for a source we have already let go:
	//
	//   obs.dll!signal_handler_connect_ref        (locking a null handler)
	//   osi-branch-output.dll!FilterCell::FilterCell
	//   osi-branch-output.dll!OutputTableRow::OutputTableRow
	//   osi-branch-output.dll!BranchOutputStatusDock::addFilter
	//
	// That is four OBS processes killed on this machine, every one of them
	// while switching a project's cameras — the ordinary thing to do before a
	// match. The defect is Branch Output's and we cannot patch it, but we do
	// choose when to hand it work: posting the additions as a SEPARATE UI task
	// lets its queue drain the removals first. Nothing waits on this — arming
	// is a separate operator action and startRecording re-reads the filters.
	if (pruned > 0) {
		auto *pending = new Config(cfg);
		obs_queue_task(
			OBS_TASK_UI,
			[](void *param) {
				std::unique_ptr<Config> c(
					static_cast<Config *>(param));
				ReplayCore::addFiltersForConfig(*c);
			},
			pending, false);
		return;
	}
	addFiltersForConfig(cfg);
}

// The "add" half of reapplyFilterSettings, split out so it can be posted to a
// later turn of the UI loop (see the note above on Branch Output's status dock).
void ReplayCore::addFiltersForConfig(const Config &cfg)
{
	// Only the CANONICAL slot for each source gets a filter (camera-dedup.hpp):
	// a later slot naming the same source is the same picture, and building
	// Branch Output a second filter for it duplicates the encoder, the file
	// and — on a GPU with a limited number of concurrent hardware sessions —
	// the odds that camera ever actually starts.
	std::array<std::string, kMaxCameras> srcNames{};
	for (int i = 0; i < kMaxCameras; i++)
		srcNames[i] = cfg.cameras[i].sourceName;
	const std::array<int, kMaxCameras> canonicalCam =
		canonicalCameraIndices(srcNames);

	for (int i = 0; i < kMaxCameras; i++) {
		if (cfg.cameras[i].sourceName.empty())
			continue;
		if (canonicalCam[i] != i)
			continue; // a duplicate source shares the canonical slot's filter
		obs_source_t *target =
			obs_get_source_by_name(cfg.cameras[i].sourceName.c_str());
		if (!target)
			continue;
		obs_source_t *filter =
			branch_output::ensureFilter(target, i, cfg);
		if (filter)
			obs_source_release(filter);
		obs_source_release(target);
	}
}

std::string ReplayCore::recordingFolderLocked() const
{
	if (config_.currentProjectName.empty())
		return config_.sessionFolder;
	// A3: UTF-8 in, UTF-8 out. path::string() on MSVC would narrow this
	// through the ANSI code page, and the result is handed to FFmpeg, to
	// os_fopen and to Branch Output — all of which want UTF-8.
	return joinUtf8(config_.sessionFolder, config_.currentProjectName);
}

std::string ReplayCore::recordingFolder() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return recordingFolderLocked();
}

bool ReplayCore::newProject(const std::string &title, std::string &errorOut)
{
	std::string base, folderName;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (recording_) {
			errorOut = "stop recording first";
			return false;
		}
		if (config_.sessionFolder.empty()) {
			errorOut = "configure session folder first";
			return false;
		}
		base = config_.sessionFolder;
		// M9: the sanitising used to be std::isalnum, which is
		// LOCALE-DEPENDENT — in a single-byte ANSI locale the
		// continuation bytes of a UTF-8 sequence pass it and in the "C"
		// locale they do not, so the same title produced a different
		// folder depending on a global nobody here sets. Either way every
		// non-ASCII character was dropped without a word and "Città"
		// became "Citt". project-name.hpp validates the UTF-8 and keeps
		// it; path-utf8.hpp is what makes keeping it safe on Windows.
		folderName = project_name::sanitize(title);
		if (folderName.empty()) {
			errorOut = "project name contains no valid characters";
			return false;
		}
		config_.currentProjectName = folderName;
	}
	saveConfig();

	std::error_code ec;
	std::string path = joinUtf8(base, folderName);
	std::filesystem::create_directories(utf8ToPath(path), ec);
	if (ec) {
		errorOut = "cannot create project folder: " + ec.message();
		return false;
	}
	// A COPY of what is configured right now, written into the new project.
	// From here the two are independent: changing this one leaves the previous
	// project's settings alone, and reopening that one gets its own back. The
	// alternative — starting empty — is eight camera slots to fill in before
	// every match, which is how a rig gets configured wrong under time
	// pressure. saveConfig() writes both files, so it seeds and persists in
	// one call.
	saveConfig();
	EventStore::instance().setSessionFolder(path);
	// Empty folder, so nothing to read back — but the index must stop pointing
	// at the previous project, or its files would answer this project's lookups.
	restartSegmentIndex(path);
	reapplyFilterSettings(); // redirect Branch Output path to project folder
	obs_log(LOG_INFO, "Project created: %s", path.c_str());
	return true;
}

bool ReplayCore::openProject(const std::string &folderName,
			     std::string &errorOut)
{
	std::string path;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (recording_) {
			errorOut = "stop recording first";
			return false;
		}
		if (config_.sessionFolder.empty()) {
			errorOut = "configure session folder first";
			return false;
		}
		// M10: this argument used to be concatenated onto the session
		// folder with no check at all, and one of the places it comes
		// from is currentProjectName read out of config.json. ".." is a
		// folder name as far as operator/ is concerned.
		if (!project_name::isSafeFolderName(folderName)) {
			errorOut = "that is not a project name: " + folderName;
			return false;
		}
		path = joinUtf8(config_.sessionFolder, folderName);
		if (!std::filesystem::is_directory(utf8ToPath(path))) {
			errorOut = "project folder not found: " + path;
			return false;
		}
		config_.currentProjectName = folderName;
	}
	// ITS settings, before anything is pointed anywhere. The Branch Output
	// filters, the segment index and the event store below are all set up
	// FROM the configuration, so loading the project's own has to happen
	// first — otherwise the project opens with the last project's cameras and
	// only picks up its own the next time something calls setConfig().
	loadProjectSettings();
	saveConfig();
	EventStore::instance().setSessionFolder(path);
	// Footage of a project recorded in an EARLIER OBS run: SegmentIndex reads
	// that run's anchors.json and places its files back on this session's
	// monotonic clock, so resolve() and segment-reader work unchanged. Note
	// this happens with nothing recording — which is the whole point.
	restartSegmentIndex(path);
	reapplyFilterSettings(); // redirect Branch Output path to project folder
	obs_log(LOG_INFO, "Project opened: %s", path.c_str());
	return true;
}

std::vector<std::string> ReplayCore::listProjects() const
{
	std::string base;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		base = config_.sessionFolder;
	}
	std::vector<std::string> result;
	if (base.empty())
		return result;
	std::error_code ec;
	for (const auto &entry :
	     std::filesystem::directory_iterator(utf8ToPath(base), ec)) {
		if (entry.is_directory(ec)) {
			std::string name =
				pathToUtf8(entry.path().filename());
			if (!name.empty() && name[0] != '.')
				result.push_back(name);
		}
	}
	std::sort(result.begin(), result.end());
	return result;
}

void ReplayCore::loadConfig()
{
	char *path = obs_module_config_path(kConfigFile);
	if (!path)
		return;
	loadConfigFile(path, false);
	bfree(path);
}

// SETTINGS BELONG TO A PROJECT, and this is the one function both files go
// through. A project written before this existed has no settings.json; the
// first time it is opened it ADOPTS what is configured now and writes it, so
// nothing has to be set up again and no project ever opens blank.
//
// `projectScoped` says where the file came from, and it decides exactly two
// fields. The session folder is where projects LIVE, so a project cannot own
// it without being able to move itself somewhere else; and the current project
// name is which project is open, which is a statement about the installation
// and not about the project. Everything else — cameras, bitrates, encoder,
// output scenes, transitions, rolls, tags — follows the project, because those
// are the things that differ between one match and the next and that used to
// arrive inherited from whatever was recorded last.
void ReplayCore::loadConfigFile(const char *path, bool projectScoped)
{
	obs_data_t *data = obs_data_create_from_json_file(path);
	if (!data)
		return;

	std::lock_guard<std::mutex> lock(mutex_);
	const std::string keepFolder = config_.sessionFolder;
	const std::string keepProject = config_.currentProjectName;
	config_.sessionFolder = obs_data_get_string(data, "sessionFolder");
	config_.currentProjectName =
		obs_data_get_string(data, "currentProjectName");
	if (projectScoped) {
		config_.sessionFolder = keepFolder;
		config_.currentProjectName = keepProject;
	}
	// GLOBAL, like the two above and for the same reason: which build is
	// installed is a fact about the machine, not about the match. A project's
	// settings.json carries the field too — one serialiser writes both files —
	// and it is ignored when the file is a project's.
	if (!projectScoped && obs_data_has_user_value(data, "updateChannel"))
		config_.updateChannel = obs_data_get_string(data, "updateChannel");
	else if (!projectScoped && parseVersion(PLUGIN_VERSION).pre > 0)
		// A BUILD THAT IS ITSELF A PRE-RELEASE DEFAULTS TO THE BETA
		// CHANNEL, and this is not opting anybody in: installing a beta
		// IS the opt-in, and it already happened. On the stable channel
		// this operator is offered nothing until the FINAL release of
		// those numbers ships — every beta between his and that one is a
		// pre-release, and pre-releases are what the stable channel
		// filters out. Someone testing beta7 is exactly the person beta8
		// was published for. An explicit choice in config.json still
		// wins, in both directions.
		config_.updateChannel = "beta";
	if (obs_data_has_user_value(data, "port"))
		config_.port = (int)obs_data_get_int(data, "port");
	if (obs_data_has_user_value(data, "splitMinutes"))
		config_.splitMinutes =
			(int)obs_data_get_int(data, "splitMinutes");
	if (obs_data_has_user_value(data, "videoBitrateKbps"))
		config_.videoBitrateKbps =
			(int)obs_data_get_int(data, "videoBitrateKbps");
	if (obs_data_has_user_value(data, "audioBitrateKbps"))
		config_.audioBitrateKbps =
			(int)obs_data_get_int(data, "audioBitrateKbps");
	config_.videoEncoderId = obs_data_get_string(data, "videoEncoderId");
	config_.outputSceneName =
		obs_data_get_string(data, "outputSceneName");
	config_.outputSceneNameB =
		obs_data_get_string(data, "outputSceneNameB");
	// Absent in every config written before B became optional, and absent must
	// mean OFF — not "the operator once had two bays". obs_data_get_bool
	// returns false for a missing key, which is the answer we want.
	config_.enableChannelB = obs_data_get_bool(data, "enableChannelB");
	// DEFAULT TRUE, so obs_data_get_bool's "missing means false" is not allowed
	// to decide: a config.json written before these existed must keep behaving
	// the way it behaves today, not come back with both switched off.
	config_.doubleClickPlays =
		!obs_data_has_user_value(data, "doubleClickPlays") ||
		obs_data_get_bool(data, "doubleClickPlays");
	config_.toOutputOnPlay = !obs_data_has_user_value(data, "toOutputOnPlay") ||
				 obs_data_get_bool(data, "toOutputOnPlay");
	config_.abOutputUsesB = obs_data_get_bool(data, "abOutputUsesB");
	config_.replaySourceName =
		obs_data_get_string(data, "replaySourceName");
	config_.musicSourceName =
		obs_data_get_string(data, "musicSourceName");
	config_.musicFilePath = obs_data_get_string(data, "musicFilePath");
	config_.transitionInName = obs_data_get_string(data, "transitionInName");
	config_.transitionOutName = obs_data_get_string(data, "transitionOutName");
	if (obs_data_has_user_value(data, "transitionMs"))
		config_.transitionMs = (int)obs_data_get_int(data, "transitionMs");
	config_.transitionMs = std::clamp(config_.transitionMs, 0, 20000);
	if (obs_data_has_user_value(data, "autoSwitchScene"))
		config_.autoSwitchScene =
			obs_data_get_bool(data, "autoSwitchScene");
	// Absent in configs written before the option existed: keep the default
	// (on) rather than reading back a false nobody chose.
	if (obs_data_has_user_value(data, "fitReplayToCanvas"))
		config_.fitReplayToCanvas =
			obs_data_get_bool(data, "fitReplayToCanvas");
	if (obs_data_has_user_value(data, "preRollMs"))
		config_.preRollMs = (int)obs_data_get_int(data, "preRollMs");
	if (obs_data_has_user_value(data, "postRollMs"))
		config_.postRollMs = (int)obs_data_get_int(data, "postRollMs");
	if (obs_data_has_user_value(data, "sortEventsByTime"))
		config_.sortEventsByTime =
			obs_data_get_bool(data, "sortEventsByTime");
	if (obs_data_has_user_value(data, "continuePastOutMs"))
		config_.continuePastOutMs =
			(int)obs_data_get_int(data, "continuePastOutMs");
	// Clamped here rather than trusted: this value lengthens what goes on air.
	config_.continuePastOutMs = std::clamp(config_.continuePastOutMs, 0, 60000);
	if (obs_data_has_user_value(data, "eventFadeMs"))
		config_.eventFadeMs = (int)obs_data_get_int(data, "eventFadeMs");
	// Same reason as above, plus a floor that is not zero: a dip shorter than
	// about a tenth of a second is a flicker, not a transition, and 0 already
	// means "cut" — so anything between is rounded up to something an eye can
	// read as deliberate.
	config_.eventFadeMs = config_.eventFadeMs <= 0
				      ? 0
				      : std::clamp(config_.eventFadeMs, 100, 4000);
	if (obs_data_has_user_value(data, "eventIdDigits"))
		config_.eventIdDigits =
			(int)obs_data_get_int(data, "eventIdDigits");
	if (obs_data_has_user_value(data, "showMultiview"))
		config_.showMultiview =
			obs_data_get_bool(data, "showMultiview");
	// GLOBAL, like updateChannel above: what colour an operator wants his
	// panel is a fact about the operator, not about the match. Project-scoped
	// it would mean opening somebody else's project re-themed your panel.
	if (!projectScoped && obs_data_has_user_value(data, "uiTheme"))
		config_.uiTheme = (int)obs_data_get_int(data, "uiTheme");
	if (!projectScoped && obs_data_has_user_value(data, "tableDensity"))
		config_.tableDensity =
			(int)obs_data_get_int(data, "tableDensity");
	if (obs_data_has_user_value(data, "eventListCount"))
		config_.eventListCount =
			(int)obs_data_get_int(data, "eventListCount");
	config_.eventListCount =
		std::clamp(config_.eventListCount, 1, kEventLists);
	// The rolls are a marking rule, so the store has to know them before the
	// first hotkey can fire — which is well before the dock exists.
	EventStore::instance().setRollNs((int64_t)config_.preRollMs * 1000000,
					 (int64_t)config_.postRollMs * 1000000);
	const char *fmt = obs_data_get_string(data, "recFormat");
	if (fmt && *fmt)
		config_.recFormat = fmt;
	if (!config_.sessionFolder.empty())
		EventStore::instance().setSessionFolder(
			recordingFolderLocked());

	obs_data_array_t *cams = obs_data_get_array(data, "cameras");
	if (cams) {
		size_t count = obs_data_array_count(cams);
		for (size_t i = 0; i < count && i < kMaxCameras; i++) {
			obs_data_t *item = obs_data_array_item(cams, i);
			config_.cameras[i].sourceName =
				obs_data_get_string(item, "sourceName");
			config_.cameras[i].displayName =
				obs_data_get_string(item, "displayName");
			obs_data_release(item);
		}
		obs_data_array_release(cams);
	}
	// GLOBAL, like uiTheme: a chatty diagnostic log is a per-operator choice,
	// not a per-project one. The env var still forces it on regardless.
	if (!projectScoped && obs_data_has_user_value(data, "verboseLog"))
		config_.verboseLog = obs_data_get_bool(data, "verboseLog");
	setVerboseLogging(config_.verboseLog);
	// The operator's own comment list. Absent in a project written before it
	// existed, which simply means no shortcuts and a plain text field.
	config_.commentPresets.clear();
	if (obs_data_array_t *pre = obs_data_get_array(data, "commentPresets")) {
		const size_t count = obs_data_array_count(pre);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(pre, i);
			const char *t = obs_data_get_string(item, "text");
			if (t && *t)
				config_.commentPresets.push_back(t);
			obs_data_release(item);
		}
		obs_data_array_release(pre);
	}
	obs_data_release(data);
}

void ReplayCore::saveConfig() const
{
	char *path = obs_module_config_path(kConfigFile);
	if (!path)
		return;
	saveConfigFile(path);
	bfree(path);
	// ...and into the project itself, so reopening it next month finds the rig
	// it was recorded with rather than the rig of whatever was opened since.
	saveProjectSettings();
}

// Where a project keeps its own settings. Empty when no project is open — the
// session folder on its own is where projects live, not a project.
std::string ReplayCore::projectSettingsPath() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (config_.currentProjectName.empty() || config_.sessionFolder.empty())
		return std::string();
	return joinUtf8(joinUtf8(config_.sessionFolder,
				config_.currentProjectName),
			kProjectSettingsFile);
}

void ReplayCore::saveProjectSettings() const
{
	const std::string p = projectSettingsPath();
	if (!p.empty())
		saveConfigFile(p.c_str());
}

// Called when a project is opened. Adopts-and-writes when the project has no
// settings of its own (see loadConfigFile): every project on disk predates this
// file, and one that opened with no cameras would be a rig to rebuild by hand
// before a match.
void ReplayCore::loadProjectSettings()
{
	const std::string p = projectSettingsPath();
	if (p.empty())
		return;
	std::error_code ec;
	if (std::filesystem::exists(p, ec)) {
		loadConfigFile(p.c_str(), true);
		obs_log(LOG_INFO, "[config] project settings loaded from %s",
			p.c_str());
	} else {
		saveConfigFile(p.c_str());
		obs_log(LOG_INFO,
			"[config] project had no settings of its own — adopted the "
			"current ones and wrote %s",
			p.c_str());
	}
}

void ReplayCore::saveConfigFile(const char *path) const
{
	std::lock_guard<std::mutex> lock(mutex_);

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "sessionFolder",
			    config_.sessionFolder.c_str());
	obs_data_set_string(data, "currentProjectName",
			    config_.currentProjectName.c_str());
	obs_data_set_string(data, "updateChannel",
			    config_.updateChannel.c_str());
	obs_data_set_int(data, "port", config_.port);
	obs_data_set_int(data, "splitMinutes", config_.splitMinutes);
	obs_data_set_int(data, "videoBitrateKbps", config_.videoBitrateKbps);
	obs_data_set_int(data, "audioBitrateKbps", config_.audioBitrateKbps);
	obs_data_set_string(data, "videoEncoderId",
			    config_.videoEncoderId.c_str());
	obs_data_set_string(data, "outputSceneName",
			    config_.outputSceneName.c_str());
	obs_data_set_string(data, "outputSceneNameB",
			    config_.outputSceneNameB.c_str());
	obs_data_set_bool(data, "enableChannelB", config_.enableChannelB);
	obs_data_set_bool(data, "doubleClickPlays", config_.doubleClickPlays);
	obs_data_set_bool(data, "toOutputOnPlay", config_.toOutputOnPlay);
	obs_data_set_bool(data, "abOutputUsesB", config_.abOutputUsesB);
	obs_data_set_string(data, "replaySourceName",
			    config_.replaySourceName.c_str());
	obs_data_set_string(data, "musicSourceName",
			    config_.musicSourceName.c_str());
	obs_data_set_string(data, "musicFilePath", config_.musicFilePath.c_str());
	obs_data_set_string(data, "transitionInName",
			    config_.transitionInName.c_str());
	obs_data_set_string(data, "transitionOutName",
			    config_.transitionOutName.c_str());
	obs_data_set_int(data, "transitionMs", config_.transitionMs);
	obs_data_set_bool(data, "autoSwitchScene", config_.autoSwitchScene);
	obs_data_set_bool(data, "fitReplayToCanvas", config_.fitReplayToCanvas);
	obs_data_set_int(data, "preRollMs", config_.preRollMs);
	obs_data_set_int(data, "postRollMs", config_.postRollMs);
	obs_data_set_bool(data, "sortEventsByTime", config_.sortEventsByTime);
	obs_data_set_int(data, "continuePastOutMs", config_.continuePastOutMs);
	obs_data_set_int(data, "eventFadeMs", config_.eventFadeMs);
	obs_data_set_int(data, "eventIdDigits", config_.eventIdDigits);
	obs_data_set_bool(data, "showMultiview", config_.showMultiview);
	obs_data_set_int(data, "uiTheme", config_.uiTheme);
	obs_data_set_int(data, "tableDensity", config_.tableDensity);
	obs_data_set_int(data, "eventListCount", config_.eventListCount);
	obs_data_set_bool(data, "verboseLog", config_.verboseLog);
	obs_data_set_string(data, "recFormat", config_.recFormat.c_str());

	obs_data_array_t *cams = obs_data_array_create();
	for (const auto &cam : config_.cameras) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "sourceName",
				    cam.sourceName.c_str());
		obs_data_set_string(item, "displayName",
				    cam.displayName.c_str());
		obs_data_array_push_back(cams, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "cameras", cams);
	obs_data_array_release(cams);

	obs_data_array_t *pre = obs_data_array_create();
	for (const auto &p : config_.commentPresets) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "text", p.c_str());
		obs_data_array_push_back(pre, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "commentPresets", pre);
	obs_data_array_release(pre);

	// The directory of whichever file this is — the plugin's own config
	// directory for the global one, the project folder for a project's.
	std::error_code ec;
	const std::filesystem::path parent = utf8ToPath(path).parent_path();
	if (!parent.empty())
		std::filesystem::create_directories(parent, ec);
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
}

} // namespace multireplay
