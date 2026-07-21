//
//  debugger_plugin.h — the DAP debugger as an ImGui-IDE plugin.
//
//  Everything debugger lives here: the Debug panel (controls / call stack /
//  variables / console + REPL), breakpoints (F9 + gutter markers via
//  contributeMarkers), session management over dap::DapClient, adapter
//  resolution (per-project association > per-extension > auto-detected native
//  adapters), the Configuration GUI, and the raddbg / Visual Studio launch
//  bridges. The core editor carries ZERO debugger code.
//
//  Hooks used: onRegister (load config), onFrame (poll + panel),
//  topLevelMenu "Debug", contributeKeybinds (conditional — F5=continue only
//  while paused, so the core's F5=Run works otherwise), contributeMarkers,
//  contributePaletteCommands. Config persists in <config>/debugger.ini.
//
//  Compiled only when IMGUIIDE_PLUGIN_DEBUGGER is defined (see CMakeLists).
//

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dap_client.h"
#include "plugin_api.h"

class DebuggerPlugin : public EditorPlugin
{
public:
    const char *id() const override { return "debugger"; }
    const char *displayName() const override { return "Debugger (DAP)"; }

    void onRegister(PluginHost &host) override;   // load debugger.ini
    void onFrame(PluginHost &host) override;      // pollDap + Debug panel
    // Lives as a "Debug" submenu inside Tools (menu-bar declutter).
    void onMenu(PluginHost &host, PluginMenu which) override;
    void contributeKeybinds(PluginHost &host, std::vector<PluginKeybind> &out) override;
    // Clicking an existing breakpoint marker in the gutter removes it.
    bool onGutterClick(PluginHost &host, const PluginDocInfo &doc, int line0) override;
    void contributeMarkers(PluginHost &host, const PluginDocInfo &doc,
                           const std::function<void(int, unsigned, unsigned,
                                                    const std::string &, const std::string &)> &add) override;
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override;

private:
    // ── session state ────────────────────────────────────────────────────
    dap::DapClient dapClient;
    bool  panelVisible = false;
    bool  sessionActive = false;
    bool  launchSent = false;
    bool  stopped = false;
    std::string stopReason;
    std::string stopFile;
    int   stopLine = -1;
    int   threadId = 0;
    int   currentFrame = 0;
    std::string program;
    std::string adapterType;
    std::string adapterPath;      // argv[0] of the running adapter (for diagnostics)
    bool  adapterInitialized = false;   // saw a successful initialize response
    bool  licenseBlocked = false;       // adapter demanded a handshake we can't sign
    std::vector<dap::StackFrame> frames;
    std::vector<dap::Scope>      scopes;
    std::unordered_map<int, std::vector<dap::Variable>> varChildren;
    std::unordered_set<int>      varRequested;
    std::string consoleText;
    bool  consoleScrollDown = false;
    char  evalBuf[256] = "";
    std::vector<std::pair<std::string, std::string>> launchExtras;
    std::vector<std::string> programArgs;
    // Breakpoints per canonical file path, 0-based lines (session-only).
    std::unordered_map<std::string, std::set<int>> breakpoints;

    // ── configuration (persisted in <config>/debugger.ini) ───────────────
    std::map<std::string, std::string> adapterOverrides; // ".ext" -> adapter cmdline
    std::map<std::string, std::string> projectAdapter;   // root -> adapter cmdline
    std::map<std::string, std::string> projectTarget;    // root -> "program|args"
    std::map<std::string, std::string> bridgeSettings;   // raddbg/devenv paths + verbs
    std::filesystem::path configPath;                    // set in onRegister
    void loadConfig();
    void saveConfig();

    // one-shot native adapter detection cache
    bool nativeAdapterDetected = false;
    std::vector<std::string> nativeAdapterArgv;
    std::string nativeAdapterType;
    std::vector<std::pair<std::string, std::string>> nativeAdapterExtras;
    // True when the ONLY native adapter present is Microsoft vsdbg, which is
    // license-locked to Visual Studio / VS Code and can't be driven here — so
    // we don't auto-select it; startSession explains it instead of aborting.
    bool nativeOnlyVsdbg = false;

    // ── helpers ──────────────────────────────────────────────────────────
    std::string canonPath(const std::string &file) const;
    std::string projectKey(PluginHost &host) const;   // canonical project root
    static bool isNativeDebugExt(const std::string &extLower);
    std::string venvPythonFor(PluginHost &host, const std::filesystem::path &scriptPath) const;
    std::vector<std::string> adapterFor(PluginHost &host, const std::string &ext,
                                        const std::filesystem::path &scriptPath,
                                        std::string &typeOut,
                                        std::vector<std::pair<std::string, std::string>> &extrasOut);
    void toggleBreakpointAtCursor(PluginHost &host);
    void sendBreakpointsFor(const std::string &canon);
    void startSession(PluginHost &host);
    void stopSession(PluginHost &host);
    void pollDap(PluginHost &host);
    void renderPanel(PluginHost &host);

    // external native debuggers (raddbg / Visual Studio)
    std::string bridgeToolPath(const char *key, std::string (*detect)()) const;
    bool externalDebugTarget(PluginHost &host, std::string &exe, std::string &args, std::string &why);
    void launchDetached(PluginHost &host, const std::vector<std::string> &argv);
    void debugInRadDbg(PluginHost &host);
    void debugInVisualStudio(PluginHost &host);
};

// Factory used by the DLL ABI shim (plugin_dll_main.cpp).
std::unique_ptr<EditorPlugin> createDebuggerPlugin();
