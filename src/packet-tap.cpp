/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See packet-tap.hpp for the rationale and the libobs references that make this
safe. This file is the M0 spike: it attaches, measures and logs. It does not
yet keep the packets — the ring lands in M1.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "packet-tap.hpp"

#include "branch-output-control.hpp"
#include "plugin-support.h"
#include "replay-core.hpp" // MR_DLOG

#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace multireplay {

namespace {

// Two registered types: libobs refuses to start an OBS_OUTPUT_AV output whose
// audio encoder is missing (can_begin_data_capture → audio_valid, obs-output.c
// :1319), and a Branch Output filter can legitimately record video only.
constexpr const char *kTapOutputIdAV = "multireplay_tap_av";
constexpr const char *kTapOutputIdV = "multireplay_tap_v";

// Be permissive: we accept whatever Branch Output's encoder happens to be.
constexpr const char *kVideoCodecs = "h264;hevc;av1;prores";
constexpr const char *kAudioCodecs =
	"aac;opus;alac;flac;pcm_s16le;pcm_s24le;pcm_f32le";

constexpr int kArmPollMs = 250;
// Branch Output builds its infrastructure and starts its output asynchronously
// after the filter is enabled; give it a generous window before giving up.
constexpr int kArmMaxAttempts = 160; // ~40 s
// How long an attached channel may stay silent before we suspect we are on the
// wrong encoder. Comfortably longer than the first-keyframe wait (keyint is 1 s)
// and far shorter than anyone's patience during a match.
constexpr int64_t kSilentAttachMs = 3000;
constexpr int kReportEveryTicks = 120; // ~30 s

// WHAT A DETACHED DISPOSAL THREAD IS STILL ALLOWED TO TOUCH (A5).
//
// dispose() runs on a thread nobody joins, because obs_output_stop() on our
// tap output can block indefinitely when Branch Output is tearing the shared
// encoder down at the same moment (measured: 20 s and still going). unload()
// therefore waits a bounded time and then CARRIES ON — which used to leave that
// thread about to re-take PacketTap::mutex_, write into a Channel and call
// obs_log, all of which belong to a module that is being taken away.
//
// This block is heap-allocated and every disposal thread holds a share of it,
// so the counter it decrements is alive whatever happens. Past `moduleGone` a
// disposal touches libobs and nothing else — no member of ours, and above all
// no obs_log, which is OUR function in OUR binary.
struct DisposalState {
	std::atomic<int> inFlight{0};
	std::atomic<bool> moduleGone{false};
};

std::shared_ptr<DisposalState> disposalState()
{
	static std::shared_ptr<DisposalState> s =
		std::make_shared<DisposalState>();
	return s;
}

} // namespace

PacketTap &PacketTap::instance()
{
	static PacketTap tap;
	return tap;
}

PacketTap::~PacketTap()
{
	unload();
}

// ---------------------------------------------------------------------------
// obs_output_info callbacks
// ---------------------------------------------------------------------------

const char *PacketTap::tapGetName(void *)
{
	return "MultiReplay Packet Tap";
}

void *PacketTap::tapCreate(obs_data_t *settings, obs_output_t *output)
{
	const int idx = (int)obs_data_get_int(settings, "cam_index");
	if (idx < 0 || idx >= kMaxTapChannels)
		return nullptr;

	Channel *ch = &PacketTap::instance().channels_[idx];
	ch->camIndex = idx;
	ch->tapOutput = output; // ownership is taken by attachLocked()
	return ch;
}

void PacketTap::tapDestroy(void *)
{
	// Channels live in the singleton; nothing to free here.
}

bool PacketTap::tapStart(void *data)
{
	auto *ch = static_cast<Channel *>(data);
	if (!ch || !ch->tapOutput)
		return false;
	// This is what appends us to the encoder's callback list
	// (obs-output.c:2360-2369 → obs_encoder_start).
	return obs_output_begin_data_capture(ch->tapOutput, 0);
}

void PacketTap::tapStop(void *data, uint64_t)
{
	auto *ch = static_cast<Channel *>(data);
	if (ch && ch->tapOutput)
		obs_output_end_data_capture(ch->tapOutput);
}

