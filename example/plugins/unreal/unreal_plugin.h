//
//  unreal_plugin.h — Unreal Engine integration as an ImGui-IDE plugin.
//
//  Wraps the pure UE helpers (unreal.{cpp,h}) behind the EditorPlugin hooks:
//    - onRegister:            augment the shared C++ language with UE type/macro
//                             vocabulary + the F*/U*/A*/… type-name heuristic
//    - onMenu(Project):       the "Unreal Engine" submenu (build / clangd DB /
//                             launch editor / open .uproject / install plugin)
//    - projectBuildCommand:   build the editor target via UnrealBuildTool
//    - resolveInclude:        module-relative UE include → on-disk header
//    - contributeAutocomplete:.uproject/.uplugin descriptor schema + names
//    - extraSourceRoot:       the engine Source/ tree for the nav panel
//
//  Compiled only when IMGUIIDE_PLUGIN_UNREAL is defined (see example/CMakeLists).
//

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "plugin_api.h"

class UnrealPlugin : public EditorPlugin
{
public:
    const char *id() const override { return "unreal"; }
    const char *displayName() const override { return "Unreal Engine integration"; }

    void onRegister(PluginHost &host) override;
    void onFrame(PluginHost &host) override; // renders the scaffolding wizard modals
    // Unreal gets its own top-level menu-bar entry rather than nesting in Project.
    const char *topLevelMenu() const override { return "Unreal"; }
    void onTopLevelMenu(PluginHost &host) override;
    // Claim .uasset/.umap (binary) so opening one shows an inspection report in a
    // tab instead of launching the external default app.
    bool openFile(PluginHost &host, const std::filesystem::path &path) override;
    void contributeAutocomplete(PluginHost &host, const PluginDocInfo &doc,
                                const std::function<void(const std::string &)> &addWord) override;
    // Palette entries only when the open project IS an Unreal project/plugin.
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override;
    std::optional<PluginBuildCommand> projectBuildCommand(const std::filesystem::path &startDir) override;
    std::optional<std::filesystem::path> resolveInclude(const std::filesystem::path &docDir,
                                                        const std::string &include) override;
    std::optional<PluginSourceRoot> extraSourceRoot(const std::filesystem::path &projectRoot) override;

private:
    // Cache the Project-menu discovery per project root — directory walks +
    // registry reads shouldn't run every frame while the menu is open.
    std::filesystem::path menuCachedRoot = std::filesystem::path("\x01");
    std::filesystem::path menuProj, menuEngine;
    std::string           menuAssoc;

    // ── Scaffolding wizards (menu sets a request; onFrame opens the modal) ──
    bool requestClassWizard = false;
    bool requestVerseWizard = false;
    char classNameBuf[128] = {0};
    int  classParentIdx = 0;
    int  classModuleIdx = 0;
    std::vector<std::filesystem::path> classModules; // Source/* module dirs, filled on open
    char verseNameBuf[128] = {0};
    void renderClassWizard(PluginHost &host);
    void renderVerseWizard(PluginHost &host);

    // ── Interactive .uproject/.uplugin descriptor editor ──────────────────
    bool requestDescriptorEditor = false;
    std::filesystem::path descriptorTarget; // the .uproject/.uplugin being edited
    int  descriptorMode = 0;                 // 0 = Plugin, 1 = Module
    char descriptorNameBuf[128] = {0};
    bool descriptorEnabled = true;           // Plugin: Enabled
    int  descriptorTypeIdx = 0;              // Module: index into unreal::moduleTypes()
    int  descriptorPhaseIdx = 0;             // Module: index into unreal::loadingPhases()
    std::vector<std::string> descriptorPluginChoices; // available plugins (engine + project), lazily filled
    std::vector<std::string> descriptorModuleChoices; // available modules (project + its plugins)
    bool descriptorChoicesLoaded = false;
    char descriptorPluginFilter[96] = {0};
    char descriptorModuleFilter[96] = {0};
    void renderDescriptorEditor(PluginHost &host);

    // Parse the active .uasset/.umap's header/name-table/imports and open a report.
    void inspectActiveUAsset(PluginHost &host);
    void inspectUAssetPath(PluginHost &host, const std::filesystem::path &asset);
};

// Factory used by plugin_registry.cpp (declared there under the same #ifdef).
std::unique_ptr<EditorPlugin> createUnrealPlugin();
