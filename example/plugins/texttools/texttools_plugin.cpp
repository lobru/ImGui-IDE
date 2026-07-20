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
std::string tJsonPretty(const std::string &s, std::string &e) { return texttools::jsonPretty(s, e); }
std::string tJsonMin(const std::string &s, std::string &e) { return texttools::jsonMinify(s, e); }
std::string tJsonXml(const std::string &s, std::string &e) { return texttools::jsonToXml(s, e); }
std::string tXmlJson(const std::string &s, std::string &e) { return texttools::xmlToJson(s, e); }

struct Tool
{
    const char *menuLabel;    // context-menu row
    const char *paletteLabel; // palette row
    std::string (*fn)(const std::string &, std::string &);
    bool separatorBefore = false;
};
const Tool kTools[] = {
    {"UPPER CASE", "Text: Selection to UPPER CASE", tUpper},
    {"lower case", "Text: Selection to lower case", tLower},
    {"Title Case", "Text: Selection to Title Case", tTitle},
    {"camelCase", "Text: Selection to camelCase", tCamel},
    {"snake_case", "Text: Selection to snake_case", tSnake},
    {"Sort Lines A->Z", "Text: Sort Lines A->Z", tSortAZ, true},
    {"Sort Lines Z->A", "Text: Sort Lines Z->A", tSortZA},
    {"Sort Lines 0->9", "Text: Sort Lines 0->9 (numeric)", tSort09},
    {"Sort Lines 9->0", "Text: Sort Lines 9->0 (numeric)", tSort90},
    {"JSON: Pretty-print", "JSON: Pretty-print Selection", tJsonPretty, true},
    {"JSON: Minify", "JSON: Minify Selection", tJsonMin},
    {"JSON -> XML", "JSON: Selection to XML", tJsonXml},
    {"XML -> JSON", "XML: Selection to JSON", tXmlJson},
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

void TextToolsPlugin::onDocumentContextMenu(PluginHost &host, const PluginDocContext &)
{
    ImGui::Separator();
    if (ImGui::BeginMenu("Text Tools"))
    {
        for (const auto &t : kTools)
        {
            if (t.separatorBefore)
                ImGui::Separator();
            if (ImGui::MenuItem(t.menuLabel))
                applyToSelection(host, t.menuLabel, t.fn);
        }
        ImGui::EndMenu();
    }
}

void TextToolsPlugin::contributePaletteCommands(PluginHost &host, const PluginDocInfo &,
                                                const std::function<void(const std::string &,
                                                                         std::function<void()>)> &add)
{
    // The host is the Editor (process lifetime) and kTools is static — both
    // safely outlive the palette's deferred run.
    PluginHost *h = &host;
    for (const auto &t : kTools)
        add(t.paletteLabel, [this, h, &t]() { applyToSelection(*h, t.menuLabel, t.fn); });
}

std::unique_ptr<EditorPlugin> createTextToolsPlugin()
{
    return std::make_unique<TextToolsPlugin>();
}
