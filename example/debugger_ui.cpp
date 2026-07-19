//
//  debugger_ui.cpp — Editor debugger (DAP) UI + native debugger bridges.
//  Ported from the performance-optimization-roadmap branch onto the plugin
//  architecture. See the Debugger (DAP) section in editor.h.
//

#include "editor.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <SDL3/SDL_process.h>

#include "imgui.h"
#include "debug_bridge.h"

std::string Editor::dbgCanonPath(const std::string &file) const
{
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(file, ec);
    return ec ? file : c.string();
}

// Key the per-project debug config on the canonical project root.
std::string Editor::dbgProjectKey() const
{
    if (projectRoot.empty())
        return {};
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(projectRoot, ec);
    return ec ? projectRoot.string() : c.string();
}


// Add the debug markers for one tab: red dots on breakpoint lines, a yellow
// line where execution is stopped. PURE ADDER — no clearing; it is the top
// layer of Editor::refreshMarkers (the single composer for the shared marker
// list), so notes and external-change markers survive debug events.
void Editor::applyDebugMarkers(TabDocument &t)
{
    std::string canon = t.filename == "untitled" ? std::string() : dbgCanonPath(t.filename);
    const std::set<int> *bps = nullptr;
    if (!canon.empty())
    {
        auto it = dbgBreakpoints.find(canon);
        if (it != dbgBreakpoints.end() && !it->second.empty())
            bps = &it->second;
    }
    bool stopHere = dbgStopped && !dbgStopFile.empty() && canon == dbgStopFile && dbgStopLine >= 0;

    t.debugMarkers = bps != nullptr || stopHere;
    if (!t.debugMarkers)
        return;

    int lineCount = t.editor.GetLineCount();
    if (bps)
        for (int line : *bps)
            if (line >= 0 && line < lineCount)
                t.editor.AddMarker(line, IM_COL32(225, 85, 85, 255), 0,
                                   "Breakpoint (F9 to remove)", "");
    if (stopHere && dbgStopLine < lineCount)
        t.editor.AddMarker(dbgStopLine, IM_COL32(240, 200, 90, 255), IM_COL32(240, 200, 90, 42),
                           "Stopped here", dbgStopReason);
}

void Editor::refreshAllDebugMarkers()
{
    for (auto &up : tabs)
        refreshMarkers(*up);   // full composition — change + notes + debug
}

void Editor::toggleBreakpointAtCursor()
{
    if (tabs.empty() || doc().filename == "untitled")
        return;   // the adapter needs a real on-disk path
    int line = 0, col = 0;
    doc().editor.GetMainCursor(line, col);
    std::string canon = dbgCanonPath(doc().filename);
    auto &set = dbgBreakpoints[canon];
    if (!set.erase(line))
        set.insert(line);
    if (set.empty())
        dbgBreakpoints.erase(canon);
    refreshMarkers(doc());
    if (dbgSessionActive)
        sendBreakpointsFor(canon);
}

void Editor::sendBreakpointsFor(const std::string &canonPath)
{
    std::vector<int> lines1;
    auto it = dbgBreakpoints.find(canonPath);
    if (it != dbgBreakpoints.end())
        for (int l : it->second)
            lines1.push_back(l + 1);   // DAP lines are 1-based
    dapClient.setBreakpoints(canonPath, lines1);
}

bool Editor::isNativeDebugExt(const std::string &extLower)
{
    return extLower == ".c" || extLower == ".cpp" || extLower == ".cc" || extLower == ".cxx" ||
           extLower == ".h" || extLower == ".hpp" || extLower == ".hxx" || extLower == ".hh" ||
           extLower == ".inl" || extLower == ".exe";
}

