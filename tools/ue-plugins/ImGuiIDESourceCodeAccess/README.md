# ImGui-IDE Source Code Access (UE5 plugin)

Makes **ImGui-IDE** selectable as Unreal Engine's source code editor — the same
integration point Visual Studio / VSCode use (`ISourceCodeAccessor`). "Open Source
Code", double-clicking a C++ class, and "Open Solution" then route to ImGui-IDE.

## Install

1. Copy this folder to your project: `<YourProject>/Plugins/ImGuiIDESourceCodeAccess/`
   (or engine-wide: `<Engine>/Plugins/Developer/ImGuiIDESourceCodeAccess/`).
2. Regenerate project files or just launch the editor — UE compiles the plugin on
   startup (it's tiny).
3. **Editor Preferences → General → Source Code → Source Code Editor → `ImGui-IDE`.**

The accessor looks for `ImGui-IDE.exe` in `C:\Program Files\ImGui-IDE\` (installer
default), then falls back to `PATH`.

## What you get

| UE action | Result |
|---|---|
| Open Solution | ImGui-IDE opens the project root (`--project`) — nav tree + symbol index |
| Double-click a C++ class / Open Source Code | ImGui-IDE opens that file |
| Add new C++ class | Files appear in the IDE's tree automatically (no solution to maintain) |

## Intellisense (clangd, VS/VSCode-grade)

In ImGui-IDE with the UE project open: **Project → Unreal Engine → Generate
IntelliSense DB (clangd)**. That runs UnrealBuildTool's `GenerateClangDatabase`
mode (UE 5.3+) with the output dropped in the project root, where the IDE's
clangd picks it up automatically. Toggle *Project → C/C++ IntelliSense* off/on
after it finishes. Build the editor target from **Project → Unreal Engine →
Build Editor Target** (or F6).

## Not yet

- Line/column targeting on open (needs a `--goto` flag in the app; file-level today).
- Debugging (out of scope for now).
