//	BlueprintEditor - An Unreal Engine style Blueprint visual scripting
//	editor for Dear ImGui.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


#pragma once


//
//	Include files
//

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"


//
//	BlueprintEditor
//
//	A single-widget node graph editor that mimics Unreal Engine's Blueprint
//	editor: event/function/variable nodes generated from a UObject-style
//	reflection registry, exec and data pins with Unreal's type colors,
//	bezier wires, pan/zoom, box select, a searchable context-sensitive
//	palette, comments, reroute nodes, copy/paste and undo/redo.
//
//	The reflection registry is a minimal mirror of Unreal's UClass/UFunction/
//	FProperty model. In an engine build it can be populated from real UObject
//	reflection data; standalone it is filled by hand (SetupDefaultRegistry
//	provides a sample UObject/AActor hierarchy plus a few kismet libraries).
//

class IMGUI_API BlueprintEditor {
public:
	// all nodes, pins and links have a graph-wide unique ID
	using ID = int;

	//
	// Pin types (modeled after Unreal's Blueprint pin categories)
	//

	enum class PinKind {
		Exec,
		Boolean,
		Byte,
		Integer,
		Float,
		String,
		Name,
		Vector,
		Rotator,
		Transform,
		Object,
		Class,
		Struct,
		Enum,
		Delegate,
		Wildcard
	};

	struct PinType {
		PinType() = default;
		PinType(PinKind k, const std::string& sub = "", bool array = false) : kind(k), subtype(sub), isArray(array) {}

		PinKind kind = PinKind::Exec;
		std::string subtype; // class name for Object/Class pins, struct name for Struct pins, enum name for Enum pins
		bool isArray = false;
	};

	//
	// UObject-style reflection model
	//

	struct Parameter {
		std::string name;
		PinType type;
		bool isOutput = false;
		std::string defaultValue;
	};

	struct Function {
		std::string name;
		std::string category;
		std::string tooltip;
		std::string keywords;
		std::string metadata; // free-form binding data for code generators (e.g. a Lua expression template)
		bool isPure = false;
		bool isStatic = false;
		std::vector<Parameter> parameters;

		// fluent builder helpers
		Function& In(const std::string& pname, const PinType& ptype, const std::string& def = "") {
			parameters.push_back(Parameter{pname, ptype, false, def});
			return *this;
		}

		Function& Out(const std::string& pname, const PinType& ptype) {
			parameters.push_back(Parameter{pname, ptype, true, ""});
			return *this;
		}

		Function& Ret(const PinType& ptype) { return Out("Return Value", ptype); }
		Function& Pure() { isPure = true; return *this; }
		Function& Static() { isStatic = true; return *this; }
		Function& Keywords(const std::string& value) { keywords = value; return *this; }
		Function& Tooltip(const std::string& value) { tooltip = value; return *this; }
		Function& Metadata(const std::string& value) { metadata = value; return *this; }
	};

	struct Property {
		std::string name;
		std::string category;
		std::string tooltip;
		PinType type;
	};

	struct Class {
		std::string name;
		std::string parentName;
		std::string tooltip;
		std::string metadata; // free-form binding data for code generators (e.g. a Lua target expression)
		std::vector<Property> properties;
		std::vector<Function> functions;
		std::vector<Function> events;

		// fluent builder helpers
		Function& AddFunction(const std::string& fname, const std::string& category = "") {
			functions.push_back(Function());
			functions.back().name = fname;
			functions.back().category = category;
			return functions.back();
		}

		Function& AddEvent(const std::string& ename) {
			events.push_back(Function());
			events.back().name = ename;
			return events.back();
		}

		Class& AddProperty(const std::string& pname, const PinType& ptype, const std::string& category = "", const std::string& tooltip2 = "") {
			properties.push_back(Property{pname, category, tooltip2, ptype});
			return *this;
		}

		Class& Metadata(const std::string& value) {
			metadata = value;
			return *this;
		}
	};

	struct Enumeration {
		std::string name;
		std::vector<std::string> values;
	};

	class TypeRegistry {
	public:
		Class& AddClass(const std::string& name, const std::string& parent = "", const std::string& tooltip = "");
		Enumeration& AddEnum(const std::string& name, const std::vector<std::string>& values);

