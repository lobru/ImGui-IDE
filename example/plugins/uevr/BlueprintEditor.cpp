//	BlueprintEditor - An Unreal Engine style Blueprint visual scripting
//	editor for Dear ImGui.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "imgui.h"

#include "TextEditor.h"

#include "BlueprintEditor.h"


//
//	Constants and local helpers
//

static const char* GRAPH_MAGIC = "ImBlueprintGraph";
static const char* CLIP_MAGIC = "ImBlueprintClipboard";

static constexpr float MIN_ZOOM = 0.3f;
static constexpr float MAX_ZOOM = 2.5f;
static constexpr size_t MAX_UNDO = 128;

static inline float lengthSquared(const ImVec2& v) {
	return v.x * v.x + v.y * v.y;
}

static inline ImVec2 vecMin(const ImVec2& a, const ImVec2& b) {
	return ImVec2(std::min(a.x, b.x), std::min(a.y, b.y));
}

static inline ImVec2 vecMax(const ImVec2& a, const ImVec2& b) {
	return ImVec2(std::max(a.x, b.x), std::max(a.y, b.y));
}

static float distanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
	ImVec2 ab = b - a;
	float t = lengthSquared(ab);

	if (t > 0.0f) {
		t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / t, 0.0f, 1.0f);
	}

	ImVec2 closest = a + ab * t;
	return std::sqrt(lengthSquared(p - closest));
}

static std::string toLower(const std::string& text) {
	std::string result = text;
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return result;
}

static std::string formatFloat(float value) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
	return buffer;
}

static void parseFloats(const std::string& text, float* values, int count) {
	const char* p = text.c_str();

	for (auto i = 0; i < count; i++) {
		values[i] = std::strtof(p, nullptr);
		const char* comma = std::strchr(p, ',');

		if (!comma) {
			break;
		}

		p = comma + 1;
	}
}

static std::string escapeString(const std::string& text) {
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

static bool tokenizeLine(const std::string& line, std::vector<std::string>& tokens) {
	tokens.clear();
	size_t i = 0;

	while (i < line.size()) {
		// skip whitespace
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
			i++;
		}

		if (i >= line.size()) {
			break;
		}

		std::string token;

		if (line[i] == '"') {
			// quoted string with escapes
			i++;
			bool closed = false;

			while (i < line.size()) {
				char c = line[i++];

				if (c == '\\' && i < line.size()) {
					char e = line[i++];
					if (e == 'n') { token += '\n'; }
					else if (e == 't') { token += '\t'; }
					else { token += e; }

				} else if (c == '"') {
					closed = true;
					break;

				} else {
					token += c;
				}
			}

			if (!closed) {
				return false;
			}

		} else {
			while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
				token += line[i++];
			}
		}

		tokens.push_back(token);
	}

	return !tokens.empty();
}


// (Custom Lua source is edited with the app's own TextEditor widget — see
// renderCustomLuaNode / ensureLuaEditor — not a raw InputTextMultiline.)


//
//	Static description tables
//

struct PinKindInfo {
	const char* name;
	const char* label;
	ImU32 color;
};

static const PinKindInfo pinKindInfos[] = {
	{"exec", "Exec", IM_COL32(255, 255, 255, 255)},
	{"bool", "Boolean", IM_COL32(148, 26, 20, 255)},
	{"byte", "Byte", IM_COL32(0, 109, 99, 255)},
	{"int", "Integer", IM_COL32(31, 224, 167, 255)},
	{"float", "Float", IM_COL32(160, 250, 68, 255)},
	{"string", "String", IM_COL32(251, 1, 203, 255)},
	{"name", "Name", IM_COL32(200, 161, 254, 255)},
	{"vector", "Vector", IM_COL32(251, 201, 19, 255)},
	{"rotator", "Rotator", IM_COL32(160, 167, 255, 255)},
	{"transform", "Transform", IM_COL32(243, 111, 33, 255)},
	{"object", "Object Reference", IM_COL32(20, 166, 239, 255)},
	{"class", "Class Reference", IM_COL32(136, 51, 204, 255)},
	{"struct", "Struct", IM_COL32(27, 88, 171, 255)},
	{"enum", "Enum", IM_COL32(0, 109, 99, 255)},
	{"delegate", "Delegate", IM_COL32(255, 56, 56, 255)},
	{"wildcard", "Wildcard", IM_COL32(160, 160, 160, 255)}
};

static const char* nodeKindNames[] = {
	"Event",
	"CustomEvent",
	"CallFunction",
	"VariableGet",
	"VariableSet",
	"FlowControl",
	"Reroute",
	"Comment",
	"CustomLua",
	"Cast"
};

// built-in flow control node definitions (Unreal's standard macros)
struct FlowPinDef {
	bool isOutput;
	BlueprintEditor::PinKind kind;
	const char* name;
	const char* defaultValue;
};

struct FlowDef {
	const char* name;
	const char* tooltip;
	std::vector<FlowPinDef> pins;
};

static const std::vector<FlowDef>& flowDefinitions() {
	static const std::vector<FlowDef> defs = {
		{"Branch", "Route execution based on a condition", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{false, BlueprintEditor::PinKind::Boolean, "Condition", "false"},
			{true, BlueprintEditor::PinKind::Exec, "True", ""},
			{true, BlueprintEditor::PinKind::Exec, "False", ""}}},
		{"Sequence", "Execute a series of pins in order", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{true, BlueprintEditor::PinKind::Exec, "Then 0", ""},
			{true, BlueprintEditor::PinKind::Exec, "Then 1", ""}}},
		{"For Loop", "Loop from the first to the last index", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{false, BlueprintEditor::PinKind::Integer, "First Index", "0"},
			{false, BlueprintEditor::PinKind::Integer, "Last Index", "10"},
			{true, BlueprintEditor::PinKind::Exec, "Loop Body", ""},
			{true, BlueprintEditor::PinKind::Integer, "Index", ""},
			{true, BlueprintEditor::PinKind::Exec, "Completed", ""}}},
		{"While Loop", "Loop while the condition is true", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{false, BlueprintEditor::PinKind::Boolean, "Condition", "false"},
			{true, BlueprintEditor::PinKind::Exec, "Loop Body", ""},
			{true, BlueprintEditor::PinKind::Exec, "Completed", ""}}},
		{"Is Valid", "Route execution by whether the object is nil", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{false, BlueprintEditor::PinKind::Wildcard, "Object", ""},
			{true, BlueprintEditor::PinKind::Exec, "Is Valid", ""},
			{true, BlueprintEditor::PinKind::Exec, "Is Not Valid", ""}}},
		{"For Each", "Execute once per array element (ipairs, 1-based)", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{false, BlueprintEditor::PinKind::Wildcard, "Array", ""},
			{true, BlueprintEditor::PinKind::Exec, "Loop Body", ""},
			{true, BlueprintEditor::PinKind::Wildcard, "Element", ""},
			{true, BlueprintEditor::PinKind::Integer, "Index", ""},
			{true, BlueprintEditor::PinKind::Exec, "Completed", ""}}},
		{"For Each (Pairs)", "Execute once per key/value of a table (pairs)", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{false, BlueprintEditor::PinKind::Wildcard, "Table", ""},
			{true, BlueprintEditor::PinKind::Exec, "Loop Body", ""},
			{true, BlueprintEditor::PinKind::Wildcard, "Key", ""},
			{true, BlueprintEditor::PinKind::Wildcard, "Value", ""},
			{true, BlueprintEditor::PinKind::Exec, "Completed", ""}}},
		{"Do Once", "Only execute once until reset", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{false, BlueprintEditor::PinKind::Exec, "Reset", ""},
			{true, BlueprintEditor::PinKind::Exec, "Completed", ""}}},
		{"Do N", "Execute N times until reset", {
			{false, BlueprintEditor::PinKind::Exec, "Enter", ""},
			{false, BlueprintEditor::PinKind::Integer, "N", "1"},
			{false, BlueprintEditor::PinKind::Exec, "Reset", ""},
			{true, BlueprintEditor::PinKind::Exec, "Exit", ""},
			{true, BlueprintEditor::PinKind::Integer, "Counter", ""}}},
		{"Flip Flop", "Alternate between the A and B outputs", {
			{false, BlueprintEditor::PinKind::Exec, "", ""},
			{true, BlueprintEditor::PinKind::Exec, "A", ""},
			{true, BlueprintEditor::PinKind::Exec, "B", ""},
			{true, BlueprintEditor::PinKind::Boolean, "Is A", ""}}},
		{"Gate", "Open or close an execution gate", {
			{false, BlueprintEditor::PinKind::Exec, "Enter", ""},
			{false, BlueprintEditor::PinKind::Exec, "Open", ""},
			{false, BlueprintEditor::PinKind::Exec, "Close", ""},
			{false, BlueprintEditor::PinKind::Exec, "Toggle", ""},
			{false, BlueprintEditor::PinKind::Boolean, "Start Closed", "false"},
			{true, BlueprintEditor::PinKind::Exec, "Exit", ""}}}
	};

	return defs;
}

const char* BlueprintEditor::pinKindName(PinKind kind) {
	return pinKindInfos[static_cast<int>(kind)].name;
}

bool BlueprintEditor::pinKindFromName(const std::string& name, PinKind& kind) {
	for (size_t i = 0; i < static_cast<size_t>(IM_ARRAYSIZE(pinKindInfos)); i++) {
		if (name == pinKindInfos[i].name) {
			kind = static_cast<PinKind>(i);
			return true;
		}
	}

	return false;
}

const char* BlueprintEditor::nodeKindName(NodeKind kind) {
	return nodeKindNames[static_cast<int>(kind)];
}

bool BlueprintEditor::nodeKindFromName(const std::string& name, NodeKind& kind) {
	for (size_t i = 0; i < static_cast<size_t>(IM_ARRAYSIZE(nodeKindNames)); i++) {
		if (name == nodeKindNames[i]) {
			kind = static_cast<NodeKind>(i);
			return true;
		}
	}

	return false;
}

ImU32 BlueprintEditor::pinColor(const PinType& type) const {
	return pinKindInfos[static_cast<int>(type.kind)].color;
}

std::string BlueprintEditor::pinTypeLabel(const PinType& type) const {
	std::string result = pinKindInfos[static_cast<int>(type.kind)].label;

	if (!type.subtype.empty()) {
		result = type.subtype + " " + result;
	}

	if (type.isArray) {
		result = "Array of " + result + "s";
	}

	return result;
}

ImU32 BlueprintEditor::headerColor(const Node& node) const {
	switch (node.kind) {
		case NodeKind::Event:
		case NodeKind::CustomEvent:
			return IM_COL32(148, 28, 28, 255);

		case NodeKind::CallFunction:
			return node.pure ? IM_COL32(60, 130, 60, 255) : IM_COL32(42, 79, 143, 255);

		case NodeKind::VariableSet:
			return IM_COL32(75, 90, 110, 255);

		case NodeKind::FlowControl:
			return IM_COL32(80, 80, 80, 255);

		case NodeKind::CustomLua:
			return IM_COL32(120, 60, 140, 255);

		default:
			return IM_COL32(60, 60, 60, 255);
	}
}


//
//	BlueprintEditor::TypeRegistry
//

BlueprintEditor::Class& BlueprintEditor::TypeRegistry::AddClass(const std::string& name, const std::string& parent, const std::string& tooltip) {
	classes.push_back(std::make_unique<Class>());
	classes.back()->name = name;
	classes.back()->parentName = parent;
	classes.back()->tooltip = tooltip;
	return *classes.back();
}

BlueprintEditor::Enumeration& BlueprintEditor::TypeRegistry::AddEnum(const std::string& name, const std::vector<std::string>& values) {
	enums.push_back(std::make_unique<Enumeration>());
	enums.back()->name = name;
	enums.back()->values = values;
	return *enums.back();
}

// UE C++ prefixes (A=actor, U=uobject-derived, F=struct, I=interface) vs the
// unprefixed names UEVR runtime reflection uses: "AActor"<->"Actor",
// "UObject"<->"Object", "UClass"<->"Class". Strip a single leading prefix letter
// only when the NEXT char is uppercase, so real names keep theirs ("AudioComp",
// "Item", "Object"). Lets built-in nodes (prefixed) interoperate with dumped
// classes (unprefixed) throughout type checking + lookup.
std::string BlueprintEditor::TypeRegistry::normalizeClassName(const std::string& n) {
	if (n.size() >= 2 && (n[0] == 'A' || n[0] == 'U' || n[0] == 'F' || n[0] == 'I') &&
	    n[1] >= 'A' && n[1] <= 'Z') {
		return n.substr(1);
	}
	return n;
}

const BlueprintEditor::Class* BlueprintEditor::TypeRegistry::FindClass(const std::string& name) const {
	for (auto& cls : classes) {
		if (cls->name == name) {
			return cls.get();
		}
	}

	// Prefix-insensitive fallback: "AActor" resolves a dumped "Actor" and vice versa.
	std::string want = normalizeClassName(name);
	for (auto& cls : classes) {
		if (normalizeClassName(cls->name) == want) {
			return cls.get();
		}
	}

	return nullptr;
}

const BlueprintEditor::Enumeration* BlueprintEditor::TypeRegistry::FindEnum(const std::string& name) const {
	for (auto& e : enums) {
		if (e->name == name) {
			return e.get();
		}
	}

	return nullptr;
}

const BlueprintEditor::Function* BlueprintEditor::TypeRegistry::FindFunction(const std::string& className, const std::string& functionName) const {
	const Class* cls = FindClass(className);
	int depth = 0;

	while (cls && depth++ < 64) {
		for (auto& function : cls->functions) {
			if (function.name == functionName) {
				return &function;
			}
		}

		cls = FindClass(cls->parentName);
	}

	return nullptr;
}

const BlueprintEditor::Function* BlueprintEditor::TypeRegistry::FindEvent(const std::string& className, const std::string& eventName) const {
	const Class* cls = FindClass(className);
	int depth = 0;

	while (cls && depth++ < 64) {
		for (auto& event : cls->events) {
			if (event.name == eventName) {
				return &event;
			}
		}

		cls = FindClass(cls->parentName);
	}

	return nullptr;
}

const BlueprintEditor::Property* BlueprintEditor::TypeRegistry::FindProperty(const std::string& className, const std::string& propertyName) const {
	const Class* cls = FindClass(className);
	int depth = 0;

	while (cls && depth++ < 64) {
		for (auto& property : cls->properties) {
			if (property.name == propertyName) {
				return &property;
			}
		}

		cls = FindClass(cls->parentName);
	}

	return nullptr;
}

bool BlueprintEditor::TypeRegistry::IsChildOf(const std::string& child, const std::string& parent) const {
	if (child.empty() || parent.empty()) {
		return true;
	}

	std::string want = normalizeClassName(parent);

	if (normalizeClassName(child) == want) {
		return true;
	}

	const Class* cls = FindClass(child);
	int depth = 0;

	while (cls && depth++ < 64) {
		if (normalizeClassName(cls->name) == want) {
			return true;
		}

		cls = FindClass(cls->parentName);
	}

	return false;
}


//
//	BlueprintEditor::BlueprintEditor
//

BlueprintEditor::BlueprintEditor() {
	undoStack.push_back(serializeGraph(false, GRAPH_MAGIC));
	undoIndex = 0;
}


//
//	BlueprintEditor::SetupDefaultRegistry
//

