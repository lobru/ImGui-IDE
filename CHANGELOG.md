# Changelog

All notable changes to ImGui-IDE. The in-app version (Help > Version) comes from
`git describe`, so release builds read their tag (e.g. `1.0`) and nightly builds
read `tag-commits-gsha`.

## Unreleased

### Added
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
