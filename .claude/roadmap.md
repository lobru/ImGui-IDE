# Editor roadmap

Bugs are fixed first, then features in priority order. Each item is sized so a
single iteration can land it; bigger items have explicit substeps.

The whole 2026-05 roadmap (drafted features + high/med/low backlog) has shipped
— see the **Shipped** changelog below. What follows in **Open** is everything
that is still genuinely outstanding.

## 🔧 Open

### Bugs needing a Windows repro
- [ ] **Cursor X mis-alignment** on long lines containing tabs. Repro: open a
      `.md` with tabs, click in the middle of a long line — cursor lands past
      the click point. The proportional-font column↔pixel rework
      (`normalizeCoordinate`, `glyphSize.x`) may have changed this; retest
      before chasing. If still present, add a temporary
      `assert(column == cursor.column)` around the click→coordinate roundtrip
      (`normalizeCoordinate` at the mouse-pos call site) to localise whether
      the drift is in the pixel→column map or the tab-stop normalisation.
- [x] **Fold preview drawn on its own row** — confirmed fixed (done long ago;
      the spurious top-level Python indent folds that caused it no longer exist
      after the indent-fold rewrite). Preview draws inline at
      `cursorScreenPos.x + textOffset + (maxColumn+1)*glyphSize.x`.

### Polish (self-contained, no build needed to write)
- (none open)

## Process notes

- Project is `/W4 /WX` MSVC. Watch out for: `int → float` narrowing,
  signed/unsigned compares, unused params/locals.
- After ImGui dep changes, run `.\build -Clean` once; otherwise incremental
  rebuilds are fine.
- The Windows v1.92.7-docking branch of Dear ImGui is in use; multiviewport is
  enabled, so any popup must be routed through `SetNextWindowViewport` if we
  want it to land on the right monitor.

---

## ✅ Shipped (changelog)

### 2026-06-11
- Distinct `///` doc-comment fold preview (`FoldRange::docComment`, set in
  `flushLineCommentBlock`, branched in the render switch → `" ///..."`).
- Nav panel: directory listings cached (TTL + dirty-flag) instead of walking the
  filesystem every frame; nameless picker + right-aligned path w/ tooltip; filter
  below the separator.
- ReShade/HLSL symbol support + highlighting (`.fx .fxh .addonfx .hlsl .hlsli`).
- Middle-mouse pan in Markdown preview / Image viewer; Dev Tools pan fixed.
- Context menus no longer draggable (`NoMove`). "Open to Left/Right" at the split
  limit docks into the correct existing pane. Status-bar trailing cluster now
  right-aligns to the true edge.

### Editor / nav features (built since the 2026-05-12 draft)
- Navigation panel — dockable project file-tree (View toggle), open-on-click,
  image-thumbnail hover tooltips.
- Split current tab side-by-side (View → Split Right, `DockBuilderSplitNode` +
  `wantSplit`).
- Project overview launcher — `example.exe --project <dir>` and File → Open
  Project… open the nav panel rooted at a dir without auto-opening a tab.
- Diff two arbitrary files / File History window.
- Settings dialog with Interpreters tab (`interpreterForExt`, `settings.json`).
- MSVC / .NET toolchain version banner (`VCToolsVersion`, `dotnet --version`).
- Sticky docking (`PassthruCentralNode`).
- Autocomplete timings instrumentation (`steady_clock`).
- Run / Run with Arguments for any script or exe from the nav tree.
- Lua go-to-def (incl. `local` declaration sites), C# "Go to Decompiled
  Source" via ilspycmd.
- Proportional-font text layout (column↔pixel mapping); non-monospace warning;
  monospace-first font picker.
- Live match-count readout in the find bar.
- Folding no longer triggers a full document re-parse.
- File-menu polish (padding, font oversample, Exit ordering).

### 2026-05-11 (round 3) — feature batch
- DLL packaging via `option(TEXTEDITOR_BUILD_SHARED)` (`build.ps1 -Shared`).
- Runtime language definitions — `Language::FromFile(path)` parses a
  `key=value` format; six starter `.lang` files (HTML, INI, YAML, CFG, BAT,
  PowerShell) auto-loaded by `loadRuntimeLanguages()`.
- File-dialog favourites + drives sidebar (ImGuiFileDialog Places feature).
- Script runner — F5 saves + runs by extension, output to a dockable window.

### 2026-05-11 (round 2)
- `///` / `//` docstring fold — runs of ≥3 single-line-comment lines fold as
  `Comment` (preview polish still open, see above).
- Header/Source toggle (Alt+O), with sibling-extension cycling.
- `#include` → Go to file context-menu hook.

### 2026-05-11
- Scroll-to-bottom on fold-preview click fixed (visual-index snap for hidden
  lines).
- `deindentLines` trailing-line over-process fixed.
- File dialog viewport pinning fixed (`dialogNeedsPlacement` gate).
- Subword move params unified across platforms.
- Click-and-drag for selected text (move/copy, single undo transaction).
- Chord shortcut queue (`Ctrl+K Ctrl+U/L/0/J`).
- SDL3 drop event API rename (`SDL_EVENT_DROP_FILE` / `event.drop.data`).

### 2026-05-10 (round 2)
- Click-Y double-scroll fixed (dropped stray `+ GetScrollY()`).
- Python indent folding rewritten (open only on strict indent increase).
- `updateVisibility` indent folds hide `[start+1, end]` inclusive.
- Per-fold-type preview indicators.
- Fold arrow visuals + double-click-to-select-range.
- Ctrl+Shift+T reopens last closed tab (32-deep).
- File dialog non-modal + multi-viewport.

### 2026-05-10 (round 1)
- Tab ↔ Shift+Tab round-trip.
- Modal/file dialogs open on the focused viewport.
- Editing inside a folded region auto-unfolds it.