void BlueprintEditor::SetupDefaultRegistry() {
	registry.Clear();

	registry.AddEnum("EEndPlayReason", {"Destroyed", "Level Transition", "End Play In Editor", "Removed From World", "Quit"});

	auto& object = registry.AddClass("UObject", "", "The base class of all Unreal objects");
	object.AddFunction("Get Class", "Utilities").Pure().Ret(PinType(PinKind::Class, "UObject")).Tooltip("Returns the class of this object");
	object.AddFunction("Get Display Name", "Utilities").Pure().Ret(PinType(PinKind::String)).Tooltip("Returns the display name of this object");
	object.AddFunction("Is Valid", "Utilities").Pure().Keywords("null check exists").Ret(PinType(PinKind::Boolean)).Tooltip("Is this object valid (not null and not pending kill)?");

	auto& actor = registry.AddClass("AActor", "UObject", "An object that can be placed or spawned in a level");
	actor.AddEvent("BeginPlay").Tooltip("Event when play begins for this actor");
	actor.AddEvent("Tick").Out("Delta Seconds", PinType(PinKind::Float)).Tooltip("Event called every frame");
	actor.AddEvent("EndPlay").Out("End Play Reason", PinType(PinKind::Enum, "EEndPlayReason")).Tooltip("Event when this actor is being removed from a level");
	actor.AddEvent("ActorBeginOverlap").Out("Other Actor", PinType(PinKind::Object, "AActor")).Tooltip("Event when this actor overlaps another actor");
	actor.AddEvent("ActorEndOverlap").Out("Other Actor", PinType(PinKind::Object, "AActor")).Tooltip("Event when this actor stops overlapping another actor");
	actor.AddEvent("Destroyed").Tooltip("Event when this actor is destroyed");
	actor.AddFunction("Get Actor Location", "Transformation").Pure().Ret(PinType(PinKind::Vector)).Keywords("position");
	actor.AddFunction("Set Actor Location", "Transformation").In("New Location", PinType(PinKind::Vector)).Ret(PinType(PinKind::Boolean)).Keywords("position move");
	actor.AddFunction("Get Actor Rotation", "Transformation").Pure().Ret(PinType(PinKind::Rotator));
	actor.AddFunction("Set Actor Rotation", "Transformation").In("New Rotation", PinType(PinKind::Rotator)).Ret(PinType(PinKind::Boolean));
	actor.AddFunction("Get Actor Transform", "Transformation").Pure().Ret(PinType(PinKind::Transform));
	actor.AddFunction("Get Actor Forward Vector", "Transformation").Pure().Ret(PinType(PinKind::Vector)).Keywords("direction");
	actor.AddFunction("Add Actor World Offset", "Transformation").In("Delta Location", PinType(PinKind::Vector)).Keywords("move translate");
	actor.AddFunction("Set Actor Scale 3D", "Transformation").In("New Scale", PinType(PinKind::Vector, "", false));
	actor.AddFunction("Get Velocity", "Utilities").Pure().Ret(PinType(PinKind::Vector)).Keywords("speed");
	actor.AddFunction("Get Distance To", "Utilities").Pure().In("Other Actor", PinType(PinKind::Object, "AActor")).Ret(PinType(PinKind::Float));
	actor.AddFunction("Destroy Actor", "Utilities").Keywords("delete remove kill");
	actor.AddFunction("Set Actor Hidden In Game", "Rendering").In("New Hidden", PinType(PinKind::Boolean)).Keywords("visible visibility");
	actor.AddProperty("Root Component", PinType(PinKind::Object, "USceneComponent"), "Components");
	actor.AddProperty("Tags", PinType(PinKind::Name, "", true), "Actor");

	auto& controller = registry.AddClass("AController", "AActor", "An actor that controls a pawn");
	controller.AddFunction("Get Controlled Pawn", "Pawn").Pure().Ret(PinType(PinKind::Object, "APawn"));

	auto& pawn = registry.AddClass("APawn", "AActor", "An actor that can be possessed and controlled");
	pawn.AddEvent("Possessed").Out("New Controller", PinType(PinKind::Object, "AController"));
	pawn.AddFunction("Add Movement Input", "Input").In("World Direction", PinType(PinKind::Vector)).In("Scale Value", PinType(PinKind::Float), "1.0").Keywords("move walk");
	pawn.AddFunction("Get Controller", "Pawn").Pure().Ret(PinType(PinKind::Object, "AController"));
	pawn.AddFunction("Is Player Controlled", "Pawn").Pure().Ret(PinType(PinKind::Boolean));

	auto& character = registry.AddClass("ACharacter", "APawn", "A pawn with walking movement and a skeletal mesh");
	character.AddEvent("Landed").Out("Hit", PinType(PinKind::Struct, "HitResult"));
	character.AddFunction("Jump", "Character").Keywords("hop");
	character.AddFunction("Stop Jumping", "Character");
	character.AddFunction("Launch Character", "Character").In("Launch Velocity", PinType(PinKind::Vector)).In("XY Override", PinType(PinKind::Boolean)).In("Z Override", PinType(PinKind::Boolean)).Keywords("throw knockback");

	auto& component = registry.AddClass("UActorComponent", "UObject", "A reusable component that can be added to an actor");
	component.AddFunction("Activate", "Components");
	component.AddFunction("Deactivate", "Components");
	component.AddFunction("Is Active", "Components").Pure().Ret(PinType(PinKind::Boolean));

	auto& scene = registry.AddClass("USceneComponent", "UActorComponent", "A component with a transform that supports attachment");
	scene.AddFunction("Get World Location", "Transformation").Pure().Ret(PinType(PinKind::Vector));
	scene.AddFunction("Set World Location", "Transformation").In("New Location", PinType(PinKind::Vector));

	registry.AddClass("USoundBase", "UObject", "A playable sound asset");

	auto& math = registry.AddClass("UKismetMathLibrary", "UObject", "Math utilities");
	math.AddFunction("Add (Float)", "Math|Float").Pure().Static().Keywords("+ plus").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Subtract (Float)", "Math|Float").Pure().Static().Keywords("- minus").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Multiply (Float)", "Math|Float").Pure().Static().Keywords("* times").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Divide (Float)", "Math|Float").Pure().Static().Keywords("/").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float), "1.0").Ret(PinType(PinKind::Float));
	math.AddFunction("Clamp (Float)", "Math|Float").Pure().Static().In("Value", PinType(PinKind::Float)).In("Min", PinType(PinKind::Float), "0.0").In("Max", PinType(PinKind::Float), "1.0").Ret(PinType(PinKind::Float));
	math.AddFunction("Lerp (Float)", "Math|Float").Pure().Static().Keywords("interpolate blend").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).In("Alpha", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Abs (Float)", "Math|Float").Pure().Static().Keywords("absolute").In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Min (Float)", "Math|Float").Pure().Static().In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Max (Float)", "Math|Float").Pure().Static().In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Square Root", "Math|Float").Pure().Static().Keywords("sqrt").In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Sin (Radians)", "Math|Trig").Pure().Static().In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Cos (Radians)", "Math|Trig").Pure().Static().In("A", PinType(PinKind::Float)).Ret(PinType(PinKind::Float));
	math.AddFunction("Add (Integer)", "Math|Integer").Pure().Static().Keywords("+ plus").In("A", PinType(PinKind::Integer)).In("B", PinType(PinKind::Integer)).Ret(PinType(PinKind::Integer));
	math.AddFunction("Multiply (Integer)", "Math|Integer").Pure().Static().Keywords("* times").In("A", PinType(PinKind::Integer)).In("B", PinType(PinKind::Integer)).Ret(PinType(PinKind::Integer));
	math.AddFunction("Clamp (Integer)", "Math|Integer").Pure().Static().In("Value", PinType(PinKind::Integer)).In("Min", PinType(PinKind::Integer), "0").In("Max", PinType(PinKind::Integer), "100").Ret(PinType(PinKind::Integer));
	math.AddFunction("Greater (Float)", "Math|Comparison").Pure().Static().Keywords("> compare").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Boolean));
	math.AddFunction("Less (Float)", "Math|Comparison").Pure().Static().Keywords("< compare").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Boolean));
	math.AddFunction("Equal (Float)", "Math|Comparison").Pure().Static().Keywords("== compare").In("A", PinType(PinKind::Float)).In("B", PinType(PinKind::Float)).Ret(PinType(PinKind::Boolean));
	math.AddFunction("AND (Boolean)", "Math|Boolean").Pure().Static().Keywords("&& logic").In("A", PinType(PinKind::Boolean)).In("B", PinType(PinKind::Boolean)).Ret(PinType(PinKind::Boolean));
	math.AddFunction("OR (Boolean)", "Math|Boolean").Pure().Static().Keywords("|| logic").In("A", PinType(PinKind::Boolean)).In("B", PinType(PinKind::Boolean)).Ret(PinType(PinKind::Boolean));
	math.AddFunction("NOT (Boolean)", "Math|Boolean").Pure().Static().Keywords("! logic invert").In("A", PinType(PinKind::Boolean)).Ret(PinType(PinKind::Boolean));
	math.AddFunction("Make Vector", "Math|Vector").Pure().Static().In("X", PinType(PinKind::Float)).In("Y", PinType(PinKind::Float)).In("Z", PinType(PinKind::Float)).Ret(PinType(PinKind::Vector));
	math.AddFunction("Break Vector", "Math|Vector").Pure().Static().In("In Vec", PinType(PinKind::Vector)).Out("X", PinType(PinKind::Float)).Out("Y", PinType(PinKind::Float)).Out("Z", PinType(PinKind::Float));
	math.AddFunction("Vector Length", "Math|Vector").Pure().Static().Keywords("magnitude size").In("A", PinType(PinKind::Vector)).Ret(PinType(PinKind::Float));
	math.AddFunction("Normalize (Vector)", "Math|Vector").Pure().Static().Keywords("unit").In("A", PinType(PinKind::Vector)).Ret(PinType(PinKind::Vector));
	math.AddFunction("Dot Product", "Math|Vector").Pure().Static().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).Ret(PinType(PinKind::Float));
	math.AddFunction("Cross Product", "Math|Vector").Pure().Static().In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).Ret(PinType(PinKind::Vector));
	math.AddFunction("Distance (Vector)", "Math|Vector").Pure().Static().In("V1", PinType(PinKind::Vector)).In("V2", PinType(PinKind::Vector)).Ret(PinType(PinKind::Float));
	math.AddFunction("Vector * Float", "Math|Vector").Pure().Static().Keywords("scale multiply").In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Float), "1.0").Ret(PinType(PinKind::Vector));
	math.AddFunction("Lerp (Vector)", "Math|Vector").Pure().Static().Keywords("interpolate blend").In("A", PinType(PinKind::Vector)).In("B", PinType(PinKind::Vector)).In("Alpha", PinType(PinKind::Float)).Ret(PinType(PinKind::Vector));
	math.AddFunction("Make Rotator", "Math|Rotator").Pure().Static().In("Roll", PinType(PinKind::Float)).In("Pitch", PinType(PinKind::Float)).In("Yaw", PinType(PinKind::Float)).Ret(PinType(PinKind::Rotator));
	math.AddFunction("Break Rotator", "Math|Rotator").Pure().Static().In("In Rot", PinType(PinKind::Rotator)).Out("Roll", PinType(PinKind::Float)).Out("Pitch", PinType(PinKind::Float)).Out("Yaw", PinType(PinKind::Float));
	math.AddFunction("Random Float In Range", "Math|Random").Pure().Static().In("Min", PinType(PinKind::Float), "0.0").In("Max", PinType(PinKind::Float), "1.0").Ret(PinType(PinKind::Float));
	math.AddFunction("Random Integer In Range", "Math|Random").Pure().Static().In("Min", PinType(PinKind::Integer), "0").In("Max", PinType(PinKind::Integer), "100").Ret(PinType(PinKind::Integer));
	math.AddFunction("Random Bool", "Math|Random").Pure().Static().Keywords("coin flip").Ret(PinType(PinKind::Boolean));
	math.AddFunction("Random Unit Vector", "Math|Random").Pure().Static().Ret(PinType(PinKind::Vector));

	auto& strings = registry.AddClass("UKismetStringLibrary", "UObject", "String utilities");
	strings.AddFunction("Append", "Utilities|String").Pure().Static().Keywords("concat combine +").In("A", PinType(PinKind::String)).In("B", PinType(PinKind::String)).Ret(PinType(PinKind::String));
	strings.AddFunction("String Length", "Utilities|String").Pure().Static().In("Source String", PinType(PinKind::String)).Ret(PinType(PinKind::Integer));
	strings.AddFunction("Contains", "Utilities|String").Pure().Static().Keywords("find search").In("Search In", PinType(PinKind::String)).In("Substring", PinType(PinKind::String)).Ret(PinType(PinKind::Boolean));
	strings.AddFunction("To Upper", "Utilities|String").Pure().Static().In("Source String", PinType(PinKind::String)).Ret(PinType(PinKind::String));
	strings.AddFunction("To Lower", "Utilities|String").Pure().Static().In("Source String", PinType(PinKind::String)).Ret(PinType(PinKind::String));
	strings.AddFunction("To String (Float)", "Utilities|String").Pure().Static().Keywords("convert cast").In("In Float", PinType(PinKind::Float)).Ret(PinType(PinKind::String));
	strings.AddFunction("To String (Integer)", "Utilities|String").Pure().Static().Keywords("convert cast").In("In Int", PinType(PinKind::Integer)).Ret(PinType(PinKind::String));
	strings.AddFunction("To String (Boolean)", "Utilities|String").Pure().Static().Keywords("convert cast").In("In Bool", PinType(PinKind::Boolean)).Ret(PinType(PinKind::String));
	strings.AddFunction("To String (Vector)", "Utilities|String").Pure().Static().Keywords("convert cast").In("In Vec", PinType(PinKind::Vector)).Ret(PinType(PinKind::String));

	auto& system = registry.AddClass("UKismetSystemLibrary", "UObject", "System utilities");
	system.AddFunction("Print String", "Development").Static().Keywords("log debug output text").In("In String", PinType(PinKind::String), "Hello").In("Print to Screen", PinType(PinKind::Boolean), "true").In("Print to Log", PinType(PinKind::Boolean), "true").In("Duration", PinType(PinKind::Float), "2.0").Tooltip("Prints a string to the log and optionally the screen");
	system.AddFunction("Delay", "Utilities|Flow Control").Static().Keywords("wait sleep latent").In("Duration", PinType(PinKind::Float), "0.2").Tooltip("Perform a latent action with a delay");
	system.AddFunction("Quit Game", "Game").Static().Keywords("exit close");
	system.AddFunction("Draw Debug Line", "Debug").Static().In("Line Start", PinType(PinKind::Vector)).In("Line End", PinType(PinKind::Vector)).In("Duration", PinType(PinKind::Float), "0.0");
	system.AddFunction("Get Game Time In Seconds", "Utilities|Time").Pure().Static().Ret(PinType(PinKind::Float));

	auto& gameplay = registry.AddClass("UGameplayStatics", "UObject", "Static gameplay utilities");
	gameplay.AddFunction("Get Player Pawn", "Game").Pure().Static().In("Player Index", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Object, "APawn"));
	gameplay.AddFunction("Get Player Character", "Game").Pure().Static().In("Player Index", PinType(PinKind::Integer), "0").Ret(PinType(PinKind::Object, "ACharacter"));
	gameplay.AddFunction("Get All Actors Of Class", "Game").Static().In("Actor Class", PinType(PinKind::Class, "AActor")).Out("Out Actors", PinType(PinKind::Object, "AActor", true));
	gameplay.AddFunction("Spawn Actor From Class", "Game").Static().Keywords("create instantiate").In("Class", PinType(PinKind::Class, "AActor")).In("Spawn Transform", PinType(PinKind::Transform)).Ret(PinType(PinKind::Object, "AActor"));
	gameplay.AddFunction("Set Game Paused", "Game").Static().In("Paused", PinType(PinKind::Boolean), "true").Ret(PinType(PinKind::Boolean));
	gameplay.AddFunction("Open Level", "Game").Static().Keywords("map load").In("Level Name", PinType(PinKind::Name));
	gameplay.AddFunction("Play Sound 2D", "Audio").Static().In("Sound", PinType(PinKind::Object, "USoundBase")).In("Volume Multiplier", PinType(PinKind::Float), "1.0");
}


//
//	BlueprintEditor::SetBlueprint
//

void BlueprintEditor::SetBlueprint(const std::string& name, const std::string& parentClass) {
	blueprintName = name;
	blueprintParentClass = parentClass;
}


//
//	BlueprintEditor variable management
//

bool BlueprintEditor::AddVariable(const std::string& name, const PinType& type, const std::string& defaultValue) {
	if (name.empty() || findVariable(name)) {
		return false;
	}

	variables.push_back(Variable{name, type, defaultValue});
	recordUndo();
	dirty = true;
	return true;
}

bool BlueprintEditor::RemoveVariable(const std::string& name) {
	for (auto i = variables.begin(); i != variables.end(); i++) {
		if (i->name == name) {
			variables.erase(i);
			recordUndo();
			dirty = true;
			return true;
		}
	}

	return false;
}

bool BlueprintEditor::RenameVariable(const std::string& oldName, const std::string& newName) {
	if (newName.empty() || oldName == newName || findVariable(newName)) {
		return false;
	}

	Variable* target = nullptr;

	for (auto& variable : variables) {
		if (variable.name == oldName) {
			target = &variable;
			break;
		}
	}

	if (!target) {
		return false;
	}

	target->name = newName;

	for (auto& node : nodes) {
		if ((node.kind == NodeKind::VariableGet || node.kind == NodeKind::VariableSet) && node.memberName == oldName) {
			node.memberName = newName;
			node.title = newName;
		}
	}

	recordUndo();
	dirty = true;
	return true;
}

ImVec2 BlueprintEditor::NextSpawnPos() {
	// top-left of the visible canvas (canvasPos is set during Render), cascaded so
	// repeated sidebar clicks fan out instead of stacking.
	ImVec2 pos = screenToGraph(ImVec2(canvasPos.x + 40.0f, canvasPos.y + 40.0f));
	pos.x += static_cast<float>(spawnCascade) * 28.0f;
	pos.y += static_cast<float>(spawnCascade) * 28.0f;
	spawnCascade = (spawnCascade + 1) % 8;
	return pos;
}

const BlueprintEditor::Variable* BlueprintEditor::findVariable(const std::string& name) const {
	for (auto& variable : variables) {
		if (variable.name == name) {
			return &variable;
		}
	}

	return nullptr;
}


//
//	BlueprintEditor lookup helpers
//

void BlueprintEditor::rebuildIndex() {
	nodeIndex.clear();
	pinIndex.clear();

	for (size_t n = 0; n < nodes.size(); n++) {
		nodeIndex[nodes[n].id] = n;

		for (size_t p = 0; p < nodes[n].pins.size(); p++) {
			pinIndex[nodes[n].pins[p].id] = std::make_pair(n, p);
		}
	}
}

BlueprintEditor::Node* BlueprintEditor::findNode(ID id) {
	auto i = nodeIndex.find(id);
	return (i == nodeIndex.end()) ? nullptr : &nodes[i->second];
}

const BlueprintEditor::Node* BlueprintEditor::findNode(ID id) const {
	auto i = nodeIndex.find(id);
	return (i == nodeIndex.end()) ? nullptr : &nodes[i->second];
}

BlueprintEditor::Pin* BlueprintEditor::findPin(ID id) {
	auto i = pinIndex.find(id);
	return (i == pinIndex.end()) ? nullptr : &nodes[i->second.first].pins[i->second.second];
}

const BlueprintEditor::Pin* BlueprintEditor::findPin(ID id) const {
	auto i = pinIndex.find(id);
	return (i == pinIndex.end()) ? nullptr : &nodes[i->second.first].pins[i->second.second];
}

const BlueprintEditor::Link* BlueprintEditor::findLink(ID id) const {
	for (auto& link : links) {
		if (link.id == id) {
			return &link;
		}
	}

	return nullptr;
}


//
//	BlueprintEditor node construction
//

BlueprintEditor::Node& BlueprintEditor::createNode(NodeKind kind, const ImVec2& pos) {
	nodes.push_back(Node());
	Node& node = nodes.back();
	node.id = makeID();
	node.kind = kind;
	node.pos = pos;
	return node;
}

void BlueprintEditor::addPin(Node& node, const std::string& name, const PinType& type, bool isOutput, const std::string& defaultValue) {
	Pin pin;
	pin.id = makeID();
	pin.node = node.id;
	pin.name = name;
	pin.type = type;
	pin.isOutput = isOutput;
	pin.defaultValue = defaultValue;
	node.pins.push_back(pin);
}

void BlueprintEditor::buildFunctionPins(Node& node, const Function& function, const std::string& className) {
	node.pure = function.isPure;

	if (!function.isPure) {
		addPin(node, "", PinType(PinKind::Exec), false);
		addPin(node, "", PinType(PinKind::Exec), true);
	}

	if (!function.isStatic) {
		addPin(node, "Target", PinType(PinKind::Object, className), false, "self");
	}

	for (auto& parameter : function.parameters) {
		addPin(node, parameter.name, parameter.type, parameter.isOutput, parameter.defaultValue);
	}
}

BlueprintEditor::ID BlueprintEditor::finishNode(Node& node) {
	ID id = node.id;
	rebuildIndex();
	recordUndo();
	dirty = true;
	return id;
}

