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

int EventStore::markIn(int64_t tNs, int angle0Based)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ReplayEvent ev;
	ev.id = nextId_++;
	ev.list = selectedList_;
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

bool EventStore::setSpeed(int id, double speed)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &ev : events_) {
		if (ev.id == id) {
			ev.speed = speed < 0 ? -1.0
					     : std::clamp(speed, 0.01, 1.0);
			save();
			return true;
		}
	}
	return false;
}

double EventStore::resolvedSpeedAt(size_t idx) const
{
	// mutex_ held by the caller.
	//
	// "Previous" is previous in CREATION order, which is the order marks were
	// taken and the order the store keeps them in. Deliberately not the sorted
	// display order: with the by-time sort on, an event inserted before others
	// (a −20s preset taken after a −5s one) would otherwise change the speed of
	// events that were already on screen with a value the operator had read.
	const int list = events_[idx].list;
	for (size_t i = idx + 1; i-- > 0;) {
		if (events_[i].list != list)
			continue;
		if (events_[i].speed >= 0)
			return events_[i].speed;
	}
	return -1.0;
}

double EventStore::resolvedSpeed(int id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (size_t i = 0; i < events_.size(); i++)
		if (events_[i].id == id)
			return resolvedSpeedAt(i);
	return -1.0;
}

bool EventStore::setAngleSpeed(int id, int angle1Based, double speed)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (angle1Based < 1 || angle1Based > kEventAngles)
		return false;
	for (auto &ev : events_) {
		if (ev.id == id) {
			ev.angles[angle1Based - 1].speed =
				speed < 0 ? -1.0
					  : std::clamp(speed, 0.01, 1.0);
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
			ev.list = list;
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

	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0; i < events_.size(); i++) {
		const auto &ev = events_[i];
		if (ev.list != list)
			continue;
		obs_data_t *e = obs_data_create();
		obs_data_set_int(e, "id", ev.id);
		obs_data_set_int(e, "tInNs", ev.tInNs);
		obs_data_set_int(e, "tOutNs", ev.tOutNs);
		obs_data_set_double(e, "speed", ev.speed);
		// What would actually play, inheritance included, so the table can
		// show an inherited speed without asking again per row.
		obs_data_set_double(e, "resolvedSpeed", resolvedSpeedAt(i));
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
		obs_data_set_double(e, "speed", ev.speed);
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
			ev.tInNs = wallToMasterNs(
				epoch_, obs_data_get_int(e, "in_wall_ns"));
			const int64_t outWall = obs_data_get_int(e, "out_wall_ns");
			ev.tOutNs = outWall < 0 ? -1
						: wallToMasterNs(epoch_, outWall);
			ev.speed = obs_data_get_double(e, "speed");
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
	obs_log(LOG_INFO, "EventStore: loaded %zu event(s)", events_.size());
	if (skippedLegacy > 0)
		obs_log(LOG_WARNING,
			"EventStore: dropped %d event(s) written in the old "
			"monotonic-time format - their marks cannot be placed "
			"on any timeline",
			skippedLegacy);
}

} // namespace multireplay
