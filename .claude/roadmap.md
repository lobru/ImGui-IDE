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
- [ ] **Fold preview drawn on its own row** in some cases. Marked "worth
      retesting" after the Python indent-fold rewrite — the spurious top-level
      indent folds that likely caused it no longer exist. Confirm fixed or
      capture a fresh repro; the preview is drawn inline at
      `cursorScreenPos.x + textOffset + (maxColumn+1)*glyphSize.x` (TextEditor.cpp
      ~L961), so an off-by-one row would point at the `py`/`lineScreenPos.y`
      calc.

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

### 2026-07-18 (round 3) — command palette + native debugger bridges
- **Ctrl+Shift+P command palette** (`view.palette`, rebindable; View menu):
  fuzzy-searchable registry of every app command — file/view/find/code/
  project/debug/git/Unreal/themes plus recent files and projects. Registry
  rebuilt per open frame (closures over live state), fuzzy subsequence
  scoring (consecutive/word-start/head bonuses), clipper-rendered list,
  arrows + Enter + Escape, actions deferred until after End().
- **Native debugger bridges** (`debug_bridge.{h,cpp}`, pure + selftest-covered):
  - *In-client DAP*: C-family debugging auto-detects the best adapter —
    Microsoft vsdbg (`cppvsdbg`, VS Code C++ tools) → OpenDebugAD7 + gdb
    (`cppdbg`, MIEngine launch extras MIMode/miDebuggerPath) → lldb-dap →
    `gdb -i dap` (gdb 14+). Native sources debug the freshest BUILT exe
    (findBuiltExe), not the source path. `buildLaunch` gained extra string
    fields for MIEngine configs.
  - *External tools with their own UI*: "Launch in raddbg" starts the target
    in RAD Debugger and pushes current breakpoints over `raddbg --ipc`
    (verb template settings-overridable: `raddbg_bp_template`, {file}/{line});
    "Launch in Visual Studio" runs `devenv /DebugExe`. Targets resolve
    Unreal-first (project's `Binaries/<plat>/<Project>Editor` or stock
    UnrealEditor + .uproject) then built exe. `[debug_bridge]` settings keys
    (raddbg / devenv / raddbg_bp_template) with env + well-known-path
    detection fallbacks. 12 new selftest checks (254 total).

### 2026-07-18 (round 2) — integrated debugger (DAP)
- **Debug Adapter Protocol client**, same split + async architecture as LSP:
  `dap_protocol.{h,cpp}` (pure builders/parsers, reuses lspproto's
  Content-Length framing, selftest-covered) + `dap_client.{h,cpp}` (SDL3
  process transport, reader thread enqueues, UI thread owns all writes,
  request-seq → kind/context pending map, adapter-gone sentinel).
- **Editor integration**: breakpoints per canonical file (F9 toggle, red
  gutter markers via the marker API; a yellow marker tracks the stopped
  line), Debug menu + rebindable F10/F11/Shift+F11/Shift+F5 keybinds (F5 =
  Continue while a session is paused, project-run otherwise), and a Debug
  panel — controls, live call stack (click to jump + switch frame),
  variables tree (scopes eager, children lazy via variablesReference),
  console streaming output events + an evaluate REPL.
- **Adapters**: Python works out of the box via debugpy
  (`python -m debugpy.adapter`, interpreter resolved like the script
  runner: override → venv → PATH). Any other language maps a command in
  settings `[debug_adapters]` (".ext=lldb-dap" etc.).
- **daptest** integration gate (mirrors lsptest, SKIP=77 without debugpy):
  spawns a real debugpy and asserts breakpoint → stopped → stack → locals →
  evaluate → continue → exit against a live session. 22 new selftest checks
  for the protocol layer (242 total).
- Breakpoints are session-only (not persisted) — persistence + conditional
  breakpoints are natural follow-ups.

### 2026-07-18 — performance + Unreal batch
- **Multithreaded project indexing.** `rebuildProjectIndex` now runs in three
  phases: serial candidate enumeration → parallel parse on a worker pool
  (`hardware_concurrency - 1`, capped at 8; per-worker identifier/def
  aggregates, per-candidate result slots, zero locks in the hot path) → serial
  merge in enumeration order (deterministic per-symbol site caps). tree-sitter's
  tags-query cache is mutex-guarded (`tsindex.cpp`) since compiled `TSQuery`s
  are immutable and shared across workers. Symbols panel shows live
  `indexing done/total` progress (`IndexState::filesTotal/filesDone`).
- **Async multithreaded search.** New `startProjectSearch` engine (shared by
  Find in Files + Find References): the walk + scan runs off the UI thread on
  a worker pool, per-file hit batches publish under a mutex with a `version`
  counter, panels stream results in live with a "(searching…)" tag. `gen`
  cancels superseded searches; a late pass can't stomp its successor's flags.
  Both features previously froze the UI for seconds on big projects.
- **Rendering optimizations.** Find in Files / References panels render
  through `ImGuiListClipper` over precomputed display rows (was: up to 5000
  Selectables per frame). Symbols panel filter mode caches match indices per
  (filter, gen) against pre-lowered names + clips. Nav flat view filters into
  an index list then clips (was: 20k Selectables per frame worst case).
- **Unreal Engine support with Blueprint-style code gen.** New pure-logic
  `unrealgen` lib (`example/unreal.{h,cpp}`): `.uproject` detection, UE-style
  class scaffolding for Actor / Character / Pawn / ActorComponent /
  SceneComponent / Object / Interface / BlueprintFunctionLibrary
  (UCLASS/GENERATED_BODY/BeginPlay/Tick per engine templates, module `*_API`
  macro, `.generated.h` include), UFUNCTION (BlueprintCallable/Pure) +
  UPROPERTY stub generation, param-list parsing (template-comma aware).
  Editor side: an "Unreal" menu appears when the project root has a
  `.uproject` — "New Unreal Class…" wizard writes + opens the pair,
  "Blueprint Stubs…" inserts declarations at the cursor. 38 new selftest
  checks incl. a parallel `extractSymbols` thread-safety exercise.

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