void PacketTap::tapEncodedPacket(void *data, struct encoder_packet *packet)
{
	auto *ch = static_cast<Channel *>(data);
	if (!ch || !packet)
		return;

	// Runs on the ENCODER THREAD, so nothing here may block it for long.
	//
	// The counters below are atomics and cost nothing. The tail of this
	// function is not free, and the comment used to claim it was ("atomics
	// only, no locks, no allocation"): keeping the bytes means ONE allocation
	// (lp.data.assign) and ONE short critical section on ringMutex to push.
	// That is the deal the ring is built on, and stating it wrongly on the
	// hottest path in the plugin is how someone later adds a second lock here
	// believing the first one is not there (L1).
	const int64_t nowNs = (int64_t)os_gettime_ns();

	if (packet->type == OBS_ENCODER_VIDEO) {
		ch->videoPackets.fetch_add(1, std::memory_order_relaxed);
		ch->videoBytes.fetch_add(packet->size, std::memory_order_relaxed);
		if (packet->keyframe)
			ch->keyframes.fetch_add(1, std::memory_order_relaxed);

		// The live-edge latency we are here to measure: how old the frame
		// is by the time we hold its encoded bytes. This is what has to
		// beat the ~1 s fragment flush of the file-based path.
		const int64_t age = nowNs / 1000 - packet->sys_dts_usec;
		ch->lastAgeUsec.store(age, std::memory_order_relaxed);
		ch->ageSumUsec.fetch_add(age, std::memory_order_relaxed);
		ch->ageSamples.fetch_add(1, std::memory_order_relaxed);
		int64_t prevMax = ch->maxAgeUsec.load(std::memory_order_relaxed);
		while (age > prevMax &&
		       !ch->maxAgeUsec.compare_exchange_weak(
			       prevMax, age, std::memory_order_relaxed))
			;

		// The shared cross-encoder clock — the whole reason this design
		// removes ReplayCore::frameLagNs().
		ch->lastSysDtsNs.store(packet->sys_dts_usec * 1000,
				       std::memory_order_relaxed);

		if (ch->firstPacketWaitMs.load(std::memory_order_relaxed) < 0) {
			const int64_t arm =
				ch->armWallNs.load(std::memory_order_relaxed);
			if (arm > 0)
				ch->firstPacketWaitMs.store(
					(nowNs - arm) / 1000000,
					std::memory_order_relaxed);
		}
	} else {
		ch->audioPackets.fetch_add(1, std::memory_order_relaxed);
		ch->audioBytes.fetch_add(packet->size, std::memory_order_relaxed);
	}

	// --- place the packet on the master clock and keep its bytes ---------
	const bool isVideo = packet->type == OBS_ENCODER_VIDEO;
	// One timeline per track, each touched only by its own encoder thread.
	MasterTimeline &tl = isVideo ? ch->videoTl : ch->audioTl;

	MasterTimeline::Input in;
	in.pts = packet->pts;
	in.dts = packet->dts;
	in.timebaseNum = packet->timebase_num;
	in.timebaseDen = packet->timebase_den;
	in.sysDtsUsec = packet->sys_dts_usec;

	const MasterTimeline::Result r = tl.normalize(in);
	if (!r.ok) {
		// Cannot be placed on the shared clock, so it cannot be aligned
		// with the other angles. Dropping it is correct; hiding it is not.
		ch->malformed.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	// generation 0 with discontinuity set is just the first packet seating
	// the offset, not a break in the footage.
	if (r.discontinuity && r.generation > 0) {
		ch->discontinuities.fetch_add(1, std::memory_order_relaxed);
		ch->generation.fetch_add(1, std::memory_order_relaxed);
	}

	LivePacket lp;
	lp.kind = isVideo ? PacketKind::Video : PacketKind::Audio;
	lp.keyframe = packet->keyframe;
	lp.masterNs = r.masterNs;
	lp.dtsNs = r.dtsNs;
	lp.generation = ch->generation.load(std::memory_order_relaxed);
	lp.data.assign(packet->data, packet->data + packet->size);

	{
		std::lock_guard<std::mutex> lock(ch->ringMutex);
		ch->ring.push(std::move(lp));
	}
}

void PacketTap::onBoOutputStopping(void *param, calldata_t *)
{
	auto *ch = static_cast<Channel *>(param);
	if (!ch)
		return;
	// Fires on an OBS thread we do not own. Do NOT touch the mutex here:
	// just raise the flag and let the arm thread tear down. Branch Output
	// releases its encoder in releaseInfrastructureIfIdle() once its own
	// outputs are idle, so we must be gone before that happens.
	ch->detachRequested.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void PacketTap::load()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (registered_)
		return;

	struct obs_output_info info = {};
	info.id = kTapOutputIdAV;
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	info.get_name = tapGetName;
	info.create = tapCreate;
	info.destroy = tapDestroy;
	info.start = tapStart;
	info.stop = tapStop;
	info.encoded_packet = tapEncodedPacket;
	info.encoded_video_codecs = kVideoCodecs;
	info.encoded_audio_codecs = kAudioCodecs;
	obs_register_output(&info);

	// Video-only variant for filters recording without audio.
	info.id = kTapOutputIdV;
	info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	info.encoded_audio_codecs = nullptr;
	obs_register_output(&info);

	registered_ = true;
	obs_log(LOG_INFO, "[tap] output types registered (%s, %s)",
		kTapOutputIdAV, kTapOutputIdV);
}

void PacketTap::unload()
{
	if (armRunning_.exchange(false)) {
		armWake_.notify_all();
		if (armThread_.joinable())
			armThread_.join();
	}
	detachAll();

	// Give the disposal threads a moment to be done with libobs objects
	// before the module goes away under them. Bounded: one of them may be
	// stuck in obs_output_stop() forever (that is why they are detached),
	// and waiting for that would trade a leak for a hang on every exit.
	auto state = disposalState();
	for (int i = 0; i < 30 && state->inFlight.load() > 0; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	if (state->inFlight.load() > 0)
		obs_log(LOG_WARNING,
			"[tap] %d detach(es) still finishing at unload — Branch "
			"Output was tearing down the encoder at the same time",
			state->inFlight.load());
	// From here a disposal that comes back late touches libobs and its own
	// heap block, and nothing of ours (A5).
	state->moduleGone.store(true);
}

void PacketTap::armAsync(const std::array<bool, kMaxTapChannels> &wanted,
			 const RingBudget &budget)
{
	// Size the live history from the configured bitrate, with headroom for
	// the peaks a CBR encoder still produces. The duration cap is the real
	// intent; the byte cap is the guard that keeps a runaway bitrate from
	// eating RAM.
	RingLimits limits;
	limits.maxDurationNs = (int64_t)budget.seconds * 1'000'000'000LL;
	limits.maxBytes = (size_t)((int64_t)budget.kbpsPerCamera * 1000 / 8 *
				   budget.seconds * 3 / 2);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		wanted_ = wanted;
		for (int i = 0; i < kMaxTapChannels; i++) {
			auto &ch = channels_[i];
			if (!wanted_[i] || ch.attached.load())
				continue;

			// Fresh session: forget the previous alignment and
			// history entirely rather than carrying a stale offset.
			ch.videoTl = MasterTimeline{};
			ch.audioTl = MasterTimeline{};
			ch.generation.store(0);
			ch.malformed.store(0);
			ch.discontinuities.store(0);
			{
				std::lock_guard<std::mutex> rlock(ch.ringMutex);
				ch.ring.clear();
				ch.ring.setLimits(limits);
			}
			// Reset the per-session measurements.
			ch.camIndex = i;
			ch.videoPackets.store(0);
			ch.audioPackets.store(0);
			ch.videoBytes.store(0);
			ch.audioBytes.store(0);
			ch.keyframes.store(0);
			ch.firstPacketWaitMs.store(-1);
			ch.lastAgeUsec.store(0);
			ch.maxAgeUsec.store(0);
			ch.ageSumUsec.store(0);
			ch.ageSamples.store(0);
			ch.lastSysDtsNs.store(0);
			ch.detachRequested.store(false);
			ch.armWallNs.store((int64_t)os_gettime_ns());
		}
	}

	obs_log(LOG_INFO,
		"[tap] live history budget: %d s per camera, cap %zu MB",
		budget.seconds, limits.maxBytes / (1024 * 1024));

	if (armRunning_.exchange(true))
		return; // already running; the new wanted_ set is picked up
	// A previous arm cycle may have ended without unload() joining it.
	if (armThread_.joinable())
		armThread_.join();
	armThread_ = std::thread([this]() { armLoop(); });
}

void PacketTap::armLoop()
{
	int attempts[kMaxTapChannels] = {};
	int tick = 0;

	while (armRunning_.load()) {
		// Filled under the lock, emptied after it: see Doomed.
		std::vector<Doomed> doomed;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			armWake_.wait_for(lock,
					  std::chrono::milliseconds(kArmPollMs),
					  [this]() { return !armRunning_.load(); });
			if (!armRunning_.load())
				break;

			for (int i = 0; i < kMaxTapChannels; i++) {
				auto &ch = channels_[i];

				if (ch.attached.load() &&
				    ch.detachRequested.load(
					    std::memory_order_acquire)) {
					obs_log(LOG_INFO,
						"[tap] cam%d: Branch Output is "
						"stopping — detaching",
						i + 1);
					doomed.push_back(detachLocked(i));
					continue;
				}

				// Attached and SILENT: we are hanging off a
				// Branch Output that no longer exists.
				//
				// Measured, not hypothesised: start a take a few
				// seconds after the previous one stopped, and the
				// output named "MultiReplay camN" is momentarily
				// the OLD one — still active, still carrying the
				// old encoder — while Branch Output builds its
				// replacement a fraction of a second later
				// (">>> new qsv encoder" in the log). Attach in
				// that window and the angle reports "attached"
				// and receives nothing for the whole match, which
				// is the worst failure this plugin can have: a
				// camera that looks armed and records air.
				//
				// It is NOT enough to compare encoders: Branch
				// Output creates a whole new obs_output, so the
				// dead one we hold keeps pointing at the dead
				// encoder and the two always agree. What settles
				// it is asking who answers to that NAME now.
				if (ch.attached.load() &&
				    ch.videoPackets.load() == 0 && ch.boOutput &&
				    (int64_t)os_gettime_ns() -
						    ch.armWallNs.load() >
					    kSilentAttachMs * 1'000'000LL) {
					const std::string boName =
						std::string(branch_output::
								    kFilterNamePrefix) +
						std::to_string(i + 1);
					obs_output_t *cur = obs_get_output_by_name(
						boName.c_str());
					const bool stale =
						cur != ch.boOutput ||
						!obs_output_active(ch.boOutput) ||
						obs_output_get_video_encoder(
							ch.boOutput) !=
							ch.videoEncoder;
					if (cur)
						obs_output_release(cur);
					if (stale) {
						obs_log(LOG_WARNING,
							"[tap] cam%d: attached but "
							"silent for %lld ms and "
							"Branch Output has moved on "
							"— re-attaching",
							i + 1,
							(long long)kSilentAttachMs);
						doomed.push_back(detachLocked(i));
						attempts[i] = 0;
						continue;
					}
				}

				if (!wanted_[i] || ch.attached.load())
					continue;
				if (attempts[i] >= kArmMaxAttempts)
					continue;

				attempts[i]++;
				if (attachLocked(i)) {
					attempts[i] = 0;
				} else if (attempts[i] == kArmMaxAttempts) {
					// Almost always one cause. Branch Output
					// decides when to record from a global
					// Interlock setting in ITS dock, not from
					// anything on the filter, so we can neither
					// set it nor read it. On anything other than
					// "Always ON" our REC enables the filter and
					// Branch Output simply declines to start -
					// silently, as far as the operator can see.
					// Naming the likely cause beats reporting
					// the symptom.
					obs_log(LOG_WARNING,
						"[tap] cam%d: no active Branch Output "
						"output named '%s%d' after %d s. Check "
						"Branch Output's Interlock setting: it "
						"must be 'Always ON' for REC to arm the "
						"recordings.",
						i + 1,
						branch_output::kFilterNamePrefix,
						i + 1,
						kArmMaxAttempts * kArmPollMs / 1000);
				}
			}
		}

		// Outside the lock, always: this is where a stuck teardown would
		// otherwise take the whole tap down with it.
		for (auto &d : doomed)
			dispose(std::move(d));

		if (++tick >= kReportEveryTicks) {
			tick = 0;
			if (anyAttached())
				obs_log(LOG_INFO, "%s", report().c_str());
		}
	}
}

