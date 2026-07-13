//	ImGui-IDE — .uasset/.umap package inspection. See unreal_uasset.h.
//
//	Field order follows UE's FPackageFileSummary/FObjectImport serializers. The
//	version gates used here (numeric so no UE headers are needed):
//	  VER_UE4_SERIALIZE_TEXT_IN_PACKAGES            = 459  (gatherable text block)
//	  VER_UE4_NAME_HASHES_SERIALIZED                = 504  (2x uint16 after each name)
//	  VER_UE4_ADDED_PACKAGE_SUMMARY_LOCALIZATION_ID = 516  (LocalizationId string)
//	  UE5 ADDED_SOFT_OBJECT_PATH_LIST               = 1008 (soft-object-path block)
//	  UE5 OPTIONAL_RESOURCES                        = 1003 (bImportOptional per import)

#include <cstring>
#include <fstream>
#include <iterator>

#include "unreal_uasset.h"

namespace unreal::uasset {

namespace {

constexpr uint32_t kPackageTag = 0x9E2A83C1u;

// Bounds-checked little-endian cursor. Any out-of-range read sets fail and
// returns zero values; callers check fail at the milestones they care about.
struct Reader {
	const uint8_t* data;
	size_t size;
	size_t pos = 0;
	bool fail = false;

	bool has(size_t n) const { return !fail && pos + n <= size; }

	uint32_t u32() {
		if (!has(4)) { fail = true; return 0; }
		uint32_t v;
		std::memcpy(&v, data + pos, 4);
		pos += 4;
		return v;
	}

	int32_t i32() { return static_cast<int32_t>(u32()); }

	uint16_t u16() {
		if (!has(2)) { fail = true; return 0; }
		uint16_t v;
		std::memcpy(&v, data + pos, 2);
		pos += 2;
		return v;
	}

	void skip(size_t n) {
		if (!has(n)) { fail = true; return; }
		pos += n;
	}

