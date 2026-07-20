//	Native-debugger bridges — raddbg (Epic's RAD Debugger), the Microsoft
//	Visual Studio debugger (devenv / vsdbg), and Microsoft's DAP adapters.
//
//	Design: our in-app debugger speaks DAP (dap_client). External native
//	debuggers bridge two ways —
//	  1. DAP adapters we can drive DIRECTLY in the existing client: vsdbg
//	     (--interpreter=vscode, the VS Code "cppvsdbg" engine), OpenDebugAD7
//	     (MIEngine, "cppdbg" over gdb), lldb-dap, and gdb 14+'s built-in
//	     `gdb -i dap`. This module locates them; the editor picks per platform.
//	  2. Tools with their OWN UI (raddbg, Visual Studio) launch externally,
//	     seeded with our target + breakpoints. Command VERBS are template-
//	     driven (settings-overridable) rather than hardcoded, so a tool
//	     updating its CLI needs a settings tweak, not a rebuild.
//
//	Pure logic (no ImGui/SDL): builders return argv vectors; detection only
//	touches the filesystem/env. Selftest-covered.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace dbgbridge {

// Split a command line on spaces (no quote handling — paths with spaces go
// through the settings override as separate template pieces).
std::vector<std::string> splitCommandLine(const std::string& cmd);

// Expand {file} and {line} placeholders in a template string.
std::string expandTemplate(std::string tmpl, const std::string& file, int line1Based);

// Infer the DAP launch "type" from an adapter command line, so a user-entered
// adapter needs no separate type field. Scans every token — adapters can hide
// behind interpreters ("python -m debugpy.adapter"). Empty means "send no
// type" (lldb-dap and gdb's DAP interpreter don't want one).
std::string inferAdapterType(const std::vector<std::string>& argv);

// ── raddbg ───────────────────────────────────────────────────────────────
// Launch: `raddbg <exe> [args…]`. raddbg attaches its own UI to the target.
std::vector<std::string> raddbgLaunch(const std::string& raddbgPath,
                                      const std::string& exe, const std::string& args);
// One `raddbg --ipc <verb>` command per breakpoint, driving the running
// instance. Default verb template: "add_breakpoint {file}:{line}".
extern const char* kRadDbgDefaultBpTemplate;
std::vector<std::vector<std::string>> raddbgBreakpointCmds(
	const std::string& raddbgPath, const std::string& bpTemplate,
	const std::vector<std::pair<std::string, int>>& fileLine1Based);

// ── Visual Studio ────────────────────────────────────────────────────────
// `devenv /DebugExe <exe> [args…]` — opens VS with the exe under its debugger.
std::vector<std::string> devenvDebugExe(const std::string& devenvPath,
                                        const std::string& exe, const std::string& args);

// ── Detection ────────────────────────────────────────────────────────────
// Each returns an absolute path or "" when not found. Env overrides win:
// RADDBG_PATH / DEVENV_PATH / VSDBG_PATH / OPENDEBUGAD7_PATH.
std::string findRadDbg();
std::string findDevenv();        // vswhere-known VS install dirs
std::string findVsdbg();         // NATIVE C/C++ vsdbg (cppvsdbg engine): VS Code
                                 // C++ tools, else C# ext, else a VS install
std::string findVsdbgManaged();  // MANAGED .NET vsdbg (coreclr engine): C# ext first
std::string findNetcoredbg();    // Samsung netcoredbg on PATH (.NET fallback)
std::string findOpenDebugAD7();  // VS Code ms-vscode.cpptools ext (MIEngine)

// True if `name` resolves to an executable on PATH (checks name and name.exe).
bool commandOnPath(const std::string& name);

// ── Unreal targets ───────────────────────────────────────────────────────
// Resolve what to debug for a UE project: prefer the project's own editor
// binary (Binaries/Win64|Linux/<Project>Editor*), else fall back to a stock
// UnrealEditor from PATH with the .uproject as its argument.
struct UnrealDebugTarget {
	std::string exe;
	std::string args;   // "" or the quoted .uproject
};
bool unrealEditorTarget(const std::filesystem::path& projectRoot,
                        const std::filesystem::path& uproject, UnrealDebugTarget& out);

} // namespace dbgbridge
