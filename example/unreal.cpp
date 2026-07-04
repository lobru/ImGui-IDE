//	ImGui-IDE — Unreal Engine 5 project integration. See unreal.h.

#include "unreal.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace unreal {

static std::string quoted(const std::filesystem::path& p)
{
	return "\"" + p.string() + "\"";
}

std::filesystem::path findUProject(const std::filesystem::path& searchStart)
{
	std::error_code ec;
	auto cur = searchStart;
	for (int depth = 0; depth < 12 && !cur.empty(); ++depth)
	{
		if (std::filesystem::is_directory(cur, ec))
		{
			for (const auto& e : std::filesystem::directory_iterator(cur, ec))
			{
				if (!e.is_regular_file())
					continue;
				auto ext = e.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext == ".uproject")
					return e.path();
			}
		}
		if (!cur.has_parent_path() || cur.parent_path() == cur)
			break;
		cur = cur.parent_path();
	}
	return {};
}

#ifdef _WIN32
// Read a REG_SZ; empty on any failure.
static std::string readRegString(HKEY root, const char* subkey, const char* value)
{
	char buf[1024];
	DWORD size = sizeof(buf);
	DWORD type = 0;
	if (RegGetValueA(root, subkey, value, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS)
		return {};
	return std::string(buf);
}
#endif

std::filesystem::path findEngineRoot(const std::filesystem::path& uproject, std::string& engineAssociation)
{
	engineAssociation.clear();
	std::error_code ec;

	// EngineAssociation from the .uproject JSON ("5.4", a source-build GUID, or
	// absent/empty for a project living inside an engine checkout).
	{
		std::ifstream f(uproject);
		if (f.is_open())
		{
			auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions*/ false);
			if (!j.is_discarded() && j.is_object())
				engineAssociation = j.value("EngineAssociation", "");
		}
	}

	auto looksLikeEngine = [&](const std::filesystem::path& root) {
		return std::filesystem::exists(root / "Engine" / "Build" / "BatchFiles", ec);
	};

	if (engineAssociation.empty())
	{
		// Relative checkout: the project sits under/next to the engine tree.
		for (auto cur = uproject.parent_path(); !cur.empty(); cur = cur.parent_path())
		{
			if (looksLikeEngine(cur))
				return cur;
			if (!cur.has_parent_path() || cur.parent_path() == cur)
				break;
		}
		return {};
	}

#ifdef _WIN32
	// Launcher installs register HKLM\SOFTWARE\EpicGames\Unreal Engine\<ver>.
	{
		std::string key = "SOFTWARE\\EpicGames\\Unreal Engine\\" + engineAssociation;
		auto dir = readRegString(HKEY_LOCAL_MACHINE, key.c_str(), "InstalledDirectory");
		if (!dir.empty() && looksLikeEngine(dir))
			return std::filesystem::path(dir);
	}
	// Source builds register per-user GUIDs under HKCU\...\Builds.
	{
		auto dir = readRegString(HKEY_CURRENT_USER, "SOFTWARE\\Epic Games\\Unreal Engine\\Builds",
			engineAssociation.c_str());
		if (!dir.empty() && looksLikeEngine(dir))
			return std::filesystem::path(dir);
	}
	// Default launcher location as a last guess.
	{
		auto guess = std::filesystem::path("C:/Program Files/Epic Games") / ("UE_" + engineAssociation);
		if (looksLikeEngine(guess))
			return guess;
	}
#endif
	return {};
}

std::string targetName(const std::filesystem::path& uproject)
{
	return uproject.stem().string() + "Editor";
}

bool hasCppSource(const std::filesystem::path& uproject)
{
	std::error_code ec;
	return std::filesystem::is_directory(uproject.parent_path() / "Source", ec);
}

static std::filesystem::path buildBat(const std::filesystem::path& engineRoot)
{
	std::error_code ec;
	auto bat = engineRoot / "Engine" / "Build" / "BatchFiles" / "Build.bat";
	return std::filesystem::exists(bat, ec) ? bat : std::filesystem::path{};
}

std::string buildEditorCommand(const std::filesystem::path& engineRoot, const std::filesystem::path& uproject)
{
	auto bat = buildBat(engineRoot);
	if (bat.empty() || !hasCppSource(uproject))
		return {};
	// Same UBT invocation the VS project generator emits for the editor target.
	return quoted(bat) + " " + targetName(uproject) + " Win64 Development -Project=" +
		quoted(uproject) + " -WaitMutex -FromMsBuild";
}