		const Class* FindClass(const std::string& name) const;
		const Enumeration* FindEnum(const std::string& name) const;
		const Function* FindFunction(const std::string& className, const std::string& functionName) const;
		const Function* FindEvent(const std::string& className, const std::string& eventName) const;
		const Property* FindProperty(const std::string& className, const std::string& propertyName) const;

		// does the walk up the parent chain: IsChildOf("ACharacter", "AActor") is true
		bool IsChildOf(const std::string& child, const std::string& parent) const;

		inline const std::vector<std::unique_ptr<Class>>& GetClasses() const { return classes; }
		inline const std::vector<std::unique_ptr<Enumeration>>& GetEnums() const { return enums; }
		inline void Clear() { classes.clear(); enums.clear(); }

	private:
		std::vector<std::unique_ptr<Class>> classes;
		std::vector<std::unique_ptr<Enumeration>> enums;
	};

	//
	// Graph model
	//

	enum class NodeKind {
		Event,
		CustomEvent,
		CallFunction,
		VariableGet,
		VariableSet,
		FlowControl,
		Reroute,
		Comment
	};

	struct Pin {
		ID id = 0;
		ID node = 0;
		std::string name;
		PinType type;
		bool isOutput = false;
		std::string defaultValue;

		// runtime state (managed by the editor while rendering)
		ImVec2 center; // screen space position of the pin icon
	};

	struct Node {
		ID id = 0;
		NodeKind kind = NodeKind::CallFunction;
		std::string className;  // owning class for events, functions and properties
		std::string memberName; // function/event/property/variable name or flow control node name
		std::string title;
		std::string subtitle;
		bool pure = false;
		ImVec2 pos;                             // graph space
		ImVec2 commentSize = ImVec2(0.0f, 0.0f); // comments only, graph space
		std::vector<Pin> pins;

		// runtime state (managed by the editor while rendering)
		ImVec2 size; // graph space, computed during rendering
	};

	struct Link {
		ID id = 0;
		ID from = 0; // an output pin
		ID to = 0;   // an input pin
	};

	struct Variable {
		std::string name;
		PinType type;
		std::string defaultValue;
	};

	// constructor
	BlueprintEditor();

	//
	// Below is the public API
	// Public member functions start with an uppercase character to be consistent with Dear ImGui
	//

	// access the reflection registry
	inline TypeRegistry& GetRegistry() { return registry; }
	inline const TypeRegistry& GetRegistry() const { return registry; }

	// populate the registry with a sample UObject/AActor hierarchy and kismet-style libraries
	void SetupDefaultRegistry();

	// set the identity of the blueprint being edited (name and parent UClass)
	void SetBlueprint(const std::string& name, const std::string& parentClass);
	inline const std::string& GetBlueprintName() const { return blueprintName; }
	inline const std::string& GetBlueprintParentClass() const { return blueprintParentClass; }

	// manage blueprint member variables
	bool AddVariable(const std::string& name, const PinType& type, const std::string& defaultValue = "");
	bool RemoveVariable(const std::string& name);
	inline const std::vector<Variable>& GetVariables() const { return variables; }

	// build a graph programmatically (all functions return the new node's ID or 0 on failure)
	ID AddEventNode(const std::string& className, const std::string& eventName, const ImVec2& pos);
	ID AddCustomEventNode(const std::string& name, const ImVec2& pos);
	ID AddCallFunctionNode(const std::string& className, const std::string& functionName, const ImVec2& pos);
	ID AddPropertyGetNode(const std::string& className, const std::string& propertyName, const ImVec2& pos);
	ID AddPropertySetNode(const std::string& className, const std::string& propertyName, const ImVec2& pos);
	ID AddVariableGetNode(const std::string& variableName, const ImVec2& pos);
	ID AddVariableSetNode(const std::string& variableName, const ImVec2& pos);
	ID AddFlowControlNode(const std::string& name, const ImVec2& pos); // "Branch", "Sequence", "For Loop", ...
	ID AddRerouteNode(const PinType& type, const ImVec2& pos);
	ID AddCommentNode(const std::string& text, const ImVec2& pos, const ImVec2& size);

	// find a pin on a node by name and direction (returns 0 when not found)
	ID FindPinID(ID node, const std::string& pinName, bool isOutput) const;

