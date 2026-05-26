#	TextEditor - A syntax highlighting text editor for ImGui
#	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
#
#	This work is licensed under the terms of the MIT license.
#	For a copy, see <https://opensource.org/licenses/MIT>.

cmake_minimum_required(VERSION 3.17 FATAL_ERROR)
project(TextEditor VERSION 1.0 DESCRIPTION "TextEditor Example Program" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(deps)

# ── TextEditor library target ────────────────────────────────────────────
# Built as a SHARED (DLL/.so) library when -DTEXTEDITOR_BUILD_SHARED=ON,
# otherwise as a STATIC library that's linked directly into `example`.
option(TEXTEDITOR_BUILD_SHARED "Build TextEditor as a shared library (DLL)" OFF)

set(TEXTEDITOR_LIB_TYPE STATIC)
if(TEXTEDITOR_BUILD_SHARED)
	set(TEXTEDITOR_LIB_TYPE SHARED)
endif()

add_library(TextEditorLib ${TEXTEDITOR_LIB_TYPE}
	../TextEditor.cpp
	../TextEditor.h
	../TextDiff.cpp
	../TextDiff.h
	../dtl.h)

target_include_directories(TextEditorLib PUBLIC ..)
target_link_libraries(TextEditorLib PUBLIC imguifd)

if(TEXTEDITOR_BUILD_SHARED)
	# Easy mode: export everything; tighten later with TEXTEDITOR_API
	# macros if there's a need to keep symbol surface small.
	set_target_properties(TextEditorLib PROPERTIES
		WINDOWS_EXPORT_ALL_SYMBOLS ON
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON)
	target_compile_definitions(TextEditorLib PUBLIC TEXTEDITOR_SHARED=1)
endif()

# Warnings policy applies to TextEditorLib too — it's the same codebase.
target_compile_options(TextEditorLib PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# ── Example executable ───────────────────────────────────────────────────
add_executable(example
	editor.cpp
	dejavu.cpp
	main.cpp)

target_link_libraries(example PRIVATE TextEditorLib SDL3::SDL3 SDL3_Headers imguifd)

# The example executable owns main(); the editor library doesn't.
target_compile_definitions(example PRIVATE STANDALONE=1)

target_compile_options(example PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
