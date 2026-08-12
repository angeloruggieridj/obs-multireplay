/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See selftest.hpp. This is the scripted form of the M0 gate.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)
#include <obs-frontend-api.h>

#include "selftest.hpp"

#include "branch-output-control.hpp"
#include "packet-tap.hpp"
#include "plugin-support.h"
#include "replay-core.hpp"

#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace multireplay {

namespace {

std::atomic<bool> g_started{false};

std::string envStr(const char *key, const std::string &def = {})
{
	const char *v = getenv(key);
	return (v && *v) ? std::string(v) : def;
}

bool envOn(const char *key)
{
	const char *v = getenv(key);
	return v && *v && strcmp(v, "0") != 0;
}

int envInt(const char *key, int def)
{
	const std::string s = envStr(key);
	if (s.empty())
		return def;
	try {
		return std::stoi(s);
	} catch (...) {
		return def;
	}
}

// Split "A,B,C" into names, trimming surrounding blanks.
std::vector<std::string> splitCsv(const std::string &s)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size() && !s.empty()) {
		size_t comma = s.find(',', start);
		if (comma == std::string::npos)
			comma = s.size();
		std::string item = s.substr(start, comma - start);
		size_t b = item.find_first_not_of(" \t");
		size_t e = item.find_last_not_of(" \t");
		if (b != std::string::npos)
			out.push_back(item.substr(b, e - b + 1));
		if (comma == s.size())
			break;
		start = comma + 1;
	}
	return out;
}

// Run `fn` on the OBS UI thread and wait for it.
//
// This matters more than it looks: Branch Output evaluates its start conditions
// from a QTimer owned by the filter. A QTimer created on a plain std::thread
// has no event loop, so it never fires and the filter is created but never
// starts recording. In the normal product flow the filters are created from the
// dock (already the UI thread); the self-test has to ask for it explicitly.
void runOnUi(const std::function<void()> &fn)
{
	if (obs_in_task_thread(OBS_TASK_UI)) {
		fn();
		return;
	}
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) { (*static_cast<const std::function<void()> *>(p))(); },
		const_cast<std::function<void()> *>(&fn), true);
}

// A synthetic camera. OBS renamed the colour source a few times; try newest first.
obs_source_t *createSyntheticCamera(int idx, uint32_t cx, uint32_t cy)
{
	static const char *kColorIds[] = {"color_source_v3", "color_source_v2",
					  "color_source"};
	// Distinct colours so the recordings are visually distinguishable.
	static const uint32_t kColors[] = {0xFF2266DD, 0xFF22AA55, 0xFFDD8822,
					   0xFFCC3355, 0xFF8844CC, 0xFF11AAAA,
					   0xFFBBBB22, 0xFF777777};

	const std::string name = "MRSelfTest Cam" + std::to_string(idx + 1);

	for (const char *id : kColorIds) {
		if (!obs_source_get_display_name(id))
			continue;
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "color",
				 kColors[idx % (int)(sizeof(kColors) /
						     sizeof(kColors[0]))]);
		obs_data_set_int(s, "width", cx);
		obs_data_set_int(s, "height", cy);
		obs_source_t *src = obs_source_create(id, name.c_str(), s, nullptr);
		obs_data_release(s);
		if (src) {
			obs_log(LOG_INFO, "[selftest] created synthetic camera '%s' (%s)",
				name.c_str(), id);
			return src;
		}
	}
	obs_log(LOG_ERROR, "[selftest] no colour source type available");
	return nullptr;
}

