/*
obs-multireplay — unit tests for angle-channels.hpp
SPDX-License-Identifier: GPL-2.0-or-later

Standalone: no OBS, no Qt. The dock's real angle1_[]/activeChannel_ state
cannot be exercised here — it lives inside MultiReplayDock, and the
regression this pins needs two real cameras to show on the gate (see
"mark_out_without_open_event_uses_notice"'s sibling check,
dock_channel_switch_keeps_angle, which is vacuously true on a one-camera
rig). This models the same interaction in miniature: a shared value hotkeys
read and write, an active-channel switch, and the unconditional tick-copy
poll() performs — the three moving parts whose ordering the real bug was in.
*/

#include "angle-channels.hpp"

#include <cstdio>

using namespace multireplay;

static int g_fail = 0;

#define CHECK(cond)                                                        \
	do {                                                                \
		if (!(cond)) {                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, \
				    #cond);                                 \
			++g_fail;                                           \
		}                                                           \
	} while (0)

namespace {

// Mirrors angle1_[2] + activeChannel_ + ReplayCore::currentAngle(), and the
// two things that touch them: a numbered hotkey, and switching the A|B / A /
// B selector. `fixed` selects whether the switch calls
// angle_channels::sharedAngleOnActivate() before the tick-copy — off
// reproduces the bug this header exists to close.
struct DockModel {
	int angle1[2] = {1, 1}; // 1-based, per channel
	int active = 0;
	int shared0 = 0; // ReplayCore::currentAngle(), 0-based
	bool fixed = true;

	// A numbered angle hotkey fired while `active` is driving the keys.
	void hotkeyAngle(int angle0)
	{
		shared0 = angle0;
		tick();
	}

	// The operator clicks the A|B / A / B selector.
	void switchTo(int which)
	{
		active = which;
		if (fixed)
			shared0 = angle_channels::sharedAngleOnActivate(
				angle1[which]);
		tick(); // setActiveChannel() calls poll() synchronously
	}

	// poll(): unconditional, every tick.
	void tick() { angle1[active] = shared0 + 1; }
};

} // namespace

static void test_hotkey_sets_only_the_active_channel()
{
	DockModel m;
	m.switchTo(0); // A
	m.hotkeyAngle(2); // "3"
	CHECK(m.angle1[0] == 3);
	CHECK(m.angle1[1] == 1); // B untouched
}

// The exact sequence the analysis's §2.4 suspicion described: prepare B on
// one angle, touch A, come back to B and find it still there.
static void test_switching_back_keeps_the_angle_that_was_left(void)
{
	DockModel m;
	m.switchTo(1); // B
	m.hotkeyAngle(2); // B is now angle 3
	CHECK(m.angle1[1] == 3);

	m.switchTo(0); // A
	m.hotkeyAngle(0); // A is now angle 1
	CHECK(m.angle1[0] == 1);

	m.switchTo(1); // back to B
	CHECK(m.angle1[1] == 3); // must still be 3, not silently reset to 1
}

// The same sequence WITHOUT the fix: proves the model reproduces the bug
// rather than being unable to fail.
static void test_the_bug_this_replaces(void)
{
	DockModel m;
	m.fixed = false;

	m.switchTo(1);
	m.hotkeyAngle(2); // B = 3
	m.switchTo(0);
	m.hotkeyAngle(0); // A = 1, shared0 = 0

	m.switchTo(1); // no push: the tick copies shared0 (0) onto B
	CHECK(m.angle1[1] == 1); // B's own angle (3) is gone
}

int main()
{
	test_hotkey_sets_only_the_active_channel();
	test_switching_back_keeps_the_angle_that_was_left();
	test_the_bug_this_replaces();

	if (g_fail) {
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("angle_channels: all checks passed\n");
	return 0;
}
