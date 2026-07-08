//
//  uevr_bridge.cpp — "UEVR Live" panel: drive a running UEVR game's Lua state
//  from the standalone IDE over a file-inbox.
//
//  Protocol (both sides agree on this):
//    IDE  writes  <ide_bridge>/cmd/<reqId>.txt   — line 1 = kind
//                                                   (run|globals|modules|inspect),
//                                                   remaining lines = payload
//    plugin runs it via exec_lua_chunk and writes <ide_bridge>/out/<reqId>.txt —
//                                                   line 1 = kind, rest = result
//    IDE  polls   <ide_bridge>/out/, routes each result to the matching tab,
//                                                   deletes the file.
//
//  ide_bridge lives under %APPDATA%\UnrealVRMod\UEVR\ide_bridge (next to the
//  UEVR scripts folder). Everything is best-effort: with no game / no plugin
//  running, sends just pile up in cmd/ harmlessly and no results ever arrive.
//

#define _CRT_SECURE_NO_WARNINGS // std::getenv("APPDATA") for the bridge folder

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <imgui.h>

#include "editor.h"

//
//  Editor::uevrBridgeDir
//

std::filesystem::path Editor::uevrBridgeDir(const char *sub) const
{
    const char *appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? std::filesystem::path(appdata) : std::filesystem::path(".");
    return base / "UnrealVRMod" / "UEVR" / "ide_bridge" / sub;
}

//
//  Editor::sendUevr
//

void Editor::sendUevr(const std::string &kind, const std::string &payload)
{
    std::error_code ec;
    auto dir = uevrBridgeDir("cmd");
    std::filesystem::create_directories(dir, ec);
    // reqId: counter + coarse time keeps files unique + roughly ordered.
    std::string reqId = std::to_string(++uevrReqCounter) + "_" +
                        std::to_string((long long)std::time(nullptr));
    std::ofstream f(dir / (reqId + ".txt"), std::ios::binary);
    if (f)
        f << kind << "\n" << payload;
}

//
//  Editor::pollUevrBridge — drain the out inbox (~5 Hz), route results.
//

void Editor::pollUevrBridge()
{
    static double nextPoll = 0.0;
    double now = ImGui::GetTime();
    if (now < nextPoll)
        return;
    nextPoll = now + 0.2;

    std::error_code ec;
    auto dir = uevrBridgeDir("out");
    if (!std::filesystem::is_directory(dir, ec))
        return;

    std::vector<std::filesystem::path> files;
    std::error_code iec;
    for (auto it = std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, iec);
         !iec && it != std::filesystem::directory_iterator() && files.size() < 32; it.increment(iec))
    {
        std::error_code fec;
        if (it->is_regular_file(fec) && !fec)
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    for (auto &p : files)
    {
        std::ifstream in(p, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::filesystem::remove(p, ec);

        auto nl = content.find('\n');
        std::string kind = (nl == std::string::npos) ? content : content.substr(0, nl);
        std::string body = (nl == std::string::npos) ? std::string() : content.substr(nl + 1);
        while (!kind.empty() && (kind.back() == '\r' || kind.back() == ' '))
            kind.pop_back();

        if (kind == "globals")
            uevrGlobals = body;
        else if (kind == "modules")
            uevrModules = body;
        else if (kind == "inspect")
            uevrInspect = body;
        else // "run" (and anything else) → the output log
        {
            std::istringstream ss(body);
            std::string line;
            while (std::getline(ss, line))
                uevrOutputLog.push_back(line);
            if (uevrOutputLog.size() > 500)
                uevrOutputLog.erase(uevrOutputLog.begin(), uevrOutputLog.begin() + (uevrOutputLog.size() - 500));
        }
    }
}

//
//  Editor::renderUevrLive
//

void Editor::renderUevrLive()
{
    if (!uevrLiveVisible)
        return;
    pollUevrBridge();

    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UEVR Live###uevrLive", &uevrLiveVisible))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Runs Lua inside a running UEVR game via the ImGui-IDE bridge plugin.");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", uevrBridgeDir("cmd").string().c_str());

    // ── REPL input ────────────────────────────────────────────────────────
    ImGui::Separator();
    bool runNow = ImGui::InputTextMultiline("##uevrRepl", uevrReplBuf, sizeof(uevrReplBuf),
                                            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f),
                                            ImGuiInputTextFlags_CtrlEnterForNewLine);
    (void)runNow;
    if (ImGui::Button("Run (Ctrl+Enter)") && uevrReplBuf[0])
    {
        uevrOutputLog.push_back("> " + std::string(uevrReplBuf));
        sendUevr("run", uevrReplBuf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Run Active Doc") && !tabs.empty())
    {
        std::string src = doc().editor.GetText();
        uevrOutputLog.push_back("> [run active document]");
        sendUevr("run", src);
    }
    ImGui::SameLine();
    if (ImGui::Button("Run Selection") && !tabs.empty() && doc().editor.AnyCursorHasSelection())
    {
        std::string sel = doc().editor.GetCurrentSelectionText();
        uevrOutputLog.push_back("> [run selection]");
        sendUevr("run", sel);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        uevrOutputLog.clear();

    // ── result tabs ───────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::BeginTabBar("##uevrTabs"))
    {
        if (ImGui::BeginTabItem("Output"))
        {
            ImGui::BeginChild("##uevrOut", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (auto &line : uevrOutputLog)
                ImGui::TextUnformatted(line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                ImGui::SetScrollHereY(1.0f); // autoscroll when pinned to bottom
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Globals"))
        {
            if (ImGui::Button("Refresh"))
                sendUevr("globals", "");
            ImGui::BeginChild("##uevrGlobals", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(uevrGlobals.empty() ? "(refresh to query the game)" : uevrGlobals.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Modules"))
        {
            if (ImGui::Button("Refresh"))
                sendUevr("modules", "");
            ImGui::BeginChild("##uevrModules", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(uevrModules.empty() ? "(refresh to query the game)" : uevrModules.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Inspect"))
        {
            ImGui::SetNextItemWidth(-90.0f);
            ImGui::InputText("##uevrInspectExpr", uevrInspectBuf, sizeof(uevrInspectBuf));
            ImGui::SameLine();
            if (ImGui::Button("Inspect") && uevrInspectBuf[0])
                sendUevr("inspect", uevrInspectBuf);
            ImGui::BeginChild("##uevrInspect", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(uevrInspect.empty() ? "(evaluate an expression against the running game)" : uevrInspect.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
