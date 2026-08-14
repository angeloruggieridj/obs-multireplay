/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "event-store.hpp"
#include "replay-core.hpp" // MR_DLOG

// obs-module.h must come before plugin-support.h (MSVC blogva linkage).
#include <obs-module.h>
#include "plugin-support.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace multireplay {

namespace {
constexpr const char *kEventsFile = "events.json";
}

EventStore &EventStore::instance()
{
	static EventStore store;
	return store;
}

void EventStore::setSessionEpoch(const SessionEpoch &epoch)
{
	std::lock_guard<std::mutex> lock(mutex_);
	epoch_ = epoch;
}

void EventStore::setSessionFolder(const std::string &folder)
{
	std::lock_guard<std::mutex> lock(mutex_);
	folder_ = folder;
	events_.clear();
	// Cleared HERE and not only in load(): a project with no events.json yet
	// would otherwise inherit the previous project's list names.
	listNames_.fill(std::string());
	nextId_ = 1;
	load();
}

void EventStore::clearAll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	events_.clear();
	// the reference controller keeps settings; ids restart with the session
	nextId_ = 1;
	version_++; // ensure UI refresh even when folder is empty
	save();
}

void EventStore::setRollNs(int64_t preNs, int64_t postNs)
{
	// Negative rolls would shorten the event instead of padding it, which is
	// not what the setting says it does.
	preRollNs_.store(std::max<int64_t>(0, preNs));
	postRollNs_.store(std::max<int64_t>(0, postNs));
}

void EventStore::selectList(int list)
{
	if (list >= 1 && list <= kEventLists)
		selectedList_ = list;
}

std::string EventStore::listName(int list) const
{
	if (list < 1 || list > kEventLists)
		return {};
	std::lock_guard<std::mutex> lock(mutex_);
	return listNames_[list - 1];
}

bool EventStore::setListName(int list, const std::string &name)
{
	if (list < 1 || list > kEventLists)
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	listNames_[list - 1] = name;
	save();
	return true;
}

int EventStore::nextOrderFor(int list) const
{
	// mutex_ held by the caller.
	int next = 0;
	for (const auto &ev : events_)
		if (ev.list == list && ev.order >= next)
			next = ev.order + 1;
	return next;
}

void EventStore::normalizeOrder()
{
	// mutex_ held by the caller.
	//
	// Dense and ascending, per list, PRESERVING the order they are already
	// in. Two things need it: a file written before ordering existed (every
	// order is 0, so the running order would depend on nothing at all), and
	// any operation that can leave a hole. Stable on the vector's own order,
	// so events that tie keep the order they were marked in.
	for (int list = 1; list <= kEventLists; list++) {
		std::vector<size_t> idx;
		for (size_t i = 0; i < events_.size(); i++)
			if (events_[i].list == list)
				idx.push_back(i);
		std::stable_sort(idx.begin(), idx.end(),
				 [this](size_t a, size_t b) {
					 return events_[a].order <
						events_[b].order;
				 });
		for (size_t k = 0; k < idx.size(); k++)
			events_[idx[k]].order = (int)k;
	}
}

bool EventStore::moveEvent(int id, int delta)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (delta == 0)
		return false;
	// Who is being moved, and the running order of the list it is in.
	const ReplayEvent *self = nullptr;
	for (const auto &ev : events_)
		if (ev.id == id)
			self = &ev;
	if (!self)
		return false;
	const int list = self->list;

	std::vector<size_t> idx;
	for (size_t i = 0; i < events_.size(); i++)
		if (events_[i].list == list)
			idx.push_back(i);
	std::stable_sort(idx.begin(), idx.end(), [this](size_t a, size_t b) {
		return events_[a].order < events_[b].order;
	});

	int pos = -1;
	for (size_t k = 0; k < idx.size(); k++)
		if (events_[idx[k]].id == id)
			pos = (int)k;
	if (pos < 0)
		return false;
	const int dest = pos + delta;
	// Refused, not clamped: "it did nothing" and "it moved to the end" are
	// different answers, and the dock says so.
	if (dest < 0 || dest >= (int)idx.size())
		return false;

	std::swap(events_[idx[(size_t)pos]].order,
		  events_[idx[(size_t)dest]].order);
	normalizeOrder();
	save();
	return true;
}

