//
//  cppgen_plugin.h — C++ member definition/declaration generation as a plugin.
//
//  Wraps the pure cppgen helpers (cppgen.{cpp,h}) behind the EditorPlugin hooks:
//    - onDocumentContextMenu: "Generate Definition(s)" on a member declaration
//      line / "Generate Declaration" on an out-of-line definition line, copied
//      to the clipboard (memoized — the hook runs every frame the menu is open).
//
//  Compiled only when IMGUIIDE_PLUGIN_CPPGEN is defined (see example/CMakeLists).
//

#pragma once

#include <memory>
#include <string>

#include "plugin_api.h"

class CppGenPlugin : public EditorPlugin
{
public:
    const char *id() const override { return "cppgen"; }
    const char *displayName() const override { return "C++ code generation"; }

    void onDocumentContextMenu(PluginHost &host, const PluginDocContext &ctx) override;

private:
    // Memoized scan results — the class scan reads whole-document text, and the
    // context-menu hook re-runs every frame the popup is open.
    std::string genKey;
    std::string genOneDef, genAllDefs, genDecl, genClass;
};

// Factory used by the DLL ABI shim (plugin_dll_main.cpp).
std::unique_ptr<EditorPlugin> createCppGenPlugin();
