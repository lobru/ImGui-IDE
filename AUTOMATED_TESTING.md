# Automated Testing — Plan & Options

Three viable approaches, in rough order of effort. They're complementary, not
exclusive. None are wired up yet; this is the execution plan.

---

## 1. AI-driven UI testing (lowest setup, available now)

Drive the running `example.exe` with a computer-use agent and walk it through
[`TESTING.md`](TESTING.md). The checklist is written to be executed top-to-bottom:
each item is an observable action + expected result.

- **Pros:** zero code, tests the real rendered UI (clicks, docking, wrapping
  visuals), works today.
- **Cons:** slow, screenshot-based assertions are fuzzy (e.g. "did the caret land
  on the right glyph" is hard to assert pixel-perfectly), not CI-friendly.
- **Use for:** smoke passes before handing builds to human testers; catching
  obvious "doesn't render / crashes on launch" regressions.

---

## 2. ImGui Test Engine (recommended for repeatable UI tests)

<https://github.com/ocornut/imgui_test_engine> — drives ImGui by item label
(`ctx->ItemClick("**/Word Wrap")`), asserts with `IM_CHECK`, runs "fast" (headless-ish,
no real input) or in GUI mode. This is the right tool for menu/settings/docking
state tests.

### Integration steps (all gated behind `option(BUILD_TESTS OFF)` so the normal
### build is never affected):

1. **deps/imgui/CMakeLists.txt** — when `BUILD_TESTS`:
   - `FetchContent_Populate(imgui_test_engine GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine GIT_TAG <tag matching v1.92.7-docking — verify, likely v1.92.x or a dated commit>)`
   - Compile `imgui_test_engine/imgui_te_*.cpp` into an `imgui_test_engine` lib.
   - `target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE)` and
     `target_include_directories(imgui PUBLIC ${imgui_test_engine_SOURCE_DIR})`
     **(both only under BUILD_TESTS — imgui.cpp #includes the engine header when
     the define is set, so leaving it on without the dep breaks the normal build).**

2. **New `tests` target** (gated): its own `test_main.cpp` that sets up SDL+GPU+
   ImGui exactly like `main.cpp`, plus:
   - `ImGuiTestEngine* e = ImGuiTestEngine_CreateContext();`
   - `ImGuiTestEngine_Start(e, ImGui::GetCurrentContext());`
   - register tests (below), `ImGuiTestEngine_QueueTests(...)`.
   - per frame after render: `ImGuiTestEngine_PostSwap(e);`
   - link `TextEditorLib`, `imgui_test_engine`, SDL3, imguifd.
   - Refactor note: `main.cpp`'s init/loop is worth extracting into a shared
     `run_frame()` helper so `tests` and `example` don't duplicate it.

3. **First tests** (high-value, low-flakiness):
   - Open Settings, toggle **Word wrap**, assert `editor.prefWordWrap == true`
     (expose a test accessor or check the live `TextEditor::IsWordWrap()`).
   - Open 3 files, assert all 3 windows share one dock node (tabs, not split) —
     `ImGui::GetWindowDockID()` equal for each.
   - View → Reset Layout, assert Nav/Output/Refs land in their expected nodes.
   - Keybind capture: focus a chord button, inject `Ctrl+Shift+K`, assert the
     recorded string == "Ctrl+Shift+K" (regression guard for the reserved-mod bug).
   - INI/Lua fold counts: SetText a known buffer, assert `foldRanges` size.

4. **Caveats to resolve on first run:** test-engine/imgui tag compatibility;
   headless GPU context on CI (may need a software/offscreen swapchain or a
   virtual display); pin the GIT_TAG once a working combo is found.

---

## 3. Custom headless logic tests (cheap regression net for pure code)

A tiny `selftest` target / `--selftest` flag that exercises the **non-UI** logic
that's most prone to subtle bugs, no ImGui context needed. Candidates already
mostly pure:
- `Editor::languageForPath()` — extension → Language mapping.
- `TextEditor` fold detection (`rebuildFoldRanges` on a Document) — Lua/INI/Python.
- Word-wrap `buildWrapRows()` row boundaries for a known line + width.
- INI tokenizer (`tokenizeIni`) classification.
- Recents dedup / cap, settings round-trip (write then read settings.txt).

Some of these need small refactors to be callable without a live ImGui context
(e.g. `buildWrapRows` reads `wrapColumns`/`glyphSize` members — pass them in, or
set them directly in the test). Worth it: these run in milliseconds and catch the
algorithmic regressions that "it compiled" never will.

---

## Recommendation

Do **#3** first (fast, CI-able, guards the wrap/fold/tokenizer math), then **#2**
for UI/docking/settings flows. Keep **#1** as the pre-release human-ish smoke pass.
All three reference the same expected behaviours documented in `TESTING.md`.
