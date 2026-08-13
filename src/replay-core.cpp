/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "replay-core.hpp"
#include "branch-output-control.hpp"
#include "event-store.hpp"
#include "playback-coordinator.hpp"
#include "packet-tap.hpp"
#include "plugin-support.h"
#include "replay-channel.hpp"
#include "segment-index.hpp"

#include <util/platform.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace multireplay {

bool debugLoggingEnabled()
{
	static const bool enabled = []() {
		const char *v = getenv("OBS_MULTIREPLAY_DEBUG");
		return v && *v && *v != '0';
	}();
	return enabled;
}

namespace {

constexpr const char *kConfigFile = "config.json";

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
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (int i = 0; i < kMaxCameras && i < kMaxSegmentCameras; i++)
			segCams[i] = !config_.cameras[i].sourceName.empty();
	}
	const SessionEpoch epoch = sessionEpoch();
	SegmentIndex::instance().start(folder, segCams, epoch.masterNs,
				       epoch.wallNs);
}

void ReplayCore::load()
{
	// BEFORE loadConfig(): that call restores the last project's events, and
	// EventStore stores its marks in wall-clock time, so it needs the epoch to
	// map them back onto this session's monotonic clock.
	seatSessionEpoch();
	EventStore::instance().setSessionEpoch(sessionEpoch());

	loadConfig();

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

// A mark is only real if there is an instant behind it. Master time is
// os_gettime_ns(), never 0 on a running machine, so 0 means the tap has
// captured nothing on this angle and nothing is playing either. Storing it
// would produce an event at master 0: visible in the list, impossible to play.
bool hotkeyMarkable(int64_t tNs)
{
	if (tNs > 0)
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

void onSimpleHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		reinterpret_cast<SimpleFn>(data)();
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
	std::lock_guard<std::mutex> lock(mutex_);

	if (recording_) {
		errorOut = "already recording";
		return false;
	}

	if (!branch_output::available()) {
		errorOut = "Branch Output plugin is not installed";
		obs_log(LOG_ERROR, "startRecording: %s", errorOut.c_str());
		return false;
	}
	if (config_.sessionFolder.empty()) {
		errorOut = "session folder is not configured";
		return false;
	}

	std::error_code ec;
	std::string recFolder = recordingFolderLocked();
	std::filesystem::create_directories(recFolder, ec);

	int started = 0;
	for (int i = 0; i < kMaxCameras; i++) {
		auto &st = cameraStatus_[i];
		st = CameraStatus{};
		st.index = i;
		st.sourceName = config_.cameras[i].sourceName;
		st.configured = !st.sourceName.empty();
		if (!st.configured)
			continue;

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
		for (int i = 0; i < kMaxCameras && i < kMaxTapChannels; i++)
			wantTap[i] = cameraStatus_[i].recording;
		RingBudget budget;
		budget.kbpsPerCamera =
			config_.videoBitrateKbps + config_.audioBitrateKbps;
		PacketTap::instance().armAsync(wantTap, budget);

		// Watch the files Branch Output writes so replay can reach back
		// past the RAM window. The epoch pair ties this session's
		// monotonic clock to wall time, which is the only thing that
		// still means anything once OBS restarts.
		std::array<bool, kMaxSegmentCameras> segCams{};
		for (int i = 0; i < kMaxCameras && i < kMaxSegmentCameras; i++)
			segCams[i] = cameraStatus_[i].recording;
		// The session epoch, not a fresh sample: the anchors already on
		// disk for this folder are about to be read back through it, and
		// so are the event marks. One pair per process (see sessionEpoch).
		const SessionEpoch epoch = sessionEpoch();
		SegmentIndex::instance().start(recFolder, segCams,
					       epoch.masterNs, epoch.wallNs);
	}

	// The encoder-startup latency detector is gone with the file-based engine:
	// packets carry sys_dts_usec, so the first captured instant IS the first
	// encoded frame. Nothing left to measure or subtract.
	EventStore::instance().setSessionFolder(recordingFolderLocked());
	EventStore::instance().setLiveMode(true); // the reference controller: recording => Live
	followLive_.store(true);                  // a new take starts at the live edge
	obs_log(LOG_INFO, "Recording started on %d camera(s)", started);
	return true;
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
	for (const auto &entry : fs::directory_iterator(folder, ec)) {
		if (!entry.is_regular_file())
			continue;
		std::string name = entry.path().filename().string();
		std::string ext = entry.path().extension().string();
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
	}
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
	for (int i = 0; i < kMaxCameras; i++) {
		if (cfg.cameras[i].sourceName.empty())
			continue;
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
	std::filesystem::path p(config_.sessionFolder);
	p /= config_.currentProjectName;
	return p.string();
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
		// Sanitize title: alphanum/dash/underscore kept, spaces → '_'.
		for (unsigned char c : title) {
			if (std::isalnum(c) || c == '-' || c == '_')
				folderName += (char)c;
			else if (c == ' ' && !folderName.empty())
				folderName += '_';
		}
		if (folderName.empty()) {
			errorOut = "project name contains no valid characters";
			return false;
		}
		config_.currentProjectName = folderName;
	}
	saveConfig();

	std::error_code ec;
	std::string path = (std::filesystem::path(base) / folderName).string();
	std::filesystem::create_directories(path, ec);
	if (ec) {
		errorOut = "cannot create project folder: " + ec.message();
		return false;
	}
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
		path = (std::filesystem::path(config_.sessionFolder) / folderName)
			       .string();
		if (!std::filesystem::is_directory(path)) {
			errorOut = "project folder not found: " + path;
			return false;
		}
		config_.currentProjectName = folderName;
	}
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
	     std::filesystem::directory_iterator(base, ec)) {
		if (entry.is_directory(ec)) {
			std::string name =
				entry.path().filename().string();
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
	obs_data_t *data = obs_data_create_from_json_file(path);
	bfree(path);
	if (!data)
		return;

	std::lock_guard<std::mutex> lock(mutex_);
	config_.sessionFolder = obs_data_get_string(data, "sessionFolder");
	config_.currentProjectName =
		obs_data_get_string(data, "currentProjectName");
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
	config_.replaySourceName =
		obs_data_get_string(data, "replaySourceName");
	config_.musicSourceName =
		obs_data_get_string(data, "musicSourceName");
	if (obs_data_has_user_value(data, "autoSwitchScene"))
		config_.autoSwitchScene =
			obs_data_get_bool(data, "autoSwitchScene");
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
	obs_data_release(data);
}

void ReplayCore::saveConfig() const
{
	std::lock_guard<std::mutex> lock(mutex_);

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "sessionFolder",
			    config_.sessionFolder.c_str());
	obs_data_set_string(data, "currentProjectName",
			    config_.currentProjectName.c_str());
	obs_data_set_int(data, "port", config_.port);
	obs_data_set_int(data, "splitMinutes", config_.splitMinutes);
	obs_data_set_int(data, "videoBitrateKbps", config_.videoBitrateKbps);
	obs_data_set_int(data, "audioBitrateKbps", config_.audioBitrateKbps);
	obs_data_set_string(data, "videoEncoderId",
			    config_.videoEncoderId.c_str());
	obs_data_set_string(data, "outputSceneName",
			    config_.outputSceneName.c_str());
	obs_data_set_string(data, "replaySourceName",
			    config_.replaySourceName.c_str());
	obs_data_set_string(data, "musicSourceName",
			    config_.musicSourceName.c_str());
	obs_data_set_bool(data, "autoSwitchScene", config_.autoSwitchScene);
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

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(kConfigFile);
	if (path) {
		obs_data_save_json_safe(data, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(data);
}

} // namespace multireplay
