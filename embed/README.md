# TextEditor embed DLL

The TextEditor core (syntax-highlighting editor widget, `TextEditor.cpp/.h` +
`TextDiff`) packaged as a shared library with a **flat C API**, for embedding a
code editor inside another ImGui application: ReShade add-ons, UEVR plugins,
game overlays, tools.

```
embed/
  CMakeLists.txt        standalone build (does not touch the desktop IDE build)
  texteditor_embed.h    the C API - the only header a host needs
  texteditor_embed.cpp  implementation
  hosts/                per-host integration templates
```

## Build

```
cmake -B build-embed -S embed
cmake --build build-embed --config Release
```

Produces `texteditor_embed.dll` (Windows) / `libtexteditor_embed.so` (Linux).
The DLL compiles its own copy of Dear ImGui (widgets only, **no backends**) and
draws into the *host's* ImGui context.

**The ImGui version must match the host's.** Pick it at configure time:

```
-DTE_EMBED_IMGUI_TAG=v1.92.7-docking     # git tag to fetch (default)
-DTE_EMBED_IMGUI_DIR=C:/src/imgui        # or: a local checkout (takes priority)
```

Verify at runtime before rendering:

```c
if (te_imgui_version_num() != IMGUI_VERSION_NUM) {
    /* refuse to render; log both numbers */
}
```

This is the documented Dear ImGui cross-DLL pattern (see "How can I use this
in a DLL?" in the ImGui FAQ): each binary compiles its own ImGui code, but they
share one context and one set of allocators. Same version, same config defines
on both sides - then it is safe.

## Minimal host (host owns the ImGui context)

```c
#include "texteditor_embed.h"

/* once, after the host's ImGui context exists */
ImGuiMemAllocFunc alloc; ImGuiMemFreeFunc free_fn; void* ud;
ImGui::GetAllocatorFunctions(&alloc, &free_fn, &ud);
te_bind_imgui(ImGui::GetCurrentContext(), alloc, free_fn, ud);

te_editor* ed = te_create();
te_set_language(ed, TE_LANG_HLSL);
te_set_palette(ed, TE_PALETTE_DARK);
te_set_text(ed, source, (size_t)-1);
te_mark_saved(ed);

/* every frame, inside an ImGui window the host began */
te_render(ed, "##shader_source", 0, 0, 0);   /* 0,0 = fill available region */

if (te_is_dirty(ed)) { /* show the dirty dot, enable Save */ }

/* Save: size, fetch, write, mark clean */
size_t n = te_get_text(ed, NULL, 0);
char* buf = malloc(n + 1);
te_get_text(ed, buf, n + 1);
/* ... write buf to disk / recompile shader ... */
te_mark_saved(ed);
```

Strings are UTF-8 throughout. Lines/columns are 0-based. All calls must come
from the host's render thread. `te_render` no-ops safely if `te_bind_imgui`
was never called, so a missing bind shows as "editor doesn't appear", not a
crash.

## Host notes

### UEVR (script editor in a plugin)

UEVR C++ plugins (PluginV1 API, see praydog/UEVR-ExamplePlugin) typically own
their **entire** ImGui lifecycle: the plugin compiles ImGui, creates the
context, initializes the DX11/DX12 backend against the game's device from
UEVR's render callbacks, and calls NewFrame/Render itself. That is also fine
for the embed DLL - bind the *plugin's* context instead of some host app's:

1. Build `texteditor_embed.dll` with `TE_EMBED_IMGUI_DIR` pointing at the same
   ImGui checkout the plugin compiles.
2. After the plugin calls `ImGui::CreateContext()`, do the `te_bind_imgui`
   dance from the snippet above (the plugin is "the host").
3. Call `te_render` inside the plugin's UI window in `on_draw_ui` (or its own
   draw pass). Set `TE_LANG_LUA` for UEVR scripting.
4. If the device resets and the plugin recreates the context, call
   `te_bind_imgui` again with the new context.

A skeleton lives in `hosts/uevr/`. It is a template against the
ExamplePlugin layout, not a drop-in build - check the TODO markers.

### ReShade (shader editor add-on) - known constraint, planned work

ReShade add-ons do not get the host's `ImGuiContext*`. Overlay drawing goes
through ReShade's **imgui function table** (`reshade_overlay.hpp`), which
covers the *public* ImGui API for one specific `IMGUI_VERSION_NUM` per ReShade
release. The TextEditor core currently calls three things from
`imgui_internal.h`:

- `ImGui::CalcItemSize` (size resolution at render start)
- `ImGui::GetCurrentWindow()` (IME viewport id + scroll state)
- `ImGui::BringWindowToDisplayFront` (autocomplete popup z-order)

Until those are shimmed behind a `TE_NO_IMGUI_INTERNAL` guard in the core (so
the whole editor compiles against the function table), the embed DLL **cannot
draw inside a ReShade overlay**. That guard is the planned next step on this
track; the C API here will not change for it. ReShade's effect-runtime API
(enumerate techniques, get effect source paths, force reload after save) is
otherwise a clean fit for `te_*` + file IO on the add-on side.

### Web (Emscripten)

The core builds with plain C++17 + ImGui and has no platform code, so an
Emscripten build of this same target (as a static lib into a wasm host that
brings its own ImGui+WebGL backend) is expected to work; the git-API-powered
web IDE host is a separate track.

## API stability

`TE_EMBED_VERSION` is semver; the C surface in `texteditor_embed.h` only grows
within a major version. The C++ classes inside the DLL are deliberately **not**
exported - the C API is the only contract.

## Verified

- Linux: builds clean with `-Werror -Wall -Wpedantic -Wextra` (GCC 11),
  exports exactly the `te_*` surface (`nm -D`), and passes a `dlopen` smoke
  test: text roundtrip + two-call sizing + truncation, line count, dirty
  tracking, markers, safe `te_render` no-op without a bound context.
- Windows/MSVC: built by the same CMake project; add it to CI alongside the
  desktop IDE build (planned).
