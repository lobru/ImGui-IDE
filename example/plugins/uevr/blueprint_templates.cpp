//
//  blueprint_templates.cpp — see blueprint_templates.h.
//
//  Each template builds against the UEVR registry SetupUEVRRegistry installs, so
//  the class/function/pin names below must match it (return pins are named
//  "Return Value", the self pin "Target", exec pins have an empty name).
//

#include <imgui.h> // ImVec2

#include "BlueprintEditor.h"

#include "blueprint_templates.h"

namespace
{
using ID = BlueprintEditor::ID;

// "Pre Engine Tick -> Print": the hello-world of UEVR scripting.
void buildHello(BlueprintEditor &bp)
{
    bp.ClearGraph();
    ID tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(-260.0f, 0.0f));
    ID print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(140.0f, 0.0f));
    bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(print, "", false));
    bp.SetPinDefaultValue(bp.FindPinID(print, "Message", false), "Hello from UEVR");
}

// "Draw UI -> Begin/Text/End Window": an on-screen ImGui panel drawn each frame.
void buildDrawUi(BlueprintEditor &bp)
{
    bp.ClearGraph();
    ID draw = bp.AddEventNode("UEVR", "Draw UI", ImVec2(-360.0f, 0.0f));
    ID begin = bp.AddCallFunctionNode("ImGui", "Begin Window", ImVec2(-60.0f, 0.0f));
    ID text = bp.AddCallFunctionNode("ImGui", "Text", ImVec2(240.0f, 0.0f));
    ID end = bp.AddCallFunctionNode("ImGui", "End Window", ImVec2(520.0f, 0.0f));
    bp.AddLink(bp.FindPinID(draw, "", true), bp.FindPinID(begin, "", false));
    bp.AddLink(bp.FindPinID(begin, "", true), bp.FindPinID(text, "", false));
    bp.AddLink(bp.FindPinID(text, "", true), bp.FindPinID(end, "", false));
    bp.SetPinDefaultValue(bp.FindPinID(begin, "Name", false), "My UEVR Panel");
    bp.SetPinDefaultValue(bp.FindPinID(text, "Text", false), "Hello, VR!");
}

// "Pre Engine Tick -> Print(pawn:get_full_name())": data flow through pure nodes.
void buildLogPawn(BlueprintEditor &bp)
{
    bp.ClearGraph();
    ID tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(-380.0f, 0.0f));
    ID pawn = bp.AddCallFunctionNode("UEVR_API", "Get Local Pawn", ImVec2(-380.0f, 170.0f));
    ID name = bp.AddCallFunctionNode("UObject", "Get Full Name", ImVec2(-60.0f, 170.0f));
    ID print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(260.0f, 0.0f));
    bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(print, "", false));                     // exec
    bp.AddLink(bp.FindPinID(pawn, "Return Value", true), bp.FindPinID(name, "Target", false));    // data
    bp.AddLink(bp.FindPinID(name, "Return Value", true), bp.FindPinID(print, "Message", false));  // data
}
} // namespace

namespace BlueprintTemplates
{
const std::vector<Template> &All()
{
    static const std::vector<Template> templates = {
        {"Hello (log each tick)", "Print a message every engine tick.", buildHello},
        {"Draw a UI panel", "Draw an on-screen ImGui window every frame.", buildDrawUi},
        {"Log the player pawn", "Print the local pawn's full name each tick.", buildLogPawn},
    };
    return templates;
}
} // namespace BlueprintTemplates
