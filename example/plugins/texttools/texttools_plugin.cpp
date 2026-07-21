//
//  texttools_plugin.cpp — see texttools_plugin.h.
//

#include "texttools_plugin.h"

#include "imgui.h"

#include "texttools.h"

namespace
{
// Adapters so every tool fits one `string(const string&, string& err)` shape.
std::string tUpper(const std::string &s, std::string &) { return texttools::convertCase(s, texttools::Case::Upper); }
std::string tLower(const std::string &s, std::string &) { return texttools::convertCase(s, texttools::Case::Lower); }
std::string tTitle(const std::string &s, std::string &) { return texttools::convertCase(s, texttools::Case::Title); }
std::string tCamel(const std::string &s, std::string &) { return texttools::convertCase(s, texttools::Case::Camel); }
std::string tSnake(const std::string &s, std::string &) { return texttools::convertCase(s, texttools::Case::Snake); }
std::string tSortAZ(const std::string &s, std::string &) { return texttools::sortLines(s, false, true); }
std::string tSortZA(const std::string &s, std::string &) { return texttools::sortLines(s, false, false); }
std::string tSort09(const std::string &s, std::string &) { return texttools::sortLines(s, true, true); }
std::string tSort90(const std::string &s, std::string &) { return texttools::sortLines(s, true, false); }
std::string tUnique(const std::string &s, std::string &) { return texttools::uniqueLines(s); }
std::string tNumber(const std::string &s, std::string &) { return texttools::numberLines(s); }
std::string tJsonPretty(const std::string &s, std::string &e) { return texttools::jsonPretty(s, e); }
std::string tJsonMin(const std::string &s, std::string &e) { return texttools::jsonMinify(s, e); }
std::string tJsonXml(const std::string &s, std::string &e) { return texttools::jsonToXml(s, e); }
std::string tXmlJson(const std::string &s, std::string &e) { return texttools::xmlToJson(s, e); }

struct Tool
{
    const char *menuLabel;    // menu row
    const char *paletteLabel; // palette row
    std::string (*fn)(const std::string &, std::string &);
    bool separatorBefore = false;
};

// Case conversions — inherently about the SELECTION. Live in the Selection
// menu and (flat) in the right-click context menu while a selection is active.
const Tool kCaseTools[] = {
    {"UPPER CASE", "Text: Selection to UPPER CASE", tUpper},
    {"lower case", "Text: Selection to lower case", tLower},
    {"Title Case", "Text: Selection to Title Case", tTitle},
    {"camelCase", "Text: Selection to camelCase", tCamel},
    {"snake_case", "Text: Selection to snake_case", tSnake},
};

// Document tools — selection-aware but never require one (whole doc when no
// selection). Live in the Edit menu; kept OUT of the context menu.
const Tool kDocTools[] = {
    {"Sort Lines A->Z", "Text: Sort Lines A->Z", tSortAZ},
    {"Sort Lines Z->A", "Text: Sort Lines Z->A", tSortZA},
    {"Sort Lines 0->9", "Text: Sort Lines 0->9 (numeric)", tSort09},
    {"Sort Lines 9->0", "Text: Sort Lines 9->0 (numeric)", tSort90},
    {"Unique Lines", "Text: Unique Lines (drop duplicates)", tUnique},
    {"Number Lines", "Text: Number Lines (1. 2. 3. ...)", tNumber},
    {"JSON: Pretty-print", "JSON: Pretty-print", tJsonPretty, true},
    {"JSON: Minify", "JSON: Minify", tJsonMin},
    {"JSON -> XML", "JSON: Convert to XML", tJsonXml},
    {"XML -> JSON", "XML: Convert to JSON", tXmlJson},
};
} // namespace

