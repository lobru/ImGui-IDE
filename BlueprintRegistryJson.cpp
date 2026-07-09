//	BlueprintRegistryJson - see BlueprintRegistryJson.h.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#include <exception>
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
