//	BlueprintLua - a UEVR Lua scripting backend for BlueprintEditor.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BlueprintImGuiNames.h"
#include "BlueprintLua.h"


//
//	Local helpers
//

using Pin = BlueprintEditor::Pin;
using PinKind = BlueprintEditor::PinKind;
using PinType = BlueprintEditor::PinType;
using Node = BlueprintEditor::Node;
using NodeKind = BlueprintEditor::NodeKind;
using ID = BlueprintEditor::ID;

// turn an arbitrary display name into a Lua identifier
static std::string identifier(const std::string& name) {
	std::string result;

	for (auto c : name) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
			result += c;
		}
	}

	if (result.empty()) {
		result = "value";
	}

	if (std::isdigit(static_cast<unsigned char>(result[0]))) {
		result = "_" + result;
	}

	return result;
}

// turn a display name into snake_case (used when a function has no template)
static std::string snakeCase(const std::string& name) {
	std::string result;
	bool lastWasSpace = false;

	for (auto c : name) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			if (lastWasSpace && !result.empty()) {
				result += '_';
			}

			result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			lastWasSpace = false;

		} else {
			lastWasSpace = true;
		}
	}

	return result.empty() ? "call" : result;
}

// quote a string as a Lua literal
static std::string luaString(const std::string& text) {
	std::string result = "\"";

	for (auto c : text) {
		if (c == '\\') { result += "\\\\"; }
		else if (c == '"') { result += "\\\""; }
		else if (c == '\n') { result += "\\n"; }
		else if (c == '\t') { result += "\\t"; }
		else { result += c; }
	}

	result += "\"";
	return result;
}

// format a numeric default (fall back to 0)
static std::string luaNumber(const std::string& text, bool isFloat) {
	if (text.empty()) {
		return isFloat ? "0.0" : "0";
	}

	char* end = nullptr;
	double value = std::strtod(text.c_str(), &end);

	if (end == text.c_str()) {
		return isFloat ? "0.0" : "0";
	}

	char buffer[64];

	if (isFloat) {
		std::snprintf(buffer, sizeof(buffer), "%g", value);
		std::string result = buffer;

		// make sure it reads as a float (but leave "inf"/"nan" alone)
		if (result.find_first_of(".eni") == std::string::npos) {
			result += ".0";
		}

		return result;
	}

	std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(value));
	return buffer;
}

// XInput button masks (see XINPUT_GAMEPAD_*)
static const std::unordered_map<std::string, const char*>& xinputButtonMasks() {
	static const std::unordered_map<std::string, const char*> masks = {
		{"DPad Up", "0x0001"},
		{"DPad Down", "0x0002"},
		{"DPad Left", "0x0004"},
		{"DPad Right", "0x0008"},
		{"Start", "0x0010"},
		{"Back", "0x0020"},
		{"Left Thumb", "0x0040"},
		{"Right Thumb", "0x0080"},
		{"Left Bumper", "0x0100"},
		{"Right Bumper", "0x0200"},
		{"A", "0x1000"},
		{"B", "0x2000"},
		{"X", "0x4000"},
		{"Y", "0x8000"}
	};

	return masks;
}


//
//	BlueprintLua::SetupUEVRRegistry
//

