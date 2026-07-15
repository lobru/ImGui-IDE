//	ImGui-IDE — Unreal Engine 5 project integration. See unreal.h.

#include "unreal.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

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

// Unreal descriptors (.uproject/.uplugin) are hand- and tool-edited and routinely
// contain things strict JSON rejects — most commonly a TRAILING COMMA before a ]
// or } (UE's own editor writes them). nlohmann is strict, so a lenient descriptor
// parses to "discarded" and everything downstream (engine lookup, module list)
// silently fails. Clean the two lenient bits UE tolerates — // and /* */ comments
// and trailing commas — then parse. String scan, comment/comma aware, so it never
// touches a `,` or `//` INSIDE a string value.
std::string stripJsonLeniencies(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	bool inStr = false, esc = false;

	for (size_t i = 0; i < in.size(); ++i)
	{
		char c = in[i];

		if (inStr)
		{
			out.push_back(c);
			if (esc)          esc = false;
			else if (c == '\\') esc = true;
			else if (c == '"')  inStr = false;
			continue;
		}

		// line comment
		if (c == '/' && i + 1 < in.size() && in[i + 1] == '/')
		{
			i += 2;
			while (i < in.size() && in[i] != '\n')
				++i;
			if (i < in.size())
				out.push_back('\n');
			continue;
		}
		// block comment
		if (c == '/' && i + 1 < in.size() && in[i + 1] == '*')
		{
			i += 2;
			while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/'))
				++i;
			i += 1; // land on the '/'
			continue;
		}
		// trailing comma: a ',' whose next non-whitespace char closes an array/object
		if (c == ',')
		{
			size_t j = i + 1;
			while (j < in.size() && (in[j] == ' ' || in[j] == '\t' || in[j] == '\r' || in[j] == '\n'))
				++j;
			if (j < in.size() && (in[j] == ']' || in[j] == '}'))
				continue; // drop the comma
		}

		if (c == '"')
			inStr = true;

		out.push_back(c);
	}

	return out;
}

// Parse a descriptor's text, tolerating the leniencies above. Returns a discarded
// json on genuine syntax errors.
nlohmann::json parseDescriptor(const std::string& text)
{
	auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
	if (!j.is_discarded())
		return j;
	return nlohmann::json::parse(stripJsonLeniencies(text), nullptr, /*allow_exceptions*/ false);
}

std::filesystem::path findUProject(const std::filesystem::path& searchStart)
{
	std::error_code ec;
	auto cur = searchStart;
	for (int depth = 0; depth < 12 && !cur.empty(); ++depth)
	{
		if (std::filesystem::is_directory(cur, ec))
		{
			// error_code overloads throughout: the throwing directory iterator
			// increment / is_regular_file() abort the app on ONE unresolvable
			// entry — walking up to C:\ hit a broken "C:\shadertoggler.ini" whose
			// status query threw std::filesystem_error and crashed the editor.
			std::error_code iec;
			for (auto it = std::filesystem::directory_iterator(
					 cur, std::filesystem::directory_options::skip_permission_denied, iec);
				 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
			{
				std::error_code fec;
				if (!it->is_regular_file(fec) || fec)
					continue;
				auto ext = it->path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext == ".uproject")
					return it->path();
			}
		}
		if (!cur.has_parent_path() || cur.parent_path() == cur)
			break;
		cur = cur.parent_path();
	}
	return {};
}

bool isPluginDescriptor(const std::filesystem::path& descriptor)
{
	auto ext = descriptor.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext == ".uplugin";
}

