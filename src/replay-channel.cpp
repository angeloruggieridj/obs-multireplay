/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See replay-channel.hpp.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)

#include "replay-channel.hpp"

#include "audio-stretch.hpp" // slow motion with sound, at the right pitch
#include "packet-tap.hpp"
#include "plugin-support.h"
#include "replay-decoder.hpp"
#include "reverse-plan.hpp"
#include "segment-reader.hpp"
#include "replay-core.hpp" // debugLoggingEnabled()

#include <media-io/video-io.h>
#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>

namespace multireplay {

namespace {

constexpr const char *kSourceId = "multireplay_channel";
// One name per channel. "Replay A" keeps the name it has always had, so an
// existing scene collection is untouched by B arriving.
constexpr const char *kSourceNames[kChannels] = {"MultiReplay - Replay A",
						 "MultiReplay - Replay B"};

// The source object itself is deliberately inert: it owns no playback state and
// makes no decisions. ReplayChannel pushes frames into it. That keeps the OBS
// lifetime (which the scene collection controls) independent of the playback
// lifetime (which the operator controls).
struct ChannelSource {
	obs_source_t *source = nullptr;
};

const char *sourceGetName(void *)
{
	// The TYPE's name, which is what OBS shows in "Add source" — not a
	// channel's. The two channels are two inputs of this one type.
	return "MultiReplay Replay";
}

void *sourceCreate(obs_data_t *, obs_source_t *source)
{
	auto *ctx = new ChannelSource();
	ctx->source = source;
	return ctx;
}

void sourceDestroy(void *data)
{
	delete static_cast<ChannelSource *>(data);
}

audio_format obsAudioFormatFor(SampleFormat f)
{
	switch (f) {
	case SampleFormat::F32Planar:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	case SampleFormat::F32:
		return AUDIO_FORMAT_FLOAT;
	case SampleFormat::S16:
		return AUDIO_FORMAT_16BIT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

speaker_layout speakersFor(uint32_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

// --- "Fit to screen", applied to every scene item that shows the replay ----
// One item at a time; see ReplayChannel::applyCanvasFit for the why.
struct FitCtx {
	obs_source_t *target = nullptr;
	uint32_t cx = 0, cy = 0;
	int changed = 0;
};

bool fitSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<FitCtx *>(param);
	// Groups are their own coordinate space, so "fill the canvas" means
	// nothing inside one: a replay deliberately put in a group is the
	// operator's own composition and is left alone.
	if (obs_sceneitem_get_source(item) != ctx->target)
		return true;

	// Already fitted: do not rewrite the transform on every clip, or the
	// scene collection would be marked dirty thirty times an evening for
	// nothing.
	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_SCALE_INNER) {
		struct vec2 have = {};
		obs_sceneitem_get_bounds(item, &have);
		if ((uint32_t)have.x == ctx->cx && (uint32_t)have.y == ctx->cy)
			return true;
	}

	struct vec2 bounds = {};
	vec2_set(&bounds, (float)ctx->cx, (float)ctx->cy);
	struct vec2 pos = {};
	vec2_set(&pos, (float)ctx->cx / 2.0f, (float)ctx->cy / 2.0f);

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_defer_update_end(item);
	ctx->changed++;
	return true;
}

// Collect the scenes first: obs_enum_scenes runs its callback with libobs'
// global source mutex held, and taking a scene's own mutex under it (which is
// what touching an item does) is a lock order we have no business inventing.
bool collectScene(void *param, obs_source_t *sceneSource)
{
	auto *out = static_cast<std::vector<obs_source_t *> *>(param);
	if (obs_source_t *ref = obs_source_get_ref(sceneSource))
		out->push_back(ref);
	return true;
}

video_format obsFormatFor(FrameFormat f)
{
	switch (f) {
	case FrameFormat::I420:
		return VIDEO_FORMAT_I420;
	case FrameFormat::NV12:
		return VIDEO_FORMAT_NV12;
	case FrameFormat::I422:
		return VIDEO_FORMAT_I422;
	case FrameFormat::I444:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

// Rows in plane `i`, which is the one thing a plane pointer does not carry and
// the one thing a COPY of a picture needs: linesize is the width of a row, and
// only the layout says how many of them there are. Getting this wrong reads past
// the frame (crash) or copies half a chroma plane (green picture).
uint32_t planeRows(FrameFormat f, int i, uint32_t height)
{
	const uint32_t half = (height + 1) / 2;
	switch (f) {
	case FrameFormat::I420:
		return i == 0 ? height : (i <= 2 ? half : 0);
	case FrameFormat::NV12:
		return i == 0 ? height : (i == 1 ? half : 0);
	case FrameFormat::I422:
	case FrameFormat::I444:
		return i <= 2 ? height : 0;
	default:
		return 0;
	}
}

// A decoded picture we OWN. ReplayDecoder hands out pointers into its own frame,
// valid only until the next receive(), and reverse playback exists precisely to
// hold pictures past that — so reverse is the only path that copies.
struct CachedFrame {
	int64_t masterNs = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	FrameFormat format = FrameFormat::Unknown;
	bool fullRange = false;
	std::vector<uint8_t> plane[4];
	int linesize[4] = {0, 0, 0, 0};

	// One allocation per plane per picture, which for a 1-second GOP is
	// thirty of them once a second — not per frame. Sized from the layout,
	// because a plane pointer says nothing about how much of it is ours.
	void adopt(const ReplayDecoder::Frame &f)
	{
		masterNs = f.masterNs;
		width = f.width;
		height = f.height;
		format = f.format;
		fullRange = f.fullRange;
		for (int i = 0; i < 4; i++) {
			const uint32_t rows = planeRows(f.format, i, f.height);
			const size_t stride =
				f.linesize[i] > 0 ? (size_t)f.linesize[i] : 0;
			linesize[i] = f.linesize[i];
			if (!rows || !stride || !f.data[i]) {
				plane[i].clear();
				linesize[i] = 0;
				continue;
			}
			plane[i].resize(stride * rows);
			memcpy(plane[i].data(), f.data[i], stride * rows);
		}
	}

	size_t bytes() const
	{
		size_t n = 0;
		for (int i = 0; i < 4; i++)
			n += plane[i].size();
		return n;
	}
};

// Hand one picture to OBS at `dueNs` on the wall clock. Shared by both
// directions: everything about the OBS-facing frame is identical, only the
// arithmetic that produced `dueNs` differs.
void outputFrame(obs_source_t *source, uint32_t width, uint32_t height,
		 FrameFormat format, bool fullRange,
		 const uint8_t *const data[4], const int linesize[4],
		 uint64_t dueNs)
{
	const video_format fmt = obsFormatFor(format);
	if (fmt == VIDEO_FORMAT_NONE)
		return;

	struct obs_source_frame2 out = {};
	out.width = width;
	out.height = height;
	out.timestamp = dueNs;
	out.format = fmt;
	out.range = fullRange ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	for (int i = 0; i < 4 && i < MAX_AV_PLANES; i++) {
		out.data[i] = const_cast<uint8_t *>(data[i]);
		out.linesize[i] = (uint32_t)linesize[i];
	}
	video_format_get_parameters_for_format(height >= 720 ? VIDEO_CS_709
							     : VIDEO_CS_601,
					       out.range, fmt, out.color_matrix,
					       out.color_range_min,
					       out.color_range_max);

	obs_source_output_video2(source, &out);
}

// The longest this code will sleep without looking up again. A frame at 5% is
// two thirds of a second away, and a Stop, a pause or a speed change that
// landed at the start of that sleep must not wait it out.
constexpr uint64_t kNapNs = 10'000'000ULL;

} // namespace

ReplayChannel &ReplayChannel::instance(Which which)
{
	static ReplayChannel a(Which::A);
	static ReplayChannel b(Which::B);
	return which == Which::B ? b : a;
}

ReplayChannel::~ReplayChannel()
{
	unload();
}

std::unique_ptr<ReplayChannel> ReplayChannel::makePreview(std::string name)
{
	// make_unique cannot reach a private constructor, and the constructor
	// stays private so nothing accidentally builds a third CHANNEL.
	std::unique_ptr<ReplayChannel> feed(new ReplayChannel(Which::A));
	feed->preview_ = true;
	feed->previewName_ = std::move(name);
	// No load(): the source type is registered once by the channels, and a
	// feed registering it again would be a second obs_register_source for the
	// same id.
	feed->ensureSource();
	return feed;
}

const char *ReplayChannel::sourceNameOf(Which which)
{
	return kSourceNames[which == Which::B ? 1 : 0];
}

const char *ReplayChannel::sourceName() const
{
	return preview_ ? previewName_.c_str() : sourceNameOf(which_);
}

void ReplayChannel::load()
{
	// The TYPE is registered once for both channels; each instance then goes
	// on to adopt or create its own input by name.
	static bool registered = false;
	if (registered)
		return;

	struct obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	// Async video + audio, exactly like a capture card: OBS handles the
	// scaling, the mixing and the A/V sync for us.
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = sourceGetName;
	info.create = sourceCreate;
	info.destroy = sourceDestroy;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	obs_register_source(&info);

	registered = true;
	obs_log(LOG_INFO, "[channel] source type registered (%s)", kSourceId);
}

void ReplayChannel::unload()
{
	std::lock_guard<std::mutex> cmd(commandMutex_);
	stopLocked();
	std::lock_guard<std::mutex> pf(prefetchMutex_);
	// The prefetch thread holds no OBS handles and cannot be aborted mid-read,
	// but it MUST be joined: a std::thread still joinable at destruction is
	// std::terminate, and that would be OBS disappearing on shutdown rather
	// than closing. It is bounded by one fetch.
	joinPrefetch();
	{
		std::lock_guard<std::mutex> lock(cacheMutex_);
		cache_.clear();
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (source_) {
		obs_source_release(source_);
		source_ = nullptr;
	}
}

void ReplayChannel::releaseSource()
{
	// Stop FIRST and outside mutex_: the worker holds its own ref while it
	// pushes frames, and releasing ours while it is mid-push would leave it
	// pushing into a source OBS is dismantling.
	std::lock_guard<std::mutex> cmd(commandMutex_);
	stopLocked();
	std::lock_guard<std::mutex> lock(mutex_);
	if (!source_)
		return;
	obs_source_release(source_);
	source_ = nullptr;
	obs_log(LOG_INFO, "[channel] released '%s' before OBS cleared scene data",
		sourceName());
}

void ReplayChannel::ensureSource()
{
	std::lock_guard<std::mutex> lock(mutex_);

	// A PREVIEW FEED OWNS ITS INPUT OUTRIGHT. No find-by-name: a private
	// source is not in the name table, so looking one up would find nothing
	// and create a second one on every call. There is nothing to adopt after a
	// scene-collection change either — the collection never held it.
	if (preview_) {
		if (source_)
			return;
		obs_data_t *psettings = obs_data_create();
		source_ = obs_source_create_private(kSourceId,
						    previewName_.c_str(),
						    psettings);
		obs_data_release(psettings);
		if (!source_) {
			obs_log(LOG_ERROR, "[channel] could not create feed '%s'",
				previewName_.c_str());
			return;
		}
		obs_source_set_async_unbuffered(source_, true);
		obs_log(LOG_INFO, "[channel] created preview feed '%s'",
			previewName_.c_str());
		return;
	}

	// Look the name up BEFORE dropping what we hold. A scene-collection
	// change replaces the objects behind the names, so the stale ref does
	// have to go — but releasing it first destroys the very input we are
	// about to adopt whenever ours is the last reference, which is the
	// normal case until the operator drags "Replay A" into a scene. OBS then
	// logged two "created OBS input" lines per start and left its audio
	// mixer holding a dead source ("Tried to sort VolumeControl for
	// 'MultiReplay - Replay A' but source is null"). Taking the new ref
	// first keeps the object alive across the swap.
	obs_source_t *existing = obs_get_source_by_name(sourceName());
	obs_source_t *previous = source_;
	source_ = existing; // already add-ref'd, may be null
	if (previous)
		obs_source_release(previous);
	if (source_)
		return;

	obs_data_t *settings = obs_data_create();
	source_ = obs_source_create(kSourceId, sourceName(), settings, nullptr);
	obs_data_release(settings);

	if (!source_) {
		obs_log(LOG_ERROR, "[channel] could not create '%s'", sourceName());
		return;
	}
	// We pace playback ourselves, so buffering would only add latency on top
	// of a schedule that is already correct.
	obs_source_set_async_unbuffered(source_, true);
	obs_log(LOG_INFO, "[channel] created OBS input '%s'", sourceName());
}

void ReplayChannel::applyCanvasFit(bool enable)
{
	fitToCanvas_.store(enable);
	if (enable)
		fitSceneItems();
}

void ReplayChannel::fitSceneItems()
{
	// A preview feed is in no scene, so there is no item to fit — and walking
	// every scene in the collection once per tile per clip would be the one
	// place a multiview could cost the UI thread real time.
	if (preview_ || !fitToCanvas_.load())
		return;

	obs_source_t *target = acquireSource();
	if (!target)
		return;

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 ||
	    ovi.base_height == 0) {
		obs_source_release(target);
		return;
	}

	std::vector<obs_source_t *> scenes;
	obs_enum_scenes(collectScene, &scenes);

	FitCtx ctx;
	ctx.target = target;
	ctx.cx = ovi.base_width;
	ctx.cy = ovi.base_height;
	for (obs_source_t *s : scenes) {
		if (obs_scene_t *scene = obs_scene_from_source(s))
			obs_scene_enum_items(scene, fitSceneItem, &ctx);
		obs_source_release(s);
	}
	obs_source_release(target);

	if (ctx.changed)
		obs_log(LOG_INFO,
			"[channel] fitted %d scene item(s) of '%s' to the %ux%u "
			"canvas (aspect preserved)",
			ctx.changed, sourceName(), ctx.cx, ctx.cy);
}

obs_source_t *ReplayChannel::acquireSource()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return source_ ? obs_source_get_ref(source_) : nullptr;
}

void ReplayChannel::setMuted(bool muted)
{
	obs_source_t *src = acquireSource();
	if (!src)
		return;
	if (obs_source_muted(src) != muted)
		obs_source_set_muted(src, muted);
	obs_source_release(src);
}

bool ReplayChannel::muted() const
{
	obs_source_t *src =
		const_cast<ReplayChannel *>(this)->acquireSource();
	if (!src)
		return false;
	const bool m = obs_source_muted(src);
	obs_source_release(src);
	return m;
}

void ReplayChannel::setOnFinished(std::function<void()> fn)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onFinished_ = std::move(fn);
}

// --- pacing ----------------------------------------------------------------

void ReplayChannel::seatPacing(int64_t masterNs, int dir)
{
	std::lock_guard<std::mutex> lock(pacingMutex_);
	pacing_.anchorWall = os_gettime_ns();
	pacing_.anchorMaster = masterNs;
	pacing_.dir = dir;
	pacing_.shownMaster = masterNs;
	pacing_.paused = paused_.load();
	pacing_.pausedAt = pacing_.anchorWall;
	pacing_.seated = true;
}

uint64_t ReplayChannel::waitForFrame(int64_t masterNs)
{
	for (;;) {
		if (abort_.load())
			return 0;

		uint64_t due = 0;
		// M6: "paused" and "aborted" used to be the same value — zero —
		// distinguished by convention. They are two different answers and
		// now they look like it, which also frees dueFor() to return a
		// legitimate zero (see its own note).
		bool pausedNow = false;
		{
			std::lock_guard<std::mutex> lock(pacingMutex_);
			if (pacing_.paused) {
				// Time spent paused is time the clip did not
				// spend playing, so the anchor slides with it and
				// this frame stays exactly as far away as it was
				// when the operator pressed pause.
				const uint64_t now = os_gettime_ns();
				pacing_.anchorWall += now - pacing_.pausedAt;
				pacing_.pausedAt = now;
				pausedNow = true;
			} else {
				due = pacing_.dueFor(masterNs);
			}
		}
		if (pausedNow) {
			std::this_thread::sleep_for(
				std::chrono::nanoseconds(kNapNs));
			continue;
		}

		const uint64_t now = os_gettime_ns();
		if (now >= due) {
			// This frame is the one on screen from here on, which is
			// what a later speed change re-anchors against.
			std::lock_guard<std::mutex> lock(pacingMutex_);
			pacing_.shownMaster = masterNs;
			return due;
		}
		const uint64_t left = due - now;
		std::this_thread::sleep_for(std::chrono::nanoseconds(
			left > kNapNs ? kNapNs : left));
	}
}

void ReplayChannel::setSpeedPct(int pct)
{
	const int clamped = pct < 5 ? 5 : (pct > 400 ? 400 : pct);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		// So the NEXT run of this clip (a re-cue, a swap) uses it too.
		speedPct_ = clamped;
	}
	std::lock_guard<std::mutex> lock(pacingMutex_);
	if (!pacing_.seated) {
		pacing_.speed = clamped / 100.0;
		return;
	}
	// Re-anchor on the frame ON SCREEN. Changing only the divisor would
	// re-date every frame already shown — at half speed the clip would jump
	// backwards, at double speed forwards — which is precisely the "it starts
	// again from the beginning" the operator was complaining about, just with
	// a different landing point.
	const uint64_t now = os_gettime_ns();
	pacing_.anchorWall = now;
	pacing_.anchorMaster = pacing_.shownMaster;
	pacing_.pausedAt = now;
	pacing_.speed = clamped / 100.0;
}

int ReplayChannel::speedPct() const
{
	std::lock_guard<std::mutex> lock(pacingMutex_);
	const double s = pacing_.speed;
	return (int)(s * 100.0 + 0.5);
}

void ReplayChannel::setPaused(bool paused)
{
	paused_.store(paused);
	std::lock_guard<std::mutex> lock(pacingMutex_);
	if (paused == pacing_.paused)
		return;
	pacing_.paused = paused;
	pacing_.pausedAt = os_gettime_ns();
}

void ReplayChannel::joinWorker()
{
	abort_.store(true);
	if (worker_.joinable())
		worker_.join();
	abort_.store(false);
}

bool ReplayChannel::play(int camIndex, int64_t inNs, int64_t outNs, int speedPct,
			 std::string &errorOut, Source source)
{
	PlayRequest req;
	req.camIndex = camIndex;
	req.inNs = inNs;
	req.outNs = outNs;
	req.speedPct = speedPct;
	req.source = source;
	return play(req, errorOut);
}

// Where a clip's packets come from, and the only place that decides. Shared by
// play() and by the prefetch thread, which is the point: what play() would have
// had to wait for can be done before it is asked for.
//
// It touches no playback state, so the two callers cannot interfere. The cache
// is the only thing they share and it has a lock of its own.
bool ReplayChannel::fetchClip(const PlayRequest &req,
			      std::vector<LivePacket> &clip, StreamConfig &cfg,
			      int64_t &presentIn, int64_t &presentOut,
			      std::string &errorOut, bool *fromCache)
{
	const ClipKey key{req.camIndex, req.inNs, req.outNs, req.source};
	if (fromCache)
		*fromCache = false;

	{
		std::lock_guard<std::mutex> lock(cacheMutex_);
		for (size_t i = 0; i < cache_.size(); i++) {
			if (!(cache_[i].key == key))
				continue;
			clip = cache_[i].clip;
			cfg = cache_[i].cfg;
			presentIn = cache_[i].presentIn;
			presentOut = cache_[i].presentOut;
			// Most recently used goes last, so what gets evicted is
			// what nobody came back to.
			CachedClip hit = std::move(cache_[i]);
			cache_.erase(cache_.begin() + (long)i);
			cache_.push_back(std::move(hit));
			if (fromCache)
				*fromCache = true;
			return true;
		}
	}

	bool got = false;
	if (req.source != Source::Segments) {
		got = PacketTap::instance().resolveRange(req.camIndex, req.inNs,
							 req.outNs, clip,
							 presentIn, presentOut);
		if (got)
			cfg = PacketTap::instance().streamConfig(req.camIndex);
		else
			errorOut = "that range is not held in full in the ring";
	}
	if (!got && req.source != Source::Ring) {
		// Older than the RAM window: the same clip, read out of the
		// files Branch Output already wrote. THIS is the slow one — an
		// open, a seek and a demux — and it is what prefetching exists
		// to get out from between the operator and the picture.
		got = segment_reader::readRange(req.camIndex, req.inNs, req.outNs,
						clip, cfg, presentIn, presentOut,
						errorOut);
	}
	if (got)
		cachePut(key, clip, cfg, presentIn, presentOut);
	return got;
}

void ReplayChannel::cachePut(const ClipKey &key,
			     const std::vector<LivePacket> &clip,
			     const StreamConfig &cfg, int64_t presentIn,
			     int64_t presentOut)
{
	size_t bytes = 0;
	for (const LivePacket &p : clip)
		bytes += p.data.size();
	// A clip bigger than the whole budget is not cached, rather than emptying
	// the cache for something that could not stay in it anyway.
	if (bytes > kClipCacheMaxBytes)
		return;

	std::lock_guard<std::mutex> lock(cacheMutex_);
	for (size_t i = 0; i < cache_.size(); i++) {
		if (cache_[i].key == key) {
			cache_.erase(cache_.begin() + (long)i);
			break;
		}
	}
	CachedClip entry;
	entry.key = key;
	entry.clip = clip;
	entry.cfg = cfg;
	entry.presentIn = presentIn;
	entry.presentOut = presentOut;
	entry.bytes = bytes;
	cache_.push_back(std::move(entry));

	size_t total = 0;
	for (const CachedClip &c : cache_)
		total += c.bytes;
	while (!cache_.empty() && (cache_.size() > kClipCacheMaxEntries ||
				   total > kClipCacheMaxBytes)) {
		total -= cache_.front().bytes;
		cache_.erase(cache_.begin());
	}
}

void ReplayChannel::joinPrefetch()
{
	// prefetchMutex_ is the CALLER's job (prefetch, reset, unload): joining
	// while holding it is the whole point — two threads must not both decide
	// the previous prefetch is theirs to join.
	if (prefetchWorker_.joinable())
		prefetchWorker_.join();
	std::lock_guard<std::mutex> lock(cacheMutex_);
	prefetchBusy_ = false;
}

bool ReplayChannel::prefetchBusy() const
{
	std::lock_guard<std::mutex> lock(cacheMutex_);
	return prefetchBusy_;
}

void ReplayChannel::prefetch(const PlayRequest &req)
{
	if (req.outNs <= req.inNs)
		return;
	// A8: prefetchWorker_ is a std::thread like any other, and two callers
	// deciding at once that the previous one is theirs to join is the same
	// race play()/stop() had. Its OWN lock, not commandMutex_: joining costs
	// a whole fetch off disk and play() must never queue behind that.
	std::lock_guard<std::mutex> pf(prefetchMutex_);
	const ClipKey key{req.camIndex, req.inNs, req.outNs, req.source};

	{
		std::lock_guard<std::mutex> lock(cacheMutex_);
		for (const CachedClip &c : cache_)
			if (c.key == key)
				return; // already have it
		if (prefetchBusy_ && prefetchInFlight_ == key)
			return; // already on its way
	}

	// One at a time. Joining the previous one is bounded by a single fetch and
	// happens on the caller's thread — the coordinator, right after it has
	// STARTED a clip, so the picture is already moving while this waits.
	joinPrefetch();
	{
		std::lock_guard<std::mutex> lock(cacheMutex_);
		prefetchInFlight_ = key;
		prefetchBusy_ = true;
	}
	prefetchWorker_ = std::thread([this, req]() {
		std::vector<LivePacket> clip;
		StreamConfig cfg;
		int64_t pin = 0, pout = 0;
		std::string err;
		const uint64_t t0 = os_gettime_ns();
		const bool ok = fetchClip(req, clip, cfg, pin, pout, err);
		if (debugLoggingEnabled())
			obs_log(LOG_INFO, "[channel] prefetch cam%d %s in %lld ms",
				req.camIndex + 1, ok ? "ready" : "failed",
				(long long)((os_gettime_ns() - t0) / 1'000'000));
		std::lock_guard<std::mutex> lock(cacheMutex_);
		prefetchBusy_ = false;
	});
}

bool ReplayChannel::play(const PlayRequest &req, std::string &errorOut)
{
	// A8: one command at a time. Two plays, or a play and a stop, used to be
	// a data race on worker_ and on abort_.
	std::lock_guard<std::mutex> cmd(commandMutex_);
	const int64_t inNs = req.inNs;
	const int64_t outNs = req.outNs;

	if (outNs <= inNs) {
		errorOut = "the range is empty";
		return false;
	}

	std::vector<LivePacket> clip;
	StreamConfig cfg;
	int64_t presentIn = 0, presentOut = 0;
	bool cached = false;
	const uint64_t tFetch0 = os_gettime_ns();
	const bool got = fetchClip(req, clip, cfg, presentIn, presentOut,
				   errorOut, &cached);
	const int64_t fetchMs =
		(int64_t)((os_gettime_ns() - tFetch0) / 1'000'000);
	if (!got)
		return false;

	if (!cfg.videoUsable()) {
		errorOut = "the camera has no usable video configuration";
		return false;
	}

	const uint64_t tJoin0 = os_gettime_ns();
	joinWorker();
	const int64_t joinMs = (int64_t)((os_gettime_ns() - tJoin0) / 1'000'000);
	// The two halves of the delay between an operator's action and the first
	// frame, kept apart because they are fixed in different ways: the fetch by
	// asking for it early, the join by how promptly the previous worker
	// notices it has been aborted. Logged only when it is worth seeing.
	if (fetchMs + joinMs >= 40)
		obs_log(LOG_INFO,
			"[channel] play cam%d: fetch %lld ms (%s), stopping the "
			"previous clip %lld ms",
			req.camIndex + 1, (long long)fetchMs,
			cached ? "cached" : "read", (long long)joinMs);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!source_) {
			errorOut = "the replay source does not exist yet";
			return false;
		}
		clip_ = std::move(clip);
		config_ = cfg;
		presentInNs_ = presentIn;
		presentOutNs_ = presentOut;
		speedPct_ = req.speedPct < 5 ? 5
					     : (req.speedPct > 400 ? 400
								   : req.speedPct);
		direction_ = req.direction;
		maxFrames_ = req.maxFrames < 0 ? 0 : req.maxFrames;
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_ = PlaybackStats{};
		stats_.reverse = req.direction == Direction::Reverse;
	}
	{
		// A new clip is never born paused: Play means play. The anchor is
		// seated by the worker on the first frame it actually shows, which
		// is later than here by however long the first decode takes.
		paused_.store(false);
		std::lock_guard<std::mutex> lock(pacingMutex_);
		pacing_ = Pacing{};
		pacing_.speed = (req.speedPct < 5 ? 5
						 : (req.speedPct > 400
							    ? 400
							    : req.speedPct)) /
				100.0;
		pacing_.dir = req.direction == Direction::Reverse ? -1 : +1;
	}

	// Re-applied on every clip because the picture size changes WITH the
	// angle - a 720p camera then a 1080p one - and because the operator may
	// have dropped the input into a scene since the last one. The bounding
	// box makes both cases right without touching a single pixel: OBS scales
	// on the GPU while compositing, which costs nothing per frame.
	fitSceneItems();

	playing_.store(true);
	worker_ = std::thread([this]() { playbackLoop(); });
	return true;
}

