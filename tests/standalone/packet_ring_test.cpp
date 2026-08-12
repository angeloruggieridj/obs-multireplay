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

#include "master-timeline.hpp"
#include "packet-ring.hpp"
#include "segment-anchor.hpp"

#include <cstdint>
#include <cstdio>

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

	if (g_fail == 0)
		std::printf("OK: all packet-ring / master-timeline tests passed\n");
	return g_fail == 0 ? 0 : 1;
}
