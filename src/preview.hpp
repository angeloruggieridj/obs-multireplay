/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <util/threading.h>

namespace multireplay {

// Multiview preview slots: 0..3 = cameras, 4 = Replay A, 5 = Replay B.
constexpr int kPreviewSlots = 6;
constexpr int kPreviewSlotA = 4;
constexpr int kPreviewSlotB = 5;

// Captures low-res snapshots of the camera sources and the Replay A/B
// sources on the OBS graphics thread, JPEG-encodes them (FFmpeg mjpeg)
// and exposes them to the web server as an MJPEG stream / JPEG snapshot.
class PreviewManager {
public:
	static PreviewManager &instance();

	void start();
	void stop();
	bool running() const { return running_; }

	// Latest JPEG for a slot (empty if none yet). `seq` identifies the
	// frame so MJPEG streams can wait for the next one.
	std::shared_ptr<std::vector<uint8_t>> latest(int slot,
						     uint64_t &seqOut) const;

	// Block until a frame newer than `lastSeq` is available (or timeout).
	std::shared_ptr<std::vector<uint8_t>>
	waitNext(int slot, uint64_t lastSeq, uint64_t &seqOut,
		 int timeoutMs) const;

	// Diagnostics for /api/preview/debug: per-slot counters that tell
	// whether the pipeline fails at capture, render or encode stage.
	std::string debugJson() const;

	// Preview cadence (frames per second served to browsers).
	static constexpr int kFps = 8;
	// Preview tile max size.
	static constexpr int kMaxWidth = 480;
	static constexpr int kMaxHeight = 270;

private:
	PreviewManager() = default;
	void threadLoop();
	// Runs on the OBS graphics thread: renders all slots into RGBA
	// buffers (rgba_[slot]).
	void captureAll();
	void encodeSlot(int slot);

	struct RawFrame {
		std::vector<uint8_t> rgba;
		int width = 0;
		int height = 0;
		bool fresh = false;
	};

	std::thread thread_;
	std::atomic<bool> running_{false};

	std::mutex rawMutex_;
	RawFrame raw_[kPreviewSlots];

	// diagnostics (atomic: written from graphics/preview threads)
	std::atomic<uint32_t> diagSourceMissing_[kPreviewSlots] = {};
	std::atomic<uint32_t> diagZeroSize_[kPreviewSlots] = {};
	std::atomic<uint32_t> diagRenderFail_[kPreviewSlots] = {};
	std::atomic<uint32_t> diagRendered_[kPreviewSlots] = {};
	std::atomic<uint32_t> diagEncoded_[kPreviewSlots] = {};
	std::atomic<uint32_t> diagCaptureTimeouts_{0};
	std::atomic<uint32_t> diagCapturePasses_{0};

	mutable std::mutex jpegMutex_;
	mutable std::condition_variable jpegCv_;
	std::shared_ptr<std::vector<uint8_t>> jpeg_[kPreviewSlots];
	uint64_t seq_[kPreviewSlots] = {};

	// FFmpeg encode contexts are created lazily inside encodeSlot().
	struct EncCtx;
	std::unique_ptr<EncCtx> enc_;

	// Persistent event used to synchronise the graphics capture task
	// with the preview thread. Allocated in start(), destroyed in stop()
	// after join so the graphics task can never signal a dead event.
	os_event_t *captureEvent_ = nullptr;
};

} // namespace multireplay
