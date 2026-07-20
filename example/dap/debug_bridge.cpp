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

// Newest ms-vscode.cpptools extension dir under ~/.vscode/extensions (the
// version is embedded in the dir name; lexicographic max is close enough).
std::filesystem::path newestCppToolsDir()
{
	const char* home = std::getenv("USERPROFILE");
	if (!home || !*home)
		home = std::getenv("HOME");
	if (!home || !*home)
		return {};
	std::filesystem::path ext = std::filesystem::path(home) / ".vscode" / "extensions";
	std::error_code ec;
	std::filesystem::path best;
	for (auto& e : std::filesystem::directory_iterator(ext, ec))
	{
		if (ec)
			break;
		auto name = e.path().filename().string();
		if (name.rfind("ms-vscode.cpptools-", 0) == 0 &&
			(best.empty() || e.path().filename().string() > best.filename().string()))
			best = e.path();
	}
	return best;
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
	auto dir = newestCppToolsDir();
	if (dir.empty())
		return {};
	auto p = dir / "debugAdapters" / "vsdbg" / "bin" / "vsdbg.exe";
	return fileExists(p) ? p.string() : std::string();
}

std::string findOpenDebugAD7()
{
	if (auto p = envPath("OPENDEBUGAD7_PATH"); !p.empty())
		return p;
	auto dir = newestCppToolsDir();
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

bool commandOnPath(const std::string& name)
{
	const char* path = std::getenv("PATH");
	if (!path || name.empty())
		return false;
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
		if (fileExists(base) || fileExists(base.string() + ".exe"))
			return true;
	}
	return false;
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
