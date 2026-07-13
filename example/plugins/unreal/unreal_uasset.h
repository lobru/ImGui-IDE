//	ImGui-IDE — .uasset/.umap package inspection (pure logic, no UI).
//
//	A small native reader for Unreal's package header (FPackageFileSummary): tag,
//	engine file versions, flags, the name table, and the import map ("what classes
//	does this asset reference"). Enough to inspect an asset inside the IDE without
//	external tools; full property editing stays in UAssetAPI/CUE4Parse territory.
//
//	Version handling: versioned packages gate optional summary fields on the real
//	FileVersionUE4/UE5; unversioned (cooked) packages assume a modern UE5 layout
//	and degrade gracefully — every read is bounds-checked, and a field that lands
//	outside the file simply truncates what we report instead of failing the parse.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace unreal::uasset {

struct Import {
	std::string classPackage; // "/Script/Engine"
	std::string className;    // "StaticMesh"
	std::string objectName;   // "SM_Rock"
	int32_t outerIndex = 0;
};

struct Summary {
	bool valid = false;       // tag matched and the fixed header parsed
	std::string error;        // human-readable reason when !valid

	int32_t legacyFileVersion = 0;
	int32_t fileVersionUE4 = 0;
	int32_t fileVersionUE5 = 0;
	int32_t fileVersionLicensee = 0;
	bool unversioned = false; // cooked asset with stripped versions (layout assumed)
	uint32_t packageFlags = 0;
	int32_t totalHeaderSize = 0;
	std::string folderName;
	std::string localizationId;

	int32_t nameCount = 0;
	int32_t exportCount = 0;
	int32_t importCount = 0;

	std::vector<std::string> names; // the name table (may be truncated on damage)
	std::vector<Import> imports;    // the import map (may be empty if unparseable)
};

// Blueprint-level view derived from the header (import map + name table). No export
// graph is parsed — full function/variable/component enumeration needs the export
// map (UAssetAPI/CUE4Parse); these fields are a best-effort, clearly-labelled
// heuristic that is reliable for detection, the generated class, and the parent.
struct BlueprintSummary {
	bool isBlueprint = false;
	std::string generatedClass;          // e.g. "BP_Door_C" (a "*_C" name-table entry)
	std::string parentClass;             // best-effort native parent (e.g. "Actor")
	std::vector<std::string> classes;    // referenced UClass imports (parent + components + ...)
};

// Classify a parsed package as a Blueprint and pull what the header reliably yields.
BlueprintSummary analyzeBlueprint(const Summary& summary);

// Parse a package from a memory buffer / from disk.
Summary parse(const void* data, size_t size);
Summary parseFile(const std::filesystem::path& path);

// A human-readable multi-line report of a Summary (what the IDE displays/opens).
// Appends a Blueprint section when analyzeBlueprint detects one.
std::string report(const Summary& summary, const std::string& title);

} // namespace unreal::uasset
