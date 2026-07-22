//
//  csvtable_plugin.h — view/edit CSV (and TSV) as an interactive table.
//
//  When the active document is a .csv/.tsv, a "CSV Table" panel (toggle in
//  Tools) parses it into a sortable, inline-editable ImGui table. Edits write
//  back to the document text (undoable via hostSetActiveText), so the table and
//  the raw text stay in sync. Pure RFC-4180-ish parsing (quoted fields with
//  embedded commas/quotes/newlines); first row = headers.
//
//  A first step toward the structured-data plugin (csv done; .db / xlsx later
//  need real libraries). Compiled only when IMGUIIDE_PLUGIN_CSVTABLE is set.
//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "plugin_api.h"

class CsvTablePlugin : public EditorPlugin
{
public:
    const char *id() const override { return "csvtable"; }
    const char *displayName() const override { return "CSV table"; }

    void onFrame(PluginHost &host) override;
    void onMenu(PluginHost &host, PluginMenu which) override;
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override;

private:
    bool visible_ = false;
    char delim_ = ',';
    // Parsed grid (row 0 = headers). Reparsed when the source text changes.
    std::vector<std::vector<std::string>> grid_;
    std::string cacheKey_;   // filename + text-size guard
    bool dirty_ = false;     // an edit needs writing back

    void reparse(const std::string &text);
    std::string serialize() const;
    static bool isCsvName(const std::string &name, char &delimOut);
};

std::unique_ptr<EditorPlugin> createCsvTablePlugin();