bool PacketTap::attachLocked(int camIndex)
{
	Channel &ch = channels_[camIndex];
	if (ch.attached.load())
		return true;
	// A previous attachment is still being let go of (see Doomed). Attaching
	// now would overwrite the output its stop callback still needs.
	if (ch.disposing.load())
		return false;

	// Branch Output names its recording output after the filter's source
	// name (plugin-main.cpp: name(obs_source_get_name(source))), and we own
	// those names. Its streaming outputs carry a " (index)" suffix, so a
	// plain lookup cannot collide with them.
	const std::string boName = std::string(branch_output::kFilterNamePrefix) +
				   std::to_string(camIndex + 1);

	obs_output_t *bo = obs_get_output_by_name(boName.c_str()); // add-ref'd
	if (!bo)
		return false; // not started yet — retry
	if (!obs_output_active(bo)) {
		obs_output_release(bo);
		return false;
	}

	obs_encoder_t *venc = obs_output_get_video_encoder(bo); // not add-ref'd
	if (!venc) {
		obs_output_release(bo);
		return false;
	}

	// Hold our own references so the objects cannot vanish under us.
	obs_encoder_t *vencRef = obs_encoder_get_ref(venc);
	if (!vencRef) {
		obs_output_release(bo);
		return false;
	}
	obs_encoder_t *aencRef = nullptr;
	if (obs_encoder_t *aenc = obs_output_get_audio_encoder(bo, 0))
		aencRef = obs_encoder_get_ref(aenc);

	// The identity proof for M0 criterion 3: this is Branch Output's own
	// encoder object, already running. We create nothing.
	ch.encoderPtr = (const void *)venc;
	ch.encoderId = obs_encoder_get_id(venc) ? obs_encoder_get_id(venc) : "?";
	ch.encoderName =
		obs_encoder_get_name(venc) ? obs_encoder_get_name(venc) : "?";
	ch.encoderWasAlreadyActive = obs_encoder_active(venc);

	// Capture what a decoder will need. Without the parameter sets the ring
	// holds bytes nobody can decode: OBS keeps SPS/PPS out of band by
	// default, so they have to be taken from the encoder here.
	{
		StreamConfig cfg;
		if (const char *codec = obs_encoder_get_codec(venc))
			cfg.videoCodec = codec;
		cfg.width = obs_encoder_get_width(venc);
		cfg.height = obs_encoder_get_height(venc);

		uint8_t *extra = nullptr;
		size_t extraSize = 0;
		if (obs_encoder_get_extra_data(venc, &extra, &extraSize) && extra &&
		    extraSize)
			cfg.videoExtradata.assign(extra, extra + extraSize);

		if (aencRef) {
			if (const char *acodec = obs_encoder_get_codec(aencRef))
				cfg.audioCodec = acodec;
			cfg.sampleRate = obs_encoder_get_sample_rate(aencRef);
			uint8_t *aextra = nullptr;
			size_t aextraSize = 0;
			if (obs_encoder_get_extra_data(aencRef, &aextra,
						       &aextraSize) &&
			    aextra && aextraSize)
				cfg.audioExtradata.assign(aextra,
							  aextra + aextraSize);
		}

		std::lock_guard<std::mutex> rlock(ch.ringMutex);
		ch.config = std::move(cfg);
	}
	if (ch.config.videoExtradata.empty())
		obs_log(LOG_WARNING,
			"[tap] cam%d: encoder exposed no video extradata - the "
			"ring may not be decodable on its own",
			camIndex + 1);

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "cam_index", camIndex);
	const std::string tapName =
		"MultiReplay tap " + std::to_string(camIndex + 1);
	obs_output_t *ours = obs_output_create(
		aencRef ? kTapOutputIdAV : kTapOutputIdV, tapName.c_str(),
		settings, nullptr);
	obs_data_release(settings);

	if (!ours) {
		obs_encoder_release(vencRef);
		if (aencRef)
			obs_encoder_release(aencRef);
		obs_output_release(bo);
		obs_log(LOG_ERROR, "[tap] cam%d: could not create tap output",
			camIndex + 1);
		return false;
	}

	obs_output_set_video_encoder(ours, vencRef);
	if (aencRef)
		obs_output_set_audio_encoder(ours, aencRef, 0);

	ch.armWallNs.store((int64_t)os_gettime_ns());

	if (!obs_output_start(ours)) {
		const char *err = obs_output_get_last_error(ours);
		obs_log(LOG_ERROR, "[tap] cam%d: obs_output_start failed: %s",
			camIndex + 1, err ? err : "(no error reported)");
		// M8: tapCreate() already pointed the channel at this output.
		// Releasing it without clearing that leaves a live object holding
		// a pointer to a destroyed one — nothing dereferences it today,
		// which is exactly what makes it worth removing now.
		if (ch.tapOutput == ours)
			ch.tapOutput = nullptr;
		obs_output_release(ours);
		obs_encoder_release(vencRef);
		if (aencRef)
			obs_encoder_release(aencRef);
		obs_output_release(bo);
		return false;
	}

	// Between the lookup above and the start just now, Branch Output may have
	// torn its pipeline down - it restarts itself whenever its settings
	// change, and that destroys the obs_view backing this encoder. Staying
	// attached to that is what crashes libobs' gpu_encode_thread, so if the
	// ground moved under us, let go at once and let the retry loop pick it up
	// when it has settled.
	if (!obs_output_active(bo) || !obs_encoder_active(venc)) {
		obs_log(LOG_WARNING,
			"[tap] cam%d: Branch Output restarted while attaching - "
			"backing off",
			camIndex + 1);
		obs_output_stop(ours);
		if (ch.tapOutput == ours)
			ch.tapOutput = nullptr; // M8, same reason as above
		obs_output_release(ours);
		obs_encoder_release(vencRef);
		if (aencRef)
			obs_encoder_release(aencRef);
		obs_output_release(bo);
		return false;
	}

	// Safety net for every teardown path we do not drive ourselves.
	if (signal_handler_t *sh = obs_output_get_signal_handler(bo)) {
		signal_handler_connect(sh, "stopping", onBoOutputStopping, &ch);
		ch.signalConnected = true;
	}

	ch.tapOutput = ours;
	ch.boOutput = bo;
	ch.videoEncoder = vencRef;
	ch.audioEncoder = aencRef;
	ch.outputName = boName;
	ch.attached.store(true);

	obs_log(LOG_INFO,
		"[tap] cam%d ATTACHED to '%s' — encoder %s ('%s', codec %s) @%p, "
		"already active: %s, audio: %s",
		camIndex + 1, boName.c_str(), ch.encoderId.c_str(),
		ch.encoderName.c_str(),
		obs_encoder_get_codec(venc) ? obs_encoder_get_codec(venc) : "?",
		ch.encoderPtr, ch.encoderWasAlreadyActive ? "yes" : "NO",
		aencRef ? "yes" : "none");
	return true;
}