void ReplayChannel::stop()
{
	std::lock_guard<std::mutex> cmd(commandMutex_);
	stopLocked();
}

void ReplayChannel::stopLocked()
{
	joinWorker();
	playing_.store(false);
}

void ReplayChannel::reset()
{
	std::lock_guard<std::mutex> cmd(commandMutex_);
	stopLocked();
	std::lock_guard<std::mutex> pf(prefetchMutex_);
	// This is "forget the previous project", so the fetched clips go too. The
	// keys are master-clock instants and could not collide across projects,
	// but holding another project's footage in RAM after being told to forget
	// it is not something to have to reason about later.
	joinPrefetch();
	{
		std::lock_guard<std::mutex> lock(cacheMutex_);
		cache_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_ = PlaybackStats{};
	}
	paused_.store(false);
	std::lock_guard<std::mutex> lock(pacingMutex_);
	pacing_ = Pacing{};
}

void ReplayChannel::playbackLoop()
{
	std::vector<LivePacket> clip;
	StreamConfig cfg;
	int64_t presentIn = 0, presentOut = 0;
	double speed = 1.0;
	Direction direction = Direction::Forward;
	int maxFrames = 0;
	obs_source_t *source = nullptr;
	std::function<void()> onFinished;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		clip = clip_;
		cfg = config_;
		presentIn = presentInNs_;
		presentOut = presentOutNs_;
		speed = speedPct_ / 100.0;
		direction = direction_;
		maxFrames = maxFrames_;
		source = source_ ? obs_source_get_ref(source_) : nullptr;
		// Taken at START, not at the end: a later play() may already have
		// installed the next clip's callback by the time we get there.
		onFinished = onFinished_;
	}
	if (!source) {
		playing_.store(false);
		return;
	}

	// WHERE THE WAIT BETWEEN THE KEY AND THE PICTURE ACTUALLY GOES.
	//
	// A cue was measured at anywhere from 45 to 125 ms with the fetch served
	// out of the ring and a preroll of zero frames — so it was neither I/O nor
	// the decoder walking a GOP, and there was nothing in the log to say what
	// it was. These two lines split the rest in half: opening the decoder, and
	// getting the first picture out of it. Behind the debug flag, so a
	// production run pays nothing.
	const uint64_t loopStartNs = os_gettime_ns();
	ReplayDecoder dec;
	std::string err;
	if (!dec.open(cfg, err)) {
		obs_log(LOG_ERROR, "[channel] %s", err.c_str());
		obs_source_release(source);
		playing_.store(false);
		// The clip is over before it began — report it, or a queue waiting
		// on this clip would sit there forever.
		if (onFinished)
			onFinished();
		return;
	}
	const uint64_t decOpenNs = os_gettime_ns() - loopStartNs;

	// --- backwards: a different schedule, not a different decode ----------
	// Everything above is shared (the source, the decoder, the failure path);
	// everything below is the forward pacing, which reverse replaces wholesale.
	if (direction == Direction::Reverse) {
		const bool completed = playReverse(source, dec, clip, presentIn,
						   presentOut, maxFrames);
		obs_source_release(source);
		playing_.store(false);
		// Reported unless we were ABORTED. "Reached its end" is not the
		// condition: a run that could not show a single picture is a clip
		// that is over, and a queue waiting on it would otherwise sit
		// there forever (the same reason the decoder-open failure above
		// reports). An abort is the caller's own doing and stays silent —
		// abort_ is still set while this thread is unwinding, and
		// joinWorker() only clears it after the join.
		if ((completed || !abort_.load()) && onFinished)
			onFinished();
		return;
	}

	// SLOW MOTION WITH SOUND — the thing that used to be missing.
	//
	// Audio rode along at 1x only, because slow motion here is wider spacing
	// between frames and the same trick does not work on samples: hand OBS the
	// same AAC buffers on a stretched cadence and it plays low and full of holes.
	// So a replay at 50% was silent, which is the loudest thing a replay can be.
	// AudioStretch (see audio-stretch.hpp) is the missing piece: WSOLA, which
	// lengthens the sound without touching its pitch.
	//
	// Two limits stay, and both are stated rather than hidden:
	//   - PLANAR FLOAT only. That is what AAC decodes to and what Branch
	//     Output's encoders produce; anything else plays silent, as before.
	//   - BACKWARDS is still silent. Reversing sound means reversing the
	//     samples, and the reverse path shows GOP-sized slices newest-first —
	//     the audio of a slice has to be turned round and laid against that
	//     schedule. The plan says not to promise it before its cost on an iGPU
	//     has been measured, and it has not been.
	//   - A PREVIEW FEED IS SILENT, and not by muting: the decoder is never
	//     opened at all. Eight tiles decoding the same action would be eight
	//     AAC decodes for sound nothing can hear — a feed is in no scene and
	//     on no output channel, so libobs mixes none of it.
	const bool wantAudio = !preview_ && !cfg.audioCodec.empty();
	ReplayAudioDecoder adec;
	if (wantAudio && !adec.open(cfg, err))
		obs_log(LOG_WARNING, "[channel] audio unavailable: %s", err.c_str());

	// Audio is still scheduled off a fixed start, because an audio buffer is
	// handed to OBS whole and cannot be re-spaced half way through — so a PAUSE
	// still walks the sound off the picture, which is why pausing remains a 1x
	// affair in practice.
	const uint64_t startWall = os_gettime_ns();
	uint64_t pushed = 0, preroll = 0, audioPushed = 0;
	int64_t firstNs = 0, lastNs = 0;

	// The stretcher, and what it needs in order to hand OBS anything: it produces
	// planar float in ITS blocks, not the decoder's, so the samples land in
	// scratch of ours. Configured on the FIRST buffer, because only a buffer says
	// what the rate, the channel count and the format really are.
	AudioStretch stretch;
	bool stretchReady = false;
	bool stretchRefused = false;
	uint32_t stretchRate = 0; // the rate it was configured for, for the flush
	int64_t audioStartNs = 0;
	uint64_t stretchedOut = 0; // output frames handed over: the timestamp clock
	std::vector<std::vector<float>> stretchBuf;
	std::vector<float *> stretchPtr;
	static constexpr uint32_t kStretchChunk = 1024;

	const auto emitPlanar = [&](uint32_t frames, uint32_t rate,
				    uint32_t channels, uint64_t ts) {
		const speaker_layout layout = speakersFor(channels);
		if (layout == SPEAKERS_UNKNOWN || frames == 0)
			return;
		struct obs_source_audio a = {};
		a.frames = frames;
		a.samples_per_sec = rate;
		a.speakers = layout;
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.timestamp = ts;
		for (uint32_t i = 0; i < channels && i < MAX_AV_PLANES; i++)
			a.data[i] = (const uint8_t *)stretchBuf[i].data();
		obs_source_output_audio(source, &a);
		audioPushed++;
	};

	// Everything the stretcher has ready. The timestamp counts OUTPUT frames from
	// where this clip's audio began: n input frames become n/speed output frames,
	// which is the same factor the video pacing applies to the same span — so the
	// two stay together without an audio clock of our own (hand-rolling one is
	// what caused the dropouts in the first engine).
	const auto drainStretch = [&](uint32_t rate, uint32_t channels) {
		for (;;) {
			const uint32_t got =
				stretch.take(stretchPtr.data(), kStretchChunk);
			if (got == 0)
				return;
			const uint64_t ts =
				startWall +
				(uint64_t)((double)(audioStartNs - presentIn) /
					   speed) +
				(uint64_t)((double)stretchedOut * 1'000'000'000.0 /
					   (double)rate);
			emitPlanar(got, rate, channels, ts);
			stretchedOut += got;
		}
	};

	const auto emitAudio = [&](const ReplayAudioDecoder::Samples &s) {
		if (s.masterNs < presentIn || s.channels == 0)
			return;

		// 1x: the decoder's own buffers, untouched, exactly as before. The
		// ordinary case must not pay for machinery it does not need.
		if (speed == 1.0) {
			const audio_format afmt = obsAudioFormatFor(s.format);
			const speaker_layout layout = speakersFor(s.channels);
			if (afmt == AUDIO_FORMAT_UNKNOWN ||
			    layout == SPEAKERS_UNKNOWN)
				return;

			struct obs_source_audio a = {};
			a.frames = s.frames;
			a.samples_per_sec = s.sampleRate;
			a.speakers = layout;
			a.format = afmt;
			// Same clock transform as the video, so OBS can sync the
			// two itself.
			a.timestamp =
				startWall +
				(uint64_t)((double)(s.masterNs - presentIn) / speed);
			for (int i = 0; i < 8 && i < MAX_AV_PLANES; i++)
				a.data[i] = s.data[i];

			obs_source_output_audio(source, &a);
			audioPushed++;
			return;
		}

		if (stretchRefused)
			return;
		if (s.format != SampleFormat::F32Planar) {
			// Said once, with the reason: silence and no explanation is
			// what sends somebody looking through the mixer.
			stretchRefused = true;
			obs_log(LOG_WARNING,
				"[channel] slow-motion audio needs planar float "
				"samples; this stream is not, so the clip plays "
				"silent");
			return;
		}
		if (!stretchReady) {
			if (!stretch.configure(s.sampleRate, s.channels, speed)) {
				stretchRefused = true;
				obs_log(LOG_WARNING,
					"[channel] cannot time-stretch %u ch at %u Hz "
					"to %.0f%% — the clip plays silent",
					s.channels, s.sampleRate, speed * 100.0);
				return;
			}
			stretchReady = true;
			stretchRate = s.sampleRate;
			audioStartNs = s.masterNs;
			stretchBuf.assign(s.channels,
					  std::vector<float>(kStretchChunk, 0.0f));
			stretchPtr.resize(s.channels);
			for (uint32_t c = 0; c < s.channels; c++)
				stretchPtr[c] = stretchBuf[c].data();
		}

		const float *planes[8] = {};
		for (uint32_t c = 0; c < s.channels && c < 8; c++)
			planes[c] = (const float *)s.data[c];
		stretch.push(planes, s.frames);
		drainStretch(s.sampleRate, s.channels);
	};

	const auto pumpAudio = [&](const LivePacket &p) {
		if (!adec.opened() || p.kind != PacketKind::Audio)
			return;
		std::string aerr;
		if (!adec.send(p, aerr))
			return;
		ReplayAudioDecoder::Samples s;
		while (adec.receive(s))
			emitAudio(s);
	};

	const auto emit = [&](const ReplayDecoder::Frame &f) {
		// Frames before the marked IN only primed the decoder.
		if (f.masterNs < presentIn) {
			preroll++;
			return;
		}
		const video_format fmt = obsFormatFor(f.format);
		if (fmt == VIDEO_FORMAT_NONE)
			return;

		// Where this frame belongs on the wall clock. Slow motion is
		// nothing more than stretching this offset — and because the
		// spacing is computed per frame against a live anchor, the dial
		// can move mid-clip without the clip restarting.
		if (pushed == 0) {
			MR_DLOG("[channel] %s first picture: decoder open "
				"%lld ms, decode %lld ms",
				sourceName(), (long long)(decOpenNs / 1'000'000),
				(long long)((os_gettime_ns() - loopStartNs -
					     decOpenNs) /
					    1'000'000));
			seatPacing(f.masterNs, +1);
		}
		const uint64_t due = waitForFrame(f.masterNs);
		if (due == 0)
			return; // aborted

		outputFrame(source, f.width, f.height, f.format, f.fullRange,
			    f.data, f.linesize, due);

		if (pushed == 0)
			firstNs = f.masterNs;
		lastNs = f.masterNs;
		pushed++;

		// Publish the playhead as it moves, not once at the end.
		// positionNs() IS this field, and the dock reads it thirty times a
		// second to drive the seekbar, the timecode and — in review mode —
		// markTimeNs(). Left until after the loop it stayed at the 0 that
		// play() resets it to for the whole clip, so the bar never moved and
		// a mark taken while reviewing landed at master 0.
		{
			std::lock_guard<std::mutex> lock(statsMutex_);
			stats_.framesPushed = pushed;
			stats_.firstFrameNs = firstNs;
			stats_.lastFrameNs = lastNs;
		}
	};

	// STOP AFTER N PICTURES, FORWARDS TOO (it used to be a reverse-only knob).
	//
	// It is what lets a CUE ask for the same range the play after it will ask
	// for. A cue is a still on the in-point, so it used to request its own
	// two-frame range — a different range, therefore a different key in the
	// clip cache, therefore its own fetch, and then the play fetched the whole
	// thing all over again. Asking for the full range and stopping on the
	// first picture makes the two one fetch: the cue pays it, and the play
	// that follows is a cache hit.
	//
	// The run is still `completed` when it stops here: it showed everything it
	// was asked to show. Reporting it as broken would make the queue treat a
	// cue as a clip that died.
	bool hitFrameCap = false;
	bool completed = true;
	ReplayDecoder::Frame frame;
	for (const auto &p : clip) {
		if (abort_.load() || hitFrameCap) {
			completed = !abort_.load();
			break;
		}
		pumpAudio(p);
		if (!dec.send(p, err)) {
			obs_log(LOG_ERROR, "[channel] %s", err.c_str());
			completed = false;
			break;
		}
		while (dec.receive(frame)) {
			emit(frame);
			if (abort_.load()) {
				completed = false;
				break;
			}
			if (maxFrames > 0 && pushed >= (uint64_t)maxFrames) {
				hitFrameCap = true;
				break;
			}
		}
	}
	if (completed && !hitFrameCap && dec.drain(err)) {
		while (dec.receive(frame) && !abort_.load())
			emit(frame);
	}
	// ...and no audio tail either when the run stopped on its frame cap: the
	// pictures were cut short deliberately, so the sound that belongs after
	// them is sound nobody asked for.
	if (completed && !hitFrameCap && adec.opened()) {
		std::string aerr;
		if (adec.drain(aerr)) {
			ReplayAudioDecoder::Samples s;
			while (adec.receive(s))
				emitAudio(s);
		}
	}
	// The stretcher holds back a block of sound by design (it needs something to
	// match against). Without this the clip would lose its last 40 ms every time
	// — at 25% that is 160 ms of a replay, and it would sound like a fault.
	if (stretchReady) {
		stretch.flush();
		drainStretch(stretchRate, stretch.channels());
	}

	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_.audioPushed = audioPushed;
		stats_.framesPushed = pushed;
		stats_.framesPreroll = preroll;
		stats_.firstFrameNs = firstNs;
		stats_.lastFrameNs = lastNs;
		stats_.lastRunCompleted = completed;
	}

	// M1: a decoder that had to hold a packet back means something stopped
	// draining it. Zero on every path this plugin has — which is exactly why
	// the old code, which reported EAGAIN as success and dropped the packet,
	// could have lost pictures for a whole session without anybody knowing.
	if (dec.stalls() > 0 || adec.stalls() > 0)
		obs_log(LOG_WARNING,
			"[channel] %s: the decoder held back %llu video and %llu "
			"audio packet(s) — its output was not being drained",
			sourceName(), (unsigned long long)dec.stalls(),
			(unsigned long long)adec.stalls());

	obs_source_release(source);
	playing_.store(false);

	// Everything except an ABORTED run reports back: an aborted clip was
	// replaced or stopped by the caller, who already knows. A clip that broke
	// half way through a decode did NOT reach its end and is still over, and
	// staying silent there left the queue waiting on it forever — the same
	// hole the decoder-open failure above was already fixed for.
	if ((completed || !abort_.load()) && onFinished)
		onFinished();
}