	// Serialized FString: int32 len; >0 = ASCII bytes incl NUL, <0 = UTF-16LE of
	// -len char16s incl NUL. Non-ASCII UTF-16 chars degrade to '?'.
	std::string fstring() {
		int32_t len = i32();

		if (fail || len == 0) { return ""; }

		std::string out;

		if (len > 0) {
			if (len > 1024 * 1024 || !has(static_cast<size_t>(len))) { fail = true; return ""; }
			out.assign(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(len));
			pos += static_cast<size_t>(len);

		} else {
			size_t chars = static_cast<size_t>(-static_cast<int64_t>(len));
			if (chars > 1024 * 1024 || !has(chars * 2)) { fail = true; return ""; }

			for (size_t i = 0; i < chars; i++) {
				uint16_t c;
				std::memcpy(&c, data + pos + i * 2, 2);
				out += (c < 128) ? static_cast<char>(c) : '?';
			}

			pos += chars * 2;
		}

		while (!out.empty() && out.back() == '\0') { out.pop_back(); }
		return out;
	}
};

// FName in the header maps: name-table index + instance number.
std::string readFName(Reader& r, const std::vector<std::string>& names) {
	int32_t index = r.i32();
	int32_t number = r.i32();

	if (r.fail || index < 0 || static_cast<size_t>(index) >= names.size()) {
		return "<bad name>";
	}

	std::string out = names[static_cast<size_t>(index)];

	if (number > 0) {
		out += "_" + std::to_string(number - 1);
	}

	return out;
}

} // namespace

Summary parse(const void* rawData, size_t size) {
	Summary s;
	Reader r{static_cast<const uint8_t*>(rawData), size};

	if (r.u32() != kPackageTag) {
		s.error = "not an Unreal package (missing 0x9E2A83C1 tag)";
		return s;
	}

	s.legacyFileVersion = r.i32();

	if (s.legacyFileVersion >= 0 || s.legacyFileVersion < -10) {
		s.error = "unsupported legacy file version " + std::to_string(s.legacyFileVersion);
		return s;
	}

	if (s.legacyFileVersion != -4) {
		r.i32(); // LegacyUE3Version
	}

	s.fileVersionUE4 = r.i32();

	if (s.legacyFileVersion <= -8) {
		s.fileVersionUE5 = r.i32();
	}

	s.fileVersionLicensee = r.i32();

	// custom versions: count x (16-byte guid + int32)
	int32_t customVersions = r.i32();

	if (customVersions < 0 || customVersions > 1024) {
		s.error = "implausible custom-version count";
		return s;
	}

	r.skip(static_cast<size_t>(customVersions) * 20);

	// cooked packages strip the versions; assume the newest layout we know
	s.unversioned = s.fileVersionUE4 == 0 && s.fileVersionUE5 == 0 && s.fileVersionLicensee == 0;
	int32_t ue4 = s.unversioned ? 522 : s.fileVersionUE4;
	int32_t ue5 = s.unversioned ? 1013 : s.fileVersionUE5;

	s.totalHeaderSize = r.i32();
	s.folderName = r.fstring();
	s.packageFlags = r.u32();
	s.nameCount = r.i32();
	int32_t nameOffset = r.i32();

	if (ue5 >= 1008) {
		r.i32(); // SoftObjectPathsCount
		r.i32(); // SoftObjectPathsOffset
	}

	if (ue4 >= 516) {
		s.localizationId = r.fstring();
	}

	if (ue4 >= 459) {
		r.i32(); // GatherableTextDataCount
		r.i32(); // GatherableTextDataOffset
	}

	s.exportCount = r.i32();
	r.i32(); // ExportOffset
	s.importCount = r.i32();
	int32_t importOffset = r.i32();

	if (r.fail) {
		s.error = "truncated package summary";
		return s;
	}

	s.valid = true; // the fixed header parsed; tables below degrade gracefully

	// ── name table ─────────────────────────────────────────────────────────
	if (s.nameCount > 0 && s.nameCount < 4 * 1024 * 1024 && nameOffset > 0 &&
		static_cast<size_t>(nameOffset) < size) {
		Reader n{r.data, size, static_cast<size_t>(nameOffset)};

		for (int32_t i = 0; i < s.nameCount && !n.fail; i++) {
			std::string name = n.fstring();

			if (ue4 >= 504) {
				n.u16(); // NonCasePreservingHash
				n.u16(); // CasePreservingHash
			}

			if (!n.fail) {
				s.names.push_back(std::move(name));
			}
		}
	}

	// ── import map ─────────────────────────────────────────────────────────
	if (!s.names.empty() && s.importCount > 0 && s.importCount < 1024 * 1024 &&
		importOffset > 0 && static_cast<size_t>(importOffset) < size) {
		Reader m{r.data, size, static_cast<size_t>(importOffset)};

		for (int32_t i = 0; i < s.importCount && !m.fail; i++) {
			Import imp;
			imp.classPackage = readFName(m, s.names);
			imp.className = readFName(m, s.names);
			imp.outerIndex = m.i32();
			imp.objectName = readFName(m, s.names);

			if (ue5 >= 1003) {
				m.i32(); // bImportOptional
			}

			if (!m.fail) {
				s.imports.push_back(std::move(imp));
			}
		}
	}

	return s;
}

Summary parseFile(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);

	if (!in) {
		Summary s;
		s.error = "cannot open " + path.string();
		return s;
	}

	std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return parse(bytes.data(), bytes.size());
}