PacketTap::Doomed PacketTap::detachLocked(int camIndex)
{
	Doomed d;
	Channel &ch = channels_[camIndex];
	if (!ch.attached.exchange(false))
		return d;

	// Cheap and safe under the lock: this only unhooks a callback list.
	if (ch.signalConnected && ch.boOutput) {
		if (signal_handler_t *sh =
			    obs_output_get_signal_handler(ch.boOutput))
			signal_handler_disconnect(sh, "stopping",
						  onBoOutputStopping, &ch);
		ch.signalConnected = false;
	}

	// Everything that can block leaves with the caller. The channel is
	// unhooked already, so as far as every reader is concerned this camera
	// is detached NOW, whatever libobs takes to finish letting go.
	//
	// tapOutput is the exception and it must stay: obs_output_stop() calls
	// our tapStop(), which ends data capture on exactly that pointer. Null
	// it here and the stop waits for a capture nobody will ever end — which
	// is precisely how a "fix" for one hang produced a worse one, with the
	// tap unable to attach anything for the rest of the session.
	d.channel = &ch;
	d.camIndex = camIndex;
	d.tapOutput = ch.tapOutput;
	d.boOutput = ch.boOutput;
	d.videoEncoder = ch.videoEncoder;
	d.audioEncoder = ch.audioEncoder;
	d.videoPackets = ch.videoPackets.load();
	d.audioPackets = ch.audioPackets.load();
	ch.boOutput = nullptr;
	ch.videoEncoder = nullptr;
	ch.audioEncoder = nullptr;
	ch.disposing.store(true); // no re-attach until the stop has returned
	return d;
}

