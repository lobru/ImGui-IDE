//
//  snippets_plugin.h — user-defined code snippets / macros as a plugin.
//
//  Snippets are name -> body pairs loaded from <config>/snippets.json (created
//  with a few starter entries on first run). Each snippet inserts its body at
//  the cursor (replacing any selection), with a couple of placeholder
//  expansions:
//     $SEL   -> the text that was selected (wrap-selection snippets)
//     $0     -> caret marker (stripped; the caret is left where it was — v1
//               does not implement tab-stops, just a single final caret hint)
//     \t, \n -> literal tab / newline in the JSON string
//  Exposed through:
//     - onMenu(Tools): a "Snippets" submenu (insert / reload / open file)
//     - contributePaletteCommands: "Snippet: <name>" for each snippet
//  The add API is just the JSON file — documented in the starter file — so a
//  user (or another tool) extends snippets without touching code.
//
//  Compiled only when IMGUIIDE_PLUGIN_SNIPPETS is defined (see CMakeLists).
//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "plugin_api.h"

class SnippetsPlugin : public EditorPlugin
{
public:
    const char *id() const override { return "snippets"; }
    const char *displayName() const override { return "Snippets"; }

    void onRegister(PluginHost &host) override;
    void onMenu(PluginHost &host, PluginMenu which) override;
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override;

private:
    std::vector<std::pair<std::string, std::string>> snippets_; // name -> body
    bool loaded_ = false;

    std::filesystem::path snippetsPath(PluginHost &host) const;
    void load(PluginHost &host);      // (re)read the JSON, seeding a default file
    void insert(PluginHost &host, const std::string &body);
};

std::unique_ptr<EditorPlugin> createSnippetsPlugin();
