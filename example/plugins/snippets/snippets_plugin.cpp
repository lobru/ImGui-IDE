//
//  snippets_plugin.cpp — see snippets_plugin.h.
//

#include "snippets_plugin.h"

#include <fstream>

#include "imgui.h"

#include <nlohmann/json.hpp>

namespace
{
// Starter snippets written on first run — also documents the format.
const char *kDefaultSnippetsJson = R"json({
  "_comment": "name -> body. $SEL = the selected text; use \n and \t for newlines/tabs. Add your own.",
  "for (i)": "for (int i = 0; i < $SEL; ++i)\n{\n\t\n}\n",
  "if": "if ($SEL)\n{\n\t\n}\n",
  "std::cout": "std::cout << $SEL << std::endl;\n",
  "guard": "#pragma once\n\n$SEL\n",
  "lambda": "[]($SEL) {\n\t\n}",
  "todo": "// TODO: $SEL\n"
})json";
} // namespace

std::filesystem::path SnippetsPlugin::snippetsPath(PluginHost &host) const
{
    return host.hostConfigDir() / "snippets.json";
}

void SnippetsPlugin::load(PluginHost &host)
{
    loaded_ = true;
    snippets_.clear();
    auto path = snippetsPath(host);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        // Seed a starter file so the feature is discoverable + self-documenting.
        std::filesystem::create_directories(path.parent_path(), ec);
        if (std::ofstream o{path, std::ios::binary})
            o << kDefaultSnippetsJson;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object())
        return;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (!it.value().is_string())
            continue; // skip the _comment (and any non-string entry)
        if (it.key().rfind('_', 0) == 0)
            continue; // underscore-prefixed keys are metadata
        snippets_.emplace_back(it.key(), it.value().get<std::string>());
    }
}

void SnippetsPlugin::onRegister(PluginHost &host)
{
    load(host);
}

void SnippetsPlugin::insert(PluginHost &host, const std::string &body)
{
    // $SEL -> current selection (so a snippet can wrap it); $0 -> stripped.
    std::string sel = host.hostActiveSelection();
    std::string out;
    out.reserve(body.size() + sel.size());
    for (size_t i = 0; i < body.size(); ++i)
    {
        if (body.compare(i, 4, "$SEL") == 0) { out += sel; i += 3; continue; }
        if (body.compare(i, 2, "$0") == 0) { i += 1; continue; }
        out += body[i];
    }
    host.hostReplaceSelection(out); // replaces the selection, else inserts at caret
}

void SnippetsPlugin::onMenu(PluginHost &host, PluginMenu which)
{
    if (which != PluginMenu::Tools)
        return;
    if (!loaded_)
        load(host);
    if (!ImGui::BeginMenu("Snippets"))
        return;
    if (snippets_.empty())
        ImGui::TextDisabled("(none — edit snippets.json)");
    for (auto &[name, body] : snippets_)
        if (ImGui::MenuItem(name.c_str()))
            insert(host, body);
    ImGui::Separator();
    if (ImGui::MenuItem("Reload snippets"))
        load(host);
    if (ImGui::MenuItem("Open snippets.json"))
        host.hostOpenFile(snippetsPath(host).string());
    ImGui::EndMenu();
}

void SnippetsPlugin::contributePaletteCommands(PluginHost &host, const PluginDocInfo &,
                                               const std::function<void(const std::string &,
                                                                        std::function<void()>)> &add)
{
    if (!loaded_)
        load(host);
    PluginHost *h = &host;
    for (auto &sn : snippets_)
    {
        std::string body = sn.second; // capture by value — outlives the deferred run
        add("Snippet: " + sn.first, [this, h, body]() { insert(*h, body); });
    }
}

std::unique_ptr<EditorPlugin> createSnippetsPlugin()
{
    return std::make_unique<SnippetsPlugin>();
}