void PacketTap::dispose(Doomed d)
{
	if (d.empty())
		return;
	auto state = disposalState();
	state->inFlight.fetch_add(1);
	// Detached on purpose. obs_output_stop() here can block indefinitely
	// when Branch Output is tearing the shared encoder down at the same
	// moment (measured: 20 s and still going), and there is nobody it would
	// be acceptable to make wait for that — not the arm thread, which
	// unload() joins, and certainly not the UI thread inside STOP.
	std::thread([this, d, state]() {
		if (d.tapOutput) {
			obs_output_stop(d.tapOutput);
			obs_output_release(d.tapOutput);
		}
		if (d.videoEncoder)
			obs_encoder_release(d.videoEncoder);
		if (d.audioEncoder)
			obs_encoder_release(d.audioEncoder);
		if (d.boOutput)
			obs_output_release(d.boOutput);
		// PAST THIS POINT THE MODULE MAY BE GONE (A5). Everything above
		// is libobs, which is still loaded; everything below is ours.
		if (!state->moduleGone.load()) {
			if (d.channel) {
				// Only now is the output really nobody's: the stop
				// has returned, so tapStop() cannot be called on it
				// again.
				std::lock_guard<std::mutex> lock(mutex_);
				if (d.channel->tapOutput == d.tapOutput)
					d.channel->tapOutput = nullptr;
				d.channel->disposing.store(false);
			}
			obs_log(LOG_INFO,
				"[tap] cam%d detached (%" PRIu64 " video, %" PRIu64
				" audio packets)",
				d.camIndex + 1, d.videoPackets, d.audioPackets);
		}
		state->inFlight.fetch_sub(1);
	}).detach();
}

