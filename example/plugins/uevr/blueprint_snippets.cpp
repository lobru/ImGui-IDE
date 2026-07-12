//
//  blueprint_snippets.cpp — see blueprint_snippets.h.
//
//  Each snippet builds a cluster relative to bp.NextSpawnPos() (view-aware,
//  cascading) and does NOT clear the graph. Flow nodes (Branch/Sequence/For Loop/
//  While Loop) come from flowDefinitions(); their pin names must match.
//

#include <imgui.h> // ImVec2

#include "BlueprintEditor.h"

#include "blueprint_snippets.h"

namespace
{
using ID = BlueprintEditor::ID;

// "For Loop (0..N) -> print each Index": the canonical counted-loop pattern.
void insertForLoop(BlueprintEditor &bp)
{
    ImVec2 base = bp.NextSpawnPos();
    ID loop = bp.AddFlowControlNode("For Loop", base);
    ID toStr = bp.AddCallFunctionNode("LuaString", "To String", ImVec2(base.x + 280.0f, base.y + 90.0f));
    ID print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(base.x + 520.0f, base.y + 20.0f));
    bp.AddLink(bp.FindPinID(loop, "Loop Body", true), bp.FindPinID(print, "", false)); // body exec
    bp.AddLink(bp.FindPinID(loop, "Index", true), bp.FindPinID(toStr, "Value", false));
    bp.AddLink(bp.FindPinID(toStr, "Return Value", true), bp.FindPinID(print, "Message", false));
}

// "While Loop -> body": a while skeleton (wire the Condition + fill the body).
void insertWhileLoop(BlueprintEditor &bp)
{
    ImVec2 base = bp.NextSpawnPos();
    ID loop = bp.AddFlowControlNode("While Loop", base);
    ID print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(base.x + 300.0f, base.y + 20.0f));
    bp.AddLink(bp.FindPinID(loop, "Loop Body", true), bp.FindPinID(print, "", false));
    bp.SetPinDefaultValue(bp.FindPinID(print, "Message", false), "loop body");
}

// "Branch": a standalone conditional (wire the Condition + the True/False execs).
void insertBranch(BlueprintEditor &bp)
{
    bp.AddFlowControlNode("Branch", bp.NextSpawnPos());
}

// "Sequence": run several exec pins in order.
void insertSequence(BlueprintEditor &bp)
{
    bp.AddFlowControlNode("Sequence", bp.NextSpawnPos());
}

// "Append + iterate a table": Items table variable, Array Add, then For Each -> print.
void insertTableIterate(BlueprintEditor &bp)
{
    // an array-typed member variable (created on demand; AddVariable rejects dupes)
    bp.AddVariable("Items", BlueprintEditor::PinType(BlueprintEditor::PinKind::Wildcard, "", true), "");

    ImVec2 base = bp.NextSpawnPos();
    ID get1 = bp.AddVariableGetNode("Items", ImVec2(base.x, base.y + 90.0f));
    ID add = bp.AddCallFunctionNode("LuaTable", "Array Add", base);
    ID get2 = bp.AddVariableGetNode("Items", ImVec2(base.x + 240.0f, base.y + 170.0f));
    ID each = bp.AddFlowControlNode("For Each", ImVec2(base.x + 260.0f, base.y));
    ID lua = bp.AddCustomLuaNode(ImVec2(base.x + 560.0f, base.y));
    bp.AddCustomLuaPin(lua, false, "Item");
    bp.SetCustomLuaSource(lua, "print(tostring({Item}))");

    bp.AddLink(bp.FindPinID(get1, "", true), bp.FindPinID(add, "Array", false));
    bp.SetPinDefaultValue(bp.FindPinID(add, "Value", false), "hello");
    bp.AddLink(bp.FindPinID(add, "", true), bp.FindPinID(each, "", false));       // exec chain
    bp.AddLink(bp.FindPinID(get2, "", true), bp.FindPinID(each, "Array", false)); // data
    bp.AddLink(bp.FindPinID(each, "Loop Body", true), bp.FindPinID(lua, "", false));
    bp.AddLink(bp.FindPinID(each, "Element", true), bp.FindPinID(lua, "Item", false));
}

// "Iterate a map (pairs)": For Each (Pairs) with a print of key=value.
void insertMapIterate(BlueprintEditor &bp)
{
    bp.AddVariable("Lookup", BlueprintEditor::PinType(BlueprintEditor::PinKind::Wildcard, "Map"), "");

    ImVec2 base = bp.NextSpawnPos();
    ID get = bp.AddVariableGetNode("Lookup", ImVec2(base.x, base.y + 120.0f));
    ID each = bp.AddFlowControlNode("For Each (Pairs)", base);
    ID lua = bp.AddCustomLuaNode(ImVec2(base.x + 320.0f, base.y));
    bp.AddCustomLuaPin(lua, false, "K");
    bp.AddCustomLuaPin(lua, false, "V");
    bp.SetCustomLuaSource(lua, "print(tostring({K}) .. \"=\" .. tostring({V}))");

    bp.AddLink(bp.FindPinID(get, "", true), bp.FindPinID(each, "Table", false));
    bp.AddLink(bp.FindPinID(each, "Loop Body", true), bp.FindPinID(lua, "", false));
    bp.AddLink(bp.FindPinID(each, "Key", true), bp.FindPinID(lua, "K", false));
    bp.AddLink(bp.FindPinID(each, "Value", true), bp.FindPinID(lua, "V", false));
}

// "Guarded object fetch": Set If Unset(Get Local Pawn) -> Is Valid gate.
void insertGuardedPawn(BlueprintEditor &bp)
{
    bp.AddVariable("Pawn", BlueprintEditor::PinType(BlueprintEditor::PinKind::Object, "UObject"), "");

    ImVec2 base = bp.NextSpawnPos();
    ID fetch = bp.AddCallFunctionNode("UEVR_API", "Get Local Pawn", ImVec2(base.x, base.y + 130.0f));
    ID init = bp.AddVariableSetIfUnsetNode("Pawn", base);
    ID get = bp.AddVariableGetNode("Pawn", ImVec2(base.x + 260.0f, base.y + 130.0f));
    ID valid = bp.AddFlowControlNode("Is Valid", ImVec2(base.x + 280.0f, base.y));

    bp.AddLink(bp.FindPinID(fetch, "Return Value", true), bp.FindPinID(init, "Pawn", false));
    bp.AddLink(bp.FindPinID(init, "", true), bp.FindPinID(valid, "", false));
    bp.AddLink(bp.FindPinID(get, "", true), bp.FindPinID(valid, "Object", false));
}
} // namespace

namespace BlueprintSnippets
{
const std::vector<Snippet> &All()
{
    static const std::vector<Snippet> snippets = {
        {"For Loop (print index)", "Counted loop 0..N that prints each index.", insertForLoop},
        {"For Each over table", "Items table + Array Add + For Each -> print each element.", insertTableIterate},
        {"For Each over map (pairs)", "Lookup map + For Each (Pairs) -> print key=value.", insertMapIterate},
        {"Guarded pawn fetch", "Set If Unset(Get Local Pawn) feeding an Is Valid gate.", insertGuardedPawn},
        {"While Loop", "Loop while a condition holds (wire the Condition).", insertWhileLoop},
        {"Branch", "If/else on a boolean condition.", insertBranch},
        {"Sequence", "Run several exec outputs in order.", insertSequence},
    };
    return snippets;
}
} // namespace BlueprintSnippets
