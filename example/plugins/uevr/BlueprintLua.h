//	BlueprintLua - a UEVR Lua scripting backend for BlueprintEditor.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


#pragma once


//
//	Include files
//

#include <string>
#include <vector>

#include "BlueprintEditor.h"


//
//	BlueprintLua
//
//	Turns BlueprintEditor graphs into UEVR Lua scripts.
//
//	SetupUEVRRegistry populates a BlueprintEditor reflection registry with the
//	UEVR Lua scripting API surface (engine tick / XInput / UI callbacks, the
//	uevr.api object, UObject property and function access, VR runtime calls,
//	XInput helpers and Lua math/string/logic utilities). Each function carries
//	a Lua expression template in its metadata which the generator expands.
//
//	GenerateScript walks the graph and emits a ready-to-run UEVR Lua script:
//	event nodes become uevr.sdk.callbacks registrations, exec wires become
//	statements, data wires become expressions, blueprint variables become
//	script-scope locals and flow control nodes become native Lua control flow.
//

namespace BlueprintLua {
	// populate the registry with the UEVR Lua scripting API and set the blueprint parent class to "UEVR"
	void SetupUEVRRegistry(BlueprintEditor& editor);

	// generate a UEVR Lua script from the blueprint graph
	std::string GenerateScript(const BlueprintEditor& editor);

	// curated UEVR Lua API identifiers (namespaces, callback names, api/vr/xinput
	// method names, uobject accessors) for a host editor's Lua autocomplete —
	// mirrors the surface SetupUEVRRegistry installs.
	const std::vector<std::string>& LuaApiIdentifiers();
}
