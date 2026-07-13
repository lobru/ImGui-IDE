//	ImGui-IDE — Unreal Engine 5 project integration (pure logic, no UI).
//
//	Discovers the .uproject for a directory, resolves its engine install
//	(EngineAssociation → registry for launcher installs / per-user source-build
//	GUIDs / default install dir / relative source checkout), and builds the
//	UnrealBuildTool command lines the app runs through the normal build/output
//	pipeline:
//	  - buildEditorCommand:     Build.bat <Project>Editor Win64 Development
//	  - generateClangDbCommand: UBT -mode=GenerateClangDatabase with the output
//	    dropped in the PROJECT root, where the bundled clangd auto-discovers it
//	    (same net effect as the VS/VSCode project generators). UE 5.3+.
//	Windows-first: on POSIX the engine probes simply fail → empty commands.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace unreal {

// The .uproject in `searchStart` or the nearest ancestor (12 levels). Empty if none.
std::filesystem::path findUProject(const std::filesystem::path& searchStart);

// Engine root for a .uproject. Fills `engineAssociation` with the raw association
// string (version like "5.4", a source-build GUID, or "" for a relative checkout).
std::filesystem::path findEngineRoot(const std::filesystem::path& uproject, std::string& engineAssociation);

// "<ProjectName>Editor" — the standard editor target for a C++ project.
std::string targetName(const std::filesystem::path& uproject);

// True when the project has a Source/ dir (C++ project; Blueprint-only has none).
bool hasCppSource(const std::filesystem::path& uproject);

// Full command line to build the editor target via UnrealBuildTool. Empty when the
// engine's Build.bat can't be found or the project has no C++ source.
std::string buildEditorCommand(const std::filesystem::path& engineRoot, const std::filesystem::path& uproject);

// Full command line to generate compile_commands.json INTO THE PROJECT ROOT
// (UBT -mode=GenerateClangDatabase -OutputDir=..., UE 5.3+). clangd rooted at the
// project then picks it up automatically. Empty when Build.bat is missing.
std::string generateClangDbCommand(const std::filesystem::path& engineRoot, const std::filesystem::path& uproject);

// Path to UnrealEditor.exe for launching the project (empty if absent).
std::filesystem::path editorBinary(const std::filesystem::path& engineRoot);

// Autocomplete word list for editing .uproject/.uplugin descriptors: schema keys,
// module Type / LoadingPhase enum values, and DISCOVERED names — project modules
// (Source/*), project plugins, engine plugins, engine modules — so dependency,
// plugin, and module fields complete against what actually exists. Cached per
// (engineRoot, projectDir); the first call does the directory scans.
const std::vector<std::string>& descriptorWords(const std::filesystem::path& engineRoot,
												const std::filesystem::path& projectDir);

// Resolve a UE-style module-relative include ("GameFramework/Actor.h",
// "CoreMinimal.h", "Actor.generated.h") against the project's and engine's module
// include roots (<Module>/Public, <Module>/Classes, module root), project plugins,
// engine plugins, and UHT-generated headers under Intermediate/. Empty if not found.
// This is what makes "Go to File" work on engine headers.
std::filesystem::path resolveInclude(const std::filesystem::path& engineRoot,
									 const std::filesystem::path& uproject,
									 const std::string& include);

// ── Interactive .uproject / descriptor editing ──────────────────────────────
// The valid picklists UE offers for a module descriptor (used to drive dropdowns
// so you never hand-type a wrong value).
const std::vector<std::string>& moduleTypes();     // Runtime, Editor, Developer, ...
const std::vector<std::string>& loadingPhases();   // Default, PostConfigInit, ...

// Add a Plugin dependency to a .uproject/.uplugin JSON string, returning the new
// text. Idempotent (a plugin already present is left alone). Empty return + err on
// parse failure.
std::string descriptorAddPlugin(const std::string& json, const std::string& name, bool enabled,
								std::string& err);

// Add a Module entry to a .uproject/.uplugin JSON string. `type` and `phase` come
// from moduleTypes()/loadingPhases(). Idempotent on the module name.
std::string descriptorAddModule(const std::string& json, const std::string& name,
								const std::string& type, const std::string& phase, std::string& err);

} // namespace unreal
