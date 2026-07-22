//
//  lint_plugin.h — lightweight heuristic static analysis for C/C++.
//
//  Line-based (no full parse) checks that flag common issues with a gutter
//  marker + tooltip and list them in an "Analysis" panel (toggle in Tools),
//  where fixable findings get a one-click Fix (rewrites the line via
//  hostSetActiveText). Checks: `== true`/`== false` redundancy, C `NULL` in
//  C++ (-> nullptr), trailing whitespace, and TODO/FIXME/XXX notes.
//
//  Only lints the ACTIVE document (contributeMarkers runs per tab but the host
//  exposes text for the active doc only). Compiled when IMGUIIDE_PLUGIN_LINT.
//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "plugin_api.h"

class LintPlugin : public EditorPlugin
{
public:
    const char *id() const override { return "lint"; }
    const char *displayName() const override { return "Static analysis"; }

    void onRegister(PluginHost &host) override;
    void onFrame(PluginHost &host) override;
    void onMenu(PluginHost &host, PluginMenu which) override;
    void contributeMarkers(PluginHost &host, const PluginDocInfo &doc,
                           const std::function<void(int, unsigned, unsigned,
                                                    const std::string &, const std::string &)> &add) override;
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override;

private:
    struct Finding {
        int line0 = 0;
        bool warn = true;             // warn (yellow) vs note (blue)
        std::string message;
        std::string fixedLine;        // non-empty = auto-fixable; the replacement line text
    };
    std::vector<Finding> findings_;
    std::vector<std::string> docLines_;   // current parse of the active doc
    std::string cacheKey_;                // filename + text-size guard
    bool panelVisible_ = false;
    bool enabled_ = true;

    // Re-scan the active document when it changed. cxx = C/C++ language.
    void refresh(PluginHost &host);
    // Rewrite line `line0` of the active doc to `newText` via hostSetActiveText.
    void applyFix(PluginHost &host, int line0, const std::string &newText);
};

std::unique_ptr<EditorPlugin> createLintPlugin();