int EventStore::markIn(int64_t tNs, int angle0Based)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ReplayEvent ev;
	ev.id = nextId_++;
	ev.list = selectedList_;
	ev.order = nextOrderFor(ev.list); // a new mark goes last
	// Pre-roll: the operator presses IN when he has SEEN the action, so the reference controller
	// backs the mark up by a configured amount. Baked into the stored in-point
	// on purpose — an in-point that reads 12:03.500 must be the one that plays.
	ev.tInNs = std::max<int64_t>(0, tNs - preRollNs_.load());
	ev.tOutNs = -1;
	ev.angles[std::clamp(angle0Based, 0, kEventAngles - 1)].enabled = true;
	ev.createdMode = liveMode_ ? "live" : "recorded";
	events_.push_back(ev);
	save();
	return ev.id;
}

bool EventStore::markOut(int64_t tNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	// close the most recent open event in the selected list
	for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
		if (it->list == selectedList_ && it->tOutNs < 0) {
			// Post-roll: keep the play running past the whistle.
			it->tOutNs = std::max(it->tInNs + 1,
					      tNs + postRollNs_.load());
			MR_DLOG(
				"[ev] markOut id=%d IN=%lldms OUT=%lldms dur=%lldms",
				it->id, (long long)(it->tInNs / 1000000),
				(long long)(it->tOutNs / 1000000),
				(long long)((it->tOutNs - it->tInNs) / 1000000));
			save();
			return true;
		}
	}
	return false;
}

int EventStore::markInOut(int64_t tNowNs, int seconds, int angle0Based)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ReplayEvent ev;
	ev.id = nextId_++;
	ev.list = selectedList_;
	ev.order = nextOrderFor(ev.list); // a new mark goes last
	// Same rolls as the manual marks, measured from NOW: the preset window is
	// the `seconds` the operator asked for, and the rolls pad it on both sides.
	ev.tOutNs = std::max<int64_t>(1, tNowNs + postRollNs_.load());
	ev.tInNs = std::max<int64_t>(0, tNowNs -
						(int64_t)seconds * 1000000000 -
						preRollNs_.load());
	ev.angles[std::clamp(angle0Based, 0, kEventAngles - 1)].enabled = true;
	ev.createdMode = liveMode_ ? "live" : "recorded";
	events_.push_back(ev);
	save();
	return ev.id;
}

bool EventStore::markCancel()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
		if (it->list == selectedList_ && it->tOutNs < 0) {
			events_.erase(std::next(it).base());
			save();
			return true;
		}
	}
	return false;
}

bool EventStore::remove(int id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = std::find_if(events_.begin(), events_.end(),
			       [id](const ReplayEvent &e) {
				       return e.id == id;
			       });
	if (it == events_.end())
		return false;
	events_.erase(it);
	save();
	return true;
}

bool EventStore::toggleAngle(int id, int angle1Based)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (angle1Based < 1 || angle1Based > kEventAngles)
		return false;
	for (auto &ev : events_) {
		if (ev.id == id) {
			auto &a = ev.angles[angle1Based - 1];
			a.enabled = !a.enabled;
			save();
			return true;
		}
	}
	return false;
}

bool EventStore::setAngle(int id, int angle1Based, bool enabled)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (angle1Based < 1 || angle1Based > kEventAngles)
		return false;
	for (auto &ev : events_) {
		if (ev.id == id) {
			ev.angles[angle1Based - 1].enabled = enabled;
			save();
			return true;
		}
	}
	return false;
}

bool EventStore::setDescription(int id, const std::string &note)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &ev : events_) {
		if (ev.id == id) {
			for (auto &a : ev.angles)
				a.note = note;
			save();
			return true;
		}
	}
	return false;
}

std::string EventStore::description(int id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &ev : events_) {
		if (ev.id == id) {
			for (const auto &a : ev.angles)
				if (!a.note.empty())
					return a.note;
			return {};
		}
	}
	return {};
}

