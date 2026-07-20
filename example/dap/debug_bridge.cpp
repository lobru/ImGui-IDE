//	Native-debugger bridges — implementation.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#define _CRT_SECURE_NO_WARNINGS   // std::getenv (MSVC C4996 vs /WX)

#include "debug_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace dbgbridge {

const char* kRadDbgDefaultBpTemplate = "add_breakpoint {file}:{line}";

std::vector<std::string> splitCommandLine(const std::string& cmd)
{
	std::vector<std::string> out;
	std::istringstream ss(cmd);
	std::string tok;
	while (ss >> tok)
		out.push_back(tok);
	return out;
}

std::string expandTemplate(std::string tmpl, const std::string& file, int line1Based)
{
	auto replaceAll = [&tmpl](const std::string& key, const std::string& value) {
		for (size_t p = tmpl.find(key); p != std::string::npos; p = tmpl.find(key, p + value.size()))
			tmpl.replace(p, key.size(), value);
	};
	replaceAll("{file}", file);
	replaceAll("{line}", std::to_string(line1Based));
	return tmpl;
}

std::string inferAdapterType(const std::vector<std::string>& argv)
{
	for (const auto& a : argv)
	{
		std::string t = a;
		for (auto& c : t)
			c = (char) std::tolower((unsigned char) c);
		if (t.find("vsdbg") != std::string::npos)
			return "cppvsdbg";
		if (t.find("opendebugad7") != std::string::npos)
			return "cppdbg";
		if (t.find("debugpy") != std::string::npos)
			return "python";
	}
	return {};
}

std::vector<std::string> raddbgLaunch(const std::string& raddbgPath,
                                      const std::string& exe, const std::string& args)
{
	std::vector<std::string> argv{raddbgPath, exe};
	for (auto& a : splitCommandLine(args))
		argv.push_back(a);
	return argv;
}

std::vector<std::vector<std::string>> raddbgBreakpointCmds(
	const std::string& raddbgPath, const std::string& bpTemplate,
	const std::vector<std::pair<std::string, int>>& fileLine1Based)
{
	std::vector<std::vector<std::string>> out;
	const std::string& tmpl = bpTemplate.empty() ? std::string(kRadDbgDefaultBpTemplate) : bpTemplate;
	out.reserve(fileLine1Based.size());
	for (auto& [file, line] : fileLine1Based)
		out.push_back({raddbgPath, "--ipc", expandTemplate(tmpl, file, line)});
	return out;
}

std::vector<std::string> devenvDebugExe(const std::string& devenvPath,
                                        const std::string& exe, const std::string& args)
{
	std::vector<std::string> argv{devenvPath, "/DebugExe", exe};
	for (auto& a : splitCommandLine(args))
		argv.push_back(a);
	return argv;
}

// ── Detection ────────────────────────────────────────────────────────────

