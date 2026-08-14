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
	CHECK(ev.tOutNs == kNoInstant); // open
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

	// A negative IN is KEPT, not clamped. It used to be floored at 0, on the
	// reasoning that time does not run backwards — but a master instant is
	// read off a clock that starts at boot, so every mark in a project
	// recorded before the last reboot converts back to a negative one. The
	// floor moved those marks to instant 0, where no footage has ever been,
	// and the events silently stopped playing. See kNoInstant.
	int id3 = store().markIn(-7 * S, 0);
	CHECK(store().get(id3, ev));
	CHECK(ev.tInNs == -7 * S);
	// ...and it is a real mark, not the "no instant" sentinel.
	CHECK(ev.tInNs != kNoInstant);
	// It closes like any other, and its duration is the honest one.
	CHECK(store().markOut(-3 * S));
	CHECK(store().get(id3, ev));
	CHECK(ev.tOutNs == -3 * S);
	CHECK(ev.tOutNs - ev.tInNs == 4 * S);
	// A closed event of yesterday's project must not read as still OPEN:
	// that was the bug, and every "is it closed?" in the plugin used to be
	// spelled `tOutNs < 0`. movePoint's out-point branch is one of them, and
	// it only runs for a closed event — so a moved OUT proves the branch was
	// taken on a negative instant.
	CHECK(store().movePoint(id3, /*inPoint*/ false, 1 * S));
	CHECK(store().get(id3, ev));
	CHECK(ev.tOutNs == -2 * S);
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

	// A preset window reaching back past the clock's own zero is KEPT as it
	// is, not clamped: see the note in test_mark_in_out. The window the
	// operator asked for is 20 s, and 20 s is what the event spans.
	int id2 = store().markInOut(3 * S, 20, 0);
	CHECK(store().get(id2, ev));
	CHECK(ev.tInNs == -17 * S);
	CHECK(ev.tOutNs == 3 * S);
	CHECK(ev.tOutNs - ev.tInNs == 20 * S);
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

