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
#endif
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

    for (auto &file : files)
    {
        std::string name = file.filename().string();
        LibHandle h = openLib(file);
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
