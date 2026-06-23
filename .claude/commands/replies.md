---
description: Read in-editor toast replies (clicks) from ImGui-IDE and act on them
---

Drain the ImGui-IDE reply outbox (text the user produced by clicking toasts in the editor) and act on each item.

Run exactly this:

```
pwsh -NoProfile -ExecutionPolicy Bypass -File "C:\Users\lbatv\source\repos\ImGuiColorTextEdit\tools\ide-plugins\imgui-ide-bridge\scripts\read-replies.ps1"
```

Then, for each printed reply line, treat it as a short instruction/feedback from the user about the editor and respond or act accordingly. If it prints `(no replies)`, say so in one line and stop.
