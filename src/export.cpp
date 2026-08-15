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

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <thread>
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
	if (!EventStore::instance().get(eventId, ev) ||
	    ev.tOutNs == kNoInstant) {
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
			// The speed the operator gave THIS angle. -1 means "the
			// slider decides", and the slider is a live control with
			// no meaning in a file written an hour later, so an
			// export with no override is an export at 100%.
			const double sp = (angle >= 0 && angle < kEventAngles &&
					   ev.angles[angle].speed > 0)
						  ? ev.angles[angle].speed
						  : 1.0;
			const int pct = (int)std::lround(sp * 100.0);

			fs::path out(folder);
			// The speed goes in the name when it is not 100%: two
			// exports of one event at two speeds are two clips, and
			// the second must not silently overwrite the first.
			out /= "event" + std::to_string(ev.id) + "_list" +
			       std::to_string(ev.list) + "_cam" +
			       std::to_string(angle + 1) +
			       (pct == 100 ? "" : "_" + std::to_string(pct) + "pct") +
			       ".mp4";

			Job job;
			job.eventId = ev.id;
			job.angle = angle;
			job.tInNs = ev.tInNs;
			job.tOutNs = ev.tOutNs;
			job.speed = sp;
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

// --- the highlight reel -----------------------------------------------------

namespace {

// Where the music comes from: the OBS audio source the operator configured, if
// it is a media source playing a LOCAL FILE. Anything else (a capture device, a
// browser) has no file to copy and is reported as such rather than silently
// producing a silent reel.
std::string musicFileFor(const std::string &sourceName)
{
	if (sourceName.empty())
		return {};
	obs_source_t *src = obs_get_source_by_name(sourceName.c_str());
	if (!src)
		return {};
	std::string file;
	if (obs_data_t *settings = obs_source_get_settings(src)) {
		const char *local = obs_data_get_string(settings, "local_file");
		if (local && *local)
			file = local;
		obs_data_release(settings);
	}
	obs_source_release(src);
	return file;
}

} // namespace

bool ExportManager::exportSequence(const std::vector<int> &eventIds,
				   bool withMusic,
				   const std::string &customFolder,
				   std::string &errorOut)
{
	// --- what goes in the reel, in the order it will be watched ----------
	// The caller's order is the running order (the dock passes the rows as
	// they are shown), and within an event the enabled angles in angle
	// order — the same expansion PlaybackCoordinator queues, so the file is
	// what the operator would have seen if he had played the selection.
	auto &index = SegmentIndex::instance();
	std::vector<ReelClip> clips;
	for (int id : eventIds) {
		ReplayEvent ev;
		if (!EventStore::instance().get(id, ev) || ev.tOutNs == kNoInstant)
			continue;
		for (int a = 0; a < kEventAngles; a++) {
			if (!ev.angles[a].enabled)
				continue;
			ReelClip c;
			c.eventId = ev.id;
			c.angle = a;
			c.speed = ev.angles[a].speed > 0 ? ev.angles[a].speed : 1.0;
			std::string pOut;
			if (!index.resolve(a, ev.tInNs, c.path, c.inOffsetNs) ||
			    !index.resolve(a, ev.tOutNs, pOut, c.outOffsetNs)) {
				obs_log(LOG_WARNING,
					"[reel] event %d angle %d has no anchored "
					"footage — left out",
					ev.id, a + 1);
				continue;
			}
			if (pOut != c.path) {
				obs_log(LOG_WARNING,
					"[reel] event %d angle %d spans a file "
					"split — left out",
					ev.id, a + 1);
				continue;
			}
			clips.push_back(std::move(c));
		}
	}
	if (clips.empty()) {
		errorOut = "nothing to export: no selected event has an enabled "
			   "angle with anchored footage";
		return false;
	}

	std::string folder = customFolder;
	if (folder.empty()) {
		fs::path p(ReplayCore::instance().recordingFolder());
		p /= "export";
		folder = p.string();
	}
	std::error_code ec;
	fs::create_directories(folder, ec);
	std::string musicPath;
	if (withMusic) {
		musicPath = musicFileFor(
			ReplayCore::instance().getConfig().musicSourceName);
		if (musicPath.empty())
			obs_log(LOG_WARNING,
				"[reel] music was asked for, but the configured "
				"music source is not a media source with a local "
				"file — the reel will carry the clips' own audio");
	}

	// A name that says what it holds: how many clips, and whether it has
	// music on it. Two reels of the same selection must not collide.
	fs::path out(folder);
	out /= "highlights_" + std::to_string(clips.size()) + "clips" +
	       (musicPath.empty() ? "" : "_music") + ".mp4";
	const std::string outPath = out.string();

	// Runs on a thread of its own: this reads and writes gigabytes, and the
	// dock's button must come back immediately.
	std::thread([this, clips, musicPath, outPath]() {
		std::string detail;
		const bool ok = runReel(clips, musicPath, outPath, detail);
		obs_log(ok ? LOG_INFO : LOG_ERROR, "[reel] %s: %s",
			ok ? "wrote" : "FAILED", ok ? outPath.c_str()
						    : detail.c_str());
	}).detach();
	return true;
}

bool ExportManager::runReel(const std::vector<ReelClip> &clips,
			    const std::string &musicPath,
			    const std::string &outPath, std::string &detail)
{
	const AVRational nsTb = {1, 1000000000};

	// Audio from the clips only makes sense when nothing is stretched: the
	// timestamps of a 50% clip are pulled apart, and pulling audio apart
	// with them is the wrong pitch with holes in it (see runJob). Music
	// replaces it outright when there is any.
	bool clipAudio = musicPath.empty();
	for (const auto &c : clips)
		if (std::abs(c.speed - 1.0) > 0.001)
			clipAudio = false;

	AVFormatContext *out = nullptr;
	avformat_alloc_output_context2(&out, nullptr, "mp4", outPath.c_str());
	if (!out) {
		detail = "cannot create output";
		return false;
	}

	// The output streams are modelled on the FIRST clip and every later one
	// has to match: stream copy cannot reconcile two different encoders or
	// two picture sizes, and a file that changes either halfway through
	// decodes as garbage from that point on. Refusing by name beats that.
	AVCodecParameters *vPar = nullptr;
	AVRational vTb = {1, 90000};
	AVStream *vOut = nullptr;
	AVStream *aOut = nullptr;
	AVFormatContext *music = nullptr;
	int musicIdx = -1;

	auto fail = [&](const std::string &why) {
		detail = why;
		if (music)
			avformat_close_input(&music);
		if (out && !(out->oformat->flags & AVFMT_NOFILE) && out->pb)
			avio_closep(&out->pb);
		avformat_free_context(out);
		return false;
	};

	{
		AVFormatContext *probe = nullptr;
		if (avformat_open_input(&probe, clips.front().path.c_str(), nullptr,
					nullptr) < 0 ||
		    avformat_find_stream_info(probe, nullptr) < 0) {
			if (probe)
				avformat_close_input(&probe);
			return fail("cannot open " + clips.front().path);
		}
		for (unsigned i = 0; i < probe->nb_streams; i++) {
			AVCodecParameters *par = probe->streams[i]->codecpar;
			if (par->codec_type == AVMEDIA_TYPE_VIDEO && !vOut) {
				vOut = avformat_new_stream(out, nullptr);
				avcodec_parameters_copy(vOut->codecpar, par);
				vOut->codecpar->codec_tag = 0;
				vTb = probe->streams[i]->time_base;
				vOut->time_base = vTb;
				vPar = vOut->codecpar;
			} else if (par->codec_type == AVMEDIA_TYPE_AUDIO &&
				   clipAudio && !aOut) {
				aOut = avformat_new_stream(out, nullptr);
				avcodec_parameters_copy(aOut->codecpar, par);
				aOut->codecpar->codec_tag = 0;
				aOut->time_base = probe->streams[i]->time_base;
			}
		}
		avformat_close_input(&probe);
	}
	if (!vOut)
		return fail("the first clip has no video stream");

	if (!musicPath.empty()) {
		if (avformat_open_input(&music, musicPath.c_str(), nullptr,
					nullptr) < 0 ||
		    avformat_find_stream_info(music, nullptr) < 0) {
			if (music)
				avformat_close_input(&music);
			music = nullptr;
			obs_log(LOG_WARNING,
				"[reel] cannot read the music file '%s' — the reel "
				"will be silent",
				musicPath.c_str());
		} else {
			for (unsigned i = 0; i < music->nb_streams; i++) {
				if (music->streams[i]->codecpar->codec_type !=
				    AVMEDIA_TYPE_AUDIO)
					continue;
				musicIdx = (int)i;
				aOut = avformat_new_stream(out, nullptr);
				avcodec_parameters_copy(aOut->codecpar,
							music->streams[i]->codecpar);
				aOut->codecpar->codec_tag = 0;
				aOut->time_base = music->streams[i]->time_base;
				break;
			}
			if (musicIdx < 0)
				obs_log(LOG_WARNING,
					"[reel] '%s' has no audio stream",
					musicPath.c_str());
		}
	}

	if (!(out->oformat->flags & AVFMT_NOFILE) &&
	    avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0)
		return fail("cannot write " + outPath);
	if (avformat_write_header(out, nullptr) < 0)
		return fail("cannot write the header");
	// The muxer OWNS the stream timebase and rewrites it inside
	// write_header (mp4 picks its own timescale). Everything after this
	// point counts in the stream's units, so the local copy has to be
	// re-read here — with the pre-header value the reel reported itself as
	// 42 minutes long and, worse, would have laid 42 minutes of music over
	// three seconds of pictures.
	vTb = vOut->time_base;

	// --- the clips, end to end -------------------------------------------
	// Each one starts at the keyframe at or before its IN, and those extra
	// frames STAY. In a single clip they are trimmed by the MP4 edit list
	// (runJob keeps them at negative pts); a reel is one continuous track,
	// where an edit list can only trim the front of the whole file. So a
	// highlight starts up to one GOP early — keyint_sec is 1, so up to a
	// second, usually much less. The alternative is re-encoding the reel,
	// which is the honest fix and a much bigger one; until then this is
	// stated rather than hidden.
	int64_t offsetTb = 0; // where the next clip begins, in vTb units
	int64_t written = 0;
	for (const auto &c : clips) {
		AVFormatContext *in = nullptr;
		if (avformat_open_input(&in, c.path.c_str(), nullptr, nullptr) < 0 ||
		    avformat_find_stream_info(in, nullptr) < 0) {
			if (in)
				avformat_close_input(&in);
			return fail("cannot open " + c.path);
		}
		int vIdx = -1, aIdx = -1;
		for (unsigned i = 0; i < in->nb_streams; i++) {
			const AVCodecParameters *par = in->streams[i]->codecpar;
			if (par->codec_type == AVMEDIA_TYPE_VIDEO && vIdx < 0)
				vIdx = (int)i;
			else if (par->codec_type == AVMEDIA_TYPE_AUDIO && aIdx < 0)
				aIdx = (int)i;
		}
		if (vIdx < 0) {
			avformat_close_input(&in);
			return fail("a clip has no video stream");
		}
		const AVCodecParameters *par = in->streams[vIdx]->codecpar;
		if (par->codec_id != vPar->codec_id || par->width != vPar->width ||
		    par->height != vPar->height) {
			const std::string why =
				"event " + std::to_string(c.eventId) + " angle " +
				std::to_string(c.angle + 1) +
				" is a different format from the first clip (" +
				std::to_string(par->width) + "x" +
				std::to_string(par->height) + " vs " +
				std::to_string(vPar->width) + "x" +
				std::to_string(vPar->height) +
				") — a reel cannot be stream-copied across that";
			avformat_close_input(&in);
			return fail(why);
		}

		const int64_t startTs = av_rescale_q(c.inOffsetNs, nsTb,
						     in->streams[vIdx]->time_base);
		av_seek_frame(in, vIdx, startTs, AVSEEK_FLAG_BACKWARD);

		AVPacket *pkt = av_packet_alloc();
		if (!pkt) {
			avformat_close_input(&in);
			return fail("out of memory");
		}
		const double stretch = c.speed > 0 ? 1.0 / c.speed : 1.0;
		int64_t base = AV_NOPTS_VALUE; // first pts written for this clip
		int64_t lastEnd = 0;           // end of this clip, in vTb units
		while (av_read_frame(in, pkt) >= 0) {
			const bool isVideo = pkt->stream_index == vIdx;
			const bool isAudio = aOut && clipAudio &&
					     pkt->stream_index == aIdx;
			if (!isVideo && !isAudio) {
				av_packet_unref(pkt);
				continue;
			}
			AVStream *ist = in->streams[pkt->stream_index];
			const int64_t ptsNs =
				av_rescale_q(pkt->pts, ist->time_base, nsTb);
			if (isVideo && ptsNs > c.outOffsetNs) {
				av_packet_unref(pkt);
				break;
			}
			AVStream *ost = isVideo ? vOut : aOut;
			if (base == AV_NOPTS_VALUE && isVideo)
				base = av_rescale_q(pkt->pts, ist->time_base,
						    ost->time_base);
			if (base == AV_NOPTS_VALUE) {
				// Audio before the first video packet has no
				// place to hang: the clip has not started yet.
				av_packet_unref(pkt);
				continue;
			}
			int64_t pts = av_rescale_q(pkt->pts, ist->time_base,
						   ost->time_base) -
				      base;
			int64_t dts = av_rescale_q(pkt->dts, ist->time_base,
						   ost->time_base) -
				      base;
			int64_t dur = av_rescale_q(pkt->duration, ist->time_base,
						   ost->time_base);
			if (stretch != 1.0) {
				pts = (int64_t)std::llround((double)pts * stretch);
				dts = (int64_t)std::llround((double)dts * stretch);
				dur = (int64_t)std::llround((double)dur * stretch);
			}
			pkt->pts = pts + offsetTb;
			pkt->dts = dts + offsetTb;
			pkt->duration = dur;
			pkt->stream_index = ost->index;
			pkt->pos = -1;
			if (isVideo)
				lastEnd = std::max(lastEnd, pts + std::max(dur, 1LL));
			if (av_interleaved_write_frame(out, pkt) < 0) {
				av_packet_free(&pkt);
				avformat_close_input(&in);
				return fail("write failed");
			}
			written++;
		}
		av_packet_free(&pkt);
		avformat_close_input(&in);
		offsetTb += lastEnd;
	}

	// --- the music, over the whole thing ---------------------------------
	// Copied, not mixed: mixing means decoding both and encoding the result,
	// and there is no encoder on this path. So music REPLACES the clips'
	// audio, which is what a highlights reel does anyway. Trimmed to the
	// reel; if the track is shorter, the reel simply ends silent.
	if (music && musicIdx >= 0 && aOut) {
		const int64_t reelNs = av_rescale_q(offsetTb, vTb, nsTb);
		AVPacket *pkt = av_packet_alloc();
		while (pkt && av_read_frame(music, pkt) >= 0) {
			if (pkt->stream_index != musicIdx) {
				av_packet_unref(pkt);
				continue;
			}
			AVStream *ist = music->streams[musicIdx];
			if (av_rescale_q(pkt->pts, ist->time_base, nsTb) > reelNs) {
				av_packet_unref(pkt);
				break;
			}
			pkt->pts = av_rescale_q(pkt->pts, ist->time_base,
						aOut->time_base);
			pkt->dts = pkt->pts;
			pkt->duration = av_rescale_q(pkt->duration, ist->time_base,
						     aOut->time_base);
			pkt->stream_index = aOut->index;
			pkt->pos = -1;
			if (av_interleaved_write_frame(out, pkt) < 0)
				break;
		}
		if (pkt)
			av_packet_free(&pkt);
	}

	av_write_trailer(out);
	if (music)
		avformat_close_input(&music);
	if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb)
		avio_closep(&out->pb);
	avformat_free_context(out);
	obs_log(LOG_INFO,
		"[reel] %zu clip(s), %lld packet(s), %lld ms, audio: %s",
		clips.size(), (long long)written,
		(long long)(av_rescale_q(offsetTb, vTb, nsTb) / 1000000),
		music ? "music" : (clipAudio ? "from the clips" : "none"));
	return true;
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

	// Slow motion is spacing, here as much as in the player: the packets are
	// copied untouched and their timestamps are stretched. No re-encode, so
	// the clip is still written in a fraction of real time.
	//
	// The audio goes with it, though — it is left out entirely when the
	// speed is not 100%. Stretching audio timestamps the same way would
	// play it at the wrong pitch with gaps in it, and there is no
	// resampler on this path. A stadium replay in slow motion is silent,
	// which is also what this plugin's own playback does.
	const bool stretched = job.speed > 0 && std::abs(job.speed - 1.0) > 0.001;
	std::vector<int> streamMap((size_t)in->nb_streams, -1);
	for (unsigned i = 0; i < in->nb_streams; i++) {
		AVCodecParameters *par = in->streams[i]->codecpar;
		if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
		    par->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		if (stretched && par->codec_type == AVMEDIA_TYPE_AUDIO)
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
			if (stretched) {
				// 50% = twice the spacing. Applied AFTER the
				// baseline subtraction so the frames the edit
				// list trims (the ones between the keyframe and
				// IN, at negative pts) stretch with the rest and
				// the clip still starts exactly on IN.
				const double f = 1.0 / job.speed;
				pkt->pts = (int64_t)std::llround((double)pkt->pts * f);
				pkt->dts = (int64_t)std::llround((double)pkt->dts * f);
				pkt->duration =
					(int64_t)std::llround((double)pkt->duration * f);
			}
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
