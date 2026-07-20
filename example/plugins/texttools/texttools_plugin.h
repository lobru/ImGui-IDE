//
//  texttools_plugin.h — everyday text-transform tools as an ImGui-IDE plugin.
//
//  Wraps the pure transforms (texttools.{cpp,h}) behind the EditorPlugin hooks:
//    - onDocumentContextMenu: "Text Tools" submenu operating on the selection
//      (case conversion, line sorting, JSON pretty/minify, JSON <-> XML)
//    - contributePaletteCommands: the same tools from Ctrl+Shift+P
//  Transforms read hostActiveSelection() and write hostReplaceSelection()
//  (undoable); with no selection they toast instead of guessing.
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

    void onDocumentContextMenu(PluginHost &host, const PluginDocContext &ctx) override;
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override;

private:
    // Apply `fn(selection, err)` to the current selection; toast on no
    // selection / transform error.
    void applyToSelection(PluginHost &host, const char *what,
                          std::string (*fn)(const std::string &, std::string &));
};

// Factory used by the DLL ABI shim (plugin_dll_main.cpp).
std::unique_ptr<EditorPlugin> createTextToolsPlugin();