// The ONLY speed an event carries is per-angle. There is no event-level speed
// and no inheritance from the previous mark: a replay speed belongs to a camera
// (the wide at 100%, the tight at 25%) or to the operator's slider.
static void test_angle_speed_is_the_only_speed()
{
	reset();
	int id = store().markIn(0, 0);
	ReplayEvent ev;

	// Default: nobody set one, so the caller's default (the slider) wins.
	CHECK(store().get(id, ev));
	for (const auto &an : ev.angles)
		CHECK(an.speed == -1.0);

	// Clamped to what the playback engine accepts: 5%..400%.
	CHECK(store().setAngleSpeed(id, 1, 5.0)); // over → 4.0 (400%)
	CHECK(store().get(id, ev));
	CHECK(ev.angles[0].speed == 4.0);

	CHECK(store().setAngleSpeed(id, 1, 0.0)); // 0 never plays → 0.05 floor
	CHECK(store().get(id, ev));
	CHECK(ev.angles[0].speed == 0.05);

	CHECK(store().setAngleSpeed(id, 1, -2.0)); // any negative → -1 sentinel
	CHECK(store().get(id, ev));
	CHECK(ev.angles[0].speed == -1.0);

	// Per ANGLE, not per event: setting one leaves the others alone. This is
	// the whole point of the field, and the reason the event-level one went.
	CHECK(store().setAngleSpeed(id, 2, 0.25));
	CHECK(store().setAngleSpeed(id, 3, 1.0));
	CHECK(store().get(id, ev));
	CHECK(ev.angles[0].speed == -1.0);
	CHECK(ev.angles[1].speed == 0.25);
	CHECK(ev.angles[2].speed == 1.0);
	CHECK(ev.angles[3].speed == -1.0);

	// A later mark inherits NOTHING: the speed the operator set on one event's
	// camera is that event's camera, not a mode he has silently entered.
	int later = store().markIn(10 * S, 1);
	CHECK(store().get(later, ev));
	for (const auto &an : ev.angles)
		CHECK(an.speed == -1.0);

	// Unknown id / out-of-range angle are refused, not written elsewhere.
	CHECK(!store().setAngleSpeed(9999, 1, 0.5));
	CHECK(!store().setAngleSpeed(id, kEventAngles + 1, 0.5));
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

	// Move IN far back → it goes there. There is no floor at 0 any more:
	// instant 0 is not the beginning of anything, it is just the moment this
	// machine happened to boot, and a mark before it is ordinary in a
	// reopened project (see test_mark_in_out).
	CHECK(store().movePoint(id, true, -1000 * S));
	CHECK(store().get(id, ev));
	CHECK(ev.tInNs == 20 * S - 1 - 1000 * S);

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

	// A pre-roll longer than the elapsed clock backs the mark up past zero
	// rather than piling every such mark onto instant 0 — where no footage
	// is, so they would all have become unplayable together.
	store().setRollNs(500 * S, 0);
	int id3 = store().markIn(5 * S, 0);
	CHECK(store().get(id3, ev));
	CHECK(ev.tInNs == -495 * S);

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

// The per-angle triplet the operator fills in during a match — IS THIS ANGLE
// IN, AT WHAT SPEED, WITH WHAT COMMENT — is three independent fields on the
// same angle. The dock edits them from one table row, so nothing may couple
// them: toggling an angle off must not forget its speed or its note.
static void test_angle_triplet_is_independent()
{
	reset();
	int id = store().markIn(10 * S, 0);
	store().markOut(11 * S);
	ReplayEvent ev;

	CHECK(store().setAngle(id, 2, true));
	CHECK(store().setAngleSpeed(id, 2, 0.5));
	CHECK(store().setAngleNote(id, 2, "tight"));
	CHECK(store().get(id, ev));
	CHECK(ev.angles[1].enabled);
	CHECK(ev.angles[1].speed == 0.5);
	CHECK(ev.angles[1].note == "tight");

	// Off and on again: the speed and the comment survive. An operator who
	// unticks an angle to shorten a sequence has not thrown away his edit.
	CHECK(store().setAngle(id, 2, false));
	CHECK(store().get(id, ev));
	CHECK(!ev.angles[1].enabled);
	CHECK(ev.angles[1].speed == 0.5);
	CHECK(ev.angles[1].note == "tight");
	CHECK(store().setAngle(id, 2, true));

	// Clearing the speed leaves the comment, and vice versa.
	CHECK(store().setAngleSpeed(id, 2, -1.0));
	CHECK(store().get(id, ev));
	CHECK(ev.angles[1].note == "tight");
	CHECK(store().setAngleNote(id, 2, ""));
	CHECK(store().get(id, ev));
	CHECK(ev.angles[1].enabled);
	CHECK(ev.angles[1].speed == -1.0);
	CHECK(ev.angles[1].note.empty());

	// A duplicate carries the whole triplet across (the reference controller), on a new id.
	CHECK(store().setAngleSpeed(id, 3, 0.25));
	CHECK(store().setAngleNote(id, 3, "wide"));
	int dup = store().duplicate(id);
	CHECK(dup != 0 && dup != id);
	CHECK(store().get(dup, ev));
	CHECK(ev.angles[2].speed == 0.25);
	CHECK(ev.angles[2].note == "wide");
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

// The running order is the operator's: a new mark goes last, and after that he
// moves it by hand. The order is what the dock draws and what a sequence plays
// in, so it has to be dense, per-list, and refuse to fall off either end.
static void test_manual_order()
{
	reset();
	int a = store().markIn(10 * S, 0);
	store().markOut(11 * S);
	int b = store().markIn(20 * S, 0);
	store().markOut(21 * S);
	int c = store().markIn(30 * S, 0);
	store().markOut(31 * S);

	const auto orderOf = [](int id) {
		ReplayEvent ev;
		return store().get(id, ev) ? ev.order : -1;
	};

	// A new mark goes last, in the order they were taken.
	CHECK(orderOf(a) == 0);
	CHECK(orderOf(b) == 1);
	CHECK(orderOf(c) == 2);

	// Move c up: c b -> ... a c b.
	CHECK(store().moveEvent(c, -1));
	CHECK(orderOf(a) == 0);
	CHECK(orderOf(c) == 1);
	CHECK(orderOf(b) == 2);

	// ...and again: c a b.
	CHECK(store().moveEvent(c, -1));
	CHECK(orderOf(c) == 0);
	CHECK(orderOf(a) == 1);
	CHECK(orderOf(b) == 2);

	// Off the top is REFUSED, not clamped: "it did nothing" and "it moved"
	// are different answers and the dock says which.
	CHECK(!store().moveEvent(c, -1));
	CHECK(orderOf(c) == 0);
	CHECK(!store().moveEvent(b, +1));
	CHECK(orderOf(b) == 2);

	// A delta of 0 is not a move.
	CHECK(!store().moveEvent(a, 0));
	// An unknown id is not a move either.
	CHECK(!store().moveEvent(9999, -1));

	// Positions stay dense after a move, so no two events ever share a place.
	CHECK(orderOf(c) + orderOf(a) + orderOf(b) == 0 + 1 + 2);

	// The order is PER LIST: an event moved to another list is appended
	// there and does not disturb the places of the list it left.
	store().selectList(2);
	int d = store().markIn(40 * S, 0);
	store().markOut(41 * S);
	CHECK(orderOf(d) == 0); // first in ITS list, not fourth overall
	store().selectList(1);
	CHECK(store().moveToList(b, 2));
	CHECK(orderOf(b) == 1); // appended after d
	CHECK(store().moveToList(b, 1));

	// A duplicate gets its own place at the end, never the original's.
	int dup = store().duplicate(a);
	CHECK(dup != 0);
	CHECK(orderOf(dup) != orderOf(a));
	CHECK(orderOf(dup) >= 2);
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
	test_angle_speed_is_the_only_speed();
	test_move_points();
	test_move_to_list_and_select();
	test_duplicate_and_last();
	test_description();
	test_chapters_text();
	test_pre_post_roll();
	test_angle_triplet_is_independent();
	test_list_names();
	test_manual_order();
	test_version_bumps();

	if (g_fail == 0)
		std::printf("OK: all event-store logic tests passed\n");
	else
		std::printf("%d CHECK(s) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
