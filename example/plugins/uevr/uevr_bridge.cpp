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

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <imgui.h>

#include "BlueprintEditor.h"
#include "BlueprintRegistryJson.h"
#include "uevr_plugin.h"

namespace
{
// The inspect/watch wire format is "name\ttype\tvalue" rows (see tools/uevr-bridge's
// handle_cmd): detect it as "every non-empty line has exactly 2 tabs" so the panel can
// render a real table instead of a text blob, with no protocol version tag needed --
// both halves of the bridge ship from the same repo.
struct BridgeRow
{
    std::string name, type, value;
};

bool looksTabDelimited(const std::string &text)
{
    if (text.empty())
        return false;
    std::istringstream ss(text);
    std::string line;
    bool any = false;
    while (std::getline(ss, line))
    {
        if (line.empty())
            continue;
        any = true;
        if (std::count(line.begin(), line.end(), '\t') != 2)
            return false;
    }
    return any;
}

// Live SDK dump: sent through the existing "inspect" round trip (an inspect payload
// is spliced into `return <payload>`, so an IIFE expression carries a whole program).
// Walks a seed set of live objects (pawn / controller / engine — "spawned objects
// only"), emits ONE class entry per distinct leaf class with its get_property_info()
// fields, in the same JSON shape as a UEVR Class Browser dump. The SDKDUMP: marker
// routes the response into the SDK index instead of the Inspect tab.
constexpr const char *kSdkDumpChunk = R"LUA((function()
    local out = {}
    local seen = {}
    local function esc(s)
        s = tostring(s or "")
        return s:gsub("\\", "\\\\"):gsub("\"", "\\\"")
    end
    local function dump(o)
        if o == nil then return end
        pcall(function()
            local c = o:get_class()
            if c == nil then return end
            local full = c:get_full_name()
            if seen[full] then return end
            seen[full] = true
            local sup = c:get_super_struct()
            local props = {}
            local pok, info = pcall(function() return o:get_property_info() end)
            if pok and info then
                for _, p in ipairs(info) do
                    props[#props + 1] = '{"name":"' .. esc(p.name) .. '","type":"' .. esc(p.type) .. '"}'
                end
            end
            out[#out + 1] = '{"name":"' .. esc(c:get_fname():to_string())
                .. '","super":"' .. esc(sup and sup:get_fname():to_string() or "")
                .. '","full_name":"' .. esc(full)
                .. '","functions":[],"properties":[' .. table.concat(props, ",") .. ']}'
        end)
    end
    pcall(function() dump(uevr.api:get_local_pawn(0)) end)
    pcall(function() dump(uevr.api:get_player_controller(0)) end)
    pcall(function() dump(uevr.api:get_engine()) end)
    return "SDKDUMP:{\"classes\":[" .. table.concat(out, ",") .. "]}"
end)())LUA";

std::vector<BridgeRow> parseTabDelimited(const std::string &text)
{
    std::vector<BridgeRow> rows;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.empty())
            continue;
        auto t1 = line.find('\t');
        auto t2 = (t1 == std::string::npos) ? std::string::npos : line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos)
            continue;
        BridgeRow row;
        row.name = line.substr(0, t1);
        row.type = line.substr(t1 + 1, t2 - t1 - 1);
        row.value = line.substr(t2 + 1);
        rows.push_back(std::move(row));
    }
    return rows;
}
} // namespace

std::filesystem::path UevrPlugin::uevrBridgeDir(const char *sub) const
{
    const char *appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata ? std::filesystem::path(appdata) : std::filesystem::path(".");
    return base / "UnrealVRMod" / "UEVR" / "ide_bridge" / sub;
}

void UevrPlugin::sendUevr(const std::string &kind, const std::string &payload)
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

