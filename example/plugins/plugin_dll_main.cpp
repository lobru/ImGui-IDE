//
//  plugin_dll_main.cpp — the extern-"C" ABI shim compiled into every plugin DLL.
//
//  One file, reused by each plugin target. The concrete factory is selected at
//  compile time via -DIMGUIIDE_PLUGIN_FACTORY=<createXxxPlugin>. See plugin_abi.h.
//

#include <memory>

#include <imgui.h>

#include "plugin_abi.h"
#include "plugin_api.h"

#ifndef IMGUIIDE_PLUGIN_FACTORY
#error "define IMGUIIDE_PLUGIN_FACTORY=<factory function name> when building a plugin DLL"
#endif

// Provided by the plugin's own translation unit (e.g. createUevrPlugin()).
std::unique_ptr<EditorPlugin> IMGUIIDE_PLUGIN_FACTORY();

IMGUIIDE_PLUGIN_EXPORT uint32_t imguiide_plugin_abi_version()
{
    return IMGUIIDE_PLUGIN_ABI_VERSION;
}

IMGUIIDE_PLUGIN_EXPORT void imguiide_plugin_bootstrap(ImGuiContext *ctx, ImGuiMemAllocFunc alloc, ImGuiMemFreeFunc free, void *userData)
{
    // Point this DLL's statically-linked imgui at the host's one context + heap.
    ImGui::SetCurrentContext(ctx);
    ImGui::SetAllocatorFunctions(alloc, free, userData);
}

IMGUIIDE_PLUGIN_EXPORT EditorPlugin *imguiide_plugin_create()
{
    return IMGUIIDE_PLUGIN_FACTORY().release(); // ownership passes to the host registry
}