bool EventStore::setAngleNote(int id, int angle1Based, const std::string &note)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (angle1Based < 1 || angle1Based > kEventAngles)
		return false;
	for (auto &ev : events_) {
		if (ev.id == id) {
			ev.angles[angle1Based - 1].note = note;
			save();
			return true;
		}
	}
	return false;
}

bool EventStore::setAngleSpeed(int id, int angle1Based, double speed)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (angle1Based < 1 || angle1Based > kEventAngles)
		return false;
	for (auto &ev : events_) {
		if (ev.id == id) {
			// Clamped to what the playback engine accepts (5..400%),
			// not to 100%: an angle can legitimately be the fast one
			// (fast forward is the same control past 1×), and a
			// value the store keeps but the engine would refuse is a
			// number that lies to the operator.
			ev.angles[angle1Based - 1].speed =
				speed < 0 ? -1.0 : std::clamp(speed, 0.05, 4.0);
			save();
			return true;
		}
	}
	return false;
}

bool EventStore::movePoint(int id, bool inPoint, int64_t deltaNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &ev : events_) {
		if (ev.id != id)
			continue;
		if (inPoint) {
			ev.tInNs = std::max<int64_t>(0, ev.tInNs + deltaNs);
			if (ev.tOutNs >= 0)
				ev.tInNs = std::min(ev.tInNs, ev.tOutNs - 1);
		} else if (ev.tOutNs >= 0) {
			ev.tOutNs = std::max(ev.tInNs + 1,
					     ev.tOutNs + deltaNs);
		}
		save();
		return true;
	}
	return false;
}

bool EventStore::moveToList(int id, int list)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (list < 1 || list > kEventLists)
		return false;
	for (auto &ev : events_) {
		if (ev.id == id) {
			// Its old place means nothing in the new list: append.
			// Asked BEFORE the move, or the event would be counted as
			// already being in the destination and would append after
			// ITSELF — landing one rung past the end, with a hole.
			const int place = nextOrderFor(list);
			ev.list = list;
			ev.order = place;
			save();
			return true;
		}
	}
	return false;
}

int EventStore::duplicate(int id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &ev : events_) {
		if (ev.id == id) {
			ReplayEvent copy = ev;
			copy.id = nextId_++; // the reference controller: copy gets a new id
			// ...and its own place at the end. Sharing the original's
			// would leave two events on the same rung of the running
			// order, and which one played first would be an accident.
			copy.order = nextOrderFor(copy.list);
			events_.push_back(copy);
			save();
			return copy.id;
		}
	}
	return 0;
}

int EventStore::lastEventId() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int bestId = 0;
	int64_t bestIn = -1;
	for (const auto &ev : events_) {
		if (ev.tOutNs >= 0 && ev.tInNs >= bestIn) {
			bestIn = ev.tInNs;
			bestId = ev.id;
		}
	}
	return bestId;
}

bool EventStore::get(int id, ReplayEvent &out) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &ev : events_) {
		if (ev.id == id) {
			out = ev;
			return true;
		}
	}
	return false;
}

std::string EventStore::listJson(int list) const
{
	std::lock_guard<std::mutex> lock(mutex_);

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "list", list);
	obs_data_set_bool(root, "liveMode", liveMode_);
	obs_data_set_int(root, "selectedList", selectedList_);

	obs_data_set_string(root, "listName", listNames_[list - 1].c_str());

	// In the RUNNING ORDER, not in the order the vector happens to hold them:
	// this is the order the dock draws and the order a sequence plays in, and
	// it is the operator's to arrange (see moveEvent).
	std::vector<const ReplayEvent *> ordered;
	for (const auto &ev : events_)
		if (ev.list == list)
			ordered.push_back(&ev);
	std::stable_sort(ordered.begin(), ordered.end(),
			 [](const ReplayEvent *a, const ReplayEvent *b) {
				 return a->order < b->order;
			 });

	obs_data_array_t *arr = obs_data_array_create();
	for (const ReplayEvent *evp : ordered) {
		const ReplayEvent &ev = *evp;
		obs_data_t *e = obs_data_create();
		obs_data_set_int(e, "id", ev.id);
		obs_data_set_int(e, "tInNs", ev.tInNs);
		obs_data_set_int(e, "tOutNs", ev.tOutNs);
		obs_data_set_int(e, "order", ev.order);
		obs_data_set_string(e, "createdMode", ev.createdMode.c_str());
		obs_data_array_t *angles = obs_data_array_create();
		for (const auto &a : ev.angles) {
			obs_data_t *ad = obs_data_create();
			obs_data_set_bool(ad, "enabled", a.enabled);
			obs_data_set_string(ad, "note", a.note.c_str());
			obs_data_set_double(ad, "speed", a.speed);
			obs_data_array_push_back(angles, ad);
			obs_data_release(ad);
		}
		obs_data_set_array(e, "angles", angles);
		obs_data_array_release(angles);
		obs_data_array_push_back(arr, e);
		obs_data_release(e);
	}
	obs_data_set_array(root, "events", arr);
	obs_data_array_release(arr);

	std::string json = obs_data_get_json(root);
	obs_data_release(root);
	return json;
}

