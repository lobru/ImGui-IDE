//
//  blueprint_window.cpp — ImGui-IDE integration of the Blueprint visual
//  scripting editor (BlueprintEditor) + its UEVR Lua backend (BlueprintLua).
//
//  A dockable "Blueprint Editor" window whose body is the node-graph widget.
//  Its registry is seeded with the UEVR Lua scripting API, so the palette
//  offers UEVR nodes and "Generate UEVR Lua" walks the graph into a runnable
//  script, opened as a new Lua editor tab. Being a normal dockable window, it
//  inherits the app's multi-viewport docking for free; the canvas pan honors
//  the app's invert-pan setting.
//

#include <imgui.h>

#include "editor.h"

#include "../BlueprintEditor.h"
#include "../BlueprintLua.h"

//
//  Editor::openLuaInNewTab
//

void Editor::openLuaInNewTab(const std::string &text)
{
    auto &t = newTab(); // untitled → Ctrl+S prompts Save As (correct for generated content)
    t.editor.SetText(text);
    t.editor.SetLanguage(TextEditor::Language::Lua());
    t.wantFocus = true;
    // originalText stays empty, so the tab reads as unsaved (dirty) — expected
    // for freshly generated code the user hasn't chosen a path for yet.
    buildAutocompleteTrie(t);
}

//
//  Editor::renderBlueprintWindow
//

void Editor::renderBlueprintWindow()
{
    if (!blueprintVisible)
        return;

    // Lazily create the widget on first show and seed it with the UEVR Lua API
    // (this also sets the blueprint parent class to "UEVR").
    if (!blueprintEditor)
    {
        blueprintEditor = std::make_unique<BlueprintEditor>();
        BlueprintLua::SetupUEVRRegistry(*blueprintEditor);
        blueprintEditor->SetBlueprint("UEVRScript", "UEVR");
    }
    auto &bp = *blueprintEditor;
    bp.SetPanInverted(prefInvertPan); // every pan surface honors the app setting

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
                openLuaInNewTab(lua);
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