namespace {

bool fileExists(const std::filesystem::path& p)
{
	std::error_code ec;
	return std::filesystem::is_regular_file(p, ec);
}

std::string envPath(const char* var)
{
	const char* v = std::getenv(var);
	return (v && *v && fileExists(v)) ? std::string(v) : std::string();
}

std::filesystem::path vscodeExtensionsDir()
{
	const char* home = std::getenv("USERPROFILE");
	if (!home || !*home)
		home = std::getenv("HOME");
	if (!home || !*home)
		return {};
	return std::filesystem::path(home) / ".vscode" / "extensions";
}

// Newest ms-vscode.cpptools-<version> extension dir that ACTUALLY CONTAINS the
// requested relative file. The bare `ms-vscode.cpptools-` prefix also matches
// `-themes` and `-extension-pack` (neither ships a debugger), and those sort
// AFTER a `-1.33.4` version string lexicographically — so a plain "newest by
// name" pick silently returned a debugger-less dir and vsdbg was never found
// (C++ debugging did nothing). Validating the payload avoids that entirely.
std::filesystem::path newestCppToolsWith(const std::filesystem::path& relFile)
{
	auto ext = vscodeExtensionsDir();
	if (ext.empty())
		return {};
	std::error_code ec;
	std::filesystem::path best;
	std::string bestName;
	for (auto& e : std::filesystem::directory_iterator(ext, ec))
	{
		if (ec)
			break;
		auto name = e.path().filename().string();
		if (name.rfind("ms-vscode.cpptools-", 0) != 0)
			continue;
		// Must be a versioned build (next char is a digit), not -themes / -pack.
		if (name.size() <= 19 || !std::isdigit((unsigned char) name[19]))
			continue;
		if (!fileExists(e.path() / relFile))
			continue;
		if (bestName.empty() || name > bestName)
		{
			bestName = name;
			best = e.path();
		}
	}
	return best;
}

// vsdbg from the C# extension (ms-dotnettools.csharp) — this is the managed
// (.NET / coreclr) debugger. Newest that contains vsdbg.exe wins.
std::filesystem::path csharpVsdbg()
{
	auto ext = vscodeExtensionsDir();
	if (ext.empty())
		return {};
	std::error_code ec;
	std::filesystem::path best;
	std::string bestName;
	for (auto& e : std::filesystem::directory_iterator(ext, ec))
	{
		if (ec)
			break;
		auto name = e.path().filename().string();
		if (name.rfind("ms-dotnettools.csharp-", 0) != 0)
			continue;
		std::filesystem::path candidates[] = {
			e.path() / ".debugger" / "x86_64" / "vsdbg.exe",
			e.path() / ".debugger" / "vsdbg.exe",
			e.path() / ".debugger" / "vsdbg",
		};
		for (auto& c : candidates)
			if (fileExists(c) && (bestName.empty() || name > bestName))
			{
				bestName = name;
				best = c;
			}
	}
	return best;
}

// vsdbg bundled with a Visual Studio install (Common7/IDE/vsdbg/vsdbg.exe).
std::filesystem::path vsInstallVsdbg()
{
	const char* pf = std::getenv("ProgramFiles");
	const char* pf86 = std::getenv("ProgramFiles(x86)");
	for (const char* base : {pf, pf86})
	{
		if (!base || !*base)
			continue;
		auto vsRoot = std::filesystem::path(base) / "Microsoft Visual Studio";
		std::error_code ec;
		if (!std::filesystem::is_directory(vsRoot, ec))
			continue;
		// Walk year/edition dirs (2022/Community, 18/Insiders, …), depth 2.
		for (auto it = std::filesystem::recursive_directory_iterator(
				 vsRoot, std::filesystem::directory_options::skip_permission_denied, ec);
			 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (it.depth() > 2)
			{
				it.disable_recursion_pending();
				continue;
			}
			auto p = it->path() / "Common7" / "IDE" / "vsdbg" / "vsdbg.exe";
			if (fileExists(p))
				return p;
		}
	}
	return {};
}

} // namespace

std::string findRadDbg()
{
	if (auto p = envPath("RADDBG_PATH"); !p.empty())
		return p;
	const char* candidates[] = {
		"C:/Program Files/raddebugger/raddbg.exe",
		"C:/raddbg/raddbg.exe",
	};
	for (const char* c : candidates)
		if (fileExists(c))
			return c;
	if (commandOnPath("raddbg"))
		return "raddbg";
	return {};
}

std::string findDevenv()
{
	if (auto p = envPath("DEVENV_PATH"); !p.empty())
		return p;
	const char* candidates[] = {
		"C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/devenv.exe",
		"C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/devenv.exe",
		"C:/Program Files/Microsoft Visual Studio/2022/Enterprise/Common7/IDE/devenv.exe",
	};
	for (const char* c : candidates)
		if (fileExists(c))
			return c;
	return {};
}

std::string findVsdbg()
{
	if (auto p = envPath("VSDBG_PATH"); !p.empty())
		return p;
	// VS Code C++ tools first (the cppvsdbg engine ships here)…
	if (auto dir = newestCppToolsWith(std::filesystem::path("debugAdapters") / "vsdbg" / "bin" / "vsdbg.exe");
		!dir.empty())
		return (dir / "debugAdapters" / "vsdbg" / "bin" / "vsdbg.exe").string();
	// …then the C# extension's copy, then a full Visual Studio install.
	if (auto p = csharpVsdbg(); !p.empty())
		return p.string();
	if (auto p = vsInstallVsdbg(); !p.empty())
		return p.string();
	return {};
}

std::string findVsdbgManaged()
{
	if (auto p = envPath("VSDBG_PATH"); !p.empty())
		return p;
	// For .NET the C# extension's vsdbg is the canonical one; any vsdbg speaks
	// coreclr though, so fall back to the C++ / VS copies.
	if (auto p = csharpVsdbg(); !p.empty())
		return p.string();
	if (auto dir = newestCppToolsWith(std::filesystem::path("debugAdapters") / "vsdbg" / "bin" / "vsdbg.exe");
		!dir.empty())
		return (dir / "debugAdapters" / "vsdbg" / "bin" / "vsdbg.exe").string();
	if (auto p = vsInstallVsdbg(); !p.empty())
		return p.string();
	return {};
}

