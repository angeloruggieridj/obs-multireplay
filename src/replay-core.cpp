/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "replay-core.hpp"
#include "branch-output-control.hpp"
#include "event-store.hpp"
#include "playback-coordinator.hpp"
#include "replay-player.hpp"
#include "plugin-support.h"

#include <util/platform.h>

#include <ctime>
#include <filesystem>
#include <system_error>

namespace multireplay {

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

void ReplayCore::load()
{
	loadConfig();

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
	if (EventStore::instance().liveMode()) {
		int64_t now = ReplayCore::instance().masterNowNs();
		if (now >= 0)
			return now;
	}
	return ReplayEngine::instance().channelA().position();
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
	 []() { EventStore::instance().markIn(hotkeyMarkTimeNs()); }},
	{"ReplayMarkOut", "Hotkey.MarkOut",
	 []() { EventStore::instance().markOut(hotkeyMarkTimeNs()); }},
	{"ReplayMarkInOut5", "Hotkey.Mark5",
	 []() { EventStore::instance().markInOut(hotkeyMarkTimeNs(), 5); }},
	{"ReplayMarkInOut10", "Hotkey.Mark10",
	 []() { EventStore::instance().markInOut(hotkeyMarkTimeNs(), 10); }},
	{"ReplayMarkInOut20", "Hotkey.Mark20",
	 []() { EventStore::instance().markInOut(hotkeyMarkTimeNs(), 20); }},
	{"ReplayPlayLastEventToOutput", "Hotkey.PlayLast",
	 []() {
		 std::string err;
		 PlaybackCoordinator::instance().playLastEvent(
			 ReplayCore::instance().getConfig().autoSwitchScene,
			 err);
	 }},
	{"ReplayPlayPause", "Hotkey.PlayPause",
	 []() {
		 auto &a = ReplayEngine::instance().channelA();
		 a.setPlaying(!a.playing());
	 }},
	{"ReplayChangeDirection", "Hotkey.Direction",
	 []() { ReplayEngine::instance().channelA().changeDirection(); }},
	{"ReplayJumpToNow", "Hotkey.JumpToNow",
	 []() { ReplayEngine::instance().channelA().jumpToEnd(); }},
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
	ReplayEngine::instance().applyTransport(
		'A', [](ReplayPlayer &p) { p.setAngle(N); });
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

	// Clear any stale session index so old recordings from a previous run
	// in the same folder are not mixed into the new session. The index will
	// be rebuilt when the user loads the session after segments are complete.
	ReplayEngine::instance().clearSession();
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
	std::filesystem::create_directories(config_.sessionFolder, ec);

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

		obs_source_t *filter =
			branch_output::ensureFilter(target, i, config_);
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
	sessionStartMinNs_ = UINT64_MAX;
	for (const auto &st : cameraStatus_) {
		if (st.recording && st.startTimestampNs < sessionStartMinNs_)
			sessionStartMinNs_ = st.startTimestampNs;
	}
	// Record wall-clock start time so SessionIndex can filter segment files
	// from previous recording sessions that share the same folder.
	sessionWallStartSec_ = (int64_t)::time(nullptr);
	EventStore::instance().setSessionFolder(config_.sessionFolder);
	EventStore::instance().setLiveMode(true); // the reference controller: recording => Live
	writeSessionManifest();
	obs_log(LOG_INFO, "Recording started on %d camera(s)", started);
	return true;
}

bool ReplayCore::stopRecording()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!recording_)
		return false;

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
	std::lock_guard<std::mutex> lock(mutex_);
	if (recording_) {
		errorOut = "stop recording first";
		return false;
	}
	if (config_.sessionFolder.empty()) {
		errorOut = "no session folder configured";
		return false;
	}

	namespace fs = std::filesystem;
	std::error_code ec;
	int removed = 0;
	for (const auto &entry :
	     fs::directory_iterator(config_.sessionFolder, ec)) {
		if (!entry.is_regular_file())
			continue;
		std::string name = entry.path().filename().string();
		std::string ext = entry.path().extension().string();
		bool isRecording = name.rfind("cam", 0) == 0 &&
				   (ext == ".mp4" || ext == ".mov");
		bool isMeta = name == "session.json" ||
			      name == "events.json";
		if (isRecording || isMeta) {
			fs::remove(entry.path(), ec);
			if (!ec)
				removed++;
		}
	}
	obs_log(LOG_INFO, "Delete All: removed %d file(s)", removed);
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
}

