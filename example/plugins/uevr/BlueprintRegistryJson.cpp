//	BlueprintRegistryJson - see BlueprintRegistryJson.h.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "BlueprintRegistryJson.h"


//
//	Local helpers
//

namespace {
using json = nlohmann::json;
using PinKind = BlueprintEditor::PinKind;
using PinType = BlueprintEditor::PinType;

// PinKind <-> string. Index order MUST match the PinKind enum in BlueprintEditor.h.
const char* const kPinKindNames[] = {
	"Exec", "Boolean", "Byte", "Integer", "Float", "String", "Name", "Vector",
	"Rotator", "Transform", "Object", "Class", "Struct", "Enum", "Delegate", "Wildcard"};
const int kPinKindCount = static_cast<int>(sizeof(kPinKindNames) / sizeof(kPinKindNames[0]));

std::string pinKindToStr(PinKind kind) {
	int i = static_cast<int>(kind);
	return (i >= 0 && i < kPinKindCount) ? kPinKindNames[i] : "Wildcard";
}

PinKind pinKindFromStr(const std::string& name) {
	for (int i = 0; i < kPinKindCount; i++) {
		if (name == kPinKindNames[i]) {
			return static_cast<PinKind>(i);
		}
	}

	return PinKind::Wildcard;
}

json pinTypeToJson(const PinType& type) {
	json j;
	j["kind"] = pinKindToStr(type.kind);

	if (!type.subtype.empty()) {
		j["subtype"] = type.subtype;
	}

	if (type.isArray) {
		j["array"] = true;
	}

	return j;
}

PinType pinTypeFromJson(const json& j) {
	PinType type;

	if (j.is_object()) {
		type.kind = pinKindFromStr(j.value("kind", "Wildcard"));
		type.subtype = j.value("subtype", "");
		type.isArray = j.value("array", false);
	}

	return type;
}

json functionToJson(const BlueprintEditor::Function& f) {
	json j;
	j["name"] = f.name;

	if (!f.category.empty()) { j["category"] = f.category; }
	if (!f.tooltip.empty()) { j["tooltip"] = f.tooltip; }
	if (!f.keywords.empty()) { j["keywords"] = f.keywords; }
	if (!f.metadata.empty()) { j["metadata"] = f.metadata; }
	if (f.isPure) { j["pure"] = true; }
	if (f.isStatic) { j["static"] = true; }

	json params = json::array();

	for (auto& p : f.parameters) {
		json pj;
		pj["name"] = p.name;
		pj["dir"] = p.isOutput ? "out" : "in";
		pj["type"] = pinTypeToJson(p.type);

		if (!p.defaultValue.empty()) {
			pj["default"] = p.defaultValue;
		}

		params.push_back(std::move(pj));
	}

	if (!params.empty()) {
		j["params"] = std::move(params);
	}

	return j;
}

// Populates an already-created Function (from AddFunction/AddEvent) from JSON.
void functionFromJson(BlueprintEditor::Function& f, const json& j) {
	f.name = j.value("name", "");
	f.category = j.value("category", "");
	f.tooltip = j.value("tooltip", "");
	f.keywords = j.value("keywords", "");
	f.metadata = j.value("metadata", "");
	f.isPure = j.value("pure", false);
	f.isStatic = j.value("static", false);

	if (j.contains("params") && j["params"].is_array()) {
		for (auto& pj : j["params"]) {
			BlueprintEditor::Parameter p;
			p.name = pj.value("name", "");
			p.isOutput = pj.value("dir", "in") == "out";
			p.defaultValue = pj.value("default", "");
			p.type = pinTypeFromJson(pj.value("type", json::object()));
			f.parameters.push_back(std::move(p));
		}
	}
}


//
//	UEVR Class Browser reflection-dump format (classes/*.json, scriptstructs/*.json)
//

// A UE reflection property-type string ("ObjectProperty", "IntProperty", ...) -> PinType.
PinType uePropertyType(const std::string& t) {
	if (t == "BoolProperty") { return PinType(PinKind::Boolean); }
	if (t == "ByteProperty") { return PinType(PinKind::Byte); }
	if (t == "IntProperty" || t == "Int8Property" || t == "Int16Property" || t == "Int64Property" ||
	    t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property") {
		return PinType(PinKind::Integer);
	}
	if (t == "FloatProperty" || t == "DoubleProperty") { return PinType(PinKind::Float); }
	if (t == "StrProperty" || t == "TextProperty" || t == "Utf8StrProperty" || t == "AnsiStrProperty") {
		return PinType(PinKind::String);
	}
	if (t == "NameProperty") { return PinType(PinKind::Name); }
	if (t == "StructProperty") { return PinType(PinKind::Struct); }
	if (t == "EnumProperty") { return PinType(PinKind::Enum); }
	if (t == "ClassProperty" || t == "SoftClassProperty" || t == "SoftClassPath") {
		return PinType(PinKind::Class, "UObject");
	}
	if (t == "ObjectProperty" || t == "WeakObjectProperty" || t == "LazyObjectProperty" ||
	    t == "SoftObjectProperty" || t == "InterfaceProperty") {
		return PinType(PinKind::Object, "UObject");
	}
	if (t == "ArrayProperty" || t == "SetProperty" || t == "MapProperty") {
		return PinType(PinKind::Wildcard, "", true);
	}
	if (t == "DelegateProperty" || t == "MulticastDelegateProperty" ||
	    t == "MulticastInlineDelegateProperty" || t == "MulticastSparseDelegateProperty") {
		return PinType(PinKind::Delegate);
	}

	return PinType(PinKind::Wildcard);
}

// True if `obj`'s "flag_names" array contains `flag`.
bool hasUeFlag(const json& obj, const char* flag) {
	auto it = obj.find("flag_names");
	if (it == obj.end() || !it->is_array()) {
		return false;
	}

	for (auto& x : *it) {
		if (x.is_string() && x.get<std::string>() == flag) {
			return true;
		}
	}

	return false;
}

// Distinguishes a UEVR reflection dump from this module's own Save() schema.
bool looksLikeUevrDump(const json& root) {
	for (const char* key : {"classes", "scriptstructs"}) {
		if (root.contains(key) && root[key].is_array() && !root[key].empty()) {
			const auto& first = root[key][0];
			if (first.is_object() && (first.contains("full_name") || first.contains("super"))) {
				return true;
			}
		}
	}

	return false;
}

// Add one UEVR-dump class/struct to the registry. UE functions are exposed the way
// the rest of the UEVR registry calls reflected methods: obj["Name"](obj, args...)
// (see the generic "Call (N Args)" nodes in BlueprintLua.cpp). Out/return params
// become a single Return Value output; extra out-params aren't modeled (a single
// expression can't cleanly capture UE multi-out returns — a Custom Lua node can).
void loadUevrClass(BlueprintEditor::TypeRegistry& registry, const json& cj) {
	if (!cj.is_object() || !cj.contains("name")) {
		return;
	}

	std::string name = cj.value("name", "");
	BlueprintEditor::Class& cls = registry.AddClass(name, cj.value("super", ""), cj.value("full_name", ""));

	if (cj.contains("properties") && cj["properties"].is_array()) {
		for (auto& pj : cj["properties"]) {
			if (pj.is_object() && pj.contains("name")) {
				cls.AddProperty(pj.value("name", ""), uePropertyType(pj.value("type", "")), name);
			}
		}
	}

	if (!cj.contains("functions") || !cj["functions"].is_array()) {
		return;
	}

	for (auto& fj : cj["functions"]) {
		if (!fj.is_object() || !fj.contains("name")) {
			continue;
		}

		std::string fname = fj.value("name", "");
		BlueprintEditor::Function& f = cls.AddFunction(fname, name);

		if (hasUeFlag(fj, "BlueprintPure")) {
			f.Pure();
		}

		std::vector<std::pair<std::string, PinType>> inputs;
		PinType retType;
		bool hasRet = false;

		if (fj.contains("params") && fj["params"].is_array()) {
			for (auto& pj : fj["params"]) {
				if (!pj.is_object()) {
					continue;
				}

				PinType pt = uePropertyType(pj.value("type", ""));
				bool isRet = hasUeFlag(pj, "ReturnParm");
				bool isOut = hasUeFlag(pj, "OutParm");

				if (isRet || (isOut && !hasRet)) {
					retType = pt;
					hasRet = true;

				} else if (!isOut) {
					inputs.push_back({pj.value("name", ""), pt});
				}
			}
		}

		std::string meta = "{target}[\"" + fname + "\"]({target}";

		for (size_t i = 0; i < inputs.size(); i++) {
			f.In(inputs[i].first, inputs[i].second);
			meta += ", {" + std::to_string(i) + "}";
		}

		meta += ")";

		if (hasRet) {
			f.Ret(retType);
		}

		f.Metadata(meta);
	}
}

void loadUevrDump(BlueprintEditor::TypeRegistry& registry, const json& root) {
	for (const char* key : {"classes", "scriptstructs"}) {
		if (root.contains(key) && root[key].is_array()) {
			for (auto& cj : root[key]) {
				loadUevrClass(registry, cj);
			}
		}
	}
}

std::string trimmed(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) { a++; }
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { b--; }
	return s.substr(a, b - a);
}
} // namespace