BlueprintEditor::ID BlueprintEditor::AddEventNode(const std::string& className, const std::string& eventName, const ImVec2& pos) {
	// events are unique: return the existing node if it was already placed
	for (auto& node : nodes) {
		if (node.kind == NodeKind::Event && node.className == className && node.memberName == eventName) {
			return node.id;
		}
	}

	const Function* event = registry.FindEvent(className, eventName);

	if (!event) {
		return 0;
	}

	Node& node = createNode(NodeKind::Event, pos);
	node.className = className;
	node.memberName = eventName;
	node.title = "Event " + eventName;
	addPin(node, "", PinType(PinKind::Exec), true);

	for (auto& parameter : event->parameters) {
		addPin(node, parameter.name, parameter.type, true);
	}

	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddCustomEventNode(const std::string& name, const ImVec2& pos) {
	std::string eventName = name;

	if (eventName.empty()) {
		// generate a unique name
		int counter = 0;
		bool unique = false;

		while (!unique) {
			eventName = "CustomEvent_" + std::to_string(counter++);
			unique = true;

			for (auto& node : nodes) {
				if (node.kind == NodeKind::CustomEvent && node.memberName == eventName) {
					unique = false;
				}
			}
		}
	}

	Node& node = createNode(NodeKind::CustomEvent, pos);
	node.memberName = eventName;
	node.title = eventName;
	node.subtitle = "Custom Event";
	addPin(node, "", PinType(PinKind::Exec), true);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddCallFunctionNode(const std::string& className, const std::string& functionName, const ImVec2& pos) {
	const Function* function = registry.FindFunction(className, functionName);

	if (!function) {
		return 0;
	}

	Node& node = createNode(NodeKind::CallFunction, pos);
	node.className = className;
	node.memberName = functionName;
	node.title = functionName;

	if (function->isStatic) {
		const Class* cls = registry.FindClass(className);
		node.subtitle = "Target is " + (cls ? cls->name : className);
	}

	buildFunctionPins(node, *function, className);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddPropertyGetNode(const std::string& className, const std::string& propertyName, const ImVec2& pos) {
	const Property* property = registry.FindProperty(className, propertyName);

	if (!property) {
		return 0;
	}

	Node& node = createNode(NodeKind::CallFunction, pos);
	node.className = className;
	node.memberName = propertyName;
	node.title = "Get " + propertyName;
	node.pure = true;
	addPin(node, "Target", PinType(PinKind::Object, className), false, "self");
	addPin(node, propertyName, property->type, true);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddPropertySetNode(const std::string& className, const std::string& propertyName, const ImVec2& pos) {
	const Property* property = registry.FindProperty(className, propertyName);

	if (!property) {
		return 0;
	}

	Node& node = createNode(NodeKind::CallFunction, pos);
	node.className = className;
	node.memberName = propertyName;
	node.title = "Set " + propertyName;
	addPin(node, "", PinType(PinKind::Exec), false);
	addPin(node, "", PinType(PinKind::Exec), true);
	addPin(node, "Target", PinType(PinKind::Object, className), false, "self");
	addPin(node, propertyName, property->type, false);
	addPin(node, propertyName, property->type, true);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddMakeStructNode(const std::string& structName, const ImVec2& pos) {
	const Class* cls = registry.FindClass(structName);

	if (!cls) {
		return 0;
	}

	// A pure node: memberName is the "$MakeStruct" sentinel the code generator keys
	// on (see BlueprintLua.cpp callExpression). One input pin per property, one
	// output pin carrying the struct itself.
	Node& node = createNode(NodeKind::CallFunction, pos);
	node.className = structName;
	node.memberName = "$MakeStruct";
	node.title = "Make " + structName;
	node.pure = true;

	for (auto& property : cls->properties) {
		addPin(node, property.name, property.type, false);
	}

	addPin(node, structName, PinType(PinKind::Struct, structName), true);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddVariableGetNode(const std::string& variableName, const ImVec2& pos) {
	const Variable* variable = findVariable(variableName);

	if (!variable) {
		return 0;
	}

	Node& node = createNode(NodeKind::VariableGet, pos);
	node.memberName = variableName;
	node.title = variableName;
	addPin(node, "", variable->type, true);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddVariableSetNode(const std::string& variableName, const ImVec2& pos) {
	const Variable* variable = findVariable(variableName);

	if (!variable) {
		return 0;
	}

	Node& node = createNode(NodeKind::VariableSet, pos);
	node.memberName = variableName;
	node.title = "Set " + variableName;
	addPin(node, "", PinType(PinKind::Exec), false);
	addPin(node, "", PinType(PinKind::Exec), true);
	addPin(node, variableName, variable->type, false, variable->defaultValue);
	addPin(node, variableName, variable->type, true);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddVariableSetIfUnsetNode(const std::string& variableName, const ImVec2& pos) {
	// The lazy-init idiom: codegen emits `X = X or (value)`, so the value expression
	// only evaluates while X is still nil (e.g. find_uobject runs once, not per tick).
	ID id = AddVariableSetNode(variableName, pos);

	if (Node* node = findNode(id)) {
		node->title = "Set " + variableName + " (if unset)";
		node->subtitle = "X = X or value";
		node->customCode = "or"; // codegen marker, serialized with the node
	}

	return id;
}

BlueprintEditor::ID BlueprintEditor::AddFlowControlNode(const std::string& name, const ImVec2& pos) {
	for (auto& def : flowDefinitions()) {
		if (name == def.name) {
			Node& node = createNode(NodeKind::FlowControl, pos);
			node.memberName = def.name;
			node.title = def.name;

			for (auto& pinDef : def.pins) {
				addPin(node, pinDef.name, PinType(pinDef.kind), pinDef.isOutput, pinDef.defaultValue);
			}

			return finishNode(node);
		}
	}

	return 0;
}

BlueprintEditor::ID BlueprintEditor::AddRerouteNode(const PinType& type, const ImVec2& pos) {
	Node& node = createNode(NodeKind::Reroute, pos);
	node.memberName = "Reroute";
	addPin(node, "", type, false);
	addPin(node, "", type, true);
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddCommentNode(const std::string& text, const ImVec2& pos, const ImVec2& size) {
	Node& node = createNode(NodeKind::Comment, pos);
	node.title = text.empty() ? "Comment" : text;
	node.commentSize = ImVec2(std::max(size.x, 120.0f), std::max(size.y, 80.0f));
	return finishNode(node);
}

BlueprintEditor::ID BlueprintEditor::AddCustomLuaNode(const ImVec2& pos) {
	Node& node = createNode(NodeKind::CustomLua, pos);
	node.title = "Custom Lua";
	node.customCode = "-- custom lua\n";
	node.commentSize = ImVec2(260.0f, 160.0f);
	addPin(node, "", PinType(PinKind::Exec), false);
	addPin(node, "", PinType(PinKind::Exec), true);
	return finishNode(node);
}

void BlueprintEditor::ensureClassAvailable(const std::string& className) {
	if (className.empty() || !auxRegistry) {
		return;
	}

	// EXACT presence check — not FindClass, which prefix-folds and would treat the
	// built-in "AActor" as the dumped "Actor" and skip pulling its (many) members.
	// The two coexist: AActor (built-in, visible) + Actor (dumped, hidden); the
	// prefix fold in IsChildOf/typesCompatible lets them interoperate.
	for (auto& c : registry.GetClasses()) {
		if (c->name == className) {
			return;
		}
	}

	const Class* src = auxRegistry->FindClass(className);

	if (!src) {
		return;
	}

	// Pull ancestors first so the parent chain (and inherited members) resolves.
	if (!src->parentName.empty()) {
		ensureClassAvailable(src->parentName);
	}

	Class& dst = registry.AddClass(src->name, src->parentName, src->tooltip);
	dst.metadata = src->metadata;
	dst.properties = src->properties;
	dst.functions = src->functions;
	dst.events = src->events;
	dst.paletteHidden = true; // usable via Cast / contextual drag, hidden from All Actions
}

BlueprintEditor::ID BlueprintEditor::AddCastNode(const std::string& className, const ImVec2& pos) {
	if (className.empty()) {
		return 0;
	}

	ensureClassAvailable(className); // so the output's class is a real, spawnable type

	Node& node = createNode(NodeKind::Cast, pos);
	node.className = className;
	node.memberName = "Cast";
	node.title = "Cast To " + className;
	node.subtitle = "is_a(\"" + className + "\")";
	node.pure = true;

	addPin(node, "Object", PinType(PinKind::Object, "UObject"), false);
	addPin(node, "As " + className, PinType(PinKind::Object, className), true);
	return finishNode(node);
}

bool BlueprintEditor::SetCustomLuaSource(ID nodeID, const std::string& source) {
	Node* node = findNode(nodeID);

	if (!node || node->kind != NodeKind::CustomLua) {
		return false;
	}

	node->customCode = source;
	recordUndo();
	dirty = true;
	return true;
}

BlueprintEditor::ID BlueprintEditor::AddCustomLuaPin(ID nodeID, bool isOutput, const std::string& name) {
	Node* node = findNode(nodeID);

	if (!node || node->kind != NodeKind::CustomLua) {
		return 0;
	}

	std::string pinName = name;

	if (pinName.empty()) {
		int count = 0;

		for (auto& pin : node->pins) {
			if (pin.type.kind != PinKind::Exec && pin.isOutput == isOutput) {
				count++;
			}
		}

		pinName = (isOutput ? "Out" : "In") + std::to_string(count);
	}

	addPin(*node, pinName, PinType(PinKind::Wildcard), isOutput);
	ID pinID = node->pins.back().id;
	rebuildIndex();
	recordUndo();
	dirty = true;
	return pinID;
}

bool BlueprintEditor::RemoveCustomLuaPin(ID nodeID, ID pinID) {
	Node* node = findNode(nodeID);

	if (!node || node->kind != NodeKind::CustomLua) {
		return false;
	}

	auto it = std::find_if(node->pins.begin(), node->pins.end(), [pinID](const Pin& p) { return p.id == pinID; });

	if (it == node->pins.end() || it->type.kind == PinKind::Exec) {
		return false; // exec pins are fixed, not dynamic
	}

	breakPinLinksInternal(pinID);
	node->pins.erase(it);
	rebuildIndex();
	recordUndo();
	dirty = true;
	return true;
}

BlueprintEditor::ID BlueprintEditor::FindPinID(ID node, const std::string& pinName, bool isOutput) const {
	const Node* n = findNode(node);

	if (n) {
		for (auto& pin : n->pins) {
			if (pin.isOutput == isOutput && pin.name == pinName) {
				return pin.id;
			}
		}
	}

	return 0;
}

bool BlueprintEditor::SetPinDefaultValue(ID pinID, const std::string& value) {
	Pin* pin = findPin(pinID);

	if (!pin) {
		return false;
	}

	pin->defaultValue = value;
	recordUndo();
	dirty = true;
	return true;
}


//
//	BlueprintEditor connection management
//

int BlueprintEditor::pinLinkCount(ID pin) const {
	int count = 0;

	for (auto& link : links) {
		if (link.from == pin || link.to == pin) {
			count++;
		}
	}

	return count;
}

bool BlueprintEditor::wouldCreateDataCycle(ID fromNode, ID toNode) const {
	// follow data links downstream from "toNode" and see if we reach "fromNode"
	std::vector<ID> queue = {toNode};
	std::unordered_set<ID> visited;

	while (!queue.empty()) {
		ID current = queue.back();
		queue.pop_back();

		if (current == fromNode) {
			return true;
		}

		if (visited.insert(current).second) {
			for (auto& link : links) {
				const Pin* from = findPin(link.from);
				const Pin* to = findPin(link.to);

				if (from && to && from->type.kind != PinKind::Exec && from->node == current) {
					queue.push_back(to->node);
				}
			}
		}
	}

	return false;
}

bool BlueprintEditor::typesCompatible(const PinType& from, const PinType& to) const {
	return typesCompatible(from, to, true);
}

// A "class token" is anything that carries a UClass at runtime. UEVR's API is
// inconsistent about how it types these — some pins are PinKind::Class, others are
// PinKind::Object with subtype "UClass"/"UStruct" (e.g. As Class's return, Spawn
// Object's input, and every method defined ON the UClass reflection class, whose
// self pin is Object "UClass"). Treat them all as one family so a class value
// plugs into any class input. (Without this, dragging a class into a node like
// "Get Objects Matching" skipped the real class/Target pin and fell through to the
// bool "Allow Default" — the reported bug.)
bool BlueprintEditor::isClassLike(const PinType& t) const {
	if (t.kind == PinKind::Class) {
		return true;
	}

	if (t.kind == PinKind::Object) {
		return t.subtype == "UClass" || t.subtype == "UStruct" ||
		       registry.IsChildOf(t.subtype, "UClass") || registry.IsChildOf(t.subtype, "UStruct");
	}

	return false;
}

bool BlueprintEditor::typesCompatible(const PinType& from, const PinType& to, bool allowTruthiness) const {
	if (from.isArray != to.isArray) {
		// A Wildcard pin is "any Lua value" — array-ness is advisory on it, so a
		// Table (array) variable wires into For Each's Wildcard "Array" pin. The
		// strict array/scalar split only applies between two TYPED pins.
		if (from.kind != PinKind::Wildcard && to.kind != PinKind::Wildcard) {
			return false;
		}
	}

	if (from.kind == PinKind::Exec || to.kind == PinKind::Exec) {
		return from.kind == to.kind;
	}

	if (from.kind == PinKind::Wildcard || to.kind == PinKind::Wildcard) {
		return true;
	}

	// Class tokens are interchangeable regardless of Class-vs-Object encoding (see
	// isClassLike). Checked before the plain Object rule so Object "UClass" reaches
	// a PinKind::Class input and vice versa.
	if (isClassLike(from) && isClassLike(to)) {
		return true;
	}

	// A class token IS-A UObject, so it may feed a generic object input whose type is
	// an ancestor of UClass (typically UObject). Not the reverse — an instance is not
	// a class — so this is one-directional.
	if (isClassLike(from) && to.kind == PinKind::Object && registry.IsChildOf("UClass", to.subtype)) {
		return true;
	}

	if (from.kind == PinKind::Object && to.kind == PinKind::Object) {
		return registry.IsChildOf(from.subtype, to.subtype);
	}

	if (from.kind == PinKind::Class && to.kind == PinKind::Class) {
		return registry.IsChildOf(from.subtype, to.subtype);
	}

	if (from.kind == PinKind::Struct && to.kind == PinKind::Struct) {
		return from.subtype == to.subtype;
	}

	if (from.kind == PinKind::Enum && to.kind == PinKind::Enum) {
		return from.subtype == to.subtype;
	}

	// implicit numeric promotions
	if (!from.isArray) {
		if (from.kind == PinKind::Byte && (to.kind == PinKind::Integer || to.kind == PinKind::Float)) {
			return true;
		}

		if (from.kind == PinKind::Integer && to.kind == PinKind::Float) {
			return true;
		}
	}

	// Lua truthiness: `if <anything>` is valid Lua (nil/false are falsy, everything
	// else truthy), so a Boolean INPUT (Branch/While conditions, bool params) accepts
	// any data connection — wire an Object straight into a Branch to nil-check it.
	// Suppressible: auto-connect-on-drop runs a strong-match pass FIRST (truthiness
	// off) so a dragged value lands on a real type match instead of an incidental
	// bool input, then a permissive pass — see the spawn-from-drag sites.
	if (allowTruthiness && to.kind == PinKind::Boolean) {
		return true;
	}

	return from.kind == to.kind;
}

bool BlueprintEditor::canConnect(ID a, ID b, std::string& error, bool strongOnly) const {
	const Pin* pa = findPin(a);
	const Pin* pb = findPin(b);

	if (!pa || !pb) {
		error = "Invalid pin";
		return false;
	}

	if (pa->node == pb->node) {
		error = "Both pins are on the same node";
		return false;
	}

	if (pa->isOutput == pb->isOutput) {
		error = "Directions are not compatible";
		return false;
	}

	const Pin* from = pa->isOutput ? pa : pb;
	const Pin* to = pa->isOutput ? pb : pa;

	// strongOnly suppresses the Lua-truthiness catch-all (anything -> bool) so a
	// caller can prefer a real type match; see the auto-connect-on-drop two-pass.
	if (!typesCompatible(from->type, to->type, !strongOnly)) {
		error = pinTypeLabel(from->type) + " is not compatible with " + pinTypeLabel(to->type);
		return false;
	}

	if (from->type.kind != PinKind::Exec && to->type.kind != PinKind::Exec && wouldCreateDataCycle(from->node, to->node)) {
		error = "Connection would create a cycle";
		return false;
	}

	return true;
}

BlueprintEditor::ID BlueprintEditor::AutoConnectForTest(ID pendingPin, ID node) {
	Node* n = findNode(node);

	if (!n || !autoConnectPendingToNode(pendingPin, *n)) {
		return 0;
	}

	for (auto& link : links) {
		if (link.from == pendingPin) {
			const Pin* to = findPin(link.to);

			if (to && to->node == node) {
				return link.to;
			}
		}
	}

	return 0;
}

bool BlueprintEditor::autoConnectPendingToNode(ID pendingPin, Node& node) {
	// Two passes: strong match first (real types, no truthiness), then permissive.
	// So dragging e.g. a class output onto a node lands on its class/Target pin
	// rather than an incidental bool input that Lua-truthiness would also accept.
	for (int pass = 0; pass < 2; ++pass) {
		bool strongOnly = pass == 0;

		for (auto& pin : node.pins) {
			std::string error;

			if (canConnect(pendingPin, pin.id, error, strongOnly)) {
				bool made = connect(pendingPin, pin.id);

				if (made) {
					recordUndo();
				}

				return made;
			}
		}
	}

	return false;
}

bool BlueprintEditor::connect(ID fromPin, ID toPin) {
	Pin* pa = findPin(fromPin);
	Pin* pb = findPin(toPin);

	if (!pa || !pb || pa->isOutput == pb->isOutput) {
		return false;
	}

	Pin* from = pa->isOutput ? pa : pb;
	Pin* to = pa->isOutput ? pb : pa;

	// ignore duplicate connections
	for (auto& link : links) {
		if (link.from == from->id && link.to == to->id) {
			return false;
		}
	}

	// like Unreal: an exec output drives one target, a data input has one source;
	// connecting to a full pin replaces the existing connection
	if (from->type.kind == PinKind::Exec) {
		links.erase(std::remove_if(links.begin(), links.end(), [from](const Link& link) { return link.from == from->id; }), links.end());

	} else {
		ID toID = to->id;
		links.erase(std::remove_if(links.begin(), links.end(), [toID](const Link& link) { return link.to == toID; }), links.end());
	}

	Link link;
	link.id = makeID();
	link.from = from->id;
	link.to = to->id;
	links.push_back(link);
	return true;
}

void BlueprintEditor::breakLinkInternal(ID link) {
	links.erase(std::remove_if(links.begin(), links.end(), [link](const Link& l) { return l.id == link; }), links.end());
	selectedLinks.erase(link);
}

void BlueprintEditor::breakPinLinksInternal(ID pin) {
	for (auto& link : links) {
		if (link.from == pin || link.to == pin) {
			selectedLinks.erase(link.id);
		}
	}

	links.erase(std::remove_if(links.begin(), links.end(), [pin](const Link& l) { return l.from == pin || l.to == pin; }), links.end());
}

void BlueprintEditor::removeNodeInternal(ID node) {
	Node* n = findNode(node);

	if (!n) {
		return;
	}

	for (auto& pin : n->pins) {
		breakPinLinksInternal(pin.id);
	}

	selectedNodes.erase(node);
	nodes.erase(nodes.begin() + static_cast<ptrdiff_t>(nodeIndex[node]));
	rebuildIndex();
}

bool BlueprintEditor::AddLink(ID fromPin, ID toPin) {
	std::string error;

	if (!canConnect(fromPin, toPin, error)) {
		return false;
	}

	if (connect(fromPin, toPin)) {
		recordUndo();
		dirty = true;
		return true;
	}

	return false;
}

void BlueprintEditor::BreakLink(ID link) {
	breakLinkInternal(link);
	recordUndo();
	dirty = true;
}

void BlueprintEditor::BreakPinLinks(ID pin) {
	breakPinLinksInternal(pin);
	recordUndo();
	dirty = true;
}


//
//	BlueprintEditor edit operations
//

void BlueprintEditor::DeleteNode(ID node) {
	removeNodeInternal(node);
	recordUndo();
	dirty = true;
}

void BlueprintEditor::DeleteSelected() {
	if (!HasSelection()) {
		return;
	}

	std::vector<ID> linkIDs(selectedLinks.begin(), selectedLinks.end());

	for (auto id : linkIDs) {
		breakLinkInternal(id);
	}

	std::vector<ID> nodeIDs(selectedNodes.begin(), selectedNodes.end());

	for (auto id : nodeIDs) {
		removeNodeInternal(id);
	}

	selectedNodes.clear();
	selectedLinks.clear();
	recordUndo();
	dirty = true;
}

void BlueprintEditor::ClearGraph() {
	nodes.clear();
	links.clear();
	selectedNodes.clear();
	selectedLinks.clear();
	rebuildIndex();
	recordUndo();
	dirty = true;
}

void BlueprintEditor::SelectAll() {
	selectedNodes.clear();
	selectedLinks.clear();

	for (auto& node : nodes) {
		selectedNodes.insert(node.id);
	}

	for (auto& link : links) {
		selectedLinks.insert(link.id);
	}
}

void BlueprintEditor::ClearSelection() {
	selectedNodes.clear();
	selectedLinks.clear();
}

void BlueprintEditor::SelectNode(ID node, bool append) {
	if (!append) {
		ClearSelection();
	}

	if (findNode(node)) {
		selectedNodes.insert(node);
	}
}

std::vector<BlueprintEditor::ID> BlueprintEditor::GetSelectedNodes() const {
	return std::vector<ID>(selectedNodes.begin(), selectedNodes.end());
}


//
//	BlueprintEditor clipboard operations
//

std::string BlueprintEditor::copySelectionToText() const {
	return serializeGraph(true, CLIP_MAGIC);
}

void BlueprintEditor::Cut() {
	if (!selectedNodes.empty()) {
		Copy();
		DeleteSelected();
	}
}

void BlueprintEditor::Copy() {
	if (!selectedNodes.empty()) {
		ImGui::SetClipboardText(copySelectionToText().c_str());
	}
}

void BlueprintEditor::Paste() {
	const char* text = ImGui::GetClipboardText();

	if (text) {
		ImVec2 target;

		if (ImGui::IsMousePosValid() && zoom > 0.0f) {
			target = screenToGraph(ImGui::GetMousePos());

		} else {
			target = screenToGraph(ImVec2(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f));
		}

		pasteText(text, target);
	}
}

void BlueprintEditor::Duplicate() {
	if (!selectedNodes.empty()) {
		// determine the center of the selection
		ImVec2 minPos(FLT_MAX, FLT_MAX);
		ImVec2 maxPos(-FLT_MAX, -FLT_MAX);

		for (auto id : selectedNodes) {
			const Node* node = findNode(id);

			if (node) {
				minPos = vecMin(minPos, node->pos);
				maxPos = vecMax(maxPos, node->pos + node->size);
			}
		}

		ImVec2 center = (minPos + maxPos) * 0.5f;
		pasteText(copySelectionToText(), center + ImVec2(48.0f, 48.0f));
	}
}

void BlueprintEditor::pasteText(const std::string& text, const ImVec2& graphPos) {
	ParsedGraph parsed;

	if (!parseGraphText(text, CLIP_MAGIC, parsed) || parsed.nodes.empty()) {
		return;
	}

	// remap all IDs to fresh ones
	std::unordered_map<ID, ID> remap;

	for (auto& node : parsed.nodes) {
		ID id = makeID();
		remap[node.id] = id;
		node.id = id;

		for (auto& pin : node.pins) {
			ID pinID = makeID();
			remap[pin.id] = pinID;
			pin.id = pinID;
			pin.node = id;
		}
	}

	// center the pasted nodes around the target position
	ImVec2 minPos(FLT_MAX, FLT_MAX);
	ImVec2 maxPos(-FLT_MAX, -FLT_MAX);

	for (auto& node : parsed.nodes) {
		minPos = vecMin(minPos, node.pos);
		maxPos = vecMax(maxPos, node.pos);
	}

	ImVec2 offset = graphPos - (minPos + maxPos) * 0.5f;
	ClearSelection();

	for (auto& node : parsed.nodes) {
		node.pos = node.pos + offset;
		selectedNodes.insert(node.id);
		nodes.push_back(std::move(node));
	}

	for (auto& link : parsed.links) {
		auto from = remap.find(link.from);
		auto to = remap.find(link.to);

		if (from != remap.end() && to != remap.end()) {
			Link newLink;
			newLink.id = makeID();
			newLink.from = from->second;
			newLink.to = to->second;
			links.push_back(newLink);
		}
	}

	rebuildIndex();
	recordUndo();
	dirty = true;
}


//
//	BlueprintEditor undo/redo
//

void BlueprintEditor::recordUndo() {
	std::string state = serializeGraph(false, GRAPH_MAGIC);

	if (!undoStack.empty() && undoStack[undoIndex] == state) {
		return;
	}

	undoStack.resize(undoIndex + 1);
	undoStack.push_back(state);
	undoIndex++;

	if (undoStack.size() > MAX_UNDO) {
		undoStack.erase(undoStack.begin());
		undoIndex--;
	}
}

void BlueprintEditor::resetUndo() {
	undoStack.clear();
	undoStack.push_back(serializeGraph(false, GRAPH_MAGIC));
	undoIndex = 0;
}

void BlueprintEditor::Undo() {
	if (CanUndo()) {
		undoIndex--;
		applyState(undoStack[undoIndex]);
		dirty = true;
	}
}

void BlueprintEditor::Redo() {
	if (CanRedo()) {
		undoIndex++;
		applyState(undoStack[undoIndex]);
		dirty = true;
	}
}

void BlueprintEditor::applyState(const std::string& state) {
	ParsedGraph parsed;

	if (parseGraphText(state, GRAPH_MAGIC, parsed)) {
		installParsed(std::move(parsed), true);
	}
}


//
//	BlueprintEditor view control
//

void BlueprintEditor::ZoomToFit() {
	pendingZoomToFit = true;
}

void BlueprintEditor::SetZoom(float value) {
	zoom = std::clamp(value, MIN_ZOOM, MAX_ZOOM);
}

void BlueprintEditor::applyPendingViewChanges() {
	if (!pendingZoomToFit) {
		return;
	}

	pendingZoomToFit = false;

	if (nodes.empty()) {
		scrolling = ImVec2(canvasSize.x * 0.5f, canvasSize.y * 0.5f);
		zoom = 1.0f;
		return;
	}

	ImVec2 minPos(FLT_MAX, FLT_MAX);
	ImVec2 maxPos(-FLT_MAX, -FLT_MAX);

	for (auto& node : nodes) {
		ImVec2 size = node.kind == NodeKind::Comment ? node.commentSize : node.size;

		if (size.x <= 0.0f || size.y <= 0.0f) {
			size = ImVec2(220.0f, 120.0f);
		}

		minPos = vecMin(minPos, node.pos);
		maxPos = vecMax(maxPos, node.pos + size);
	}

	ImVec2 bounds = maxPos - minPos;
	float padding = 60.0f;
	float zx = canvasSize.x / (bounds.x + padding * 2.0f);
	float zy = canvasSize.y / (bounds.y + padding * 2.0f);
	zoom = std::clamp(std::min(zx, zy), MIN_ZOOM, 1.0f);

	ImVec2 center = (minPos + maxPos) * 0.5f;
	scrolling = ImVec2(canvasSize.x * 0.5f - center.x * zoom, canvasSize.y * 0.5f - center.y * zoom);
}


//
//	BlueprintEditor serialization
//

std::string BlueprintEditor::serializeGraph(bool selectionOnly, const char* magic) const {
	std::string out = std::string(magic) + " 1\n";
	char buffer[256];

	if (!selectionOnly) {
		out += "blueprint " + escapeString(blueprintName) + " " + escapeString(blueprintParentClass) + "\n";

		for (auto& variable : variables) {
			out += "variable " + escapeString(variable.name) + " " + pinKindName(variable.type.kind) + " " +
				escapeString(variable.type.subtype) + " " + (variable.type.isArray ? "1" : "0") + " " +
				escapeString(variable.defaultValue) + "\n";
		}
	}

	for (auto& node : nodes) {
		if (selectionOnly && !selectedNodes.count(node.id)) {
			continue;
		}

		std::snprintf(buffer, sizeof(buffer), "node %d %s ", node.id, nodeKindName(node.kind));
		out += buffer;
		out += escapeString(node.className) + " " + escapeString(node.memberName) + " " +
			escapeString(node.title) + " " + escapeString(node.subtitle);
		std::snprintf(buffer, sizeof(buffer), " %d %.3f %.3f %.3f %.3f\n",
			node.pure ? 1 : 0,
			static_cast<double>(node.pos.x), static_cast<double>(node.pos.y),
			static_cast<double>(node.commentSize.x), static_cast<double>(node.commentSize.y));
		out += buffer;

		// customCode: the CustomLua source, and small per-node markers on other kinds
		// (e.g. "or" on a Set-If-Unset VariableSet). The parser applies it regardless
		// of kind, so serializing whenever non-empty is backward compatible.
		if (!node.customCode.empty()) {
			out += "lua " + escapeString(node.customCode) + "\n";
		}

		for (auto& pin : node.pins) {
			std::snprintf(buffer, sizeof(buffer), "pin %d %s %s ", pin.id, pin.isOutput ? "out" : "in", pinKindName(pin.type.kind));
			out += buffer;
			out += escapeString(pin.type.subtype) + " " + (pin.type.isArray ? "1" : "0") + " " +
				escapeString(pin.name) + " " + escapeString(pin.defaultValue) + "\n";
		}
	}

	for (auto& link : links) {
		if (selectionOnly) {
			const Pin* from = findPin(link.from);
			const Pin* to = findPin(link.to);

			if (!from || !to || !selectedNodes.count(from->node) || !selectedNodes.count(to->node)) {
				continue;
			}
		}

		std::snprintf(buffer, sizeof(buffer), "link %d %d %d\n", link.id, link.from, link.to);
		out += buffer;
	}

	return out;
}

bool BlueprintEditor::parseGraphText(const std::string& text, const char* magic, ParsedGraph& result) const {
	size_t start = 0;
	bool first = true;
	std::vector<std::string> tokens;

	while (start <= text.size()) {
		size_t end = text.find('\n', start);

		if (end == std::string::npos) {
			end = text.size();
		}

		std::string line = text.substr(start, end - start);
		start = end + 1;

		if (!tokenizeLine(line, tokens)) {
			continue;
		}

		if (first) {
			if (tokens[0] != magic) {
				return false;
			}

			first = false;
			continue;
		}

		if (tokens[0] == "blueprint" && tokens.size() >= 3) {
			result.name = tokens[1];
			result.parent = tokens[2];

		} else if (tokens[0] == "variable" && tokens.size() >= 6) {
			Variable variable;
			variable.name = tokens[1];

			if (pinKindFromName(tokens[2], variable.type.kind)) {
				variable.type.subtype = tokens[3];
				variable.type.isArray = tokens[4] == "1";
				variable.defaultValue = tokens[5];
				result.variables.push_back(variable);
			}

		} else if (tokens[0] == "node" && tokens.size() >= 12) {
			Node node;
			node.id = std::atoi(tokens[1].c_str());

			if (nodeKindFromName(tokens[2], node.kind)) {
				node.className = tokens[3];
				node.memberName = tokens[4];
				node.title = tokens[5];
				node.subtitle = tokens[6];
				node.pure = tokens[7] == "1";
				node.pos.x = std::strtof(tokens[8].c_str(), nullptr);
				node.pos.y = std::strtof(tokens[9].c_str(), nullptr);
				node.commentSize.x = std::strtof(tokens[10].c_str(), nullptr);
				node.commentSize.y = std::strtof(tokens[11].c_str(), nullptr);
				result.nodes.push_back(std::move(node));
			}

		} else if (tokens[0] == "lua" && tokens.size() >= 2 && !result.nodes.empty()) {
			result.nodes.back().customCode = tokens[1];

		} else if (tokens[0] == "pin" && tokens.size() >= 7 && !result.nodes.empty()) {
			Pin pin;
			pin.id = std::atoi(tokens[1].c_str());
			pin.isOutput = tokens[2] == "out";

			if (pinKindFromName(tokens[3], pin.type.kind)) {
				pin.type.subtype = tokens[4];
				pin.type.isArray = tokens[5] == "1";
				pin.name = tokens[6];
				pin.defaultValue = tokens.size() >= 8 ? tokens[7] : "";
				pin.node = result.nodes.back().id;
				result.nodes.back().pins.push_back(std::move(pin));
			}

		} else if (tokens[0] == "link" && tokens.size() >= 4) {
			Link link;
			link.id = std::atoi(tokens[1].c_str());
			link.from = std::atoi(tokens[2].c_str());
			link.to = std::atoi(tokens[3].c_str());
			result.links.push_back(link);
		}
	}

	return !first;
}

void BlueprintEditor::installParsed(ParsedGraph&& parsed, bool includeIdentity) {
	if (includeIdentity) {
		if (!parsed.name.empty()) {
			blueprintName = parsed.name;
		}

		if (!parsed.parent.empty()) {
			blueprintParentClass = parsed.parent;
		}

		variables = std::move(parsed.variables);
	}

	nodes = std::move(parsed.nodes);
	links = std::move(parsed.links);
	selectedNodes.clear();
	selectedLinks.clear();
	rebuildIndex();

	// drop links with missing endpoints
	links.erase(std::remove_if(links.begin(), links.end(), [this](const Link& link) {
		return !findPin(link.from) || !findPin(link.to);
	}), links.end());

	// make sure new IDs don't collide with loaded ones
	ID maxID = 0;

	for (auto& node : nodes) {
		maxID = std::max(maxID, node.id);

		for (auto& pin : node.pins) {
			maxID = std::max(maxID, pin.id);
		}
	}

	for (auto& link : links) {
		maxID = std::max(maxID, link.id);
	}

	nextID = maxID + 1;
}

std::string BlueprintEditor::SaveToString() const {
	return serializeGraph(false, GRAPH_MAGIC);
}

bool BlueprintEditor::LoadFromString(const std::string& text) {
	ParsedGraph parsed;

	if (!parseGraphText(text, GRAPH_MAGIC, parsed)) {
		return false;
	}

	installParsed(std::move(parsed), true);
	resetUndo();
	dirty = false;
	pendingZoomToFit = true;
	return true;
}


//
//	BlueprintEditor::Render
//

void BlueprintEditor::Render(const char* title, const ImVec2& size, bool border) {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(38, 38, 38, 255));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.0f);

	ImGui::BeginChild(
		title,
		size,
		border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);

	drawList = ImGui::GetWindowDrawList();
	canvasPos = ImGui::GetCursorScreenPos();
	canvasSize = ImGui::GetContentRegionAvail();
	canvasSize.x = std::max(canvasSize.x, 64.0f);
	canvasSize.y = std::max(canvasSize.y, 64.0f);
	baseFontSize = ImGui::GetFontSize();

	applyPendingViewChanges();

	hoveredNode = 0;
	hoveredPin = 0;
	hoveredLink = 0;
	edited = false;

	renderGrid();

	// nodes are submitted front-to-back so the topmost node claims the mouse
	// first, while each is drawn into its own channel to preserve visual order
	// (channel 0 = comments, channel 1 = links, channel 2+ = regular nodes)
	int channels = static_cast<int>(nodes.size()) + 2;
	splitter.Split(drawList, channels);

	for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; i--) {
		Node& node = nodes[static_cast<size_t>(i)];
		splitter.SetCurrentChannel(drawList, node.kind == NodeKind::Comment ? 0 : i + 2);
		renderNode(node);
	}

	splitter.SetCurrentChannel(drawList, 1);
	renderLinks();
	renderPendingLink();
	splitter.Merge(drawList);

	renderBoxSelect();
	handleCanvasInput();
	handleKeyboard();
	renderOverlay();
	renderGraphContextMenu();
	renderNodeContextMenu();

	if (edited) {
		dirty = true;
	}

	// apply deferred z-order change
	if (bringToFront) {
		auto i = nodeIndex.find(bringToFront);

		if (i != nodeIndex.end() && i->second + 1 < nodes.size()) {
			Node node = std::move(nodes[i->second]);
			nodes.erase(nodes.begin() + static_cast<ptrdiff_t>(i->second));
			nodes.push_back(std::move(node));
			rebuildIndex();
		}

		bringToFront = 0;
	}

	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}


//
//	BlueprintEditor::renderGrid
//

void BlueprintEditor::renderGrid() {
	if (showGrid) {
		float step = 16.0f * zoom;

		if (step >= 4.0f) {
			int firstX = static_cast<int>(std::floor(-scrolling.x / step));
			int lastX = static_cast<int>(std::ceil((canvasSize.x - scrolling.x) / step));

			for (int i = firstX; i <= lastX; i++) {
				float x = canvasPos.x + scrolling.x + static_cast<float>(i) * step;
				ImU32 color = (i % 8 == 0) ? IM_COL32(20, 20, 20, 255) : IM_COL32(52, 52, 52, 255);
				drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), color);
			}

			int firstY = static_cast<int>(std::floor(-scrolling.y / step));
			int lastY = static_cast<int>(std::ceil((canvasSize.y - scrolling.y) / step));

			for (int i = firstY; i <= lastY; i++) {
				float y = canvasPos.y + scrolling.y + static_cast<float>(i) * step;
				ImU32 color = (i % 8 == 0) ? IM_COL32(20, 20, 20, 255) : IM_COL32(52, 52, 52, 255);
				drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), color);
			}
		}
	}

	// Unreal-style watermark in the top right corner
	ImFont* font = ImGui::GetFont();
	float bigSize = baseFontSize * 2.4f;
	ImVec2 textSize = font->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, "BLUEPRINT");
	ImVec2 pos(canvasPos.x + canvasSize.x - textSize.x - 16.0f, canvasPos.y + 10.0f);
	drawList->AddText(font, bigSize, pos, IM_COL32(255, 255, 255, 22), "BLUEPRINT");

	std::string caption = blueprintName + " > " + blueprintParentClass;
	ImVec2 captionSize = font->CalcTextSizeA(baseFontSize, FLT_MAX, 0.0f, caption.c_str());
	drawList->AddText(font, baseFontSize, ImVec2(canvasPos.x + canvasSize.x - captionSize.x - 16.0f, pos.y + bigSize + 2.0f), IM_COL32(255, 255, 255, 40), caption.c_str());
}


//
//	BlueprintEditor::renderPinIcon
//

void BlueprintEditor::renderPinIcon(const ImVec2& center, const PinType& type, bool connected) const {
	ImU32 color = pinColor(type);

	if (type.kind == PinKind::Exec) {
		// pentagon arrow like Unreal's exec pins
		float w = 5.0f * zoom;
		float h = 6.0f * zoom;
		ImVec2 points[5] = {
			ImVec2(center.x - w, center.y - h),
			ImVec2(center.x + 1.0f * zoom, center.y - h),
			ImVec2(center.x + w, center.y),
			ImVec2(center.x + 1.0f * zoom, center.y + h),
			ImVec2(center.x - w, center.y + h)};

		if (connected) {
			drawList->AddConvexPolyFilled(points, 5, color);

		} else {
			drawList->AddPolyline(points, 5, color, ImDrawFlags_Closed, std::max(1.0f, 1.5f * zoom));
		}

	} else if (type.isArray) {
		// grid of squares for array pins
		float s = 2.4f * zoom;
		float gap = 2.8f * zoom;

		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ImVec2 c(center.x + static_cast<float>(x) * gap, center.y + static_cast<float>(y) * gap);
				drawList->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f), color);
			}
		}

	} else {
		float radius = 5.0f * zoom;

		if (connected) {
			drawList->AddCircleFilled(center, radius, color);

		} else {
			drawList->AddCircle(center, radius - 0.5f * zoom, color, 0, std::max(1.0f, 1.6f * zoom));
		}
	}
}


