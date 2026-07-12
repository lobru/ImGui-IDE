//
//  blueprint_window.cpp — the dockable "Blueprint Editor" window.
//
//  Its body is the BlueprintEditor node-graph widget, its registry seeded with
//  the UEVR Lua scripting API so the palette offers UEVR nodes and "Generate
//  UEVR Lua" walks the graph into a runnable script opened as a new Lua tab.
//  Being a normal dockable window it inherits the app's multi-viewport docking;
//  the canvas pan honors the host's invert-pan setting.
//

#define _CRT_SECURE_NO_WARNINGS // std::getenv("APPDATA") for the Import Game SDK menu

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <imgui.h>

#include "BlueprintEditor.h"
#include "BlueprintLua.h"
#include "BlueprintLuaImport.h"
#include "BlueprintRegistryJson.h"
#include "blueprint_snippets.h"
#include "blueprint_templates.h"

#include "uevr_plugin.h"

void UevrPlugin::loadSdkDefinitions(BlueprintEditor &bp)
{
    // Merge the whole <exe>/sdk tree into the registry at create time: this module's
    // own exported JSON, and dropped-in UEVR sdk_dump trees (classes/*.json +
    // enums/*.lua). LoadSdkDir is recursive and non-throwing; malformed files are
    // skipped (optional data — the interactive Import path surfaces errors instead).
    if (!sdkDir.empty())
        BlueprintRegistryJson::LoadSdkDir(bp.GetRegistry(), sdkDir);
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

        // Offer to restore an autosave from a previous (possibly crashed) session.
        std::error_code ec;
        auto path = blueprintAutosavePath();
        if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0)
        {
            std::ifstream in(path, std::ios::binary);
            blueprintRecoveryData.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            blueprintRecoveryAvailable = !blueprintRecoveryData.empty();
        }
    }
    return *blueprintEditor;
}

std::filesystem::path UevrPlugin::blueprintAutosavePath() const
{
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "ImGuiIDE";
    std::filesystem::create_directories(dir, ec);
    return dir / "blueprint_autosave.bp";
}

void UevrPlugin::autosaveBlueprint()
{
    if (!blueprintEditor)
        return;
    double now = ImGui::GetTime();
    if (now < nextBlueprintAutosave)
        return;
    nextBlueprintAutosave = now + 15.0; // ~every 15s while there are unsaved edits
    if (!blueprintEditor->IsDirty())
        return;
    std::ofstream out(blueprintAutosavePath(), std::ios::binary | std::ios::trunc);
    if (out)
        out << blueprintEditor->SaveToString();
}

namespace
{
// The basic Lua-typed variables the sidebar can create, in combo order.
BlueprintEditor::PinType sidebarVarPinType(int idx)
{
    using PK = BlueprintEditor::PinKind;
    switch (idx)
    {
    case 0: return BlueprintEditor::PinType(PK::String);
    case 1: return BlueprintEditor::PinType(PK::Float); // Lua "number"
    case 2: return BlueprintEditor::PinType(PK::Boolean);
    case 3: return BlueprintEditor::PinType(PK::Vector);
    case 4: return BlueprintEditor::PinType(PK::Integer);
    case 5: return BlueprintEditor::PinType(PK::Object, "UObject");

    default: return BlueprintEditor::PinType(PK::Wildcard);
    }
}

const char *pinKindLabel(BlueprintEditor::PinKind k)
{
    using PK = BlueprintEditor::PinKind;
    switch (k)
    {
    case PK::String: return "string";
    case PK::Float: return "number";
    case PK::Integer: return "int";
    case PK::Boolean: return "bool";
    case PK::Vector: return "vector";
    case PK::Object: return "object";
    default: return "?";
    }
}
} // namespace

