/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See segment-index.hpp.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "segment-index.hpp"

#include "packet-tap.hpp"
#include "path-utf8.hpp"
#include "plugin-support.h"
#include "segment-anchor.hpp"
#include "session-clock.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace multireplay {

namespace {

constexpr int kScanIntervalMs = 500;
// Anchoring re-demuxes the file, so it must not run at scan rate: on a modest
// machine that I/O showed up as extra encoder latency on the live path, which
// is the one thing this subsystem must never disturb.
constexpr int kAnchorEveryNScans = 4; // ~2 s
// A file's window only stays in the ring for as long as the ring is deep, so
// give up long before that rather than retrying forever.
constexpr int kMaxAnchorAttempts = 30; // ~60 s
// Attempt count for a file we have stopped trying to anchor. It stays in the
// pending list wearing this marker rather than being erased: scanFolder only
// skips files it already knows about, so an erased one was immediately
// rediscovered, re-queued with a fresh counter and given up on again - a loop
// that re-demuxed the same files forever and filled the log with the same two
// warnings every couple of seconds.
constexpr int kAnchorAbandoned = -1;
constexpr const char *kAnchorsFile = "anchors.json";

// Branch Output writes "cam<N>_<timestamp>.mp4" (see branch-output-control).
int cameraIndexFromName(const std::string &name)
{
	if (name.rfind("cam", 0) != 0 || name.size() < 5)
		return -1;
	const char digit = name[3];
	if (digit < '1' || digit > '8')
		return -1;
	if (name[4] != '_')
		return -1;
	return digit - '1';
}

bool isRecordingFile(const std::filesystem::path &p)
{
	const std::string ext = pathToUtf8(p.extension());
	return ext == ".mp4" || ext == ".mov" || ext == ".mkv";
}

struct FilePacket {
	uint32_t size = 0;
	int64_t ptsNs = 0; // presentation time within the file
};

// The first `count` video packets of a file, with their in-file timestamps.
// Empty on failure, which for a file that has just been created simply means
// "not readable yet".
std::vector<FilePacket> probeVideoPackets(const std::string &path, size_t count)
{
	std::vector<FilePacket> packets;
	AVFormatContext *fmt = nullptr;

	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return packets;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return packets;
	}

	int videoStream = -1;
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoStream = (int)i;
			break;
		}
	}
	if (videoStream < 0) {
		avformat_close_input(&fmt);
		return packets;
	}
	const AVRational tb = fmt->streams[videoStream]->time_base;

	AVPacket *pkt = av_packet_alloc();
	while (pkt && packets.size() < count && av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == videoStream && pkt->size > 0) {
			FilePacket fp;
			fp.size = (uint32_t)pkt->size;
			const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts
								     : pkt->dts;
			fp.ptsNs = (ts == AV_NOPTS_VALUE || tb.den == 0)
					   ? 0
					   : av_rescale_q(ts, tb,
							  AVRational{1, 1'000'000'000});
			packets.push_back(fp);
		}
		av_packet_unref(pkt);
	}
	if (pkt)
		av_packet_free(&pkt);
	avformat_close_input(&fmt);
	return packets;
}

// How far a recording's own timestamps reach, in ns. 0 = the file does not
// say, and that is a real answer: it is returned as "unknown" rather than
// turned into a number the timeline would then be drawn from.
//
// libavformat is asked HOW it knows. When it cannot read the length from the
// container or from the packets it will happily divide the file size by the
// bitrate (AVFMT_DURATION_FROM_BITRATE) - a guess, and on a fragmented MP4
// still being written a bad one. That answer is refused.
int64_t probeFileDurationNs(const std::string &path)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return 0;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return 0;
	}

	const AVRational nsBase{1, 1'000'000'000};
	int64_t ns = 0;

	// The video stream's own duration, when the container carries one.
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		const AVStream *st = fmt->streams[i];
		if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
			continue;
		if (st->duration > 0 && st->duration != AV_NOPTS_VALUE &&
		    st->time_base.den > 0)
			ns = av_rescale_q(st->duration, st->time_base, nsBase);
		break;
	}

	// ...and the container's, which for a fragmented file is read back from
	// the last packets. The larger of the two is the one that saw more of the
	// file: a header written when recording began is stale, never ahead.
	if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE &&
	    fmt->duration_estimation_method != AVFMT_DURATION_FROM_BITRATE) {
		const int64_t fromContainer = av_rescale_q(
			fmt->duration, AVRational{1, AV_TIME_BASE}, nsBase);
		ns = std::max(ns, fromContainer);
	}

	avformat_close_input(&fmt);
	return ns > 0 ? ns : 0;
}

