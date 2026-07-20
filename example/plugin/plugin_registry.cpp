//
//  plugin_registry.cpp — constructs the plugins compiled into this build.
//
//  Each plugin is behind its own IMGUIIDE_PLUGIN_* CMake flag (default ON). A
//  core build with every flag off compiles none of the plugin code and this
//  function adds nothing, leaving a clean plugin-less editor.
//

#include "plugin_registry.h"

#ifdef IMGUIIDE_PLUGIN_UNREAL
std::unique_ptr<EditorPlugin> createUnrealPlugin();
#endif

#ifdef IMGUIIDE_PLUGIN_UEVR
std::unique_ptr<EditorPlugin> createUevrPlugin();
#endif

void registerBuiltinPlugins(PluginRegistry &registry)
{
#ifdef IMGUIIDE_PLUGIN_UNREAL
    registry.add(createUnrealPlugin());
#endif

#ifdef IMGUIIDE_PLUGIN_UEVR
    registry.add(createUevrPlugin());
#endif

    (void)registry;
}
