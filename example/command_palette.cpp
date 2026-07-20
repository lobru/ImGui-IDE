//
//  command_palette.cpp — VS-Code-style Ctrl+Shift+P command palette.
//
//  Modular action registry, not a hard-coded per-frame list:
//    - buildPaletteActions() runs ONCE when the palette opens: core actions
//      (gated on document/project state), plugin contributions (tagged with the
//      contributing plugin via the registry), recents.
//    - refilterPalette() runs only when the QUERY changes — per-frame work is
//      just rendering the cached rows, which is what makes typing snappy.
//    - Ranking blends the fuzzy score with persisted usage counts + recency
//      ([palette_usage] in settings), so an empty query lists your most-used
//      actions first and frequent picks float up.
//    - Every row carries a source tag ("Filetype: Lua", "Plugin: ...") rendered
//      dim on the right + as a hover tooltip, so it's clear WHY a row is shown.
//    - Run a row by click, Enter, Alt+1..9 — or plain 1..9 while the query is
//      still empty. Arrows/PageUp/PageDown move the selection; the list is a
//      normal scrollable child.
//

#include "editor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
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

void Editor::notePaletteUse(const std::string &id)
{
    auto &u = paletteUsage[id];
    ++u.uses;
    u.last = (long long) std::time(nullptr);
    saveSettings();
}

