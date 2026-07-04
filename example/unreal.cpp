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

} // namespace unreal