void PacketTap::detachAll()
{
	// Called from STOP, i.e. from the UI thread. It must come back promptly
	// whatever state Branch Output is in, so the blocking half happens after
	// the lock is dropped, on threads of its own (see Doomed).
	std::vector<Doomed> doomed;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (int i = 0; i < kMaxTapChannels; i++) {
			wanted_[i] = false;
			doomed.push_back(detachLocked(i));
		}
	}
	for (auto &d : doomed)
		dispose(std::move(d));
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

bool PacketTap::anyAttached() const
{
	for (const auto &ch : channels_)
		if (ch.attached.load())
			return true;
	return false;
}

int PacketTap::attachedCount() const
{
	int n = 0;
	for (const auto &ch : channels_)
		if (ch.attached.load())
			n++;
	return n;
}

bool PacketTap::resolveRange(int camIndex, int64_t inNs, int64_t outNs,
			     std::vector<LivePacket> &packetsOut,
			     int64_t &presentInNs, int64_t &presentOutNs) const
{
	if (camIndex < 0 || camIndex >= kMaxTapChannels)
		return false;

	const Channel &ch = channels_[camIndex];

	// WHERE the range is, under the lock. The COPY, in pieces (M3).
	//
	// This used to hold ringMutex for the whole copy — tens of megabytes for
	// a long clip, every byte of it a memcpy plus an allocation, while the
	// encoder thread has to take that same lock for EVERY packet it
	// produces. A slow fetch was therefore a latency spike on the live path,
	// i.e. on the one thing the ring exists to protect.
	//
	// The indices are made stable by evictedPackets(), which only ever
	// grows: a packet's global index is its position plus everything thrown
	// away before it. Between chunks the encoder gets the lock; if eviction
	// somehow reached a packet we had not copied yet we REFUSE rather than
	// hand back a clip with a hole in it — the same rule as everywhere else
	// on this path, and in practice unreachable, since eviction takes the
	// oldest first and so does this loop.
	uint64_t firstGlobal = 0, lastGlobal = 0;
	{
		std::lock_guard<std::mutex> lock(ch.ringMutex);
		ResolvedRange r;
		if (!ch.ring.resolveRange(inNs, outNs, r))
			return false;
		const uint64_t base = ch.ring.evictedPackets();
		firstGlobal = base + r.decodeStart;
		lastGlobal = base + r.last;
		presentInNs = r.presentInNs;
		presentOutNs = r.presentOutNs;
	}

	packetsOut.clear();
	// Outside the lock on purpose: one allocation for the whole clip, and
	// the encoder thread does not wait for it.
	packetsOut.reserve((size_t)(lastGlobal - firstGlobal + 1));

	// Small enough that the encoder never waits long, large enough that the
	// lock is not taken once per packet.
	constexpr uint64_t kChunk = 32;
	for (uint64_t g = firstGlobal; g <= lastGlobal;) {
		std::lock_guard<std::mutex> lock(ch.ringMutex);
		const uint64_t evicted = ch.ring.evictedPackets();
		const uint64_t stop = std::min(g + kChunk - 1, lastGlobal);
		for (; g <= stop; g++) {
			if (g < evicted) {
				packetsOut.clear();
				return false;
			}
			const size_t local = (size_t)(g - evicted);
			if (local >= ch.ring.size()) {
				packetsOut.clear();
				return false;
			}
			packetsOut.push_back(ch.ring.at(local));
		}
	}
	return true;
}

