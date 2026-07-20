//  TextEdit//  TextEditor - A syntax highlighting text editor for ImGui
//  Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//  Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//
//  This work is licensed under the terms of the MIT license.
//  For a copy, see <https://opensource.org/licenses/MIT>.

//
//  Include files
//
#define _CRT_SECURE_NO_WARNINGS // for std::getenv used in #include resolution
#ifndef _WIN32
// Expose POSIX declarations (popen/pclose, etc.) under strict -std=c++17, which
// defines __STRICT_ANSI__ and would otherwise hide them in glibc. Must come
// before any standard header is included.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif
#ifdef _WIN32
#include <Windows.h>
#include <process.h>  // _popen / _pclose
#include <shellapi.h> // SHFileOperation for recycle-bin delete
#elif defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath for get_module_path()
#endif
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
// The codebase uses the MSVC spellings _popen/_pclose at several call sites that
// aren't individually platform-guarded; map them to the POSIX names so they
// compile and work on Linux/macOS. (Guarded call sites already use popen/pclose
// directly in their #else branch, so this only affects the unguarded ones.)
#define _popen popen
#define _pclose pclose
#endif

#include <nlohmann/json.hpp>

#include "ImGuiFileDialog.h"
#include "imgui.h"
#include "imgui_internal.h"

// stb_image — define our own static-linkage copy in this translation unit so
// the image viewer can load PNG/JPG/etc. (ImGuiFileDialog also embeds it, but
// gates exports inconsistently — easier to just have our own private copy.)
// Suppress C4505 (unreferenced static functions) — stb_image has many.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "stb/stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "editor.h"
#include "plugin_loader.h"
#include "tsindex.h"

#include <chrono>

namespace
{
// Lightweight scoped timer. Logs to stderr only when a block exceeds 1ms, so
// it is silent in the common case and surfaces real stalls. Visible in a
// terminal (AttachConsole). Remove the threshold to log everything.
struct ScopedTimer
{
    const char *name;
    std::chrono::steady_clock::time_point t0;
    explicit ScopedTimer(const char *n)
        : name(n), t0(std::chrono::steady_clock::now())
    {
    }
    ~ScopedTimer()
    {
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        if (ms > 1.0)
            std::fprintf(stderr, "[perf] %s: %.1f ms\n", name, ms);
    }
};
} // namespace

// Forward decls so callers above the definitions (e.g. Editor::tryToQuit)
// can persist favourites at any quit/teardown path.
static void saveFileDialogPlaces();
static void populateFileDialogPlaces();
static std::filesystem::path resolveOutermostRoot(std::filesystem::path start);

//
//  Constants
//

#if __APPLE__
#define SHORTCUT "Cmd-"
#else
#define SHORTCUT "Ctrl-"
#endif

// Files at or above this size load on a worker thread (TextEditor::SetTextAsync)
// instead of stalling the frame in SetText. Well under the 8 MB large-file cutoff:
// a 1 MB source file already costs tens of ms to glyph-build + colorize.
static constexpr size_t kAsyncLoadBytes = 1u * 1024 * 1024;

static const char *demo =
    "// Demo C++ Code\n"
    "\n"
    "#include <iostream>\n"
    "#include <random>\n"
    "#include <vector>\n"
    "\n"
    "int main(int, char**) {\n"
    "   std::random_device rd;\n"
    "   std::mt19937 gen(rd());\n"
    "   std::uniform_int_distribution<> distrib(0, 1000);\n"
    "   std::vector<int> numbers;\n"
    "\n"
    "   for (auto i = 0; i < 100; i++) {\n"
    "       numbers.emplace_back(distrib(gen));\n"
    "   }\n"
    "\n"
    "   for (auto n : numbers) {\n"
    "       std::cout << n << std::endl;\n"
    "   }\n"
    "\n"
    "   return 0;\n"
    "}\n";

//
//  Editor::languageForPath
//

std::unordered_map<std::string, const TextEditor::Language *> &Editor::runtimeLanguagesByExt()
{
    static std::unordered_map<std::string, const TextEditor::Language *> map;
    return map;
}

std::unordered_map<std::string, std::string> &Editor::extLanguageOverrides()
{
    static std::unordered_map<std::string, std::string> map;
    return map;
}

const TextEditor::Language *Editor::languageByName(const std::string &name)
{
    if (name.empty() || name == "None")
        return nullptr;
    if (name == "C") return TextEditor::Language::C();
    if (name == "C++") return TextEditor::Language::Cpp();
    if (name == "C#") return TextEditor::Language::Cs();
    if (name == "AngelScript") return TextEditor::Language::AngelScript();
    if (name == "Lua") return TextEditor::Language::Lua();
    if (name == "Python") return TextEditor::Language::Python();
    if (name == "GLSL") return TextEditor::Language::Glsl();
    if (name == "HLSL") return TextEditor::Language::Hlsl();
    if (name == "JSON") return TextEditor::Language::Json();
    if (name == "Markdown") return TextEditor::Language::Markdown();
    if (name == "SQL") return TextEditor::Language::Sql();
    if (name == "INI") return TextEditor::Language::Ini();
    // Runtime-loaded languages (HTML, YAML, XAML, …) — match by their name.
    for (auto &[ext, lang] : runtimeLanguagesByExt())
    {
        (void) ext;
        if (lang && lang->name == name)
            return lang;
    }
    return nullptr;
}

static std::filesystem::path get_module_path()
{
#ifdef _WIN32
    char buf[256]{};
    if (auto d = GetModuleFileNameA(nullptr, buf, sizeof(buf)))
    {
        (void)d;
        return std::filesystem::path(buf);
    }
    return std::filesystem::current_path();
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
    {
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(std::filesystem::path(buf), ec);
        return ec ? std::filesystem::path(buf) : canon;
    }
    return std::filesystem::current_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : p;
#endif
}

// Launch another copy of this executable as a separate instance. `--new` bypasses
// per-project coalescing, so this always opens a distinct window; an optional
// project root opens that folder in the new window. Best-effort (no-op on failure).
static void spawnNewInstance(const std::string &projectRoot)
{
    std::filesystem::path exe = get_module_path();
#ifdef _WIN32
    std::string args = "--new";
    if (!projectRoot.empty())
        args += " --project \"" + projectRoot + "\"";
    ShellExecuteA(nullptr, "open", exe.string().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "\"" + exe.string() + "\" --new";
    if (!projectRoot.empty())
        cmd += " --project \"" + projectRoot + "\"";
    cmd += " &";
    (void)std::system(cmd.c_str());
#endif
}

// Copy the bundled *.lang into the writable user languages dir as editable
// starting points — but only if that dir has no .lang yet, so it never clobbers
// the user's edits or re-adds files they deleted. NOTE: an unedited copy here
// overrides the identical bundled definition; a future bundled change won't reach
// a type the user has a copy of until they delete that copy.
static void seedUserLanguages()
{
    std::error_code ec;
    auto userDir = Editor::userConfigDir() / "languages";
    auto isLang = [](const std::filesystem::path &p) {
        auto x = p.extension().string();
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        return x == ".lang";
    };
    if (std::filesystem::is_directory(userDir, ec))
        for (const auto &e : std::filesystem::directory_iterator(userDir, ec))
            if (e.is_regular_file() && isLang(e.path()))
                return; // already populated — leave it alone

    const std::filesystem::path roots[] = {
        get_module_path().parent_path() / "languages",
        std::filesystem::current_path() / "languages",
        std::filesystem::current_path() / ".." / "languages",
        std::filesystem::current_path() / ".." / ".." / "languages",
        std::filesystem::current_path() / ".." / ".." / ".." / "example" / "languages",
    };
    for (const auto &src : roots)
    {
        if (!std::filesystem::is_directory(src, ec))
            continue;
        // Count bundled .lang PRESENT in this root — that (not copy success) is what
        // selects the root. Tying the break to a copy result was wrong: skip_existing
        // returns false on a benign already-exists, and a real copy failure would
        // otherwise look like success. Copy stays best-effort (bundled set still loads
        // regardless; the Settings button opens the folder so emptiness is visible).
        int found = 0;
        for (const auto &e : std::filesystem::directory_iterator(src, ec))
        {
            if (!e.is_regular_file() || !isLang(e.path()))
                continue;
            ++found;
            std::filesystem::create_directories(userDir, ec);
            std::error_code cec;
            std::filesystem::copy_file(e.path(), userDir / e.path().filename(),
                                       std::filesystem::copy_options::skip_existing, cec);
        }
        if (found > 0)
            break; // located the bundled set; don't probe lower-priority roots
    }
}

void Editor::loadRuntimeLanguages()
{
    auto &byExt = runtimeLanguagesByExt();
    if (!byExt.empty())
        return; // already loaded
    seedUserLanguages(); // first run: drop editable copies of the bundled set in the user dir

    // Load every .lang in `root`, registering by extension (later calls override
    // earlier ones). Returns how many languages it added.
    auto loadDir = [&byExt](const std::filesystem::path &root) -> int {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            return 0;
        int n = 0;
        for (const auto &entry : std::filesystem::directory_iterator(root, ec))
        {
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".lang")
                continue;
            if (auto *lang = TextEditor::Language::FromFile(entry.path().string()))
            {
                for (const auto &e : lang->extensions)
                    byExt[e] = lang;
                ++n;
            }
        }
        return n;
    };

    // Bundled set: <exe-dir>/languages first (the installed/deployed location —
    // get_module_path() is the exe FILE, so take its parent), then dev-tree
    // CWD-relative fallbacks. Stop at the first root that actually has .lang files.
    const std::filesystem::path bundled[] = {
        get_module_path().parent_path() / "languages",
        std::filesystem::current_path() / "languages",
        std::filesystem::current_path() / ".." / "languages",
        std::filesystem::current_path() / ".." / ".." / "languages",
        std::filesystem::current_path() / ".." / ".." / ".." / "example" / "languages",
    };
    for (const auto &root : bundled)
        if (loadDir(root) > 0)
            break;

    // User languages in a WRITABLE per-user dir, loaded LAST so they add to or
    // override the bundled set. A Program Files install makes <exe-dir>/languages
    // read-only, so this is where users drop their own / edited .lang files.
    loadDir(userConfigDir() / "languages");
}

const TextEditor::Language *Editor::languageForPath(const std::string &path)
{
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Filename-based matches (extensionless files: CMakeLists.txt, Dockerfile, …)
    auto fname = std::filesystem::path(path).filename().string();
    std::string fnameLower = fname;
    std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (fnameLower == "cmakelists.txt")
    {
        auto &byExt = runtimeLanguagesByExt();
        auto it = byExt.find(".cmake");
        if (it != byExt.end())
            return it->second;
    }

    // User-set persistent association for this extension wins over the built-ins
    // (Settings [filetypes], set from the status-bar language picker). "None" maps
    // to nullptr = plain text, a valid choice.
    if (!ext.empty())
    {
        auto ov = extLanguageOverrides().find(ext);
        if (ov != extLanguageOverrides().end())
            return languageByName(ov->second);
    }

    // Built-in matches take precedence; fall back to user-loaded definitions.
    // `.h` maps to C++ (a superset of C): C code still highlights correctly, but
    // C++ headers stop losing class/namespace/template highlighting. Only `.c`
    // stays pure C.
    if (ext == ".c")
        return TextEditor::Language::C();
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
        ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".inl")
        return TextEditor::Language::Cpp();
    if (ext == ".cs")
        return TextEditor::Language::Cs();
    if (ext == ".as")
        return TextEditor::Language::AngelScript();
    if (ext == ".lua")
        return TextEditor::Language::Lua();
    if (ext == ".py" || ext == ".pyw")
        return TextEditor::Language::Python();
    if (ext == ".glsl" || ext == ".vert" || ext == ".frag" ||
        ext == ".geom" || ext == ".comp" || ext == ".tesc" || ext == ".tese")
        return TextEditor::Language::Glsl();
    if (ext == ".hlsl" || ext == ".hlsli" || ext == ".fx" || ext == ".fxh" || ext == ".addonfx")
        return TextEditor::Language::Hlsl();
    if (ext == ".json" || ext == ".jsonl" || ext == ".uplugin" || ext == ".uproject" || ext == ".gltf")
        return TextEditor::Language::Json();
    if (ext == ".md" || ext == ".markdown")
        return TextEditor::Language::Markdown();
    if (ext == ".sql")
        return TextEditor::Language::Sql();
    if (ext == ".ini" || ext == ".cfg" || ext == ".conf")
        return TextEditor::Language::Ini();

    // Runtime-defined languages (HTML, INI, YAML, CFG, BAT, PS1, etc.)
    auto &byExt = runtimeLanguagesByExt();
    auto it = byExt.find(ext);
    if (it != byExt.end())
        return it->second;
    return nullptr;
}

//
//  Editor::Editor
//

bool Editor::sSkipDemo = false;

Editor::Editor()
{
    // Load user-defined language definitions (HTML, INI, YAML, CFG, BAT, PS1).
    loadRuntimeLanguages();
    // Merge any on-disk symbol packs into the completion member table. Done here,
    // before the first document / render, so the registry is fully populated by the
    // time member completion queries it (no race with the read path).
    loadSymbolPacks();
    // Load editor-wide preferences (interpreter overrides, build commands, etc.)
    loadSettings();
    applyTheme(prefTheme); // restore the saved theme over main.cpp's default
    // If the user picked a custom font in a previous session, load it now into
    // the atlas so the first render uses it. Empty path = stick with bundled.
    if (!fontPath.empty())
        applyFont();

    // Construct the in-process plugins compiled into this build and let them
    // register (e.g. augment the shared C++ language) BEFORE the first document
    // — the demo tab below opens as C++. enabled() reads flags loaded above.
    registerBuiltinPlugins(pluginRegistry);
    // Feature plugins packaged as DLLs under <exe>/plugins are loaded at runtime,
    // so a lean core exe can ship without them and gain them by dropping in a DLL.
    // They join the same registry as any compiled-in plugins, before registerAll
    // applies persisted enable flags and runs onRegister. Results go to both stderr
    // (dev console) and <config>/plugins.log so a GUI launch leaves a trace to
    // diagnose a plugin that didn't load.
    {
        std::error_code ec;
        std::filesystem::create_directories(userConfigDir(), ec);
        std::ofstream pluginLog(userConfigDir() / "plugins.log", std::ios::trunc);
        loadPluginDLLs(pluginRegistry, hostExeDir() / "plugins",
                       [&pluginLog](const std::string &msg) {
                           std::fprintf(stderr, "%s\n", msg.c_str());
                           if (pluginLog)
                               pluginLog << msg << "\n";
                       });
    }
    pluginRegistry.registerAll(*this);

    // Skip the demo when this is a second-or-later launch (settings file
    // already exists) OR when main.cpp told us to (via --project or
    // positional file arguments). On a true first run, fall through to the
    // demo so the user has something to look at.
    if (seenFirstRun || sSkipDemo)
    {
        auto &t = newTab();
        (void)t;
        return;
    }

    auto &t = newTab();
    t.originalText = demo;
    t.editor.SetText(demo);
    t.editor.SetLanguage(TextEditor::Language::Cpp());
    t.version = t.editor.GetUndoIndex();
    t.filename = "untitled";
    // Populate the trie up front so context-menu items like
    // "Go to Definition" can gate themselves on `trie.contains(word)` even
    // when autocomplete is disabled. Cheap for short docs.
    buildAutocompleteTrie(t);
}

//
//  Editor::newTab
//

Editor::TabDocument &Editor::newTab()
{
    auto t = std::make_unique<TabDocument>();
    t->id = nextId++;
    t->filename = "untitled";
    t->open = true;
    t->wantFocus = true;
    t->version = t->editor.GetUndoIndex();
    // Apply current editor prefs so toggles from Settings persist into new tabs.
    t->editor.SetAutoIndentEnabled(prefAutoIndent);
    t->editor.SetCompletePairedGlyphs(prefCompletePairs);
    t->editor.SetPanInverted(prefInvertPan);
    t->editor.SetPanScrollAccel(prefPanScrollAccel);
    t->diff.SetPanInverted(prefInvertPan); // the diff view honors the same scroll prefs
    t->diff.SetPanScrollAccel(prefPanScrollAccel);
    t->editor.SetWordWrap(prefWordWrap);
    t->editor.SetWrapWidth(static_cast<float>(prefWrapWidthPx));
    if (focusMode)
        t->editor.SetShowScrollbarMiniMapEnabled(false); // honor focus mode for tabs opened while focused
    applyKeybindOverridesToEditor(t->editor);            // user keybind remaps into this editor
    tabs.push_back(std::move(t));
    activeTab = tabs.size() - 1;
    // Configure autocomplete for ONLY the new tab. Do NOT call
    // setAutocompleteMode() here: it rebuilds EVERY open tab's trie
    // (re-scanning each doc's identifiers + re-inserting the whole project
    // index), which produced a multi-second stall when adding a tab while a
    // large file like imgui.cpp was open. Other tabs' tries are already built
    // and unchanged, so only the brand-new tab needs setup.
    if (autocomplete)
        configureTabAutocomplete(*tabs.back());
    return *tabs.back();
}

//
//  Editor::openLuaInNewTab — PluginHost::hostOpenLuaTab. Open arbitrary text as
//  a new, never-saved Lua editor tab (used by the UEVR plugin's "Generate UEVR
//  Lua"). Untitled → Ctrl+S prompts Save As, which is correct for generated
//  content.
//
void Editor::openLuaInNewTab(const std::string &text)
{
    auto &t = newTab();
    t.editor.SetText(text);
    t.editor.SetLanguage(TextEditor::Language::Lua());
    t.wantFocus = true;
    // originalText stays empty, so the tab reads as unsaved (dirty) — expected
    // for freshly generated code the user hasn't chosen a path for yet.
    buildAutocompleteTrie(t);
}

Editor::TabDocument &Editor::newTab(const std::string &path, bool split, int index)
{
    auto t = std::make_unique<TabDocument>();
    t->id = nextId++;
    t->filename = path;
    t->open = true;
    t->wantFocus = true;
    t->wantSplit = split;
    t->version = t->editor.GetUndoIndex();

    size_t insertedAt = 0;
    if (index >= 0 && static_cast<size_t>(index) <= tabs.size())
    {
        auto it = tabs.emplace(tabs.begin() + index, std::move(t));
        insertedAt = static_cast<size_t>(it - tabs.begin());
    }
    else
    {
        tabs.push_back(std::move(t));
        insertedAt = tabs.size() - 1;
    }
    activeTab = insertedAt;
    applyKeybindOverridesToEditor(tabs[insertedAt]->editor); // user keybind remaps into this editor
    if (autocomplete)
        configureTabAutocomplete(*tabs[insertedAt]);
    return *tabs[insertedAt];
}

//
//  Editor::closeTab
//

void Editor::closeTab(size_t idx)
{
    if (idx >= tabs.size())
        return;

    // Tell clangd the document is gone.
    if (lspClient.spawned())
    {
        std::string uri = lspUriForTab(*tabs[idx]);
        if (!uri.empty())
        {
            lspClient.didClose(uri);
            lspDiagnostics.erase(lsp::uriToPath(uri)); // don't leak this file's diagnostics
        }
        lspDocHash.erase(tabs[idx]->id);
    }

    // Remember this tab so Ctrl+Shift+T can bring it back. Skip empty untitled.
    {
        const auto &t = *tabs[idx];
        std::string txt = t.editor.GetText();
        if (!(t.filename == "untitled" && txt.empty()))
        {
            recentlyClosed.push_back({t.filename, std::move(txt)});
            if (recentlyClosed.size() > 32)
                recentlyClosed.erase(recentlyClosed.begin());
        }
    }

    if (tabs.size() <= 1)
    {
        auto &t = *tabs[0];
        t.originalText.clear();
        t.editor.SetText("");
        t.editor.SetLanguage(nullptr);
        t.version = t.editor.GetUndoIndex();
        t.filename = "untitled";
        t.open = true;
        activeTab = 0;
        return;
    }
    tabs.erase(tabs.begin() + static_cast<std::ptrdiff_t>(idx));
    if (activeTab >= tabs.size())
        activeTab = tabs.size() - 1;
    else if (activeTab > idx)
        activeTab -= 1;
}

// ── Script runner ─────────────────────────────────────────────────────
// Map file extension → interpreter command. Adjust by editing here or via
// runtimeLanguages later. Output is captured into `script->output` and shown
// in a docked Output window. Synchronous — fine for small scripts; large
// outputs may block briefly.
// Returns an interpreter command prefix, or "" if the file should be invoked
// directly (the OS shell knows how to handle the extension). nullptr means
// "no interpreter mapped — don't run".
static const char *interpreterForExt(const std::string &ext)
{
    if (ext == ".py" || ext == ".pyw")
        return "python";
    if (ext == ".ps1")
        return "powershell -NoProfile -ExecutionPolicy Bypass -File";
    // .bat / .cmd: _popen on Windows already pipes through cmd.exe /c, so
    // passing the path directly is enough. Wrapping it in another `cmd /c`
    // caused the quote-collapsing rules to mangle paths with spaces and
    // could crash the spawned shell on some setups.
    if (ext == ".bat" || ext == ".cmd")
        return "";
    if (ext == ".sh")
        return "bash";
    if (ext == ".lua")
        return "lua";
    if (ext == ".js")
        return "node";
    return nullptr;
}

// Static wrapper expected by editor.h; runs the active doc's script.
void Editor::runScriptForActiveDoc()
{
}

void Editor::renderScriptOutputWindow()
{
    if (!script->visible)
        return;
    ImGui::SetNextWindowSize(ImVec2(600, 200), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Output###outputPanel", &script->visible))
    {
        if (ImGui::SmallButton("Clear"))
        {
            std::lock_guard<std::mutex> lock(script->mutex);
            script->output.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy"))
        {
            std::string snap;
            {
                std::lock_guard<std::mutex> lock(script->mutex);
                snap = script->output;
            }
            ImGui::SetClipboardText(snap.c_str());
        }
        ImGui::SameLine();
        if (script->running.load())
            ImGui::TextDisabled("(running…)");
        else
            ImGui::TextDisabled("(F5 to re-run)");
        ImGui::Separator();
        // CLAUDE make the text split into lines and add line selection and text copying
        ImGui::BeginChild("##outputScroll", ImVec2(0, 0), 0,
                          ImGuiWindowFlags_HorizontalScrollbar |
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
        // Snapshot under the lock so the worker thread can keep appending
        // while we draw (worst case the next frame catches up).
        std::string snapshot;
        {
            std::lock_guard<std::mutex> lock(script->mutex);
            snapshot = script->output;
        }
        ImGui::TextUnformatted(snapshot.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        middleMousePanScroll(3); // output log
        ImGui::EndChild();
    }
    ImGui::End();
}

// Spawn the active document's interpreter on a background thread, stream
// stdout+stderr into `script->output`. Detached — multiple presses of F5 in
// quick succession will queue threads, only the latest one's output wins
// (older ones drop their writes when they discover `script->gen` has moved).
// Walk up from a starting directory looking for the first build entry-point
// — convention-based discovery so each project just drops a script in its root.
//
// On Windows we prefer `build.bat`/`build.cmd` first (most projects ship one),
// then `build.ps1`, then a Makefile / CMakeLists.txt at the same level. The
// search is capped at 8 levels up to avoid recursing into the filesystem root.
std::pair<std::filesystem::path, std::string> Editor::findProjectBuildCommand(std::filesystem::path start)
{
    std::error_code ec;
    struct Candidate
    {
        const char *name;
        const char *interp;
    };
    static const Candidate cands[] = {
#ifdef _WIN32
        {"build.bat", ""}, // direct invoke via cmd.exe
        {"build.cmd", ""},
        {"build.ps1", "powershell -NoProfile -ExecutionPolicy Bypass -File"},
#else
        {"build.sh", "bash"},
#endif
        {"Makefile", "make"},
        // CMakeLists.txt isn't directly runnable but tells us the project root
        // — fall back to invoking `cmake --build .` from here.
    };

    auto cur = start;
    for (int level = 0; level < 8; ++level)
    {
        for (auto &c : cands)
        {
            auto p = cur / c.name;
            if (std::filesystem::is_regular_file(p, ec))
            {
                return {p, c.interp};
            }
        }

        // A project-type plugin (e.g. Unreal) claims this level first: UE
        // projects also carry a GENERATED .sln that `dotnet build` can't build,
        // so the plugin's build command must take precedence over .sln here.
        if (auto pc = pluginRegistry.buildCommand(cur))
            return {pc->path, pc->command};

        // Project files at this level — scan once, pick the most specific.
        std::filesystem::path sln, csproj, vcxproj;
        for (auto &entry : std::filesystem::directory_iterator(cur, ec))
        {
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (ext == ".sln" && sln.empty())
                sln = entry.path();
            else if (ext == ".csproj" && csproj.empty())
                csproj = entry.path();
            else if (ext == ".vcxproj" && vcxproj.empty())
                vcxproj = entry.path();
        }
        // Prefer .sln (covers C# + C++ multi-project), then .csproj, then .vcxproj.
        // dotnet's CLI handles .sln, .csproj transparently; vcxproj needs msbuild.
        if (!sln.empty())
            return {sln, "dotnet build"};
        if (!csproj.empty())
            return {csproj, "dotnet build"};
        if (!vcxproj.empty())
            return {vcxproj, "msbuild"};

        // CMakeLists.txt → `cmake --build <buildDir>` if we can find one.
        if (std::filesystem::is_regular_file(cur / "CMakeLists.txt", ec))
        {
            for (const char *sub : {"build", "out/build/x64-Debug", "out/build"})
            {
                auto bd = cur / sub;
                if (std::filesystem::is_directory(bd, ec))
                {
                    return {bd, "cmake --build ."};
                }
            }
            return {cur, "cmake -B build && cmake --build build"};
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return {{}, {}};
}

// Single home for the run pipe loop. Spawns `cmd` (cd'd to runDir when set) on
// a detached thread, captures stdout+stderr into the Output panel. The shared_ptr
// is captured BY VALUE so the ScriptState outlives the Editor if the user quits
// while a script is still blocked in fread. A newer gen supersedes older threads.
void Editor::runCommandInOutputPanel(const std::string &cmd, const std::filesystem::path &runDir)
{
    std::string rd = runDir.empty() ? std::string() : runDir.string();
    int gen = ++script->gen;
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->output = rd.empty() ? ("$ " + cmd + "\n")
                                    : ("$ cd " + rd + " && " + cmd + "\n");
        script->visible = true;
    }
    script->running = true;

    auto scriptCtx = script;
    std::thread([scriptCtx, cmd, rd, gen]() {
        std::string full;
#ifdef _WIN32
        full = rd.empty()
                   ? ("\"< NUL " + cmd + " 2>&1\"")
                   : ("\"< NUL pushd \"" + rd + "\" && " + cmd + " 2>&1 & popd\"");
        FILE *pipe = _popen(full.c_str(), "r");
#else
        full = rd.empty()
                   ? ("< /dev/null " + cmd + " 2>&1")
                   : ("cd \"" + rd + "\" && < /dev/null " + cmd + " 2>&1");
        FILE *pipe = popen(full.c_str(), "r");
#endif
        if (!pipe)
        {
            std::lock_guard<std::mutex> lock(scriptCtx->mutex);
            if (gen == scriptCtx->gen.load())
                scriptCtx->output += "[failed to spawn]\n";
            scriptCtx->running = false;
            return;
        }
        char buf[4096];
        bool truncated = false;
        while (size_t n = fread(buf, 1, sizeof(buf), pipe))
        {
            std::lock_guard<std::mutex> lock(scriptCtx->mutex);
            if (gen != scriptCtx->gen.load())
                break; // superseded by a newer run
            scriptCtx->output.append(buf, n);
            if (scriptCtx->output.size() > (8u << 20))
            {
                scriptCtx->output.append("\n[…truncated after 8 MB…]\n");
                truncated = true;
                break;
            }
        }
        int rc =
#ifdef _WIN32
            _pclose(pipe);
#else
            pclose(pipe);
#endif
        std::lock_guard<std::mutex> lock(scriptCtx->mutex);
        if (gen == scriptCtx->gen.load() && !truncated)
            scriptCtx->output += "\n[exit " + std::to_string(rc) + "]\n";
        scriptCtx->running = false; })
        .detach();
}

// PluginHost::hostExeDir — the directory the executable lives in (for locating
// bundled assets like ue-plugins/). get_module_path() is file-local here.
std::filesystem::path Editor::hostExeDir() const
{
    return get_module_path().parent_path();
}

// PluginHost::hostAugmentCppLanguage — apply a plugin's UE-style vocabulary to the
// editor's single shared C++ Language. Runs in the exe, which owns the real
// TextEditor::Language statics, so DLL plugins (with their own static TextEditor)
// still colour the same definition the editor renders with.
void Editor::hostAugmentCppLanguage(const std::vector<std::string> &types,
                                    const std::vector<std::string> &keywords,
                                    bool (*isTypeLike)(const std::string &))
{
    TextEditor::Language *cpp = TextEditor::Language::CppMutable();
    if (!cpp)
        return;
    for (auto &t : types)
        cpp->declarations.insert(t);
    for (auto &k : keywords)
        cpp->keywords.insert(k);
    if (isTypeLike)
        cpp->isTypeLike = isTypeLike;
}

void Editor::runProjectBuild()
{
    // A pinned per-project choice (Build/Run Targets picker, "Set F6") wins
    // outright — "cmd|dir", dir optional (defaults to the project root).
    if (!projectRoot.empty())
    {
        auto it = projectBuildOverrides.find(dbgProjectKey());
        if (it == projectBuildOverrides.end())
            it = projectBuildOverrides.find(projectRoot.string()); // legacy key
        if (it != projectBuildOverrides.end() && !it->second.empty())
        {
            auto bar = it->second.find('|');
            std::string cmd = it->second.substr(0, bar);
            std::filesystem::path dir = bar == std::string::npos
                                            ? projectRoot
                                            : std::filesystem::path(it->second.substr(bar + 1));
            std::string token = "%CONFIG%";
            for (size_t pos = 0; (pos = cmd.find(token, pos)) != std::string::npos;)
            {
                cmd.replace(pos, token.size(), activeBuildConfig);
                pos += activeBuildConfig.size();
            }
            runCommandInOutputPanel(cmd, dir);
            return;
        }
    }

    std::filesystem::path startDir;
    if (!tabs.empty() && doc().filename != "untitled")
    {
        startDir = std::filesystem::path(doc().filename).parent_path();
    }
    else
    {
        startDir = std::filesystem::current_path();
    }

    auto [scriptPath, interp] = findProjectBuildCommand(startDir);
    if (scriptPath.empty())
    {
        // Don't give up with a message — show the user every candidate we CAN
        // see and let them pick (and pin) one.
        pushToast("No build target auto-detected — pick one", IM_COL32(240, 200, 90, 255));
        openBuildPicker();
        return;
    }

    // For Makefile / CMake the "script path" we found is a directory we should
    // `cd` to before running; for build.* it's the script file itself.
    std::filesystem::path runDir;
    std::string cmd;
    bool isFileScript = std::filesystem::is_regular_file(scriptPath);
    if (isFileScript)
    {
        runDir = scriptPath.parent_path();
        if (!interp.empty())
            cmd = interp + " \"" + scriptPath.string() + "\"";
        else
            cmd = "\"" + scriptPath.string() + "\"";
    }
    else
    {
        runDir = scriptPath;
        cmd = interp;
    }

    // Substitute %CONFIG% with the active build configuration so presets like
    // "cmake --build . --config %CONFIG%" pick up Debug / Release.
    {
        std::string token = "%CONFIG%";
        for (size_t pos = 0; (pos = cmd.find(token, pos)) != std::string::npos;)
        {
            cmd.replace(pos, token.size(), activeBuildConfig);
            pos += activeBuildConfig.size();
        }
    }

    runCommandInOutputPanel(cmd, runDir);
}

// Walk projectRoot looking for a built executable under the usual build
// dirs. Returns the newest exe so a fresh `cmake --build` is picked up.
std::filesystem::path Editor::findBuiltExe() const
{
    if (projectRoot.empty())
        return {};
    std::vector<std::filesystem::path> searchRoots = {
        projectRoot / "out" / "build",
        projectRoot / "build",
        projectRoot / "bin",
        projectRoot / "x64",
        projectRoot,
    };
    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    std::error_code ec;
    for (auto &root : searchRoots)
    {
        if (!std::filesystem::exists(root, ec))
            continue;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file(ec))
                continue;
            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
#ifdef _WIN32
            if (ext != ".exe")
                continue;
#else
            if (ext == ".so" || ext == ".dylib")
                continue;
            // On POSIX, we look for files with no extension that are executable.
            // (Naive — good enough for common CMake / Ninja output layouts.)
            if (!ext.empty())
                continue;
#endif
            auto t = std::filesystem::last_write_time(it->path(), ec);
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (best.empty() || t > bestTime)
            {
                best = it->path();
                bestTime = t;
            }
        }
    }
    return best;
}

// Resolve the active document to a runnable command. Saves a dirty buffer
// first (so the run reflects on-disk state). Returns {command, workingDir};
// the working dir is the script's own folder so relative paths in the script
// resolve. On failure returns {empty, _} with the reason in the Output panel.
std::string Editor::venvPythonFor(const std::filesystem::path &scriptPath) const
{
    std::error_code ec;
    auto check = [&](const std::filesystem::path &venvDir) -> std::string {
#ifdef _WIN32
        auto py = venvDir / "Scripts" / "python.exe";
#else
        auto py = venvDir / "bin" / "python";
#endif
        return std::filesystem::is_regular_file(py, ec) ? py.string() : std::string();
    };
    // 1. An already-activated virtualenv.
    if (const char *ve = std::getenv("VIRTUAL_ENV"))
    {
        auto p = check(ve);
        if (!p.empty())
            return p;
    }
    // 2. A .venv/venv/env dir, walking up from the script's dir then the project root.
    std::vector<std::filesystem::path> bases;
    if (!scriptPath.empty())
        bases.push_back(scriptPath.parent_path());
    if (!projectRoot.empty())
        bases.push_back(projectRoot);
    for (const auto &base : bases)
    {
        auto cur = base;
        for (int i = 0; i < 6; ++i)
        {
            for (const char *name : {".venv", "venv", "env"})
            {
                auto p = check(cur / name);
                if (!p.empty())
                    return p;
            }
            if (!cur.has_parent_path() || cur.parent_path() == cur)
                break;
            cur = cur.parent_path();
        }
    }
    return {};
}

std::pair<std::string, std::filesystem::path> Editor::docScriptCommand()
{
    if (tabs.empty())
        return {};
    auto &t = doc();
    if (t.filename == "untitled")
        return {};

    auto ext = std::filesystem::path(t.filename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Settings interpreter override takes precedence over the built-in
    // per-extension defaults so the user can swap python -> python3, etc.
    std::string interpStr;
    auto over = interpreterOverrides.find(ext);
    if (over != interpreterOverrides.end())
    {
        interpStr = over->second;
    }
    else if (const char *def = interpreterForExt(ext))
    {
        interpStr = def;
    }
    else
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->output = "[no interpreter mapped for " + ext + "]\n";
        script->visible = true;
        return {};
    }

    // Python: when the interpreter is the built-in default (no explicit override),
    // prefer a project virtualenv's python so the script runs with the venv's
    // packages — what the user almost always wants when a venv exists.
    if (over == interpreterOverrides.end() && (ext == ".py" || ext == ".pyw"))
    {
        std::string venv = venvPythonFor(std::filesystem::path(t.filename));
        if (!venv.empty())
            interpStr = "\"" + venv + "\"";
    }

    if (isDirty())
        saveFile();

    std::string cmd = interpStr.empty()
                          ? ("\"" + t.filename + "\"")
                          : (interpStr + " \"" + t.filename + "\"");
    return {cmd, std::filesystem::path(t.filename).parent_path()};
}

// What F5 runs: the freshest built exe under projectRoot if there is one, else
// the active document treated as a script.
std::pair<std::string, std::filesystem::path> Editor::projectRunCommand()
{
    if (!projectRoot.empty())
    {
        // A pinned F5 target (Build/Run Targets picker) wins — "cmd|dir".
        auto it = projectRunOverrides.find(dbgProjectKey());
        if (it != projectRunOverrides.end() && !it->second.empty())
        {
            auto bar = it->second.find('|');
            std::string cmd = it->second.substr(0, bar);
            std::filesystem::path dir = bar == std::string::npos
                                            ? projectRoot
                                            : std::filesystem::path(it->second.substr(bar + 1));
            return {cmd, dir};
        }
        auto exe = findBuiltExe();
        if (!exe.empty())
            return {"\"" + exe.string() + "\"", exe.parent_path()};
    }
    return docScriptCommand();
}

void Editor::runProjectExeOrScript()
{
    auto [cmd, dir] = projectRunCommand();
    if (!cmd.empty())
        runCommandInOutputPanel(cmd, dir);
}

void Editor::runProjectWithArgs()
{
    auto [cmd, dir] = projectRunCommand();
    if (!cmd.empty())
        requestRunWithArgs(cmd, dir);
}

void Editor::runScriptForDoc()
{
    auto [cmd, dir] = docScriptCommand();
    if (!cmd.empty())
        runCommandInOutputPanel(cmd, dir);
}

// ── Run arbitrary nav-tree files (scripts / executables) ──────────────

bool Editor::navIsRunnable(const std::string &p) const
{
    auto ext = std::filesystem::path(p).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#ifdef _WIN32
    if (ext == ".exe" || ext == ".com" || ext == ".bat" || ext == ".cmd")
        return true;
#endif
    if (interpreterOverrides.count(ext))
        return true;                          // user-mapped interpreter
    return interpreterForExt(ext) != nullptr; // .py/.ps1/.sh/.lua/.js/...
}

std::pair<std::string, std::filesystem::path> Editor::runCommandForPath(const std::string &p) const
{
    std::filesystem::path path(p);
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string interp;
    auto over = interpreterOverrides.find(ext);
    if (over != interpreterOverrides.end())
        interp = over->second;
    else if (const char *def = interpreterForExt(ext))
        interp = def;
    // interp == "" → run the file directly (an .exe, or a .bat the shell handles).
    std::string cmd = interp.empty()
                          ? ("\"" + path.string() + "\"")
                          : (interp + " \"" + path.string() + "\"");
    return {cmd, path.parent_path()};
}

// ── Run with arguments — deferred-open modal ──────────────────────────

void Editor::requestRunWithArgs(const std::string &baseCmd, const std::filesystem::path &runDir)
{
    runArgsBaseCmd = baseCmd;
    runArgsDir = runDir;
    runArgsBuf[0] = '\0';
    runArgsRequested = true; // renderRunArgsPopup() opens the modal next frame
}

void Editor::renderRunArgsPopup()
{
    if (runArgsRequested)
    {
        ImGui::OpenPopup("Run with Arguments");
        runArgsRequested = false;
    }
    if (ImGui::BeginPopupModal("Run with Arguments", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("%s", runArgsBaseCmd.c_str());
        ImGui::SetNextItemWidth(440.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("Arguments", runArgsBuf, sizeof(runArgsBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
        if (ImGui::Button("Run") || enter)
        {
            // Args are appended verbatim — the shell (_popen → cmd.exe /c) does
            // its own quoting/splitting, which is exactly what a run box wants.
            std::string cmd = runArgsBaseCmd;
            if (runArgsBuf[0])
            {
                cmd += ' ';
                cmd += runArgsBuf;
            }
            runCommandInOutputPanel(cmd, runArgsDir);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Resolve the per-language brace pref (Allman/new-line = true) for a file ext.
bool Editor::formatBraceNewLineForExt(const std::string &ext) const
{
    if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".cxx" ||
        ext == ".hxx" || ext == ".cc" || ext == ".hh" || ext == ".inl" || ext == ".m" || ext == ".mm")
        return prefFormatBraceCpp;
    if (ext == ".cs")
        return prefFormatBraceCs;
    if (ext == ".js" || ext == ".jsx" || ext == ".ts" || ext == ".tsx" || ext == ".mjs" || ext == ".cjs")
        return prefFormatBraceJs;
    if (ext == ".java")
        return prefFormatBraceJava;
    return prefFormatBraceNewLine;
}

// Format the active document with clang-format (Microsoft base style = VS-like
// spacing; brace placement from the setting; ColumnLimit 0 so it fixes braces /
// indentation / spacing WITHOUT reflowing lines to a width). Undo-safe: the whole
// buffer is replaced via a single select-all + replace transaction, so Ctrl+Z
// reverts the format in one step. No-ops if clang-format isn't on PATH or the
// language isn't one it handles.
void Editor::formatActiveDocument()
{
    if (tabs.empty())
        return;
    auto &t = doc();

    std::string ext;
    if (t.filename == "untitled")
        ext = ".cpp"; // assume C++ for an unsaved buffer
    else
    {
        ext = std::filesystem::path(t.filename).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    }
    // Per-language formatter dispatch. clang-format handles the C-family + JS/TS/
    // proto; other languages delegate to their canonical CLI formatter (each
    // optional — a clear error if it isn't on PATH).
    enum class Tool
    {
        ClangFormat,
        Stylua,
        Black,
        Rustfmt,
        Gofmt,
        None
    };
    static const std::unordered_set<std::string> clangExt = {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".cxx",
        ".hxx",
        ".cc",
        ".hh",
        ".inl",
        ".m",
        ".mm",
        ".cs",
        ".java",
        ".js",
        ".jsx",
        ".ts",
        ".tsx",
        ".mjs",
        ".cjs",
        ".proto",
    };
    Tool tool = Tool::None;
    if (clangExt.count(ext))
        tool = Tool::ClangFormat;
    else if (ext == ".lua")
        tool = Tool::Stylua;
    else if (ext == ".py" || ext == ".pyw")
        tool = Tool::Black;
    else if (ext == ".rs")
        tool = Tool::Rustfmt;
    else if (ext == ".go")
        tool = Tool::Gofmt;
    if (tool == Tool::None)
    {
        showError("Format: no formatter configured for '" + ext + "' files.");
        return;
    }

    std::error_code ec;
    auto dir = userConfigDir() / "fmt";
    std::filesystem::create_directories(dir, ec);
    auto src = dir / ("buffer" + (ext.empty() ? std::string(".txt") : ext));
    {
        std::ofstream f(src, std::ios::binary | std::ios::trunc);
        f << t.editor.GetText();
    }

    // Run a command, capturing stdout + the shell exit code.
    std::string out;
    auto run = [&out](const std::string &cmd) -> int {
        out.clear();
        FILE *p = _popen(cmd.c_str(), "r");
        if (!p)
            return -1;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
            out.append(buf, n);
        return _pclose(p); // shell/tool exit code (non-zero = tool missing or failed)
    };

    std::string q = "\"" + src.string() + "\"";
    if (tool == Tool::ClangFormat)
    {
        std::ofstream cf(dir / ".clang-format", std::ios::trunc);
        cf << "BasedOnStyle: Microsoft\n"
           << "BreakBeforeBraces: " << (formatBraceNewLineForExt(ext) ? "Allman" : "Attach") << "\n"
           << "ColumnLimit: 0\n"      // fix structure/spacing without reflowing to a width
           << "SortIncludes: false\n"; // Microsoft style sorts #includes alphabetically, which
                                       // reorders order-dependent headers (Windows.h, precompiled
                                       // / first headers) and breaks the build. Use the boolean
                                       // `false` (not `Never`) — the enum values are clang-format
                                       // 13+, but `false` parses on 12 and 13+ alike.
        cf.close();
        int rc = run("clang-format " + q + " 2>nul");
        std::filesystem::remove(src, ec);
        if (rc != 0 || out.empty())
        {
            showError("Format: clang-format failed (on PATH? syntax error?).");
            return;
        }
    }
    else if (tool == Tool::Gofmt)
    {
        int rc = run("gofmt " + q + " 2>nul"); // gofmt writes to stdout
        std::filesystem::remove(src, ec);
        if (rc != 0 || out.empty())
        {
            showError("Format: gofmt failed (is it on PATH?).");
            return;
        }
    }
    else
    {
        // stylua / black / rustfmt format the file IN PLACE, so stdout isn't the
        // result — capture stderr too and surface the tool's ACTUAL message (line/
        // column of a syntax error) instead of a generic guess.
        const char *name = (tool == Tool::Stylua) ? "stylua" : (tool == Tool::Black) ? "black"
                                                                                     : "rustfmt";
        std::string cmd = (tool == Tool::Stylua)  ? "stylua " + q + " 2>&1"
                          : (tool == Tool::Black) ? "black -q " + q + " 2>&1"
                                                  : "rustfmt " + q + " 2>&1";
        int rc = run(cmd);
        if (rc != 0)
        {
            std::filesystem::remove(src, ec);
            if (out.find("is not recognized") != std::string::npos || out.find("cannot find") != std::string::npos)
                showError(std::string("Format: ") + name + " is not installed (not found on PATH).");
            else
            {
                // first few lines of the tool's own diagnostics (path prefix trimmed)
                std::string detail = out.substr(0, out.find('\0'));
                size_t cut = 0, lines = 0;
                while (cut < detail.size() && lines < 4)
                    if (detail[cut++] == '\n')
                        lines++;
                detail = detail.substr(0, cut);
                while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r'))
                    detail.pop_back();
                showError(std::string("Format: ") + name + " rejected the document:\n\n" +
                          (detail.empty() ? "(no diagnostics reported)" : detail));
            }
            return;
        }
        std::ifstream rf(src, std::ios::binary);
        std::stringstream ss;
        ss << rf.rdbuf();
        out = ss.str();
        std::filesystem::remove(src, ec);
        if (out.empty())
        {
            showError(std::string("Format: ") + name + " produced no output.");
            return;
        }
    }
    // Normalise CRLF -> LF (the editor stores LF internally).
    if (out.find('\r') != std::string::npos)
    {
        std::string s;
        s.reserve(out.size());
        for (char c : out)
            if (c != '\r')
                s += c;
        out.swap(s);
    }
    // Drop a single trailing newline so we don't grow the file each format.
    if (!out.empty() && out.back() == '\n')
        out.pop_back();

    if (out == t.editor.GetText())
        return; // already formatted — no-op (no undo churn)

    int line = 0, col = 0;
    t.editor.GetCursor(line, col, 0);
    t.editor.SelectAll();
    t.editor.ReplaceTextInCurrentCursor(out); // one undoable transaction
    t.editor.SetCursor(line, 0);
    t.editor.ScrollToLine(line, TextEditor::Scroll::alignMiddle);
}

void Editor::toggleHeaderSource()
{
    if (tabs.empty())
        return;
    std::filesystem::path p(doc().filename);
    if (!p.has_extension())
        return;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Candidate extensions for the "other side"
    std::vector<std::string> candidates;
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh")
    {
        candidates = {".cpp", ".cc", ".cxx", ".c", ".m", ".mm", ".inl"};
    }
    else if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
             ext == ".m" || ext == ".mm" || ext == ".inl")
    {
        candidates = {".h", ".hpp", ".hxx", ".hh"};
    }
    else
    {
        return;
    }

    auto stem = p.parent_path() / p.stem();
    for (const auto &candExt : candidates)
    {
        auto candidate = stem;
        candidate += candExt;
        // 1. Already open? Just focus its tab.
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            if (std::filesystem::path(tabs[i]->filename) == candidate)
            {
                activeTab = i;
                tabs[i]->wantFocus = true;
                return;
            }
        }
        // 2. Exists on disk next to the source? Open it.
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
        {
            openFile(candidate.string());
            return;
        }
    }

    // 3. Project-tree fallback — handles include/ vs src/ splits (the common
    // case the same-dir search misses). Search the project root (or the doc's
    // repo root) for <stem><candidateExt>; pick the match whose path shares the
    // longest prefix with the source file (nearest sibling tree wins).
    {
        std::filesystem::path searchRoot = projectRoot;
        if (searchRoot.empty())
            searchRoot = resolveOutermostRoot(p.parent_path());
        std::error_code ec;
        std::unordered_set<std::string> wanted;
        for (auto &candExt : candidates)
            wanted.insert(p.stem().string() + candExt);

        std::filesystem::path best;
        size_t bestCommon = 0;
        auto srcStr = p.generic_string();
        int budget = 40000;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 searchRoot, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (it->is_directory(ec))
            {
                auto n = it->path().filename().string();
                if (n == ".git" || n == "node_modules" || n == "out" || n == "build" ||
                    n == "obj" || n == "bin" || n == ".vs")
                    it.disable_recursion_pending();
                continue;
            }
            if (--budget < 0)
                break;
            if (!wanted.count(it->path().filename().string()))
                continue;
            // Longest common path prefix with the source = nearest relative.
            auto cand = it->path().generic_string();
            size_t common = 0;
            while (common < cand.size() && common < srcStr.size() && cand[common] == srcStr[common])
                ++common;
            if (best.empty() || common > bestCommon)
            {
                best = it->path();
                bestCommon = common;
            }
        }
        if (!best.empty())
        {
            // Re-check open tabs for the resolved path before opening.
            for (size_t i = 0; i < tabs.size(); ++i)
            {
                if (std::filesystem::path(tabs[i]->filename) == best)
                {
                    activeTab = i;
                    tabs[i]->wantFocus = true;
                    return;
                }
            }
            openFile(best.string());
            return;
        }
    }
    // 4. Nothing found — silently no-op.
}

// ── Navigation panel ─────────────────────────────────────────────────
//
// Dockable file-tree view of the project root. Lazy directory iteration:
// folder children are only enumerated when the user expands the node, so
// huge project trees don't stall startup.

// Snapshot the currently-open file tabs (skip untitled / dirty) into
// projectSessions, keyed by the current projectRoot. Idempotent — overwrites
// the previous snapshot for this root.
void Editor::saveCurrentProjectSession()
{
    if (projectRoot.empty())
        return;
    std::vector<std::string> files;
    for (auto &up : tabs)
    {
        auto &t = *up;
        if (t.filename == "untitled")
            continue;
        files.push_back(t.filename);
    }
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(projectRoot, ec);
    projectSessions[(ec ? projectRoot : canon).string()] = std::move(files);
}

// Close every non-untitled tab. Dirty tabs are kept so the user doesn't lose
// unsaved work when switching projects — they'll just remain open in the new
// project view until the user closes / saves them explicitly.
void Editor::closeAllProjectTabs()
{
    for (size_t i = tabs.size(); i-- > 0;)
    {
        auto &t = *tabs[i];
        if (t.filename == "untitled")
            continue;
        if (isDirtyTab(i))
            continue;
        closeTab(i);
    }
}

// Open files from the saved session for `root` (if any).
void Editor::restoreProjectSession(const std::filesystem::path &root)
{
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(root, ec);
    auto it = projectSessions.find((ec ? root : canon).string());
    if (it == projectSessions.end())
        return;
    for (auto &f : it->second)
    {
        std::error_code fec;
        if (std::filesystem::exists(f, fec))
            openFile(f);
    }
}

// Walk up from a path to the outermost enclosing repository root, so opening a
// project file buried inside a larger tree (e.g. imgui/examples/foo) roots the
// nav panel at the whole project (imgui/) where its local dependencies live.
// Strategy: the outermost ancestor that contains a `.git` entry; failing that,
// the highest ancestor still holding a solution/workspace-level marker; else the
// folder itself.
static std::filesystem::path resolveOutermostRoot(std::filesystem::path start)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(start, ec))
        start = start.parent_path();
    std::filesystem::path best = start;
    std::filesystem::path cur = start;
    for (int i = 0; i < 64; ++i)
    {
        // Outermost wins: keep overwriting `best` as we climb past each marker.
        if (std::filesystem::exists(cur / ".git", ec) ||
            std::filesystem::exists(cur / ".hg", ec) ||
            std::filesystem::exists(cur / ".svn", ec))
            best = cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return best;
}

void Editor::setProjectRoot(const std::filesystem::path &p)
{
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec || abs.empty())
        return;
    // Respect exactly what the user opened: a folder is used as-is, a project
    // file resolves to its containing folder. (Earlier we climbed to the
    // outermost git root, but that over-reached on monorepos — e.g. opening
    // uevr-frontend/uevr jumped up to the whole uevr-frontend tree.)
    if (std::filesystem::is_regular_file(abs, ec))
        abs = abs.parent_path();
    // If we're switching projects, snapshot the outgoing project's open tabs
    // so reopening that project later restores the workspace.
    if (!projectRoot.empty())
    {
        auto oldCanon = std::filesystem::weakly_canonical(projectRoot, ec);
        auto newCanon = std::filesystem::weakly_canonical(abs, ec);
        if (oldCanon != newCanon)
        {
            saveCurrentProjectSession();
            closeAllProjectTabs();
        }
    }
    projectRoot = abs;
    navPanelVisible = true;
    rememberRecentProject(abs.string());
    loadNotes();   // sticky notes are per-project (sidecar at the new root)
#ifdef _WIN32
    // Persist the last project path to HKCU so external tools (and a "reopen last
    // project" flow) can find where we last were.
    {
        HKEY key = nullptr;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\ImGui-IDE", 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
        {
            std::string s = abs.string();
            RegSetValueExA(key, "LastProject", 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(s.c_str()),
                           static_cast<DWORD>(s.size() + 1));
            RegCloseKey(key);
        }
    }
#endif
    restoreProjectSession(abs);
    // Load the cached symbol index so it's usable instantly, then kick off the
    // background rebuild (which reuses the cache to skip unchanged files).
    loadIndexCache();
    rebuildProjectIndex();
    // Spawn clangd for this project (real C/C++ completion; ts is the fallback).
    startLspForProject();
    // Persist immediately so a crash/quit doesn't lose the session.
    saveSettings();
}

void Editor::openProjectFolderPicker()
{
    // Re-use the file dialog. Accepting either a folder OR a project file
    // (.sln/.csproj/.vcxproj/.uproject/CMakeLists.txt). Project files are
    // resolved to their parent directory below in the dialog-close path.
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    IGFD::FileDialogConfig config;
    config.countSelectionMax = 1;
    // OptionalFileName lets the user navigate into a folder and just hit
    // "Open" without selecting any file — that's how you pick a plain folder
    // as the project root. Without this, the dialog refuses to validate
    // unless a file is highlighted.
    config.path = dialogStartDir();
    config.flags = ImGuiFileDialogFlags_DontShowHiddenFiles | ImGuiFileDialogFlags_OptionalFileName;
    populateFileDialogPlaces();
    // Filter spec is OR-of-extensions plus a "Any folder" pass-through.
    const char *filter = "Project ({.sln,.csproj,.vcxproj,.uproject,.uplugin}){.sln,.csproj,.vcxproj,.uproject,.uplugin},CMakeLists.txt{CMakeLists.txt},All{.*}";
    ImGuiFileDialog::Instance()->OpenDialog("project-open", "Open Project (folder or project file)...", filter, config);
    state = State::openProject;
}

void Editor::openAddSourceLocationPicker()
{
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    IGFD::FileDialogConfig config;
    config.countSelectionMax = 1;
    config.path = dialogStartDir();
    config.flags = ImGuiFileDialogFlags_DontShowHiddenFiles | ImGuiFileDialogFlags_OptionalFileName;
    populateFileDialogPlaces();
    ImGuiFileDialog::Instance()->OpenDialog("add-source-loc", "Add Source Location (folder to index)...",
                                            nullptr, config); // nullptr filter = folders only
    state = State::addSourceLoc;
}

void Editor::addSourceLocation(const std::string &path)
{
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_directory(path, ec))
        return;
    auto canon = std::filesystem::weakly_canonical(path, ec);
    std::string key = ec ? path : canon.string();
    while (key.size() > 1 && (key.back() == '\\' || key.back() == '/'))
        key.pop_back(); // trailing-separator spellings must not create a second entry
    // De-dupe, case-insensitively on Windows (the same dir reached via a
    // different case/spelling must not become a second entry) — and don't
    // re-add the project root itself.
    for (auto &e : extraSourceLocations)
    {
#ifdef _WIN32
        if (_stricmp(e.c_str(), key.c_str()) == 0)
            return;
#else
        if (e == key)
            return;
#endif
    }
    if (!projectRoot.empty())
    {
        auto pc = std::filesystem::weakly_canonical(projectRoot, ec);
        if (!ec && pc.string() == key)
            return;
    }
    extraSourceLocations.push_back(key);
    saveSettings();
    navMarkListDirty();
    rebuildProjectIndex(); // pick the new root up in go-to-def + autocomplete
}

void Editor::removeSourceLocation(const std::string &path)
{
    auto it = std::find(extraSourceLocations.begin(), extraSourceLocations.end(), path);
    if (it == extraSourceLocations.end())
        return;
    extraSourceLocations.erase(it);
    saveSettings();
    navMarkListDirty();
    rebuildProjectIndex();
}

bool Editor::navIsExcluded(const std::filesystem::path &p) const
{
    if (navExcluded.empty())
        return false; // hot path: called per visible row per frame
    // lexically_normal (pure string), NOT weakly_canonical (a filesystem syscall):
    // canonicalizing every row every frame dropped the nav to ~1 fps on big dirs.
    auto it = navExcluded.find(p.lexically_normal().string());
    return it != navExcluded.end() && it->second;
}

// ── Nav multi-select (canonical-path keyed, same as exclusion) ──
static std::string navCanonKey(const std::filesystem::path &p)
{
    // lexically_normal (pure string) not weakly_canonical (filesystem syscall):
    // this is called per visible row per frame (selection + render-order tracking),
    // and canonicalizing thousands of rows every frame tanked the nav to ~1 fps.
    return p.lexically_normal().string();
}

bool Editor::navIsSelected(const std::filesystem::path &p) const
{
    if (navSelected.empty())
        return false; // hot path: called per visible row per frame
    return navSelected.count(navCanonKey(p)) != 0;
}

void Editor::navToggleSelected(const std::filesystem::path &p)
{
    std::string k = navCanonKey(p);
    if (!navSelected.erase(k))
        navSelected.insert(k);
}

void Editor::navSetOnlySelected(const std::filesystem::path &p)
{
    navSelected.clear();
    navSelected.insert(navCanonKey(p));
}

// ── Nav shift-click range selection ──
// navVisibleOrder is filled in render order each frame; a shift-click records a
// target, and navApplyRangeSelect() (called after the tree renders) selects every
// entry between the anchor and the target inclusive.
void Editor::navTrackVisible(const std::filesystem::path &p)
{
    navVisibleOrder.push_back(navCanonKey(p));
}
void Editor::navSetAnchor(const std::filesystem::path &p)
{
    navSelAnchor = navCanonKey(p);
}
void Editor::navRangeRequestTo(const std::filesystem::path &p)
{
    navRangeTarget = navCanonKey(p);
}

void Editor::navApplyRangeSelect()
{
    if (navRangeTarget.empty())
        return;
    std::string target = navRangeTarget;
    navRangeTarget.clear();
    if (navSelAnchor.empty())
    {
        navSelected.insert(target);
        navSelAnchor = target;
        return;
    }
    int ai = -1, ti = -1;
    for (int i = 0; i < (int)navVisibleOrder.size(); ++i)
    {
        if (navVisibleOrder[i] == navSelAnchor)
            ai = i;
        if (navVisibleOrder[i] == target)
            ti = i;
    }
    if (ai < 0 || ti < 0)
    {
        navSelected.insert(target); // anchor/target not both visible — just add it
        return;
    }
    if (ai > ti)
        std::swap(ai, ti);
    for (int i = ai; i <= ti; ++i)
        navSelected.insert(navVisibleOrder[i]);
}

// ── Nav name filter ──
bool Editor::navNameMatches(const std::string &name) const
{
    if (navFilterBuf[0] == 0)
        return true;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return lower(name).find(lower(navFilterBuf)) != std::string::npos;
}

// True if `dir` has any descendant whose name matches the filter. Backed by a set
// rebuilt with ONE project walk whenever the filter text changes (cheap per-frame
// after that). Bounded so a giant tree can't stall a keystroke.
bool Editor::navDirHasMatch(const std::filesystem::path &dir) const
{
    if (navFilterBuf[0] == 0)
        return true;
    if (navMatchFilterCached != navFilterBuf)
    {
        navMatchFilterCached = navFilterBuf;
        navMatchDirs.clear();
        auto root = workspaceRoot(); // never CWD (could be System32)
        if (root.empty())
            return true;
        std::error_code ec;
        int budget = 200000;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator() && budget-- > 0; it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (!navNameMatches(it->path().filename().string()))
                continue;
            // Mark every ancestor dir up to the project root.
            for (auto a = it->path().parent_path();; a = a.parent_path())
            {
                navMatchDirs.insert(navCanonKey(a));
                if (a == root || !a.has_parent_path())
                    break;
            }
        }
    }
    return navMatchDirs.count(navCanonKey(dir)) != 0;
}

bool Editor::navIsCodeFile(const std::filesystem::path &p) const
{
    // "Code-only" filter — same set as the project-wide grep walker, plus a
    // few project-file extensions so .sln / .csproj show up in the tree.
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static const std::unordered_set<std::string> ok = {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".cxx",
        ".hxx",
        ".cc",
        ".hh",
        ".m",
        ".mm",
        ".inl",
        ".cs",
        ".vb",
        ".fs",
        ".fsx",
        ".java",
        ".kt",
        ".scala",
        ".groovy",
        ".py",
        ".pyw",
        ".rb",
        ".php",
        ".pl",
        ".js",
        ".jsx",
        ".ts",
        ".tsx",
        ".mjs",
        ".cjs",
        ".go",
        ".rs",
        ".swift",
        ".lua",
        ".sh",
        ".ps1",
        ".psm1",
        ".sql",
        ".r",
        ".jl",
        ".dart",
        ".sln",
        ".csproj",
        ".vcxproj",
        ".uproject",
        ".uplugin",
        ".cmake",
        ".cmakelists",
        ".html",
        ".htm",
        ".xml",
        ".xaml",
        ".json",
        ".yaml",
        ".yml",
        ".toml",
        ".ini",
        ".md",
        ".rst",
        ".tex",
        ".lang",
    };
    return ok.count(ext) != 0 || p.filename() == "CMakeLists.txt" || p.filename() == "Makefile" || p.filename() == "Dockerfile" || p.filename() == "requirements.txt";
}

// Drag-drop a file or folder onto a target folder. Ctrl held during drop =
// copy, otherwise move. Refuses to move a folder into itself or its own
// descendant.
void Editor::navMoveOrCopy(const std::string &src,
                           const std::string &destDir, bool copy)
{
    std::error_code ec;
    std::filesystem::path s(src), d(destDir);
    if (!std::filesystem::exists(d, ec) || !std::filesystem::is_directory(d, ec))
        return;
    auto canonSrc = std::filesystem::weakly_canonical(s, ec);
    auto canonDst = std::filesystem::weakly_canonical(d, ec);
    // Block move-into-self / move-into-own-subtree.
    for (auto p = canonDst; !p.empty(); p = p.parent_path())
    {
        if (p == canonSrc)
            return;
        if (p == p.parent_path())
            break;
    }
    auto target = canonDst / s.filename();
    if (copy)
    {
        std::filesystem::copy(s, target,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              ec);
    }
    else
    {
        std::filesystem::rename(s, target, ec);
    }
    navMarkListDirty();
}

// Cached, sorted listing of one directory. The nav tree calls this every frame
// instead of re-running a directory_iterator (+ sort + per-entry is_directory)
// per expanded folder — that filesystem churn was the panel's dominant cost. A
// 0.5s TTL absorbs external changes; the nav's own edits call navMarkListDirty.
// Safe to hold the returned reference across recursive calls: inserting new keys
// into an unordered_map never invalidates references to existing elements.
const std::vector<std::filesystem::directory_entry> &
Editor::navCachedDir(const std::filesystem::path &dir)
{
    const double kTTL = 0.5;
    double now = ImGui::GetTime();
    auto &slot = navDirCache[dir.string()];
    if (slot.scanned >= 0.0 && (now - slot.scanned) < kTTL)
        return slot.entries;
    slot.entries.clear();
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
            break;
        slot.entries.push_back(e);
    }
    int sortMode = navSortMode;
    auto lowerName = [](const std::filesystem::directory_entry &e) {
        auto n = e.path().filename().string();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return n;
    };
    std::sort(slot.entries.begin(), slot.entries.end(), [&](const auto &a, const auto &b) {
        // Folders always sort before files, regardless of mode.
        bool aDir = a.is_directory(), bDir = b.is_directory();
        if (aDir != bDir)
            return aDir;

        std::error_code sec;
        switch (sortMode)
        {
        case 1: // Modified — newest first (directory_entry caches the stat from the walk)
        {
            auto at = a.last_write_time(sec);
            auto bt = b.last_write_time(sec);
            if (at != bt)
                return at > bt;
            break; // tie -> fall through to name
        }
        case 2: // Size — largest first (folders compared by name, they have no meaningful size)
            if (!aDir)
            {
                auto as = a.file_size(sec);
                auto bs = b.file_size(sec);
                if (as != bs)
                    return as > bs;
            }
            break;
        case 3: // Type — by extension, then name
        {
            auto ae = a.path().extension().string(), be = b.path().extension().string();
            std::transform(ae.begin(), ae.end(), ae.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            std::transform(be.begin(), be.end(), be.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ae != be)
                return ae < be;
            break;
        }
        default:
            break;
        }
        return lowerName(a) < lowerName(b); // Name mode, and tie-breaker for the others
    });
    slot.scanned = now;
    return slot.entries;
}

// Cached whole-tree file list for flat view. The recursive walk (up to 20000
// entries) used to run every frame; now it runs at most every 0.5s (or on edit).
// The list is unfiltered by the name box — that filter is applied per-frame at
// render so changing it doesn't force a re-walk.
const std::vector<Editor::NavFlatItem> &
Editor::navCachedFlatList(const std::filesystem::path &root, bool showDot)
{
    const double kTTL = 0.5;
    double now = ImGui::GetTime();
    std::string rootKey = root.string();
    if (navFlatCacheTime >= 0.0 && navFlatCacheRoot == rootKey &&
        navFlatCacheDot == showDot && (now - navFlatCacheTime) < kTTL)
        return navFlatCache;

    navFlatCache.clear();
    navFlatCacheRoot = rootKey;
    navFlatCacheDot = showDot;
    navFlatCacheTime = now;

    std::error_code ec;
    std::unordered_set<std::string> seen;
    int budget = 20000;
    auto isBuildDir = [](const std::string &n) {
        return n == ".git" || n == ".svn" || n == ".hg" || n == "node_modules" ||
               n == "bin" || n == "obj" || n == "out" || n == "build" ||
               n == "target" || n == ".vs" || n == ".vscode" || n == ".idea" ||
               n == "__pycache__" || n == "packages" || n == "Debug" || n == "Release";
    };
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        auto name = it->path().filename().string();
        if (it->is_directory(ec))
        {
            bool dotdir = !showDot && !name.empty() && name[0] == '.';
            if (dotdir || isBuildDir(name) || navIsExcluded(it->path()))
                it.disable_recursion_pending();
            continue;
        }
        if (--budget < 0)
            break;
        if (!it->is_regular_file(ec))
            continue;
        if (!showDot && !name.empty() && name[0] == '.')
            continue;
        if (navIsExcluded(it->path()))
            continue;
        if (!navIsCodeFile(it->path()))
            continue;
        auto canon = std::filesystem::weakly_canonical(it->path(), ec);
        if (!seen.insert((ec ? it->path() : canon).string()).second)
            continue;
        navFlatCache.push_back({it->path(), name});
    }
    std::sort(navFlatCache.begin(), navFlatCache.end(), [](const NavFlatItem &a, const NavFlatItem &b) {
        std::string an = a.name, bn = b.name;
        std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return an < bn; });
    return navFlatCache;
}

// Best-effort "send to recycle bin" so right-click → Delete isn't a one-way
// road. On Windows uses SHFileOperation with FOF_ALLOWUNDO; on POSIX falls
// back to xdg-trash if available, otherwise plain remove_all.
void Editor::navDeletePath(const std::string &p)
{
#ifdef _WIN32
    // SHFileOperationA needs a double-NUL-terminated source buffer.
    std::string buf = p;
    buf.push_back('\0');
    buf.push_back('\0');
    SHFILEOPSTRUCTA op{};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationA(&op) != 0)
    {
        // Fall back to permanent delete if recycle bin failed (e.g. network drive).
        std::error_code dec;
        std::filesystem::remove_all(p, dec);
    }
#else
    // Try gio trash, then fallback to permanent delete.
    std::string cmd = "gio trash \"" + p + "\" 2>/dev/null";
    if (std::system(cmd.c_str()) != 0)
    {
        std::error_code dec;
        std::filesystem::remove_all(p, dec);
    }
#endif
    navMarkListDirty();
}

void Editor::navOpenPathInExplorer(const std::string &path)
{
#ifdef _WIN32
    // /select, opens Explorer with the file/dir highlighted; for a dir,
    // passing the dir itself opens it.
    std::error_code ec;
    bool isDir = std::filesystem::is_directory(path, ec);
    std::string cmd;
    if (isDir)
        cmd = "explorer \"" + path + "\"";
    else
        cmd = "explorer /select,\"" + path + "\"";
    [[maybe_unused]] int rc = std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    [[maybe_unused]] int rc = std::system(cmd.c_str());
#endif
}

void Editor::navOpenExternally(const std::string &path)
{
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    [[maybe_unused]] int rc = std::system(cmd.c_str());
#endif
}

// One tree-node row for a single entry. Encapsulates the context menu, rename
// inline-edit, click-to-open behaviour, and recurses for directories.
static void renderDirNode(Editor *self,
                          const std::filesystem::path &dir,
                          int depth,
                          bool showDot,
                          std::string &contextPath,
                          std::string &renameTarget,
                          char *renameBuf,
                          std::string &pendingDelete);

static void navRenderEntry(Editor *self,
                           const std::filesystem::directory_entry &e,
                           int depth,
                           bool showDot,
                           std::string &contextPath,
                           std::string &renameTarget,
                           char *renameBuf,
                           std::string &pendingDelete)
{
    std::error_code ec;
    auto name = e.path().filename().string();
    bool isDir = e.is_directory(ec);
    auto absPath = e.path().string();

    // Name filter: hide non-matching files, and folders with no matching
    // descendant. Surviving folders auto-expand below so the matches show.
    if (self->navFilterActive())
    {
        if (isDir)
        {
            if (!self->navDirHasMatch(e.path()))
                return;
        }
        else if (!self->navNameMatches(name))
            return;
    }
    self->navTrackVisible(e.path()); // render order, for shift-click range select

    // In-place rename: replace the row with an InputText.
    if (renameTarget == absPath)
    {
        ImGui::PushID(absPath.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool commit = ImGui::InputText("##rename", renameBuf, 256,
                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivated())
        {
            // Treat focus loss as cancel unless Enter committed.
            if (!commit)
                renameTarget.clear();
        }
        if (commit)
        {
            std::string newName = renameBuf;
            if (!newName.empty() && newName != name)
            {
                auto target = e.path().parent_path() / newName;
                std::error_code rec;
                std::filesystem::rename(e.path(), target, rec);
                self->navMarkListDirty();
            }
            renameTarget.clear();
        }
        ImGui::PopID();
        return;
    }

    // Helper — hover tooltip with file type + timestamps. Cheap: only fires
    // when the user dwells on the row.
    auto navTooltip = [&](const std::filesystem::directory_entry &ent, bool dir) {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
            return;
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(ent.path().filename().string().c_str());
        ImGui::Separator();
        std::error_code lec;
        if (dir)
        {
            ImGui::TextDisabled("Folder");
        }
        else
        {
            auto ext = ent.path().extension().string();
            ImGui::TextDisabled("Type: %s", ext.empty() ? "(none)" : ext.c_str() + 0);
            auto sz = std::filesystem::file_size(ent.path(), lec);
            if (!lec)
            {
                if (sz < 1024)
                    ImGui::TextDisabled("Size: %llu B", (unsigned long long)sz);
                else if (sz < 1024ull * 1024)
                    ImGui::TextDisabled("Size: %.1f KB", sz / 1024.0);
                else if (sz < 1024ull * 1024 * 1024)
                    ImGui::TextDisabled("Size: %.1f MB", sz / (1024.0 * 1024.0));
                else
                    ImGui::TextDisabled("Size: %.2f GB", sz / (1024.0 * 1024.0 * 1024.0));
            }
        }
        // Modified time is the only one std::filesystem gives us portably.
        // Created time is Win32-only — gate accordingly.
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(ent.path().string().c_str(), GetFileExInfoStandard, &fad))
        {
            auto fmt = [](FILETIME ft) -> std::string {
                SYSTEMTIME st;
                FileTimeToSystemTime(&ft, &st);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                              (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                              (int)st.wHour, (int)st.wMinute);
                return buf;
            };
            ImGui::TextDisabled("Created:  %s", fmt(fad.ftCreationTime).c_str());
            ImGui::TextDisabled("Modified: %s", fmt(fad.ftLastWriteTime).c_str());
        }
#else
        auto mt = std::filesystem::last_write_time(ent.path(), lec);
        if (!lec)
        {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                mt - decltype(mt)::clock::now() + std::chrono::system_clock::now());
            std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&cftime));
            ImGui::TextDisabled("Modified: %s", buf);
        }
#endif
        // Image files get a thumbnail preview below the metadata.
        if (!dir)
        {
            auto ext = ent.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (Editor::isImageExt(ext))
            {
                ImGui::Separator();
                self->navShowImageThumbnail(ent.path().string());
            }
        }
        ImGui::EndTooltip();
    };

    // Helper — drag source + drag target. Folders accept drops as
    // move/copy targets; files act as drag sources.
    auto navDnD = [&](bool dir) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            self->navDragSourceSet(absPath);
            ImGui::SetDragDropPayload("NAVPATH", absPath.c_str(), absPath.size() + 1);
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::EndDragDropSource();
        }
        if (dir && ImGui::BeginDragDropTarget())
        {
            if (const auto *payload = ImGui::AcceptDragDropPayload("NAVPATH"))
            {
                std::string src((const char *)payload->Data, payload->DataSize - 1);
                bool copy = ImGui::GetIO().KeyCtrl;
                self->navMoveOrCopy(src, absPath, copy);
            }
            ImGui::EndDragDropTarget();
        }
    };

    if (isDir)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (self->navIsSelected(absPath))
            flags |= ImGuiTreeNodeFlags_Selected;
        // Collapse-all / expand-all: force every folder node open/closed for the
        // one frame the toolbar button set the request.
        if (self->navBulkOpenRequest() >= 0)
            ImGui::SetNextItemOpen(self->navBulkOpenRequest() == 1, ImGuiCond_Always);
        else if (self->navFilterActive())
            ImGui::SetNextItemOpen(true, ImGuiCond_Always); // reveal filtered matches
        bool open = ImGui::TreeNodeEx(name.c_str(), flags);
        navTooltip(e, true);
        navDnD(true);
        // Ctrl-click a folder toggles its selection; shift-click extends the range
        // (the click also toggles the expand state — acceptable; selection is for
        // batch actions like exclude). A plain click just expands (no select change).
        if (ImGui::IsItemClicked())
        {
            ImGuiIO &dio = ImGui::GetIO();
            if (dio.KeyShift)
                self->navRangeRequestTo(e.path());
            else if (dio.KeyCtrl)
            {
                self->navToggleSelected(absPath);
                self->navSetAnchor(e.path());
            }
        }
        if (ImGui::BeginPopupContextItem())
        {
            contextPath = absPath;
            if (!self->navIsSelected(absPath))
                self->navSetOnlySelected(absPath);
            ImGui::EndPopup();
        }
        if (open)
        {
            if (!self->navIsFlat())
            {
                renderDirNode(self, e.path(), depth + 1, showDot,
                              contextPath, renameTarget, renameBuf, pendingDelete);
            }
            ImGui::TreePop();
        }
    }
    else
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (self->navIsSelected(absPath))
            flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::TreeNodeEx(name.c_str(), flags);
        navTooltip(e, false);
        navDnD(false);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            ImGuiIO &cio = ImGui::GetIO();
            if (cio.KeyShift)
                self->navRangeRequestTo(e.path()); // shift-click → select anchor..here
            else if (cio.KeyCtrl)
            {
                self->navToggleSelected(absPath); // ctrl-click → add/remove from selection
                self->navSetAnchor(e.path());
            }
            else
            {
                self->navSetOnlySelected(absPath);
                self->navSetAnchor(e.path());
                self->openFile(absPath);
            }
        }
        if (ImGui::BeginPopupContextItem())
        {
            contextPath = absPath;
            // Right-clicking an unselected item targets just it; right-clicking a
            // selected one keeps the whole multi-selection for the action.
            if (!self->navIsSelected(absPath))
                self->navSetOnlySelected(absPath);
            ImGui::EndPopup();
        }
    }
}

static void renderDirNode(Editor *self,
                          const std::filesystem::path &dir,
                          int depth,
                          bool showDot,
                          std::string &contextPath,
                          std::string &renameTarget,
                          char *renameBuf,
                          std::string &pendingDelete)
{
    if (depth > 20)
        return;
    // Cached, sorted listing (TTL-refreshed) — no per-frame filesystem walk.
    // The reference stays valid across the recursive renderDirNode calls below:
    // those insert new keys into navDirCache, which never invalidates references
    // to existing unordered_map elements.
    const auto &entries = self->navCachedDir(dir);
    for (auto &e : entries)
    {
        auto name = e.path().filename().string();
        if (!showDot && !name.empty() && name[0] == '.')
            continue;
        bool excluded = self->navIsExcluded(e.path());
        if (excluded && !self->navIsShowingExcluded())
            continue;
        std::error_code dec;
        bool isDir = e.is_directory(dec);
        // Code-only filter: still show folders (you need them to navigate),
        // but hide non-source files — EXCEPT images and PDFs, which are viewable
        // assets worth keeping in the tree (navIsCodeFile itself stays source-only,
        // since the grep/symbol walkers reuse it).
        if (self->navIsCodeOnly() && !isDir && !self->navIsCodeFile(e.path()))
        {
            auto fext = e.path().extension().string();
            std::transform(fext.begin(), fext.end(), fext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (!Editor::isImageExt(fext) && fext != ".pdf")
                continue;
        }
        navRenderEntry(self, e, depth, showDot,
                       contextPath, renameTarget, renameBuf, pendingDelete);
    }
}

// Flat view: every file under root in one alphabetical list (no folder
// hierarchy), honoring the same filters. Click opens; hover shows the relative
// path; right-click sets the context path for the shared popup.
static void navRenderFlat(Editor *self, const std::filesystem::path &root,
                          bool showDot, std::string &contextPath)
{
    // Cached whole-tree listing (built at most every 0.5s, not every frame). The
    // name filter is applied here per-frame so typing in it doesn't re-walk.
    const auto &files = self->navCachedFlatList(root, showDot);
    int id = 0;
    for (const auto &item : files)
    {
        if (!self->navNameMatches(item.name)) // name filter (cheap string compare)
            continue;
        ImGui::PushID(id++);
        if (ImGui::Selectable(item.name.c_str()))
            self->openFile(item.path.string());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
        {
            std::error_code rec;
            auto rel = std::filesystem::relative(item.path, root, rec);
            auto ext = item.path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            ImGui::BeginTooltip();
            ImGui::TextUnformatted((rec ? item.path : rel).string().c_str());
            if (Editor::isImageExt(ext))
                self->navShowImageThumbnail(item.path.string());
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem())
        {
            contextPath = item.path.string();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// Middle-mouse pan/scroll for a plain scrolling ImGui window (nav tree, settings).
// Mirrors the editor widget's pan via the public scroll API. It tracks its OWN
// per-frame delta (lastPos) rather than Get/ResetMouseDragDelta: the editor
// renders first and would consume/reset the global drag delta, leaving this at
// zero. windowKey distinguishes windows so a drag started in one can't bleed into
// another. Call once inside the target window (while it is the current window).
void Editor::middleMousePanScroll(int windowKey)
{
    static int activeKey = 0;         // 0 = no pan in progress
    static ImVec2 anchor;             // screen-space click point — drives accel distance
    static ImVec2 lastPos;            // previous-frame mouse pos — drives per-frame scroll
    static int snapAxis = 2;          // 1=horizontal, 2=vertical (re-decided per frame, hysteresis)
    static ImVec2 velEMA{0.0f, 0.0f}; // smoothed velocity for the snap decision

    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (activeKey == 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        activeKey = windowKey;
        anchor = lastPos = ImGui::GetMousePos();
        snapAxis = 2; // start vertical-biased
        velEMA = ImVec2(0.0f, 0.0f);
    }
    if (activeKey != windowKey)
        return; // not the panned window
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    {
        activeKey = 0;
        return;
    }

    ImVec2 pos = ImGui::GetMousePos();
    ImVec2 delta = pos - lastPos; // self-tracked movement
    lastPos = pos;

    // Quadratic acceleration by distance from the anchor (same curve as the editor).
    ImVec2 rel = pos - anchor;
    float dist = std::sqrt(rel.x * rel.x + rel.y * rel.y);
    float ref = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    float t = (ref > 0.0f) ? dist / ref : 0.0f;
    float accel = (prefPanScrollAccel <= 0.0f) ? 1.0f : (1.0f + t * t * prefPanScrollAccel * 0.25f);
    if (accel > 16.0f)
        accel = 16.0f;

    float panSign = prefInvertPan ? -1.0f : 1.0f;
    // Axis SNAP (same as the editor): re-decide every frame so direction can change
    // mid-drag, but snap to one axis (no diagonal). Smoothed velocity + hysteresis +
    // strong vertical bias prevent flip-flop. Horizontal only when there's real
    // horizontal scroll room (nav/settings usually have none → always vertical).
    velEMA.x = velEMA.x * 0.7f + delta.x * 0.3f;
    velEMA.y = velEMA.y * 0.7f + delta.y * 0.3f;
    float vAx = std::fabs(velEMA.x), vAy = std::fabs(velEMA.y);
    bool canHoriz = ImGui::GetScrollMaxX() > 24.0f;
    if (snapAxis == 1)
    {
        if (!canHoriz || vAx < vAy * 1.5f)
            snapAxis = 2;
    }
    else
    {
        if (canHoriz && vAx > vAy * 3.0f)
            snapAxis = 1;
    }
    if (snapAxis == 1)
        ImGui::SetScrollX(ImGui::GetScrollX() - panSign * delta.x * 0.35f);
    else
        ImGui::SetScrollY(ImGui::GetScrollY() - panSign * delta.y * accel);
}

std::filesystem::path Editor::workspaceRoot() const
{
    if (!projectRoot.empty())
        return projectRoot;
    // No project: root at the active document's folder if it's a real on-disk
    // file. Never the process CWD — a command-line / Explorer launch can leave
    // that at C:\Windows\System32, and walking it freezes the nav.
    if (!tabs.empty())
    {
        size_t at = (size_t)activeTab < tabs.size() ? (size_t)activeTab : 0;
        const std::string &fn = tabs[at]->filename;
        if (!fn.empty() && fn != "untitled")
        {
            std::error_code ec;
            auto p = std::filesystem::path(fn).parent_path();
            if (!p.empty() && std::filesystem::is_directory(p, ec))
                return p;
        }
    }
    return {};
}

// Write every dirty document that already has a path (skips "untitled"). No
// trailing-whitespace strip — must not move the caret mid-edit. Returns the count.
int Editor::saveDirtyTitledDocs()
{
    int saved = 0;
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        if (!isDirtyTab(i))
            continue;
        TabDocument &t = *tabs[i];
        if (t.filename.empty() || t.filename == "untitled")
            continue;
        try
        {
            std::ofstream stream(t.filename.c_str());
            stream << t.editor.GetText();
            stream.close();
            t.version = t.editor.GetUndoIndex();
            t.syncedText = t.editor.GetText();
            recordDiskMtime(t); // our own write — keep the external-change watch quiet
            ++saved;
        }
        catch (...)
        {
            // leave it dirty; retry next time
        }
    }
    return saved;
}

void Editor::autoSaveTick()
{
    if (!prefAutoSave || tabs.empty())
        return;
    double now = ImGui::GetTime();
    int interval = prefAutoSaveSec < 5 ? 5 : prefAutoSaveSec;
    if (now - lastAutoSave < (double)interval)
        return;
    lastAutoSave = now;
    saveDirtyTitledDocs();
}

void Editor::renderNavigationPanel()
{
    if (!navPanelVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(180.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Navigation##projectNav", &navPanelVisible))
    {
        auto root = workspaceRoot(); // projectRoot / active doc's folder / none (never CWD)

        // A nav edit (rename/move/delete/paste) invalidated the cached listings.
        // Clear here — at the top, before anything iterates a cached vector — so a
        // mutation that fires mid-render can't pull the rug out from under the loop.
        if (navListDirty)
        {
            navDirCache.clear();
            navFlatCacheTime = -1.0;
            navListDirty = false;
        }

        // With no root at all (no project, no open doc), the picker/path/filter
        // rows are dead controls — show a single clear entry point instead.
        static bool navEditingPath = false;
        static bool navPathFocus = false; // one-shot: focus the input on entry
        static char navPathBuf[1024] = {0};
        if (root.empty())
        {
            ImGui::Spacing();
            if (ImGui::Button("Open Project\xe2\x80\xa6", ImVec2(-FLT_MIN, 0.0f)))
                openProjectFolderPicker();
            ImGui::TextDisabled("or open a file to root the\ntree at its folder");
        }
        else
        {
        // Row 1: the project path fills the row (no picker button — click the path
        // to edit it in place, or use File > Open Project). Right-aligned +
        // truncated (tail kept) by default with the full path on hover; a setting
        // switches to wrapping.

        if (navEditingPath)
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (navPathFocus)
            {
                ImGui::SetKeyboardFocusHere();
                navPathFocus = false;
            }
            bool commit = ImGui::InputText("##navPathEdit", navPathBuf, sizeof(navPathBuf),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
            if (commit)
            {
                std::error_code pec;
                std::filesystem::path np(navPathBuf);
                if (std::filesystem::is_directory(np, pec))
                    setProjectRoot(np);
                navEditingPath = false;
            }
            else if (ImGui::IsItemDeactivated())
                navEditingPath = false; // focus loss without Enter reverts to display
        }
        else
        {
            const std::string full = root.string();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (navPathWrap)
            {
                ImGui::TextWrapped("%s", full.c_str());
            }
            else
            {
                // Truncate the FRONT (keep the tail = deepest folder) to fit, then
                // right-align what remains.
                float avail = ImGui::GetContentRegionAvail().x;
                std::string shown = full;
                if (ImGui::CalcTextSize(full.c_str()).x > avail && full.size() > 1)
                {
                    size_t lo = 0;
                    while (lo < full.size() &&
                           ImGui::CalcTextSize(("..." + full.substr(lo)).c_str()).x > avail)
                        ++lo;
                    shown = "..." + full.substr(lo);
                }
                float w = ImGui::CalcTextSize(shown.c_str()).x;
                if (avail > w + 2.0f)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w));
                ImGui::TextUnformatted(shown.c_str());
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(click to edit)", full.c_str());
            if (ImGui::IsItemClicked())
            {
                std::snprintf(navPathBuf, sizeof(navPathBuf), "%s", full.c_str());
                navEditingPath = true;
                navPathFocus = true;
            }
        }

        // Row 2: Filters popup + collapse/expand. Filter box goes BELOW the separator.
        // Compact: a single ☰ glyph; tinted when any non-default filter is active.
        {
            bool filtersActive = navShowDotFiles || navCodeOnly || navShowExcluded || navFlatFiles;
            if (filtersActive)
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 130, 250, 255));
            if (ImGui::SmallButton("\xe2\x89\xa1")) // ≡ (U+2261 — present in DejaVu; U+2630 was tofu)
                ImGui::OpenPopup("##navFilters");
            if (filtersActive)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(filtersActive ? "Filters (active)" : "Filters");
        }
        if (ImGui::BeginPopup("##navFilters", ImGuiWindowFlags_NoMove))
        {
            ImGui::Checkbox("Show dotfiles (.git, .env, ...)", &navShowDotFiles);
            ImGui::Checkbox("Code / project files only (+ images, PDF)", &navCodeOnly);
            ImGui::Checkbox("Show excluded items", &navShowExcluded);
            ImGui::Checkbox("Flat view (no folder nesting)", &navFlatFiles);
            ImGui::Checkbox("Wrap project path", &navPathWrap);
            if (!ueSourceDir.empty())
                ImGui::Checkbox(("Show " + ueSourceLabel).c_str(), &navShowUeSource);
            // Manage existing source locations here (Sort + Add live inline in the
            // header row now). Each row removes with its ✕.
            if (!extraSourceLocations.empty())
            {
                ImGui::Separator();
                ImGui::TextDisabled("Source locations");
                for (const auto &loc : extraSourceLocations)
                {
                    ImGui::PushID(loc.c_str());
                    if (ImGui::SmallButton("x"))
                        navPendingRemoveSourceLoc = loc;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(std::filesystem::path(loc).filename().string().c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", loc.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("-"))
            navSetAllOpen = 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Collapse all folders");
        ImGui::SameLine();
        if (ImGui::SmallButton("+"))
            navSetAllOpen = 1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Expand all folders");

        // Inline sort + add-location — one click, in the panel, no wasted rows.
        static const char *kSortItems[] = {"Name", "Modified (newest)", "Size (largest)", "Type"};
        ImGui::SameLine();
        if (ImGui::SmallButton("Sort"))
            ImGui::OpenPopup("##navSortPop");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sort by: %s", kSortItems[navSortMode]);
        if (ImGui::BeginPopup("##navSortPop"))
        {
            for (int i = 0; i < IM_ARRAYSIZE(kSortItems); ++i)
                if (ImGui::MenuItem(kSortItems[i], nullptr, navSortMode == i))
                {
                    navSortMode = i;
                    navMarkListDirty();
                    saveSettings();
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+Dir"))
            openAddSourceLocationPicker();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Add a source location (shown as a nav root + indexed for symbols)");
        ImGui::Separator();

        // Name filter — below the separator. Only entries whose name contains this
        // show (matching files keep their ancestor folders visible + auto-expand).
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##navFilter", "filter by name...", navFilterBuf, sizeof(navFilterBuf));
        } // end has-root header

        navContextPath.clear();
        navVisibleOrder.clear(); // rebuilt this frame in render order (shift-range select)
        std::error_code ec;
        if (std::filesystem::is_directory(root, ec))
        {
            if (navFlatFiles)
                navRenderFlat(this, root, navShowDotFiles, navContextPath);
            else
                renderDirNode(this, root, 0, navShowDotFiles,
                              navContextPath, navRenameTarget, navRenameBuf, navPendingDelete);
        }
        else if (!root.empty())
        {
            ImGui::TextDisabled("(folder not found: %s)", root.string().c_str());
        }

        // ── Plugin-provided extra source root ────────────────────────────────
        // A project-type plugin (e.g. Unreal) can expose a second read-only root
        // — the engine's Source tree — so its headers are browsable. Resolving it
        // walks the filesystem, so memoize per workspace root. The tree is lazy
        // (an expanded folder lists its children), so a collapsed node is cheap
        // even for a huge tree; hide it entirely via Filters for speed.
        if (!root.empty())
        {
            std::string rk = root.string();
            if (rk != ueSourceKey)
            {
                ueSourceKey = rk;
                ueSourceDir.clear();
                ueSourceLabel.clear();
                if (auto extra = pluginRegistry.extraSourceRoot(root))
                {
                    std::error_code uec;
                    if (std::filesystem::is_directory(extra->path, uec))
                    {
                        ueSourceDir = extra->path.string();
                        ueSourceLabel = extra->label;
                    }
                }
            }
            if (!ueSourceDir.empty() && navShowUeSource)
            {
                ImGui::Separator();
                if (ImGui::TreeNodeEx(ueSourceLabel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    renderDirNode(this, std::filesystem::path(ueSourceDir), 1, navShowDotFiles,
                                  navContextPath, navRenameTarget, navRenameBuf, navPendingDelete);
                    ImGui::TreePop();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n(hide via Filters for speed)", ueSourceDir.c_str());
            }
        }

        // ── Extra source locations ───────────────────────────────────────────
        // User-added roots outside the project (indexed into symbols too). Each is
        // a collapsible tree; right-click the header to remove it.
        for (const auto &loc : extraSourceLocations)
        {
            std::error_code lec;
            if (!std::filesystem::is_directory(loc, lec))
                continue;
            ImGui::Separator();
            ImGui::PushID(loc.c_str());
            std::string label = std::filesystem::path(loc).filename().string();
            if (label.empty())
                label = loc;
            label += "  (source)";
            bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(right-click to remove)", loc.c_str());
            if (ImGui::BeginPopupContextItem("##srcloc"))
            {
                if (ImGui::MenuItem("Remove source location"))
                    navPendingRemoveSourceLoc = loc;
                ImGui::EndPopup();
            }
            if (open)
            {
                renderDirNode(this, std::filesystem::path(loc), 1, navShowDotFiles,
                              navContextPath, navRenameTarget, navRenameBuf, navPendingDelete);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (!navPendingRemoveSourceLoc.empty())
        {
            removeSourceLocation(navPendingRemoveSourceLoc);
            navPendingRemoveSourceLoc.clear();
        }

        // ── Open elsewhere ───────────────────────────────────────────────────
        // Tabs whose file lives OUTSIDE the project root — opened by browsing or
        // by Go-to-Definition jumping into engine/system headers. Listing them
        // here gives each a visible location; clicking focuses (and scrolls the
        // tab bar to) that tab. Shown for every project type.
        {
            std::error_code rec;
            auto rootCanon = root.empty() ? std::filesystem::path()
                                          : std::filesystem::weakly_canonical(root, rec);
            auto isUnderRoot = [&](const std::string &fn) {
                if (rootCanon.empty())
                    return false;
                std::error_code lec;
                auto c = std::filesystem::weakly_canonical(std::filesystem::path(fn), lec);
                auto rs = rootCanon.generic_string();
                auto cs = c.generic_string();
                if (cs.size() < rs.size() || cs.compare(0, rs.size(), rs) != 0)
                    return false;
                // Require a path-separator boundary so "C:/proj" doesn't match
                // the sibling "C:/project2/...".
                return cs.size() == rs.size() || cs[rs.size()] == '/';
            };
            std::vector<size_t> externals;
            for (size_t i = 0; i < tabs.size(); ++i)
            {
                const auto &fn = tabs[i]->filename;
                if (fn == "untitled")
                    continue;
                if (!isUnderRoot(fn))
                    externals.push_back(i);
            }
            if (!externals.empty())
            {
                ImGui::Separator();
                std::string hdr = "Open Elsewhere (" + std::to_string(externals.size()) + ")";
                if (ImGui::TreeNodeEx(hdr.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth |
                                                       ImGuiTreeNodeFlags_DefaultOpen))
                {
                    for (size_t idx : externals)
                    {
                        std::filesystem::path fp(tabs[idx]->filename);
                        std::string leaf = fp.filename().string();
                        ImGui::PushID((int)idx);
                        bool selected = (idx == (size_t)activeTab);
                        if (ImGui::Selectable(leaf.c_str(), selected))
                        {
                            activeTab = idx;
                            tabs[idx]->wantFocus = true;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", tabs[idx]->filename.c_str());
                        // Dim parent-folder hint on the same row (right-aligned).
                        std::string dir = fp.parent_path().filename().string();
                        if (!dir.empty())
                        {
                            float avail = ImGui::GetContentRegionAvail().x;
                            float w = ImGui::CalcTextSize(dir.c_str()).x;
                            ImGui::SameLine();
                            if (avail > w + 8.0f)
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w - 4.0f));
                            ImGui::TextDisabled("%s", dir.c_str());
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            }
        }

        navApplyRangeSelect(); // resolve a pending shift-click range now that order is known
        navSetAllOpen = -1;    // bulk open/close request consumed this frame

        // Context-menu popup. BeginPopupContextItem above sets contextPath when
        // the user right-clicks a tree row; we open the popup here with that
        // path baked in so menu items act on the correct entry.
        if (!navContextPath.empty())
        {
            ImGui::OpenPopup("##navCtx");
        }
        if (ImGui::BeginPopup("##navCtx", ImGuiWindowFlags_NoMove))
        {
            // Cache the path on first open so it survives across frames while
            // the popup is up. (BeginPopupContextItem only fires once.)
            static std::string ctxPath;
            if (!navContextPath.empty())
                ctxPath = navContextPath;
            std::error_code cec;
            bool isDir = std::filesystem::is_directory(ctxPath, cec);
            ImGui::TextDisabled("%s", std::filesystem::path(ctxPath).filename().string().c_str());
            ImGui::Separator();
            if (!isDir && ImGui::MenuItem("Open"))
            {
                openFile(ctxPath);
            }
            if (!isDir && ImGui::MenuItem("Open to Left"))
            {
                openFileToSide(ctxPath, -1);
            }
            if (!isDir && ImGui::MenuItem("Open to Right"))
            {
                openFileToSide(ctxPath, +1);
            }
            {
                auto mext = std::filesystem::path(ctxPath).extension().string();
                std::transform(mext.begin(), mext.end(), mext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (!isDir && isMarkdownExt(mext) && ImGui::MenuItem("Preview Markdown"))
                {
                    openFile(ctxPath);       // make it the active doc
                    mdPreviewVisible = true; // preview renders the active markdown doc
                }
            }
            // Create new file / folder. Target directory = the right-clicked
            // folder itself, or the parent folder of a right-clicked file. The
            // new entry is created with a unique default name and immediately
            // dropped into the inline-rename editor (VSCode/Explorer style).
            ImGui::Separator();
            {
                std::filesystem::path parent = isDir ? std::filesystem::path(ctxPath)
                                                     : std::filesystem::path(ctxPath).parent_path();
                auto createEntry = [&](bool folder) {
                    std::error_code nec;
                    std::filesystem::path target = parent / (folder ? "New Folder" : "New File.txt");
                    for (int n = 2; std::filesystem::exists(target, nec); ++n)
                        target = parent / (folder ? ("New Folder (" + std::to_string(n) + ")")
                                                  : ("New File (" + std::to_string(n) + ").txt"));
                    bool ok = false;
                    if (folder)
                        ok = std::filesystem::create_directory(target, nec) && !nec;
                    else
                    {
                        std::ofstream f(target);
                        ok = f.good();
                    }
                    if (ok)
                    {
                        navMarkListDirty();
                        navRenameTarget = target.string();
                        std::snprintf(navRenameBuf, sizeof(navRenameBuf), "%s",
                                      target.filename().string().c_str());
                    }
                };
                if (ImGui::MenuItem("New File"))
                    createEntry(false);
                if (ImGui::MenuItem("New Folder"))
                    createEntry(true);
            }

            // Shell/path actions — separated from the editor-open group above, since
            // "Open in Explorer" is a different meaning of "open" than "Open" (editor).
            ImGui::Separator();
            if (ImGui::MenuItem("Reveal in Explorer"))
            {
                navOpenPathInExplorer(ctxPath);
            }
            if (ImGui::MenuItem("Copy path"))
            {
                ImGui::SetClipboardText(ctxPath.c_str());
            }
            if (!isDir && navIsRunnable(ctxPath))
            {
                ImGui::Separator();
                auto [base, dir] = runCommandForPath(ctxPath);
                if (ImGui::MenuItem("Run"))
                {
                    runCommandInOutputPanel(base, dir);
                }
                if (ImGui::MenuItem("Run with Arguments..."))
                {
                    requestRunWithArgs(base, dir);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy"))
            {
                navClipboardPath = ctxPath;
                navClipboardIsCut = false;
            }
            if (ImGui::MenuItem("Cut"))
            {
                navClipboardPath = ctxPath;
                navClipboardIsCut = true;
            }
            if (ImGui::MenuItem("Paste", nullptr, false,
                                !navClipboardPath.empty() && isDir))
            {
                auto src = std::filesystem::path(navClipboardPath);
                auto dst = std::filesystem::path(ctxPath) / src.filename();
                std::error_code pec;
                if (navClipboardIsCut)
                    std::filesystem::rename(src, dst, pec);
                else
                    std::filesystem::copy(src, dst,
                                          std::filesystem::copy_options::recursive |
                                              std::filesystem::copy_options::overwrite_existing,
                                          pec);
                if (navClipboardIsCut)
                    navClipboardPath.clear();
                navMarkListDirty();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename"))
            {
                navRenameTarget = ctxPath;
                auto leaf = std::filesystem::path(ctxPath).filename().string();
                std::snprintf(navRenameBuf, sizeof(navRenameBuf), "%s", leaf.c_str());
            }
            if (ImGui::MenuItem("Delete"))
            {
                navPendingDelete = ctxPath;
                ImGui::OpenPopup("##navConfirmDel");
            }
            ImGui::Separator();
            // Same key scheme as navCanonKey / navIsExcluded (lexically_normal).
            std::string canonKey = std::filesystem::path(ctxPath).lexically_normal().string();
            bool excluded = navExcluded.count(canonKey) && navExcluded[canonKey];
            // The action targets the whole multi-selection (navSelected always
            // includes the right-clicked item). The right-clicked item's state
            // picks Exclude vs Re-include; the count suffix shows the batch size.
            size_t selN = navSelected.size();
            std::string suffix = (selN > 1) ? (" (" + std::to_string(selN) + ")") : std::string();
            if (!excluded)
            {
                if (ImGui::MenuItem(("Exclude from view" + suffix).c_str()))
                {
                    for (auto &k : navSelected)
                        navExcluded[k] = true;
                    rebuildProjectIndex(); // drop the now-hidden paths' symbols
                    navMarkListDirty();    // flat view filters on the cached list
                }
            }
            else
            {
                if (ImGui::MenuItem(("Re-include in view" + suffix).c_str()))
                {
                    for (auto &k : navSelected)
                        navExcluded.erase(k);
                    rebuildProjectIndex(); // re-index the now-visible paths
                    navMarkListDirty();    // flat view filters on the cached list
                }
            }
            ImGui::EndPopup();
        }

        // Confirm delete — destructive, so gate behind a yes/no.
        // Delete sends to recycle bin on Windows (FOF_ALLOWUNDO) so it's
        // recoverable. The "Force delete" checkbox bypasses that for the
        // rare case where the user really wants permanent removal.
        static bool forceDelete = false;
        if (!navPendingDelete.empty())
            ImGui::OpenPopup("##navConfirmDel");
        if (ImGui::BeginPopupModal("##navConfirmDel", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Delete %s?",
                        std::filesystem::path(navPendingDelete).filename().string().c_str());
#ifdef _WIN32
            ImGui::TextDisabled(forceDelete
                                    ? "Permanent — bypasses Recycle Bin."
                                    : "Sent to Recycle Bin (recoverable).");
#else
            ImGui::TextDisabled(forceDelete
                                    ? "Permanent — bypasses trash."
                                    : "Sent to trash via gio (if available).");
#endif
            ImGui::Checkbox("Force delete (skip recycle bin)", &forceDelete);
            ImGui::Separator();
            if (ImGui::Button("Delete"))
            {
                if (forceDelete)
                {
                    std::error_code dec;
                    std::filesystem::remove_all(navPendingDelete, dec);
                }
                else
                {
                    navDeletePath(navPendingDelete);
                }
                navMarkListDirty();
                navPendingDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                navPendingDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Middle-mouse pan/scroll inside the nav tree (same gesture as the editor).
        middleMousePanScroll(1);
    }
    ImGui::End();
}

void Editor::navInitDockLayout(unsigned int dockId)
{
    // One-time default layout (only when imgui.ini has no saved layout):
    //   ┌──────┬─────────────────┬────────┐
    //   │ Nav  │   documents     │  Find  │
    //   │ left │   (central)     │  Refs  │  ← right
    //   │      ├─────────────────┤ (right)│
    //   │      │   Output        │        │  ← Output spans the bottom of center
    //   └──────┴─────────────────┴────────┘
    // Documents dock into the central node (centralDockId) so new ones open
    // as tabs there. Find References → right pane, Output → bottom pane.
    if (dockLayoutInitialized)
        return;
    dockLayoutInitialized = true;
    if (ImGui::DockBuilderGetNode(dockId) != nullptr)
    {
        // A saved layout exists — don't clobber it. Docs still route to the
        // root's central node via SetNextWindowDockID below.
        centralDockId = dockId;
        return;
    }
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID dockMain = dockId;
    ImGuiID leftId = 0, rightId = 0, bottomId = 0;
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.20f, &leftId, &dockMain);
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.25f, &rightId, &dockMain);
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.25f, &bottomId, &dockMain);

    ImGui::DockBuilderDockWindow("Navigation##projectNav", leftId);
    ImGui::DockBuilderDockWindow("Symbols###symbolsPanel", leftId); // tabs with Navigation
    ImGui::DockBuilderDockWindow("###refsPanel", rightId);
    ImGui::DockBuilderDockWindow("###outputPanel", bottomId);
    centralDockId = dockMain; // documents go here as tabs
    ImGui::DockBuilderFinish(dockId);
}

// ── Manual viewport control ──────────────────────────────────────────
//
// Multi-viewport is enabled; windows only leave the main OS window when the
// user drags them out or invokes these. Pop-out undocks the active document
// and positions it just outside the main viewport (which makes ImGui give it
// its own OS window next frame). Remerge docks it back into the central node.

void Editor::popOutActiveDoc(int dir)
{
    if (tabs.empty())
        return;
    auto label = windowLabelFor(*tabs[activeTab]);
    ImGui::DockBuilderDockWindow(label.c_str(), 0); // undock → floating
    ImGuiViewport *mv = ImGui::GetMainViewport();
    float w = mv->Size.x * 0.45f;
    float x = (dir < 0) ? (mv->Pos.x - w - 8.0f)           // just left of main
                        : (mv->Pos.x + mv->Size.x + 8.0f); // just right of main
    ImGui::SetWindowPos(label.c_str(), ImVec2(x, mv->Pos.y), ImGuiCond_Always);
    ImGui::SetWindowSize(label.c_str(), ImVec2(w, mv->Size.y * 0.9f), ImGuiCond_Always);
    tabs[activeTab]->wantFocus = true;
}

void Editor::remergeActiveWindow()
{
    if (tabs.empty())
        return;
    auto label = windowLabelFor(*tabs[activeTab]);
    // Reuse the captured root id (this runs from a menu/keybind, a different ID
    // stack than the dockspace submission — GetID here would return a phantom id).
    ImGuiID rootId = mainDockId ? (ImGuiID)mainDockId : ImGui::GetID("MainDockSpace");
    ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(rootId);
    ImGuiID target = central ? central->ID
                             : (centralDockId ? (ImGuiID)centralDockId : rootId);
    ImGui::DockBuilderDockWindow(label.c_str(), target);
    ImGui::DockBuilderFinish(rootId);
    tabs[activeTab]->wantFocus = true;
}

void Editor::remergeAllWindows()
{
    // Rebuild the default docked layout AND explicitly re-dock every open
    // document into the central node. (SetNextWindowDockID with FirstUseEver
    // won't move windows that have already been positioned, so we must dock
    // them here directly.)
    // Reuse the captured root id — recomputing GetID from this menu/keybind
    // context yields a different (phantom) id, which is what left windows adrift
    // after Reset Layout.
    ImGuiID root = mainDockId ? (ImGuiID)mainDockId : ImGui::GetID("MainDockSpace");
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

    ImGuiID dockMain = root, leftId = 0, rightId = 0, bottomId = 0;
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.20f, &leftId, &dockMain);
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.25f, &rightId, &dockMain);
    ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.25f, &bottomId, &dockMain);

    ImGui::DockBuilderDockWindow("Navigation##projectNav", leftId);
    ImGui::DockBuilderDockWindow("Symbols###symbolsPanel", leftId); // tabs with Navigation
    ImGui::DockBuilderDockWindow("###refsPanel", rightId);
    ImGui::DockBuilderDockWindow("###outputPanel", bottomId);
    for (auto &up : tabs)
    {
        ImGui::DockBuilderDockWindow(windowLabelFor(*up).c_str(), dockMain);
        up->dockedOnce = true; // they're now docked where we put them
    }
    ImGui::DockBuilderFinish(root);

    centralDockId = dockMain;
    dockLayoutInitialized = true; // we just built it; don't let the one-shot re-run
}

// ── Split current tab to the right ────────────────────────────────────
//
// Uses ImGui's DockBuilder to split the central dock node horizontally and
// move the active doc into the new right pane. Builds the split once on
// command and clears the flag, so subsequent frames don't re-split.
void Editor::splitActiveTabRight()
{
    if (tabs.size() < 2)
        return; // need a second tab to split into
    wantSplitRight = true;
}

void Editor::openFileToSide(const std::string &path, int dir)
{
    openFile(path); // opens or focuses the file
    if (tabs.empty() || dir == 0)
        return;
    // The opened/focused file is the active tab; queue it to be docked into a
    // side split next frame (renderDockedDocuments does the DockBuilder work).
    pendingSideDocId = tabs[activeTab]->id;
    pendingSideDir = dir;
}

// ── Per-project symbol index ─────────────────────────────────────────

std::shared_ptr<const Editor::ProjectIndex> Editor::indexSnapshot()
{
    std::lock_guard<std::mutex> lock(indexState->mutex);
    return indexState->index;
}

// Per-project cache file under %APPDATA%/ImGuiColorTextEdit/index/, named by a
// hash of the project root so each project gets its own.
std::string Editor::indexCachePath() const
{
    if (projectRoot.empty())
        return {};
    size_t h = std::hash<std::string>{}(projectRoot.string());
    char name[32];
    std::snprintf(name, sizeof(name), "%016zx.idx", (size_t)h);
    std::error_code ec;
    auto dir = userConfigDir() / "index";
    std::filesystem::create_directories(dir, ec);
    return (dir / name).string();
}

// Load the on-disk symbol cache and publish it as the initial index, so the
// Symbols panel / go-to-def / member completion work immediately on project open
// (before the background rebuild finishes). The cache is also kept so the rebuild
// can skip re-parsing unchanged files.
void Editor::loadIndexCache()
{
    std::string path = indexCachePath();
    if (path.empty())
        return;
    std::unordered_map<std::string, ts::FileSyms> disk;
    if (!ts::readIndexCache(path, disk) || disk.empty())
        return;

    auto idx = std::make_shared<ProjectIndex>();
    std::unordered_set<std::string> idset;
    for (auto &kv : disk)
    {
        for (auto &sym : kv.second.symbols)
        {
            idset.insert(sym.name);
            if (!sym.isDefinition)
                continue;
            auto &dv = idx->tsDefs[sym.name];
            if (dv.size() < 32)
                dv.push_back({kv.first, sym.line, sym.kind});
            if (!sym.enclosingType.empty())
            {
                auto &mv = idx->members[sym.enclosingType];
                if (mv.size() < 256)
                    mv.push_back(sym.name);
            }
        }
        if (!kv.second.symbols.empty())
            idx->fileSymbols[kv.first] = kv.second.symbols;
        for (auto &tkv : kv.second.memberTypes)
            for (auto &mkv : tkv.second)
                idx->memberTypes[tkv.first][mkv.first] = mkv.second;
    }
    idx->identifiers.assign(idset.begin(), idset.end());
    std::sort(idx->identifiers.begin(), idx->identifiers.end());

    std::lock_guard<std::mutex> lock(indexState->mutex);
    indexState->index = idx;
    indexState->cache = std::move(disk);
}

Editor::~Editor()
{
    lspClient.stop(); // graceful shutdown + join the reader thread (no std::terminate)
}

// ── LSP (clangd) integration ─────────────────────────────────────────────────
// clangd gives real C/C++ intellisense (full std::/system/ImGui members, accurate
// completion) that the tree-sitter index can't. Tree-sitter stays the instant
// baseline; clangd refines the completion popup asynchronously via pollLsp().

void Editor::detectClangd()
{
    const char *candidates[] = {
        "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clangd.exe",
        "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/bin/clangd.exe",
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/x64/bin/clangd.exe",
        "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin/clangd.exe",
        "C:/Program Files/LLVM/bin/clangd.exe",
    };
    std::error_code ec;
    for (const char *c : candidates)
        if (std::filesystem::exists(c, ec))
        {
            clangdPath = c;
            return;
        }
    clangdPath = "clangd"; // last resort: rely on PATH (start() fails cleanly if absent)
}

void Editor::startLspForProject()
{
    if (!lspEnabled || projectRoot.empty())
        return;
    if (clangdPath.empty())
        detectClangd();
    lspClient.stop();
    lspDocHash.clear();
    lspClient.start(clangdPath, lsp::pathToUri(projectRoot.string()));
}

std::string Editor::lspUriForTab(const TabDocument &t) const
{
    if (t.filename.empty() || t.filename == "untitled")
        return {};
    return lsp::pathToUri(t.filename);
}

bool Editor::lspActiveForExt(const std::string &extLower) const
{
    if (!lspEnabled || !lspClient.ready())
        return false;
    return extLower == ".cpp" || extLower == ".cc" || extLower == ".cxx" || extLower == ".c" ||
           extLower == ".h" || extLower == ".hpp" || extLower == ".hxx" || extLower == ".hh" || extLower == ".inl";
}

void Editor::lspSyncDoc(TabDocument &t)
{
    if (t.largeFile)
        return; // didOpen/didChange ship the WHOLE buffer to clangd — skip for huge files
    std::string uri = lspUriForTab(t);
    if (uri.empty())
        return;
    std::string text = t.editor.GetText();
    std::size_t h = std::hash<std::string>{}(text);
    if (!lspClient.isOpen(uri))
    {
        lspClient.didOpen(uri, "cpp", text);
        lspDocHash[t.id] = h;
    }
    else if (lspDocHash[t.id] != h)
    {
        lspClient.didChange(uri, text);
        lspDocHash[t.id] = h;
    }
}

void Editor::pollLsp()
{
    if (!lspClient.spawned())
        return;
    for (auto &r : lspClient.poll())
    {
        if (r.serverGone)
        {
            pushToast("clangd stopped", IM_COL32(240, 200, 90, 255));
            lspDiagnostics.clear();
            continue;
        }
        // Completion: refine the popup only with a NON-empty reply for the LATEST
        // request, and only if its tab is still active (avoids wiping the ts
        // baseline on clangd's parse-not-ready empty replies / a tab switch).
        if (r.kind == lsp::ResultKind::Completion && r.id == lspCompletionId && !r.completionItems.empty())
        {
            if (lspCompletionTab && !tabs.empty() && &doc() == lspCompletionTab)
            {
                std::vector<std::string> items;
                std::unordered_set<std::string> seen;
                for (auto &it : r.completionItems)
                {
                    const std::string &s = it.insertText.empty() ? it.label : it.insertText;
                    if (!s.empty() && seen.insert(s).second)
                        items.push_back(s);
                    if (items.size() >= 50)
                        break;
                }
                if (!items.empty())
                    doc().editor.SetAutoCompleteSuggestions(items);
            }
        }
        // Definition: clangd's (often cross-file, more accurate) target for the
        // latest go-to-def request. The instant tree-sitter jump already happened;
        // this navigates to clangd's location if it differs.
        else if (r.kind == lsp::ResultKind::Definition && r.id == lspDefinitionId && !r.locations.empty())
        {
            const auto &loc = r.locations[0];
            std::string p = lsp::uriToPath(loc.uri);
            if (!p.empty())
            {
                // Record the FROM position as of NOW (this reply may land frames
                // after the request, by which time the cursor moved). Only if the
                // sync path didn't already record this go-to-def. Using the live
                // location — not the stale captured origin — keeps Back correct.
                if (navJumpOriginValid)
                {
                    navHistory.record(currentNavLocation());
                    navJumpOriginValid = false;
                }
                openFile(p);
                if (!tabs.empty())
                {
                    auto &e = doc().editor;
                    e.SetCursor(loc.line, 0);
                    e.SelectLine(loc.line);
                    e.ScrollToLine(loc.line, TextEditor::Scroll::alignMiddle);
                }
            }
            lspDefinitionId = 0; // consume; ignore stale duplicates
        }
        // Hover: clangd's type/signature for the word under the mouse. The tooltip
        // renders lspHoverText next frame while the hover persists.
        else if (r.kind == lsp::ResultKind::Hover && r.id == lspHoverId)
        {
            lspHoverText = r.hoverText;
        }
        // Diagnostics: server-pushed errors/warnings keyed by file. Key on the
        // canonical PATH (not the raw URI) so the status-bar lookup matches even if
        // clangd echoes a differently-encoded URI than we sent. Empty = "now clean".
        else if (r.kind == lsp::ResultKind::Diagnostics)
        {
            std::string key = lsp::uriToPath(r.uri);
            if (key.empty())
                continue;
            if (r.diagnostics.empty())
                lspDiagnostics.erase(key);
            else
                lspDiagnostics[key] = std::move(r.diagnostics);
        }
    }
}

// Background walk of the project's code files. Collects every identifier token
// (for autocomplete) and the definition sites of each symbol (for Go-to-Def).
// Published atomically when done; a newer build (gen) supersedes an older one.
void Editor::rebuildProjectIndex()
{
    if (projectRoot.empty())
        return;
    auto st = indexState; // by value → outlives Editor
    if (st->building.exchange(true))
    {
        // A build is in flight; record that the project changed so the running
        // build re-runs a pass when it finishes (otherwise this save's edits stay
        // unindexed until the next save — the dropped-mid-build-save bug).
        st->rebuildRequested = true;
        return;
    }
    st->rebuildRequested = false;
    int gen = ++st->gen;
    std::filesystem::path root = projectRoot;
    std::string cachePath = indexCachePath();
    // Snapshot the user's "Exclude from view" set (UI-owned) so the background
    // walk skips the same paths the nav tree hides — keeping them out of symbols,
    // go-to-def and autocomplete too.
    auto excludedSnap = navExcluded;
    // Extra source locations are indexed alongside the project root (go-to-def +
    // autocomplete over folders outside the project). Snapshot for the worker.
    auto extraSnap = extraSourceLocations;

    std::thread([st, gen, root, cachePath, excludedSnap, extraSnap]() {
        // Snapshot the previous build's per-file cache once; reused across passes
        // to skip re-parsing files whose mtime+size are unchanged.
        std::unordered_map<std::string, ts::FileSyms> oldCache;
        {
            std::lock_guard<std::mutex> lk(st->mutex);
            oldCache = st->cache;
        }
        // Mirror navIsExcluded() against the snapshot (canonical-path keyed).
        auto isExcluded = [&excludedSnap](const std::filesystem::path& p) {
            if (excludedSnap.empty()) return false;
            std::error_code e;
            auto key = std::filesystem::weakly_canonical(p, e);
            auto it = excludedSnap.find((e ? p : key).string());
            return it != excludedSnap.end() && it->second;
        };
        auto extOk = [](const std::string &e) {
            static const std::unordered_set<std::string> ok = {
                ".c",
                ".h",
                ".cpp",
                ".hpp",
                ".cxx",
                ".hxx",
                ".cc",
                ".hh",
                ".m",
                ".mm",
                ".inl",
                ".cs",
                ".vb",
                ".fs",
                ".fsx",
                ".java",
                ".kt",
                ".kts",
                ".scala",
                ".groovy",
                ".py",
                ".pyw",
                ".rb",
                ".php",
                ".pl",
                ".js",
                ".jsx",
                ".ts",
                ".tsx",
                ".mjs",
                ".cjs",
                ".go",
                ".rs",
                ".swift",
                ".lua",
                ".sh",
                ".ps1",
                ".psm1",
                ".sql",
                ".r",
                ".jl",
                ".dart",
                // Shader sources (ReShade FX / HLSL) — indexed via the C++ grammar.
                ".fx",
                ".fxh",
                ".addonfx",
                ".hlsl",
                ".hlsli",
            };
            return ok.count(e) != 0;
        };
        auto skipDir = [](const std::string &n) {
            // NOTE: deps/vendor are intentionally NOT skipped — dependency SOURCE
            // (ImGui, glm, etc.) should be indexed so its symbols resolve for
            // go-to-def + autocomplete. Still skip build output, VCS, node_modules
            // (huge/minified), NuGet packages (compiled), and Backup folders.
            std::string l = n;
            std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            // Unreal Engine generates enormous non-source trees; never index them.
            if (n == "Intermediate" || n == "Binaries" || n == "Saved" ||
                n == "DerivedDataCache" || n == "Build")
                return true;
            // cmake-build-debug / cmake-build-release / cmake-build-* (CLion, etc.)
            if (l.rfind("cmake-build", 0) == 0)
                return true;
            return n == ".git" || n == ".svn" || n == ".hg" || n == "node_modules" ||
                   n == "bin" || n == "obj" || n == "out" || n == "build" ||
                   n == "target" || n == ".vs" || n == ".vscode" || n == ".idea" ||
                   n == "__pycache__" || n == "packages" ||
                   n == ".cache" || n == ".gradle" || n == "dist" || n == "coverage" ||
                   n == ".next" || n == ".nuxt" || n == ".tox" ||
                   n == "venv" || n == ".venv" ||
                   n == "Backup" || n == "backup" || n == "Backups" || n == "backups";
        };
        static const std::unordered_set<std::string> defKw = {
            "class",
            "struct",
            "interface",
            "enum",
            "record",
            "namespace",
            "trait",
            "typedef",
            "type",
            "def",
            "fn",
            "function",
            "module",
            "protocol",
            "actor",
            "union",
        };
        auto isIdentStart = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
        auto isIdent = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };

        // Build (re)runs in a loop: if a save lands mid-build it sets
        // rebuildRequested, and we run one more pass with a fresh gen so the edit
        // is indexed. try/catch guarantees `building` is reset even on a throw —
        // otherwise a single failed pass would wedge the index for the session.
        int curGen = gen;
        for (;;)
        {
        try
        {
        auto idx = std::make_shared<ProjectIndex>();
        std::unordered_set<std::string> idset;
        std::unordered_map<std::string, ts::FileSyms> newCache;   // this pass's cache (-> disk)
        // Fold a file's tree-sitter symbols into the index aggregates.
        auto aggregate = [&](const std::string &fpath, const std::vector<ts::Symbol> &syms) {
            for (auto &sym : syms)
            {
                idset.insert(sym.name);
                if (!sym.isDefinition)
                    continue;
                auto &dv = idx->tsDefs[sym.name];
                if (dv.size() < 32)
                    dv.push_back({fpath, sym.line, sym.kind});
                if (!sym.enclosingType.empty())
                {
                    auto &mv = idx->members[sym.enclosingType];
                    if (mv.size() < 256)
                        mv.push_back(sym.name);
                }
            }
            if (!syms.empty())
                idx->fileSymbols[fpath] = syms;
        };
        // Fold a file's per-type member types into the project-wide map (cross-file chains).
        auto mergeMemberTypes = [&](const ts::MemberTypeMap &mt) {
            for (auto &tkv : mt)
                for (auto &mkv : tkv.second)
                    idx->memberTypes[tkv.first][mkv.first] = mkv.second;
        };
        std::error_code ec;
        int budget = 120000;   // covers large SDK trees (UE/UEVR); the build is
                               // background-threaded + mtime-cached so a big walk
                               // doesn't stall the UI or re-parse unchanged files

        // Scan the project root first, then each extra source location (a shared
        // budget caps the total walk). Duplicate files across roots collapse in the
        // aggregate/cache maps, so overlapping roots are harmless.
        std::vector<std::filesystem::path> scanRoots;
        scanRoots.push_back(root);
        for (auto &e : extraSnap)
            scanRoots.push_back(std::filesystem::path(e));

        for (auto &scanRoot : scanRoots)
        {
            if (budget < 0)
                break;
            std::error_code rootEc;
            if (!std::filesystem::is_directory(scanRoot, rootEc))
                continue;

        for (auto it = std::filesystem::recursive_directory_iterator(
                 scanRoot, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (curGen != st->gen.load())
            {
                st->building = false;
                return;
            } // superseded
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (it->is_directory(ec))
            {
                if (skipDir(it->path().filename().string()) || isExcluded(it->path()))
                    it.disable_recursion_pending();
                continue;
            }
            if (--budget < 0)
                break;
            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (!extOk(ext))
                continue;
            if (isExcluded(it->path()))
                continue;   // user hid this file from the nav tree → keep it out of the index

            std::string fileStr = it->path().string();
            bool tsLang = ts::langForExtension(ext) != ts::Lang::None;

            // mtime + size key the cache staleness check.
            std::error_code mec, sec;
            auto wtime = std::filesystem::last_write_time(it->path(), mec);
            long long mtime = mec ? 0 : (long long) wtime.time_since_epoch().count();
            std::uintmax_t fsz = std::filesystem::file_size(it->path(), sec);
            unsigned long long fsize = sec ? 0 : (unsigned long long) fsz;

            // Incremental: an unchanged ts file reuses cached symbols and skips the
            // (expensive) read + parse entirely. Non-ts files always re-tokenize
            // (their identifiers aren't cached).
            if (tsLang && mtime != 0)
            {
                auto cit = oldCache.find(fileStr);
                if (cit != oldCache.end() && cit->second.mtime == mtime && cit->second.size == fsize)
                {
                    aggregate(fileStr, cit->second.symbols);
                    mergeMemberTypes(cit->second.memberTypes);
                    newCache.emplace(fileStr, cit->second);
                    continue;
                }
            }

            std::ifstream f(it->path(), std::ios::binary);
            if (!f.is_open())
                continue;
            std::string whole((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();

            // Tree-sitter pass (C/C++/C#/…): accurate, kind-ranked definition sites +
            // per-type member names. Skip very large files so a generated mega-header
            // can't stall the background build.
            bool tsFile = tsLang;
            if (tsFile && whole.size() > 1500000)
                tsFile = false;
            if (tsFile)
            {
                ts::Lang flang = ts::langForExtension(ext);
                auto syms = ts::extractSymbols(flang, whole);
                auto mtypes = ts::extractMemberTypes(flang, whole);   // {} for non-C++/C#
                aggregate(fileStr, syms);
                mergeMemberTypes(mtypes);
                newCache.emplace(fileStr, ts::FileSyms{mtime, fsize, std::move(syms), std::move(mtypes)});
            }

            // Identifier + heuristic-definition tokenization. Heuristic def sites are
            // suppressed for ts files — tsDefs is authoritative there. Move the buffer
            // in (the ts pass above was its last reader) to avoid a duplicate copy.
            std::istringstream ss(std::move(whole));
            std::string line;
            int lineNo = 0;
            while (std::getline(ss, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                int curLine = lineNo++;
                size_t n = line.size();
                size_t lead = 0;
                while (lead < n && (line[lead] == ' ' || line[lead] == '\t'))
                    ++lead;
                bool hashLine = lead < n && line[lead] == '#';
                // A comment line still contributes its identifiers (autocomplete
                // wants them) but must NOT register definition sites — otherwise a
                // commented-out foreign snippet (C++ "-- struct FVector ..." in a
                // Lua file) indexes as a real def and Go-to-Definition jumps to it.
                bool commentLine = false;
                if (lead < n)
                {
                    char c0 = line[lead];
                    char c1 = (lead + 1 < n) ? line[lead + 1] : '\0';
                    commentLine = (c0 == '/' && c1 == '/') || (c0 == '-' && c1 == '-') || c0 == ';' || c0 == '*';
                }

                std::string prevTok;
                size_t i = 0;
                bool firstTok = true;
                while (i < n)
                {
                    if (!isIdentStart(line[i]))
                    {
                        ++i;
                        continue;
                    }
                    size_t s2 = i;
                    while (i < n && isIdent(line[i]))
                        ++i;
                    std::string tok = line.substr(s2, i - s2);
                    idset.insert(tok);

                    int score = 0;
                    if (commentLine)
                        score = 0; // comment → never a def site
                    else if (defKw.count(prevTok))
                        score = 100; // class/def/... NAME
                    else if (hashLine && prevTok == "define")
                        score = 80; // #define NAME
                    else
                    {
                        size_t p = i;
                        while (p < n && line[p] == ' ')
                            ++p;
                        // `type NAME(` — a signature, but ONLY when a real type token
                        // precedes NAME. Without this, a constructor call / usage like
                        // `return ImVec4(` or `= Foo(` indexes the USE site as a def.
                        static const std::unordered_set<std::string> notType = {
                            "return",
                            "new",
                            "delete",
                            "sizeof",
                            "throw",
                            "case",
                            "co_return",
                            "co_await",
                            "and",
                            "or",
                            "not",
                            "if",
                            "while",
                            "for",
                            "switch",
                            "do",
                            "else",
                        };
                        bool typeBefore = !prevTok.empty() && !notType.count(prevTok);
                        if (p < n && line[p] == '(' && !firstTok && typeBefore)
                            score = 50; // type NAME(
                        else if (firstTok && s2 == lead)
                        { // NAME = at col0
                            size_t q = i;
                            while (q < n && line[q] == ' ')
                                ++q;
                            if (q < n && line[q] == '=' && (q + 1 >= n || line[q + 1] != '='))
                                score = 30;
                        }
                    }
                    if (score > 0 && !tsFile)
                    {
                        auto &v = idx->defs[tok];
                        if (v.size() < 32)
                            v.push_back({fileStr, curLine, score}); // cap per symbol
                    }
                    prevTok = tok;
                    firstTok = false;
                }
            }
        }
        } // end for each scanRoot

        idx->identifiers.assign(idset.begin(), idset.end());
        std::sort(idx->identifiers.begin(), idx->identifiers.end());

        if (curGen == st->gen.load())
        {
            // Persist the cache to disk (outside the lock — it's file I/O), then
            // publish the index + keep the cache for the next incremental pass.
            if (!cachePath.empty())
                ts::writeIndexCache(cachePath, newCache);
            std::lock_guard<std::mutex> lock(st->mutex);
            st->index = idx;
            st->cache = std::move(newCache);
        }
        }                              // end try
        catch (...) {}                 // never leave `building` stuck true on a throw
        if (!st->rebuildRequested.exchange(false))
            break;                     // no save landed during this pass → done
        curGen = ++st->gen;            // a save did → one more pass, fresh gen
        }                              // end for
        st->building = false; })
        .detach();
}

// ── Hover hints + Find References (imgui-bundle style) ──────────────

void Editor::renderHoverTooltip(TabDocument &t)
{
    // Track mouse idleness over the editor window. When the cursor sits
    // still on the same word for hoverDelaySec, pop a tooltip with the
    // symbol info: known/unknown, count of references in the doc, and the
    // best-effort definition line via the existing scoring heuristic.
    ImVec2 mp = ImGui::GetMousePos();
    bool stillOnSameSpot =
        std::abs(mp.x - hoverPos.x) < 2.0f && std::abs(mp.y - hoverPos.y) < 2.0f;
    if (!stillOnSameSpot)
    {
        hoverPos = mp;
        hoverIdleSec = 0.0f;
        hoverWord.clear();
    }
    else
    {
        hoverIdleSec += ImGui::GetIO().DeltaTime;
    }
    if (hoverIdleSec < hoverDelaySec)
        return;

    std::string hext = std::filesystem::path(t.filename).extension().string();
    std::transform(hext.begin(), hext.end(), hext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    bool lspOn = lspActiveForExt(hext);

    if (hoverWord.empty())
    {
        hoverWord = t.editor.GetWordAtScreenPos(mp);
        if (hoverWord.empty())
        {
            hoverIdleSec = 0.0f;
            return;
        }
        // Freshly hovered a word → ask clangd for its type/docs (async; shown when
        // it lands via pollLsp). Cleared here so a stale reply can't bleed across.
        lspHoverText.clear();
        lspHoverId = 0;
        if (lspOn)
        {
            int hl = 0, hb = 0;
            if (t.editor.GetBytePosAtScreenPos(mp, hl, hb))
            {
                lspSyncDoc(t);
                std::string uri = lspUriForTab(t);
                if (!uri.empty())
                {
                    int id = lspClient.requestHover(uri, hl, hb);
                    if (id)
                        lspHoverId = id;
                }
            }
        }
    }

    // Without clangd, only known identifiers (the trie) get a tooltip; with clangd
    // we show anything it can describe (std::, members, externals…).
    if (!lspOn && !t.trie.contains(hoverWord))
        return;

    // Count occurrences and find the best-scoring definition line.
    int refCount = 0;
    int defLine = -1;
    {
        auto &ed = t.editor;
        int lines = ed.GetLineCount();
        // Quick whole-word scan; cheap enough for an interactive tooltip.
        for (int ln = 0; ln < lines; ++ln)
        {
            auto text = ed.GetLineText(ln);
            size_t pos = 0;
            while ((pos = text.find(hoverWord, pos)) != std::string::npos)
            {
                bool leftOk = (pos == 0) || (!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_');
                size_t end = pos + hoverWord.size();
                bool rightOk = (end >= text.size()) || (!std::isalnum(static_cast<unsigned char>(text[end])) && text[end] != '_');
                if (leftOk && rightOk)
                {
                    ++refCount;
                    if (defLine < 0 && text.find('{', end) != std::string::npos)
                        defLine = ln;
                }
                pos = end;
            }
        }
    }
    ImGui::BeginTooltip();
    if (!lspHoverText.empty())
    {
        // clangd's type / signature / docs (real intellisense).
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(lspHoverText.c_str());
        ImGui::PopTextWrapPos();
    }
    else
    {
        ImGui::Text("%s", hoverWord.c_str());
        ImGui::Separator();
        if (defLine >= 0)
            ImGui::TextDisabled("definition near line %d", defLine + 1);
        ImGui::TextDisabled("%d reference%s in file", refCount, refCount == 1 ? "" : "s");
        if (lspOn)
            ImGui::TextDisabled("clangd…");
    }
    ImGui::TextDisabled("(right-click → Find References)");
    ImGui::EndTooltip();
}

// Project-wide Go to Definition. Walks the project tree, grepping each text
// file for patterns that look like a definition of `word`. First file+line
// match wins — opens it and scrolls there. Generic across languages; works
// for C# (class/struct/interface/record/enum/method), C/C++ (struct/class/
// typedef/method/#define), Python (def/class), Lua (function/local), JS/TS
// (function/class/const/let).
void Editor::openCSharpLearn(const std::string &rawSymbol)
{
    // C# SDK types (Console, List<T>, System.Diagnostics.Process, ...) ship as
    // metadata-only reference assemblies -- no .cs source on disk to grep to. So
    // "navigate to an SDK item" means open its Microsoft Learn page. Default to
    // the Learn SEARCH endpoint (resolves for bare names, generics, locals); only
    // deep-link to the API page when the token is rooted in a known BCL namespace.
    std::string sym = rawSymbol;
    if (auto lt = sym.find('<'); lt != std::string::npos)
        sym.erase(lt); // drop generics
    while (!sym.empty() && (sym.back() == ' ' || sym.back() == '.' || sym.back() == '\t'))
        sym.pop_back();
    while (!sym.empty() && (sym.front() == ' ' || sym.front() == '\t'))
        sym.erase(sym.begin());
    if (sym.empty())
        return;

    auto urlEncode = [](const std::string &s) {
        static const char *hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s)
        {
            if (std::isalnum(c) || c == '.' || c == '_' || c == '-')
                out.push_back((char)c);
            else
            {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    };

    auto rootIsBcl = [&]() {
        auto dot = sym.find('.');
        if (dot == std::string::npos)
            return false; // unqualified -> search
        std::string root = sym.substr(0, dot);
        return root == "System" || root == "Microsoft" || root == "Windows" || root == "Internal" || root == "Mono";
    };

    // Resolve the Learn "?view=" moniker so the docs match the version the user
    // actually targets. Priority: (1) the project's <TargetFramework> from a
    // nearby .csproj — the version their code references; (2) the highest .NET
    // runtime installed locally; (3) nothing (Learn then shows its latest).
    auto dotnetView = [this]() -> std::string {
        auto tfmToView = [](std::string tfm) -> std::string {
            std::transform(tfm.begin(), tfm.end(), tfm.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            auto majMinAt = [](const std::string &s, size_t at) -> std::string {
                size_t i = at;
                std::string maj, min;
                while (i < s.size() && std::isdigit((unsigned char)s[i]))
                    maj += s[i++];
                if (i < s.size() && s[i] == '.')
                {
                    ++i;
                    while (i < s.size() && std::isdigit((unsigned char)s[i]))
                        min += s[i++];
                }
                if (maj.empty())
                    return "";
                return maj + "." + (min.empty() ? "0" : min);
            };
            if (tfm.rfind("netstandard", 0) == 0)
            {
                auto v = majMinAt(tfm, 11);
                return v.empty() ? "" : "netstandard-" + v;
            }
            if (tfm.rfind("netcoreapp", 0) == 0)
            {
                auto v = majMinAt(tfm, 10);
                return v.empty() ? "" : "net-" + v;
            }
            if (tfm.rfind("net", 0) == 0 && tfm.size() > 3 && std::isdigit((unsigned char)tfm[3]))
            {
                if (tfm.find('.') != std::string::npos)
                {
                    auto v = majMinAt(tfm, 3);
                    return v.empty() ? "" : "net-" + v;
                }
                std::string d = tfm.substr(3); // net48 -> netframework-4.8
                if (d.size() >= 2)
                    return "netframework-" + std::string(1, d[0]) + "." + d.substr(1);
            }
            return "";
        };
        std::error_code ec;
        std::filesystem::path root = projectRoot;
        if (root.empty() && !tabs.empty() && doc().filename != "untitled")
            root = std::filesystem::path(doc().filename).parent_path();
        // 1. nearest .csproj's TargetFramework, walking up a few levels.
        std::filesystem::path cur = root;
        for (int up = 0; !cur.empty() && up < 5; ++up)
        {
            for (auto &e : std::filesystem::directory_iterator(cur, ec))
            {
                if (ec)
                    break;
                if (!e.is_regular_file(ec))
                    continue;
                auto ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (ext != ".csproj")
                    continue;
                std::ifstream f(e.path());
                std::stringstream ss;
                ss << f.rdbuf();
                std::string text = ss.str();
                auto tag = [&](const std::string &t) -> std::string {
                    auto a = text.find("<" + t + ">");
                    if (a == std::string::npos)
                        return "";
                    a += t.size() + 2;
                    auto b = text.find("</" + t + ">", a);
                    return b == std::string::npos ? "" : text.substr(a, b - a);
                };
                std::string tfm = tag("TargetFramework");
                if (tfm.empty())
                {
                    std::string m = tag("TargetFrameworks");
                    auto sc = m.find(';');
                    tfm = (sc == std::string::npos) ? m : m.substr(0, sc);
                }
                while (!tfm.empty() && (tfm.back() == ' ' || tfm.back() == '\r' || tfm.back() == '\n' || tfm.back() == '\t'))
                    tfm.pop_back();
                while (!tfm.empty() && (tfm.front() == ' ' || tfm.front() == '\t'))
                    tfm.erase(tfm.begin());
                auto view = tfmToView(tfm);
                if (!view.empty())
                    return view;
            }
            if (cur.has_parent_path() && cur.parent_path() != cur)
                cur = cur.parent_path();
            else
                break;
        }
#ifdef _WIN32
        // 2. highest installed runtime under the shared framework folder.
        std::filesystem::path shared = "C:/Program Files/dotnet/shared/Microsoft.NETCore.App";
        int bestMaj = -1, bestMin = -1;
        for (auto &e : std::filesystem::directory_iterator(shared, ec))
        {
            if (ec)
                break;
            if (!e.is_directory(ec))
                continue;
            std::string nm = e.path().filename().string();
            size_t i = 0;
            std::string a, b;
            while (i < nm.size() && std::isdigit((unsigned char)nm[i]))
                a += nm[i++];
            if (i < nm.size() && nm[i] == '.')
            {
                ++i;
                while (i < nm.size() && std::isdigit((unsigned char)nm[i]))
                    b += nm[i++];
            }
            if (a.empty() || b.empty())
                continue;
            int maj = std::atoi(a.c_str()), min = std::atoi(b.c_str());
            if (maj > bestMaj || (maj == bestMaj && min > bestMin))
            {
                bestMaj = maj;
                bestMin = min;
            }
        }
        if (bestMaj >= 0)
            return "net-" + std::to_string(bestMaj) + "." + std::to_string(bestMin);
#endif
        return "";
    };

    std::string url;
    if (rootIsBcl())
    {
        std::string lower = sym;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        url = "https://learn.microsoft.com/en-us/dotnet/api/" + urlEncode(lower);
        std::string view = dotnetView();
        if (!view.empty())
            url += "?view=" + view;
    }
    else
    {
        url = "https://learn.microsoft.com/en-us/search/?terms=" + urlEncode(sym) + "&category=Reference";
    }

#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    [[maybe_unused]] int rc = std::system(("open \"" + url + "\"").c_str());
#else
    [[maybe_unused]] int rc = std::system(("xdg-open \"" + url + "\"").c_str());
#endif
}

// Decompile a BCL type to real C# via ilspycmd and open it read-only. The whole
// pipeline (tool auto-install, DLL resolution, decompile) runs on a detached
// thread; pollDecompile() opens the result on the UI thread. On any failure the
// state's `error` is set and pollDecompile falls back to the Learn page.
//
// Resolution (verified against ilspycmd 10.x): most System.* types are FORWARDED
// — they aren't in their namesake runtime DLL but in System.Private.CoreLib.
// ilspycmd reports this as "... was not found in the module being decompiled,
// but only in <X>", so we try a best-guess DLL first and, on that error, parse
// the named module and retry once.
void Editor::openCSharpDecompiled(const std::string &rawSymbol)
{
#ifdef _WIN32
    if (decompileState->running.load())
        return; // one at a time

    // Last segment + generic arity: "System.Collections.Generic.List<int>" ->
    // fullName "System.Collections.Generic.List`1" for ilspycmd's -t.
    std::string full = rawSymbol;
    {
        // strip a generic argument list but remember its arity
        int arity = 0;
        if (auto lt = full.find('<'); lt != std::string::npos)
        {
            // count top-level commas + 1 for the arg count
            int depth = 0;
            arity = 1;
            for (size_t i = lt; i < full.size(); ++i)
            {
                if (full[i] == '<')
                    ++depth;
                else if (full[i] == '>')
                {
                    if (--depth == 0)
                        break;
                }
                else if (full[i] == ',' && depth == 1)
                    ++arity;
            }
            full = full.substr(0, lt);
        }
        while (!full.empty() && (full.back() == ' ' || full.back() == '.' || full.back() == '\t'))
            full.pop_back();
        while (!full.empty() && (full.front() == ' ' || full.front() == '\t'))
            full.erase(full.begin());
        if (arity > 0)
            full += "`" + std::to_string(arity);
    }
    if (full.empty())
    {
        decompileState->error = "empty symbol";
        return;
    }

    auto st = decompileState;
    st->running = true;
    st->done = false;
    st->published = true; // nothing to publish yet
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->symbol = full;
        st->resultPath.clear();
        st->error.clear();
    }
    std::filesystem::path cacheDir = userConfigDir() / "decompiled";

    std::thread([st, full, cacheDir]() {
        auto fail = [&](const std::string &msg) {
            std::lock_guard<std::mutex> lk(st->mutex);
            st->error = msg;
            st->done = true;
            st->published = false;
            st->running = false;
        };
        std::error_code ec;

        // ilspycmd path; auto-install if missing.
        std::filesystem::path home = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "";
        std::filesystem::path ilspy = home / ".dotnet" / "tools" / "ilspycmd.exe";
        if (!std::filesystem::is_regular_file(ilspy, ec))
        {
            (void)std::system("dotnet tool install -g ilspycmd >NUL 2>&1");
            if (!std::filesystem::is_regular_file(ilspy, ec))
            {
                fail("ilspycmd not available (install failed)");
                return;
            }
        }

        // Highest installed runtime dir.
        std::filesystem::path shared = "C:/Program Files/dotnet/shared/Microsoft.NETCore.App";
        std::filesystem::path rtDir;
        int bestMaj = -1, bestMin = -1, bestPatch = -1;
        for (auto &e : std::filesystem::directory_iterator(shared, ec))
        {
            if (ec)
                break;
            if (!e.is_directory(ec))
                continue;
            std::string nm = e.path().filename().string();
            int v[3] = {0, 0, 0};
            size_t i = 0;
            for (int part = 0; part < 3 && i < nm.size(); ++part)
            {
                std::string num;
                while (i < nm.size() && std::isdigit((unsigned char)nm[i]))
                    num += nm[i++];
                if (i < nm.size() && nm[i] == '.')
                    ++i;
                v[part] = num.empty() ? 0 : std::atoi(num.c_str());
            }
            if (v[0] > bestMaj || (v[0] == bestMaj && (v[1] > bestMin || (v[1] == bestMin && v[2] > bestPatch))))
            {
                bestMaj = v[0];
                bestMin = v[1];
                bestPatch = v[2];
                rtDir = e.path();
            }
        }
        if (rtDir.empty())
        {
            fail("no .NET runtime found");
            return;
        }

        // Plain type name (no arity) for cache filename + a first DLL guess.
        std::string plain = full;
        if (auto bt = plain.find('`'); bt != std::string::npos)
            plain = plain.substr(0, bt);

        // Run ilspycmd <dll> -r <runtimeDir> -t <full>, capture stdout+stderr.
        // -r puts the whole runtime directory on the resolver path so dependent
        // assemblies (System.Diagnostics, etc.) load into the type system —
        // without it ilspycmd throws "Could not find type definition ...".
        auto runIlspy = [&](const std::filesystem::path &dll, std::string &out) -> int {
            std::string cmd = "\"" + ilspy.string() + "\" \"" + dll.string() +
                              "\" -r \"" + rtDir.string() + "\" -t \"" + full + "\" 2>&1";
            FILE *p = _popen(("\"" + cmd + "\"").c_str(), "r");
            if (!p)
                return -1;
            char buf[4096];
            out.clear();
            while (fgets(buf, sizeof(buf), p))
                out += buf;
            return _pclose(p);
        };

        // First guess: a DLL named after the namespace (works for non-forwarded
        // types like System.Diagnostics.Process). Fall back to CoreLib if the
        // guessed DLL doesn't exist.
        std::filesystem::path guess = rtDir / (plain.substr(0, plain.find_last_of('.')) + ".dll");
        if (plain.find('.') == std::string::npos || !std::filesystem::is_regular_file(guess, ec))
            guess = rtDir / "System.Private.CoreLib.dll";

        std::string out;
        runIlspy(guess, out);
        // Forwarded-type retry: "... only in <Module>".
        if (out.find("was not found in the module") != std::string::npos)
        {
            auto k = out.find("but only in ");
            if (k != std::string::npos)
            {
                std::string mod = out.substr(k + 12);
                // take up to end-of-line / whitespace
                size_t e2 = mod.find_first_of("\r\n \t");
                if (e2 != std::string::npos)
                    mod = mod.substr(0, e2);
                if (!mod.empty())
                {
                    std::filesystem::path dll2 = rtDir / (mod + ".dll");
                    if (std::filesystem::is_regular_file(dll2, ec))
                        runIlspy(dll2, out);
                }
            }
        }

        // Validate: output must be real C#, not an error or an ilspycmd stack
        // trace (it can exit 0 while printing an exception for the type).
        if (out.find("was not found") != std::string::npos ||
            out.find("Could not find type definition") != std::string::npos ||
            out.find("Exception:") != std::string::npos ||
            out.find("   at ICSharpCode.") != std::string::npos ||
            out.find(plain.substr(plain.find_last_of('.') + 1)) == std::string::npos ||
            out.size() < 40)
        {
            fail("could not decompile " + full);
            return;
        }

        // Cache to <config>/decompiled/<Type>.cs (sanitize the name).
        std::filesystem::create_directories(cacheDir, ec);
        std::string fname = plain;
        for (auto &c : fname)
            if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' || c == '`')
                c = '_';
        std::filesystem::path outPath = cacheDir / (fname + ".cs");
        {
            std::ofstream f(outPath, std::ios::binary);
            if (!f)
            {
                fail("cannot write cache file");
                return;
            }
            f << "// Decompiled from " << rtDir.filename().string()
              << " by ilspycmd — read-only. Not original source.\n\n"
              << out;
        }
        std::lock_guard<std::mutex> lk(st->mutex);
        st->resultPath = outPath.string();
        st->done = true;
        st->published = false;
        st->running = false; })
        .detach();
#else
    (void)rawSymbol;
#endif
}

void Editor::pollDecompile()
{
    if (decompileState->published.load())
        return;
    std::string path, err, sym;
    {
        std::lock_guard<std::mutex> lk(decompileState->mutex);
        path = decompileState->resultPath;
        err = decompileState->error;
        sym = decompileState->symbol;
    }
    decompileState->published = true;
    if (!err.empty() || path.empty())
    {
        // Fall back to the Learn page — better than nothing.
        showError("Decompile failed for '" + sym + "': " + (err.empty() ? "unknown" : err) + "\nFalling back to Microsoft Learn.");
        openCSharpLearn(sym);
        return;
    }
    openFile(path);
    if (!tabs.empty())
        doc().editor.SetReadOnlyEnabled(true);
}

// Test a chord string ("Ctrl+Shift+N", "Alt+O", "F6") against the live key
// state this frame: EXACT modifier set (so Ctrl+G doesn't also fire on
// Ctrl+Shift+G) plus the named key just pressed. The key name is matched via
// ImGui::GetKeyName so it round-trips with the recorder, which builds chords
// the same way. Multi-stroke chords (containing a space, e.g. "Ctrl+K Ctrl+U")
// are widget-internal and intentionally not handled here.
// Map a single key token to the name ImGui::GetKeyName returns. Chord strings
// are authored with glyphs ("=", "-", "\\", "/", "[", "]", ";", "'", ",", ".",
// "`") but GetKeyName yields words ("Equal", "Minus", "Backslash", …). Without
// this, every punctuation binding silently fails to match — which is what broke
// Ctrl+= / Ctrl+- / Ctrl+\ (zoom in/out, split right). Letter/digit/F-key/named
// tokens already match GetKeyName verbatim, so they pass through unchanged.
static std::string normalizeKeyToken(const std::string &tok)
{
    if (tok == "=")
        return "Equal";
    if (tok == "-")
        return "Minus";
    if (tok == "\\")
        return "Backslash";
    if (tok == "/")
        return "Slash";
    if (tok == "[")
        return "LeftBracket";
    if (tok == "]")
        return "RightBracket";
    if (tok == ";")
        return "Semicolon";
    if (tok == "'")
        return "Apostrophe";
    if (tok == ",")
        return "Comma";
    if (tok == ".")
        return "Period";
    if (tok == "`")
        return "GraveAccent";
    if (tok == "+")
        return "KeypadAdd"; // bare '+' captured from the keypad
    return tok;
}

bool Editor::keyChordPressed(const std::string &chord) const
{
    if (chord.empty())
        return false;

    // Single-combo matcher ("Ctrl+Shift+U"): exact modifier set + named key
    // pressed this frame. Used directly for one-stroke binds and as each half of
    // a two-stroke chord.
    auto matchCombo = [](const std::string &combo) -> bool {
        if (combo.empty())
            return false;
        bool needCtrl = false, needShift = false, needAlt = false, needSuper = false;
        std::string keyName;
        size_t pos = 0;
        while (pos < combo.size())
        {
            size_t plus = combo.find('+', pos);
            std::string tok = (plus == std::string::npos) ? combo.substr(pos)
                                                          : combo.substr(pos, plus - pos);
            if (tok == "Ctrl")
                needCtrl = true;
            else if (tok == "Shift")
                needShift = true;
            else if (tok == "Alt")
                needAlt = true;
            else if (tok == "Super")
                needSuper = true;
            else if (!tok.empty())
                keyName = tok;
            if (plus == std::string::npos)
                break;
            pos = plus + 1;
            if (pos < combo.size() && combo[pos] == '+')
            {
                keyName = "+";
                break;
            }
        }
        if (keyName.empty())
            return false;
        keyName = normalizeKeyToken(keyName);
        ImGuiIO &io = ImGui::GetIO();
        if (io.KeyCtrl != needCtrl || io.KeyShift != needShift ||
            io.KeyAlt != needAlt || io.KeySuper != needSuper)
            return false;
        for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END;
             k = (ImGuiKey)(k + 1))
        {
            const char *n = ImGui::GetKeyName(k);
            if (n && n[0] && keyName == n)
                return ImGui::IsKeyPressed(k, false);
        }
        return false;
    };

    // Two-stroke chord ("Ctrl+K Ctrl+U"): match the first combo to arm a pending
    // prefix, then the second within a short window. keyChordPending holds the
    // SECOND combo we're waiting for (or empty). Decay/cancel handled in
    // tickKeyChordPending(), called once per frame from render().
    auto sp = chord.find(' ');
    if (sp == std::string::npos)
        return matchCombo(chord);

    std::string first = chord.substr(0, sp);
    std::string second = chord.substr(sp + 1);
    if (!keyChordPending.empty() && keyChordPending == second)
    {
        if (matchCombo(second))
        {
            keyChordPending.clear();
            return true;
        }
        return false; // still waiting; tick() expires it
    }
    // Not yet armed: arm when the first combo is pressed. Don't fire this frame.
    if (matchCombo(first))
    {
        keyChordPending = second;
        keyChordPendingAge = 0.0f;
    }
    return false;
}

// Per-frame decay of the pending two-stroke prefix: a chord must complete within
// ~1.2s, and Escape cancels it. Called from render() before shortcut dispatch.
void Editor::tickKeyChordPending()
{
    if (keyChordPending.empty())
        return;
    keyChordPendingAge += ImGui::GetIO().DeltaTime;
    if (keyChordPendingAge > 1.2f || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        keyChordPending.clear();
}

bool Editor::keybindPressed(const char *id, const char *defaultChord) const
{
    auto it = keybindOverrides.find(id);
    const std::string &chord = (it != keybindOverrides.end() && !it->second.empty())
                                   ? it->second
                                   : std::string(defaultChord);
    return keyChordPressed(chord);
}

namespace
{
// Catalogue ids that map to a TextEditor-internal action, paired with the
// widget action id and the editor's DEFAULT chord. Single source of truth
// for pushing overrides into the widget; mirrors the Settings → Keybinds
// catalogue rows whose widgetAction is non-null. Default chord is applied
// when the user has no override, so a rebind cleanly reverts on reset.
struct WidgetBind
{
    const char *id;
    const char *widgetAction;
    const char *defaultChord;
};
static const WidgetBind kWidgetBinds[] = {
    {"edit.undo", "undo", "Ctrl+Z"},
    {"edit.redo", "redo", "Ctrl+Y"},
    {"edit.cut", "cut", "Ctrl+X"},
    {"edit.copy", "copy", "Ctrl+C"},
    {"edit.paste", "paste", "Ctrl+V"},
    {"edit.selAll", "selectAll", "Ctrl+A"},
    {"edit.addOcc", "addNextOccurrence", "Ctrl+D"},
    {"edit.indent", "indent", "Ctrl+]"},
    {"edit.deindent", "deindent", "Ctrl+["},
    {"edit.comment", "toggleComments", "Ctrl+/"},
    {"edit.selAllOcc", "selectAllOccurrences", "Ctrl+Shift+D"},
    {"edit.moveUp", "moveLineUp", "Alt+UpArrow"},
    {"edit.moveDown", "moveLineDown", "Alt+DownArrow"},
    {"find.find", "find", "Ctrl+F"},
    {"find.next", "findNext", "F3"},
    {"find.findAll", "findAll", "Ctrl+Shift+G"},
    {"code.foldAll", "foldAll", "Ctrl+0"},
    {"code.unfoldAll", "unfoldAll", "Ctrl+J"},
    {"code.foldCur", "foldCurrent", "Ctrl+Shift+["},
    {"code.unfoldCur", "unfoldCurrent", "Ctrl+Shift+]"},
    // upperCase/lowerCase default to two-stroke Ctrl+K chords, which the
    // widget's single-combo override can't express. Only push an override
    // when the user actually rebinds them to a single chord; otherwise leave
    // the widget's built-in Ctrl+K Ctrl+U / Ctrl+K Ctrl+L default in place.
    {"code.upper", "upperCase", ""},
    {"code.lower", "lowerCase", ""},
};
} // namespace

void Editor::applyKeybindOverridesToEditor(TextEditor &ed) const
{
    for (auto &wb : kWidgetBinds)
    {
        auto it = keybindOverrides.find(wb.id);
        bool haveOverride = (it != keybindOverrides.end() && !it->second.empty());
        // Push the override when set; otherwise push the default (or clear, for
        // the two-stroke-default actions whose defaultChord is empty).
        std::string chord = haveOverride ? it->second : std::string(wb.defaultChord);
        ed.SetKeyChordOverride(wb.widgetAction, chord);
    }
}

void Editor::applyKeybindOverridesToEditors()
{
    for (auto &tp : tabs)
        applyKeybindOverridesToEditor(tp->editor);
}

// Discover the MSVC toolchain + Windows SDK include directories WITHOUT relying
// on a Developer-Command-Prompt environment (%INCLUDE% etc.). Sources, in order:
//   1. %INCLUDE% / %VCToolsInstallDir% if present (free when launched from a dev
//      prompt or build.ps1 -Run);
//   2. vswhere.exe -> VS install -> newest VC\Tools\MSVC\<ver>\include;
//   3. well-known "Program Files\Microsoft Visual Studio\<year>\<edition>" glob;
//   4. Windows SDK: registry KitsRoot10 (or well-known) -> newest
//      Include\<ver>\{ucrt,um,shared}.
// Cached after the first call (the vswhere subprocess + dir walks run once).
const std::vector<std::filesystem::path> &Editor::systemIncludeDirs()
{
    if (sysIncludeComputed_)
        return sysIncludeDirs_;
    sysIncludeComputed_ = true;
    std::error_code ec;
    auto add = [&](std::filesystem::path p) {
        if (p.empty())
            return;
        if (!std::filesystem::is_directory(p, ec))
            return;
        for (auto &e : sysIncludeDirs_)
            if (e == p)
                return; // dedupe
        sysIncludeDirs_.push_back(std::move(p));
    };
    auto newestSubdir = [&](const std::filesystem::path &base, const char *mustHave) -> std::filesystem::path {
        std::filesystem::path best;
        for (auto it = std::filesystem::directory_iterator(base, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            if (!it->is_directory(ec))
                continue;
            if (mustHave && !std::filesystem::is_directory(it->path() / mustHave, ec))
                continue;
            if (best.empty() || it->path().filename().string() > best.filename().string())
                best = it->path();
        }
        return best;
    };

    // 1. Environment (dev-cmd launch) — fast path.
    if (const char *inc = std::getenv("INCLUDE"))
    {
        std::string s = inc, part;
        for (char ch : s)
        {
            if (ch == ';')
            {
                if (!part.empty())
                    add(part);
                part.clear();
            }
            else
                part += ch;
        }
        if (!part.empty())
            add(part);
    }
    if (const char *vct = std::getenv("VCToolsInstallDir"))
        add(std::filesystem::path(vct) / "include");

    auto haveMsvc = [&] {
        for (auto &d : sysIncludeDirs_)
            if (d.string().find("\\VC\\Tools\\MSVC\\") != std::string::npos)
                return true;
        return false;
    };

    // 2/3. Locate the VS install and pick the newest MSVC toolchain include dir.
    if (!haveMsvc())
    {
        std::filesystem::path vs;
        if (const char *pf86 = std::getenv("ProgramFiles(x86)"))
        {
            std::filesystem::path vsw = std::filesystem::path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
            if (std::filesystem::is_regular_file(vsw, ec))
            {
                std::string cmd = "\"\"" + vsw.string() + "\" -latest -property installationPath\"";
                if (FILE *p = _popen(cmd.c_str(), "r"))
                {
                    char buf[1024];
                    std::string out;
                    while (fgets(buf, sizeof(buf), p))
                        out += buf;
                    _pclose(p);
                    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
                        out.pop_back();
                    if (!out.empty())
                        vs = out;
                }
            }
        }
        if (vs.empty())
        { // well-known fallback glob
            for (const char *pf : {std::getenv("ProgramFiles"), std::getenv("ProgramFiles(x86)")})
            {
                if (!pf)
                    continue;
                std::filesystem::path base = std::filesystem::path(pf) / "Microsoft Visual Studio";
                if (!std::filesystem::is_directory(base, ec))
                    continue;
                for (auto y = std::filesystem::directory_iterator(base, ec);
                     !ec && y != std::filesystem::directory_iterator(); y.increment(ec))
                {
                    if (!y->is_directory(ec))
                        continue;
                    for (auto e = std::filesystem::directory_iterator(y->path(), ec);
                         !ec && e != std::filesystem::directory_iterator(); e.increment(ec))
                    {
                        if (e->is_directory(ec) && std::filesystem::is_directory(e->path() / "VC" / "Tools" / "MSVC", ec))
                            vs = e->path();
                    }
                }
            }
        }
        if (!vs.empty())
        {
            auto ver = newestSubdir(vs / "VC" / "Tools" / "MSVC", "include");
            if (!ver.empty())
                add(ver / "include");
        }
    }

    // 4. Windows SDK (covers <windows.h>, CRT headers). KitsRoot10 from registry.
    //    Windows-only: the registry + Windows-SDK layout don't exist elsewhere.
#ifdef _WIN32
    {
        std::filesystem::path kits;
        char val[1024];
        DWORD sz;
        for (const char *sub : {"SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots",
                                "SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"})
        {
            sz = sizeof(val);
            if (RegGetValueA(HKEY_LOCAL_MACHINE, sub, "KitsRoot10", RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS)
            {
                kits = std::string(val);
                break;
            }
        }
        if (kits.empty())
        {
            std::filesystem::path wk = "C:/Program Files (x86)/Windows Kits/10";
            if (std::filesystem::is_directory(wk, ec))
                kits = wk;
        }
        if (!kits.empty())
        {
            auto ver = newestSubdir(kits / "Include", "ucrt");
            if (!ver.empty())
            {
                add(ver / "ucrt");
                add(ver / "um");
                add(ver / "shared");
            }
        }
    }
#endif

    return sysIncludeDirs_;
}

static int rankKind(ts::Kind k)
{
    switch (k)
    {
    case ts::Kind::Class:
    case ts::Kind::Struct:
    case ts::Kind::Enum:
    case ts::Kind::Type:
        return 5; // types win (the usual go-to-def target)
    case ts::Kind::Method:
    case ts::Kind::Function:
        return 4;
    case ts::Kind::Field:
    case ts::Kind::Constant:
        return 3;
    case ts::Kind::Variable:
        return 2;
    case ts::Kind::Module:
        return 1;
    default:
        return 0;
    }
}

// Go-to-definition via the prebuilt tree-sitter index (accurate for C++/C#).
// Reads the table — no per-jump scan; returns true if it resolved + navigated.
bool Editor::tsGoToDefinition(const std::string &symbol)
{
    if (symbol.empty())
        return false;
    auto pidx = indexSnapshot();
    if (!pidx)
        return false;
    auto it = pidx->tsDefs.find(symbol);
    if (it == pidx->tsDefs.end() || it->second.empty())
        return false;

    std::string bestFile;
    int bestLine = -1;
    int bestRank = -1;
    for (auto &d : it->second)
    {
        int r = rankKind(d.kind);
        if (r > bestRank)
        {
            bestRank = r;
            bestFile = d.file;
            bestLine = d.line;
        }
    }

    if (bestLine < 0)
        return false;

    openFile(bestFile);
    if (!tabs.empty())
    {
        auto &e = doc().editor;
        e.SetCursor(bestLine, 0);
        e.SelectLine(bestLine);
        e.ScrollToLine(bestLine, TextEditor::Scroll::alignMiddle);
    }
    return true;
}

// Go-to-definition within the CURRENT document via a live tree-sitter parse.
// Index-free, so it works for the FIRST jumps right after opening a file (before
// the background index finishes) and for any same-file definition in every
// supported language — directly addressing "go-to-def rarely usable".
bool Editor::tsGoToDefinitionInDoc(const std::string &symbol)
{
    if (symbol.empty() || tabs.empty())
        return false;
    std::string ext = std::filesystem::path(doc().filename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    ts::Lang lang = ts::langForExtension(ext);
    if (lang == ts::Lang::None)
        return false;
    auto syms = ts::extractSymbols(lang, doc().editor.GetText());
    int bestLine = -1, bestRank = -1;
    for (auto &s : syms)
        if (s.isDefinition && s.name == symbol)
        {
            int r = rankKind(s.kind);
            if (r > bestRank)
            {
                bestRank = r;
                bestLine = s.line;
            }
        }
    if (bestLine < 0)
        return false;
    auto &e = doc().editor;
    e.SetCursor(bestLine, 0);
    e.SelectLine(bestLine);
    e.ScrollToLine(bestLine, TextEditor::Scroll::alignMiddle);
    return true;
}

// ── Navigation history (back/forward) ────────────────────────────────────────

NavLocation Editor::currentNavLocation() const
{
    NavLocation l;
    if (tabs.empty())
        return l;
    const TabDocument &t = *tabs[activeTab];
    if (t.filename.empty() || t.filename == "untitled")
        return l;
    l.file = t.filename;
    t.editor.GetCursor(l.line, l.column, 0);
    return l;
}

// Push the pending go-to-def origin onto the history (once per go-to-def). Called
// at each actual jump site — sync (ts/index/grep) and async (LSP reply) — so the
// origin is recorded regardless of which strategy resolved the definition.
void Editor::commitPendingNavJump()
{
    if (!navJumpOriginValid)
        return;
    navHistory.record(navJumpOrigin);
    navJumpOriginValid = false;
}

void Editor::applyNavLocation(const NavLocation &l)
{
    if (l.file.empty())
        return;
    openFile(l.file);
    // If the file is gone (deleted/renamed since it was recorded) openFile leaves
    // the previously-active tab in front — don't move ITS cursor to l's position.
    if (tabs.empty() || doc().filename != l.file)
        return;
    auto &e = doc().editor;
    e.SetCursor(l.line, l.column);
    e.SelectLine(l.line);
    e.ScrollToLine(l.line, TextEditor::Scroll::alignMiddle);
}

void Editor::navigateBack()
{
    NavLocation dst;
    if (navHistory.back(currentNavLocation(), dst))
        applyNavLocation(dst);
}

void Editor::navigateForward()
{
    NavLocation dst;
    if (navHistory.forward(currentNavLocation(), dst))
        applyNavLocation(dst);
}

void Editor::goToDefinitionProjectWide(const std::string &word, bool declaration)
{
    ScopedTimer _t(declaration ? "goToDeclaration" : "goToDefinition");
    if (word.empty())
        return;

    // Snapshot where we are NOW; committed to the back stack at the first real
    // jump below (sync or async LSP) so Back returns to this spot.
    navJumpOrigin = currentNavLocation();
    navJumpOriginValid = navJumpOrigin.valid();

    // LSP (clangd) go-to-definition: fire async at the cursor BEFORE the tree-sitter
    // fallbacks below. The instant ts jump still happens; pollLsp() then navigates to
    // clangd's (often cross-file, more accurate) target when the reply lands.
    if (!declaration && !tabs.empty())
    {
        std::string aext = std::filesystem::path(doc().filename).extension().string();
        std::transform(aext.begin(), aext.end(), aext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (lspActiveForExt(aext))
        {
            std::string uri = lspUriForTab(doc());
            if (!uri.empty())
            {
                lspSyncDoc(doc());
                int line = 0, byteCol = 0;
                doc().editor.GetCursorBytePosition(line, byteCol, 0);
                int id = lspClient.requestDefinition(uri, line, byteCol);
                if (id)
                {
                    lspDefinitionId = id;
                    lspDefLine = line;
                    lspDefCol = byteCol;
                }
            }
        }
    }

    // Qualified names (System.Diagnostics.Process, std::vector, foo->bar): grep
    // for the LAST segment (the type/member name that actually appears at a
    // definition site); keep the full name for messaging + filename matching.
    std::string symbol = word;
    {
        size_t cut = symbol.size();
        for (size_t i = 0; i < symbol.size(); ++i)
        {
            if (symbol[i] == '.')
                cut = i + 1;
            else if (symbol[i] == ':' && i + 1 < symbol.size() && symbol[i + 1] == ':')
                cut = i + 2;
            else if (symbol[i] == '-' && i + 1 < symbol.size() && symbol[i + 1] == '>')
                cut = i + 2;
        }
        if (cut < symbol.size())
            symbol = symbol.substr(cut);
    }
    if (symbol.empty())
        return;

    // Search root: projectRoot if set, else the active doc's directory.
    std::filesystem::path root = projectRoot;
    if (root.empty() && !tabs.empty() && doc().filename != "untitled")
    {
        root = std::filesystem::path(doc().filename).parent_path();
    }
    if (root.empty())
        root = std::filesystem::current_path();

    // Fast path: the prebuilt project index already knows the definition sites.
    // Definition mode only — the index can't tell a declaration (header
    // prototype) from a definition (body), so "Go to Declaration" must fall
    // through to the grep below, which applies the header/body bias. The grep
    // is now fast for declarations too because deps/ is excluded from the walk.
    if (!declaration)
    {
        // Current document first: a live parse is instant and index-free, so the
        // first jumps after opening a file resolve same-file defs without waiting
        // on the background index.
        if (tsGoToDefinitionInDoc(symbol))
        {
            commitPendingNavJump();
            return;
        }

        // Then the prebuilt project index: a real parse beats the grep index for
        // accuracy (this is the fix for C# go-to-def landing on the wrong site).
        if (tsGoToDefinition(symbol))
        {
            commitPendingNavJump();
            return;
        }

        // For a C# document, tree-sitter is authoritative over the project's own
        // source. A miss means the symbol isn't defined here (a NuGet/BCL type
        // like Newtonsoft.Json) — DON'T fall through to the grep, which matches
        // unrelated files (e.g. a C++ json-forwards.h in a Backup folder).
        if (!tabs.empty())
        {
            std::string aext = std::filesystem::path(doc().filename).extension().string();
            std::transform(aext.begin(), aext.end(), aext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (aext == ".cs")
            {
                pushToast("No project definition for '" + word + "' (external/NuGet type?)", IM_COL32(240, 200, 90, 255));
                return;
            }
        }

        if (auto idx = indexSnapshot())
        {
            auto it = idx->defs.find(symbol);
            if (it != idx->defs.end() && !it->second.empty())
            {
                const DefSite *best = nullptr;
                for (auto &d : it->second)
                    if (!best || d.score > best->score)
                        best = &d;
                // Only trust a STRONG index hit (a real definition keyword, score
                // 100). A weak hit (heuristic `type NAME(` / `NAME =`, score < 100)
                // can be a usage mis-scored as a def, so fall through to the grep +
                // deps-fallback below, which scores more carefully and can reach
                // bundled libraries.
                if (best && best->score >= 100)
                {
                    commitPendingNavJump();
                    openFile(best->file);
                    if (!tabs.empty())
                    {
                        auto &e = doc().editor;
                        e.SetCursor(best->line, 0);
                        e.SelectLine(best->line); // highlight the whole line, not just the gutter
                        e.ScrollToLine(best->line, TextEditor::Scroll::alignMiddle);
                    }
                    return;
                }
            }
        }
    }

    // Definition patterns — checked in order; first match wins. Keep these
    // language-agnostic where possible.
    const std::vector<std::string> defKeywords = {
        "class",
        "struct",
        "interface",
        "enum",
        "record",
        "namespace",
        "trait",
        "typedef",
        "type",
        "def",
        "fn",
        "function",
        "module",
        "protocol",
        "actor",
    };

    // File extensions worth scanning — source-y, excludes binaries and
    // build / vcs / cache dirs.
    auto extOk = [](const std::string &e) {
        static const std::unordered_set<std::string> ok = {
            ".c", ".h", ".cpp", ".hpp", ".cxx", ".hxx", ".cc", ".hh",
            ".m", ".mm", ".inl",
            ".cs", ".vb", ".fs", ".fsx", ".fxh", ".fx", ".hlsl", ".glsl",
            ".java", ".kt", ".kts", ".scala", ".groovy",
            ".py", ".pyw", ".rb", ".php", ".pl",
            ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs",
            ".go", ".rs", ".swift",
            ".lua", ".sh", ".ps1", ".psm1",
            ".sql", ".r", ".jl", ".dart",
            ".cmake", ".txt",
            ".xaml", ".axaml", ".xml", // XAML/XML — lets C#↔XAML jump by x:Name / x:Class
        };
        return ok.count(e) != 0;
    };
    // deps/vendor are skipped on the FIRST pass (keeps project go-to-def fast and
    // out of library internals). If nothing is found we retry WITH them included
    // (two-pass call below), so a symbol that lives only in a bundled dependency
    // — e.g. ImVector in deps/imgui/imgui.h — is still reachable.
    bool includeDeps = false;
    auto skipDir = [&includeDeps](const std::string &name) {
        if (!includeDeps && (name == "deps" || name == "vendor"))
            return true;
        return name == ".git" || name == ".svn" || name == ".hg" || name == "node_modules" || name == "bin" || name == "obj" || name == "out" || name == "build" || name == "target" || name == ".vs" || name == ".vscode" || name == ".idea" || name == "__pycache__" || name == "Backup" || name == "backup" || name == "Backups" || name == "backups";
    };

    auto isWordBoundary = [](char c) {
        return !(std::isalnum((unsigned char)c) || c == '_');
    };

    // Score each match — favours stronger definition signals.
    struct Hit
    {
        std::filesystem::path path;
        int line;
        int score;
        std::string preview;
    };
    std::vector<Hit> hits;
    std::error_code ec;

    // One full directory walk + scan. Wrapped in a lambda so we can run it twice:
    // once with deps excluded (fast, project-only) and, if that finds nothing,
    // again with deps included (reaches symbols defined only in bundled libs).
    auto runScan = [&]() {
        int budget = 8000; // file budget — keep walks bounded on huge projects

        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (it->is_directory(ec))
            {
                if (skipDir(it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            if (--budget < 0)
                break;
            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (!extOk(ext))
                continue;
            if (navIsExcluded(it->path()))
                continue;

            // File-level bias. Declaration mode favours headers; definition mode
            // favours implementation files. A file named after the symbol
            // (User.cs / User.h for "User") gets a bonus — "go to the file".
            bool isHeader = (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh");
            int fileBonus = 0;
            if (declaration && isHeader)
                fileBonus += 8;
            if (!declaration && !isHeader)
                fileBonus += 4;
            {
                auto stem = it->path().stem().string();
                if (stem == symbol)
                    fileBonus += 6;
            }

            std::ifstream f(it->path());
            if (!f.is_open())
                continue;
            std::string line;
            int lineNo = 0;
            while (std::getline(f, line))
            {
                ++lineNo;
                // Strip leading whitespace for cheap pattern checks.
                size_t s = 0;
                while (s < line.size() && (line[s] == ' ' || line[s] == '\t'))
                    ++s;
                // Skip comment lines. A definition never lives in a comment, and
                // pasted foreign-language snippets (e.g. C++ "-- struct FVector ..."
                // commented out inside a Lua file) otherwise score as real defs and
                // hijack Go-to-Definition. Covers the common leading markers across
                // languages: // (C/C++/C#/JS), -- (Lua/SQL), # (Python/shell/Ruby),
                // ; (ini/asm), * (inside C block comments / doc comments).
                if (s < line.size())
                {
                    char c0 = line[s];
                    char c1 = (s + 1 < line.size()) ? line[s + 1] : '\0';
                    if ((c0 == '/' && c1 == '/') || (c0 == '-' && c1 == '-') || c0 == '#' || c0 == ';' || c0 == '*')
                    {
                        continue;
                    }
                }
                // Whole-word scan for the (last-segment) symbol.
                size_t pos = 0;
                while ((pos = line.find(symbol, pos)) != std::string::npos)
                {
                    bool leftOk = (pos == 0) || isWordBoundary(line[pos - 1]);
                    bool rightOk = (pos + symbol.size() >= line.size()) || isWordBoundary(line[pos + symbol.size()]);
                    if (!leftOk || !rightOk)
                    {
                        pos += 1;
                        continue;
                    }

                    int score = 0;
                    // Strong signals: a definition keyword immediately to the left.
                    for (auto &kw : defKeywords)
                    {
                        if (pos >= kw.size() + 1)
                        {
                            size_t kwStart = pos - kw.size() - 1;
                            if (line[kwStart + kw.size()] == ' ' &&
                                line.compare(kwStart, kw.size(), kw) == 0 &&
                                (kwStart == 0 || isWordBoundary(line[kwStart - 1])))
                            {
                                score = 100;
                                break;
                            }
                        }
                    }
                    // Medium: #define <symbol>.
                    if (score == 0 && s < line.size() && line[s] == '#')
                    {
                        if (line.find("#define", s) == s)
                            score = 80;
                    }
                    // XAML/XML named element: `x:Name="symbol"`, `x:Key="symbol"`,
                    // `x:Class="…symbol"`, or a bare `Name="symbol"`. The symbol is the
                    // quoted value, so look just left of the match for `="` and an
                    // attribute name. This is what lets Go-to-Definition jump from a
                    // C# member to the XAML element that declares it (and back).
                    if (score == 0 && pos >= 2 && (line[pos - 1] == '"' || line[pos - 1] == '\'') && line[pos - 2] == '=')
                    {
                        size_t a = (pos >= 2) ? pos - 2 : 0; // at '='
                        size_t e2 = a;                       // walk back over attr name
                        while (e2 > 0 && (std::isalnum((unsigned char)line[e2 - 1]) ||
                                          line[e2 - 1] == ':' || line[e2 - 1] == '_' || line[e2 - 1] == '.' ||
                                          line[e2 - 1] == '-'))
                            --e2;
                        std::string attr = line.substr(e2, a - e2);
                        if (attr == "x:Name" || attr == "Name" || attr == "x:Key" ||
                            attr == "x:Class" || attr == "x:Uid")
                            score = 90;
                    }
                    // Is `pos` inside a "..." or '...' string literal on this line?
                    // Count unescaped quotes before pos; an odd count = inside a string.
                    // Definitions never live inside string literals, so the loose
                    // `type NAME(` / `NAME =` heuristics below must NOT fire there —
                    // otherwise text like Content="...with Windows (user mode)" scores
                    // `Windows` as a fake definition. (The XAML attr="value" rule above
                    // is deliberately exempt: it matches the quoted value on purpose.)
                    bool insideString = false;
                    {
                        char q = 0;
                        for (size_t ci = 0; ci < pos && ci < line.size(); ++ci)
                        {
                            char c = line[ci];
                            if (q)
                            {
                                if (c == q && line[ci - 1] != '\\')
                                    q = 0;
                            }
                            else if (c == '"' || c == '\'')
                                q = c;
                        }
                        insideString = (q != 0);
                    }
                    // Medium-weak: `<type> <symbol>(` — a method/function signature.
                    // Require a type-like token immediately before the symbol so a
                    // constructor CALL or type usage (`= ImVec4(`, `return Foo(`,
                    // `, Bar(`) is NOT mistaken for a definition. Qualified method
                    // defs (`Ret Class::Method(`) and pointer/ref/template returns
                    // (`Foo* f(`, `vector<int> g(`) still count.
                    if (score == 0 && !insideString)
                    {
                        size_t p = pos + symbol.size();
                        while (p < line.size() && line[p] == ' ')
                            ++p;
                        if (p < line.size() && line[p] == '(' && pos > 0)
                        {
                            size_t b = pos;
                            while (b > 0 && line[b - 1] == ' ')
                                --b;
                            bool typeBefore = false;
                            if (b > 0)
                            {
                                char pc = line[b - 1];
                                if (pc == '*' || pc == '&' || pc == '>' || pc == ':')
                                {
                                    typeBefore = true;
                                }
                                else if (std::isalnum((unsigned char)pc) || pc == '_')
                                {
                                    size_t s3 = b;
                                    while (s3 > 0 && (std::isalnum((unsigned char)line[s3 - 1]) || line[s3 - 1] == '_'))
                                        --s3;
                                    std::string prev = line.substr(s3, b - s3);
                                    static const std::unordered_set<std::string> notType = {
                                        "return",
                                        "new",
                                        "delete",
                                        "sizeof",
                                        "throw",
                                        "case",
                                        "co_return",
                                        "co_await",
                                        "and",
                                        "or",
                                        "not",
                                        "if",
                                        "while",
                                        "for",
                                        "switch",
                                        "do",
                                    };
                                    typeBefore = !notType.count(prev);
                                }
                            }
                            if (typeBefore)
                                score = 50;
                        }
                    }
                    // Lua: a name introduced by `local` is a definition site. Matches
                    // `local NAME = …` and comma lists `local a, b, c = …`; the names
                    // are the identifiers before the first '=' on a `local` line. The
                    // `function` keyword is excluded so `local function f` still scores
                    // `f` via the strong-keyword rule above, not the bare `local`.
                    if (score == 0 && ext == ".lua" && symbol != "function")
                    {
                        bool startsLocal = s + 5 <= line.size() && line.compare(s, 5, "local") == 0 && (s + 5 >= line.size() || isWordBoundary(line[s + 5]));
                        if (startsLocal && pos > s)
                        {
                            // First '=' that is a real assignment (not ==, ~=, <=, >=).
                            size_t eq = std::string::npos;
                            for (size_t k = s; k < line.size(); ++k)
                            {
                                char c = line[k];
                                if (c == '=' && (k + 1 >= line.size() || line[k + 1] != '=') && (k == 0 || (line[k - 1] != '=' && line[k - 1] != '~' && line[k - 1] != '<' && line[k - 1] != '>')))
                                {
                                    eq = k;
                                    break;
                                }
                            }
                            if (eq == std::string::npos || pos < eq)
                                score = 70;
                        }
                    }
                    // Weak: `<symbol> = ` top-level assignment.
                    if (score == 0 && !insideString && pos == s)
                    {
                        size_t p = pos + symbol.size();
                        while (p < line.size() && line[p] == ' ')
                            ++p;
                        if (p < line.size() && line[p] == '=' &&
                            (p + 1 >= line.size() || line[p + 1] != '='))
                        {
                            score = 30;
                        }
                    }
                    if (score > 0)
                    {
                        // Declaration vs definition tie-break: a line ending in ';'
                        // is a prototype/declaration; one with '{' is a body. Nudge
                        // toward the requested kind.
                        bool endsSemicolon = !line.empty() && line.find_last_not_of(" \t") != std::string::npos && line[line.find_last_not_of(" \t")] == ';';
                        bool hasBrace = line.find('{', pos) != std::string::npos;
                        if (declaration && endsSemicolon)
                            score += 12;
                        if (!declaration && hasBrace)
                            score += 12;
                        score += fileBonus;

                        Hit h;
                        h.path = it->path();
                        h.line = lineNo - 1; // 0-based for SetCursor
                        h.score = score;
                        h.preview = line.substr(0, (std::min)((size_t)200, line.size()));
                        hits.push_back(std::move(h));
                    }
                    pos += symbol.size();
                }
            }
        }
    }; // end runScan

    // First pass: project source only (deps excluded → fast, no library noise).
    runScan();
    // Fallback: if the project has no hit, widen to bundled deps/vendor. This is
    // the only path that reaches a symbol like ImVector that lives solely in
    // deps/imgui/imgui.h, while keeping the common case off the library tree.
    if (hits.empty())
    {
        includeDeps = true;
        ec.clear();
        runScan();
    }

    if (hits.empty())
    {
        showError(std::string("No ") + (declaration ? "declaration" : "definition") + " of '" + word + "' found under " + root.string());
        return;
    }
    // Best score wins; ties broken by first file encountered.
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit &a, const Hit &b) { return a.score > b.score; });
    auto &best = hits.front();
    commitPendingNavJump();
    openFile(best.path.string());
    // openFile leaves the newly-opened tab as the active one — scroll it.
    if (!tabs.empty())
    {
        auto &e = doc().editor;
        e.SetCursor(best.line, 0);
        e.SelectLine(best.line); // highlight the whole line, not just the gutter
        e.ScrollToLine(best.line, TextEditor::Scroll::alignMiddle);
    }
}

void Editor::findReferencesOf(TabDocument &t, const std::string &word)
{
    referencesWord = word;
    referencesHits.clear();
    referencesFileCount = 0;
    referencesVisible = true;

    // Remember which tab this search ran against so the panel's "Search all
    // files" checkbox can re-run the same query at a wider scope.
    referencesTab = &t;

    if (word.empty())
    {
        // Cancel any in-flight search so stale results don't stream in.
        ++refSearch->gen;
        std::lock_guard<std::mutex> lk(refSearch->mutex);
        refSearch->hits.clear();
        refSearch->fileCount = 0;
        refSearch->truncated = false;
        ++refSearch->version;
        return;
    }

    std::string activeLabel = t.filename == "untitled" ? "(untitled)" : t.filename;
    std::string activeCanon;
    if (t.filename != "untitled")
    {
        std::error_code ec;
        activeCanon = std::filesystem::weakly_canonical(t.filename, ec).string();
    }

    // Default scope is the ACTIVE FILE only (its live buffer, so unsaved edits
    // count) — an empty root skips the disk walk entirely. The user widens to
    // the whole project via the panel checkbox (referencesAllFiles), which
    // re-runs with the real root; deps/vendor are intentionally searched there.
    std::filesystem::path root;
    if (referencesAllFiles)
    {
        root = projectRoot;
        if (root.empty() && t.filename != "untitled")
            root = std::filesystem::path(t.filename).parent_path();
    }

    startProjectSearch(refSearch, word, /*caseSensitive=*/true, /*wholeWord=*/true,
                       /*maxHits=*/5000, /*skipDepsVendor=*/false,
                       root, activeCanon, activeLabel, t.editor.GetText());
}

void Editor::renderReferencesPanel()
{
    if (!referencesVisible)
        return;
    // Stream in fresh results from the async search.
    if (pollProjectSearch(*refSearch, refSearchSeen, referencesHits, referencesFileCount,
                          referencesTruncated))
        buildSearchRows(referencesHits, referencesRows);
    ImGui::SetNextWindowSize(ImVec2(440.0f, 360.0f), ImGuiCond_FirstUseEver);
    // Stable dock ID (### resets the hash seed) so the window can be pre-docked
    // to the right by navInitDockLayout regardless of the symbol in the title.
    std::string title = std::string("References: ") + referencesWord + "###refsPanel";
    if (ImGui::Begin(title.c_str(), &referencesVisible))
    {
        ImGui::TextDisabled("%zu match%s across %d file%s%s%s",
                            referencesHits.size(), referencesHits.size() == 1 ? "" : "es",
                            referencesFileCount, referencesFileCount == 1 ? "" : "s",
                            referencesTruncated ? " (truncated)" : "",
                            refSearch->running.load() ? "  (searching…)" : "");
        // Scope toggle: default is the active file; tick to widen to the whole
        // project and re-run the same query against the tab it came from.
        if (ImGui::Checkbox("Search all files", &referencesAllFiles))
        {
            // Guard the stored pointer: the source tab may have been closed since
            // the search ran. Only re-run if it's still a live tab.
            bool alive = false;
            for (auto &up : tabs)
                if (up.get() == referencesTab)
                {
                    alive = true;
                    break;
                }
            if (alive)
                findReferencesOf(*referencesTab, referencesWord);
        }
        ImGui::Separator();

        ImGui::BeginChild("##refResults");
        renderSearchHits(referencesHits, referencesRows);
        middleMousePanScroll(4);   // references panel
        ImGui::EndChild();
    }
    ImGui::End();
}

// ── Symbol / definition browser ───────────────────────────────────────
//
// A dockable outline. Document mode parses the active file with tree-sitter
// (cached by filename + edit count so it only re-parses on change); Project
// mode lists the whole background index and shows its build status so the user
// can watch the index populate. Click a row to jump to the definition.

static const char *symKindTag(ts::Kind k)
{
    switch (k)
    {
    case ts::Kind::Function:
        return "fn";
    case ts::Kind::Method:
        return "fn";
    case ts::Kind::Class:
        return "class";
    case ts::Kind::Struct:
        return "struct";
    case ts::Kind::Enum:
        return "enum";
    case ts::Kind::Type:
        return "type";
    case ts::Kind::Field:
        return "field";
    case ts::Kind::Variable:
        return "var";
    case ts::Kind::Constant:
        return "const";
    case ts::Kind::Module:
        return "ns";
    case ts::Kind::Macro:
        return "macro";
    default:
        return "sym";
    }
}

// Render a type→members tree from a flat symbol list. `jump(line)` navigates to a
// definition. Shared by the Symbols panel's Document and Project (per-file) views.
template <class JumpFn>
static void renderSymbolGroup(const std::vector<ts::Symbol> &syms, const std::string &filter,
                              JumpFn jump, int &uid)
{
    auto match = [&](const std::string &n) {
        if (filter.empty())
            return true;
        std::string l = n;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return l.find(filter) != std::string::npos;
    };
    auto label = [](const ts::Symbol &s) { return std::string(symKindTag(s.kind)) + "  " + s.name; };
    auto isTypeKind = [](ts::Kind k) {
        return k == ts::Kind::Class || k == ts::Kind::Struct || k == ts::Kind::Enum ||
               k == ts::Kind::Type || k == ts::Kind::Module;
    };

    if (!filter.empty())
    {
        for (auto &s : syms)
        {
            if (!s.isDefinition || !match(s.name))
                continue;
            ImGui::PushID(uid++);
            if (ImGui::Selectable(label(s).c_str()))
                jump(s.line);
            ImGui::PopID();
        }
        return;
    }

    std::unordered_map<std::string, std::vector<const ts::Symbol *>> membersByType;
    std::vector<const ts::Symbol *> topLevel;
    for (auto &s : syms)
    {
        if (!s.isDefinition)
            continue;
        if (s.enclosingType.empty())
            topLevel.push_back(&s);
        else
            membersByType[s.enclosingType].push_back(&s);
    }
    std::unordered_set<std::string> shownTypes;
    for (auto *s : topLevel)
    {
        ImGui::PushID(uid++);
        auto mit = membersByType.find(s->name);
        bool hasMembers = isTypeKind(s->kind) && mit != membersByType.end() && !mit->second.empty();
        if (hasMembers)
        {
            shownTypes.insert(s->name);
            bool open = ImGui::TreeNodeEx(label(*s).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                jump(s->line);
            if (open)
            {
                for (auto *m : mit->second)
                {
                    ImGui::PushID(uid++);
                    if (ImGui::Selectable(label(*m).c_str()))
                        jump(m->line);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
        else
        {
            if (ImGui::Selectable(label(*s).c_str()))
                jump(s->line);
        }
        ImGui::PopID();
    }
    for (auto &kv : membersByType)
    {
        if (shownTypes.count(kv.first))
            continue;
        ImGui::PushID(uid++);
        std::string header = kv.first + "  (members)";
        if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
        {
            for (auto *m : kv.second)
            {
                ImGui::PushID(uid++);
                if (ImGui::Selectable(label(*m).c_str()))
                    jump(m->line);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void Editor::renderSymbolsPanel()
{
    if (!symbolsPanelVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(300.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Symbols###symbolsPanel", &symbolsPanelVisible))
    {
        ImGui::End();
        return;
    }

    if (ImGui::RadioButton("Document", !symbolsProjectMode))
        symbolsProjectMode = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Project", symbolsProjectMode))
        symbolsProjectMode = true;

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##symFilter", "filter…", symbolsFilter, sizeof(symbolsFilter));
    std::string filter = symbolsFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    if (!symbolsProjectMode)
    {
        // ── Document outline (live parse of the active file) ──
        if (tabs.empty())
        {
            ImGui::TextDisabled("(no document)");
            ImGui::End();
            return;
        }
        auto &ed = doc().editor;
        std::string fname = doc().filename;
        std::string ext = std::filesystem::path(fname).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        ts::Lang lang = ts::langForExtension(ext);
        if (lang == ts::Lang::None)
        {
            ImGui::TextDisabled("(no symbols for this file type)");
            ImGui::End();
            return;
        }
        // Re-parse only when the file or its edit count changed.
        size_t undo = ed.GetUndoIndex();
        if (fname != symbolsCacheFile || undo != symbolsCacheUndo)
        {
            symbolsCacheFile = fname;
            symbolsCacheUndo = undo;
            symbolsCacheSyms = ts::extractSymbols(lang, ed.GetText());
        }
        ImGui::Separator();
        if (symbolsCacheSyms.empty())
        {
            ImGui::TextDisabled("(no symbols)");
            ImGui::End();
            return;
        }
        auto jump = [&](int line) {
            navHistory.record(currentNavLocation()); // so Back returns here
            auto &e = doc().editor;
            e.SetCursor(line, 0);
            e.SelectLine(line);
            e.ScrollToLine(line, TextEditor::Scroll::alignMiddle);
        };
        if (ImGui::BeginChild("##symDoc"))
        {
            int uid = 0;
            renderSymbolGroup(symbolsCacheSyms, filter, jump, uid);
            middleMousePanScroll(5); // symbols (document)
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    // ── Project index ──
    auto idx = indexSnapshot();
    bool building = indexState->building.load();
    int gen = indexState->gen.load();
    if (idx)
        ImGui::Text("gen %d%s · %zu files · %zu types", gen, building ? "  (building…)" : "",
                    idx->fileSymbols.size(), idx->members.size());
    else
        ImGui::TextDisabled("(no index — open a project)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Rebuild"))
        rebuildProjectIndex();
    ImGui::Separator();

    if (!idx)
    {
        ImGui::End();
        return;
    }
    // Rebuild the per-gen caches: sorted file list (tree) + flat rows (filter view).
    if (symbolsProjectGen != gen)
    {
        symbolsFiles.clear();
        symbolsProjectRows.clear();
        for (auto &kv : idx->fileSymbols)
            symbolsFiles.push_back(kv.first);
        std::sort(symbolsFiles.begin(), symbolsFiles.end());
        for (auto &kv : idx->tsDefs)
            for (auto &d : kv.second)
                symbolsProjectRows.push_back({kv.first, d.file, d.line, d.kind, {}});
        std::sort(symbolsProjectRows.begin(), symbolsProjectRows.end(),
                  [](const SymRow &a, const SymRow &b) { return a.name < b.name; });
        for (auto &r : symbolsProjectRows) // precompute lowercase once per gen (filter is case-insensitive)
        {
            r.lname = r.name;
            std::transform(r.lname.begin(), r.lname.end(), r.lname.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        }
        symbolsFilterCache = std::string(1, '\x01'); // force a refilter against the new rows
        symbolsProjectGen = gen;
    }

    auto projectJump = [&](const std::string &file, int line) {
        navHistory.record(currentNavLocation()); // so Back returns here
        openFile(file);
        if (!tabs.empty())
        {
            auto &e = doc().editor;
            e.SetCursor(line, 0);
            e.SelectLine(line);
            e.ScrollToLine(line, TextEditor::Scroll::alignMiddle);
        }
    };

    if (ImGui::BeginChild("##symProj"))
    {
        if (!filter.empty())
        {
            // Flat filtered list across the whole project. Matching scans every
            // project row, so cache the matched indices and only recompute when the
            // filter text (or index gen) changes — otherwise this ran over thousands
            // of rows (allocating a lowercased string each) every frame.
            if (filter != symbolsFilterCache)
            {
                symbolsFilteredIdx.clear();
                for (int i = 0; i < (int)symbolsProjectRows.size(); ++i)
                    if (symbolsProjectRows[i].lname.find(filter) != std::string::npos)
                        symbolsFilteredIdx.push_back(i);
                symbolsFilterCache = filter;
            }
            int shown = 0;
            for (int ri : symbolsFilteredIdx)
            {
                if (shown >= 2000)
                    break;
                const auto &row = symbolsProjectRows[ri];
                ImGui::PushID(shown++);
                std::string lbl = std::string(symKindTag(row.kind)) + "  " + row.name;
                if (ImGui::Selectable(lbl.c_str()))
                    projectJump(row.file, row.line);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s:%d", row.file.c_str(), row.line + 1);
                ImGui::PopID();
            }
            ImGui::TextDisabled("%zu match%s%s", symbolsFilteredIdx.size(),
                                symbolsFilteredIdx.size() == 1 ? "" : "es",
                                symbolsFilteredIdx.size() > 2000 ? " (showing first 2000)" : "");
        }
        else
        {
            // Tree: file → types → members (same grouping as the Document tab).
            int uid = 0;
            for (auto &file : symbolsFiles)
            {
                auto it = idx->fileSymbols.find(file);
                if (it == idx->fileSymbols.end() || it->second.empty())
                    continue;
                std::string leaf = std::filesystem::path(file).filename().string();
                ImGui::PushID(uid++);
                bool open = ImGui::TreeNodeEx(leaf.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", file.c_str());
                if (open)
                {
                    auto jump = [&](int line) { projectJump(file, line); };
                    renderSymbolGroup(it->second, filter, jump, uid);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        middleMousePanScroll(6); // symbols (project)
    }
    ImGui::EndChild();
    ImGui::End();
}

// ── Find in Files — project-wide text search ──────────────────────────

void Editor::openFindInFiles()
{
    findInFilesVisible = true;
    findInFilesFocus = true;
    // Seed the query from a single-line selection, if any, and search immediately.
    if (!tabs.empty() && doc().editor.AnyCursorHasSelection())
    {
        std::string sel = doc().editor.GetCurrentSelectionText();
        if (!sel.empty() && sel.find('\n') == std::string::npos && sel.size() < sizeof(findInFilesQuery))
        {
            std::snprintf(findInFilesQuery, sizeof(findInFilesQuery), "%s", sel.c_str());
            runFindInFiles();
        }
    }
}

void Editor::runFindInFiles()
{
    // The scan itself runs on the async worker-pool engine — this just
    // gathers the query + scope and kicks it off. Results stream into the
    // panel via fifSearch as the workers publish them.
    findInFilesHits.clear();
    findInFilesFileCount = 0;
    findInFilesTruncated = false;

    std::filesystem::path root = projectRoot;
    if (root.empty() && !tabs.empty() && doc().filename != "untitled")
        root = std::filesystem::path(doc().filename).parent_path();
    if (root.empty())
        root = std::filesystem::current_path();

    std::string activeCanon, activeLabel, activeText;
    if (!tabs.empty() && doc().filename != "untitled")
    {
        std::error_code ec;
        activeCanon = std::filesystem::weakly_canonical(doc().filename, ec).string();
        activeLabel = doc().filename;
        activeText = doc().editor.GetText();
    }

    startProjectSearch(fifSearch, findInFilesQuery, findInFilesCase, findInFilesWholeWord,
                       /*maxHits=*/5000, /*skipDepsVendor=*/true,
                       root, std::move(activeCanon), std::move(activeLabel), std::move(activeText));
}

void Editor::renderFindInFilesPanel()
{
    if (!findInFilesVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(500.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Find in Files###findInFiles", &findInFilesVisible))
    {
        if (findInFilesFocus)
        {
            ImGui::SetKeyboardFocusHere();
            findInFilesFocus = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        bool go = ImGui::InputTextWithHint("##fifQuery", "Search text in project…",
                                           findInFilesQuery, sizeof(findInFilesQuery), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Checkbox("Match case", &findInFilesCase);
        ImGui::SameLine();
        ImGui::Checkbox("Whole word", &findInFilesWholeWord);
        ImGui::SameLine();
        if (ImGui::Button("Search"))
            go = true;
        if (go)
            runFindInFiles();

        // Stream in fresh results from the async search.
        if (pollProjectSearch(*fifSearch, fifSearchSeen, findInFilesHits, findInFilesFileCount,
                              findInFilesTruncated))
            buildSearchRows(findInFilesHits, findInFilesRows);

        ImGui::TextDisabled("%zu match%s across %d file%s%s%s",
                            findInFilesHits.size(), findInFilesHits.size() == 1 ? "" : "es",
                            findInFilesFileCount, findInFilesFileCount == 1 ? "" : "s",
                            findInFilesTruncated ? " (truncated)" : "",
                            fifSearch->running.load() ? "  (searching…)" : "");
        ImGui::Separator();

        ImGui::BeginChild("##fifResults");
        renderSearchHits(findInFilesHits, findInFilesRows);
        middleMousePanScroll(7);   // find-in-files results
        ImGui::EndChild();
    }
    ImGui::End();
}

// ── Developer tools — inspectors + "where is this feature's code" map ──

void Editor::renderDevTools()
{
    // Dear ImGui's own inspectors (rendered whenever toggled on, even if the
    // Developer Tools window itself is closed).
    if (devShowMetrics)
        ImGui::ShowMetricsWindow(&devShowMetrics);
    if (devShowStackTool)
        ImGui::ShowIDStackToolWindow(&devShowStackTool);
    if (devShowDebugLog)
        ImGui::ShowDebugLogWindow(&devShowDebugLog);
    if (devShowDemo)
        ImGui::ShowDemoWindow(&devShowDemo);

    if (!devToolsVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(520.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Developer Tools###devTools", &devToolsVisible))
    {
        ImGui::Separator();
        if (ImGui::TreeNode("Dear ImGui Tools"))
        {
            ImGui::Checkbox("Metrics / Debugger", &devShowMetrics);
            ImGui::Checkbox("ID Stack Tool (inspect widget IDs)", &devShowStackTool);
            ImGui::Checkbox("Debug Log", &devShowDebugLog);
            ImGui::Checkbox("Dear ImGui Demo", &devShowDemo);
            ImGui::TreePop();
        }

        // ── Live external-edit watch state ───────────────────────────────
        ImGui::SeparatorText("External edit watch");
        {
            auto cfg = userConfigDir();
            ImGui::Text("Poll: 1.0s   Open docs: %d   Toasts live: %d",
                        (int)tabs.size(), (int)toasts.size());
            {
                float fps = ImGui::GetIO().Framerate;
                ImU32 pc = (fps >= 55.0f)   ? IM_COL32(120, 200, 120, 255)
                           : (fps >= 30.0f) ? IM_COL32(240, 200, 90, 255)
                                            : IM_COL32(240, 110, 90, 255);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pc),
                                   "Perf: %.0f fps   worst UI build (3s): %.1f ms   slow frames: %d",
                                   fps, fpsWorstMs, fpsSlowCount);
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset perf"))
                {
                    fpsSlowCount = 0;
                    fpsWorstMs = 0.0f;
                    fpsWindowWorstMs = 0.0f;
                }
            }
            if (ImGui::SmallButton("Open crash.log"))
                navOpenExternally((cfg / "crash.log").string());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open config dir"))
                navOpenExternally(cfg.string());
            ImGui::SameLine();
            if (ImGui::SmallButton("Test toast"))
                pushToast("\xe2\x9c\x8e Dev Tools test toast", IM_COL32(170, 130, 250, 255));
            ImGui::SameLine();
            if (ImGui::SmallButton("Activity log"))
                externalChangesVisible = true;

            if (ImGui::BeginTable("##watch", 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Open document");
                ImGui::TableSetupColumn("Watch state");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < tabs.size(); ++i)
                {
                    auto &t = *tabs[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::filesystem::path(t.filename).filename().string().c_str());
                    ImGui::TableNextColumn();
                    const char *st;
                    ImU32 col;
                    if (t.filename == "untitled")
                    {
                        st = "unsaved (not watched)";
                        col = IM_COL32(150, 150, 150, 255);
                    }
                    else if (t.externalChange)
                    {
                        st = "CONFLICT — disk changed under edits";
                        col = IM_COL32(240, 180, 70, 255);
                    }
                    else if (t.externallyTouched)
                    {
                        st = "external edit (unseen)";
                        col = IM_COL32(170, 130, 250, 255);
                    }
                    else if (t.externalMarkers)
                    {
                        st = "reloaded — change markers shown";
                        col = IM_COL32(130, 200, 255, 255);
                    }
                    else
                    {
                        st = "in sync";
                        col = IM_COL32(120, 200, 120, 255);
                    }
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", st);
                }
                ImGui::EndTable();
            }
        }

        // ── Claude / external changes — reply inline ─────────────────────
        ImGui::SeparatorText("Claude changes \xe2\x80\x94 reply");
        {
            int totalRanges = 0;
            for (auto &up : tabs)
                totalRanges += (int)up->changedRanges.size();
            if (totalRanges == 0)
                ImGui::TextDisabled("No marked changes. When Claude edits an open file, its changed lines "
                                    "get a purple gutter dot \xe2\x80\x94 click it, a row here, or right-click "
                                    "a line number to reply.");
            else
            {
                ImGui::TextDisabled("Click a change to type a reply (Send now, or queue for batch).");
                for (size_t i = 0; i < tabs.size(); ++i)
                {
                    auto &t = *tabs[i];
                    if (t.changedRanges.empty())
                        continue;
                    std::string fn = std::filesystem::path(t.filename).filename().string();
                    for (auto &rg : t.changedRanges)
                    {
                        ImGui::PushID((const void *)&rg);
                        char label[160];
                        if (rg.first == rg.second)
                            std::snprintf(label, sizeof(label), "\xe2\x9c\x8e %s : line %d", fn.c_str(), rg.first + 1);
                        else
                            std::snprintf(label, sizeof(label), "\xe2\x9c\x8e %s : lines %d-%d", fn.c_str(),
                                          rg.first + 1, rg.second + 1);
                        if (ImGui::Button(label))
                        {
                            t.wantFocus = true;
                            t.editor.SetCursor(rg.first, 0);
                            t.editor.ScrollToLine(rg.first, TextEditor::Scroll::alignMiddle);
                            requestReply(t.filename, rg.first, t.filename + ":" + std::to_string(rg.first + 1));
                        }
                        ImGui::PopID();
                    }
                }
            }
            if (!replyBatch.empty())
            {
                ImGui::Spacing();
                ImGui::Text("%d comment%s queued for batch", (int)replyBatch.size(),
                            replyBatch.size() == 1 ? "" : "s");
                ImGui::SameLine();
                if (ImGui::SmallButton("Submit batch"))
                    flushReplyBatch();
                ImGui::SameLine();
                if (ImGui::SmallButton("Discard"))
                    replyBatch.clear();
            }
        }

        ImGui::Separator();
        if (ImGui::TreeNode("Source code for features"))
        {

            ImGui::TextWrapped("Click a row to jump to the function (project-wide go-to-def). "
                               "Clicking auto-opens the ImGui-IDE source repo as the project if it isn't already.");
            std::error_code selfec;
            bool selfRepoOpen = !projectRoot.empty() &&
                                std::filesystem::exists(projectRoot / "example" / "editor.cpp", selfec);
            if (!selfRepoOpen)
            {
                auto self = findSelfRepoRoot();
                if (!self.empty())
                {
                    if (ImGui::SmallButton("Open ImGui-IDE repo"))
                        setProjectRoot(self);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", self.filename().string().c_str());
                }
                else
                    ImGui::TextDisabled("(source repo not found next to this build)");
            }
            ImGui::Spacing();

            struct DevLoc
            {
                const char *feature;
                const char *symbol;
                const char *file;
            };
            static const DevLoc locs[] = {
                {"Menu bar / menus", "renderMenuBar", "example/editor.cpp"},
                {"Settings window", "renderSettings", "example/editor.cpp"},
                {"Navigation panel", "renderNavigationPanel", "example/editor.cpp"},
                {"Find in Files", "renderFindInFilesPanel", "example/editor.cpp"},
                {"References panel", "renderReferencesPanel", "example/editor.cpp"},
                {"Run / Output panel", "runCommandInOutputPanel", "example/editor.cpp"},
                {"Document tabs / docking", "renderDockedDocuments", "example/editor.cpp"},
                {"Status bar", "renderStatusBar", "example/editor.cpp"},
                {"Go to Definition", "goToDefinitionProjectWide", "example/editor.cpp"},
                {"Keybind dispatch", "keybindPressed", "example/editor.cpp"},
                {"System include discovery", "systemIncludeDirs", "example/editor.cpp"},
                {"Settings persistence", "saveSettings", "example/editor.cpp"},
                {"Git status poll", "pollGitStatus", "example/editor.cpp"},
                {"Git actions / dialogs", "renderGitDialogs", "example/editor.cpp"},
                {"Co-editing watch", "checkExternalChanges", "example/editor.cpp"},
                {"External reload", "reloadFromDisk", "example/editor.cpp"},
                {"External change markers", "markChangedLines", "example/editor.cpp"},
                {"Toast notifications", "renderToasts", "example/editor.cpp"},
                {"Crash / assert capture", "installCrashHandlers", "example/main.cpp"},
                {"Format Document", "formatActiveDocument", "example/editor.cpp"},
                {".editorconfig cascade", "applyEditorConfig", "example/editor.cpp"},
                {"Markdown preview", "renderMarkdownPreview", "example/editor.cpp"},
                {"Image viewer", "renderImageWindows", "example/editor.cpp"},
                {"Diff viewer", "renderDiff", "example/editor.cpp"},
                {"Editor text render", "renderText", "TextEditor.cpp"},
                {"Find / Replace bar", "renderFindReplace", "TextEditor.cpp"},
                {"Syntax colorizer", "updateChangedLines", "TextEditor.cpp"},
                {"Folding (build ranges)", "rebuildFoldRanges", "TextEditor.cpp"},
                {"Gutter markers", "renderMarkers", "TextEditor.cpp"},
                {"Mouse / pan-scroll", "handleMouseInteractions", "TextEditor.cpp"},
            };
            // No ScrollY: let the table grow to full height so the outer window owns
            // the scroll — that's what middleMousePanScroll(8) below pans. With an
            // internal table scroll the pan would target the (non-scrolling) outer
            // window and do nothing.
            if (ImGui::BeginTable("##devmap", 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
            {
                ImGui::TableSetupColumn("Feature");
                ImGui::TableSetupColumn("Function - file");
                ImGui::TableHeadersRow();
                for (int i = 0; i < (int)(sizeof(locs) / sizeof(locs[0])); ++i)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    if (ImGui::Selectable(locs[i].feature, false, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        // Auto-open the IDE's own repo so these project-wide lookups resolve.
                        std::error_code rec;
                        if (projectRoot.empty() ||
                            !std::filesystem::exists(projectRoot / "example" / "editor.cpp", rec))
                        {
                            auto self = findSelfRepoRoot();
                            if (!self.empty())
                                setProjectRoot(self);
                        }
                        goToDefinitionProjectWide(locs[i].symbol, false);
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s  -  %s", locs[i].symbol, locs[i].file);
                    ImGui::PopID();
                }
                ImGui::EndTable();
                ImGui::TreePop();
            }
        }
        middleMousePanScroll(8); // developer tools
    }
    ImGui::End();
}

// ── Markdown preview ──────────────────────────────────────────────────

// Render one logical line of markdown text with inline styles (**bold**,
// *italic*, `code`, [text](url)), word-wrapped to wrapWidth. Links open in the
// browser on click.
void Editor::renderMarkdownInline(const std::string &text, float wrapWidth)
{
    // Fast path: a line with no inline markup needs no per-word styling/measuring.
    // ImGui::TextUnformatted with a wrap pos wraps AND clips off-screen cheaply —
    // this is the bulk of typical markdown and the main perf win.
    if (text.find_first_of("*_`[") == std::string::npos)
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    struct Word
    {
        std::string s;
        int style;
        std::string url;
    }; // style 0 norm,1 bold,2 italic,3 code
    std::vector<Word> words;
    std::string cur;
    int style = 0;
    auto flush = [&] { if (!cur.empty()) { words.push_back({ cur, style, std::string() }); cur.clear(); } };

    for (size_t i = 0; i < text.size();)
    {
        char c = text[i];
        if (style == 3)
        { // inside `code` — literal until closing `
            if (c == '`')
            {
                flush();
                style = 0;
                ++i;
            }
            else if (c == ' ')
            {
                flush();
                ++i;
            }
            else
            {
                cur += c;
                ++i;
            }
            continue;
        }
        if (c == '`')
        {
            flush();
            style = 3;
            ++i;
            continue;
        }
        if (c == '[')
        { // [text](url)
            size_t close = text.find(']', i + 1);
            size_t op = (close != std::string::npos) ? text.find('(', close + 1) : std::string::npos;
            size_t cp = (op != std::string::npos) ? text.find(')', op + 1) : std::string::npos;
            if (close != std::string::npos && op == close + 1 && cp != std::string::npos)
            {
                flush();
                std::string ltext = text.substr(i + 1, close - i - 1);
                std::string lurl = text.substr(op + 1, cp - op - 1);
                std::string w;
                for (char lc : ltext)
                {
                    if (lc == ' ')
                    {
                        if (!w.empty())
                        {
                            words.push_back({w, 0, lurl});
                            w.clear();
                        }
                    }
                    else
                        w += lc;
                }
                if (!w.empty())
                    words.push_back({w, 0, lurl});
                i = cp + 1;
                continue;
            }
        }
        if (c == '*' || c == '_')
        {
            bool dbl = (i + 1 < text.size() && text[i + 1] == c);
            flush();
            if (dbl)
            {
                style = (style == 1) ? 0 : 1;
                i += 2;
            } // bold
            else
            {
                style = (style == 2) ? 0 : 2;
                i += 1;
            } // italic
            continue;
        }
        if (c == ' ')
        {
            flush();
            ++i;
            continue;
        }
        cur += c;
        ++i;
    }
    flush();

    const float spaceW = ImGui::CalcTextSize(" ").x;
    float x = 0.0f;
    bool firstOnLine = true;
    for (auto &wd : words)
    {
        float w = ImGui::CalcTextSize(wd.s.c_str()).x;
        if (!firstOnLine)
        {
            if (x + spaceW + w <= wrapWidth)
            {
                ImGui::SameLine(0.0f, spaceW);
                x += spaceW;
            }
            else
            {
                x = 0.0f;
                firstOnLine = true;
            } // wrap: next word starts a new line
        }
        bool link = !wd.url.empty();
        ImU32 col = link            ? IM_COL32(90, 160, 255, 255)
                    : wd.style == 3 ? IM_COL32(220, 170, 90, 255)  // code
                    : wd.style == 1 ? IM_COL32(255, 255, 255, 255) // bold (bright)
                    : wd.style == 2 ? IM_COL32(205, 205, 165, 255) // italic
                                    : ImGui::GetColorU32(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(wd.s.c_str());
        ImGui::PopStyleColor();
        if (link)
        {
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", wd.url.c_str());
            if (ImGui::IsItemClicked())
            {
#ifdef _WIN32
                ShellExecuteA(nullptr, "open", wd.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
        }
        x += w;
        firstOnLine = false;
    }
    if (words.empty())
        ImGui::NewLine();
}

void Editor::renderMarkdownPreview()
{
    if (!mdPreviewVisible)
        return;
    // Autohide: only show while the active document is markdown. Switch to a
    // non-md doc and the panel vanishes; switch back and it returns. mdPreviewVisible
    // stays the user's intent, so there's never a stale "(open a .md file)" panel.
    if (tabs.empty())
        return;
    {
        std::string aext = std::filesystem::path(doc().filename).extension().string();
        std::transform(aext.begin(), aext.end(), aext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (aext != ".md" && aext != ".markdown")
            return;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Markdown Preview###mdPreview", &mdPreviewVisible))
    {
        auto &t = doc();

        // Cache the serialized text — re-fetch only when the doc or its edit
        // count changes, so we don't allocate a whole-document copy every frame.
        std::string key = t.filename + ":" + std::to_string(t.editor.GetUndoIndex());
        if (key != mdCacheKey)
        {
            mdCacheText = t.editor.GetText();
            mdCacheKey = key;
        }

        const float avail = ImGui::GetContentRegionAvail().x;
        std::istringstream ss(mdCacheText);
        std::string line;
        bool inCode = false;
        std::string codeTag, codeBuf;
        int codeBlockIdx = 0;
        size_t mdCodeUsed = 0;

        // Emit one fenced block as an embedded read-only TextEditor — the same
        // widget/colorizer the editor itself uses, so ```cpp / ```lua / ```json
        // render fully syntax-highlighted. Widgets are cached per block and only
        // re-set when the block's content or tag changes.
        auto emitCodeBlock = [&]() {
            std::string cacheKey = codeTag + "\x1f" + codeBuf;
            if (mdCodeUsed >= mdCodeEditors.size())
                mdCodeEditors.push_back(std::make_unique<MdCodeBlock>());
            auto &blk = *mdCodeEditors[mdCodeUsed];
            if (blk.key != cacheKey)
            {
                blk.key = cacheKey;
                blk.ed.SetText(codeBuf);
                blk.ed.SetReadOnlyEnabled(true);
                blk.ed.SetShowLineNumbersEnabled(false);
                blk.ed.SetShowScrollbarMiniMapEnabled(false);
                // Fence tag -> language: try it as an extension, then aliases.
                std::string tag = codeTag;
                std::transform(tag.begin(), tag.end(), tag.begin(),
                               [](unsigned char c) { return (char) std::tolower(c); });
                if (tag == "c++" || tag == "cplusplus") tag = "cpp";
                else if (tag == "python") tag = "py";
                else if (tag == "csharp") tag = "cs";
                else if (tag == "javascript") tag = "js";
                else if (tag == "typescript") tag = "ts";
                else if (tag == "shell" || tag == "bash" || tag == "zsh") tag = "sh";
                else if (tag == "powershell") tag = "ps1";
                else if (tag == "markdown") tag = "md";
                blk.ed.SetLanguage(tag.empty() ? nullptr : languageForPath("fence." + tag));
            }
            int lines = (std::max)(1, blk.ed.GetLineCount());
            float h = (std::min)(lines, 24) * ImGui::GetTextLineHeightWithSpacing() +
                      ImGui::GetStyle().FramePadding.y * 4.0f;
            ImGui::PushID(codeBlockIdx);
            blk.ed.Render("##mdcode", ImVec2(avail, h), /*border*/ true);
            ImGui::PopID();
            ++mdCodeUsed;
            ++codeBlockIdx;
            codeBuf.clear();
            codeTag.clear();
        };

        while (std::getline(ss, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::string trimmed = line;
            size_t a = trimmed.find_first_not_of(" \t");

            // fenced code block toggle (info string after the fence names the language)
            if (line.rfind("```", 0) == 0 || line.rfind("~~~", 0) == 0)
            {
                if (!inCode)
                {
                    codeTag = line.substr(3);
                    size_t ts = codeTag.find_first_not_of(" \t");
                    codeTag = ts == std::string::npos ? std::string() : codeTag.substr(ts);
                    size_t te = codeTag.find_first_of(" \t");
                    if (te != std::string::npos)
                        codeTag.resize(te);
                    codeBuf.clear();
                    inCode = true;
                }
                else
                {
                    inCode = false;
                    emitCodeBlock();
                }
                continue;
            }
            if (inCode)
            {
                codeBuf += line;
                codeBuf += '\n';
                continue;
            }
            if (a == std::string::npos)
            {
                ImGui::Spacing();
                continue;
            } // blank line
            std::string body = line.substr(a);

            if (body == "---" || body == "***" || body == "___")
            {
                ImGui::Separator();
                continue;
            }

            int h = 0;
            while (h < (int)body.size() && body[h] == '#')
                ++h;
            if (h > 0 && h <= 6 && h < (int)body.size() && body[h] == ' ')
            {
                float scale = (h == 1) ? 1.7f : (h == 2) ? 1.4f
                                            : (h == 3)   ? 1.2f
                                                         : 1.1f;
                ImGui::PushFont(nullptr, ImGui::GetFontSize() * scale); // bigger heading text
                renderMarkdownInline(body.substr(h + 1), avail);
                ImGui::PopFont();
                if (h <= 2)
                    ImGui::Separator();
                continue;
            }
            if (body[0] == '>')
            {
                std::string q = body.substr(1);
                if (!q.empty() && q[0] == ' ')
                    q = q.substr(1);
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
                renderMarkdownInline(q, avail - ImGui::GetStyle().IndentSpacing);
                ImGui::PopStyleColor();
                ImGui::Unindent();
                continue;
            }
            if (body.rfind("- ", 0) == 0 || body.rfind("* ", 0) == 0 || body.rfind("+ ", 0) == 0)
            {
                ImGui::Indent();
                ImGui::TextUnformatted("\xe2\x80\xa2"); // bullet
                ImGui::SameLine();
                renderMarkdownInline(body.substr(2),
                                     avail - ImGui::GetStyle().IndentSpacing - ImGui::CalcTextSize("\xe2\x80\xa2 ").x);
                ImGui::Unindent();
                continue;
            }
            renderMarkdownInline(body, avail);
        }
        if (inCode)
            emitCodeBlock(); // unterminated fence at EOF still renders
        middleMousePanScroll(9); // markdown preview
    }
    ImGui::End();
}

// ── Image viewer + non-text dispatch ───────────────────────────────

bool Editor::isImageExt(const std::string &ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".gif" || ext == ".psd" || ext == ".hdr" || ext == ".pic";
}

bool Editor::isMarkdownExt(const std::string &ext)
{
    return ext == ".md" || ext == ".markdown" || ext == ".mdown" || ext == ".mkd" || ext == ".mdwn";
}

bool Editor::isBinaryExt(const std::string &ext)
{
    // Files we should not try to render as text. Executables are handled
    // separately (run them); everything else here hands off to the OS.
    return ext == ".exe" || ext == ".dll" || ext == ".so" || ext == ".dylib" || ext == ".lib" || ext == ".a" || ext == ".obj" || ext == ".o" || ext == ".pdb" || ext == ".zip" || ext == ".7z" || ext == ".tar" || ext == ".gz" || ext == ".rar" || ext == ".pdf" || ext == ".mp3" || ext == ".mp4" || ext == ".mov" || ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".webm" || ext == ".mkv" || ext == ".bin" || ext == ".iso";
}

void Editor::openImageFile(const std::string &path)
{
    // If already open, focus the existing image window.
    for (auto &img : images)
    {
        if (img->path == path)
        {
            img->wantFocus = true;
            img->open = true;
            return;
        }
    }
    int w = 0, h = 0, n = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!pixels)
    {
        showError(std::string("Could not load image: ") + path + "\n(" + (stbi_failure_reason() ? stbi_failure_reason() : "unknown") + ")");
        return;
    }
    auto img = std::make_unique<ImageDoc>();
    img->path = path;
    img->windowTitle = std::filesystem::path(path).filename().string() + "##img:" + path;
    img->w = w;
    img->h = h;
    img->tex = IM_NEW(ImTextureData)();
    img->tex->Create(ImTextureFormat_RGBA32, w, h);
    std::memcpy(img->tex->GetPixels(), pixels, (size_t)w * h * 4);
    img->tex->Status = ImTextureStatus_WantCreate;
    img->tex->UseColors = true;
    stbi_image_free(pixels);
    // Register so PlatformIO.Textures sees this each frame. Pushing to
    // PlatformIO.Textures directly is wrong — UpdateTexturesEndFrame()
    // resize(0)'s that list every frame and rebuilds it from FontAtlases +
    // g.UserTextures, so a direct push only survives the current frame
    // (which is why the first-time upload never happened and the image
    // stayed in "loading…" forever).
    ImGui::RegisterUserTexture(img->tex);
    img->wantFocus = true;
    images.push_back(std::move(img));
}

void Editor::navShowImageThumbnail(const std::string &path)
{
    auto it = thumbCache.find(path);
    if (it == thumbCache.end())
    {
        // First hover for this path — load + downscale to a thumbnail once.
        Thumb th;
        int w = 0, h = 0, n = 0;
        stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!pixels || w <= 0 || h <= 0)
        {
            if (pixels)
                stbi_image_free(pixels);
            th.failed = true;
            thumbCache.emplace(path, th);
            return;
        }
        // Cap the long edge so the GPU texture (and tooltip) stays small.
        const int kMax = 256;
        int tw = w, thh = h;
        if (w > kMax || h > kMax)
        {
            float s = (float)kMax / (float)(w > h ? w : h);
            tw = (std::max)(1, (int)(w * s));
            thh = (std::max)(1, (int)(h * s));
        }
        std::vector<stbi_uc> scaled((size_t)tw * thh * 4); // NB: not 'small' — that's a Windows macro
        // Nearest-neighbour box sample — fine for a hover thumbnail, no deps.
        for (int y = 0; y < thh; ++y)
        {
            int sy = (int)((long long)y * h / thh);
            for (int x = 0; x < tw; ++x)
            {
                int sx = (int)((long long)x * w / tw);
                const stbi_uc *src = pixels + ((size_t)sy * w + sx) * 4;
                stbi_uc *dst = scaled.data() + ((size_t)y * tw + x) * 4;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
        stbi_image_free(pixels);
        th.tex = IM_NEW(ImTextureData)();
        th.tex->Create(ImTextureFormat_RGBA32, tw, thh);
        std::memcpy(th.tex->GetPixels(), scaled.data(), scaled.size());
        th.tex->Status = ImTextureStatus_WantCreate;
        th.tex->UseColors = true;
        th.w = tw;
        th.h = thh;
        ImGui::RegisterUserTexture(th.tex);
        it = thumbCache.emplace(path, th).first;
    }

    Thumb &th = it->second;
    if (th.failed)
    {
        ImGui::TextDisabled("(preview unavailable)");
        return;
    }
    if (th.tex && th.tex->Status == ImTextureStatus_OK && th.tex->TexID != ImTextureID_Invalid)
        ImGui::Image(th.tex->GetTexRef(), ImVec2((float)th.w, (float)th.h));
    else
        ImGui::TextDisabled("loading preview…");
}

void Editor::renderImageWindows()
{
    for (auto it = images.begin(); it != images.end();)
    {
        auto &img = **it;
        if (!img.open)
        {
            // Queue the GPU texture for destroy + unregister so the next
            // EndFrame removes it from PlatformIO.Textures. The ImTextureData
            // itself gets IM_DELETE'd by ImGui's lifecycle once the backend
            // reports it Destroyed.
            if (img.tex)
            {
                img.tex->WantDestroyNextFrame = true;
                ImGui::UnregisterUserTexture(img.tex);
            }
            it = images.erase(it);
            continue;
        }
        if (img.wantFocus)
        {
            ImGui::SetNextWindowFocus();
            img.wantFocus = false;
        }
        ImGui::SetNextWindowSize(ImVec2((float)(std::min)(img.w + 40, 900),
                                        (float)(std::min)(img.h + 80, 700)),
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin(img.windowTitle.c_str(), &img.open))
        {
            ImGui::Text("%dx%d", img.w, img.h);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("zoom", &img.zoom, 0.1f, 8.0f, "%.2fx");
            if (ImGui::SmallButton("1:1"))
            {
                img.zoom = 1.0f;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Fit"))
            {
                auto avail = ImGui::GetContentRegionAvail();
                float zx = avail.x / (float)img.w;
                float zy = (avail.y - 30.0f) / (float)img.h;
                img.zoom = (std::max)(0.05f, (std::min)(zx, zy));
            }
            ImGui::Separator();
            ImGui::BeginChild("##imgScroll", ImVec2(0, 0),
                              ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
            // Only draw once the backend has uploaded the texture to the GPU.
            // On the first frame after openImageFile pushes the ImTextureData,
            // its TexID is still ImTextureID_Invalid — calling Image() then
            // generates an ImDrawCmd referencing a non-uploaded texture and
            // trips the renderer's "tex_id != Invalid" assert. Wait one frame.
            if (img.tex && img.tex->Status == ImTextureStatus_OK && img.tex->TexID != ImTextureID_Invalid)
            {
                // Auto-fit on first display: scale so the whole image is
                // visible, but never magnify past 1:1 (small images stay
                // crisp at native size). Avoids a huge image opening at 1:1
                // and overflowing the window.
                if (!img.fitted && img.w > 0 && img.h > 0)
                {
                    auto avail = ImGui::GetContentRegionAvail();
                    if (avail.x > 1.0f && avail.y > 1.0f)
                    {
                        float zx = avail.x / (float)img.w;
                        float zy = avail.y / (float)img.h;
                        img.zoom = (std::min)(1.0f, (std::min)(zx, zy));
                        img.fitted = true;
                    }
                }
                ImGui::Image(img.tex->GetTexRef(),
                             ImVec2(img.w * img.zoom, img.h * img.zoom));
            }
            else
            {
                ImGui::TextDisabled("loading…");
            }
            middleMousePanScroll(10); // image viewport
            ImGui::EndChild();
        }
        ImGui::End();
        ++it;
    }
}

// (PDF viewer moved into the pdfview plugin — plugins/pdfview/pdf_plugin.cpp.)

// ── Font discovery + dynamic loading ────────────────────────────────

void Editor::discoverFonts()
{
    availableFonts.clear();
    std::vector<std::filesystem::path> roots;
#ifdef _WIN32
    if (auto win = std::getenv("WINDIR"))
        roots.emplace_back(std::filesystem::path(win) / "Fonts");
    else
        roots.emplace_back("C:\\Windows\\Fonts");
    if (auto local = std::getenv("LOCALAPPDATA"))
        roots.emplace_back(std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts");
#else
    roots.emplace_back("/usr/share/fonts");
    roots.emplace_back("/usr/local/share/fonts");
    if (auto home = std::getenv("HOME"))
        roots.emplace_back(std::filesystem::path(home) / ".fonts");
#endif
    std::error_code ec;
    for (auto &root : roots)
    {
        if (!std::filesystem::exists(root, ec))
            continue;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file(ec))
                continue;
            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".ttf" || ext == ".otf")
                availableFonts.push_back(it->path().string());
        }
    }
    // Likely-monospace fonts sort first (this is a grid editor; mono is the
    // right pick). Heuristic on the filename — cheap and good enough; the
    // authoritative non-monospace warning on the active font (advance-based)
    // still fires if a mis-sorted font is selected. Within each group: A→Z.
    std::sort(availableFonts.begin(), availableFonts.end(),
              [](const std::string &a, const std::string &b) {
                  auto lower = [](std::string s) {
                      std::transform(s.begin(), s.end(), s.begin(),
                                     [](unsigned char c) { return (char)std::tolower(c); });
                      return s;
                  };
                  std::string fa = lower(std::filesystem::path(a).filename().string());
                  std::string fb = lower(std::filesystem::path(b).filename().string());
                  bool ma = fontNameLooksMonospace(fa);
                  bool mb = fontNameLooksMonospace(fb);
                  if (ma != mb)
                      return ma; // monospace group first
                  return fa < fb;
              });
}

// Filename heuristic for "probably a monospace font". Catches the common
// naming conventions; not authoritative (the advance-based warning is).
bool Editor::fontNameLooksMonospace(const std::string &lowerName)
{
    static const char *hints[] = {
        "mono",
        "consol",
        "courier",
        "cascadia",
        "code",
        "fixed",
        "term",
        "jetbrains",
        "firacode",
        "fira code",
        "hack",
        "inconsolata",
        "sourcecodepro",
        "source code",
        "ubuntu mono",
        "dejavusansmono",
        "liberation mono",
        "menlo",
        "andale",
        "pragmata",
        "iosevka",
        "noto mono",
        "sfmono",
        "victor mono",
        "space mono",
        "anonymous",
        "operator mono",
    };
    for (auto h : hints)
        if (lowerName.find(h) != std::string::npos)
            return true;
    return false;
}

void Editor::applyFont()
{
    activeFont = nullptr;
    if (fontPath.empty())
        return;
    std::error_code ec;
    if (!std::filesystem::exists(fontPath, ec))
        return;
    ImFontConfig cfg;
    auto leaf = std::filesystem::path(fontPath).stem().string();
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", leaf.c_str());
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    // Bake at the user's editor size (not a hardcoded 15) so the atlas is sharp
    // at the size we actually render. Clamp to the Settings slider range.
    float bake = (prefFontSize >= 8.0f && prefFontSize <= 40.0f) ? prefFontSize : 15.0f;
    activeFont = ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath.c_str(), bake, &cfg);
}

// ── Settings dialog + persistence ────────────────────────────────────
//
// Tiny hand-rolled "lines of key=value" serializer at <configDir>/settings.txt.
// Sections are headers in square brackets. Avoid a real JSON dep for now
// since the surface is small.

static std::filesystem::path settingsPath()
{
    return Editor::userConfigDir() / "settings.txt";
}

void Editor::loadSettings()
{
    std::ifstream f(settingsPath());
    if (!f.is_open())
        return;
    std::string section;
    std::string line;
    while (std::getline(f, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (section == "interpreters")
            interpreterOverrides[k] = v;
        else if (section == "filetypes")
            extLanguageOverrides()[k] = v;
        else if (section == "build")
            projectBuildOverrides[k] = v;
        else if (section == "debug_adapters")        // ".ext" -> adapter cmdline
            debugAdapterOverrides[k] = v;
        else if (section == "debug_bridge")          // raddbg/devenv paths + verb templates
            debugBridgeSettings[k] = v;
        else if (section == "debug_project_adapter") // <project root> -> adapter cmdline
            dbgProjectAdapter[k] = v;
        else if (section == "debug_project_target")  // <project root> -> "program|args"
            dbgProjectTarget[k] = v;
        else if (section == "run")                   // <project root> -> "cmd|dir" (F5 pin)
            projectRunOverrides[k] = v;
        else if (section == "palette_usage")         // action id -> "uses|lastEpoch"
        {
            auto bar = v.find('|');
            PaletteUse u;
            u.uses = std::atoi(v.c_str());
            if (bar != std::string::npos)
                u.last = std::strtoll(v.c_str() + bar + 1, nullptr, 10);
            paletteUsage[k] = u;
        }
        else if (section == "keybinds")
        {
            if (!v.empty())
                keybindOverrides[k] = v;
        }
        else if (section == "editor")
        {
            if (k == "auto_indent")
                prefAutoIndent = (v == "1" || v == "true");
            else if (k == "complete_pairs")
                prefCompletePairs = (v == "1" || v == "true");
            else if (k == "format_brace_newline")
                prefFormatBraceNewLine = (v == "1" || v == "true");
            else if (k == "format_brace_cpp")
                prefFormatBraceCpp = (v == "1" || v == "true");
            else if (k == "format_brace_cs")
                prefFormatBraceCs = (v == "1" || v == "true");
            else if (k == "format_brace_js")
                prefFormatBraceJs = (v == "1" || v == "true");
            else if (k == "format_brace_java")
                prefFormatBraceJava = (v == "1" || v == "true");
            else if (k == "show_fps")
                prefShowFps = (v == "1" || v == "true");
            else if (k == "theme")
            {
                prefTheme = std::atoi(v.c_str());
                if (prefTheme < 0 || prefTheme >= themeCount())
                    prefTheme = 0;
            }
            else if (k == "ctrl_scroll_zoom")
                prefCtrlScrollZoom = (v == "1" || v == "true");
            else if (k == "autocomplete")
                autocomplete = (v == "1" || v == "true");
            else if (k == "pan_invert")                    // renamed from invert_pan so the old saved
                prefInvertPan = (v == "1" || v == "true"); // value is dropped → new inverted default applies once
            else if (k == "auto_update")
                prefAutoUpdate = (v == "1" || v == "true");
            else if (k == "last_update_check")
                lastUpdateCheckEpoch = std::strtoll(v.c_str(), nullptr, 10);
            else if (k == "auto_save")
                prefAutoSave = (v == "1" || v == "true");
            else if (k == "auto_save_sec")
                prefAutoSaveSec = std::atoi(v.c_str());
            else if (k == "update_channel")
                prefUpdateChannel = std::atoi(v.c_str());
            else if (k == "pan_scroll_accel")
                prefPanScrollAccel = std::strtof(v.c_str(), nullptr);
            else if (k == "word_wrap")
                prefWordWrap = (v == "1" || v == "true");
            else if (k == "wrap_width")
                prefWrapWidthPx = std::atoi(v.c_str());
            else if (k == "fps_limit")
                prefFpsLimit = std::atoi(v.c_str());
            else if (k == "idle_throttle")
                prefIdleThrottle = (v == "1" || v == "true");
            else if (k == "live_coding")
                prefLiveCoding = (v == "1" || v == "true");
            else if (k == "font_size")
                prefFontSize = std::strtof(v.c_str(), nullptr);
            else if (k == "font_path")
                fontPath = v;
            else if (k == "nav_show_dot")
                navShowDotFiles = (v == "1" || v == "true");
            else if (k == "nav_show_excl")
                navShowExcluded = (v == "1" || v == "true");
            else if (k == "nav_code_only")
                navCodeOnly = (v == "1" || v == "true");
            else if (k == "nav_flat")
                navFlatFiles = (v == "1" || v == "true");
            else if (k == "nav_sort")
            {
                navSortMode = std::atoi(v.c_str());
                if (navSortMode < 0 || navSortMode > 3)
                    navSortMode = 0;
            }
            else if (k == "extra_src")
            {
                // Canonicalize + case-insensitive de-dupe. Earlier builds could
                // persist the same root repeatedly (different case/slashes beat
                // the exact-string check), and every duplicate rendered another
                // identical "(source)" tree in the nav panel — with clashing
                // ImGui IDs on their context menus.
                if (!v.empty())
                {
                    std::error_code cec;
                    auto canon = std::filesystem::weakly_canonical(v, cec);
                    std::string key = cec ? v : canon.string();
                    while (key.size() > 1 && (key.back() == '\\' || key.back() == '/'))
                        key.pop_back();
                    bool dup = std::any_of(extraSourceLocations.begin(), extraSourceLocations.end(),
                                           [&](const std::string &e) {
#ifdef _WIN32
                                               return _stricmp(e.c_str(), key.c_str()) == 0;
#else
                                               return e == key;
#endif
                                           });
                    if (!dup)
                        extraSourceLocations.push_back(key);
                }
            }
            else if (k == "nav_path_wrap")
                navPathWrap = (v == "1" || v == "true");
            else if (k == "nav_ue_source")
                navShowUeSource = (v == "1" || v == "true");
        }
        else if (section == "toolchains")
        {
            if (k == "msvc")
                activeMsvcPath = v;
            else if (k == "dotnet")
                activeDotnetPath = v;
            else if (k == "config")
                activeBuildConfig = v;
        }
        else if (section == "state")
        {
            if (k == "seen_first_run")
                seenFirstRun = (v == "1" || v == "true");
        }
        else if (section == "plugins")
        {
            // generic key -> bool store for plugins (enable toggles etc.);
            // read before registerAll() so plugins pick up their saved state.
            pluginFlags[k] = (v == "1" || v == "true");
        }
        else if (section == "recent_files")
        {
            if (!v.empty() &&
                std::find(recentFiles.begin(), recentFiles.end(), v) == recentFiles.end())
                recentFiles.push_back(v);
        }
        else if (section == "recent_projects")
        {
            if (!v.empty() &&
                std::find(recentProjects.begin(), recentProjects.end(), v) == recentProjects.end())
                recentProjects.push_back(v);
        }
        else if (section == "project_sessions")
        {
            // Format: <abs_root>=<file1>|<file2>|...
            std::vector<std::string> files;
            size_t pos = 0;
            while (pos < v.size())
            {
                size_t bar = v.find('|', pos);
                if (bar == std::string::npos)
                    bar = v.size();
                if (bar > pos)
                    files.push_back(v.substr(pos, bar - pos));
                pos = bar + 1;
            }
            if (!files.empty())
                projectSessions[k] = std::move(files);
        }
    }
    if (recentFiles.size() > 20)
        recentFiles.resize(20);
    if (recentProjects.size() > 20)
        recentProjects.resize(20);
}

void Editor::rememberRecentFile(const std::string &path)
{
    if (path.empty() || path == "untitled")
        return;
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(path, ec);
    std::string key = ec ? path : canon.string();
    auto it = std::find(recentFiles.begin(), recentFiles.end(), key);
    if (it != recentFiles.end())
        recentFiles.erase(it);
    recentFiles.insert(recentFiles.begin(), key);
    if (recentFiles.size() > 20)
        recentFiles.resize(20);
}

void Editor::rememberRecentProject(const std::string &path)
{
    if (path.empty())
        return;
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(path, ec);
    std::string key = ec ? path : canon.string();
    auto it = std::find(recentProjects.begin(), recentProjects.end(), key);
    if (it != recentProjects.end())
        recentProjects.erase(it);
    recentProjects.insert(recentProjects.begin(), key);
    if (recentProjects.size() > 20)
        recentProjects.resize(20);
}

void Editor::saveSettings()
{
    std::error_code ec;
    std::filesystem::create_directories(settingsPath().parent_path(), ec);
    std::ofstream f(settingsPath(), std::ios::trunc);
    if (!f.is_open())
        return;
    f << "# imguicolortext settings — generated by the editor's Settings dialog.\n";
    f << "[editor]\n";
    f << "auto_indent=" << (prefAutoIndent ? "1" : "0") << "\n";
    f << "complete_pairs=" << (prefCompletePairs ? "1" : "0") << "\n";
    f << "format_brace_newline=" << (prefFormatBraceNewLine ? "1" : "0") << "\n";
    f << "format_brace_cpp=" << (prefFormatBraceCpp ? "1" : "0") << "\n";
    f << "format_brace_cs=" << (prefFormatBraceCs ? "1" : "0") << "\n";
    f << "format_brace_js=" << (prefFormatBraceJs ? "1" : "0") << "\n";
    f << "format_brace_java=" << (prefFormatBraceJava ? "1" : "0") << "\n";
    f << "show_fps=" << (prefShowFps ? "1" : "0") << "\n";
    f << "theme=" << prefTheme << "\n";
    f << "ctrl_scroll_zoom=" << (prefCtrlScrollZoom ? "1" : "0") << "\n";
    f << "autocomplete=" << (autocomplete ? "1" : "0") << "\n";
    f << "pan_invert=" << (prefInvertPan ? "1" : "0") << "\n";
    f << "pan_scroll_accel=" << prefPanScrollAccel << "\n";
    f << "auto_update=" << (prefAutoUpdate ? "1" : "0") << "\n";
    f << "last_update_check=" << lastUpdateCheckEpoch << "\n";
    f << "auto_save=" << (prefAutoSave ? "1" : "0") << "\n";
    f << "auto_save_sec=" << prefAutoSaveSec << "\n";
    f << "update_channel=" << prefUpdateChannel << "\n";
    f << "word_wrap=" << (prefWordWrap ? "1" : "0") << "\n";
    f << "wrap_width=" << prefWrapWidthPx << "\n";
    f << "fps_limit=" << prefFpsLimit << "\n";
    f << "idle_throttle=" << (prefIdleThrottle ? "1" : "0") << "\n";
    f << "live_coding=" << (prefLiveCoding ? "1" : "0") << "\n";
    f << "nav_show_dot=" << (navShowDotFiles ? "1" : "0") << "\n";
    f << "nav_show_excl=" << (navShowExcluded ? "1" : "0") << "\n";
    f << "nav_code_only=" << (navCodeOnly ? "1" : "0") << "\n";
    f << "nav_flat=" << (navFlatFiles ? "1" : "0") << "\n";
    f << "nav_sort=" << navSortMode << "\n";
    for (const auto &loc : extraSourceLocations)
        f << "extra_src=" << loc << "\n";
    f << "nav_path_wrap=" << (navPathWrap ? "1" : "0") << "\n";
    f << "nav_ue_source=" << (navShowUeSource ? "1" : "0") << "\n";
    f << "font_size=" << prefFontSize << "\n";
    if (!fontPath.empty())
        f << "font_path=" << fontPath << "\n";
    f << "\n";
    f << "[toolchains]\n";
    if (!activeMsvcPath.empty())
        f << "msvc=" << activeMsvcPath << "\n";
    if (!activeDotnetPath.empty())
        f << "dotnet=" << activeDotnetPath << "\n";
    f << "config=" << activeBuildConfig << "\n";
    f << "\n";
    f << "[state]\n";
    f << "seen_first_run=1\n\n";
    if (!pluginFlags.empty())
    {
        f << "[plugins]\n";
        for (auto &[k, v] : pluginFlags)
            f << k << "=" << (v ? "1" : "0") << "\n";
        f << "\n";
    }
    f << "[recent_files]\n";
    for (auto &p : recentFiles)
        f << "path=" << p << "\n";
    f << "\n[recent_projects]\n";
    for (auto &p : recentProjects)
        f << "path=" << p << "\n";
    f << "\n";
    f << "[interpreters]\n";
    for (auto &[k, v] : interpreterOverrides)
        f << k << "=" << v << "\n";
    f << "\n[filetypes]\n";
    for (auto &[k, v] : extLanguageOverrides())
        f << k << "=" << v << "\n";
    f << "\n[build]\n";
    for (auto &[k, v] : projectBuildOverrides)
        f << k << "=" << v << "\n";
    f << "\n[debug_adapters]\n";
    for (auto &[k, v] : debugAdapterOverrides)
        f << k << "=" << v << "\n";
    f << "\n[debug_bridge]\n";
    for (auto &[k, v] : debugBridgeSettings)
        f << k << "=" << v << "\n";
    f << "\n[debug_project_adapter]\n";
    for (auto &[k, v] : dbgProjectAdapter)
        f << k << "=" << v << "\n";
    f << "\n[debug_project_target]\n";
    for (auto &[k, v] : dbgProjectTarget)
        f << k << "=" << v << "\n";
    f << "\n[run]\n";
    for (auto &[k, v] : projectRunOverrides)
        f << k << "=" << v << "\n";
    f << "\n[palette_usage]\n";
    for (auto &[k, v] : paletteUsage)
        if (v.uses > 0 && k.find('=') == std::string::npos && k.find('\n') == std::string::npos)
            f << k << "=" << v.uses << "|" << v.last << "\n";
    f << "\n[keybinds]\n";
    for (auto &[k, v] : keybindOverrides)
        f << k << "=" << v << "\n";
    f << "\n[project_sessions]\n";
    for (auto &[root, files] : projectSessions)
    {
        if (files.empty())
            continue;
        f << root << "=";
        for (size_t i = 0; i < files.size(); ++i)
        {
            if (i)
                f << "|";
            f << files[i];
        }
        f << "\n";
    }
}

void Editor::detectToolchains()
{
    if (toolchainsDetected)
        return;
    toolchainsDetected = true;
    [[maybe_unused]] std::error_code ec;

#ifdef _WIN32
    // Enumerate MSVC installations under each VS edition.
    for (const char *edition : {"18", "2022", "2019"})
    {
        for (const char *flavour : {"Community", "Professional", "Enterprise", "BuildTools", "Preview"})
        {
            std::filesystem::path msvcRoot = std::string("C:\\Program Files\\Microsoft Visual Studio\\") + edition + "\\" + flavour + "\\VC\\Tools\\MSVC";
            if (!std::filesystem::is_directory(msvcRoot, ec))
                continue;
            // Use the error_code overloads throughout: the throwing directory
            // iterator increment / is_directory() abort the whole editor on a
            // single unreadable entry (dangling reparse point, permission
            // denied), which crashed the app when Settings scanned toolchains.
            std::error_code iec;
            for (auto it = std::filesystem::directory_iterator(msvcRoot, iec);
                 !iec && it != std::filesystem::directory_iterator(); it.increment(iec))
            {
                std::error_code dec;
                if (!it->is_directory(dec) || dec)
                    continue;
                auto cl = it->path() / "bin" / "Hostx64" / "x64" / "cl.exe";
                if (std::filesystem::is_regular_file(cl, ec))
                {
                    std::string ver = it->path().filename().string();
                    detectedMsvc.push_back({std::string("VS ") + edition + " " + flavour + " — " + ver, cl.string()});
                }
            }
        }
    }
    // Also accept an active env var if we were launched from a Dev Prompt.
    if (const char *envVer = std::getenv("VCToolsVersion"))
    {
        if (const char *envDir = std::getenv("VCToolsInstallDir"))
        {
            std::filesystem::path cl = std::filesystem::path(envDir) / "bin" / "Hostx64" / "x64" / "cl.exe";
            detectedMsvc.push_back({std::string("Active env — ") + envVer, cl.string()});
        }
    }
#endif

    if (activeMsvcPath.empty() && !detectedMsvc.empty())
        activeMsvcPath = detectedMsvc.front().path;

    // .NET SDKs via `dotnet --list-sdks`, on a DETACHED background thread.
    // Running it synchronously here is what hung the Settings dialog: dotnet's
    // _popen can block for seconds, or indefinitely on first-run/telemetry
    // prompts. Results land in detectedDotnetSdks via pollDotnetProbe() on the
    // main thread. Captures `dotnetState` by value so it survives Editor
    // destruction (script-runner pattern).
    auto ds = dotnetState;
    std::thread([ds]() {
#ifdef _WIN32
        FILE *p = _popen("dotnet --list-sdks 2>NUL", "r");
#else
        FILE *p = popen("dotnet --list-sdks 2>/dev/null", "r");
#endif
        std::vector<DetectedTool> found;
        if (p)
        {
            char buf[256];
            while (fgets(buf, sizeof(buf), p))
            {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty())
                    continue;
                auto br = line.find('[');
                std::string ver = (br != std::string::npos) ? line.substr(0, br) : line;
                while (!ver.empty() && ver.back() == ' ')
                    ver.pop_back();
                std::string path = (br != std::string::npos && line.back() == ']')
                                       ? line.substr(br + 1, line.size() - br - 2)
                                       : "";
                found.push_back({ver, path});
            }
#ifdef _WIN32
            _pclose(p);
#else
            pclose(p);
#endif
        }
        std::lock_guard<std::mutex> lock(ds->mutex);
        ds->sdks = std::move(found);
        ds->done = true; })
        .detach();
}

// Publish background `dotnet --list-sdks` results into detectedDotnetSdks once
// the probe finishes. Cheap to call every frame (an atomic check until done).
void Editor::pollDotnetProbe()
{
    if (dotnetPublished || !dotnetState->done.load())
        return;
    std::lock_guard<std::mutex> lock(dotnetState->mutex);
    detectedDotnetSdks = dotnetState->sdks;
    if (activeDotnetPath.empty() && !detectedDotnetSdks.empty())
        activeDotnetPath = "dotnet";
    dotnetPublished = true;
}

void Editor::renderSettings()
{
    if (!settingsVisible)
        return;
    detectToolchains();
    pollDotnetProbe();
    ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
    // On the frame the window is (re)opened we must NOT treat the menu click that
    // opened it as a "click outside" dismissal.
    const bool justOpened = settingsFocusRequest;
    // Clicking File → Settings while the window is already open (possibly
    // collapsed or behind other windows) should restore + focus it.
    if (settingsFocusRequest)
    {
        // Multi-viewport is always on, so a floating Settings window can become its
        // own OS window — and SetNextWindowFocus() raises the ImGui window but NOT
        // the OS window, so it stayed buried behind the main app window. Pin it into
        // the MAIN viewport (rendered inside the main OS window — nothing can cover
        // it) and center it so it's always on-screen, uncollapsed, and focused.
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        settingsFocusRequest = false;
    }
    // NoDocking keeps Settings a floating dialog (a docked tab can't be raised to
    // the top by focus alone).
    bool winOpen = ImGui::Begin("Settings##editorSettings", &settingsVisible, ImGuiWindowFlags_NoDocking);
    // Capture focus/hover while the settings window is the current window — used
    // below (after End) for Esc / click-outside dismissal.
    bool settingsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    bool settingsHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    if (winOpen)
    {
        if (ImGui::BeginTabBar("##settingsTabs"))
        {
            if (ImGui::BeginTabItem("Editor"))
            {
                ImGui::Checkbox("Auto-indent on new line", &prefAutoIndent);
                ImGui::Checkbox("Auto-complete matching brackets / quotes", &prefCompletePairs);
                ImGui::TextDisabled("Format Document — braces on their own line (Allman), per language:");
                ImGui::Checkbox("C / C++", &prefFormatBraceCpp);
                ImGui::SameLine();
                ImGui::Checkbox("C#", &prefFormatBraceCs);
                ImGui::SameLine();
                ImGui::Checkbox("JS / TS", &prefFormatBraceJs);
                ImGui::SameLine();
                ImGui::Checkbox("Java", &prefFormatBraceJava);
                ImGui::SameLine();
                ImGui::Checkbox("Other", &prefFormatBraceNewLine);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Code > Format Document (Alt+Shift+F) runs clang-format.\nChecked = brace on next line (Allman); unchecked = attached (same line).");
                ImGui::Checkbox("Show FPS on status bar", &prefShowFps);
                ImGui::Checkbox("Ctrl + scroll wheel adjusts editor font size", &prefCtrlScrollZoom);
                ImGui::Checkbox("Autosave", &prefAutoSave);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Periodically save documents that already have a path.\nNew (untitled) buffers are skipped — they need Save As first.");
                if (prefAutoSave)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::SliderInt("every (s)", &prefAutoSaveSec, 5, 300))
                        lastAutoSave = 0.0; // apply the new interval promptly
                }
                ImGui::Checkbox("Invert middle-mouse pan direction", &prefInvertPan);
                ImGui::SetNextItemWidth(220.0f);
                ImGui::SliderFloat("Pan/scroll acceleration", &prefPanScrollAccel, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Middle-mouse scroll speed-up by distance from the click point.\n0 = constant speed (no acceleration).");
                ImGui::Checkbox("Word wrap", &prefWordWrap);
                if (prefWordWrap)
                {
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::SliderInt("Wrap width (px, 0 = fit window)", &prefWrapWidthPx, 0, 2000);
                }
                ImGui::SliderFloat("Editor font size", &prefFontSize, 10.0f, 28.0f, "%.0f");

                if (ImGui::Button("Open user languages folder"))
                {
                    std::error_code lec;
                    auto dir = userConfigDir() / "languages";
                    std::filesystem::create_directories(dir, lec);
                    seedUserLanguages(); // populate with editable copies if still empty
                    navOpenPathInExplorer(dir.string());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Drop your own / edited *.lang files here (writable, unlike a\nProgram Files install). They override or extend the bundled set on next launch.");

                ImGui::Separator();
                ImGui::TextDisabled("Performance");
                // 0 = unlimited. Clamp display so the slider can reach "off".
                ImGui::SliderInt("Max FPS (0 = unlimited)", &prefFpsLimit, 0, 240);
                ImGui::Checkbox("Throttle to ~10 FPS when window unfocused (idle)", &prefIdleThrottle);

                ImGui::Spacing();
                ImGui::TextDisabled("Unreal");
                if (ImGui::Checkbox("Live coding owns F11 (don't toggle Focus Mode)", &prefLiveCoding))
                {
                    liveCodingToastShown = false;
                    saveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("With an Unreal project open, Unreal Live Coding compiles on Ctrl+Alt+F11.\n"
                                      "While this is on, the editor leaves F11 alone \xe2\x80\x94 use View \xe2\x96\xb8 Focus Mode instead.\n"
                                      "A chorded F11 (Ctrl/Alt/Shift) never toggles Focus Mode either way.");
                ImGui::SameLine();
                ImGui::TextDisabled(projectIsUnreal() ? "(Unreal project open)" : "(no Unreal project)");

                if (!tabs.empty())
                {
                    auto &e = doc().editor;
                    e.SetAutoIndentEnabled(prefAutoIndent);
                    e.SetCompletePairedGlyphs(prefCompletePairs);
                }
                // Pan-invert + word-wrap apply to every open doc, not just the active one.
                for (auto &up : tabs)
                {
                    up->editor.SetPanInverted(prefInvertPan);
                    up->editor.SetPanScrollAccel(prefPanScrollAccel);
                    up->diff.SetPanInverted(prefInvertPan);
                    up->diff.SetPanScrollAccel(prefPanScrollAccel);
                    up->editor.SetWordWrap(prefWordWrap);
                    up->editor.SetWrapWidth(static_cast<float>(prefWrapWidthPx));
                }
                fontSize = prefFontSize;

                // Font selector — enumerate system TTFs once, then pick by filename.
                // "(Bundled DejaVu)" maps to fontPath empty / activeFont nullptr.
                if (availableFonts.empty())
                    discoverFonts();
                std::string current = fontPath.empty()
                                          ? std::string("(Bundled DejaVu)")
                                          : std::filesystem::path(fontPath).filename().string();
                if (ImGui::BeginCombo("Editor font", current.c_str()))
                {
                    if (ImGui::Selectable("(Bundled DejaVu)", fontPath.empty()))
                    {
                        fontPath.clear();
                        activeFont = nullptr;
                    }
                    // availableFonts is pre-sorted monospace-first. Drop a labeled
                    // separator at the boundary so the recommended (mono) fonts are
                    // visually grouped above the rest.
                    bool emittedNonMonoHeader = false;
                    for (auto &p : availableFonts)
                    {
                        std::string name = std::filesystem::path(p).filename().string();
                        std::string lname = name;
                        std::transform(lname.begin(), lname.end(), lname.begin(),
                                       [](unsigned char c) { return (char)std::tolower(c); });
                        if (!emittedNonMonoHeader && !fontNameLooksMonospace(lname))
                        {
                            emittedNonMonoHeader = true;
                            ImGui::Separator();
                            ImGui::TextDisabled("Other fonts (not monospace — may look uneven)");
                        }
                        bool selected = (p == fontPath);
                        if (ImGui::Selectable(name.c_str(), selected))
                        {
                            fontPath = p;
                            applyFont();
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                // This is a monospace (grid) editor: every column is one cell wide.
                // A proportional font (Comic Sans, Arial, ...) gets each glyph
                // jammed into a fixed cell, so narrow letters look gappy and wide
                // ones cramped. Detect it by comparing a narrow vs wide glyph's
                // advance under the active font and warn — the fix is to pick a
                // monospace font, not a spacing tweak.
                {
                    ImFont *f = activeFont ? activeFont : ImGui::GetFont();
                    if (f)
                    {
                        ImGui::PushFont(f, prefFontSize);
                        float wi = ImGui::CalcTextSize("i").x;
                        float wm = ImGui::CalcTextSize("M").x;
                        ImGui::PopFont();
                        if (wi > 0.0f && std::fabs(wm - wi) > 0.5f)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
                            ImGui::TextWrapped("This font is not monospaced — text will look unevenly spaced "
                                               "in the editor grid. Pick a fixed-width font (e.g. Consolas, "
                                               "Cascadia Mono, DejaVu Sans Mono) for proper alignment.");
                            ImGui::PopStyleColor();
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            // Only present when at least one feature plugin is compiled in
            // (IMGUIIDE_PLUGIN_*). A core build has none, so no tab appears.
            if (!pluginRegistry.all().empty() && ImGui::BeginTabItem("Plugins"))
            {
                ImGui::TextDisabled("Feature plugins compiled into this build. Disabling one removes its\n"
                                    "menus, windows and language extras at runtime (no restart needed).");
                ImGui::Separator();
                for (auto &p : pluginRegistry.all())
                {
                    bool on = p->enabled();
                    std::string label = std::string(p->displayName()) + "##plugin_" + p->id();
                    if (ImGui::Checkbox(label.c_str(), &on))
                    {
                        pluginRegistry.setEnabled(*this, *p, on);
                        saveSettings();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", p->id());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Interpreters"))
            {
                ImGui::TextDisabled("Run command for F5, per file type. Empty = use OS default.");
                ImGui::Separator();
                // Each row: language label (e.g. "Python (.py, .pyw)") + input.
                // Group extensions that share an interpreter so we don't show
                // .py and .pyw as two rows for the same Python config.
                struct LangRow
                {
                    const char *name;
                    std::vector<const char *> exts;
                    const char *def;
                };
                static const LangRow rows[] = {
                    {"Python", {".py", ".pyw"}, "python"},
                    {"PowerShell", {".ps1"}, "powershell -NoProfile -ExecutionPolicy Bypass -File"},
                    {"Bash", {".sh"}, "bash"},
                    {"Batch", {".bat", ".cmd"}, ""}, // direct via cmd.exe
                    {"Lua", {".lua"}, "lua"},
                    {"JavaScript", {".js"}, "node"},
                };
                for (size_t i = 0; i < IM_ARRAYSIZE(rows); ++i)
                {
                    ImGui::PushID((int)i);
                    // Label on its own line (input goes full-width below, otherwise
                    // SetNextItemWidth(-FLT_MIN) pushes the right-side label off-screen).
                    std::string label = rows[i].name;
                    label += " (";
                    for (size_t e = 0; e < rows[i].exts.size(); ++e)
                    {
                        if (e)
                            label += ", ";
                        label += rows[i].exts[e];
                    }
                    label += ")";
                    ImGui::TextUnformatted(label.c_str());
                    // Read first ext's value as the canonical, write to all exts
                    // so editing once applies to .py and .pyw together.
                    auto &canon = interpreterOverrides[rows[i].exts[0]];
                    if (canon.empty())
                        canon = rows[i].def;
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", canon.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##interp", buf, sizeof(buf)))
                    {
                        canon = buf;
                        for (auto ext : rows[i].exts)
                            interpreterOverrides[ext] = canon;
                    }
                    ImGui::Spacing();
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Toolchains"))
            {
                // MSVC: pick from detected installs, or hand-edit the cl.exe path.
                ImGui::TextDisabled("C / C++ compiler (MSVC). Detected installs:");
                if (detectedMsvc.empty())
                {
                    ImGui::BulletText("(none detected — install Visual Studio's C++ workload, or set %%VCToolsInstallDir%%)");
                }
                else
                {
                    if (ImGui::BeginCombo("##msvcSel",
                                          activeMsvcPath.empty() ? "(none selected)" : activeMsvcPath.c_str()))
                    {
                        for (auto &t : detectedMsvc)
                        {
                            bool selected = (t.path == activeMsvcPath);
                            std::string item = t.label + "   " + t.path;
                            if (ImGui::Selectable(item.c_str(), selected))
                                activeMsvcPath = t.path;
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
                {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf), "%s", activeMsvcPath.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##msvcPath", buf, sizeof(buf)))
                        activeMsvcPath = buf;
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled(".NET SDK. Detected SDKs:");
                if (detectedDotnetSdks.empty())
                {
                    ImGui::BulletText("(dotnet --list-sdks returned nothing — install .NET SDK)");
                }
                else
                {
                    if (ImGui::BeginCombo("##dotnetSel",
                                          activeDotnetPath.empty() ? "(none selected)" : activeDotnetPath.c_str()))
                    {
                        // "dotnet" is on PATH — that's the common case.
                        bool isPath = (activeDotnetPath == "dotnet");
                        if (ImGui::Selectable("dotnet (on PATH)", isPath))
                            activeDotnetPath = "dotnet";
                        if (isPath)
                            ImGui::SetItemDefaultFocus();
                        for (auto &sdk : detectedDotnetSdks)
                        {
                            std::string item = "SDK " + sdk.label + "   " + sdk.path;
                            bool sel = (activeDotnetPath == sdk.path);
                            if (ImGui::Selectable(item.c_str(), sel))
                                activeDotnetPath = sdk.path;
                        }
                        ImGui::EndCombo();
                    }
                }
                {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf), "%s", activeDotnetPath.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##dotnetPath", buf, sizeof(buf)))
                        activeDotnetPath = buf;
                }
                ImGui::Spacing();
                ImGui::Separator();
                // Build configuration — applied to MSBuild / CMake / dotnet presets.
                // Tokens %CONFIG% expand to this in any build command.
                ImGui::TextDisabled("Build configuration (substituted as %%CONFIG%% in build commands):");
                static const char *configs[] = {
                    "Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
                int curIdx = 0;
                for (int i = 0; i < IM_ARRAYSIZE(configs); ++i)
                    if (activeBuildConfig == configs[i])
                        curIdx = i;
                if (ImGui::Combo("##buildCfg", &curIdx, configs, IM_ARRAYSIZE(configs)))
                    activeBuildConfig = configs[curIdx];
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Build"))
            {
                ImGui::TextDisabled("Per-project build command (F6). Keyed by absolute project root.");
                ImGui::Separator();

                // Preset commands the user can pick from for a fresh override.
                static const char *presets[] = {
                    "dotnet build",
                    "msbuild /m /v:minimal",
                    "cmake --build .",
                    "cmake --build build",
                    "cmake --build out/build/x64-Debug",
                    "ninja",
                    "make",
                    "make -j8",
                };
                static int presetIdx = 0;
                ImGui::Combo("Preset", &presetIdx, presets, IM_ARRAYSIZE(presets));
                ImGui::SameLine();
                if (!projectRoot.empty())
                {
                    if (ImGui::Button("Apply to current project"))
                        projectBuildOverrides[projectRoot.string()] = presets[presetIdx];
                }
                else
                {
                    ImGui::TextDisabled("(no current project — Open Project... first)");
                }
                ImGui::Spacing();

                if (projectBuildOverrides.empty())
                {
                    ImGui::TextDisabled("(no overrides yet)");
                }
                for (auto it = projectBuildOverrides.begin(); it != projectBuildOverrides.end();)
                {
                    ImGui::PushID(it->first.c_str());
                    ImGui::TextDisabled("%s", it->first.c_str());
                    char buf[512];
                    std::snprintf(buf, sizeof(buf), "%s", it->second.c_str());
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f);
                    if (ImGui::InputText("##cmd", buf, sizeof(buf)))
                        it->second = buf;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove"))
                    {
                        it = projectBuildOverrides.erase(it);
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::PopID();
                    ++it;
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Keybinds"))
            {
                ImGui::TextDisabled("Click a chord to record a new one. Esc cancels, Backspace clears.");
                ImGui::Separator();
                // Editable keybind catalogue. The default chord lives here as a
                // fallback so user overrides can be diffed/reset. Overrides go
                // into `keybindOverrides` (in-memory only for now — persist in a
                // follow-up if the user actually rebinds anything).
                struct Bind
                {
                    const char *id;
                    const char *action;
                    const char *defaultCombo;
                    const char *group;
                    bool rebindable;
                    const char *widgetAction;
                };
                static const Bind binds[] = {
                    {"file.new", "New tab", "Ctrl+N", "File", true, nullptr},
                    {"file.open", "Open file...", "Ctrl+O", "File", true, nullptr},
                    {"file.save", "Save", "Ctrl+S", "File", true, nullptr},
                    {"file.saveAs", "Save As...", "Ctrl+Shift+S", "File", true, nullptr},
                    {"file.close", "Close tab", "Ctrl+W", "File", true, nullptr},
                    {"file.reopen", "Reopen last closed tab", "Ctrl+Shift+T", "File", true, nullptr},
                    {"file.history", "File History", "Ctrl+I", "File", true, nullptr},

                    {"edit.undo", "Undo", "Ctrl+Z", "Edit", true, "undo"},
                    {"edit.redo", "Redo", "Ctrl+Y", "Edit", true, "redo"},
                    {"edit.cut", "Cut", "Ctrl+X", "Edit", true, "cut"},
                    {"edit.copy", "Copy", "Ctrl+C", "Edit", true, "copy"},
                    {"edit.paste", "Paste", "Ctrl+V", "Edit", true, "paste"},
                    {"edit.selAll", "Select all", "Ctrl+A", "Edit", true, "selectAll"},
                    {"edit.addOcc", "Add next occurrence", "Ctrl+D", "Edit", true, "addNextOccurrence"},
                    {"edit.selAllOcc", "Select all occurrences", "Ctrl+Shift+D", "Edit", true, "selectAllOccurrences"},
                    {"edit.indent", "Indent line(s)", "Ctrl+]", "Edit", true, "indent"},
                    {"edit.deindent", "De-indent line(s)", "Ctrl+[", "Edit", true, "deindent"},
                    {"edit.comment", "Toggle comments", "Ctrl+/", "Edit", true, "toggleComments"},
                    {"edit.moveUp", "Move line(s) up", "Alt+UpArrow", "Edit", true, "moveLineUp"},
                    {"edit.moveDown", "Move line(s) down", "Alt+DownArrow", "Edit", true, "moveLineDown"},

                    {"find.find", "Find", "Ctrl+F", "Find", true, "find"},
                    {"find.next", "Find next", "F3", "Find", true, "findNext"},
                    {"find.findAll", "Find all", "Ctrl+Shift+G", "Find", true, "findAll"},
                    {"find.inFiles", "Find in Files", "Ctrl+Shift+F", "Find", true, nullptr},
                    {"find.goto", "Go to Line...", "Ctrl+G", "Find", true, nullptr},

                    {"code.foldAll", "Fold all", "Ctrl+0", "Code", true, "foldAll"},
                    {"code.unfoldAll", "Unfold all", "Ctrl+J", "Code", true, "unfoldAll"},
                    {"code.foldCur", "Fold current", "Ctrl+Shift+[", "Code", true, "foldCurrent"},
                    {"code.unfoldCur", "Unfold current", "Ctrl+Shift+]", "Code", true, "unfoldCurrent"},
                    {"code.upper", "Selection -> UPPERCASE", "Ctrl+K Ctrl+U", "Code", true, "upperCase"},
                    {"code.lower", "Selection -> lowercase", "Ctrl+K Ctrl+L", "Code", true, "lowerCase"},
                    {"code.hSrc", "Switch Header / Source", "Alt+O", "Code", true, nullptr},
                    {"code.format", "Format Document", "Alt+Shift+F", "Code", true, nullptr},
                    {"nav.back", "Navigate back", "Ctrl+Alt+LeftArrow", "Code", true, nullptr},
                    {"nav.forward", "Navigate forward", "Ctrl+Alt+RightArrow", "Code", true, nullptr},

                    {"view.splitR", "Split tab right", "Ctrl+\\", "View", true, nullptr},
                    {"view.zoomIn", "Zoom in", "Ctrl++", "View", true, nullptr},
                    {"view.zoomOut", "Zoom out", "Ctrl+-", "View", true, nullptr},
                    {"view.cycleNext", "Cycle tabs forward", "Ctrl+Tab", "View", true, nullptr},
                    {"view.cyclePrev", "Cycle tabs backward", "Ctrl+Shift+Tab", "View", true, nullptr},

                    {"proj.run", "Run", "F5", "Project", true, nullptr},
                    {"proj.build", "Build project", "F6", "Project", true, nullptr},
                    {"view.palette", "Command palette", "Ctrl+Shift+P", "View", true, nullptr},
                };

                // Plugin-contributed binds join the catalogue under each plugin's
                // own group (rebindable + persisted like the built-ins). The
                // string storage lives in pluginKeybinds; the Bind rows only view
                // it, so refresh the member first.
                pluginKeybinds.clear();
                pluginRegistry.keybinds(*this, pluginKeybinds);
                std::vector<Bind> allBinds(std::begin(binds), std::end(binds));
                for (auto &pk : pluginKeybinds)
                    allBinds.push_back({pk.id.c_str(), pk.action.c_str(), pk.defaultChord.c_str(),
                                        pk.group.c_str(), true, nullptr});

                static std::string capturingId; // id of the row currently waiting for a chord
                static int capturingFrame = 0;  // frame we started — avoid catching the click that opened us
                static int curFrame = 0;
                ++curFrame;
                static std::string captureStroke1; // first combo of a two-stroke chord being recorded, or empty
                // Mirror capture state to the member so the global shortcut dispatch
                // can swallow keys while a chord is being recorded (1-frame lag is fine).
                keybindCapturing = !capturingId.empty();

                // Build the list of distinct groups in declaration order.
                std::vector<const char *> groupOrder;
                for (auto &b : allBinds)
                {
                    bool present = false;
                    for (auto g : groupOrder)
                        if (std::strcmp(g, b.group) == 0)
                        {
                            present = true;
                            break;
                        }
                    if (!present)
                        groupOrder.push_back(b.group);
                }

                auto chordFor = [&](const Bind &b) -> std::string {
                    auto it = keybindOverrides.find(b.id);
                    return it != keybindOverrides.end() ? it->second : b.defaultCombo;
                };

                // Capture-next-chord logic. We watch IsKeyPressed across the
                // modifier-aware keys and assemble a "Mod+Key" string.
                auto tryCaptureChord = [&](const std::string &targetId) {
                    if (capturingId != targetId)
                        return;
                    if (curFrame == capturingFrame)
                        return; // skip frame the popup opened on
                    ImGuiIO &io = ImGui::GetIO();
                    // A staged first stroke commits as a SINGLE chord if the user
                    // lets go of Ctrl without pressing a second combo.
                    if (!captureStroke1.empty() && !io.KeyCtrl)
                    {
                        keybindOverrides[targetId] = captureStroke1;
                        captureStroke1.clear();
                        applyKeybindOverridesToEditors();
                        saveSettings();
                        capturingId.clear();
                        return;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    {
                        capturingId.clear();
                        captureStroke1.clear();
                        return;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
                    {
                        keybindOverrides.erase(targetId);
                        applyKeybindOverridesToEditors();
                        saveSettings();
                        capturingId.clear();
                        captureStroke1.clear();
                        return;
                    }
                    // Walk every named key and grab the first non-modifier one
                    // pressed this frame. We must skip BOTH the physical modifier
                    // keys (Left/Right Ctrl/Shift/Alt/Super) AND the four
                    // ImGuiKey_ReservedForModXXX aliases — those live inside the
                    // NamedKey range too, and IsKeyPressed fires on them when a
                    // modifier goes down, which previously captured garbage like
                    // "Ctrl+" with no real key (the "polls mods twice" bug).
                    auto isModifierKey = [](ImGuiKey k) {
                        return k == ImGuiKey_LeftCtrl || k == ImGuiKey_RightCtrl ||
                               k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift ||
                               k == ImGuiKey_LeftAlt || k == ImGuiKey_RightAlt ||
                               k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper ||
                               k == ImGuiKey_ReservedForModCtrl ||
                               k == ImGuiKey_ReservedForModShift ||
                               k == ImGuiKey_ReservedForModAlt ||
                               k == ImGuiKey_ReservedForModSuper;
                    };
                    for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END;
                         k = (ImGuiKey)(k + 1))
                    {
                        if (isModifierKey(k))
                            continue;
                        if (!ImGui::IsKeyPressed(k, false))
                            continue;
                        // Assemble one combo: every held modifier + the key.
                        std::string combo;
                        if (io.KeyCtrl)
                            combo += "Ctrl+";
                        if (io.KeyShift)
                            combo += "Shift+";
                        if (io.KeyAlt)
                            combo += "Alt+";
                        if (io.KeySuper)
                            combo += "Super+";
                        const char *name = ImGui::GetKeyName(k);
                        if (!name || !name[0])
                            continue; // unnamed key — ignore
                        combo += name;

                        // Two-stroke recording (VSCode "Ctrl+K Ctrl+U" style): if this
                        // is the FIRST combo and Ctrl is still held, stage it and keep
                        // listening for a second combo. Pressing a second combo joins
                        // them with a space; pressing the same first combo again (or a
                        // non-Ctrl combo) commits a single chord. This makes both
                        // single and chord rebinds reachable without an extra button.
                        if (captureStroke1.empty())
                        {
                            if (io.KeyCtrl)
                            {
                                // stage as potential first stroke; commit if nothing follows
                                captureStroke1 = combo;
                                return; // keep capturing this frame's target
                            }
                            keybindOverrides[targetId] = combo; // plain single chord
                        }
                        else
                        {
                            // We already have a first stroke. A different second combo
                            // makes a two-stroke chord; repeating the first commits single.
                            if (combo == captureStroke1)
                                keybindOverrides[targetId] = combo; // user repeated → single
                            else
                                keybindOverrides[targetId] = captureStroke1 + " " + combo;
                            captureStroke1.clear();
                        }
                        applyKeybindOverridesToEditors();
                        saveSettings();
                        capturingId.clear();
                        return;
                    }
                };

                // One collapsible TreeNode per group, each containing its own
                // nested table so groups visually mean something.
                for (auto group : groupOrder)
                {
                    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
                    if (!ImGui::TreeNodeEx(group, ImGuiTreeNodeFlags_DefaultOpen))
                        continue;
                    if (ImGui::BeginTable((std::string("##kbtbl_") + group).c_str(), 3,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                              ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.10f);
                        for (auto &b : allBinds)
                        {
                            if (std::strcmp(b.group, group) != 0)
                                continue;
                            ImGui::PushID(b.id);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(b.action);
                            ImGui::TableNextColumn();
                            if (b.rebindable)
                            {
                                std::string label;
                                if (capturingId == b.id)
                                    label = captureStroke1.empty()
                                                ? std::string("press chord…")
                                                : (captureStroke1 + " , press 2nd (or release Ctrl)");
                                else
                                    label = chordFor(b);
                                if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)))
                                {
                                    capturingId = b.id;
                                    capturingFrame = curFrame;
                                    captureStroke1.clear(); // fresh capture
                                }
                                tryCaptureChord(b.id);
                            }
                            else
                            {
                                // Editor-internal / fixed chord — shown for reference, not rebindable here.
                                ImGui::TextDisabled("%s", chordFor(b).c_str());
                            }
                            ImGui::TableNextColumn();
                            if (b.rebindable && keybindOverrides.count(b.id) != 0)
                            {
                                if (ImGui::SmallButton("reset"))
                                {
                                    keybindOverrides.erase(b.id);
                                    applyKeybindOverridesToEditors();
                                    saveSettings();
                                }
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (ImGui::Button("Save"))
        {
            saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close"))
        {
            saveSettings();
            settingsVisible = false;
        }

        // Middle-mouse pan/scroll inside the settings window (same gesture as the editor).
        middleMousePanScroll(2);
    }
    ImGui::End();

    // Easy dismissal: Esc while the window is focused, or a primary click outside
    // it. Guards: skip on the frame it was opened (so the menu click that opened it
    // doesn't immediately count as an outside click), and whenever any popup is
    // open (font/combo dropdowns and the menu bar) so interacting with those
    // doesn't close it.
    if (settingsVisible)
    {
        bool anyPopup = ImGui::IsPopupOpen(nullptr,
                                           ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (settingsFocused && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            settingsVisible = false;
        }
        else if (!justOpened && !settingsHovered && !anyPopup &&
                 (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                  ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        {
            settingsVisible = false;
        }
    }

    // Persist on ANY close, including the window's [x] (which, unlike the Close
    // button, doesn't save) and the Esc / click-outside paths above. Without this,
    // a live slider edit is lost on close and reopening reloads the stale value
    // from disk — i.e. "the settings change didn't stick". settingsVisible was true
    // on entry (early-return above), so it being false now means it closed this frame.
    if (!settingsVisible)
        saveSettings();
}

// ── Diff against a chosen second file ────────────────────────────────
void Editor::openDiffOtherDialog()
{
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    IGFD::FileDialogConfig config;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_DontShowHiddenFiles;
    populateFileDialogPlaces();
    ImGuiFileDialog::Instance()->OpenDialog("diff-other", "Pick file to diff against active doc...", ".*", config);
    diffOtherMode = true;
}

void Editor::openContainingFolder()
{
    if (tabs.empty() || doc().filename == "untitled")
        return;
#ifdef _WIN32
    // "explorer /select,<path>" highlights the file inside its folder.
    std::string cmd = "/select,\"" + doc().filename + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" +
                      std::filesystem::path(doc().filename).parent_path().string() + "\"";
    [[maybe_unused]] int rc = std::system(cmd.c_str());
#endif
}

void Editor::reopenLastClosedTab()
{
    if (recentlyClosed.empty())
        return;
    ClosedTab last = std::move(recentlyClosed.back());
    recentlyClosed.pop_back();

    auto &t = newTab();
    t.filename = last.filename;
    t.originalText = last.text;
    t.editor.SetText(last.text);
    t.editor.SetLanguage(languageForPath(last.filename));
    t.version = t.editor.GetUndoIndex();
    t.wantFocus = true;
    buildAutocompleteTrie(t);
}

//
//  Editor::windowLabelFor
//

std::string Editor::windowLabelFor(const TabDocument &t) const
{
    std::string title = std::filesystem::path(t.filename).filename().string();
    if (title.empty())
        title = "untitled";
    if (t.externallyTouched)
        title = "\xe2\x9c\x8e " + title; // ✎ Claude edited this since you last viewed it
    if (t.editor.GetUndoIndex() != t.version)
        title += " *";
    title += "###Doc" + std::to_string(t.id);
    return title;
}

//
//  Editor::newFile
//

void Editor::newFile()
{
    newTab();
}

void Editor::newFile(std::string &path)
{
    // create a new untitled tab pre-named "path" (no read from disk)
    int idx = activeTab < tabs.size() ? static_cast<int>(activeTab) + 1 : -1;
    auto &t = newTab(path, /*split*/ false, idx);
    t.editor.SetLanguage(languageForPath(path));
}

//
//  Editor::openFile
//

// ── .editorconfig — per-project / per-language settings cascade ────────

// Wildcard match (*, ?) of a filename-only editorconfig pattern against `s`.
static bool ecWildMatch(const std::string &pat, const std::string &s)
{
    size_t pi = 0, si = 0, star = std::string::npos, ss = 0;
    while (si < s.size())
    {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si]))
        {
            ++pi;
            ++si;
        }
        else if (pi < pat.size() && pat[pi] == '*')
        {
            star = pi++;
            ss = si;
        }
        else if (star != std::string::npos)
        {
            pi = star + 1;
            si = ++ss;
        }
        else
            return false;
    }
    while (pi < pat.size() && pat[pi] == '*')
        ++pi;
    return pi == pat.size();
}

// Match an editorconfig section glob against a basename. Supports *, ?, **
// (treated as *), and one level of {a,b,c} brace alternation.
static bool ecGlobMatch(const std::string &pattern, const std::string &name)
{
    auto lb = pattern.find('{');
    if (lb != std::string::npos)
    {
        auto rb = pattern.find('}', lb);
        if (rb != std::string::npos)
        {
            std::string pre = pattern.substr(0, lb), post = pattern.substr(rb + 1);
            std::string inner = pattern.substr(lb + 1, rb - lb - 1);
            for (size_t start = 0;;)
            {
                size_t comma = inner.find(',', start);
                std::string alt = inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (ecGlobMatch(pre + alt + post, name))
                    return true;
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
            return false;
        }
    }
    std::string p = pattern;
    for (size_t i; (i = p.find("**")) != std::string::npos;)
        p.replace(i, 2, "*");
    auto sl = p.find_last_of("/\\");
    if (sl != std::string::npos)
        p = p.substr(sl + 1); // basename only
    return ecWildMatch(p, name);
}

// Does this .editorconfig have `root = true` in its preamble (before any section)?
static bool ecFileIsRoot(const std::filesystem::path &cfg)
{
    std::ifstream f(cfg);
    std::string line;
    while (std::getline(f, line))
    {
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos)
            continue;
        if (line[a] == '#' || line[a] == ';')
            continue;
        if (line[a] == '[')
            return false;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(a, eq - a);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
            k.pop_back();
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (k == "root")
        {
            std::string v = line.substr(eq + 1);
            size_t b = v.find_first_not_of(" \t");
            v = (b == std::string::npos) ? std::string() : v.substr(b);
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return v.rfind("true", 0) == 0;
        }
    }
    return false;
}

void Editor::applyEditorConfig(TabDocument &t)
{
    if (t.filename == "untitled")
        return;
    std::error_code ec;
    std::filesystem::path file = t.filename;
    std::string fname = file.filename().string();

    std::vector<std::filesystem::path> configs; // nearest first
    for (auto cur = file.parent_path();;)
    {
        auto cfg = cur / ".editorconfig";
        if (std::filesystem::is_regular_file(cfg, ec))
        {
            configs.push_back(cfg);
            if (ecFileIsRoot(cfg))
                break; // stop the upward search
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    if (configs.empty())
        return;

    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    };
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };

    // Resolve properties: process farthest -> nearest so nearer files override.
    std::unordered_map<std::string, std::string> props;
    for (auto it = configs.rbegin(); it != configs.rend(); ++it)
    {
        std::ifstream f(*it);
        std::string line;
        bool sawSection = false, sectionMatches = false;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::string s = trim(line);
            if (s.empty() || s[0] == '#' || s[0] == ';')
                continue;
            if (s[0] == '[')
            {
                auto rb = s.find(']');
                if (rb == std::string::npos)
                    continue;
                sawSection = true;
                sectionMatches = ecGlobMatch(s.substr(1, rb - 1), fname);
                continue;
            }
            auto eq = s.find('=');
            if (eq == std::string::npos)
                continue;
            if (sawSection && sectionMatches)
                props[lower(trim(s.substr(0, eq)))] = trim(s.substr(eq + 1));
        }
    }

    auto &e = t.editor;
    int tabWidth = -1;
    if (auto p = props.find("tab_width"); p != props.end())
        tabWidth = std::atoi(p->second.c_str());
    if (auto p = props.find("indent_style"); p != props.end())
        e.SetInsertSpacesOnTabs(lower(p->second) == "space");
    if (auto p = props.find("indent_size"); p != props.end())
    {
        if (lower(p->second) == "tab")
        {
            if (tabWidth > 0 && tabWidth <= 32)
                e.SetTabSize(tabWidth);
        }
        else
        {
            int n = std::atoi(p->second.c_str());
            if (n > 0 && n <= 32)
                e.SetTabSize(n);
        }
    }
    else if (tabWidth > 0 && tabWidth <= 32)
    {
        e.SetTabSize(tabWidth);
    }
}

void Editor::openFile()
{
    showFileOpen();
}

void Editor::openFile(const std::string &path)
{
    // Dispatch by file kind: images go to the image viewer; executables run;
    // other binary blobs open in the OS default handler. Only text-ish files
    // fall through to the editor below.
    {
        auto ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (isImageExt(ext))
        {
            openImageFile(path);
            return;
        }
        // Let a plugin claim a binary type before we hand it to the OS — the pdfview
        // plugin opens .pdf in a dockable in-app window, the Unreal plugin turns a
        // .uasset/.umap into an inspection report opened in a tab — instead of
        // launching the external default app. (.pdf is in isBinaryExt, so it takes
        // this path.)
        if ((ext == ".exe" || isBinaryExt(ext)) && pluginRegistry.openFile(*this, path))
            return;
        if (ext == ".exe")
        {
            navOpenExternally(path);
            return;
        }
        if (isBinaryExt(ext))
        {
            navOpenExternally(path);
            return;
        }
        // Unknown extension? Sniff the first 4 KB — if it contains NUL bytes
        // it's almost certainly binary. Hand off to the OS rather than
        // dumping garbage into the text editor.
        {
            std::ifstream f(path, std::ios::binary);
            if (f.is_open())
            {
                char probe[4096];
                f.read(probe, sizeof(probe));
                std::streamsize n = f.gcount();
                for (std::streamsize i = 0; i < n; ++i)
                {
                    if (probe[i] == '\0')
                    {
                        // Binary-by-content (e.g. .uasset, which isn't in isBinaryExt):
                        // offer it to a plugin before the OS opener.
                        if (pluginRegistry.openFile(*this, path))
                            return;
                        navOpenExternally(path);
                        return;
                    }
                }
            }
        }
    }
    // If this file is already open in some tab, just bring it forward instead
    // of opening a second copy. Compare canonical paths so different ways of
    // spelling the same path (relative vs absolute, mixed separators, etc.)
    // resolve to the same tab.
    {
        std::error_code ec;
        auto target = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
        if (ec)
            target = std::filesystem::path(path);
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            auto &tab = *tabs[i];
            if (tab.filename == "untitled")
                continue;
            auto existing = std::filesystem::weakly_canonical(std::filesystem::path(tab.filename), ec);
            if (ec)
                existing = std::filesystem::path(tab.filename);
            if (existing == target)
            {
                activeTab = i;
                tab.wantFocus = true;
                return;
            }
        }
    }
    try
    {
        std::ifstream stream(path.c_str());
        if (!stream)
            throw std::runtime_error("Cannot open file: " + path);
        std::string text;
        stream.seekg(0, std::ios::end);
        auto endPos = stream.tellg();
        stream.seekg(0, std::ios::beg);
        // tellg() returns -1 on a failed/odd stream; reserve((size_t)-1) throws
        // std::length_error. Only pre-reserve on a valid, positive size.
        if (endPos > 0)
            text.reserve(static_cast<size_t>(endPos));
        text.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        stream.close();

        // reuse current tab if it's an empty untitled document
        bool reuse = !tabs.empty() &&
                     doc().filename == "untitled" &&
                     doc().editor.IsEmpty() &&
                     !isDirty();
        TabDocument *target = nullptr;
        if (reuse)
        {
            target = &doc();
        }
        else
        {
            target = &newTab();
        }

        // Large-file mode: whole-document intelligence (trie build, LSP sync,
        // folding, bracket matching) all walk the full text — on a many-MB file
        // that's seconds of stall on open and per-edit. Keep such files fast,
        // plain editors instead. (Gates BEFORE SetText so the fold scan is skipped.)
        updateLargeFileMode(*target, text.size());

        target->originalText = text;
        target->syncedText = text; // 3-way merge base = last reconciled content
        // Language FIRST: an async load colorizes on the worker, so it needs to know
        // the language before it starts.
        target->editor.SetLanguage(languageForPath(path));

        // Building glyphs + colorizing is O(bytes) and stalls the frame for seconds
        // on a multi-MB file. Past the threshold, do it on a worker: the tab opens
        // immediately (empty + read-only) and the finished document swaps in.
        if (text.size() >= kAsyncLoadBytes)
        {
            target->editor.SetTextAsync(text);
            target->pendingLoad = true; // trie/LSP wait for the worker (doc is empty until then)
        }
        else
        {
            target->editor.SetText(text);
        }
        target->version = target->editor.GetUndoIndex();
        target->filename = path;
        target->wantFocus = true;
        recordDiskMtime(*target);   // baseline for external-change watch
        applyEditorConfig(*target); // .editorconfig overrides indent/tab for this file
        rememberRecentFile(path);
        // Make this tab the active one so SetNextWindowFocus in
        // renderDockedDocuments actually points at this doc.
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            if (tabs[i].get() == target)
            {
                activeTab = i;
                break;
            }
        }

        // On an async load the document is still empty here — the identifier walk
        // would produce a trie with only the language's own keywords in it. Defer
        // it (and everything else that reads the document) to finishPendingLoads().
        if (!target->pendingLoad)
        {
            buildAutocompleteTrie(*target);
            reanchorNotesFor(*target);   // follow notes whose lines moved on disk
            refreshMarkers(*target);     // notes + any existing breakpoints
        }
    }
    catch (std::exception &e)
    {
        showError(e.what());
    }
}

//
//  Editor::finishPendingLoads
//
//  A tab opened with SetTextAsync has an empty document until its worker lands.
//  Once the editor swaps the finished document in (it polls in Render), do the
//  whole-document work that was deferred at open time.
//

void Editor::finishPendingLoads()
{
    for (auto &tab : tabs)
    {
        if (!tab->pendingLoad || tab->editor.IsLoading())
            continue;
        tab->pendingLoad = false;
        tab->version = tab->editor.GetUndoIndex(); // loaded content is the clean baseline
        buildAutocompleteTrie(*tab);
        reanchorNotesFor(*tab);
        refreshMarkers(*tab);
    }

    if (notesDirty)
        saveNotes();
}

//
//  Editor::saveFile
//

void Editor::saveFile()
{
    try
    {
        auto &t = doc();
        t.editor.StripTrailingWhitespaces();
        std::string out = t.editor.GetText(); // serialize ONCE (was twice — slow on huge files)
        std::ofstream stream(t.filename.c_str());
        stream << out;
        stream.close();
        t.version = t.editor.GetUndoIndex();
        t.syncedText = std::move(out); // persisted content = new merge base
        recordDiskMtime(t);                // our own write — re-baseline so the watch ignores it
        // Refresh the project symbol index so go-to-def / autocomplete pick up
        // edits. Cheap: the build is one-at-a-time guarded + gen-superseded.
        if (!projectRoot.empty())
            rebuildProjectIndex();
    }
    catch (std::exception &e)
    {
        showError(e.what());
    }
}

void Editor::saveFile(std::string &path)
{
    // save active document under a new path; auto-detect language if unset
    try
    {
        auto &t = doc();
        t.editor.StripTrailingWhitespaces();
        std::ofstream stream(path.c_str());
        stream << t.editor.GetText();
        stream.close();
        t.filename = path;
        if (t.editor.GetLanguage() == nullptr)
            t.editor.SetLanguage(languageForPath(path));
        t.version = t.editor.GetUndoIndex();
        t.syncedText = t.editor.GetText(); // persisted content = new merge base
        recordDiskMtime(t);                // our own write — re-baseline so the watch ignores it
        if (!projectRoot.empty())
            rebuildProjectIndex();
    }
    catch (std::exception &e)
    {
        showError(e.what());
    }
}

//
//  External-change watch — safe co-editing with Claude / other tools
//

//  Re-baseline the on-disk write time we've reconciled with. Called after our
//  own load/save so the watch never mistakes our write for someone else's.
void Editor::recordDiskMtime(TabDocument &t)
{
    if (t.filename == "untitled")
        return;
    std::error_code ec;
    auto mt = std::filesystem::last_write_time(t.filename, ec);
    if (!ec)
        t.diskMtime = mt;
}

//  Re-read the file from disk into a clean buffer, preserving the cursor —
//  CLAMPED into the reloaded document because an external edit may have shrunk
//  the file (a stale out-of-range cursor is the classic assert source here).
// Source code vs everything else (logs, text, data/markup, binaries). Drives the
// "quiet reload" for non-code files: they update in place without toasts or gutter
// change markers, so an appending .log doesn't spam.
bool Editor::isCodeExtension(const std::string &extLower)
{
    static const std::unordered_set<std::string> code = {
        ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hxx", ".hh", ".inl", ".ipp", ".m", ".mm",
        ".cs", ".java", ".kt", ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs",
        ".py", ".pyw", ".lua", ".go", ".rs", ".as",
        ".glsl", ".vert", ".frag", ".geom", ".comp", ".tesc", ".tese",
        ".hlsl", ".hlsli", ".fx", ".fxh", ".addonfx", ".sql"
    };
    return code.count(extLower) != 0;
}

void Editor::reloadFromDisk(TabDocument &t, bool quiet)
{
    std::ifstream stream(t.filename.c_str());
    if (!stream.is_open())
        return;
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();

    std::string oldText = t.editor.GetText(); // pre-reload buffer, for change markers

    int line = 0, column = 0;
    t.editor.GetCurrentCursor(line, column);

    updateLargeFileMode(t, text.size()); // external edit may cross the 8MB threshold
    t.editor.SetText(text);
    t.originalText = text;
    t.syncedText = text; // reconciled with disk
    t.version = t.editor.GetUndoIndex();
    t.externalChange = false;
    recordDiskMtime(t);
    rememberRecentFile(t.filename);
    if (quiet)
        clearChangeMarks(t); // non-code (log/data): update silently, no gutter markers
    else
        markChangedLines(t, oldText); // gutter-highlight exactly what changed (widget diffs vs current text)

    int total = (std::max)(t.editor.GetLineCount(), 1);
    line = (std::max)(0, (std::min)(line, total - 1));
    int len = static_cast<int>(t.editor.GetLineText(line).size());
    column = (std::max)(0, (std::min)(column, len));
    t.editor.SetCursor(line, column);
    t.editor.ScrollToLine(line, TextEditor::Scroll::alignMiddle);

    if (!projectRoot.empty())
        rebuildProjectIndex();
}

//  Poll open files' mtime (throttled). Real change: clean buffer reloads, dirty
//  buffer flags a conflict. Content backstop ignores benign/own touches.
void Editor::checkExternalChanges()
{
    double now = ImGui::GetTime();
    if (now - extWatchTime < 1.0)
        return;
    extWatchTime = now;

    for (size_t i = 0; i < tabs.size(); ++i)
    {
        auto &t = *tabs[i];
        if (t.filename == "untitled")
            continue;
        if (t.externalChange)
            continue; // already flagged, awaiting the user

        std::error_code ec;
        auto mt = std::filesystem::last_write_time(t.filename, ec);
        if (ec)
            continue; // deleted / locked → keep the buffer
        if (mt == t.diskMtime)
            continue; // unchanged since we last reconciled

        // Content backstop: a mtime bump with identical bytes (our own save,
        // a `touch`, a no-op formatter) is not a real change — just re-baseline.
        std::ifstream f(t.filename.c_str());
        std::string disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        disk.erase(std::remove(disk.begin(), disk.end(), '\r'), disk.end());
        std::string mine = t.editor.GetText();
        mine.erase(std::remove(mine.begin(), mine.end(), '\r'), mine.end());
        if (disk == mine)
        {
            t.diskMtime = mt;
            continue;
        }

        std::string fname = std::filesystem::path(t.filename).filename().string();
        // Log / data / text files (e.g. a live crash log that keeps appending) update
        // the view silently: no toast, no tab badge, no gutter markers, no activity
        // feed — only code files announce external edits.
        std::string cext = std::filesystem::path(t.filename).extension().string();
        std::transform(cext.begin(), cext.end(), cext.begin(),
                       [](unsigned char c) { return (char) std::tolower(c); });
        bool code = isCodeExtension(cext);

        t.externallyTouched = code; // badge only for code files
        if (!isDirtyTab(i))
        {
            reloadFromDisk(t, /*quiet*/ !code); // clean → take their version
            if (code)
            {
                pushToast("\xe2\x9c\x8e External edit:  " + fname + "  \xe2\x80\x94 reloaded",
                          IM_COL32(170, 130, 250, 255));
                logExternalChange(t.filename, "reloaded (clean)");
            }
        }
        else
        {
            t.externalChange = true; // dirty → conflict, show the bar
            if (code)
            {
                pushToast("\xe2\x9a\xa0 External edit:  " + fname + "  \xe2\x80\x94 conflict (you have unsaved edits)",
                          IM_COL32(240, 180, 70, 255));
                logExternalChange(t.filename, "conflict (unsaved)");
            }
        }
    }
}

//  Gutter-highlight the lines a reload changed. Prefix/suffix trim keeps the LCS
//  bounded so a localized edit in a huge file is cheap.
// Drop every "external/Claude change" decoration from a doc — gutter markers, the
// clickable reply-dot decorator, and the tracked ranges — together, so they never
// drift out of sync (a partial clear left stale clickable dots after the user
// started editing).
void Editor::clearChangeMarks(TabDocument &t)
{
    t.editor.ClearLineDecorator();
    t.changedRanges.clear();
    t.externalMarkers = false;
    // Rebuild the shared marker list WITHOUT the change layer — sticky notes and
    // debug breakpoints must survive an external-change clear (they used to be
    // blanket-wiped here and only reappear when their own event re-fired).
    refreshMarkers(t);
}

// Re-add the external-change gutter markers from the tracked ranges (same
// colors/tooltips the widget's MarkChangedLines uses) — the composer's lowest
// layer, so notes and debug markers win a shared line.
void Editor::addChangeMarkers(TabDocument &t)
{
    if (t.changedRanges.empty())
        return;
    const ImU32 gutter = IM_COL32(170, 130, 250, 255);
    const ImU32 bg = IM_COL32(120, 90, 200, 38);
    int lineCount = t.editor.GetLineCount();
    for (auto &r : t.changedRanges)
        for (int ln = (std::max)(0, r.first); ln <= r.second && ln < lineCount; ++ln)
            t.editor.AddMarker(ln, gutter, bg, "Changed externally",
                               "Changed on disk by an external tool");
}

// THE marker composer. The widget has ONE marker slot per line and only a
// whole-list clear, and three features share it (external-change, sticky notes,
// debug breakpoints/stop-line) — each used to clear+redraw independently and
// clobber the others. Every rebuild now goes through here: one clear, then the
// layers lowest-priority-first (a later AddMarker on the same line wins the
// slot), so all three stay live no matter which feature triggered the refresh.
void Editor::refreshMarkers(TabDocument &t)
{
    t.editor.ClearMarkers();
    addChangeMarkers(t);   // lowest: broad "this file changed" wash
    applyNoteMarkers(t);   // middle: sticky-note diamonds
    applyDebugMarkers(t);  // highest: breakpoints / stopped-here must stay visible
}

void Editor::markChangedLines(TabDocument &t, const std::string &oldText)
{
    // The diff + gutter markers now live in the widget (TextEditor::MarkChangedLines,
    // which is headless-testable); it diffs oldText against its own current text. We
    // keep the app-side pieces here: the tracked ranges (Dev Tools list / gutter
    // hit-test) and the clickable reply-dot decorator.
    t.editor.ClearLineDecorator();
    t.changedRanges = t.editor.MarkChangedLines(oldText); // clears + adds change markers
    t.externalMarkers = !t.changedRanges.empty();
    // The widget's clear wiped the other layers — put notes + debug back on top.
    applyNoteMarkers(t);
    applyDebugMarkers(t);
    if (t.changedRanges.empty())
        return;
    t.editor.SetLineDecorator(-1.6f, [this](TextEditor::Decorator &d) {
        TabDocument *td = nullptr;
        for (auto &up : tabs)
            if (&up->editor == d.editor)
            {
                td = up.get();
                break;
            }
        if (!td || !lineIsChanged(*td, d.line))
            return;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##claudeReplyDot", ImVec2(d.width, d.glyphSize.y));
        bool hov = ImGui::IsItemHovered();
        if (hov)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Reply to Claude about line %d", d.line + 1);
        }
        ImU32 col = hov ? IM_COL32(205, 165, 255, 255) : IM_COL32(170, 130, 250, 210);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(pos.x + d.width * 0.5f, pos.y + d.glyphSize.y * 0.5f), d.glyphSize.y * 0.20f, col);
        if (ImGui::IsItemClicked())
            requestReply(td->filename, d.line, td->filename + ":" + std::to_string(d.line + 1));
    });
}

bool Editor::lineIsChanged(const TabDocument &t, int line) const
{
    for (auto &r : t.changedRanges)
        if (line >= r.first && line <= r.second)
            return true;
    return false;
}

// Open the "Reply to Claude" popup about `file`:`line0` (line0 < 0 / file empty =
// general or toast feedback). The modal opens next frame via renderReplyPopup().
void Editor::requestReply(const std::string &file, int line0, const std::string &contextLabel)
{
    replyTargetFile = file;
    replyTargetLine = line0;
    replyContextLabel = contextLabel.empty() ? std::string("General feedback to Claude") : contextLabel;
    replyBuf[0] = '\0';
    replyPopupRequested = true;
}

// Turn a typed message into an outbox entry. immediate == "Send now", else queue it
// for batch submission (Submit batch flushes the queue as one file).
void Editor::submitReply(const char *message, bool immediate)
{
    if (!message || !*message)
        return;
    std::string prefix;
    if (!replyTargetFile.empty())
    {
        std::string rel = replyTargetFile;
        std::error_code ec;
        if (!projectRoot.empty())
        {
            auto r = std::filesystem::relative(replyTargetFile, projectRoot, ec);
            if (!ec && !r.empty())
                rel = r.generic_string();
        }
        else
            rel = std::filesystem::path(replyTargetFile).filename().string();
        prefix = "[" + rel + (replyTargetLine >= 0 ? ":" + std::to_string(replyTargetLine + 1) : "") + "] ";
    }
    std::string body = prefix + message;
    if (immediate)
    {
        writeToastReply(body);
        pushToast("Reply sent to Claude", IM_COL32(120, 200, 120, 255), 2);
    }
    else
    {
        replyBatch.push_back(body);
        pushToast("Comment queued (" + std::to_string(replyBatch.size()) + ")", IM_COL32(170, 130, 250, 255), 2);
    }
}

// Flush all queued comments as a single reply file (one "- " bullet per comment).
void Editor::flushReplyBatch()
{
    if (replyBatch.empty())
        return;
    std::string all = "Batched feedback from ImGui-IDE:\n";
    for (auto &c : replyBatch)
        all += "- " + c + "\n";
    writeToastReply(all);
    pushToast("Batch sent: " + std::to_string(replyBatch.size()) + " comment(s)", IM_COL32(120, 200, 120, 255), 2);
    replyBatch.clear();
}

void Editor::renderReplyPopup()
{
    if (replyPopupRequested)
    {
        ImGui::OpenPopup("Reply to Claude###claudeReply");
        replyPopupRequested = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Reply to Claude###claudeReply", nullptr, ImGuiWindowFlags_NoMove))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(190, 155, 255, 255));
        ImGui::TextWrapped("%s", replyContextLabel.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##claudeMsg", replyBuf, sizeof(replyBuf), ImVec2(-FLT_MIN, 120.0f));

        bool has = replyBuf[0] != '\0';
        if (!has)
            ImGui::BeginDisabled();
        if (ImGui::Button("Send now"))
        {
            submitReply(replyBuf, true);
            replyBuf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add comment")) // queue for batch, then back to the editor to mark the next change
        {
            submitReply(replyBuf, false);
            replyBuf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        if (!has)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            replyBuf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        if (!replyBatch.empty())
        {
            ImGui::Separator();
            ImGui::Text("%d comment%s queued for batch", (int)replyBatch.size(),
                        replyBatch.size() == 1 ? "" : "s");
            if (ImGui::Button("Submit batch"))
            {
                flushReplyBatch();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard batch"))
                replyBatch.clear();
        }
        ImGui::EndPopup();
    }
}

// The 3-way merge (diff3) moved into the widget — see TextEditor::Merge3, which is
// pure logic and unit-tested headless. mergeExternalChange() below calls it.

//  Merge the on-disk (external) version into the dirty buffer over the last
//  reconciled base. Non-overlapping changes auto-merge; overlaps get git-style
//  <<<<<<< / ======= / >>>>>>> markers. Result is left DIRTY so the user saves.
void Editor::mergeExternalChange(TabDocument &t)
{
    std::ifstream f(t.filename.c_str());
    if (!f.is_open())
        return;
    std::string theirs((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    std::string mine = t.editor.GetText();
    bool conflict = false;
    std::string merged = TextEditor::Merge3(t.syncedText, mine, theirs, conflict);

    int line = 0, column = 0;
    t.editor.GetCurrentCursor(line, column);

    updateLargeFileMode(t, merged.size()); // merged content may cross the 8MB threshold
    t.editor.SetText(merged);
    t.externalChange = false;
    clearChangeMarks(t);
    recordDiskMtime(t);                      // baseline to disk so the watch won't re-fire
    t.version = t.editor.GetUndoIndex() + 1; // merged != disk → force dirty so the user saves

    int total = (std::max)(t.editor.GetLineCount(), 1);
    line = (std::max)(0, (std::min)(line, total - 1));
    int len = static_cast<int>(t.editor.GetLineText(line).size());
    column = (std::max)(0, (std::min)(column, len));
    t.editor.SetCursor(line, column);
    t.editor.ScrollToLine(line, TextEditor::Scroll::alignMiddle);

    std::string fname = std::filesystem::path(t.filename).filename().string();
    if (conflict)
    {
        pushToast("\xe2\x9a\xa0 Merged " + fname + " with conflicts \xe2\x80\x94 resolve <<<<<<< markers, then save",
                  IM_COL32(240, 180, 70, 255));
        logExternalChange(t.filename, "merged (conflicts)");
    }
    else
    {
        pushToast("\xe2\x9c\x93 Merged " + fname + " cleanly \xe2\x80\x94 review and save",
                  IM_COL32(120, 200, 120, 255));
        logExternalChange(t.filename, "merged");
    }
}

//  Queue a transient corner notification.
void Editor::pushToast(const std::string &text, ImU32 accent, int action)
{
    toasts.push_back({text, ImGui::GetTime() + 5.0, accent, action});
    if (toasts.size() > 6)
        toasts.erase(toasts.begin());
}

// Clicking a (non-update) toast drops its text into <configDir>/replies/ — the
// outbox half of the feedback bridge. A Claude Code watcher / the imgui-ide-bridge
// plugin can tail this folder to receive in-editor replies.
void Editor::writeToastReply(const std::string &text)
{
    std::error_code ec;
    auto dir = userConfigDir() / "replies";
    std::filesystem::create_directories(dir, ec);
    static int seq = 0;
    char name[64];
    std::snprintf(name, sizeof(name), "reply_%lld_%d.txt", (long long)std::time(nullptr), seq++);
    std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
    if (f)
        f << text;
}

// External toast API. Any process can show a toast in the running editor by
// writing a small text file into <configDir>/toasts/. File contents:
//   optional "<severity>|<message>"  (severity = info|warn|error|success|ok),
//   or just the raw message. The file is deleted once shown. Polled at ~5 Hz so
// it costs nothing per frame; this is the bridge other tools (e.g. a Claude Code
// plugin) use to surface notifications in the IDE.
// Single-instance open inbox — a second launch (single-instance forwarding in
// main.cpp) drops "<verb>|<arg>" lines here; we open them in THIS window.
void Editor::pollOpenInbox()
{
    static double nextPoll = 0.0;
    double now = ImGui::GetTime();
    if (now < nextPoll)
        return;
    nextPoll = now + 0.2;

    std::error_code ec;
    // Per-project inbox: only requests routed to THIS instance's project.
    auto dir = userConfigDir() / "open" / instanceKey;
    if (!std::filesystem::exists(dir, ec))
        return;

    std::vector<std::filesystem::path> files;
    for (auto &e : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (e.is_regular_file(ec))
            files.push_back(e.path());
        if (files.size() >= 32)
            break;
    }
    std::sort(files.begin(), files.end());
    for (auto &f : files)
    {
        std::ifstream in(f, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::filesystem::remove(f, ec);

        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            auto bar = line.find('|');
            if (bar == std::string::npos)
                continue;
            std::string verb = line.substr(0, bar), arg = line.substr(bar + 1);
            if (verb == "project" && !arg.empty())
                setProjectRoot(arg);
            else if (verb == "file" && !arg.empty())
                openFile(arg);
            else if (verb == "raise")
                wantRaiseWindow = true;
        }
    }
}

void Editor::pollToastInbox()
{
    static double nextPoll = 0.0;
    double now = ImGui::GetTime();
    if (now < nextPoll)
        return;
    nextPoll = now + 0.2;

    std::error_code ec;
    auto dir = userConfigDir() / "toasts";
    if (!std::filesystem::exists(dir, ec))
        return;

    std::vector<std::filesystem::path> files;
    for (auto &e : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (e.is_regular_file(ec))
            files.push_back(e.path());
        if (files.size() >= 64)
            break; // flood guard
    }
    std::sort(files.begin(), files.end()); // timestamped names → in-order display

    int shown = 0;
    for (auto &f : files)
    {
        if (shown++ >= 16)
            break; // cap per poll
        std::ifstream in(f, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::filesystem::remove(f, ec); // consume regardless of parse outcome
        while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                                    content.back() == ' ' || content.back() == '\t'))
            content.pop_back();
        if (content.empty())
            continue;

        ImU32 color = IM_COL32(150, 160, 255, 255); // info (default, blue-violet)
        std::string msg = content;
        size_t sep = content.find_first_of("|\t");
        if (sep != std::string::npos && sep <= 8) // short leading severity tag only
        {
            std::string sev = content.substr(0, sep);
            std::transform(sev.begin(), sev.end(), sev.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            bool known = true;
            if (sev == "warn" || sev == "warning")
                color = IM_COL32(240, 200, 90, 255);
            else if (sev == "error" || sev == "err")
                color = IM_COL32(224, 96, 96, 255);
            else if (sev == "success" || sev == "ok")
                color = IM_COL32(120, 200, 120, 255);
            else if (sev == "info")
                color = IM_COL32(150, 160, 255, 255);
            else
                known = false;
            if (known)
                msg = content.substr(sep + 1);
        }
        if (!msg.empty())
            pushToast(msg, color);
    }
}

// ── In-app updater ───────────────────────────────────────────────────────────
// Point these at the repo that hosts the published releases + installer assets.
static const char *kUpdateOwner = "lobru";
static const char *kUpdateRepo = "ImGui-IDE";

void Editor::checkForUpdates(bool manual)
{
    if (updateFuture.valid())
        return; // a check is already in flight
    updateCheckManual = manual;
    lastUpdateCheckEpoch = (long long)std::time(nullptr);
    std::string owner = kUpdateOwner, repo = kUpdateRepo;
    bool nightly = (prefUpdateChannel == 1);
    updateFuture = std::async(std::launch::async,
                              [owner, repo, nightly] { return updater::fetchLatest(owner, repo, nightly); });
}

void Editor::pollUpdates()
{
    // Background 12 h check (skipped while one runs or an update is already known).
    if (prefAutoUpdate && !updateFuture.valid() && !updateAvailable)
    {
        long long now = (long long)std::time(nullptr);
        if (lastUpdateCheckEpoch == 0 || now - lastUpdateCheckEpoch > 12 * 3600)
            checkForUpdates(false);
    }

    // Drain a finished check.
    if (updateFuture.valid() &&
        updateFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        updateInfo = updateFuture.get();
        bool newer = updateInfo.ok && !updateInfo.notFound &&
                     updater::isNewer(updateInfo.tag, kAppVersion);
        updateAvailable = newer;
        if (newer)
        {
            pushToast("\xe2\x86\x91 Update available: " + updateInfo.tag + " (click)", IM_COL32(120, 200, 120, 255), 1);
            showUpdateDialog = true;
        }
        else if (updateCheckManual)
        {
            if (!updateInfo.ok)
                pushToast("Update check failed: " + updateInfo.error, IM_COL32(224, 96, 96, 255));
            else
                pushToast("ImGui-IDE is up to date (" + std::string(kAppVersion) + ")",
                          IM_COL32(150, 160, 255, 255));
        }
        updateCheckManual = false;
    }

    // Drain a finished download → launch the installer.
    if (updateDownloadFuture.valid() &&
        updateDownloadFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        bool ok = updateDownloadFuture.get();
        if (ok)
        {
            // No installer — overwrite the exe in place (running one renamed aside).
            std::string err;
            if (updater::applyUpdate(updateDownloadPath, updater::runningExePath(), err))
            {
                updateDownloadState = 2;
                pushToast("Updated to " + updateInfo.tag + " \xe2\x80\x94 restart to apply",
                          IM_COL32(120, 200, 120, 255));
            }
            else
            {
                updateDownloadState = 3;
                pushToast("Update failed: " + err, IM_COL32(224, 96, 96, 255));
            }
        }
        else
        {
            updateDownloadState = 3;
            pushToast("Download failed", IM_COL32(224, 96, 96, 255));
        }
    }
}

void Editor::renderUpdateDialog()
{
    if (showUpdateDialog)
    {
        ImGui::OpenPopup("Update available##upd");
        showUpdateDialog = false;
    }
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Update available##upd", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("ImGui-IDE %s is available.", updateInfo.tag.c_str());
        ImGui::TextDisabled("You have %s.", kAppVersion);
        ImGui::Separator();
        if (!updateInfo.notes.empty())
        {
            ImGui::TextDisabled("Release notes:");
            ImGui::BeginChild("##notes", ImVec2(440.0f, 160.0f), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", updateInfo.notes.c_str());
            ImGui::EndChild();
        }
        if (updateDownloadState == 1)
            ImGui::TextDisabled("Downloading %s\xe2\x80\xa6", updateInfo.assetName.c_str());
        else if (updateDownloadState == 2)
            ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "Updated \xe2\x80\x94 restart ImGui-IDE to apply.");
        else if (updateDownloadState == 3)
            ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.45f, 1.0f), "Update failed \xe2\x80\x94 try GitHub.");
        ImGui::Spacing();

        bool busy = (updateDownloadState == 1);
        ImGui::BeginDisabled(busy);
        if (!updateInfo.assetUrl.empty())
        {
            if (ImGui::Button("Download & Update", ImVec2(160.0f, 0.0f)))
            {
                std::string name = updateInfo.assetName.empty() ? "ImGui-IDE-update.exe" : updateInfo.assetName;
                updateDownloadPath = (userConfigDir() / name).string();
                std::string url = updateInfo.assetUrl, path = updateDownloadPath;
                updateDownloadFuture = std::async(std::launch::async,
                                                  [url, path] { return updater::download(url, path); });
                updateDownloadState = 1;
            }
            ImGui::SameLine();
        }
        if (updateDownloadState == 2)
        {
            if (ImGui::Button("Restart Now", ImVec2(120.0f, 0.0f)))
            {
                saveDirtyTitledDocs(); // don't lose on-disk edits on the silent exit
                saveSettings();
                updater::relaunch(updater::runningExePath());
                done = true; // exit so the freshly-swapped exe takes over
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("View on GitHub", ImVec2(140.0f, 0.0f)))
            updater::openUrl(updateInfo.htmlUrl.empty()
                                 ? ("https://github.com/" + std::string(kUpdateOwner) + "/" + kUpdateRepo + "/releases")
                                 : updateInfo.htmlUrl);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(80.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void Editor::toggleFocusMode()
{
    setFocusMode(!focusMode);
}

void Editor::setFocusMode(bool on)
{
    if (on == focusMode)
        return;
    if (on)
    {
        // Snapshot the current panel layout, then hide everything but the docs.
        focusSnap.nav = navPanelVisible;
        focusSnap.sym = symbolsPanelVisible;
        focusSnap.refs = referencesVisible;
        focusSnap.fif = findInFilesVisible;
        focusSnap.output = script ? script->visible : false;
        focusSnap.dev = devToolsVisible;
        focusSnap.ext = externalChangesVisible;
        focusSnap.md = mdPreviewVisible;
        navPanelVisible = symbolsPanelVisible = referencesVisible = findInFilesVisible = false;
        devToolsVisible = externalChangesVisible = mdPreviewVisible = false;
        if (script)
            script->visible = false;
        for (auto &up : tabs)
            up->editor.SetShowScrollbarMiniMapEnabled(false);
        focusMode = true;
    }
    else
    {
        navPanelVisible = focusSnap.nav;
        symbolsPanelVisible = focusSnap.sym;
        referencesVisible = focusSnap.refs;
        findInFilesVisible = focusSnap.fif;
        devToolsVisible = focusSnap.dev;
        externalChangesVisible = focusSnap.ext;
        mdPreviewVisible = focusSnap.md;
        if (script)
            script->visible = focusSnap.output;
        for (auto &up : tabs)
            up->editor.SetShowScrollbarMiniMapEnabled(true);
        focusMode = false;
    }
}

//  Draw queued toasts stacked at the top-right of the work area, fading out in
//  their final second. NoInputs so they never eat clicks.
void Editor::renderToasts()
{
    if (toasts.empty())
        return;
    double now = ImGui::GetTime();
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
                                [now](const Toast &t) { return now >= t.expiry; }),
                 toasts.end());
    if (toasts.empty())
        return;

    ImGuiViewport *vp = ImGui::GetMainViewport();
    const float pad = 14.0f;
    float y = vp->WorkPos.y + pad;

    // No NoInputs — toasts are clickable (act on / dismiss). They stay top-right,
    // small, and non-focus-stealing, so they don't get in the way.
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;

    int clickedAction = -1; // dispatch AFTER the loop (avoids mutating toasts mid-iterate)
    std::string clickedText;
    for (size_t i = 0; i < toasts.size(); ++i)
    {
        auto &to = toasts[i];
        float remain = static_cast<float>(to.expiry - now);
        float alpha = remain < 1.0f ? remain : 1.0f; // fade the last second
        ImU32 accent = (to.accent & 0x00FFFFFFu) | (static_cast<ImU32>(alpha * 255.0f) << 24);

        char id[32];
        std::snprintf(id, sizeof(id), "##toast%zu", i);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowBgAlpha(0.93f * alpha);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, y),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, accent);
        if (ImGui::Begin(id, nullptr, flags))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            ImGui::TextUnformatted(to.text.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsWindowHovered())
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    clickedAction = to.action;
                    clickedText = to.text;
                    to.expiry = now; // dismiss this toast next frame
                }
            }
            y += ImGui::GetWindowHeight() + 8.0f;
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    if (clickedAction == 1)
        showUpdateDialog = true; // update toast → open the updater
    else if (clickedAction == 0)
        requestReply("", -1, "Re: " + clickedText); // open the type-to-Claude popup (instant reply)
    // clickedAction == 2 (system confirmations): click only dismisses, no re-reply.
}

//  Append an external-change event to the persistent activity feed.
void Editor::logExternalChange(const std::string &path, const std::string &kind)
{
    ExtChange e;
    e.file = std::filesystem::path(path).filename().string();
    e.path = path;
    e.kind = kind;
    e.time = ImGui::GetTime();
    extChangeLog.push_back(e);
    if (extChangeLog.size() > 200)
        extChangeLog.erase(extChangeLog.begin());
}

//  Dockable, persistent log of Claude / external edits to open files — toasts
//  fade after 5s, this keeps the history. Click a file to (re)open it.
void Editor::renderExternalChanges()
{
    if (!externalChangesVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(440.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("External Changes###externalChanges", &externalChangesVisible))
    {
        ImGui::Text("%d external change%s", (int)extChangeLog.size(), extChangeLog.size() == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
            extChangeLog.clear();
        ImGui::TextDisabled("On-disk edits to open files (you, Claude, git, any tool). Click a row to open.");
        ImGui::Separator();

        if (extChangeLog.empty())
        {
            ImGui::TextDisabled("No external edits yet.");
            ImGui::TextWrapped("When an external tool (e.g. Claude) edits a file you have open, "
                               "it shows here — newest first. Click a row to open the file.");
        }
        else if (ImGui::BeginTable("##cactivity", 3,
                                   ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("File");
            ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            double now = ImGui::GetTime();
            for (int idx = (int)extChangeLog.size() - 1; idx >= 0; --idx) // newest first
            {
                auto &e = extChangeLog[idx];
                ImGui::TableNextRow();
                ImGui::PushID(idx);

                ImGui::TableNextColumn();
                int ago = (int)(now - e.time);
                char when[32];
                if (ago < 60)
                    std::snprintf(when, sizeof(when), "%ds ago", ago);
                else if (ago < 3600)
                    std::snprintf(when, sizeof(when), "%dm ago", ago / 60);
                else
                    std::snprintf(when, sizeof(when), "%dh ago", ago / 3600);
                ImGui::TextDisabled("%s", when);

                ImGui::TableNextColumn();
                if (ImGui::Selectable(e.file.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                    openFile(e.path);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", e.path.c_str());

                ImGui::TableNextColumn();
                ImU32 col = (e.kind.find("conflict") != std::string::npos) ? IM_COL32(240, 180, 70, 255)
                            : (e.kind.find("merged") != std::string::npos) ? IM_COL32(120, 200, 120, 255)
                                                                           : IM_COL32(170, 130, 250, 255);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", e.kind.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        middleMousePanScroll(11); // external changes
    }
    ImGui::End();
}

//
//  Editor::render — host viewport, dockspace, per-document windows
//

void Editor::render()
{
    auto perfT0 = std::chrono::steady_clock::now(); // frame-build cost (fps watchdog)

    // Publish a finished background decompile (opens the cached .cs read-only,
    // or falls back to the Learn page on failure). Cheap atomic check per frame.
    pollDecompile();
    pollLsp(); // drain clangd results → refine completion popup

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;

    ImGui::Begin("##EditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    renderMenuBar();

    // dockspace fills remaining area minus status bar
    auto &style = ImGui::GetStyle();
    float statusBarHeight = ImGui::GetFrameHeight() + 2.0f * style.WindowPadding.y;
    ImVec2 dockArea = ImGui::GetContentRegionAvail();
    dockArea.y -= statusBarHeight + style.ItemSpacing.y;

    ImGuiID dockId = ImGui::GetID("MainDockSpace");
    // Capture the root id computed in THIS window's ID stack so menu/keybind
    // handlers (which run in other ID-stack contexts) reuse it instead of
    // recomputing GetID — see mainDockId in editor.h.
    mainDockId = dockId;
    // One-shot default layout: Nav docked into a left split. Only runs if
    // no imgui.ini layout already exists for this dockspace.
    navInitDockLayout(dockId);
    // Deferred Reset Layout: rebuild here, in the host context and BEFORE the
    // DockSpace() submission below, so the new tree is the one DockSpace uses.
    if (wantResetLayout)
    {
        wantResetLayout = false;
        remergeAllWindows();
    }
    // Sticky single-doc layout: PassthruCentralNode lets the doc fill the
    // work area cleanly. (We don't auto-hide the tab bar — the user wants
    // it always visible to track open docs.)
    ImGui::DockSpace(dockId, dockArea, ImGuiDockNodeFlags_PassthruCentralNode);

    checkExternalChanges(); // reload clean docs / flag conflicts when disk changes under us
    pollOpenInbox();        // single-instance: <configDir>/open/* → open here + raise
    pollToastInbox();       // external toast API: <configDir>/toasts/* → on-screen toasts
    pollUpdates();          // GitHub release check (auto 12 h / manual) + download drain
    autoSaveTick();         // periodic save of dirty, on-disk documents (if enabled)
    pollCloneCompletion();  // rebuild the index once an in-progress git clone lands
    pollGithubBrowser();    // open a fetched remote file read-only (UI thread)

    renderDockedDocuments();
    finishPendingLoads();    // after Render: that's where an async doc load lands

    // A modal can't be opened from inside the text context menu (popup-in-popup),
    // so the menu item just records the line and we open it here.
    if (wantNotePopupLine >= 0 && !tabs.empty())
    {
        openNotePopup(doc(), wantNotePopupLine);
        wantNotePopupLine = -1;
    }
    renderNotePopup();
    renderNotesPanel();

    renderNavigationPanel();
    renderGitHistory();
    pollDap();          // drain debug-adapter results (stops, output, stack, vars)
    renderDebugPanel();
    renderCommandPalette();
    renderBuildPicker();
    renderTour();   // after the panels exist, so a step can anchor to a real window
    renderGithubBrowser();
    renderImageWindows();
    renderScriptOutputWindow();
    renderRunArgsPopup();
    renderReferencesPanel();
    renderSymbolsPanel();
    renderFindInFilesPanel();
    appShortcutsSuppressed = false;                 // a focused plugin window may re-raise it below
    pluginRegistry.frame(*this); // in-process plugins: dockable windows + polling
    renderDevTools();
    renderMarkdownPreview();
    renderGitDialogs();
    renderExternalChanges(); // persistent external-edit feed
    renderSettings();
    renderToasts(); // transient Claude-edited notifications, drawn over everything
    renderReplyPopup(); // "Reply to Claude" modal (gutter dot / Dev Tools / toast)

    // Diff-against-other file picker — overlay on any state.
    if (diffOtherMode)
    {
        auto dlg = ImGuiFileDialog::Instance();
        ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
        if (!vp)
            vp = ImGui::GetMainViewport();
        if (dialogNeedsPlacement)
        {
            ImGui::SetNextWindowViewport(vp->ID);
            ImVec2 sz(vp->Size.x * 0.6f, vp->Size.y * 0.6f);
            ImVec2 pos(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
                       vp->Pos.y + (vp->Size.y - sz.y) * 0.5f);
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
            dialogNeedsPlacement = false;
        }
        bool finished = dlg->Display("diff-other", ImGuiWindowFlags_NoCollapse,
                                     ImVec2(480.0f, 320.0f), vp->Size);
        if (finished)
        {
            if (dlg->IsOk() && !tabs.empty())
            {
                diffOtherPath = dlg->GetFilePathName();
                std::ifstream f(diffOtherPath);
                std::string contentB((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                auto &t = doc();
                t.diff.SetLanguage(t.editor.GetLanguage());
                t.diff.SetText(contentB, t.editor.GetText());
                state = State::diff;
            }
            diffOtherMode = false;
            dlg->Close();
        }
    }

    ImGui::Spacing();
    renderStatusBar();

    if (state == State::diff)
    {
        renderDiff();
    }
    else if (state == State::openFile)
    {
        renderFileOpen();
    }
    else if (state == State::saveFileAs)
    {
        renderSaveAs();
    }
    else if (state == State::confirmClose)
    {
        renderConfirmClose();
    }
    else if (state == State::confirmQuit)
    {
        renderConfirmQuit();
    }
    else if (state == State::confirmError)
    {
        renderConfirmError();
    }
    else if (state == State::gotoLine)
    {
        renderGotoLine();
    }
    else if (state == State::openProject)
    {
        // Re-uses the existing file-open dialog mechanism but on the
        // "project-open" key, opened by openProjectFolderPicker().
        ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
        if (!vp)
            vp = ImGui::GetMainViewport();
        auto dialog = ImGuiFileDialog::Instance();
        if (dialogNeedsPlacement)
        {
            ImGui::SetNextWindowViewport(vp->ID);
            ImVec2 sz(vp->Size.x * 0.7f, vp->Size.y * 0.7f);
            ImVec2 pos(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
                       vp->Pos.y + (vp->Size.y - sz.y) * 0.5f);
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
            dialogNeedsPlacement = false;
        }
        bool finished = dialog->Display("project-open", ImGuiWindowFlags_NoCollapse,
                                        ImVec2(480.0f, 320.0f), vp->Size);
        saveFileDialogPlaces();
        if (finished)
        {
            if (dialog->IsOk())
            {
                // If the user picked a *file* (e.g. project.sln, foo.csproj),
                // use its containing directory as the project root. Otherwise
                // use the current dialog directory.
                std::string picked = dialog->GetFilePathName();
                std::error_code ec;
                std::filesystem::path target;
                if (!picked.empty() && std::filesystem::is_regular_file(picked, ec))
                {
                    target = std::filesystem::path(picked).parent_path();
                }
                else
                {
                    target = dialog->GetCurrentPath();
                }
                setProjectRoot(target);
            }
            state = State::edit;
            dialog->Close();
        }
    }
    else if (state == State::addSourceLoc)
    {
        ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
        if (!vp)
            vp = ImGui::GetMainViewport();
        auto dialog = ImGuiFileDialog::Instance();
        if (dialogNeedsPlacement)
        {
            ImGui::SetNextWindowViewport(vp->ID);
            ImVec2 sz(vp->Size.x * 0.7f, vp->Size.y * 0.7f);
            ImVec2 pos(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f, vp->Pos.y + (vp->Size.y - sz.y) * 0.5f);
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
            dialogNeedsPlacement = false;
        }
        bool finished = dialog->Display("add-source-loc", ImGuiWindowFlags_NoCollapse,
                                        ImVec2(480.0f, 320.0f), vp->Size);
        saveFileDialogPlaces();
        if (finished)
        {
            if (dialog->IsOk())
            {
                std::string picked = dialog->GetFilePathName();
                std::error_code ec;
                std::filesystem::path target =
                    (!picked.empty() && std::filesystem::is_regular_file(picked, ec))
                        ? std::filesystem::path(picked).parent_path()
                        : std::filesystem::path(dialog->GetCurrentPath());
                addSourceLocation(target.string());
            }
            state = State::edit;
            dialog->Close();
        }
    }

    ImGui::End();

    // remove any tabs that got closed via window 'X'
    for (size_t i = 0; i < tabs.size();)
    {
        if (!tabs[i]->open)
        {
            if (isDirtyTab(i))
            {
                size_t toClose = i;
                tabs[i]->open = true; // prevent immediate disappearance
                activeTab = i;
                showConfirmClose([this, toClose]() { closeTab(toClose); });
                ++i;
            }
            else
            {
                closeTab(i);
            }
        }
        else
        {
            ++i;
        }
    }

    // guarantee at least one document exists
    if (tabs.empty())
        newTab();

    auto perfT1 = std::chrono::steady_clock::now();
    updateFpsWatch(std::chrono::duration<double, std::milli>(perfT1 - perfT0).count());
}

//  Frame-time watchdog: rolling 3s worst UI-build time + count of frames over the
//  30fps budget (shown in Dev Tools, slow frames logged to stderr).
void Editor::updateFpsWatch(double renderMs)
{
    double now = ImGui::GetTime();
    if (fpsWindowStart == 0.0)
        fpsWindowStart = now;
    if (renderMs > fpsWindowWorstMs)
        fpsWindowWorstMs = static_cast<float>(renderMs);

    if (renderMs > 33.0 && now > 2.0) // skip warm-up; 33ms = a 30fps frame
    {
        ++fpsSlowCount;
        if (fpsSlowCount <= 20 || (fpsSlowCount % 100) == 0)
            std::fprintf(stderr, "[perf] slow frame: UI build %.1f ms (#%d)\n", renderMs, fpsSlowCount);
    }

    if (now - fpsWindowStart >= 3.0) // publish the rolling window
    {
        fpsWorstMs = fpsWindowWorstMs;
        fpsWindowWorstMs = 0.0f;
        fpsWindowStart = now;
    }
}

//
//  Editor::renderDockedDocuments
//

void Editor::renderDockedDocuments()
{
    // Target the dockspace's CENTRAL node for new documents. DockBuilderGetCentralNode
    // returns it regardless of the saved layout, so new docs always land in the
    // editing area as tabs — never derived from a (possibly floating/popped-out)
    // window, which is what made tabs cascade out into their own viewports.
    ImGuiID rootId = ImGui::GetID("MainDockSpace");
    ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(rootId);
    ImGuiID centralId = central ? central->ID
                                : (centralDockId ? (ImGuiID)centralDockId : rootId);

    // Honor a pending "split right" request by splitting the central node and
    // re-docking the active doc into the new right pane.
    if ((wantSplitRight || wantSplitLeft) && tabs.size() >= 2 && activeTab < tabs.size())
    {
        if (countDocNodes() >= 2)
        {
            pushToast("Split limit: only two side-by-side editor panes", IM_COL32(240, 200, 90, 255));
        }
        else if (central && central->IsLeafNode()) // only split a real leaf node
        {
            ImGuiID newId = 0, otherId = 0;
            ImGui::DockBuilderSplitNode(centralId, wantSplitLeft ? ImGuiDir_Left : ImGuiDir_Right,
                                        0.5f, &newId, &otherId);
            if (newId != 0)
            {
                ImGui::DockBuilderDockWindow(windowLabelFor(*tabs[activeTab]).c_str(), newId);
                ImGui::DockBuilderFinish(rootId);
                tabs[activeTab]->wantFocus = true;
                tabs[activeTab]->dockedOnce = true;
            }
        }
        wantSplitRight = false;
        wantSplitLeft = false;
    }

    // Re-fetch central (a split above may have reallocated nodes).
    central = ImGui::DockBuilderGetCentralNode(rootId);

    // Honor a pending "open to left/right" — a file opened from the nav panel
    // that should land in a split beside the central docs, not as a tab.
    if (pendingSideDir != 0)
    {
        if (countDocNodes() >= 2)
        {
            // Already at the split limit. Don't dump the file into whatever node
            // is "central" (that's always the left pane) — dock it as a tab in the
            // existing LEFT or RIGHT pane so it lands on the requested side.
            ImGuiDockNode *leftNode = nullptr, *rightNode = nullptr;
            for (auto &up : tabs)
            {
                ImGuiWindow *w = ImGui::FindWindowByName(windowLabelFor(*up).c_str());
                if (!w || !w->DockNode)
                    continue;
                ImGuiDockNode *nd = w->DockNode;
                if (!leftNode || nd->Pos.x < leftNode->Pos.x)
                    leftNode = nd;
                if (!rightNode || nd->Pos.x > rightNode->Pos.x)
                    rightNode = nd;
            }
            ImGuiDockNode *target = (pendingSideDir < 0) ? leftNode : rightNode;
            if (target)
            {
                for (auto &up : tabs)
                {
                    if (up->id != pendingSideDocId)
                        continue;
                    ImGui::DockBuilderDockWindow(windowLabelFor(*up).c_str(), target->ID);
                    ImGui::DockBuilderFinish(rootId);
                    up->dockedOnce = true; // keep the loop from re-docking it to centre
                    up->wantFocus = true;
                    break;
                }
            }
        }
        else if (central && central->IsLeafNode())
        {
            for (auto &up : tabs)
            {
                if (up->id != pendingSideDocId)
                    continue;
                ImGuiID sideId = 0, restId = 0;
                ImGui::DockBuilderSplitNode(centralId,
                                            pendingSideDir < 0 ? ImGuiDir_Left : ImGuiDir_Right,
                                            0.5f, &sideId, &restId);
                if (sideId != 0)
                {
                    ImGui::DockBuilderDockWindow(windowLabelFor(*up).c_str(), sideId);
                    ImGui::DockBuilderFinish(rootId);
                    up->dockedOnce = true; // don't let the loop re-dock it to centre
                    up->wantFocus = true;
                }
                break;
            }
        }
        pendingSideDir = 0;
        pendingSideDocId = 0;
    }

    // Re-fetch central: the split / open-to-side DockBuilder ops above may have
    // reallocated nodes, invalidating the cached pointer.
    central = ImGui::DockBuilderGetCentralNode(rootId);

    // Markdown preview: the first frame it opens, dock it into a split to the
    // RIGHT of the documents so it sits beside the .md file (not floating).
    if (mdPreviewVisible && !mdPreviewWasVisible)
        mdPreviewWantDock = true;
    mdPreviewWasVisible = mdPreviewVisible;
    // Only split a real LEAF central node — splitting a non-leaf (already-split)
    // node is undefined in DockBuilder and can deref a null node.
    if (mdPreviewWantDock && central && central->IsLeafNode())
    {
        ImGuiID sideId = 0, restId = 0;
        ImGui::DockBuilderSplitNode(centralId, ImGuiDir_Right, 0.4f, &sideId, &restId);
        if (sideId != 0)
        {
            ImGui::DockBuilderDockWindow("Markdown Preview###mdPreview", sideId);
            ImGui::DockBuilderFinish(rootId);
        }
        mdPreviewWantDock = false;
    }
    else if (mdPreviewWantDock && central)
    {
        mdPreviewWantDock = false; // central not splittable right now — leave preview floating
    }

    for (size_t i = 0; i < tabs.size(); ++i)
    {
        auto &t = *tabs[i];
        // Force a brand-new doc into the central node exactly once (Always, so
        // it actually moves), then leave it alone so the user can drag it out
        // or split it afterwards.
        if (!t.dockedOnce)
        {
            ImGui::SetNextWindowDockID(centralId, ImGuiCond_Always);
            t.dockedOnce = true;
        }
        bool focusing = t.wantFocus;
        if (focusing)
        {
            ImGui::SetNextWindowFocus();
            t.wantFocus = false;
        }
        renderDocumentWindow(t);
        if (focusing)
        {
            std::string lbl = windowLabelFor(t);
            ImGui::SetWindowFocus(lbl.c_str());
            // SetWindowFocus selects the dock tab but doesn't reliably bring an
            // off-screen tab into view. Queue an explicit scroll on the owning
            // dock node's tab bar so opening a file (nav click, Go-to-Definition,
            // "Open Elsewhere") always reveals its tab. MUST use the ImGuiTabItem*
            // overload: TabBarQueueFocus(tab_bar, const char*) asserts it is NOT a
            // dock-node tab bar (IM_ASSERT DockNode flag == 0), which aborted the
            // app when opening any file into an existing dock node's tab bar.
            if (ImGuiWindow *w = ImGui::FindWindowByName(lbl.c_str()))
                if (w->DockNode && w->DockNode->TabBar)
                    if (ImGuiTabItem *tab = ImGui::TabBarFindTabByID(w->DockNode->TabBar, w->TabId))
                        ImGui::TabBarQueueFocus(w->DockNode->TabBar, tab);
        }
    }

    // Re-fetch the central node: the split/side/markdown-preview DockBuilder
    // operations above can reallocate the dock-node pool, leaving the `central`
    // pointer cached at the top of this function DANGLING. Reading the stale
    // pointer (e.g. central->TabBar->BarRect below) reads freed memory and
    // crashes — this is the "markdown preview crash".
    central = ImGui::DockBuilderGetCentralNode(rootId);

    // "+" button on the central node's tab bar → new tab. AmendTabBar lets us
    // append our own widget to a dock node's auto-generated tab bar.
    if (central && ImGui::DockNodeBeginAmendTabBar(central))
    {
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
        {
            newFile();
        }
        ImGui::DockNodeEndAmendTabBar();
    }

    // Mouse-wheel over the tab strip scrolls it horizontally (like the scroll
    // arrows) instead of changing the selection.
    if (central && central->TabBar)
    {
        ImGuiTabBar *tb = central->TabBar;
        if (ImGui::IsMouseHoveringRect(tb->BarRect.Min, tb->BarRect.Max, false))
        {
            float wheel = ImGui::GetIO().MouseWheel + ImGui::GetIO().MouseWheelH;
            if (wheel != 0.0f)
            {
                float maxScroll = (std::max)(0.0f, tb->WidthAllTabs - tb->BarRect.GetWidth());
                tb->ScrollingTarget = ImClamp(tb->ScrollingTarget - wheel * 80.0f, 0.0f, maxScroll);
            }
        }
    }

    // Right-click a document tab → context menu. Open on RELEASE (the standard
    // ImGui idiom) so the same click doesn't immediately dismiss the popup.
    // NOTE: ImGui also selects the tab on right-click by design; blocking that
    // cleanly fought the dock tab bar too hard, so we accept it for now.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
    {
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            ImGuiWindow *w = ImGui::FindWindowByName(windowLabelFor(*tabs[i]).c_str());
            if (!w)
                continue;
            const ImRect &r = w->DC.DockTabItemRect;
            if (r.GetWidth() <= 0.0f)
                continue;
            if (ImGui::IsMouseHoveringRect(r.Min, r.Max, false))
            {
                tabCtxIdx = (int)i;
                ImGui::OpenPopup("##tabCtxMenu");
                break;
            }
        }
    }
    if (tabCtxIdx >= 0 && tabCtxIdx < (int)tabs.size() && ImGui::BeginPopup("##tabCtxMenu", ImGuiWindowFlags_NoMove))
    {
        renderTabContextMenu(tabCtxIdx);
        ImGui::EndPopup();
    }
}

//  Count distinct dock nodes that currently host document tabs — used to cap
//  the editor at two side-by-side document panes (plus nav / panels).
int Editor::countDocNodes() const
{
    std::unordered_set<ImGuiID> nodes;
    for (auto &up : tabs)
    {
        ImGuiWindow *w = ImGui::FindWindowByName(windowLabelFor(*up).c_str());
        if (w && w->DockNode)
            nodes.insert(w->DockNode->ID);
    }
    return (int)nodes.size();
}

//  Right-click-a-tab context menu. Close actions just flag open=false and let
//  the render() cleanup run the normal dirty-confirm path (no data loss).
void Editor::renderTabContextMenu(int idx)
{
    if (idx < 0 || idx >= (int)tabs.size())
        return;
    auto &t = *tabs[idx];
    bool hasPath = t.filename != "untitled";
    int n = (int)tabs.size();

    // Build the visual tab order of the SAME dock node as this tab, so the
    // left/right/others closes never touch tabs in another split or pop-out.
    std::vector<int> nodeOrder; // indices into `tabs`, in this node's tab order
    int posInNode = -1;
    {
        ImGuiWindow *tw = ImGui::FindWindowByName(windowLabelFor(t).c_str());
        ImGuiDockNode *node = tw ? tw->DockNode : nullptr;
        if (node && node->TabBar)
        {
            for (int ti = 0; ti < node->TabBar->Tabs.Size; ++ti)
            {
                ImGuiWindow *tiw = node->TabBar->Tabs[ti].Window;
                if (!tiw)
                    continue;
                for (int j = 0; j < n; ++j)
                {
                    ImGuiWindow *w = ImGui::FindWindowByName(windowLabelFor(*tabs[j]).c_str());
                    if (w == tiw)
                    {
                        if (j == idx)
                            posInNode = (int)nodeOrder.size();
                        nodeOrder.push_back(j);
                        break;
                    }
                }
            }
        }
    }
    bool grouped = posInNode >= 0 && nodeOrder.size() > 1;

    ImGui::TextDisabled("%s", std::filesystem::path(t.filename).filename().string().c_str());
    ImGui::Separator();

    if (ImGui::MenuItem("Close"))
    {
        t.open = false;
    }
    if (ImGui::MenuItem("Close Others", nullptr, false, grouped))
        for (int j : nodeOrder)
            if (j != idx)
                tabs[j]->open = false;
    if (ImGui::MenuItem("Close to the Right", nullptr, false, grouped && posInNode < (int)nodeOrder.size() - 1))
        for (int k = posInNode + 1; k < (int)nodeOrder.size(); ++k)
            tabs[nodeOrder[k]]->open = false;
    if (ImGui::MenuItem("Close to the Left", nullptr, false, grouped && posInNode > 0))
        for (int k = 0; k < posInNode; ++k)
            tabs[nodeOrder[k]]->open = false;

    ImGui::Separator();
    if (ImGui::MenuItem("Split Left", nullptr, false, n >= 2))
    {
        activeTab = idx;
        wantSplitLeft = true;
    }
    if (ImGui::MenuItem("Split Right", nullptr, false, n >= 2))
    {
        activeTab = idx;
        wantSplitRight = true;
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Diff with Current Tab", nullptr, false, idx != (int)activeTab))
    {
        auto &cur = doc();
        cur.diff.SetLanguage(cur.editor.GetLanguage());
        cur.diff.SetText(t.editor.GetText(), cur.editor.GetText()); // this tab (left) vs current (right)
        dialogViewportId = ImGui::GetMainViewport()->ID;
        dialogNeedsPlacement = true;
        state = State::diff;
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Reveal in Explorer", nullptr, false, hasPath))
        navOpenPathInExplorer(t.filename);
    if (ImGui::MenuItem("Copy File Path", nullptr, false, hasPath))
        ImGui::SetClipboardText(t.filename.c_str());
}

//
//  Editor::renderDocumentWindow
//

void Editor::renderDocumentWindow(TabDocument &t)
{
    std::string label = windowLabelFor(t);

    bool open = t.open;
    if (!ImGui::Begin(label.c_str(), &open, ImGuiWindowFlags_NoCollapse))
    {
        t.open = open;
        ImGui::End();
        return;
    }
    t.open = open;

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        // find this tab's index and make it active
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            if (tabs[i].get() == &t)
            {
                activeTab = i;
                break;
            }
        }
        t.externallyTouched = false; // user is looking at it now → drop the tab badge
    }

    // Once the user starts editing again, the Claude-changed gutter markers have
    // served their purpose — clear them so they don't linger over fresh edits.
    if (t.externalMarkers && t.editor.GetUndoIndex() != t.version)
        clearChangeMarks(t); // user started editing → drop markers, dot decorator, and ranges together

    ImGui::PushFont(activeFont, fontSize);
    t.editor.SetTextContextMenuCallback([this, &t](int line, int column) {
        // (removed) per-open trie rebuild was dead work: nothing in this menu
        // reads the trie, and autocomplete maintains its own trie on tab open + edit.

        // Resolve the "operative word" for word-aware menu items:
        //   1. If there's an active selection, use that text — respects
        //      multi-select and lets the user pick the exact symbol.
        //   2. Otherwise, the word at the right-click's (line, column).
        // The screen-position lookup we used to do was wrong: it captured
        // the layout cursor at render time (top-left of editor child),
        // so GetWordAtScreenPos returned garbage from row 0.
        std::string word;
        if (t.editor.AnyCursorHasSelection())
        {
            word = t.editor.GetCurrentSelectionText();
            // Strip whitespace — multi-line selections often start/end
            // with newlines that break GoToDefinitionOf lookups.
            while (!word.empty() && (word.back() == '\n' || word.back() == '\r' || word.back() == ' ' || word.back() == '\t'))
                word.pop_back();
            while (!word.empty() && (word.front() == '\n' || word.front() == '\r' || word.front() == ' ' || word.front() == '\t'))
                word.erase(word.begin());
        }
        if (word.empty())
        {
            word = t.editor.GetWordAt(line, column);
        }

        if (ImGui::MenuItem("Copy", "Ctrl-C"))
        {
            t.editor.Copy();
        }
        if (ImGui::MenuItem("Cut", "Ctrl-X"))
        {
            t.editor.Cut();
        }
        if (!projectRoot.empty())
        {
            if (ImGui::MenuItem("Add Sticky Note…"))
                wantNotePopupLine = line; // opened after the menu closes (popup-in-popup)
        }
        if (ImGui::MenuItem("Paste", "Ctrl-V"))
        {
            t.editor.Paste();
        }
        if (ImGui::MenuItem("Select All", "Ctrl-A"))
        {
            t.editor.SelectAll();
        }
        // Path helpers — only on whitespace right-click. If the user clicked
        // on a word the word-aware items (Go to Definition, etc.) take priority.
        if (word.empty() && t.filename != "untitled")
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Open Containing Folder"))
            {
                openContainingFolder();
            }
            if (ImGui::MenuItem("Copy File Path"))
            {
                ImGui::SetClipboardText(t.filename.c_str());
            }
            ImGui::Separator();
        }

        // "Go to file" for #include / require / import "path" or <path> on the
        // current line. Best-effort lexical match — resolves relative to the
        // current document's directory.
        std::string lineText = t.editor.GetLineText(line);
        auto extractInclude = [](const std::string &s) -> std::string {
            auto p1 = s.find('"');
            auto p2 = (p1 != std::string::npos) ? s.find('"', p1 + 1) : std::string::npos;
            if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1 + 1)
            {
                return s.substr(p1 + 1, p2 - p1 - 1);
            }
            auto a1 = s.find('<');
            auto a2 = (a1 != std::string::npos) ? s.find('>', a1 + 1) : std::string::npos;
            if (a1 != std::string::npos && a2 != std::string::npos && a2 > a1 + 1)
            {
                return s.substr(a1 + 1, a2 - a1 - 1);
            }
            return {};
        };
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            return (a == std::string::npos) ? std::string{} : s.substr(a, b - a + 1);
        };
        std::string trimmed = trim(lineText);
        bool isInclude = trimmed.rfind("#include", 0) == 0 || trimmed.rfind("import ", 0) == 0 || trimmed.rfind("from ", 0) == 0;
        if (isInclude)
        {
            std::string inc = extractInclude(trimmed);
            if (!inc.empty())
            {
                // MEMOIZED — this callback re-runs every frame the popup is open and the
                // resolution below recursively walks the project tree + MSVC/Windows SDK
                // include dirs (tens of thousands of files); running it ~60x/sec froze
                // the app on any #include line. Recompute only when (doc,include) change.
                std::string memoKey = t.filename + "|" + inc;
                if (memoKey != ctxIncludeKey)
                {
                    ctxIncludeKey = memoKey;
                    // Resolution strategy (in order):
                    //  1. <docDir>/inc                       — sibling file
                    //  2. Walk up the doc's parent directories for up to 6 levels;
                    //     at each level try <level>/inc and also try every dir
                    //     whose name is the inc's first path segment.
                    //  3. Find a project root (directory containing *.sln, *.vcxproj,
                    //     CMakeLists.txt, or .git); recursively search there (cap
                    //     depth at 4) for any file matching the include's basename.
                    //  4. System include roots (MSVC toolchain, Windows SDK) read
                    //     from environment vars; lets <vector>, <string>, etc. work.
                    std::error_code ec;
                    std::filesystem::path docDir = std::filesystem::path(t.filename).parent_path();
                    std::filesystem::path incPath(inc);
                    std::filesystem::path candidate;
                    bool found = false;

                    auto tryPath = [&](const std::filesystem::path &p) {
                        if (!found && std::filesystem::is_regular_file(p, ec))
                        {
                            candidate = p;
                            found = true;
                        }
                    };

                    // 1.
                    tryPath(docDir / incPath);

                    // 2.
                    if (!found)
                    {
                        auto cur = docDir;
                        for (int i = 0; i < 6 && !found; ++i)
                        {
                            tryPath(cur / incPath);
                            if (cur.has_parent_path() && cur.parent_path() != cur)
                                cur = cur.parent_path();
                            else
                                break;
                        }
                    }

                    // 3. Find project root, then recursive search.
                    if (!found)
                    {
                        std::filesystem::path projectRoot;
                        {
                            auto cur = docDir;
                            for (int i = 0; i < 8 && projectRoot.empty(); ++i)
                            {
                                for (auto &entry : std::filesystem::directory_iterator(cur, ec))
                                {
                                    if (!entry.is_regular_file() && !entry.is_directory())
                                        continue;
                                    auto name = entry.path().filename().string();
                                    auto ext = entry.path().extension().string();
                                    if (ext == ".sln" || ext == ".vcxproj" || ext == ".uproject" ||
                                        name == "CMakeLists.txt" || name == ".git")
                                    {
                                        projectRoot = cur;
                                        break;
                                    }
                                }
                                if (!projectRoot.empty())
                                    break;
                                if (cur.has_parent_path() && cur.parent_path() != cur)
                                    cur = cur.parent_path();
                                else
                                    break;
                            }
                        }
                        if (!projectRoot.empty())
                        {
                            auto wantedName = incPath.filename().string();
                            auto wantedSuffix = incPath.generic_string();
                            int depth = 0;
                            for (auto it = std::filesystem::recursive_directory_iterator(
                                     projectRoot, std::filesystem::directory_options::skip_permission_denied, ec);
                                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                            {
                                // Prune below a generous cap — but only stop
                                // RECURSING; still test the current entry. The old
                                // `>4 → continue` skipped files sitting AT the cap,
                                // so nested layouts like a submodule SDK
                                // (deps/submodules/UESDK/src/sdk/Foo.hpp, depth 5)
                                // never matched. Deep enough for vendored trees,
                                // shallow enough to stay bounded with the budget.
                                if (it.depth() > 9)
                                    it.disable_recursion_pending();
                                if (it->is_directory(ec))
                                {
                                    // Skip heavy non-source trees so the file budget
                                    // reaches real dependency headers.
                                    auto dn = it->path().filename().string();
                                    if (dn == ".git" || dn == "node_modules" || dn == "Binaries" ||
                                        dn == "Intermediate" || dn == "DerivedDataCache" ||
                                        dn == "build" || dn == "out" || dn == ".vs")
                                        it.disable_recursion_pending();
                                    continue;
                                }
                                if (++depth > 50000)
                                    break; // safety bound
                                if (!it->is_regular_file(ec))
                                    continue;
                                auto p = it->path();
                                if (p.filename().string() != wantedName)
                                    continue;
                                // Prefer matches whose tail matches the include (so
                                // "imgui/imgui.h" doesn't pick a random "imgui.h" in
                                // the build tree).
                                auto pStr = p.generic_string();
                                if (pStr.size() >= wantedSuffix.size() &&
                                    pStr.compare(pStr.size() - wantedSuffix.size(), wantedSuffix.size(), wantedSuffix) == 0)
                                {
                                    candidate = p;
                                    found = true;
                                    break;
                                }
                                // Remember first basename match as fallback.
                                if (!found)
                                {
                                    candidate = p;
                                    found = true;
                                }
                            }
                        }
                    }

                    // 3.5. Project-type plugins (e.g. Unreal): UE includes are
                    // MODULE-relative ("GameFramework/Actor.h" lives in Engine/
                    // Source/Runtime/Engine/Classes/...), so neither the project
                    // walk nor the system roots can find them. The plugin resolves
                    // against its module/plugin/generated include roots.
                    if (!found)
                    {
                        if (auto hit = pluginRegistry.resolveInclude(docDir, inc))
                        {
                            candidate = *hit;
                            found = true;
                        }
                    }

                    // 4. System include roots — MSVC + Windows SDK. Auto-detected
                    // from environment vars set by vcvarsall.bat / a Developer
                    // Command Prompt. Headers like <vector>, <string>, <windows.h>
                    // only resolve when the editor itself was launched from such
                    // a prompt (which our build script does, so .h files like
                    // <vector> work out of the box in dev builds).
                    if (!found)
                    {
                        // Self-discovered MSVC + Windows SDK include dirs (cached) —
                        // no dev-cmd %INCLUDE% needed; see systemIncludeDirs().
                        const std::vector<std::filesystem::path> &sysRoots = systemIncludeDirs();

                        auto wantedName = incPath.filename().string();
                        for (const auto &root : sysRoots)
                        {
                            if (!std::filesystem::is_directory(root, ec))
                                continue;
                            // First try a direct join (handles <foo.h> sitting at the root)
                            auto direct = root / incPath;
                            if (std::filesystem::is_regular_file(direct, ec))
                            {
                                candidate = direct;
                                found = true;
                                break;
                            }
                            // Then recurse one level for SDK subfolders like ucrt/, shared/, um/
                            int budget = 20000;
                            for (auto it = std::filesystem::recursive_directory_iterator(
                                     root, std::filesystem::directory_options::skip_permission_denied, ec);
                                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                            {
                                if (it.depth() > 3)
                                {
                                    it.disable_recursion_pending();
                                    continue;
                                }
                                if (--budget < 0)
                                    break;
                                if (!it->is_regular_file(ec))
                                    continue;
                                if (it->path().filename().string() == wantedName)
                                {
                                    candidate = it->path();
                                    found = true;
                                    break;
                                }
                            }
                            if (found)
                                break;
                        }
                    }

                    ctxIncludeFound = found;
                    ctxIncludeResult = candidate.string();
                }
                std::string label = "Go to File: " + inc;
                if (ImGui::MenuItem(label.c_str(), nullptr, false, ctxIncludeFound))
                {
                    openFile(ctxIncludeResult);
                }
            }
        }

        // Log-file references: "path(123)" (MSVC), "path:123" (clang/gcc/UE
        // callstacks), and UE crash "[File:...cpp] [Line: 123]" on the current
        // line resolve to a project file + line — crash logs jump straight to
        // code. Memoized like the include resolution (this re-runs per frame).
        if (!isInclude)
        {
            std::string logKey = t.filename + "|" + std::to_string(line) + "|" + lineText;
            if (logKey != ctxLogKey)
            {
                ctxLogKey = logKey;
                ctxLogFile.clear();
                ctxLogLabel.clear();
                ctxLogLine = 0;

                // Parse: scan for a run of path characters containing a '.',
                // followed by "(digits)" or ":digits". Also UE's split form.
                auto isPathChar = [](char c) {
                    return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '/' ||
                           c == '\\' || c == '-' || c == '+' || c == '~';
                };
                std::string refFile;
                int refLine = 0;
                const std::string &s = lineText;
                for (size_t i = 0; i < s.size() && refFile.empty(); ++i)
                {
                    if (!std::isdigit((unsigned char)s[i]))
                        continue;
                    size_t dEnd = i;
                    while (dEnd < s.size() && std::isdigit((unsigned char)s[dEnd]))
                        ++dEnd;
                    // "(123)" or ":123" — the char before the digits.
                    if (i == 0)
                        continue;
                    char sep = s[i - 1];
                    if (sep == '(' && (dEnd >= s.size() || s[dEnd] != ')'))
                        continue;
                    if (sep != '(' && sep != ':')
                        continue;
                    size_t pEnd = i - 1; // exclusive end of the path
                    size_t pBeg = pEnd;
                    while (pBeg > 0 && isPathChar(s[pBeg - 1]))
                        --pBeg;
                    // Optional Windows drive prefix ("C:") directly before the path.
                    if (pBeg >= 2 && s[pBeg - 1] == ':' && std::isalpha((unsigned char)s[pBeg - 2]) &&
                        (pBeg == 2 || !isPathChar(s[pBeg - 3])))
                        pBeg -= 2;
                    std::string cand = s.substr(pBeg, pEnd - pBeg);
                    // Must look like a file: has an extension dot that isn't leading.
                    auto dot = cand.find_last_of('.');
                    if (cand.size() < 3 || dot == std::string::npos || dot == 0 ||
                        dot + 1 >= cand.size())
                        continue;
                    refFile = cand;
                    refLine = std::atoi(s.substr(i, dEnd - i).c_str());
                    i = dEnd;
                }
                // UE crash split form: [File:D:\...\x.cpp] ... [Line: 123]
                if (refFile.empty())
                {
                    auto fp = s.find("[File:");
                    auto lp = s.find("[Line:");
                    if (fp != std::string::npos && lp != std::string::npos)
                    {
                        auto fe = s.find(']', fp);
                        if (fe != std::string::npos)
                        {
                            refFile = s.substr(fp + 6, fe - fp - 6);
                            refLine = std::atoi(s.c_str() + lp + 6);
                        }
                    }
                }

                if (!refFile.empty() && refLine > 0)
                {
                    // Resolve: absolute + exists wins; otherwise search the project
                    // for the basename (bounded, like the include resolution).
                    std::error_code lec;
                    std::filesystem::path rp(refFile);
                    if (rp.is_absolute() && std::filesystem::is_regular_file(rp, lec))
                        ctxLogFile = rp.string();
                    else if (!projectRoot.empty())
                    {
                        auto wanted = rp.filename().string();
                        int budget = 50000;
                        for (auto wit = std::filesystem::recursive_directory_iterator(
                                 projectRoot, std::filesystem::directory_options::skip_permission_denied, lec);
                             !lec && wit != std::filesystem::recursive_directory_iterator(); wit.increment(lec))
                        {
                            if (wit.depth() > 6)
                            {
                                wit.disable_recursion_pending();
                                continue;
                            }
                            if (--budget <= 0)
                                break;
                            if (wit->is_regular_file(lec) && wit->path().filename().string() == wanted)
                            {
                                ctxLogFile = wit->path().string();
                                break;
                            }
                        }
                    }
                    if (!ctxLogFile.empty())
                    {
                        ctxLogLine = refLine;
                        ctxLogLabel = "Go to " + std::filesystem::path(refFile).filename().string() +
                                      "(" + std::to_string(refLine) + ")";
                    }
                }
            }
            if (!ctxLogFile.empty() && ctxLogLine > 0)
            {
                if (ImGui::MenuItem(ctxLogLabel.c_str()))
                {
                    openFile(ctxLogFile);
                    if (!tabs.empty() && doc().filename == ctxLogFile)
                    {
                        auto &e = doc().editor;
                        e.SetCursor(ctxLogLine - 1, 0);
                        e.SelectLine(ctxLogLine - 1);
                        e.ScrollToLine(ctxLogLine - 1, TextEditor::Scroll::alignMiddle);
                    }
                }
            }
        }

        // On an #include / import line the only meaningful navigation is
        // "Go to File" (above). Suppress the symbol items (Go to
        // Definition / Declaration / Find References / Learn) — the word
        // under the cursor is a header/module name, and grepping it just
        // jumps to unrelated uses in files that aren't even included here.
        if (!isInclude && !word.empty())
        {
            // Preserve any existing selection — only auto-select the word
            // under the cursor when the user right-clicked on bare text.
            if (!t.editor.AnyCursorHasSelection())
            {
                t.editor.SelectWord(line, column);
            }
            // Qualified name (System.Diagnostics, std::vector) for the
            // navigation items; plain word for in-file occurrence ops.
            std::string qualified = t.editor.GetQualifiedWordAt(line, column);
            if (qualified.empty())
                qualified = word;

            // Cheap gate for the navigation items: only show Go to
            // Definition / Declaration when the (last-segment) symbol is
            // actually navigable. Uses data we ALREADY have — no extra
            // scanning:
            //   * a language keyword/declaration (if/for/class/int/...) is
            //     never a definition target → hide;
            //   * otherwise it must be known: in this file's identifier trie
            //     OR have a recorded definition site in the project index.
            // This keeps the menu honest (no "Go to Definition" that just
            // pops "not found") and is O(keywords)+O(1), only while the
            // popup is open.
            const auto *lang = t.editor.GetLanguage();
            std::string seg = qualified;
            {
                size_t cut = seg.size();
                for (size_t i = 0; i < seg.size(); ++i)
                {
                    if (seg[i] == '.')
                        cut = i + 1;
                    else if (seg[i] == ':' && i + 1 < seg.size() && seg[i + 1] == ':')
                        cut = i + 2;
                    else if (seg[i] == '-' && i + 1 < seg.size() && seg[i + 1] == '>')
                        cut = i + 2;
                }
                if (cut < seg.size())
                    seg = seg.substr(cut);
            }
            bool isKeyword = false;
            if (lang)
            {
                for (auto &kw : lang->keywords)
                    if (kw == seg)
                    {
                        isKeyword = true;
                        break;
                    }
                if (!isKeyword)
                    for (auto &kw : lang->declarations)
                        if (kw == seg)
                        {
                            isKeyword = true;
                            break;
                        }
            }
            bool known = !seg.empty() && t.trie.contains(seg);
            if (!known && !seg.empty())
                if (auto idx = indexSnapshot())
                    // Consult BOTH indexes: the grep `defs` AND the tree-sitter `tsDefs`.
                    // tsDefs is where non-clangd languages (Lua, Go, Rust, Python, JS)
                    // record definitions and is exactly what tsGoToDefinition() resolves
                    // against — checking only `defs` hid "Go to Definition" for them
                    // (e.g. cross-file Lua functions) even though the jump would succeed.
                    known = idx->defs.count(seg) != 0 || idx->tsDefs.count(seg) != 0;
            bool navigable = known && !isKeyword;

            // Definition = where it's defined (bodies / .cpp). Declaration =
            // where it's declared (prototypes / headers). Both project-wide,
            // both handle namespaced/qualified names.
            if (navigable && ImGui::MenuItem("Go to Definition"))
            {
                goToDefinitionProjectWide(qualified, false);
            }
            // Declaration vs definition only means something where
            // headers exist (C/C++); other languages have a single
            // definition site, so don't show it for them.
            {
                bool hasDecl = lang && (lang->name == "C" || lang->name == "C++");
                if (navigable && hasDecl && ImGui::MenuItem("Go to Declaration"))
                {
                    goToDefinitionProjectWide(qualified, true);
                }
                // C# SDK symbols have no on-disk source (ref assemblies).
                // For a fully namespace-qualified BCL type we can decompile
                // the runtime assembly to real C# (ilspycmd); for anything
                // else, fall back to the Microsoft Learn docs page.
                if (lang && lang->name == "C#" && !isKeyword && !seg.empty())
                {
                    // Only treat a symbol as a documented .NET/BCL type when it's
                    // namespace-qualified under a Microsoft-owned root. Project
                    // types, NuGet packages (Newtonsoft.*, etc.) and linked
                    // projects have their own roots, so the Decompile / Learn
                    // options no longer appear for them (they're meaningless there).
                    bool bclRooted = false;
                    if (auto dot = qualified.find('.'); dot != std::string::npos)
                    {
                        std::string r = qualified.substr(0, dot);
                        bclRooted = (r == "System" || r == "Microsoft" ||
                                     r == "Windows" || r == "Internal" || r == "Mono");
                    }
                    // A symbol the project itself defines is never a BCL type.
                    if (navigable)
                        bclRooted = false;
                    if (bclRooted)
                    {
                        if (ImGui::MenuItem("Go to Decompiled Source"))
                            openCSharpDecompiled(qualified);
                        if (ImGui::MenuItem("Look up in Microsoft Learn"))
                            openCSharpLearn(qualified);
                    }
                }
            }
            if (ImGui::MenuItem("Find References"))
            {
                findReferencesOf(t, word);
            }
            if (ImGui::MenuItem("Select All Occurrences (this file)"))
            {
                t.editor.SelectAllOccurrencesOf(word, true, true);
            }
        }

        // Plugin context-menu contributions (e.g. the cppgen plugin's Generate
        // Definition/Declaration). Runs inside the popup scope every frame it's
        // open — the context carries lineCount/undoIndex so plugins can memoize
        // whole-document work instead of re-pulling the text.
        {
            PluginDocContext pctx;
            pctx.doc.filename = t.filename;
            pctx.doc.extLower = std::filesystem::path(t.filename).extension().string();
            std::transform(pctx.doc.extLower.begin(), pctx.doc.extLower.end(), pctx.doc.extLower.begin(),
                           [](unsigned char c) { return (char) std::tolower(c); });
            if (const auto *glang = t.editor.GetLanguage())
                pctx.doc.languageName = glang->name;
            pctx.line = line;
            pctx.word = word;
            pctx.lineText = t.editor.GetLineText(line);
            pctx.lineCount = t.editor.GetLineCount();
            pctx.docVersion = (int) t.editor.GetUndoIndex();
            pluginRegistry.documentContextMenu(*this, pctx);
        }

        ImGui::Separator();
        ImGui::Text("Line %d, column %d", line + 1, column + 1); });

    // Conflict bar: disk changed (Claude / another tool) while this buffer has
    // unsaved edits. The user picks who wins — take theirs, keep mine, or diff.
    if (t.externalChange)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.20f, 0.04f, 1.0f));
        ImGui::BeginChild("##extChangeBar", ImVec2(0.0f, ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
                           "\xe2\x9a\xa0 Changed on disk while you have unsaved edits.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload (theirs)"))
        {
            reloadFromDisk(t);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep Mine"))
        {
            recordDiskMtime(t);
            t.externalChange = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Merge"))
        {
            mergeExternalChange(t);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("3-way merge your edits with the on-disk version (conflicts get <<<<<<< markers)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Diff"))
        {
            std::ifstream f(t.filename.c_str());
            std::string disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();
            t.diff.SetLanguage(t.editor.GetLanguage());
            t.diff.SetText(disk, t.editor.GetText()); // theirs (disk) vs mine (buffer)
            dialogViewportId = ImGui::GetMainViewport()->ID;
            dialogNeedsPlacement = true;
            state = State::diff;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImVec2 editorSize = ImGui::GetContentRegionAvail();
    t.editor.Render("##editorContent", editorSize);

    // Hover hint: when the user hovers a known symbol in the editor for
    // `hoverDelaySec` without moving the mouse, show a tooltip with the
    // symbol's best-guess definition line + a reference count.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemActive())
    {
        renderHoverTooltip(t);
    }

    ImGui::PopFont();
    ImGui::End();
}

//
//  Editor::tryToQuit
//

void Editor::tryToQuit()
{
    // Persist file-dialog favourites + editor settings BEFORE the quit path
    // can pull us out of the message loop. Wrapped: a filesystem throw here must
    // not escape into the SDL event handler and abort the process on close.
    try
    {
        saveFileDialogPlaces();
        // Snapshot the current tab set into projectSessions for whatever
        // projectRoot is active, so next launch with the same --project arg
        // restores the workspace.
        saveCurrentProjectSession();
        saveSettings();
        // Flush the ImGui layout right now too, so window positions / docking
        // state are guaranteed-persisted even on abrupt close.
        if (auto *fn = ImGui::GetIO().IniFilename)
            ImGui::SaveIniSettingsToDisk(fn);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "tryToQuit: save failed: %s\n", e.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "tryToQuit: save failed (unknown)\n");
    }

    bool anyDirty = false;
    for (size_t i = 0; i < tabs.size(); ++i)
        if (isDirtyTab(i))
        {
            anyDirty = true;
            break;
        }

    if (anyDirty)
        showConfirmQuit();
    else
        done = true;
}

//
//  Editor::renderMenuBar
//

void Editor::renderMenuBar()
{
    if (ImGui::BeginMenuBar())
    {
        // Dropdown popups inherit WindowPadding at BeginMenu time. Widen the
        // horizontal padding so item labels get a clear left margin instead of
        // kissing (and clipping against) the popup's left edge. Pushed AFTER
        // BeginMenuBar so the menu bar's own item layout is unaffected, popped
        // just before EndMenuBar so the push/pop stay balanced.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(18.0f, ImGui::GetStyle().WindowPadding.y));
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Tab", SHORTCUT "N"))
            {
                newFile();
            }
            if (ImGui::MenuItem("Open...", SHORTCUT "O"))
            {
                openFile();
            }
            if (ImGui::MenuItem("Open Project..."))
            {
                openProjectFolderPicker();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Window"))
            {
                spawnNewInstance(""); // a fresh, projectless instance
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open another ImGui-IDE window (run multiple instances side by side)");
            if (ImGui::MenuItem("New Window (this project)", nullptr, false, !projectRoot.empty()))
            {
                spawnNewInstance(projectRoot.string()); // second window on the same project
            }
            ImGui::Separator();
            // Plugins contribute File-menu items here (e.g. UEVR "Open Script").
            pluginRegistry.menu(*this, PluginMenu::File);
            // Recent lists: show the FILENAME as the menu label (full absolute
            // paths made the submenu hundreds of px wide, overflowing back over
            // the parent menu). The full path goes in a hover tooltip.
            auto recentRow = [](const std::string &path) -> bool {
                std::string leaf = std::filesystem::path(path).filename().string();
                if (leaf.empty())
                    leaf = path;
                bool clicked = ImGui::MenuItem(leaf.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
                    ImGui::SetTooltip("%s", path.c_str());
                return clicked;
            };
            if (ImGui::BeginMenu("Open Recent File", !recentFiles.empty()))
            {
                // PushID per row — repeated leaf names (or paths containing ##
                // metacharacters) would otherwise alias to the same widget ID.
                for (size_t i = 0; i < recentFiles.size(); ++i)
                {
                    ImGui::PushID((int)i);
                    if (recentRow(recentFiles[i]))
                        openFile(recentFiles[i]);
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear"))
                {
                    recentFiles.clear();
                    saveSettings();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Open Recent Project", !recentProjects.empty()))
            {
                for (size_t i = 0; i < recentProjects.size(); ++i)
                {
                    ImGui::PushID((int)i);
                    // Projects are directories — show the folder name.
                    std::string leaf = std::filesystem::path(recentProjects[i]).filename().string();
                    if (leaf.empty())
                        leaf = recentProjects[i];
                    if (ImGui::MenuItem(leaf.c_str()))
                        setProjectRoot(recentProjects[i]);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
                        ImGui::SetTooltip("%s", recentProjects[i].c_str());
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear"))
                {
                    recentProjects.clear();
                    saveSettings();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", SHORTCUT "S", nullptr, isSavable()))
            {
                saveFile();
            }
            if (ImGui::MenuItem("Save As...", SHORTCUT "Shift+S"))
            {
                showSaveFileAs();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close Tab", SHORTCUT "W"))
            {
                if (isDirty())
                    showConfirmClose([this]() { closeTab(activeTab); });
                else
                    closeTab(activeTab);
            }
            if (ImGui::MenuItem("Reopen Closed Tab", SHORTCUT "Shift+T",
                                nullptr, !recentlyClosed.empty()))
            {
                reopenLastClosedTab();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("File History…", " " SHORTCUT "I"))
            {
                showDiff();
            }
            if (ImGui::MenuItem("Diff Against File…"))
            {
                openDiffOtherDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Settings..."))
            {
                // Only reload from disk when opening fresh — re-clicking while the
                // window is already open must not clobber unsaved slider edits.
                if (!settingsVisible)
                {
                    loadSettings();
                    applyKeybindOverridesToEditors();
                }
                settingsVisible = true;
                settingsFocusRequest = true; // un-collapse + focus even if already open
            }
            ImGui::Separator();
            // Path utilities — handy when the current doc lives on disk.
            bool hasPath = !tabs.empty() && doc().filename != "untitled";
            if (ImGui::MenuItem("Open Containing Folder", nullptr, false, hasPath))
            {
                openContainingFolder();
            }
            if (ImGui::MenuItem("Copy File Path", nullptr, false, hasPath))
            {
                ImGui::SetClipboardText(doc().filename.c_str());
            }
            ImGui::Separator();

            // Close the application (respects unsaved-changes confirmation).
            if (ImGui::MenuItem("Exit"))
            {
                tryToQuit();
            }
            ImGui::EndMenu();
        }

        auto &e = doc().editor;

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", " " SHORTCUT "Z", nullptr, e.CanUndo()))
            {
                e.Undo();
            }
#if __APPLE__
            if (ImGui::MenuItem("Redo", "^" SHORTCUT "Z", nullptr, e.CanRedo()))
            {
                e.Redo();
            }
#else
            if (ImGui::MenuItem("Redo", " " SHORTCUT "Y", nullptr, e.CanRedo()))
            {
                e.Redo();
            }
#endif
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", " " SHORTCUT "X", nullptr, e.AnyCursorHasSelection()))
            {
                e.Cut();
            }
            if (ImGui::MenuItem("Copy", " " SHORTCUT "C", nullptr, e.AnyCursorHasSelection()))
            {
                e.Copy();
            }
            if (ImGui::MenuItem("Paste", " " SHORTCUT "V", nullptr, ImGui::GetClipboardText() != nullptr))
            {
                e.Paste();
            }
            ImGui::Separator();
            bool flag;
            flag = e.IsInsertSpacesOnTabs();
            if (ImGui::MenuItem("Insert Spaces on Tabs", nullptr, &flag))
            {
                e.SetInsertSpacesOnTabs(flag);
            }
            if (ImGui::MenuItem("Tabs To Spaces"))
            {
                e.TabsToSpaces();
            }
            if (ImGui::MenuItem("Spaces To Tabs", nullptr, nullptr, !e.IsInsertSpacesOnTabs()))
            {
                e.SpacesToTabs();
            }
            if (ImGui::MenuItem("Strip Trailing Whitespaces"))
            {
                e.StripTrailingWhitespaces();
            }
            ImGui::Separator();
            flag = e.IsFoldingEnabled();
            if (ImGui::MenuItem("Enable Folding", nullptr, &flag))
            {
                e.SetFoldingEnabled(flag);
            }
            if (ImGui::MenuItem("Fold All", " " SHORTCUT "0", nullptr, e.IsFoldingEnabled()))
            {
                e.FoldAll();
            }
            if (ImGui::MenuItem("Unfold All", " " SHORTCUT "J", nullptr, e.IsFoldingEnabled()))
            {
                e.UnfoldAll();
            }
            if (ImGui::MenuItem("Fold Current", " " SHORTCUT "Shift+[", nullptr, e.IsFoldingEnabled()))
            {
                e.FoldCurrent();
            }
            if (ImGui::MenuItem("Unfold Current", " " SHORTCUT "Shift+]", nullptr, e.IsFoldingEnabled()))
            {
                e.UnfoldCurrent();
            }
            ImGui::Separator();
            flag = e.IsCompletingPairedGlyphs();
            if (ImGui::MenuItem("Auto-complete Brackets", nullptr, &flag))
            {
                e.SetCompletePairedGlyphs(flag);
                prefCompletePairs = flag;
            }
            flag = e.IsAutoIndentEnabled();
            if (ImGui::MenuItem("Auto-indent", nullptr, &flag))
            {
                e.SetAutoIndentEnabled(flag);
                prefAutoIndent = flag;
            }
            if (ImGui::MenuItem("Autocomplete (Trie)", nullptr, &autocomplete))
            {
                setAutocompleteMode(autocomplete);
            }

            if (ImGui::MenuItem("Format Document", "Alt+Shift+F"))
            {
                formatActiveDocument();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Navigate Back", "Ctrl+Alt+Left", false, navHistory.canBack()))
            {
                navigateBack();
            }
            if (ImGui::MenuItem("Navigate Forward", "Ctrl+Alt+Right", false, navHistory.canForward()))
            {
                navigateForward();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Selection"))
        {
            if (ImGui::MenuItem("Select All", " " SHORTCUT "A", nullptr, !e.IsEmpty()))
            {
                e.SelectAll();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Indent Line(s)", " " SHORTCUT "]", nullptr, !e.IsEmpty()))
            {
                e.IndentLines();
            }
            if (ImGui::MenuItem("Deindent Line(s)", " " SHORTCUT "[", nullptr, !e.IsEmpty()))
            {
                e.DeindentLines();
            }
            if (ImGui::MenuItem("Move Line(s) Up", nullptr, nullptr, !e.IsEmpty()))
            {
                e.MoveUpLines();
            }
            if (ImGui::MenuItem("Move Line(s) Down", nullptr, nullptr, !e.IsEmpty()))
            {
                e.MoveDownLines();
            }
            if (ImGui::MenuItem("Toggle Comments", " " SHORTCUT "/", nullptr, e.HasLanguage()))
            {
                e.ToggleComments();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("To Uppercase", "Ctrl-K Ctrl-U", nullptr, e.AnyCursorHasSelection()))
            {
                e.SelectionToUpperCase();
            }
            if (ImGui::MenuItem("To Lowercase", "Ctrl-K Ctrl-L", nullptr, e.AnyCursorHasSelection()))
            {
                e.SelectionToLowerCase();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Add Next Occurrence", " " SHORTCUT "D", nullptr, e.CurrentCursorHasSelection()))
            {
                e.AddNextOccurrence();
            }
            if (ImGui::MenuItem("Select All Occurrences", "^" SHORTCUT "D", nullptr, e.CurrentCursorHasSelection()))
            {
                e.SelectAllOccurrences();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Find"))
        {
            if (ImGui::MenuItem("Find", " " SHORTCUT "F"))
            {
                e.OpenFindReplaceWindow();
            }
            if (ImGui::MenuItem("Find Next", "F3", nullptr, e.HasFindString()))
            {
                e.FindNext();
            }
            if (ImGui::MenuItem("Find All", "^" SHORTCUT "G", nullptr, e.HasFindString()))
            {
                e.FindAll();
            }
            if (ImGui::MenuItem("Find in Files…", "^" SHORTCUT "F"))
            {
                openFindInFiles();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Go to Line…", " " SHORTCUT "G"))
            {
                showGotoLine();
            }
            ImGui::EndMenu();
        }

        // VIEW — appearance toggles.
        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Command Palette...", "Ctrl+Shift+P"))
            {
                openCommandPalette();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Zoom In", " " SHORTCUT "+"))
            {
                increaseFontSIze();
            }
            if (ImGui::MenuItem("Zoom Out", " " SHORTCUT "-"))
            {
                decreaseFontSIze();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Notes", nullptr, notesVisible))
                notesVisible = !notesVisible;
            ImGui::Separator();
            // With Unreal Live Coding owning F11, don't advertise a shortcut that
            // deliberately does nothing — the menu item still works.
            if (ImGui::MenuItem("Focus Mode", liveCodingOwnsF11() ? "" : "F11", focusMode))
            {
            	toggleFocusMode();
            }
            if (ImGui::BeginMenu("Theme"))
            {
                for (int i = 0; i < themeCount(); ++i)
                {
                    if (ImGui::MenuItem(themeName(i), nullptr, prefTheme == i))
                    {
                        prefTheme = i;
                        applyTheme(i);
                        saveSettings();
                    }
                }
                ImGui::Separator();
                ImGui::MenuItem("Style Editor...", nullptr, &showStyleEditor);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            bool flag;
            flag = e.IsShowLineNumbersEnabled();
            if (ImGui::MenuItem("Show Line Numbers", nullptr, &flag))
            {
                e.SetShowLineNumbersEnabled(flag);
            }
            flag = e.IsShowWhitespacesEnabled();
            if (ImGui::MenuItem("Show Whitespaces", nullptr, &flag))
            {
                e.SetShowWhitespacesEnabled(flag);
            }
            flag = e.IsShowSpacesEnabled();
            if (ImGui::MenuItem("Show Spaces", nullptr, &flag))
            {
                e.SetShowSpacesEnabled(flag);
            }
            flag = e.IsShowTabsEnabled();
            if (ImGui::MenuItem("Show Tabs", nullptr, &flag))
            {
                e.SetShowTabsEnabled(flag);
            }
            flag = e.IsShowingMatchingBrackets();
            if (ImGui::MenuItem("Show Matching Brackets", nullptr, &flag))
            {
                e.SetShowMatchingBrackets(flag);
            }
            flag = e.IsShowPanScrollIndicatorEnabled();
            if (ImGui::MenuItem("Show Pan/Scroll Indicator", nullptr, &flag))
            {
                e.SetShowPanScrollIndicatorEnabled(flag);
            }
            flag = e.IsMiddleMousePanMode();
            if (ImGui::MenuItem("Middle Mouse Pan", nullptr, &flag))
            {
                if (flag)
                    e.SetMiddleMousePanMode();
                else
                    e.SetMiddleMouseScrollMode();
            }
            if (ImGui::MenuItem("Word Wrap", nullptr, &prefWordWrap))
            {
                for (auto &up : tabs)
                    up->editor.SetWordWrap(prefWordWrap);
            }
            ImGui::Separator();
           	if (ImGui::MenuItem("Navigation Panel", nullptr, &navPanelVisible))
            {
            }

        	// Markdown preview — context-sensitive: only when the active document
            // is a markdown file (not an always-on global toggle).
            {
                auto ext = std::filesystem::path(doc().filename).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (isMarkdownExt(ext))
                {
                    if (ImGui::MenuItem("Preview Markdown", nullptr, &mdPreviewVisible))
                    {
                    }
                }
            }
            // "Switch Header/Source" only really applies to C/C++ files — hide
            // it for other languages so the menu stays focused. Render it ABOVE
            // Exit so Exit is always the last item in the menu.
            bool isCxxDoc = false;
            if (!tabs.empty() && doc().filename != "untitled")
            {
                auto *lang = doc().editor.GetLanguage();
                if (lang)
                {
                    const std::string &n = lang->name;
                    isCxxDoc = (n == "C" || n == "C++");
                }
            }
            if (isCxxDoc)
            {
                if (ImGui::MenuItem("Switch Header/Source", "Alt+O"))
                {
                    toggleHeaderSource();
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Split Right", SHORTCUT "\\"))
            {
                splitActiveTabRight();
            }

            if (ImGui::MenuItem("Pop Out Left", "Ctrl+Alt+←"))
            {
                popOutActiveDoc(-1);
            }
            if (ImGui::MenuItem("Pop Out Right", "Ctrl+Alt+→"))
            {
                popOutActiveDoc(+1);
            }
            if (ImGui::MenuItem("Merge Window Back", "Ctrl+Alt+M"))
            {
                remergeActiveWindow();
            }
            if (ImGui::MenuItem("Merge All Windows", "Ctrl+Alt+Shift+M"))
            {
                remergeAllWindows();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Layout Now"))
            {
                if (auto *fn = ImGui::GetIO().IniFilename)
                    ImGui::SaveIniSettingsToDisk(fn);
            }
            if (ImGui::MenuItem("Reset Layout"))
            {
                // Defer the rebuild to just before the next DockSpace() in the host
                // window's context. Doing it here (menu popup, and AFTER this frame's
                // DockSpace) plus the old LoadIniSettingsFromMemory("") clear made the
                // empty-settings reload discard the DockBuilder rebuild next frame —
                // every window floated and stayed floated. The deferred path matches
                // the working first-run navInitDockLayout timing exactly.
                wantResetLayout = true;
            }

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools"))
        {
            if (ImGui::MenuItem("Developer Tools", nullptr, &devToolsVisible))
            {
            }
            if (ImGui::MenuItem("External Changes", nullptr, &externalChangesVisible))
            {
            }
            // Plugins contribute their tool-panel toggles here (Blueprint, UEVR Live).
            pluginRegistry.menu(*this, PluginMenu::Tools);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Git"))
        {
            bool inRepo;
            {
                std::lock_guard<std::mutex> lk(gitInfo->mutex);
                inRepo = !gitInfo->branch.empty();
            }
            if (!inRepo)
                ImGui::TextDisabled("(not a git repository)");

            if (ImGui::MenuItem("Fetch", nullptr, false, inRepo))
            {
                runGit("fetch");
            }
            if (ImGui::MenuItem("Pull", nullptr, false, inRepo))
            {
                runGit("pull");
            }
            // Switch Branch — lists local branches (current is checked); switching
            // shells out to `git checkout`. A dirty tree makes git refuse the
            // switch (output shows why) rather than us clobbering changes.
            {
                std::string cur;
                std::vector<std::string> branches;
                {
                    std::lock_guard<std::mutex> lk(gitInfo->mutex);
                    cur = gitInfo->branch;
                    branches = gitInfo->branches;
                }
                if (ImGui::BeginMenu("Switch Branch", inRepo && !branches.empty()))
                {
                    for (const auto &b : branches)
                    {
                        bool isCur = (b == cur);
                        if (ImGui::MenuItem(b.c_str(), nullptr, isCur) && !isCur)
                            runGit("checkout \"" + b + "\"");
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("New Branch", inRepo))
                {
                    ImGui::TextDisabled("Create + switch to a new branch");
                    ImGui::SetNextItemWidth(200.0f);
                    bool go = ImGui::InputTextWithHint("##newbranch", "branch name", gitNewBranchBuf,
                                                       sizeof(gitNewBranchBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("Create") || go) && gitNewBranchBuf[0])
                    {
                        runGit(std::string("checkout -b \"") + gitNewBranchBuf + "\"");
                        gitNewBranchBuf[0] = '\0';
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Commit All…", nullptr, false, inRepo))
            {
                gitCommitMsg[0] = '\0';
                gitCommitRequest = true;
            }
            if (ImGui::MenuItem("Push", nullptr, false, inRepo))
            {
                runGit("push");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("History…", nullptr, &gitHistoryVisible, inRepo))
            {
                if (gitHistoryVisible)
                    refreshGitHistory();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse commits; checkout / reset / revert any of them");
            if (ImGui::MenuItem("Status", nullptr, false, inRepo))
            {
                runGit("status");
            }
            if (ImGui::MenuItem("Open on Web", nullptr, false, inRepo))
            {
                openGitOnWeb();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open the 'origin' remote in your browser");
            if (ImGui::BeginMenu("Submodules", inRepo))
            {
                if (ImGui::MenuItem("Update (init, recursive)"))
                    runGit("submodule update --init --recursive");
                if (ImGui::MenuItem("Sync URLs"))
                    runGit("submodule sync --recursive");
                if (ImGui::MenuItem("Status"))
                    runGit("submodule status --recursive");
                if (ImGui::MenuItem("Pull latest (remote)"))
                    runGit("submodule update --remote --recursive");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Discard All Changes…", nullptr, false, inRepo))
            {
                gitDiscardRequest = true;
            }
            if (ImGui::MenuItem("Compare File with Revision…", nullptr, false, inRepo && !tabs.empty()))
            {
                std::snprintf(gitRevBuf, sizeof(gitRevBuf), "HEAD");
                gitRevCompareRequest = true;
            }

            if (ImGui::MenuItem("Browse GitHub Repo…", nullptr, &ghBrowseVisible))
            {
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse a public GitHub repo's files read-only over the API (no clone)");

            if (ImGui::MenuItem("Clone Repository from URL…"))
            {
                gitCloneUrl[0] = '\0';
                if (gitCloneDir[0] == '\0')
                {
                    // default parent = current project's parent, else cwd
                    std::error_code dec;
                    auto parent = !projectRoot.empty() ? projectRoot.parent_path()
                                                       : std::filesystem::current_path();
                    std::snprintf(gitCloneDir, sizeof(gitCloneDir), "%s", parent.string().c_str());
                    (void)dec;
                }
                gitCloneRequest = true;
            }
            ImGui::EndMenu();
        }
        // PROJECT — build / run / project tooling.
        if (ImGui::BeginMenu("Project"))
        {

            if (ImGui::MenuItem("Symbols", nullptr, &symbolsPanelVisible))
            {
            }

            if (ImGui::MenuItem("IntelliSense / LS", nullptr, &lspEnabled))
            {
                if (lspEnabled)
                    startLspForProject();
                else
                    lspClient.stop();
            }

            if (ImGui::MenuItem("Build Project", "F6"))
            {
                runProjectBuild();
            }
            if (ImGui::MenuItem("Build / Run Targets..."))
            {
                openBuildPicker();
            }
            if (ImGui::MenuItem("Run", "F5"))
            {
                runProjectExeOrScript();
            }
            if (ImGui::MenuItem("Run with Arguments..."))
            {
                runProjectWithArgs();
            }
            if (ImGui::MenuItem("Run Active Document (script)"))
            {
                runScriptForDoc();
            }
            if (ImGui::MenuItem("Output", "F5", &script->visible))
            {
            }
            ImGui::Separator();
            // ── Debugger (DAP) ──
            if (!dbgSessionActive)
            {
                if (ImGui::MenuItem("Start Debugging", nullptr, false, !tabs.empty()))
                    startDebugSession();
            }
            else
            {
                if (ImGui::MenuItem("Stop Debugging", "Shift+F5"))
                    stopDebugSession();
                if (dbgStopped)
                {
                    if (ImGui::MenuItem("Continue", "F5"))
                        dapClient.continueExec(dbgThreadId);
                    if (ImGui::MenuItem("Step Over"))
                        dapClient.next(dbgThreadId);
                    if (ImGui::MenuItem("Step Into"))
                        dapClient.stepIn(dbgThreadId);
                    if (ImGui::MenuItem("Step Out"))
                        dapClient.stepOut(dbgThreadId);
                }
            }
            if (ImGui::MenuItem("Toggle Breakpoint", "F9", false, !tabs.empty()))
                toggleBreakpointAtCursor();
            ImGui::MenuItem("Debug Panel", nullptr, &dbgPanelVisible);
            if (ImGui::BeginMenu("Launch in External Debugger"))
            {
                if (ImGui::MenuItem("raddbg"))
                    debugInRadDbg();
                if (ImGui::MenuItem("Visual Studio"))
                    debugInVisualStudio();
                ImGui::EndMenu();
            }
            // Project-type plugins (e.g. Unreal) contribute their submenu here.
            pluginRegistry.menu(*this, PluginMenu::Project);

            ImGui::Separator();
            if (ImGui::MenuItem("Open Project..."))
            {
                openProjectFolderPicker();
            }
            ImGui::EndMenu();
        }

        // Plugins that own a top-level menu-bar entry (e.g. Unreal) draw it here,
        // between Project and Help — a dedicated menu rather than nesting in Project.
        pluginRegistry.topLevelMenus(*this);

        if (ImGui::BeginMenu("Help"))
        {
            ImGui::MenuItem(("Version  " + std::string(kAppVersion)).c_str(), nullptr, false, false);
            if (ImGui::MenuItem("Take the Tour"))
                startTour();
            if (ImGui::MenuItem("Save Screenshot", "Ctrl-Alt-S"))
                requestScreenshot();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reads back the frame the app itself drew (PNG, to %s).\n"
                                  "Not an OS screen grab: it can't see a GPU swapchain.",
                                  (userConfigDir() / "screenshots").generic_string().c_str());
            if (!lastScreenshotPath.empty() && ImGui::MenuItem("Open Screenshots Folder"))
                navOpenExternally((userConfigDir() / "screenshots").string());
            ImGui::Separator();
            if (ImGui::MenuItem("Check for Updates", nullptr, false, !updateFuture.valid()))
                checkForUpdates(true);
            if (updateAvailable && ImGui::MenuItem("Download Update…"))
                showUpdateDialog = true;
            ImGui::MenuItem("Auto-check (every 12 h)", nullptr, &prefAutoUpdate);
            if (ImGui::BeginMenu("Update Channel"))
            {
                if (ImGui::MenuItem("Stable", nullptr, prefUpdateChannel == 0))
                {
                    prefUpdateChannel = 0;
                    lastUpdateCheckEpoch = 0; // re-check against the new channel soon
                }
                if (ImGui::MenuItem("Nightly", nullptr, prefUpdateChannel == 1))
                {
                    prefUpdateChannel = 1;
                    lastUpdateCheckEpoch = 0;
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Changelog…"))
            {
                // Open the bundled CHANGELOG.md as a document (markdown highlight +
                // preview). Fall back to the GitHub releases page if it isn't found.
                std::error_code cec;
                std::filesystem::path cl = get_module_path().parent_path() / "CHANGELOG.md";
                if (!std::filesystem::exists(cl, cec))
                {
                    auto repo = findSelfRepoRoot();
                    if (!repo.empty())
                        cl = repo / "CHANGELOG.md";
                }
                if (std::filesystem::exists(cl, cec))
                    openFile(cl.string());
                else
                    updater::openUrl("https://github.com/" + std::string(kUpdateOwner) + "/" + kUpdateRepo + "/releases");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About ImGui-IDE"))
                showAboutDialog = true;
            ImGui::EndMenu();
        }

        // Back / forward through the jump history — shown ONLY once there's history
        // to go to (they were awkward as always-present greyed arrows). Ctrl+Alt+
        // Left/Right (Alt+arrow is subword nav now), mouse thumb buttons, Edit menu.
        if (navHistory.canBack() || navHistory.canForward())
        {
            if (navHistory.canBack())
            {
                if (ImGui::ArrowButton("##navback", ImGuiDir_Left))
                    navigateBack();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Back  (Ctrl+Alt+Left / mouse back)");
                ImGui::SameLine(0.0f, 2.0f);
            }
            if (navHistory.canForward())
            {
                if (ImGui::ArrowButton("##navfwd", ImGuiDir_Right))
                    navigateForward();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Forward  (Ctrl+Alt+Right / mouse forward)");
            }
        }

        // Active project name — right-aligned so the workspace is always visible.
        // Folder (or project-file parent) name; full path on hover.
        if (!projectRoot.empty())
        {
            std::string pname = projectRoot.filename().string();
            if (pname.empty())
                pname = projectRoot.string();
            std::string label = "project: " + pname;
            float w = ImGui::CalcTextSize(label.c_str()).x;
            float avail = ImGui::GetContentRegionAvail().x;
            if (avail > w + 16.0f)
                ImGui::SameLine(ImGui::GetCursorPosX() + avail - w - 12.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", label.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", projectRoot.string().c_str());
        }

        ImGui::PopStyleVar(); // WindowPadding pushed after BeginMenuBar
        ImGui::EndMenuBar();
    }

    // Help > About — opened at the host-window level (not inside the menu popup,
    // which would mismatch the popup ID stack).
    if (showAboutDialog)
    {
        ImGui::OpenPopup("About ImGui-IDE");
        showAboutDialog = false;
    }
    if (ImGui::BeginPopupModal("About ImGui-IDE", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("ImGui-IDE  %s", kAppVersion);
        ImGui::TextDisabled("(c) 2026 Logan Brunet  -  MIT");
        ImGui::Separator();
        ImGui::TextDisabled("A lightweight IDE-lite editor.");
        ImGui::TextDisabled("Dear ImGui (docking) + SDL3 + tree-sitter.");
        ImGui::Spacing();
        ImGui::TextDisabled("Editor widget forked from ImGuiColorTextEdit");
        ImGui::TextDisabled("by Johan A. Goossens.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    renderUpdateDialog(); // "Update available" modal (opened by the auto/manual check)

    // View > Style Editor — ImGui's built-in live style editor for fine-tuning the
    // current theme's colours/sizes. Changes are live (not persisted); pick a Theme
    // again to reset. Lets the user customize on top of the chosen palette.
    if (showStyleEditor)
    {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 540.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Style Editor", &showStyleEditor))
            ImGui::ShowStyleEditor();
        ImGui::End();
    }

    // global keyboard shortcuts (work whenever no input wants the keys)
    ImGuiIO &io = ImGui::GetIO();
    // While the Settings keybind capture is listening, swallow ALL app shortcuts so
    // the keys the user presses to set a chord don't also trigger app actions.
    const bool capturingKeybind = keybindCapturing && settingsVisible;
    // F11 toggles focus mode even while a document or input box has keyboard focus
    // (it's not a text key). But ONLY bare F11: Unreal's Live Coding compile is
    // Ctrl+Alt+F11, and this used to ignore modifiers entirely — so triggering a
    // live-coding compile also flipped the editor into Focus Mode. And when a UE
    // project is open with live coding enabled, F11 belongs to Unreal outright.
    if (!capturingKeybind && ImGui::IsKeyPressed(ImGuiKey_F11, false))
    {
        const bool anyMod = io.KeyCtrl || io.KeyAlt || io.KeyShift || io.KeySuper;

        if (anyMod)
        {
            // a chorded F11 is somebody else's hotkey — never ours
        }
        else if (liveCodingOwnsF11())
        {
            if (!liveCodingToastShown)
            {
                liveCodingToastShown = true;
                pushToast("F11 is reserved for Unreal Live Coding \xe2\x80\x94 "
                          "use View \xe2\x96\xb8 Focus Mode (or turn off Settings \xe2\x96\xb8 Live coding)",
                          IM_COL32(240, 200, 90, 255), 4);
            }
        }
        else
        {
            toggleFocusMode();
        }
    }
    if (!capturingKeybind && !appShortcutsSuppressed &&
        (!io.WantCaptureKeyboard || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)))
    {

        // App-level shortcuts dispatched through the rebindable keybind registry.
        // keybindPressed(id, default) consults the user override (if any) for the
        // id, else the default chord, and does an EXACT modifier+key match — so a
        // rebind to a different chord takes effect here, and e.g. Ctrl+G never
        // also fires under Ctrl+Shift+G. The id/default pairs mirror the Settings
        // → Keybinds catalogue. Order: more-specific (Shift) chords first where a
        // plain chord could otherwise swallow them.
        tickKeyChordPending(); // age out / cancel a half-entered two-stroke chord
        // Debugger keys. F9 toggles a breakpoint. Shift+F5 stops a live session.
        // F5 CONTINUES only while paused, otherwise it falls through to Run (F5's
        // existing binding). Stepping is on the Debug panel buttons — F10/F11 stay
        // free (F11 is Focus Mode). Starting a session is explicit (Tools ▸ Debug).
        if (keybindPressed("view.palette", "Ctrl+Shift+P"))
        {
            openCommandPalette();
        }
        else if (!tabs.empty() && ImGui::IsKeyPressed(ImGuiKey_F9, false))
        {
            toggleBreakpointAtCursor();
        }
        else if (dbgSessionActive && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F5, false))
        {
            stopDebugSession();
        }
        else if (dbgStopped && !io.KeyShift && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F5, false))
        {
            dapClient.continueExec(dbgThreadId);
        }
        else if (keybindPressed("view.screenshot", "Ctrl+Alt+S"))
        {
            requestScreenshot();
        }
        else if (keybindPressed("file.reopen", "Ctrl+Shift+T"))
        {
            reopenLastClosedTab();
        }
        else if (keybindPressed("file.saveAs", "Ctrl+Shift+S"))
        {
            showSaveFileAs();
        }
        else if (keybindPressed("file.new", "Ctrl+N"))
        {
            newFile();
        }
        else if (keybindPressed("file.open", "Ctrl+O"))
        {
            openFile();
        }
        else if (keybindPressed("file.close", "Ctrl+W"))
        {
            if (isDirty())
                showConfirmClose([this]() { closeTab(activeTab); });
            else
                closeTab(activeTab);
        }
        else if (keybindPressed("file.save", "Ctrl+S"))
        {
            if (doc().filename == "untitled")
                showSaveFileAs();
            else
                saveFile();
        }
        else if (keybindPressed("file.history", "Ctrl+I"))
        {
            showDiff();
        }
        else if (keybindPressed("find.inFiles", "Ctrl+Shift+F"))
        {
            openFindInFiles();
        }
        else if (keybindPressed("find.goto", "Ctrl+G"))
        {
            showGotoLine();
        }
        else if (keybindPressed("view.splitR", "Ctrl+\\"))
        {
            splitActiveTabRight();
        }
        else if (keybindPressed("view.zoomIn", "Ctrl+="))
        {
            increaseFontSIze();
        }
        else if (keybindPressed("view.zoomOut", "Ctrl+-"))
        {
            decreaseFontSIze();
        }
        else if (keybindPressed("code.hSrc", "Alt+O"))
        {
            toggleHeaderSource();
        }
        else if (keybindPressed("code.format", "Alt+Shift+F"))
        {
            formatActiveDocument();
        }
        else if (keybindPressed("nav.back", "Ctrl+Alt+LeftArrow"))
        {
            navigateBack();
        }
        else if (keybindPressed("nav.forward", "Ctrl+Alt+RightArrow"))
        {
            navigateForward();
        }
        else if (keybindPressed("proj.run", "F5"))
        {
            runProjectExeOrScript();
        }
        else if (keybindPressed("proj.build", "F6"))
        {
            runProjectBuild();
        }
        else
        {
            // Plugin-contributed keybinds — same matcher, same override store
            // ([keybinds] in settings), rebindable in Settings > Keybinds under
            // each plugin's group. Collected fresh so enable/disable applies.
            pluginKeybinds.clear();
            pluginRegistry.keybinds(*this, pluginKeybinds);
            for (auto &pk : pluginKeybinds)
                if (pk.run && keybindPressed(pk.id.c_str(), pk.defaultChord.c_str()))
                {
                    pk.run();
                    break;
                }
        }

        // Mouse thumb buttons: X1 = back, X2 = forward (browser convention).
        if (ImGui::IsMouseClicked(3))
            navigateBack();
        else if (ImGui::IsMouseClicked(4))
            navigateForward();

        // Tab cycling — now individually rebindable. Check the backward (Shift)
        // binding first so the default Ctrl+Shift+Tab isn't swallowed by the
        // forward Ctrl+Tab match.
        if (!tabs.empty() && keybindPressed("view.cyclePrev", "Ctrl+Shift+Tab"))
        {
            activeTab = (activeTab == 0) ? tabs.size() - 1 : activeTab - 1;
            tabs[activeTab]->wantFocus = true;
        }
        else if (!tabs.empty() && keybindPressed("view.cycleNext", "Ctrl+Tab"))
        {
            activeTab = (activeTab + 1) % tabs.size();
            tabs[activeTab]->wantFocus = true;
        }
        // Ctrl+Alt viewport control: pop the active doc out to its own OS window
        // (←/→) or merge windows back in (M = current, Shift+M = all). Fixed
        // chords (multi-action group), not individually rebindable.
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyDown(ImGuiMod_Alt))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
                popOutActiveDoc(-1);
            else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
                popOutActiveDoc(+1);
            else if (ImGui::IsKeyPressed(ImGuiKey_M, false))
            {
                if (ImGui::IsKeyDown(ImGuiMod_Shift))
                    remergeAllWindows();
                else
                    remergeActiveWindow();
            }
        }
        // Ctrl + scroll wheel: nudge editor font size. Toggleable in Settings
        // because some users prefer Ctrl+wheel to scroll their navigation, not
        // rescale text. Skip when an input widget is focused so it doesn't
        // fight with the find/replace text box.
        if (prefCtrlScrollZoom && ImGui::IsKeyDown(ImGuiMod_Ctrl) && !ImGui::IsAnyItemActive())
        {
            float w = ImGui::GetIO().MouseWheel;
            if (w > 0.0f)
            {
                increaseFontSIze();
                prefFontSize = fontSize;
            }
            else if (w < 0.0f)
            {
                decreaseFontSIze();
                prefFontSize = fontSize;
            }
        }
    }
}

//
//  Editor::renderStatusBar
//

// Refresh git branch/dirty/ahead-behind off the UI thread, throttled. Captures
// the shared_ptr by value so the worker outlives the Editor if it quits mid-run.
void Editor::pollGitStatus()
{
    std::string root = projectRoot.empty() ? std::string() : projectRoot.string();
    if (root.empty() && !tabs.empty() && doc().filename != "untitled")
        root = std::filesystem::path(doc().filename).parent_path().string();
    if (root.empty())
        return;

    double now = ImGui::GetTime();
    if (root == gitPollRoot && (now - gitPollTime) < 2.5)
        return; // throttle
    gitPollRoot = root;
    gitPollTime = now;

    auto gi = gitInfo;
    if (gi->building.exchange(true))
        return; // a poll is already running
    std::thread([gi, root]() {
        std::string branch;
        int dirty = 0, ahead = 0, behind = 0;
        std::vector<std::string> branches;
        // Local branch names for the Switch Branch menu (cheap; runs on the poll).
        {
            std::string bcmd = "git -C \"" + root + "\" branch --format=%(refname:short) 2>nul";
            if (FILE *bp = _popen(bcmd.c_str(), "r"))
            {
                char b[1024];
                while (fgets(b, sizeof(b), bp))
                {
                    std::string ln = b;
                    while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r' || ln.back() == ' '))
                        ln.pop_back();
                    if (!ln.empty())
                        branches.push_back(ln);
                }
                _pclose(bp);
            }
        }
        std::string cmd = "git -C \"" + root + "\" status --porcelain=v2 --branch 2>nul";
        if (FILE *p = _popen(cmd.c_str(), "r"))
        {
            char buf[8192];
            std::string out;
            while (fgets(buf, sizeof(buf), p))
                out += buf;
            _pclose(p);
            std::istringstream ss(out);
            std::string line;
            while (std::getline(ss, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("# branch.head ", 0) == 0)
                    branch = line.substr(14);
                else if (line.rfind("# branch.ab ", 0) == 0)
                    std::sscanf(line.c_str() + 12, "+%d -%d", &ahead, &behind);
                else if (!line.empty() && line[0] != '#')
                    ++dirty; // a changed tracked/untracked entry
            }
        }
        {
            std::lock_guard<std::mutex> lk(gi->mutex);
            gi->branch = branch;
            gi->dirty = dirty;
            gi->ahead = ahead;
            gi->behind = behind;
            gi->branches = std::move(branches);
        }
        gi->building = false; })
        .detach();
}

std::string Editor::gitRoot()
{
    if (!projectRoot.empty())
        return projectRoot.string();
    if (!tabs.empty() && doc().filename != "untitled")
        return std::filesystem::path(doc().filename).parent_path().string();
    return {};
}

void Editor::runGit(const std::string &args)
{
    std::string root = gitRoot();
    if (root.empty())
    {
        showError("Git: no project / repository directory.");
        return;
    }
    runCommandInOutputPanel("git " + args, root);
    gitPollTime = -1000.0; // force the status indicator to refresh after the action
}

std::string Editor::gitCapture(const std::string &args) const
{
    std::string root = const_cast<Editor *>(this)->gitRoot();
    if (root.empty())
        return {};

    std::string cmd = "git -C \"" + root + "\" " + args + " 2>nul";
    std::string out;

    if (FILE *p = _popen(cmd.c_str(), "r"))
    {
        char buf[4096];
        while (fgets(buf, sizeof(buf), p))
            out += buf;
        _pclose(p);
    }

    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// ── Git remote → web ─────────────────────────────────────────────────────────

std::string Editor::gitWebUrl() const
{
    return gitRemoteToWebUrl(const_cast<Editor *>(this)->gitCapture("remote get-url origin"));
}

void Editor::openGitOnWeb()
{
    std::string url = gitWebUrl();
    if (url.empty())
    {
        pushToast("Git: no web-browsable 'origin' remote", IM_COL32(230, 160, 90, 255), 3);
        return;
    }
    navOpenExternally(url);
}

// ── Commit history / rollback ────────────────────────────────────────────────

void Editor::refreshGitHistory()
{
    std::string root = gitRoot();
    if (root.empty())
        return;

    std::string scope;
    if (gitHistoryFileScope && !tabs.empty() && doc().filename != "untitled")
        scope = doc().filename;

    {
        std::lock_guard<std::mutex> lk(gitLog->mutex);
        if (gitLog->loading)
            return;
        gitLog->loading = true;
        gitLog->scopeFile = scope;
        gitLog->status = "loading…";
    }

    auto log = gitLog;

    std::thread([log, root, scope]() {
        // A unit-separated format so subjects with spaces/pipes survive parsing.
        std::string cmd = "git -C \"" + root +
                          "\" log -n 200 --pretty=format:\"%H\x1f%h\x1f%an\x1f%ad\x1f%s\" --date=short";
        if (!scope.empty())
            cmd += " -- \"" + scope + "\"";
        cmd += " 2>nul";

        std::vector<GitLogEntry> entries;

        if (FILE *p = _popen(cmd.c_str(), "r"))
        {
            std::string out;
            char buf[8192];
            while (fgets(buf, sizeof(buf), p))
                out += buf;
            _pclose(p);

            std::istringstream ss(out);
            std::string line;
            while (std::getline(ss, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;

                GitLogEntry e;
                std::string *fields[] = {&e.hash, &e.shortHash, &e.author, &e.date, &e.subject};
                size_t start = 0;
                int fi = 0;
                for (; fi < 5; fi++)
                {
                    size_t sep = line.find('\x1f', start);
                    std::string val = line.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
                    *fields[fi] = val;
                    if (sep == std::string::npos)
                        break;
                    start = sep + 1;
                }
                if (!e.hash.empty())
                    entries.push_back(std::move(e));
            }
        }

        std::lock_guard<std::mutex> lk(log->mutex);
        log->entries = std::move(entries);
        log->loading = false;
        log->status = log->entries.empty() ? "no commits" : "";
    }).detach();
}

void Editor::gitCheckoutCommit(const std::string &hash)
{
    // Detached checkout — the user gets a warning modal first (renderGitHistory).
    runGit("checkout " + hash);
    refreshGitHistory();
}

void Editor::gitResetToCommit(const std::string &hash, bool hard)
{
    runGit(std::string("reset ") + (hard ? "--hard " : "--soft ") + hash);
    refreshGitHistory();
}

void Editor::gitRevertCommit(const std::string &hash)
{
    // --no-edit: don't drop the user into an editor we don't control.
    runGit("revert --no-edit " + hash);
    refreshGitHistory();
}

void Editor::renderGitHistory()
{
    if (!gitHistoryVisible)
        return;

    ImGui::SetNextWindowSize(ImVec2(720, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Git History", &gitHistoryVisible))
    {
        ImGui::End();
        return;
    }

    if (gitRoot().empty())
    {
        ImGui::TextDisabled("Not a git repository.");
        ImGui::End();
        return;
    }

    if (ImGui::Button("Refresh"))
        refreshGitHistory();
    ImGui::SameLine();
    if (ImGui::Checkbox("This file only", &gitHistoryFileScope))
    {
        gitHistorySelected = -1;
        refreshGitHistory();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open on Web"))
        openGitOnWeb();

    // First open: kick a load.
    bool needInitialLoad = false;
    {
        std::lock_guard<std::mutex> lk(gitLog->mutex);
        needInitialLoad = gitLog->entries.empty() && !gitLog->loading && gitLog->status.empty();
    }
    if (needInitialLoad)
        refreshGitHistory();

    ImGui::Separator();

    std::vector<GitLogEntry> entries;
    bool loading;
    std::string status;
    {
        std::lock_guard<std::mutex> lk(gitLog->mutex);
        entries = gitLog->entries;
        loading = gitLog->loading;
        status = gitLog->status;
    }

    if (loading)
        ImGui::TextDisabled("Loading history…");
    else if (entries.empty())
        ImGui::TextDisabled("%s", status.empty() ? "No commits." : status.c_str());

    if (ImGui::BeginTable("##gitlog", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Commit", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)entries.size(); i++)
        {
            const auto &e = entries[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(e.shortHash.c_str(), gitHistorySelected == i,
                                  ImGuiSelectableFlags_SpanAllColumns))
                gitHistorySelected = i;

            if (ImGui::BeginPopupContextItem("##commitctx"))
            {
                gitHistorySelected = i;
                if (ImGui::MenuItem("Compare file with this commit", nullptr, false,
                                    !tabs.empty() && doc().filename != "untitled"))
                {
                    std::snprintf(gitRevBuf, sizeof(gitRevBuf), "%s", e.hash.c_str());
                    gitRevCompareRequest = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Checkout (detached)"))
                {
                    gitPendingAction = 1;
                    gitPendingHash = e.hash;
                }
                if (ImGui::MenuItem("Soft reset to here"))
                {
                    gitPendingAction = 2;
                    gitPendingHash = e.hash;
                }
                if (ImGui::MenuItem("Hard reset to here"))
                {
                    gitPendingAction = 3;
                    gitPendingHash = e.hash;
                }
                if (ImGui::MenuItem("Revert this commit"))
                {
                    gitPendingAction = 4;
                    gitPendingHash = e.hash;
                }
                if (ImGui::MenuItem("Copy hash"))
                    ImGui::SetClipboardText(e.hash.c_str());
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(e.subject.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", e.author.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", e.date.c_str());

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // Confirm the destructive rollbacks — a hard reset or a detached checkout on a
    // dirty tree loses work, so name exactly what's about to happen.
    if (gitPendingAction != 0)
        ImGui::OpenPopup("Confirm git action");

    if (ImGuiViewport *vp = ImGui::GetWindowViewport())
    {
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Confirm git action", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const char *what =
            gitPendingAction == 1 ? "Check out this commit (detached HEAD)?\nUncommitted changes may block or be lost."
            : gitPendingAction == 2 ? "Soft reset to this commit?\nHistory after it becomes staged changes (work kept)."
            : gitPendingAction == 3 ? "HARD reset to this commit?\nAll changes after it are DISCARDED. This cannot be undone."
                                    : "Revert this commit?\nA new commit undoing its changes is created.";

        ImGui::TextUnformatted(what);
        ImGui::TextDisabled("%s", gitPendingHash.substr(0, 10).c_str());
        ImGui::Separator();

        const bool destructive = gitPendingAction == 1 || gitPendingAction == 3;
        if (destructive)
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 60, 55, 255));
        if (ImGui::Button(destructive ? "Yes, do it" : "Confirm", ImVec2(120, 0)))
        {
            switch (gitPendingAction)
            {
            case 1: gitCheckoutCommit(gitPendingHash); break;
            case 2: gitResetToCommit(gitPendingHash, false); break;
            case 3: gitResetToCommit(gitPendingHash, true); break;
            case 4: gitRevertCommit(gitPendingHash); break;
            }
            gitPendingAction = 0;
            ImGui::CloseCurrentPopup();
        }
        if (destructive)
            ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            gitPendingAction = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ── GitHub repo browser helpers ─────────────────────────────────────────────
namespace
{
// Minimal, tolerant extraction of the string value for `"key":"..."` starting at
// or after `from`. Handles \" and \\ escapes. Returns "" and leaves `end` = npos
// if not found.
std::string jsonStringField(const std::string &json, const std::string &key, size_t from, size_t &end)
{
    end = std::string::npos;
    std::string needle = "\"" + key + "\":";
    size_t k = json.find(needle, from);
    if (k == std::string::npos)
        return {};
    size_t q1 = json.find('"', k + needle.size());
    if (q1 == std::string::npos)
        return {};
    std::string out;
    for (size_t i = q1 + 1; i < json.size(); ++i)
    {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size())
        {
            char n = json[++i];
            out += (n == 'n') ? '\n' : (n == 't') ? '\t' : n; // \/ \" \\ -> literal
            continue;
        }
        if (c == '"')
        {
            end = i;
            return out;
        }
        out += c;
    }
    return {};
}

// A folder/file node for the GitHub browser's tree view, built from the flat blob
// path list. std::map keeps children name-sorted; folders sort before files at
// render time.
struct GhTreeNode
{
    std::map<std::string, GhTreeNode> dirs;
    std::vector<std::pair<std::string, std::string>> files; // {display name, full path}
};

// Build a directory tree from sorted "a/b/c.h" blob paths.
GhTreeNode buildGhTree(const std::vector<std::string> &paths)
{
    GhTreeNode root;
    for (const auto &p : paths)
    {
        GhTreeNode *cur = &root;
        size_t start = 0;
        while (true)
        {
            size_t slash = p.find('/', start);
            if (slash == std::string::npos)
            {
                cur->files.emplace_back(p.substr(start), p);
                break;
            }
            cur = &cur->dirs[p.substr(start, slash - start)];
            start = slash + 1;
        }
    }
    return root;
}

// Decode standard base64 (skips embedded newlines / non-alphabet chars, stops at
// '='). Used for the GitHub contents API, whose "content" is newline-wrapped base64.
std::string base64Decode(const std::string &in)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rev[256];
    for (int i = 0; i < 256; ++i)
        rev[i] = -1;
    for (int i = 0; i < 64; ++i)
        rev[(unsigned char)T[i]] = i;
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in)
    {
        if (rev[c] < 0)
        {
            if (c == '=')
                break;
            continue; // whitespace / newline
        }
        val = (val << 6) + rev[c];
        bits += 6;
        if (bits >= 0)
        {
            out += char((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

// Pull the blob (file) paths out of a GitHub git/trees response.
std::vector<std::string> parseTreeBlobs(const std::string &json)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (true)
    {
        size_t pe = std::string::npos;
        std::string path = jsonStringField(json, "path", i, pe);
        if (pe == std::string::npos)
            break;
        size_t te = std::string::npos;
        std::string type = jsonStringField(json, "type", pe, te);
        if (type == "blob")
            out.push_back(path);
        i = (te == std::string::npos) ? pe + 1 : te + 1;
        if (out.size() > 20000)
            break; // safety cap for huge repos
    }
    return out;
}
} // namespace

void Editor::fetchGithubTree(const std::string &owner, const std::string &repo, const std::string &ref,
                             const std::string &token)
{
    auto gb = ghBrowse;
    if (gb->loading.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lk(gb->mutex);
        gb->status = "Fetching " + owner + "/" + repo + " \xe2\x80\xa6";
        gb->files.clear();
    }
    std::thread([gb, owner, repo, ref, token]() {
        std::string useRef = ref;
        std::string err, body;
        int status = 0;
        // Resolve the default branch when none was given.
        if (useRef.empty())
        {
            if (updater::apiGet("https://api.github.com/repos/" + owner + "/" + repo, status, body, err, token) &&
                status == 200)
            {
                size_t e = std::string::npos;
                std::string def = jsonStringField(body, "default_branch", 0, e);
                useRef = def.empty() ? "main" : def;
            }
            else
                useRef = "main";
        }

        std::string treeUrl = "https://api.github.com/repos/" + owner + "/" + repo +
                              "/git/trees/" + useRef + "?recursive=1";
        std::vector<std::string> files;
        std::string statusLine;
        if (updater::apiGet(treeUrl, status, body, err, token) && status == 200)
        {
            files = parseTreeBlobs(body);
            statusLine = std::to_string(files.size()) + " files in " + owner + "/" + repo + "@" + useRef;
            if (body.find("\"truncated\":true") != std::string::npos)
                statusLine += " (truncated by GitHub)";
        }
        else if (status == 403)
            statusLine = "GitHub rate limit hit (unauthenticated). Try again later.";
        else if (status == 404)
            statusLine = "Not found: " + owner + "/" + repo + "@" + useRef;
        else
            statusLine = "Fetch failed: " + (err.empty() ? ("HTTP " + std::to_string(status)) : err);

        std::sort(files.begin(), files.end());
        {
            std::lock_guard<std::mutex> lk(gb->mutex);
            gb->owner = owner;
            gb->repo = repo;
            gb->ref = useRef;
            gb->files = std::move(files);
            gb->status = statusLine;
        }
        gb->loading = false; })
        .detach();
}

void Editor::fetchGithubFile(const std::string &owner, const std::string &repo, const std::string &ref,
                             const std::string &path, const std::string &token)
{
    auto gb = ghBrowse;
    std::filesystem::path cacheRoot = userConfigDir() / "github" / owner / repo;
    std::thread([gb, owner, repo, ref, path, token, cacheRoot]() {
        std::string err, body, content;
        int status = 0;
        if (token.empty())
        {
            // Public: the raw endpoint returns the file bytes directly.
            std::string rawUrl = "https://raw.githubusercontent.com/" + owner + "/" + repo + "/" + ref + "/" + path;
            if (updater::apiGet(rawUrl, status, body, err) && status == 200)
                content = body;
        }
        else
        {
            // Private / authenticated: the contents API returns base64 (raw.github
            // doesn't honor PATs). Extract + decode the "content" field.
            std::string api = "https://api.github.com/repos/" + owner + "/" + repo +
                              "/contents/" + path + "?ref=" + ref;
            if (updater::apiGet(api, status, body, err, token) && status == 200)
            {
                size_t e = std::string::npos;
                content = base64Decode(jsonStringField(body, "content", 0, e));
            }
        }
        if (status != 200)
        {
            std::lock_guard<std::mutex> lk(gb->mutex);
            gb->status = "Open failed (" + path + "): " + (err.empty() ? "HTTP " + std::to_string(status) : err);
            return;
        }
        // Stage to a local temp path mirroring the repo layout, then let the UI open
        // it read-only (tab creation must happen on the UI thread).
        std::error_code ec;
        std::filesystem::path dest = cacheRoot / path;
        std::filesystem::create_directories(dest.parent_path(), ec);
        {
            std::ofstream f(dest, std::ios::binary | std::ios::trunc);
            f << content;
        }
        {
            std::lock_guard<std::mutex> lk(gb->mutex);
            gb->pendingOpenPath = dest.string();
            gb->status = "Opened " + path;
        }
        gb->pendingOpen = true; })
        .detach();
}

void Editor::pollGithubBrowser()
{
    if (!ghBrowse->pendingOpen.exchange(false))
        return;
    std::string p;
    {
        std::lock_guard<std::mutex> lk(ghBrowse->mutex);
        p = ghBrowse->pendingOpenPath;
    }
    if (p.empty())
        return;
    openFile(p);
    if (!tabs.empty())
        doc().editor.SetReadOnlyEnabled(true); // remote content: view-only
}

void Editor::renderGithubBrowser()
{
    if (!ghBrowseVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GitHub Repo Browser", &ghBrowseVisible))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Browse a GitHub repo read-only (no clone). Token optional.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool go = ImGui::InputTextWithHint("##ghrepo", "owner/repo", ghOwnerRepoBuf, sizeof(ghOwnerRepoBuf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##ghref", "branch (blank=default)", ghRefBuf, sizeof(ghRefBuf));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ghtok", "PAT (private repos / rate limit; not saved)", ghToken, sizeof(ghToken),
                             ImGuiInputTextFlags_Password);
    bool loading = ghBrowse->loading.load();
    if ((ImGui::Button(loading ? "Fetching\xe2\x80\xa6" : "Fetch") || go) && !loading)
    {
        std::string ownerRepo = ghOwnerRepoBuf;
        auto slash = ownerRepo.find('/');
        if (slash != std::string::npos && slash > 0 && slash + 1 < ownerRepo.size())
            fetchGithubTree(ownerRepo.substr(0, slash), ownerRepo.substr(slash + 1), ghRefBuf, ghToken);
        else
        {
            std::lock_guard<std::mutex> lk(ghBrowse->mutex);
            ghBrowse->status = "Enter owner/repo (e.g. ocornut/imgui)";
        }
    }

    std::string status, owner, repo, ref;
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lk(ghBrowse->mutex);
        status = ghBrowse->status;
        files = ghBrowse->files;
        owner = ghBrowse->owner;
        repo = ghBrowse->repo;
        ref = ghBrowse->ref;
    }
    if (!status.empty())
        ImGui::TextWrapped("%s", status.c_str());
    ImGui::Separator();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ghfilter", "filter files", ghFileFilter, sizeof(ghFileFilter));

    std::string needle = ghFileFilter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    ImGui::BeginChild("##ghfiles", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (!needle.empty())
    {
        // Filtered: a flat matching list (a tree of scattered matches reads poorly).
        int shown = 0;
        for (const auto &f : files)
        {
            std::string low = f;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (low.find(needle) == std::string::npos)
                continue;
            if (shown++ >= 2000)
            {
                ImGui::TextDisabled("(more — refine the filter)");
                break;
            }
            if (ImGui::Selectable(f.c_str()))
                fetchGithubFile(owner, repo, ref, f, ghToken);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open %s read-only", f.c_str());
        }
    }
    else
    {
        // Unfiltered: a collapsible folder tree (rebuilt only when the repo changes).
        static std::string cacheSig;
        static GhTreeNode cacheTree;
        std::string sig = owner + "/" + repo + "@" + ref + "#" + std::to_string(files.size());
        if (sig != cacheSig)
        {
            cacheTree = buildGhTree(files);
            cacheSig = sig;
        }
        std::function<void(const GhTreeNode &)> draw = [&](const GhTreeNode &node) {
            for (const auto &d : node.dirs)
            {
                if (ImGui::TreeNodeEx(d.first.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    draw(d.second);
                    ImGui::TreePop();
                }
            }
            for (const auto &f : node.files)
            {
                ImGui::TreeNodeEx(f.first.c_str(),
                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                      ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Bullet);
                if (ImGui::IsItemClicked())
                    fetchGithubFile(owner, repo, ref, f.second, ghToken);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Open %s read-only", f.second.c_str());
            }
        };
        draw(cacheTree);
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::cloneRepository(const std::string &url, const std::string &parentDir)
{
    if (url.empty() || url.find('"') != std::string::npos)
    {
        showError("Clone: enter a repository URL (without quotes).");
        return;
    }
    std::error_code ec;
    std::filesystem::path parent = parentDir.empty() ? std::filesystem::current_path()
                                                     : std::filesystem::path(parentDir);
    std::filesystem::create_directories(parent, ec);
    // repo name = last URL segment minus a trailing .git
    std::string name = url;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0)
        name.resize(name.size() - 4);
    if (name.empty())
        name = "repo";
    auto dest = parent / name;
    runCommandInOutputPanel("git clone \"" + url + "\" \"" + dest.string() + "\"", parent.string());
    // Root the nav at the destination; it auto-populates as files land (cache TTL).
    setProjectRoot(dest);
    pushToast("Cloning into " + dest.string() + " \xe2\x80\xa6", IM_COL32(150, 160, 255, 255), 2);
    pendingCloneRoot = dest.string(); // pollCloneCompletion() rebuilds the index once .git lands
    pendingCloneSince = ImGui::GetTime();
    gitPollTime = -1000.0;
}

void Editor::pollCloneCompletion()
{
    if (pendingCloneRoot.empty())
        return;
    static double next = 0.0;
    double now = ImGui::GetTime();
    if (now < next)
        return;
    next = now + 1.0; // check at ~1 Hz while a clone is outstanding
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(pendingCloneRoot) / ".git", ec))
    {
        rebuildProjectIndex(); // repo landed → populate symbols / go-to-def
        gitPollTime = -1000.0; // refresh the branch indicator
        pushToast("Clone complete \xe2\x80\x94 indexing", IM_COL32(120, 200, 120, 255), 2);
        pendingCloneRoot.clear();
    }
    else if (now - pendingCloneSince > 600.0) // give up after 10 min (clone failed / cancelled)
    {
        pendingCloneRoot.clear();
    }
}

void Editor::compareActiveFileWithRevision(const std::string &rev)
{
    if (tabs.empty())
        return;
    auto &t = doc();
    std::string root = gitRoot();
    if (root.empty())
    {
        showError("Not a git repository.");
        return;
    }
    if (t.filename.empty() || t.filename == "untitled")
    {
        showError("Save the file before comparing against a revision.");
        return;
    }
    std::error_code ec;
    auto rel = std::filesystem::relative(t.filename, root, ec);
    if (ec || rel.empty())
    {
        showError("Active file is outside the repository.");
        return;
    }
    std::string relg = rel.generic_string(); // git wants forward slashes
    std::string r = rev.empty() ? std::string("HEAD") : rev;
    if (r.find('"') != std::string::npos)
    {
        showError("Revision can't contain a double-quote."); // would break the shell command
        return;
    }
#ifdef _WIN32
    std::string cmd = "git -C \"" + root + "\" show \"" + r + ":" + relg + "\" 2>NUL";
    FILE *p = _popen(cmd.c_str(), "rb");
#else
    std::string cmd = "git -C \"" + root + "\" show \"" + r + ":" + relg + "\" 2>/dev/null";
    FILE *p = _popen(cmd.c_str(), "r");
#endif
    std::string content;
    if (p)
    {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
            content.append(buf, n);
        _pclose(p);
    }
    if (content.empty())
    {
        showError("git show " + r + ":" + relg + " returned nothing (new file, or unknown revision).");
        return;
    }
    t.diff.SetLanguage(t.editor.GetLanguage());
    t.diff.SetText(content, t.editor.GetText()); // left = revision, right = current buffer
    dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    state = State::diff;
}

std::filesystem::path Editor::findSelfRepoRoot() const
{
    std::error_code ec;
    std::filesystem::path starts[] = {get_module_path().parent_path(),
                                      std::filesystem::current_path()};
    for (auto start : starts)
    {
        for (auto d = start; !d.empty(); d = d.parent_path())
        {
            if (std::filesystem::exists(d / "example" / "editor.cpp", ec))
                return d;
            if (d == d.parent_path())
                break;
        }
    }
    return {};
}

// Commit-message + discard-confirm modals (rendered at render() top level so they
// pop out properly; anchored to the main viewport like the other dialogs).
void Editor::renderGitDialogs()
{
    if (gitCommitRequest)
    {
        ImGui::OpenPopup("Git Commit");
        gitCommitRequest = false;
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Git Commit", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("Stages all changes (git add -A) then commits.");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##gitmsg", gitCommitMsg, sizeof(gitCommitMsg), ImVec2(420.0f, 110.0f));
        ImGui::Separator();
        bool hasMsg = gitCommitMsg[0] != '\0';
        if (!hasMsg)
            ImGui::BeginDisabled();
        if (ImGui::Button("Commit", ImVec2(90.0f, 0.0f)))
        {
            std::string root = gitRoot();
            std::error_code ec;
            auto tmp = userConfigDir() / "git_commit_msg.txt";
            std::filesystem::create_directories(tmp.parent_path(), ec);
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f << gitCommitMsg;
            }
            runCommandInOutputPanel("git add -A && git commit -F \"" + tmp.string() + "\"", root);
            gitPollTime = -1000.0;
            ImGui::CloseCurrentPopup();
        }
        if (!hasMsg)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (gitDiscardRequest)
    {
        ImGui::OpenPopup("Discard Changes");
        gitDiscardRequest = false;
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Discard Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Discard ALL tracked changes in the working tree?");
        ImGui::TextDisabled("Runs git checkout -- .  (untracked files are kept).");
        ImGui::Separator();
        if (ImGui::Button("Discard", ImVec2(90.0f, 0.0f)))
        {
            runGit("checkout -- .");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Clone a repository from a URL, then open it as the project.
    if (gitCloneRequest)
    {
        ImGui::OpenPopup("Clone Repository");
        gitCloneRequest = false;
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Clone Repository", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("Runs git clone, then opens the result as the project.");
        ImGui::SetNextItemWidth(440.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##cloneurl", "https://github.com/user/repo.git", gitCloneUrl, sizeof(gitCloneUrl));
        ImGui::SetNextItemWidth(440.0f);
        ImGui::InputTextWithHint("##clonedir", "parent directory to clone into", gitCloneDir, sizeof(gitCloneDir));
        ImGui::Separator();
        bool hasUrl = gitCloneUrl[0] != '\0';
        if (!hasUrl)
            ImGui::BeginDisabled();
        if (ImGui::Button("Clone", ImVec2(90.0f, 0.0f)))
        {
            cloneRepository(gitCloneUrl, gitCloneDir);
            ImGui::CloseCurrentPopup();
        }
        if (!hasUrl)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Compare the active file against a git revision (git show <rev>:<path>).
    if (gitRevCompareRequest)
    {
        ImGui::OpenPopup("Compare with Revision");
        gitRevCompareRequest = false;
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Compare with Revision", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("Diffs the active file against a revision (HEAD, a tag,\nbranch, or commit sha).");
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool go = ImGui::InputText("revision", gitRevBuf, sizeof(gitRevBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
        if (ImGui::Button("Compare", ImVec2(90.0f, 0.0f)) || go)
        {
            compareActiveFileWithRevision(gitRevBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ── Themes ───────────────────────────────────────────────────────────────────
static const char *kThemeNames[] = {
    "ImGui-IDE Dark",
    "Midnight",
    "Light",
    "High Contrast",
    "Classic Dark",
};
int Editor::themeCount()
{
    return (int)(sizeof(kThemeNames) / sizeof(kThemeNames[0]));
}
const char *Editor::themeName(int i)
{
    return (i >= 0 && i < themeCount()) ? kThemeNames[i] : "?";
}

// Apply a built-in theme: shared rounding/borders + a per-theme palette. Does NOT
// touch structural prefs (window padding, scrollbar size) so those persist across
// theme switches. The ImGui style editor (View > Style Editor) can override live.
void Editor::applyTheme(int index)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBarBorderSize = 1.0f;

    ImVec4 *c = s.Colors;
    switch (index)
    {
    case 4: // Classic Dark — stock ImGui, no recolor.
        ImGui::StyleColorsDark();
        return;

    case 3: // High Contrast — near-black with bright amber/cyan accents.
        ImGui::StyleColorsDark();
        c[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.04f, 0.05f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
        c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.06f, 0.98f);
        c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.14f, 0.06f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.85f, 0.55f, 0.10f, 0.55f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.95f, 0.65f, 0.15f, 0.80f);
        c[ImGuiCol_HeaderActive] = ImVec4(1.00f, 0.72f, 0.20f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.65f, 0.15f, 0.70f);
        c[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.72f, 0.20f, 1.00f);
        c[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.95f, 0.95f, 1.00f);
        c[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.24f, 0.06f, 1.00f);
        c[ImGuiCol_Text] = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
        return;

    case 2: // Light.
        ImGui::StyleColorsLight();
        c[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.40f, 0.55f, 0.85f, 0.55f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.55f, 0.85f, 0.75f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.50f, 0.80f, 1.00f);
        c[ImGuiCol_TabSelected] = ImVec4(0.62f, 0.72f, 0.92f, 1.00f);
        c[ImGuiCol_TabHovered] = ImVec4(0.55f, 0.66f, 0.90f, 1.00f);
        return;

    case 1: // Midnight — deep blue-black, cyan accents.
        ImGui::StyleColorsDark();
        c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.12f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.09f, 0.14f, 1.00f);
        c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.07f, 0.11f, 0.98f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.13f, 0.22f, 1.00f);
        c[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.11f, 0.17f, 1.00f);
        c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.21f, 1.00f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.19f, 0.28f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.16f, 0.30f, 0.45f, 1.00f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.40f, 0.60f, 1.00f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.48f, 0.72f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.34f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.34f, 0.52f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.46f, 0.70f, 1.00f);
        c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.80f, 0.95f, 1.00f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.65f, 0.90f, 1.00f);
        c[ImGuiCol_TabSelected] = ImVec4(0.14f, 0.26f, 0.40f, 1.00f);
        c[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.40f, 0.60f, 1.00f);
        c[ImGuiCol_DockingPreview] = ImVec4(0.25f, 0.55f, 0.85f, 0.70f);
        return;

    case 0: // ImGui-IDE Dark (default) — cool gray.
    default:
        ImGui::StyleColorsDark();
        c[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.115f, 0.130f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.120f, 0.130f, 0.145f, 1.00f);
        c[ImGuiCol_PopupBg] = ImVec4(0.095f, 0.105f, 0.120f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(0.220f, 0.240f, 0.275f, 0.55f);
        c[ImGuiCol_FrameBg] = ImVec4(0.160f, 0.175f, 0.200f, 1.00f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.215f, 0.235f, 0.270f, 1.00f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.255f, 0.285f, 0.330f, 1.00f);
        c[ImGuiCol_TitleBg] = ImVec4(0.085f, 0.095f, 0.110f, 1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.130f, 0.150f, 0.185f, 1.00f);
        c[ImGuiCol_MenuBarBg] = ImVec4(0.130f, 0.140f, 0.160f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.200f, 0.245f, 0.310f, 1.00f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.255f, 0.315f, 0.400f, 1.00f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.300f, 0.380f, 0.480f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.200f, 0.230f, 0.285f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.280f, 0.340f, 0.425f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.340f, 0.420f, 0.525f, 1.00f);
        c[ImGuiCol_Separator] = ImVec4(0.250f, 0.270f, 0.305f, 1.00f);
        c[ImGuiCol_Tab] = ImVec4(0.130f, 0.150f, 0.180f, 1.00f);
        c[ImGuiCol_TabHovered] = ImVec4(0.260f, 0.320f, 0.400f, 1.00f);
        c[ImGuiCol_TabSelected] = ImVec4(0.205f, 0.260f, 0.340f, 1.00f);
        c[ImGuiCol_TabDimmed] = ImVec4(0.110f, 0.120f, 0.135f, 1.00f);
        c[ImGuiCol_TabDimmedSelected] = ImVec4(0.160f, 0.190f, 0.240f, 1.00f);
        c[ImGuiCol_CheckMark] = ImVec4(0.450f, 0.620f, 0.900f, 1.00f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.400f, 0.550f, 0.800f, 1.00f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.480f, 0.640f, 0.900f, 1.00f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.090f, 0.100f, 0.115f, 0.60f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.275f, 0.300f, 0.340f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.340f, 0.370f, 0.420f, 1.00f);
        c[ImGuiCol_DockingPreview] = ImVec4(0.300f, 0.420f, 0.620f, 0.70f);
        return;
    }
}

void Editor::renderStatusBar()
{
    // Built-in language list + every runtime-defined language discovered at
    // startup, de-duplicated by name. Cached after first build.
    static std::vector<std::string> langNamesV;
    static std::vector<const TextEditor::Language *> langDefsV;
    if (langNamesV.empty())
    {
        langNamesV = {"None", "C", "C++", "C#", "AngelScript", "Lua", "Python", "GLSL", "HLSL", "JSON", "Markdown", "SQL", "INI"};
        langDefsV = {
            nullptr,
            TextEditor::Language::C(),
            TextEditor::Language::Cpp(),
            TextEditor::Language::Cs(),
            TextEditor::Language::AngelScript(),
            TextEditor::Language::Lua(),
            TextEditor::Language::Python(),
            TextEditor::Language::Glsl(),
            TextEditor::Language::Hlsl(),
            TextEditor::Language::Json(),
            TextEditor::Language::Markdown(),
            TextEditor::Language::Sql(),
            TextEditor::Language::Ini()};
        // Append runtime languages (HTML, INI, YAML, CFG, BAT, PS1, CMake, XML, XAML…)
        std::unordered_set<std::string> seen(langNamesV.begin(), langNamesV.end());
        for (auto &[ext, lang] : runtimeLanguagesByExt())
        {
            (void)ext;
            if (!lang || seen.count(lang->name))
                continue;
            seen.insert(lang->name);
            langNamesV.push_back(lang->name);
            langDefsV.push_back(lang);
        }
    }

    auto &t = doc();
    auto &e = t.editor;
    std::string langName = e.GetLanguageName();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::BeginChild("StatusBar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    // Capture the line's start X and full width BEFORE any item so the right
    // cluster can be aligned to the true right edge. (Reading the remaining avail
    // later — after the left cluster — yields a width short by the left cluster,
    // which is what made the trailing status sit mid-bar instead of flush right.)
    const float statusStartX = ImGui::GetCursorPosX();
    const float statusFullW = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(120.0f);

    if (ImGui::BeginCombo("##LangSel", langName.c_str()))
    {
        for (size_t n = 0; n < langNamesV.size(); n++)
        {
            bool selected = (langName == langNamesV[n]);
            if (ImGui::Selectable(langNamesV[n].c_str(), selected))
            {
                e.SetLanguage(langDefsV[n]);
                buildAutocompleteTrie(t);
                // Persist the extension -> language association so this and future
                // files of the same type reopen with the chosen language.
                auto pext = std::filesystem::path(t.filename).extension().string();
                std::transform(pext.begin(), pext.end(), pext.begin(),
                               [](unsigned char c) { return (char) std::tolower(c); });
                if (!pext.empty() && t.filename != "untitled")
                {
                    extLanguageOverrides()[pext] = langNamesV[n];
                    saveSettings();
                }
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::AlignTextToFramePadding();

    // Toolchain selector — only when a project is loaded AND the active doc
    // language is one we recognize a toolchain for. Renders as a combo so
    // the user can switch active MSVC / .NET inline without leaving the
    // editor. Selection persists via saveSettings on quit.
    const auto *langPtr = e.GetLanguage();
    if (langPtr && !projectRoot.empty())
    {
        const std::string &ln = langPtr->name;
        if (ln == "C" || ln == "C++")
        {
            detectToolchains();
            std::string label = "MSVC: ";
            if (activeMsvcPath.empty())
                label += "(none)";
            else
            {
                // Cache the extracted version segment — only re-walk the path when
                // activeMsvcPath changes, not every frame a C/C++ file is active.
                static std::string verCache, verCacheFor;
                if (verCacheFor != activeMsvcPath)
                {
                    verCacheFor = activeMsvcPath;
                    verCache.clear();
                    auto p = std::filesystem::path(activeMsvcPath);
                    for (auto it = p; !it.empty() && it.has_parent_path(); it = it.parent_path())
                    {
                        auto name = it.filename().string();
                        if (!name.empty() && (name[0] >= '0' && name[0] <= '9') && name.find('.') != std::string::npos)
                        {
                            verCache = name;
                            break;
                        }
                    }
                }
                label += verCache;
            }
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("##msvcStatusSel", label.c_str()))
            {
                for (auto &tc : detectedMsvc)
                {
                    bool sel = (tc.path == activeMsvcPath);
                    if (ImGui::Selectable(tc.label.c_str(), sel))
                        activeMsvcPath = tc.path;
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        else if (ln == "C#")
        {
            detectToolchains();
            pollDotnetProbe();
            std::string label = ".NET: ";
            label += detectedDotnetSdks.empty() ? "(none)" : detectedDotnetSdks.front().label;
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("##dotnetStatusSel", label.c_str()))
            {
                for (auto &sdk : detectedDotnetSdks)
                {
                    bool sel = (sdk.path == activeDotnetPath);
                    std::string item = "SDK " + sdk.label;
                    if (ImGui::Selectable(item.c_str(), sel))
                        activeDotnetPath = sdk.path;
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
    }

    // Git branch / dirty / ahead-behind indicator (background-polled). Sits
    // after the language + toolchain selectors so the repo-wide status reads
    // last in the left cluster.
    pollGitStatus();
    {
        std::string gb;
        int gd = 0, ga = 0, gbh = 0;
        {
            std::lock_guard<std::mutex> lk(gitInfo->mutex);
            gb = gitInfo->branch;
            gd = gitInfo->dirty;
            ga = gitInfo->ahead;
            gbh = gitInfo->behind;
        }
        if (!gb.empty())
        {
            std::string label = "git: " + gb;
            if (gd > 0)
                label += "  " + std::to_string(gd) + "*";
            if (ga > 0)
                label += "  \xe2\x86\x91" + std::to_string(ga); // up arrow
            if (gbh > 0)
                label += " \xe2\x86\x93" + std::to_string(gbh); // down arrow
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", label.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("branch %s — %d changed, %d ahead, %d behind", gb.c_str(), gd, ga, gbh);
        }
    }

    int line, column;
    e.GetCurrentCursor(line, column);
    float glyphWidth = ImGui::CalcTextSize("#").x;
    char status[256];
    // Filename intentionally omitted — it's already the tab label and window
    // title; keeping it here is the widest trailing chunk and forced the cluster
    // to overlap the left cluster on a narrow bar.
    if (prefShowFps)
    {
        std::snprintf(status, sizeof(status),
                      "%.0f fps  Ln %d, Col %d  Tab: %d",
                      ImGui::GetIO().Framerate, line + 1, column + 1, e.GetTabSize());
    }
    else
    {
        std::snprintf(status, sizeof(status),
                      "Ln %d, Col %d  Tab: %d",
                      line + 1, column + 1, e.GetTabSize());
    }

    // ── Right cluster: status text + dirty dot + clangd + diagnostics, right-
    //    aligned AS A GROUP. The offset must reserve the WHOLE cluster's width or
    //    the trailing items clip off the edge; and the dirty dot gets a real item
    //    slot (Dummy) so the clangd label can't overlap it ("●langd" cramming).
    const float frameH = ImGui::GetFrameHeight();
    const float statusW = ImGui::CalcTextSize(status).x;
    const float dotW = frameH; // dirty-state dot slot

    // Pre-resolve the active file's diagnostics once (for width AND render). Keyed
    // on the canonical path so it matches the store regardless of URI form.
    int errs = 0, warns = 0;
    const std::vector<lsp::Diagnostic> *diags = nullptr;
    const bool lspReady = lspClient.ready();
    if (lspEnabled && lspReady && !tabs.empty() && !lspDiagnostics.empty())
    {
        std::string key = lsp::uriToPath(lspUriForTab(doc()));
        auto it = key.empty() ? lspDiagnostics.end() : lspDiagnostics.find(key);
        if (it != lspDiagnostics.end())
        {
            for (const auto &d : it->second)
                (d.severity <= 1 ? errs : warns)++;
            if (errs || warns)
                diags = &it->second;
        }
    }
    const char *clangdLbl = lspReady ? "clangd \xE2\x97\x8F" : "clangd \xE2\x97\x8B";
    char ebuf[32] = {0}, wbuf[32] = {0};
    if (errs)
        std::snprintf(ebuf, sizeof(ebuf), "\xE2\x9C\x96 %d", errs);
    if (warns)
        std::snprintf(wbuf, sizeof(wbuf), "\xE2\x9A\xA0 %d", warns);

    // Measure the whole trailing cluster so it right-aligns without clipping.
    float trailing = statusW + glyphWidth + dotW;
    if (lspEnabled)
        trailing += glyphWidth * 2.0f + ImGui::CalcTextSize(clangdLbl).x;
    if (diags)
    {
        trailing += glyphWidth * 1.5f;
        if (errs)
            trailing += ImGui::CalcTextSize(ebuf).x;
        if (warns)
            trailing += ImGui::CalcTextSize(wbuf).x + (errs ? glyphWidth : 0.0f);
    }
    // Align the trailing cluster to the bar's true right edge, but never start it
    // left of the left cluster's right edge (lang/toolchain/git) — that overlap is
    // what crammed the status text over the MSVC combo on a narrow bar. The last
    // submitted item IS the left cluster's tail; GetItemRectMax is screen-space, so
    // convert to the local X that SameLine() expects. When the bar is too narrow
    // for both, the cluster butts against the left one and clips at the right edge
    // (preferable to drawing on top of the left cluster).
    float leftEndLocal = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    float off = statusStartX + statusFullW - trailing - glyphWidth;
    if (off < leftEndLocal + glyphWidth)
        off = leftEndLocal + glyphWidth;

    ImGui::SameLine(off);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(status);

    // Dirty dot — reserve a real slot so the next item can't draw over it.
    ImGui::SameLine(0.0f, glyphWidth);
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(dotW, frameH));
        float o = frameH * 0.5f, radius = o * 0.6f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + o, p.y + o), radius,
            isDirty() ? IM_COL32(164, 0, 0, 255) : IM_COL32(164, 164, 164, 255));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(isDirty() ? "Unsaved changes" : "Saved");
    }

    // clangd status: filled green dot once the handshake is done, hollow grey while
    // connecting / unavailable. Lets the user confirm real intellisense is live.
    if (lspEnabled)
    {
        ImGui::SameLine(0.0f, glyphWidth * 2.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(lspReady ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                           "%s", clangdLbl);

        if (diags)
        {
            ImGui::SameLine(0.0f, glyphWidth * 1.5f);
            ImGui::AlignTextToFramePadding();
            ImGui::BeginGroup(); // group so the tooltip covers BOTH counts
            if (errs)
                ImGui::TextColored(ImVec4(0.92f, 0.45f, 0.45f, 1.0f), "%s", ebuf);
            if (warns)
            {
                if (errs)
                    ImGui::SameLine(0.0f, glyphWidth);
                ImGui::TextColored(ImVec4(0.92f, 0.80f, 0.40f, 1.0f), "%s", wbuf);
            }
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                int shown = 0;
                for (const auto &d : *diags)
                {
                    if (shown++ >= 12)
                    {
                        ImGui::TextDisabled("…");
                        break;
                    }
                    ImVec4 cc = d.severity <= 1 ? ImVec4(0.92f, 0.45f, 0.45f, 1.0f)
                                                : ImVec4(0.92f, 0.80f, 0.40f, 1.0f);
                    ImGui::TextColored(cc, "%d: %s", d.line + 1, d.message.c_str());
                }
                ImGui::EndTooltip();
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

//
//  Editor::showDiff
//

void Editor::showDiff()
{
    auto &t = doc();
    t.diff.SetLanguage(t.editor.GetLanguage());
    t.diff.SetText(t.originalText, t.editor.GetText());
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    state = State::diff;
}

//
//  Editor::showFileOpen
//

std::filesystem::path Editor::userConfigDir()
{
    // Stable absolute path so settings round-trip across launches regardless
    // of which directory the editor was started from. Used for both the
    // favourites blob and the ImGui layout .ini file.
    //   Windows: %APPDATA%\ImGuiColorTextEdit
    //   POSIX:   $XDG_CONFIG_HOME/imguicolortext  (or  $HOME/.config/...)
    static std::filesystem::path cached;
    if (!cached.empty())
        return cached;

    std::filesystem::path base;
#ifdef _WIN32
    if (const char *appData = std::getenv("APPDATA"))
    {
        base = std::filesystem::path(appData) / "ImGuiColorTextEdit";
    }
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
    {
        base = std::filesystem::path(xdg) / "imguicolortext";
    }
    else if (const char *home = std::getenv("HOME"))
    {
        base = std::filesystem::path(home) / ".config" / "imguicolortext";
    }
#endif
    // Last-resort fallback: next to the cwd. Should only hit on weird envs.
    if (base.empty())
    {
        base = std::filesystem::current_path() / ".imguicolortext";
    }

    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    cached = std::move(base);
    return cached;
}

static std::filesystem::path placesStatePath()
{
    return Editor::userConfigDir() / "places.txt";
}

// One-shot population of the file dialog's built-in Places pane:
//   * "Drives"     — system drives (C:\, D:\, …) or "/" + "~" on POSIX
//     (canBeSaved=false so they don't get serialised).
//   * "Favourites" — user-editable; serialised by IGFD between runs.
// Persistence is via IGFD's SerializePlaces/DeserializePlaces blobs, written
// to .claude/places.txt on dialog close.
static void populateFileDialogPlaces()
{
    auto *dlg = ImGuiFileDialog::Instance();
    static bool done = false;
    if (done)
        return;
    done = true;

    // IMPORTANT ORDER: groups must be created BEFORE DeserializePlaces runs.
    // IGFD's DeserializePlaces calls GetPlacesGroupPtr (which does NOT create
    // missing groups) — if the "Favourites" group doesn't exist yet, every
    // saved place is silently dropped. Likewise AddPlacesGroup replaces the
    // existing group object, so doing it AFTER Deserialize wipes the loaded
    // places. Create the groups first, then load into them.

    // 1. Create both groups in their final state.
    dlg->AddPlacesGroup("Drives", 0, /*canEdit*/ false, /*opened*/ true);
    dlg->AddPlacesGroup("Favourites", 1, /*canEdit*/ true, /*opened*/ true);

    // 2. Populate Drives (non-persistent — re-enumerated every run).
    if (auto *grp = dlg->GetPlacesGroupPtr("Drives"))
    {
#ifdef _WIN32
        wchar_t drives[256];
        DWORD n = GetLogicalDriveStringsW(255, drives);
        if (n > 0 && n < 256)
        {
            for (wchar_t *p = drives; *p; p += wcslen(p) + 1)
            {
                char buf[8] = {0};
                for (int i = 0; p[i] && i < 7; ++i)
                    buf[i] = static_cast<char>(p[i]);
                std::string label(buf);
                if (!label.empty() && label.back() == '\\')
                    label.pop_back(); // "C:"
                grp->AddPlace(label, buf, /*canBeSaved*/ false);
            }
        }
#else
        grp->AddPlace("/", "/", /*canBeSaved*/ false);
        if (const char *home = std::getenv("HOME"))
            grp->AddPlace("Home", home, false);
#endif
    }

    // 3. Load the persisted Favourites *into* the existing group.
    std::ifstream f(placesStatePath(), std::ios::binary);
    if (f.is_open())
    {
        std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!blob.empty())
            dlg->DeserializePlaces(blob);
    }
}

// Write the current Places state (only entries with canBeSaved=true) to disk.
// Idempotent — keeps the last-written blob in a static so per-frame calls
// while the dialog is open are essentially free unless the user actually
// changed something (added/removed a favourite).
static void saveFileDialogPlaces()
{
    static std::string lastWritten; // process-lifetime cache
    auto blob = ImGuiFileDialog::Instance()->SerializePlaces(/*forceAll*/ false);
    if (blob == lastWritten)
        return; // no change since last write
    std::error_code ec;
    std::filesystem::create_directories(placesStatePath().parent_path(), ec);
    std::ofstream f(placesStatePath(), std::ios::binary | std::ios::trunc);
    if (f.is_open())
    {
        f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        lastWritten = std::move(blob);
    }
}

// Where file/project dialogs should start. Prefer the active document's folder,
// then the open project root, then the process cwd — NOT wherever the app
// happened to be launched from. IGFD treats an empty path as "last/cwd", so we
// pass an explicit directory.
std::string Editor::dialogStartDir() const
{
    std::error_code ec;
    if (!tabs.empty() && doc().filename != "untitled")
    {
        auto p = std::filesystem::path(doc().filename).parent_path();
        if (!p.empty() && std::filesystem::is_directory(p, ec))
            return p.string();
    }
    if (!projectRoot.empty() && std::filesystem::is_directory(projectRoot, ec))
        return projectRoot.string();
    return std::filesystem::current_path(ec).string();
}

void Editor::showFileOpen()
{
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    populateFileDialogPlaces();
    IGFD::FileDialogConfig config;
    config.path = dialogStartDir();
    config.countSelectionMax = 1;
    config.flags =
        ImGuiFileDialogFlags_DontShowHiddenFiles |
        ImGuiFileDialogFlags_ReadOnlyFileNameField;
    ImGuiFileDialog::Instance()->OpenDialog("file-open", "Select File to Open...", ".*", config);
    state = State::openFile;
}

//
//  Editor::showSaveFileAs
//

void Editor::showSaveFileAs()
{
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    IGFD::FileDialogConfig config;
    config.path = dialogStartDir();
    config.countSelectionMax = 1;
    config.flags =
        ImGuiFileDialogFlags_DontShowHiddenFiles |
        ImGuiFileDialogFlags_ConfirmOverwrite;
    populateFileDialogPlaces();
    ImGuiFileDialog::Instance()->OpenDialog("file-saveas", "Save File as...", "*", config);
    state = State::saveFileAs;
}

//
//  Editor::showConfirmClose
//

void Editor::showConfirmClose(std::function<void()> callback)
{
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    onConfirmClose = callback;
    state = State::confirmClose;
}

//
//  Editor::showConfirmQuit
//

void Editor::showConfirmQuit()
{
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    state = State::confirmQuit;
}

//
//  Editor::showError
//

void Editor::showError(const std::string &message)
{
    // GetWindowViewport() asserts (fires IM_ASSERT → abort) when there is no
    // current window — which is the case when openFile() fails during STARTUP,
    // before the first frame (e.g. a session-restored file that was since moved).
    // Only ask for the window's viewport when a window is actually current;
    // otherwise fall back to the main viewport, which is valid outside a frame.
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->CurrentWindow)
        dialogViewportId = ImGui::GetWindowViewport()->ID;
    else if (ctx)
        dialogViewportId = ImGui::GetMainViewport()->ID;
    dialogNeedsPlacement = true;
    errorMessage = message;
    state = State::confirmError;
}

//
//  Editor::renderDiff
//

void Editor::renderDiff()
{
    ImGuiViewport *viewport = ImGui::FindViewportByID(dialogViewportId);
    if (!viewport)
        viewport = ImGui::GetMainViewport();
    // Non-modal so the user can dock it, drag it onto another monitor (becomes
    // its own OS window via multi-viewport), and keep editing while it's open.
    // Position + viewport only on first appearance (same trick we use for the
    // file dialog), otherwise we'd snap it back to centre every frame.
    if (dialogNeedsPlacement)
    {
        ImGui::SetNextWindowViewport(viewport->ID);
        ImVec2 sz(viewport->Size.x * 0.8f, viewport->Size.y * 0.8f);
        ImVec2 pos(viewport->Pos.x + (viewport->Size.x - sz.x) * 0.5f,
                   viewport->Pos.y + (viewport->Size.y - sz.y) * 0.5f);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
        dialogNeedsPlacement = false;
    }

    bool open = true;
    if (ImGui::Begin("File History — Changes since open##history", &open))
    {
        ImVec2 region = ImGui::GetContentRegionAvail();
        ImVec2 diffSize(region.x, (std::max)(region.y - ImGui::GetFrameHeightWithSpacing() - 8.0f, 100.0f));
        doc().diff.Render("diff", diffSize, true);
        ImGui::Separator();
        bool sideBySide = doc().diff.GetSideBySideMode();
        if (ImGui::Checkbox("Show side-by-side", &sideBySide))
        {
            doc().diff.SetSideBySideMode(sideBySide);
        }
        ImGui::SameLine();
        // Revert button — only enabled when the doc has unsaved changes
        // against the on-open snapshot. Use ReplaceSectionText so the revert
        // is a normal transaction the regular Undo / Redo can step through.
        bool dirty = isDirty();
        if (!dirty)
            ImGui::BeginDisabled();
        if (ImGui::Button("Revert to opened version"))
        {
            auto &ed = doc().editor;
            int last = (std::max)(ed.GetLineCount() - 1, 0);
            std::string lastLineText = ed.GetLineText(last);
            int lastCol = (int)lastLineText.size();
            ed.ReplaceSectionText(0, 0, last, lastCol, doc().originalText);
            doc().version = ed.GetUndoIndex();
        }
        if (!dirty)
            ImGui::EndDisabled();

        // Undo / Redo right here so the user can step back into the reverted
        // state if they hit Revert and changed their mind.
        ImGui::SameLine();
        auto &ed = doc().editor;
        if (!ed.CanUndo())
            ImGui::BeginDisabled();
        if (ImGui::Button("Undo"))
            ed.Undo();
        if (!ed.CanUndo())
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (!ed.CanRedo())
            ImGui::BeginDisabled();
        if (ImGui::Button("Redo"))
            ed.Redo();
        if (!ed.CanRedo())
            ImGui::EndDisabled();
    }
    ImGui::End();
    if (!open)
        state = State::edit;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
    {
        // Esc closes only when no editor input is active.
        // (left intentionally simple — full focus tracking is overkill here.)
    }
}

//
//  Editor::renderFileOpen
//

void Editor::renderFileOpen()
{
    ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
    if (!vp)
        vp = ImGui::GetMainViewport();
    ImVec2 maxSize = vp->Size;
    ImVec2 minSize = ImVec2(480.0f, 320.0f);
    auto dialog = ImGuiFileDialog::Instance();
    // Only set viewport + initial pos/size ONCE when the dialog opens.
    // Calling SetNextWindowViewport every frame would snap the dialog back to
    // the original viewport even after the user drags it out into its own window.
    if (dialogNeedsPlacement)
    {
        ImGui::SetNextWindowViewport(vp->ID);
        ImVec2 initialSize(vp->Size.x * 0.7f, vp->Size.y * 0.7f);
        ImVec2 initialPos(vp->Pos.x + (vp->Size.x - initialSize.x) * 0.5f,
                          vp->Pos.y + (vp->Size.y - initialSize.y) * 0.5f);
        ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
        dialogNeedsPlacement = false;
    }

    bool finished = dialog->Display("file-open", ImGuiWindowFlags_NoCollapse, minSize, maxSize);
    // Save every frame the dialog is open — the saver is no-op if the
    // serialized blob hasn't changed since the last successful write, so this
    // is cheap. Guarantees user-added favourites persist even if the user
    // kills the process before the dialog closes cleanly.
    saveFileDialogPlaces();
    if (finished)
    {
        if (dialog->IsOk())
        {
            openFile(dialog->GetFilePathName());
        }
        state = State::edit;
        dialog->Close();
        saveFileDialogPlaces();
    }
}

//
//  Editor::renderSaveAs
//

void Editor::renderSaveAs()
{
    ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
    if (!vp)
        vp = ImGui::GetMainViewport();
    ImVec2 maxSize = vp->Size;
    ImVec2 minSize = ImVec2(480.0f, 320.0f);
    auto dialog = ImGuiFileDialog::Instance();
    if (dialogNeedsPlacement)
    {
        ImGui::SetNextWindowViewport(vp->ID);
        ImVec2 initialSize(vp->Size.x * 0.7f, vp->Size.y * 0.7f);
        ImVec2 initialPos(vp->Pos.x + (vp->Size.x - initialSize.x) * 0.5f,
                          vp->Pos.y + (vp->Size.y - initialSize.y) * 0.5f);
        ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
        dialogNeedsPlacement = false;
    }

    bool finished = dialog->Display("file-saveas", ImGuiWindowFlags_NoCollapse, minSize, maxSize);
    saveFileDialogPlaces(); // same per-frame cheap-no-op save as in renderFileOpen
    if (finished)
    {
        if (dialog->IsOk())
        {
            doc().filename = dialog->GetFilePathName();
            if (doc().editor.GetLanguage() == nullptr)
                doc().editor.SetLanguage(languageForPath(doc().filename));
            saveFile();
            state = State::edit;
        }
        else
        {
            state = State::edit;
        }
        dialog->Close();
        saveFileDialogPlaces();
    }
}

//
//  Editor::renderConfirmClose
//

void Editor::renderConfirmClose()
{
    ImGui::OpenPopup("Confirm Close");
    ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
    if (!vp)
        vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Confirm Close", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("This file has unsaved changes!\nDo you want to discard them?\n\n");
        ImGui::Separator();
        static constexpr float buttonWidth = 80.0f;
        ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Discard", ImVec2(buttonWidth, 0.0f)))
        {
            state = State::edit;
            if (onConfirmClose)
                onConfirmClose();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            state = State::edit;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

//
//  Editor::renderConfirmQuit
//

void Editor::renderConfirmQuit()
{
    ImGui::OpenPopup("Quit Editor?");
    ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
    if (!vp)
        vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Quit Editor?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Unsaved changes in these documents:");
        ImGui::Spacing();
        int dirtyCount = 0;
        for (size_t i = 0; i < tabs.size(); ++i)
            if (isDirtyTab(i))
                ++dirtyCount;
        // Scroll if there are a lot; otherwise the popup just grows to fit.
        float rowH = ImGui::GetTextLineHeightWithSpacing();
        float listH = (dirtyCount > 10 ? 10.0f : (float)(dirtyCount < 1 ? 1 : dirtyCount)) * rowH;
        ImGui::BeginChild("##unsavedList", ImVec2(360.0f, listH), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            if (!isDirtyTab(i))
                continue;
            const std::string &fn = tabs[i]->filename;
            std::string name = (fn.empty() || fn == "untitled")
                                   ? "untitled"
                                   : std::filesystem::path(fn).filename().string();
            ImGui::BulletText("%s", name.c_str());
            if (fn != "untitled" && !fn.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", fn.c_str());
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextDisabled("Quit anyway and lose these edits?");
        ImGui::Separator();
        static constexpr float buttonWidth = 80.0f;
        ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Quit", ImVec2(buttonWidth, 0.0f)))
        {
            done = true;
            state = State::edit;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            state = State::edit;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

//
//  Editor::renderConfirmError
//

void Editor::renderConfirmError()
{
    ImGui::OpenPopup("Error");
    // Always anchor this global dialog to the MAIN viewport (not whatever
    // dialogViewportId happens to be) — otherwise, triggered from a floating
    // window like Developer Tools, the popup is confined to that window's bounds
    // instead of popping out / centering on the main window.
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();

    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s\n", errorMessage.c_str());
        ImGui::Separator();
        static constexpr float buttonWidth = 80.0f;
        ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth);
        if (ImGui::Button("OK", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            errorMessage.clear();
            state = State::edit;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

//
//  Editor::showGotoLine / renderGotoLine
//

void Editor::showGotoLine()
{
    if (auto *vp = ImGui::GetWindowViewport())
        dialogViewportId = vp->ID;
    else
        dialogViewportId = ImGui::GetMainViewport()->ID;
    gotoLineBuf[0] = '\0';
    state = State::gotoLine;
}

void Editor::renderGotoLine()
{
    ImGui::OpenPopup("Go to Line");
    ImGuiViewport *vp = ImGui::FindViewportByID(dialogViewportId);
    if (!vp)
        vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Go to Line", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        auto &t = doc();
        int lineCount = t.editor.GetLineCount();
        ImGui::Text("Line (1 - %d), optional :column", lineCount);
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Auto-focus the input on first frame, accept Enter to commit.
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##gotoLine", gotoLineBuf, sizeof(gotoLineBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);

        auto commit = [&]() {
            // Parse "<line>" or "<line>:<col>" (1-based input, 0-based internal).
            int targetLine = 0, targetCol = 0;
            const char *p = gotoLineBuf;
            while (*p == ' ' || *p == '\t')
                ++p;
            while (*p >= '0' && *p <= '9')
            {
                targetLine = targetLine * 10 + (*p - '0');
                ++p;
            }
            if (*p == ':')
            {
                ++p;
                while (*p >= '0' && *p <= '9')
                {
                    targetCol = targetCol * 10 + (*p - '0');
                    ++p;
                }
            }
            if (targetLine > 0)
            {
                // (std::min)/(std::max) parens — <Windows.h> defines min/max
                // as macros up at the top of this file and we don't want them
                // eating the function names.
                int maxLine = (std::max)(lineCount - 1, 0);
                int ln = (std::min)(targetLine - 1, maxLine);
                int col = (std::max)(targetCol - 1, 0);
                navHistory.record(currentNavLocation()); // so Back returns here
                t.editor.SetCursor(ln, col);
                t.editor.ScrollToLine(ln, TextEditor::Scroll::alignMiddle);
                t.wantFocus = true;
            }
            state = State::edit;
            ImGui::CloseCurrentPopup();
        };

        ImGui::Separator();
        static constexpr float buttonWidth = 80.0f;
        ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Go", ImVec2(buttonWidth, 0.0f)) || enter)
        {
            commit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            state = State::edit;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

//
//  Editor::setAutocompleteMode
//

// ── Member-aware autocomplete helpers ────────────────────────────────────

static bool tsIsIdentChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static std::string tsLastSeg(std::string t)
{
    if (auto p = t.rfind("::"); p != std::string::npos)
        t = t.substr(p + 2);
    if (auto p = t.rfind('.'); p != std::string::npos)
        t = t.substr(p + 1);
    return t;
}

// Best-effort syntactic resolution of receiver IDENT's declared type from the
// buffer. Matches "Type recv" (decl/param) and "recv = new Type(" (C#). Skips
// auto/var and keywords. Returns "" when not confidently resolved (caller then
// falls back to normal identifier completion — precise-only, never wrong).
static std::string resolveReceiverType(const std::string &buf, const std::string &recv)
{
    static const std::unordered_set<std::string> notType = {
        "return",
        "new",
        "delete",
        "sizeof",
        "throw",
        "case",
        "if",
        "while",
        "for",
        "switch",
        "do",
        "else",
        "auto",
        "var",
        "const",
        "static",
        "public",
        "private",
        "protected",
        "internal",
        "virtual",
        "override",
        "readonly",
        "volatile",
        "mutable",
        "using",
        "namespace",
        "class",
        "struct",
        "enum",
        "void",
        "and",
        "or",
        "not",
        "co_return",
        "co_await",
        "ref",
        "out",
        "in",
        "unsafe",
        "async",
        "await",
        "this",
        "base",
        "typename",
        "template",
        "return",
        "goto",
        "default",
    };
    if (recv.empty())
        return {};
    size_t n = buf.size();
    auto isSp = [](char c) { return c == ' ' || c == '\t'; };
    for (size_t i = 0; i + recv.size() <= n;)
    {
        if (buf.compare(i, recv.size(), recv) != 0)
        {
            ++i;
            continue;
        }
        size_t e = i + recv.size();
        bool wbL = (i == 0) || !tsIsIdentChar(buf[i - 1]);
        bool wbR = (e >= n) || !tsIsIdentChar(buf[e]);
        if (!wbL || !wbR)
        {
            ++i;
            continue;
        }

        // Pattern A: recv = new Type(
        size_t p = e;
        while (p < n && isSp(buf[p]))
            ++p;
        if (p < n && buf[p] == '=' && (p + 1 >= n || buf[p + 1] != '='))
        {
            ++p;
            while (p < n && isSp(buf[p]))
                ++p;
            if (p + 4 <= n && buf.compare(p, 3, "new") == 0 && (isSp(buf[p + 3]) || buf[p + 3] == '\n'))
            {
                p += 3;
                while (p < n && (isSp(buf[p]) || buf[p] == '\n' || buf[p] == '\r'))
                    ++p;
                size_t ts = p;
                while (p < n && (tsIsIdentChar(buf[p]) || buf[p] == '.' || buf[p] == ':'))
                    ++p;
                if (p > ts)
                    return tsLastSeg(buf.substr(ts, p - ts));
            }
        }

        // Pattern B: Type recv  (the token immediately before recv is a type).
        // Allow '<' too so a templated type (std::vector<int> recv) is captured
        // whole rather than stopping inside the angle brackets.
        size_t q = i;
        while (q > 0 && isSp(buf[q - 1]))
            --q;
        size_t te = q;
        while (q > 0 && (tsIsIdentChar(buf[q - 1]) || buf[q - 1] == '.' || buf[q - 1] == ':' ||
                         buf[q - 1] == '*' || buf[q - 1] == '&' || buf[q - 1] == '>' || buf[q - 1] == '<'))
            --q;
        std::string tok = buf.substr(q, te - q);
        // Drop template arguments: std::vector<int> -> std::vector.
        if (auto lt = tok.find('<'); lt != std::string::npos)
            tok = tok.substr(0, lt);
        while (!tok.empty() && (tok.back() == '*' || tok.back() == '&'))
            tok.pop_back();
        std::string simple = tsLastSeg(tok);
        if (!simple.empty() && tsIsIdentChar(simple[0]) && !(simple[0] >= '0' && simple[0] <= '9') && !notType.count(simple))
            return simple;

        i = e;
    }
    return {};
}

void Editor::configureTabAutocomplete(TabDocument &t)
{
    // Wire the autocomplete callback + debounced rebuild for a SINGLE tab and
    // build its trie once. Split out of setAutocompleteMode() so newTab() can
    // set up only the new tab instead of rebuilding EVERY open tab's trie
    // (which re-scanned big docs + re-inserted the whole project index on every
    // "+" press — the multi-second new-tab stall).
    TabDocument *tptr = &t;
    // Right-click a line number → "Reply to Claude about this line" (any line). The
    // purple gutter dot on Claude-changed lines is the left-click equivalent; both
    // open the reply popup → <configDir>/replies outbox.
    t.editor.SetLineNumberContextMenuCallback([this, tptr](int line) {
        // ✎ — U+270E, present in DejaVu (the � emoji is not; it rendered as tofu).
        if (ImGui::MenuItem("\xe2\x9c\x8e Reply to Claude about this line"))
            requestReply(tptr->filename, line, tptr->filename + ":" + std::to_string(line + 1));
    });
    TextEditor::AutoCompleteConfig config;
    config.callback = [this, tptr](TextEditor::AutoCompleteState &state) {
        if (tptr->largeFile)
            return; // large-file mode: no per-keystroke completion work (trie is empty anyway)
        // LSP completion (clangd): fire async at the cursor BEFORE the tree-sitter
        // paths below (which fill an instant baseline). pollLsp() refines the popup
        // when clangd replies. searchTermEndIndex is a CODEPOINT index → byte offset
        // (clangd negotiated utf-8). Fires unconditionally so the member-path return
        // doesn't pre-empt it.
        {
            std::string lext = std::filesystem::path(tptr->filename).extension().string();
            std::transform(lext.begin(), lext.end(), lext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (lspActiveForExt(lext))
            {
                lspSyncDoc(*tptr);
                std::string luri = lspUriForTab(*tptr);
                if (!luri.empty())
                {
                    std::string lln = tptr->editor.GetLineText((int)state.line);
                    size_t bcol = 0;
                    for (size_t cp = 0; cp < (size_t)state.searchTermEndIndex && bcol < lln.size(); ++cp)
                    {
                        ++bcol;
                        while (bcol < lln.size() && (((unsigned char)lln[bcol]) & 0xC0) == 0x80)
                            ++bcol;
                    }
                    int rid = lspClient.requestCompletion(luri, (int)state.line, (int)bcol);
                    if (rid)
                    {
                        lspCompletionId = rid;
                        lspCompletionTab = tptr;
                        state.suggestionsPromise = true;
                    }
                }
            }
        }
        // Member-aware completion: if the text right before the search term is a
        // member-access operator (. / -> / ::) on a receiver whose type we can
        // resolve, list ONLY that type's members. Precise-only: if anything is
        // unresolved we fall through to normal identifier completion.
        {
            std::string ln = tptr->editor.GetLineText((int)state.line);
            // searchTermStartIndex is a CODEPOINT index; ln is UTF-8 BYTES. Walk
            // that many codepoints to get the byte offset, else any multibyte
            // char earlier on the line shifts every following byte test.
            size_t k = 0;
            for (size_t cp = 0; cp < (size_t)state.searchTermStartIndex && k < ln.size(); ++cp)
            {
                ++k;
                while (k < ln.size() && (((unsigned char)ln[k]) & 0xC0) == 0x80)
                    ++k;
            }
            auto isSp = [](char c) { return c == ' ' || c == '\t'; };
            size_t op = k;
            while (op > 0 && isSp(ln[op - 1]))
                --op;
            std::string oper;
            if (op >= 2 && ln[op - 1] == '>' && ln[op - 2] == '-')
            {
                oper = "->";
                op -= 2;
            }
            else if (op >= 2 && ln[op - 1] == ':' && ln[op - 2] == ':')
            {
                oper = "::";
                op -= 2;
            }
            else if (op >= 1 && ln[op - 1] == '.')
            {
                oper = ".";
                op -= 1;
            }

            if (!oper.empty())
            {
                // Identifier byte: ASCII ident char OR any UTF-8 byte (>=0x80), so a
                // non-ASCII identifier (café.member) isn't truncated mid-codepoint.
                auto isIdentByte = [](char c) { return tsIsIdentChar(c) || ((unsigned char)c >= 0x80); };
                bool badBase = false;
                // If a balanced (...) ends just before `p`, skip it so a method call
                // `get()` reads as the segment `get` (-> its return type). Returns
                // false on an unbalanced/odd paren (caller bails). No-op if no ')'.
                auto skipCall = [&](size_t &p) -> bool {
                    if (p == 0 || ln[p - 1] != ')')
                        return true;
                    int depth = 0;
                    size_t i = p;
                    while (i > 0)
                    {
                        char c = ln[i - 1];
                        --i;
                        if (c == ')')
                            ++depth;
                        else if (c == '(')
                        {
                            if (--depth == 0)
                            {
                                p = i;
                                return true;
                            }
                        }
                    }
                    return false;
                };
                // If a balanced [...] ends just before `p`, skip it and set `sub`
                // (subscript v[i] -> element type). false only on unbalanced.
                auto skipBracket = [&](size_t &p, bool &sub) -> bool {
                    if (p == 0 || ln[p - 1] != ']')
                        return true;
                    int depth = 0;
                    size_t i = p;
                    while (i > 0)
                    {
                        char c = ln[i - 1];
                        --i;
                        if (c == ']')
                            ++depth;
                        else if (c == '[')
                        {
                            if (--depth == 0)
                            {
                                p = i;
                                sub = true;
                                return true;
                            }
                        }
                    }
                    return false;
                };
                size_t r = op;
                while (r > 0 && isSp(ln[r - 1]))
                    --r;
                if (!skipCall(r))
                    badBase = true; // completing the result of a call: foo().|
                while (r > 0 && isSp(ln[r - 1]))
                    --r;
                bool recvSub = false;
                if (!skipBracket(r, recvSub))
                    badBase = true; // completing an element: v[i].|
                while (r > 0 && isSp(ln[r - 1]))
                    --r;
                size_t rEnd = r;
                while (r > 0 && isIdentByte(ln[r - 1]))
                    --r;
                std::string receiver = ln.substr(r, rEnd - r);
                // Build the receiver CHAIN for member-of-member completion:
                // a.b.c -> {a,b,c}, walking back over '.'/'->' segments, skipping a
                // call's (...) or a subscript's [...] so a.get().b and v[i].b chain
                // through the return/element type. '::' stays a single segment (the
                // receiver IS the type). A member op with an unresolvable receiver or
                // a qualified base bails.
                std::vector<std::string> chain;
                chain.push_back(receiver);
                if (recvSub)
                    chain.push_back("[]"); // v[i]. -> [v, "[]"]
                if (oper != "::" && !badBase)
                {
                    size_t seg = r;
                    for (;;)
                    {
                        size_t q = seg;
                        while (q > 0 && isSp(ln[q - 1]))
                            --q;
                        if (q >= 2 && ln[q - 1] == '>' && ln[q - 2] == '-')
                            q -= 2;
                        else if (q >= 1 && ln[q - 1] == '.')
                            q -= 1;
                        else
                            break; // no further member op
                        while (q > 0 && isSp(ln[q - 1]))
                            --q;
                        if (!skipCall(q))
                        {
                            badBase = true;
                            break;
                        } // unbalanced parens
                        while (q > 0 && isSp(ln[q - 1]))
                            --q;
                        bool sub = false;
                        if (!skipBracket(q, sub))
                        {
                            badBase = true;
                            break;
                        } // unbalanced bracket
                        while (q > 0 && isSp(ln[q - 1]))
                            --q;
                        size_t e = q;
                        while (q > 0 && isIdentByte(ln[q - 1]))
                            --q;
                        if (q == e)
                        {
                            badBase = true;
                            break;
                        } // op w/ no ident receiver
                        if (q >= 2 && ln[q - 1] == ':' && ln[q - 2] == ':')
                        {
                            badBase = true;
                            break;
                        } // qualified base
                        if (sub)
                            chain.insert(chain.begin(), "[]"); // subscript on this segment (after its ident)
                        chain.insert(chain.begin(), ln.substr(q, e - q));
                        seg = q;
                    }
                }
                if (!badBase && !chain.front().empty() &&
                    !(chain.front()[0] >= '0' && chain.front()[0] <= '9'))
                {
                    // '::' receiver IS the type. '.'/'->' resolve the chain's final
                    // type — scope-aware tree-sitter on the live document (handles
                    // auto/var, params, fields, this, and member-of-member). The
                    // scope-blind string scan is a fallback for the single-hop case.
                    std::string type;
                    if (oper == "::")
                    {
                        type = receiver;
                    }
                    else
                    {
                        ts::Lang lang = ts::langForExtension(
                            std::filesystem::path(tptr->filename).extension().string());
                        if (lang != ts::Lang::None)
                        {
                            // Pass the project index so chains hop into types from
                            // other files (o.inner.items where Inner is in a header).
                            auto idxSnap = indexSnapshot();
                            type = ts::resolveMemberChain(lang, tptr->editor.GetText(),
                                                          (int)state.line, (int)k, chain,
                                                          idxSnap ? &idxSnap->memberTypes : nullptr);
                        }
                        if (type.empty() && chain.size() == 1)
                            type = resolveReceiverType(tptr->editor.GetText(), receiver);
                    }
                    if (!type.empty())
                    {
                        // Union project-defined members (from the index) with the
                        // curated STL table, so everyday receivers (std::vector v;
                        // v.) complete even though the index never parses system
                        // headers. Project members win ties (inserted first).
                        std::vector<std::string> members;
                        if (auto idx = indexSnapshot())
                        {
                            auto mit = idx->members.find(type);
                            if (mit != idx->members.end())
                                members.insert(members.end(), mit->second.begin(), mit->second.end());
                        }
                        if (const auto *stl = ts::stlMembers(type))
                            members.insert(members.end(), stl->begin(), stl->end());
                        if (!members.empty())
                        {
                            TextEditor::Trie mtrie;
                            for (auto &m : members)
                                mtrie.insert(m);
                            std::vector<std::string> hits;
                            mtrie.findSuggestions(hits, state.searchTerm);
                            if (!hits.empty())
                            {
                                state.suggestions = std::move(hits);
                                return; // members only
                            }
                            // Members exist but none match the search term — fall
                            // through to identifier completion (never an empty popup).
                        }
                    }
                }
            }
        }

        // Merge this file's identifiers with the shared project-wide trie.
        // Queried separately because findSuggestions() clears its output
        // vector. File-local matches rank first; project matches fill the
        // rest, deduped, capped at 20.
        if (projectTrieGen != indexState->gen.load())
            buildProjectTrie();
        std::vector<std::string> local, proj;
        tptr->trie.findSuggestions(local, state.searchTerm);
        projectTrie.findSuggestions(proj, state.searchTerm);
        state.suggestions.clear();
        std::unordered_set<std::string> seen;
        for (auto &s : local)
        {
            if (state.suggestions.size() >= 20)
                break;
            if (seen.insert(s).second)
                state.suggestions.push_back(s);
        }
        for (auto &s : proj)
        {
            if (state.suggestions.size() >= 20)
                break;
            if (seen.insert(s).second)
                state.suggestions.push_back(s);
        }
    };
    // UE descriptors (.uproject/.uplugin) are JSON — every completable token
    // (key, enum value, plugin/module name) sits INSIDE a string literal, which
    // suppresses autocomplete by default.
    {
        std::string cext = std::filesystem::path(t.filename).extension().string();
        std::transform(cext.begin(), cext.end(), cext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (cext == ".uproject" || cext == ".uplugin")
            config.triggerInStrings = true;
    }
    t.editor.SetAutoCompleteConfig(&config);
    t.editor.SetChangeCallback([this, tptr]() { buildAutocompleteTrie(*tptr); }, 3000);
    buildAutocompleteTrie(t);
}

void Editor::setAutocompleteMode(bool flag)
{
    // Global toggle: (re)configure or tear down autocomplete on ALL tabs. The
    // per-tab path lives in configureTabAutocomplete(); adding a tab must NOT
    // call this (use that helper for just the new tab) or it rebuilds every
    // trie.
    for (auto &tp : tabs)
    {
        auto &t = *tp;
        if (flag)
        {
            configureTabAutocomplete(t);
        }
        else
        {
            t.editor.SetAutoCompleteConfig(nullptr);
            t.editor.SetChangeCallback(nullptr);
        }
    }
}

//
//  Editor::buildProjectTrie
//

// One shared project-wide identifier trie for the whole editor, rebuilt once
// per index generation (NOT once per tab). Previously every tab kept its own
// full copy of the project index inside its trie; opening N tabs allocated N
// giant tries and froze the app. Now each tab's trie is file-local only and
// this single shared trie carries the project-wide identifiers.
void Editor::buildProjectTrie()
{
    ScopedTimer _t("buildProjectTrie");
    int g = indexState->gen.load();
    projectTrie.clear();
    if (auto idx = indexSnapshot())
    {
        for (auto &id : idx->identifiers)
            projectTrie.insert(id);
    }
    projectTrieGen = g;
}

//
//  Editor::buildAutocompleteTrie
//

//
//  ── Symbol packs ───────────────────────────────────────────────────────────
//
//  A pack is JSON: { "name": "...", "types": { "TypeName": ["m1","m2", ...] } }.
//  Members are name-only (completion inserts the name), matching the compiled
//  ts::stlMembers table it merges into. Loaded once at startup, before the render
//  loop queries members, so there's no race with the registry read path.
//

int Editor::loadSymbolPackFile(const std::filesystem::path &file)
{
    std::ifstream f(file, std::ios::binary);
    if (!f.is_open())
        return 0;

    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // A malformed or hand-edited pack must not take startup down — a bad file is
    // "no symbols", not a crash.
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return 0;

    auto types = j.find("types");
    if (types == j.end() || !types->is_object())
        return 0;

    int added = 0;
    for (auto it = types->begin(); it != types->end(); ++it)
    {
        if (!it.value().is_array())
            continue;

        std::vector<std::string> members;
        for (const auto &m : it.value())
        {
            if (m.is_string())
                members.push_back(m.get<std::string>());
        }

        if (!members.empty())
        {
            ts::registerTypeMembers(it.key(), members);
            added++;
        }
    }

    return added;
}

void Editor::loadSymbolPacks()
{
    std::error_code ec;
    std::vector<std::filesystem::path> dirs = {
        hostExeDir() / "symbols",
        userConfigDir() / "symbols",
    };

    int types = 0;
    for (const auto &dir : dirs)
    {
        if (!std::filesystem::is_directory(dir, ec))
            continue;

        for (auto &entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec)
                break;
            if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
                types += loadSymbolPackFile(entry.path());
        }
    }

    symbolPacksLoaded = types;
}

//
//  ── Screenshot ─────────────────────────────────────────────────────────────
//
//  The request is fulfilled by main.cpp (it owns the GPU device and this frame's
//  draw data); the editor only decides WHERE it goes and reports the result.
//

void Editor::requestScreenshot()
{
    std::error_code ec;
    std::filesystem::path dir = userConfigDir() / "screenshots";
    std::filesystem::create_directories(dir, ec);

    screenshotPath = screenshot::timestampedPath(dir.generic_string());
    screenshotPending = true;
}

bool Editor::consumeScreenshotRequest(std::string &outPath)
{
    if (!screenshotPending)
        return false;

    screenshotPending = false;
    outPath = screenshotPath;
    return true;
}

void Editor::onScreenshotWritten(bool ok, const std::string &path)
{
    if (!ok)
    {
        pushToast("Screenshot failed", IM_COL32(230, 110, 100, 255), 3);
        return;
    }

    lastScreenshotPath = path;
    pushToast("Screenshot saved \xe2\x80\x94 " + std::filesystem::path(path).filename().string(),
              IM_COL32(120, 200, 130, 255), 4);
}

//
//  ── Guided tour ────────────────────────────────────────────────────────────
//
//  Help ▸ Take the Tour. Every step turns its panel ON and anchors a card to the
//  real window, so the tour drives the actual UI instead of describing it.
//

const std::vector<Editor::TourStep> &Editor::tourSteps()
{
    static const std::vector<TourStep> steps = {
        {"Welcome to ImGui-IDE",
         "A self-hosting IDE: it's written in the code it edits.\n\n"
         "This tour opens each panel as it goes — everything you see is live, not a mockup.\n"
         "Esc or Skip leaves at any point.",
         nullptr, nullptr},

        {"The project tree",
         "Open a folder and the tree indexes it in the background.\n\n"
         "Sort by name, modified, size or type; add EXTRA source locations to pull folders "
         "outside the project into both the tree and the symbol cache. The 'code / project "
         "files' filter keeps images and PDFs.",
         "Navigation##projectNav", &Editor::navPanelVisible},

        {"Symbols & go-to-definition",
         "A tree-sitter indexer walks C/C++/C#/Lua/Python/Go/Rust into a kind-ranked symbol "
         "cache, refined by clangd where it's available.\n\n"
         "F12 jumps to a definition, Ctrl+T searches every symbol in the project.",
         "Symbols###symbolsPanel", &Editor::symbolsPanelVisible},

        {"Find in files",
         "Project-wide search with the same exclusions the indexer uses — Unreal's "
         "Intermediate/, Binaries/, Saved/ and friends stay out of your results.",
         "Find in Files###findInFiles", &Editor::findInFilesVisible},

        {"Sticky notes",
         "Right-click any line ▸ Add Sticky Note. Notes live in a sidecar, never in your "
         "source, and they FOLLOW their line when the code moves — each one remembers the "
         "text it was attached to.\n\n"
         "Every note is stamped with the git commit and author it was written at, so this "
         "panel reads as a history of the commentary on a file.",
         "Notes", &Editor::notesVisible},

        {"Plugins do the heavy lifting",
         "The Blueprint visual scripting graph, the live UEVR game bridge and the Unreal "
         "tooling are real DLLs — a core build links ZERO of their code.\n\n"
         "Settings ▸ Plugins toggles each at runtime. Open a .uasset and the Unreal plugin "
         "claims the binary and shows it as colored, foldable JSON.",
         nullptr, nullptr},

        {"That's the tour",
         "Everything else is in the menus: split panes, diff and 3-way merge, formatters, "
         "themes, a PDF viewer, git branch switching and a GitHub repo browser that needs no "
         "clone.\n\n"
         "Help ▸ Take the Tour brings this back any time.",
         nullptr, nullptr},
    };

    return steps;
}

void Editor::startTour()
{
    tourActive = true;
    tourIndex = 0;
}

void Editor::renderTour()
{
    if (!tourActive)
        return;

    const auto &steps = tourSteps();

    if (tourIndex < 0 || tourIndex >= (int)steps.size())
    {
        tourActive = false;
        return;
    }

    const TourStep &step = steps[tourIndex];

    // Open the panel this step is about — a tour that talks about a panel it
    // never shows is just a slideshow.
    if (step.toggle)
        this->*(step.toggle) = true;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        tourActive = false;
        return;
    }

    // Anchor the card to the window being described (and outline it), else center it.
    bool anchored = false;

    if (step.window)
    {
        if (ImGuiWindow *w = ImGui::FindWindowByName(step.window))
        {
            if (w->Active && w->Size.x > 1.0f)
            {
                ImGui::GetForegroundDrawList(w->Viewport)
                    ->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y),
                              IM_COL32(240, 200, 90, 230), 4.0f, 0, 2.5f);

                ImGui::SetNextWindowViewport(w->Viewport->ID);
                ImGui::SetNextWindowPos(ImVec2(w->Pos.x + w->Size.x + 12.0f, w->Pos.y + 12.0f),
                                        ImGuiCond_Always);
                anchored = true;
            }
        }
    }

    if (!anchored)
    {
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(240, 200, 90, 230));

    if (ImGui::Begin("##tour", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("%d / %d", tourIndex + 1, (int)steps.size());
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 34.0f);
        if (ImGui::SmallButton("x"))
            tourActive = false;

        ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.35f, 1.0f), "%s", step.title);

        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(step.body);
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginDisabled(tourIndex == 0);
        if (ImGui::Button("Back", ImVec2(80, 0)))
            tourIndex--;
        ImGui::EndDisabled();

        ImGui::SameLine();
        const bool last = tourIndex == (int)steps.size() - 1;
        if (ImGui::Button(last ? "Done" : "Next", ImVec2(80, 0)))
        {
            if (last)
                tourActive = false;
            else
                tourIndex++;
        }

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 90.0f);
        if (ImGui::Button("Skip", ImVec2(70, 0)))
            tourActive = false;
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

//
//  ── Sticky notes ───────────────────────────────────────────────────────────
//
//  Notes live in <projectRoot>/.imguiide/notes.json — inside the repo on purpose,
//  so a team can commit them and so they travel with a branch, but in a dot-dir
//  the nav panel already hides.
//

std::filesystem::path Editor::notesSidecarPath() const
{
    if (projectRoot.empty())
        return {};

    return projectRoot / ".imguiide" / "notes.json";
}

std::string Editor::notesRelPath(const std::string &filename) const
{
    std::error_code ec;
    std::filesystem::path p(filename);

    if (!projectRoot.empty())
    {
        auto rel = std::filesystem::relative(p, projectRoot, ec);
        if (!ec && !rel.empty() && rel.native().rfind(L"..", 0) != 0)
            p = rel;
    }

    // posix separators: a note written on Windows must still match on a checkout
    // elsewhere, and it keeps the sidecar diffable.
    std::string s = p.generic_string();
    return s;
}

void Editor::gitNoteStamp(std::string &commit, std::string &author) const
{
    std::string root = const_cast<Editor *>(this)->gitRoot();
    if (root.empty())
        return;

    auto capture = [&root](const std::string &args) -> std::string {
        std::string cmd = "git -C \"" + root + "\" " + args + " 2>nul";
        std::string out;

        if (FILE *p = _popen(cmd.c_str(), "r"))
        {
            char buf[512];
            while (fgets(buf, sizeof(buf), p))
                out += buf;
            _pclose(p);
        }

        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    };

    commit = capture("rev-parse --short HEAD");
    author = capture("config user.name");
}

void Editor::loadNotes()
{
    stickyNotes.clear();
    notesDirty = false;

    auto path = notesSidecarPath();
    if (path.empty())
        return;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return;

    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    stickyNotes = notes::fromJson(text);

    // Anchors are resolved lazily, per file, as tabs open (reanchorNotesFor) —
    // reading every annotated file here would stall startup on a big project.
}

void Editor::saveNotes()
{
    auto path = notesSidecarPath();
    if (path.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    if (!f.is_open())
    {
        pushToast("Notes: could not write .imguiide/notes.json", IM_COL32(230, 110, 100, 255), 3);
        return;
    }

    f << notes::toJson(stickyNotes);
    notesDirty = false;
}

int Editor::noteCountFor(const std::string &filename) const
{
    const std::string rel = notesRelPath(filename);
    int n = 0;

    for (const auto &note : stickyNotes)
    {
        if (note.file == rel && (!note.resolved || notesShowResolved))
            n++;
    }

    return n;
}

void Editor::reanchorNotesFor(TabDocument &t)
{
    const std::string rel = notesRelPath(t.filename);

    bool any = false;
    for (const auto &note : stickyNotes)
    {
        if (note.file == rel)
        {
            any = true;
            break;
        }
    }

    if (!any)
        return;

    std::vector<std::string> lines;
    const int count = t.editor.GetLineCount();
    lines.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; i++)
        lines.push_back(t.editor.GetLineText(i));

    auto before = stickyNotes;
    notes::reanchorFile(stickyNotes, rel, lines);

    // persist a note that FOLLOWED its line, so the move isn't recomputed forever
    for (size_t i = 0; i < stickyNotes.size(); i++)
    {
        if (stickyNotes[i].file == rel && stickyNotes[i].line != before[i].line)
        {
            notesDirty = true;
            break;
        }
    }
}

void Editor::applyNoteMarkers(TabDocument &t)
{
    const std::string rel = notesRelPath(t.filename);

    for (const auto &note : stickyNotes)
    {
        if (note.file != rel || (note.resolved && !notesShowResolved))
            continue;

        std::string tip = note.text;

        if (!note.author.empty() || !note.commit.empty())
        {
            tip += "\n\n\xe2\x80\x94 " + (note.author.empty() ? std::string("(unknown)") : note.author);
            if (!note.commit.empty())
                tip += " @ " + note.commit;
        }

        if (note.orphaned)
            tip += "\n(orphaned: the line this was written on is gone)";

        const ImU32 col = note.orphaned  ? IM_COL32(230, 110, 100, 255)
                          : note.resolved ? IM_COL32(120, 130, 145, 255)
                                          : IM_COL32(240, 200, 90, 255);

        t.editor.AddMarker(note.line, col, 0, "\xe2\x97\x86 note", tip);
    }
}

void Editor::openNotePopup(TabDocument &t, int line)
{
    notePendingFile = notesRelPath(t.filename);
    notePendingLine = line;
    noteEditIndex = -1;
    noteInputBuffer[0] = 0;
    ImGui::OpenPopup("Sticky Note");
}

void Editor::renderNotePopup()
{
    // Multi-viewport is on: a modal must be pinned to the focused viewport or it
    // opens on whichever monitor ImGui last felt like.
    if (ImGuiViewport *vp = ImGui::GetWindowViewport())
    {
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }

    if (!ImGui::BeginPopupModal("Sticky Note", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const bool editing = noteEditIndex >= 0 && noteEditIndex < (int)stickyNotes.size();

    ImGui::TextDisabled("%s:%d", (editing ? stickyNotes[noteEditIndex].file : notePendingFile).c_str(),
                        (editing ? stickyNotes[noteEditIndex].line : notePendingLine) + 1);
    ImGui::Spacing();

    ImGui::SetNextItemWidth(460.0f);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextMultiline("##noteText", noteInputBuffer, sizeof(noteInputBuffer),
                              ImVec2(460.0f, 90.0f));

    ImGui::Spacing();

    const bool empty = notes::trim(noteInputBuffer).empty();

    ImGui::BeginDisabled(empty);
    if (ImGui::Button(editing ? "Save" : "Add note", ImVec2(110, 0)))
    {
        if (editing)
        {
            stickyNotes[noteEditIndex].text = noteInputBuffer;
        }
        else
        {
            notes::Note note;
            note.file = notePendingFile;
            note.line = notePendingLine;
            note.text = noteInputBuffer;
            note.epoch = (long long)std::time(nullptr);

            // The anchor is what makes the note sticky: remember the LINE TEXT, so
            // when the line moves we can find it again instead of pointing at
            // whatever code happens to occupy that number later.
            if (!tabs.empty())
            {
                for (auto &tab : tabs)
                {
                    if (notesRelPath(tab->filename) == note.file)
                    {
                        note.anchor = tab->editor.GetLineText(note.line);
                        break;
                    }
                }
            }

            gitNoteStamp(note.commit, note.author);
            stickyNotes.push_back(std::move(note));
        }

        notesDirty = true;
        saveNotes();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0)))
        ImGui::CloseCurrentPopup();

    if (editing)
    {
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(110, 0)))
        {
            stickyNotes.erase(stickyNotes.begin() + noteEditIndex);
            notesDirty = true;
            saveNotes();
            noteEditIndex = -1;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndPopup();
}

void Editor::renderNotesPanel()
{
    if (!notesVisible)
        return;

    ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Notes", &notesVisible))
    {
        ImGui::End();
        return;
    }

    if (projectRoot.empty())
    {
        ImGui::TextDisabled("Open a project to keep sticky notes.");
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##notesFilter", "filter…", notesFilter, sizeof(notesFilter));
    ImGui::SameLine();
    if (ImGui::Checkbox("Show resolved", &notesShowResolved))
    {
        for (auto &tab : tabs)
        {
            refreshMarkers(*tab);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d note%s", (int)stickyNotes.size(), stickyNotes.size() == 1 ? "" : "s");

    ImGui::Separator();

    if (stickyNotes.empty())
    {
        ImGui::TextDisabled("No notes yet — right-click a line ▸ Add Sticky Note.");
        ImGui::End();
        return;
    }

    // Blame-style: notes grouped by the commit they were written against, newest
    // first. That's the whole point of stamping the commit — you can see what the
    // commentary on this code looked like at each point in its history.
    std::string lowerFilter = notesFilter;
    std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (ImGui::BeginTable("##notes", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Commit", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int eraseIndex = -1;

        for (int i = 0; i < (int)stickyNotes.size(); i++)
        {
            auto &note = stickyNotes[i];

            if (note.resolved && !notesShowResolved)
                continue;

            if (!lowerFilter.empty())
            {
                std::string hay = note.text + " " + note.file;
                std::transform(hay.begin(), hay.end(), hay.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (hay.find(lowerFilter) == std::string::npos)
                    continue;
            }

            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            if (note.orphaned)
            {
                ImGui::TextColored(ImVec4(0.90f, 0.43f, 0.39f, 1.0f), "⚠");
                ImGui::SameLine();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The line this note was written on no longer exists.");
            }

            if (note.resolved)
                ImGui::TextDisabled("%s", note.text.c_str());
            else
                ImGui::TextWrapped("%s", note.text.c_str());

            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable((note.file + ":" + std::to_string(note.line + 1)).c_str()))
            {
                std::filesystem::path abs = projectRoot / note.file;
                openFile(abs.string());
                if (!tabs.empty())
                    doc().editor.SetCursor(note.line, 0);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", note.commit.empty() ? "—" : note.commit.c_str());
            if (ImGui::IsItemHovered() && !note.author.empty())
                ImGui::SetTooltip("%s", note.author.c_str());

            ImGui::TableSetColumnIndex(3);
            bool resolved = note.resolved;
            if (ImGui::Checkbox("##res", &resolved))
            {
                note.resolved = resolved;
                notesDirty = true;
                saveNotes();
                for (auto &tab : tabs)
                {
                    refreshMarkers(*tab);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Resolved");

            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                eraseIndex = i;

            ImGui::PopID();
        }

        ImGui::EndTable();

        if (eraseIndex >= 0)
        {
            stickyNotes.erase(stickyNotes.begin() + eraseIndex);
            notesDirty = true;
            saveNotes();
            for (auto &tab : tabs)
            {
                refreshMarkers(*tab);
            }
        }
    }

    ImGui::End();
}

//
//  Editor::projectIsUnreal / liveCodingOwnsF11
//
//  Is the open project an Unreal project (a .uproject at its root)? Cached per
//  project root — this is consulted from the per-frame hotkey path, and a
//  directory scan there would be absurd.
//

bool Editor::projectIsUnreal()
{
    if (projectRoot.empty())
        return false;

    if (unrealProjectCache != projectRoot.string())
    {
        unrealProjectCache = projectRoot.string();
        unrealProjectCached = false;

        std::error_code ec;
        std::filesystem::directory_iterator it(projectRoot, ec), end;

        for (; !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec) && it->path().extension() == ".uproject")
            {
                unrealProjectCached = true;
                break;
            }
        }
    }

    return unrealProjectCached;
}

bool Editor::liveCodingOwnsF11() const
{
    return prefLiveCoding && const_cast<Editor *>(this)->projectIsUnreal();
}

void Editor::updateLargeFileMode(TabDocument &t, size_t bytes)
{
    bool large = bytes > 8u * 1024 * 1024;
    if (large == t.largeFile)
        return;
    t.largeFile = large;
    t.editor.SetFoldingEnabled(!large);
    t.editor.SetShowMatchingBrackets(!large);
    if (large)
    {
        t.trie.clear();
        pushToast("Large file \xe2\x80\x94 code intelligence disabled for speed",
                  IM_COL32(240, 200, 90, 255), 2);
    }
    else
    {
        // Shrank back under the threshold (external edit / merge) — re-arm.
        buildAutocompleteTrie(t);
    }
}

void Editor::buildAutocompleteTrie(TabDocument &t)
{
    ScopedTimer _t("buildAutocompleteTrie");
    t.trie.clear();
    if (t.largeFile)
        return; // whole-doc identifier walk + trie inserts — skipped in large-file mode
    auto language = t.editor.GetLanguage();
    if (language)
    {
        for (auto &word : language->keywords)
            t.trie.insert(word);
        for (auto &word : language->declarations)
            t.trie.insert(word);
        for (auto &word : language->identifiers)
            t.trie.insert(word);
    }
    t.editor.IterateIdentifiers([&](const std::string &id) { t.trie.insert(id); });

    // Plugins contribute language/descriptor-specific completion words (e.g. the
    // UEVR Lua API for Lua docs, or UE .uproject/.uplugin schema tokens).
    {
        PluginDocInfo info;
        info.filename = t.filename;
        info.extLower = std::filesystem::path(t.filename).extension().string();
        std::transform(info.extLower.begin(), info.extLower.end(), info.extLower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        info.languageName = language ? language->name : std::string();
        pluginRegistry.autocomplete(*this, info, [&](const std::string &w) { t.trie.insert(w); });
    }
}