void UevrPlugin::renderBlueprintSidebar(PluginHost &host, BlueprintEditor &bp)
{
    ImGui::BeginChild("##bpSidebar", ImVec2(210.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX, ImGuiWindowFlags_HorizontalScrollbar);

    // ── UEVR SDK callbacks: click to drop the event node ──────────────────
    if (ImGui::CollapsingHeader("Callbacks", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (const BlueprintEditor::Class *uevrClass = bp.GetRegistry().FindClass("UEVR"))
        {
            for (const auto &ev : uevrClass->events)
            {
                if (ImGui::Selectable(ev.name.c_str()))
                    bp.AddEventNode("UEVR", ev.name, bp.NextSpawnPos());
                if (ImGui::IsItemHovered() && !ev.tooltip.empty())
                    ImGui::SetTooltip("%s", ev.tooltip.c_str());
            }
        }
    }

    // ── Variables: typed at creation, click Get/Set to drop a node ─────────
    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::SmallButton("+ Variable"))
            ImGui::OpenPopup("##addVar");

        if (ImGui::BeginPopup("##addVar"))
        {
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            bool enter = ImGui::InputTextWithHint("##vn", "name", sidebarVarName, sizeof(sidebarVarName),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            const char *types[] = {"String", "Number", "Boolean", "Vector", "Integer", "Object", "Class",};
            ImGui::SetNextItemWidth(150.0f);
            ImGui::Combo("##vt", &sidebarVarType, types, IM_ARRAYSIZE(types));
            if ((ImGui::Button("Add") || enter) && sidebarVarName[0])
            {
                bp.AddVariable(sidebarVarName, sidebarVarPinType(sidebarVarType), "");
                sidebarVarName[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Separator();
        std::string toRemove;
        bool requestRename = false;
        for (const auto &var : bp.GetVariables())
        {
            ImGui::PushID(var.name.c_str());
            ImGui::Selectable(var.name.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(var.name.c_str()).x, 0.0f));
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("double-click to rename");
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    sidebarRenameVar = var.name;
                    std::snprintf(sidebarRenameBuf, sizeof(sidebarRenameBuf), "%s", var.name.c_str());
                    requestRename = true;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", pinKindLabel(var.type.kind));
            if (ImGui::SmallButton("Get"))
                bp.AddVariableGetNode(var.name, bp.NextSpawnPos());
            ImGui::SameLine();
            if (ImGui::SmallButton("Set"))
                bp.AddVariableSetNode(var.name, bp.NextSpawnPos());
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                toRemove = var.name;
            ImGui::PopID();
        }
        if (!toRemove.empty())
            bp.RemoveVariable(toRemove);

        // Rename popup (opened after the loop so its ID isn't inside a per-var PushID).
        if (requestRename)
            ImGui::OpenPopup("##bpVarRename");
        if (ImGui::BeginPopup("##bpVarRename"))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            bool done = ImGui::InputText("##rn", sidebarRenameBuf, sizeof(sidebarRenameBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::Button("Rename");
            if (done && sidebarRenameBuf[0] && !sidebarRenameVar.empty())
            {
                bp.RenameVariable(sidebarRenameVar, sidebarRenameBuf);
                sidebarRenameVar.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (bp.GetVariables().empty())
            ImGui::TextDisabled("(no variables yet)");
    }

    host.hostMiddleMousePanScroll(105);
    ImGui::EndChild();
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
        // While this window is focused it handles its own Ctrl+Z/C/V/Del etc, so tell
        // the host to skip the document's app-level shortcuts (route keys here).
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            host.hostSuppressAppShortcuts();

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
                if (ImGui::BeginMenu("Insert Snippet"))
                {
                    for (auto &snip : BlueprintSnippets::All())
                    {
                        if (ImGui::MenuItem(snip.name.c_str()))
                        {
                            snip.insert(bp); // inserts at the view cursor, keeps the graph
                            host.hostToast("Inserted snippet: " + snip.name);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", snip.description.c_str());
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
                    // Merge the active document into the live registry — the palette
                    // rebuilds from it on next open. Accepts this module's own JSON,
                    // a UEVR reflection dump (.json), or a UE4SS enum annotation (.lua).
                    std::string text = host.hostActiveText();
                    size_t firstNonSpace = text.find_first_not_of(" \t\r\n");
                    std::string error;
                    if (firstNonSpace == std::string::npos)
                        host.hostToast("Open an SDK .json (or enum .lua) in the editor first, then import it");
                    else if (text[firstNonSpace] == '{')
                    {
                        if (!BlueprintRegistryJson::Load(bp.GetRegistry(), text, error))
                            host.hostError("SDK import failed: " + error);
                        else
                            host.hostToast("Imported SDK nodes into the palette");
                    }
                    else if (text.find("---@enum") != std::string::npos)
                    {
                        int n = BlueprintRegistryJson::LoadEnumLua(bp.GetRegistry(), text);
                        host.hostToast("Imported " + std::to_string(n) + " enum(s) into the palette");
                    }
                    else
                        host.hostError("Unrecognized SDK document (expected a JSON dump or a ---@enum .lua)");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Merge the active document (JSON dump or enum .lua) into the node palette at runtime");
                if (const char *appdata = std::getenv("APPDATA"))
                {
                    // Import a game's dumped SDK straight from where UEVR writes it:
                    // %APPDATA%/UnrealVRMod/<game>/sdk_dump.
                    std::filesystem::path uevrRoot = std::filesystem::path(appdata) / "UnrealVRMod";
                    std::error_code uec;
                    if (std::filesystem::is_directory(uevrRoot, uec) && ImGui::BeginMenu("Import Game SDK"))
                    {
                        int shown = 0;
                        std::error_code iec;
                        for (auto git = std::filesystem::directory_iterator(
                                 uevrRoot, std::filesystem::directory_options::skip_permission_denied, iec);
                             !iec && git != std::filesystem::directory_iterator() && shown < 100; git.increment(iec))
                        {
                            std::error_code dec;
                            if (!git->is_directory(dec) || dec)
                                continue;
                            std::filesystem::path dump = git->path() / "sdk_dump";
                            std::error_code sec;
                            if (!std::filesystem::is_directory(dump, sec))
                                continue;
                            std::string game = git->path().filename().string();
                            if (ImGui::MenuItem(game.c_str()))
                            {
                                int n = BlueprintRegistryJson::LoadSdkDir(bp.GetRegistry(), dump);
                                host.hostToast("Imported " + std::to_string(n) + " classes from " + game);
                            }
                            ++shown;
                        }
                        if (shown == 0)
                            ImGui::TextDisabled("(no <game>/sdk_dump found)");
                        ImGui::EndMenu();
                    }
                }
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

        if (blueprintRecoveryAvailable)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 200, 90, 255));
            ImGui::TextUnformatted("\xe2\x9a\xa0 Autosave from a previous session found.");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Recover"))
            {
                bp.LoadFromString(blueprintRecoveryData);
                blueprintRecoveryAvailable = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Discard"))
            {
                std::error_code ec;
                std::filesystem::remove(blueprintAutosavePath(), ec);
                blueprintRecoveryAvailable = false;
            }
            ImGui::Separator();
        }

        renderBlueprintSidebar(host, bp);
        ImGui::SameLine();
        bp.Render("BlueprintCanvas");
        autosaveBlueprint(); // periodic tmp-file autosave while dirty
    }
    ImGui::End();
}
