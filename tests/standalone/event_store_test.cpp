/*
obs-multireplay — standalone logic tests for the REAL EventStore.
SPDX-License-Identifier: GPL-2.0-or-later

Compiles src/event-store.cpp against the fake obs headers (tests/fakeobs) and
no-op obs stubs (obs_stub.cpp). No session folder is set, so save()/load() are
inert and we exercise pure in-memory event logic: mark in/out, presets, edits,
clamping, last-event selection, descriptions and YouTube chapter formatting.
*/

#include "event-store.hpp"

#include <cstdio>
#include <string>

using namespace multireplay;

static int g_fail = 0;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__,  \
				    #cond);                                 \
			++g_fail;                                           \
		}                                                           \
	} while (0)

static constexpr int64_t S = 1000000000LL; // 1 second in ns

static EventStore &store()
{
	return EventStore::instance();
}

static void reset()
{
	store().clearAll();
	store().selectList(1);
	store().setLiveMode(true);
	store().setRollNs(0, 0); // pre/post roll off unless a test asks for it
}

static void test_mark_in_out()
{
	reset();
	int id = store().markIn(5 * S, /*angle0*/ 2); // CAM3
	CHECK(id == 1);
	ReplayEvent ev;
	CHECK(store().get(id, ev));
	CHECK(ev.tInNs == 5 * S);
	CHECK(ev.tOutNs == -1); // open
	CHECK(ev.angles[2].enabled);
	CHECK(!ev.angles[0].enabled);
	CHECK(ev.createdMode == "live");

	// markOut closes the open event with OUT >= IN+1.
	CHECK(store().markOut(9 * S));
	CHECK(store().get(id, ev));
	CHECK(ev.tOutNs == 9 * S);

	// No open event left → markOut returns false.
	CHECK(!store().markOut(10 * S));

	// markOut clamps OUT to at least IN+1.
	int id2 = store().markIn(100 * S, 0);
	CHECK(store().markOut(50 * S)); // before IN
	CHECK(store().get(id2, ev));
	CHECK(ev.tOutNs == 100 * S + 1);

	// Negative IN clamps to 0.
	int id3 = store().markIn(-7 * S, 0);
	CHECK(store().get(id3, ev));
	CHECK(ev.tInNs == 0);
}

static void test_mark_in_out_preset()
{
	reset();
	store().setLiveMode(false);
	int id = store().markInOut(30 * S, 10, /*angle0*/ 1); // last 10s, CAM2
	ReplayEvent ev;
	CHECK(store().get(id, ev));
	CHECK(ev.tOutNs == 30 * S);
	CHECK(ev.tInNs == 20 * S);
	CHECK(ev.angles[1].enabled);
	CHECK(ev.createdMode == "recorded");

	// Preset window larger than NOW clamps IN to 0.
	int id2 = store().markInOut(3 * S, 20, 0);
	CHECK(store().get(id2, ev));
	CHECK(ev.tInNs == 0);
	CHECK(ev.tOutNs == 3 * S);
}

static void test_mark_cancel()
{
	reset();
	int id = store().markIn(5 * S, 0);
	CHECK(store().markCancel()); // removes the open event
	ReplayEvent ev;
	CHECK(!store().get(id, ev));
	CHECK(!store().markCancel()); // nothing open now
}

static void test_angle_edits_bounds()
{
	reset();
	int id = store().markIn(0, 0);

	// 1-based angle bounds: 0 and 9 are invalid.
	CHECK(!store().toggleAngle(id, 0));
	CHECK(!store().toggleAngle(id, kEventAngles + 1));
	CHECK(!store().setAngle(id, 0, true));
	CHECK(!store().setAngleNote(id, 9, "x"));
	CHECK(!store().setAngleSpeed(id, 0, 0.5));

	// Valid toggles.
	CHECK(store().setAngle(id, 3, true));
	ReplayEvent ev;
	CHECK(store().get(id, ev));
	CHECK(ev.angles[2].enabled);
	CHECK(store().toggleAngle(id, 3)); // now off
	CHECK(store().get(id, ev));
	CHECK(!ev.angles[2].enabled);

	// Unknown id → false.
	CHECK(!store().setAngle(999, 1, true));
}

static void test_speed_clamping()
{
	reset();
	int id = store().markIn(0, 0);
	ReplayEvent ev;

	CHECK(store().setSpeed(id, 2.0)); // over → 1.0
	CHECK(store().get(id, ev));
	CHECK(ev.speed == 1.0);

	CHECK(store().setSpeed(id, 0.0)); // 0 is invalid playback → 0.01 floor
	CHECK(store().get(id, ev));
	CHECK(ev.speed == 0.01);

	CHECK(store().setSpeed(id, -1.0)); // sentinel "inherit"
	CHECK(store().get(id, ev));
	CHECK(ev.speed == -1.0);

	CHECK(store().setAngleSpeed(id, 1, 5.0)); // over → 1.0
	CHECK(store().get(id, ev));
	CHECK(ev.angles[0].speed == 1.0);

	CHECK(store().setAngleSpeed(id, 1, -2.0)); // any negative → -1 sentinel
	CHECK(store().get(id, ev));
	CHECK(ev.angles[0].speed == -1.0);
}

