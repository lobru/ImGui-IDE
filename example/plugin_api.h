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
#include <vector>

// Top-level menus a plugin can contribute into (mirror renderMenuBar()).
enum class PluginMenu
{
    File,
    Edit,
    Selection,
    Find,
    View,
    Tools,
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

// Where a document context menu (right-click in the editor) was opened. The
// version/lineCount pair lets a plugin memoize expensive whole-text work (the
// menu hook runs every frame the popup is open) and only re-pull the text via
// PluginHost::hostActiveText() when the document actually changed.
struct PluginDocContext
{
    PluginDocInfo doc;
    int         line = 0;      // 0-based cursor line the menu was opened on
    std::string word;          // identifier under the cursor (may be empty)
    std::string lineText;      // full text of that line
    int         lineCount = 0; // document line count (memo key part)
    int         docVersion = 0;// undo index (memo key part)
};

// One rebindable app-level shortcut a plugin contributes. Shows up in
// Settings > Keybinds under the plugin's own group, participates in the same
// override persistence ([keybinds] in settings), and dispatches through the
// host's chord matcher every frame the app shortcuts run.
struct PluginKeybind
{
    std::string id;           // stable bind id — prefix with the plugin id ("terminal.toggle")
    std::string action;       // human label for the Settings row
    std::string defaultChord; // e.g. "Ctrl+`" or "Ctrl+K Ctrl+T" (two-stroke supported)
    std::function<void()> run;
    std::string group;        // left empty by the plugin; the registry fills the display name
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
    virtual std::string hostActiveFilename() const = 0;       // active doc's path ("" if none; "untitled" if unsaved)

    // feedback + running
    virtual void hostToast(const std::string &text) = 0;
    virtual void hostError(const std::string &message) = 0;                                     // modal error dialog

    // hand a message to the connected AI assistant (Claude) via the editor's
    // reply outbox — the same channel the in-editor "Reply to Claude" feature
    // uses. A Claude Code loop watching the project picks it up. Fire-and-forget.
    virtual void hostSendToClaude(const std::string &message) = 0;

    // Called by a plugin window (during onFrame) that currently has focus and
    // handles its own Ctrl+Z/C/V/etc, so the editor SKIPS its app-level keyboard
    // shortcuts this frame and the keys route to the plugin. Reset each frame.
    virtual void hostSuppressAppShortcuts() = 0;

    virtual void hostRunInDir(const std::string &command, const std::filesystem::path &dir) = 0; // run in the Output panel
    virtual void hostRunProjectBuild() = 0;                                                     // the F6 build resolver

    // executable + repo locations (for locating bundled assets / dev-tree fallback)
    virtual std::filesystem::path hostExeDir() const = 0;
    virtual std::filesystem::path hostRepoRoot() const = 0;

    // shared UI preferences a plugin's own surfaces must honor
    virtual bool hostPanInverted() const = 0; // invert-pan setting (every pan surface honors it)

    // Apply the editor's middle-mouse drag-to-pan/scroll to the CURRENT ImGui window
    // (call inside a scroll child, like the editor does in its panels). Honors the
    // invert-pan + accel prefs. windowKey must be unique across all live pan surfaces;
    // the editor uses 1..99, so plugins should use 100+.
    virtual void hostMiddleMousePanScroll(int windowKey) = 0;

    // augment the editor's shared C++ language definition (add type/keyword
    // vocabulary, install an isTypeLike fallback). Routed through the host so a
    // plugin — including one loaded from a DLL with its own static TextEditor —
    // mutates the ONE Language the editor actually highlights with, not a private
    // copy. isTypeLike may be null to leave the existing fallback in place.
    virtual void hostAugmentCppLanguage(const std::vector<std::string> &types,
                                        const std::vector<std::string> &keywords,
                                        bool (*isTypeLike)(const std::string &)) = 0;

    // small persisted key -> bool store (runtime enable toggles, etc.)
    virtual bool hostGetFlag(const std::string &key, bool def) const = 0;
    virtual void hostSetFlag(const std::string &key, bool value) = 0;

    // Replace the active document's current selection (undoable). No-op when
    // nothing is selected — the workhorse for text-transform plugins. NOTE:
    // appended last deliberately; adding host virtuals anywhere else reorders
    // the vtable for every existing plugin (see IMGUIIDE_PLUGIN_ABI_VERSION).
    virtual void hostReplaceSelection(const std::string &text) = 0;
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

    // Own a TOP-LEVEL menu-bar entry. Return the menu title (e.g. "Unreal") to get
    // a dedicated menu between Tools and Help instead of nesting items under an
    // existing menu; return nullptr (default) for no top-level menu. When present,
    // the host opens the BeginMenu for you and calls onTopLevelMenu() inside it.
    virtual const char *topLevelMenu() const { return nullptr; }
    virtual void onTopLevelMenu(PluginHost &) {}

    // contribute autocomplete words for a document (add() inserts one word)
    virtual void contributeAutocomplete(PluginHost &, const PluginDocInfo &,
                                        const std::function<void(const std::string &)> &) {}

    // Contribute rebindable app-level keybinds (append to `out`). Collected each
    // frame from enabled plugins; the registry stamps `group` with displayName().
    virtual void contributeKeybinds(PluginHost &, std::vector<PluginKeybind> &) {}

    // Contribute items into the editor's right-click context menu (called inside
    // the popup's scope, every frame it is open — draw ImGui::MenuItem yourself
    // and memoize anything expensive on ctx.line/lineCount/docVersion).
    virtual void onDocumentContextMenu(PluginHost &, const PluginDocContext &) {}

    // Contribute command-palette entries (Ctrl+Shift+P). Called every frame the
    // palette is open; add(label, run) inserts one command. `doc` describes the
    // ACTIVE document (empty fields when none), so a plugin can gate commands on
    // file type; gate on project type by probing hostProjectRoot() (e.g. the
    // Unreal plugin only contributes when the project has a .uproject/.uplugin).
    virtual void contributePaletteCommands(PluginHost &, const PluginDocInfo &,
                                           const std::function<void(const std::string &,
                                                                    std::function<void()>)> &) {}

    // project-type provider: how to build/run a project rooted at startDir
    virtual std::optional<PluginBuildCommand> projectBuildCommand(const std::filesystem::path &) { return std::nullopt; }

    // go-to-file resolver: map an #include/require to an on-disk path
    virtual std::optional<std::filesystem::path> resolveInclude(const std::filesystem::path &, const std::string &) { return std::nullopt; }

    // extra read-only source root to expose in the nav panel for this project
    virtual std::optional<PluginSourceRoot> extraSourceRoot(const std::filesystem::path &) { return std::nullopt; }

    // Claim a file the host would otherwise open in an external app (a binary type
    // the editor can't display). Return true if handled — e.g. the Unreal plugin
    // turns a .uasset into an inspection report and opens THAT in a tab, so binary
    // assets are viewable in-app instead of launching the OS default. Called before
    // the host falls back to the external opener.
    virtual bool openFile(PluginHost &, const std::filesystem::path &) { return false; }

protected:
    bool enabledState = true; // backs enabled()/setEnabled(); persisted by the registry
};