StreamConfig PacketTap::streamConfig(int camIndex) const
{
	if (camIndex < 0 || camIndex >= kMaxTapChannels)
		return {};
	const Channel &ch = channels_[camIndex];
	std::lock_guard<std::mutex> lock(ch.ringMutex);
	return ch.config;
}

std::vector<AnchorSample> PacketTap::videoSamples(int camIndex) const
{
	std::vector<AnchorSample> out;
	if (camIndex < 0 || camIndex >= kMaxTapChannels)
		return out;

	const Channel &ch = channels_[camIndex];
	std::lock_guard<std::mutex> lock(ch.ringMutex);
	out.reserve(ch.ring.size() / 2);
	for (size_t i = 0; i < ch.ring.size(); i++) {
		const LivePacket &p = ch.ring.at(i);
		if (p.kind != PacketKind::Video)
			continue;
		out.push_back({p.masterNs, (uint32_t)p.data.size()});
	}
	return out;
}

int64_t PacketTap::newestNs(int camIndex) const
{
	if (camIndex < 0 || camIndex >= kMaxTapChannels)
		return 0;
	const Channel &ch = channels_[camIndex];
	std::lock_guard<std::mutex> lock(ch.ringMutex);
	return ch.ring.newestNs();
}

int64_t PacketTap::oldestReplayableNs(int camIndex) const
{
	if (camIndex < 0 || camIndex >= kMaxTapChannels)
		return 0;
	const Channel &ch = channels_[camIndex];
	std::lock_guard<std::mutex> lock(ch.ringMutex);
	int64_t ns = 0;
	return ch.ring.oldestKeyframeNs(ns) ? ns : 0;
}

