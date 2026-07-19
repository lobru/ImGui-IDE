//
//  cppgen_plugin.cpp — C++ definition/declaration generation plugin.
//  Logic moved verbatim from the editor's context-menu block; the editor core
//  no longer carries any cppgen code.
//

#include "cppgen_plugin.h"

#include <string>

#include "imgui.h"

#include "cppgen.h"

void CppGenPlugin::onDocumentContextMenu(PluginHost &host, const PluginDocContext &ctx)
{
    if (ctx.doc.languageName != "C++" && ctx.doc.languageName != "C")
        return;

    // Memoized on (doc|line|lineCount|version) — the class scan reads
    // whole-document text and this hook re-runs every frame the popup is open.
    std::string key = ctx.doc.filename + "|" + std::to_string(ctx.line) + "|" +
                      std::to_string(ctx.lineCount) + "|" + std::to_string(ctx.docVersion);
    if (key != genKey)
    {
        genKey = key;
        std::string full = host.hostActiveText();
        std::string cls;
        genOneDef = cppgen::generateOneDefinition(full, ctx.line, &cls);
        genClass = cls;
        genAllDefs = cppgen::generateClassDefinitions(full, ctx.line, &cls);
        if (genClass.empty())
            genClass = cls;
        // Reverse (decl from def) only makes sense on an out-of-line definition
        // line — one containing "::" and not inside a class body.
        genDecl.clear();
        if (cls.empty() && ctx.lineText.find("::") != std::string::npos &&
            ctx.lineText.find('(') != std::string::npos)
            genDecl = cppgen::declarationFromDefinition(ctx.lineText);
    }

    if (!genOneDef.empty() || !genAllDefs.empty() || !genDecl.empty())
        ImGui::Separator();
    if (!genOneDef.empty() && ImGui::MenuItem("Generate Definition"))
    {
        ImGui::SetClipboardText(genOneDef.c_str());
        host.hostToast("\xe2\x9c\x8e Definition copied \xe2\x80\x94 paste into the .cpp");
    }
    if (!genAllDefs.empty())
    {
        std::string lbl = "Generate Definitions for " +
                          (genClass.empty() ? std::string("class") : genClass);
        if (ImGui::MenuItem(lbl.c_str()))
        {
            ImGui::SetClipboardText(genAllDefs.c_str());
            host.hostToast("\xe2\x9c\x8e Definitions copied \xe2\x80\x94 paste into the .cpp");
        }
    }
    if (!genDecl.empty() && ImGui::MenuItem("Generate Declaration"))
    {
        ImGui::SetClipboardText(genDecl.c_str());
        host.hostToast("\xe2\x9c\x8e Declaration copied \xe2\x80\x94 paste into the header");
    }
}

std::unique_ptr<EditorPlugin> createCppGenPlugin()
{
    return std::make_unique<CppGenPlugin>();
}
