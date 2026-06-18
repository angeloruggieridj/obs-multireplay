/*
obs-multireplay — no-op obs stubs for the STANDALONE event-store test.
SPDX-License-Identifier: GPL-2.0-or-later

The logic tests never set a session folder, so EventStore::save()/load() return
before touching obs_data and these bodies are never executed at runtime — they
exist only so the real event-store.cpp links into a standalone exe. Definitions
deliberately do as little as possible.
*/

#include <obs-module.h> // FAKE (tests/fakeobs)
#include <cstdarg>

// A single throwaway object every handle points at; never dereferenced because
// save()/load()/listJson() are not exercised by the logic tests.
static char g_dummy;

extern "C" {

obs_data_t *obs_data_create(void) { return (obs_data_t *)&g_dummy; }
obs_data_t *obs_data_create_from_json_file(const char *) { return nullptr; }
void obs_data_release(obs_data_t *) {}

void obs_data_set_int(obs_data_t *, const char *, long long) {}
void obs_data_set_bool(obs_data_t *, const char *, bool) {}
void obs_data_set_double(obs_data_t *, const char *, double) {}
void obs_data_set_string(obs_data_t *, const char *, const char *) {}
void obs_data_set_array(obs_data_t *, const char *, obs_data_array_t *) {}

long long obs_data_get_int(obs_data_t *, const char *) { return 0; }
bool obs_data_get_bool(obs_data_t *, const char *) { return false; }
double obs_data_get_double(obs_data_t *, const char *) { return 0.0; }
const char *obs_data_get_string(obs_data_t *, const char *) { return ""; }
bool obs_data_has_user_value(obs_data_t *, const char *) { return false; }
obs_data_array_t *obs_data_get_array(obs_data_t *, const char *)
{
	return nullptr;
}
const char *obs_data_get_json(obs_data_t *) { return "{}"; }

bool obs_data_save_json_safe(obs_data_t *, const char *, const char *,
			     const char *)
{
	return true;
}

obs_data_array_t *obs_data_array_create(void)
{
	return (obs_data_array_t *)&g_dummy;
}
void obs_data_array_release(obs_data_array_t *) {}
size_t obs_data_array_count(obs_data_array_t *) { return 0; }
obs_data_t *obs_data_array_item(obs_data_array_t *, size_t) { return nullptr; }
size_t obs_data_array_push_back(obs_data_array_t *, obs_data_t *) { return 0; }

// Declared in plugin-support.h (the real header is used).
void obs_log(int, const char *, ...) {}

} // extern "C"

// Declared in replay-core.hpp; keep diagnostic logging off in tests.
namespace multireplay {
bool debugLoggingEnabled() { return false; }
}