void Editor::buildPaletteActions()
{
    palActions.clear();
    auto add = [&](std::string label, const char *chord, std::string source, std::function<void()> fn) {
        PaletteAction a;
        a.id = label;   // labels are stable — good usage keys
        a.label = std::move(label);
        a.source = std::move(source);
        a.chord = chord;
        a.run = std::move(fn);
        palActions.push_back(std::move(a));
    };

    bool hasDoc = !tabs.empty();
    std::string ext;
    if (hasDoc && doc().filename != "untitled")
    {
        ext = std::filesystem::path(doc().filename).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char) std::tolower(c); });
    }
    const std::string docSrc = ext.empty() ? std::string("Document") : ("Filetype: " + ext);

    // File
    add("File: New", "Ctrl+N", "Core", [this] { newFile(); });
    add("File: Open...", "Ctrl+O", "Core", [this] { openFile(); });
    add("File: Open Project...", "", "Core", [this] { openProjectFolderPicker(); });
    if (hasDoc)
    {
        add("File: Save", "Ctrl+S", docSrc, [this] {
            if (doc().filename == "untitled") showSaveFileAs();
            else saveFile();
        });
        add("File: Save As...", "Ctrl+Shift+S", docSrc, [this] { showSaveFileAs(); });
        add("File: Close Tab", "Ctrl+W", docSrc, [this] {
            if (isDirty()) showConfirmClose([this]() { closeTab(activeTab); });
            else closeTab(activeTab);
        });
        add("File: Open Containing Folder", "", docSrc, [this] { openContainingFolder(); });
    }
    add("File: Reopen Closed Tab", "Ctrl+Shift+T", "Core", [this] { reopenLastClosedTab(); });
    add("File: Exit", "", "Core", [this] { tryToQuit(); });

    // View / panels
    add("View: Navigation Panel", "", "Core", [this] { navPanelVisible = !navPanelVisible; });
    add("View: Symbols Panel", "", "Core", [this] { symbolsPanelVisible = !symbolsPanelVisible; });
    add("View: References Panel", "", "Core", [this] { referencesVisible = !referencesVisible; });
    add("View: Markdown Preview", "", "Core", [this] { mdPreviewVisible = !mdPreviewVisible; });
    add("View: External Changes", "", "Core", [this] { externalChangesVisible = !externalChangesVisible; });
    add("View: Debug Panel", "", "Debugger", [this] { dbgPanelVisible = !dbgPanelVisible; });
    add("View: Developer Tools", "", "Core", [this] { devToolsVisible = !devToolsVisible; });
    add("View: Style Editor", "", "Core", [this] { showStyleEditor = !showStyleEditor; });
    add("View: Settings", "", "Core", [this] { settingsVisible = true; settingsFocusRequest = true; });
    if (hasDoc)
        add("View: Split Tab Right", "Ctrl+\\", docSrc, [this] { splitActiveTabRight(); });
    add("View: Zoom In", "Ctrl+=", "Core", [this] { increaseFontSIze(); });
    add("View: Zoom Out", "Ctrl+-", "Core", [this] { decreaseFontSIze(); });
    if (hasDoc)
    {
        add("View: Pop Out Document (Left)", "Ctrl+Alt+Left", docSrc, [this] { popOutActiveDoc(-1); });
        add("View: Pop Out Document (Right)", "Ctrl+Alt+Right", docSrc, [this] { popOutActiveDoc(1); });
        add("View: Merge Window Back", "Ctrl+Alt+M", docSrc, [this] { remergeActiveWindow(); });
    }
    add("View: Merge All Windows", "Ctrl+Alt+Shift+M", "Core", [this] { remergeAllWindows(); });
    for (int i = 0; i < themeCount(); ++i)
        add(std::string("Theme: ") + themeName(i), prefTheme == i ? "(current)" : "", "Theme",
            [this, i] {
                prefTheme = i;
                applyTheme(i);
                saveSettings();
            });

    // Find / navigate
    add("Find: In Files...", "Ctrl+Shift+F", "Core", [this] { openFindInFiles(); });
    add("Find: Go to Line...", "Ctrl+G", "Core", [this] { showGotoLine(); });
    add("Go: Back", "Alt+Left", "Core", [this] { navigateBack(); });
    add("Go: Forward", "Alt+Right", "Core", [this] { navigateForward(); });

    // Code / project
    if (hasDoc)
    {
        add("Code: Toggle Header/Source", "Alt+O", docSrc, [this] { toggleHeaderSource(); });
        add("Code: Format Document", "Alt+Shift+F", docSrc, [this] { formatActiveDocument(); });
        add("Diff: Against Saved", "", docSrc, [this] { showDiff(); });
        add("Diff: Against Another File...", "", docSrc, [this] { openDiffOtherDialog(); });
    }
    add("Code: Rebuild Symbol Index", "", "Project", [this] { rebuildProjectIndex(); });
    add("Project: Run", "F5", "Project", [this] { runProjectExeOrScript(); });
    add("Project: Run with Arguments...", "", "Project", [this] { runProjectWithArgs(); });
    add("Project: Build", "F6", "Project", [this] { runProjectBuild(); });
    if (hasDoc)
        add("Project: Run Active Document", "", docSrc, [this] { runScriptForDoc(); });

    // Debug
    if (!dbgSessionActive)
        add("Debug: Start Debugging", "", "Debugger", [this] { startDebugSession(); });
    else
    {
        add("Debug: Stop", "Shift+F5", "Debugger", [this] { stopDebugSession(); });
        if (dbgStopped)
        {
            add("Debug: Continue", "F5", "Debugger", [this] { dapClient.continueExec(dbgThreadId); });
            add("Debug: Step Over", "F10", "Debugger", [this] { dapClient.next(dbgThreadId); });
            add("Debug: Step Into", "F11", "Debugger", [this] { dapClient.stepIn(dbgThreadId); });
            add("Debug: Step Out", "Shift+F11", "Debugger", [this] { dapClient.stepOut(dbgThreadId); });
        }
    }
    if (hasDoc)
        add("Debug: Toggle Breakpoint", "F9", docSrc, [this] { toggleBreakpointAtCursor(); });
    add("Debug: Configure Adapters / Target...", "", "Debugger", [this] { dbgPanelVisible = true; });
    add("Debug: Launch in raddbg", "", "Debugger", [this] { debugInRadDbg(); });
    add("Debug: Launch in Visual Studio", "", "Debugger", [this] { debugInVisualStudio(); });

    // Git
    add("Git: Commit...", "", "Git", [this] { gitCommitRequest = true; });
    add("Git: Discard Changes...", "", "Git", [this] { gitDiscardRequest = true; });
    add("Git: History...", "", "Git", [this] { gitHistoryVisible = true; refreshGitHistory(); });
    add("Git: Open on Web", "", "Git", [this] { openGitOnWeb(); });

    // Editor extras
    add("View: Notes", "", "Core", [this] { notesVisible = !notesVisible; });
    add("View: Save Screenshot", "Ctrl+Alt+S", "Core", [this] { requestScreenshot(); });
    add("Help: Take the Tour", "", "Core", [this] { startTour(); });

    // Plugin contributions — gated by file type and/or project type inside each
    // plugin; the registry tags every entry with the contributing plugin.
    {
        PluginDocInfo info;
        if (hasDoc)
        {
            info.filename = doc().filename;
            info.extLower = ext;
            if (auto *lang = doc().editor.GetLanguage())
                info.languageName = lang->name;
        }
        pluginRegistry.paletteCommandsTagged(*this, info,
            [&](const std::string &label, std::function<void()> fn, const char *plugin) {
                add(label, "", std::string("Plugin: ") + (plugin ? plugin : "?"), std::move(fn));
            });
    }

    // Recents
    {
        int shown = 0;
        for (auto &p : recentFiles)
        {
            if (++shown > 8) break;
            std::string leaf = std::filesystem::path(p).filename().string();
            add("Recent File: " + leaf + "  (" + p + ")", "", "Recent", [this, p] { openFile(p); });
        }
        shown = 0;
        for (auto &p : recentProjects)
        {
            if (++shown > 5) break;
            std::string leaf = std::filesystem::path(p).filename().string();
            add("Recent Project: " + leaf, "", "Recent", [this, p] { setProjectRoot(std::filesystem::path(p)); });
        }
    }
}

