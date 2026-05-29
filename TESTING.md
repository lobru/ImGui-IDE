# Manual Test Checklist

Features added on the `lobotomy/main` fork that need human verification. Most
have only been compile-verified, not runtime-verified. Group by area; check the
box when confirmed working. Note OS + build (Debug/Release) when filing issues.

> Tip: after launching, do **View → Reset Layout** once — older `imgui.ini`
> files carry stale floating/viewport state from earlier builds.

## Editor core
- [ ] Open a file, type, undo/redo, copy/cut/paste.
- [ ] **Word wrap** (View → Word Wrap, or Settings → Editor): long lines wrap to
      window width; no horizontal scrollbar appears.
  - [ ] Click into a wrapped line → caret lands on the glyph you clicked.
  - [ ] Arrow Up/Down moves row-by-row through wrapped rows (not whole lines).
  - [ ] Drag-select across wrapped text highlights correctly.
  - [ ] Wrap width slider (Settings) changes the wrap column; 0 = fit window.
  - [ ] Known v1 gaps in wrap mode: fold arrows, bracket-match highlight, line
        markers, and minimap are intentionally disabled — confirm they're absent,
        not glitching.
- [ ] **Pan/scroll indicator**: middle-click-drag shows the ring+arrows indicator
      **at the click point**; arrow toward drag direction brightens; the
      View toggle and Code → Middle Mouse Pan mode both work.
- [ ] Ctrl + scroll wheel zooms font (toggleable in Settings).
- [ ] Right-edge padding: text doesn't kiss the vertical scrollbar.

## Folding
- [ ] C/C++/C#: brace folds, `#if/#endif`, `#pragma region`, `// region`.
- [ ] Python: indentation folds.
- [ ] **Lua**: `function`/`if`/`for`/`while`/`do`/`repeat` … `end`/`until` fold;
      one-line `if x then y end` does NOT fold.
- [ ] **INI**: each `[section]` folds down to the line before the next header.

## Languages
- [ ] `.ini`/`.cfg`/`.conf` → INI highlighting: `[headers]` colored, `key=` keys
      highlighted, `;` and `#` comments.
- [ ] `.xaml` → XAML highlighting.
- [ ] Language dropdown in status bar lists INI + runtime langs.

## Symbol navigation (right-click in editor)
- [ ] Right-click a symbol → menu shows the clicked/selected word's actions.
- [ ] **Go to Definition** jumps to the definition project-wide (test C# across
      files — class/method/interface).
- [ ] **Find References** opens the panel grouped by file; clicking a hit opens
      that file at the line.
- [ ] Having a selection + right-click does NOT shrink the selection.

## Navigation panel
- [ ] Docked left by default; shows project tree.
- [ ] Header toggles: `.dot` (dotfiles), `code` (source-only), `excl` (show
      excluded), `flat` (folders only) — and their defaults persist across runs.
- [ ] Hover a file → tooltip with type, size, created, modified.
- [ ] Right-click: Open, Open in Explorer, Copy path, Copy/Cut/Paste, Rename
      (inline), Delete, Exclude/Re-include.
- [ ] **Delete** → goes to Recycle Bin by default; "Force delete" checkbox does
      permanent delete.
- [ ] Drag a file onto a folder = move; hold Ctrl = copy; can't move into self.
- [ ] Image files open in the image viewer (not as text); `.exe` runs; other
      binaries open in OS default; unknown binary (NUL bytes) doesn't open as text.

## Project / run
- [ ] Open a folder OR a `.sln`/`.csproj`/`.vcxproj` (CLI arg and folder picker).
- [ ] Reopening a project restores the previously open tabs.
- [ ] **F5**: runs the project's built exe if found, else runs the active script
      (Python/Lua/PowerShell/etc.).
- [ ] **F6**: build (uses build.bat/CMake/etc.); `%CONFIG%` expands to the
      selected Debug/Release config.
- [ ] Output panel docks bottom; Copy button works.

## Docking / windows / viewports
- [ ] Opening multiple files → they stack as **tabs** in the centre (do NOT pop
      out into separate OS windows).
- [ ] Multi-viewport: dragging a tab out of the main window makes it its own OS
      window; dropping it back re-docks.
- [ ] Pop-out hotkeys: Ctrl+Alt+← / Ctrl+Alt+→ pop active doc out left/right.
- [ ] Merge hotkeys: Ctrl+Alt+M (current) / Ctrl+Alt+Shift+M (all) re-dock.
- [ ] **FPS does NOT tank** when a window is popped out (idle throttle must only
      fire when the whole app is unfocused — was the 200→10 bug).
- [ ] Find References panel docks right; Output docks bottom.

## Image viewer
- [ ] PNG/JPG/BMP/TGA/GIF open in a dockable window, auto-fit on open, no crash
      (the ImTextureData/RegisterUserTexture path).
- [ ] Zoom slider, 1:1, Fit work; multiple images open at once.

## Settings persistence (`%APPDATA%\ImGuiColorTextEdit\settings.txt`)
- [ ] Toggle settings, restart → they persist (auto-indent, complete-pairs,
      show-fps, ctrl-scroll-zoom, invert-pan, word-wrap+width, fps-limit,
      idle-throttle, font, nav filters, toolchain config, recents).
- [ ] Recent Files / Recent Projects menus populate and "Clear" works.
- [ ] Font selector lists system fonts and applies the choice.

## Keybinds (Settings → Keybinds)
- [ ] Groups are collapsible tree nodes with nested tables.
- [ ] Click a chord → "press chord…" → press e.g. Ctrl+Shift+K → records cleanly
      as `Ctrl+Shift+K` (NOT garbage / not just "Ctrl+"). Esc cancels, Backspace
      clears, reset reverts.
- [ ] KNOWN GAP: recorded chords do not yet re-route live actions (catalogue +
      capture only). Tracked as a follow-up.

## Toolchains (Settings → Toolchains)
- [ ] MSVC installs auto-detected; .NET SDKs via `dotnet --list-sdks`.
- [ ] Build config combo (Debug/Release/RelWithDebInfo/MinSizeRel) persists.
- [ ] Status-bar toolchain selector appears for C/C++/C# docs when a project is open.

## Diff / history
- [ ] File History (Ctrl+I) shows changes since open; side-by-side toggle; revert.
- [ ] Diff Against File… picks a second file and diffs.
