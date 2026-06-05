# ImGuiColorTextEdit → "IDE-lite" — Status Update

**Date:** 2026-06-05
**Repo:** github.com/lobotomy-x/ImGuiColorTextEdit (branch `lobotomy/main`)
**Stack:** C++17 · Dear ImGui 1.92 (docking + multi-viewport) · SDL3 + SDLGPU3 · MSVC `/W4 /WX`
**Scale:** ~26k LOC across the core files · 678 commits on the fork

## What it is

A heavily-extended fork of `goossens/ImGuiColorTextEdit` that turns a syntax-highlighting
text-widget demo into a lightweight, self-hosting IDE. It builds itself: the editor is
developed inside the editor, with a "where is this feature's code" map that jumps to the
function that draws whatever you're looking at.

The long game is an embeddable editor core (the portable `TextEditor.cpp/.h` widget) that can
drop into other host apps — eventual ReShade / UEVR overlay builds — with the app shell
(`example/`) as the full desktop IDE.

## What I built this cycle

### Editor & language tooling
- **Project navigation** tree with filters (dotfiles / code-only / excluded / flat), collapse &
  expand-all, per-file context menu, and inline rename / delete (recycle-bin safe).
- **Go-to-definition & find-references**, project-wide and grep-based (no LSP dependency), plus
  **Go-to-File** that self-discovers MSVC + Windows SDK include paths from vswhere and the
  registry — no Developer Command Prompt required.
- **Find in Files** — project-wide search panel.
- **Format Document** via clang-format with per-language brace styles, applied undo-safe.
- **Folding** for braces, Python indents, Lua keyword blocks, INI sections, C# `#region`; plus
  preprocessor highlight carried across backslash-continued lines.
- **Word wrap**, column/rectangular selection, image viewer, and a script/exe runner with
  Python venv auto-detection.
- **`.editorconfig` cascade** — per-project / per-language indent & wrap settings.

### Git integration
- Status bar branch indicator (dirty / ahead / behind), background-polled off the UI thread.
- Git menu: fetch / pull / commit / push / discard / status, plus diff-against-file.

### "Safe co-editing with an AI agent" (the headline feature)
A workflow where an external agent (e.g. Claude) edits the same files on disk while you have
them open — without losing work or losing track of who changed what:
- **External-change detection** — throttled mtime poll; clean buffers auto-reload, dirty buffers
  raise a conflict bar.
- **3-way merge** of your unsaved edits against the on-disk version over the last reconciled base
  (diff3 with conflict markers; large-file fallback).
- **Visibility layer** — fading corner toasts, a tab badge until you view the change, violet
  gutter markers on the exact changed lines (LCS diff, prefix/suffix-trimmed for speed), and a
  persistent dockable "External Changes" activity feed.

### Reliability & performance
- **Crash capture** — CRT report hook + `std::set_terminate` + SEH filter route asserts /
  exceptions / access violations to a `crash.log`, turning an un-repro'able shutdown assert into
  a self-reporting one.
- **Frame-time watchdog** — measures per-frame UI-build cost, tracks a rolling worst + slow-frame
  count, surfaced in Dev Tools and logged when over the 30 fps budget.
- Earlier perf work: O(1) visual-index fold mapping, debounced post-edit reparse (fixed a
  240→30 fps typing cliff in large files), axis-snap pan/scroll.

### UX polish
- VS-style **tab context menu**: close / close others / close left / close right (scoped to the
  tab's own dock node), split left/right, diff-with-current, reveal in explorer, copy path —
  built on ImGui internals (`DockTabItemRect`, dock-node tab bars) with right-click no longer
  stealing the active tab, and mouse-wheel scrolling the tab strip.
- Multi-viewport-aware dialogs pinned to the main viewport; rebindable two-stroke keybind system
  with a persistent app-level registry; status bar with language + toolchain selectors and an
  FPS readout.

## Notable engineering challenges solved
- **Docking-tab-bar customization** required reading ImGui internals (`ImGuiWindowTempData`,
  `ImGuiDockNode`, `ImGuiTabBar`) since the public API doesn't expose per-tab context menus —
  including pinning down that ImGui re-selects a tab on *both* mouse-down and mouse-release.
- **Concurrent-edit correctness** — picking the right 3-way merge base (a dedicated `syncedText`
  snapshot, not "content at open"), guarding self-saves with mtime re-baselining plus a
  byte-compare backstop, and clamping the restored cursor so a shrunk file can't trigger an
  out-of-range assert.
- **Self-discovering toolchains** so the app works when launched from Explorer, not just a dev
  prompt.

## Current state
Core IDE features ship and the build is green (`/W4 /WX`); a headless self-test guards the
non-UI logic (colorizer, folding, language mapping). Verification of visual/cursor features is
runtime-owned — they're confirmed in the running app, not just by "it compiles."

**Open / in progress:** a working-tree reformat of the app shell needs review (a co-edit merge
appears to have collapsed some menu structure); ImGui Test Engine for headless GUI testing;
deeper agent integration tied to a live session.

## Roadmap
1. Tighter live-agent integration (attribution beyond on-disk diffing).
2. Emscripten + embeddable-DLL builds toward the ReShade / UEVR overlay targets.
3. Headless GUI test coverage.
