/*
obs-multireplay — unit tests for the live packet ring and the master timeline.
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, no FFmpeg, so this runs in CI on every platform.

These two units carry the correctness of the whole rewrite. The old engine
guessed the recording's lag behind the wall clock and clamped whenever a
request fell outside what it held; both habits produced replays that were
subtly wrong on air. So the properties pinned here are:

  - two cameras whose encoders number their packets from different zeros must
    still resolve one marker to the SAME instant, and
  - a range that cannot be served exactly must be refused, never clamped.
*/

#include "audio-stretch.hpp"
#include "master-timeline.hpp"
#include "packet-ring.hpp"
#include "reverse-plan.hpp"
#include "segment-anchor.hpp"
#include "session-clock.hpp"
#include "timeline-map.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace multireplay;

static int g_fail = 0;

#define CHECK(cond)                                                         \
	do {                                                                \
		if (!(cond)) {                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, \
				    #cond);                                 \
			++g_fail;                                           \
		}                                                           \
	} while (0)

static constexpr int64_t ms(int64_t v)
{
	return v * 1'000'000LL;
}

// --- builders ---------------------------------------------------------------

static LivePacket vpkt(int64_t masterNs, bool keyframe, uint32_t gen = 0,
		       size_t size = 1000)
{
	LivePacket p;
	p.kind = PacketKind::Video;
	p.keyframe = keyframe;
	p.masterNs = masterNs;
	p.dtsNs = masterNs;
	p.generation = gen;
	p.data.assign(size, 0u);
	return p;
}

static LivePacket apkt(int64_t masterNs, uint32_t gen = 0, size_t size = 100)
{
	LivePacket p;
	p.kind = PacketKind::Audio;
	p.masterNs = masterNs;
	p.dtsNs = masterNs;
	p.generation = gen;
	p.data.assign(size, 0u);
	return p;
}

// Fill a ring with `gops` GOPs of `fps` frames each, one keyframe per GOP,
// starting at masterNs = startNs. One audio packet every other frame.
static void fillGops(PacketRing &ring, int gops, int fps, int64_t startNs,
		     uint32_t gen = 0, size_t vsize = 1000)
{
	const int64_t frameNs = 1'000'000'000LL / fps;
	int64_t t = startNs;
	for (int g = 0; g < gops; g++) {
		for (int f = 0; f < fps; f++) {
			ring.push(vpkt(t, f == 0, gen, vsize));
			if (f % 2 == 0)
				ring.push(apkt(t, gen));
			t += frameNs;
		}
	}
}

// --- master timeline --------------------------------------------------------

