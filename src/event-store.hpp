/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

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
	};
	std::array<Angle, kEventAngles> angles;
	std::string createdMode; // "live" | "recorded"
};

// Holds the 20 event lists. Persists to events.json in the session folder
// on every mutation (crash-safe via obs_data_save_json_safe).
class EventStore {
public:
	static EventStore &instance();

	void setSessionFolder(const std::string &folder); // loads events.json
	void clearAll();                                  // the reference controller "Delete All"

	// --- Live / Recorded mode (the reference controller Live button) ---
	void setLiveMode(bool live) { liveMode_ = live; }
	bool liveMode() const { return liveMode_; }

	// --- List selection ---
	void selectList(int list); // 1..20
	int selectedList() const { return selectedList_; }

	// --- Event creation (times on the master timeline) ---
	int markIn(int64_t tNs);                    // returns event id
	bool markOut(int64_t tNs);                  // closes last open event
	int markInOut(int64_t tNowNs, int seconds); // -5/-10/-20 presets
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
	bool setSpeed(int id, double speed); // <0 = "--"
	bool movePoint(int id, bool inPoint, int64_t deltaNs);
	bool moveToList(int id, int list);
	int duplicate(int id); // copy gets a new id (the reference controller behaviour)

	// Most recent event by time (the reference controller "last event"), 0 if none.
	int lastEventId() const;
	bool get(int id, ReplayEvent &out) const;

	// JSON of one list's events (for the web UI).
	std::string listJson(int list) const;

private:
	EventStore() = default;
	void save() const; // mutex_ must be held
	void load();       // mutex_ must be held

	mutable std::mutex mutex_;
	std::string folder_;
	std::vector<ReplayEvent> events_;
	int nextId_ = 1;
	std::atomic<bool> liveMode_{true};
	std::atomic<int> selectedList_{1};
};

} // namespace multireplay