TapStats PacketTap::stats(int camIndex) const
{
	TapStats s;
	if (camIndex < 0 || camIndex >= kMaxTapChannels)
		return s;

	std::lock_guard<std::mutex> lock(mutex_);
	const Channel &ch = channels_[camIndex];
	s.attached = ch.attached.load();
	s.camIndex = camIndex;
	s.outputName = ch.outputName;
	s.encoderPtr = ch.encoderPtr;
	s.encoderId = ch.encoderId;
	s.encoderName = ch.encoderName;
	s.encoderWasAlreadyActive = ch.encoderWasAlreadyActive;
	s.videoPackets = ch.videoPackets.load();
	s.audioPackets = ch.audioPackets.load();
	s.videoBytes = ch.videoBytes.load();
	s.audioBytes = ch.audioBytes.load();
	s.keyframes = ch.keyframes.load();
	s.firstPacketWaitMs = ch.firstPacketWaitMs.load();
	s.lastAgeUsec = ch.lastAgeUsec.load();
	s.maxAgeUsec = ch.maxAgeUsec.load();
	const uint64_t n = ch.ageSamples.load();
	s.avgAgeUsec = n ? (ch.ageSumUsec.load() / (int64_t)n) : 0;
	s.lastSysDtsNs = ch.lastSysDtsNs.load();
	s.malformedPackets = ch.malformed.load();
	s.discontinuities = ch.discontinuities.load();

	{
		std::lock_guard<std::mutex> rlock(ch.ringMutex);
		s.ringPackets = ch.ring.size();
		s.ringBytes = ch.ring.bytes();
		s.ringSpanNs = ch.ring.spanNs();
		s.ringOldestNs = ch.ring.oldestNs();
		s.ringNewestNs = ch.ring.newestNs();
		s.evictedPackets = ch.ring.evictedPackets();
	}
	return s;
}

int64_t PacketTap::crossAngleSkewNs() const
{
	int64_t lo = INT64_MAX, hi = INT64_MIN;
	for (const auto &ch : channels_) {
		if (!ch.attached.load())
			continue;
		const int64_t t = ch.lastSysDtsNs.load();
		if (t <= 0)
			continue;
		lo = std::min(lo, t);
		hi = std::max(hi, t);
	}
	if (lo == INT64_MAX || hi == INT64_MIN)
		return 0;
	return hi - lo;
}

std::string PacketTap::report() const
{
	std::ostringstream os;
	os << "[tap] ---- M0 packet tap report ----\n";
	os << "[tap] attached channels: " << attachedCount() << "\n";

	for (int i = 0; i < kMaxTapChannels; i++) {
		const TapStats s = stats(i);
		if (!s.attached && s.videoPackets == 0)
			continue;
		os << "[tap] cam" << (i + 1) << " '" << s.outputName << "'"
		   << " enc=" << s.encoderId << "@" << s.encoderPtr
		   << " active-before-attach=" << (s.encoderWasAlreadyActive ? "yes" : "NO")
		   << " vpkts=" << s.videoPackets << " keyf=" << s.keyframes
		   << " apkts=" << s.audioPackets
		   << " vMB=" << (s.videoBytes / (1024 * 1024))
		   << " firstPacketWait=" << s.firstPacketWaitMs << "ms"
		   << " age(last/avg/max)=" << (s.lastAgeUsec / 1000) << "/"
		   << (s.avgAgeUsec / 1000) << "/" << (s.maxAgeUsec / 1000) << "ms"
		   << " ring=" << s.ringPackets << "pk/"
		   << (s.ringBytes / (1024 * 1024)) << "MB/"
		   << (s.ringSpanNs / 1000000) << "ms"
		   << " evicted=" << s.evictedPackets
		   << " malformed=" << s.malformedPackets
		   << " disc=" << s.discontinuities << "\n";
	}

	os << "[tap] cross-angle skew: " << (crossAngleSkewNs() / 1000000)
	   << " ms\n";
	os << "[tap] OBS: fps=" << obs_get_active_fps()
	   << " avgFrameTime=" << (obs_get_average_frame_time_ns() / 1000000.0)
	   << "ms lagged=" << obs_get_lagged_frames()
	   << " total=" << obs_get_total_frames() << "\n";
	os << "[tap] --------------------------------";
	return os.str();
}

} // namespace multireplay
