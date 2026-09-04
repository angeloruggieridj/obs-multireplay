/*
obs-multireplay — what a bay switch must do to the angle hotkeys share
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Numbered angle hotkeys (Stream Deck, OBS hotkey bindings) know nothing about
channel A or B: they only set ReplayCore::currentAngle(), ONE shared value,
and the dock's poll() copies that value onto whichever channel (bay) is
currently active, every tick — a hotkey means "the channel driving the keys
right now" (the A|B / A / B selector), the same rule marking and playback
keys already follow.

That copy is unconditional and runs on every tick, INCLUDING the one
setActiveChannel() triggers immediately after flipping which channel is
active. Switching the selector does not, by itself, touch the shared value —
so without the push this header computes, switching FROM a channel a hotkey
had just moved TO a channel that had a DIFFERENT angle stored from earlier
had the very next tick silently overwrite the newly active channel's own
angle with whatever the previous channel had left behind. Two bays exist
precisely so one can be lined up while the other is on air; an angle that
does not survive being looked away from defeats that.
*/

#pragma once

namespace multireplay {

namespace angle_channels {

// The 0-based value to push into the shared angle the instant `which`
// becomes the active channel — BEFORE the tick that copies the shared value
// back onto it runs. Pushing the channel's OWN angle makes that copy a
// no-op instead of a silent overwrite.
inline int sharedAngleOnActivate(int activatedChannelAngle1Based)
{
	return activatedChannelAngle1Based - 1;
}

} // namespace angle_channels

} // namespace multireplay