//
//	BlueprintRegistryJson::Save
//

std::string BlueprintRegistryJson::Save(const BlueprintEditor::TypeRegistry& registry) {
	json root;
	json classes = json::array();

	for (auto& cptr : registry.GetClasses()) {
		const BlueprintEditor::Class& c = *cptr;
		json cj;
		cj["name"] = c.name;

		if (!c.parentName.empty()) { cj["parent"] = c.parentName; }
		if (!c.tooltip.empty()) { cj["tooltip"] = c.tooltip; }
		if (!c.metadata.empty()) { cj["metadata"] = c.metadata; }

		json props = json::array();

		for (auto& p : c.properties) {
			json pj;
			pj["name"] = p.name;
			pj["type"] = pinTypeToJson(p.type);

			if (!p.category.empty()) { pj["category"] = p.category; }
			if (!p.tooltip.empty()) { pj["tooltip"] = p.tooltip; }

			props.push_back(std::move(pj));
		}

		if (!props.empty()) { cj["properties"] = std::move(props); }

		json funcs = json::array();

		for (auto& f : c.functions) {
			funcs.push_back(functionToJson(f));
		}

		if (!funcs.empty()) { cj["functions"] = std::move(funcs); }

		json events = json::array();

		for (auto& e : c.events) {
			events.push_back(functionToJson(e));
		}

		if (!events.empty()) { cj["events"] = std::move(events); }

		classes.push_back(std::move(cj));
	}

	root["classes"] = std::move(classes);

	json enums = json::array();

	for (auto& eptr : registry.GetEnums()) {
		json ej;
		ej["name"] = eptr->name;
		ej["values"] = eptr->values;
		enums.push_back(std::move(ej));
	}

	root["enums"] = std::move(enums);
	return root.dump(2);
}


