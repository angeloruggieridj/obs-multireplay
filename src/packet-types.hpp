/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

The value type that crosses the boundary between the encoder thread and the
replay engine. Deliberately free of any OBS type so the ring and the timeline
maths can be unit-tested on any platform with no libobs at all.

Packet bytes handed to us by libobs are only valid for the duration of the
callback, so a LivePacket owns its bytes.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace multireplay {

enum class PacketKind { Video, Audio };

// Everything a decoder needs to make sense of one camera's packets, captured
// once when the tap attaches.
//
// The extradata matters more than it looks: OBS encoders keep the parameter
// sets (SPS/PPS for H.264) out of band by default, so a packet stream on its
// own is not decodable. Capturing them at attach time keeps the ring
// self-describing even after its head has been evicted.
struct StreamConfig {
	std::string videoCodec; // "h264" | "hevc" | "av1"
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> videoExtradata;

	std::string audioCodec; // "aac" | "opus" | ...
	uint32_t sampleRate = 0;
	std::vector<uint8_t> audioExtradata;

	bool videoUsable() const
	{
		return !videoCodec.empty() && width > 0 && height > 0;
	}
};

// One encoded packet, already normalized onto the master timeline.
struct LivePacket {
	PacketKind kind = PacketKind::Video;
	bool keyframe = false;

	// Presentation and decode time on the SHARED master clock (ns). Every
	// camera's packets land on this same clock, which is what makes one
	// marker mean the same instant on every angle.
	int64_t masterNs = 0;
	int64_t dtsNs = 0;

	// Bumped whenever the stream is discontinuous (encoder restart, clock
	// jump). A range that would straddle two generations is not resolvable.
	uint32_t generation = 0;

	std::vector<uint8_t> data;

	size_t bytes() const { return data.size(); }
};

} // namespace multireplay
