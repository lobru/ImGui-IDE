---
description: Send a toast notification to the running ImGui-IDE
argument-hint: "[info|warn|error|success] <message>"
---

Send a toast to the running ImGui-IDE.

Parse `$ARGUMENTS`: if the first token is `info|warn|error|success`, that's the severity and the rest is the message; otherwise severity is `info` and the whole string is the message. If empty, ask for the message in one line and stop.

Run exactly this, then report one line:

```
pwsh -NoProfile -ExecutionPolicy Bypass -File "C:\Users\lbatv\source\repos\ImGuiColorTextEdit\tools\ide-plugins\imgui-ide-bridge\scripts\send-toast.ps1" -Severity "<severity>" -Message "<message>"
```
