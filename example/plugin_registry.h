//
//  plugin_registry.h — owns the built-in plugins and fans hooks out to them.
//
//  Editor holds one PluginRegistry, implements PluginHost, and calls the small
//  fan-out helpers below at each extension point. With no plugins compiled in
//  (all IMGUIIDE_PLUGIN_* off) every helper is a no-op and behavior is identical
//  to a plugin-less core.
//

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "plugin_api.h"

class PluginRegistry
{
public:
    void add(std::unique_ptr<EditorPlugin> plugin)
    {
        if (plugin)
            plugins.push_back(std::move(plugin));
    }

    const std::vector<std::unique_ptr<EditorPlugin>> &all() const { return plugins; }

    // one-time registration (language augmentation, etc.)
    void registerAll(PluginHost &host)
    {
        for (auto &p : plugins)
            p->onRegister(host);
    }

    // per-frame windows + polling
    void frame(PluginHost &host)
    {
        for (auto &p : plugins)
            if (p->enabled())
                p->onFrame(host);
    }

    // contribute into a menu (call inside that menu's scope)
    void menu(PluginHost &host, PluginMenu which)
    {
        for (auto &p : plugins)
            if (p->enabled())
                p->onMenu(host, which);
    }

    // gather autocomplete words
    void autocomplete(PluginHost &host, const PluginDocInfo &doc,
                      const std::function<void(const std::string &)> &addWord)
    {
        for (auto &p : plugins)
            if (p->enabled())
                p->contributeAutocomplete(host, doc, addWord);
    }

    // first plugin that claims the project wins
    std::optional<PluginBuildCommand> buildCommand(const std::filesystem::path &startDir)
    {
        for (auto &p : plugins)
            if (p->enabled())
                if (auto cmd = p->projectBuildCommand(startDir))
                    return cmd;
        return std::nullopt;
    }

    // first plugin that resolves the include wins
    std::optional<std::filesystem::path> resolveInclude(const std::filesystem::path &docDir,
                                                        const std::string &include)
    {
        for (auto &p : plugins)
            if (p->enabled())
                if (auto path = p->resolveInclude(docDir, include))
                    return path;
        return std::nullopt;
    }

private:
    std::vector<std::unique_ptr<EditorPlugin>> plugins;
};

// Constructs the plugins compiled into this build (guarded by IMGUIIDE_PLUGIN_*).
// Defined in plugin_registry.cpp.
void registerBuiltinPlugins(PluginRegistry &registry);
