/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

PacketTap (M0 spike) — receives Branch Output's ENCODED packets live, without
forking Branch Output and without creating a single extra encoder.

How it works
------------
A libobs encoder keeps a LIST of packet callbacks (obs-encoder.c:723);
obs_encoder_start() appends a consumer to an already-running encoder without
restarting it (obs-encoder.c:709-736), and it is obs-output.c:2360-2369 that
calls it when an output begins data capture. That is exactly how OBS shares one
encoder between streaming and recording — and how Branch Output itself already
feeds one encoder to its recording output and up to 8 streaming outputs.

So we register our own obs_output type (OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED),
assign it the encoder Branch Output is ALREADY running, and start it. We become
one more consumer on that encoder's callback list.

Two libobs details make this safe rather than lucky:
  - pair_encoders() early-outs on `if (video->active) return;` (obs-output.c:2542),
    so attaching to an active encoder cannot disturb Branch Output's own
    encoder pairing state;
  - the packets carry `sys_dts_usec` (obs-encoder.h:120) — the system DTS in
    microseconds, on the SAME clock for every encoder. That is the shared
    reference that makes the master timeline MEASURED instead of estimated,
    and it is what finally kills ReplayCore::frameLagNs().

Discovery
---------
Branch Output names its recording output after the filter's source name
(plugin-main.cpp: `name(obs_source_get_name(source))`, then
`obs_output_create(outputId, qUtf8Printable(name), ...)`). We own those filter
names (branch-output-control.hpp: kFilterNamePrefix), so the output name is
known exactly — no guessing. Its streaming outputs use a "name (index)" suffix,
so a plain name lookup cannot collide with them.

Lifecycle hazard (the one real risk)
------------------------------------
Branch Output releases its encoder in releaseInfrastructureIfIdle() once all of
ITS outputs are idle. If our output were still running we would keep that
encoder — and its video pipeline — alive underneath a filter that believes it
has torn everything down. So we detach BEFORE the filter is disabled (the
normal path, which we control), and we also listen to the Branch Output
output's own "stop"/"stopping" signals for every path we do not control (user
disables the filter, source removed, scene collection change, OBS shutdown).

M0 is fail-SOFT: if the tap cannot attach, recording still proceeds and the
plugin behaves exactly as before. It becomes fail-CLOSED in M1, once the packet
ring is the authoritative live edge.
*/

#pragma once

#include <obs.h>

#include "master-timeline.hpp"
#include "packet-ring.hpp"
#include "segment-anchor.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace multireplay {

constexpr int kMaxTapChannels = 8;

// Immutable snapshot of one tapped angle, for logging and the M0 verdict.
struct TapStats {
	bool attached = false;
	int camIndex = 0; // 0-based
	std::string outputName;
	// Identity proof that no new encoder was created: this is Branch
	// Output's own encoder object, shared, not a copy.
	const void *encoderPtr = nullptr;
	std::string encoderId;
	std::string encoderName;
	bool encoderWasAlreadyActive = false;

	uint64_t videoPackets = 0;
	uint64_t audioPackets = 0;
	uint64_t videoBytes = 0;
	uint64_t audioBytes = 0;
	uint64_t keyframes = 0;

	// Arm → first packet received (ms). The wait for the first keyframe.
	int64_t firstPacketWaitMs = -1;
	// Packet age at arrival: os_gettime_ns()/1000 - packet->sys_dts_usec.
	// This is the live-edge latency that has to beat the ~1 s fragment flush.
	int64_t lastAgeUsec = 0;
	int64_t maxAgeUsec = 0;
	int64_t avgAgeUsec = 0;
	// Last normalized master timestamp (ns), from sys_dts_usec.
	int64_t lastSysDtsNs = 0;

	// --- live ring (M1) ---
	size_t ringPackets = 0;
	size_t ringBytes = 0;
	int64_t ringSpanNs = 0;
	int64_t ringOldestNs = 0;
	int64_t ringNewestNs = 0;
	uint64_t evictedPackets = 0;
	// Packets libobs handed us that could not be placed on the master clock
	// (zero timebase, missing sys_dts). Should be 0; anything else is a bug
	// or a broken encoder, and is worth surfacing rather than swallowing.
	uint64_t malformedPackets = 0;
	// Encoder restarts / clock jumps observed on this camera.
	uint64_t discontinuities = 0;
};

// How much live history to keep per camera. Encoded packets are cheap: at
// 12 Mbps, 90 s costs ~135 MB per camera, so eight cameras still fit in RAM
// while covering far more than any replay operator reaches back for.
struct RingBudget {
	int seconds = 90;
	int kbpsPerCamera = 12500; // video + audio, used to size the byte cap
};

class PacketTap {
public:
	static PacketTap &instance();

	// Registers the "multireplay_tap" output type. Call from obs_module_load.
	void load();
	// Detaches everything and stops the arm thread. Call from obs_module_unload.
	void unload();

	// Arm the tap for the given cameras (index 0-based, true = tap it).
	// Branch Output starts its output asynchronously after the filter is
	// enabled, so attaching is retried on a background thread until each BO
	// output goes active or the attempt budget runs out.
	void armAsync(const std::array<bool, kMaxTapChannels> &wanted,
		      const RingBudget &budget = {});