void runSelfTest()
{
	const int durationSecs = envInt("OBS_MULTIREPLAY_SELFTEST_SECS", 25);
	const std::string sourcesCsv = envStr("OBS_MULTIREPLAY_SELFTEST_SOURCES");
	const bool useRealSources = !sourcesCsv.empty();
	std::vector<std::string> realNames = splitCsv(sourcesCsv);

	int camCount = useRealSources
			       ? (int)realNames.size()
			       : envInt("OBS_MULTIREPLAY_SELFTEST_CAMS", 2);
	camCount = std::max(1, std::min(camCount, kMaxCameras));

	std::string outPath = envStr("OBS_MULTIREPLAY_SELFTEST_OUT");
	if (outPath.empty()) {
		std::error_code ec;
		outPath = (std::filesystem::temp_directory_path(ec) /
			   "obs-multireplay-selftest.json")
				  .string();
	}

	obs_log(LOG_INFO,
		"[selftest] START — cams=%d duration=%ds sources=%s out=%s",
		camCount, durationSecs,
		useRealSources ? sourcesCsv.c_str() : "(synthetic)",
		outPath.c_str());

	// Let OBS finish settling (scene collection, devices, encoders).
	std::this_thread::sleep_for(std::chrono::seconds(3));

	struct obs_video_info ovi = {};
	const bool haveOvi = obs_get_video_info(&ovi);
	const uint32_t cx = haveOvi ? ovi.base_width : 1920;
	const uint32_t cy = haveOvi ? ovi.base_height : 1080;
	const double canvasFps =
		haveOvi && ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 30.0;

	// A throwaway Config: never touches the operator's persisted settings.
	Config cfg;
	std::error_code ec;
	const std::filesystem::path folder =
		std::filesystem::temp_directory_path(ec) / "obs-multireplay-selftest";
	std::filesystem::create_directories(folder, ec);
	cfg.sessionFolder = folder.string();
	cfg.splitMinutes = 20;

	// --- Acquire the cameras and arm Branch Output ------------------------
	// All of this runs on the UI thread (see runOnUi): a Branch Output filter
	// created off it never starts recording.
	std::vector<obs_source_t *> cams(camCount, nullptr);
	std::vector<bool> owned(camCount, false);
	obs_scene_t *testScene = nullptr;
	std::array<bool, kMaxTapChannels> want{};
	int armed = 0;

	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (useRealSources) {
				cams[i] = obs_get_source_by_name(
					realNames[i].c_str());
				if (!cams[i])
					obs_log(LOG_ERROR,
						"[selftest] source '%s' not found",
						realNames[i].c_str());
			} else {
				cams[i] = createSyntheticCamera(i, cx, cy);
				owned[i] = cams[i] != nullptr;
				if (cams[i]) {
					obs_source_inc_showing(cams[i]);
					obs_source_inc_active(cams[i]);
				}
			}
		}

		// Branch Output ignores a filter whose parent is not part of the
		// frontend scene collection (sourceInFrontend()), so the synthetic
		// cameras need a scene to live in. It does NOT have to be the
		// program scene — Branch Output renders through its own obs_view —
		// so we never touch what the operator has on air.
		if (!useRealSources) {
			testScene = obs_scene_create("MRSelfTest Scene");
			if (testScene) {
				for (int i = 0; i < camCount; i++)
					if (cams[i])
						obs_scene_add(testScene, cams[i]);
				obs_log(LOG_INFO,
					"[selftest] synthetic cameras placed in "
					"'MRSelfTest Scene'");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] could not create the test scene");
			}
		}

		for (int i = 0; i < camCount; i++) {
			if (!cams[i])
				continue;
			obs_source_t *filter =
				branch_output::ensureFilter(cams[i], i, cfg);
			if (!filter) {
				obs_log(LOG_ERROR,
					"[selftest] cam%d: filter creation failed",
					i + 1);
				continue;
			}
			branch_output::setEnabled(filter, true);
			obs_source_release(filter);
			want[i] = true;
			armed++;
		}
	});
	obs_log(LOG_INFO, "[selftest] armed %d Branch Output filter(s)", armed);

	const uint32_t laggedBefore = obs_get_lagged_frames();
	const uint32_t totalBefore = obs_get_total_frames();

	PacketTap::instance().armAsync(want);

	// --- Measure ---------------------------------------------------------
	for (int s = 0; s < durationSecs; s++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (s == 5 || s == durationSecs - 1)
			obs_log(LOG_INFO, "%s",
				PacketTap::instance().report().c_str());
	}

	const uint32_t laggedAfter = obs_get_lagged_frames();
	const uint32_t totalAfter = obs_get_total_frames();
	const int64_t skewNs = PacketTap::instance().crossAngleSkewNs();
	const int attached = PacketTap::instance().attachedCount();

	std::vector<TapStats> stats;
	for (int i = 0; i < camCount; i++)
		stats.push_back(PacketTap::instance().stats(i));

	// --- Tear down in the order the lifecycle demands ---------------------
	// Detach BEFORE disabling the filters: Branch Output frees its encoder
	// in releaseInfrastructureIfIdle() once its own outputs go idle.
	PacketTap::instance().detachAll();

	// Tearing down a Branch Output filter destroys its QTimer, so this goes
	// back on the UI thread too.
	runOnUi([&]() {
		if (testScene) {
			obs_source_remove(obs_scene_get_source(testScene));
			obs_scene_release(testScene);
		}

		for (int i = 0; i < camCount; i++) {
			if (!cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				branch_output::setEnabled(f, false);
				obs_source_filter_remove(cams[i], f);
				obs_source_release(f);
			}
			if (owned[i]) {
				obs_source_dec_active(cams[i]);
				obs_source_dec_showing(cams[i]);
				obs_source_remove(cams[i]);
			}
			obs_source_release(cams[i]);
		}
	});

	// --- Verdict ----------------------------------------------------------
	const double frameMs = 1000.0 / (canvasFps > 0 ? canvasFps : 30.0);

	bool passAttached = attached == armed && armed > 0;
	bool passNoNewEncoder = armed > 0;
	bool passPackets = armed > 0;
	int64_t worstMaxAgeMs = 0;
	for (const auto &s : stats) {
		if (!want[s.camIndex])
			continue;
		if (!s.encoderWasAlreadyActive)
			passNoNewEncoder = false;
		if (s.videoPackets == 0)
			passPackets = false;
		worstMaxAgeMs = std::max(worstMaxAgeMs, s.maxAgeUsec / 1000);
	}
	// Live edge must beat the ~1 s fragment flush by a wide margin; we allow
	// 250 ms before calling it a failure, and report the real number anyway.
	const bool passLatency = passPackets && worstMaxAgeMs <= 250;
	// Sampling is asynchronous, so allow two frame times of apparent skew.
	const bool passSkew = attached < 2 ||
			      (double)(skewNs / 1000000) <= frameMs * 2.0;
	const uint32_t laggedDelta = laggedAfter - laggedBefore;
	const uint32_t totalDelta = totalAfter - totalBefore;
	const double laggedPct =
		totalDelta ? 100.0 * laggedDelta / totalDelta : 0.0;
	const bool passImpact = laggedPct <= 1.0;

	const bool pass = passAttached && passNoNewEncoder && passPackets &&
			  passLatency && passSkew && passImpact;

	// --- Report -----------------------------------------------------------
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "verdict", pass ? "PASS" : "FAIL");
	obs_data_set_string(root, "plugin_version", PLUGIN_VERSION);
	obs_data_set_int(root, "cameras_armed", armed);
	obs_data_set_int(root, "cameras_attached", attached);
	obs_data_set_int(root, "duration_secs", durationSecs);
	obs_data_set_bool(root, "synthetic_sources", !useRealSources);
	obs_data_set_int(root, "canvas_width", cx);
	obs_data_set_int(root, "canvas_height", cy);
	obs_data_set_double(root, "canvas_fps", canvasFps);

	obs_data_t *checks = obs_data_create();
	obs_data_set_bool(checks, "all_channels_attached", passAttached);
	obs_data_set_bool(checks, "no_new_encoder_created", passNoNewEncoder);
	obs_data_set_bool(checks, "packets_received", passPackets);
	obs_data_set_bool(checks, "live_edge_latency_ok", passLatency);
	obs_data_set_bool(checks, "cross_angle_skew_ok", passSkew);
	obs_data_set_bool(checks, "obs_impact_ok", passImpact);
	obs_data_set_obj(root, "checks", checks);
	obs_data_release(checks);

	obs_data_set_int(root, "worst_max_packet_age_ms", worstMaxAgeMs);
	obs_data_set_int(root, "cross_angle_skew_ms", skewNs / 1000000);
	obs_data_set_int(root, "obs_lagged_frames_delta", laggedDelta);
	obs_data_set_int(root, "obs_total_frames_delta", totalDelta);
	obs_data_set_double(root, "obs_lagged_pct", laggedPct);

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &s : stats) {
		if (!want[s.camIndex])
			continue;
		obs_data_t *c = obs_data_create();
		obs_data_set_int(c, "cam", s.camIndex + 1);
		obs_data_set_string(c, "bo_output_name", s.outputName.c_str());
		obs_data_set_string(c, "encoder_id", s.encoderId.c_str());
		obs_data_set_string(c, "encoder_name", s.encoderName.c_str());
		obs_data_set_bool(c, "encoder_was_already_active",
				  s.encoderWasAlreadyActive);
		obs_data_set_int(c, "video_packets", (long long)s.videoPackets);
		obs_data_set_int(c, "keyframes", (long long)s.keyframes);
		obs_data_set_int(c, "audio_packets", (long long)s.audioPackets);
		obs_data_set_int(c, "video_bytes", (long long)s.videoBytes);
		obs_data_set_int(c, "first_packet_wait_ms", s.firstPacketWaitMs);
		obs_data_set_int(c, "packet_age_avg_ms", s.avgAgeUsec / 1000);
		obs_data_set_int(c, "packet_age_max_ms", s.maxAgeUsec / 1000);
		obs_data_array_push_back(arr, c);
		obs_data_release(c);
	}
	obs_data_set_array(root, "channels", arr);
	obs_data_array_release(arr);

	if (!obs_data_save_json_safe(root, outPath.c_str(), "tmp", "bak"))
		obs_log(LOG_ERROR, "[selftest] could not write report to %s",
			outPath.c_str());
	obs_log(LOG_INFO, "[selftest] VERDICT=%s — report written to %s",
		pass ? "PASS" : "FAIL", outPath.c_str());
	obs_data_release(root);

	// There is no public obs_frontend quit API, so the runner script closes
	// OBS once this report file appears. Writing it is the "done" signal.
}

} // namespace

void maybeRunSelfTest()
{
	if (!envOn("OBS_MULTIREPLAY_SELFTEST"))
		return;
	if (g_started.exchange(true))
		return; // FINISHED_LOADING can fire more than once
	std::thread(runSelfTest).detach();
}

} // namespace multireplay