void UevrPlugin::pollUevrBridge()
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
        {
            // A live SDK dump rides the inspect round trip with a marker so it feeds
            // the SDK index (autocomplete + Expose SDK Class) instead of the tab.
            size_t marker = body.find("SDKDUMP:");
            if (marker != std::string::npos)
            {
                std::string json = body.substr(marker + 8);
                std::string error;
                size_t before = sdkIndex.GetClasses().size();
                if (BlueprintRegistryJson::Load(sdkIndex, json, error))
                {
                    rebuildSdkWords();
                    uevrOutputLog.push_back("[sdk] indexed " +
                                            std::to_string(sdkIndex.GetClasses().size() - before) +
                                            " live classes (autocomplete + Expose SDK Class)");
                }
                else
                    uevrOutputLog.push_back("[sdk] live dump parse failed: " + error);
            }
            else
                uevrInspect = body;
        }
        else if (kind == "watch")
        {
            // response is one "type\tpreview" line per expression, in the same order
            // uevrWatches was sent in -- order is the correlation key
            std::istringstream ss(body);
            std::string line;
            size_t i = 0;
            while (std::getline(ss, line) && i < uevrWatches.size())
            {
                auto tab = line.find('\t');
                if (tab != std::string::npos)
                {
                    uevrWatches[i].type = line.substr(0, tab);
                    uevrWatches[i].preview = line.substr(tab + 1);
                }
                ++i;
            }
        }
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