//
//	BlueprintEditor::pinHasDefaultEditor
//

bool BlueprintEditor::pinHasDefaultEditor(const Pin& pin) const {
	if (pin.isOutput || pin.type.isArray || pin.name == "Target") {
		return false;
	}

	switch (pin.type.kind) {
		case PinKind::Boolean:
		case PinKind::Byte:
		case PinKind::Integer:
		case PinKind::Float:
		case PinKind::String:
		case PinKind::Name:
		case PinKind::Vector:
		case PinKind::Rotator:
		case PinKind::Enum:
		case PinKind::Class:
			return true;

		default:
			return false;
	}
}


//
//	BlueprintEditor::defaultEditorWidth
//

float BlueprintEditor::defaultEditorWidth(const Pin& pin) const {
	switch (pin.type.kind) {
		case PinKind::Boolean: return ImGui::GetFrameHeight();
		case PinKind::Byte: return 44.0f * zoom;
		case PinKind::Integer: return 56.0f * zoom;
		case PinKind::Float: return 64.0f * zoom;
		case PinKind::String: return 100.0f * zoom;
		case PinKind::Name: return 84.0f * zoom;
		case PinKind::Vector: return 150.0f * zoom;
		case PinKind::Rotator: return 150.0f * zoom;
		case PinKind::Enum: return 110.0f * zoom;
		case PinKind::Class: return 110.0f * zoom;
		default: return 0.0f;
	}
}