std::string generateClangDbCommand(const std::filesystem::path& engineRoot, const std::filesystem::path& uproject)
{
	auto bat = buildBat(engineRoot);
	if (bat.empty())
		return {};
	// Drop compile_commands.json in the PROJECT root — clangd (rooted there)
	// discovers it by ancestor search, giving VS/VSCode-grade UE intellisense.
	// -OutputDir needs UE 5.3+; older engines write to the engine root instead.
	return quoted(bat) + " -mode=GenerateClangDatabase -project=" + quoted(uproject) + " " +
		targetName(uproject) + " Win64 Development -OutputDir=" + quoted(uproject.parent_path());
}

std::filesystem::path editorBinary(const std::filesystem::path& engineRoot)
{
	std::error_code ec;
	auto exe = engineRoot / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe";
	return std::filesystem::exists(exe, ec) ? exe : std::filesystem::path{};
}

std::filesystem::path resolveInclude(const std::filesystem::path& engineRoot,
									 const std::filesystem::path& uproject,
									 const std::string& include)
{
	if (include.empty())
		return {};
	std::error_code ec;
	std::filesystem::path inc(include);

	// Try <moduleDir>/{Public,Classes,Private,.}/<include> — the four include
	// roots UBT gives every module.
	auto tryModule = [&](const std::filesystem::path& mod) -> std::filesystem::path {
		for (const char* sub : { "Public", "Classes", "Private", "" })
		{
			auto p = *sub ? mod / sub / inc : mod / inc;
			if (std::filesystem::is_regular_file(p, ec))
				return p;
		}
		return {};
	};
	// Try every module under a Source/ dir.
	auto trySourceTree = [&](const std::filesystem::path& sourceDir) -> std::filesystem::path {
		if (!std::filesystem::is_directory(sourceDir, ec))
			return {};
		// Direct join first (top-level headers / already-module-qualified includes).
		if (auto p = sourceDir / inc; std::filesystem::is_regular_file(p, ec))
			return p;
		for (const auto& mod : std::filesystem::directory_iterator(sourceDir, ec))
		{
			if (!mod.is_directory())
				continue;
			if (auto hit = tryModule(mod.path()); !hit.empty())
				return hit;
		}
		return {};
	};

	auto projDir = uproject.parent_path();

	// 1. The game's own modules.
	if (auto hit = trySourceTree(projDir / "Source"); !hit.empty())
		return hit;

	// 2. Project plugins: Plugins/<name>/Source/<module>/...
	{
		auto plugins = projDir / "Plugins";
		if (std::filesystem::is_directory(plugins, ec))
			for (const auto& plug : std::filesystem::directory_iterator(plugins, ec))
			{
				if (!plug.is_directory())
					continue;
				if (auto hit = trySourceTree(plug.path() / "Source"); !hit.empty())
					return hit;
			}
	}

	// 3. UHT-generated headers ("Actor.generated.h"):
	//    Intermediate/Build/Win64/<Target>/Inc/<Module>/UHT/<name>
	if (include.find(".generated.h") != std::string::npos)
	{
		auto wanted = inc.filename();
		auto incRoot = projDir / "Intermediate" / "Build" / "Win64";
		if (std::filesystem::is_directory(incRoot, ec))
			for (const auto& target : std::filesystem::directory_iterator(incRoot, ec))
			{
				auto incDir = target.path() / "Inc";
				if (!std::filesystem::is_directory(incDir, ec))
					continue;
				for (const auto& mod : std::filesystem::directory_iterator(incDir, ec))
				{
					auto p = mod.path() / "UHT" / wanted;
					if (std::filesystem::is_regular_file(p, ec))
						return p;
				}
			}
	}

	if (engineRoot.empty())
		return {};

	// 4. Engine modules: Engine/Source/{Runtime,Editor,Developer,Programs}/<module>/...
	for (const char* cat : { "Runtime", "Editor", "Developer", "Programs" })
	{
		if (auto hit = trySourceTree(engineRoot / "Engine" / "Source" / cat); !hit.empty())
			return hit;
	}

	// 5. Engine plugins (bounded walk: Engine/Plugins/**/Source, depth-capped —
	//    plugin folders nest one or two categories deep).
	{
		auto plugRoot = engineRoot / "Engine" / "Plugins";
		if (std::filesystem::is_directory(plugRoot, ec))
		{
			int budget = 4000;
			for (auto it = std::filesystem::recursive_directory_iterator(
					 plugRoot, std::filesystem::directory_options::skip_permission_denied, ec);
				 !ec && it != std::filesystem::recursive_directory_iterator(); ++it)
			{
				if (--budget <= 0)
					break;
				if (it.depth() > 3 || !it->is_directory(ec))
				{
					if (it.depth() > 3)
						it.disable_recursion_pending();
					continue;
				}
				if (it->path().filename() == "Source")
				{
					it.disable_recursion_pending(); // don't descend into module trees
					if (auto hit = trySourceTree(it->path()); !hit.empty())
						return hit;
				}
			}
		}
	}

	return {};
}

} // namespace unreal
