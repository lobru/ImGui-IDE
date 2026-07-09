//
//  blueprint_window.cpp — the dockable "Blueprint Editor" window.
//
//  Its body is the BlueprintEditor node-graph widget, its registry seeded with
//  the UEVR Lua scripting API so the palette offers UEVR nodes and "Generate
//  UEVR Lua" walks the graph into a runnable script opened as a new Lua tab.
//  Being a normal dockable window it inherits the app's multi-viewport docking;
//  the canvas pan honors the host's invert-pan setting.
//

#include <fstream>
#include <iterator>
#include <system_error>

#include <imgui.h>

#include "BlueprintEditor.h"
#include "BlueprintLua.h"
#include "BlueprintLuaImport.h"
#include "BlueprintRegistryJson.h"
#include "blueprint_templates.h"

#include "uevr_plugin.h"

void UevrPlugin::loadSdkDefinitions(BlueprintEditor &bp)
{
    // Merge every <exe>/sdk/*.json API definition into the registry at create time.
    // Malformed files are skipped (these are optional user-supplied SDK dumps); the
    // interactive Import path surfaces parse errors to the user instead.
    std::error_code ec;
    if (sdkDir.empty() || !std::filesystem::is_directory(sdkDir, ec))
        return;

    // Non-throwing iteration (directory_iterator's range-for operator++ throws even
    // when constructed with an error_code — see plugin_loader.cpp).
    for (auto it = std::filesystem::directory_iterator(sdkDir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec || it->path().extension() != ".json")
            continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        BlueprintRegistryJson::Load(bp.GetRegistry(), text, error);
    }
}

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
        loadSdkDefinitions(*blueprintEditor); // merge any <exe>/sdk/*.json SDK dumps
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
                if (ImGui::BeginMenu("New from Template"))
                {
                    for (auto &tmpl : BlueprintTemplates::All())
                    {
                        if (ImGui::MenuItem(tmpl.name.c_str()))
                        {
                            tmpl.build(bp);
                            host.hostToast("Loaded template: " + tmpl.name);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", tmpl.description.c_str());
                    }
                    ImGui::EndMenu();
                }
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
                if (ImGui::MenuItem("Adapt Active Doc with AI\xe2\x80\xa6"))
                {
                    // The structural importer only handles shapes it recognizes; for
                    // arbitrary hand-written scripts, hand the source to Claude (via the
                    // editor's reply bridge) to restructure/clean/port it instead.
                    std::string source = host.hostActiveText();
                    if (source.empty())
                        host.hostToast("No active document to adapt");
                    else
                    {
                        std::string prompt =
                            "[ImGui-IDE / UEVR Blueprint] Please adapt the UEVR Lua script below so it works well in\n"
                            "this IDE. Recognize the uevr.sdk.callbacks handlers and uevr.api / uevr.params / imgui\n"
                            "calls, restructure into clear event handlers, fix anything broken or deprecated, and\n"
                            "leave a short comment on anything that must remain a raw Lua block. Save the adapted\n"
                            "script next to the project (or open it in the IDE) when done.\n\n"
                            "--- BEGIN SCRIPT ---\n" +
                            source + "\n--- END SCRIPT ---\n";
                        host.hostSendToClaude(prompt);
                        host.hostToast("Sent to Claude to adapt \xe2\x80\x94 watch for the reply");
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Send the active document to Claude to restructure/clean/port when structural import can't");
                ImGui::Separator();
                if (ImGui::MenuItem("Import SDK (Active Doc)"))
                {
                    // Merge a JSON API definition (open in the editor) into the live
                    // registry — the palette rebuilds from it on its next open.
                    std::string text = host.hostActiveText();
                    std::string error;
                    if (text.empty())
                        host.hostToast("Open an SDK .json in the editor first, then import it");
                    else if (!BlueprintRegistryJson::Load(bp.GetRegistry(), text, error))
                        host.hostError("SDK import failed: " + error);
                    else
                        host.hostToast("Imported SDK nodes into the palette");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Merge a JSON API definition (the active document) into the node palette at runtime");
                if (ImGui::MenuItem("Export API to sdk/uevr_api.json"))
                {
                    // Dump the live API to editable JSON: edit it and re-import, or
                    // leave it in sdk/ to auto-load on the next launch.
                    std::error_code ec;
                    std::filesystem::create_directories(sdkDir, ec);
                    std::filesystem::path path = sdkDir / "uevr_api.json";
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    if (out)
                    {
                        out << BlueprintRegistryJson::Save(bp.GetRegistry());
                        out.close();
                        host.hostOpenFile(path.string());
                        host.hostToast("Exported API to " + path.string());
                    }
                    else
                        host.hostError("Could not write " + path.string());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Dump the live node API to editable JSON (data-driven: edit it, then Import, or drop files into sdk/)");
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