// Resolve the adapter command for a file extension. The settings override
// ([debug_adapters] ".ext=cmd arg …", split on spaces) wins; Python falls back
// to debugpy through the same interpreter the script runner would use; the
// C-family bridges into the best native DAP adapter found on this machine
// (Microsoft's vsdbg / OpenDebugAD7 from the VS Code C++ tools, lldb-dap,
// or gdb 14+'s built-in DAP interpreter), detected once and cached.
std::vector<std::string> Editor::debugAdapterFor(const std::string &ext,
                                                 const std::filesystem::path &scriptPath,
                                                 std::string &adapterType,
                                                 std::vector<std::pair<std::string, std::string>> &launchExtras) const
{
    adapterType.clear();
    launchExtras.clear();
    auto ov = debugAdapterOverrides.find(ext);
    if (ov != debugAdapterOverrides.end() && !ov->second.empty())
        return dbgbridge::splitCommandLine(ov->second);
    if (ext == ".py" || ext == ".pyw")
    {
        adapterType = "python";
        std::string python;
        auto it = interpreterOverrides.find(".py");
        if (it != interpreterOverrides.end() && !it->second.empty())
            python = it->second;
        if (python.empty())
            python = venvPythonFor(scriptPath);
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
                // Microsoft VS debugger engine (the VS Code "cppvsdbg" plugin).
                nativeAdapterArgv = {vsdbg, "--interpreter=vscode"};
                nativeAdapterType = "cppvsdbg";
            }
            else if (auto ad7 = dbgbridge::findOpenDebugAD7();
                     !ad7.empty() && dbgbridge::commandOnPath("gdb"))
            {
                // Microsoft MIEngine (the VS Code "cppdbg" plugin) over gdb.
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
                // gdb >= 14 ships a native DAP interpreter.
                nativeAdapterArgv = {"gdb", "-i", "dap"};
            }
        }
        adapterType = nativeAdapterType;
        launchExtras = nativeAdapterExtras;
        return nativeAdapterArgv;
    }
    return {};
}

void Editor::startDebugSession()
{
    // ── Resolve what to debug + which adapter. The per-PROJECT association
    // (Debug panel > Configuration) wins over the per-extension mapping; with a
    // project target configured, no document even needs to be focused.
    std::string projKey = dbgProjectKey();
    std::string projAdapter;
    std::string projProgram;
    std::vector<std::string> projArgs;
    if (!projKey.empty())
    {
        if (auto it = dbgProjectAdapter.find(projKey); it != dbgProjectAdapter.end())
            projAdapter = it->second;
        if (auto it = dbgProjectTarget.find(projKey); it != dbgProjectTarget.end() && !it->second.empty())
        {
            auto bar = it->second.find('|');
            projProgram = it->second.substr(0, bar);
            if (bar != std::string::npos)
                projArgs = dbgbridge::splitCommandLine(it->second.substr(bar + 1));
        }
    }

    bool haveDoc = !tabs.empty() && doc().filename != "untitled";
    if (!haveDoc && projProgram.empty())
    {
        pushToast("Debug: open a file, or set a project target in Debug panel > Configuration",
                  IM_COL32(240, 200, 90, 255));
        return;
    }
    if (haveDoc && isSavable())
        saveFile();   // debug what's on screen, not a stale disk copy

    std::filesystem::path prog = !projProgram.empty() ? std::filesystem::path(projProgram)
                                                      : std::filesystem::path(doc().filename);
    auto ext = prog.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    std::string type;
    std::vector<std::pair<std::string, std::string>> extras;
    std::vector<std::string> argv;
    if (!projAdapter.empty())
    {
        argv = dbgbridge::splitCommandLine(projAdapter);
        type = dbgbridge::inferAdapterType(argv);
        if (type == "cppdbg")   // MIEngine wants its MI backend named
            extras = {{"MIMode", "gdb"}, {"miDebuggerPath", "gdb"}};
    }
    else
        argv = debugAdapterFor(ext, prog, type, extras);
    if (argv.empty())
    {
        pushToast("No debug adapter for " + ext + " — configure one in Debug panel > Configuration",
                  IM_COL32(240, 110, 90, 255));
        dbgPanelVisible = true;   // put the fix next to the failure
        return;
    }
    // C-family sources aren't the debuggee — the built executable is (unless an
    // explicit project target already names the program to run).
    if (projProgram.empty() && isNativeDebugExt(ext) && ext != ".exe")
    {
        auto exe = findBuiltExe();
        if (exe.empty())
        {
            pushToast("Debug: no built executable found — build first (F6)", IM_COL32(240, 200, 90, 255));
            return;
        }
        prog = exe;
    }

    if (dapClient.spawned())
        dapClient.stop();   // stale session
    dbgSessionActive = false;
    dbgLaunchSent = false;
    dbgStopped = false;
    dbgStopFile.clear();
    dbgStopLine = -1;
    dbgThreadId = 0;
    dbgCurrentFrame = 0;
    dbgFrames.clear();
    dbgScopes.clear();
    dbgVarChildren.clear();
    dbgVarRequested.clear();
    dbgConsole.clear();

    if (!dapClient.start(argv))
    {
        pushToast("Debug adapter failed to start: " + argv[0], IM_COL32(240, 110, 90, 255));
        return;
    }
    dbgProgram = prog.string();
    dbgProgramArgs = std::move(projArgs);
    dbgAdapterType = type;
    dbgLaunchExtras = std::move(extras);
    dbgSessionActive = true;
    dbgPanelVisible = true;
    dbgConsole += "[adapter] " + argv[0] + " started\n";
    dbgConsoleScrollDown = true;
}

