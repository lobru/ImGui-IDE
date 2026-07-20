# Editor roadmap

Bugs are fixed first, then features in priority order. Each item is sized so a
single iteration can land it; bigger items have explicit substeps.

The whole 2026-05 roadmap (drafted features + high/med/low backlog) has shipped
— see the **Shipped** changelog below. What follows in **Open** is everything
that is still genuinely outstanding.

## 🚀 User backlog (2026-07-20) — palette rework + UX batch

- [x] **Command palette rework** (deae8f9) (user: "utter garbage — hardcoded list"): modular
      action registry built ON OPEN (not per frame), file/project gating, plugin
      sources, usage+recency-biased ranking (persisted), query-change-only
      refiltering (the actual snappiness fix), selection via scroll/arrows/number
      keys, dim source tag + hover tip per row ("Filetype: Lua", "Plugin: X").
- [x] **Build/Run discovery for multilevel projects** (7beb9eb — picker + [build]/[run] pins; F6 was ALSO ignoring the [build] override entirely): enumerate ALL candidate
      targets (sln/cmake/uproject/cargo/npm/scripts, subdirs included) and present
      a GRAPHICAL picker; remember choice per project; when auto-detect fails,
      show the candidates instead of giving up. Same principle for debugger setup.
      (user: "you try and fail to automate and instead of giving that power to
      the user you just give up. stop that")
- [x] **Debugger fully plugin-ized** (7th DLL imguiide_debugger, ABI 4): panel,
      breakpoints (markers via contributeMarkers), adapters, project pins,
      raddbg/VS bridges — core carries ZERO debugger code. Auto-setup: silent
      adapter detection + venv debugpy + built-exe target + project pins; the
      Configuration GUI ships in the plugin panel. Config in debugger.ini.
      Plugin keybinds now dispatch BEFORE core (conditional emission = F5
      continue only while paused, F9 only with a doc).
- [x] **Text-tools plugin** (a0ec237, 6th DLL; hostReplaceSelection added, ABI 3): json<->xml, json minify/pretty, sort
      lines/selection (numeric/alpha, asc/desc), case convert selection
      (camel/snake/upper/lower/title). Needs hostReplaceSelection host API.
- [ ] **Markdown preview: fenced code blocks** syntax-highlighted (we own a
      highlighting editor — use its colorizer, or render mono + per-token colors).
- [ ] **File-structure reorg**: features into folders; only editor.{h,cpp} +
      main.cpp stay top-level in example/.
- [ ] **Appearance settings**: line spacing, fold-preview style/spacing, more
      functional visual knobs.
- [ ] **Symbols viewer**: more language/project aware (group by construct per
      language, project-type sections).

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

## 🚀 User backlog (2026-07-15) — new batch

### Git (mostly shipped 481c163)
- [x] Commit history browsing + rollback (VS-style): Git ▸ History… — log on a
      worker, checkout/soft-reset/hard-reset/revert with confirm modals, file-scope
      toggle, compare-file-with-commit.
- [x] Open repo in web browser (origin remote → https; `gitRemoteToWebUrl`, tested).
- [x] Submodule support (update --init --recursive / sync / status / --remote).
- [ ] Deeper git: stage/unstage hunks, per-file diff view in the History window,
      stash list, tag browsing, blame gutter (notes already do a blame-style view).

### Symbols / autocomplete (foundation shipped c83e224)
- [x] **Disk symbol packs** — `ts::registerTypeMembers` + `<exe>/symbols/*.json`,
      merged at startup, augments compiled types. Ships cpp-std + python packs.
- [ ] **Pregenerate the big packs**: STL by C++ standard (14/17/20/23), .NET BCL
      (4.8/6/8/10), Lua stdlib, a full Unreal pack. These are DATA now — generate
      (script or LLM) against each framework's reference; no code change needed.
- [ ] **Signatures, not just names**: extend the pack schema + completion to carry
      parameter lists / return types (real VS-like tooltips). Bigger: touches the
      completion popup + resolver.
- [ ] **Worker-thread variable-type tracking**: move the per-file live type
      inference (resolveMemberChain and friends) onto a worker so a big file's
      type map builds off the UI thread.

### Menus
- [x] UEVR plugin grouped under one Tools ▸ "UEVR / Blueprint" submenu (was 3 flat).
      (Unreal plugin was already gated behind a .uproject-only "Unreal Engine" menu.)

### Blueprint / cppgen (LARGE — flagged, not started)
- [x] BP node groups collapse by default when browsing (was force-open every frame).
- [ ] **Forward connections still awkward** — dragging OUT of an output pin to make
      a node. Audit the drag-target hit-testing + the pending-pin auto-connect.
- [ ] **Templates/snippets are generated through C++** — backwards. Move to storing
      snapshots as **annotated Lua** and generate consistent bidirectional Lua from
      annotations/keywords, so a contributor (or an AI) can add examples without
      touching C++. USER FLAGGED as possibly excessive — DO NOT REGRESS the working
      codegen; design a parallel path first.
- [x] **cppgen "loses virtual members"** (f0725d7) — the parser was actually correct
      on every virtual shape (override/final dropped, const/noexcept kept, pure→no
      body, verified by probe). The real loss was whole-class generation SILENTLY
      skipping pure virtuals; it now emits a marker comment so they stay visible.
- [ ] Make cppgen a plugin, expanded with snippets + more static codegen.
- [ ] **Plugin-ize the standalone file-classes** (pdfview, notes, lsp_protocol, …)
      — ONLY if no features are lost. Audit each for host-API gaps first.
- [ ] BP core is hard to hand-edit — the annotated-Lua move above is the enabler for
      retargeting it (e.g. normal UE via UnrealSharp codegen).

### PR #3 port (lobotomy-x #3 — 165 commits stale, UE-in-core arch; ported not merged)
- [x] **DAP debugger backend** (bf05d3c) — dapproto/dapclient/dbgbridge libs + daptest.
- [x] **DAP debugger UI** (5d6540c) — debugger_ui.cpp: breakpoints (F9), Debug panel
      (stack/vars/console), stop marker, pollDap; raddbg/VS bridges. UE-editor
      target dropped (plugin owns UE).
- [x] **Command palette** (b936b5d) — command_palette.cpp: Ctrl+Shift+P fuzzy actions;
      UE commands dropped, this build's features added (debug/git-history/notes/…).
- [x] **Async project search** (d263cb7) — references + find-in-files scan on a
      worker pool (search_async.cpp: ProjectSearch + startProjectSearch + poll +
      clipper-rendered rows). Non-blocking on huge trees; no regression (old search
      was sync single-root, never covered extra locations).
- [ ] **Parallel indexing** — PR's rebuildProjectIndex uses a thread pool. Mine is
      single-thread background (already non-blocking). Porting must preserve my
      extraSourceLocations indexing. Deferred.
- [ ] daptest live debugpy round-trip fails (breakpoint stop not observed on 1.8.5).
- [x] **Marker clobbering fixed** (2026-07-19) — `Editor::refreshMarkers`
  composer: one clear, then change→notes→debug layers (later add wins the
  per-line slot). applyNote/applyDebug are pure adders; every rebuild
  (breakpoint toggle, note edit, external change, file open) recomposes all
  three, so none wipes the others anymore.

### Debugger usability (user feedback 2026-07-19)
- [x] **Settings actually persisted** — `[debug_adapters]` / `[debug_bridge]` were
      referenced by toasts but NEVER loaded/saved (dead config, the "doesn't do
      anything" complaint). Wired into loadSettings/saveSettings, plus new
      `[debug_project_adapter]` / `[debug_project_target]`.
- [x] **Per-PROJECT associations** — the project binds an adapter command and a
      debug target (program + args); wins over the per-extension mapping.
      `dbgbridge::inferAdapterType` derives the DAP launch type from the command
      (vsdbg→cppvsdbg, OpenDebugAD7→cppdbg, debugpy→python); DAP launch now
      carries debuggee argv ("args").
- [x] **GUI configuration** — Debug panel ▸ Configuration: project adapter/target
      editors ("built exe"/"active file" fillers), per-file-type adapter override,
      Save (persists), Detect Adapters + detection readout. Palette:
      "Debug: Configure Adapters / Target...".
- [ ] Verify a real end-to-end stop (native vsdbg on this machine) — daptest covers
      the protocol; live confirm still owed.
- [ ] Launch-config presets (multiple named targets per project, VS-style dropdown).

### Plugins
- [x] **Top-level plugin menus** (409e817) — `EditorPlugin::topLevelMenu()` gives a
      plugin its own menu-bar entry; Unreal moved out of Project into a top-level
      "Unreal" menu.
- [x] **NEW: integrated terminal plugin** (71a92b4) — Tools ▸ Terminal, a persistent
      project-local shell (cmd/$SHELL) with piped I/O, worker-thread output,
      Ctrl-C/Restart/Clear. Third DLL plugin.
- [x] **Palette contributions API** (2026-07-19) — `contributePaletteCommands`
      hook + registry fan-out; entries gate on file type (PluginDocInfo) and/or
      project type (hostProjectRoot probe). Unreal contributes its commands only
      inside UE projects; terminal adds Terminal: Toggle. Selftest-covered.
- [ ] **Plugin splits — the loose files next to editor.cpp** (user: "many could
      be plugins, e.g. entire debugger"). Order by API-gap size:
  1. [x] **cppgen → plugin** (2026-07-19) — new `onDocumentContextMenu` hook +
     `PluginDocContext` (line/word/lineText + lineCount/docVersion memo keys);
     cppgen.{h,cpp} moved to plugins/cppgen, 4th DLL `imguiide_cppgen`; editor
     core carries zero cppgen code; selftest still covers the pure helpers.
  2. [x] **pdfview → plugin** (2026-07-19) — 5th DLL `imguiide_pdfview`; claims
     .pdf via the openFile hook (isBinaryExt path), windows render in onFrame,
     pan via hostMiddleMousePanScroll(111). C++/WinRT (windowsapp) moved off
     the core exe into the plugin DLL.
  3. **notes → plugin** (marker composer DONE — remaining gap: marker + gutter
     hit-test + git-blame host APIs; notes also feed reanchor-on-load).
  4. [x] **debugger → plugin** (2026-07-20) — host API v4 (hostConfigDir/
     hostActiveCursorLine/hostJumpTo/hostRefreshMarkers/hostFindBuiltExe/
     hostSaveActiveDocument) + contributeMarkers hook; plugin keybind dispatch
     moved BEFORE core so conditional emission shadows F5/F9/F10/F11
     contextually.
- [x] **Keybind-contribution hook** (2026-07-19) — `PluginKeybind` +
      `contributeKeybinds`; registry stamps the plugin display name as the
      Settings group; binds dispatch through the same chord matcher/override
      store and are rebindable in Settings ▸ Keybinds. Terminal contributes
      "Ctrl+`" toggle. Unblocks the debugger split.

### UE tooling
- [x] **.uplugin repos recognized as UE projects** (3728573) — engine-source nav +
      descriptor editor + class wizard for a standalone plugin checkout.
- [x] **Tolerant descriptor parsing** (c6cc269) — a .uproject/.uplugin with trailing
      commas / comments still parses (fixed "UE source linking broken").
- [x] **Auto-populate available plugins** (5452f7a) — the descriptor editor's
      plugin-dependency picker lists every .uplugin under the project + engine
      Plugins trees (224 on a real UE 5.4), filterable. No more free-text typos.
- [x] **Auto-populate available MODULES** (f7bf26b) — the descriptor editor's Module
      picker lists every <Name>.Build.cs under the project Source + Plugins trees,
      filterable. Verified: Calculator→2, JsonAsAsset plugin→3.

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