void TextToolsPlugin::applyToSelection(PluginHost &host, const char *what,
                                       std::string (*fn)(const std::string &, std::string &))
{
    std::string sel = host.hostActiveSelection();
    if (sel.empty())
    {
        host.hostToast(std::string(what) + ": select some text first");
        return;
    }
    std::string err;
    std::string out = fn(sel, err);
    if (!err.empty())
    {
        host.hostToast(std::string(what) + ": " + err);
        return;
    }
    host.hostReplaceSelection(out);
}

void TextToolsPlugin::applySelectionOrDoc(PluginHost &host, const char *what,
                                          std::string (*fn)(const std::string &, std::string &))
{
    std::string sel = host.hostActiveSelection();
    bool useSel = !sel.empty();
    std::string in = useSel ? std::move(sel) : host.hostActiveText();
    if (in.empty())
    {
        host.hostToast(std::string(what) + ": document is empty");
        return;
    }
    std::string err;
    std::string out = fn(in, err);
    if (!err.empty())
    {
        host.hostToast(std::string(what) + ": " + err);
        return;
    }
    if (useSel)
        host.hostReplaceSelection(out);
    else
        host.hostSetActiveText(out);
}

void TextToolsPlugin::onMenu(PluginHost &host, PluginMenu which)
{
    if (which == PluginMenu::Tools)
    {
        // JSON/XML + sorting live under Tools > Text Tools (user-preferred
        // home). Whole-document tools — they operate on the selection when
        // one is active. Sorting a SINGLE-line selection is meaningless —
        // line tools enable only for a multi-line selection or no selection.
        if (!ImGui::BeginMenu("Text Tools"))
            return;
        std::string sel = host.hostActiveSelection();
        bool sortOk = sel.empty() || sel.find('\n') != std::string::npos;
        for (const auto &t : kDocTools)
        {
            if (t.separatorBefore)
                ImGui::Separator();
            bool isLineTool = t.fn == tSortAZ || t.fn == tSortZA || t.fn == tSort09 ||
                              t.fn == tSort90 || t.fn == tUnique || t.fn == tNumber;
            if (ImGui::MenuItem(t.menuLabel, nullptr, false, !isLineTool || sortOk))
                applySelectionOrDoc(host, t.menuLabel, t.fn);
        }
        ImGui::EndMenu();
    }
    else if (which == PluginMenu::Selection)
    {
        // Extra case conversions next to the core To Uppercase/To Lowercase
        // rows (which stay core — the widget handles those two natively).
        ImGui::Separator();
        bool hasSel = !host.hostActiveSelection().empty();
        for (const Tool *t : {&kCaseTools[2], &kCaseTools[3], &kCaseTools[4]})
            if (ImGui::MenuItem(t->menuLabel, nullptr, false, hasSel))
                applyToSelection(host, t->menuLabel, t->fn);
    }
}

void TextToolsPlugin::onDocumentContextMenu(PluginHost &host, const PluginDocContext &)
{
    // Case tools only, flat (no submenu), and only while a selection exists.
    if (host.hostActiveSelection().empty())
        return;
    ImGui::Separator();
    for (const auto &t : kCaseTools)
        if (ImGui::MenuItem(t.menuLabel))
            applyToSelection(host, t.menuLabel, t.fn);
}

void TextToolsPlugin::contributePaletteCommands(PluginHost &host, const PluginDocInfo &,
                                                const std::function<void(const std::string &,
                                                                         std::function<void()>)> &add)
{
    // The host is the Editor (process lifetime) and the tool tables are static —
    // both safely outlive the palette's deferred run.
    PluginHost *h = &host;
    for (const auto &t : kCaseTools)
        add(t.paletteLabel, [this, h, &t]() { applyToSelection(*h, t.menuLabel, t.fn); });
    for (const auto &t : kDocTools)
        add(t.paletteLabel, [this, h, &t]() { applySelectionOrDoc(*h, t.menuLabel, t.fn); });
}

std::unique_ptr<EditorPlugin> createTextToolsPlugin()
{
    return std::make_unique<TextToolsPlugin>();
}