void Editor::stopDebugSession()
{
    if (dapClient.spawned())
    {
        dapClient.disconnect(/*terminateDebuggee*/ true);   // best-effort grace…
        dapClient.stop();                                   // …then force
    }
    dbgSessionActive = false;
    dbgLaunchSent = false;
    dbgStopped = false;
    dbgStopFile.clear();
    dbgStopLine = -1;
    dbgFrames.clear();
    dbgScopes.clear();
    dbgVarChildren.clear();
    dbgVarRequested.clear();
    refreshAllDebugMarkers();
}

void Editor::dbgJumpTo(const std::string &file, int line0)
{
    navHistory.record(currentNavLocation());   // Back returns to pre-stop position
    openFile(file);
    if (!tabs.empty())
    {
        auto &ed = doc().editor;
        ed.SetCursor(line0, 0);
        ed.ScrollToLine(line0, TextEditor::Scroll::alignMiddle);
    }
}

void Editor::pollDap()
{
    if (!dapClient.spawned())
        return;
    for (auto &r : dapClient.poll())
    {
        switch (r.kind)
        {
            case dap::ResultKind::Initialize:
                // Capabilities negotiated → fire the launch.
                if (r.success && !dbgLaunchSent)
                {
                    std::filesystem::path prog(dbgProgram);
                    dapClient.launch(dbgAdapterType, dbgProgram, prog.parent_path().string(),
                                     /*stopOnEntry*/ false, dbgLaunchExtras, dbgProgramArgs);
                    dbgLaunchSent = true;
                }
                break;

            case dap::ResultKind::EvInitialized:
                // Adapter is ready for configuration: send every file's
                // breakpoints, then configurationDone starts the debuggee.
                for (auto &kv : dbgBreakpoints)
                    sendBreakpointsFor(kv.first);
                dapClient.configurationDone();
                break;

            case dap::ResultKind::Launch:
                if (!r.success)
                {
                    dbgConsole += "[error] launch failed — is the adapter installed? "
                                  "(python: pip install debugpy)\n";
                    dbgConsoleScrollDown = true;
                    stopDebugSession();
                }
                break;

            case dap::ResultKind::EvStopped:
                dbgStopped = true;
                dbgStopReason = r.stopped.reason;
                dbgThreadId = r.stopped.threadId;
                dapClient.stackTrace(dbgThreadId);
                break;

            case dap::ResultKind::StackTrace:
                dbgFrames = r.frames;
                if (!dbgFrames.empty())
                {
                    const auto &top = dbgFrames.front();
                    dbgCurrentFrame = top.id;
                    if (!top.sourcePath.empty())
                    {
                        dbgStopFile = dbgCanonPath(top.sourcePath);
                        dbgStopLine = top.line - 1;   // wire is 1-based
                        dbgJumpTo(top.sourcePath, dbgStopLine);
                    }
                    refreshAllDebugMarkers();
                    dapClient.scopes(top.id);
                }
                break;

            case dap::ResultKind::Scopes:
                dbgScopes = r.scopes;
                dbgVarChildren.clear();
                dbgVarRequested.clear();
                // Fetch the cheap scopes' top-level variables right away.
                for (auto &s : dbgScopes)
                    if (!s.expensive && s.variablesReference > 0 &&
                        dbgVarRequested.insert(s.variablesReference).second)
                        dapClient.variables(s.variablesReference);
                break;

            case dap::ResultKind::Variables:
                dbgVarRequested.erase(r.requestContext);
                dbgVarChildren[r.requestContext] = r.variables;
                break;

            case dap::ResultKind::Evaluate:
                dbgConsole += (r.success && !r.evaluateResult.empty())
                                  ? ("=> " + r.evaluateResult + "\n")
                                  : "=> (error)\n";
                dbgConsoleScrollDown = true;
                break;

            case dap::ResultKind::Continue:
            case dap::ResultKind::Next:
            case dap::ResultKind::StepIn:
            case dap::ResultKind::StepOut:
            case dap::ResultKind::EvContinued:
                // Running again — clear the stop state until the next stop event.
                dbgStopped = false;
                dbgStopFile.clear();
                dbgStopLine = -1;
                dbgFrames.clear();
                dbgScopes.clear();
                refreshAllDebugMarkers();
                break;

            case dap::ResultKind::EvOutput:
                dbgConsole += r.outputText;
                if (!dbgConsole.empty() && dbgConsole.back() != '\n')
                    dbgConsole += '\n';
                dbgConsoleScrollDown = true;
                break;

            case dap::ResultKind::EvExited:
                dbgConsole += "[exited] code " + std::to_string(r.exitCode) + "\n";
                dbgConsoleScrollDown = true;
                break;

            case dap::ResultKind::EvTerminated:
            case dap::ResultKind::AdapterGone:
                dbgConsole += r.kind == dap::ResultKind::EvTerminated ? "[terminated]\n"
                                                                      : "[adapter gone]\n";
                dbgConsoleScrollDown = true;
                stopDebugSession();
                break;

            default:
                break;   // SetBreakpoints / ConfigurationDone / Pause / Disconnect acks
        }
    }
}

