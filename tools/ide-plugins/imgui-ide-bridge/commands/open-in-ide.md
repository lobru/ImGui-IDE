---
description: Open the current project in ImGui-IDE (like `code .`)
---

Open the current working directory as a project in ImGui-IDE.

Run exactly this and report the result in one line (success or the error):

```
pwsh -NoProfile -ExecutionPolicy Bypass -File "${CLAUDE_PLUGIN_ROOT}/scripts/open-in-ide.ps1" -Project "<absolute path of the current project root>"
```

Replace `<absolute path of the current project root>` with the real absolute path. Do not open a tab or modify any files — this only launches the editor rooted at the folder.
