//
//  command_palette.cpp — VS-Code-style Ctrl+Shift+P command palette.
//  Ported from the performance-optimization-roadmap branch onto the plugin
//  architecture (UE commands dropped — Unreal is a plugin now).
//

#include "editor.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "imgui.h"
#include "imgui_internal.h"

// Subsequence fuzzy match: -1 if `needle` isn't a subsequence of `label`, else a
// score rewarding consecutive + word-start hits (so "gof" ranks "Git: ..." well).
static int paletteFuzzyScore(const std::string &label, const char *needle)
{
    if (!needle || !*needle)
        return 0;   // empty query matches everything equally
    int score = 0;
    size_t li = 0;
    bool prevMatched = false;
    for (const char *n = needle; *n; ++n)
    {
        char nc = (char) std::tolower((unsigned char) *n);
        if (nc == ' ')
        {
            prevMatched = false;
            continue;   // spaces in the query just separate words
        }
        bool found = false;
        for (; li < label.size(); ++li)
        {
            char lc = (char) std::tolower((unsigned char) label[li]);
            if (lc == nc)
            {
                bool wordStart = li == 0 || label[li - 1] == ' ' || label[li - 1] == ':' ||
                                 label[li - 1] == '/' || label[li - 1] == '.';
                score += prevMatched ? 3 : (wordStart ? 2 : 1);
                if (li == 0)
                    score += 2;   // matching the label head reads best
                prevMatched = true;
                ++li;
                found = true;
                break;
            }
            prevMatched = false;
        }
        if (!found)
            return -1;
    }
    return score;
}

void Editor::openCommandPalette()
{
    palVisible = true;
    palFocus = true;
    palQuery[0] = 0;
    palSelected = 0;
}