// Find a .uplugin near `searchStart`: the folder itself, one level of children
// (plugin repos ship it as <Name>/<Name>.uplugin), then walking UP (a plugin
// checked out inside an engine's Engine/Plugins or a project's Plugins tree).
static std::filesystem::path findUPlugin(const std::filesystem::path& searchStart)
{
	std::error_code ec;

	auto scanDir = [&](const std::filesystem::path& dir) -> std::filesystem::path {
		std::error_code iec;
		for (auto it = std::filesystem::directory_iterator(
				 dir, std::filesystem::directory_options::skip_permission_denied, iec);
			 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
		{
			std::error_code fec;
			if (!it->is_regular_file(fec) || fec)
				continue;
			if (isPluginDescriptor(it->path()))
				return it->path();
		}
		return {};
	};

	if (!std::filesystem::is_directory(searchStart, ec))
		return {};

	// the folder itself
	if (auto here = scanDir(searchStart); !here.empty())
		return here;

	// one level of immediate subfolders (the common <Repo>/<Name>/<Name>.uplugin)
	{
		std::error_code iec;
		for (auto it = std::filesystem::directory_iterator(
				 searchStart, std::filesystem::directory_options::skip_permission_denied, iec);
			 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
		{
			std::error_code dec;
			if (it->is_directory(dec) && !dec)
			{
				if (auto found = scanDir(it->path()); !found.empty())
					return found;
			}
		}
	}

	// walk up (a plugin living under Engine/Plugins/... or Project/Plugins/...)
	for (auto cur = searchStart; cur.has_parent_path() && cur.parent_path() != cur; cur = cur.parent_path())
	{
		if (auto found = scanDir(cur); !found.empty())
			return found;
	}

	return {};
}

std::filesystem::path findUProjectOrPlugin(const std::filesystem::path& searchStart)
{
	// A .uproject wins (it's the richer, buildable descriptor); a .uplugin repo is
	// the fallback so a standalone plugin checkout is still recognized as UE.
	if (auto proj = findUProject(searchStart); !proj.empty())
		return proj;
	return findUPlugin(searchStart);
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

std::vector<std::string> availablePlugins(const std::filesystem::path& engineRoot,
										  const std::filesystem::path& projectDir)
{
	std::set<std::string> names;
	std::error_code ec;

	// Every <name>.uplugin under a Plugins/ tree, depth-capped (plugin folders
	// nest a category or two deep; a full recursive walk of an engine is huge).
	auto scan = [&](const std::filesystem::path& root, int maxDepth) {
		if (!std::filesystem::is_directory(root, ec))
			return;
		int budget = 20000;
		for (auto it = std::filesystem::recursive_directory_iterator(
				 root, std::filesystem::directory_options::skip_permission_denied, ec);
			 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (--budget <= 0)
				break;
			if (it.depth() > maxDepth)
			{
				it.disable_recursion_pending();
				continue;
			}
			std::error_code fec;
			if (it->is_regular_file(fec) && !fec && isPluginDescriptor(it->path()))
				names.insert(it->path().stem().string());
		}
	};

	if (!projectDir.empty())
		scan(projectDir / "Plugins", 6);
	if (!engineRoot.empty())
		scan(engineRoot / "Engine" / "Plugins", 5);

	return { names.begin(), names.end() };
}

std::filesystem::path findEngineRoot(const std::filesystem::path& uproject, std::string& engineAssociation)
{
	engineAssociation.clear();
	std::error_code ec;

	// A .uproject carries "EngineAssociation" ("5.4", a source-build GUID, or empty
	// for a project inside an engine checkout). A .uplugin instead carries
	// "EngineVersion" ("4.26.0") — reduce it to the "major.minor" the launcher
	// registers ("4.26"). Either way the value feeds the same engine lookup below.
	{
		std::ifstream f(uproject);
		if (f.is_open())
		{
			std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			auto j = parseDescriptor(text); // tolerant: UE descriptors have trailing commas
			if (!j.is_discarded() && j.is_object())
			{
				if (isPluginDescriptor(uproject))
				{
					std::string ver = j.value("EngineVersion", "");
					// "4.26.0" -> "4.26" (drop the patch component the registry omits)
					size_t first = ver.find('.');
					if (first != std::string::npos)
					{
						size_t second = ver.find('.', first + 1);
						engineAssociation = (second == std::string::npos) ? ver : ver.substr(0, second);
					}
					else
					{
						engineAssociation = ver;
					}
				}
				else
				{
					engineAssociation = j.value("EngineAssociation", "");
				}
			}
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

const std::vector<std::string>& descriptorWords(const std::filesystem::path& engineRoot,
												const std::filesystem::path& projectDir)
{
	// Cached per (engine, project) — the scans below walk real directory trees.
	static std::map<std::string, std::vector<std::string>> cache;
	std::string key = engineRoot.string() + "|" + projectDir.string();
	auto found = cache.find(key);
	if (found != cache.end())
		return found->second;

	std::vector<std::string> w = {
		// .uproject / .uplugin descriptor keys
		"FileVersion", "EngineAssociation", "Category", "Description", "Modules", "Plugins",
		"Name", "Type", "LoadingPhase", "AdditionalDependencies", "Enabled", "Optional",
		"MarketplaceURL", "SupportedTargetPlatforms", "TargetPlatforms", "TargetAllowList",
		"TargetDenyList", "PlatformAllowList", "PlatformDenyList", "ProgramAllowList",
		"PreBuildSteps", "PostBuildSteps", "Version", "VersionName", "FriendlyName",
		"CreatedBy", "CreatedByURL", "DocsURL", "SupportURL", "EnabledByDefault",
		"CanContainContent", "IsBetaVersion", "IsExperimentalVersion", "Installed",
		"ExplicitlyLoaded", "HasExplicitPlatforms", "Hidden", "NoCode", "EpicSampleNameHash",
		// module Type values
		"Runtime", "RuntimeNoCommandlet", "RuntimeAndProgram", "CookedOnly", "UncookedOnly",
		"Developer", "DeveloperTool", "Editor", "EditorNoCommandlet", "EditorAndProgram",
		"Program", "ServerOnly", "ClientOnly", "ClientOnlyNoCommandlet",
		// LoadingPhase values
		"EarliestPossible", "PostConfigInit", "PostSplashScreen", "PreEarlyLoadingScreen",
		"PreLoadingScreen", "PreDefault", "Default", "PostDefault", "PostEngineInit", "None",
		// common platform names for the allow/deny lists
		"Win64", "Linux", "LinuxArm64", "Mac", "Android", "IOS", "TVOS",
	};

	std::error_code ec;
	auto addDirNames = [&](const std::filesystem::path& dir) {
		if (!std::filesystem::is_directory(dir, ec))
			return;
		std::error_code iec;
		for (auto it = std::filesystem::directory_iterator(
				 dir, std::filesystem::directory_options::skip_permission_denied, iec);
			 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
		{
			std::error_code dec;
			if (it->is_directory(dec) && !dec)
				w.push_back(it->path().filename().string());
		}
	};

	// Project modules + project plugin names.
	addDirNames(projectDir / "Source");
	{
		auto plugins = projectDir / "Plugins";
		std::error_code iec;
		if (std::filesystem::is_directory(plugins, ec))
			for (auto it = std::filesystem::directory_iterator(
					 plugins, std::filesystem::directory_options::skip_permission_denied, iec);
				 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
			{
				std::error_code dec;
				if (!it->is_directory(dec) || dec)
					continue;
				w.push_back(it->path().filename().string());
				addDirNames(it->path() / "Source"); // plugin modules too
			}
	}

	if (!engineRoot.empty())
	{
		// Engine modules (dependency names).
		for (const char* cat : { "Runtime", "Editor", "Developer", "Programs" })
			addDirNames(engineRoot / "Engine" / "Source" / cat);
		// Engine plugin names: **.uplugin stems, bounded walk.
		auto plugRoot = engineRoot / "Engine" / "Plugins";
		if (std::filesystem::is_directory(plugRoot, ec))
		{
			int budget = 6000;
			for (auto it = std::filesystem::recursive_directory_iterator(
					 plugRoot, std::filesystem::directory_options::skip_permission_denied, ec);
				 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
			{
				if (--budget <= 0)
					break;
				if (it.depth() > 3)
				{
					it.disable_recursion_pending();
					continue;
				}
				std::error_code fec;
				if (it->is_regular_file(fec) && !fec && it->path().extension() == ".uplugin")
					w.push_back(it->path().stem().string());
			}
		}
	}

	std::sort(w.begin(), w.end());
	w.erase(std::unique(w.begin(), w.end()), w.end());
	return cache.emplace(std::move(key), std::move(w)).first->second;
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
		std::error_code iec;
		for (auto it = std::filesystem::directory_iterator(
				 sourceDir, std::filesystem::directory_options::skip_permission_denied, iec);
			 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
		{
			std::error_code dec;
			if (!it->is_directory(dec) || dec)
				continue;
			if (auto hit = tryModule(it->path()); !hit.empty())
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
		std::error_code iec;
		if (std::filesystem::is_directory(plugins, ec))
			for (auto it = std::filesystem::directory_iterator(
					 plugins, std::filesystem::directory_options::skip_permission_denied, iec);
				 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
			{
				std::error_code dec;
				if (!it->is_directory(dec) || dec)
					continue;
				if (auto hit = trySourceTree(it->path() / "Source"); !hit.empty())
					return hit;
			}
	}

	// 3. UHT-generated headers ("Actor.generated.h"):
	//    Intermediate/Build/Win64/<Target>/Inc/<Module>/UHT/<name>
	if (include.find(".generated.h") != std::string::npos)
	{
		auto wanted = inc.filename();
		auto incRoot = projDir / "Intermediate" / "Build" / "Win64";
		std::error_code iec;
		if (std::filesystem::is_directory(incRoot, ec))
			for (auto ti = std::filesystem::directory_iterator(
					 incRoot, std::filesystem::directory_options::skip_permission_denied, iec);
				 !iec && ti != std::filesystem::directory_iterator(); ti.increment(iec))
			{
				auto incDir = ti->path() / "Inc";
				if (!std::filesystem::is_directory(incDir, ec))
					continue;
				std::error_code mec;
				for (auto mi = std::filesystem::directory_iterator(
						 incDir, std::filesystem::directory_options::skip_permission_denied, mec);
					 !mec && mi != std::filesystem::directory_iterator(); mi.increment(mec))
				{
					auto p = mi->path() / "UHT" / wanted;
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
				 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
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

const std::vector<std::string>& moduleTypes() {
	static const std::vector<std::string> v = {
		"Runtime", "RuntimeNoCommandlet", "RuntimeAndProgram", "CookedOnly", "UncookedOnly",
		"Developer", "DeveloperTool", "Editor", "EditorNoCommandlet", "EditorAndProgram",
		"Program", "ServerOnly", "ClientOnly", "ClientOnlyNoCommandlet",
	};
	return v;
}

const std::vector<std::string>& loadingPhases() {
	static const std::vector<std::string> v = {
		"Default", "PostConfigInit", "PostSplashScreen", "PreEarlyLoadingScreen",
		"PreLoadingScreen", "PreDefault", "PostDefault", "PostEngineInit", "None",
	};
	return v;
}

std::string descriptorAddPlugin(const std::string& json, const std::string& name, bool enabled,
								std::string& err) {
	auto j = parseDescriptor(json); // tolerant: UE descriptors have trailing commas
	if (j.is_discarded() || !j.is_object()) {
		err = "not a valid descriptor (JSON object expected)";
		return {};
	}

	if (!j.contains("Plugins") || !j["Plugins"].is_array()) {
		j["Plugins"] = nlohmann::json::array();
	}

	for (auto& p : j["Plugins"]) {
		if (p.is_object() && p.value("Name", std::string()) == name) {
			p["Enabled"] = enabled; // already present -> just update the flag
			return j.dump(2) + "\n";
		}
	}

	j["Plugins"].push_back({{"Name", name}, {"Enabled", enabled}});
	return j.dump(2) + "\n";
}

std::string descriptorAddModule(const std::string& json, const std::string& name,
								const std::string& type, const std::string& phase, std::string& err) {
	auto j = parseDescriptor(json); // tolerant: UE descriptors have trailing commas
	if (j.is_discarded() || !j.is_object()) {
		err = "not a valid descriptor (JSON object expected)";
		return {};
	}

	if (!j.contains("Modules") || !j["Modules"].is_array()) {
		j["Modules"] = nlohmann::json::array();
	}

	for (auto& m : j["Modules"]) {
		if (m.is_object() && m.value("Name", std::string()) == name) {
			m["Type"] = type; // already present -> update type/phase
			m["LoadingPhase"] = phase;
			return j.dump(2) + "\n";
		}
	}

	j["Modules"].push_back({{"Name", name}, {"Type", type}, {"LoadingPhase", phase}});
	return j.dump(2) + "\n";
}

} // namespace unreal