//
//	BlueprintEditor::renderDefaultEditor
//

void BlueprintEditor::renderDefaultEditor(Pin& pin, const ImVec2& pos, float width) {
	ImGui::SetCursorScreenPos(pos);
	ImGui::PushID(pin.id);

	switch (pin.type.kind) {
		case PinKind::Boolean: {
			bool value = pin.defaultValue == "true";

			if (ImGui::Checkbox("##default", &value)) {
				pin.defaultValue = value ? "true" : "false";
				edited = true;
				recordUndo();
			}

			break;
		}

		case PinKind::Byte:
		case PinKind::Integer: {
			int value = std::atoi(pin.defaultValue.c_str());
			ImGui::SetNextItemWidth(width);

			if (ImGui::DragInt("##default", &value, 0.25f)) {
				if (pin.type.kind == PinKind::Byte) {
					value = std::clamp(value, 0, 255);
				}

				pin.defaultValue = std::to_string(value);
				edited = true;
			}

			if (ImGui::IsItemDeactivatedAfterEdit()) {
				recordUndo();
			}

			break;
		}

		case PinKind::Float: {
			float value = std::strtof(pin.defaultValue.c_str(), nullptr);
			ImGui::SetNextItemWidth(width);

			if (ImGui::DragFloat("##default", &value, 0.05f, 0.0f, 0.0f, "%.3f")) {
				pin.defaultValue = formatFloat(value);
				edited = true;
			}

			if (ImGui::IsItemDeactivatedAfterEdit()) {
				recordUndo();
			}

			break;
		}

		case PinKind::String:
		case PinKind::Name: {
			char buffer[256];
			std::snprintf(buffer, sizeof(buffer), "%s", pin.defaultValue.c_str());
			ImGui::SetNextItemWidth(width);

			if (ImGui::InputText("##default", buffer, sizeof(buffer))) {
				pin.defaultValue = buffer;
				edited = true;
			}

			if (ImGui::IsItemDeactivatedAfterEdit()) {
				recordUndo();
			}

			break;
		}

		case PinKind::Vector:
		case PinKind::Rotator: {
			float values[3] = {0.0f, 0.0f, 0.0f};
			parseFloats(pin.defaultValue, values, 3);
			ImGui::SetNextItemWidth(width);

			if (ImGui::DragFloat3("##default", values, 0.05f, 0.0f, 0.0f, "%.2f")) {
				pin.defaultValue = formatFloat(values[0]) + "," + formatFloat(values[1]) + "," + formatFloat(values[2]);
				edited = true;
			}

			if (ImGui::IsItemDeactivatedAfterEdit()) {
				recordUndo();
			}

			break;
		}

		case PinKind::Enum: {
			const Enumeration* enumeration = registry.FindEnum(pin.type.subtype);
			ImGui::SetNextItemWidth(width);

			const char* preview = pin.defaultValue.empty() ?
				((enumeration && !enumeration->values.empty()) ? enumeration->values[0].c_str() : "") :
				pin.defaultValue.c_str();

			if (ImGui::BeginCombo("##default", preview)) {
				if (enumeration) {
					for (auto& value : enumeration->values) {
						if (ImGui::Selectable(value.c_str(), value == pin.defaultValue)) {
							pin.defaultValue = value;
							edited = true;
							recordUndo();
						}
					}
				}

				ImGui::EndCombo();
			}

			break;
		}

		case PinKind::Class: {
			ImGui::SetNextItemWidth(width);
			const char* preview = pin.defaultValue.empty() ? "Select Class" : pin.defaultValue.c_str();

			if (ImGui::BeginCombo("##default", preview)) {
				for (auto& cls : registry.GetClasses()) {
					if (registry.IsChildOf(cls->name, pin.type.subtype)) {
						if (ImGui::Selectable(cls->name.c_str(), cls->name == pin.defaultValue)) {
							pin.defaultValue = cls->name;
							edited = true;
							recordUndo();
						}
					}
				}

				ImGui::EndCombo();
			}

			break;
		}

		default:
			break;
	}

	ImGui::PopID();
}


//
//	BlueprintEditor::handlePinInteraction
//

void BlueprintEditor::handlePinInteraction(Pin& pin, const ImVec2& center) {
	pin.center = center;
	ImGuiIO& io = ImGui::GetIO();
	float radius = std::max(8.0f, 9.0f * zoom);

	ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
	ImGui::PushID(pin.id);
	ImGui::InvisibleButton("##pin", ImVec2(radius * 2.0f, radius * 2.0f));

	// manual hover tracking (used when links are dropped and for context menus).
	// While dragging a link the pin is a DROP TARGET, so (a) ignore hoveredNode --
	// otherwise an earlier-rendered node's body blocks a later node's pin, which made
	// dropping onto input pins much harder than onto outputs -- and (b) widen the
	// snap radius so the wire connects when released merely NEAR a pin.
	bool dragging = action == Action::dragLink;
	float hoverR = dragging ? radius * 1.9f : radius;
	if (hoveredPin == 0 && (hoveredNode == 0 || dragging) && ImGui::IsWindowHovered() &&
		io.MousePos.x >= center.x - hoverR && io.MousePos.x <= center.x + hoverR &&
		io.MousePos.y >= center.y - hoverR && io.MousePos.y <= center.y + hoverR) {
		hoveredPin = pin.id;
	}

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && action == Action::none) {
		std::string label = pin.name.empty() ? pinTypeLabel(pin.type) : pin.name + "\n" + pinTypeLabel(pin.type);
		ImGui::SetTooltip("%s", label.c_str());
	}

	if (ImGui::IsItemActivated() && io.KeyAlt) {
		// alt-click breaks all links on the pin (like Unreal)
		if (pinLinkCount(pin.id)) {
			breakPinLinksInternal(pin.id);
			recordUndo();
			dirty = true;
		}

	} else if (ImGui::IsItemActive() && !io.KeyAlt && action == Action::none && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
		action = Action::dragLink;
		actionPin = pin.id;
	}

	ImGui::PopID();
}


//
//	BlueprintEditor::renderNode
//

void BlueprintEditor::renderNode(Node& node) {
	if (node.kind == NodeKind::Comment) {
		renderCommentNode(node);
		return;
	}

	if (node.kind == NodeKind::Reroute) {
		renderRerouteNode(node);
		return;
	}

	if (node.kind == NodeKind::CustomLua) {
		renderCustomLuaNode(node);
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	float z = zoom;
	bool compact = node.kind == NodeKind::VariableGet;

	ImGui::PushID(node.id);
	ImGui::PushFont(nullptr, baseFontSize * z);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f * z, 2.0f * z));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * z, 3.0f * z));

	ImFont* font = ImGui::GetFont();
	float textH = ImGui::GetTextLineHeight();
	float frameH = ImGui::GetFrameHeight();
	float iconSize = 15.0f * z;
	float padX = 9.0f * z;
	float inset = 5.0f * z;
	float rowH = std::max(iconSize, frameH) + 4.0f * z;
	float smallSize = baseFontSize * 0.78f * z;

	// split pins into columns
	std::vector<Pin*> inputs;
	std::vector<Pin*> outputs;

	for (auto& pin : node.pins) {
		(pin.isOutput ? outputs : inputs).push_back(&pin);
	}

	// measure the header
	ImVec2 titleSize = ImGui::CalcTextSize(node.title.c_str());
	float subtitleW = node.subtitle.empty() ? 0.0f : font->CalcTextSizeA(smallSize, FLT_MAX, 0.0f, node.subtitle.c_str()).x;
	float headerH = compact ? 0.0f : (titleSize.y + (node.subtitle.empty() ? 0.0f : smallSize + 2.0f * z) + 8.0f * z);

	// measure the pin rows
	float maxIn = 0.0f;
	float maxOut = 0.0f;

	for (auto pin : inputs) {
		float w = iconSize + 4.0f * z + (pin->name.empty() ? 0.0f : ImGui::CalcTextSize(pin->name.c_str()).x);
		bool connected = pinLinkCount(pin->id) != 0;

		if (!connected && pinHasDefaultEditor(*pin)) {
			w += 5.0f * z + defaultEditorWidth(*pin);

		} else if (!connected && pin->name == "Target") {
			w += 5.0f * z + ImGui::CalcTextSize("self").x;
		}

		maxIn = std::max(maxIn, w);
	}

	for (auto pin : outputs) {
		float w = iconSize + (pin->name.empty() ? 0.0f : 4.0f * z + ImGui::CalcTextSize(pin->name.c_str()).x);
		maxOut = std::max(maxOut, w);
	}

	size_t rows = std::max(inputs.size(), outputs.size());
	float width, height;

	if (compact) {
		width = padX + titleSize.x + 10.0f * z + iconSize + inset;
		height = rowH + 6.0f * z;

	} else {
		width = std::max(std::max(titleSize.x, subtitleW) + 2.0f * padX, inset * 2.0f + maxIn + 26.0f * z + maxOut);
		width = std::max(width, 110.0f * z);
		height = headerH + static_cast<float>(rows) * rowH + 8.0f * z;
	}

	node.size = ImVec2(width / z, height / z);

	ImVec2 p0 = graphToScreen(node.pos);
	ImVec2 p1 = p0 + ImVec2(width, height);
	float rounding = 6.0f * z;
	bool selected = selectedNodes.count(node.id) != 0;

	// draw the node background
	if (compact) {
		ImU32 color = pinColor(node.pins.empty() ? PinType(PinKind::Wildcard) : node.pins[0].type);
		drawList->AddRectFilled(p0, p1, IM_COL32(30, 30, 30, 235), rounding);
		drawList->AddRectFilled(p0, p1, (color & 0x00FFFFFF) | (static_cast<ImU32>(52) << 24), rounding);

	} else {
		drawList->AddRectFilled(p0, p1, IM_COL32(22, 22, 22, 235), rounding);

		if (headerH > 0.0f) {
			drawList->AddRectFilled(p0, ImVec2(p1.x, p0.y + headerH), headerColor(node), rounding, ImDrawFlags_RoundCornersTop);
			drawList->AddLine(ImVec2(p0.x, p0.y + headerH), ImVec2(p1.x, p0.y + headerH), IM_COL32(0, 0, 0, 130), std::max(1.0f, z));
		}
	}

	drawList->AddRect(p0, p1, selected ? IM_COL32(255, 175, 40, 255) : IM_COL32(70, 70, 70, 255), rounding, 0, selected ? std::max(1.5f, 2.0f * z) : 1.0f);

	// draw the title
	if (compact) {
		drawList->AddText(font, baseFontSize * z, ImVec2(p0.x + padX, p0.y + (height - titleSize.y) * 0.5f), IM_COL32(235, 235, 235, 255), node.title.c_str());

	} else {
		drawList->AddText(font, baseFontSize * z, ImVec2(p0.x + padX, p0.y + 4.0f * z), IM_COL32(255, 255, 255, 255), node.title.c_str());

		if (!node.subtitle.empty()) {
			drawList->AddText(font, smallSize, ImVec2(p0.x + padX, p0.y + 4.0f * z + titleSize.y + 1.0f * z), IM_COL32(200, 200, 200, 170), node.subtitle.c_str());
		}
	}

	// draw the pins
	float rowStart = compact ? p0.y : p0.y + headerH + 4.0f * z;

	for (size_t i = 0; i < inputs.size(); i++) {
		Pin& pin = *inputs[i];
		float centerY = compact ? (p0.y + height * 0.5f) : (rowStart + static_cast<float>(i) * rowH + rowH * 0.5f);
		ImVec2 center(p0.x + inset + iconSize * 0.5f, centerY);
		bool connected = pinLinkCount(pin.id) != 0 || (action == Action::dragLink && actionPin == pin.id);

		renderPinIcon(center, pin.type, connected);
		float x = center.x + iconSize * 0.5f + 4.0f * z;

		if (!pin.name.empty()) {
			ImVec2 labelSize = ImGui::CalcTextSize(pin.name.c_str());
			drawList->AddText(font, baseFontSize * z, ImVec2(x, centerY - labelSize.y * 0.5f), IM_COL32(224, 224, 224, 255), pin.name.c_str());
			x += labelSize.x + 5.0f * z;
		}

		if (pinLinkCount(pin.id) == 0) {
			if (pinHasDefaultEditor(pin)) {
				renderDefaultEditor(pin, ImVec2(x, centerY - frameH * 0.5f), defaultEditorWidth(pin));

			} else if (pin.name == "Target") {
				drawList->AddText(font, baseFontSize * z, ImVec2(x, centerY - textH * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), "self");
			}
		}

		handlePinInteraction(pin, center);
	}

	for (size_t i = 0; i < outputs.size(); i++) {
		Pin& pin = *outputs[i];
		float centerY = compact ? (p0.y + height * 0.5f) : (rowStart + static_cast<float>(i) * rowH + rowH * 0.5f);
		ImVec2 center(p1.x - inset - iconSize * 0.5f, centerY);
		bool connected = pinLinkCount(pin.id) != 0 || (action == Action::dragLink && actionPin == pin.id);

		renderPinIcon(center, pin.type, connected);

		if (!pin.name.empty()) {
			ImVec2 labelSize = ImGui::CalcTextSize(pin.name.c_str());
			drawList->AddText(font, baseFontSize * z, ImVec2(center.x - iconSize * 0.5f - 4.0f * z - labelSize.x, centerY - labelSize.y * 0.5f), IM_COL32(224, 224, 224, 255), pin.name.c_str());
		}

		handlePinInteraction(pin, center);
	}

	// track hover (pins were given a chance first, nodes above us were handled earlier)
	if (hoveredNode == 0 && ImGui::IsWindowHovered() &&
		io.MousePos.x >= p0.x && io.MousePos.x <= p1.x &&
		io.MousePos.y >= p0.y && io.MousePos.y <= p1.y) {
		hoveredNode = node.id;
	}

	// the node body button is submitted last so pins and value editors get priority
	ImGui::SetCursorScreenPos(p0);
	ImGui::InvisibleButton("##nodebody", ImVec2(width, height));

	if (ImGui::IsItemActivated()) {
		if (io.KeyCtrl) {
			if (selected) {
				selectedNodes.erase(node.id);

			} else {
				selectedNodes.insert(node.id);
			}

		} else if (!selected) {
			selectedNodes.clear();
			selectedLinks.clear();
			selectedNodes.insert(node.id);
		}

		bringToFront = node.id;
		action = Action::dragNodes;
		nodesMoved = false;
	}

	if (ImGui::IsItemActive() && action == Action::dragNodes && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
		nodesMoved = true;

		for (auto id : selectedNodes) {
			Node* n = findNode(id);

			if (n) {
				n->pos = n->pos + io.MouseDelta / z;
			}
		}
	}

	if (ImGui::IsItemDeactivated() && action == Action::dragNodes) {
		action = Action::none;

		if (nodesMoved) {
			nodesMoved = false;
			recordUndo();
			dirty = true;

		} else if (!io.KeyCtrl && selected && selectedNodes.size() + selectedLinks.size() > 1) {
			// plain click on an already selected node makes it the only selection
			selectedNodes.clear();
			selectedLinks.clear();
			selectedNodes.insert(node.id);
		}
	}

	ImGui::PopStyleVar(2);
	ImGui::PopFont();
	ImGui::PopID();
}


