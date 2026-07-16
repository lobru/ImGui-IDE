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
#include <string>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "plugin_api.h"

class PluginRegistry
{
public:
    // Plugins can come from DLLs (see plugin_loader.cpp) whose code — including the
    // EditorPlugin vtable — is unmapped as the process tears down. Deleting a plugin
    // then would dispatch a virtual dtor through freed memory (access violation on
    // shutdown). Plugins live for the whole process and hold no resources needing an
    // explicit dtor, so release (intentionally leak) them here; the OS reclaims the
    // memory. This sidesteps the cross-DLL delete-at-teardown hazard entirely.
    ~PluginRegistry()
    {
        for (auto &p : plugins)
            p.release();
    }

    void add(std::unique_ptr<EditorPlugin> plugin)
    {
        if (plugin)
            plugins.push_back(std::move(plugin));
    }

    const std::vector<std::unique_ptr<EditorPlugin>> &all() const { return plugins; }

    // settings key a plugin's persisted enabled flag lives under.
    static std::string enabledKey(const std::string &id) { return "plugin." + id + ".enabled"; }

    // one-time registration. Loads each plugin's persisted enabled flag (default
    // on), then runs onRegister only for the enabled ones — a plugin disabled at
    // startup does not augment shared state (e.g. C++ language keywords) until
    // enabled. Call after the host's settings have loaded.
    void registerAll(PluginHost &host)
    {
        for (auto &p : plugins)
        {
            p->setEnabled(host.hostGetFlag(enabledKey(p->id()), true));
            if (p->enabled() && registered.insert(p->id()).second)
                p->onRegister(host);
        }
    }

    // flip a plugin's enabled state at runtime, persist it through the host, and
    // lazily run its one-time onRegister the first time it becomes enabled.
    void setEnabled(PluginHost &host, EditorPlugin &plugin, bool value)
    {
        plugin.setEnabled(value);
        host.hostSetFlag(enabledKey(plugin.id()), value);
        if (value && registered.insert(plugin.id()).second)
            plugin.onRegister(host);
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

    // Render each enabled plugin's own top-level menu-bar entry (BeginMenu owned by
    // the host). Call between the Tools and Help menus in renderMenuBar().
    void topLevelMenus(PluginHost &host)
    {
        for (auto &p : plugins)
        {
            if (!p->enabled())
                continue;
            const char *title = p->topLevelMenu();
            if (title && *title && ImGui::BeginMenu(title))
            {
                p->onTopLevelMenu(host);
                ImGui::EndMenu();
            }
        }
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

    // first plugin that claims the file (before external-open) wins
    bool openFile(PluginHost &host, const std::filesystem::path &path)
    {
        for (auto &p : plugins)
            if (p->enabled())
                if (p->openFile(host, path))
                    return true;
        return false;
    }

    // first plugin that offers an extra nav source root for this project wins
    std::optional<PluginSourceRoot> extraSourceRoot(const std::filesystem::path &projectRoot)
    {
        for (auto &p : plugins)
            if (p->enabled())
                if (auto root = p->extraSourceRoot(projectRoot))
                    return root;
        return std::nullopt;
    }

private:
    std::vector<std::unique_ptr<EditorPlugin>> plugins;
    std::unordered_set<std::string> registered; // ids whose onRegister already ran
};

// Constructs the plugins compiled into this build (guarded by IMGUIIDE_PLUGIN_*).
// Defined in plugin_registry.cpp.
void registerBuiltinPlugins(PluginRegistry &registry);