void Editor::refilterPalette()
{
    palRows.clear();
    palRows.reserve(palActions.size());
    const long long now = (long long) std::time(nullptr);

    // Usage-derived boost: frequent picks float up; very recent picks more so.
    auto usageBoost = [&](const PaletteAction &a) {
        auto it = paletteUsage.find(a.id);
        if (it == paletteUsage.end())
            return 0;
        int b = (std::min)(it->second.uses, 10) * 2;
        long long age = now - it->second.last;
        if (age < 3600)               b += 8;   // used within the hour
        else if (age < 7 * 86400)     b += 4;   // used this week
        return b;
    };

    if (!palQuery[0])
    {
        // Empty query: most-recently/most-often used first, then declaration order.
        for (int i = 0; i < (int) palActions.size(); ++i)
            palRows.push_back({usageBoost(palActions[(size_t) i]), i});
        std::stable_sort(palRows.begin(), palRows.end(),
                         [](const auto &a, const auto &b) { return a.first > b.first; });
    }
    else
    {
        for (int i = 0; i < (int) palActions.size(); ++i)
        {
            int s = paletteFuzzyScore(palActions[(size_t) i].label, palQuery);
            if (s < 0)
                continue;
            palRows.push_back({s + usageBoost(palActions[(size_t) i]), i});
        }
        std::stable_sort(palRows.begin(), palRows.end(),
                         [](const auto &a, const auto &b) { return a.first > b.first; });
    }
    if (palSelected >= (int) palRows.size())
        palSelected = palRows.empty() ? 0 : (int) palRows.size() - 1;
    if (palSelected < 0)
        palSelected = 0;
}

void Editor::openCommandPalette()
{
    palVisible = true;
    palFocus = true;
    palQuery[0] = 0;
    palSelected = 0;
    palLastQuery = "\x01";       // force the first refilter
    buildPaletteActions();       // registry snapshot for THIS opening — not per frame
}