// A file we have tried this many times and still cannot read a length from is
// declared unknown out loud, and left alone.
constexpr int kMaxDurationProbes = 3;

} // namespace

SegmentIndex &SegmentIndex::instance()
{
	static SegmentIndex idx;
	return idx;
}

SegmentIndex::~SegmentIndex()
{
	stop();
}

void SegmentIndex::start(const std::string &folder,
			 const std::array<bool, kMaxSegmentCameras> &cams,
			 const std::array<int, kMaxSegmentCameras> &canonical,
			 int64_t epochMasterNs, int64_t epochWallNs)
{
	stop();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		folder_ = folder;
		cams_ = cams;
		canonical_ = canonical;
		epochMasterNs_ = epochMasterNs;
		epochWallNs_ = epochWallNs;
		for (auto &v : segments_)
			v.clear();
		for (auto &v : pending_)
			v.clear();
		// Before the watcher runs: whatever earlier sessions anchored in
		// this folder is known immediately, so a project opened now can
		// be played at once and scanFolder() does not queue those files
		// for a re-anchor that could never succeed.
		load();
		recomputeBoundaries();
	}
	running_.store(true);
	watcher_ = std::thread([this]() { watchLoop(); });
	obs_log(LOG_INFO, "[segments] watching %s", folder.c_str());
}

void SegmentIndex::stop()
{
	if (!running_.exchange(false))
		return;
	wake_.notify_all();
	if (watcher_.joinable())
		watcher_.join();
	save();
}

void SegmentIndex::watchLoop()
{
	int tick = 0;
	while (running_.load()) {
		scanFolder();
		if (tick++ % kAnchorEveryNScans == 0) {
			tryAnchorPending();
			// Same beat, same reason: both are a demux, and the live
			// path must not pay for either at scan rate.
			refreshDurations();
		}
		{
			std::unique_lock<std::mutex> lock(mutex_);
			recomputeBoundaries();
			wake_.wait_for(lock,
				       std::chrono::milliseconds(kScanIntervalMs),
				       [this]() { return !running_.load(); });
		}
	}
}

void SegmentIndex::scanFolder()
{
	std::string folder;
	std::array<bool, kMaxSegmentCameras> cams{};
	{
		std::lock_guard<std::mutex> lock(mutex_);
		folder = folder_;
		cams = cams_;
	}
	if (folder.empty())
		return;

	std::error_code ec;
	for (const auto &entry :
	     std::filesystem::directory_iterator(utf8ToPath(folder), ec)) {
		if (ec)
			break;
		if (!entry.is_regular_file(ec) || !isRecordingFile(entry.path()))
			continue;

		// A3: UTF-8, never path::string(). This path is stored, written
		// into anchors.json and handed to FFmpeg, and on MSVC
		// path::string() narrows through the ANSI code page — so a
		// project folder with an accent in it produced a path nothing
		// downstream could open.
		const std::string name = pathToUtf8(entry.path().filename());
		const int cam = cameraIndexFromName(name);
		if (cam < 0 || cam >= kMaxSegmentCameras || !cams[cam])
			continue;

		const std::string full = pathToUtf8(entry.path());
		std::lock_guard<std::mutex> lock(mutex_);

		const auto known = std::any_of(segments_[cam].begin(),
					       segments_[cam].end(),
					       [&](const RecordingSegment &s) {
						       return s.path == full;
					       });
		if (known)
			continue;
		const auto waiting = std::any_of(
			pending_[cam].begin(), pending_[cam].end(),
			[&](const auto &p) { return p.first == full; });
		if (waiting)
			continue;

		pending_[cam].emplace_back(full, 0);
		obs_log(LOG_INFO, "[segments] cam%d: new file %s", cam + 1,
			name.c_str());
	}
}

