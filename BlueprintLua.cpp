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

	// the pseudo class providing UEVR's script entry points
	auto& uevr = registry.AddClass("UEVR", "", "UEVR script entry points");
	uevr.AddEvent("Pre Engine Tick").Out("Engine", PinType(PinKind::Object, "UEngine")).Out("Delta Seconds", PinType(PinKind::Float)).Metadata("uevr.sdk.callbacks.on_pre_engine_tick").Tooltip("Called before the engine ticks each frame");
	uevr.AddEvent("Post Engine Tick").Out("Engine", PinType(PinKind::Object, "UEngine")).Out("Delta Seconds", PinType(PinKind::Float)).Metadata("uevr.sdk.callbacks.on_post_engine_tick").Tooltip("Called after the engine ticks each frame");
	uevr.AddEvent("Pre Slate Draw Window").Out("Renderer", PinType(PinKind::Object, "UObject")).Out("Viewport Info", PinType(PinKind::Object, "UObject")).Metadata("uevr.sdk.callbacks.on_pre_slate_draw_window").Tooltip("Called before slate draws the window");
	uevr.AddEvent("XInput Get State").Out("Retval", PinType(PinKind::Integer)).Out("User Index", PinType(PinKind::Integer)).Out("State", PinType(PinKind::Struct, "XINPUT_STATE")).Metadata("uevr.sdk.callbacks.on_xinput_get_state").Tooltip("Called when the game polls the gamepad; the state can be inspected or modified");
	uevr.AddEvent("Draw UI").Metadata("uevr.sdk.callbacks.on_draw_ui").Tooltip("Called when the UEVR overlay UI is drawn");
	uevr.AddEvent("Script Reset").Metadata("uevr.sdk.callbacks.on_script_reset").Tooltip("Called when the script is reset or unloaded");

	// UObject access (UEVR exposes properties and functions through metatables)
	auto& object = registry.AddClass("UObject", "", "An Unreal object accessed through UEVR's reflection bindings");
	object.AddFunction("Get Full Name", "UObject").Pure().Ret(PinType(PinKind::String)).Metadata("{target}:get_full_name()");
	object.AddFunction("Get FName", "UObject").Pure().Ret(PinType(PinKind::Name)).Metadata("{target}:get_fname():to_string()");
	object.AddFunction("Get Class", "UObject").Pure().Ret(PinType(PinKind::Object, "UObject")).Metadata("{target}:get_class()");
	object.AddFunction("Is A", "UObject").Pure().In("Class", PinType(PinKind::Object, "UObject")).Ret(PinType(PinKind::Boolean)).Metadata("{target}:is_a({0})");
	object.AddFunction("Is Valid", "UObject").Pure().Keywords("null nil check").Ret(PinType(PinKind::Boolean)).Metadata("({target} ~= nil)");
	object.AddFunction("Get Property", "UObject").Pure().Keywords("read field").In("Name", PinType(PinKind::String)).Ret(PinType(PinKind::Wildcard)).Metadata("{target}[{0}]");
	object.AddFunction("Set Property", "UObject").Keywords("write field").In("Name", PinType(PinKind::String)).In("Value", PinType(PinKind::Wildcard)).Metadata("{target}[{0}] = {1}");
	object.AddFunction("Call (No Args)", "UObject").Keywords("invoke function method").In("Function Name", PinType(PinKind::String), "Jump").Ret(PinType(PinKind::Wildcard)).Metadata("{target}[{0}]({target})");
	object.AddFunction("Call (1 Arg)", "UObject").Keywords("invoke function method").In("Function Name", PinType(PinKind::String), "SetActorScale3D").In("Arg", PinType(PinKind::Wildcard)).Ret(PinType(PinKind::Wildcard)).Metadata("{target}[{0}]({target}, {1})");

	auto& actor = registry.AddClass("AActor", "UObject", "An actor in the game world");
	actor.AddProperty("bHidden", PinType(PinKind::Boolean), "Actor");
	actor.AddProperty("bCanBeDamaged", PinType(PinKind::Boolean), "Actor");
	registry.AddClass("APawn", "AActor", "A possessable actor");
	registry.AddClass("UEngine", "UObject", "The engine singleton");

	// the uevr.api object
	auto& api = registry.AddClass("UEVR_API", "", "The uevr.api object");
	api.AddFunction("Find UObject", "UEVR|API").Pure().Static().Keywords("search class object").In("Name", PinType(PinKind::String), "Class /Script/Engine.Pawn").Ret(PinType(PinKind::Object, "UObject")).Metadata("uevr.api:find_uobject({0})").Tooltip("Find an object by its full name");
	api.AddFunction("Get Local Pawn", "UEVR|API").Pure().Static().Keywords("player character").In("Player Index", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Object, "APawn")).Metadata("uevr.api:get_local_pawn({0})");
	api.AddFunction("Get Player Controller", "UEVR|API").Pure().Static().In("Player Index", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Object, "UObject")).Metadata("uevr.api:get_player_controller({0})");
	api.AddFunction("Get Engine", "UEVR|API").Pure().Static().Ret(PinType(PinKind::Object, "UEngine")).Metadata("uevr.api:get_engine()");
	api.AddFunction("Execute Command", "UEVR|API").Static().Keywords("console cvar").In("Command", PinType(PinKind::String)).Metadata("uevr.api:execute_command({0})").Tooltip("Execute a console command");
	api.AddFunction("Print", "UEVR|API").Static().Keywords("log output debug").In("Message", PinType(PinKind::String), "Hello from UEVR").Metadata("print({0})").Tooltip("Print to the UEVR log");
	api.AddFunction("Log Info", "UEVR|API").Static().Keywords("log output debug").In("Message", PinType(PinKind::String)).Metadata("uevr.params.functions.log_info({0})");

	// the uevr.params.vr runtime object
	auto& vr = registry.AddClass("UEVR_VRData", "", "The uevr.params.vr object");
	vr.AddFunction("Is HMD Active", "UEVR|VR").Pure().Static().Keywords("headset").Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_hmd_active()");
	vr.AddFunction("Is Using Controllers", "UEVR|VR").Pure().Static().Keywords("motion").Ret(PinType(PinKind::Boolean)).Metadata("uevr.params.vr.is_using_controllers()");
	vr.AddFunction("Get HMD Index", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_hmd_index()");
	vr.AddFunction("Get Left Controller Index", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_left_controller_index()");
	vr.AddFunction("Get Right Controller Index", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Integer)).Metadata("uevr.params.vr.get_right_controller_index()");
	vr.AddFunction("Trigger Haptic Vibration", "UEVR|VR").Static().Keywords("rumble feedback controller").In("Seconds From Now", PinType(PinKind::Float), "0.0").In("Duration", PinType(PinKind::Float), "0.1").In("Frequency", PinType(PinKind::Float), "1.0").In("Amplitude", PinType(PinKind::Float), "300.0").In("Source", PinType(PinKind::Integer), "1").Metadata("uevr.params.vr.trigger_haptic_vibration({0}, {1}, {2}, {3}, {4})");
	vr.AddFunction("Get Mod Value", "UEVR|VR").Pure().Static().Keywords("setting option").In("Name", PinType(PinKind::String), "VR_RoomscaleMovement").Ret(PinType(PinKind::String)).Metadata("uevr.params.vr.get_mod_value({0})");
	vr.AddFunction("Set Mod Value", "UEVR|VR").Static().Keywords("setting option").In("Name", PinType(PinKind::String)).In("Value", PinType(PinKind::String)).Metadata("uevr.params.vr.set_mod_value({0}, {1})");
	vr.AddFunction("Recenter View", "UEVR|VR").Static().Keywords("reset origin").Metadata("uevr.params.vr.recenter_view()");
	vr.AddFunction("Get Standing Origin", "UEVR|VR").Pure().Static().Ret(PinType(PinKind::Vector)).Metadata("uevr.params.vr.get_standing_origin()");
	vr.AddFunction("Set Standing Origin", "UEVR|VR").Static().In("Origin", PinType(PinKind::Vector)).Metadata("uevr.params.vr.set_standing_origin({0})");

	// helpers to inspect the XINPUT_STATE passed to the XInput Get State event
	auto& xinput = registry.AddClass("XInput", "", "XINPUT_STATE helpers");
	xinput.AddFunction("Is Button Pressed", "UEVR|XInput").Pure().Static().Keywords("gamepad controller").In("State", PinType(PinKind::Struct, "XINPUT_STATE")).In("Button", PinType(PinKind::Enum, "EXInputButton"), "A").Ret(PinType(PinKind::Boolean)).Metadata("(({0}.Gamepad.wButtons & {1}) ~= 0)");
	xinput.AddFunction("Get Buttons", "UEVR|XInput").Pure().Static().In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Ret(PinType(PinKind::Integer)).Metadata("{0}.Gamepad.wButtons");
	xinput.AddFunction("Get Left Trigger", "UEVR|XInput").Pure().Static().In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Ret(PinType(PinKind::Integer)).Metadata("{0}.Gamepad.bLeftTrigger");
	xinput.AddFunction("Get Right Trigger", "UEVR|XInput").Pure().Static().In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Ret(PinType(PinKind::Integer)).Metadata("{0}.Gamepad.bRightTrigger");
	xinput.AddFunction("Get Left Thumb", "UEVR|XInput").Pure().Static().Keywords("stick axis").In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Out("X", PinType(PinKind::Integer)).Out("Y", PinType(PinKind::Integer)).Metadata("{0}.Gamepad.sThumbLX|{0}.Gamepad.sThumbLY");
	xinput.AddFunction("Get Right Thumb", "UEVR|XInput").Pure().Static().Keywords("stick axis").In("State", PinType(PinKind::Struct, "XINPUT_STATE")).Out("X", PinType(PinKind::Integer)).Out("Y", PinType(PinKind::Integer)).Metadata("{0}.Gamepad.sThumbRX|{0}.Gamepad.sThumbRY");

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
	std::string defaultLiteral(const Pin& pin) const;
	const Pin* targetPin(const Node& node) const;
	std::vector<const Pin*> dataInputs(const Node& node) const;
	std::vector<const Pin*> dataOutputs(const Node& node) const;
	const Pin* findExecOutput(const Node& node, const std::string& name) const;

	// statements
	void emitChain(const Pin* execOutput, std::string& out, int indent);
	void emitNode(const Node& node, const Pin& enteredPin, std::string& out, int indent);
	void emitFlowControl(const Node& node, const Pin& enteredPin, std::string& out, int indent);
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
	std::vector<const Pin*> inputs = dataInputs(node);
	std::string result;
	size_t i = 0;

	while (i < segment.size()) {
		if (segment[i] == '{') {
			size_t close = segment.find('}', i);

			if (close != std::string::npos) {
				std::string key = segment.substr(i + 1, close - i - 1);

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
				}
			}
		}

		result += segment[i++];
	}

	return result;
}

