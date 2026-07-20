//
//  plugin_loader.cpp — see plugin_loader.h.
//

#include "plugin_loader.h"

#include <imgui.h>

#include "plugin_abi.h"
#include "plugin_api.h"
#include "plugin_registry.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace
{
// Platform shims. A successfully loaded handle is intentionally kept resident
// (never freed) so the plugin's code + vtable stay mapped for the whole process;
// the OS reclaims it at exit, after the registry has destroyed the plugin object.
#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle openLib(const std::filesystem::path &p) { return LoadLibraryW(p.wstring().c_str()); }
void *findSym(LibHandle h, const char *name) { return reinterpret_cast<void *>(GetProcAddress(h, name)); }
void closeLib(LibHandle h) { FreeLibrary(h); }
bool isPluginFile(const std::filesystem::path &p) { return p.extension() == ".dll"; }
unsigned long currentPid() { return GetCurrentProcessId(); }
#else
using LibHandle = void *;
LibHandle openLib(const std::filesystem::path &p) { return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); }
void *findSym(LibHandle h, const char *name) { return dlsym(h, name); }
void closeLib(LibHandle h) { dlclose(h); }
bool isPluginFile(const std::filesystem::path &p)
{
    auto e = p.extension();
    return e == ".so" || e == ".dylib";
}
unsigned long currentPid() { return static_cast<unsigned long>(::getpid()); }
#endif

// The dir prefix for our per-process shadow copies (see shadowCopyDir).
const std::string kShadowPrefix = "imguiide-plugins-";

// A private, per-process directory under the system temp dir into which every
// plugin DLL is copied before loading. Loading the COPY (not the build output)
// means the running app never holds an OS lock on plugins/<name>.dll, so a
// concurrent `ninja` can overwrite it — no more LNK1168 "cannot open ... for
// writing" while the IDE is up. Returns an empty path if temp is unusable, in
// which case the caller falls back to loading in place.
std::filesystem::path shadowCopyDir()
{
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
        return {};

    // Sweep stale shadow dirs from earlier runs first. A dir whose DLLs are still
    // mapped by a LIVE process can't be removed (the files stay locked), so this
    // naturally leaves other running instances' copies alone and reaps only dead
    // ones — no PID-liveness probing needed.
    for (auto it = std::filesystem::directory_iterator(base, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        std::error_code fec;
        if (it->is_directory(fec) && !fec &&
            it->path().filename().string().rfind(kShadowPrefix, 0) == 0)
        {
            std::error_code rec;
            std::filesystem::remove_all(it->path(), rec); // best effort; live dirs fail
        }
    }

    std::filesystem::path dir = base / (kShadowPrefix + std::to_string(currentPid()));
    std::filesystem::remove_all(dir, ec); // clear a leftover from a recycled PID
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return {};
    return dir;
}
} // namespace

void loadPluginDLLs(PluginRegistry &registry, const std::filesystem::path &pluginsDir,
                    const std::function<void(const std::string &)> &log)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(pluginsDir, ec))
        return; // no plugins/ dir → nothing to load (a clean core build)

    // Fetch the host's live imgui context + allocators once; every DLL is booted
    // onto the same pair so it shares our single context and heap.
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    ImGuiMemAllocFunc allocFn = nullptr;
    ImGuiMemFreeFunc freeFn = nullptr;
    void *allocUd = nullptr;
    ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &allocUd);

    // Manual increment with an error_code: a range-for over directory_iterator only
    // passes ec to construction — its operator++ still THROWS. plugins/ is exactly the
    // kind of directory that can change under us (a concurrent build writing .dll/.pdb),
    // so iterate the fully non-throwing way, with a separate ec per entry so one bad
    // entry doesn't abort the whole scan.
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::directory_iterator(pluginsDir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        std::error_code fec;
        if (it->is_regular_file(fec) && !fec && isPluginFile(it->path()))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end()); // deterministic load order

    // Load from a private shadow copy so the build-output DLLs stay unlocked while
    // the app runs (see shadowCopyDir). If temp is unusable, shadowDir is empty and
    // we load each plugin in place — correct, just re-lockable by a concurrent build.
    std::filesystem::path shadowDir = shadowCopyDir();

    for (auto &file : files)
    {
        std::string name = file.filename().string();

        std::filesystem::path loadPath = file;
        if (!shadowDir.empty())
        {
            std::error_code cec;
            std::filesystem::path copy = shadowDir / file.filename();
            std::filesystem::copy_file(file, copy, std::filesystem::copy_options::overwrite_existing, cec);
            if (!cec)
                loadPath = copy; // load the copy; original left unlocked for rebuilds
        }

        LibHandle h = openLib(loadPath);
        if (!h)
        {
            log("plugin: failed to load " + name);
            continue;
        }

        auto versionFn = reinterpret_cast<ImguiidePluginAbiVersionFn>(findSym(h, IMGUIIDE_PLUGIN_ABI_VERSION_SYMBOL));
        auto bootstrapFn = reinterpret_cast<ImguiidePluginBootstrapFn>(findSym(h, IMGUIIDE_PLUGIN_BOOTSTRAP_SYMBOL));
        auto createFn = reinterpret_cast<ImguiidePluginCreateFn>(findSym(h, IMGUIIDE_PLUGIN_CREATE_SYMBOL));
        if (!versionFn || !bootstrapFn || !createFn)
        {
            log("plugin: " + name + " is not an ImGui-IDE plugin (missing ABI exports) — skipped");
            closeLib(h);
            continue;
        }

        uint32_t version = versionFn();
        if (version != IMGUIIDE_PLUGIN_ABI_VERSION)
        {
            log("plugin: " + name + " ABI v" + std::to_string(version) + " != host v" +
                std::to_string(IMGUIIDE_PLUGIN_ABI_VERSION) + " — skipped");
            closeLib(h);
            continue;
        }

        bootstrapFn(ctx, allocFn, freeFn, allocUd);

        EditorPlugin *plugin = createFn();
        if (!plugin)
        {
            log("plugin: " + name + " create() returned null — skipped");
            closeLib(h);
            continue;
        }

        std::string id = plugin->id();
        registry.add(std::unique_ptr<EditorPlugin>(plugin)); // handle stays resident
        log("plugin: loaded '" + id + "' (" + name + ")");
    }
}