// ---------------------------------------------------------------------------
// Reverse
// ---------------------------------------------------------------------------
//
// Two threads, and the reason is a measurement, not a preference. The pictures
// of one GOP can only be shown once the whole GOP has been decoded, so a single
// thread alternates "decode ~30 frames" with "show ~30 frames": on an iGPU that
// decode is ~100 ms, once per GOP, i.e. once per second of footage — a visible
// freeze on air, every second, and worse the slower the review. So the decode
// runs ahead on a thread of its own while this one paces the cache it already
// has. Depth one: two caches in flight, each half the channel's budget.
bool ReplayChannel::playReverse(obs_source_t *source, ReplayDecoder &dec,
				const std::vector<LivePacket> &clip,
				int64_t presentInNs, int64_t presentOutNs,
				int maxFrames)
{
	// One decode pass' worth of pictures, ready to show.
	struct ReadyChunk {
		std::vector<CachedFrame> frames; // ascending by instant
		// What it costs, carried with it so the consumer can give the
		// budget back when it has finished showing it (M7).
		size_t bytes = 0;
	};

	// The size of a picture, measured. The plan divides the cache budget by
	// it, so a guess here is a guess about how many decode passes the run
	// costs: assuming the widest layout (I444) would halve the slice for the
	// NV12 streams Branch Output's encoders actually produce and double the
	// decoding for nothing. One keyframe is enough to learn it.
	size_t frameBytes = 0;
	{
		ReplayDecoder::Frame probe;
		std::string perr;
		int fed = 0;
		for (const LivePacket &p : clip) {
			if (p.kind != PacketKind::Video)
				continue;
			if (!dec.send(p, perr) || ++fed > 16)
				break;
			if (dec.receive(probe)) {
				for (int i = 0; i < 4; i++) {
					const uint32_t rows = planeRows(
						probe.format, i, probe.height);
					if (rows && probe.linesize[i] > 0)
						frameBytes += (size_t)probe.linesize[i] *
							      rows;
				}
				break;
			}
		}
		// Whatever the probe left behind is reference state for a GOP we
		// are not going to show first.
		dec.flush();
	}
	if (frameBytes == 0) {
		// Nothing decoded: fall back to the widest layout so the budget
		// still holds, rather than dividing by zero and planning to hold
		// the whole clip.
		const uint32_t w = dec.width() ? dec.width() : 1920;
		const uint32_t h = dec.height() ? dec.height() : 1080;
		frameBytes = (size_t)w * h * 3;
	}

	reverse_plan::Budget budget;
	budget.frameBytes = frameBytes;
	// HALF the channel's budget per pass: the producer is filling one cache
	// while the consumer still holds the previous one.
	budget.maxBytes = kReverseCacheBudgetBytes / 2;
	const auto passes = reverse_plan::plan(clip, presentInNs, presentOutNs,
					       budget);
	// What this run INTENDS to show, which for a frame step is the cap and not
	// the plan: the plan works in whole GOPs, so a step back plans thirty
	// pictures and shows two. Reporting the plan there would make every step
	// look like a run that lost twenty-eight frames — and "pushed == planned"
	// is the property that catches a schedule losing a slice, so it has to hold
	// for both kinds of run or it is worth nothing.
	const int planned = maxFrames > 0
				    ? std::min(maxFrames,
					       reverse_plan::plannedFrames(passes))
				    : reverse_plan::plannedFrames(passes);
	obs_log(LOG_INFO,
		"[channel] reverse: %zu decode pass(es), %d picture(s) to show, "
		"%zu KiB each, %d per pass%s",
		passes.size(), planned, frameBytes / 1024,
		reverse_plan::framesPerChunk(budget),
		maxFrames > 0 ? " (frame step)" : "");
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_.framesPlanned = planned;
	}
	if (passes.empty()) {
		obs_log(LOG_WARNING,
			"[channel] reverse: nothing to show in that range");
		return false;
	}

	// --- the hand-off ------------------------------------------------------
	// Both waits below are TIMED, and that is not belt-and-braces: abort_ is
	// set by another thread (joinWorker) and cannot notify a condition
	// variable that lives on this stack. An untimed wait on it is a thread
	// asleep forever, and since joinWorker() then join()s this one — from the
	// UI thread, through stop() — that sleep would be a frozen OBS. Polling it
	// every 20 ms costs nothing and cannot hang.
	constexpr auto kPoll = std::chrono::milliseconds(20);
	std::mutex qm;
	std::condition_variable qcv;
	std::deque<ReadyChunk> ready;
	bool decodeFailed = false;
	bool producerDone = false;
	// Set by the consumer when it has shown everything it was going to show
	// (maxFrames), so the producer stops decoding passes nobody will see.
	std::atomic<bool> stopDecoding{false};
	// M7: the HIGH-WATER MARK OF EVERYTHING HELD AT ONCE, not of one pass.
	// It used to be max(bytes of the pass being handed over), which by design
	// is half the story: the whole point of the second thread is that TWO
	// caches are in flight — one queued, one being shown. So the gate check
	// reverse_cache_within_budget was verifying half of what it claimed to,
	// against a budget written for both.
	size_t heldBytes = 0; // guarded by qm
	size_t cachePeak = 0; // guarded by qm

	std::thread producer([&]() {
		// Whatever happens below — a decode error, an abort, the last
		// pass — the consumer must be told, or it waits for a pass that
		// will never come.
		struct Announce {
			std::mutex &m;
			std::condition_variable &cv;
			bool &flag;
			~Announce()
			{
				std::lock_guard<std::mutex> lock(m);
				flag = true;
				cv.notify_all();
			}
		} announce{qm, qcv, producerDone};

		ReplayDecoder::Frame frame;
		std::string derr;
		for (const reverse_plan::Chunk &pass : passes) {
			if (abort_.load() || stopDecoding.load())
				break;

			// Each pass starts from a keyframe, so the decoder must
			// forget the previous GOP: without the flush the first
			// pictures of this one are predicted against references
			// that belong to a different part of the timeline.
			dec.flush();

			ReadyChunk chunk;
			chunk.frames.reserve((size_t)pass.frames);
			const auto keep = [&](const ReplayDecoder::Frame &f) {
				if (f.masterNs < pass.keepFromNs ||
				    f.masterNs > pass.keepToNs)
					return; // decoded only to build state
				chunk.frames.emplace_back();
				chunk.frames.back().adopt(f);
			};

			bool failed = false;
			for (size_t i = pass.decodeStart;
			     i < pass.decodeEnd && i < clip.size(); i++) {
				if (abort_.load() || stopDecoding.load())
					return; // nobody is waiting for this pass
				if (!dec.send(clip[i], derr)) {
					obs_log(LOG_ERROR, "[channel] reverse: %s",
						derr.c_str());
					failed = true;
					break;
				}
				while (dec.receive(frame))
					keep(frame);
			}
			if (!failed && dec.drain(derr)) {
				while (dec.receive(frame))
					keep(frame);
			}
			if (failed) {
				std::lock_guard<std::mutex> lock(qm);
				decodeFailed = true;
				return; // ~Announce notifies
			}

			// Presentation order. The decoder emits in it already;
			// sorting makes that an invariant of this code rather
			// than an assumption about FFmpeg's output.
			std::sort(chunk.frames.begin(), chunk.frames.end(),
				  [](const CachedFrame &a, const CachedFrame &b) {
					  return a.masterNs < b.masterNs;
				  });

			size_t bytes = 0;
			for (const CachedFrame &f : chunk.frames)
				bytes += f.bytes();

			// Depth one: hold at most one finished pass while the
			// consumer shows the previous one. This is the whole
			// memory bound, and the pass is KEPT until there is room
			// for it — a timed wait that gave up would drop a slice
			// of the range and call the run complete.
			for (bool handed = false; !handed;) {
				std::unique_lock<std::mutex> lock(qm);
				if (abort_.load() || stopDecoding.load())
					return;
				if (!ready.empty()) {
					qcv.wait_for(lock, kPoll, [&]() {
						return ready.empty() ||
						       abort_.load() ||
						       stopDecoding.load();
					});
					continue;
				}
				chunk.bytes = bytes;
				heldBytes += bytes;
				cachePeak = std::max(cachePeak, heldBytes);
				ready.push_back(std::move(chunk));
				qcv.notify_all();
				handed = true;
			}
		}
	});

	// --- pacing, newest picture first --------------------------------------
	uint64_t pushed = 0;
	int64_t firstNs = 0, lastNs = 0;
	bool completed = true;
	bool done = false;

	while (!done) {
		ReadyChunk chunk;
		{
			std::unique_lock<std::mutex> lock(qm);
			while (ready.empty() && !producerDone && !abort_.load())
				qcv.wait_for(lock, kPoll);
			if (abort_.load()) {
				completed = false;
				break;
			}
			if (ready.empty()) {
				// The producer is finished. Whether the run is
				// too depends on WHY it finished.
				if (decodeFailed)
					completed = false;
				break;
			}
			chunk = std::move(ready.front());
			ready.pop_front();
			qcv.notify_all(); // the producer may fill the slot again
		}

		for (size_t i = chunk.frames.size(); i-- > 0;) {
			if (abort_.load()) {
				completed = false;
				done = true;
				break;
			}
			const CachedFrame &f = chunk.frames[i];

			// The clock starts on the FIRST picture actually shown,
			// not when the run began: the first decode pass costs
			// real time, and starting the clock before it would make
			// every frame of the first GOP already late.
			if (pushed == 0) {
				seatPacing(f.masterNs, -1);
				firstNs = f.masterNs;
			}
			// Backwards the offset grows as the instant falls, which
			// is the anchor's `dir` and nothing else — so pause and a
			// live speed change work here exactly as they do forwards.
			const uint64_t due = waitForFrame(f.masterNs);
			if (due == 0) {
				completed = false;
				done = true;
				break;
			}

			const uint8_t *data[4] = {
				f.plane[0].empty() ? nullptr : f.plane[0].data(),
				f.plane[1].empty() ? nullptr : f.plane[1].data(),
				f.plane[2].empty() ? nullptr : f.plane[2].data(),
				f.plane[3].empty() ? nullptr : f.plane[3].data()};
			outputFrame(source, f.width, f.height, f.format,
				    f.fullRange, data, f.linesize, due);

			lastNs = f.masterNs;
			pushed++;
			{
				std::lock_guard<std::mutex> lock(statsMutex_);
				stats_.framesPushed = pushed;
				stats_.firstFrameNs = firstNs;
				stats_.lastFrameNs = lastNs;
			}

			// A step back is a reverse run of exactly two pictures:
			// the one on screen and the one before it. Stopping here
			// rather than planning a two-frame range is what keeps
			// the step exact — the plan works in whole GOPs.
			if (maxFrames > 0 && pushed >= (uint64_t)maxFrames) {
				done = true;
				break;
			}
		}
		// Shown, so its pictures are about to be freed: the budget goes
		// back before the next pass is admitted (M7).
		{
			std::lock_guard<std::mutex> lock(qm);
			heldBytes = heldBytes > chunk.bytes ? heldBytes - chunk.bytes
							    : 0;
		}
	}

	stopDecoding.store(true);
	qcv.notify_all();
	if (producer.joinable())
		producer.join();

	size_t peak = 0;
	{
		std::lock_guard<std::mutex> lock(qm);
		peak = cachePeak;
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex_);
		stats_.framesPushed = pushed;
		stats_.firstFrameNs = firstNs;
		stats_.lastFrameNs = lastNs;
		stats_.lastRunCompleted = completed;
		stats_.cacheBytesPeak = peak;
	}
	obs_log(LOG_INFO,
		"[channel] reverse: showed %llu picture(s) of %d, %lld ms → %lld ms, "
		"cache peak %zu KiB%s",
		(unsigned long long)pushed, planned,
		(long long)(firstNs / 1000000), (long long)(lastNs / 1000000),
		peak / 1024,
		completed ? "" : (abort_.load() ? ", aborted" : ", incomplete"));
	return completed;
}

ReplayChannel::PlaybackStats ReplayChannel::stats() const
{
	std::lock_guard<std::mutex> lock(statsMutex_);
	return stats_;
}

} // namespace multireplay
