---
description: Send a toast notification to the running ImGui-IDE
argument-hint: "[info|warn|error|success] <message>"
---

Send a toast to the running ImGui-IDE.

Parse `$ARGUMENTS`: if the first whitespace-delimited token is `info`, `warn`, `error`, or `success`, that's the severity; the remainder is the message. Otherwise severity is `info` and the whole string is the message. If `$ARGUMENTS` is empty, ask for the message in one line and stop.

Then run exactly this and report one line:

```
pwsh -NoProfile -ExecutionPolicy Bypass -File "${CLAUDE_PLUGIN_ROOT}/scripts/send-toast.ps1" -Severity "<severity>" -Message "<message>"
```

The IDE shows it within ~0.2s if it's running; if not, the file waits in the inbox and appears on next launch.