static void test_move_points()
{
	reset();
	int id = store().markIn(10 * S, 0);
	store().markOut(20 * S);
	ReplayEvent ev;

	// Move IN forward 2s.
	CHECK(store().movePoint(id, /*inPoint*/ true, 2 * S));
	CHECK(store().get(id, ev));
	CHECK(ev.tInNs == 12 * S);

	// Move IN past OUT → clamps to OUT-1.
	CHECK(store().movePoint(id, true, 100 * S));
	CHECK(store().get(id, ev));
	CHECK(ev.tInNs == 20 * S - 1);

	// Move IN far negative → clamps to 0.
	CHECK(store().movePoint(id, true, -1000 * S));
	CHECK(store().get(id, ev));
	CHECK(ev.tInNs == 0);

	// Move OUT earlier than IN+1 → clamps to IN+1.
	CHECK(store().movePoint(id, /*inPoint*/ false, -1000 * S));
	CHECK(store().get(id, ev));
	CHECK(ev.tOutNs == ev.tInNs + 1);
}

static void test_move_to_list_and_select()
{
	reset();
	int id = store().markIn(0, 0);
	CHECK(!store().moveToList(id, 0));               // out of range
	CHECK(!store().moveToList(id, kEventLists + 1)); // out of range
	CHECK(store().moveToList(id, 5));
	ReplayEvent ev;
	CHECK(store().get(id, ev));
	CHECK(ev.list == 5);

	// selectList ignores out-of-range, keeps previous.
	store().selectList(7);
	CHECK(store().selectedList() == 7);
	store().selectList(0);
	CHECK(store().selectedList() == 7);
	store().selectList(kEventLists + 5);
	CHECK(store().selectedList() == 7);
}

static void test_duplicate_and_last()
{
	reset();
	// Three closed events with different IN times across creation order.
	int a = store().markIn(100 * S, 0);
	store().markOut(110 * S);
	int b = store().markIn(300 * S, 0);
	store().markOut(310 * S);
	int c = store().markIn(200 * S, 0);
	store().markOut(210 * S);

	// last = most recent by IN time (the reference controller), i.e. event b at 300s.
	CHECK(store().lastEventId() == b);

	// duplicate gets a fresh id but copies the range.
	int dup = store().duplicate(a);
	CHECK(dup != a && dup != b && dup != c && dup != 0);
	ReplayEvent ev;
	CHECK(store().get(dup, ev));
	CHECK(ev.tInNs == 100 * S);
	CHECK(ev.tOutNs == 110 * S);

	// Open events do not count as "last".
	store().markIn(9999 * S, 0); // open, highest IN
	CHECK(store().lastEventId() == b);
}

static void test_description()
{
	reset();
	int id = store().markIn(0, 0);
	CHECK(store().description(id).empty());

	store().setAngleNote(id, 3, "angle3 note");
	CHECK(store().description(id) == "angle3 note"); // first non-empty

	store().setDescription(id, "global"); // writes every angle
	ReplayEvent ev;
	CHECK(store().get(id, ev));
	for (const auto &an : ev.angles)
		CHECK(an.note == "global");
	CHECK(store().description(id) == "global");
}

static void test_chapters_text()
{
	reset();
	// Event 1: 0s, with a note.
	int e1 = store().markIn(0, 0);
	store().markOut(5 * S);
	store().setDescription(e1, "Kickoff");

	// Event 2: 1h 1m 1s, no note → "Clip <id>".
	int e2 = store().markIn(3661 * S, 0);
	store().markOut(3665 * S);

	// Event 3: open → excluded from chapters.
	store().markIn(7200 * S, 0);

	std::string txt = store().chaptersText(1);
	std::string expected =
		std::string("0:00 Kickoff\n") + "1:01:01 Clip " +
		std::to_string(e2) + "\n";
	if (txt != expected)
		std::printf("chapters got:\n%s---\nexpected:\n%s---\n",
			    txt.c_str(), expected.c_str());
	CHECK(txt == expected);

	// A 65s event formats as M:SS without an hour field.
	reset();
	int e = store().markIn(65 * S, 0);
	store().markOut(70 * S);
	store().setDescription(e, "Goal");
	CHECK(store().chaptersText(1) == "1:05 Goal\n");
}

