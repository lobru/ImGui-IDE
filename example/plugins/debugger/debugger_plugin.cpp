//
//  debugger_plugin.cpp — see debugger_plugin.h. Ported from the editor-core
//  debugger (debugger_ui.cpp); the core now carries zero debugger code.
//

#define _CRT_SECURE_NO_WARNINGS   // std::getenv (MSVC C4996 vs /WX)

#include "debugger_plugin.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <SDL3/SDL_process.h>

#include "imgui.h"
#include "debug_bridge.h"

// ── config persistence (debugger.ini: tiny section/key=value format) ────────

void DebuggerPlugin::loadConfig()
{
    std::ifstream f(configPath);
    if (!f.is_open())
        return;
    std::string section, line;
    while (std::getline(f, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (section == "adapters")
            adapterOverrides[k] = v;
        else if (section == "project_adapter")
            projectAdapter[k] = v;
        else if (section == "project_target")
            projectTarget[k] = v;
        else if (section == "bridge")
            bridgeSettings[k] = v;
    }
}

void DebuggerPlugin::saveConfig()
{
    if (configPath.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(configPath.parent_path(), ec);
    std::ofstream f(configPath, std::ios::trunc);
    if (!f.is_open())
        return;
    f << "# ImGui-IDE debugger plugin configuration.\n[adapters]\n";
    for (auto &[k, v] : adapterOverrides)
        f << k << "=" << v << "\n";
    f << "\n[project_adapter]\n";
    for (auto &[k, v] : projectAdapter)
        f << k << "=" << v << "\n";
    f << "\n[project_target]\n";
    for (auto &[k, v] : projectTarget)
        f << k << "=" << v << "\n";
    f << "\n[bridge]\n";
    for (auto &[k, v] : bridgeSettings)
        f << k << "=" << v << "\n";
}

void DebuggerPlugin::onRegister(PluginHost &host)
{
    configPath = host.hostConfigDir() / "debugger.ini";
    loadConfig();
}

// ── helpers ──────────────────────────────────────────────────────────────

std::string DebuggerPlugin::canonPath(const std::string &file) const
{
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(file, ec);
    return ec ? file : c.string();
}

std::string DebuggerPlugin::projectKey(PluginHost &host) const
{
    auto root = host.hostProjectRoot();
    if (root.empty())
        return {};
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(root, ec);
    return ec ? root.string() : c.string();
}

bool DebuggerPlugin::isNativeDebugExt(const std::string &extLower)
{
    return extLower == ".c" || extLower == ".cpp" || extLower == ".cc" || extLower == ".cxx" ||
           extLower == ".h" || extLower == ".hpp" || extLower == ".hxx" || extLower == ".hh" ||
           extLower == ".inl" || extLower == ".exe";
}

// Prefer a project virtualenv's python for debugpy (mirrors the script runner).
std::string DebuggerPlugin::venvPythonFor(PluginHost &host, const std::filesystem::path &scriptPath) const
{
    std::error_code ec;
    auto check = [&](const std::filesystem::path &venvDir) -> std::string {
#ifdef _WIN32
        auto py = venvDir / "Scripts" / "python.exe";
#else
        auto py = venvDir / "bin" / "python";
#endif
        return std::filesystem::is_regular_file(py, ec) ? py.string() : std::string();
    };
    if (const char *ve = std::getenv("VIRTUAL_ENV"))
    {
        auto p = check(ve);
        if (!p.empty())
            return p;
    }
    std::vector<std::filesystem::path> bases;
    if (!scriptPath.empty())
        bases.push_back(scriptPath.parent_path());
    if (auto root = host.hostProjectRoot(); !root.empty())
        bases.push_back(root);
    for (const auto &base : bases)
    {
        auto cur = base;
        for (int i = 0; i < 6; ++i)
        {
            for (const char *name : {".venv", "venv", "env"})
            {
                auto p = check(cur / name);
                if (!p.empty())
                    return p;
            }
            if (!cur.has_parent_path() || cur.parent_path() == cur)
                break;
            cur = cur.parent_path();
        }
    }
    return {};
}

// Resolve the adapter command for a file extension. The per-extension override
// wins; Python falls back to debugpy through the venv/python; the C-family
// bridges into the best native DAP adapter found on this machine (vsdbg /
// OpenDebugAD7+gdb / lldb-dap / gdb 14+), detected once and cached.
std::vector<std::string> DebuggerPlugin::adapterFor(PluginHost &host, const std::string &ext,
                                                    const std::filesystem::path &scriptPath,
                                                    std::string &typeOut,
                                                    std::vector<std::pair<std::string, std::string>> &extrasOut)
{
    typeOut.clear();
    extrasOut.clear();
    auto ov = adapterOverrides.find(ext);
    if (ov != adapterOverrides.end() && !ov->second.empty())
    {
        auto argv = dbgbridge::splitCommandLine(ov->second);
        typeOut = dbgbridge::inferAdapterType(argv);
        return argv;
    }
    if (ext == ".py" || ext == ".pyw")
    {
        typeOut = "python";
        std::string python = venvPythonFor(host, scriptPath);
        if (python.empty())
            python = "python";
        return {python, "-m", "debugpy.adapter"};
    }
    if (isNativeDebugExt(ext))
    {
        if (!nativeAdapterDetected)
        {
            nativeAdapterDetected = true;   // detect once; tools don't appear mid-session
            if (auto vsdbg = dbgbridge::findVsdbg(); !vsdbg.empty())
            {
                nativeAdapterArgv = {vsdbg, "--interpreter=vscode"};
                nativeAdapterType = "cppvsdbg";
            }
            else if (auto ad7 = dbgbridge::findOpenDebugAD7();
                     !ad7.empty() && dbgbridge::commandOnPath("gdb"))
            {
                nativeAdapterArgv = {ad7};
                nativeAdapterType = "cppdbg";
                nativeAdapterExtras = {{"MIMode", "gdb"}, {"miDebuggerPath", "gdb"}};
            }
            else if (dbgbridge::commandOnPath("lldb-dap"))
            {
                nativeAdapterArgv = {"lldb-dap"};
            }
            else if (dbgbridge::commandOnPath("gdb"))
            {
                nativeAdapterArgv = {"gdb", "-i", "dap"};
            }
        }
        typeOut = nativeAdapterType;
        extrasOut = nativeAdapterExtras;
        return nativeAdapterArgv;
    }
    return {};
}

// ── breakpoints + markers ────────────────────────────────────────────────

void DebuggerPlugin::toggleBreakpointAtCursor(PluginHost &host)
{
    std::string file = host.hostActiveFilename();
    int line = host.hostActiveCursorLine();
    if (file.empty() || file == "untitled" || line < 0)
        return;   // the adapter needs a real on-disk path
    std::string canon = canonPath(file);
    auto &set = breakpoints[canon];
    if (!set.erase(line))
        set.insert(line);
    if (set.empty())
        breakpoints.erase(canon);
    host.hostRefreshMarkers();
    if (sessionActive)
        sendBreakpointsFor(canon);
}

void DebuggerPlugin::contributeMarkers(PluginHost &, const PluginDocInfo &doc,
                                       const std::function<void(int, unsigned, unsigned,
                                                                const std::string &, const std::string &)> &add)
{
    if (doc.filename.empty() || doc.filename == "untitled")
        return;
    std::string canon = canonPath(doc.filename);
    if (auto it = breakpoints.find(canon); it != breakpoints.end())
        for (int line : it->second)
            add(line, IM_COL32(225, 85, 85, 255), 0, "Breakpoint (F9 to remove)", "");
    if (stopped && !stopFile.empty() && canon == stopFile && stopLine >= 0)
        add(stopLine, IM_COL32(240, 200, 90, 255), IM_COL32(240, 200, 90, 42),
            "Stopped here", stopReason);
}

void DebuggerPlugin::sendBreakpointsFor(const std::string &canon)
{
    std::vector<int> lines1;
    auto it = breakpoints.find(canon);
    if (it != breakpoints.end())
        for (int l : it->second)
            lines1.push_back(l + 1);   // DAP lines are 1-based
    dapClient.setBreakpoints(canon, lines1);
}

// ── session ──────────────────────────────────────────────────────────────

void DebuggerPlugin::startSession(PluginHost &host)
{
    // Project association (adapter + optional explicit target) wins; with a
    // project target configured no document needs to be focused.
    std::string projKey = projectKey(host);
    std::string projAdapterCmd, projProgram;
    std::vector<std::string> projArgs;
    if (!projKey.empty())
    {
        if (auto it = projectAdapter.find(projKey); it != projectAdapter.end())
            projAdapterCmd = it->second;
        if (auto it = projectTarget.find(projKey); it != projectTarget.end() && !it->second.empty())
        {
            auto bar = it->second.find('|');
            projProgram = it->second.substr(0, bar);
            if (bar != std::string::npos)
                projArgs = dbgbridge::splitCommandLine(it->second.substr(bar + 1));
        }
    }

    std::string active = host.hostActiveFilename();
    bool haveDoc = !active.empty() && active != "untitled";
    if (!haveDoc && projProgram.empty())
    {
        host.hostToast("Debug: open a file, or set a project target in Debug panel > Configuration");
        panelVisible = true;
        return;
    }
    if (haveDoc)
        host.hostSaveActiveDocument();   // debug what's on screen, not a stale disk copy

    std::filesystem::path prog = !projProgram.empty() ? std::filesystem::path(projProgram)
                                                      : std::filesystem::path(active);
    auto ext = prog.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    std::string type;
    std::vector<std::pair<std::string, std::string>> extras;
    std::vector<std::string> argv;
    if (!projAdapterCmd.empty())
    {
        argv = dbgbridge::splitCommandLine(projAdapterCmd);
        type = dbgbridge::inferAdapterType(argv);
        if (type == "cppdbg")
            extras = {{"MIMode", "gdb"}, {"miDebuggerPath", "gdb"}};
    }
    else
        argv = adapterFor(host, ext, prog, type, extras);
    if (argv.empty())
    {
        host.hostToast("No debug adapter for " + ext + " — configure one in Debug panel > Configuration");
        panelVisible = true;   // put the fix next to the failure
        return;
    }
    // C-family sources aren't the debuggee — the built executable is (unless an
    // explicit project target already names the program).
    if (projProgram.empty() && isNativeDebugExt(ext) && ext != ".exe")
    {
        auto exe = host.hostFindBuiltExe();
        if (exe.empty())
        {
            host.hostToast("Debug: no built executable found — build first (F6)");
            return;
        }
        prog = exe;
    }

    if (dapClient.spawned())
        dapClient.stop();   // stale session
    sessionActive = false;
    launchSent = false;
    stopped = false;
    stopFile.clear();
    stopLine = -1;
    threadId = 0;
    currentFrame = 0;
    frames.clear();
    scopes.clear();
    varChildren.clear();
    varRequested.clear();
    consoleText.clear();

    if (!dapClient.start(argv))
    {
        host.hostToast("Debug adapter failed to start: " + argv[0]);
        return;
    }
    program = prog.string();
    programArgs = std::move(projArgs);
    adapterType = type;
    launchExtras = std::move(extras);
    sessionActive = true;
    panelVisible = true;
    consoleText += "[adapter] " + argv[0] + " started\n";
    consoleScrollDown = true;
}

void DebuggerPlugin::stopSession(PluginHost &host)
{
    if (dapClient.spawned())
    {
        dapClient.disconnect(/*terminateDebuggee*/ true);   // best-effort grace…
        dapClient.stop();                                   // …then force
    }
    sessionActive = false;
    launchSent = false;
    stopped = false;
    stopFile.clear();
    stopLine = -1;
    frames.clear();
    scopes.clear();
    varChildren.clear();
    varRequested.clear();
    host.hostRefreshMarkers();
}

void DebuggerPlugin::pollDap(PluginHost &host)
{
    if (!dapClient.spawned())
        return;
    for (auto &r : dapClient.poll())
    {
        switch (r.kind)
        {
            case dap::ResultKind::Initialize:
                if (r.success && !launchSent)
                {
                    std::filesystem::path prog(program);
                    dapClient.launch(adapterType, program, prog.parent_path().string(),
                                     /*stopOnEntry*/ false, launchExtras, programArgs);
                    launchSent = true;
                }
                break;

            case dap::ResultKind::EvInitialized:
                for (auto &kv : breakpoints)
                    sendBreakpointsFor(kv.first);
                dapClient.configurationDone();
                break;

            case dap::ResultKind::Launch:
                if (!r.success)
                {
                    consoleText += "[error] launch failed — is the adapter installed? "
                                   "(python: pip install debugpy)\n";
                    consoleScrollDown = true;
                    stopSession(host);
                }
                break;

            case dap::ResultKind::EvStopped:
                stopped = true;
                stopReason = r.stopped.reason;
                threadId = r.stopped.threadId;
                dapClient.stackTrace(threadId);
                break;

            case dap::ResultKind::StackTrace:
                frames = r.frames;
                if (!frames.empty())
                {
                    const auto &top = frames.front();
                    currentFrame = top.id;
                    if (!top.sourcePath.empty())
                    {
                        stopFile = canonPath(top.sourcePath);
                        stopLine = top.line - 1;   // wire is 1-based
                        host.hostJumpTo(top.sourcePath, stopLine);
                    }
                    host.hostRefreshMarkers();
                    dapClient.scopes(top.id);
                }
                break;

            case dap::ResultKind::Scopes:
                scopes = r.scopes;
                varChildren.clear();
                varRequested.clear();
                for (auto &s : scopes)
                    if (!s.expensive && s.variablesReference > 0 &&
                        varRequested.insert(s.variablesReference).second)
                        dapClient.variables(s.variablesReference);
                break;

            case dap::ResultKind::Variables:
                varRequested.erase(r.requestContext);
                varChildren[r.requestContext] = r.variables;
                break;

            case dap::ResultKind::Evaluate:
                consoleText += (r.success && !r.evaluateResult.empty())
                                   ? ("=> " + r.evaluateResult + "\n")
                                   : "=> (error)\n";
                consoleScrollDown = true;
                break;

            case dap::ResultKind::Continue:
            case dap::ResultKind::Next:
            case dap::ResultKind::StepIn:
            case dap::ResultKind::StepOut:
            case dap::ResultKind::EvContinued:
                stopped = false;
                stopFile.clear();
                stopLine = -1;
                frames.clear();
                scopes.clear();
                host.hostRefreshMarkers();
                break;

            case dap::ResultKind::EvOutput:
                consoleText += r.outputText;
                if (!consoleText.empty() && consoleText.back() != '\n')
                    consoleText += '\n';
                consoleScrollDown = true;
                break;

            case dap::ResultKind::EvExited:
                consoleText += "[exited] code " + std::to_string(r.exitCode) + "\n";
                consoleScrollDown = true;
                break;

            case dap::ResultKind::EvTerminated:
            case dap::ResultKind::AdapterGone:
                consoleText += r.kind == dap::ResultKind::EvTerminated ? "[terminated]\n"
                                                                       : "[adapter gone]\n";
                consoleScrollDown = true;
                stopSession(host);
                break;

            default:
                break;   // SetBreakpoints / ConfigurationDone / Pause / Disconnect acks
        }
    }
}

// ── hooks: frame / keybinds / menu / palette ─────────────────────────────

void DebuggerPlugin::onFrame(PluginHost &host)
{
    pollDap(host);
    renderPanel(host);
}

void DebuggerPlugin::contributeKeybinds(PluginHost &host, std::vector<PluginKeybind> &out)
{
    PluginHost *h = &host;
    // Conditional emission is the precedence mechanism: only bind chords while
    // they mean something, so the core's own F5=Run / F11=Focus win otherwise.
    std::string active = host.hostActiveFilename();
    if (!active.empty() && active != "untitled")
        out.push_back({"debugger.breakpoint", "Toggle breakpoint", "F9",
                       [this, h] { toggleBreakpointAtCursor(*h); }, {}});
    if (sessionActive)
        out.push_back({"debugger.stop", "Stop debugging", "Shift+F5",
                       [this, h] { stopSession(*h); }, {}});
    if (stopped)
    {
        out.push_back({"debugger.continue", "Continue", "F5",
                       [this] { dapClient.continueExec(threadId); }, {}});
        out.push_back({"debugger.stepOver", "Step over", "F10",
                       [this] { dapClient.next(threadId); }, {}});
        out.push_back({"debugger.stepInto", "Step into", "F11",
                       [this] { dapClient.stepIn(threadId); }, {}});
        out.push_back({"debugger.stepOut", "Step out", "Shift+F11",
                       [this] { dapClient.stepOut(threadId); }, {}});
    }
}

void DebuggerPlugin::onTopLevelMenu(PluginHost &host)
{
    if (!sessionActive)
    {
        if (ImGui::MenuItem("Start Debugging"))
            startSession(host);
    }
    else
    {
        if (ImGui::MenuItem("Stop Debugging", "Shift+F5"))
            stopSession(host);
        ImGui::BeginDisabled(!stopped);
        if (ImGui::MenuItem("Continue", "F5"))
            dapClient.continueExec(threadId);
        if (ImGui::MenuItem("Step Over", "F10"))
            dapClient.next(threadId);
        if (ImGui::MenuItem("Step Into", "F11"))
            dapClient.stepIn(threadId);
        if (ImGui::MenuItem("Step Out", "Shift+F11"))
            dapClient.stepOut(threadId);
        ImGui::EndDisabled();
    }
    if (ImGui::MenuItem("Toggle Breakpoint", "F9"))
        toggleBreakpointAtCursor(host);
    ImGui::MenuItem("Debug Panel", nullptr, &panelVisible);
    ImGui::Separator();
    if (ImGui::MenuItem("Launch in raddbg"))
        debugInRadDbg(host);
    if (ImGui::MenuItem("Launch in Visual Studio"))
        debugInVisualStudio(host);
}

void DebuggerPlugin::contributePaletteCommands(PluginHost &host, const PluginDocInfo &,
                                               const std::function<void(const std::string &,
                                                                        std::function<void()>)> &add)
{
    PluginHost *h = &host;
    if (!sessionActive)
        add("Debug: Start Debugging", [this, h] { startSession(*h); });
    else
    {
        add("Debug: Stop", [this, h] { stopSession(*h); });
        if (stopped)
        {
            add("Debug: Continue", [this] { dapClient.continueExec(threadId); });
            add("Debug: Step Over", [this] { dapClient.next(threadId); });
            add("Debug: Step Into", [this] { dapClient.stepIn(threadId); });
            add("Debug: Step Out", [this] { dapClient.stepOut(threadId); });
        }
    }
    add("Debug: Toggle Breakpoint", [this, h] { toggleBreakpointAtCursor(*h); });
    add("Debug: Panel / Configuration...", [this] { panelVisible = true; });
    add("Debug: Launch in raddbg", [this, h] { debugInRadDbg(*h); });
    add("Debug: Launch in Visual Studio", [this, h] { debugInVisualStudio(*h); });
}

// ── the Debug panel ──────────────────────────────────────────────────────

void DebuggerPlugin::renderPanel(PluginHost &host)
{
    if (!panelVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug###debugPanel", &panelVisible))
    {
        if (!sessionActive)
        {
            std::string projKey = projectKey(host);
            std::string active = host.hostActiveFilename();
            std::string ext;
            if (!active.empty() && active != "untitled")
            {
                ext = std::filesystem::path(active).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
            }

            if (ImGui::Button("Start Debugging"))
                startSession(host);
            ImGui::SameLine();
            // Preview the SAME resolution startSession will do.
            std::string hint;
            if (auto it = projectAdapter.find(projKey); !projKey.empty() && it != projectAdapter.end())
                hint = "project adapter: " + it->second;
            else if (!ext.empty())
            {
                std::string type;
                std::vector<std::pair<std::string, std::string>> extras;
                auto argv = adapterFor(host, ext, active, type, extras);
                if (argv.empty())
                    hint = "no adapter for " + ext + " — configure below";
                else
                {
                    hint = "adapter: ";
                    for (size_t i = 0; i < argv.size(); ++i)
                        hint += (i ? " " : "") + argv[i];
                }
            }
            else
                hint = "(no active document — set a project target below)";
            ImGui::TextDisabled("%s", hint.c_str());
            ImGui::TextDisabled("F9 toggles a breakpoint on the cursor line.");

            // ── Configuration: per-project adapter + target, per-type adapter ──
            if (ImGui::CollapsingHeader("Configuration"))
            {
                static std::string cfgShownFor = "\x01";
                static char cfgAdapter[512] = "";
                static char cfgTarget[512] = "";
                static char cfgArgs[512] = "";
                static char cfgExtAdapter[512] = "";
                std::string wantShown = projKey + "\x1f" + ext;
                if (cfgShownFor != wantShown)
                {
                    cfgShownFor = wantShown;
                    cfgAdapter[0] = cfgTarget[0] = cfgArgs[0] = cfgExtAdapter[0] = 0;
                    if (auto it = projectAdapter.find(projKey); it != projectAdapter.end())
                        std::snprintf(cfgAdapter, sizeof(cfgAdapter), "%s", it->second.c_str());
                    if (auto it = projectTarget.find(projKey); it != projectTarget.end())
                    {
                        auto bar = it->second.find('|');
                        std::snprintf(cfgTarget, sizeof(cfgTarget), "%s", it->second.substr(0, bar).c_str());
                        if (bar != std::string::npos)
                            std::snprintf(cfgArgs, sizeof(cfgArgs), "%s", it->second.substr(bar + 1).c_str());
                    }
                    if (auto it = adapterOverrides.find(ext); !ext.empty() && it != adapterOverrides.end())
                        std::snprintf(cfgExtAdapter, sizeof(cfgExtAdapter), "%s", it->second.c_str());
                }

                if (projKey.empty())
                    ImGui::TextDisabled("No project open — open one to set a project association.");
                else
                {
                    ImGui::Text("Project: %s", std::filesystem::path(projKey).filename().string().c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", projKey.c_str());
                    ImGui::SetNextItemWidth(-140.0f);
                    ImGui::InputTextWithHint("Adapter##proj", "empty = auto by file type (e.g. vsdbg --interpreter=vscode)",
                                             cfgAdapter, sizeof(cfgAdapter));
                    ImGui::SetNextItemWidth(-140.0f);
                    ImGui::InputTextWithHint("Target##proj", "program to debug (empty = built exe / active file)",
                                             cfgTarget, sizeof(cfgTarget));
                    ImGui::SameLine(0.0f, 4.0f);
                    if (ImGui::SmallButton("built exe"))
                    {
                        auto exe = host.hostFindBuiltExe();
                        if (!exe.empty())
                            std::snprintf(cfgTarget, sizeof(cfgTarget), "%s", exe.c_str());
                        else
                            host.hostToast("No built executable found — build first (F6)");
                    }
                    ImGui::SameLine(0.0f, 4.0f);
                    if (ImGui::SmallButton("active file"))
                    {
                        if (!active.empty() && active != "untitled")
                            std::snprintf(cfgTarget, sizeof(cfgTarget), "%s", active.c_str());
                    }
                    ImGui::SetNextItemWidth(-140.0f);
                    ImGui::InputTextWithHint("Arguments##proj", "debuggee argv (space separated)",
                                             cfgArgs, sizeof(cfgArgs));
                }
                if (!ext.empty())
                {
                    ImGui::SetNextItemWidth(-140.0f);
                    ImGui::InputTextWithHint((std::string("Adapter for ") + ext + "##ext").c_str(),
                                             "empty = built-in default", cfgExtAdapter, sizeof(cfgExtAdapter));
                }

                if (ImGui::Button("Save Configuration"))
                {
                    if (!projKey.empty())
                    {
                        if (cfgAdapter[0]) projectAdapter[projKey] = cfgAdapter;
                        else               projectAdapter.erase(projKey);
                        std::string tgt = cfgTarget;
                        if (!tgt.empty() && cfgArgs[0])
                            tgt += std::string("|") + cfgArgs;
                        if (!tgt.empty()) projectTarget[projKey] = tgt;
                        else              projectTarget.erase(projKey);
                    }
                    if (!ext.empty())
                    {
                        if (cfgExtAdapter[0]) adapterOverrides[ext] = cfgExtAdapter;
                        else                  adapterOverrides.erase(ext);
                    }
                    saveConfig();
                    host.hostToast("Debug configuration saved");
                }
                ImGui::SameLine();
                if (ImGui::Button("Detect Adapters"))
                    nativeAdapterDetected = false;   // re-probe on next resolution

                {
                    std::string type;
                    std::vector<std::pair<std::string, std::string>> extras;
                    auto nat = adapterFor(host, ".cpp", {}, type, extras);
                    if (nat.empty())
                        ImGui::TextDisabled("native (C/C++): none found — install VS Code C++ tools, lldb-dap, or gdb 14+");
                    else
                    {
                        std::string s;
                        for (size_t i = 0; i < nat.size(); ++i)
                            s += (i ? " " : "") + nat[i];
                        ImGui::TextDisabled("native (C/C++): %s", s.c_str());
                    }
                    ImGui::TextDisabled("python: debugpy via the project interpreter (pip install debugpy)");
                }
            }
        }
        else
        {
            ImGui::BeginDisabled(!stopped);
            if (ImGui::Button("Continue"))
                dapClient.continueExec(threadId);
            ImGui::SameLine();
            if (ImGui::Button("Over"))
                dapClient.next(threadId);
            ImGui::SameLine();
            if (ImGui::Button("Into"))
                dapClient.stepIn(threadId);
            ImGui::SameLine();
            if (ImGui::Button("Out"))
                dapClient.stepOut(threadId);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(stopped);
            if (ImGui::Button("Pause"))
                dapClient.pause(threadId != 0 ? threadId : 1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                stopSession(host);

            if (stopped)
                ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1.0f), "Paused (%s)  %s:%d",
                                   stopReason.c_str(),
                                   stopFile.empty() ? "?" : std::filesystem::path(stopFile).filename().string().c_str(),
                                   stopLine + 1);
            else
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Running — %s",
                                   std::filesystem::path(program).filename().string().c_str());
        }

        // ── Call stack ───────────────────────────────────────────────────
        ImGui::SeparatorText("Call stack");
        ImGui::BeginChild("##dbgStack", ImVec2(0.0f, 110.0f), ImGuiChildFlags_None);
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const auto &fr = frames[i];
            ImGui::PushID((int) i);
            std::string leaf = fr.sourcePath.empty()
                                   ? std::string("<no source>")
                                   : std::filesystem::path(fr.sourcePath).filename().string();
            char row[512];
            std::snprintf(row, sizeof(row), "%s  \xe2\x80\x94  %s:%d", fr.name.c_str(), leaf.c_str(), fr.line);
            if (ImGui::Selectable(row, fr.id == currentFrame))
            {
                currentFrame = fr.id;
                dapClient.scopes(fr.id);
                if (!fr.sourcePath.empty())
                    host.hostJumpTo(fr.sourcePath, fr.line - 1);
            }
            if (!fr.sourcePath.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s:%d", fr.sourcePath.c_str(), fr.line);
            ImGui::PopID();
        }
        if (frames.empty())
            ImGui::TextDisabled(sessionActive ? "(running)" : "(no session)");
        ImGui::EndChild();

        // ── Variables ────────────────────────────────────────────────────
        ImGui::SeparatorText("Variables");
        ImGui::BeginChild("##dbgVars", ImVec2(0.0f, 160.0f), ImGuiChildFlags_None);
        std::function<void(const dap::Variable &, int)> renderVar =
            [&](const dap::Variable &v, int depth) {
                ImGui::PushID(v.name.c_str());
                if (v.variablesReference > 0 && depth < 8)
                {
                    bool open = ImGui::TreeNode("##var", "%s = %s", v.name.c_str(), v.value.c_str());
                    if (!v.type.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", v.type.c_str());
                    if (open)
                    {
                        auto it = varChildren.find(v.variablesReference);
                        if (it == varChildren.end())
                        {
                            if (varRequested.insert(v.variablesReference).second)
                                dapClient.variables(v.variablesReference);
                            ImGui::TextDisabled("(loading\xe2\x80\xa6)");
                        }
                        else
                            for (const auto &c : it->second)
                                renderVar(c, depth + 1);
                        ImGui::TreePop();
                    }
                }
                else
                {
                    ImGui::BulletText("%s = %s", v.name.c_str(), v.value.c_str());
                    if (!v.type.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", v.type.c_str());
                }
                ImGui::PopID();
            };
        for (auto &s : scopes)
        {
            if (ImGui::TreeNodeEx(s.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto it = varChildren.find(s.variablesReference);
                if (it == varChildren.end())
                {
                    if (s.variablesReference > 0 &&
                        varRequested.insert(s.variablesReference).second)
                        dapClient.variables(s.variablesReference);
                    ImGui::TextDisabled("(loading\xe2\x80\xa6)");
                }
                else
                    for (const auto &v : it->second)
                        renderVar(v, 0);
                ImGui::TreePop();
            }
        }
        if (scopes.empty())
            ImGui::TextDisabled(stopped ? "(loading\xe2\x80\xa6)" : "(pause to inspect)");
        ImGui::EndChild();

        // ── Console (output events + REPL) ──────────────────────────────
        ImGui::SeparatorText("Console");
        float inputH = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##dbgConsole", ImVec2(0.0f, -inputH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        host.hostMiddleMousePanScroll(130);   // honors the invert-pan preference
        ImGui::TextUnformatted(consoleText.c_str());
        if (consoleScrollDown)
        {
            ImGui::SetScrollHereY(1.0f);
            consoleScrollDown = false;
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##dbgEval", "evaluate expression (Enter)\xe2\x80\xa6",
                                     evalBuf, sizeof(evalBuf), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (evalBuf[0] && dapClient.ready())
            {
                consoleText += std::string("> ") + evalBuf + "\n";
                consoleScrollDown = true;
                dapClient.evaluate(evalBuf, stopped ? currentFrame : 0);
                evalBuf[0] = 0;
                ImGui::SetKeyboardFocusHere(-1);   // keep typing
            }
        }
    }
    ImGui::End();
}

// ── External native debuggers (raddbg / Visual Studio) ─────────────────

std::string DebuggerPlugin::bridgeToolPath(const char *key, std::string (*detect)()) const
{
    auto it = bridgeSettings.find(key);
    if (it != bridgeSettings.end() && !it->second.empty())
        return it->second;
    return detect();
}

bool DebuggerPlugin::externalDebugTarget(PluginHost &host, std::string &exe, std::string &args, std::string &why)
{
    exe.clear();
    args.clear();
    auto built = host.hostFindBuiltExe();
    if (!built.empty())
    {
        exe = built;
        return true;
    }
    std::string active = host.hostActiveFilename();
    if (!active.empty() && active != "untitled")
    {
        auto ext = std::filesystem::path(active).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (ext == ".exe")
        {
            exe = active;
            return true;
        }
    }
    why = "no built executable found — build first (F6)";
    return false;
}

void DebuggerPlugin::launchDetached(PluginHost &host, const std::vector<std::string> &argv)
{
    if (argv.empty())
        return;
    std::vector<const char *> args;
    args.reserve(argv.size() + 1);
    for (auto &a : argv)
        args.push_back(a.c_str());
    args.push_back(nullptr);
    SDL_Process *p = SDL_CreateProcess(args.data(), /*pipe_stdio*/ false);
    if (!p)
    {
        host.hostToast("Failed to launch " + argv[0]);
        return;
    }
    SDL_DestroyProcess(p);   // releases the handle; the child keeps running
}

void DebuggerPlugin::debugInRadDbg(PluginHost &host)
{
    std::string raddbg = bridgeToolPath("raddbg", dbgbridge::findRadDbg);
    if (raddbg.empty())
    {
        host.hostToast("raddbg not found — set its path in debugger.ini [bridge] raddbg=");
        return;
    }
    std::string exe, args, why;
    if (!externalDebugTarget(host, exe, args, why))
    {
        host.hostToast("raddbg: " + why);
        return;
    }
    launchDetached(host, dbgbridge::raddbgLaunch(raddbg, exe, args));
    std::vector<std::pair<std::string, int>> bps;
    for (auto &kv : breakpoints)
        for (int line : kv.second)
            bps.emplace_back(kv.first, line + 1);
    std::string tmpl;
    if (auto it = bridgeSettings.find("raddbg_bp_template"); it != bridgeSettings.end())
        tmpl = it->second;
    for (auto &cmd : dbgbridge::raddbgBreakpointCmds(raddbg, tmpl, bps))
        launchDetached(host, cmd);
    host.hostToast("Launched raddbg" + (bps.empty() ? std::string() : " (+" + std::to_string(bps.size()) + " breakpoints)"));
}

void DebuggerPlugin::debugInVisualStudio(PluginHost &host)
{
    std::string devenv = bridgeToolPath("devenv", dbgbridge::findDevenv);
    if (devenv.empty())
    {
        host.hostToast("Visual Studio (devenv) not found — set its path in debugger.ini [bridge] devenv=");
        return;
    }
    std::string exe, args, why;
    if (!externalDebugTarget(host, exe, args, why))
    {
        host.hostToast("VS debugger: " + why);
        return;
    }
    launchDetached(host, dbgbridge::devenvDebugExe(devenv, exe, args));
    host.hostToast("Launched Visual Studio debugger");
}

std::unique_ptr<EditorPlugin> createDebuggerPlugin()
{
    return std::make_unique<DebuggerPlugin>();
}
