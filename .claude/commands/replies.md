---
description: Read in-editor toast replies (clicks) from ImGui-IDE and act on them
---

Drain the ImGui-IDE reply outbox and act on each item. The user produces these in the editor by clicking a toast, a purple gutter change-dot, a row in Dev Tools > "Claude changes", or right-clicking a line number — then typing a message and choosing **Send now** or queuing it for **batch** submission.

Run exactly this:

```
pwsh -NoProfile -ExecutionPolicy Bypass -File "C:\Users\lbatv\source\repos\ImGuiColorTextEdit\tools\ide-plugins\imgui-ide-bridge\scripts\read-replies.ps1"
```

Then, for each printed reply line, treat it as short feedback from the user about the editor and act on it. A line may begin with a `[path:line]` prefix (project-relative) — that is the exact file and 1-based line the feedback is about, so open/go there before acting. A batched submission prints as a `Batched feedback from ImGui-IDE:` header followed by `- ` bullets; treat each bullet as its own item. If it prints `(no replies)`, say so in one line and stop.
