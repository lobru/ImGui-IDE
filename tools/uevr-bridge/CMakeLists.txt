cmake_minimum_required(VERSION 3.20)
project(imgui_ide_uevr_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at your UEVR checkout so we can find include/uevr/Plugin.hpp.
# Defaults to the layout used by the UEVR examples tree (this dir copied under
# <UEVR>/examples/imgui_ide_uevr_bridge/); override with -DUEVR_ROOT=<path>.
set(UEVR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "UEVR checkout root")

if(NOT EXISTS "${UEVR_ROOT}/include/uevr/Plugin.hpp")
    message(FATAL_ERROR
        "UEVR SDK not found at ${UEVR_ROOT}/include/uevr/Plugin.hpp. "
        "Pass -DUEVR_ROOT=<path-to-UEVR-checkout> at configure time.")
endif()

# Headless: no ImGui, no ImGuiColorTextEdit — just the UEVR SDK headers.
add_library(imgui_ide_uevr_bridge SHARED Plugin.cpp)

target_include_directories(imgui_ide_uevr_bridge PRIVATE "${UEVR_ROOT}/include")

if(MSVC)
    target_compile_options(imgui_ide_uevr_bridge PRIVATE
        /W3 /permissive- /Zc:__cplusplus /utf-8)
endif()

set_target_properties(imgui_ide_uevr_bridge PROPERTIES
    OUTPUT_NAME "imgui_ide_uevr_bridge"
    SUFFIX ".dll")