void Editor::renderCommandPalette()
{
    if (!palVisible)
        return;

    struct PalCmd {
        std::string           label;
        const char           *chord;   // display only ("" = none)
        std::function<void()> run;
    };
    std::vector<PalCmd> cmds;
    auto add = [&](std::string label, const char *chord, std::function<void()> fn) {
        cmds.push_back({std::move(label), chord, std::move(fn)});
    };

    bool hasDoc = !tabs.empty();
    // File
    add("File: New", "Ctrl+N", [this] { newFile(); });
    add("File: Open...", "Ctrl+O", [this] { openFile(); });
    add("File: Open Project...", "", [this] { openProjectFolderPicker(); });
    if (hasDoc)
    {
        add("File: Save", "Ctrl+S", [this] {
            if (doc().filename == "untitled") showSaveFileAs();
            else saveFile();
        });
        add("File: Save As...", "Ctrl+Shift+S", [this] { showSaveFileAs(); });
        add("File: Close Tab", "Ctrl+W", [this] {
            if (isDirty()) showConfirmClose([this]() { closeTab(activeTab); });
            else closeTab(activeTab);
        });
        add("File: Open Containing Folder", "", [this] { openContainingFolder(); });
    }
    add("File: Reopen Closed Tab", "Ctrl+Shift+T", [this] { reopenLastClosedTab(); });
    add("File: Exit", "", [this] { tryToQuit(); });

    // View / panels
    add("View: Navigation Panel", "", [this] { navPanelVisible = !navPanelVisible; });
    add("View: Symbols Panel", "", [this] { symbolsPanelVisible = !symbolsPanelVisible; });
    add("View: References Panel", "", [this] { referencesVisible = !referencesVisible; });
    add("View: Markdown Preview", "", [this] { mdPreviewVisible = !mdPreviewVisible; });
    add("View: External Changes", "", [this] { externalChangesVisible = !externalChangesVisible; });
    add("View: Debug Panel", "", [this] { dbgPanelVisible = !dbgPanelVisible; });
    add("View: Developer Tools", "", [this] { devToolsVisible = !devToolsVisible; });
    add("View: Style Editor", "", [this] { showStyleEditor = !showStyleEditor; });
    add("View: Settings", "", [this] { settingsVisible = true; settingsFocusRequest = true; });
    if (hasDoc)
        add("View: Split Tab Right", "Ctrl+\\", [this] { splitActiveTabRight(); });
    add("View: Zoom In", "Ctrl+=", [this] { increaseFontSIze(); });
    add("View: Zoom Out", "Ctrl+-", [this] { decreaseFontSIze(); });
    if (hasDoc)
    {
        add("View: Pop Out Document (Left)", "Ctrl+Alt+Left", [this] { popOutActiveDoc(-1); });
        add("View: Pop Out Document (Right)", "Ctrl+Alt+Right", [this] { popOutActiveDoc(1); });
        add("View: Merge Window Back", "Ctrl+Alt+M", [this] { remergeActiveWindow(); });
    }
    add("View: Merge All Windows", "Ctrl+Alt+Shift+M", [this] { remergeAllWindows(); });
    for (int i = 0; i < themeCount(); ++i)
        add(std::string("Theme: ") + themeName(i), prefTheme == i ? "(current)" : "", [this, i] {
            prefTheme = i;
            applyTheme(i);
            saveSettings();
        });

    // Find / navigate
    add("Find: In Files...", "Ctrl+Shift+F", [this] { openFindInFiles(); });
    add("Find: Go to Line...", "Ctrl+G", [this] { showGotoLine(); });
    add("Go: Back", "Alt+Left", [this] { navigateBack(); });
    add("Go: Forward", "Alt+Right", [this] { navigateForward(); });

    // Code / project
    if (hasDoc)
    {
        add("Code: Toggle Header/Source", "Alt+O", [this] { toggleHeaderSource(); });
        add("Code: Format Document", "Alt+Shift+F", [this] { formatActiveDocument(); });
        add("Diff: Against Saved", "", [this] { showDiff(); });
        add("Diff: Against Another File...", "", [this] { openDiffOtherDialog(); });
    }
    add("Code: Rebuild Symbol Index", "", [this] { rebuildProjectIndex(); });
    add("Project: Run", "F5", [this] { runProjectExeOrScript(); });
    add("Project: Run with Arguments...", "", [this] { runProjectWithArgs(); });
    add("Project: Build", "F6", [this] { runProjectBuild(); });
    if (hasDoc)
        add("Project: Run Active Document", "", [this] { runScriptForDoc(); });

    // Debug
    if (!dbgSessionActive)
        add("Debug: Start Debugging", "", [this] { startDebugSession(); });
    else
    {
        add("Debug: Stop", "Shift+F5", [this] { stopDebugSession(); });
        if (dbgStopped)
        {
            add("Debug: Continue", "F5", [this] { dapClient.continueExec(dbgThreadId); });
            add("Debug: Step Over", "F10", [this] { dapClient.next(dbgThreadId); });
            add("Debug: Step Into", "F11", [this] { dapClient.stepIn(dbgThreadId); });
            add("Debug: Step Out", "Shift+F11", [this] { dapClient.stepOut(dbgThreadId); });
        }
    }
    if (hasDoc)
        add("Debug: Toggle Breakpoint", "F9", [this] { toggleBreakpointAtCursor(); });
    add("Debug: Configure Adapters / Target...", "", [this] { dbgPanelVisible = true; });
    add("Debug: Launch in raddbg", "", [this] { debugInRadDbg(); });
    add("Debug: Launch in Visual Studio", "", [this] { debugInVisualStudio(); });

    // Git
    add("Git: Commit...", "", [this] { gitCommitRequest = true; });
    add("Git: Discard Changes...", "", [this] { gitDiscardRequest = true; });
    add("Git: History...", "", [this] { gitHistoryVisible = true; refreshGitHistory(); });
    add("Git: Open on Web", "", [this] { openGitOnWeb(); });

    // Editor extras (features this build adds)
    add("View: Notes", "", [this] { notesVisible = !notesVisible; });
    add("View: Save Screenshot", "Ctrl+Alt+S", [this] { requestScreenshot(); });
    add("Help: Take the Tour", "", [this] { startTour(); });

    // Plugin contributions — each enabled plugin adds its own entries, gated on
    // the active document's file type and/or the current project type.
    {
        PluginDocInfo info;
        if (hasDoc)
        {
            info.filename = doc().filename;
            info.extLower = std::filesystem::path(doc().filename).extension().string();
            std::transform(info.extLower.begin(), info.extLower.end(), info.extLower.begin(),
                           [](unsigned char c) { return (char) std::tolower(c); });
            if (auto *lang = doc().editor.GetLanguage())
                info.languageName = lang->name;
        }
        pluginRegistry.paletteCommands(*this, info,
                                       [&](const std::string &label, std::function<void()> fn) {
                                           add(label, "", std::move(fn));
                                       });
    }

    // Recents
    {
        int shown = 0;
        for (auto &p : recentFiles)
        {
            if (++shown > 8) break;
            std::string leaf = std::filesystem::path(p).filename().string();
            add("Recent File: " + leaf + "  (" + p + ")", "", [this, p] { openFile(p); });
        }
        shown = 0;
        for (auto &p : recentProjects)
        {
            if (++shown > 5) break;
            std::string leaf = std::filesystem::path(p).filename().string();
            add("Recent Project: " + leaf, "", [this, p] { setProjectRoot(std::filesystem::path(p)); });
        }
    }

    // Filter + rank.
    struct Row { int score; int idx; };
    std::vector<Row> rows;
    rows.reserve(cmds.size());
    for (int i = 0; i < (int) cmds.size(); ++i)
    {
        int s = paletteFuzzyScore(cmds[(size_t) i].label, palQuery);
        if (s >= 0)
            rows.push_back({s, i});
    }
    if (palQuery[0])
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.score > b.score; });
    if (palSelected >= (int) rows.size())
        palSelected = rows.empty() ? 0 : (int) rows.size() - 1;
    if (palSelected < 0)
        palSelected = 0;

    // Float top-center over the main viewport (multi-viewport aware).
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const float width = 560.0f;
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
    std::function<void()> pending;   // run AFTER End() — actions may open dialogs/popups
    if (ImGui::Begin("##cmdPalette", nullptr, flags))
    {
        if (palFocus)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        bool entered = ImGui::InputTextWithHint("##palQuery", "Type a command…", palQuery,
                                                sizeof(palQuery), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited())
            palSelected = 0;

        // Keyboard: arrows move the selection even while the input keeps focus.
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            palSelected = rows.empty() ? 0 : (palSelected + 1) % (int) rows.size();
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            palSelected = rows.empty() ? 0 : (palSelected + (int) rows.size() - 1) % (int) rows.size();
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
            palSelected = (std::min)((int) rows.size() - 1, palSelected + 10);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            palSelected = (std::max)(0, palSelected - 10);

        ImGui::Separator();
        ImGui::BeginChild("##palList", ImVec2(0.0f, 360.0f));
        ImGuiListClipper clipper;
        clipper.Begin((int) rows.size());
        // Keep the keyboard selection visible while stepping through the list.
        static int lastSel = -1;
        bool selMoved = palSelected != lastSel;
        lastSel = palSelected;
        while (clipper.Step())
        {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
            {
                const PalCmd &c = cmds[(size_t) rows[(size_t) r].idx];
                ImGui::PushID(r);
                if (ImGui::Selectable(c.label.c_str(), r == palSelected))
                {
                    pending = c.run;
                    palVisible = false;
                }
                if (r == palSelected && selMoved)
                    ImGui::SetScrollHereY(0.5f);
                if (c.chord && c.chord[0])
                {
                    ImGui::SameLine();
                    float w = ImGui::CalcTextSize(c.chord).x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
                    ImGui::TextDisabled("%s", c.chord);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled("%zu command%s   ↑↓ select · Enter run · Esc close",
                            rows.size(), rows.size() == 1 ? "" : "s");

        if (entered && !rows.empty())
        {
            pending = cmds[(size_t) rows[(size_t) palSelected].idx].run;
            palVisible = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            palVisible = false;
        // Clicking anywhere else dismisses the palette (after the open frame,
        // where focus hasn't landed yet).
        if (!palFocus && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            palVisible = false;
        palFocus = false;
    }
    ImGui::End();
    if (pending)
        pending();
}