void BlueprintLua::SetupUEVRRegistry(BlueprintEditor& editor) {
	auto& registry = editor.GetRegistry();
	registry.Clear();

	registry.AddEnum("EXInputButton", {
		"A", "B", "X", "Y",
		"Left Bumper", "Right Bumper",
		"Start", "Back",
		"Left Thumb", "Right Thumb",
		"DPad Up", "DPad Down", "DPad Left", "DPad Right"});

	// the pseudo class providing UEVR's script entry points (all 18 uevr.sdk.callbacks
	// registrations; verified against lua-api/lib/src/ScriptContext.cpp's add_callback list)
	auto& uevr = registry.AddClass("UEVR", "", "UEVR script entry points");
	uevr.AddEvent("Pre Engine Tick").Out("Engine", PinType(PinKind::Object, "UEngine")).Out("Delta Seconds", PinType(PinKind::Float)).Metadata("uevr.sdk.callbacks.on_pre_engine_tick").Tooltip("Called before the engine ticks each frame");
	uevr.AddEvent("Post Engine Tick").Out("Engine", PinType(PinKind::Object, "UEngine")).Out("Delta Seconds", PinType(PinKind::Float)).Metadata("uevr.sdk.callbacks.on_post_engine_tick").Tooltip("Called after the engine ticks each frame");
	uevr.AddEvent("Pre Slate Draw Window").Out("Renderer", PinType(PinKind::Object, "UObject")).Out("Viewport Info", PinType(PinKind::Object, "UObject")).Metadata("uevr.sdk.callbacks.on_pre_slate_draw_window_render_thread").Tooltip("Called before slate draws the window, on the render thread");
	uevr.AddEvent("Post Slate Draw Window").Out("Renderer", PinType(PinKind::Object, "UObject")).Out("Viewport Info", PinType(PinKind::Object, "UObject")).Metadata("uevr.sdk.callbacks.on_post_slate_draw_window_render_thread").Tooltip("Called after slate draws the window, on the render thread");
	uevr.AddEvent("XInput Get State").Out("Retval", PinType(PinKind::Integer)).Out("User Index", PinType(PinKind::Integer)).Out("State", PinType(PinKind::Struct, "XINPUT_STATE")).Metadata("uevr.sdk.callbacks.on_xinput_get_state").Tooltip("Called when the game polls the gamepad; the state can be inspected or modified");
	uevr.AddEvent("XInput Set State").Out("Retval", PinType(PinKind::Integer)).Out("User Index", PinType(PinKind::Integer)).Out("Vibration", PinType(PinKind::Struct, "XINPUT_VIBRATION")).Metadata("uevr.sdk.callbacks.on_xinput_set_state").Tooltip("Called when the game sets gamepad vibration; the vibration can be inspected or modified");
	uevr.AddEvent("Draw UI").Metadata("uevr.sdk.callbacks.on_draw_ui").Tooltip("Called when the UEVR overlay UI is drawn");
	uevr.AddEvent("Script Reset").Metadata("uevr.sdk.callbacks.on_script_reset").Tooltip("Called when the script is reset or unloaded");
	uevr.AddEvent("Frame").Metadata("uevr.sdk.callbacks.on_frame").Tooltip("Called every frame");
	uevr.AddEvent("Pawn Changed").Metadata("uevr.sdk.callbacks.on_pawn_changed").Tooltip("Called when the local player's pawn changes");
	uevr.AddEvent("View Target Changed").Metadata("uevr.sdk.callbacks.on_view_target_changed").Tooltip("Called when the active camera view target changes");
	uevr.AddEvent("Level Changed").Metadata("uevr.sdk.callbacks.on_level_changed").Tooltip("Called when the game's level changes");
	uevr.AddEvent("Lua Event").Out("Event Name", PinType(PinKind::String)).Out("Event Data", PinType(PinKind::String)).Metadata("uevr.sdk.callbacks.on_lua_event").Tooltip("Called when any script dispatches a custom event (see Dispatch Custom Event)");
	uevr.AddEvent("Early Calculate Stereo View Offset").Out("Device", PinType(PinKind::Object, "UObject")).Out("View Index", PinType(PinKind::Integer)).Out("World To Meters", PinType(PinKind::Float)).Out("Position", PinType(PinKind::Vector)).Out("Rotation", PinType(PinKind::Rotator)).Out("Is Double", PinType(PinKind::Boolean)).Metadata("uevr.sdk.callbacks.on_early_calculate_stereo_view_offset").Tooltip("Called early in stereo view offset calculation; position/rotation can be modified");
	uevr.AddEvent("Pre Calculate Stereo View Offset").Out("Device", PinType(PinKind::Object, "UObject")).Out("View Index", PinType(PinKind::Integer)).Out("World To Meters", PinType(PinKind::Float)).Out("Position", PinType(PinKind::Vector)).Out("Rotation", PinType(PinKind::Rotator)).Out("Is Double", PinType(PinKind::Boolean)).Metadata("uevr.sdk.callbacks.on_pre_calculate_stereo_view_offset").Tooltip("Called before stereo view offset calculation; position/rotation can be modified");
	uevr.AddEvent("Post Calculate Stereo View Offset").Out("Device", PinType(PinKind::Object, "UObject")).Out("View Index", PinType(PinKind::Integer)).Out("World To Meters", PinType(PinKind::Float)).Out("Position", PinType(PinKind::Vector)).Out("Rotation", PinType(PinKind::Rotator)).Out("Is Double", PinType(PinKind::Boolean)).Metadata("uevr.sdk.callbacks.on_post_calculate_stereo_view_offset").Tooltip("Called after stereo view offset calculation; position/rotation can be modified");
	uevr.AddEvent("Pre Viewport Client Draw").Out("Viewport Client", PinType(PinKind::Object, "UObject")).Out("Viewport", PinType(PinKind::Object, "UObject")).Out("Canvas", PinType(PinKind::Object, "UObject")).Metadata("uevr.sdk.callbacks.on_pre_viewport_client_draw").Tooltip("Called before the game viewport draws");
	uevr.AddEvent("Post Viewport Client Draw").Out("Viewport Client", PinType(PinKind::Object, "UObject")).Out("Viewport", PinType(PinKind::Object, "UObject")).Out("Canvas", PinType(PinKind::Object, "UObject")).Metadata("uevr.sdk.callbacks.on_post_viewport_client_draw").Tooltip("Called after the game viewport draws");

	// UObject access (UEVR exposes properties and functions through metatables); the
	// reflection helpers below are sourced from APIUE.lua's ergonomic wrappers, which is
	// what real scripts call, rather than the raw ScriptContext.cpp sol2 usertype names
	auto& object = registry.AddClass("UObject", "", "An Unreal object accessed through UEVR's reflection bindings");
	object.AddFunction("Get Full Name", "UObject").Pure().Ret(PinType(PinKind::String)).Metadata("{target}:get_full_name()");
	object.AddFunction("Get FName", "UObject").Pure().Ret(PinType(PinKind::Name)).Metadata("{target}:get_fname():to_string()");
	object.AddFunction("Get Class", "UObject").Pure().Ret(PinType(PinKind::Object, "UObject")).Metadata("{target}:get_class()");
	object.AddFunction("Get Outer", "UObject").Pure().Keywords("owner package").Ret(PinType(PinKind::Object, "UObject")).Metadata("{target}:get_outer()");
	object.AddFunction("As Class", "UObject").Pure().Keywords("cast").Ret(PinType(PinKind::Object, "UClass")).Metadata("{target}:as_class()");
	object.AddFunction("As Struct", "UObject").Pure().Keywords("cast").Ret(PinType(PinKind::Object, "UStruct")).Metadata("{target}:as_struct()");
	object.AddFunction("As Function", "UObject").Pure().Keywords("cast").Ret(PinType(PinKind::Object, "UFunction")).Metadata("{target}:as_function()");
	object.AddFunction("Is A", "UObject").Pure().In("Class", PinType(PinKind::Object, "UObject")).Ret(PinType(PinKind::Boolean)).Metadata("{target}:is_a({0})");
	object.AddFunction("Is Valid", "UObject").Pure().Keywords("null nil check").Ret(PinType(PinKind::Boolean)).Metadata("({target} ~= nil)");
	object.AddFunction("Get Property", "UObject").Pure().Keywords("read field").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Wildcard)).Metadata("{target}[{0}]");
	object.AddFunction("Set Property", "UObject").Keywords("write field").In("Name", PinType(PinKind::String)).In("Value", PinType(PinKind::Wildcard)).Metadata("{target}[{0}] = {1}");
	object.AddFunction("Get Bool Property", "UObject").Pure().Keywords("read field typed").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Boolean)).Metadata("{target}:get_bool_property({0})");
	object.AddFunction("Get Float Property", "UObject").Pure().Keywords("read field typed").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Float)).Metadata("{target}:get_float_property({0})");
	object.AddFunction("Get Int Property", "UObject").Pure().Keywords("read field typed").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Integer)).Metadata("{target}:get_int_property({0})");
	object.AddFunction("Get UInt Property", "UObject").Pure().Keywords("read field typed").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Integer)).Metadata("{target}:get_uint_property({0})");
	object.AddFunction("Get FName Property", "UObject").Pure().Keywords("read field typed").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Name)).Metadata("{target}:get_fname_property({0}):to_string()");
	object.AddFunction("Get UObject Property", "UObject").Pure().Keywords("read field typed").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Object, "UObject")).Metadata("{target}:get_uobject_property({0})");
	object.AddFunction("Get Properties", "UObject").Pure().Keywords("reflection fields inspect list").Ret(PinType(PinKind::Wildcard, "", true)).Metadata("{target}:get_property_info()").Tooltip("Returns an array of {name, type, flags, offset, Value} records");
	object.AddFunction("Find Property", "UObject").Pure().Keywords("reflection field").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Object, "UObject")).Metadata("{target}:find_property({0})").Tooltip("Target should be a UClass/UStruct (see As Class / As Struct / Get Class)");
	object.AddFunction("Find Function", "UObject").Pure().Keywords("reflection method").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Object, "UObject")).Metadata("{target}:find_function({0})").Tooltip("Target should be a UClass/UStruct (see As Class / As Struct / Get Class)");
	object.AddFunction("Get Children", "UObject").Pure().Keywords("reflection fields").Ret(PinType(PinKind::Object, "UObject")).Metadata("{target}:get_children()").Tooltip("Target should be a UClass/UStruct (see As Class / As Struct / Get Class)");
	object.AddFunction("Call (No Args)", "UObject").Keywords("invoke function method").In("Function Name", PinType(PinKind::String), "Jump").Ret(PinType(PinKind::Wildcard)).Metadata("{target}[{0}]({target})");
	object.AddFunction("Call (1 Arg)", "UObject").Keywords("invoke function method").In("Function Name", PinType(PinKind::String), "SetActorScale3D").In("Arg", PinType(PinKind::Wildcard)).Ret(PinType(PinKind::Wildcard)).Metadata("{target}[{0}]({target}, {1})");

	auto& actor = registry.AddClass("AActor", "UObject", "An actor in the game world");
	actor.AddProperty("bHidden", PinType(PinKind::Boolean), "Actor");
	actor.AddProperty("bCanBeDamaged", PinType(PinKind::Boolean), "Actor");
	registry.AddClass("APawn", "AActor", "A possessable actor");
	registry.AddClass("UEngine", "UObject", "The engine singleton");
	auto& uclass = registry.AddClass("UClass", "UObject", "A class or struct's reflection metadata");
	auto& ustruct = registry.AddClass("UStruct", "UObject", "A class or struct's reflection metadata");
	auto& ufunction = registry.AddClass("UFunction", "UObject", "A reflected function");

	// the uevr.api object
	auto& api = registry.AddClass("UEVR_API", "", "The uevr.api object");
	api.AddFunction("Find UObject", "UEVR|API").Pure().Static().Keywords("search class object").In("Name", PinType(PinKind::String), "Class /Script/Engine.Pawn").Ret(PinType(PinKind::Object, "UObject")).Metadata("uevr.api:find_uobject({0})").Tooltip("Find an object by its full name");
	api.AddFunction("To UObject", "UEVR|API").Pure().Static().Keywords("cast address pointer").In("Address", PinType(PinKind::Integer)).Ret(PinType(PinKind::Object, "UObject")).Metadata("uevr.api:to_uobject({0})");
	api.AddFunction("Get Local Pawn", "UEVR|API").Pure().Static().Keywords("player character").In("Player Index", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Object, "APawn")).Metadata("uevr.api:get_local_pawn({0})");
	api.AddFunction("Get Player Controller", "UEVR|API").Pure().Static().In("Player Index", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Object, "UObject")).Metadata("uevr.api:get_player_controller({0})");
	api.AddFunction("Get Engine", "UEVR|API").Pure().Static().Ret(PinType(PinKind::Object, "UEngine")).Metadata("uevr.api:get_engine()");
	api.AddFunction("Spawn Object", "UEVR|API").Static().Keywords("instantiate create").In("Class", PinType(PinKind::Object, "UClass")).In("Outer", PinType(PinKind::Object, "UObject")).Ret(PinType(PinKind::Object, "UObject")).Metadata("uevr.api:spawn_object({0}, {1})");
	api.AddFunction("Add Component By Class", "UEVR|API").Static().Keywords("attach create").In("Actor", PinType(PinKind::Object, "AActor")).In("Class", PinType(PinKind::Object, "UClass")).In("Deferred", PinType(PinKind::Boolean), "false").Ret(PinType(PinKind::Object, "UObject")).Metadata("uevr.api:add_component_by_class({0}, {1}, {2})");
	api.AddFunction("Execute Command", "UEVR|API").Static().Keywords("console cvar").In("Command", PinType(PinKind::String)).Metadata("uevr.api:execute_command({0})").Tooltip("Execute a console command");
	api.AddFunction("Dispatch Custom Event", "UEVR|API").Static().Keywords("broadcast lua event").In("Event Name", PinType(PinKind::String)).In("Event Data", PinType(PinKind::String), "").Metadata("uevr.api:dispatch_custom_event({0}, {1})").Tooltip("Broadcasts a Lua Event to every script's \"Lua Event\" handler");
	api.AddFunction("Print", "UEVR|API").Static().Keywords("log output debug").In("Message", PinType(PinKind::String), "Hello from UEVR").Metadata("print({0})").Tooltip("Print to the UEVR log");
	api.AddFunction("Log Info", "UEVR|API").Static().Keywords("log output debug").In("Message", PinType(PinKind::String)).Metadata("uevr.params.functions.log_info({0})");
	api.AddFunction("Log Warn", "UEVR|API").Static().Keywords("log output debug warning").In("Message", PinType(PinKind::String)).Metadata("uevr.params.functions.log_warn({0})");
	api.AddFunction("Log Error", "UEVR|API").Static().Keywords("log output debug error").In("Message", PinType(PinKind::String)).Metadata("uevr.params.functions.log_error({0})");
	api.AddFunction("Is Drawing UI", "UEVR|API").Pure().Static().Keywords("menu overlay open input focus").Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.functions.is_drawing_ui()").Tooltip("True while UEVR's own overlay UI has input focus");

	// the uevr.params.vr runtime object (verified-safe subset: excludes the out-parameter
	// pose/transform/origin/rotation-offset/joystick-source family, whose exact Lua calling
	// convention -- pass a pre-allocated Vector/Quaternion object to be mutated in place --
	// isn't safely representable as a single-expression node template; use a Custom Lua
	// node for those, e.g. "local p = Vector3f.new(0,0,0) uevr.params.vr.get_pose(idx, p, rot)")
	auto& vr = registry.AddClass("UEVR_VRData", "", "The uevr.params.vr object");
	vr.AddFunction("Is Runtime Ready", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_runtime_ready()");
	vr.AddFunction("Is OpenVR", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_openvr()");
	vr.AddFunction("Is OpenXR", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_openxr()");
	vr.AddFunction("Is HMD Active", "UEVR|VR").Pure().Static().Keywords("headset").Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_hmd_active()");
	vr.AddFunction("Is Using Controllers", "UEVR|VR").Pure().Static().Keywords("motion").Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_using_controllers()");
	vr.AddFunction("Get HMD Index", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_hmd_index()");
	vr.AddFunction("Get Left Controller Index", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_left_controller_index()");
	vr.AddFunction("Get Right Controller Index", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_right_controller_index()");
	vr.AddFunction("Get HMD Width", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_hmd_width()");
	vr.AddFunction("Get HMD Height", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_hmd_height()");
	vr.AddFunction("Get UI Width", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_ui_width()");
	vr.AddFunction("Get UI Height", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_ui_height()");
	vr.AddFunction("Get Movement Orientation", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_movement_orientation()");
	vr.AddFunction("Get Aim Method", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_aim_method()");
	vr.AddFunction("Set Aim Method", "UEVR|VR").Static().In("Method", PinType(PinKind::Integer)).Metadata("uevr.params.vr.set_aim_method({0})");
	vr.AddFunction("Is Aim Allowed", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_aim_allowed()");
	vr.AddFunction("Set Aim Allowed", "UEVR|VR").Static().In("Allowed", PinType(PinKind::Boolean), "true").Metadata("uevr.params.vr.set_aim_allowed({0})");
	vr.AddFunction("Is Snap Turn Enabled", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_snap_turn_enabled()");
	vr.AddFunction("Set Snap Turn Enabled", "UEVR|VR").Static().In("Enabled", PinType(PinKind::Boolean), "true").Metadata("uevr.params.vr.set_snap_turn_enabled({0})");
	vr.AddFunction("Set Decoupled Pitch Enabled", "UEVR|VR").Static().In("Enabled", PinType(PinKind::Boolean), "true").Metadata("uevr.params.vr.set_decoupled_pitch_enabled({0})");
	vr.AddFunction("Get Action Handle", "UEVR|VR").Pure().Static().Keywords("input openxr openvr").In("Action Path", PinType(PinKind::String), "/actions/default/in/Trigger").Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_action_handle({0})");
	vr.AddFunction("Is Action Active", "UEVR|VR").Pure().Static().Keywords("input pressed").In("Action", PinType(PinKind::Integer)).In("Source", PinType(PinKind::Integer)).Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_action_active({0}, {1})");
	vr.AddFunction("Trigger Haptic Vibration", "UEVR|VR").Static().Keywords("rumble feedback controller").In("Seconds From Now", PinType(PinKind::Float), "0.0").In("Duration", PinType(PinKind::Float), "0.1").In("Frequency", PinType(PinKind::Float), "1.0").In("Amplitude", PinType(PinKind::Float), "300.0").In("Source", PinType(PinKind::Integer), "1").Metadata("uevr.params.vr.trigger_haptic_vibration({0}, {1}, {2}, {3}, {4})");
	vr.AddFunction("Get Mod Value", "UEVR|VR").Pure().Static().Keywords("setting option").In("Name", PinType(PinKind::String), "VR_RoomscaleMovement").Ret(PinType(PinKind::String)).Metadata("uevr.params.vr.get_mod_value({0})");
	vr.AddFunction("Set Mod Value", "UEVR|VR").Static().Keywords("setting option").In("Name", PinType(PinKind::String)).In("Value", PinType(PinKind::String)).Metadata("uevr.params.vr.set_mod_value({0}, {1})");
	vr.AddFunction("Recenter View", "UEVR|VR").Static().Keywords("reset origin").Metadata("uevr.params.vr.recenter_view()");
	vr.AddFunction("Recenter Horizon", "UEVR|VR").Static().Keywords("reset level").Metadata("uevr.params.vr.recenter_horizon()");
	vr.AddFunction("Save Config", "UEVR|VR").Static().Metadata("uevr.params.vr.save_config()");
	vr.AddFunction("Reload Config", "UEVR|VR").Static().Metadata("uevr.params.vr.reload_config()");

	// helpers to inspect the XINPUT_STATE passed to the XInput Get State event
	auto& xinput = registry.AddClass("XInput", "", "XINPUT_STATE helpers");
	xinput.AddFunction("Is Button Pressed", "UEVR|XInput").Pure().Static().Keywords("gamepad controller").In("State", PinType(PinKind::Struct, "XINPUT_STATE")).In("Button", PinType(PinKind::Enum, "EXInputButton"), "A").Ret(PinType(PinKind::Boolean)).Metadata("(({0}.Gamepad.wButtons & {1}) ~= 0)");
	xinput.AddFunction("Get Buttons", "UEVR|XInput").Pure().Static().In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Ret(PinType(PinKind::Integer)).Metadata("{0}.Gamepad.wButtons");
	xinput.AddFunction("Get Left Trigger", "UEVR|XInput").Pure().Static().In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Ret(PinType(PinKind::Integer)).Metadata("{0}.Gamepad.bLeftTrigger");
	xinput.AddFunction("Get Right Trigger", "UEVR|XInput").Pure().Static().In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Ret(PinType(PinKind::Integer)).Metadata("{0}.Gamepad.bRightTrigger");
	xinput.AddFunction("Get Left Thumb", "UEVR|XInput").Pure().Static().Keywords("stick axis").In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Out("X", PinType(PinKind::Integer)).Out("Y", PinType(PinKind::Integer)).Metadata("{0}.Gamepad.sThumbLX|{0}.Gamepad.sThumbLY");
	xinput.AddFunction("Get Right Thumb", "UEVR|XInput").Pure().Static().Keywords("stick axis").In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Out("X", PinType(PinKind::Integer)).Out("Y", PinType(PinKind::Integer)).Metadata("{0}.Gamepad.sThumbRX|{0}.Gamepad.sThumbRY");

	// ImGui widgets: draw calls are impure (exec-pinned, order matters); queries are pure.
	// Interactive widgets in the real binding return (changed, new_value) via
	// sol::variadic_results -- "(select(2, imgui.x(...)))" truncates to just the new value
	// in a single expression, which is what the impure-node single-local-capture codegen
	// path needs (see Generator::emitNode's CallFunction case in this file). Verified
	// against the real signatures in src/mods/bindings/ImGui.cpp; trailing sol::object
	// (flags/size/condition) parameters are passed literal nil rather than exposed as
	// pins, keeping this set to the common case -- anything else is a Custom Lua node away.
	auto& imgui = registry.AddClass("ImGui", "", "The imgui table (UEVR's Dear ImGui Lua bindings)");
	imgui.AddFunction("Begin Window", "ImGui|Window").Keywords("panel").In("Name", PinType(PinKind::String), "Window").Ret(PinType(PinKind::Boolean)).Metadata("imgui.begin_window({0}, nil, nil)").Tooltip("Returns false while the window is collapsed/closed -- guard the body with a Branch");
	imgui.AddFunction("End Window", "ImGui|Window").Metadata("imgui.end_window()").Tooltip("Always call once per Begin Window, regardless of its return value");
	imgui.AddFunction("Begin Child Window", "ImGui|Window").Keywords("panel scroll region").In("Name", PinType(PinKind::String), "Child").Ret(PinType(PinKind::Boolean)).Metadata("imgui.begin_child_window({0}, nil, nil, nil)");
	imgui.AddFunction("End Child Window", "ImGui|Window").Metadata("imgui.end_child_window()");
	imgui.AddFunction("Set Next Window Size", "ImGui|Window").In("Width", PinType(PinKind::Float), "300.0").In("Height", PinType(PinKind::Float), "200.0").Metadata("imgui.set_next_window_size(Vector2f.new({0}, {1}), nil)").Tooltip("Call before Begin Window");
	imgui.AddFunction("Text", "ImGui|Text").Keywords("label print").In("Text", PinType(PinKind::String), "Text").Metadata("imgui.text({0}, nil)");
	imgui.AddFunction("Text Wrapped", "ImGui|Text").In("Text", PinType(PinKind::String), "Text").Metadata("imgui.text_wrapped({0})");
	imgui.AddFunction("Text Disabled", "ImGui|Text").In("Text", PinType(PinKind::String), "Text").Metadata("imgui.text_disabled({0})");
	imgui.AddFunction("Bullet Text", "ImGui|Text").In("Text", PinType(PinKind::String), "Text").Metadata("imgui.bullet_text({0})");
	imgui.AddFunction("Button", "ImGui|Input").Keywords("click").In("Label", PinType(PinKind::String), "Button").Ret(PinType(PinKind::Boolean)).Metadata("imgui.button({0}, nil, nil)");
	imgui.AddFunction("Small Button", "ImGui|Input").Keywords("click").In("Label", PinType(PinKind::String), "Button").Ret(PinType(PinKind::Boolean)).Metadata("imgui.small_button({0})");
	imgui.AddFunction("Arrow Button", "ImGui|Input").Keywords("click direction").In("Str Id", PinType(PinKind::String), "arrow").In("Dir", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Boolean)).Metadata("imgui.arrow_button({0}, {1})");
	imgui.AddFunction("Checkbox", "ImGui|Input").Keywords("toggle bool").In("Label", PinType(PinKind::String), "Checkbox").In("Value", PinType(PinKind::Boolean), "false").Ret(PinType(PinKind::Boolean)).Metadata("(select(2, imgui.checkbox({0}, {1})))");
	imgui.AddFunction("Radio Button", "ImGui|Input").Keywords("select option").In("Label", PinType(PinKind::String), "Option").In("Active", PinType(PinKind::Boolean), "false").Ret(PinType(PinKind::Boolean)).Metadata("imgui.radio_button({0}, {1})").Tooltip("Returns true when clicked this frame; drive Active from your own selection state");
	imgui.AddFunction("Slider Float", "ImGui|Input").Keywords("range value").In("Label", PinType(PinKind::String), "Slider").In("Value", PinType(PinKind::Float), "0.0").In("Min", PinType(PinKind::Float), "0.0").In("Max", PinType(PinKind::Float), "1.0").Ret(PinType(PinKind::Float)).Metadata("(select(2, imgui.slider_float({0}, {1}, {2}, {3}, \"%.3f\", nil)))");
	imgui.AddFunction("Slider Int", "ImGui|Input").Keywords("range value").In("Label", PinType(PinKind::String), "Slider").In("Value", PinType(PinKind::Integer), "0").In("Min", PinType(PinKind::Integer), "0").In("Max", PinType(PinKind::Integer), "100").Ret(PinType(PinKind::Integer)).Metadata("(select(2, imgui.slider_int({0}, {1}, {2}, {3}, \"%d\", nil)))");
	imgui.AddFunction("Drag Float", "ImGui|Input").Keywords("range value").In("Label", PinType(PinKind::String), "Drag").In("Value", PinType(PinKind::Float), "0.0").In("Speed", PinType(PinKind::Float), "1.0").In("Min", PinType(PinKind::Float), "0.0").In("Max", PinType(PinKind::Float), "0.0").Ret(PinType(PinKind::Float)).Metadata("(select(2, imgui.drag_float({0}, {1}, {2}, {3}, {4}, \"%.3f\", nil)))");
	imgui.AddFunction("Drag Int", "ImGui|Input").Keywords("range value").In("Label", PinType(PinKind::String), "Drag").In("Value", PinType(PinKind::Integer), "0").In("Speed", PinType(PinKind::Float), "1.0").In("Min", PinType(PinKind::Integer), "0").In("Max", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Integer)).Metadata("(select(2, imgui.drag_int({0}, {1}, {2}, {3}, {4}, \"%d\", nil)))");
	imgui.AddFunction("Input Text", "ImGui|Input").Keywords("string field").In("Label", PinType(PinKind::String), "Input").In("Value", PinType(PinKind::String), "").Ret(PinType(PinKind::String)).Metadata("(select(2, imgui.input_text({0}, {1}, 0)))");
	imgui.AddFunction("Color Edit3", "ImGui|Input").Keywords("rgb picker").In("Label", PinType(PinKind::String), "Color").In("Color", PinType(PinKind::Vector), "1,1,1").Ret(PinType(PinKind::Vector)).Metadata("(select(2, imgui.color_edit3({0}, {1}, nil)))");
	imgui.AddFunction("Same Line", "ImGui|Layout").Metadata("imgui.same_line()");
	imgui.AddFunction("Separator", "ImGui|Layout").Metadata("imgui.separator()");
	imgui.AddFunction("Spacing", "ImGui|Layout").Metadata("imgui.spacing()");
	imgui.AddFunction("Indent", "ImGui|Layout").In("Width", PinType(PinKind::Integer), "0").Metadata("imgui.indent({0})");
	imgui.AddFunction("Unindent", "ImGui|Layout").In("Width", PinType(PinKind::Integer), "0").Metadata("imgui.unindent({0})");
	imgui.AddFunction("Begin Group", "ImGui|Layout").Metadata("imgui.begin_group()");
	imgui.AddFunction("End Group", "ImGui|Layout").Metadata("imgui.end_group()");
	imgui.AddFunction("Begin Table", "ImGui|Layout").Keywords("columns grid").In("Str Id", PinType(PinKind::String), "table").In("Columns", PinType(PinKind::Integer), "2").Ret(PinType(PinKind::Boolean)).Metadata("imgui.begin_table({0}, {1}, nil, nil, nil)");
	imgui.AddFunction("End Table", "ImGui|Layout").Metadata("imgui.end_table()");
	imgui.AddFunction("Table Next Row", "ImGui|Layout").Metadata("imgui.table_next_row(nil, nil)");
	imgui.AddFunction("Table Next Column", "ImGui|Layout").Ret(PinType(PinKind::Boolean)).Metadata("imgui.table_next_column()");
	imgui.AddFunction("Tree Node", "ImGui|Tree").Keywords("collapse expand").In("Label", PinType(PinKind::String), "Node").Ret(PinType(PinKind::Boolean)).Metadata("imgui.tree_node({0}, nil)").Tooltip("If true, call Tree Pop after the children");
	imgui.AddFunction("Tree Pop", "ImGui|Tree").Metadata("imgui.tree_pop()");
	imgui.AddFunction("Collapsing Header", "ImGui|Tree").Keywords("section").In("Name", PinType(PinKind::String), "Section").Ret(PinType(PinKind::Boolean)).Metadata("imgui.collapsing_header({0})");
	imgui.AddFunction("Open Popup", "ImGui|Popup").In("Str Id", PinType(PinKind::String), "popup").Metadata("imgui.open_popup({0}, nil)");
	imgui.AddFunction("Begin Popup", "ImGui|Popup").In("Str Id", PinType(PinKind::String), "popup").Ret(PinType(PinKind::Boolean)).Metadata("imgui.begin_popup({0}, nil)").Tooltip("If true, call End Window after the contents (popups share End Window)");
	imgui.AddFunction("Begin Popup Modal", "ImGui|Popup").In("Str Id", PinType(PinKind::String), "popup").Ret(PinType(PinKind::Boolean)).Metadata("imgui.begin_popup_modal({0}, nil, nil)").Tooltip("If true, call End Window after the contents (popups share End Window)");
	imgui.AddFunction("Close Current Popup", "ImGui|Popup").Metadata("imgui.close_current_popup()");
	imgui.AddFunction("Progress Bar", "ImGui|Misc").In("Progress", PinType(PinKind::Float), "0.0").In("Overlay", PinType(PinKind::String), "").Metadata("imgui.progress_bar({0}, nil, {1})");
	imgui.AddFunction("Is Item Hovered", "ImGui|Query").Pure().Ret(PinType(PinKind::Boolean)).Metadata("imgui.is_item_hovered(nil)").Tooltip("Refers to the most recently submitted widget");
	imgui.AddFunction("Is Item Clicked", "ImGui|Query").Pure().Ret(PinType(PinKind::Boolean)).Metadata("imgui.is_item_clicked()").Tooltip("Refers to the most recently submitted widget");
	// escape hatch: reaches every imgui.* binding by name (see BlueprintImGuiNames.h),
	// mirroring the existing UObject "Call (N Args)" generic-call pattern
	imgui.AddFunction("Call (No Args)", "ImGui|Call").Keywords("escape hatch raw").In("Function Name", PinType(PinKind::Enum, "ImGuiFunctionName"), "same_line").Ret(PinType(PinKind::Wildcard)).Metadata("imgui[{0}]()");
	imgui.AddFunction("Call (1 Arg)", "ImGui|Call").Keywords("escape hatch raw").In("Function Name", PinType(PinKind::Enum, "ImGuiFunctionName"), "text").In("Arg 1", PinType(PinKind::Wildcard)).Ret(PinType(PinKind::Wildcard)).Metadata("imgui[{0}]({1})");
	imgui.AddFunction("Call (2 Args)", "ImGui|Call").Keywords("escape hatch raw").In("Function Name", PinType(PinKind::Enum, "ImGuiFunctionName"), "button").In("Arg 1", PinType(PinKind::Wildcard)).In("Arg 2", PinType(PinKind::Wildcard)).Ret(PinType(PinKind::Wildcard)).Metadata("imgui[{0}]({1}, {2})");
	imgui.AddFunction("Call (3 Args)", "ImGui|Call").Keywords("escape hatch raw").In("Function Name", PinType(PinKind::Enum, "ImGuiFunctionName"), "checkbox").In("Arg 1", PinType(PinKind::Wildcard)).In("Arg 2", PinType(PinKind::Wildcard)).In("Arg 3", PinType(PinKind::Wildcard)).Ret(PinType(PinKind::Wildcard)).Metadata("imgui[{0}]({1}, {2}, {3})");
	imgui.AddFunction("Call (4 Args)", "ImGui|Call").Keywords("escape hatch raw").In("Function Name", PinType(PinKind::Enum, "ImGuiFunctionName"), "slider_float").In("Arg 1", PinType(PinKind::Wildcard)).In("Arg 2", PinType(PinKind::Wildcard)).In("Arg 3", PinType(PinKind::Wildcard)).In("Arg 4", PinType(PinKind::Wildcard)).Ret(PinType(PinKind::Wildcard)).Metadata("imgui[{0}]({1}, {2}, {3}, {4})");

	registry.AddEnum("ImGuiFunctionName", imguiFunctionNames());

	// Lua math, logic and string utilities
	auto& math = registry.AddClass("LuaMath", "", "Lua math utilities");
	math.AddFunction("Add (Float)", "Math|Float").Pure().Static().Keywords("+ plus").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float)).Metadata("({0} + {1})");
	math.AddFunction("Subtract (Float)", "Math|Float").Pure().Static().Keywords("- minus").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float)).Metadata("({0} - {1})");
	math.AddFunction("Multiply (Float)", "Math|Float").Pure().Static().Keywords("* times").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float)).Metadata("({0} * {1})");
	math.AddFunction("Divide (Float)", "Math|Float").Pure().Static().Keywords("/").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float), "1.0").Ret(PinType(PinKind::Float)).Metadata("({0} / {1})");
	math.AddFunction("Clamp (Float)", "Math|Float").Pure().Static().In("Value", PinType(PinKind::Float)).In("Min", PinType(PinKind::Float), "0.0").In("Max", PinType(PinKind::Float), "1.0").Ret(PinType(PinKind::Float)).Metadata("math.min(math.max({0}, {1}), {2})");
	math.AddFunction("Lerp (Float)", "Math|Float").Pure().Static().Keywords("interpolate blend").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).In("Alpha", PinType(PinKind::Float)).Ret(PinType(PinKind::Float)).Metadata("({0} + ({1} - {0}) * {2})");
	math.AddFunction("Abs (Float)", "Math|Float").Pure().Static().Keywords("absolute").In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Float)).Metadata("math.abs({0})");
	math.AddFunction("Floor", "Math|Float").Pure().Static().Keywords("round").In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Integer)).Metadata("math.floor({0})");
	math.AddFunction("Sin", "Math|Float").Pure().Static().In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Float)).Metadata("math.sin({0})");
	math.AddFunction("Cos", "Math|Float").Pure().Static().In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Float)).Metadata("math.cos({0})");
	math.AddFunction("Random Float", "Math|Random").Pure().Static().Ret(PinType(PinKind::Float)).Metadata("math.random()");
	math.AddFunction("Random Integer In Range", "Math|Random").Pure().Static().In("Min", PinType(PinKind::Integer), "1").In("Max", PinType(PinKind::Integer), "100").Ret(PinType(PinKind::Integer)).Metadata("math.random({0}, {1})");
	math.AddFunction("Greater (Float)", "Math|Comparison").Pure().Static().Keywords("> compare").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Boolean)).Metadata("({0} > {1})");
	math.AddFunction("Less (Float)", "Math|Comparison").Pure().Static().Keywords("< compare").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Boolean)).Metadata("({0} < {1})");
	math.AddFunction("Equal (Float)", "Math|Comparison").Pure().Static().Keywords("== compare").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Boolean)).Metadata("({0} == {1})");
	math.AddFunction("AND (Boolean)", "Math|Boolean").Pure().Static().Keywords("&& logic").In("A", PinType(PinKind::Boolean)).In("B", PinType(PinKind::Boolean)).Ret(PinType(PinKind::Boolean)).Metadata("({0} and {1})");
	math.AddFunction("OR (Boolean)", "Math|Boolean").Pure().Static().Keywords("|| logic").In("A", PinType(PinKind::Boolean)).In("B", PinType(PinKind::Boolean)).Ret(PinType(PinKind::Boolean)).Metadata("({0} or {1})");
	math.AddFunction("NOT (Boolean)", "Math|Boolean").Pure().Static().Keywords("! logic invert").In("A", PinType(PinKind::Boolean)).Ret(PinType(PinKind::Boolean)).Metadata("(not {0})");
	math.AddFunction("Make Vector", "Math|Vector").Pure().Static().In("X", PinType(PinKind::Float)).In("Y", PinType(PinKind::Float)).In("Z", PinType(PinKind::Float)).Ret(PinType(PinKind::Vector)).Metadata("Vector3f.new({0}, {1}, {2})");
	math.AddFunction("Break Vector", "Math|Vector").Pure().Static().In("In Vec", PinType(PinKind::Vector)).Out("X", PinType(PinKind::Float)).Out("Y", PinType(PinKind::Float)).Out("Z", PinType(PinKind::Float)).Metadata("{0}.x|{0}.y|{0}.z");

	auto& strings = registry.AddClass("LuaString", "", "Lua string utilities");
	strings.AddFunction("Append", "Utilities|String").Pure().Static().Keywords("concat combine ..").In("A", PinType(PinKind::String)).In("B", PinType(PinKind::String)).Ret(PinType(PinKind::String)).Metadata("({0} .. {1})");
	strings.AddFunction("To String", "Utilities|String").Pure().Static().Keywords("convert cast").In("Value", PinType(PinKind::Wildcard)).Ret(PinType(PinKind::String)).Metadata("tostring({0})");
	strings.AddFunction("To Number", "Utilities|String").Pure().Static().Keywords("convert cast parse").In("Value", PinType(PinKind::String)).Ret(PinType(PinKind::Float)).Metadata("tonumber({0})");
	strings.AddFunction("String Length", "Utilities|String").Pure().Static().In("Value", PinType(PinKind::String)).Ret(PinType(PinKind::Integer)).Metadata("#({0})");
	strings.AddFunction("Contains", "Utilities|String").Pure().Static().Keywords("find search").In("Search In", PinType(PinKind::String)).In("Substring", PinType(PinKind::String)).Ret(PinType(PinKind::Boolean)).Metadata("(string.find({0}, {1}, 1, true) ~= nil)");
	strings.AddFunction("To Upper", "Utilities|String").Pure().Static().In("Value", PinType(PinKind::String)).Ret(PinType(PinKind::String)).Metadata("string.upper({0})");
	strings.AddFunction("To Lower", "Utilities|String").Pure().Static().In("Value", PinType(PinKind::String)).Ret(PinType(PinKind::String)).Metadata("string.lower({0})");

	// -- Datatypes (Vector/Quaternion/Transform/Matrix/StructObject) --
	auto& vector3f = registry.AddClass("Vector3f", "", "UEVR glm vector math (Vector3f/2f/4f). Complements LuaMath Make/Break Vector.");
	auto& quaternionf = registry.AddClass("Quaternionf", "", "UEVR quaternion rotations - from_euler, slerp, rotate, to_mat4, identity.");
	auto& transformf = registry.AddClass("Transformf", "", "UEVR transform - translation/rotation/scale, inverse, to/from matrix.");
	auto& matrix4x4f = registry.AddClass("Matrix4x4f", "", "UEVR 4x4 matrix - transform point/vector, inverse, decompose, identity.");
	auto& structObject = registry.AddClass("StructObject", "", "UEVR StructObject - construct a UStruct, get/set properties, typed read/write.");
	vector3f.AddFunction("Dot (Vector3f)", "Math|Vector3").Pure().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:dot({1})");
	vector3f.AddFunction("Cross (Vector3f)", "Math|Vector3").Pure().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:cross({1})");
	vector3f.AddFunction("Length (Vector3f)", "Math|Vector3").Pure().In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:length()");
	vector3f.AddFunction("Length Squared (Vector3f)", "Math|Vector3").Pure().In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:length_squared()");
	vector3f.AddFunction("Distance (Vector3f)", "Math|Vector3").Pure().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:distance({1})");
	vector3f.AddFunction("Normalize (Vector3f)", "Math|Vector3").Pure().In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:normalized()");
	vector3f.AddFunction("Reflect (Vector3f)", "Math|Vector3").Pure().In("V", PinType(PinKind::Vector)).In("Normal", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:reflect({1})");
	vector3f.AddFunction("Refract (Vector3f)", "Math|Vector3").Pure().In("V", PinType(PinKind::Vector)).In("Normal", PinType(PinKind::Vector)).In("Eta", PinType(PinKind::Float), "1.0").Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:refract({1}, {2})");
	vector3f.AddFunction("Lerp (Vector3f)", "Math|Vector3").Pure().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).In("Alpha", PinType(PinKind::Float), "0.5").Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:lerp({1}, {2})");
	vector3f.AddFunction("Add (Vector3f)", "Math|Vector3").Pure().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("({0} + {1})");
	vector3f.AddFunction("Subtract (Vector3f)", "Math|Vector3").Pure().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("({0} - {1})");
	vector3f.AddFunction("Scale (Vector3f)", "Math|Vector3").Pure().In("Vec", PinType(PinKind::Vector)).In("Scalar", PinType(PinKind::Float), "1.0").Out("Retval", PinType(PinKind::Vector)).Metadata("({0} * {1})");
	vector3f.AddFunction("Negate (Vector3f)", "Math|Vector3").Pure().In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("(-{0})");
	vector3f.AddFunction("Make Vector2f", "Math|Vector2").Pure().Static().In("X", PinType(PinKind::Float), "0.0").In("Y", PinType(PinKind::Float), "0.0").Out("Retval", PinType(PinKind::Struct, "Vector2f")).Metadata("Vector2f.new({0}, {1})");
	vector3f.AddFunction("Break Vector2f", "Math|Vector2").Pure().In("In Vec", PinType(PinKind::Struct, "Vector2f")).Out("X", PinType(PinKind::Float)).Out("Y", PinType(PinKind::Float)).Metadata("{0}.x|{0}.y");
	vector3f.AddFunction("Length (Vector2f)", "Math|Vector2").Pure().In("Vec", PinType(PinKind::Struct, "Vector2f")).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:length()");
	vector3f.AddFunction("Normalize (Vector2f)", "Math|Vector2").Pure().In("Vec", PinType(PinKind::Struct, "Vector2f")).Out("Retval", PinType(PinKind::Struct, "Vector2f")).Metadata("{0}:normalized()");
	vector3f.AddFunction("Make Vector4f", "Math|Vector4").Pure().Static().In("X", PinType(PinKind::Float), "0.0").In("Y", PinType(PinKind::Float), "0.0").In("Z", PinType(PinKind::Float), "0.0").In("W", PinType(PinKind::Float), "0.0").Out("Retval", PinType(PinKind::Struct, "Vector4f")).Metadata("Vector4f.new({0}, {1}, {2}, {3})");
	vector3f.AddFunction("Break Vector4f", "Math|Vector4").Pure().In("In Vec", PinType(PinKind::Struct, "Vector4f")).Out("X", PinType(PinKind::Float)).Out("Y", PinType(PinKind::Float)).Out("Z", PinType(PinKind::Float)).Out("W", PinType(PinKind::Float)).Metadata("{0}.x|{0}.y|{0}.z|{0}.w");
	vector3f.AddFunction("Dot (Vector4f)", "Math|Vector4").Pure().In("A", PinType(PinKind::Struct, "Vector4f")).In("B", PinType(PinKind::Struct, "Vector4f")).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:dot({1})");
	vector3f.AddFunction("Normalize (Vector4f)", "Math|Vector4").Pure().In("Vec", PinType(PinKind::Struct, "Vector4f")).Out("Retval", PinType(PinKind::Struct, "Vector4f")).Metadata("{0}:normalized()");
	quaternionf.AddFunction("Make Quaternion", "Math|Quaternion").Pure().Static().In("X", PinType(PinKind::Float), "0.0").In("Y", PinType(PinKind::Float), "0.0").In("Z", PinType(PinKind::Float), "0.0").In("W", PinType(PinKind::Float), "1.0").Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("Quaternionf.new({0}, {1}, {2}, {3})");
	quaternionf.AddFunction("Quaternion From Euler", "Math|Quaternion").Pure().Static().In("Rotation", PinType(PinKind::Vector), "0,0,0").Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("Quaternionf.from_euler({0})");
	quaternionf.AddFunction("Quaternion Identity", "Math|Quaternion").Pure().Static().Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("Quaternionf.identity()");
	quaternionf.AddFunction("Slerp (Quaternion)", "Math|Quaternion").Pure().In("A", PinType(PinKind::Struct, "Quaternionf")).In("B", PinType(PinKind::Struct, "Quaternionf")).In("Alpha", PinType(PinKind::Float), "0.5").Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("{0}:slerp({1}, {2})");
	quaternionf.AddFunction("Rotate Vector (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:rotate({1})");
	quaternionf.AddFunction("Unrotate Vector (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:unrotate({1})");
	quaternionf.AddFunction("Multiply (Quaternion)", "Math|Quaternion").Pure().In("A", PinType(PinKind::Struct, "Quaternionf")).In("B", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("({0} * {1})");
	quaternionf.AddFunction("Inverse (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("{0}:inverse()");
	quaternionf.AddFunction("Normalize (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("{0}:normalized()");
	quaternionf.AddFunction("Quaternion To Matrix", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Struct, "Matrix4x4f")).Metadata("{0}:to_mat4()");
	quaternionf.AddFunction("Quaternion To Euler", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Rotation", PinType(PinKind::Vector)).Metadata("{0}:rotator()");
	quaternionf.AddFunction("Dot (Quaternion)", "Math|Quaternion").Pure().In("A", PinType(PinKind::Struct, "Quaternionf")).In("B", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:dot({1})");
	quaternionf.AddFunction("Conjugate (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("{0}:conjugate()");
	quaternionf.AddFunction("Get Forward Axis (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:x_axis()");
	quaternionf.AddFunction("Get Right Axis (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:y_axis()");
	quaternionf.AddFunction("Get Up Axis (Quaternion)", "Math|Quaternion").Pure().In("Quat", PinType(PinKind::Struct, "Quaternionf")).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:z_axis()");
	transformf.AddFunction("Make Transform", "Math|Transform").Pure().Static().In("Translation", PinType(PinKind::Vector), "0,0,0").In("Rotation", PinType(PinKind::Struct, "Quaternionf")).In("Scale", PinType(PinKind::Vector), "1,1,1").Out("Retval", PinType(PinKind::Struct, "Transformf")).Metadata("Transformf.new({0}, {1}, {2})");
	transformf.AddFunction("Break Transform", "Math|Transform").Pure().In("In Transform", PinType(PinKind::Struct, "Transformf")).Out("Translation", PinType(PinKind::Vector)).Out("Rotation", PinType(PinKind::Struct, "Quaternionf")).Out("Scale", PinType(PinKind::Vector)).Metadata("{0}.translation|{0}.rotation|{0}.scale3d");
	transformf.AddFunction("Inverse Transform", "Math|Transform").Pure().In("In Transform", PinType(PinKind::Struct, "Transformf")).Out("Retval", PinType(PinKind::Struct, "Transformf")).Metadata("{0}:inverse()");
	transformf.AddFunction("Transform To Matrix", "Math|Transform").Pure().In("In Transform", PinType(PinKind::Struct, "Transformf")).Out("Retval", PinType(PinKind::Struct, "Matrix4x4f")).Metadata("{0}:to_matrix()");
	transformf.AddFunction("Relative Transform", "Math|Transform").Pure().In("A", PinType(PinKind::Struct, "Transformf")).In("B", PinType(PinKind::Struct, "Transformf")).Out("Retval", PinType(PinKind::Struct, "Transformf")).Metadata("{0}:relative({1})");
	transformf.AddFunction("Transform From Matrix", "Math|Transform").Pure().Static().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).Out("Retval", PinType(PinKind::Struct, "Transformf")).Metadata("Transformf.from_matrix({0})");
	matrix4x4f.AddFunction("Matrix Identity", "Math|Matrix").Pure().Static().Out("Retval", PinType(PinKind::Struct, "Matrix4x4f")).Metadata("Quaternionf.identity():to_mat4()");
	matrix4x4f.AddFunction("Make Matrix (From Rows)", "Math|Matrix").Pure().Static().In("Row 0", PinType(PinKind::Struct, "Vector4f")).In("Row 1", PinType(PinKind::Struct, "Vector4f")).In("Row 2", PinType(PinKind::Struct, "Vector4f")).In("Row 3", PinType(PinKind::Struct, "Vector4f")).Out("Retval", PinType(PinKind::Struct, "Matrix4x4f")).Metadata("Matrix4x4f.new({0}, {1}, {2}, {3})");
	matrix4x4f.AddFunction("Transform Point (Matrix)", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).In("Point", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:transform_point({1})");
	matrix4x4f.AddFunction("Transform Vector (Matrix)", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Vector)).Metadata("{0}:transform_vector({1})");
	matrix4x4f.AddFunction("Transform Vector4 (Matrix)", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).In("Vec", PinType(PinKind::Vector)).Out("Retval", PinType(PinKind::Struct, "Vector4f")).Metadata("{0}:transform_vector4({1})");
	matrix4x4f.AddFunction("Inverse Matrix", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).Out("Retval", PinType(PinKind::Struct, "Matrix4x4f")).Metadata("{0}:inverse()");
	matrix4x4f.AddFunction("Multiply (Matrix)", "Math|Matrix").Pure().In("A", PinType(PinKind::Struct, "Matrix4x4f")).In("B", PinType(PinKind::Struct, "Matrix4x4f")).Out("Retval", PinType(PinKind::Struct, "Matrix4x4f")).Metadata("({0} * {1})");
	matrix4x4f.AddFunction("Matrix Decompose", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).Out("Retval", PinType(PinKind::Struct, "Transformf")).Metadata("{0}:decompose()");
	matrix4x4f.AddFunction("Matrix To Quaternion", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).Out("Retval", PinType(PinKind::Struct, "Quaternionf")).Metadata("{0}:to_quat()");
	matrix4x4f.AddFunction("Transpose Matrix", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).Out("Retval", PinType(PinKind::Struct, "Matrix4x4f")).Metadata("{0}:transpose()");
	matrix4x4f.AddFunction("Matrix Determinant", "Math|Matrix").Pure().In("Matrix", PinType(PinKind::Struct, "Matrix4x4f")).Out("Retval", PinType(PinKind::Float)).Metadata("{0}:determinant()");
	structObject.AddFunction("Make Struct Object", "UEVR|StructObject").Pure().Static().In("Struct Definition", PinType(PinKind::Object)).Out("Retval", PinType(PinKind::Struct, "StructObject")).Metadata("StructObject.new({0})");
	structObject.AddFunction("Get Property (Struct Object)", "UEVR|StructObject").Pure().In("Struct", PinType(PinKind::Struct, "StructObject")).In("Name", PinType(PinKind::String)).Out("Value", PinType(PinKind::Wildcard)).Metadata("{0}:get_property({1})");
	structObject.AddFunction("Set Property (Struct Object)", "UEVR|StructObject").In("Struct", PinType(PinKind::Struct, "StructObject")).In("Name", PinType(PinKind::String)).In("Value", PinType(PinKind::Wildcard)).Metadata("{0}:set_property({1}, {2})");
	structObject.AddFunction("Read Float (Struct Object)", "UEVR|StructObject").Pure().In("Struct", PinType(PinKind::Struct, "StructObject")).In("Offset", PinType(PinKind::Integer), "0").Out("Value", PinType(PinKind::Float)).Metadata("{0}:read_float({1})");
	structObject.AddFunction("Read QWord (Struct Object)", "UEVR|StructObject").Pure().In("Struct", PinType(PinKind::Struct, "StructObject")).In("Offset", PinType(PinKind::Integer), "0").Out("Value", PinType(PinKind::Integer)).Metadata("{0}:read_qword({1})");
	structObject.AddFunction("Write Float (Struct Object)", "UEVR|StructObject").In("Struct", PinType(PinKind::Struct, "StructObject")).In("Offset", PinType(PinKind::Integer), "0").In("Value", PinType(PinKind::Float)).Metadata("{0}:write_float({1}, {2})");
	structObject.AddFunction("Get Address (Struct Object)", "UEVR|StructObject").Pure().In("Struct", PinType(PinKind::Struct, "StructObject")).Out("Address", PinType(PinKind::Integer)).Metadata("{0}:get_address()");
	structObject.AddFunction("Get Struct Def (Struct Object)", "UEVR|StructObject").Pure().In("Struct", PinType(PinKind::Struct, "StructObject")).Out("Retval", PinType(PinKind::Object)).Metadata("{0}:get_struct()");

	// -- Lua tables / arrays (plain nodes) --
	auto& luaTable = registry.AddClass("LuaTable", "", "Lua table / array helpers");
	luaTable.AddFunction("Array Get", "Utilities|Array").Pure().Static().In("Array", PinType(PinKind::Wildcard, "", true)).In("Index", PinType(PinKind::Integer), "1").Out("Element", PinType(PinKind::Wildcard)).Metadata("({0})[{1}]");
	luaTable.AddFunction("Array Set", "Utilities|Array").In("Array", PinType(PinKind::Wildcard, "", true)).In("Index", PinType(PinKind::Integer), "1").In("Value", PinType(PinKind::Wildcard)).Metadata("{0}[{1}] = {2}");
	luaTable.AddFunction("Array Length", "Utilities|Array").Pure().Static().In("Array", PinType(PinKind::Wildcard, "", true)).Out("Length", PinType(PinKind::Integer)).Metadata("#({0})");
	luaTable.AddFunction("Array Add", "Utilities|Array").In("Array", PinType(PinKind::Wildcard, "", true)).In("Value", PinType(PinKind::Wildcard)).Metadata("table.insert({0}, {1})");
	luaTable.AddFunction("Array Insert At", "Utilities|Array").In("Array", PinType(PinKind::Wildcard, "", true)).In("Index", PinType(PinKind::Integer), "1").In("Value", PinType(PinKind::Wildcard)).Metadata("table.insert({0}, {1}, {2})");
	luaTable.AddFunction("Array Remove At", "Utilities|Array").In("Array", PinType(PinKind::Wildcard, "", true)).In("Index", PinType(PinKind::Integer), "1").Metadata("table.remove({0}, {1})");
	luaTable.AddFunction("Array Contains", "Utilities|Array").Pure().Static().In("Array", PinType(PinKind::Wildcard, "", true)).In("Item", PinType(PinKind::Wildcard)).Out("Found", PinType(PinKind::Boolean)).Metadata("(function(__t,__v) for _,__e in ipairs(__t) do if __e == __v then return true end end return false end)({0}, {1})");
	luaTable.AddFunction("Array Is Empty", "Utilities|Array").Pure().Static().In("Array", PinType(PinKind::Wildcard, "", true)).Out("Is Empty", PinType(PinKind::Boolean)).Metadata("(({0}) == nil or #({0}) == 0)");
	luaTable.AddFunction("Map Get", "Utilities|Map").Pure().Static().In("Map", PinType(PinKind::Wildcard, "Map")).In("Key", PinType(PinKind::Wildcard)).Out("Value", PinType(PinKind::Wildcard)).Metadata("({0})[{1}]");
	luaTable.AddFunction("Map Set", "Utilities|Map").In("Map", PinType(PinKind::Wildcard, "Map")).In("Key", PinType(PinKind::Wildcard)).In("Value", PinType(PinKind::Wildcard)).Metadata("{0}[{1}] = {2}");
	luaTable.AddFunction("Map Contains", "Utilities|Map").Pure().Static().In("Map", PinType(PinKind::Wildcard, "Map")).In("Key", PinType(PinKind::Wildcard)).Out("Has Key", PinType(PinKind::Boolean)).Metadata("(({0})[{1}] ~= nil)");
	luaTable.AddFunction("Map Remove", "Utilities|Map").In("Map", PinType(PinKind::Wildcard, "Map")).In("Key", PinType(PinKind::Wildcard)).Metadata("{0}[{1}] = nil");

	// -- UEVR API: object-hook / reflection / console / raw calls / VR --
	auto& uObjectHook = registry.AddClass("UObjectHook", "", "UEVR's UObjectHook (uevr.types.UObjectHook) - enumerate live instances by class and attach objects to VR motion controllers");
	auto& motionControllerState = registry.AddClass("MotionControllerState", "", "Per-object VR motion-controller attach state (from UObjectHook.get_or_add_motion_controller_state)");
	auto& fConsoleManager = registry.AddClass("FConsoleManager", "", "The console manager (uevr.api:get_console_manager()) - look up console variables and commands");
	auto& iConsoleVariable = registry.AddClass("IConsoleVariable", "", "A console variable (cvar) - get/set its int/float/string value");
	auto& iConsoleCommand = registry.AddClass("IConsoleCommand", "", "A console command - execute it with an argument string");
	auto& fUObjectArray = registry.AddClass("FUObjectArray", "", "The global UObject array (uevr.api:get_uobject_array()) - iterate every live UObject");
	auto& fField = registry.AddClass("FField", "", "A reflected field (property list node); walk with get_next");
	auto& fProperty = registry.AddClass("FProperty", "FField", "A reflected property - offset, flags and parameter kind");
	auto& fFieldClass = registry.AddClass("FFieldClass", "", "The class of an FField/FProperty (e.g. FloatProperty, ObjectProperty)");
	auto& uGameViewportClient = registry.AddClass("UGameViewportClient", "UObject", "The game viewport client - exec console commands through it");
	auto& uEVR_System = registry.AddClass("UEVR_System", "", "Top-level uevr.* helpers (address conversion, raw native calls, inline mid-hooks)");
	auto& midHook = registry.AddClass("MidHook", "", "A live safetyhook mid-hook handle returned by uevr.hook_create_mid");
	uObjectHook.AddFunction("Get First Object By Class", "UEVR|Hook").Pure().Static().In("Class", PinType(PinKind::Class, "UClass")).In("Allow Default", PinType(PinKind::Boolean), "false").Out("Object", PinType(PinKind::Object, "UObject")).Metadata("uevr.types.UObjectHook.get_first_object_by_class({0}, {1})");
	uObjectHook.AddFunction("Get Objects By Class", "UEVR|Hook").Pure().Static().In("Class", PinType(PinKind::Class, "UClass")).In("Allow Default", PinType(PinKind::Boolean), "false").Out("Objects", PinType(PinKind::Wildcard)).Metadata("uevr.types.UObjectHook.get_objects_by_class({0}, {1})");
	uObjectHook.AddFunction("Get Or Add Motion Controller State", "UEVR|Hook").Static().In("Object", PinType(PinKind::Object, "UObject")).Out("State", PinType(PinKind::Object, "MotionControllerState")).Metadata("uevr.types.UObjectHook.get_or_add_motion_controller_state({0})");
	uObjectHook.AddFunction("Exists", "UEVR|Hook").Pure().Static().In("Object", PinType(PinKind::Object, "UObject")).Out("Valid", PinType(PinKind::Boolean)).Metadata("uevr.types.UObjectHook.exists({0})");
	motionControllerState.AddFunction("Set Location Offset", "UEVR|Hook").In("Offset", PinType(PinKind::Vector)).Metadata("{target}:set_location_offset({0})");
	motionControllerState.AddFunction("Set Rotation Offset", "UEVR|Hook").In("Rotation", PinType(PinKind::Vector)).Metadata("{target}:set_rotation_offset({0})");
	motionControllerState.AddFunction("Set Hand", "UEVR|Hook").In("Hand", PinType(PinKind::Integer), "1").Metadata("{target}:set_hand({0})");
	motionControllerState.AddFunction("Set Permanent", "UEVR|Hook").In("Permanent", PinType(PinKind::Boolean), "true").Metadata("{target}:set_permanent({0})");
	uclass.AddFunction("Get Class Default Object", "UEVR|Reflection").Pure().Out("CDO", PinType(PinKind::Object, "UObject")).Metadata("{target}:get_class_default_object()");
	uclass.AddFunction("Get Objects Matching", "UEVR|Reflection").Pure().In("Allow Default", PinType(PinKind::Boolean), "false").Out("Objects", PinType(PinKind::Wildcard)).Metadata("{target}:get_objects_matching({0})");
	uclass.AddFunction("Get First Object Matching", "UEVR|Reflection").Pure().In("Allow Default", PinType(PinKind::Boolean), "false").Out("Object", PinType(PinKind::Object, "UObject")).Metadata("{target}:get_first_object_matching({0})");
	ustruct.AddFunction("Get Super Struct", "UEVR|Reflection").Pure().Out("Super", PinType(PinKind::Object, "UStruct")).Metadata("{target}:get_super_struct()");
	ustruct.AddFunction("Get Child Properties", "UEVR|Reflection").Pure().Out("First Field", PinType(PinKind::Object, "FField")).Metadata("{target}:get_child_properties()");
	ustruct.AddFunction("Get Properties Size", "UEVR|Reflection").Pure().Out("Size", PinType(PinKind::Integer)).Metadata("{target}:get_properties_size()");
	ufunction.AddFunction("Call (Caller)", "UEVR|Reflection").In("Caller", PinType(PinKind::Object, "UObject")).Out("Return", PinType(PinKind::Wildcard)).Metadata("{target}:call({0})");
	ufunction.AddFunction("Call (Caller, 1 Arg)", "UEVR|Reflection").In("Caller", PinType(PinKind::Object, "UObject")).In("Arg", PinType(PinKind::Wildcard)).Out("Return", PinType(PinKind::Wildcard)).Metadata("{target}:call({0}, {1})");
	ufunction.AddFunction("Get Native Function", "UEVR|Reflection").Pure().Out("Address", PinType(PinKind::Wildcard)).Metadata("{target}:get_native_function()");
	ufunction.AddFunction("Process Event", "UEVR|Reflection").In("Object", PinType(PinKind::Object, "UObject")).In("Params", PinType(PinKind::Wildcard)).Metadata("{target}:process_event({0}, {1})");
	ufunction.AddFunction("Get Function Flags", "UEVR|Reflection").Pure().Out("Flags", PinType(PinKind::Integer)).Metadata("{target}:get_function_flags()");
	ufunction.AddFunction("Set Function Flags", "UEVR|Reflection").In("Flags", PinType(PinKind::Integer)).Metadata("{target}:set_function_flags({0})");
	object.AddFunction("Get Address", "UEVR|Object").Pure().Out("Address", PinType(PinKind::Integer)).Metadata("{target}:get_address()");
	object.AddFunction("Get Short Name", "UEVR|Object").Pure().Out("Name", PinType(PinKind::String)).Metadata("{target}:get_short_name()");
	object.AddFunction("Get Double Property", "UEVR|Object").Pure().In("Name", PinType(PinKind::String)).Out("Value", PinType(PinKind::Float)).Metadata("{target}:get_double_property({0})");
	object.AddFunction("Call Function By Name", "UEVR|Object").In("Function Name", PinType(PinKind::String), "Jump").Out("Return", PinType(PinKind::Wildcard)).Metadata("{target}:call({0})");
	object.AddFunction("Call Function By Name (1 Arg)", "UEVR|Object").In("Function Name", PinType(PinKind::String), "SetActorHiddenInGame").In("Arg", PinType(PinKind::Wildcard)).Out("Return", PinType(PinKind::Wildcard)).Metadata("{target}:call({0}, {1})");
	object.AddFunction("Read Float", "UEVR|Memory").Pure().In("Offset", PinType(PinKind::Integer)).Out("Value", PinType(PinKind::Float)).Metadata("{target}:read_float({0})");
	object.AddFunction("Write Dword", "UEVR|Memory").In("Offset", PinType(PinKind::Integer)).In("Value", PinType(PinKind::Integer)).Metadata("{target}:write_dword({0}, {1})");
	api.AddFunction("Get UObject Array", "UEVR|API").Pure().Static().Out("Array", PinType(PinKind::Object, "FUObjectArray")).Metadata("uevr.api:get_uobject_array()");
	api.AddFunction("Get Console Manager", "UEVR|API").Pure().Static().Out("Manager", PinType(PinKind::Object, "FConsoleManager")).Metadata("uevr.api:get_console_manager()");
	fConsoleManager.AddFunction("Find Variable", "UEVR|Console").Pure().In("Name", PinType(PinKind::String), "r.ScreenPercentage").Out("Variable", PinType(PinKind::Object, "IConsoleVariable")).Metadata("{target}:find_variable({0})");
	fConsoleManager.AddFunction("Find Command", "UEVR|Console").Pure().In("Name", PinType(PinKind::String)).Out("Command", PinType(PinKind::Object, "IConsoleCommand")).Metadata("{target}:find_command({0})");
	iConsoleVariable.AddFunction("Set", "UEVR|Console").In("Value", PinType(PinKind::Wildcard)).Metadata("{target}:set({0})");
	iConsoleVariable.AddFunction("Get Int", "UEVR|Console").Pure().Out("Value", PinType(PinKind::Integer)).Metadata("{target}:get_int()");
	iConsoleVariable.AddFunction("Get Float", "UEVR|Console").Pure().Out("Value", PinType(PinKind::Float)).Metadata("{target}:get_float()");
	iConsoleCommand.AddFunction("Execute", "UEVR|Console").In("Args", PinType(PinKind::String)).Metadata("{target}:execute({0})");
	fUObjectArray.AddFunction("Get Object Count", "UEVR|Reflection").Pure().Out("Count", PinType(PinKind::Integer)).Metadata("{target}:get_object_count()");
	fUObjectArray.AddFunction("Get Object", "UEVR|Reflection").Pure().In("Index", PinType(PinKind::Integer)).Out("Object", PinType(PinKind::Object, "UObject")).Metadata("{target}:get_object({0})");
	fField.AddFunction("Get Next", "UEVR|Reflection").Pure().Out("Next", PinType(PinKind::Object, "FField")).Metadata("{target}:get_next()");
	fField.AddFunction("Get Field Name", "UEVR|Reflection").Pure().Out("Name", PinType(PinKind::String)).Metadata("{target}:get_fname():to_string()");
	fField.AddFunction("As Property", "UEVR|Reflection").Pure().Out("Property", PinType(PinKind::Object, "FProperty")).Metadata("{target}:as_property()");
	fField.AddFunction("Get Field Class", "UEVR|Reflection").Pure().Out("Class", PinType(PinKind::Object, "FFieldClass")).Metadata("{target}:get_class()");
	fProperty.AddFunction("Get Offset", "UEVR|Reflection").Pure().Out("Offset", PinType(PinKind::Integer)).Metadata("{target}:get_offset()");
	fProperty.AddFunction("Is Out Param", "UEVR|Reflection").Pure().Out("Value", PinType(PinKind::Boolean)).Metadata("{target}:is_out_param()");
	fFieldClass.AddFunction("Get Name", "UEVR|Reflection").Pure().Out("Name", PinType(PinKind::String)).Metadata("{target}:get_name()");
	uEVR_System.AddFunction("To Address", "UEVR|System").Pure().Static().In("Pointer", PinType(PinKind::Wildcard)).Out("Address", PinType(PinKind::Integer)).Metadata("uevr.to_address({0})");
	uEVR_System.AddFunction("Call Native Function", "UEVR|System").Static().In("Target Address", PinType(PinKind::Integer)).In("Self / Arg0", PinType(PinKind::Wildcard)).Out("Return", PinType(PinKind::Wildcard)).Metadata("uevr.call_function({0}, {1})");
	uEVR_System.AddFunction("Remove Mid Hook", "UEVR|System").Static().In("Target Address", PinType(PinKind::Integer)).Out("Removed", PinType(PinKind::Boolean)).Metadata("uevr.hook_remove_mid({0})");
	uEVR_System.AddFunction("Create Mid Hook", "UEVR|System").Static().In("Target Address", PinType(PinKind::Integer)).In("Callback", PinType(PinKind::Wildcard)).Out("Hook", PinType(PinKind::Object, "MidHook")).Metadata("uevr.hook_create_mid({0}, {1})");
	midHook.AddFunction("Remove", "UEVR|System").Out("Removed", PinType(PinKind::Boolean)).Metadata("{target}:remove()");
	uGameViewportClient.AddFunction("Exec", "UEVR|Console").In("Command", PinType(PinKind::String)).Metadata("{target}:exec({0})");
	vr.AddFunction("Get Lowest XInput Index", "UEVR|VR").Pure().Static().Out("Index", PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_lowest_xinput_index()");
	vr.AddFunction("Get Left Joystick Source", "UEVR|VR").Pure().Static().Out("Source", PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_left_joystick_source()");
	vr.AddFunction("Get Right Joystick Source", "UEVR|VR").Pure().Static().Out("Source", PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_right_joystick_source()");
	vr.AddFunction("Get Joystick Axis", "UEVR|VR").Pure().Static().In("Source", PinType(PinKind::Integer)).Out("Axis", PinType(PinKind::Vector)).Metadata("uevr.params.vr.get_joystick_axis({0})");

	editor.SetBlueprint(editor.GetBlueprintName(), "UEVR");
}


//
//	The Lua code generator
//

namespace {

class Generator {
public:
	explicit Generator(const BlueprintEditor& blueprint) : bp(blueprint), registry(blueprint.GetRegistry()) {}

	std::string run();

private:
	// graph navigation
	const Pin* linkedDataSource(ID inputPin) const;
	const Pin* execTargetPin(ID outputPin) const;

	// expressions
	std::string inputExpression(const Pin& pin, int depth);
	std::string outputExpression(const Pin& pin, int depth);
	std::string callExpression(const Node& node, size_t outputIndex, int depth);
	std::string expandTemplate(const std::string& tmpl, const Node& node, size_t outputIndex, int depth);
	std::string substituteTokens(const std::string& text, const Node& node, int depth);
	std::string defaultLiteral(const Pin& pin) const;
	const Pin* targetPin(const Node& node) const;
	std::vector<const Pin*> dataInputs(const Node& node) const;
	std::vector<const Pin*> dataOutputs(const Node& node) const;
	const Pin* findExecOutput(const Node& node, const std::string& name) const;

	// statements
	void emitChain(const Pin* execOutput, std::string& out, int indent);
	void emitNode(const Node& node, const Pin& enteredPin, std::string& out, int indent);
	void emitFlowControl(const Node& node, const Pin& enteredPin, std::string& out, int indent);
	void emitCustomLuaBlock(const Node& node, std::string& out, int indent);
	std::string& stateLocal(const Node& node, const char* prefix, const char* initial);
	static void line(std::string& out, int indent, const std::string& text);

	const BlueprintEditor& bp;
	const BlueprintEditor::TypeRegistry& registry;
	std::string preamble;
	std::unordered_map<ID, std::string> pinNames;   // event args, loop indices, impure results
	std::unordered_map<ID, std::string> stateNames; // flow control node -> state local
	std::vector<ID> execStack;                      // recursion guard for exec cycles
};

void Generator::line(std::string& out, int indent, const std::string& text) {
	for (auto i = 0; i < indent; i++) {
		out += "    ";
	}

	out += text;
	out += "\n";
}

const Pin* Generator::linkedDataSource(ID inputPin) const {
	for (auto& link : bp.GetLinks()) {
		if (link.to == inputPin) {
			return bp.GetPin(link.from);
		}
	}

	return nullptr;
}

const Pin* Generator::execTargetPin(ID outputPin) const {
	for (auto& link : bp.GetLinks()) {
		if (link.from == outputPin) {
			return bp.GetPin(link.to);
		}
	}

	return nullptr;
}

const Pin* Generator::targetPin(const Node& node) const {
	for (auto& pin : node.pins) {
		if (!pin.isOutput && pin.name == "Target") {
			return &pin;
		}
	}

	return nullptr;
}

std::vector<const Pin*> Generator::dataInputs(const Node& node) const {
	std::vector<const Pin*> result;

	for (auto& pin : node.pins) {
		if (!pin.isOutput && pin.type.kind != PinKind::Exec && pin.name != "Target") {
			result.push_back(&pin);
		}
	}

	return result;
}

std::vector<const Pin*> Generator::dataOutputs(const Node& node) const {
	std::vector<const Pin*> result;

	for (auto& pin : node.pins) {
		if (pin.isOutput && pin.type.kind != PinKind::Exec) {
			result.push_back(&pin);
		}
	}

	return result;
}

const Pin* Generator::findExecOutput(const Node& node, const std::string& name) const {
	for (auto& pin : node.pins) {
		if (pin.isOutput && pin.type.kind == PinKind::Exec && pin.name == name) {
			return &pin;
		}
	}

	return nullptr;
}

std::string Generator::defaultLiteral(const Pin& pin) const {
	switch (pin.type.kind) {
		case PinKind::Boolean:
			return pin.defaultValue == "true" ? "true" : "false";

		case PinKind::Byte:
		case PinKind::Integer:
			return luaNumber(pin.defaultValue, false);

		case PinKind::Float:
			return luaNumber(pin.defaultValue, true);

		case PinKind::String:
		case PinKind::Name:
			return luaString(pin.defaultValue);

		case PinKind::Vector: {
			float values[3] = {0.0f, 0.0f, 0.0f};
			const char* p = pin.defaultValue.c_str();

			for (auto i = 0; i < 3; i++) {
				values[i] = std::strtof(p, nullptr);
				const char* comma = std::strchr(p, ',');

				if (!comma) {
					break;
				}

				p = comma + 1;
			}

			char buffer[96];
			std::snprintf(buffer, sizeof(buffer), "Vector3f.new(%g, %g, %g)",
				static_cast<double>(values[0]), static_cast<double>(values[1]), static_cast<double>(values[2]));
			return buffer;
		}

		case PinKind::Enum: {
			if (pin.type.subtype == "EXInputButton") {
				auto& masks = xinputButtonMasks();
				auto mask = masks.find(pin.defaultValue.empty() ? "A" : pin.defaultValue);

				if (mask != masks.end()) {
					return mask->second;
				}
			}

			return luaString(pin.defaultValue);
		}

		default:
			return "nil";
	}
}

std::string Generator::expandTemplate(const std::string& tmpl, const Node& node, size_t outputIndex, int depth) {
	// multi-output templates use one segment per data output, separated by '|'
	std::vector<std::string> segments;
	size_t start = 0;

	while (start <= tmpl.size()) {
		size_t end = tmpl.find('|', start);

		if (end == std::string::npos) {
			segments.push_back(tmpl.substr(start));
			break;
		}

		segments.push_back(tmpl.substr(start, end - start));
		start = end + 1;
	}

	std::string segment = segments[std::min(outputIndex, segments.size() - 1)];
	return substituteTokens(segment, node, depth);
}

std::string Generator::substituteTokens(const std::string& text, const Node& node, int depth) {
	std::vector<const Pin*> inputs = dataInputs(node);
	std::string result;
	size_t i = 0;

	while (i < text.size()) {
		if (text[i] == '{') {
			size_t close = text.find('}', i);

			if (close != std::string::npos) {
				std::string key = text.substr(i + 1, close - i - 1);

				if (key == "target") {
					const Pin* target = targetPin(node);
					result += target ? inputExpression(*target, depth + 1) : "nil";
					i = close + 1;
					continue;

				} else if (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos) {
					size_t index = static_cast<size_t>(std::atoi(key.c_str()));

					if (index < inputs.size()) {
						result += inputExpression(*inputs[index], depth + 1);

					} else {
						result += "nil";
					}

					i = close + 1;
					continue;

				} else if (!key.empty()) {
					// named reference (CustomLua pins): matched by identifier-normalized
					// name rather than position, so it stays valid across pin add/remove
					const Pin* match = nullptr;

					for (auto input : inputs) {
						if (identifier(input->name) == identifier(key)) {
							match = input;
							break;
						}
					}

					result += match ? inputExpression(*match, depth + 1) : "nil --[[ unknown input '" + key + "' ]]";
					i = close + 1;
					continue;
				}
			}
		}

		result += text[i++];
	}

	return result;
}

std::string Generator::callExpression(const Node& node, size_t outputIndex, int depth) {
	// "Make Struct" node (AddMakeStructNode): a Lua table literal keyed by the exact
	// UE property names — how UEVR constructs a script-struct argument. Keys are the
	// raw property names (UE names are valid Lua identifiers).
	if (node.memberName == "$MakeStruct") {
		std::string body;

		for (auto input : dataInputs(node)) {
			std::string value = inputExpression(*input, depth + 1);

			if (value.rfind("nil", 0) == 0) {
				continue; // skip unset object/struct fields (nil keys are no-ops anyway)
			}

			if (!body.empty()) {
				body += ", ";
			}

			body += input->name + " = " + value;
		}

		return "{" + body + "}";
	}

	const BlueprintEditor::Function* function = registry.FindFunction(node.className, node.memberName);

	if (function) {
		if (!function->metadata.empty()) {
			return expandTemplate(function->metadata, node, outputIndex, depth);
		}

		// no template: emit a conventional method or free function call
		std::string args;

		for (auto input : dataInputs(node)) {
			if (!args.empty()) {
				args += ", ";
			}

			args += inputExpression(*input, depth + 1);
		}

		if (function->isStatic) {
			return snakeCase(node.memberName) + "(" + args + ")";
		}

		const Pin* target = targetPin(node);
		std::string targetExpression = target ? inputExpression(*target, depth + 1) : "nil";
		return targetExpression + ":" + snakeCase(node.memberName) + "(" + args + ")";
	}

	// property accessor nodes reference the property directly
	const BlueprintEditor::Property* property = registry.FindProperty(node.className, node.memberName);

	if (property) {
		const Pin* target = targetPin(node);
		std::string targetExpression = target ? inputExpression(*target, depth + 1) : "nil";
		return targetExpression + "." + identifier(node.memberName);
	}

	return "nil --[[ unknown member " + node.memberName + " ]]";
}

std::string Generator::outputExpression(const Pin& pin, int depth) {
	if (depth > 64) {
		return "nil --[[ expression too deep ]]";
	}

	// names registered by events, loops and executed impure calls win
	auto named = pinNames.find(pin.id);

	if (named != pinNames.end()) {
		return named->second;
	}

	const Node* node = bp.GetNode(pin.node);

	if (!node) {
		return "nil";
	}

	switch (node->kind) {
		case NodeKind::VariableGet:
		case NodeKind::VariableSet:
			return identifier(node->memberName);

		case NodeKind::Reroute: {
			for (auto& input : node->pins) {
				if (!input.isOutput) {
					return inputExpression(input, depth + 1);
				}
			}

			return "nil";
		}

		case NodeKind::CallFunction: {
			const BlueprintEditor::Function* function = registry.FindFunction(node->className, node->memberName);

			if (function && !function->isPure) {
				// impure result that was never executed on this chain
				return "nil --[[ '" + node->title + "' has not been executed ]]";
			}

			// determine which data output this pin is
			std::vector<const Pin*> outputs = dataOutputs(*node);

			for (size_t i = 0; i < outputs.size(); i++) {
				if (outputs[i]->id == pin.id) {
					return callExpression(*node, i, depth);
				}
			}

			return callExpression(*node, 0, depth);
		}

		case NodeKind::CustomLua:
			// pinNames is checked at the top of this function already; reaching here
			// means this output was read before the node executed on this chain
			return "nil --[[ '" + node->title + "' has not been executed ]]";

		default:
			return "nil --[[ unsupported source '" + node->title + "' ]]";
	}
}

std::string Generator::inputExpression(const Pin& pin, int depth) {
	if (depth > 64) {
		return "nil --[[ expression too deep ]]";
	}

	const Pin* source = linkedDataSource(pin.id);

	if (source) {
		return outputExpression(*source, depth);
	}

	// unconnected "self" targets have no meaning in a UEVR script
	if (pin.name == "Target" && pin.defaultValue == "self") {
		return "nil --[[ Target not connected ]]";
	}

	return defaultLiteral(pin);
}

std::string& Generator::stateLocal(const Node& node, const char* prefix, const char* initial) {
	auto existing = stateNames.find(node.id);

	if (existing == stateNames.end()) {
		std::string name = std::string(prefix) + std::to_string(node.id);
		preamble += "local " + name + " = " + initial + "\n";
		existing = stateNames.insert(std::make_pair(node.id, name)).first;
	}

	return existing->second;
}

void Generator::emitChain(const Pin* execOutput, std::string& out, int indent) {
	if (!execOutput) {
		return;
	}

	const Pin* target = execTargetPin(execOutput->id);

	if (!target) {
		return;
	}

	const Node* node = bp.GetNode(target->node);

	if (!node) {
		return;
	}

	for (auto id : execStack) {
		if (id == node->id) {
			line(out, indent, "-- execution cycle detected at '" + node->title + "', stopping");
			return;
		}
	}

	execStack.push_back(node->id);
	emitNode(*node, *target, out, indent);
	execStack.pop_back();
}

void Generator::emitNode(const Node& node, const Pin& enteredPin, std::string& out, int indent) {
	switch (node.kind) {
		case NodeKind::Reroute: {
			for (auto& pin : node.pins) {
				if (pin.isOutput) {
					emitChain(&pin, out, indent);
				}
			}

			break;
		}

		case NodeKind::VariableSet: {
			std::string value = "nil";

			for (auto& pin : node.pins) {
				if (!pin.isOutput && pin.type.kind != PinKind::Exec) {
					value = inputExpression(pin, 0);
				}
			}

			line(out, indent, identifier(node.memberName) + " = " + value);
			emitChain(findExecOutput(node, ""), out, indent);
			break;
		}

		case NodeKind::CallFunction: {
			const BlueprintEditor::Function* function = registry.FindFunction(node.className, node.memberName);
			const BlueprintEditor::Property* property = function ? nullptr : registry.FindProperty(node.className, node.memberName);

			if (property) {
				// property set node
				const Pin* target = targetPin(node);
				std::string targetExpression = target ? inputExpression(*target, 0) : "nil";
				std::string value = "nil";

				for (auto input : dataInputs(node)) {
					value = inputExpression(*input, 0);
				}

				std::string reference = targetExpression + "." + identifier(node.memberName);
				line(out, indent, reference + " = " + value);

				for (auto output : dataOutputs(node)) {
					pinNames[output->id] = reference;
				}

			} else {
				std::string expression = callExpression(node, 0, 0);
				std::vector<const Pin*> outputs = dataOutputs(node);
				bool used = false;

				for (auto output : outputs) {
					for (auto& link : bp.GetLinks()) {
						if (link.from == output->id) {
							used = true;
						}
					}
				}

				if (used && !outputs.empty()) {
					std::string name = "result" + std::to_string(node.id);
					line(out, indent, "local " + name + " = " + expression);

					for (auto output : outputs) {
						pinNames[output->id] = name;
					}

				} else {
					line(out, indent, expression);
				}
			}

			emitChain(findExecOutput(node, ""), out, indent);
			break;
		}

		case NodeKind::FlowControl:
			emitFlowControl(node, enteredPin, out, indent);
			break;

		case NodeKind::CustomLua: {
			for (auto output : dataOutputs(node)) {
				line(out, indent, "local " + identifier(output->name) + " = nil");
			}

			emitCustomLuaBlock(node, out, indent);

			for (auto output : dataOutputs(node)) {
				pinNames[output->id] = identifier(output->name);
			}

			emitChain(findExecOutput(node, ""), out, indent);
			break;
		}

		default:
			line(out, indent, "-- cannot execute node '" + node.title + "'");
			break;
	}
}

void Generator::emitCustomLuaBlock(const Node& node, std::string& out, int indent) {
	// substituted directly against the raw multi-line source, NOT through expandTemplate:
	// real Lua can legitimately contain '|' (bitwise op, string/table content), so
	// expandTemplate's split-on-'|' multi-output segmenting must never run over free-form
	// user code.
	std::string substituted = substituteTokens(node.customCode, node, 0);
	size_t start = 0;

	while (start < substituted.size()) {
		size_t end = substituted.find('\n', start);

		if (end == std::string::npos) {
			line(out, indent, substituted.substr(start));
			break;
		}

		line(out, indent, substituted.substr(start, end - start));
		start = end + 1;
	}
}

void Generator::emitFlowControl(const Node& node, const Pin& enteredPin, std::string& out, int indent) {
	const std::string& name = node.memberName;

	auto conditionExpression = [this, &node](const char* pinName) {
		for (auto& pin : node.pins) {
			if (!pin.isOutput && pin.name == pinName) {
				return inputExpression(pin, 0);
			}
		}

		return std::string("false");
	};

	if (name == "Branch") {
		line(out, indent, "if " + conditionExpression("Condition") + " then");
		emitChain(findExecOutput(node, "True"), out, indent + 1);
		const Pin* falsePin = findExecOutput(node, "False");

		if (falsePin && execTargetPin(falsePin->id)) {
			line(out, indent, "else");
			emitChain(falsePin, out, indent + 1);
		}

		line(out, indent, "end");

	} else if (name == "Sequence") {
		for (auto& pin : node.pins) {
			if (pin.isOutput && pin.type.kind == PinKind::Exec) {
				emitChain(&pin, out, indent);
			}
		}

	} else if (name == "For Loop") {
		std::string index = "index" + std::to_string(node.id);

		for (auto output : dataOutputs(node)) {
			pinNames[output->id] = index;
		}

		line(out, indent, "for " + index + " = " + conditionExpression("First Index") + ", " + conditionExpression("Last Index") + " do");
		emitChain(findExecOutput(node, "Loop Body"), out, indent + 1);
		line(out, indent, "end");
		emitChain(findExecOutput(node, "Completed"), out, indent);

	} else if (name == "While Loop") {
		line(out, indent, "while " + conditionExpression("Condition") + " do");
		emitChain(findExecOutput(node, "Loop Body"), out, indent + 1);
		line(out, indent, "end");
		emitChain(findExecOutput(node, "Completed"), out, indent);

	} else if (name == "Do Once") {
		std::string& state = stateLocal(node, "doOnce", "false");

		if (enteredPin.name == "Reset") {
			line(out, indent, state + " = false");

		} else {
			line(out, indent, "if not " + state + " then");
			line(out, indent + 1, state + " = true");
			emitChain(findExecOutput(node, "Completed"), out, indent + 1);
			line(out, indent, "end");
		}

	} else if (name == "Do N") {
		std::string& state = stateLocal(node, "doN", "0");

		if (enteredPin.name == "Reset") {
			line(out, indent, state + " = 0");

		} else {
			for (auto output : dataOutputs(node)) {
				pinNames[output->id] = state;
			}

			line(out, indent, "if " + state + " < " + conditionExpression("N") + " then");
			line(out, indent + 1, state + " = " + state + " + 1");
			emitChain(findExecOutput(node, "Exit"), out, indent + 1);
			line(out, indent, "end");
		}

	} else if (name == "Flip Flop") {
		std::string& state = stateLocal(node, "flipFlop", "false");

		for (auto output : dataOutputs(node)) {
			pinNames[output->id] = state;
		}

		line(out, indent, state + " = not " + state);
		line(out, indent, "if " + state + " then");
		emitChain(findExecOutput(node, "A"), out, indent + 1);
		const Pin* b = findExecOutput(node, "B");

		if (b && execTargetPin(b->id)) {
			line(out, indent, "else");
			emitChain(b, out, indent + 1);
		}

		line(out, indent, "end");

	} else if (name == "Gate") {
		// the gate starts open unless "Start Closed" is set
		bool startClosed = false;

		for (auto& pin : node.pins) {
			if (!pin.isOutput && pin.name == "Start Closed") {
				startClosed = pin.defaultValue == "true";
			}
		}

		std::string& state = stateLocal(node, "gate", startClosed ? "false" : "true");

		if (enteredPin.name == "Open") {
			line(out, indent, state + " = true");

		} else if (enteredPin.name == "Close") {
			line(out, indent, state + " = false");

		} else if (enteredPin.name == "Toggle") {
			line(out, indent, state + " = not " + state);

		} else {
			line(out, indent, "if " + state + " then");
			emitChain(findExecOutput(node, "Exit"), out, indent + 1);
			line(out, indent, "end");
		}

	} else {
		line(out, indent, "-- unsupported flow control node '" + name + "'");
	}
}

std::string Generator::run() {
	std::string body;

	// custom events become local functions
	for (auto& node : bp.GetNodes()) {
		if (node.kind == NodeKind::CustomEvent) {
			const Pin* exec = findExecOutput(node, "");

			body += "local function " + identifier(node.memberName) + "()\n";
			std::string inner;
			emitChain(exec, inner, 1);
			body += inner.empty() ? "    -- empty\n" : inner;
			body += "end\n\n";
		}
	}

	// event nodes become callback registrations
	for (auto& node : bp.GetNodes()) {
		if (node.kind != NodeKind::Event) {
			continue;
		}

		const BlueprintEditor::Function* event = registry.FindEvent(node.className, node.memberName);
		std::string callback = (event && !event->metadata.empty()) ?
			event->metadata :
			"uevr.sdk.callbacks.on_" + snakeCase(node.memberName);

		// register the event parameters as callback arguments
		std::string args;

		for (auto& pin : node.pins) {
			if (pin.isOutput && pin.type.kind != PinKind::Exec) {
				std::string arg = identifier(pin.name);

				if (!args.empty()) {
					args += ", ";
				}

				args += arg;
				pinNames[pin.id] = arg;
			}
		}

		const Pin* exec = findExecOutput(node, "");

		if (!exec || !execTargetPin(exec->id)) {
			continue; // nothing wired up
		}

		body += callback + "(function(" + args + ")\n";
		std::string inner;
		emitChain(exec, inner, 1);
		body += inner.empty() ? "    -- empty\n" : inner;
		body += "end)\n\n";
	}

	// assemble the script
	std::string script;
	script += "-- " + bp.GetBlueprintName() + ".lua\n";
	script += "-- generated by BlueprintEditor (UEVR Lua backend)\n\n";

	if (!bp.GetVariables().empty()) {
		for (auto& variable : bp.GetVariables()) {
			Pin fake;
			fake.type = variable.type;
			fake.defaultValue = variable.defaultValue;
			script += "local " + identifier(variable.name) + " = " + defaultLiteral(fake) + "\n";
		}

		script += "\n";
	}

	if (!preamble.empty()) {
		script += preamble + "\n";
	}

	script += body;

	// trim trailing newlines down to one
	while (script.size() > 1 && script[script.size() - 1] == '\n' && script[script.size() - 2] == '\n') {
		script.pop_back();
	}

	return script;
}

} // anonymous namespace


//
//	BlueprintLua::GenerateScript
//

std::string BlueprintLua::GenerateScript(const BlueprintEditor& editor) {
	Generator generator(editor);
	return generator.run();
}


//
//	BlueprintLua::LuaApiIdentifiers
//

const std::vector<std::string>& BlueprintLua::LuaApiIdentifiers() {
	// Mirrors the identifiers SetupUEVRRegistry exposes (and the ones the
	// generator emits). Whole-word completions for a Lua editor's trie —
	// namespaces, sdk callback names, api/vr/xinput methods, UObject accessors,
	// plus the handful of Lua stdlib helpers the codegen leans on. The full
	// imgui.* binding surface is appended from BlueprintImGuiNames.h rather than
	// hand-duplicated here.
	static const std::vector<std::string> words = [] {
		std::vector<std::string> list = {
			// namespaces / roots
			"uevr", "api", "params", "sdk", "callbacks", "functions", "vr", "imgui",
			// sdk callback registration names (all 18; see SetupUEVRRegistry)
			"on_pre_engine_tick", "on_post_engine_tick",
			"on_pre_slate_draw_window_render_thread", "on_post_slate_draw_window_render_thread",
			"on_xinput_get_state", "on_xinput_set_state", "on_draw_ui", "on_script_reset",
			"on_frame", "on_pawn_changed", "on_view_target_changed", "on_level_changed",
			"on_lua_event", "on_early_calculate_stereo_view_offset",
			"on_pre_calculate_stereo_view_offset", "on_post_calculate_stereo_view_offset",
			"on_pre_viewport_client_draw", "on_post_viewport_client_draw",
			// uevr.api methods
			"find_uobject", "to_uobject", "get_local_pawn", "get_player_controller", "get_engine",
			"spawn_object", "add_component_by_class", "execute_command", "dispatch_custom_event",
			"get_uobject_array", "get_console_manager",
			// uevr.params.functions
			"log_info", "log_error", "log_warn", "is_drawing_ui",
			// UObject accessors
			"get_full_name", "get_fname", "get_class", "get_outer", "as_class", "as_struct",
			"as_function", "is_a", "to_string", "get_property", "set_property", "call_function",
			"get_bool_property", "get_float_property", "get_int_property", "get_uint_property",
			"get_fname_property", "get_uobject_property", "get_property_info", "get_properties",
			"find_property", "find_function", "get_children", "get_child_properties",
			// uevr.params.vr
			"is_runtime_ready", "is_openvr", "is_openxr", "is_hmd_active", "is_using_controllers",
			"get_hmd_index", "get_left_controller_index", "get_right_controller_index",
			"get_hmd_width", "get_hmd_height", "get_ui_width", "get_ui_height",
			"get_movement_orientation", "get_aim_method", "set_aim_method", "is_aim_allowed",
			"set_aim_allowed", "is_snap_turn_enabled", "set_snap_turn_enabled",
			"set_decoupled_pitch_enabled", "get_action_handle", "is_action_active",
			"trigger_haptic_vibration", "get_mod_value", "set_mod_value",
			"recenter_view", "recenter_horizon", "save_config", "reload_config",
			"get_standing_origin", "set_standing_origin", "get_rotation_offset", "set_rotation_offset",
			// XINPUT_STATE fields the xinput helpers touch
			"Gamepad", "wButtons", "bLeftTrigger", "bRightTrigger",
			"sThumbLX", "sThumbLY", "sThumbRX", "sThumbRY",
			// math / vector helpers the generator emits
			"Vector3f", "Vector2f", "new", "select",
			// Lua stdlib the codegen leans on (handy in hand-written scripts too)
			"print", "tostring", "tonumber", "pairs", "ipairs", "string", "math",
			"format", "floor", "ceil", "random", "min", "max", "abs",
		};

		const auto& imguiNames = imguiFunctionNames();
		list.insert(list.end(), imguiNames.begin(), imguiNames.end());
		return list;
	}();

	return words;
}
