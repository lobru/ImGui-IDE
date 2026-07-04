---
description: Read queued in-editor replies from ImGui-IDE and act on them
---

Drain the ImGui-IDE reply outbox and act on each item. The user produces these in the editor by clicking a toast, a purple gutter change-dot, a row in Dev Tools > "Claude changes", or right-clicking a line number — then typing a message and choosing **Send now** or queuing a **batch**.

(Note: the plugin's UserPromptSubmit hook normally drains these automatically into context on your next message — this command is the manual/explicit path.)

Run exactly this:

```
pwsh -NoProfile -ExecutionPolicy Bypass -File "${CLAUDE_PLUGIN_ROOT}/scripts/read-replies.ps1"
```

Then, for each printed reply, treat it as feedback/instruction from the user about the editor and act on it. A line may begin with a `[path:line]` prefix (project-relative, 1-based line) — open/go there before acting. A batched submission prints as a `Batched feedback from ImGui-IDE:` header followed by `- ` bullets; treat each bullet as its own item. If it prints `(no replies)`, say so in one line and stop.