std::string findNetcoredbg()
{
	if (auto p = envPath("NETCOREDBG_PATH"); !p.empty())
		return p;
	return commandOnPath("netcoredbg") ? std::string("netcoredbg") : std::string();
}

std::string findOpenDebugAD7()
{
	if (auto p = envPath("OPENDEBUGAD7_PATH"); !p.empty())
		return p;
	auto dir = newestCppToolsWith(std::filesystem::path("debugAdapters") / "bin" / "OpenDebugAD7.exe");
	if (dir.empty())
		dir = newestCppToolsWith(std::filesystem::path("debugAdapters") / "bin" / "OpenDebugAD7");
	if (dir.empty())
		return {};
	for (const char* leaf : {"OpenDebugAD7.exe", "OpenDebugAD7"})
	{
		auto p = dir / "debugAdapters" / "bin" / leaf;
		if (fileExists(p))
			return p.string();
	}
	return {};
}

// Resolve `name` (or name.exe) to its absolute path on PATH, else "".
static std::string resolveOnPath(const std::string& name)
{
	const char* path = std::getenv("PATH");
	if (!path || name.empty())
		return {};
#ifdef _WIN32
	const char sep = ';';
#else
	const char sep = ':';
#endif
	std::string entry;
	std::istringstream ss(path);
	while (std::getline(ss, entry, sep))
	{
		if (entry.empty())
			continue;
		std::filesystem::path base = std::filesystem::path(entry) / name;
		if (fileExists(base))
			return base.string();
		if (fileExists(base.string() + ".exe"))
			return base.string() + ".exe";
	}
	return {};
}

bool commandOnPath(const std::string& name)
{
	return !resolveOnPath(name).empty();
}

std::string findLldbDap()
{
	if (auto p = envPath("LLDB_DAP_PATH"); !p.empty())
		return p;
	// LLVM renamed the DAP adapter lldb-vscode -> lldb-dap in LLVM 18; accept
	// both. Apache-2.0 (LLVM exception) — freely usable in any host, unlike the
	// Microsoft vsdbg engine, which is license-locked to VS / VS Code.
	for (const char* name : {"lldb-dap", "lldb-vscode"})
		if (auto p = resolveOnPath(name); !p.empty())
			return p;
	// Common LLVM install locations when it isn't on PATH.
	std::vector<std::filesystem::path> roots;
	if (const char* e = std::getenv("LLVM_PATH"); e && *e)
		roots.emplace_back(std::filesystem::path(e) / "bin");
	for (const char* pf : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"})
		if (const char* e = std::getenv(pf); e && *e)
			roots.emplace_back(std::filesystem::path(e) / "LLVM" / "bin");
	for (const auto& root : roots)
		for (const char* leaf : {"lldb-dap.exe", "lldb-vscode.exe", "lldb-dap", "lldb-vscode"})
			if (auto c = root / leaf; fileExists(c))
				return c.string();
	return {};
}

bool unrealEditorTarget(const std::filesystem::path& projectRoot,
                        const std::filesystem::path& uproject, UnrealDebugTarget& out)
{
	out = {};
	std::string project = uproject.stem().string();
	// The project's own editor binaries, most-specific first. (Editor targets
	// build as <Project>Editor; some templates emit UnrealEditor into the
	// project's Binaries dir.)
	const char* platforms[] = {"Win64", "Linux", "Mac"};
	const std::string names[] = {project + "Editor.exe", project + "Editor",
	                             "UnrealEditor.exe", "UnrealEditor"};
	std::error_code ec;
	for (const char* plat : platforms)
	{
		for (const auto& name : names)
		{
			auto p = projectRoot / "Binaries" / plat / name;
			if (std::filesystem::is_regular_file(p, ec))
			{
				out.exe = p.string();
				// A stock UnrealEditor binary still needs the project argument;
				// a <Project>Editor target has it baked in.
				if (name.rfind("UnrealEditor", 0) == 0)
					out.args = uproject.string();
				return true;
			}
		}
	}
	// Fall back to an engine UnrealEditor from PATH, opening the .uproject.
	if (commandOnPath("UnrealEditor"))
	{
		out.exe = "UnrealEditor";
		out.args = uproject.string();
		return true;
	}
	return false;
}

} // namespace dbgbridge