	// create/remove connections (AddLink validates like the interactive editor does)
	bool AddLink(ID fromPin, ID toPin);
	void BreakLink(ID link);
	void BreakPinLinks(ID pin);

	// edit operations
	void DeleteNode(ID node);
	void DeleteSelected();
	void ClearGraph();
	void SelectAll();
	void ClearSelection();
	void SelectNode(ID node, bool append = false);
	bool HasSelection() const { return !selectedNodes.empty() || !selectedLinks.empty(); }
	std::vector<ID> GetSelectedNodes() const;

	// clipboard operations (use the Dear ImGui clipboard)
	void Cut();
	void Copy();
	void Paste();
	void Duplicate();

	// undo/redo
	inline bool CanUndo() const { return undoIndex > 0; }
	inline bool CanRedo() const { return undoIndex + 1 < undoStack.size(); }
	void Undo();
	void Redo();

	// read access to the graph (for inspection and code generators)
	inline const std::vector<Node>& GetNodes() const { return nodes; }
	inline const std::vector<Link>& GetLinks() const { return links; }
	inline const Node* GetNode(ID id) const { return findNode(id); }
	inline const Pin* GetPin(ID id) const { return findPin(id); }

	// statistics and state
	inline size_t GetNodeCount() const { return nodes.size(); }
	inline size_t GetLinkCount() const { return links.size(); }
	inline bool IsDirty() const { return dirty; }
	inline void ClearDirty() { dirty = false; }

	// view control
	void ZoomToFit();
	inline float GetZoom() const { return zoom; }
	void SetZoom(float value);
	inline void SetShowGrid(bool value) { showGrid = value; }
	inline bool IsShowingGrid() const { return showGrid; }
	inline void SetContextSensitive(bool value) { contextSensitive = value; }
	inline bool IsContextSensitive() const { return contextSensitive; }
	inline void SetPanInverted(bool value) { panInverted = value; } // honor the host app's invert-pan setting
	inline bool IsPanInverted() const { return panInverted; }

	// serialization (a simple line-based text format)
	std::string SaveToString() const;
	bool LoadFromString(const std::string& text);

	// render the editor in the current window (call this every frame)
	void Render(const char* title, const ImVec2& size = ImVec2(), bool border = false);

private:
	// a parsed graph (used by load and paste)
	struct ParsedGraph {
		std::string name;
		std::string parent;
		std::vector<Variable> variables;
		std::vector<Node> nodes;
		std::vector<Link> links;
	};

	// an entry in the "add node" palette
	struct PaletteAction {
		std::string category;
		std::string name;
		std::string keywords;
		std::vector<std::pair<PinType, bool>> pins; // (type, isOutput) used for context-sensitive filtering
		std::function<ID(const ImVec2&)> spawn;
	};

	// interactive states
	enum class Action {
		none,
		dragNodes,
		boxSelect,
		dragLink,
		pan,
		resizeComment
	};

	// ID and lookup management
	inline ID makeID() { return nextID++; }
	void rebuildIndex();
	Node* findNode(ID id);
	const Node* findNode(ID id) const;
	Pin* findPin(ID id);
	const Pin* findPin(ID id) const;
	const Link* findLink(ID id) const;
	const Variable* findVariable(const std::string& name) const;

	// node construction helpers
	Node& createNode(NodeKind kind, const ImVec2& pos);
	void addPin(Node& node, const std::string& name, const PinType& type, bool isOutput, const std::string& defaultValue = "");
	void buildFunctionPins(Node& node, const Function& function, const std::string& className);
	ID finishNode(Node& node);

	// connection management
	bool canConnect(ID a, ID b, std::string& error) const;
	bool connect(ID fromPin, ID toPin);
	void breakLinkInternal(ID link);
	void breakPinLinksInternal(ID pin);
	void removeNodeInternal(ID node);
	int pinLinkCount(ID pin) const;
	bool wouldCreateDataCycle(ID fromNode, ID toNode) const;
	bool typesCompatible(const PinType& from, const PinType& to) const;

	// undo and change tracking
	void recordUndo();
	void resetUndo();

	// serialization helpers
	std::string serializeGraph(bool selectionOnly, const char* magic) const;
	bool parseGraphText(const std::string& text, const char* magic, ParsedGraph& result) const;
	void installParsed(ParsedGraph&& graph, bool includeIdentity);
	void applyState(const std::string& state);
	void pasteText(const std::string& text, const ImVec2& graphPos);
	std::string copySelectionToText() const;