void SegmentIndex::tryAnchorPending()
{
	for (int cam = 0; cam < kMaxSegmentCameras; cam++) {
		if (!running_.load())
			return;
		std::vector<std::pair<std::string, int>> todo;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			todo = pending_[cam];
		}
		if (todo.empty())
			continue;

		// The ring is the evidence; read it once per camera per pass.
		const std::vector<AnchorSample> ring =
			PacketTap::instance().videoSamples(cam);

		// No evidence at all — the index is watching a folder outside REC
		// (an opened project). Probing would still demux every pending
		// file every couple of seconds only to conclude nothing, so skip
		// straight to counting the attempt. Those files are simply not in
		// anchors.json, and nothing here could place them.
		if (ring.empty()) {
			std::lock_guard<std::mutex> lock(mutex_);
			auto &pend = pending_[cam];
			for (auto it = pend.begin(); it != pend.end();) {
				if (it->second == kAnchorAbandoned) {
					++it;
					continue;
				}
				if (++it->second >= kMaxAnchorAttempts) {
					obs_log(LOG_WARNING,
						"[segments] cam%d: %s has no anchor "
						"on record and no live packets to "
						"derive one from - it stays "
						"unplayable rather than guessed",
						cam + 1,
						pathToUtf8(utf8ToPath(it->first)
									  .filename())
								.c_str());
					it->second = kAnchorAbandoned;
				}
				++it;
			}
			continue;
		}

		bool anchoredThisPass = false;
		for (auto &entry : todo) {
			// STOP is pressed on the GUI thread and joins this one.
			// Probing a file is a full demux of a couple of hundred
			// packets, and with several files pending that added up to
			// seconds of frozen interface. Bail between files so the
			// join costs at most one probe.
			if (!running_.load())
				return;
			if (entry.second == kAnchorAbandoned)
				continue; // already given up on; do not re-demux it
			const std::string &path = entry.first;

			// Probe a stretch of the file, not just its opening. The
			// tap can attach a moment after Branch Output starts
			// recording, so the very first packets of a session's
			// first file may never have passed through the ring -
			// but a window a little further in certainly did. Any
			// matched window anchors the file, because the packet's
			// own in-file timestamp says how far it sits from zero.
			const auto probed = probeVideoPackets(path, 240);

			int64_t anchorMasterNs = 0;
			AnchorResult res = AnchorResult::TooFewSamples;

			for (size_t off = 0;
			     off + kAnchorFingerprintLen <= probed.size();
			     off += kAnchorFingerprintLen / 2) {
				std::vector<uint32_t> window;
				window.reserve(kAnchorFingerprintLen);
				for (size_t k = 0; k < kAnchorFingerprintLen; k++)
					window.push_back(probed[off + k].size);

				int64_t matchedMasterNs = 0;
				const AnchorResult r =
					findAnchor(ring, window, matchedMasterNs);
				if (r == AnchorResult::Found) {
					anchorMasterNs = matchedMasterNs -
							 probed[off].ptsNs;
					res = r;
					break;
				}
				// Remember the most informative failure to log.
				if (res != AnchorResult::Ambiguous)
					res = r;
			}

			std::lock_guard<std::mutex> lock(mutex_);
			auto &pend = pending_[cam];
			auto it = std::find_if(pend.begin(), pend.end(),
					       [&](const auto &p) {
						       return p.first == path;
					       });
			if (it == pend.end())
				continue;

			if (res == AnchorResult::Found) {
				RecordingSegment seg;
				seg.path = path;
				seg.anchorMasterNs = anchorMasterNs;
				seg.anchorWallNs =
					epochWallNs_ +
					(anchorMasterNs - epochMasterNs_);
				seg.anchored = true;
				segments_[cam].push_back(seg);
				std::sort(segments_[cam].begin(),
					  segments_[cam].end(),
					  [](const RecordingSegment &a, const RecordingSegment &b) {
						  return a.anchorMasterNs <
							 b.anchorMasterNs;
					  });
				pend.erase(it);
				anchoredThisPass = true;
				obs_log(LOG_INFO,
					"[segments] cam%d: anchored %s at master %lld ms",
					cam + 1,
					pathToUtf8(utf8ToPath(path).filename())
						.c_str(),
					(long long)(anchorMasterNs / 1000000));
				continue;
			}

			// The fingerprint is the file's encoded packet sizes
			// against the ring's. If a muxer ever reshapes packets
			// on the way to disk these stop lining up, and that is
			// invisible without seeing both sides.
			if (it->second == 6 && !probed.empty() && !ring.empty()) {
				std::string a, b;
				for (size_t k = 0; k < 6 && k < probed.size(); k++)
					a += std::to_string(probed[k].size) + " ";
				for (size_t k = 0; k < 6 && k < ring.size(); k++)
					b += std::to_string(ring[k].size) + " ";
				obs_log(LOG_INFO,
					"[segments] cam%d: still unmatched - file sizes [%s] "
					"vs oldest ring sizes [%s] (%zu file, %zu ring)",
					cam + 1, a.c_str(), b.c_str(), probed.size(),
					ring.size());
			}

			if (++it->second >= kMaxAnchorAttempts) {
				obs_log(LOG_WARNING,
					"[segments] cam%d: giving up on %s (%s) - "
					"its opening is no longer in the ring, so "
					"its position cannot be established without "
					"guessing",
					cam + 1,
					pathToUtf8(utf8ToPath(path).filename())
						.c_str(),
					res == AnchorResult::Ambiguous
						? "ambiguous"
						: (res == AnchorResult::NotFound
							   ? "not found"
							   : "not readable"));
				it->second = kAnchorAbandoned;
			}
		}

		// Write the anchor down NOW, not at STOP. anchors.json is the only
		// evidence a later run has - a file whose opening has left the ring
		// can never be re-anchored - and it used to be written only by
		// stop(). A take that ended with OBS being killed (or crashing)
		// therefore left its recordings on disk with no anchor at all: the
		// 18:27 take in this project is 183 MB per camera and permanently
		// unplayable for exactly that reason, and the next run's log says
		// so with "giving up on cam1_...18-27-00.mp4 (not found)".
		// Outside the lock: save() takes it.
		if (anchoredThisPass)
			save();
	}
}