	// --- live ring access (M1) ---
	// Resolve a master-timeline range against one camera's live history.
	// Strict: false when the range is not fully and continuously held (see
	// PacketRing::resolveRange). Copies out the packets so the caller never
	// holds the ring lock while decoding.
	bool resolveRange(int camIndex, int64_t inNs, int64_t outNs,
			  std::vector<LivePacket> &packetsOut,
			  int64_t &presentInNs, int64_t &presentOutNs) const;
	// What a decoder needs for this camera (codec, size, parameter sets).
	StreamConfig streamConfig(int camIndex) const;
	// (masterNs, size) of the video packets currently held, oldest first.
	// This is the evidence a Branch Output file is anchored against — see
	// segment-anchor.hpp.
	std::vector<AnchorSample> videoSamples(int camIndex) const;
	// Newest instant captured on a camera, 0 if none. This is the live edge.
	int64_t newestNs(int camIndex) const;
	// Earliest instant that can actually be replayed, 0 if none.
	int64_t oldestReplayableNs(int camIndex) const;

	// Detach every attached channel. MUST run BEFORE the Branch Output
	// filters are disabled (see the lifecycle note above).
	void detachAll();

	bool anyAttached() const;
	int attachedCount() const;

	// Per-channel snapshot, safe from any thread.
	TapStats stats(int camIndex) const;
	// Human-readable multi-line block for the OBS log (the M0 evidence).
	std::string report() const;
	// Cross-angle skew (ns) between the newest packet of each attached
	// angle. This is the number that replaces the frameLagNs() guesswork.
	int64_t crossAngleSkewNs() const;

private:
	PacketTap() = default;
	~PacketTap();
	PacketTap(const PacketTap &) = delete;
	PacketTap &operator=(const PacketTap &) = delete;

	// One tapped angle. Counters are atomic because encoded_packet() runs on
	// the encoder thread and must never block it.
	struct Channel {
		std::atomic<bool> attached{false};
		// Set by the Branch Output "stopping" signal (which fires on an OBS
		// thread we do not own). The arm thread does the actual detach, so
		// the signal never re-enters our mutex and cannot deadlock.
		std::atomic<bool> detachRequested{false};
		bool signalConnected = false;
		int camIndex = 0;
		std::string outputName;

		obs_output_t *tapOutput = nullptr; // ours (owned)
		obs_output_t *boOutput = nullptr;  // Branch Output's (ref held)
		obs_encoder_t *videoEncoder = nullptr; // ref held
		obs_encoder_t *audioEncoder = nullptr; // ref held

		const void *encoderPtr = nullptr;
		std::string encoderId;
		std::string encoderName;
		bool encoderWasAlreadyActive = false;

		std::atomic<uint64_t> videoPackets{0};
		std::atomic<uint64_t> audioPackets{0};
		std::atomic<uint64_t> videoBytes{0};
		std::atomic<uint64_t> audioBytes{0};
		std::atomic<uint64_t> keyframes{0};

		std::atomic<int64_t> armWallNs{0};
		std::atomic<int64_t> firstPacketWaitMs{-1};
		std::atomic<int64_t> lastAgeUsec{0};
		std::atomic<int64_t> maxAgeUsec{0};
		std::atomic<int64_t> ageSumUsec{0};
		std::atomic<uint64_t> ageSamples{0};
		std::atomic<int64_t> lastSysDtsNs{0};

		// --- master timeline + live ring (M1) ---
		// One MasterTimeline per track: video and audio come from two
		// different encoders, each numbering from its own zero, and each
		// delivering on its own thread — so each is touched by exactly
		// one thread and needs no lock of its own.
		MasterTimeline videoTl;
		MasterTimeline audioTl;
		std::atomic<uint64_t> malformed{0};
		std::atomic<uint64_t> discontinuities{0};
		// Continuity is a property of the CAMERA, not of one track: an
		// audio break wrecks A/V pairing just as a video break does, so
		// both bump this and every packet carries it.
		std::atomic<uint32_t> generation{0};

		// The ring is shared with readers, so it is the one thing here
		// that takes a lock. Critical sections stay short: the encoder
		// thread must never wait.
		mutable std::mutex ringMutex;
		PacketRing ring;
		StreamConfig config; // guarded by ringMutex
	};

	// --- obs_output_info callbacks (C ABI) ---
	static const char *tapGetName(void *typeData);
	static void *tapCreate(obs_data_t *settings, obs_output_t *output);
	static void tapDestroy(void *data);
	static bool tapStart(void *data);
	static void tapStop(void *data, uint64_t ts);
	static void tapEncodedPacket(void *data, struct encoder_packet *packet);

	// Branch Output's output is going away — detach before it tears down.
	static void onBoOutputStopping(void *param, calldata_t *cd);

	void armLoop();
	bool attachLocked(int camIndex); // mutex_ held
	void detachLocked(int camIndex); // mutex_ held

	mutable std::mutex mutex_;
	std::array<Channel, kMaxTapChannels> channels_{};
	std::array<bool, kMaxTapChannels> wanted_{};

	std::thread armThread_;
	std::atomic<bool> armRunning_{false};
	std::condition_variable armWake_;
	bool registered_ = false;
};

} // namespace multireplay
