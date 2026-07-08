# ImGui-IDE ↔ UEVR bridge plugin

A tiny, **headless** UEVR plugin that lets ImGui-IDE's **UEVR Live** panel run
Lua inside a running UEVR game — a REPL plus live Globals / Modules / Inspect —
without the IDE and the game sharing a process.

## How it works

The IDE and this plugin talk over a file inbox, no sockets:

```
%APPDATA%\UnrealVRMod\UEVR\ide_bridge\
    cmd\   ← ImGui-IDE writes command files here
    out\   ← this plugin writes results here
```

Each command file's first line is a kind (`run` | `globals` | `modules` |
`inspect`); the rest is the payload (Lua code, or an expression to inspect).
On every ~12th engine tick the plugin drains `cmd\`, runs each request through
UEVR's `exec_lua_chunk`, writes the result to `out\<reqId>.txt`, and deletes the
command. The IDE polls `out\` at ~5 Hz and shows the results.

With no game running, the IDE's sends simply accumulate in `cmd\` and nothing
comes back — everything is best-effort on both ends.

## Build

Needs only the UEVR SDK headers (`<UEVR>/include/uevr/Plugin.hpp`). Easiest is
to build it inside your UEVR checkout the same way as the bundled examples:

```
cmake -S tools/uevr-bridge -B build -DUEVR_ROOT=<path-to-UEVR-checkout>
cmake --build build --config Release
```

This produces `imgui_ide_uevr_bridge.dll`.

## Install

Copy `imgui_ide_uevr_bridge.dll` into UEVR's plugins folder for your game
(alongside `UEVRBackend.dll`), or drop it where UEVR loads per-game plugins.
Launch the game with UEVR, then in ImGui-IDE open **View → UEVR Live (bridge)**
and Run / Refresh. (ImGui-IDE can also install it for you from the Blueprint /
UEVR menu once bundled next to the exe.)

> Requires a UEVR build whose `exec_lua_chunk` is available. If the plugin logs
> "exec_lua_chunk unavailable — rebuild UEVRBackend", update UEVR.
