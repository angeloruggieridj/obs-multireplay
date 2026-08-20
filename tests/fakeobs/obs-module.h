/*
obs-multireplay — FAKE obs-module.h for STANDALONE unit tests ONLY.
SPDX-License-Identifier: GPL-2.0-or-later

This is NOT the real OBS header. It declares the tiny obs surface that the
pure-logic sources (event-store.cpp via replay-core.hpp) reference so they can
be compiled and linked into a standalone test executable without libobs.
The matching no-op definitions live in tests/standalone/obs_stub.cpp.

It is only ever on the include path for the manual standalone test build
(tests/standalone) — the CMake project never sees this directory.
*/

#pragma once

#include <cstdint>
#include <cstddef>

// --- log levels (values mirror libobs; only used as opaque ints here) -------
#define LOG_ERROR 100
#define LOG_WARNING 200
#define LOG_INFO 300
#define LOG_DEBUG 400

// --- hotkeys (referenced only in replay-core.hpp declarations) --------------
typedef size_t obs_hotkey_id;
#define OBS_INVALID_HOTKEY_ID ((obs_hotkey_id)(~0ULL))

// --- opaque obs_data types --------------------------------------------------
struct obs_data;
struct obs_data_array;
typedef struct obs_data obs_data_t;
typedef struct obs_data_array obs_data_array_t;

#ifdef __cplusplus
extern "C" {
#endif

obs_data_t *obs_data_create(void);
obs_data_t *obs_data_create_from_json_file(const char *file);
obs_data_t *obs_data_create_from_json(const char *json);
void obs_data_release(obs_data_t *data);

void obs_data_set_int(obs_data_t *data, const char *name, long long val);
void obs_data_set_bool(obs_data_t *data, const char *name, bool val);
void obs_data_set_double(obs_data_t *data, const char *name, double val);
void obs_data_set_string(obs_data_t *data, const char *name, const char *val);
void obs_data_set_array(obs_data_t *data, const char *name,
			obs_data_array_t *array);

long long obs_data_get_int(obs_data_t *data, const char *name);
bool obs_data_get_bool(obs_data_t *data, const char *name);
double obs_data_get_double(obs_data_t *data, const char *name);
const char *obs_data_get_string(obs_data_t *data, const char *name);
bool obs_data_has_user_value(obs_data_t *data, const char *name);
obs_data_array_t *obs_data_get_array(obs_data_t *data, const char *name);
const char *obs_data_get_json(obs_data_t *data);

bool obs_data_save_json_safe(obs_data_t *data, const char *file,
			     const char *backup_ext, const char *temp_ext);

obs_data_array_t *obs_data_array_create(void);
void obs_data_array_release(obs_data_array_t *array);
size_t obs_data_array_count(obs_data_array_t *array);
obs_data_t *obs_data_array_item(obs_data_array_t *array, size_t idx);
size_t obs_data_array_push_back(obs_data_array_t *array, obs_data_t *obj);

#ifdef __cplusplus
}
#endif