//
//	BlueprintEditor::renderCommentNode
//

void BlueprintEditor::renderCommentNode(Node& node) {
	ImGuiIO& io = ImGui::GetIO();
	float z = zoom;

	node.commentSize.x = std::max(node.commentSize.x, 120.0f);
	node.commentSize.y = std::max(node.commentSize.y, 80.0f);
	node.size = node.commentSize;

	ImGui::PushID(node.id);
	ImGui::PushFont(nullptr, baseFontSize * z);

	ImVec2 p0 = graphToScreen(node.pos);
	ImVec2 p1 = p0 + node.commentSize * z;
	float headerH = ImGui::GetTextLineHeight() + 8.0f * z;
	bool selected = selectedNodes.count(node.id) != 0;

	drawList->AddRectFilled(p0, p1, IM_COL32(130, 130, 130, 30), 4.0f * z);
	drawList->AddRectFilled(p0, ImVec2(p1.x, p0.y + headerH), IM_COL32(130, 130, 130, 90), 4.0f * z, ImDrawFlags_RoundCornersTop);
	drawList->AddRect(p0, p1, selected ? IM_COL32(255, 175, 40, 255) : IM_COL32(130, 130, 130, 120), 4.0f * z, 0, selected ? 2.0f : 1.0f);
	drawList->AddText(ImGui::GetFont(), baseFontSize * z, ImVec2(p0.x + 8.0f * z, p0.y + 4.0f * z), IM_COL32(255, 255, 255, 220), node.title.c_str());

	// resize grip in the bottom right corner
	float grip = 14.0f * z;
	drawList->AddTriangleFilled(ImVec2(p1.x - grip, p1.y), ImVec2(p1.x, p1.y - grip), p1, IM_COL32(130, 130, 130, 140));

	ImGui::SetCursorScreenPos(ImVec2(p1.x - grip, p1.y - grip));
	ImGui::InvisibleButton("##resize", ImVec2(grip, grip));

	if (ImGui::IsItemActivated()) {
		action = Action::resizeComment;
		actionNode = node.id;
	}

	if (ImGui::IsItemActive() && action == Action::resizeComment) {
		node.commentSize = node.commentSize + io.MouseDelta / z;
		node.commentSize.x = std::max(node.commentSize.x, 120.0f);
		node.commentSize.y = std::max(node.commentSize.y, 80.0f);
	}

	if (ImGui::IsItemDeactivated() && action == Action::resizeComment) {
		action = Action::none;
		actionNode = 0;
		recordUndo();
		dirty = true;
	}

	// only the header is interactive so box selects can start on the body
	if (hoveredNode == 0 && ImGui::IsWindowHovered() &&
		io.MousePos.x >= p0.x && io.MousePos.x <= p1.x &&
		io.MousePos.y >= p0.y && io.MousePos.y <= p0.y + headerH) {
		hoveredNode = node.id;
	}

	ImGui::SetCursorScreenPos(p0);
	ImGui::InvisibleButton("##commentheader", ImVec2(std::max(1.0f, p1.x - p0.x), headerH));

	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		renameNode = node.id;
		std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", node.title.c_str());
		ImGui::OpenPopup("##BlueprintRename");
	}

	if (ImGui::IsItemActivated()) {
		if (io.KeyCtrl) {
			if (selected) {
				selectedNodes.erase(node.id);

			} else {
				selectedNodes.insert(node.id);
			}

		} else if (!selected) {
			selectedNodes.clear();
			selectedLinks.clear();
			selectedNodes.insert(node.id);
		}

		action = Action::dragNodes;
		nodesMoved = false;

		// dragging a comment takes all fully contained nodes along (like Unreal)
		commentCapture.clear();

		for (auto& other : nodes) {
			if (other.id != node.id && !selectedNodes.count(other.id)) {
				ImVec2 otherSize = other.kind == NodeKind::Comment ? other.commentSize : other.size;

				if (other.pos.x >= node.pos.x && other.pos.y >= node.pos.y &&
					other.pos.x + otherSize.x <= node.pos.x + node.commentSize.x &&
					other.pos.y + otherSize.y <= node.pos.y + node.commentSize.y) {
					commentCapture.push_back(other.id);
				}
			}
		}
	}

	if (ImGui::IsItemActive() && action == Action::dragNodes && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
		nodesMoved = true;

		for (auto id : selectedNodes) {
			Node* n = findNode(id);

			if (n) {
				n->pos = n->pos + io.MouseDelta / z;
			}
		}

		for (auto id : commentCapture) {
			Node* n = findNode(id);

			if (n) {
				n->pos = n->pos + io.MouseDelta / z;
			}
		}

		if (!selectedNodes.count(node.id)) {
			node.pos = node.pos + io.MouseDelta / z;
		}
	}

	if (ImGui::IsItemDeactivated() && action == Action::dragNodes) {
		action = Action::none;
		commentCapture.clear();

		if (nodesMoved) {
			nodesMoved = false;
			recordUndo();
			dirty = true;
		}
	}

	// the rename popup has to be rendered in the same ID scope it was opened in
	if (ImGui::BeginPopup("##BlueprintRename")) {
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}

		bool done = ImGui::InputText("##rename", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		done |= ImGui::Button("OK");

		if (done) {
			Node* n = findNode(renameNode);

			if (n && renameBuffer[0]) {
				n->title = renameBuffer;
				recordUndo();
				dirty = true;
			}

			renameNode = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	ImGui::PopFont();
	ImGui::PopID();
}


//
//	BlueprintEditor::renderRerouteNode
//

void BlueprintEditor::renderRerouteNode(Node& node) {
	ImGuiIO& io = ImGui::GetIO();
	float z = zoom;
	node.size = ImVec2(16.0f, 16.0f);

	ImGui::PushID(node.id);

	ImVec2 center = graphToScreen(ImVec2(node.pos.x + 8.0f, node.pos.y + 8.0f));
	bool selected = selectedNodes.count(node.id) != 0;
	ImU32 color = pinColor(node.pins.empty() ? PinType(PinKind::Wildcard) : node.pins[0].type);

	if (selected) {
		drawList->AddCircle(center, 8.5f * z, IM_COL32(255, 175, 40, 255), 0, 2.0f);
	}

	drawList->AddCircleFilled(center, 5.5f * z, color);

	// pin hotspots sit just left and right of the circle
	for (auto& pin : node.pins) {
		ImVec2 pinCenter(center.x + (pin.isOutput ? 10.0f : -10.0f) * z, center.y);
		handlePinInteraction(pin, pinCenter);
	}

	if (hoveredNode == 0 && ImGui::IsWindowHovered() && lengthSquared(io.MousePos - center) <= 8.0f * z * 8.0f * z) {
		hoveredNode = node.id;
	}

	ImGui::SetCursorScreenPos(ImVec2(center.x - 7.0f * z, center.y - 7.0f * z));
	ImGui::InvisibleButton("##reroute", ImVec2(14.0f * z, 14.0f * z));

	if (ImGui::IsItemActivated()) {
		if (io.KeyCtrl) {
			if (selected) {
				selectedNodes.erase(node.id);

			} else {
				selectedNodes.insert(node.id);
			}

		} else if (!selected) {
			selectedNodes.clear();
			selectedLinks.clear();
			selectedNodes.insert(node.id);
		}

		action = Action::dragNodes;
		nodesMoved = false;
	}

	if (ImGui::IsItemActive() && action == Action::dragNodes && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
		nodesMoved = true;

		for (auto id : selectedNodes) {
			Node* n = findNode(id);

			if (n) {
				n->pos = n->pos + io.MouseDelta / z;
			}
		}
	}

	if (ImGui::IsItemDeactivated() && action == Action::dragNodes) {
		action = Action::none;

		if (nodesMoved) {
			nodesMoved = false;
			recordUndo();
			dirty = true;
		}
	}

	ImGui::PopID();
}


//
//	BlueprintEditor::renderCustomLuaNode
//

BlueprintEditor::CustomLuaEditorState& BlueprintEditor::ensureLuaEditor(const Node& node) {
	CustomLuaEditorState& state = luaEditors[node.id];

	if (!state.editor) {
		state.editor = std::make_shared<TextEditor>();
		state.editor->SetLanguage(TextEditor::Language::Lua());
		state.editor->SetShowLineNumbersEnabled(false);      // node boxes are small
		state.editor->SetShowScrollbarMiniMapEnabled(false);
		state.editor->SetText(node.customCode);
		state.lastUndoIndex = state.editor->GetUndoIndex();
	}

	return state;
}

void BlueprintEditor::renderCustomLuaNode(Node& node) {
	ImGuiIO& io = ImGui::GetIO();
	float z = zoom;

	node.commentSize.x = std::max(node.commentSize.x, 220.0f);
	node.commentSize.y = std::max(node.commentSize.y, 140.0f);
	node.size = node.commentSize;

	ImGui::PushID(node.id);
	ImGui::PushFont(nullptr, baseFontSize * z);

	ImVec2 p0 = graphToScreen(node.pos);
	ImVec2 p1 = p0 + node.commentSize * z;
	float headerH = ImGui::GetTextLineHeight() + 8.0f * z;
	float rowH = std::max(15.0f * z, ImGui::GetFrameHeight()) + 4.0f * z;
	float inset = 5.0f * z;
	float iconSize = 15.0f * z;
	bool selected = selectedNodes.count(node.id) != 0;

	drawList->AddRectFilled(p0, p1, IM_COL32(22, 22, 22, 235), 6.0f * z);
	drawList->AddRectFilled(p0, ImVec2(p1.x, p0.y + headerH), headerColor(node), 6.0f * z, ImDrawFlags_RoundCornersTop);
	drawList->AddLine(ImVec2(p0.x, p0.y + headerH), ImVec2(p1.x, p0.y + headerH), IM_COL32(0, 0, 0, 130), std::max(1.0f, z));
	drawList->AddRect(p0, p1, selected ? IM_COL32(255, 175, 40, 255) : IM_COL32(70, 70, 70, 255), 6.0f * z, 0, selected ? std::max(1.5f, 2.0f * z) : 1.0f);
	drawList->AddText(ImGui::GetFont(), baseFontSize * z, ImVec2(p0.x + 9.0f * z, p0.y + 4.0f * z), IM_COL32(255, 255, 255, 255), node.title.c_str());

	// resize grip, bottom right (mirrors renderCommentNode)
	float grip = 14.0f * z;
	drawList->AddTriangleFilled(ImVec2(p1.x - grip, p1.y), ImVec2(p1.x, p1.y - grip), p1, IM_COL32(130, 130, 130, 140));
	ImGui::SetCursorScreenPos(ImVec2(p1.x - grip, p1.y - grip));
	ImGui::InvisibleButton("##resize", ImVec2(grip, grip));

	if (ImGui::IsItemActivated()) {
		action = Action::resizeComment;
		actionNode = node.id;
	}

	if (ImGui::IsItemActive() && action == Action::resizeComment) {
		node.commentSize = node.commentSize + io.MouseDelta / z;
		node.commentSize.x = std::max(node.commentSize.x, 220.0f);
		node.commentSize.y = std::max(node.commentSize.y, 140.0f);
	}

	if (ImGui::IsItemDeactivated() && action == Action::resizeComment) {
		action = Action::none;
		actionNode = 0;
		recordUndo();
		dirty = true;
	}

	// header: drag to move/select, double-click to rename (mirrors renderCommentNode)
	if (hoveredNode == 0 && ImGui::IsWindowHovered() &&
		io.MousePos.x >= p0.x && io.MousePos.x <= p1.x &&
		io.MousePos.y >= p0.y && io.MousePos.y <= p0.y + headerH) {
		hoveredNode = node.id;
	}

	ImGui::SetCursorScreenPos(p0);
	ImGui::InvisibleButton("##luaheader", ImVec2(std::max(1.0f, p1.x - p0.x), headerH));

	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		renameNode = node.id;
		std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", node.title.c_str());
		ImGui::OpenPopup("##BlueprintRename");
	}

	if (ImGui::IsItemActivated()) {
		if (io.KeyCtrl) {
			if (selected) {
				selectedNodes.erase(node.id);

			} else {
				selectedNodes.insert(node.id);
			}

		} else if (!selected) {
			selectedNodes.clear();
			selectedLinks.clear();
			selectedNodes.insert(node.id);
		}

		action = Action::dragNodes;
		nodesMoved = false;
	}

	if (ImGui::IsItemActive() && action == Action::dragNodes && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
		nodesMoved = true;

		for (auto id : selectedNodes) {
			Node* n = findNode(id);

			if (n) {
				n->pos = n->pos + io.MouseDelta / z;
			}
		}
	}

	if (ImGui::IsItemDeactivated() && action == Action::dragNodes) {
		action = Action::none;

		if (nodesMoved) {
			nodesMoved = false;
			recordUndo();
			dirty = true;
		}
	}

	if (ImGui::BeginPopup("##BlueprintRename")) {
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}

		bool done = ImGui::InputText("##rename", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		done |= ImGui::Button("OK");

		if (done) {
			Node* n = findNode(renameNode);

			if (n && renameBuffer[0]) {
				n->title = renameBuffer;
				recordUndo();
				dirty = true;
			}

			renameNode = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	// split pins: exec pins are fixed (top row), data pins are dynamic (user-managed)
	std::vector<Pin*> execIn, execOut, dataIn, dataOut;

	for (auto& pin : node.pins) {
		if (pin.type.kind == PinKind::Exec) {
			(pin.isOutput ? execOut : execIn).push_back(&pin);

		} else {
			(pin.isOutput ? dataOut : dataIn).push_back(&pin);
		}
	}

	float execY = p0.y + headerH + rowH * 0.5f;

	for (auto pin : execIn) {
		ImVec2 center(p0.x + inset + iconSize * 0.5f, execY);
		bool connected = pinLinkCount(pin->id) != 0 || (action == Action::dragLink && actionPin == pin->id);
		renderPinIcon(center, pin->type, connected);
		handlePinInteraction(*pin, center);
	}

	for (auto pin : execOut) {
		ImVec2 center(p1.x - inset - iconSize * 0.5f, execY);
		bool connected = pinLinkCount(pin->id) != 0 || (action == Action::dragLink && actionPin == pin->id);
		renderPinIcon(center, pin->type, connected);
		handlePinInteraction(*pin, center);
	}

	// dynamic data pins: inline-renameable name field + remove button on each row
	float bodyY = p0.y + headerH + rowH;
	size_t pinRows = std::max(dataIn.size(), dataOut.size());
	float pinFieldW = std::max(40.0f, (p1.x - p0.x) * 0.5f - (inset + iconSize + 30.0f * z));
	ID removePin = 0;

	for (size_t i = 0; i < pinRows; i++) {
		float rowY = bodyY + static_cast<float>(i) * rowH;
		float centerY = rowY + rowH * 0.5f;
		float fieldY = rowY + (rowH - ImGui::GetFrameHeight()) * 0.5f;

		if (i < dataIn.size()) {
			Pin* pin = dataIn[i];
			ImVec2 center(p0.x + inset + iconSize * 0.5f, centerY);
			bool connected = pinLinkCount(pin->id) != 0 || (action == Action::dragLink && actionPin == pin->id);
			renderPinIcon(center, pin->type, connected);
			handlePinInteraction(*pin, center);

			ImGui::PushID(pin->id);
			ImGui::SetCursorScreenPos(ImVec2(center.x + iconSize * 0.5f + 4.0f * z, fieldY));
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%s", pin->name.c_str());
			ImGui::SetNextItemWidth(pinFieldW);

			if (ImGui::InputText("##pinname", buffer, sizeof(buffer))) {
				pin->name = buffer;
				edited = true;
			}

			if (ImGui::IsItemDeactivatedAfterEdit()) {
				recordUndo();
			}

			ImGui::SameLine();

			if (ImGui::SmallButton("x")) {
				removePin = pin->id;
			}

			ImGui::PopID();
		}

		if (i < dataOut.size()) {
			Pin* pin = dataOut[i];
			ImVec2 center(p1.x - inset - iconSize * 0.5f, centerY);
			bool connected = pinLinkCount(pin->id) != 0 || (action == Action::dragLink && actionPin == pin->id);
			renderPinIcon(center, pin->type, connected);
			handlePinInteraction(*pin, center);

			ImGui::PushID(pin->id);
			float fieldX = center.x - iconSize * 0.5f - 4.0f * z - pinFieldW;
			ImGui::SetCursorScreenPos(ImVec2(fieldX - 22.0f * z, fieldY));

			if (ImGui::SmallButton("x")) {
				removePin = pin->id;
			}

			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(fieldX, fieldY));
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%s", pin->name.c_str());
			ImGui::SetNextItemWidth(pinFieldW);

			if (ImGui::InputText("##pinname", buffer, sizeof(buffer))) {
				pin->name = buffer;
				edited = true;
			}

			if (ImGui::IsItemDeactivatedAfterEdit()) {
				recordUndo();
			}

			ImGui::PopID();
		}
	}

	if (removePin != 0) {
		RemoveCustomLuaPin(node.id, removePin);
	}

	float buttonsY = bodyY + static_cast<float>(pinRows) * rowH;
	ImGui::SetCursorScreenPos(ImVec2(p0.x + inset, buttonsY + 2.0f * z));

	if (ImGui::SmallButton("+ Input")) {
		AddCustomLuaPin(node.id, false);
	}

	ImGui::SetCursorScreenPos(ImVec2(p1.x - inset - 62.0f * z, buttonsY + 2.0f * z));

	if (ImGui::SmallButton("+ Output")) {
		AddCustomLuaPin(node.id, true);
	}

	// the Lua source box fills whatever body space remains down to the resize grip.
	// It's a real embedded TextEditor (the app's own widget): Lua syntax highlighting,
	// proper cursor/selection/undo — not a plain InputTextMultiline.
	float codeY = buttonsY + rowH;
	ImVec2 codePos(p0.x + inset, codeY);
	ImVec2 codeSize(std::max(40.0f, p1.x - p0.x - 2.0f * inset), std::max(40.0f, p1.y - 6.0f * z - codeY));

	ImGui::SetCursorScreenPos(codePos);

	CustomLuaEditorState& state = ensureLuaEditor(node);
	TextEditor& ed = *state.editor;
	size_t undoNow = ed.GetUndoIndex();

	if (undoNow != state.lastUndoIndex) {
		// user edited inside the widget since last frame → push into the node
		state.lastUndoIndex = undoNow;
		node.customCode = ed.GetText();
		state.undoPending = true;
		state.lastEditTime = ImGui::GetTime();
		edited = true;
		dirty = true;

	} else if (ed.GetText() != node.customCode) {
		// node text changed from OUTSIDE the widget (graph undo/redo, SetCustomLuaSource,
		// import) → pull it into the editor
		ed.SetText(node.customCode);
		state.lastUndoIndex = ed.GetUndoIndex();
	}

	ed.Render("##luasource", codeSize, true);

	// Record ONE graph-undo snapshot per editing burst (debounced) instead of one per
	// keystroke — the widget itself provides fine-grained undo while typing.
	if (state.undoPending && ImGui::GetTime() - state.lastEditTime > 0.6) {
		state.undoPending = false;
		recordUndo();
	}

	// hover tracking over the body (below the header) so a canvas box-select doesn't
	// start from on top of the node's own widgets
	if (hoveredNode == 0 && ImGui::IsWindowHovered() &&
		io.MousePos.x >= p0.x && io.MousePos.x <= p1.x &&
		io.MousePos.y > p0.y + headerH && io.MousePos.y <= p1.y) {
		hoveredNode = node.id;
	}

	ImGui::PopFont();
	ImGui::PopID();
}


//
//	BlueprintEditor::drawWire
//

void BlueprintEditor::drawWire(const ImVec2& from, const ImVec2& to, ImU32 color, float thickness) const {
	float dx = to.x - from.x;
	float tangent = std::clamp(std::fabs(dx) * 0.5f, 40.0f * zoom, 220.0f * zoom);

	if (dx < 0.0f) {
		tangent += std::min(-dx * 0.25f, 120.0f * zoom);
	}

	drawList->AddBezierCubic(from, ImVec2(from.x + tangent, from.y), ImVec2(to.x - tangent, to.y), to, color, thickness);
}


//
//	BlueprintEditor::wireDistance
//

float BlueprintEditor::wireDistance(const ImVec2& from, const ImVec2& to, const ImVec2& point) const {
	float dx = to.x - from.x;
	float tangent = std::clamp(std::fabs(dx) * 0.5f, 40.0f * zoom, 220.0f * zoom);

	if (dx < 0.0f) {
		tangent += std::min(-dx * 0.25f, 120.0f * zoom);
	}

	ImVec2 c1(from.x + tangent, from.y);
	ImVec2 c2(to.x - tangent, to.y);

	// sample the bezier and measure the distance to each segment
	float result = FLT_MAX;
	ImVec2 previous = from;
	constexpr int steps = 24;

	for (int i = 1; i <= steps; i++) {
		float t = static_cast<float>(i) / static_cast<float>(steps);
		float u = 1.0f - t;
		ImVec2 current =
			from * (u * u * u) +
			c1 * (3.0f * u * u * t) +
			c2 * (3.0f * u * t * t) +
			to * (t * t * t);

		result = std::min(result, distanceToSegment(point, previous, current));
		previous = current;
	}

	return result;
}


//
//	BlueprintEditor::renderLinks
//

void BlueprintEditor::renderLinks() {
	ImGuiIO& io = ImGui::GetIO();
	bool detectHover = ImGui::IsWindowHovered() && hoveredNode == 0 && hoveredPin == 0 && action == Action::none;
	float bestDistance = std::max(6.0f, 5.0f * zoom);

	for (auto& link : links) {
		const Pin* from = findPin(link.from);
		const Pin* to = findPin(link.to);

		if (!from || !to) {
			continue;
		}

		if (detectHover) {
			float distance = wireDistance(from->center, to->center, io.MousePos);

			if (distance < bestDistance) {
				bestDistance = distance;
				hoveredLink = link.id;
			}
		}
	}

	for (auto& link : links) {
		const Pin* from = findPin(link.from);
		const Pin* to = findPin(link.to);

		if (!from || !to) {
			continue;
		}

		bool isExec = from->type.kind == PinKind::Exec;
		ImU32 color = pinColor(from->type);
		float thickness = (isExec ? 3.0f : 2.2f) * zoom;

		if (selectedLinks.count(link.id)) {
			color = IM_COL32(255, 175, 40, 255);
			thickness += 1.0f * zoom;

		} else if (hoveredLink == link.id) {
			thickness += 1.0f * zoom;
		}

		drawWire(from->center, to->center, color, thickness);
	}
}


//
//	BlueprintEditor::renderPendingLink
//

void BlueprintEditor::renderPendingLink() {
	if (action != Action::dragLink) {
		return;
	}

	const Pin* pin = findPin(actionPin);

	if (!pin) {
		action = Action::none;
		return;
	}

	ImVec2 mouse = ImGui::GetMousePos();
	ImU32 color = pinColor(pin->type);

	if (hoveredPin != 0 && hoveredPin != actionPin) {
		std::string error;
		bool valid = canConnect(actionPin, hoveredPin, error);
		color = valid ? IM_COL32(80, 220, 80, 255) : IM_COL32(230, 60, 60, 255);

		if (!valid) {
			ImGui::SetTooltip("%s", error.c_str());
		}
	}

	if (pin->isOutput) {
		drawWire(pin->center, mouse, color, 2.5f * zoom);

	} else {
		drawWire(mouse, pin->center, color, 2.5f * zoom);
	}
}


//
//	BlueprintEditor::renderBoxSelect
//

void BlueprintEditor::renderBoxSelect() {
	if (action != Action::boxSelect) {
		return;
	}

	ImVec2 a = graphToScreen(actionOrigin);
	ImVec2 b = ImGui::GetMousePos();
	ImVec2 min = vecMin(a, b);
	ImVec2 max = vecMax(a, b);

	drawList->AddRectFilled(min, max, IM_COL32(120, 170, 255, 26));
	drawList->AddRect(min, max, IM_COL32(120, 170, 255, 140));
}


//
//	BlueprintEditor::renderOverlay
//

void BlueprintEditor::renderOverlay() {
	ImFont* font = ImGui::GetFont();
	char buffer[128];

	std::snprintf(buffer, sizeof(buffer), "%d nodes  |  %d links  |  %.0f%%",
		static_cast<int>(nodes.size()), static_cast<int>(links.size()), static_cast<double>(zoom * 100.0f));

	drawList->AddText(font, baseFontSize, ImVec2(canvasPos.x + 10.0f, canvasPos.y + canvasSize.y - baseFontSize - 8.0f), IM_COL32(255, 255, 255, 110), buffer);

	const char* hint = "Right-Click to Create Nodes";
	ImVec2 hintSize = font->CalcTextSizeA(baseFontSize, FLT_MAX, 0.0f, hint);
	drawList->AddText(font, baseFontSize, ImVec2(canvasPos.x + canvasSize.x - hintSize.x - 10.0f, canvasPos.y + canvasSize.y - baseFontSize - 8.0f), IM_COL32(255, 255, 255, 70), hint);
}


//
//	BlueprintEditor::handleCanvasInput
//

void BlueprintEditor::handleCanvasInput() {
	ImGuiIO& io = ImGui::GetIO();
	bool hovered = ImGui::IsWindowHovered();

	// zoom around the mouse cursor
	if (hovered && io.MouseWheel != 0.0f) {
		float newZoom = std::clamp(zoom * std::pow(1.15f, io.MouseWheel), MIN_ZOOM, MAX_ZOOM);

		if (newZoom != zoom) {
			ImVec2 graphPoint = screenToGraph(io.MousePos);
			zoom = newZoom;
			scrolling = ImVec2(io.MousePos.x - canvasPos.x - graphPoint.x * zoom, io.MousePos.y - canvasPos.y - graphPoint.y * zoom);
		}
	}

	// finish a link drag
	if (action == Action::dragLink && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
		if (hoveredPin != 0 && hoveredPin != actionPin) {
			std::string error;

			if (canConnect(actionPin, hoveredPin, error) && connect(actionPin, hoveredPin)) {
				recordUndo();
				dirty = true;
			}

		} else if (hovered && hoveredPin == 0 && hoveredNode == 0) {
			// dropped on empty canvas: open the palette filtered to compatible actions
			pendingLinkPin = actionPin;
			popupGraphPos = screenToGraph(io.MousePos);
			searchBuffer[0] = 0;
			paletteFocusSearch = true;
			buildPalette();
			ImGui::OpenPopup("##BlueprintGraphMenu");
		}

		action = Action::none;
		actionPin = 0;
	}

	// link interactions
	if (hoveredLink != 0 && action == Action::none) {
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			// double-click inserts a reroute node
			Link link = *findLink(hoveredLink);
			const Pin* from = findPin(link.from);

			if (from) {
				PinType type = from->type;
				ImVec2 graphPoint = screenToGraph(io.MousePos);
				ID reroute = AddRerouteNode(type, ImVec2(graphPoint.x - 8.0f, graphPoint.y - 8.0f));
				Node* rerouteNode = findNode(reroute);

				if (rerouteNode) {
					breakLinkInternal(link.id);
					connect(link.from, rerouteNode->pins[0].id);
					connect(rerouteNode->pins[1].id, link.to);
					recordUndo();
					dirty = true;
				}
			}

		} else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			if (io.KeyAlt) {
				breakLinkInternal(hoveredLink);
				recordUndo();
				dirty = true;

			} else {
				if (!io.KeyCtrl) {
					selectedNodes.clear();
					selectedLinks.clear();
				}

				if (selectedLinks.count(hoveredLink)) {
					selectedLinks.erase(hoveredLink);

				} else {
					selectedLinks.insert(hoveredLink);
				}
			}
		}
	}

	// box select
	if (action == Action::none && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
		hoveredNode == 0 && hoveredPin == 0 && hoveredLink == 0 &&
		!ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
		action = Action::boxSelect;
		actionOrigin = screenToGraph(io.MousePos);

		if (!io.KeyCtrl && !io.KeyShift) {
			selectedNodes.clear();
			selectedLinks.clear();
		}
	}

	if (action == Action::boxSelect && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		action = Action::none;
		ImVec2 endPoint = screenToGraph(io.MousePos);
		ImVec2 min = vecMin(actionOrigin, endPoint);
		ImVec2 max = vecMax(actionOrigin, endPoint);

		for (auto& node : nodes) {
			ImVec2 nodeSize = node.kind == NodeKind::Comment ? node.commentSize : node.size;
			ImVec2 nodeMax = node.pos + nodeSize;

			if (node.kind == NodeKind::Comment) {
				// comments must be fully contained (like Unreal)
				if (node.pos.x >= min.x && node.pos.y >= min.y && nodeMax.x <= max.x && nodeMax.y <= max.y) {
					selectedNodes.insert(node.id);
				}

			} else if (node.pos.x <= max.x && nodeMax.x >= min.x && node.pos.y <= max.y && nodeMax.y >= min.y) {
				selectedNodes.insert(node.id);
			}
		}
	}

	// pan with the right or middle mouse button
	if (action == Action::none && hovered &&
		(ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 4.0f))) {
		action = Action::pan;
	}

	if (action == Action::pan) {
		scrolling = panInverted ? (scrolling - io.MouseDelta) : (scrolling + io.MouseDelta);

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
			action = Action::none;
		}
	}

	// right-click (without dragging) opens a context menu
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
		rightClickPos = io.MousePos;
	}

	if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && action != Action::pan &&
		lengthSquared(io.MousePos - rightClickPos) < 25.0f) {
		if (hoveredPin != 0) {
			const Pin* pin = findPin(hoveredPin);

			if (pin) {
				contextNode = pin->node;
				ImGui::OpenPopup("##BlueprintNodeMenu");
			}

		} else if (hoveredNode != 0) {
			contextNode = hoveredNode;
			ImGui::OpenPopup("##BlueprintNodeMenu");

		} else {
			pendingLinkPin = 0;
			popupGraphPos = screenToGraph(io.MousePos);
			searchBuffer[0] = 0;
			paletteFocusSearch = true;
			buildPalette();
			ImGui::OpenPopup("##BlueprintGraphMenu");
		}
	}
}


