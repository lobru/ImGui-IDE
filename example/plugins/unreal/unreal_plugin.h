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
    void onMenu(PluginHost &host, PluginMenu which) override;
    // Claim .uasset/.umap (binary) so opening one shows an inspection report in a
    // tab instead of launching the external default app.
    bool openFile(PluginHost &host, const std::filesystem::path &path) override;
    void contributeAutocomplete(PluginHost &host, const PluginDocInfo &doc,
                                const std::function<void(const std::string &)> &addWord) override;
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

    // Parse the active .uasset/.umap's header/name-table/imports and open a report.
    void inspectActiveUAsset(PluginHost &host);
    void inspectUAssetPath(PluginHost &host, const std::filesystem::path &asset);
};

// Factory used by plugin_registry.cpp (declared there under the same #ifdef).
std::unique_ptr<EditorPlugin> createUnrealPlugin();