void Editor::renderDebugPanel()
{
    if (!dbgPanelVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug###debugPanel", &dbgPanelVisible))
    {
        if (!dbgSessionActive)
        {
            std::string projKey = dbgProjectKey();
            std::string ext;
            if (!tabs.empty() && doc().filename != "untitled")
            {
                ext = std::filesystem::path(doc().filename).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
            }

            if (ImGui::Button("Start Debugging"))
                startDebugSession();
            ImGui::SameLine();
            // Preview the SAME resolution startDebugSession will do: project
            // association first, then the per-extension mapping.
            std::string hint;
            if (auto it = dbgProjectAdapter.find(projKey); !projKey.empty() && it != dbgProjectAdapter.end())
                hint = "project adapter: " + it->second;
            else if (!ext.empty())
            {
                std::filesystem::path p(doc().filename);
                std::string type;
                std::vector<std::pair<std::string, std::string>> extras;
                auto argv = debugAdapterFor(ext, p, type, extras);
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

            // ── Configuration: per-project adapter + target, per-type adapter. ──
            if (ImGui::CollapsingHeader("Configuration"))
            {
                static std::string cfgShownFor = "\x01";   // refill buffers on project/ext change
                static char cfgAdapter[512] = "";
                static char cfgTarget[512] = "";
                static char cfgArgs[512] = "";
                static char cfgExtAdapter[512] = "";
                std::string wantShown = projKey + "\x1f" + ext;
                if (cfgShownFor != wantShown)
                {
                    cfgShownFor = wantShown;
                    cfgAdapter[0] = cfgTarget[0] = cfgArgs[0] = cfgExtAdapter[0] = 0;
                    if (auto it = dbgProjectAdapter.find(projKey); it != dbgProjectAdapter.end())
                        std::snprintf(cfgAdapter, sizeof(cfgAdapter), "%s", it->second.c_str());
                    if (auto it = dbgProjectTarget.find(projKey); it != dbgProjectTarget.end())
                    {
                        auto bar = it->second.find('|');
                        std::snprintf(cfgTarget, sizeof(cfgTarget), "%s", it->second.substr(0, bar).c_str());
                        if (bar != std::string::npos)
                            std::snprintf(cfgArgs, sizeof(cfgArgs), "%s", it->second.substr(bar + 1).c_str());
                    }
                    if (auto it = debugAdapterOverrides.find(ext); !ext.empty() && it != debugAdapterOverrides.end())
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
                        auto exe = findBuiltExe();
                        if (!exe.empty())
                            std::snprintf(cfgTarget, sizeof(cfgTarget), "%s", exe.string().c_str());
                        else
                            pushToast("No built executable found — build first (F6)", IM_COL32(240, 200, 90, 255));
                    }
                    ImGui::SameLine(0.0f, 4.0f);
                    if (ImGui::SmallButton("active file"))
                    {
                        if (!tabs.empty() && doc().filename != "untitled")
                            std::snprintf(cfgTarget, sizeof(cfgTarget), "%s", doc().filename.c_str());
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
                        if (cfgAdapter[0]) dbgProjectAdapter[projKey] = cfgAdapter;
                        else               dbgProjectAdapter.erase(projKey);
                        std::string tgt = cfgTarget;
                        if (!tgt.empty() && cfgArgs[0])
                            tgt += std::string("|") + cfgArgs;
                        if (!tgt.empty()) dbgProjectTarget[projKey] = tgt;
                        else              dbgProjectTarget.erase(projKey);
                    }
                    if (!ext.empty())
                    {
                        if (cfgExtAdapter[0]) debugAdapterOverrides[ext] = cfgExtAdapter;
                        else                  debugAdapterOverrides.erase(ext);
                    }
                    saveSettings();
                    pushToast("Debug configuration saved", IM_COL32(120, 200, 120, 255));
                }
                ImGui::SameLine();
                if (ImGui::Button("Detect Adapters"))
                    nativeAdapterDetected = false;   // re-probe on next resolution

                // Detection readout — what the native (C/C++) fallback found.
                {
                    std::string type;
                    std::vector<std::pair<std::string, std::string>> extras;
                    auto nat = debugAdapterFor(".cpp", {}, type, extras);
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
            ImGui::BeginDisabled(!dbgStopped);
            if (ImGui::Button("Continue"))
                dapClient.continueExec(dbgThreadId);
            ImGui::SameLine();
            if (ImGui::Button("Over"))
                dapClient.next(dbgThreadId);
            ImGui::SameLine();
            if (ImGui::Button("Into"))
                dapClient.stepIn(dbgThreadId);
            ImGui::SameLine();
            if (ImGui::Button("Out"))
                dapClient.stepOut(dbgThreadId);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(dbgStopped);
            if (ImGui::Button("Pause"))
                dapClient.pause(dbgThreadId != 0 ? dbgThreadId : 1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                stopDebugSession();

            if (dbgStopped)
                ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1.0f), "Paused (%s)  %s:%d",
                                   dbgStopReason.c_str(),
                                   dbgStopFile.empty() ? "?" : std::filesystem::path(dbgStopFile).filename().string().c_str(),
                                   dbgStopLine + 1);
            else
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Running — %s",
                                   std::filesystem::path(dbgProgram).filename().string().c_str());
        }

        // ── Call stack ───────────────────────────────────────────────────
        ImGui::SeparatorText("Call stack");
        ImGui::BeginChild("##dbgStack", ImVec2(0.0f, 110.0f), ImGuiChildFlags_None);
        for (size_t i = 0; i < dbgFrames.size(); ++i)
        {
            const auto &f = dbgFrames[i];
            ImGui::PushID((int) i);
            std::string leaf = f.sourcePath.empty()
                                   ? std::string("<no source>")
                                   : std::filesystem::path(f.sourcePath).filename().string();
            char row[512];
            std::snprintf(row, sizeof(row), "%s  —  %s:%d", f.name.c_str(), leaf.c_str(), f.line);
            if (ImGui::Selectable(row, f.id == dbgCurrentFrame))
            {
                dbgCurrentFrame = f.id;
                dapClient.scopes(f.id);
                if (!f.sourcePath.empty())
                    dbgJumpTo(f.sourcePath, f.line - 1);
            }
            if (!f.sourcePath.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s:%d", f.sourcePath.c_str(), f.line);
            ImGui::PopID();
        }
        if (dbgFrames.empty())
            ImGui::TextDisabled(dbgSessionActive ? "(running)" : "(no session)");
        ImGui::EndChild();

        // ── Variables ────────────────────────────────────────────────────
        ImGui::SeparatorText("Variables");
        ImGui::BeginChild("##dbgVars", ImVec2(0.0f, 160.0f), ImGuiChildFlags_None);
        // Recursive variable rows: expandable when the adapter says the value
        // has children (variablesReference > 0); children fetched on first open.
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
                        auto it = dbgVarChildren.find(v.variablesReference);
                        if (it == dbgVarChildren.end())
                        {
                            if (dbgVarRequested.insert(v.variablesReference).second)
                                dapClient.variables(v.variablesReference);
                            ImGui::TextDisabled("(loading…)");
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
        for (auto &s : dbgScopes)
        {
            if (ImGui::TreeNodeEx(s.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto it = dbgVarChildren.find(s.variablesReference);
                if (it == dbgVarChildren.end())
                {
                    if (s.variablesReference > 0 &&
                        dbgVarRequested.insert(s.variablesReference).second)
                        dapClient.variables(s.variablesReference);
                    ImGui::TextDisabled("(loading…)");
                }
                else
                    for (const auto &v : it->second)
                        renderVar(v, 0);
                ImGui::TreePop();
            }
        }
        if (dbgScopes.empty())
            ImGui::TextDisabled(dbgStopped ? "(loading…)" : "(pause to inspect)");
        ImGui::EndChild();

        // ── Console (output events + REPL) ──────────────────────────────
        ImGui::SeparatorText("Console");
        float inputH = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##dbgConsole", ImVec2(0.0f, -inputH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(dbgConsole.c_str());
        if (dbgConsoleScrollDown)
        {
            ImGui::SetScrollHereY(1.0f);
            dbgConsoleScrollDown = false;
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##dbgEval", "evaluate expression (Enter)…",
                                     dbgEvalBuf, sizeof(dbgEvalBuf), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (dbgEvalBuf[0] && dapClient.ready())
            {
                dbgConsole += std::string("> ") + dbgEvalBuf + "\n";
                dbgConsoleScrollDown = true;
                dapClient.evaluate(dbgEvalBuf, dbgStopped ? dbgCurrentFrame : 0);
                dbgEvalBuf[0] = 0;
                ImGui::SetKeyboardFocusHere(-1);   // keep typing
            }
        }
    }
    ImGui::End();
}

// ── External native debuggers (raddbg / Visual Studio) ─────────────────
//
// These tools bring their own UI, so instead of speaking DAP to them we
// launch them at our target and seed raddbg with our breakpoints over its
// --ipc channel. Command builders live in debug_bridge.{h,cpp} (pure,
// selftest-covered); verbs are settings-templated so a CLI change in the
// tool is a settings tweak, not a rebuild.

// Settings override wins ([debug_bridge] key=path), else auto-detect.
std::string Editor::bridgeToolPath(const char *key, std::string (*detect)()) const
{
    auto it = debugBridgeSettings.find(key);
    if (it != debugBridgeSettings.end() && !it->second.empty())
        return it->second;
    return detect();
}

// What an external debugger should target: the Unreal editor for .uproject
// roots, else the freshest built exe, else the active document if it IS an
// exe. `why` explains a false return for the toast.
bool Editor::externalDebugTarget(std::string &exe, std::string &args, std::string &why)
{
    exe.clear();
    args.clear();
    // (UE-editor targeting lives in the Unreal plugin now; core debugs the built
    // exe or the active script.)
    auto built = findBuiltExe();
    if (!built.empty())
    {
        exe = built.string();
        return true;
    }
    if (!tabs.empty() && doc().filename != "untitled")
    {
        auto ext = std::filesystem::path(doc().filename).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (ext == ".exe")
        {
            exe = doc().filename;
            return true;
        }
    }
    why = "no built executable found — build first (F6)";
    return false;
}

// Spawn without pipes and drop the handle — the tool owns its own lifetime.
void Editor::launchDetached(const std::vector<std::string> &argv)
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
        pushToast("Failed to launch " + argv[0], IM_COL32(240, 110, 90, 255));
        return;
    }
    SDL_DestroyProcess(p);   // releases the handle; the child keeps running
}

void Editor::debugInRadDbg()
{
    std::string raddbg = bridgeToolPath("raddbg", dbgbridge::findRadDbg);
    if (raddbg.empty())
    {
        pushToast("raddbg not found — set [debug_bridge] raddbg=<path> in settings",
                  IM_COL32(240, 110, 90, 255));
        return;
    }
    std::string exe, args, why;
    if (!externalDebugTarget(exe, args, why))
    {
        pushToast("raddbg: " + why, IM_COL32(240, 200, 90, 255));
        return;
    }
    launchDetached(dbgbridge::raddbgLaunch(raddbg, exe, args));
    // Seed our breakpoints into the running instance. The verb template is
    // settings-overridable (raddbg_bp_template, {file}/{line} placeholders).
    std::vector<std::pair<std::string, int>> bps;
    for (auto &kv : dbgBreakpoints)
        for (int line : kv.second)
            bps.emplace_back(kv.first, line + 1);
    std::string tmpl;
    if (auto it = debugBridgeSettings.find("raddbg_bp_template"); it != debugBridgeSettings.end())
        tmpl = it->second;
    for (auto &cmd : dbgbridge::raddbgBreakpointCmds(raddbg, tmpl, bps))
        launchDetached(cmd);
    pushToast("Launched raddbg" + (bps.empty() ? std::string() : " (+" + std::to_string(bps.size()) + " breakpoints)"),
              IM_COL32(120, 200, 120, 255));
}

void Editor::debugInVisualStudio()
{
    std::string devenv = bridgeToolPath("devenv", dbgbridge::findDevenv);
    if (devenv.empty())
    {
        pushToast("Visual Studio (devenv) not found — set [debug_bridge] devenv=<path> in settings",
                  IM_COL32(240, 110, 90, 255));
        return;
    }
    std::string exe, args, why;
    if (!externalDebugTarget(exe, args, why))
    {
        pushToast("VS debugger: " + why, IM_COL32(240, 200, 90, 255));
        return;
    }
    launchDetached(dbgbridge::devenvDebugExe(devenv, exe, args));
    pushToast("Launched Visual Studio debugger", IM_COL32(120, 200, 120, 255));
}
