//
//  texttools_plugin.h — everyday text-transform tools as an ImGui-IDE plugin.
//
//  Wraps the pure transforms (texttools.{cpp,h}) behind the EditorPlugin hooks:
//    - onMenu(Edit): JSON/XML + line-sorting tools. Selection-aware but never
//      REQUIRE a selection — with none they transform the whole document.
//    - onMenu(Selection): the extra case-conversion tools (Title/camel/snake;
//      UPPER/lower are core editor items already).
//    - onDocumentContextMenu: case tools FLAT (separator, no submenu), only
//      while a selection is active. JSON/XML/sorting stay out of the popup.
//    - contributePaletteCommands: everything from Ctrl+Shift+P.
//  Transforms write through hostReplaceSelection (selection) or
//  hostSetActiveText (whole document) — both undoable.
//
//  Compiled only when IMGUIIDE_PLUGIN_TEXTTOOLS is defined (see CMakeLists).
//

#pragma once

#include <memory>
#include <string>

#include "plugin_api.h"

class TextToolsPlugin : public EditorPlugin
{
public:
    const char *id() const override { return "texttools"; }
    const char *displayName() const override { return "Text tools"; }

    void onMenu(PluginHost &host, PluginMenu which) override;
    void onDocumentContextMenu(PluginHost &host, const PluginDocContext &ctx) override;
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override;

private:
    // Apply `fn(text, err)` to the current selection; toast on no selection /
    // transform error. Used by the case tools (which are about the selection).
    void applyToSelection(PluginHost &host, const char *what,
                          std::string (*fn)(const std::string &, std::string &));
    // Apply `fn` to the selection when one exists, else to the whole document.
    // Used by the JSON/XML/sort tools (no selection required).
    void applySelectionOrDoc(PluginHost &host, const char *what,
                             std::string (*fn)(const std::string &, std::string &));
};

// Factory used by the DLL ABI shim (plugin_dll_main.cpp).
std::unique_ptr<EditorPlugin> createTextToolsPlugin();
