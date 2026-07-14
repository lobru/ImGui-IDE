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
- [~] Move backgroundable work OFF the UI thread. Already threaded: symbol index
      (`rebuildProjectIndex`), git poll, decompile, clone-watch. Named targets:
  - [x] **Fold detection** (b1e6f82) — the scan turned out to be pure (line text +
        comment markers only, no glyph colors), so it was extracted to
        `Folder::computeFoldRanges` and runs on a worker above 5000 lines. Apply
        (visibility) stays on the UI thread; stale/again/generation guards; the web
        build stays synchronous. selftest 474.
  - [x] **Large-file loading** (77c9a0b) — `TextEditor::SetTextAsync`: a worker builds
        and colorizes a private `Document` (it's a plain `vector<Line>`; `Colorizer`
        is stateless), `render()` swaps it in via `pollLoad()`. Files ≥1 MB open
        instantly as an empty read-only tab; trie build deferred to
        `finishPendingLoads()`. Web build stays sync. selftest 500.
  - [ ] The file READ is still synchronous — only the tokenize is off-thread. Move
        the `ifstream` read onto the worker too (needs the tab to exist before the
        bytes do).
  - [ ] Audit other per-frame walks for threadability (nav flat list is cached at
        0.5s; consider worker rebuild).

### C++ coloring
- [x] **Multi-line `#define`** (f01c3ac) — the colorizer blobbed the ENTIRE directive
      line one flat color while continuation lines tokenized as code, so a multi-line
      macro looked broken. Now only `#` + the directive word are preprocessor-colored
      and the rest of the line tokenizes normally; `#include <x>` headers are strings.

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
- [x] **Sticky notes** (8e13519) — sidecar `<project>/.imguiide/notes.json`, notes
      anchored by LINE TEXT so they follow the code when it moves (nearest-match
      re-anchor; trimmed compare survives a re-indent; honest `orphaned` flag when
      the line is gone). Gutter markers + View ▸ Notes.
- [x] **Git stamp / blame-style view** (8e13519) — each note records the commit +
      author it was written at; the Notes panel is filterable, jump-to-line,
      resolvable. 13 headless checks.

### Live coding
- [x] **Hotkey conflict** (f64e9b5) — a CHORDED F11 (Unreal Live Coding is
      Ctrl+Alt+F11) never toggles Focus Mode; with a UE project open + the new
      "Live coding owns F11" setting, bare F11 is left to Unreal too (one-time
      toast → View ▸ Focus Mode).
- [ ] Live-coding features for the app itself (hot-reload the app's own
      code/plugins) — the plugin DLLs are already shadow-copied, so a reload path
      is plausible; not started.

### UE plugin expansion
- [x] **Graphical class browser** (5fe9665) — Tools ▸ Class Browser (UEVR): the SDK
      reflection hierarchy as a real tree (prefix-folded parents, cycle-capped,
      flat hit-list when filtered); clicking a member spawns its node on the canvas.
- [ ] **Asset editing + saving** — write `.uasset` (UAssetAPI/CUE4Parse territory),
      beyond the current read-only JSON inspection.
- [ ] **.pak extraction**.
- [ ] **.ini editing** with automation/helpers (like the descriptor editor).
- [ ] **UE live bridge** — an in-editor↔running-UE bridge like the UEVR plugin's.

### Demo / tour
- [x] Feature-tour Artifact (schematics) — https://claude.ai/code/artifact/a1252d6d-df1d-460a-890e-d807fe092835
- [x] Standalone HTML export (c69d532) — `docs/feature-tour.html`, self-contained.
- [x] **Interactive tour IN the app** (64ee373) — Help ▸ Take the Tour: 7 steps that
      force-open each panel, outline it, and anchor a card to it.
- [ ] Real screenshots — **blocked, not by permission**: the app renders through
      SDL3's D3D12 swapchain and the computer-use capture path grabs it as a flat
      black rectangle (GPU-composed content isn't in that capture). Would need a
      DXGI-duplication / in-app screenshot key instead. Consider adding a "save
      screenshot" command to the app itself.

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
