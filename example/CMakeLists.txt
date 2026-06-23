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

# ── Tree-sitter symbol index ─────────────────────────────────────────────
# Pure logic (parse → tagged symbols), no ImGui/SDL — links into both the app
# and the headless self-test. Pulls in the isolated treesitter dep.
add_library(tsindex STATIC tsindex.cpp tsindex.h)
target_link_libraries(tsindex PUBLIC treesitter)
target_include_directories(tsindex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tsindex PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# ── LSP protocol layer ───────────────────────────────────────────────────
# Pure JSON-RPC framing/builders/parsers + UTF offset conversion (no SDL), so
# the headless selftest can link it. The transport (lsp_client) adds SDL.
add_library(lspproto STATIC lsp_protocol.cpp lsp_protocol.h)
target_link_libraries(lspproto PUBLIC nlohmann_json)
target_include_directories(lspproto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lspproto PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# ── LSP client transport (spawns clangd via SDL3 process API) ─────────────
add_library(lspclient STATIC lsp_client.cpp lsp_client.h)
target_link_libraries(lspclient PUBLIC lspproto SDL3::SDL3 SDL3_Headers)
target_include_directories(lspclient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lspclient PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# ── Example executable ───────────────────────────────────────────────────
add_executable(example
	editor.cpp
	updater.cpp
	dejavu.cpp
	main.cpp)

target_link_libraries(example PRIVATE TextEditorLib SDL3::SDL3 SDL3_Headers imguifd tsindex lspclient)

# In-app updater talks to GitHub over HTTPS via WinHTTP (Windows only).
if(WIN32)
	target_link_libraries(example PRIVATE winhttp)
endif()

# Brand the output binary ImGui-IDE(.exe). The CMake target stays `example` (so
# build scripts / object paths are unchanged) — only the produced file is renamed.
set_target_properties(example PROPERTIES OUTPUT_NAME "ImGui-IDE")

# The example executable owns main(); the editor library doesn't.
target_compile_definitions(example PRIVATE STANDALONE=1)

# Version string from git: tag for a release ("1.0"), tag-commits-sha for nightly
# ("1.0-7-gabc1234"), or a short sha if untagged. Surfaced as IMGUI_IDE_VERSION
# and shown in About / used by the in-app updater's newer-than check.
execute_process(
	COMMAND git describe --tags --always --dirty
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
	OUTPUT_VARIABLE IMGUI_IDE_GIT_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(NOT IMGUI_IDE_GIT_VERSION)
	set(IMGUI_IDE_GIT_VERSION "0.0.0-dev")
endif()
target_compile_definitions(example PRIVATE IMGUI_IDE_VERSION="${IMGUI_IDE_GIT_VERSION}")
message(STATUS "ImGui-IDE version: ${IMGUI_IDE_GIT_VERSION}")

# Link as a GUI (WINDOWS) subsystem app so no console window appears when the
# editor is launched from Explorer / a shortcut. main() stays the entry point
# via /ENTRY:mainCRTStartup (the WINDOWS subsystem would otherwise want WinMain).
# main.cpp re-attaches to a parent terminal's console at runtime so command-line
# launches keep their stdout/stderr.
if(WIN32 AND MSVC)
	set_target_properties(example PROPERTIES WIN32_EXECUTABLE ON)
	target_link_options(example PRIVATE /ENTRY:mainCRTStartup)
	# Embed the app icon (Explorer + taskbar) via the Windows resource script.
	target_sources(example PRIVATE app.rc)
endif()

target_compile_options(example PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# Sync the runtime language definitions (languages/*.lang) next to the built
# executable after every build. loadRuntimeLanguages() looks in <exe-dir>/languages
# first, so without this the deployed copy goes stale — newly added .lang files
# (xaml, xml, cmake, …) silently never load. copy_directory_if_different only
# touches changed files, so it's cheap on no-op rebuilds.
add_custom_command(TARGET example POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
		"${CMAKE_CURRENT_SOURCE_DIR}/languages"
		"$<TARGET_FILE_DIR:example>/languages"
	COMMENT "Syncing languages/ next to example.exe")

# ── Headless logic self-test ─────────────────────────────────────────────
# A tiny console executable that exercises the pure (non-UI) code paths —
# colorizer, fold detection — with no SDL/GPU/ImGui context. It links the same
# TextEditorLib + imgui (for ImFont/ImVec2 types) but never creates a context,
# so it runs in CI and even while example.exe is open. `ctest` or run directly.
add_executable(selftest selftest.cpp)
target_link_libraries(selftest PRIVATE TextEditorLib imguifd tsindex lspproto)
target_compile_options(selftest PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
enable_testing()
add_test(NAME selftest COMMAND selftest)

# ── LSP integration gate ──────────────────────────────────────────────────
# Spawns a real clangd and asserts a completion + go-to-def round trip. SKIPs
# (ctest return code 77) when no clangd is found, so CI without clangd stays green.
add_executable(lsptest lsptest.cpp)
target_link_libraries(lsptest PRIVATE lspclient SDL3::SDL3 SDL3_Headers)
target_compile_options(lsptest PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
add_test(NAME lsptest COMMAND lsptest)
set_tests_properties(lsptest PROPERTIES SKIP_RETURN_CODE 77)
