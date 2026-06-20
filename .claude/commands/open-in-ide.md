---
description: Open the current project in ImGui-IDE (like `code .`)
---

Launch ImGui-IDE rooted at the current project. Run exactly this, then report the result in one line:

```
pwsh -NoProfile -ExecutionPolicy Bypass -File "C:\Users\lbatv\source\repos\ImGuiColorTextEdit\tools\ide-plugins\imgui-ide-bridge\scripts\open-in-ide.ps1" -Project "<absolute path of the current project root>"
```

Replace the placeholder with the real absolute path. This only launches the editor — do not open tabs or modify files.
