//
//  uevr_plugin.cpp — UevrPlugin wiring: menu items, autocomplete, per-frame
//  dispatch, and the factory. The heavy per-window bodies live alongside in
//  blueprint_window.cpp and uevr_bridge.cpp.
//

#define _CRT_SECURE_NO_WARNINGS // std::getenv("APPDATA") for the UEVR Scripts menu

#include <cstdio>

#include <imgui.h>

#include "BlueprintEditor.h"
#include "BlueprintLua.h"

#include "uevr_plugin.h"

// BlueprintEditor is only complete in this TU (and the sibling .cpp files), so
// the ctor/dtor of the unique_ptr member must be emitted here.
UevrPlugin::UevrPlugin() = default;
UevrPlugin::~UevrPlugin() = default;

std::unique_ptr<EditorPlugin> createUevrPlugin()
{
    return std::make_unique<UevrPlugin>();
}

void UevrPlugin::onRegister(PluginHost &host)
{
    // Remember where startup/imported SDK definitions live so ensureBlueprintEditor
    // can merge them and the Graph menu can export back to the same folder.
    sdkDir = host.hostExeDir() / "sdk";
}

void UevrPlugin::onFrame(PluginHost &host)
{
    renderBlueprintWindow(host);
    renderUevrLive(host);
}

void UevrPlugin::onMenu(PluginHost &host, PluginMenu which)
{
    // Panel toggles live under Tools (the host's reorganized "Panels" menu). These
    // are plugin windows, so they belong with the other tool panels, not in View.
    if (which == PluginMenu::Tools)
    {
        if (ImGui::MenuItem("Blueprint Editor (UEVR)", nullptr, &blueprintVisible))
        {
        }
        if (ImGui::MenuItem("UEVR Live (bridge)", nullptr, &uevrLiveVisible))
        {
        }
        return;
    }

    if (which == PluginMenu::File)
    {
        // Import/open Lua from the global UEVR scripts folder
        // (%APPDATA%\UnrealVRMod\UEVR\Scripts) — the same folder the UEVR Lua
        // editor + "Generate UEVR Lua" target.
        const char *appdata = std::getenv("APPDATA");
        if (!appdata)
            return;
        std::filesystem::path uevrScripts =
            std::filesystem::path(appdata) / "UnrealVRMod" / "UEVR" / "Scripts";
        std::error_code uec;
        if (!std::filesystem::is_directory(uevrScripts, uec) ||
            !ImGui::BeginMenu("Open UEVR Script"))
            return;

        if (ImGui::MenuItem("Open Scripts Folder in Nav"))
            host.hostSetProjectRoot(uevrScripts.string());
        ImGui::Separator();
        int shown = 0;
        std::error_code iec;
        for (auto it = std::filesystem::directory_iterator(
                 uevrScripts, std::filesystem::directory_options::skip_permission_denied, iec);
             !iec && it != std::filesystem::directory_iterator() && shown < 200; it.increment(iec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec)
                continue;
            auto ext = it->path().extension();
            if (ext != ".lua" && ext != ".txt")
                continue;
            std::string leaf = it->path().filename().string();
            if (ImGui::MenuItem(leaf.c_str()))
                host.hostOpenFile(it->path().string());
            ++shown;
        }
        if (shown == 0)
            ImGui::TextDisabled("(no .lua scripts found)");
        ImGui::EndMenu();
    }
}

void UevrPlugin::contributeAutocomplete(PluginHost &, const PluginDocInfo &doc,
                                        const std::function<void(const std::string &)> &addWord)
{
    // Lua docs (real .lua files AND generated "untitled" Lua tabs) complete the
    // UEVR scripting API — namespaces, sdk callbacks, api/vr methods, UObject
    // accessors. Keyed on the language, not the extension, so "Generate UEVR
    // Lua" tabs get it too.
    if (doc.languageName == "Lua")
    {
        for (const auto &word : BlueprintLua::LuaApiIdentifiers())
            addWord(word);

        // Imported SDK dumps feed autocomplete here (class + function + property
        // names) instead of flooding the node palette — see loadSdkDefinitions.
        for (const auto &word : sdkWords)
            addWord(word);
    }
}