BlueprintSummary analyzeBlueprint(const Summary& s) {
	BlueprintSummary b;
	auto contains = [](const std::string& hay, const char* needle) {
		return hay.find(needle) != std::string::npos;
	};

	for (auto& imp : s.imports) {
		// A UClass import ("className == Class") is a class this asset references —
		// the native parent is among these, alongside any component classes.
		if (imp.className == "Class") {
			b.classes.push_back(imp.objectName);
		}

		if (contains(imp.className, "BlueprintGeneratedClass") ||
			contains(imp.objectName, "BlueprintGeneratedClass") ||
			imp.className == "Blueprint" || imp.objectName == "Blueprint" ||
			imp.className == "WidgetBlueprintGeneratedClass" ||
			imp.className == "AnimBlueprintGeneratedClass") {
			b.isBlueprint = true;
		}
	}

	for (auto& n : s.names) {
		if (n == "BlueprintGeneratedClass" || n == "WidgetBlueprintGeneratedClass" ||
			n == "AnimBlueprintGeneratedClass") {
			b.isBlueprint = true;
		}
	}

	// The generated class is the "*_C" name-table entry (BP_Door -> BP_Door_C).
	for (auto& n : s.names) {
		if (n.size() > 2 && n.compare(n.size() - 2, 2, "_C") == 0 &&
			n.find(' ') == std::string::npos && n.find('/') == std::string::npos) {
			b.generatedClass = n;
			break;
		}
	}

	// Best-effort parent: the first referenced class that isn't a BP/meta class.
	for (auto& c : b.classes) {
		if (c == "BlueprintGeneratedClass" || c == "WidgetBlueprintGeneratedClass" ||
			c == "AnimBlueprintGeneratedClass" || c == "Blueprint" || c == "Object" ||
			c == "Class" || c == "Package") {
			continue;
		}
		b.parentClass = c;
		break;
	}

	return b;
}

std::string report(const Summary& s, const std::string& title) {
	std::string out = "UAsset inspection — " + title + "\n";
	out += std::string(out.size() - 1, '=') + "\n\n";

	if (!s.valid) {
		out += "PARSE FAILED: " + s.error + "\n";
		return out;
	}

	// Blueprint-level summary first (the most useful view when the asset is a BP).
	BlueprintSummary bp = analyzeBlueprint(s);
	if (bp.isBlueprint) {
		out += "Blueprint asset\n---------------\n";
		out += "  Generated class:  " + (bp.generatedClass.empty() ? "(unknown)" : bp.generatedClass) + "\n";
		out += "  Parent (guess):   " + (bp.parentClass.empty() ? "(unknown)" : bp.parentClass) + "\n";
		if (!bp.classes.empty()) {
			out += "  Referenced classes:";
			for (auto& c : bp.classes) {
				out += " " + c;
			}
			out += "\n";
		}
		out += "  Note: functions/variables/components need the export graph "
			   "(UAssetAPI/CUE4Parse); the above is derived from the package header.\n\n";
	}

	out += "File versions:  UE4 " + std::to_string(s.fileVersionUE4) +
		   "  UE5 " + std::to_string(s.fileVersionUE5) +
		   "  licensee " + std::to_string(s.fileVersionLicensee) +
		   (s.unversioned ? "  (unversioned/cooked — modern layout assumed)" : "") + "\n";
	out += "Header size:    " + std::to_string(s.totalHeaderSize) + " bytes\n";
	out += "Package flags:  0x" + [](uint32_t f) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08X", f);
		return std::string(buf);
	}(s.packageFlags) + "\n";

	if (!s.folderName.empty() && s.folderName != "None") {
		out += "Folder:         " + s.folderName + "\n";
	}

	out += "Counts:         " + std::to_string(s.nameCount) + " names, " +
		   std::to_string(s.importCount) + " imports, " +
		   std::to_string(s.exportCount) + " exports\n";

	if (!s.imports.empty()) {
		out += "\nImports (referenced classes/objects):\n";

		for (auto& imp : s.imports) {
			out += "  " + imp.classPackage + "." + imp.className + "  " + imp.objectName + "\n";
		}
	}

	if (!s.names.empty()) {
		out += "\nName table (" + std::to_string(s.names.size()) + "):\n";

		for (auto& n : s.names) {
			out += "  " + n + "\n";
		}
	}

	return out;
}

} // namespace unreal::uasset
