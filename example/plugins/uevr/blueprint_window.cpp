//
//  blueprint_window.cpp — the dockable "Blueprint Editor" window.
//
//  Its body is the BlueprintEditor node-graph widget, its registry seeded with
//  the UEVR Lua scripting API so the palette offers UEVR nodes and "Generate
//  UEVR Lua" walks the graph into a runnable script opened as a new Lua tab.
//  Being a normal dockable window it inherits the app's multi-viewport docking;
//  the canvas pan honors the host's invert-pan setting.
//

#include <imgui.h>

#include "BlueprintEditor.h"
#include "BlueprintLua.h"
#include "BlueprintLuaImport.h"

#include "uevr_plugin.h"

BlueprintEditor &UevrPlugin::ensureBlueprintEditor()
{
    // Lazily create the widget on first use and seed it with the UEVR Lua API (this also
    // sets the blueprint parent class to "UEVR"). Shared by the window itself and by
    // insertLiveValueAsNode, so an "insert as node" click works even if the Blueprint
    // window was never opened this session.
    if (!blueprintEditor)
    {
        blueprintEditor = std::make_unique<BlueprintEditor>();
        BlueprintLua::SetupUEVRRegistry(*blueprintEditor);
        blueprintEditor->SetBlueprint("UEVRScript", "UEVR");
    }
    return *blueprintEditor;
}

void UevrPlugin::renderBlueprintWindow(PluginHost &host)
{
    if (!blueprintVisible)
        return;

    auto &bp = ensureBlueprintEditor();
    bp.SetPanInverted(host.hostPanInverted()); // every pan surface honors the app setting

    ImGui::SetNextWindowSize(ImVec2(1100.0f, 680.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Blueprint Editor###blueprintEditor", &blueprintVisible, ImGuiWindowFlags_MenuBar))
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Graph"))
            {
                if (ImGui::MenuItem("New / Clear Graph"))
                    bp.ClearGraph();
                ImGui::Separator();
                if (ImGui::MenuItem("Save Snapshot"))
                    blueprintSnapshot = bp.SaveToString();
                if (ImGui::MenuItem("Load Snapshot", nullptr, nullptr, !blueprintSnapshot.empty()))
                    bp.LoadFromString(blueprintSnapshot);
                ImGui::Separator();
                if (ImGui::MenuItem("Import Lua (Active Doc)"))
                {
                    std::string source = host.hostActiveText();
                    std::string error;
                    if (source.empty())
                        host.hostToast("No active document to import");
                    else if (!BlueprintLuaImport::ImportScript(bp, source, error))
                        host.hostError("Import failed: " + error);
                    else
                        host.hostToast("Imported Lua into the graph");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Best-effort: recognized statements become nodes, everything else becomes a Custom Lua node");
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", nullptr, bp.CanUndo()))
                    bp.Undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", nullptr, bp.CanRedo()))
                    bp.Redo();
                ImGui::Separator();
                if (ImGui::MenuItem("Cut", "Ctrl+X", nullptr, bp.HasSelection()))
                    bp.Cut();
                if (ImGui::MenuItem("Copy", "Ctrl+C", nullptr, bp.HasSelection()))
                    bp.Copy();
                if (ImGui::MenuItem("Paste", "Ctrl+V"))
                    bp.Paste();
                if (ImGui::MenuItem("Duplicate", "Ctrl+D", nullptr, bp.HasSelection()))
                    bp.Duplicate();
                if (ImGui::MenuItem("Delete", "Del", nullptr, bp.HasSelection()))
                    bp.DeleteSelected();
                ImGui::Separator();
                if (ImGui::MenuItem("Select All", "Ctrl+A"))
                    bp.SelectAll();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Zoom to Fit", "Home"))
                    bp.ZoomToFit();
                bool grid = bp.IsShowingGrid();
                if (ImGui::MenuItem("Show Grid", nullptr, &grid))
                    bp.SetShowGrid(grid);
                bool ctx = bp.IsContextSensitive();
                if (ImGui::MenuItem("Context Sensitive Menu", nullptr, &ctx))
                    bp.SetContextSensitive(ctx);
                ImGui::EndMenu();
            }

            // The headline action: turn the graph into a runnable UEVR Lua
            // script and open it as a new Lua editor tab.
            if (ImGui::MenuItem("Generate UEVR Lua"))
            {
                std::string lua = BlueprintLua::GenerateScript(bp);
                host.hostOpenLuaTab(lua);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Walk the graph into a UEVR Lua script and open it in a new tab");

            if (ImGui::MenuItem("Run in UEVR"))
            {
                // Generate + send straight to a running game via the UEVR Live bridge.
                sendUevr("run", BlueprintLua::GenerateScript(bp));
                uevrLiveVisible = true; // surface the output
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Generate the script and run it in a running UEVR game (see UEVR Live)");

            ImGui::EndMenuBar();
        }

        bp.Render("BlueprintCanvas");
    }
    ImGui::End();
}