void SegmentIndex::refreshDurations()
{
	// Is anything still being appended to? The tap is attached exactly while
	// Branch Output is recording, so this says whether the NEWEST file of
	// each camera is still growing. Every other file is closed, whatever the
	// recorder is doing.
	const bool writing = PacketTap::instance().anyAttached();

	// Pick one file, under the lock; measure it outside (a demux takes long
	// enough that holding the mutex would stall resolve() on the playback
	// thread).
	std::string target;
	int targetCam = -1;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (int cam = 0; cam < kMaxSegmentCameras && target.empty();
		     cam++) {
			auto &segs = segments_[cam];
			for (size_t i = 0; i < segs.size(); i++) {
				RecordingSegment &s = segs[i];
				if (!s.anchored)
					continue;
				// Still being written: its length now is not its
				// length, and the live edge covers it anyway.
				// It gets measured once the take ends.
				if (writing && i + 1 == segs.size())
					continue;
				if (s.durationFinal ||
				    s.durationProbes >= kMaxDurationProbes)
					continue;
				target = s.path;
				targetCam = cam;
				break;
			}
		}
	}
	if (target.empty() || !running_.load())
		return;

	const int64_t measured = probeFileDurationNs(target);

	std::lock_guard<std::mutex> lock(mutex_);
	auto &segs = segments_[targetCam];
	auto it = std::find_if(segs.begin(), segs.end(),
			       [&](const RecordingSegment &s) {
				       return s.path == target;
			       });
	if (it == segs.end())
		return;

	if (measured > 0) {
		// A length is accepted as FINAL only when two reads agree. The tap
		// detaches before Branch Output's filters are disabled (that order
		// is required, see packet-tap), so there is always a window where
		// nothing is "writing" by our reckoning and the muxer is still
		// flushing its last fragment. One read taken in that window would
		// freeze the timeline a second short of the footage; a second
		// identical read means the file has stopped moving.
		const bool settled = measured == it->durationNs;
		it->durationNs = std::max(it->durationNs, measured);
		it->durationFinal = settled;
		it->durationProbes = 0;
		if (settled)
			obs_log(LOG_INFO, "[segments] cam%d: %s is %lld ms long",
				targetCam + 1,
				pathToUtf8(utf8ToPath(target).filename())
					.c_str(),
				(long long)(it->durationNs / 1000000));
		return;
	}

	if (++it->durationProbes >= kMaxDurationProbes) {
		// Said, not guessed. A file whose end cannot be read simply does
		// not extend the timeline: the alternative is a bar whose length
		// comes from a bitrate division, which is a number that looks
		// exactly as real as a measured one.
		obs_log(LOG_WARNING,
			"[segments] cam%d: %s does not say how long it is - its "
			"end stays unknown rather than estimated, so it does not "
			"extend the timeline",
			targetCam + 1,
			pathToUtf8(utf8ToPath(target).filename()).c_str());
	}
}

