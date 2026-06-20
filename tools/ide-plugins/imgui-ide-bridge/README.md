# imgui-ide-bridge

A Claude Code plugin that bridges to **ImGui-IDE**:

- `/imgui-ide-bridge:open-in-ide` — open the current project in ImGui-IDE (like `code .`).
- `/imgui-ide-bridge:toast [severity] <message>` — show a toast in the running editor.

## The toast API

The editor watches `%APPDATA%\ImGuiColorTextEdit\toasts\` (~5 Hz). **Any** process can
surface a toast by dropping a UTF-8 text file there — no plugin required:

```
<severity>|<message>
```

`severity` is one of `info` `warn` `error` `success` `ok` (optional; default `info`).
The file is deleted once shown. `scripts/send-toast.ps1` does exactly this:

```powershell
pwsh -File scripts/send-toast.ps1 -Severity success -Message "build green"
```

## Opening a project

`scripts/open-in-ide.ps1 -Project <dir>` launches the editor with `--project <dir>`.
Override the editor path with the `IMGUI_IDE_EXE` environment variable (defaults to the
local Release build).

## Install

These commands are also available immediately as project commands in
`.claude/commands/` (open-in-ide, toast). To install as a global plugin:

```
/plugin marketplace add C:\Users\lbatv\source\repos\ImGuiColorTextEdit\tools\ide-plugins
/plugin install imgui-ide-bridge@local-imgui-ide
```

Restart Claude Code after install so the commands load.

## Optional: toast when Claude finishes

Add a `Stop` hook that calls `send-toast.ps1` — note it fires every turn, so it's off by
default. Example `hooks/hooks.json`:

```json
{ "hooks": { "Stop": [ { "matcher": "*", "hooks": [
  { "type": "command", "command": "pwsh -NoProfile -File \"${CLAUDE_PLUGIN_ROOT}/scripts/send-toast.ps1\" -Severity success -Message \"Claude finished\"" }
] } ] } }
```