static void test_rescale()
{
	int64_t ns = 0;
	CHECK(rescaleToNs(30, 1, 30, ns) && ns == 1'000'000'000LL);
	CHECK(rescaleToNs(1, 1, 1000, ns) && ns == 1'000'000LL);
	CHECK(!rescaleToNs(1, 0, 1000, ns));  // zero numerator
	CHECK(!rescaleToNs(1, 1, 0, ns));     // zero denominator
	CHECK(!rescaleToNs(1, 1, -30, ns));   // negative denominator

	// A long session must not overflow: 10 hours at 1/1000.
	CHECK(rescaleToNs(36'000'000, 1, 1000, ns) && ns == 36'000LL * 1'000'000'000LL);
}

static void test_timeline_rejects_malformed()
{
	MasterTimeline tl;
	CHECK(!tl.normalize({0, 0, 0, 1000, 5}).ok);   // no numerator
	CHECK(!tl.normalize({0, 0, 1, 0, 5}).ok);      // no denominator
	CHECK(!tl.normalize({0, 0, 1, 1000, 0}).ok);   // no shared-clock stamp
	CHECK(!tl.aligned());
}

static void test_timeline_first_packet_seats_offset()
{
	MasterTimeline tl;
	// Encoder counts from 0; the system clock says that frame was at 5.000 s.
	auto r = tl.normalize({0, 0, 1, 1000, 5'000'000});
	CHECK(r.ok && r.discontinuity && r.generation == 0);
	CHECK(r.masterNs == ms(5000));
	CHECK(tl.aligned() && tl.offsetNs() == ms(5000));

	// 40 ms later by the encoder's own count.
	r = tl.normalize({40, 40, 1, 1000, 5'040'000});
	CHECK(r.ok && !r.discontinuity && r.masterNs == ms(5040));
}

// THE property the whole design rests on: two encoders numbering their packets
// from completely different zeros must map one instant to one master time.
static void test_timeline_cross_angle_equality()
{
	MasterTimeline camA, camB;

	// Same real instant (system clock 12.000 s) but wildly different
	// private timelines: cam A started long ago, cam B just started.
	CHECK(camA.normalize({900, 900, 1, 1000, 12'000'000}).masterNs == ms(12000));
	CHECK(camB.normalize({0, 0, 1, 1000, 12'000'000}).masterNs == ms(12000));

	// And they stay equal as they advance at their own pts rates.
	const auto a = camA.normalize({940, 940, 1, 1000, 12'040'000});
	const auto b = camB.normalize({40, 40, 1, 1000, 12'040'000});
	CHECK(a.ok && b.ok);
	CHECK(a.masterNs == b.masterNs);
	CHECK(a.masterNs == ms(12040));

	// Different timebases must not break the equality either.
	MasterTimeline camC; // counts in frames at 1/30
	CHECK(camC.normalize({360, 360, 1, 30, 12'000'000}).masterNs == ms(12000));
}

static void test_timeline_discontinuity()
{
	MasterTimeline tl;
	CHECK(tl.normalize({0, 0, 1, 1000, 1'000'000}).ok);
	CHECK(tl.generation() == 0);

	// Jitter well inside tolerance must NOT open a new generation.
	auto r = tl.normalize({40, 40, 1, 1000, 1'040'000 + 50'000}); // +50 ms
	CHECK(r.ok && !r.discontinuity && tl.generation() == 0);

	// A jump past tolerance is a real discontinuity: new generation, and the
	// offset re-seats so later packets are correct rather than skewed.
	r = tl.normalize({80, 80, 1, 1000, 9'000'000}); // system clock jumped
	CHECK(r.ok && r.discontinuity && r.generation == 1);
	CHECK(r.masterNs == ms(9000));

	tl.reset();
	CHECK(tl.generation() == 2 && !tl.aligned());
}

// --- packet ring ------------------------------------------------------------

static void test_ring_resolves_from_previous_keyframe()
{
	PacketRing ring;
	// 3 GOPs of 25 frames (40 ms each) starting at t=0.
	fillGops(ring, 3, 25, 0);

	ResolvedRange r;
	// IN mid-GOP-2 (1.24 s), OUT inside GOP 3.
	CHECK(ring.resolveRange(ms(1240), ms(2200), r));
	CHECK(r.decodeStartNs == ms(1000));  // decode starts at GOP 2's keyframe
	CHECK(r.presentInNs == ms(1240));    // but presents exactly at IN
	CHECK(r.presentOutNs == ms(2200));
	CHECK(ring.at(r.decodeStart).keyframe);
	CHECK(ring.at(r.decodeStart).kind == PacketKind::Video);

	// An IN that lands exactly on a keyframe decodes from that keyframe.
	CHECK(ring.resolveRange(ms(1000), ms(1400), r));
	CHECK(r.decodeStartNs == ms(1000) && r.presentInNs == ms(1000));
}

static void test_ring_refuses_rather_than_clamps()
{
	PacketRing ring;
	fillGops(ring, 2, 25, ms(1000)); // covers [1000, 3000) ms

	ResolvedRange r;
	CHECK(!ring.resolveRange(ms(500), ms(1500), r));  // starts before history
	CHECK(!ring.resolveRange(ms(1500), ms(9000), r)); // runs past the live edge
	CHECK(!ring.resolveRange(ms(2000), ms(1000), r)); // inverted
	CHECK(ring.resolveRange(ms(1500), ms(1600), r));  // inside: fine

	PacketRing empty;
	CHECK(!empty.resolveRange(0, ms(100), r));
}

static void test_ring_refuses_without_keyframe()
{
	PacketRing ring;
	// Video with no keyframe at all: nothing is decodable.
	for (int i = 0; i < 10; i++)
		ring.push(vpkt(ms(i * 40), false));

	ResolvedRange r;
	CHECK(!ring.resolveRange(ms(80), ms(200), r));
}

static void test_ring_refuses_across_discontinuity()
{
	PacketRing ring;
	fillGops(ring, 2, 25, 0, /*gen*/ 0);
	fillGops(ring, 2, 25, ms(2000), /*gen*/ 1);

	ResolvedRange r;
	// Entirely inside one generation: fine.
	CHECK(ring.resolveRange(ms(1200), ms(1800), r) && r.generation == 0);
	CHECK(ring.resolveRange(ms(2200), ms(2800), r) && r.generation == 1);
	// Straddling the seam must be refused, not spliced.
	CHECK(!ring.resolveRange(ms(1800), ms(2200), r));
}

static void test_ring_evicts_whole_gops()
{
	// Budget for roughly two GOPs of video (25 x 1000 bytes) plus audio.
	RingLimits lim;
	lim.maxBytes = 60'000;
	PacketRing ring(lim);

	fillGops(ring, 6, 25, 0);

	CHECK(ring.bytes() <= lim.maxBytes);
	CHECK(ring.evictedPackets() > 0);
	// Whatever survived must still start on a keyframe, or the oldest
	// seconds would be held but undecodable.
	CHECK(!ring.empty());
	CHECK(ring.at(0).kind == PacketKind::Video && ring.at(0).keyframe);

	int64_t oldestKey = -1;
	CHECK(ring.oldestKeyframeNs(oldestKey));
	CHECK(oldestKey == ring.oldestNs());

	// The evicted head must genuinely be gone.
	ResolvedRange r;
	CHECK(!ring.resolveRange(0, ms(100), r));
}

static void test_ring_evicts_by_duration()
{
	RingLimits lim;
	lim.maxDurationNs = ms(2000);
	PacketRing ring(lim);

	fillGops(ring, 6, 25, 0); // 6 s of footage into a 2 s window

	CHECK(ring.spanNs() <= lim.maxDurationNs);
	CHECK(ring.at(0).keyframe);
	CHECK(ring.newestNs() == ms(5960)); // last frame of the 6th GOP
}

static void test_ring_keeps_a_single_oversized_gop()
{
	// One GOP larger than the whole budget: dropping into it would leave an
	// undecodable ring, so the ring stays over budget on purpose.
	RingLimits lim;
	lim.maxBytes = 100;
	PacketRing ring(lim);
	fillGops(ring, 1, 25, 0);

	CHECK(!ring.empty());
	CHECK(ring.at(0).keyframe);
	CHECK(ring.bytes() > lim.maxBytes);
}

static void test_ring_handles_interleaved_audio()
{
	PacketRing ring;
	// Audio arriving slightly ahead of the video it belongs with: the newest
	// instant must come from presentation time, not from push order.
	ring.push(vpkt(ms(0), true));
	ring.push(apkt(ms(120)));
	ring.push(vpkt(ms(40), false));
	ring.push(vpkt(ms(80), false));

	CHECK(ring.newestNs() == ms(120));
	CHECK(ring.oldestNs() == ms(0));

	ResolvedRange r;
	CHECK(ring.resolveRange(ms(40), ms(80), r));
	CHECK(r.presentInNs == ms(40) && r.presentOutNs == ms(80));
	// The out-of-order audio packet still falls inside the span.
	CHECK(r.last >= 2);
}

// --- segment anchoring ------------------------------------------------------

// Ring of `n` video packets, 40 ms apart, with sizes that vary the way real
// encoded frames do.
static std::vector<AnchorSample> anchorRing(int n, int64_t startNs = 0)
{
	std::vector<AnchorSample> v;
	v.reserve((size_t)n);
	for (int i = 0; i < n; i++) {
		AnchorSample s;
		s.masterNs = startNs + (int64_t)i * ms(40);
		// Deterministic, but spread the way real encoded frames are: the
		// fingerprint is only as good as the variety of its sizes, and a
		// tight cyclic sequence would collide under the size tolerance in
		// a way genuine footage never does.
		uint64_t h = (uint64_t)i * 0x9E3779B97F4A7C15ull;
		h ^= h >> 30;
		h *= 0xBF58476D1CE4E5B9ull;
		h ^= h >> 27;
		s.size = 2000 + (uint32_t)(h % 60000u);
		v.push_back(s);
	}
	return v;
}

static std::vector<uint32_t> sizesFrom(const std::vector<AnchorSample> &ring,
				       size_t at, size_t count)
{
	std::vector<uint32_t> out;
	for (size_t i = at; i < ring.size() && out.size() < count; i++)
		out.push_back(ring[i].size);
	return out;
}

static void test_anchor_finds_exact_position()
{
	const auto ring = anchorRing(400);
	// A file whose first packets are the ones at ring index 137.
	const auto fileSizes = sizesFrom(ring, 137, kAnchorFingerprintLen + 4);

	int64_t anchor = -1;
	CHECK(findAnchor(ring, fileSizes, anchor) == AnchorResult::Found);
	CHECK(anchor == ring[137].masterNs);

	// Matching right at both ends must work too.
	const auto atStart = sizesFrom(ring, 0, kAnchorFingerprintLen);
	CHECK(findAnchor(ring, atStart, anchor) == AnchorResult::Found);
	CHECK(anchor == ring[0].masterNs);

	const auto atEnd = sizesFrom(ring, ring.size() - kAnchorFingerprintLen,
				     kAnchorFingerprintLen);
	CHECK(findAnchor(ring, atEnd, anchor) == AnchorResult::Found);
	CHECK(anchor == ring[ring.size() - kAnchorFingerprintLen].masterNs);
}

// A packet does not reach the file byte-identical: Annex B start codes become
// MP4 length prefixes, which measured as a steady +1 byte per packet against
// Branch Output. The fingerprint has to survive that without becoming vague.
static void test_anchor_tolerates_container_size_drift()
{
	const auto ring = anchorRing(300);
	auto fileSizes = sizesFrom(ring, 88, kAnchorFingerprintLen + 4);
	for (auto &s : fileSizes)
		s += 1;

	int64_t anchor = -1;
	CHECK(findAnchor(ring, fileSizes, anchor) == AnchorResult::Found);
	CHECK(anchor == ring[88].masterNs);

	// ...but drift beyond the tolerance is a different stream, not slack.
	auto wayOff = sizesFrom(ring, 88, kAnchorFingerprintLen + 4);
	for (auto &s : wayOff)
		s += 200;
	CHECK(findAnchor(ring, wayOff, anchor) == AnchorResult::NotFound);
}

static void test_anchor_refuses_when_absent()
{
	const auto ring = anchorRing(200);
	// A fingerprint that simply is not in this ring: the file's opening has
	// already been evicted, so the anchor is unknowable - say so.
	std::vector<uint32_t> foreign(kAnchorFingerprintLen, 123456u);

	int64_t anchor = -1;
	CHECK(findAnchor(ring, foreign, anchor) == AnchorResult::NotFound);
	CHECK(anchor == -1); // untouched
}

static void test_anchor_refuses_when_ambiguous()
{
	// Static content (a colour bar, a frozen camera) compresses to nearly
	// identical frames, so many positions tie. Guessing one would put the
	// replay minutes away from the marker; refusing is the only safe answer.
	std::vector<AnchorSample> flat;
	for (int i = 0; i < 100; i++)
		flat.push_back({(int64_t)i * ms(40), 2048u});

	std::vector<uint32_t> fileSizes(kAnchorFingerprintLen, 2048u);
	int64_t anchor = -1;
	CHECK(findAnchor(flat, fileSizes, anchor) == AnchorResult::Ambiguous);
}

static void test_anchor_needs_enough_evidence()
{
	const auto ring = anchorRing(50);
	int64_t anchor = -1;

	std::vector<uint32_t> tooShort = sizesFrom(ring, 0, 3);
	CHECK(findAnchor(ring, tooShort, anchor) == AnchorResult::TooFewSamples);

	const auto tinyRing = anchorRing(4);
	const auto enoughFile = sizesFrom(ring, 0, kAnchorFingerprintLen);
	CHECK(findAnchor(tinyRing, enoughFile, anchor) ==
	      AnchorResult::TooFewSamples);
}

// --- session clock ----------------------------------------------------------
//
// Monotonic time is what the engine measures with and what dies with the
// process; wall time is what survives into events.json / anchors.json. Every
// mark and every file anchor crosses this bridge twice (save, then load in a
// later run), so a lossy or lopsided conversion would move footage under the
// operator's markers - silently, and only after a restart.

static void test_session_clock_round_trips()
{
	// A plausible pair: ~2 days of uptime, wall time in 2026.
	const SessionEpoch e{172'800'000'000'000LL, 1'776'000'000'000'000'000LL};

	// The epoch instant itself maps to itself, both ways.
	CHECK(masterToWallNs(e, e.masterNs) == e.wallNs);
	CHECK(wallToMasterNs(e, e.wallNs) == e.masterNs);

	// An instant 90 s after the epoch is 90 s later on the other clock too:
	// the mapping is a pure offset, it must not scale or drift.
	const int64_t master = e.masterNs + 90LL * 1'000'000'000LL;
	const int64_t wall = masterToWallNs(e, master);
	CHECK(wall == e.wallNs + 90LL * 1'000'000'000LL);
	CHECK(wallToMasterNs(e, wall) == master);

	// Marks BEFORE the epoch happen for real: the epoch is seated at module
	// load, and the ring can hold packets stamped a hair earlier.
	const int64_t before = e.masterNs - 5LL * 1'000'000'000LL;
	CHECK(wallToMasterNs(e, masterToWallNs(e, before)) == before);

	// Exact round trip over a long session, one sample per second.
	for (int64_t s = 0; s < 6 * 3600; s += 137) {
		const int64_t m = e.masterNs + s * 1'000'000'000LL;
		CHECK(wallToMasterNs(e, masterToWallNs(e, m)) == m);
	}
}

// The point of the whole exercise: a mark written yesterday, read back in a
// process whose monotonic clock counts from a completely different origin,
// must land on the same real instant - i.e. the same distance from that
// session's own epoch.
static void test_session_clock_across_sessions()
{
	const SessionEpoch yesterday{172'800'000'000'000LL,
				     1'776'000'000'000'000'000LL};
	// Next run: machine rebooted (monotonic near zero), 26 hours later.
	const SessionEpoch today{3'600'000'000'000LL,
				 1'776'000'000'000'000'000LL +
					 26LL * 3600 * 1'000'000'000LL};

	// A mark 42 s into yesterday's session.
	const int64_t markedMaster = yesterday.masterNs + 42LL * 1'000'000'000LL;
	const int64_t persisted = masterToWallNs(yesterday, markedMaster);

	// Today it resolves to a master value that is meaningless in isolation
	// but exactly right relative to today's epoch...
	const int64_t reloaded = wallToMasterNs(today, persisted);
	CHECK(reloaded != markedMaster); // different origin: it MUST differ
	CHECK(reloaded - today.masterNs == persisted - today.wallNs);

	// ...and a segment anchored at the same instant lands on the same value,
	// which is the property resolve() depends on: mark and footage still meet.
	const int64_t anchorWall = persisted;
	CHECK(wallToMasterNs(today, anchorWall) == reloaded);

	// A mark 10 s later stays 10 s later.
	const int64_t later = masterToWallNs(yesterday,
					     markedMaster + 10LL * 1'000'000'000LL);
	CHECK(wallToMasterNs(today, later) - reloaded == 10LL * 1'000'000'000LL);
}

// An unseated epoch (the standalone/no-session case) must be the identity, or
// a store with no epoch would quietly rewrite every mark it saves.
static void test_session_clock_identity_when_unseated()
{
	const SessionEpoch none{};
	CHECK(masterToWallNs(none, 0) == 0);
	CHECK(masterToWallNs(none, 1'234'567'890LL) == 1'234'567'890LL);
	CHECK(wallToMasterNs(none, 1'234'567'890LL) == 1'234'567'890LL);
}

// Wall-clock nanoseconds are already ~1.8e18, a fifth of the int64 range, so
// the order of operations is load-bearing: the difference of the two same-clock
// terms has to be taken FIRST. Adding the offset to the wall value before
// subtracting would blow past INT64_MAX on values this size.
static void test_session_clock_does_not_overflow()
{
	const int64_t kMax = std::numeric_limits<int64_t>::max();

	// Year-2200-ish wall time with a very large monotonic value.
	const SessionEpoch e{7'000'000'000'000'000'000LL,
			     7'200'000'000'000'000'000LL};

	const int64_t master = e.masterNs + 3600LL * 1'000'000'000LL;
	const int64_t wall = masterToWallNs(e, master);
	CHECK(wall == e.wallNs + 3600LL * 1'000'000'000LL);
	CHECK(wall > 0 && wall < kMax); // no wrap into negative territory
	CHECK(wallToMasterNs(e, wall) == master);

	// The naive grouping (epochWall - epochMaster + masterNs) would compute
	// 7.2e18 + 7.0e18 here; check the real helpers stay well inside range.
	CHECK(masterToWallNs(e, e.masterNs) == e.wallNs);
	CHECK(wallToMasterNs(e, e.wallNs) == e.masterNs);

	// The reverse skew (huge monotonic, small wall) must be just as safe.
	const SessionEpoch skewed{8'000'000'000'000'000'000LL, 1'000'000'000LL};
	CHECK(masterToWallNs(skewed, skewed.masterNs + 1000) == 1'000'001'000LL);
	CHECK(wallToMasterNs(skewed, 1'000'001'000LL) ==
	      skewed.masterNs + 1000);
}

// --- the position bar's axis (timeline-map.hpp) -----------------------------
// The operator records, stops, waits, records again. What the bar must show is
// the MATERIAL, joined end to end — not the afternoon, with a hole in it.

static void test_timeline_joins_two_takes()
{
	TimelineMap m;
	// Two minutes, a minute of nothing, two more minutes.
	m.setSpans({{0, ms(120000)}, {ms(180000), ms(300000)}});
	CHECK(m.spanCount() == 2);
	// Four minutes of footage, not five of session.
	CHECK(m.totalNs() == ms(240000));

	// The join: the last instant of take 1 and the first of take 2 are
	// neighbours on the bar.
	CHECK(m.fractionOf(ms(120000)) == 0.5);
	CHECK(m.fractionOf(ms(180000)) == 0.5);
	// Halfway along the bar is the start of take 2, not the middle of the
	// pause — which is the whole point.
	CHECK(m.instantAt(0.5) == ms(180000));
	CHECK(m.instantAt(0.25) == ms(60000));
	CHECK(m.instantAt(0.75) == ms(240000));

	// Round trip on instants that footage covers.
	for (int64_t t : {ms(1), ms(60000), ms(119999), ms(180001), ms(299999)})
		CHECK(m.instantAt(m.fractionOf(t)) == t);
}

static void test_timeline_gap_has_no_position_of_its_own()
{
	TimelineMap m;
	m.setSpans({{0, ms(120000)}, {ms(180000), ms(300000)}});
	CHECK(m.covers(ms(60000)));
	CHECK(!m.covers(ms(150000))); // inside the pause
	CHECK(!m.covers(ms(400000))); // past the end
	// It still has to answer, and it answers with the join rather than a
	// position inside footage that does not exist.
	CHECK(m.fractionOf(ms(150000)) == 0.5);
	// Before the first frame and after the last.
	CHECK(m.fractionOf(-ms(5000)) == 0.0);
	CHECK(m.fractionOf(ms(999999)) == 1.0);
}

static void test_timeline_merges_file_splits()
{
	TimelineMap m;
	// A 20-minute split: the next file starts a few ms after the previous
	// one ends. That is one recording, and a seam drawn there would be a lie.
	m.setSpans({{0, ms(1200000)}, {ms(1200040), ms(2400000)}});
	CHECK(m.spanCount() == 1);
	CHECK(m.totalNs() == ms(2400000));

	// Overlap (two cameras of the same take) collapses too.
	TimelineMap two;
	two.setSpans({{ms(1000), ms(61000)}, {ms(1200), ms(61200)}});
	CHECK(two.spanCount() == 1);
	CHECK(two.totalNs() == ms(60200));
}

static void test_timeline_handles_negative_instants()
{
	// Footage recorded before the machine's last boot maps to negative
	// master instants (see kNoInstant). Nothing here may treat that as
	// "empty".
	TimelineMap m;
	m.setSpans({{-ms(300000), -ms(180000)}, {-ms(60000), ms(60000)}});
	CHECK(m.spanCount() == 2);
	CHECK(m.totalNs() == ms(240000));
	CHECK(m.fractionOf(-ms(240000)) == 0.25);
	CHECK(m.instantAt(0.0) == -ms(300000));
	CHECK(m.instantAt(1.0) == ms(60000));
}

static void test_timeline_empty_is_empty()
{
	TimelineMap m;
	CHECK(m.empty());
	CHECK(m.totalNs() == 0);
	CHECK(m.fractionOf(1234) < 0.0); // "there is no bar", not "position 0"
	CHECK(!m.covers(1234));
	// Spans that say nothing are dropped rather than drawn as zero-width.
	m.setSpans({{500, 500}, {900, 100}});
	CHECK(m.empty());
	CHECK(m.spanCount() == 0);
}

// --- reverse: the schedule that plays a range backwards ---------------------
//
// Reverse is the one feature whose correctness is arithmetic and whose failure
// mode is invisible: a schedule that loses the middle slice of a range still
// plays, still runs backwards, and simply skips a third of a second. So what is
// pinned here is coverage (every wanted frame is shown exactly once), order
// (strictly descending across GOP boundaries), and the memory bound (no pass
// larger than the cache, whatever the GOP).

// 3 GOPs of 10 frames at 100 ms, keyframes at 0 / 1000 / 2000 ms.
static std::vector<LivePacket> reverseClip()
{
	std::vector<LivePacket> clip;
	for (int i = 0; i < 30; i++)
		clip.push_back(vpkt(ms(i * 100), i % 10 == 0));
	return clip;
}

// Every instant the plan will show, in the order it will show them.
static std::vector<int64_t>
reverseOrder(const std::vector<reverse_plan::Chunk> &chunks,
	     const std::vector<LivePacket> &clip)
{
	std::vector<int64_t> shown;
	for (const auto &c : chunks) {
		// What one decode pass keeps: the frames of its GOP inside the
		// keep window, shown newest-first.
		std::vector<int64_t> kept;
		for (size_t i = c.decodeStart; i < c.decodeEnd && i < clip.size();
		     i++) {
			const LivePacket &p = clip[i];
			if (p.kind != PacketKind::Video)
				continue;
			if (p.masterNs >= c.keepFromNs && p.masterNs <= c.keepToNs)
				kept.push_back(p.masterNs);
		}
		std::sort(kept.begin(), kept.end());
		for (size_t i = kept.size(); i-- > 0;)
			shown.push_back(kept[i]);
	}
	return shown;
}

static void test_reverse_shows_every_frame_once_descending()
{
	const auto clip = reverseClip();
	reverse_plan::Budget b;
	b.frameBytes = 1000;
	b.maxBytes = 1'000'000; // room for the whole GOP: one pass each
	const auto chunks = reverse_plan::plan(clip, 0, ms(2900), b);

	CHECK(chunks.size() == 3); // one pass per GOP
	CHECK(reverse_plan::plannedFrames(chunks) == 30);

	const auto shown = reverseOrder(chunks, clip);
	CHECK(shown.size() == 30);
	// Strictly descending, start to finish - which is also what proves the
	// GOPs themselves are walked from the last to the first. A plan that got
	// the GOP order wrong would still be descending WITHIN each pass.
	bool descending = true;
	for (size_t i = 1; i < shown.size(); i++)
		if (shown[i] >= shown[i - 1])
			descending = false;
	CHECK(descending);
	CHECK(shown.front() == ms(2900));
	CHECK(shown.back() == 0);
}

static void test_reverse_respects_the_requested_range()
{
	const auto clip = reverseClip();
	reverse_plan::Budget b;
	b.frameBytes = 1000;
	b.maxBytes = 1'000'000;
	// Straddles two GOPs, and starts and ends in the middle of them.
	const auto chunks = reverse_plan::plan(clip, ms(850), ms(1250), b);

	const auto shown = reverseOrder(chunks, clip);
	// 850..1250 with a frame every 100 ms is 900, 1000, 1100, 1200: the
	// endpoints are inclusive but they are not frames, and nothing outside
	// the range is shown to round the count up.
	CHECK(shown.size() == 4);
	CHECK(shown.front() == ms(1200));
	CHECK(shown.back() == ms(900));
	// The frames before the IN are still DECODED (they are the reference
	// state of that GOP), so the pass must start at the keyframe and not at
	// the IN - the difference between a correct first picture and a smear.
	CHECK(chunks.back().decodeStart == 0);
	CHECK(chunks.back().keepFromNs == ms(900));
}

static void test_reverse_splits_a_gop_too_big_for_the_cache()
{
	const auto clip = reverseClip();
	reverse_plan::Budget b;
	b.frameBytes = 1000;
	b.maxBytes = 4000; // four pictures per pass
	CHECK(reverse_plan::framesPerChunk(b) == 4);

	const auto chunks = reverse_plan::plan(clip, 0, ms(2900), b);
	// 10 frames per GOP in slices of 4 = 4+4+2, three passes per GOP.
	CHECK(chunks.size() == 9);
	for (const auto &c : chunks)
		CHECK(c.frames <= 4);
	// Re-decoded, not truncated: nothing is lost, it just costs more passes.
	CHECK(reverse_plan::plannedFrames(chunks) == 30);

	const auto shown = reverseOrder(chunks, clip);
	CHECK(shown.size() == 30);
	bool descending = true;
	for (size_t i = 1; i < shown.size(); i++)
		if (shown[i] >= shown[i - 1])
			descending = false;
	CHECK(descending);
	// Every pass of a split GOP starts from that GOP's keyframe, because a
	// slice in the middle still needs the frames before it decoded.
	CHECK(chunks[0].decodeStart == 20);
	CHECK(chunks[1].decodeStart == 20);
	CHECK(chunks[2].decodeStart == 20);
}

static void test_reverse_budget_edges()
{
	// A cache too small for even one picture still shows one at a time:
	// degraded, not refused.
	reverse_plan::Budget tiny;
	tiny.frameBytes = 3'110'400; // 1080p NV12
	tiny.maxBytes = 1000;
	CHECK(reverse_plan::framesPerChunk(tiny) == 1);

	// No budget at all = unbounded, which is what the pure planner means by
	// zero. The channel always passes one.
	reverse_plan::Budget none;
	CHECK(reverse_plan::framesPerChunk(none) == 0);

	// The real numbers: half of the channel's 160 MiB budget, 1080p NV12 =
	// 3.1 MB a picture, so a 1-second GOP at 30 fps fits in one pass. This is
	// the case the whole design is sized for, and it must not silently become
	// two passes.
	reverse_plan::Budget real;
	real.frameBytes = 3'110'400;
	real.maxBytes = 80u * 1024u * 1024u;
	CHECK(reverse_plan::framesPerChunk(real) >= 25);

	// Nothing in the range, and a backwards range: an empty plan, not a
	// crash and not a plan that shows something.
	const auto clip = reverseClip();
	reverse_plan::Budget b;
	b.frameBytes = 1000;
	b.maxBytes = 1'000'000;
	CHECK(reverse_plan::plan(clip, ms(9000), ms(9500), b).empty());
	CHECK(reverse_plan::plan(clip, ms(500), ms(100), b).empty());
	CHECK(reverse_plan::plan({}, 0, ms(100), b).empty());

	// A span with no keyframe at all has no reverse: there is nothing to
	// decode from, and inventing a start would show predicted frames against
	// references that were never decoded.
	std::vector<LivePacket> noKey;
	for (int i = 0; i < 5; i++)
		noKey.push_back(vpkt(ms(i * 100), false));
	CHECK(reverse_plan::plan(noKey, 0, ms(400), b).empty());
}

static void test_reverse_ignores_audio_packets()
{
	// Audio interleaves with video in the ring, and the picture cache must
	// not count it: an audio packet inside the keep window is not a frame,
	// and a pass sized as though it were would hold fewer pictures than the
	// budget allows.
	std::vector<LivePacket> clip;
	for (int i = 0; i < 10; i++) {
		clip.push_back(vpkt(ms(i * 100), i == 0));
		clip.push_back(apkt(ms(i * 100) + ms(10)));
	}
	reverse_plan::Budget b;
	b.frameBytes = 1000;
	b.maxBytes = 1'000'000;
	const auto chunks = reverse_plan::plan(clip, 0, ms(900), b);
	CHECK(chunks.size() == 1);
	CHECK(chunks[0].frames == 10);
	// The pass spans the whole GOP including its audio, which the decoder
	// ignores - trimming it would be an optimisation with a demuxing bug
	// waiting inside it.
	CHECK(chunks[0].decodeEnd == clip.size());
	CHECK(reverseOrder(chunks, clip).size() == 10);
}

// --- a review must play THROUGH the seam between two takes ------------------
// Recorded five minutes, stopped, talked for a minute, recorded five more: the
// bar joins them, but in master time they are a minute apart. "Ten seconds of
// footage from here" therefore cannot be one range — it would include the gap,
// which is not footage, and the engine refuses (rightly) rather than invent it.
// So it comes back as the pieces that hold it.
static void test_review_crosses_a_junction()
{
	TimelineMap m;
	// take 1: 0..5 s, then a 60 s gap, then take 2: 65..80 s
	m.setSpans({{0, ms(5000)}, {ms(65000), ms(80000)}});

	// Well inside take 1: one piece, exactly as long as asked.
	auto r = m.spansFrom(ms(1000), ms(2000));
	CHECK(r.size() == 1);
	CHECK(r[0].startNs == ms(1000));
	CHECK(r[0].endNs == ms(3000));

	// TWO seconds before the end of take 1, asking for ten: 2 s at the end of
	// the first take and 8 s at the START of the second — not 10 s of wall time,
	// which would land in the middle of the gap and play nothing.
	r = m.spansFrom(ms(3000), ms(10000));
	CHECK(r.size() == 2);
	CHECK(r[0].startNs == ms(3000));
	CHECK(r[0].endNs == ms(5000));
	CHECK(r[1].startNs == ms(65000));
	CHECK(r[1].endNs == ms(73000));
	// Every piece is footage, and they add up to what was asked for.
	int64_t total = 0;
	for (const auto &s : r)
		total += s.lengthNs();
	CHECK(total == ms(10000));

	// More than there is: give what there is, and stop.
	r = m.spansFrom(ms(4000), ms(60000));
	total = 0;
	for (const auto &s : r)
		total += s.lengthNs();
	CHECK(total == ms(1000) + ms(15000)); // 1 s left of take 1 + all of take 2

	// An instant INSIDE the gap belongs to no take: start where the footage is.
	r = m.spansFrom(ms(30000), ms(2000));
	CHECK(r.size() == 1);
	CHECK(r[0].startNs == ms(65000));
	CHECK(r[0].endNs == ms(67000));

	// Past the end, nothing asked for, and no map at all: empty, not a piece of
	// something that does not exist.
	CHECK(m.spansFrom(ms(99000), ms(2000)).empty());
	CHECK(m.spansFrom(ms(1000), 0).empty());
	CHECK(TimelineMap().spansFrom(ms(1000), ms(2000)).empty());
}

// ---------------------------------------------------------------------------
// AudioStretch — slow motion with sound, at the right pitch.
//
// Three properties, and each of them is a way the feature can be wrong while
// looking right: the wrong LENGTH drifts the sound out from under the picture, a
// changed PITCH is the bug the whole algorithm exists to avoid, and touching the
// 1x path would colour the case that is on air most of the time.
// ---------------------------------------------------------------------------

// A sine at `hz`, planar, `channels` identical channels.
static std::vector<std::vector<float>> makeTone(uint32_t rate, uint32_t channels,
					       double hz, uint32_t frames)
{
	std::vector<std::vector<float>> pcm(channels,
					    std::vector<float>(frames, 0.0f));
	for (uint32_t i = 0; i < frames; i++) {
		const float v = (float)std::sin(2.0 * 3.14159265358979 * hz *
						(double)i / (double)rate);
		for (uint32_t c = 0; c < channels; c++)
			pcm[c][i] = v;
	}
	return pcm;
}

// Rising-edge zero crossings per second: the cheapest honest pitch meter for a
// tone, and it needs no FFT in a test that must build everywhere.
static double dominantHz(const std::vector<float> &x, uint32_t rate)
{
	if (x.size() < 2)
		return 0.0;
	int crossings = 0;
	for (size_t i = 1; i < x.size(); i++)
		if (x[i - 1] <= 0.0f && x[i] > 0.0f)
			crossings++;
	return (double)crossings * (double)rate / (double)x.size();
}

// Run `pcm` through a configured stretcher and collect everything it produces.
static std::vector<std::vector<float>>
stretchAll(AudioStretch &st, const std::vector<std::vector<float>> &pcm,
	   uint32_t chunk)
{
	const uint32_t channels = (uint32_t)pcm.size();
	const uint32_t frames = (uint32_t)pcm[0].size();
	std::vector<std::vector<float>> out(channels);
	std::vector<std::vector<float>> scratch(channels,
						std::vector<float>(1 << 15, 0.0f));
	std::vector<float *> scratchPtr(channels);
	for (uint32_t c = 0; c < channels; c++)
		scratchPtr[c] = scratch[c].data();

	const auto drain = [&]() {
		for (;;) {
			const uint32_t got =
				st.take(scratchPtr.data(), (uint32_t)(1 << 15));
			if (got == 0)
				return;
			for (uint32_t c = 0; c < channels; c++)
				out[c].insert(out[c].end(), scratch[c].begin(),
					      scratch[c].begin() + got);
		}
	};

	std::vector<const float *> inPtr(channels);
	for (uint32_t at = 0; at < frames; at += chunk) {
		const uint32_t n = std::min(chunk, frames - at);
		for (uint32_t c = 0; c < channels; c++)
			inPtr[c] = pcm[c].data() + at;
		st.push(inPtr.data(), n);
		drain();
	}
	st.flush();
	drain();
	return out;
}

static void test_stretch_length_follows_the_speed()
{
	const uint32_t rate = 48000;
	const auto pcm = makeTone(rate, 2, 440.0, rate); // one second

	// Half speed is twice as long, double speed is half as long. The tolerance
	// is one block (40 ms): the block is the unit this algorithm works in, and
	// anything looser would let a real drift through.
	for (double speed : {0.25, 0.5, 0.75, 2.0}) {
		AudioStretch st;
		CHECK(st.configure(rate, 2, speed));
		const auto out = stretchAll(st, pcm, 1024);
		CHECK(out.size() == 2);
		const double want = (double)rate / speed;
		const double got = (double)out[0].size();
		if (std::fabs(got - want) >= 0.04 * (double)rate)
			std::printf("  speed %.2f: want %.0f got %.0f\n", speed,
				    want, got);
		CHECK(std::fabs(got - want) < 0.04 * (double)rate);
		// Both channels, always the same length: a stereo pair that drifts
		// apart is worse than silence.
		CHECK(out[0].size() == out[1].size());
	}
}

static void test_stretch_keeps_the_pitch()
{
	const uint32_t rate = 48000;
	const double hz = 440.0;
	const auto pcm = makeTone(rate, 1, hz, rate * 2);

	// THE POINT OF THE WHOLE FILE. Resampling would give 220 Hz at half speed;
	// a listener hears that instantly, which is why the replay was mute instead.
	for (double speed : {0.5, 2.0}) {
		AudioStretch st;
		CHECK(st.configure(rate, 1, speed));
		const auto out = stretchAll(st, pcm, 4096);
		CHECK(!out[0].empty());
		const double got = dominantHz(out[0], rate);
		CHECK(std::fabs(got - hz) < 20.0);
	}
}

static void test_stretch_leaves_1x_alone()
{
	const uint32_t rate = 48000;
	const auto pcm = makeTone(rate, 2, 1000.0, 4096);

	AudioStretch st;
	CHECK(st.configure(rate, 2, 1.0));
	CHECK(st.passthrough());
	const auto out = stretchAll(st, pcm, 512);
	CHECK(out[0].size() == pcm[0].size());
	// Sample for sample, not "close enough": at 1x nothing may be laid down,
	// faded or searched for.
	bool identical = true;
	for (size_t i = 0; i < pcm[0].size(); i++)
		if (out[0][i] != pcm[0][i] || out[1][i] != pcm[1][i])
			identical = false;
	CHECK(identical);

	// ...and what it refuses: no channels, an impossible rate, a speed outside
	// what the engine itself will play. A caller that gets false plays the clip
	// silent, which is the honest answer and the old behaviour.
	AudioStretch bad;
	CHECK(!bad.configure(rate, 0, 0.5));
	CHECK(!bad.configure(100, 2, 0.5));
	CHECK(!bad.configure(rate, 2, 0.001));
	CHECK(!bad.configure(rate, 2, 9.0));
	CHECK(!bad.configured());
}

int main()
{
	test_rescale();
	test_timeline_rejects_malformed();
	test_timeline_first_packet_seats_offset();
	test_timeline_cross_angle_equality();
	test_timeline_discontinuity();

	test_ring_resolves_from_previous_keyframe();
	test_ring_refuses_rather_than_clamps();
	test_ring_refuses_without_keyframe();
	test_ring_refuses_across_discontinuity();
	test_ring_evicts_whole_gops();
	test_ring_evicts_by_duration();
	test_ring_keeps_a_single_oversized_gop();
	test_ring_handles_interleaved_audio();

	test_anchor_finds_exact_position();
	test_anchor_tolerates_container_size_drift();
	test_anchor_refuses_when_absent();
	test_anchor_refuses_when_ambiguous();
	test_anchor_needs_enough_evidence();

	test_session_clock_round_trips();
	test_session_clock_across_sessions();
	test_session_clock_identity_when_unseated();
	test_session_clock_does_not_overflow();

	test_reverse_shows_every_frame_once_descending();
	test_reverse_respects_the_requested_range();
	test_reverse_splits_a_gop_too_big_for_the_cache();
	test_reverse_budget_edges();
	test_reverse_ignores_audio_packets();

	test_timeline_joins_two_takes();
	test_timeline_gap_has_no_position_of_its_own();
	test_timeline_merges_file_splits();
	test_timeline_handles_negative_instants();
	test_timeline_empty_is_empty();
	test_review_crosses_a_junction();

	test_stretch_length_follows_the_speed();
	test_stretch_keeps_the_pitch();
	test_stretch_leaves_1x_alone();

	if (g_fail == 0)
		std::printf("OK: all packet-ring / master-timeline tests passed\n");
	return g_fail == 0 ? 0 : 1;
}
