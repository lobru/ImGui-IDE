# Changelog

All notable changes to ImGui-IDE. The in-app version (Help > Version) comes from
`git describe`, so release builds read their tag (e.g. `1.0`) and nightly builds
read `tag-commits-gsha`.

## Unreleased

### Added
- In-app updater with **Stable / Nightly** channels (Help > Update Channel),
  auto-check every 12 h, and in-place exe replacement (no installer) with an
  optional one-click **Restart Now**.
- Git-derived versioning (`IMGUI_IDE_VERSION`) shown in Help and About.
- Autosave (Settings > Autosave) for documents that already have a path.
- Focus mode (F11) — hides side panels + minimaps for a cheaper, distraction-free view.
- External toast API (`%APPDATA%\ImGuiColorTextEdit\toasts\`) + the
  `imgui-ide-bridge` Claude Code plugin (open-in-ide, toast).

### Fixed
- Installed build no longer loses runtime languages (`loadRuntimeLanguages` now
  resolves `<exe-dir>/languages`, not `<exe-file>/languages`).
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
