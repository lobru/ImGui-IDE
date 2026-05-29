#	TextEditor - A syntax highlighting text editor for ImGui
#	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
#
#	This work is licensed under the terms of the MIT license.
#	For a copy, see <https://opensource.org/licenses/MIT>.

cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

# Optionally accelerate (re)builds with ccache when it is available.
option(TEXTEDITOR_CCACHE "Use ccache if found" ON)
if(TEXTEDITOR_CCACHE)
	find_program(CCACHE ccache)
	if(CCACHE)
		set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE}")
		set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE}")
	endif()
endif()

project(TextEditor
	VERSION 1.0
	DESCRIPTION "A syntax highlighting text editor for ImGui"
	LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TEXTEDITOR_BUILD_EXAMPLE "Build the TextEditor example program" ON)

# Dependency resolution strategy.  FETCH downloads pinned releases (the
# default, fully self-contained); LOCAL uses an SDL3 already installed on
# the system.  Dear ImGui and ImGuiFileDialog have no stable system
# packages, so they are always fetched or taken from a bundled copy.
set(DEPS "FETCH" CACHE STRING "Dependency resolution for SDL3: FETCH or LOCAL")
set_property(CACHE DEPS PROPERTY STRINGS FETCH LOCAL)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Each dependency uses a bundled copy under deps/<name> when present,
# otherwise it is downloaded from a pinned, hash-verified release.

# --- SDL3 (FETCH or LOCAL) ---

if(DEPS STREQUAL "LOCAL")
	find_package(SDL3 REQUIRED)
	set(SDL3_HEADERS_TARGET SDL3::Headers)
	message(STATUS "SDL3: system")
else()
	if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/sdl")
		set(FETCHCONTENT_SOURCE_DIR_SDL "${CMAKE_CURRENT_SOURCE_DIR}/deps/sdl")
		set(_sdl_origin "bundled")
	else()
		set(_sdl_origin "downloaded")
	endif()
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		sdl
		URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.4/SDL3-3.4.4.tar.gz
		URL_HASH SHA256=ee712dbe6a89bb140bbfc2ce72358fb5ee5cc2240abeabd54855012db30b3864
	)
	FetchContent_MakeAvailable(sdl)
	set(SDL3_HEADERS_TARGET SDL3_Headers)
	message(STATUS "SDL3: ${_sdl_origin}")

	if(CMAKE_GENERATOR STREQUAL "Xcode")
		set_target_properties(SDL3-static PROPERTIES XCODE_ATTRIBUTE_GCC_WARN_ABOUT_DEPRECATED_FUNCTIONS "NO")
		set_target_properties(SDL3-static PROPERTIES XCODE_ATTRIBUTE_GCC_WARN_64_TO_32_BIT_CONVERSION "NO")
		set_target_properties(SDL3-static PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
	endif()
endif()

# --- Dear ImGui (always bundled or downloaded) ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")
	set(FETCHCONTENT_SOURCE_DIR_IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")
	message(STATUS "Dear ImGui: bundled")
else()
	message(STATUS "Dear ImGui: downloaded")
endif()
FetchContent_Declare(
	imgui
	URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.7.tar.gz
	URL_HASH SHA256=b21b14ce1ef6dd4d85fa54f68f8449a9be94f3b453aba7fe4ea2a9764e43a5ef
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
	${imgui_SOURCE_DIR}/imgui_demo.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)
target_compile_definitions(imgui PUBLIC
	IMGUI_DEFINE_MATH_OPERATORS
	IMGUI_DISABLE_OBSOLETE_FUNCTIONS
	IMGUI_DISABLE_OBSOLETE_KEYIO)
target_include_directories(imgui PUBLIC
	${imgui_SOURCE_DIR}
	${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC ${SDL3_HEADERS_TARGET})

# --- ImGuiFileDialog (always bundled or downloaded) ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/imguifiledialog")
	set(FETCHCONTENT_SOURCE_DIR_IMGUIFD "${CMAKE_CURRENT_SOURCE_DIR}/deps/imguifiledialog")
	message(STATUS "ImGuiFileDialog: bundled")
else()
	message(STATUS "ImGuiFileDialog: downloaded")
endif()
FetchContent_Declare(
	imguifd
	URL https://github.com/goossens/ImGuiFileDialog/archive/d0e97b2adc3d3452d72c750c7305dc0291acd052.tar.gz
	URL_HASH SHA256=04bcd41f54fd57ede0864be200482c196d352d1a5d4498f7cd88fef3cf7029fd
	# Point at a directory with no CMakeLists.txt so MakeAvailable only
	# fetches the sources and skips add_subdirectory().  ImGuiFileDialog's
	# own CMakeLists.txt expects find_package(imgui) and would otherwise
	# add a second, broken target to the default build.
	SOURCE_SUBDIR do-not-build
)
FetchContent_MakeAvailable(imguifd)

add_library(imguifd STATIC ${imguifd_SOURCE_DIR}/ImGuiFileDialog.cpp)
target_compile_definitions(imguifd PUBLIC CUSTOM_IMGUIFILEDIALOG_CONFIG="IgfdConfig.h")
target_include_directories(imguifd PUBLIC
	${imguifd_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/example/deps/imguifiledialog)
target_link_libraries(imguifd PUBLIC imgui)

# --- TextEditor (the widget itself) ---

add_library(TextEditor STATIC
	TextEditor.cpp
	TextEditor.h
	TextDiff.cpp
	TextDiff.h
	dtl.h)
target_include_directories(TextEditor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(TextEditor PUBLIC imgui)
target_compile_options(TextEditor PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
add_library(TextEditor::TextEditor ALIAS TextEditor)

# --- Example program ---

if(TEXTEDITOR_BUILD_EXAMPLE)
	add_executable(example
		example/editor.cpp
		example/dejavu.cpp
		example/main.cpp)
	target_link_libraries(example PRIVATE TextEditor imguifd SDL3::SDL3)
	target_compile_options(example PRIVATE
		$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
		$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
endif()
