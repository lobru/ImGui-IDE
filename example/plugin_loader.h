//
//  plugin_loader.h — loads feature plugins packaged as DLLs at startup.
//
//  Scans a plugins directory for shared libraries and, for each valid one,
//  verifies the ABI version, hands it the host's imgui context + allocators,
//  constructs its EditorPlugin, and adds it to the registry. DLL plugins and
//  plugins compiled straight into the exe (registerBuiltinPlugins) coexist in
//  the same registry and share every downstream code path (menus, Settings
//  toggle, persistence).
//

#pragma once

#include <filesystem>
#include <functional>
#include <string>

class PluginRegistry;

// Load every plugin DLL found under `pluginsDir`. `log` receives one
// human-readable line per plugin (loaded / skipped and why) for startup logging.
// Successfully loaded modules stay resident for the process lifetime. Absent
// directory is a no-op, not an error — that's simply a plugin-less core build.
void loadPluginDLLs(PluginRegistry &registry, const std::filesystem::path &pluginsDir,
                    const std::function<void(const std::string &)> &log);
