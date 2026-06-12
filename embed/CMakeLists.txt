#	TextEditor embed DLL - standalone build
#	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
#
#	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
#	This work is licensed under the terms of the MIT license.
#	For a copy, see <https://opensource.org/licenses/MIT>.
#
#	Builds texteditor_embed.dll/.so: the TextEditor core + a flat C API +
#	a private copy of Dear ImGui (no backends - the HOST renders; we draw into
#	its context). Standalone on purpose: this file does not touch the desktop
#	IDE build under example/, so the two can evolve independently.
#
#	  cmake -B build-embed -S embed
#	  cmake --build build-embed --config Release
#
#	The ImGui version MUST match the host's (same IMGUI_VERSION_NUM, same
#	config defines). Two ways to control it:
#	  -DTE_EMBED_IMGUI_TAG=v1.92.7-docking   # git tag to fetch (default)
#	  -DTE_EMBED_IMGUI_DIR=path/to/imgui     # use a local checkout (wins)
#	Hosts verify at runtime via te_imgui_version_num().

cmake_minimum_required(VERSION 3.17 FATAL_ERROR)
project(TextEditorEmbed VERSION 1.0 DESCRIPTION "Embeddable TextEditor DLL" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TE_EMBED_IMGUI_TAG "v1.92.7-docking" CACHE STRING "Dear ImGui git tag to build against (match your host!)")
set(TE_EMBED_IMGUI_DIR "" CACHE PATH "Local Dear ImGui checkout (overrides TE_EMBED_IMGUI_TAG)")

if(TE_EMBED_IMGUI_DIR)
	set(imgui_SOURCE_DIR "${TE_EMBED_IMGUI_DIR}")
else()
	include(FetchContent)
	FetchContent_Declare(imgui
		GIT_REPOSITORY "https://github.com/ocornut/imgui"
		GIT_TAG "${TE_EMBED_IMGUI_TAG}")
	FetchContent_MakeAvailable(imgui)
endif()

add_library(texteditor_embed SHARED
	../TextEditor.cpp
	../TextEditor.h
	../TextDiff.cpp
	../TextDiff.h
	../dtl.h
	texteditor_embed.cpp
	texteditor_embed.h
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp)

target_include_directories(texteditor_embed
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE .. ${imgui_SOURCE_DIR})

#	TE_EMBED_BUILD selects __declspec(dllexport) in the header; everything
#	else (the bundled ImGui, the C++ TextEditor class) stays internal -
#	hidden visibility on ELF, no dllexport on Windows - so the host can link
#	its own ImGui without symbol collisions.
target_compile_definitions(texteditor_embed PRIVATE TE_EMBED_BUILD=1)

set_target_properties(texteditor_embed PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
	POSITION_INDEPENDENT_CODE ON)

#	Same warnings policy as the rest of the project for OUR sources; the
#	bundled ImGui translation units build with default warnings (upstream code,
#	not ours to fix).
set(TE_EMBED_OWN_SOURCES
	../TextEditor.cpp
	../TextDiff.cpp
	texteditor_embed.cpp)

foreach(src ${TE_EMBED_OWN_SOURCES})
	set_property(SOURCE ${src} APPEND PROPERTY COMPILE_OPTIONS
		$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Werror;-Wall;-Wpedantic;-Wextra>
		$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX>)
endforeach()

#	── headless integration test ───────────────────────────────────────
#	The test executable plays the HOST: it compiles its own ImGui (null
#	backends, no window/GPU) and drives the DLL through the C API. This is
#	the cross-binary contract test - run it on every platform that builds
#	the DLL.
option(TE_EMBED_BUILD_TESTS "Build the headless render test" ON)

if(TE_EMBED_BUILD_TESTS)
	enable_testing()

	add_executable(te_render_test
		tests/render_test.cpp
		${imgui_SOURCE_DIR}/imgui.cpp
		${imgui_SOURCE_DIR}/imgui_draw.cpp
		${imgui_SOURCE_DIR}/imgui_tables.cpp
		${imgui_SOURCE_DIR}/imgui_widgets.cpp
		${imgui_SOURCE_DIR}/backends/imgui_impl_null.cpp)

	target_include_directories(te_render_test PRIVATE
		${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

	target_link_libraries(te_render_test PRIVATE texteditor_embed)

	set_property(SOURCE tests/render_test.cpp APPEND PROPERTY COMPILE_OPTIONS
		$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Werror;-Wall;-Wpedantic;-Wextra>
		$<$<CXX_COMPILER_ID:MSVC>:/W4;/WX>)

	add_test(NAME te_render_test COMMAND te_render_test)

	#	the test loads the DLL at runtime; keep it next to the executable on
	#	Windows so no PATH fiddling is needed
	if(WIN32)
		add_custom_command(TARGET te_render_test POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_FILE:texteditor_embed> $<TARGET_FILE_DIR:te_render_test>)
	endif()
endif()