void SegmentIndex::recomputeBoundaries()
{
	// A segment covers up to where the next one starts; the newest is still
	// growing and is left open.
	for (auto &segs : segments_) {
		for (size_t i = 0; i + 1 < segs.size(); i++)
			segs[i].endMasterNs = segs[i + 1].anchorMasterNs;
		if (!segs.empty())
			segs.back().endMasterNs = kNoInstant;
	}
}

int SegmentIndex::owner(int camIndex) const
{
	if (camIndex < 0 || camIndex >= kMaxSegmentCameras)
		return camIndex;
	return canonical_[camIndex];
}

bool SegmentIndex::resolve(int camIndex, int64_t masterNs, std::string &pathOut,
			   int64_t &fileTimeNsOut) const
{
	if (camIndex < 0 || camIndex >= kMaxSegmentCameras)
		return false;

	std::lock_guard<std::mutex> lock(mutex_);
	// A slot that only duplicates an earlier slot's source never wrote a file
	// series of its own — the footage lives under the canonical slot instead
	// (see start() and camera-dedup.hpp), and it is the same footage.
	const auto &segs = segments_[owner(camIndex)];
	for (size_t i = 0; i < segs.size(); i++) {
		const RecordingSegment &s = segs[i];
		if (!s.anchored || masterNs < s.anchorMasterNs)
			continue;
		// L4: coveredEndNs(), not endMasterNs. For the last file of a
		// REOPENED project endMasterNs is kNoInstant — nothing follows it
		// — and reading that as "still growing" made this answer yes for
		// any instant after the anchor, hours past the end of a recording
		// that finished yesterday. The measured duration knows better.
		const int64_t covered = s.coveredEndNs();
		if (covered != kNoInstant && masterNs >= covered)
			continue;
		pathOut = s.path;
		fileTimeNsOut = masterNs - s.anchorMasterNs;
		return true;
	}
	return false;
}

int64_t SegmentIndex::oldestNs(int camIndex) const
{
	if (camIndex < 0 || camIndex >= kMaxSegmentCameras)
		return kNoInstant;
	std::lock_guard<std::mutex> lock(mutex_);
	const auto &segs = segments_[owner(camIndex)];
	return segs.empty() ? kNoInstant : segs.front().anchorMasterNs;
}