// the reference controller pre/post roll: seconds added to the start and the end of every event as
// it is marked. The stored in/out are the padded ones — what the list shows is
// what will play.
static void test_pre_post_roll()
{
	reset();
	store().setRollNs(2 * S, 3 * S);

	// Mark In backs up by the pre-roll, Mark Out runs on by the post-roll.
	int id = store().markIn(50 * S, 0);
	ReplayEvent ev;
	CHECK(store().get(id, ev));
	CHECK(ev.tInNs == 48 * S);
	CHECK(store().markOut(60 * S));
	CHECK(store().get(id, ev));
	CHECK(ev.tOutNs == 63 * S);

	// The preset window is the seconds asked for, padded on both sides.
	int id2 = store().markInOut(100 * S, 10, 0);
	CHECK(store().get(id2, ev));
	CHECK(ev.tInNs == 88 * S);  // 100 - 10 - 2
	CHECK(ev.tOutNs == 103 * S); // 100 + 3

	// A pre-roll longer than the timeline still clamps at 0, and OUT stays
	// strictly after IN.
	store().setRollNs(500 * S, 0);
	int id3 = store().markIn(5 * S, 0);
	CHECK(store().get(id3, ev));
	CHECK(ev.tInNs == 0);

	// Negative rolls are not a way to shorten an event.
	store().setRollNs(-4 * S, -4 * S);
	CHECK(store().preRollNs() == 0);
	CHECK(store().postRollNs() == 0);

	// Off by default = the old behaviour, exactly.
	reset();
	int id4 = store().markIn(7 * S, 0);
	CHECK(store().get(id4, ev));
	CHECK(ev.tInNs == 7 * S);
}

// the reference controller: an event with no speed of its own inherits the previous event's.
static void test_speed_inheritance()
{
	reset();
	int a = store().markIn(10 * S, 0);
	store().markOut(11 * S);
	int b = store().markIn(20 * S, 0);
	store().markOut(21 * S);
	int c = store().markIn(30 * S, 0);
	store().markOut(31 * S);

	// Nothing set anywhere → "no answer", so the caller's default wins.
	CHECK(store().resolvedSpeed(a) < 0);
	CHECK(store().resolvedSpeed(c) < 0);

	// b at 50%: b and everything after it inherit, a (before it) does not.
	CHECK(store().setSpeed(b, 0.5));
	CHECK(store().resolvedSpeed(a) < 0);
	CHECK(store().resolvedSpeed(b) == 0.5);
	CHECK(store().resolvedSpeed(c) == 0.5);

	// An explicit value on c wins over the inherited one.
	CHECK(store().setSpeed(c, 0.25));
	CHECK(store().resolvedSpeed(c) == 0.25);

	// Inheritance never crosses lists: a new list starts from nothing.
	store().selectList(2);
	int d = store().markIn(40 * S, 0);
	store().markOut(41 * S);
	CHECK(store().resolvedSpeed(d) < 0);
	store().selectList(1);

	// An unknown id has no speed rather than someone else's.
	CHECK(store().resolvedSpeed(9999) < 0);
}

// the reference controller: the 20 lists can be named, and the names belong to the project.
static void test_list_names()
{
	reset();
	CHECK(store().listName(1).empty()); // never named
	CHECK(store().setListName(1, "Gol"));
	CHECK(store().listName(1) == "Gol");
	CHECK(store().listName(2).empty()); // naming one does not name them all

	// Out-of-range lists are refused, not silently written elsewhere.
	CHECK(!store().setListName(0, "x"));
	CHECK(!store().setListName(kEventLists + 1, "x"));
	CHECK(store().listName(0).empty());

	// Renaming bumps the version so the dock redraws its combo.
	uint64_t v = store().version();
	CHECK(store().setListName(3, "Falli"));
	CHECK(store().version() > v);

	// "Delete All" wipes the events and KEEPS the settings (the reference controller).
	store().clearAll();
	CHECK(store().listName(1) == "Gol");
	store().setListName(1, "");
	store().setListName(3, "");
}

static void test_version_bumps()
{
	reset();
	uint64_t v0 = store().version();
	store().markIn(0, 0);
	CHECK(store().version() > v0);
	uint64_t v1 = store().version();
	store().selectList(2); // not a persisted mutation → no save()
	CHECK(store().version() == v1);
}

int main()
{
	test_mark_in_out();
	test_mark_in_out_preset();
	test_mark_cancel();
	test_angle_edits_bounds();
	test_speed_clamping();
	test_move_points();
	test_move_to_list_and_select();
	test_duplicate_and_last();
	test_description();
	test_chapters_text();
	test_pre_post_roll();
	test_speed_inheritance();
	test_list_names();
	test_version_bumps();

	if (g_fail == 0)
		std::printf("OK: all event-store logic tests passed\n");
	else
		std::printf("%d CHECK(s) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
