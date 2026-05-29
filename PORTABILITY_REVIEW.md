# Widget vs App Review — and Porting the Non-Platform Code

Goal: drop this fork's **non-platform editor improvements** into an existing app
that already embeds an older TextEditor (e.g. ReShade / UEVR overlays).

## TL;DR

- The **portable unit is `TextEditor.cpp/.h` + `TextDiff.cpp/.h` + `dtl.h`** (+ the
  `.lang` files, with a loader or hardcoded). It pulls **only `imgui.h`,
  `imgui_internal.h`, and the C++ standard library** — no SDL, no Windows.h, no
  ImGuiFileDialog, no process spawning. Verified by include scan.
- Every 1.92-era ImGui-ism we used (`ImTextureData`/`TexRef`, 2-arg `PushFont`,
  `ImGuiKey_ReservedForMod*`) lives in **`example/editor.cpp` (app side)**, NOT in
  the portable unit. The widget uses only long-stable ImGui calls
  (`GetFont`/`GetFontSize`/`ImFont::RenderChar`/`CalcTextSize`/`AddTriangleFilled`/
  draw-list text+lines). No `DockBuilder` in the widget either.
- **platform-free ≠ portable.** The gating fact is **ImGui *version* + which
  TextEditor *lineage* the target app embeds.** That's step 0 below.

## Step 0 (BLOCKING): identify the target

Before any code moves, answer:
1. **Which app**, and where's its source?
2. **Its ImGui version** — same era as `v1.92.7-docking`, or older?
3. **Its TextEditor lineage** — is it an older build of *this* (goossens) TextEditor
   (similar API, our changes are mostly additive), or the classic BalazsJako
   ImGuiColorTextEdit (totally different API → much bigger reconcile)?

These pick the strategy:
- **Same lineage + compatible ImGui** → drop our `TextEditor.*`/`TextDiff.*` in,
  reconcile a handful of call sites. Easy.
- **Same lineage, older ImGui** → back-port surface is tiny (font rendering only,
  see below). Medium.
- **Different lineage** → our widget replaces theirs; their app's editor call
  sites must be rewritten to our API. Big — scope separately.

## Back-port surface (if their ImGui is older)

Only font handling is version-sensitive in the widget:
- `font = ImGui::GetFont(); fontSize = ImGui::GetFontSize();` — stable for years.
- `font->RenderChar(drawList, fontSize, glyphPos, col, codepoint)` — 5-arg form,
  stable across recent versions; verify against their ImGui.
Everything else (`CalcTextSize`, `GetTextLineHeightWithSpacing`, draw-list
primitives) is ancient-stable. So even an older-ImGui target is a small back-port.

## Classification

### Widget-level — in the portable unit (TextEditor.*/TextDiff.*), travels for free
- Word wrap (`SetWordWrap`/`SetWrapWidth`, `buildWrapRows`, `renderTextWrapped`,
  wrap-aware mouse/cursor) — **off by default**, additive.
- Folding: brace / `#if` / region / pragma / comment / Python-indent / **Lua
  keyword** / **INI bracket**.
- Hardcoded `Language::Ini()` (bracket-header tokenizer).
- Pan/scroll indicator (anchored at click, direction highlight) + `SetPanInverted`.
- `GetWordAt(line,col)`, `GetCurrentSelectionText`.
- TextDiff side-by-side right-edge padding.
- The 3 upstream bug fixes ported earlier.
- New public APIs are **all additive + default-off**, so an existing app's current
  TextEditor usage keeps working unchanged after the swap.

### App-shell — stays in `example/`, NOT ported (or reimplemented per host app)
Multi-doc/tabs, navigation panel + tree, docking layout / pop-out / merge,
settings persistence, recents, project sessions, Go-to-Definition/Find-References
(project-wide grep + open-in-tab), goto-line / find dialogs, menu + status bars,
diff dialogs.

### Platform / backend — stays, needs per-host adaptation if ever wanted
Process spawning (run/build, `_popen`, threads), toolchain detection, recycle-bin
delete / open-in-explorer (`shellapi`), font discovery, **image viewer**
(stb_image → `ImTextureData` → `RegisterUserTexture`; SDLGPU3/1.92-specific —
re-do against the host's renderer), FPS limiter (SDL, in `main.cpp`),
`%APPDATA%` config dir. ImGuiFileDialog dependency.

### Widget-level but currently misplaced in `example/editor.cpp`
Migrate these INTO TextEditor **during the port** (validated, minimal — not now):
- `buildAutocompleteTrie` — pure document→Trie scan; Trie is already a widget type.
  Natural fit as `TextEditor::BuildIdentifierTrie()`.
- Optional later: a directory `.lang` loader on `TextEditor::Language` (the
  `FromFile` parser is already widget-level; only the discovery loop is in the app).
- Leave hover-hint and in-file find-references as app code for now — turning them
  into widget callbacks enlarges the API surface to reconcile against the host's
  ImGui. Smallest widget diff = easiest port.

## Port plan

1. **Step 0** above (identify target + ImGui version + lineage).
2. Copy `TextEditor.cpp/.h`, `TextDiff.cpp/.h`, `dtl.h` into the target; build
   against its ImGui; fix only the font back-port spots if flagged.
3. Reconcile the target app's editor call sites against our API (diff public
   headers; our additions are opt-in so most existing calls are unaffected).
4. Bring the `.lang` files + a loader, or hardcode the langs the host needs.
5. Migrate `buildAutocompleteTrie` into the widget if the host wants autocomplete.
6. Re-implement any wanted platform features (image viewer, run/build) against the
   host's renderer/OS — separate, optional.