std::string ReplayCore::pickVideoEncoder() const
{
	for (const char *id : kEncoderPreference) {
		if (encoderExists(id))
			return id;
	}
	return "obs_x264";
}

int64_t ReplayCore::masterNowNs() const
{
	if (!recording_ || sessionStartMinNs_ == 0 ||
	    sessionStartMinNs_ == UINT64_MAX)
		return -1;
	return (int64_t)(os_gettime_ns() - sessionStartMinNs_);
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
	std::lock_guard<std::mutex> lock(mutex_);

	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "version", PLUGIN_VERSION);
	obs_data_set_bool(root, "recording", recording_);
	obs_data_set_bool(root, "branchOutputAvailable",
			  branch_output::available());
	obs_data_set_string(root, "sessionFolder",
			    config_.sessionFolder.c_str());
	obs_data_set_int(root, "diskFreeBytes", diskFreeBytes());
	obs_data_set_int(root, "estimatedMinutesRemaining",
			 estimatedMinutesRemaining());
	obs_data_set_string(root, "videoEncoder",
			    config_.videoEncoderId.empty()
				    ? pickVideoEncoder().c_str()
				    : config_.videoEncoderId.c_str());

	obs_data_array_t *cams = obs_data_array_create();
	for (const auto &st : cameraStatus_) {
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
			config_.sessionFolder);

	obs_data_array_t *cams = obs_data_get_array(data, "cameras");
	if (cams) {
		size_t count = obs_data_array_count(cams);
		for (size_t i = 0; i < count && i < kMaxCameras; i++) {
			obs_data_t *item = obs_data_array_item(cams, i);
			config_.cameras[i].sourceName =
				obs_data_get_string(item, "sourceName");
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
	obs_data_set_int(data, "port", config_.port);
	obs_data_set_int(data, "splitMinutes", config_.splitMinutes);
	obs_data_set_int(data, "videoBitrateKbps", config_.videoBitrateKbps);
	obs_data_set_int(data, "audioBitrateKbps", config_.audioBitrateKbps);
	obs_data_set_string(data, "videoEncoderId",
			    config_.videoEncoderId.c_str());
	obs_data_set_string(data, "outputSceneName",
			    config_.outputSceneName.c_str());
	obs_data_set_string(data, "musicSourceName",
			    config_.musicSourceName.c_str());
	obs_data_set_bool(data, "autoSwitchScene", config_.autoSwitchScene);
	obs_data_set_string(data, "recFormat", config_.recFormat.c_str());

	obs_data_array_t *cams = obs_data_array_create();
	for (const auto &cam : config_.cameras) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "sourceName",
				    cam.sourceName.c_str());
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

void ReplayCore::writeSessionManifest() const
{
	// session.json: per-camera start timestamps on the shared monotonic
	// clock. This is the seed of the master timeline (M2 builds the full
	// frame index on top of it).
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "manifestVersion", 1);
	obs_data_set_int(data, "createdWallClock",
			 (int64_t)time(nullptr));

	obs_data_array_t *cams = obs_data_array_create();
	for (const auto &st : cameraStatus_) {
		if (!st.recording)
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "camera", st.index + 1);
		obs_data_set_string(item, "sourceName", st.sourceName.c_str());
		obs_data_set_int(item, "startTimestampNs",
				 static_cast<int64_t>(st.startTimestampNs));
		obs_data_array_push_back(cams, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "cameras", cams);
	obs_data_array_release(cams);

	std::filesystem::path p(config_.sessionFolder);
	p /= "session.json";
	obs_data_save_json_safe(data, p.string().c_str(), "tmp", "bak");
	obs_data_release(data);
}

} // namespace multireplay
