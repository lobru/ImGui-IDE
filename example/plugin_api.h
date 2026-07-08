//
//  plugin_api.h — ImGui-IDE in-process plugin API.
//
//  A plugin is a C++ module compiled into the app that extends the editor
//  through a small set of opt-in hooks, without touching Editor internals: it
//  only talks to the host through PluginHost. Domain-specific features (Unreal
//  Engine integration, UEVR/Blueprint tooling) live in plugins so the core
//  "ImGui IDE" carries none of their code when their CMake flag is off.
//
//  This header is intentionally light — no imgui, no Editor. Plugins that draw
//  UI include <imgui.h> themselves inside their onFrame/onMenu hooks.
//

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

// Top-level menus a plugin can contribute into (mirror renderMenuBar()).
enum class PluginMenu
{
    File,
    Edit,
    Selection,
    Find,
    View,
    Project,
    Help
};

// What the host knows about the document autocomplete is being built for.
struct PluginDocInfo
{
    std::string filename;     // may be "untitled"
    std::string extLower;     // ".lua", ".uproject", ... (lowercased; may be empty)
    std::string languageName; // TextEditor language name, e.g. "Lua", "C++" (may be empty)
};

// A build/run command a project-type provider returns. Mirrors the editor's
// internal build-command pair: `path` is either a script FILE (run
// `command "path"` in its parent dir) or a DIRECTORY (run `command` cd'd into
// it); `command` is the interpreter or the full command string.
struct PluginBuildCommand
{
    std::filesystem::path path;
    std::string           command;
};

// An extra read-only source root a plugin exposes in the navigation panel
// (e.g. a UE project's engine Source/ tree).
struct PluginSourceRoot
{
    std::string           label; // shown in the nav tree + Filters checkbox
    std::filesystem::path path;  // directory to browse
};

//
//  PluginHost — services the editor exposes to plugins. Implemented by Editor.
//  Method names are host-prefixed to avoid clashing with Editor's own members.
//
class PluginHost
{
public:
    virtual ~PluginHost() = default;

    // project + documents
    virtual std::filesystem::path hostProjectRoot() const = 0;
    virtual void hostSetProjectRoot(const std::string &path) = 0;
    virtual void hostOpenFile(const std::string &path) = 0;
    virtual void hostOpenLuaTab(const std::string &text) = 0; // new untitled Lua tab
    virtual std::string hostActiveText() const = 0;           // active doc's full text ("" if none)
    virtual std::string hostActiveSelection() const = 0;      // active doc's selection ("" if none)

    // feedback + running
    virtual void hostToast(const std::string &text) = 0;
    virtual void hostError(const std::string &message) = 0;                                     // modal error dialog
    virtual void hostRunInDir(const std::string &command, const std::filesystem::path &dir) = 0; // run in the Output panel
    virtual void hostRunProjectBuild() = 0;                                                     // the F6 build resolver

    // executable + repo locations (for locating bundled assets / dev-tree fallback)
    virtual std::filesystem::path hostExeDir() const = 0;
    virtual std::filesystem::path hostRepoRoot() const = 0;

    // shared UI preferences a plugin's own surfaces must honor
    virtual bool hostPanInverted() const = 0; // invert-pan setting (every pan surface honors it)

    // small persisted key -> bool store (runtime enable toggles, etc.)
    virtual bool hostGetFlag(const std::string &key, bool def) const = 0;
    virtual void hostSetFlag(const std::string &key, bool value) = 0;
};

//
//  EditorPlugin — one plugin. Every hook is opt-in (default no-op / nullopt).
//
class EditorPlugin
{
public:
    virtual ~EditorPlugin() = default;

    virtual const char *id() const = 0;          // stable identifier ("unreal", "uevr")
    virtual const char *displayName() const = 0; // shown in Settings

    // runtime enable/disable. The registry loads/persists this through the host's
    // flag store and gates every hook on it (a disabled plugin contributes
    // nothing — no menus, windows, autocomplete, language extras).
    virtual bool enabled() const { return enabledState; }
    virtual void setEnabled(bool value) { enabledState = value; }

    // one-time, at Editor construction (e.g. augment a shared language definition)
    virtual void onRegister(PluginHost &) {}

    // per-frame: draw dockable windows, poll inboxes, refresh caches
    virtual void onFrame(PluginHost &) {}

    // contribute items into the named menu (called inside that menu's scope)
    virtual void onMenu(PluginHost &, PluginMenu) {}

    // contribute autocomplete words for a document (add() inserts one word)
    virtual void contributeAutocomplete(PluginHost &, const PluginDocInfo &,
                                        const std::function<void(const std::string &)> &) {}

    // project-type provider: how to build/run a project rooted at startDir
    virtual std::optional<PluginBuildCommand> projectBuildCommand(const std::filesystem::path &) { return std::nullopt; }

    // go-to-file resolver: map an #include/require to an on-disk path
    virtual std::optional<std::filesystem::path> resolveInclude(const std::filesystem::path &, const std::string &) { return std::nullopt; }

    // extra read-only source root to expose in the nav panel for this project
    virtual std::optional<PluginSourceRoot> extraSourceRoot(const std::filesystem::path &) { return std::nullopt; }

protected:
    bool enabledState = true; // backs enabled()/setEnabled(); persisted by the registry
};