int64_t SegmentIndex::newestNs(int camIndex) const
{
	if (camIndex < 0 || camIndex >= kMaxSegmentCameras)
		return kNoInstant;
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t newest = kNoInstant;
	for (const RecordingSegment &s : segments_[owner(camIndex)]) {
		if (!s.anchored)
			continue;
		// A closed segment ends where the next one begins; that is known
		// without touching the disk. Only the last one needs its measured
		// length, and a segment that has neither contributes nothing -
		// which is how "not determinable" is expressed here.
		//
		// A duration is a LENGTH, so > 0 is the right test for it. An
		// end is an INSTANT, and instants may be negative (see
		// kNoInstant) - testing one for > 0 is the bug this whole
		// sentinel exists to prevent.
		int64_t end = kNoInstant;
		if (s.endMasterNs != kNoInstant)
			end = s.endMasterNs;
		if (s.durationNs > 0)
			end = std::max(end, s.anchorMasterNs + s.durationNs);
		newest = std::max(newest, end);
	}
	return newest;
}

int64_t SegmentIndex::projectEndNs() const
{
	int64_t newest = kNoInstant;
	for (int cam = 0; cam < kMaxSegmentCameras; cam++)
		newest = std::max(newest, newestNs(cam));
	return newest;
}

int64_t SegmentIndex::projectOriginNs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t oldest = kNoInstant;
	for (const auto &segs : segments_) {
		if (segs.empty())
			continue;
		// Each camera's vector is sorted by anchor, so its front is its
		// own oldest.
		const int64_t first = segs.front().anchorMasterNs;
		if (oldest == kNoInstant || first < oldest)
			oldest = first;
	}
	return oldest;
}

bool SegmentIndex::coversAnyCamera(int64_t masterNs) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &segs : segments_) {
		for (const RecordingSegment &s : segs) {
			if (!s.anchored || masterNs < s.anchorMasterNs)
				continue;
			// No end means "still the newest, still growing", which
			// covers everything after its anchor.
			if (s.endMasterNs == kNoInstant ||
			    masterNs < s.endMasterNs)
				return true;
		}
	}
	return false;
}

std::vector<std::pair<int64_t, int64_t>> SegmentIndex::recordedSpans() const
{
	std::vector<std::pair<int64_t, int64_t>> spans;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &cam : segments_) {
		for (const auto &s : cam) {
			if (!s.anchored)
				continue;
			// The measured length first: it is the only value that
			// describes THIS file. endMasterNs is where the next file
			// took over — the same thing for a split, and wrong for
			// the last file of a take that has stopped.
			int64_t end = kNoInstant;
			if (s.durationNs > 0)
				end = s.anchorMasterNs + s.durationNs;
			else if (s.endMasterNs != kNoInstant)
				end = s.endMasterNs;
			if (end == kNoInstant || end <= s.anchorMasterNs)
				continue; // unknown end: not ours to invent
			spans.push_back({s.anchorMasterNs, end});
		}
	}
	return spans;
}

std::vector<RecordingSegment> SegmentIndex::segments(int camIndex) const
{
	if (camIndex < 0 || camIndex >= kMaxSegmentCameras)
		return {};
	std::lock_guard<std::mutex> lock(mutex_);
	return segments_[owner(camIndex)];
}

int SegmentIndex::anchoredCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int n = 0;
	for (const auto &segs : segments_)
		n += (int)segs.size();
	return n;
}

int SegmentIndex::unanchoredCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int n = 0;
	for (const auto &p : pending_)
		n += (int)p.size();
	return n;
}

