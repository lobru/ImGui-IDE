//
//  uevr_plugin.h — UEVR / Blueprint tooling as an ImGui-IDE plugin.
//
//  Bundles the Blueprint visual-scripting editor (node graph → UEVR Lua codegen)
//  and the "UEVR Live" bridge (drive a running UEVR game's Lua over a file
//  inbox) behind the EditorPlugin interface. All state that used to live on
//  Editor lives here; the plugin only touches the editor through PluginHost.
//
//  Compiled only when IMGUIIDE_PLUGIN_UEVR is defined (see example/CMakeLists).
//

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "plugin_api.h"

class BlueprintEditor; // heavy widget header — included only in the .cpp files

class UevrPlugin : public EditorPlugin
{
public:
    UevrPlugin();
    ~UevrPlugin() override; // out-of-line: BlueprintEditor is incomplete here

    const char *id() const override { return "uevr"; }
    const char *displayName() const override { return "UEVR / Blueprint tools"; }

    void onRegister(PluginHost &host) override;
    void onFrame(PluginHost &host) override;
    void onMenu(PluginHost &host, PluginMenu which) override;
    void contributeAutocomplete(PluginHost &host, const PluginDocInfo &doc,
                                const std::function<void(const std::string &)> &addWord) override;

private:
    // ── Blueprint visual scripting editor ──────────────────────────────────
    std::unique_ptr<BlueprintEditor> blueprintEditor; // lazily created on first show
    bool        blueprintVisible = false;
    std::string blueprintSnapshot;                    // Graph > Save/Load Snapshot buffer
    void renderBlueprintWindow(PluginHost &host);

    // "My Blueprint"-style left panel: UEVR SDK callbacks + user variables (typed
    // at creation), click to drop Event / Get / Set nodes onto the canvas.
    void renderBlueprintSidebar(PluginHost &host, BlueprintEditor &bp);
    char sidebarVarName[128] = {0};
    int  sidebarVarType = 0;
    char sidebarRenameBuf[128] = {0};
    std::string sidebarRenameVar; // variable being renamed (double-click), "" if none

    // Autosave + crash recovery: the dirty graph is periodically written to a tmp
    // file; on first open, if one is found (e.g. after a crash) recovery is offered.
    std::filesystem::path blueprintAutosavePath() const; // <temp>/ImGuiIDE/blueprint_autosave.bp
    void autosaveBlueprint();
    double      nextBlueprintAutosave = 0.0;
    bool        blueprintRecoveryAvailable = false;
    std::string blueprintRecoveryData;

    // Lazily creates/returns the Blueprint editor. Shared by renderBlueprintWindow and
    // insertLiveValueAsNode so an "insert as node" click works even if the Blueprint
    // window was never opened this session.
    BlueprintEditor &ensureBlueprintEditor();

    // ── Data-driven SDK registry ────────────────────────────────────────────
    // <exe>/sdk holds optional JSON API definitions (see BlueprintRegistryJson).
    // Any *.json there is merged into the node registry at editor-create time, so
    // dropping in a game's SDK dump adds its nodes without a rebuild. sdkDir is
    // captured in onRegister; the Blueprint window's Graph menu also imports the
    // active document and exports the live API here.
    std::filesystem::path sdkDir;
    void loadSdkDefinitions(BlueprintEditor &bp); // merge every sdkDir/*.json

    // ── UEVR Live bridge (file-inbox IPC) ──────────────────────────────────
    bool uevrLiveVisible = false;
    char uevrReplBuf[4096]   = {0};
    char uevrInspectBuf[256] = "uevr.api:get_local_pawn(0)";
    std::vector<std::string> uevrOutputLog;            // REPL / run output (capped)
    std::string uevrGlobals, uevrModules, uevrInspect; // dump-tab text
    int uevrReqCounter = 0;
    std::filesystem::path uevrBridgeDir(const char *sub) const; // <ide_bridge>/<sub>
    void sendUevr(const std::string &kind, const std::string &payload);
    void pollUevrBridge();                             // drain the out inbox (~5 Hz)
    void renderUevrLive(PluginHost &host);             // the dockable panel

    // Live-value → Blueprint node bridge: turns an arbitrary inspected/watched
    // expression into a Custom Lua node (see BlueprintEditor::AddCustomLuaNode) --
    // a single registry Function can't represent an arbitrary expression, but a
    // one-line Custom Lua node always can.
    void insertLiveValueAsNode(const std::string &expr, const std::string &label);

    // ── Watch list (batched, stateless -- the full list is resent every refresh) ────
    struct WatchEntry
    {
        std::string expr, type, preview;
    };
    std::vector<WatchEntry> uevrWatches;
    char uevrWatchExprBuf[256] = "uevr.api:get_local_pawn(0):get_full_name()";
    void sendWatchBatch();
};

// Factory used by plugin_registry.cpp (declared there under the same #ifdef).
std::unique_ptr<EditorPlugin> createUevrPlugin();
