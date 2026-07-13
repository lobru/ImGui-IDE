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

// The widget header is needed here for the nested TypeRegistry (the SDK side index);
// the BlueprintEditor INSTANCE stays behind a unique_ptr created in the .cpp files.
#include "BlueprintEditor.h"

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

    // ── Data-driven SDK index ───────────────────────────────────────────────
    // Imported SDK dumps (<exe>/sdk auto-load, Import Game SDK, Import Active Doc)
    // land in a SIDE index, not the node registry — a 10k-class dump would make
    // the palette unusable. The index feeds (a) Lua autocomplete (sdkWords, via
    // contributeAutocomplete) and (b) on-demand "Expose SDK Class", which copies
    // ONE class's nodes into the live registry (BlueprintRegistryJson::ExposeClass).
    std::filesystem::path sdkDir;
    BlueprintEditor::TypeRegistry sdkIndex;
    std::vector<std::string> sdkWords;     // deduped class/function/property names
    char sdkExposeFilter[128] = {0};
    void loadSdkDefinitions();             // fill sdkIndex from sdkDir (recursive)
    void rebuildSdkWords();

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

    // Render a bridge text blob (Globals / Modules / Inspect) as an interactive
    // ImGui table: `cols` columns (3 = Name/Type/Value, 2 = Name/Type), filtered by
    // `filter` (case-insensitive substring on the name). When allowInsert, each row
    // gets an "ins" button that drops a Custom Lua node reading that name. Tolerates
    // both tab- and space-delimited bridge output (see parseBridgeRows).
    void renderBridgeTable(const char *id, const std::string &text, const char *filter, int cols, bool allowInsert);

    // ── Interactive Globals tree (lazy Lua-object inspection) ───────────────
    // Each row is a live global (or a key inside an expanded table). Table-typed
    // rows expand on click: their keys/values are fetched on demand over the bridge
    // (pairs() enumeration; debug.getinfo for functions), so you can drill into
    // nested Lua tables/objects. `path` is the Lua access expression used to refetch.
    struct GlobalNode
    {
        std::string key, type, value, path;
        bool expandable = false; // type == "table" (children fetchable)
        bool fetched = false;    // children already retrieved
        std::vector<GlobalNode> children;
    };
    std::vector<GlobalNode> uevrGlobalsTree;
    std::string uevrGlobalsSource;              // the raw dump the tree was built from
    std::vector<std::string> uevrPendingPairs;  // paths awaiting a PAIRS response
    void rebuildGlobalsTree();                  // (re)build the top level from uevrGlobals
    static GlobalNode *findNodeByPath(std::vector<GlobalNode> &nodes, const std::string &path);
    void requestGlobalChildren(GlobalNode &node);
    void applyPairsResponse(const std::string &pathId, const std::string &rows);
    void renderGlobalsTree(const char *filter);

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
