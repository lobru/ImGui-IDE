# Editor roadmap

Bugs are fixed first, then features in priority order. Each item is sized so a
single iteration can land it; bigger items have explicit substeps.

The whole 2026-05 roadmap (drafted features + high/med/low backlog) has shipped
— see the **Shipped** changelog below. What follows in **Open** is everything
that is still genuinely outstanding.

## 🔧 Open

### Bugs needing a Windows repro
- [x] **Cursor X mis-alignment** on long lines containing tabs — FIXED
      (2026-06-26, `7db583b`). Root cause: the cursor-placement click path
      floored the visual column via int `xToColumn` before `normalizeCoordinate`,
      dropping the sub-cell offset; on wide tab cells that biased the caret off
      the click. The hover/word path already passed a fractional column — the
      cursor path (and the word-wrap branch) now do too (monospace = exact
      `x/glyphSize.x`, proportional keeps the measured walk). New
      `CaretColumnAtVisual()` + 5 headless selftest cases lock the snap math.
      Runtime click-accuracy on a tabbed long line is the final visual confirm.
- [x] **Fold preview drawn on its own row** — confirmed fixed (done long ago;
      the spurious top-level Python indent folds that caused it no longer exist
      after the indent-fold rewrite). Preview draws inline at
      `cursorScreenPos.x + textOffset + (maxColumn+1)*glyphSize.x`.

### Polish (self-contained, no build needed to write)
- (none open)

## 🚀 User backlog (2026-07-13) — big requests, capture so nothing is lost

Status: [ ] not started · [~] in progress/partial · [x] done. Items sized for one
iteration where possible; big ones have substeps.

### Performance — worker threads (MAJOR)
- [ ] Move backgroundable work OFF the UI thread. Already threaded: symbol index
      (`rebuildProjectIndex`), git poll, decompile, clone-watch. Named targets:
  - [ ] **Fold detection** — compute fold ranges on a worker, publish to the doc
        (careful: folds feed rendering; needs a snapshot/version handshake like
        the symbol index).
  - [ ] **Large-file splitting/loading** — chunked/threaded load so opening a huge
        file doesn't stall the frame (there's a large-file mode already; make the
        initial read + tokenize incremental/off-thread).
  - [ ] Audit other per-frame walks for threadability (nav flat list is cached at
        0.5s; consider worker rebuild).

### Nav panel UX
- [x] **Sort + Add-Source-Location inline in the nav header** (7e85e73) — compact
      "Sort" menu + "+Dir" buttons in the button row; popup keeps checkboxes +
      manage-list only.

### Installer / updater / registry
- [x] **Registry path** (a1ca0fe) — HKCU\Software\ImGui-IDE ExePath + InstallDir on
      startup, LastProject on project switch.
- [ ] **Update the installer** to match current build + read/write that registry key.
- [ ] **Cut a release + push the in-app updater feed** so Help ▸ Check for Updates
      sees the new build.

### AI Claude plugin + reply feature (VERIFY END-TO-END)
- [ ] The send-to-Claude / reply-to-Claude bridge exists (hostSendToClaude, the
      reply loop) but we've NEVER actually run it live. Set it up, wire it to a
      real Claude endpoint/inbox, and confirm the round trip works.

### Persistent comments / sticky notes
- [ ] Comments/annotations saved to a sidecar file, persisting as **sticky notes**
      anchored to lines.
- [ ] Plug into git: associate notes with commits → a **blame-style** annotation
      view.

### Live coding
- [ ] Live-coding features for the app itself (hot-reload the app's own
      code/plugins) and for Unreal.
- [ ] **Hotkey conflict**: when a UE project is open AND live coding is enabled,
      pressing the live-coding hotkeys must NOT also toggle Focus Mode. (Needs a
      live-coding-enabled flag + hotkey gating.)

### UE plugin expansion
- [ ] **Asset editing + saving** — write `.uasset` (UAssetAPI/CUE4Parse territory),
      beyond the current read-only JSON inspection.
- [ ] **.pak extraction**.
- [ ] **.ini editing** with automation/helpers (like the descriptor editor).
- [ ] **UE live bridge** — an in-editor↔running-UE bridge like the UEVR plugin's.
- [ ] **Graphical class browser plugin** — browse the reflection/class hierarchy.
- [ ] More UE-side work as it comes up.

### Demo / tour
- [x] Feature-tour Artifact (schematics) — https://claude.ai/code/artifact/a1252d6d-df1d-460a-890e-d807fe092835
- [ ] Real screenshots (blocked: needs the app staged front/maximized/project-open;
      declined/unstaged during the 2026-07-13 attempt).
- [ ] **Interactive tour IN the app** (or via linking) — guided highlights of
      features from inside ImGui-IDE.

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
