/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See segment-reader.hpp.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "segment-reader.hpp"

#include "plugin-support.h"
#include "segment-index.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>

namespace multireplay {
namespace segment_reader {

namespace {

constexpr AVRational kNs = {1, 1'000'000'000};

const char *codecName(AVCodecID id)
{
	switch (id) {
	case AV_CODEC_ID_H264:
		return "h264";
	case AV_CODEC_ID_HEVC:
		return "hevc";
	case AV_CODEC_ID_AV1:
		return "av1";
	case AV_CODEC_ID_AAC:
		return "aac";
	case AV_CODEC_ID_OPUS:
		return "opus";
	case AV_CODEC_ID_FLAC:
		return "flac";
	case AV_CODEC_ID_ALAC:
		return "alac";
	case AV_CODEC_ID_PCM_S16LE:
		return "pcm_s16le";
	default:
		return "";
	}
}

// Read one file's contribution to the range. `fromNs`/`toNs` are master times;
// `anchorNs` is where this file's time zero sits on the master clock.
bool readOne(const std::string &path, int64_t anchorNs, int64_t fromNs,
	     int64_t toNs, std::vector<LivePacket> &out, StreamConfig &cfg,
	     bool wantConfig, std::string &errorOut)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
		errorOut = "could not open " + path;
		return false;
	}
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		errorOut = "could not read the streams of " + path;
		return false;
	}

	int vs = -1, as = -1;
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		const AVCodecParameters *par = fmt->streams[i]->codecpar;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO && vs < 0)
			vs = (int)i;
		else if (par->codec_type == AVMEDIA_TYPE_AUDIO && as < 0)
			as = (int)i;
	}
	if (vs < 0) {
		avformat_close_input(&fmt);
		errorOut = path + " has no video";
		return false;
	}

	if (wantConfig) {
		// From the file, not the encoder: MP4 rewrites H.264 parameter
		// sets into avcC, and a decoder fed the encoder's Annex B copy
		// while reading file packets would be subtly wrong.
		const AVCodecParameters *vp = fmt->streams[vs]->codecpar;
		cfg.videoCodec = codecName(vp->codec_id);
		cfg.width = (uint32_t)vp->width;
		cfg.height = (uint32_t)vp->height;
		if (vp->extradata && vp->extradata_size > 0)
			cfg.videoExtradata.assign(
				vp->extradata, vp->extradata + vp->extradata_size);
		if (as >= 0) {
			const AVCodecParameters *ap = fmt->streams[as]->codecpar;
			cfg.audioCodec = codecName(ap->codec_id);
			cfg.sampleRate = (uint32_t)ap->sample_rate;
			if (ap->extradata && ap->extradata_size > 0)
				cfg.audioExtradata.assign(
					ap->extradata,
					ap->extradata + ap->extradata_size);
		}
	}

	// Start from the keyframe at or before IN: the frames between it and the
	// marked frame are decoded and dropped downstream, which is what keeps
	// playback starting exactly on the marker.
	const int64_t seekTs = av_rescale_q(std::max<int64_t>(0, fromNs - anchorNs),
					    kNs, fmt->streams[vs]->time_base);
	av_seek_frame(fmt, vs, seekTs, AVSEEK_FLAG_BACKWARD);

	AVPacket *pkt = av_packet_alloc();
	bool ok = pkt != nullptr;
	while (ok && av_read_frame(fmt, pkt) >= 0) {
		const int idx = pkt->stream_index;
		if ((idx != vs && idx != as) || pkt->size <= 0) {
			av_packet_unref(pkt);
			continue;
		}

		const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
		if (ts == AV_NOPTS_VALUE) {
			av_packet_unref(pkt);
			continue;
		}
		const int64_t masterNs =
			anchorNs +
			av_rescale_q(ts, fmt->streams[idx]->time_base, kNs);

		if (masterNs > toNs) {
			// Video is in order, so once past the OUT we are done;
			// trailing audio would only be discarded anyway.
			if (idx == vs) {
				av_packet_unref(pkt);
				break;
			}
			av_packet_unref(pkt);
			continue;
		}

		LivePacket lp;
		lp.kind = idx == vs ? PacketKind::Video : PacketKind::Audio;
		lp.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
		lp.masterNs = masterNs;
		lp.dtsNs = pkt->dts != AV_NOPTS_VALUE
				   ? anchorNs + av_rescale_q(
							pkt->dts,
							fmt->streams[idx]->time_base,
							kNs)
				   : masterNs;
		lp.generation = 0;
		lp.data.assign(pkt->data, pkt->data + pkt->size);
		out.push_back(std::move(lp));

		av_packet_unref(pkt);
	}

	if (pkt)
		av_packet_free(&pkt);
	avformat_close_input(&fmt);
	return ok;
}

} // namespace

bool readRange(int camIndex, int64_t inNs, int64_t outNs,
	       std::vector<LivePacket> &packetsOut, StreamConfig &configOut,
	       int64_t &presentInNs, int64_t &presentOutNs, std::string &errorOut)
{
	if (outNs <= inNs) {
		errorOut = "the range is empty";
		return false;
	}

	const auto segs = SegmentIndex::instance().segments(camIndex);
	if (segs.empty()) {
		errorOut = "no anchored recording file for this camera";
		return false;
	}

	packetsOut.clear();
	bool haveConfig = false;

	for (const auto &s : segs) {
		if (!s.anchored)
			continue;
		// Does this file overlap the requested range at all?
		const int64_t segEnd = s.endMasterNs != 0 ? s.endMasterNs : outNs + 1;
		if (segEnd <= inNs || s.anchorMasterNs > outNs)
			continue;

		if (!readOne(s.path, s.anchorMasterNs, inNs, outNs, packetsOut,
			     configOut, !haveConfig, errorOut))
			return false;
		haveConfig = true;
	}

	if (!haveConfig || packetsOut.empty()) {
		errorOut = "the anchored files do not cover that range";
		return false;
	}

	// Keep the mixed-kind sequence in presentation order across a file
	// boundary; a stable sort preserves the interleave within each file.
	std::stable_sort(packetsOut.begin(), packetsOut.end(),
			 [](const LivePacket &a, const LivePacket &b) {
				 return a.dtsNs < b.dtsNs;
			 });

	bool haveIn = false;
	for (const auto &p : packetsOut) {
		if (p.kind != PacketKind::Video)
			continue;
		if (p.masterNs >= inNs && (!haveIn || p.masterNs < presentInNs)) {
			presentInNs = p.masterNs;
			haveIn = true;
		}
		if (p.masterNs <= outNs &&
		    (!haveIn || p.masterNs > presentOutNs || presentOutNs == 0))
			presentOutNs = p.masterNs;
	}
	if (!haveIn) {
		errorOut = "no video frame at or after the requested in-point";
		return false;
	}
	return true;
}

} // namespace segment_reader
} // namespace multireplay
