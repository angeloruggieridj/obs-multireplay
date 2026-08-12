/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "session-clock.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace multireplay {

constexpr int kEventLists = 20;
constexpr int kEventAngles = 8; // M5: full reference parity

// One broadcast-style replay event: a time range on the master timeline with
// per-angle enable flags and notes. Footage is never touched.
struct ReplayEvent {
	int id = 0;        // broadcast-style fixed numeric id (new id on copy)
	int list = 1;      // 1..20
	int64_t tInNs = 0; // master timeline
	int64_t tOutNs = -1; // -1 = open event (Mark In without Mark Out yet)
	double speed = -1.0; // -1 = "--" (inherit), else 0..1
	struct Angle {
		bool enabled = false;
		std::string note;
		double speed = -1.0; // -1 = inherit event/default speed, else 0..1
	};
	std::array<Angle, kEventAngles> angles;
	std::string createdMode; // "live" | "recorded"
};

// Holds the 20 event lists. Persists to events.json in the session folder
// on every mutation (crash-safe via obs_data_save_json_safe).
class EventStore {
public:
	static EventStore &instance();

	// The monotonic↔wall pair used to persist marks. Marks live in memory as
	// master (monotonic) time, which is meaningless across restarts, so
	// events.json stores them as absolute wall-clock ns and they are mapped
	// back through this epoch on load. MUST be set before setSessionFolder(),
	// which loads. Left unset (all-zero) the conversion is the identity, which
	// is what the standalone tests want. See session-clock.hpp.
	void setSessionEpoch(const SessionEpoch &epoch);

	void setSessionFolder(const std::string &folder); // loads events.json
	void clearAll();                                  // the reference controller "Delete All"

	// --- Live / Recorded mode (the reference controller Live button) ---
	void setLiveMode(bool live) { liveMode_ = live; }
	bool liveMode() const { return liveMode_; }

	// --- List selection ---
	void selectList(int list); // 1..20
	int selectedList() const { return selectedList_; }

	// --- Event creation (times on the master timeline) ---
	// angle0Based: 0-based camera to enable (0 = CAM1). Replaces the old
	// "always CAM1" default so the UI can pass the selected angle.
	int markIn(int64_t tNs, int angle0Based = 0);                    // returns event id
	bool markOut(int64_t tNs);                                       // closes last open event
	int markInOut(int64_t tNowNs, int seconds, int angle0Based = 0); // -5/-10/-20 presets
	bool markCancel();                          // deletes last open event

	// --- Editing ---
	bool remove(int id);
	bool toggleAngle(int id, int angle1Based);
	bool setAngle(int id, int angle1Based, bool enabled);
	bool setAngleNote(int id, int angle1Based, const std::string &note);
	// Event-level description: stored as the note on every angle so it is
	// independent of which cameras are enabled and survives toggling them.
	bool setDescription(int id, const std::string &note);
	std::string description(int id) const; // first non-empty angle note
	bool setSpeed(int id, double speed); // event default; <0 = "--"
	// Per-angle speed override; <0 = inherit the event/default speed.
	bool setAngleSpeed(int id, int angle1Based, double speed);
	bool movePoint(int id, bool inPoint, int64_t deltaNs);
	bool moveToList(int id, int list);
	int duplicate(int id); // copy gets a new id (the reference controller behaviour)

	// Most recent event by time (the reference controller "last event"), 0 if none.
	int lastEventId() const;
	bool get(int id, ReplayEvent &out) const;

	// JSON of one list's events (for the web UI).
	std::string listJson(int list) const;

	// YouTube-compatible chapter timestamps for all completed events in list.
	// Format: "MM:SS Description\n" (or H:MM:SS if >= 1 hour).
	// Description = first non-empty angle note (lowest-index angle).
	// `originNs` is the master-timeline instant that counts as 0:00 — marks
	// are absolute instants on a clock that starts with OBS, not offsets into
	// a recording, so the caller says where the video begins.
	std::string chaptersText(int list, int64_t originNs = 0) const;

	// Monotonic counter: incremented on every mutation. The dock polls this
	// to auto-refresh the event table without a callback mechanism.
	uint64_t version() const { return version_.load(); }

private:
	EventStore() = default;
	void save() const; // mutex_ must be held
	void load();       // mutex_ must be held

	mutable std::mutex mutex_;
	SessionEpoch epoch_; // see setSessionEpoch()
	std::string folder_;
	std::vector<ReplayEvent> events_;
	int nextId_ = 1;
	std::atomic<bool> liveMode_{true};
	std::atomic<int> selectedList_{1};
	mutable std::atomic<uint64_t> version_{0};
};

} // namespace multireplay