	// coordinate transforms
	inline ImVec2 graphToScreen(const ImVec2& v) const { return ImVec2(canvasPos.x + scrolling.x + v.x * zoom, canvasPos.y + scrolling.y + v.y * zoom); }
	inline ImVec2 screenToGraph(const ImVec2& v) const { return ImVec2((v.x - canvasPos.x - scrolling.x) / zoom, (v.y - canvasPos.y - scrolling.y) / zoom); }

	// rendering and interaction
	void renderGrid();
	void renderNode(Node& node);
	void renderCommentNode(Node& node);
	void renderRerouteNode(Node& node);
	void renderDefaultEditor(Pin& pin, const ImVec2& pos, float width);
	float defaultEditorWidth(const Pin& pin) const;
	bool pinHasDefaultEditor(const Pin& pin) const;
	void renderPinIcon(const ImVec2& center, const PinType& type, bool connected) const;
	void handlePinInteraction(Pin& pin, const ImVec2& center);
	void renderLinks();
	void renderPendingLink();
	void renderBoxSelect();
	void renderOverlay();
	void handleCanvasInput();
	void handleKeyboard();
	void renderGraphContextMenu();
	void renderNodeContextMenu();
	void applyPendingViewChanges();
	void drawWire(const ImVec2& from, const ImVec2& to, ImU32 color, float thickness) const;
	float wireDistance(const ImVec2& from, const ImVec2& to, const ImVec2& point) const;

	// palette
	void buildPalette();
	bool paletteActionMatchesPin(const PaletteAction& action, const Pin& pin) const;

	// static description tables
	static const char* pinKindName(PinKind kind);
	static bool pinKindFromName(const std::string& name, PinKind& kind);
	static const char* nodeKindName(NodeKind kind);
	static bool nodeKindFromName(const std::string& name, NodeKind& kind);
	ImU32 pinColor(const PinType& type) const;
	std::string pinTypeLabel(const PinType& type) const;
	ImU32 headerColor(const Node& node) const;

	// reflection registry and blueprint identity
	TypeRegistry registry;
	std::string blueprintName = "NewBlueprint";
	std::string blueprintParentClass = "AActor";
	std::vector<Variable> variables;

	// the graph (nodes are stored in draw order, back to front)
	std::vector<Node> nodes;
	std::vector<Link> links;
	ID nextID = 1;
	std::unordered_map<ID, size_t> nodeIndex;
	std::unordered_map<ID, std::pair<size_t, size_t>> pinIndex;

	// selection
	std::unordered_set<ID> selectedNodes;
	std::unordered_set<ID> selectedLinks;

	// view state
	ImVec2 scrolling = ImVec2(0.0f, 0.0f);
	float zoom = 1.0f;
	bool showGrid = true;
	bool contextSensitive = true;
	bool panInverted = false;
	bool pendingZoomToFit = true;

	// undo state
	std::vector<std::string> undoStack;
	size_t undoIndex = 0;
	bool dirty = false;

	// per-frame state
	ImDrawList* drawList = nullptr;
	ImDrawListSplitter splitter;
	ImVec2 canvasPos;
	ImVec2 canvasSize;
	float baseFontSize = 16.0f;
	ID hoveredNode = 0;
	ID hoveredPin = 0;
	ID hoveredLink = 0;
	ID bringToFront = 0;
	bool edited = false; // set when a default value widget was edited this frame

	// interaction state
	Action action = Action::none;
	ID actionPin = 0;      // link drag source
	ID actionNode = 0;     // comment being resized
	ImVec2 actionOrigin;   // graph space anchor for box select
	ImVec2 rightClickPos;  // screen space right mouse down position
	bool nodesMoved = false;
	std::vector<ID> commentCapture; // nodes moved along with a comment

	// context menu state
	ImVec2 popupGraphPos;
	ID pendingLinkPin = 0;
	ID contextNode = 0;
	ID renameNode = 0;
	char searchBuffer[128] = {0};
	char renameBuffer[128] = {0};
	std::vector<PaletteAction> palette;
	bool paletteFocusSearch = false;
};
