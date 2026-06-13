/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "event-store.hpp"

// obs-module.h must come before plugin-support.h (MSVC blogva linkage).
#include <obs-module.h>
#include "plugin-support.h"

#include <algorithm>
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

void EventStore::setSessionFolder(const std::string &folder)
{
	std::lock_guard<std::mutex> lock(mutex_);
	folder_ = folder;
	events_.clear();
	nextId_ = 1;
	load();
}

void EventStore::clearAll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	events_.clear();
	// the reference controller keeps settings; ids restart with the session
	nextId_ = 1;
	save();
}

void EventStore::selectList(int list)
{
	if (list >= 1 && list <= kEventLists)
		selectedList_ = list;
}

int EventStore::markIn(int64_t tNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ReplayEvent ev;
	ev.id = nextId_++;
	ev.list = selectedList_;
	ev.tInNs = std::max<int64_t>(0, tNs);
	ev.tOutNs = -1;
	ev.angles[0].enabled = true; // default angle: camera 1 (the reference controller default)
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
			it->tOutNs = std::max(it->tInNs + 1, tNs);
			save();
			return true;
		}
	}
	return false;
}

int EventStore::markInOut(int64_t tNowNs, int seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ReplayEvent ev;
	ev.id = nextId_++;
	ev.list = selectedList_;
	ev.tOutNs = std::max<int64_t>(1, tNowNs);
	ev.tInNs = std::max<int64_t>(0,
				     ev.tOutNs - (int64_t)seconds * 1000000000);
	ev.angles[0].enabled = true;
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

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &ev : events_) {
		if (ev.list != list)
			continue;
		obs_data_t *e = obs_data_create();
		obs_data_set_int(e, "id", ev.id);
		obs_data_set_int(e, "tInNs", ev.tInNs);
		obs_data_set_int(e, "tOutNs", ev.tOutNs);
		obs_data_set_double(e, "speed", ev.speed);
		obs_data_set_string(e, "createdMode", ev.createdMode.c_str());
		obs_data_array_t *angles = obs_data_array_create();
		for (const auto &a : ev.angles) {
			obs_data_t *ad = obs_data_create();
			obs_data_set_bool(ad, "enabled", a.enabled);
			obs_data_set_string(ad, "note", a.note.c_str());
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

void EventStore::save() const
{
	if (folder_.empty())
		return;

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "nextId", nextId_);
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &ev : events_) {
		obs_data_t *e = obs_data_create();
		obs_data_set_int(e, "id", ev.id);
		obs_data_set_int(e, "list", ev.list);
		obs_data_set_int(e, "tInNs", ev.tInNs);
		obs_data_set_int(e, "tOutNs", ev.tOutNs);
		obs_data_set_double(e, "speed", ev.speed);
		obs_data_set_string(e, "createdMode", ev.createdMode.c_str());
		obs_data_array_t *angles = obs_data_array_create();
		for (const auto &a : ev.angles) {
			obs_data_t *ad = obs_data_create();
			obs_data_set_bool(ad, "enabled", a.enabled);
			obs_data_set_string(ad, "note", a.note.c_str());
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

	obs_data_array_t *arr = obs_data_get_array(root, "events");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *e = obs_data_array_item(arr, i);
			ReplayEvent ev;
			ev.id = (int)obs_data_get_int(e, "id");
			ev.list = (int)obs_data_get_int(e, "list");
			ev.tInNs = obs_data_get_int(e, "tInNs");
			ev.tOutNs = obs_data_get_int(e, "tOutNs");
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
}

} // namespace multireplay
