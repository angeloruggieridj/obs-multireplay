# A project of its OWN, deliberately: it is configured and built by hand while
# working on the layout, and it must not be able to affect the plugin's build or
# the CI. It needs Qt6 Widgets and nothing else — no libobs, no FFmpeg — which is
# the whole reason the layout was moved into src/dock-layout.
#
#   cmake -S tools/dock-mockup -B build_mockup -DCMAKE_PREFIX_PATH=<qt6 dir>
#   cmake --build build_mockup --config RelWithDebInfo
#   build_mockup/RelWithDebInfo/dock-mockup.exe <out-dir>
cmake_minimum_required(VERSION 3.22)
project(dock-mockup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(dock-mockup
  main.cpp
  ../../src/dock-layout.cpp
)
target_link_libraries(dock-mockup PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