//
//	BlueprintRegistryJson::Load
//

bool BlueprintRegistryJson::Load(BlueprintEditor::TypeRegistry& registry, const std::string& jsonText, std::string& error) {
	json root;

	try {
		root = json::parse(jsonText);

	} catch (const std::exception& e) {
		error = e.what();
		return false;
	}

	if (!root.is_object()) {
		error = "top-level JSON is not an object";
		return false;
	}

	// A UEVR Class Browser reflection dump goes through the dedicated converter;
	// anything else is treated as this module's own Save() schema.
	if (looksLikeUevrDump(root)) {
		loadUevrDump(registry, root);
		return true;
	}

	if (root.contains("classes") && root["classes"].is_array()) {
		for (auto& cj : root["classes"]) {
			if (!cj.is_object() || !cj.contains("name")) {
				continue;
			}

			BlueprintEditor::Class& cls =
				registry.AddClass(cj.value("name", ""), cj.value("parent", ""), cj.value("tooltip", ""));
			cls.metadata = cj.value("metadata", "");

			if (cj.contains("properties") && cj["properties"].is_array()) {
				for (auto& pj : cj["properties"]) {
					cls.AddProperty(pj.value("name", ""), pinTypeFromJson(pj.value("type", json::object())),
					                pj.value("category", ""), pj.value("tooltip", ""));
				}
			}

			if (cj.contains("functions") && cj["functions"].is_array()) {
				for (auto& fj : cj["functions"]) {
					functionFromJson(cls.AddFunction(""), fj);
				}
			}

			if (cj.contains("events") && cj["events"].is_array()) {
				for (auto& ej : cj["events"]) {
					functionFromJson(cls.AddEvent(""), ej);
				}
			}
		}
	}

	if (root.contains("enums") && root["enums"].is_array()) {
		for (auto& ej : root["enums"]) {
			if (!ej.is_object() || !ej.contains("name")) {
				continue;
			}

			std::vector<std::string> values;

			if (ej.contains("values") && ej["values"].is_array()) {
				for (auto& v : ej["values"]) {
					if (v.is_string()) {
						values.push_back(v.get<std::string>());
					}
				}
			}

			registry.AddEnum(ej.value("name", ""), values);
		}
	}

	return true;
}


