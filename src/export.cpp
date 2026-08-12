/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "export.hpp"

#include <obs-module.h>
#include "plugin-support.h"

#include "event-store.hpp"
#include "replay-core.hpp"
#include "segment-index.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <filesystem>
#include <system_error>

namespace multireplay {

namespace fs = std::filesystem;

ExportManager &ExportManager::instance()
{
	static ExportManager mgr;
	return mgr;
}

bool ExportManager::exportEvent(int eventId, int angle1Based,
				const std::string &customFolder,
				std::string &errorOut)
{
	ReplayEvent ev;
	if (!EventStore::instance().get(eventId, ev) || ev.tOutNs < 0) {
		errorOut = "event not found or still open";
		return false;
	}

	// angle1Based >= 1: export that single angle.
	// angle1Based == 0: export EVERY enabled angle (one file each) —
	// the reference controller exports all checked angles, not just the first.
	std::vector<int> angles;
	if (angle1Based >= 1) {
		angles.push_back(angle1Based - 1);
	} else {
		for (int i = 0; i < kEventAngles; i++)
			if (ev.angles[i].enabled)
				angles.push_back(i);
		if (angles.empty())
			angles.push_back(0);
	}

	std::string folder = customFolder;
	if (folder.empty()) {
		fs::path p(ReplayCore::instance().recordingFolder());
		p /= "export";
		folder = p.string();
	}
	std::error_code ec;
	fs::create_directories(folder, ec);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (int angle : angles) {
			fs::path out(folder);
			out /= "event" + std::to_string(ev.id) + "_list" +
			       std::to_string(ev.list) + "_cam" +
			       std::to_string(angle + 1) + ".mp4";

			Job job;
			job.eventId = ev.id;
			job.angle = angle;
			job.tInNs = ev.tInNs;
			job.tOutNs = ev.tOutNs;
			job.outPath = out.string();
			jobs_.push_back(std::move(job));
		}

		if (!workerRunning_) {
			if (thread_.joinable())
				thread_.join();
			workerRunning_ = true;
			thread_ = std::thread([this]() { worker(); });
		}
	}
	return true;
}

bool ExportManager::exportLastEvent(const std::string &customFolder,
				    std::string &errorOut)
{
	int id = EventStore::instance().lastEventId();
	if (id == 0) {
		errorOut = "no completed events yet";
		return false;
	}
	return exportEvent(id, 0, customFolder, errorOut);
}

void ExportManager::worker()
{
	while (true) {
		Job local;
		bool found = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (auto &j : jobs_) {
				if (j.state == "queued") {
					j.state = "running";
					local = j; // copy while vector is stable
					found = true;
					break;
				}
			}
			if (!found) {
				workerRunning_ = false;
				return;
			}
		}

		bool ok = runJob(local);

		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &j : jobs_) {
			if (j.eventId == local.eventId &&
			    j.outPath == local.outPath &&
			    j.state == "running") {
				j.state = ok ? "done" : "failed";
				j.detail = local.detail;
				break;
			}
		}
	}
}

