cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# ── THE VERSION THE BINARY REPORTS, PRE-RELEASE SUFFIX AND ALL ───────────────
#
# project(VERSION) will not take "1.0.0-beta7", so the numbers live in
# buildspec's `version` and the suffix in its `prerelease` — and the two are put
# back together here, for plugin-support.c.in and therefore for
# Updater::currentVersion().
#
# This is not cosmetic. Until it existed, a build tagged 1.0.0-beta7 reported
# itself as "1.0.0" — and 1.0.0 is NEWER than every 1.0.0-betaN (a final release
# is later than its own pre-releases, see version-compare.hpp). So the updater
# compared beta8 against 1.0.0, found it older, and told an operator running a
# beta that he had the newest build on the channel. Every beta after the one he
# installed was invisible to him, and so was 1.0.0 itself, until 1.0.1 shipped.
#
# The tag and this pair CANNOT drift: push.yaml recomposes the same string from
# buildspec and refuses to publish a release whose tag disagrees with it.
string(JSON _prerelease ERROR_VARIABLE _prerelease_err GET ${buildspec} prerelease)
if(_prerelease_err OR _prerelease STREQUAL "NOTFOUND")
  set(_prerelease "")
endif()
if(_prerelease STREQUAL "")
  set(PLUGIN_VERSION_FULL "${CMAKE_PROJECT_VERSION}")
else()
  set(PLUGIN_VERSION_FULL "${CMAKE_PROJECT_VERSION}-${_prerelease}")
endif()
message(STATUS "obs-multireplay reports itself as ${PLUGIN_VERSION_FULL}")

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

# libcurl, for the updater. obs-deps ships it on Windows and macOS and OBS
# itself already loads it at runtime, so nothing extra is deployed; on Linux it
# is the system package (libcurl4-openssl-dev in CI).
#
# NOT Qt Network, and the reason is in the logs of the machine this was written
# on: "No functional TLS backend was found", eleven times, at every OBS start.
# An HTTPS request through Qt would fail on exactly the rig we have.
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

# FFmpeg for the playback engine (decode of recorded segments).
# Finder copied from obs-studio (cmake/finders); obs-deps prebuilt provide
# FFmpeg on Windows/macOS, system packages on Linux (libavformat-dev etc.).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")
find_package(
  FFmpeg
  REQUIRED
  COMPONENTS avcodec avformat avutil swscale swresample
)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/replay-core.cpp
    src/branch-output-control.cpp
    # Fetching and verifying the Branch Output INSTALLER, which is not an update
    # of ours: without that plugin this one cannot record at all, so a fresh
    # machine is offered the file its own platform knows how to install.
    src/branch-output-install.cpp
    src/branch-output-install.hpp
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
    # Checking for, fetching and staging a newer build. The comparison it turns
    # on is pure and unit tested (version-compare.hpp): whether 1.0.0 beats
    # 1.0.0-beta2 cannot be tested by running the plugin.
    src/updater.cpp
    src/updater.hpp
    src/version-compare.hpp
    # ...and the three decisions inside it that cannot be tested by running the
    # plugin: which asset is for THIS platform, whether the bytes that arrived
    # are the ones the release announced, and what the install helper says.
    src/update-asset.hpp
    src/update-installer.hpp
    src/sha256.hpp
    # the download size cap acts on bytes written, not a declared Content-Length
    src/size-guard.hpp
    # v1.3: which packets to decode, in which order, to play a range backwards
    src/reverse-plan.hpp
    # slow motion WITH SOUND: a time stretch that leaves the pitch alone
    src/audio-stretch.hpp
    # paths are UTF-8 everywhere, and a project title is not a folder name
    src/path-utf8.hpp
    src/project-name.hpp
    src/segment-anchor.hpp
    # monotonic <-> wall clock: what makes a project survive an OBS restart
    src/session-clock.hpp
    # the position bar's axis: recorded spans, joined, no wall-clock holes
    src/timeline-map.hpp
    src/segment-index.cpp
    src/segment-index.hpp
    src/segment-reader.cpp
    src/segment-reader.hpp
    # M4: pre-flight + runtime health. Findings only; never touches Program.
    src/health-rules.hpp
    src/health.cpp
    src/health.hpp
    # Scripted M0 gate (no-op unless OBS_MULTIREPLAY_SELFTEST is set)
    src/selftest.cpp
    src/selftest.hpp
    src/event-store.cpp
    src/playback-coordinator.cpp
    src/export.cpp
    # Native Qt dock (replaces the former browser UI)
    src/qt-display.cpp
    src/qt-display.hpp
    src/multireplay-dock.cpp
    src/multireplay-dock.hpp
    # which bay a numbered hotkey means, and a localized headline for a known
    # engine rejection
    src/angle-channels.hpp
    src/error-locale.hpp
    src/dock-internal.hpp
    src/dock-window.cpp
    src/dock-settings.cpp
    src/dock-poll.cpp
    src/dock-layout.cpp
    src/dock-layout.hpp
    src/dock-style.hpp
    src/dock-icons.cpp
    src/dock-icons.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# --- Unit tests (pure logic, no OBS/FFmpeg deps) -----------------------------
# The targets live in tests/CMakeLists.txt, which is ALSO a project in its own
# right: the CI configures that directory directly and runs ctest on all three
# platforms, without obs-deps, Qt or FFmpeg. Building them here as well keeps
# "ctest --test-dir build_x64" working for local iteration.
option(ENABLE_TESTS "Build obs-multireplay unit tests" ON)
if(ENABLE_TESTS)
  # At the TOP level, before the subdirectory: without it CTest writes no
  # subdirs() entry and "ctest --test-dir build_x64" reports the tests that
  # happened to be registered last time rather than the ones that exist.
  enable_testing()
  add_subdirectory(tests)
endif()