void UevrPlugin::renderUevrLive(PluginHost &host)
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

    // Focused: the REPL / inspect fields want the keys, not the document's shortcuts.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        host.hostSuppressAppShortcuts();

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
    if (ImGui::Button("Run Active Doc"))
    {
        std::string src = host.hostActiveText();
        if (!src.empty())
        {
            uevrOutputLog.push_back("> [run active document]");
            sendUevr("run", src);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Run Selection"))
    {
        std::string sel = host.hostActiveSelection();
        if (!sel.empty())
        {
            uevrOutputLog.push_back("> [run selection]");
            sendUevr("run", sel);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        uevrOutputLog.clear();
    ImGui::SameLine();
    if (ImGui::Button("Import SDK from Game"))
    {
        // Dump the live seed objects' classes (pawn/controller/engine) into the SDK
        // index over the bridge — no file dump needed. Result lands via the poll.
        uevrOutputLog.push_back("> [import sdk from game]");
        sendUevr("inspect", kSdkDumpChunk);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Index the classes of live objects (pawn, controller, engine) for autocomplete + Expose SDK Class");
    ImGui::SameLine();
    if (ImGui::Button("Reset Scripts"))
    {
        // Tell the running game to reload all its UEVR Lua scripts (game-side calls
        // param->functions->reset_lua_scripts). Clears stale globals/hooks/callbacks
        // left over from earlier Run commands or a since-edited script.
        uevrOutputLog.push_back("> [reset scripts]");
        sendUevr("reset", "");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reload all UEVR Lua scripts in the running game (like UEVR's own Reset scripts)");

    // ── result tabs ───────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::BeginTabBar("##uevrTabs"))
    {
        if (ImGui::BeginTabItem("Output"))
        {
            ImGui::BeginChild("##uevrOut", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            for (auto &line : uevrOutputLog)
                ImGui::TextUnformatted(line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                ImGui::SetScrollHereY(1.0f); // autoscroll when pinned to bottom
            host.hostMiddleMousePanScroll(100);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Globals"))
        {
            if (ImGui::Button("Refresh"))
                sendUevr("globals", "");
            ImGui::BeginChild("##uevrGlobals", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            if (uevrGlobals.empty())
            {
                ImGui::TextUnformatted("(refresh to query the game)");
            }
            else if (looksTabDelimited(uevrGlobals))
            {
                // newer bridges emit name\ttype\tvalue rows -> render a real table
                auto rows = parseTabDelimited(uevrGlobals);
                if (ImGui::BeginTable("##uevrGlobalsTable", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Global");
                    ImGui::TableSetupColumn("Type");
                    ImGui::TableSetupColumn("Value");
                    ImGui::TableSetupColumn("##ins", ImGuiTableColumnFlags_WidthFixed, 36.0f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < rows.size(); ++i)
                    {
                        auto &row = rows[i];
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.type.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.value.c_str());
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("ins") && !row.name.empty())
                            insertLiveValueAsNode(row.name, row.name);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Insert a Custom Lua node reading this global");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            else
            {
                // older bridge builds return the legacy text blob
                ImGui::TextUnformatted(uevrGlobals.c_str());
            }
            host.hostMiddleMousePanScroll(101);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Modules"))
        {
            if (ImGui::Button("Refresh"))
                sendUevr("modules", "");
            ImGui::BeginChild("##uevrModules", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(uevrModules.empty() ? "(refresh to query the game)" : uevrModules.c_str());
            host.hostMiddleMousePanScroll(102);
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
            if (uevrInspect.empty())
            {
                ImGui::TextUnformatted("(evaluate an expression against the running game)");
            }
            else if (looksTabDelimited(uevrInspect))
            {
                auto rows = parseTabDelimited(uevrInspect);
                if (ImGui::BeginTable("##uevrInspectTable", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Type");
                    ImGui::TableSetupColumn("Value");
                    ImGui::TableSetupColumn("##ins", ImGuiTableColumnFlags_WidthFixed, 36.0f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < rows.size(); ++i)
                    {
                        auto &row = rows[i];
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.type.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.value.c_str());
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("ins") && !row.name.empty())
                            insertLiveValueAsNode(std::string(uevrInspectBuf) + "[\"" + row.name + "\"]", row.name);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Insert a Custom Lua node reading this property");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            else
            {
                ImGui::TextUnformatted(uevrInspect.c_str());
            }
            host.hostMiddleMousePanScroll(103);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Watch"))
        {
            ImGui::SetNextItemWidth(-70.0f);
            bool addNow = ImGui::InputText("##uevrWatchExpr", uevrWatchExprBuf, sizeof(uevrWatchExprBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Add") || addNow) && uevrWatchExprBuf[0])
            {
                uevrWatches.push_back(WatchEntry{uevrWatchExprBuf, "", ""});
                sendWatchBatch();
            }

            static double nextWatchSend = 0.0;
            double now = ImGui::GetTime();
            if (!uevrWatches.empty() && now >= nextWatchSend)
            {
                nextWatchSend = now + 1.0; // its own ~1 Hz cadence, separate from pollUevrBridge's 0.2s poll timer
                sendWatchBatch();
            }

            ImGui::BeginChild("##uevrWatch", ImVec2(0, 0), ImGuiChildFlags_Borders);
            if (uevrWatches.empty())
            {
                ImGui::TextUnformatted("(add an expression to watch it live, refreshed ~1x/sec)");
            }
            else if (ImGui::BeginTable("##uevrWatchTable", 4,
                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Expression");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableHeadersRow();
                int removeIndex = -1;
                for (size_t i = 0; i < uevrWatches.size(); ++i)
                {
                    auto &w = uevrWatches[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(w.expr.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(w.type.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(w.preview.c_str());
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("ins"))
                        insertLiveValueAsNode(w.expr, w.expr);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x"))
                        removeIndex = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (removeIndex >= 0)
                    uevrWatches.erase(uevrWatches.begin() + removeIndex);
                ImGui::EndTable();
            }
            host.hostMiddleMousePanScroll(104);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void UevrPlugin::sendWatchBatch()
{
    std::string payload;
    for (auto &w : uevrWatches)
        payload += w.expr + "\n";
    sendUevr("watch", payload);
}

void UevrPlugin::insertLiveValueAsNode(const std::string &expr, const std::string &label)
{
    auto &bp = ensureBlueprintEditor();
    auto nodeId = bp.AddCustomLuaNode(ImVec2(0.0f, 0.0f));
    if (nodeId == 0)
        return;
    bp.AddCustomLuaPin(nodeId, true, "Out0");
    bp.SetCustomLuaSource(nodeId, "-- " + label + "\nOut0 = " + expr);
    blueprintVisible = true;
}