//
//	BlueprintRegistryJson::LoadEnumLua
//

int BlueprintRegistryJson::LoadEnumLua(BlueprintEditor::TypeRegistry& registry, const std::string& luaText) {
	// UE4SS annotation shape:
	//     ---@enum EAxis
	//     EAxis = {
	//         X = 1,
	//         ...
	//     }
	std::istringstream in(luaText);
	std::string raw;
	std::string pendingEnum;
	std::vector<std::string> values;
	bool inBody = false;
	int added = 0;

	while (std::getline(in, raw)) {
		std::string line = trimmed(raw);

		if (!inBody) {
			const std::string tag = "---@enum ";

			if (line.rfind(tag, 0) == 0) {
				// first whitespace-delimited token after the tag is the enum name
				std::string rest = trimmed(line.substr(tag.size()));
				size_t sp = rest.find_first_of(" \t");
				pendingEnum = sp == std::string::npos ? rest : rest.substr(0, sp);
				values.clear();

			} else if (!pendingEnum.empty() && line.find('{') != std::string::npos) {
				inBody = true; // the "Name = {" line
			}

			continue;
		}

		if (line.find('}') != std::string::npos) {
			registry.AddEnum(pendingEnum, values);
			added++;
			pendingEnum.clear();
			inBody = false;
			continue;
		}

		size_t eq = line.find('=');

		if (eq != std::string::npos) {
			std::string key = trimmed(line.substr(0, eq));

			if (!key.empty()) {
				values.push_back(key);
			}
		}
	}

	return added;
}


//
//	BlueprintRegistryJson::ExposeClass
//

bool BlueprintRegistryJson::ExposeClass(BlueprintEditor::TypeRegistry& dst, const BlueprintEditor::TypeRegistry& src,
                                        const std::string& className) {
	const BlueprintEditor::Class* from = src.FindClass(className);

	if (!from || dst.FindClass(className)) {
		return false;
	}

	BlueprintEditor::Class& to = dst.AddClass(from->name, from->parentName, from->tooltip);
	to.metadata = from->metadata;
	to.properties = from->properties;
	to.functions = from->functions;
	to.events = from->events;
	to.paletteHidden = true; // reachable via contextual drag, never in the flat palette
	return true;
}


//
//	BlueprintRegistryJson::LoadSdkDir
//

int BlueprintRegistryJson::LoadSdkDir(BlueprintEditor::TypeRegistry& registry, const std::filesystem::path& dir,
                                      const std::function<void(const std::string&)>& log) {
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) {
		return 0;
	}

	auto note = [&](const std::string& msg) {
		if (log) {
			log(msg);
		}
	};

	int classesBefore = static_cast<int>(registry.GetClasses().size());

	// Non-throwing recursive walk (recursive_directory_iterator's operator++ throws
	// even when constructed with an error_code, so drive it with increment(ec)).
	std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
	std::filesystem::recursive_directory_iterator end;

	for (; !ec && it != end; it.increment(ec)) {
		std::error_code fec;
		if (!it->is_regular_file(fec) || fec) {
			continue;
		}

		std::filesystem::path path = it->path();
		std::string ext = path.extension().string();
		std::ifstream file(path, std::ios::binary);

		if (!file) {
			continue;
		}

		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::string leaf = path.filename().string();

		if (ext == ".json") {
			std::string err;
			if (Load(registry, text, err)) {
				note("sdk: loaded " + leaf);
			} else {
				note("sdk: skipped " + leaf + " (" + err + ")");
			}

		} else if (ext == ".lua" && text.find("---@enum") != std::string::npos) {
			// class annotation .lua files duplicate the .json; only enum files add
			int n = LoadEnumLua(registry, text);
			note("sdk: loaded " + std::to_string(n) + " enum(s) from " + leaf);
		}
	}

	return static_cast<int>(registry.GetClasses().size()) - classesBefore;
}
