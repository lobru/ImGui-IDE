//	BlueprintRegistryJson - serialize the Blueprint node registry to/from JSON.
//
//	The BlueprintEditor's TypeRegistry (the classes / functions / events / enums
//	that back the palette and codegen) is normally filled by hand-written C++
//	(BlueprintLua::SetupUEVRRegistry). This turns that same data into an editable
//	JSON document: Save() dumps the live registry, Load() merges a JSON SDK
//	definition back in. Together they make the API data-driven -- the built-in
//	surface can be exported, edited, and re-imported, and a different game's SDK
//	can be imported at runtime without recompiling.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


#pragma once


#include <string>

#include "BlueprintEditor.h"


namespace BlueprintRegistryJson {
	// Serialize `registry`'s classes + enums to a pretty-printed JSON string.
	std::string Save(const BlueprintEditor::TypeRegistry& registry);

	// Merge the classes + enums described by `jsonText` INTO `registry` (additive:
	// existing entries are left in place, described ones are appended -- FindClass
	// keeps first-wins, so import extends the palette rather than replacing it).
	// Returns false with a human-readable reason in `error` on malformed JSON.
	bool Load(BlueprintEditor::TypeRegistry& registry, const std::string& jsonText, std::string& error);
}
