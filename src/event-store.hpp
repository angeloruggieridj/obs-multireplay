/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "session-clock.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multireplay {

constexpr int kEventLists = 20;
constexpr int kEventAngles = 8; // M5: full reference parity

// One broadcast-style replay event: a time range on the master timeline with
// per-angle enable flags, notes and speeds. Footage is never touched.
//
// THERE IS NO PER-EVENT SPEED, and there never should have been one. A replay
// speed belongs to an ANGLE — the wide shot at 100% and the tight one at 25% of
// the same action is the whole point of a multi-camera replay — or to the
// operator's slider, which is the default for every angle that does not say
// otherwise. The event-level speed added in M3 (with its inheritance from the
// previous mark) sat between those two and answered a question nobody asks; it
// is gone, together with its column.
struct ReplayEvent {
	int id = 0;   // broadcast-style fixed numeric id (new id on copy)
	int list = 1; // 1..20
	// Both are INSTANTS on the master timeline, and an instant may be
	// negative — a mark taken before the machine's last boot converts back to
	// one, which is every mark in a reopened project. So "open" is kNoInstant
	// and not "< 0": with the old sentinel every closed event of yesterday's
	// project read as still open, and none of them would play.
	int64_t tInNs = kNoInstant;
	int64_t tOutNs = kNoInstant; // open event: Mark In without a Mark Out yet
	// Position in the RUNNING ORDER of its list: dense, ascending, and the
	// order the dock draws (and plays) the list in. A new mark goes last;
	// after that the operator owns it — a highlights reel is not "the order
	// I happened to press the button in". Persisted with the event.
	int order = 0;
	// WHAT THIS EVENT IS, in the operator words. ONE per event and not one
	// per angle: a goal is a goal on every camera that saw it, and the note
	// per angle asked the operator the same question once for each lens - so
	// in practice it was answered on one of them and left blank on the rest,
	// which is why reading it back always meant "the first angle that has
	// one". Files written before this carry it on the angles; the loader
	// takes the first non-empty one and moves it here.
	std::string note;
	struct Angle {
		bool enabled = false;
		// -1 = use the default (the dock's slider). Otherwise a factor:
		// 0.05..4.0, the range the playback engine accepts (5..400%).
		double speed = -1.0;
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

	// Wait for the pending events.json write to reach the disk, then stop the
	// writer. Called from obs_module_unload — see the note on save().
	void shutdown();
	void clearAll();                                  // the reference controller "Delete All"

	// --- Live / Recorded mode (the reference controller Live button) ---
	void setLiveMode(bool live) { liveMode_ = live; }
	bool liveMode() const { return liveMode_; }

	// --- Pre/post roll (the reference controller) ----------------------------------------------
	// Extra time added to the start and the end of an event. Applied WHEN THE
	// MARK IS TAKEN, not at playback: the in/out the operator reads in the list
	// are then the ones that will play, and he can still trim them. Both 0 (the
	// default) is byte-for-byte the old behaviour.
	void setRollNs(int64_t preNs, int64_t postNs);
	int64_t preRollNs() const { return preRollNs_.load(); }
	int64_t postRollNs() const { return postRollNs_.load(); }

	// --- List selection ---
	void selectList(int list); // 1..20
	int selectedList() const { return selectedList_; }

	// --- List names (the reference controller: the 20 lists can be named) ----------------------
	// "" = never named; the UI then shows the bare number.
	std::string listName(int list) const;
	bool setListName(int list, const std::string &name);

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
	// Event-level description: stored as the note on every angle so it is
	// independent of which cameras are enabled and survives toggling them.
	bool setDescription(int id, const std::string &note);
	std::string description(int id) const; // first non-empty angle note
	// Per-angle speed override; <0 = use the caller's default (the slider).
	// This is the ONLY per-event speed there is — see the note on ReplayEvent.
	bool setAngleSpeed(int id, int angle1Based, double speed);
	bool movePoint(int id, bool inPoint, int64_t deltaNs);
	bool moveToList(int id, int list);
	// Move an event `delta` places within the running order of its list
	// (-1 = one earlier, +1 = one later). False when it is already at the
	// end it is being pushed towards, or the id is unknown. Persisted.
	bool moveEvent(int id, int delta);
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
	// A writer thread still joinable at destruction is std::terminate, and
	// this is a function-local singleton — so the destructor is the backstop
	// for the case where obs_module_unload did not run (a crash on the way
	// out, a host that unloads differently).
	~EventStore();
	// SERIALISE under mutex_, WRITE somewhere else (M4).
	//
	// save() used to call obs_data_save_json_safe() with mutex_ held, on
	// every mark, tick, note and reorder — twenty-two call sites. On a
	// session folder that lives on a NAS that is a network round trip per
	// gesture, taken on the UI thread or the hotkey thread, while holding the
	// lock the dock has to take thirty times a second to redraw the event
	// table. Now it builds the JSON (which does need the lock: it reads the
	// events) and hands the bytes to a writer thread that coalesces a burst
	// of edits into one write.
	void save() const; // mutex_ must be held
	void load();       // mutex_ must be held
	// Hand `json` to the writer for `path`. Last one wins; no lock of ours is
	// held while anything touches the disk.
	void queueWrite(const std::string &path, const std::string &json) const;
	void writerLoop();
	// Block until nothing is pending (bounded). Used by shutdown() and
	// whenever the store is about to point at another project.
	void flushWrites() const;
	// Make every list's `order` dense and ascending, keeping the order they
	// are already in. mutex_ must be held. Runs after a load (a file written
	// before ordering existed has all-zero orders, and two events must never
	// share a place) and after a move.
	void normalizeOrder();
	// Next free place at the end of `list`. mutex_ must be held.
	int nextOrderFor(int list) const;

	mutable std::mutex mutex_;
	SessionEpoch epoch_; // see setSessionEpoch()
	std::string folder_;
	std::vector<ReplayEvent> events_;
	std::array<std::string, kEventLists> listNames_;
	int nextId_ = 1;
	std::atomic<bool> liveMode_{true};
	std::atomic<int> selectedList_{1};
	// Pre/post roll, see setRollNs(). Atomic so the hotkey path can read them
	// without taking the store lock.
	std::atomic<int64_t> preRollNs_{0};
	std::atomic<int64_t> postRollNs_{0};
	mutable std::atomic<uint64_t> version_{0};

	// --- the events.json writer (see save) ------------------------------
	mutable std::mutex writeMutex_;
	mutable std::condition_variable writeCv_;
	mutable std::string pendingPath_;
	mutable std::string pendingJson_;
	mutable bool pendingDirty_ = false;
	// A write is on the disk right now. flushWrites() waits for this too, or
	// it would report "flushed" the instant the writer picked the payload up.
	mutable bool writing_ = false;
	mutable bool flushNow_ = false;
	mutable bool writerStop_ = false;
	mutable bool writerStarted_ = false;
	mutable std::thread writer_;
};

} // namespace multireplay