std::string EventStore::chaptersText(int list, int64_t originNs) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::string out;
	for (const auto &ev : events_) {
		if (ev.list != list || ev.tOutNs < 0)
			continue;
		int64_t relNs = ev.tInNs - originNs;
		if (relNs < 0)
			relNs = 0; // marked before the video starts
		int64_t totalSec = relNs / 1000000000LL;
		int h = (int)(totalSec / 3600);
		int m = (int)((totalSec % 3600) / 60);
		int s = (int)(totalSec % 60);
		char ts[16];
		if (h > 0)
			snprintf(ts, sizeof(ts), "%d:%02d:%02d", h, m, s);
		else
			snprintf(ts, sizeof(ts), "%d:%02d", m, s);
		// First non-empty angle note (lowest index = highest priority).
		std::string desc;
		for (const auto &a : ev.angles) {
			if (!a.note.empty()) {
				desc = a.note;
				break;
			}
		}
		if (desc.empty())
			desc = "Clip " + std::to_string(ev.id);
		out += ts;
		out += ' ';
		out += desc;
		out += '\n';
	}
	return out;
}

void EventStore::save() const
{
	version_++; // notify pollers of a change before persisting
	if (folder_.empty())
		return;

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "nextId", nextId_);

	// The 20 list names travel with the events: they name THIS project's
	// lists ("Gol", "Falli"), not a global preference.
	{
		obs_data_array_t *names = obs_data_array_create();
		for (const auto &n : listNames_) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", n.c_str());
			obs_data_array_push_back(names, item);
			obs_data_release(item);
		}
		obs_data_set_array(root, "listNames", names);
		obs_data_array_release(names);
	}

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &ev : events_) {
		obs_data_t *e = obs_data_create();
		obs_data_set_int(e, "id", ev.id);
		obs_data_set_int(e, "list", ev.list);
		// The running order belongs to the project: an operator who has
		// arranged a highlights reel has not arranged it for one session.
		obs_data_set_int(e, "order", ev.order);
		// Marks go out in ABSOLUTE wall-clock ns, never in master time:
		// master is monotonic system time, exact within the session and
		// meaningless after a restart, so a file written with it could
		// never be lined up with the footage again. The explicit *_wall_ns
		// names exist so nobody mistakes one clock for the other.
		// out_wall_ns == -1 is the open-event sentinel (Mark In with no
		// Mark Out yet); real wall values are ~1.8e18, never negative.
		obs_data_set_int(e, "in_wall_ns",
				 masterToWallNs(epoch_, ev.tInNs));
		obs_data_set_int(e, "out_wall_ns",
				 ev.tOutNs < 0
					 ? -1
					 : masterToWallNs(epoch_, ev.tOutNs));
		// No event-level "speed" is written any more: the only speeds are
		// the per-angle ones below and the operator's slider. An old file
		// that still carries one is simply ignored on load.
		obs_data_set_string(e, "createdMode", ev.createdMode.c_str());
		obs_data_array_t *angles = obs_data_array_create();
		for (const auto &a : ev.angles) {
			obs_data_t *ad = obs_data_create();
			obs_data_set_bool(ad, "enabled", a.enabled);
			obs_data_set_string(ad, "note", a.note.c_str());
			obs_data_set_double(ad, "speed", a.speed);
			obs_data_array_push_back(angles, ad);
			obs_data_release(ad);
		}
		obs_data_set_array(e, "angles", angles);
		obs_data_array_release(angles);
		obs_data_array_push_back(arr, e);
		obs_data_release(e);
	}
	obs_data_set_array(root, "events", arr);
	obs_data_array_release(arr);

	std::filesystem::path p(folder_);
	p /= kEventsFile;
	obs_data_save_json_safe(root, p.string().c_str(), "tmp", "bak");
	obs_data_release(root);
}

