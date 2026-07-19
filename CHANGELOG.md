# Changelog

All notable changes to ImGui-IDE. The in-app version (Help > Version) comes from
`git describe`, so release builds read their tag (e.g. `1.0`) and nightly builds
read `tag-commits-gsha`.

## Unreleased

### Added
- **Debugger (DAP)**: in-app debugging with breakpoints (F9, gutter dots), a Debug
  panel (controls / call stack / expandable variables / console + REPL), stepping
  (F10/F11/Shift+F11, F5 continue), Python via debugpy out of the box, and native
  C/C++ via auto-detected vsdbg / OpenDebugAD7+gdb / lldb-dap / gdb 14+. External
  **raddbg** and **Visual Studio** launch bridges (raddbg seeded with your
  breakpoints). **Per-project debugger associations**: the Debug panel's
  Configuration section binds an adapter command and a debug target (program +
  arguments) to the *project* — overriding the per-file-type mapping — with
  file-type overrides and adapter re-detection in the same GUI; everything
  persists in settings (`[debug_adapters]`, `[debug_project_adapter]`,
  `[debug_project_target]`, `[debug_bridge]`).
- **Command palette** (Ctrl+Shift+P): fuzzy-searchable list of every app action —
  files, panels, git, debug, themes, recents. **Plugins contribute palette
  commands** through the plugin API, gated by file type and/or project type: the
  Unreal plugin's entries (build, clangd DB, launch editor, class wizard, ...)
  appear only in Unreal projects; the terminal plugin adds Terminal: Toggle.
- **Async project search**: Find in Files and Find References scan on worker
  threads (active buffer first) and stream results into clipper-rendered panels —
  no UI stalls on big trees.
- **Go to File hardening**: nested vendored SDK headers resolve (depth cap now
  prunes instead of skipping), and engine headers opened outside any project
  resolve their engine-relative includes via the owning engine tree.
- **Unreal Engine C++ + descriptor support**: UE types (`int32`, `FString`, `TArray`,
  smart pointers, math types) and macros (`UCLASS`, `UPROPERTY`, `UE_LOG`, ...) are
  syntax-highlighted; **Go to File** resolves module-relative engine includes
  (`GameFramework/Actor.h`, `*.generated.h`); `.uproject`/`.uplugin` autocomplete the
  schema, `Type`/`LoadingPhase` values, and discovered project/engine module + plugin
  names; one-click "Install IDE plugin into project".
- **`.log` language + jump-to-source**: crash/log files are severity-highlighted, and
  right-clicking a log line with a `file(line)` / `file:line` / UE `[File:][Line:]`
  reference jumps to that spot in the project.
- **Find in selection**: latch the `⊂` button in the find bar to scope Find All /
  Replace All to the current selection.
- **Unreal Engine 5 integration**: F6 builds the editor target via UnrealBuildTool
  (engine auto-discovered from the `.uproject`), Project > Unreal Engine menu with
  Build / **Generate IntelliSense DB** (clangd compile database in the project root,
  UE 5.3+) / Launch Unreal Editor, and a UE-side `ImGuiIDESourceCodeAccess` plugin so
  ImGui-IDE is selectable as the engine's source editor (Editor Prefs > Source Code).
- **In-app PDF viewer** (Windows): `.pdf` opens in a dockable window — lazy per-page
  rendering by the OS PDF engine, zoom/fit-width, middle-mouse pan.
- **Large-file mode**: files over 8 MB open instantly with whole-document
  intelligence (completion trie, LSP sync, folding, bracket matching) disabled;
  re-evaluated when the file shrinks/grows across the threshold.
- **Claude bridge installed as a real plugin** (`imgui-ide-bridge` 0.2.0): in-editor
  replies now reach Claude **automatically** via a prompt hook draining the reply
  outbox — plus `/toast`, `/open-in-ide`, `/replies` in every project.
- Nav panel filter button compacted to a glyph (tinted when filters are active).
- **Reply to Claude** feedback loop: when an external tool edits an open file, its
  changed lines get a clickable purple gutter dot. Click it (or right-click a line
  number, a row in Dev Tools > "Claude changes", or a toast) to type a message and
  **Send now** or **Add comment** for **batch** submission. Replies land in
  `%APPDATA%\ImGuiColorTextEdit\replies\` for a watcher/CLI to pick up.
- **Persistent file-type associations**: pick a language in the status-bar picker and
  that extension reopens with it (Settings `[filetypes]`).
- The per-user languages folder is **auto-populated** with editable copies of the
  bundled definitions on first run (and via Settings > Open user languages folder).
- Clone a repository from a URL, and compare the active file against a git revision.
- Clickable toasts (open the updater, or reply to Claude); Markdown preview auto-hides
  when no `.md` is focused; subword navigation/selection keybinds; find-bar prev/next.
- In-app updater with **Stable / Nightly** channels (Help > Update Channel),
  auto-check every 12 h, and in-place exe replacement (no installer) with an
  optional one-click **Restart Now**.
- Git-derived versioning (`IMGUI_IDE_VERSION`) shown in Help and About.
- Autosave (Settings > Autosave) for documents that already have a path.
- Focus mode (F11) — hides side panels + minimaps for a cheaper, distraction-free view.
- External toast API (`%APPDATA%\ImGuiColorTextEdit\toasts\`) + the
  `imgui-ide-bridge` Claude Code plugin (open-in-ide, toast).

### Fixed
- Clicking in a long line that contains tabs now lands the caret exactly under the
  click (the click path fed a pre-floored column to the snap; it now passes the true
  sub-cell position).
- Language-agnostic indent guides (VSCode-style) draw at every indent level when
  folding is enabled — brace-less languages (Lua, Python) included.
- "Go to Definition" now appears for Lua (and Go / Rust / Python / JS) symbols — the
  menu gate consulted only the grep index, never the tree-sitter index those languages
  use, so the jump was available but never offered.
- Installed build no longer loses runtime languages (`loadRuntimeLanguages` now
  resolves `<exe-dir>/languages`, not `<exe-file>/languages`). A writable per-user
  languages folder also lets read-only (Program Files) installs be extended/overridden.
- Git revision / clone-URL fields reject a double-quote that would break the shell
  command. Updater rejects truncated downloads (anti-brick).
- Nav no longer indexes the process CWD (e.g. System32) when launched without a
  project; roots at the active document's folder or shows a hint.
- Symbols project filter cached (no per-frame rescan); nav directory listings
  cached; removed per-row `weakly_canonical` syscalls that pinned large dirs at ~1 fps.
- Quit prompt lists the specific unsaved documents.
- Status-bar trailing cluster right-aligns without overlapping the left cluster.

## 1.0

- First tagged release of ImGui-IDE (fork of goossens/ImGuiColorTextEdit + Dear
  ImGui docking + SDL3 + tree-sitter): multi-tab docking, project nav tree,
  tree-sitter symbols / go-to-def, clangd intelligence, folding, themes,
  Windows installer + Explorer integration.