std::string Generator::callExpression(const Node& node, size_t outputIndex, int depth) {
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

		default:
			line(out, indent, "-- cannot execute node '" + node.title + "'");
			break;
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
	// plus the handful of Lua stdlib helpers the codegen leans on.
	static const std::vector<std::string> words = {
		// namespaces / roots
		"uevr", "api", "params", "sdk", "callbacks", "functions", "vr",
		// sdk callback registration names
		"on_pre_engine_tick", "on_post_engine_tick", "on_pre_slate_draw_window",
		"on_xinput_get_state", "on_draw_ui", "on_script_reset",
		"on_lua_event", "on_frame",
		// uevr.api methods
		"find_uobject", "get_local_pawn", "get_player_controller", "get_engine",
		"execute_command", "get_uobject_array", "find_uobjects",
		// uevr.params.functions
		"log_info", "log_error", "log_warn",
		// UObject accessors
		"get_full_name", "get_fname", "get_class", "is_a", "to_string",
		"get_property", "set_property", "call_function",
		// uevr.params.vr
		"is_hmd_active", "is_using_controllers", "get_hmd_index",
		"get_left_controller_index", "get_right_controller_index",
		"trigger_haptic_vibration", "get_mod_value", "set_mod_value",
		"recenter_view", "get_standing_origin", "set_standing_origin",
		"get_rotation_offset", "set_rotation_offset", "get_position_offset",
		// XINPUT_STATE fields the xinput helpers touch
		"Gamepad", "wButtons", "bLeftTrigger", "bRightTrigger",
		"sThumbLX", "sThumbLY", "sThumbRX", "sThumbRY",
		// math / vector helpers the generator emits
		"Vector3f", "Vector2f", "new",
		// Lua stdlib the codegen leans on (handy in hand-written scripts too)
		"print", "tostring", "tonumber", "pairs", "ipairs", "string", "math",
		"format", "floor", "ceil", "random", "min", "max", "abs",
	};
	return words;
}
