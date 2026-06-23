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

### ReShade (shader editor add-on) - core unblocked, skeleton in hosts/reshade/

ReShade add-ons do not get the host's `ImGuiContext*`. Overlay drawing goes
through ReShade's **imgui function table** (`reshade_overlay.hpp`), which
covers the *public* ImGui API for one specific `IMGUI_VERSION_NUM` per ReShade
release.

The core's `imgui_internal.h` dependency is now behind a guard: configure with
`-DTE_EMBED_NO_IMGUI_INTERNAL=ON` (`TE_NO_IMGUI_INTERNAL` for raw compiles)
and the whole editor builds against the public API only - verified by
compiling with `imgui_internal.h` absent from the include path, and the
cross-binary contract test passes against the shim-built DLL. Graceful cost:
IME positioning, the scrollbar minimap, the diff view's custom synced
horizontal scrollbars, and the autocomplete popup z-order nudge
(`CalcItemSize` was replaced with a public-API replica outright).

A starting template lives in `hosts/reshade/reshade_addon.cpp` (same TEMPLATE
convention as `hosts/uevr/` - `#if 0` guarded, TODO markers). It has the add-on
scaffold ready: `DllMain` -> `register_addon` / `register_overlay` "Shader
Editor", lazy `te_bind_imgui` to ReShade's context (with the
`te_imgui_version_num()` guard), the `te_render` editor, and load/save of the
selected `.fx`. The TODOs are the version-specific ReShade bits that can only be
validated against a real ReShade install + a running game:

- enumerate the active preset's effect files via the `effect_runtime` API
  (`enumerate_techniques` -> effect name -> resolve path);
- force the edited effect to recompile after save (the `reload_effect` path);
- the build itself: compile this file + the core (`TE_NO_IMGUI_INTERNAL=1`)
  against the ReShade SDK headers at ReShade's pinned ImGui version, supplying
  ImGui via `reshade_overlay.hpp` (no bundled `imgui.cpp`), output `*.addon64`.

Because it needs hardware + a game to verify, it is intentionally left as a
fill-in-the-TODOs template rather than a CI-built target.

### Web (Emscripten)

Two hosts live in `hosts/web/`, both built to a single self-contained .html
(open straight from disk, no server):

- `main.cpp` / `build_web.sh` - minimal test host: one editor, samples,
  localStorage persistence.
- `ide_main.cpp` / `build_ide.sh` - **cloud IDE**: where the desktop shell
  walks the filesystem with std::filesystem/WinAPI, this shell talks to the
  GitHub REST API (CORS-enabled, works from any origin). Repo tree from
  `git/trees?recursive=1`, file reads via the contents API (raw media type),
  saves are commits via PUT (blob sha from the tree; 409/422 conflicts
  surfaced in the status line), branch picker, new-file, and code search
  (GitHub indexes the default branch only). Auth is a fine-grained PAT pasted
  into Settings, persisted in localStorage. The transport-free parsing layer
  (`gh_parse.h`) is unit-tested natively by `tests/gh_parse_test.cpp`.

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
