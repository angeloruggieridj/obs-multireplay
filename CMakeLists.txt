cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Gui REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# C++20 for the plugin sources (std::filesystem, designated initializers)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Threads::Threads)

# FFmpeg for the playback engine (decode of recorded segments).
# Finder copied from obs-studio (cmake/finders); obs-deps prebuilt provide
# FFmpeg on Windows/macOS, system packages on Linux (libavformat-dev etc.).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/replay-core.cpp
    src/branch-output-control.cpp
    src/session-index.cpp
    # M0: live encoded-packet tap on Branch Output's existing encoders
    src/packet-tap.cpp
    src/packet-tap.hpp
    # M1: master timeline + bounded in-memory packet history (the live edge)
    src/packet-types.hpp
    src/master-timeline.hpp
    src/packet-ring.cpp
    src/packet-ring.hpp
    # M2: decode the ring back into frames, and "Replay A" as a real OBS input
    src/replay-decoder.cpp
    src/replay-decoder.hpp
    src/replay-channel.cpp
    src/replay-channel.hpp
    src/segment-anchor.hpp
    # Scripted M0 gate (no-op unless OBS_MULTIREPLAY_SELFTEST is set)
    src/selftest.cpp
    src/selftest.hpp
    # Playback engine: OBS Media Source (ffmpeg_source) driver
    src/media-replay.cpp
    src/media-replay.hpp
    src/event-store.cpp
    src/playback-coordinator.cpp
    src/export.cpp
    # Native Qt dock (replaces the former browser UI)
    src/qt-display.cpp
    src/qt-display.hpp
    src/multireplay-dock.cpp
    src/multireplay-dock.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# --- Unit smoke tests (pure timeline math, no OBS/FFmpeg deps) ---------------
# Standalone exe; run with `ctest` from the build dir. Builds with the project.
option(ENABLE_TESTS "Build obs-multireplay unit tests" ON)
if(ENABLE_TESTS)
  enable_testing()
  add_executable(obs-multireplay-tests tests/timeline_math_test.cpp)
  target_include_directories(obs-multireplay-tests PRIVATE src)
  target_compile_features(obs-multireplay-tests PRIVATE cxx_std_20)
  add_test(NAME timeline_math COMMAND obs-multireplay-tests)

  # EventStore logic test: compiles the REAL src/event-store.cpp against a tiny
  # fake obs surface (tests/fakeobs) + no-op stubs. No libobs/FFmpeg needed, so
  # it runs everywhere the timeline-math test does. tests/fakeobs is PRIVATE to
  # this target only and never reaches the plugin build.
  add_executable(
    obs-multireplay-eventstore-tests
    tests/standalone/event_store_test.cpp
    tests/standalone/obs_stub.cpp
    src/event-store.cpp
  )
  target_include_directories(
    obs-multireplay-eventstore-tests PRIVATE tests/fakeobs src
  )
  target_compile_features(obs-multireplay-eventstore-tests PRIVATE cxx_std_20)
  add_test(NAME event_store COMMAND obs-multireplay-eventstore-tests)

  # Live packet ring + master timeline. These carry the correctness of the
  # rewrite (cross-angle marker equality, refuse-never-clamp), and they are
  # written free of OBS/FFmpeg types precisely so they can be tested here.
  add_executable(
    obs-multireplay-packet-tests
    tests/standalone/packet_ring_test.cpp
    src/packet-ring.cpp
  )
  target_include_directories(obs-multireplay-packet-tests PRIVATE src)
  target_compile_features(obs-multireplay-packet-tests PRIVATE cxx_std_20)
  add_test(NAME packet_ring COMMAND obs-multireplay-packet-tests)
endif()