void EventStore::load()
{
	std::filesystem::path p(folder_);
	p /= kEventsFile;
	obs_data_t *root =
		obs_data_create_from_json_file(p.string().c_str());
	if (!root)
		return;

	nextId_ = (int)obs_data_get_int(root, "nextId");
	if (nextId_ < 1)
		nextId_ = 1;

	listNames_.fill(std::string());
	if (obs_data_array_t *names = obs_data_get_array(root, "listNames")) {
		const size_t n = obs_data_array_count(names);
		for (size_t i = 0; i < n && i < (size_t)kEventLists; i++) {
			obs_data_t *item = obs_data_array_item(names, i);
			const char *nm = obs_data_get_string(item, "name");
			listNames_[i] = nm ? nm : "";
			obs_data_release(item);
		}
		obs_data_array_release(names);
	}

	int skippedLegacy = 0;
	obs_data_array_t *arr = obs_data_get_array(root, "events");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *e = obs_data_array_item(arr, i);
			// Pre-wall-clock files stored tInNs/tOutNs, i.e. raw
			// monotonic time from a process that no longer exists.
			// There is no epoch that makes those numbers mean
			// anything, so they are dropped rather than loaded as
			// marks pointing at an arbitrary instant.
			if (!obs_data_has_user_value(e, "in_wall_ns")) {
				skippedLegacy++;
				obs_data_release(e);
				continue;
			}
			ReplayEvent ev;
			ev.id = (int)obs_data_get_int(e, "id");
			ev.list = (int)obs_data_get_int(e, "list");
			// Absent in files written before manual ordering existed:
			// 0 for everyone, which normalizeOrder() below turns into
			// the order they are stored in — the order they were
			// marked in, which is what those projects always showed.
			ev.order = (int)obs_data_get_int(e, "order");
			ev.tInNs = wallToMasterNs(
				epoch_, obs_data_get_int(e, "in_wall_ns"));
			const int64_t outWall = obs_data_get_int(e, "out_wall_ns");
			ev.tOutNs = outWall < 0 ? -1
						: wallToMasterNs(epoch_, outWall);
			// "speed" at event level (M3) is deliberately NOT read
			// back: it no longer exists, and silently applying a
			// speed the operator can no longer see or change would
			// be worse than dropping it.
			ev.createdMode =
				obs_data_get_string(e, "createdMode");
			obs_data_array_t *angles =
				obs_data_get_array(e, "angles");
			if (angles) {
				size_t n = obs_data_array_count(angles);
				for (size_t j = 0;
				     j < n && j < (size_t)kEventAngles; j++) {
					obs_data_t *ad = obs_data_array_item(
						angles, j);
					ev.angles[j].enabled =
						obs_data_get_bool(ad,
								  "enabled");
					ev.angles[j].note =
						obs_data_get_string(ad,
								    "note");
					ev.angles[j].speed =
						obs_data_has_user_value(ad,
									"speed")
							? obs_data_get_double(
								  ad, "speed")
							: -1.0;
					obs_data_release(ad);
				}
				obs_data_array_release(angles);
			}
			events_.push_back(std::move(ev));
			obs_data_release(e);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(root);
	normalizeOrder();
	obs_log(LOG_INFO, "EventStore: loaded %zu event(s)", events_.size());
	if (skippedLegacy > 0)
		obs_log(LOG_WARNING,
			"EventStore: dropped %d event(s) written in the old "
			"monotonic-time format - their marks cannot be placed "
			"on any timeline",
			skippedLegacy);
}

} // namespace multireplay