void SegmentIndex::save() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (folder_.empty())
		return;

	const std::string outPath = joinUtf8(folder_, kAnchorsFile);

	// Never write an empty index over a file that has entries. save() runs
	// from stop(), and start() calls stop() first - so a run that failed to
	// read anchors.json (or was pointed at the folder before it could) erased
	// it on the next REC press. That is exactly what happened here: the file
	// this project was left with is 15 bytes, `{"segments":[]}`, and every
	// session since has opened with "loaded 0 anchored file(s)". An index with
	// nothing in it has nothing to say about a folder; silence is the correct
	// output, not a blank sheet over the evidence.
	int held = 0;
	for (const auto &segs : segments_)
		held += (int)segs.size();
	if (held == 0) {
		std::error_code ec;
		if (std::filesystem::exists(utf8ToPath(outPath), ec)) {
			obs_log(LOG_INFO,
				"[segments] nothing anchored this run - leaving %s "
				"as it is rather than blanking it",
				kAnchorsFile);
			return;
		}
	}

	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (int cam = 0; cam < kMaxSegmentCameras; cam++) {
		for (const auto &s : segments_[cam]) {
			obs_data_t *o = obs_data_create();
			obs_data_set_int(o, "cam", cam + 1);
			obs_data_set_string(o, "path", s.path.c_str());
			// Only the wall value is written: the monotonic one is
			// meaningless once this process is gone.
			obs_data_set_int(o, "anchor_wall_ns", s.anchorWallNs);
			obs_data_array_push_back(arr, o);
			obs_data_release(o);
		}
	}
	obs_data_set_array(root, "segments", arr);
	obs_data_array_release(arr);

	obs_data_save_json_safe(root, outPath.c_str(), "tmp", "bak");
	obs_data_release(root);
}

void SegmentIndex::load()
{
	// mutex_ must be held by the caller (start()).
	//
	// This is what makes "open yesterday's project" work. The files of a
	// previous session CANNOT be re-anchored against the ring — the ring is
	// this process's, and it is empty — so anchors.json is the only evidence
	// there is. A file sitting in the folder without an entry here therefore
	// stays unresolvable, and that is the correct answer: inventing a
	// position for it is exactly the guessing this engine was rewritten to
	// remove.
	//
	// Only the wall-clock anchor was persisted (see save()), so each segment
	// is put back on THIS session's monotonic clock through the current
	// epoch. resolve() and segment-reader then work unchanged, because they
	// only ever see anchorMasterNs.
	if (folder_.empty())
		return;

	const std::string path = joinUtf8(folder_, kAnchorsFile);
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	if (!root)
		return;

	const SessionEpoch epoch{epochMasterNs_, epochWallNs_};
	int loaded = 0, dropped = 0;

	obs_data_array_t *arr = obs_data_get_array(root, "segments");
	if (arr) {
		const size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *o = obs_data_array_item(arr, i);
			const int cam = (int)obs_data_get_int(o, "cam") - 1;
			const int64_t wall =
				obs_data_get_int(o, "anchor_wall_ns");
			const char *stored = obs_data_get_string(o, "path");

			if (cam < 0 || cam >= kMaxSegmentCameras || !cams_[cam] ||
			    wall <= 0 || !stored || !*stored) {
				obs_data_release(o);
				continue;
			}

			// Re-root on the folder we were pointed at: a project
			// copied to another drive keeps its anchors, and the
			// path must also match what scanFolder() builds or the
			// file would be queued for anchoring a second time.
			std::error_code ec;
			const std::filesystem::path full =
				utf8ToPath(folder_) /
				utf8ToPath(stored).filename();
			if (!std::filesystem::is_regular_file(full, ec)) {
				dropped++;
				obs_data_release(o);
				continue;
			}

			const std::string fullStr = pathToUtf8(full);
			const bool known =
				std::any_of(segments_[cam].begin(),
					    segments_[cam].end(),
					    [&](const RecordingSegment &s) {
						    return s.path == fullStr;
					    });
			if (!known) {
				RecordingSegment seg;
				seg.path = fullStr;
				seg.anchorWallNs = wall;
				seg.anchorMasterNs = wallToMasterNs(epoch, wall);
				seg.anchored = true;
				segments_[cam].push_back(seg);
				loaded++;
			}
			obs_data_release(o);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(root);

	for (auto &segs : segments_)
		std::sort(segs.begin(), segs.end(),
			  [](const RecordingSegment &a, const RecordingSegment &b) {
				  return a.anchorMasterNs < b.anchorMasterNs;
			  });

	obs_log(LOG_INFO,
		"[segments] loaded %d anchored file(s) from %s (%d entry/ies "
		"whose file is gone)",
		loaded, kAnchorsFile, dropped);
}

} // namespace multireplay
