# obs-multireplay — the unit tests, buildable WITHOUT OBS, Qt or FFmpeg.
#
# This file works two ways on purpose:
#
#   add_subdirectory(tests)                     from the plugin build
#   cmake -S tests -B build_tests && ctest      on its own
#
# The second is what the CI uses, and it is the point of the file. The tests
# were compiled by the plugin build and never run by anything (P1): 66 checks
# written, 0 executed automatically, on a project whose own audit calls the
# unit tests the thing that has to grow. Configuring the plugin needs obs-deps,
# Qt6 and FFmpeg on three platforms; these targets need a C++20 compiler, so
# they can be a merge gate on every push instead of a thing somebody remembers.

cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(obs-multireplay-tests LANGUAGES CXX)
endif()

enable_testing()

set(MR_SRC "${CMAKE_CURRENT_LIST_DIR}/../src")
set(MR_TESTS "${CMAKE_CURRENT_LIST_DIR}/standalone")
set(MR_FAKEOBS "${CMAKE_CURRENT_LIST_DIR}/fakeobs")

# Warnings are the point of running these somewhere other than MSVC: two of the
# three portability build breaks this project has had were things MSVC accepts
# and clang refuses (std::max(int64_t, 1LL), a re-declared class member).
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

function(mr_add_test name ctest_name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${MR_SRC}")
  target_compile_features(${name} PRIVATE cxx_std_20)
  add_test(NAME ${ctest_name} COMMAND ${name})
endfunction()

# EventStore logic test: compiles the REAL src/event-store.cpp against a tiny
# fake obs surface (tests/fakeobs) + no-op stubs. No libobs/FFmpeg needed, so it
# runs anywhere, CI included. tests/fakeobs is PRIVATE to this target only and
# never reaches the plugin build.
mr_add_test(
  obs-multireplay-eventstore-tests
  event_store
  "${MR_TESTS}/event_store_test.cpp"
  "${MR_TESTS}/obs_stub.cpp"
  "${MR_SRC}/event-store.cpp"
)
target_include_directories(obs-multireplay-eventstore-tests PRIVATE "${MR_FAKEOBS}")
find_package(Threads REQUIRED)
target_link_libraries(obs-multireplay-eventstore-tests PRIVATE Threads::Threads)

# Live packet ring + master timeline + the reverse schedule + the audio stretch.
# These carry the correctness of the rewrite (cross-angle marker equality,
# refuse-never-clamp, "backwards loses no frame and no more RAM than budgeted"),
# and they are written free of OBS/FFmpeg types precisely so they can be tested
# here.
mr_add_test(
  obs-multireplay-packet-tests
  packet_ring
  "${MR_TESTS}/packet_ring_test.cpp"
  "${MR_SRC}/packet-ring.cpp"
)

# M4 health rules. Thresholds are opinions about numbers, and this is the only
# place they can be argued with cheaply: no OBS, no disk, no cameras, so the
# "disk is full" and "the angle died" cases are testable at all.
mr_add_test(obs-multireplay-health-tests health_rules "${MR_TESTS}/health_rules_test.cpp")

# Version comparison, for the updater. "1.0.0 is newer than 1.0.0-beta2" and
# "beta10 is newer than beta2" are the two an updater gets wrong in front of an
# operator.
mr_add_test(obs-multireplay-version-tests version_compare "${MR_TESTS}/version_compare_test.cpp")

# The rest of the updater: the digest that says the download is what was
# announced (B1), the installer script that carries no path (B2), and the asset
# choice that refuses another platform instead of falling back to it (A2).
mr_add_test(obs-multireplay-updater-tests updater "${MR_TESTS}/updater_test.cpp")

# Paths as bytes (A3) and what a project title becomes on disk (M9, M10). The
# encoding bug these pin is invisible on Linux and macOS and breaks Windows, so
# it matters that this target runs on all three.
mr_add_test(obs-multireplay-paths-tests paths "${MR_TESTS}/paths_test.cpp")

# Which camera slot owns the Branch Output filter/encoder when several slots
# name the same source. Pins the fix for "eight identical filters on one
# source, and only the first camera's preview ever shows a picture" — a
# resource-exhaustion bug that is otherwise only visible on a real GPU.
mr_add_test(obs-multireplay-camera-dedup-tests camera_dedup "${MR_TESTS}/camera_dedup_test.cpp")

# Which angle a numbered hotkey means when two bays exist, and what a bay
# switch must do to the value they share so switching AWAY from a bay does
# not leave the value the OTHER bay reads from stale.
mr_add_test(obs-multireplay-angle-channels-tests angle_channels "${MR_TESTS}/angle_channels_test.cpp")
