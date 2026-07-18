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

# ── DAP protocol layer ───────────────────────────────────────────────────
# Pure Debug Adapter Protocol builders/parsers (no SDL) — reuses lspproto's
# Content-Length framing. Links into the app and the headless selftest.
add_library(dapproto STATIC dap_protocol.cpp dap_protocol.h)
target_link_libraries(dapproto PUBLIC lspproto nlohmann_json)
target_include_directories(dapproto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dapproto PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# ── Native-debugger bridges (raddbg / Visual Studio / MS DAP adapters) ────
# Pure command builders + tool detection, no ImGui/SDL — links into the app
# and the headless selftest.
add_library(dbgbridge STATIC debug_bridge.cpp debug_bridge.h)
target_include_directories(dbgbridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dbgbridge PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# ── DAP client transport (spawns a debug adapter via SDL3 process API) ────
add_library(dapclient STATIC dap_client.cpp dap_client.h)
target_link_libraries(dapclient PUBLIC dapproto SDL3::SDL3 SDL3_Headers)
target_include_directories(dapclient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dapclient PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)

# ── Example executable ───────────────────────────────────────────────────
add_executable(example
	editor.cpp
	updater.cpp
	pdfview.cpp
	cppgen.cpp
	notes.cpp
	screenshot.cpp
	plugin_registry.cpp
	plugin_loader.cpp
	dejavu.cpp
	main.cpp)

target_link_libraries(example PRIVATE TextEditorLib SDL3::SDL3 SDL3_Headers imguifd tsindex lspclient)

# ── Feature plugins, packaged as runtime-loaded DLLs ──────────────────────
# Domain-specific features live in plugins, each behind its own flag (default
# ON). Each enabled plugin builds as a MODULE (a dlopen-only shared library)
# dropped into <exe>/plugins and loaded at startup by plugin_loader.cpp. The core
# exe links none of their code, so a plugin-less build is a genuinely smaller
# binary that gains a feature simply by having its DLL present. See plugin_api.h
# (the C++ interface) and plugin_abi.h (the extern-"C" DLL boundary).
option(IMGUIIDE_PLUGIN_UNREAL "Build the Unreal Engine integration plugin (DLL)" ON)
option(IMGUIIDE_PLUGIN_UEVR "Build the UEVR / Blueprint tooling plugin (DLL)" ON)
option(IMGUIIDE_PLUGIN_TERMINAL "Build the integrated terminal plugin (DLL)" ON)

# Static deps linked INTO a plugin DLL must be position-independent (a no-op on
# Windows; required to pull a static archive into a shared object elsewhere).
if(IMGUIIDE_PLUGIN_UNREAL OR IMGUIIDE_PLUGIN_UEVR OR IMGUIIDE_PLUGIN_TERMINAL)
	set_property(TARGET imgui imguifd TextEditorLib PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

# imguiide_add_plugin(<target> <factory> <sources...>)
# Builds one plugin DLL: the given sources + the shared extern-"C" ABI shim, with
# the shim wired to <factory> (the plugin's createXxxPlugin). The .dll lands in
# <exe>/plugins and is built alongside the exe. Only the 3 ABI functions are
# exported; imgui is linked in statically and shares the host context at runtime.
function(imguiide_add_plugin name factory)
	add_library(${name} MODULE ${ARGN} plugins/plugin_dll_main.cpp)
	target_link_libraries(${name} PRIVATE imguifd)
	target_include_directories(${name} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/..)
	target_compile_definitions(${name} PRIVATE IMGUIIDE_PLUGIN_FACTORY=${factory})
	set_target_properties(${name} PROPERTIES
		PREFIX ""
		LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:example>/plugins"
		RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:example>/plugins")
	target_compile_options(${name} PRIVATE
		$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
		$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
	# Free a locked output before linking: a running app maps plugins/<name>.dll and
	# would otherwise LNK1168 the relink. The app loads a shadow copy (plugin_loader
	# .cpp), so this is belt-and-suspenders, but it also unblocks the FIRST rebuild
	# after this change while an OLD (pre-shadow-copy) app is still running.
	add_custom_command(TARGET ${name} PRE_LINK
		COMMAND ${CMAKE_COMMAND} -DFILE=$<TARGET_FILE:${name}>
		        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rename_aside.cmake
		VERBATIM)
	add_dependencies(example ${name}) # build + deploy the DLL whenever the exe builds
endfunction()

if(IMGUIIDE_PLUGIN_UNREAL)
	imguiide_add_plugin(imguiide_unreal createUnrealPlugin
		plugins/unreal/unreal_plugin.cpp
		plugins/unreal/unreal.cpp
		plugins/unreal/unreal_codegen.cpp
		plugins/unreal/unreal_uasset.cpp)
	target_include_directories(imguiide_unreal PRIVATE plugins/unreal)
	target_link_libraries(imguiide_unreal PRIVATE nlohmann_json) # unreal.cpp parses .uproject JSON
	target_compile_definitions(imguiide_unreal PRIVATE IMGUIIDE_PLUGIN_UNREAL=1)
endif()

if(IMGUIIDE_PLUGIN_UEVR)
	# BlueprintEditor/BlueprintLua/Import are used ONLY by this plugin, so they
	# live in plugins/uevr and compile into its DLL (not TextEditorLib).
	imguiide_add_plugin(imguiide_uevr createUevrPlugin
		plugins/uevr/uevr_plugin.cpp
		plugins/uevr/blueprint_window.cpp
		plugins/uevr/class_browser.cpp
		plugins/uevr/blueprint_templates.cpp
		plugins/uevr/blueprint_snippets.cpp
		plugins/uevr/uevr_bridge.cpp
		plugins/uevr/BlueprintEditor.cpp
		plugins/uevr/BlueprintLua.cpp
		plugins/uevr/BlueprintLuaImport.cpp
		plugins/uevr/BlueprintRegistryJson.cpp)
	target_include_directories(imguiide_uevr PRIVATE plugins/uevr)
	# nlohmann_json: BlueprintRegistryJson parses/emits SDK JSON. TextEditorLib: Custom
	# Lua nodes embed the app's own TextEditor widget (self-contained static copy in the
	# DLL; Lua highlighting reads only its own statics, so no cross-DLL shared state).
	target_link_libraries(imguiide_uevr PRIVATE nlohmann_json TextEditorLib)
	target_compile_definitions(imguiide_uevr PRIVATE IMGUIIDE_PLUGIN_UEVR=1)
endif()

if(IMGUIIDE_PLUGIN_TERMINAL)
	# Integrated terminal: a persistent shell with piped I/O. No extra deps beyond
	# the OS process API (Win32 / posix_spawn).
	imguiide_add_plugin(imguiide_terminal createTerminalPlugin
		plugins/terminal/terminal_plugin.cpp)
	target_include_directories(imguiide_terminal PRIVATE plugins/terminal)
	target_compile_definitions(imguiide_terminal PRIVATE IMGUIIDE_PLUGIN_TERMINAL=1)
endif()

# In-app updater talks to GitHub over HTTPS via WinHTTP; the PDF viewer uses the
# OS PDF engine via C++/WinRT (windowsapp). Both Windows only.
if(WIN32)
	target_link_libraries(example PRIVATE winhttp windowsapp)
endif()

# Brand the output binary ImGui-IDE(.exe). The CMake target stays `example` (so
# build scripts / object paths are unchanged) — only the produced file is renamed.
# Overridable so a dev/test build can emit a different file (e.g.
# -DIMGUIIDE_EXE_NAME=ImGui-IDE-run) and never have to overwrite — or wait on — a
# separately-running ImGui-IDE.exe.
set(IMGUIIDE_EXE_NAME "ImGui-IDE" CACHE STRING "Base name of the produced app executable")
set_target_properties(example PROPERTIES OUTPUT_NAME "${IMGUIIDE_EXE_NAME}")

# Same rename-aside trick for the exe: if the app is running from its own output
# path, rename the running image out of the way so the linker can write a fresh
# one (Windows keeps executing the renamed file; the new binary is used next
# launch). Unlike the plugin DLLs there's no shadow-copy equivalent for the exe,
# so this is the sole mechanism that lets a rebuild proceed while the app runs.
add_custom_command(TARGET example PRE_LINK
	COMMAND ${CMAKE_COMMAND} -DFILE=$<TARGET_FILE:example>
	        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rename_aside.cmake
	VERBATIM)

# Dev/test builds can retarget the whole app payload into a staging directory:
# because the plugin DLLs and synced assets all follow $<TARGET_FILE_DIR:example>,
# moving the exe moves everything with it. A rebuild then never has to overwrite —
# or wait on a file lock from — a separately-running install (exe OR its plugins).
set(IMGUIIDE_STAGING_DIR "" CACHE PATH "If set, output the app + its plugins here")
if(IMGUIIDE_STAGING_DIR)
	set_target_properties(example PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${IMGUIIDE_STAGING_DIR}")
endif()

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
	# Also drop CHANGELOG.md next to the exe so Help > Changelog opens it in-app.
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_CURRENT_SOURCE_DIR}/../CHANGELOG.md"
		"$<TARGET_FILE_DIR:example>/CHANGELOG.md"
	# Bundle the UE source-code-accessor plugin so Project > Unreal Engine >
	# "Install IDE plugin..." can copy it into a game's Plugins/ folder.
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
		"${CMAKE_CURRENT_SOURCE_DIR}/../tools/ue-plugins"
		"$<TARGET_FILE_DIR:example>/ue-plugins"
	# Pregenerated symbol packs (type→member maps) for framework-wide completion.
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
		"${CMAKE_CURRENT_SOURCE_DIR}/symbols"
		"$<TARGET_FILE_DIR:example>/symbols"
	COMMENT "Syncing languages/ + CHANGELOG.md + ue-plugins/ + symbols/ next to example.exe")

# ── Headless logic self-test ─────────────────────────────────────────────
# A tiny console executable that exercises the pure (non-UI) code paths —
# colorizer, fold detection — with no SDL/GPU/ImGui context. It links the same
# TextEditorLib + imgui (for ImFont/ImVec2 types) but never creates a context,
# so it runs in CI and even while example.exe is open. `ctest` or run directly.
add_executable(selftest selftest.cpp cppgen.cpp notes.cpp screenshot.cpp)
target_link_libraries(selftest PRIVATE TextEditorLib imguifd tsindex lspproto)
# The UE + Blueprint tests are gated on their plugin flags; compile the (now
# plugin-owned) sources into selftest when the flag is on so they still run.
if(IMGUIIDE_PLUGIN_UNREAL)
	target_sources(selftest PRIVATE plugins/unreal/unreal.cpp plugins/unreal/unreal_plugin.cpp
		plugins/unreal/unreal_codegen.cpp plugins/unreal/unreal_uasset.cpp)
	target_include_directories(selftest PRIVATE plugins/unreal)
	target_compile_definitions(selftest PRIVATE IMGUIIDE_PLUGIN_UNREAL=1)
endif()
if(IMGUIIDE_PLUGIN_UEVR)
	target_sources(selftest PRIVATE
		plugins/uevr/BlueprintEditor.cpp plugins/uevr/BlueprintLua.cpp plugins/uevr/BlueprintLuaImport.cpp
		plugins/uevr/BlueprintRegistryJson.cpp plugins/uevr/blueprint_templates.cpp
		plugins/uevr/blueprint_snippets.cpp)
	target_include_directories(selftest PRIVATE plugins/uevr)
	target_link_libraries(selftest PRIVATE nlohmann_json)
	target_compile_definitions(selftest PRIVATE IMGUIIDE_PLUGIN_UEVR=1)
endif()
target_compile_options(selftest PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
enable_testing()
add_test(NAME selftest COMMAND selftest)

# ── Headless performance regression net ──────────────────────────────────
# Times the dominant non-UI hot paths (load+colorize, fold detection, color
# lookups, serialization, cursor normalization) on a large synthetic doc. Prints
# every timing and fails only on a gross (order-of-magnitude) regression, so it's
# a CI tripwire rather than a flaky benchmark.
add_executable(perftest perftest.cpp)
target_link_libraries(perftest PRIVATE TextEditorLib imguifd tsindex)
target_compile_options(perftest PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
add_test(NAME perftest COMMAND perftest)

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

# ── DAP integration gate ──────────────────────────────────────────────────
# Spawns a real debugpy adapter and asserts a breakpoint → stop → inspect →
# continue → exit round trip. SKIPs (77) when python/debugpy is absent.
add_executable(daptest daptest.cpp)
target_link_libraries(daptest PRIVATE dapclient SDL3::SDL3 SDL3_Headers)
target_compile_options(daptest PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror -Wall -Wpedantic -Wextra>
	$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /WX>)
add_test(NAME daptest COMMAND daptest)
set_tests_properties(daptest PROPERTIES SKIP_RETURN_CODE 77)
