//
//  plugin_abi.h — the C ABI boundary a runtime-loaded plugin DLL exports.
//
//  ImGui-IDE can load feature plugins as DLLs at startup (see plugin_loader.cpp)
//  in addition to the ones compiled straight into the exe. A plugin DLL keeps
//  the same C++ EditorPlugin / PluginHost interface (plugin_api.h) — this file
//  only adds the tiny extern-"C" bridge the host uses to find and boot it:
//
//    uint32_t      imguiide_plugin_abi_version();
//    void          imguiide_plugin_bootstrap(ImGuiContext*, ImGuiMemAllocFunc,
//                                             ImGuiMemFreeFunc, void* userData);
//    EditorPlugin* imguiide_plugin_create();
//
//  Both halves are built by the same toolchain from the same headers, so the C++
//  vtable of EditorPlugin is ABI-compatible across the boundary; the version
//  number below guards against a plugin DLL built against an older interface.
//
//  imgui is linked statically into each plugin DLL, so the DLL has its own GImGui.
//  bootstrap() hands the DLL the host's context + allocators (SetCurrentContext +
//  SetAllocatorFunctions) so both draw into the one shared context and heap — the
//  standard "ImGui across a DLL boundary" pattern.
//

#pragma once

#include <cstdint>

#include <imgui.h> // ImGuiContext, ImGuiMemAllocFunc, ImGuiMemFreeFunc

class EditorPlugin;

// Bump whenever PluginHost / EditorPlugin's layout or the exports below change.
//  2: EditorPlugin gained contributeKeybinds / onDocumentContextMenu /
//     contributePaletteCommands virtuals (2026-07-19) — vtable slots shifted,
//     so a v1 DLL under a v2 host dispatches into the WRONG functions (this
//     exact mismatch crashed the installed build with stale plugin DLLs).
//  3: PluginHost gained hostReplaceSelection (2026-07-20, appended last).
//  4: PluginHost gained hostConfigDir/hostActiveCursorLine/hostJumpTo/
//     hostRefreshMarkers/hostFindBuiltExe; EditorPlugin gained
//     contributeMarkers (2026-07-20, all appended — the debugger is a plugin).
#define IMGUIIDE_PLUGIN_ABI_VERSION 4u

// Exact export names the loader resolves. Keep in sync with plugin_dll_main.cpp.
#define IMGUIIDE_PLUGIN_ABI_VERSION_SYMBOL "imguiide_plugin_abi_version"
#define IMGUIIDE_PLUGIN_BOOTSTRAP_SYMBOL "imguiide_plugin_bootstrap"
#define IMGUIIDE_PLUGIN_CREATE_SYMBOL "imguiide_plugin_create"

#if defined(_WIN32)
#define IMGUIIDE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define IMGUIIDE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Function-pointer types matching the exports (used by the host-side loader).
extern "C"
{
    typedef uint32_t (*ImguiidePluginAbiVersionFn)();
    typedef void (*ImguiidePluginBootstrapFn)(ImGuiContext *, ImGuiMemAllocFunc, ImGuiMemFreeFunc, void *);
    typedef EditorPlugin *(*ImguiidePluginCreateFn)();
}