//
//	BlueprintEditor::handleKeyboard
//

void BlueprintEditor::handleKeyboard() {
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) ||
		ImGui::IsAnyItemActive() ||
		ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	bool ctrl = io.KeyCtrl || io.KeySuper;

	if (ImGui::IsKeyPressed(ImGuiKey_Delete) && HasSelection()) {
		DeleteSelected();

	} else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		ClearSelection();

	} else if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
		ZoomToFit();

	} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
		SelectAll();

	} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
		Copy();

	} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
		Cut();

	} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
		Paste();

	} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
		Duplicate();

	} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
		if (io.KeyShift) {
			Redo();

		} else {
			Undo();
		}

	} else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
		Redo();
	}
}


//
//	BlueprintEditor::buildPalette
//

void BlueprintEditor::buildPalette() {
	palette.clear();

	auto signatureForFunction = [](const Function& function, const std::string& className, std::vector<std::pair<PinType, bool>>& pins) {
		if (!function.isPure) {
			pins.push_back(std::make_pair(PinType(PinKind::Exec), false));
			pins.push_back(std::make_pair(PinType(PinKind::Exec), true));
		}

		if (!function.isStatic) {
			pins.push_back(std::make_pair(PinType(PinKind::Object, className), false));
		}

		for (auto& parameter : function.parameters) {
			pins.push_back(std::make_pair(parameter.type, parameter.isOutput));
		}
	};

	// events from the blueprint's parent class chain
	std::unordered_set<std::string> seenEvents;
	const Class* cls = registry.FindClass(blueprintParentClass);
	int depth = 0;

	while (cls && depth++ < 64) {
		for (auto& event : cls->events) {
			if (seenEvents.insert(event.name).second) {
				PaletteAction paletteAction;
				paletteAction.category = "Add Event";
				paletteAction.name = "Event " + event.name;
				paletteAction.keywords = event.keywords + " " + cls->name;
				paletteAction.pins.push_back(std::make_pair(PinType(PinKind::Exec), true));

				for (auto& parameter : event.parameters) {
					paletteAction.pins.push_back(std::make_pair(parameter.type, true));
				}

				std::string className = cls->name;
				std::string eventName = event.name;
				paletteAction.spawn = [this, className, eventName](const ImVec2& pos) { return AddEventNode(className, eventName, pos); };
				palette.push_back(std::move(paletteAction));
			}
		}

		cls = registry.FindClass(cls->parentName);
	}

	{
		PaletteAction paletteAction;
		paletteAction.category = "Add Event";
		paletteAction.name = "Add Custom Event...";
		paletteAction.keywords = "custom event";
		paletteAction.pins.push_back(std::make_pair(PinType(PinKind::Exec), true));
		paletteAction.spawn = [this](const ImVec2& pos) { return AddCustomEventNode("", pos); };
		palette.push_back(std::move(paletteAction));
	}

	// flow control nodes
	for (auto& def : flowDefinitions()) {
		PaletteAction paletteAction;
		paletteAction.category = "Flow Control";
		paletteAction.name = def.name;
		paletteAction.keywords = "flow macro";

		for (auto& pinDef : def.pins) {
			paletteAction.pins.push_back(std::make_pair(PinType(pinDef.kind), pinDef.isOutput));
		}

		std::string name = def.name;
		paletteAction.spawn = [this, name](const ImVec2& pos) { return AddFlowControlNode(name, pos); };
		palette.push_back(std::move(paletteAction));
	}

	// functions and properties from all registered classes
	for (auto& registered : registry.GetClasses()) {
		// On-demand SDK classes (Cast / contextual pick / Expose) are reachable when
		// dragging from a typed pin, but must NOT flood the flat "All Actions" list.
		if (registered->paletteHidden) {
			continue;
		}

		for (auto& function : registered->functions) {
			PaletteAction paletteAction;
			paletteAction.category = function.category.empty() ? registered->name : function.category;
			paletteAction.name = function.name;
			paletteAction.keywords = function.keywords + " " + registered->name;
			signatureForFunction(function, registered->name, paletteAction.pins);

			std::string className = registered->name;
			std::string functionName = function.name;
			paletteAction.spawn = [this, className, functionName](const ImVec2& pos) { return AddCallFunctionNode(className, functionName, pos); };
			palette.push_back(std::move(paletteAction));
		}

		for (auto& property : registered->properties) {
			std::string className = registered->name;
			std::string propertyName = property.name;

			PaletteAction get;
			get.category = "Variables|" + registered->name;
			get.name = "Get " + property.name;
			get.keywords = "property " + registered->name;
			get.pins.push_back(std::make_pair(PinType(PinKind::Object, className), false));
			get.pins.push_back(std::make_pair(property.type, true));
			get.spawn = [this, className, propertyName](const ImVec2& pos) { return AddPropertyGetNode(className, propertyName, pos); };
			palette.push_back(std::move(get));

			PaletteAction set;
			set.category = "Variables|" + registered->name;
			set.name = "Set " + property.name;
			set.keywords = "property " + registered->name;
			set.pins.push_back(std::make_pair(PinType(PinKind::Exec), false));
			set.pins.push_back(std::make_pair(PinType(PinKind::Exec), true));
			set.pins.push_back(std::make_pair(PinType(PinKind::Object, className), false));
			set.pins.push_back(std::make_pair(property.type, false));
			set.pins.push_back(std::make_pair(property.type, true));
			set.spawn = [this, className, propertyName](const ImVec2& pos) { return AddPropertySetNode(className, propertyName, pos); };
			palette.push_back(std::move(set));
		}
	}

	// blueprint member variables
	for (auto& variable : variables) {
		std::string variableName = variable.name;

		PaletteAction get;
		get.category = "Variables";
		get.name = "Get " + variable.name;
		get.keywords = "variable";
		get.pins.push_back(std::make_pair(variable.type, true));
		get.spawn = [this, variableName](const ImVec2& pos) { return AddVariableGetNode(variableName, pos); };
		palette.push_back(std::move(get));

		PaletteAction set;
		set.category = "Variables";
		set.name = "Set " + variable.name;
		set.keywords = "variable";
		set.pins.push_back(std::make_pair(PinType(PinKind::Exec), false));
		set.pins.push_back(std::make_pair(PinType(PinKind::Exec), true));
		set.pins.push_back(std::make_pair(variable.type, false));
		set.pins.push_back(std::make_pair(variable.type, true));
		set.spawn = [this, variableName](const ImVec2& pos) { return AddVariableSetNode(variableName, pos); };
		palette.push_back(std::move(set));

		PaletteAction lazySet;
		lazySet.category = "Variables";
		lazySet.name = "Set " + variable.name + " (If Unset)";
		lazySet.keywords = "variable lazy init or nil once";
		lazySet.pins.push_back(std::make_pair(PinType(PinKind::Exec), false));
		lazySet.pins.push_back(std::make_pair(PinType(PinKind::Exec), true));
		lazySet.pins.push_back(std::make_pair(variable.type, false));
		lazySet.pins.push_back(std::make_pair(variable.type, true));
		lazySet.spawn = [this, variableName](const ImVec2& pos) { return AddVariableSetIfUnsetNode(variableName, pos); };
		palette.push_back(std::move(lazySet));
	}

	// utilities
	{
		PaletteAction paletteAction;
		paletteAction.category = "Utilities";
		paletteAction.name = "Add Comment";
		paletteAction.keywords = "note text";
		paletteAction.spawn = [this](const ImVec2& pos) { return AddCommentNode("Comment", pos, ImVec2(400.0f, 250.0f)); };
		palette.push_back(std::move(paletteAction));
	}

	{
		PaletteAction paletteAction;
		paletteAction.category = "Utilities";
		paletteAction.name = "Add Reroute Node";
		paletteAction.keywords = "knot wire";
		paletteAction.pins.push_back(std::make_pair(PinType(PinKind::Wildcard), false));
		paletteAction.pins.push_back(std::make_pair(PinType(PinKind::Wildcard), true));

		paletteAction.spawn = [this](const ImVec2& pos) {
			PinType type(PinKind::Wildcard);
			const Pin* pending = findPin(pendingLinkPin);

			if (pending) {
				type = pending->type;
			}

			return AddRerouteNode(type, pos);
		};

		palette.push_back(std::move(paletteAction));
	}

	{
		PaletteAction paletteAction;
		paletteAction.category = "Utilities";
		paletteAction.name = "Add Custom Lua Node";
		paletteAction.keywords = "code script raw lua escape hatch";
		paletteAction.pins.push_back(std::make_pair(PinType(PinKind::Exec), false));
		paletteAction.pins.push_back(std::make_pair(PinType(PinKind::Exec), true));
		paletteAction.spawn = [this](const ImVec2& pos) { return AddCustomLuaNode(pos); };
		palette.push_back(std::move(paletteAction));
	}

	std::sort(palette.begin(), palette.end(), [](const PaletteAction& a, const PaletteAction& b) {
		return a.category == b.category ? a.name < b.name : a.category < b.category;
	});
}


