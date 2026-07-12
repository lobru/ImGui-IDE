//	ImGui-IDE — Unreal Engine source scaffolding (pure logic, no UI).
//
//	Generates the same boilerplate the UE editor's "New C++ Class" wizard emits
//	(UCLASS + GENERATED_BODY + the parent's canonical overrides) and UEFN Verse
//	creative-device scaffolds. Pure string builders — the plugin UI decides where
//	the files land (classFilePaths honors a module's Public/Private split).

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace unreal::codegen {

// A supported parent class for the "New UE C++ Class" wizard.
struct ParentClass {
	const char* name;      // "AActor", "UActorComponent", ...
	const char* include;   // "GameFramework/Actor.h", ...
	const char* blurb;     // one-line description shown in the picker
};

// The wizard's parent choices, in menu order (Actor first, like the UE wizard).
const std::vector<ParentClass>& parentClasses();

// What the generator needs to know. className is the BARE name ("MyActor");
// the A/U prefix is derived from the parent. moduleName drives the _API macro.
struct ClassSpec {
	std::string className;   // "MyActor" (no prefix)
	std::string parentClass; // one of parentClasses()[i].name
	std::string moduleName;  // "MyGame" -> MYGAME_API
};

// "AMyActor" / "UMyComponent" — parent's prefix letter + bare name.
std::string prefixedClassName(const ClassSpec& spec);

// The full <Name>.h / <Name>.cpp contents (empty on an unknown parent).
std::string generateClassHeader(const ClassSpec& spec);
std::string generateClassSource(const ClassSpec& spec);

// Where the pair should land for a module rooted at moduleDir: Public/Private
// when those dirs exist (the UE convention), else both next to the .Build.cs.
std::pair<std::filesystem::path, std::filesystem::path>
classFilePaths(const std::filesystem::path& moduleDir, const std::string& className);

// Module directories (containing *.Build.cs) under <project>/Source.
std::vector<std::filesystem::path> moduleDirs(const std::filesystem::path& uproject);

// A UEFN Verse creative-device scaffold. deviceName should be snake_case
// ("my_device"); sanitizeVerseName coerces arbitrary input to that convention.
std::string sanitizeVerseName(const std::string& raw);
std::string generateVerseDevice(const std::string& deviceName);

} // namespace unreal::codegen