// Stream-copy the packets of [tIn, tOut] from the recording into a new MP4.
bool ExportManager::runJob(Job &job)
{
	// Which recorded file holds those instants, and where inside it. This is
	// the same anchored index the replay reads from, so an export and a replay
	// of the same event necessarily cut at the same frame.
	//
	// M4 limitation: a clip must live inside one 20-minute segment
	// (events spanning a split are rare; documented).
	std::string path;
	int64_t inOffsetNs = 0, outOffsetNs = 0;
	{
		// Resolve both ends; they must land in the same file.
		std::string pathOut;
		auto &index = SegmentIndex::instance();
		if (!index.resolve(job.angle, job.tInNs, path, inOffsetNs) ||
		    !index.resolve(job.angle, job.tOutNs, pathOut,
				   outOffsetNs)) {
			job.detail = "cannot resolve event time on angle "
				     "(no anchored recording covers it)";
			return false;
		}
		if (path != pathOut) {
			job.detail = "event spans a file split (not yet "
				     "supported) — export each half";
			return false;
		}
	}

	AVFormatContext *in = nullptr;
	if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0) {
		job.detail = "cannot open " + path;
		return false;
	}
	if (avformat_find_stream_info(in, nullptr) < 0) {
		avformat_close_input(&in);
		job.detail = "no stream info";
		return false;
	}

	AVFormatContext *out = nullptr;
	avformat_alloc_output_context2(&out, nullptr, "mp4",
				       job.outPath.c_str());
	if (!out) {
		avformat_close_input(&in);
		job.detail = "cannot create output";
		return false;
	}

	std::vector<int> streamMap((size_t)in->nb_streams, -1);
	for (unsigned i = 0; i < in->nb_streams; i++) {
		AVCodecParameters *par = in->streams[i]->codecpar;
		if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
		    par->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		AVStream *os = avformat_new_stream(out, nullptr);
		avcodec_parameters_copy(os->codecpar, par);
		os->codecpar->codec_tag = 0;
		os->time_base = in->streams[i]->time_base;
		streamMap[i] = (int)(out->nb_streams - 1);
	}

	bool ok = false;
	do {
		if (!(out->oformat->flags & AVFMT_NOFILE) &&
		    avio_open(&out->pb, job.outPath.c_str(),
			      AVIO_FLAG_WRITE) < 0) {
			job.detail = "cannot open output file";
			break;
		}
		if (avformat_write_header(out, nullptr) < 0) {
			job.detail = "cannot write header";
			break;
		}

		// Seek to the keyframe at/before In on the video stream.
		int vIdx = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1,
					       nullptr, 0);
		AVRational nsTb{1, 1000000000};
		int64_t seekTs = av_rescale_q(job.tInNs >= 0 ? inOffsetNs : 0,
					      nsTb,
					      in->streams[vIdx]->time_base);
		if (in->streams[vIdx]->start_time != AV_NOPTS_VALUE)
			seekTs += in->streams[vIdx]->start_time;
		av_seek_frame(in, vIdx, seekTs, AVSEEK_FLAG_BACKWARD);

		AVPacket *pkt = av_packet_alloc();
		if (!pkt) {
			job.detail = "out of memory";
			break;
		}
		// Frame-accurate start without re-encoding: baseline timestamps at
		// the IN point (not the preceding keyframe). Frames between the
		// keyframe and IN keep NEGATIVE pts; the MP4 muxer writes an edit
		// list that trims them, so playback begins exactly at IN (same trick
		// as `ffmpeg -ss <t> -c copy`). Per-stream baseline in the stream's
		// own time_base.
		std::vector<int64_t> inBase((size_t)in->nb_streams, 0);
		for (unsigned i = 0; i < in->nb_streams; i++) {
			int64_t stt = in->streams[i]->start_time != AV_NOPTS_VALUE
					      ? in->streams[i]->start_time
					      : 0;
			inBase[i] = av_rescale_q(inOffsetNs, nsTb,
						 in->streams[i]->time_base) +
				    stt;
		}
		ok = true;
		while (av_read_frame(in, pkt) >= 0) {
			int sIdx = pkt->stream_index;
			if (streamMap[(size_t)sIdx] < 0) {
				av_packet_unref(pkt);
				continue;
			}
			AVStream *ist = in->streams[sIdx];
			int64_t ptsNs = av_rescale_q(
				pkt->pts - (ist->start_time != AV_NOPTS_VALUE
						    ? ist->start_time
						    : 0),
				ist->time_base, nsTb);
			if (sIdx == vIdx && ptsNs > outOffsetNs) {
				av_packet_unref(pkt);
				break;
			}

			AVStream *ost =
				out->streams[streamMap[(size_t)sIdx]];
			pkt->pts = av_rescale_q(pkt->pts - inBase[(size_t)sIdx],
						ist->time_base, ost->time_base);
			pkt->dts = av_rescale_q(pkt->dts - inBase[(size_t)sIdx],
						ist->time_base, ost->time_base);
			pkt->duration = av_rescale_q(pkt->duration,
						     ist->time_base,
						     ost->time_base);
			pkt->stream_index = streamMap[(size_t)sIdx];
			pkt->pos = -1;
			if (av_interleaved_write_frame(out, pkt) < 0) {
				job.detail = "write failed";
				ok = false;
				break;
			}
		}
		av_packet_free(&pkt);
		av_write_trailer(out);
	} while (false);

	if (out && !(out->oformat->flags & AVFMT_NOFILE) && out->pb)
		avio_closep(&out->pb);
	avformat_free_context(out);
	avformat_close_input(&in);

	if (ok) {
		obs_log(LOG_INFO, "export: wrote %s", job.outPath.c_str());
	} else {
		obs_log(LOG_WARNING, "export failed: %s", job.detail.c_str());
		std::error_code ec;
		std::filesystem::remove(job.outPath, ec);
	}
	return ok;
}

std::string ExportManager::statusJson() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &j : jobs_) {
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "eventId", j.eventId);
		obs_data_set_int(item, "angle", j.angle + 1);
		obs_data_set_string(item, "outPath", j.outPath.c_str());
		obs_data_set_string(item, "state", j.state.c_str());
		obs_data_set_string(item, "detail", j.detail.c_str());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "jobs", arr);
	obs_data_array_release(arr);
	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

} // namespace multireplay