void Editor::renderCommandPalette()
{
    if (!palVisible)
        return;

    // Refilter only when the query text actually changed.
    if (palLastQuery != palQuery)
    {
        palLastQuery = palQuery;
        refilterPalette();
    }

    // Number-key selection. Alt+1..9 always runs the Nth visible row; plain
    // 1..9 does too while the query is EMPTY (checked before InputText appends
    // the digit to the buffer this frame). With a non-empty query, digits type.
    int numberPick = -1;
    {
        ImGuiIO &io = ImGui::GetIO();
        bool allowPlain = palQuery[0] == 0 && !io.KeyCtrl && !io.KeyShift;
        for (int d = 0; d < 9; ++d)
        {
            if (ImGui::IsKeyPressed((ImGuiKey) (ImGuiKey_1 + d), false) &&
                (io.KeyAlt || allowPlain))
            {
                numberPick = d;
                break;
            }
        }
    }

    // Float top-center over the main viewport (multi-viewport aware).
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const float width = 620.0f;
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
    std::function<void()> pending;   // run AFTER End() — actions may open dialogs/popups
    std::string pendingId;
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
            palSelected = palRows.empty() ? 0 : (palSelected + 1) % (int) palRows.size();
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            palSelected = palRows.empty() ? 0 : (palSelected + (int) palRows.size() - 1) % (int) palRows.size();
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
            palSelected = (std::min)((int) palRows.size() - 1, palSelected + 10);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            palSelected = (std::max)(0, palSelected - 10);

        ImGui::Separator();
        ImGui::BeginChild("##palList", ImVec2(0.0f, 380.0f));
        ImGuiListClipper clipper;
        clipper.Begin((int) palRows.size());
        // Keep the keyboard selection visible while stepping through the list.
        static int lastSel = -1;
        bool selMoved = palSelected != lastSel;
        lastSel = palSelected;
        while (clipper.Step())
        {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
            {
                const PaletteAction &a = palActions[(size_t) palRows[(size_t) r].second];
                ImGui::PushID(r);
                // Dim number hint for the first nine rows (Alt+N / N on empty query).
                if (r < 9)
                {
                    ImGui::TextDisabled("%d", r + 1);
                    ImGui::SameLine(0.0f, 8.0f);
                }
                else
                {
                    ImGui::TextDisabled(" ");
                    ImGui::SameLine(0.0f, 8.0f);
                }
                if (ImGui::Selectable(a.label.c_str(), r == palSelected,
                                      ImGuiSelectableFlags_AllowOverlap))
                {
                    pending = a.run;
                    pendingId = a.id;
                    palVisible = false;
                }
                if (ImGui::IsItemHovered() && !a.source.empty())
                    ImGui::SetTooltip("%s", a.source.c_str());
                if (r == palSelected && selMoved)
                    ImGui::SetScrollHereY(0.5f);
                // Right side: dim source tag, then the chord.
                float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                float x = right;
                if (a.chord && a.chord[0])
                    x -= ImGui::CalcTextSize(a.chord).x;
                std::string src = a.source;
                if (src.size() > 34)
                    src = src.substr(0, 32) + "…";
                float srcW = src.empty() ? 0.0f : ImGui::CalcTextSize(src.c_str()).x;
                if (!src.empty())
                {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(x - srcW - (a.chord && a.chord[0] ? 16.0f : 0.0f));
                    ImGui::TextDisabled("%s", src.c_str());
                }
                if (a.chord && a.chord[0])
                {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(right - ImGui::CalcTextSize(a.chord).x);
                    ImGui::TextDisabled("%s", a.chord);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled("%zu action%s   ↑↓ select · Enter run · Alt+1…9 quick-run · Esc close",
                            palRows.size(), palRows.size() == 1 ? "" : "s");

        if (numberPick >= 0 && numberPick < (int) palRows.size())
        {
            const PaletteAction &a = palActions[(size_t) palRows[(size_t) numberPick].second];
            pending = a.run;
            pendingId = a.id;
            palVisible = false;
        }
        else if (entered && !palRows.empty())
        {
            const PaletteAction &a = palActions[(size_t) palRows[(size_t) palSelected].second];
            pending = a.run;
            pendingId = a.id;
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
    {
        notePaletteUse(pendingId);   // usage/recency feeds the next ranking
        pending();
    }
}
