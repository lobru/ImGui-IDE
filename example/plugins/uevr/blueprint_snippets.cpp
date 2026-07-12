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
} // namespace

namespace BlueprintSnippets
{
const std::vector<Snippet> &All()
{
    static const std::vector<Snippet> snippets = {
        {"For Loop (print index)", "Counted loop 0..N that prints each index.", insertForLoop},
        {"While Loop", "Loop while a condition holds (wire the Condition).", insertWhileLoop},
        {"Branch", "If/else on a boolean condition.", insertBranch},
        {"Sequence", "Run several exec outputs in order.", insertSequence},
    };
    return snippets;
}
} // namespace BlueprintSnippets