//
//	BlueprintEditor::paletteActionMatchesPin
//

bool BlueprintEditor::paletteActionMatchesPin(const PaletteAction& paletteAction, const Pin& pin) const {
	for (auto& candidate : paletteAction.pins) {
		if (candidate.second != pin.isOutput) {
			bool compatible = pin.isOutput ?
				typesCompatible(pin.type, candidate.first) :
				typesCompatible(candidate.first, pin.type);

			if (compatible) {
				return true;
			}
		}
	}

	return false;
}


//
//	BlueprintEditor::RenderPalettePanel
//
//	The UE-style palette: every registered node grouped by category, plus a
//	browsable SDK class list (from the imported side index) whose members can be
//	dropped straight onto the canvas. Clicking spawns at the next free slot.
//

void BlueprintEditor::RenderPalettePanel() {
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##palSearch", "Search nodes + classes...", paletteSearchBuffer,
	                         sizeof(paletteSearchBuffer));
	ImGui::Separator();

	std::string search = toLower(paletteSearchBuffer);

	ImGui::BeginChild("##palList", ImVec2(0.0f, 0.0f));

	auto spawnAt = [this](const PaletteAction& entry) {
		ID id = entry.spawn(NextSpawnPos());

		if (id) {
			SelectNode(id);
		}
	};

	// ── registered nodes, grouped by category ──────────────────────────────
	std::map<std::string, std::vector<const PaletteAction*>> groups;

	for (auto& entry : palette) {
		if (!search.empty()) {
			std::string hay = toLower(entry.name + " " + entry.category + " " + entry.keywords);

			if (hay.find(search) == std::string::npos) {
				continue;
			}
		}

		groups[entry.category].push_back(&entry);
	}

	for (auto& group : groups) {
		// A search implies you want to see the hits — open the groups then.
		ImGuiTreeNodeFlags flags = search.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen;

		if (ImGui::TreeNodeEx(group.first.c_str(), flags)) {
			for (auto entry : group.second) {
				ImGui::PushID(entry);

				if (ImGui::Selectable(entry->name.c_str())) {
					spawnAt(*entry);
				}

				ImGui::PopID();
			}

			ImGui::TreePop();
		}
	}

	// ── SDK classes (imported dump) ────────────────────────────────────────
	// Gated on a search: a full game dump is 10k classes and listing them all
	// would be unusable (and is exactly the flood we removed from the palette).
	if (auxRegistry) {
		ImGui::Separator();

		if (search.size() < 2) {
			ImGui::TextDisabled("SDK classes: type 2+ chars to search");

		} else if (ImGui::TreeNodeEx("SDK Classes", ImGuiTreeNodeFlags_DefaultOpen)) {
			int shown = 0;

			for (auto& cls : auxRegistry->GetClasses()) {
				if (shown >= 40) {
					ImGui::TextDisabled("(more — refine the search)");
					break;
				}

				if (toLower(cls->name).find(search) == std::string::npos) {
					continue;
				}

				++shown;
				ImGui::PushID(cls.get());

				if (ImGui::TreeNode(cls->name.c_str())) {
					for (auto& fn : cls->functions) {
						ImGui::PushID(&fn);

						if (ImGui::Selectable(fn.name.c_str())) {
							ensureClassAvailable(cls->name);
							ID id = AddCallFunctionNode(cls->name, fn.name, NextSpawnPos());

							if (id) {
								SelectNode(id);
							}
						}

						ImGui::PopID();
					}

					for (auto& prop : cls->properties) {
						ImGui::PushID(&prop);
						std::string label = "Get " + prop.name;

						if (ImGui::Selectable(label.c_str())) {
							ensureClassAvailable(cls->name);
							ID id = AddPropertyGetNode(cls->name, prop.name, NextSpawnPos());

							if (id) {
								SelectNode(id);
							}
						}

						ImGui::PopID();
					}

					ImGui::TreePop();
				}

				ImGui::PopID();
			}

			if (shown == 0) {
				ImGui::TextDisabled("(no class matches)");
			}

			ImGui::TreePop();
		}
	}

	// Middle-mouse drag-scroll, same as every other surface.
	if (panScrollHook) {
		panScrollHook(107);
	}

	ImGui::EndChild();
}


//
//	BlueprintEditor::renderGraphContextMenu
//

void BlueprintEditor::renderGraphContextMenu() {
	// Fixed width. This popup right-aligns its "Context Sensitive" checkbox relative to the
	// content region; in an auto-resizing window (max width FLT_MAX) that feeds the window's
	// own width back into its content, so the checkbox's right edge lands a few px past the
	// current width and the menu grows every frame without limit until it's unusable. Pinning
	// min == max removes the free dimension and breaks the loop.
	ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));

	if (ImGui::BeginPopup("##BlueprintGraphMenu")) {
		const Pin* pending = findPin(pendingLinkPin);

		if (pending) {
			ImGui::TextDisabled("Actions with a %s pin", pinTypeLabel(pending->type).c_str());

		} else {
			ImGui::TextDisabled("All Actions for this Blueprint");
		}

		float checkboxWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Context Sensitive").x;
		ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionAvail().x - checkboxWidth));
		ImGui::Checkbox("Context Sensitive", &contextSensitive);

		if (paletteFocusSearch) {
			ImGui::SetKeyboardFocusHere();
			paletteFocusSearch = false;
		}

		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##search", "Search", searchBuffer, sizeof(searchBuffer));
		ImGui::Separator();

		// Context-sensitive "Make <Struct>" (Unreal UX): dragging from a struct input
		// pin offers a Make Struct node whose { field = value } table output feeds it.
		if (pending && !pending->isOutput && pending->type.kind == PinKind::Struct &&
			!pending->type.subtype.empty() && registry.FindClass(pending->type.subtype)) {
			std::string makeLabel = "Make " + pending->type.subtype;

			if (ImGui::Selectable(makeLabel.c_str())) {
				ID id = AddMakeStructNode(pending->type.subtype, popupGraphPos);

				if (id) {
					SelectNode(id);
					Node* node = findNode(id);

					if (node && pendingLinkPin) {
						autoConnectPendingToNode(pendingLinkPin, *node);
					}
				}

				pendingLinkPin = 0;
				ImGui::CloseCurrentPopup();
			}

			ImGui::Separator();
		}

		// Class-contextual members (Unreal UX): dragging from a TYPED object pin lists
		// that class's functions + properties — pulled from the live registry AND the
		// imported SDK side index (auxRegistry), so SDK members are reachable on the
		// object that has them WITHOUT flooding the global palette. The class comes from
		// the pin's subtype (established by an event's typed output, a Cast node, or a
		// get-objects-by-class result). Members are copied into the registry on first use.
		std::string search = toLower(searchBuffer);

		if (pending && pending->isOutput &&
			(pending->type.kind == PinKind::Object || isClassLike(pending->type)) &&
			!pending->type.subtype.empty()) {
			// Walk the class + ancestors, gathering the class from BOTH the live
			// registry AND the SDK index at each step. Prefix folding means a pin
			// typed "AActor" (built-in) also pulls the dumped "Actor" and its members,
			// so SpawnActor's output offers Actor's SDK methods without flooding the
			// flat palette. Sources are {ownerClassName, Class*}.
			std::vector<std::pair<std::string, const Class*>> sources;
			std::string walk = pending->type.subtype;

			for (int guard = 0; guard < 64 && !walk.empty(); ++guard) {
				const Class* r = registry.FindClass(walk);
				const Class* a = auxRegistry ? auxRegistry->FindClass(walk) : nullptr;
				const Class* step = r ? r : a; // drive the parent walk off whichever exists

				if (!step) {
					break;
				}

				if (r) {
					sources.push_back(std::make_pair(r->name, r));
				}

				if (a && a != r) {
					sources.push_back(std::make_pair(a->name, a));
				}

				walk = step->parentName;
			}

			if (!sources.empty()) {
				std::string header = "Members of " + pending->type.subtype;
				ImGui::SetNextItemOpen(true, ImGuiCond_Once);

				if (ImGui::TreeNode(header.c_str())) {
					int shown = 0;
					std::set<std::string> seenMembers; // dedup same-named members across sources
					auto matchesSearch = [&](const std::string& n) {
						return search.empty() || toLower(n).find(search) != std::string::npos;
					};

					for (auto& srcPair : sources) {
						const std::string& clsName = srcPair.first;
						const Class* c = srcPair.second;

						for (auto& fn : c->functions) {
							if (shown >= 300 || !matchesSearch(fn.name) || !seenMembers.insert("f:" + fn.name).second) {
								continue;
							}

							std::string label = fn.name + "##fn_" + clsName;

							if (ImGui::Selectable(label.c_str())) {
								ensureClassAvailable(clsName);
								ID id = AddCallFunctionNode(clsName, fn.name, popupGraphPos);

								if (id) {
									SelectNode(id);

									if (Node* n = findNode(id)) {
										if (pendingLinkPin) {
											autoConnectPendingToNode(pendingLinkPin, *n);
										}
									}
								}

								pendingLinkPin = 0;
								ImGui::CloseCurrentPopup();
							}

							++shown;
						}

						for (auto& prop : c->properties) {
							if (shown >= 300 || !matchesSearch(prop.name) || !seenMembers.insert("p:" + prop.name).second) {
								continue;
							}

							std::string label = "Get " + prop.name + "##prop_" + clsName;

							if (ImGui::Selectable(label.c_str())) {
								ensureClassAvailable(clsName);
								ID id = AddPropertyGetNode(clsName, prop.name, popupGraphPos);

								if (id) {
									SelectNode(id);

									if (Node* n = findNode(id)) {
										if (pendingLinkPin) {
											autoConnectPendingToNode(pendingLinkPin, *n);
										}
									}
								}

								pendingLinkPin = 0;
								ImGui::CloseCurrentPopup();
							}

							++shown;
						}
					}

					if (shown == 0) {
						ImGui::TextDisabled(search.empty() ? "(no members)" : "(no member matches)");
					}

					ImGui::TreePop();
				}

				ImGui::Separator();
			}
		}

		// "Cast To <Class>" (establishes an object's class so its members become
		// reachable): when dragging from an object output, type a class name in the
		// search box to cast to it. Sourced from the SDK index; capped.
		if (pending && pending->isOutput && pending->type.kind == PinKind::Object &&
			auxRegistry && search.size() >= 2) {
			int castsShown = 0;

			if (ImGui::TreeNodeEx("Cast To...", ImGuiTreeNodeFlags_DefaultOpen)) {
				for (auto& c : auxRegistry->GetClasses()) {
					if (castsShown >= 20) {
						ImGui::TextDisabled("(refine the search)");
						break;
					}

					if (toLower(c->name).find(search) == std::string::npos) {
						continue;
					}

					std::string label = "Cast To " + c->name + "##cast";

					if (ImGui::Selectable(label.c_str())) {
						ID id = AddCastNode(c->name, popupGraphPos);

						if (id) {
							SelectNode(id);

							if (Node* n = findNode(id)) {
								if (pendingLinkPin) {
									autoConnectPendingToNode(pendingLinkPin, *n);
								}
							}
						}

						pendingLinkPin = 0;
						ImGui::CloseCurrentPopup();
					}

					++castsShown;
				}

				ImGui::TreePop();
			}

			ImGui::Separator();
		}

		// filter the palette
		std::vector<const PaletteAction*> filtered;

		for (auto& paletteAction : palette) {
			if (pending && contextSensitive && !paletteActionMatchesPin(paletteAction, *pending)) {
				continue;
			}

			if (!search.empty()) {
				std::string haystack = toLower(paletteAction.name + " " + paletteAction.category + " " + paletteAction.keywords);
				size_t start = 0;
				bool match = true;

				while (start < search.size() && match) {
					size_t end = search.find(' ', start);

					if (end == std::string::npos) {
						end = search.size();
					}

					if (end > start && haystack.find(search.substr(start, end - start)) == std::string::npos) {
						match = false;
					}

					start = end + 1;
				}

				if (!match) {
					continue;
				}
			}

			filtered.push_back(&paletteAction);
		}

		const PaletteAction* activated = nullptr;
		ImGui::BeginChild("##actions", ImVec2(0.0f, 360.0f));

		if (search.empty()) {
			// grouped by category
			std::map<std::string, std::vector<const PaletteAction*>> groups;

			for (auto paletteAction : filtered) {
				groups[paletteAction->category].push_back(paletteAction);
			}

			for (auto& group : groups) {
				ImGui::SetNextItemOpen(true, ImGuiCond_Once);

				if (ImGui::TreeNode(group.first.c_str())) {
					for (auto paletteAction : group.second) {
						if (ImGui::Selectable(paletteAction->name.c_str())) {
							activated = paletteAction;
						}
					}

					ImGui::TreePop();
				}
			}

		} else {
			for (auto paletteAction : filtered) {
				std::string label = paletteAction->name + "##" + paletteAction->category;

				if (ImGui::Selectable(label.c_str())) {
					activated = paletteAction;
				}

				ImGui::SameLine();
				ImGui::TextDisabled("(%s)", paletteAction->category.c_str());
			}
		}

		// Middle-mouse drag-scroll the node list, like every other scroll surface.
		if (panScrollHook) {
			panScrollHook(106);
		}

		ImGui::EndChild();

		if (activated) {
			ID id = activated->spawn(popupGraphPos);

			if (id) {
				SelectNode(id);

				// auto-connect to the pin the palette was opened from
				if (pendingLinkPin) {
					Node* node = findNode(id);

					if (node) {
						autoConnectPendingToNode(pendingLinkPin, *node);
					}
				}
			}

			pendingLinkPin = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}


//
//	BlueprintEditor::renderNodeContextMenu
//

void BlueprintEditor::renderNodeContextMenu() {
	bool openRename = false;

	if (ImGui::BeginPopup("##BlueprintNodeMenu")) {
		Node* node = findNode(contextNode);

		if (!node) {
			ImGui::CloseCurrentPopup();

		} else {
			ImGui::TextDisabled("%s", node->title.empty() ? nodeKindName(node->kind) : node->title.c_str());
			ImGui::Separator();

			if (ImGui::MenuItem("Delete", "Del")) {
				if (selectedNodes.count(contextNode)) {
					DeleteSelected();

				} else {
					DeleteNode(contextNode);
				}
			}

			if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
				if (!selectedNodes.count(contextNode)) {
					SelectNode(contextNode);
				}

				Duplicate();
			}

			bool hasLinks = false;

			for (auto& pin : node->pins) {
				if (pinLinkCount(pin.id)) {
					hasLinks = true;
				}
			}

			if (ImGui::MenuItem("Break All Links", nullptr, false, hasLinks)) {
				std::vector<ID> pins;

				for (auto& pin : node->pins) {
					pins.push_back(pin.id);
				}

				for (auto pin : pins) {
					breakPinLinksInternal(pin);
				}

				recordUndo();
				dirty = true;
			}

			if (node->kind == NodeKind::FlowControl && node->memberName == "Sequence") {
				if (ImGui::MenuItem("Add Sequence Pin")) {
					int count = 0;

					for (auto& pin : node->pins) {
						if (pin.isOutput) {
							count++;
						}
					}

					addPin(*node, "Then " + std::to_string(count), PinType(PinKind::Exec), true);
					rebuildIndex();
					recordUndo();
					dirty = true;
				}
			}

			if (node->kind == NodeKind::Comment || node->kind == NodeKind::CustomEvent || node->kind == NodeKind::CustomLua) {
				if (ImGui::MenuItem("Rename...")) {
					renameNode = contextNode;
					std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", node->title.c_str());
					openRename = true;
				}
			}
		}

		ImGui::EndPopup();
	}

	if (openRename) {
		ImGui::OpenPopup("##BlueprintNodeRename");
	}

	if (ImGui::BeginPopup("##BlueprintNodeRename")) {
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}

		bool done = ImGui::InputText("##rename", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		done |= ImGui::Button("OK");

		if (done) {
			Node* node = findNode(renameNode);

			if (node && renameBuffer[0]) {
				node->title = renameBuffer;

				if (node->kind == NodeKind::CustomEvent) {
					node->memberName = renameBuffer;
				}

				recordUndo();
				dirty = true;
			}

			renameNode = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}
