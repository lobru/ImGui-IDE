#requires -Version 5.1
<#
.SYNOPSIS
  UserPromptSubmit hook: auto-drain ImGui-IDE replies into Claude's context.
.DESCRIPTION
  Replying inside ImGui-IDE (gutter change-dot, Dev Tools change row, line-number
  right-click, or a toast click) writes files to <APPDATA>\ImGuiColorTextEdit\replies\.
  This hook runs on every user prompt: if replies are queued it prints them (stdout
  becomes model context) and deletes them — the in-editor feedback loop closes
  without the user ever running /replies. Prints NOTHING when the outbox is empty.
#>
$dir = Join-Path $env:APPDATA 'ImGuiColorTextEdit\replies'
if (-not (Test-Path $dir)) { exit 0 }
$files = Get-ChildItem -LiteralPath $dir -Filter *.txt -ErrorAction SilentlyContinue | Sort-Object Name
if (-not $files) { exit 0 }

$out = @()
$out += '[ImGui-IDE replies] The user sent feedback from inside the editor. Treat each item as an instruction. A [path:line] prefix names the project-relative file and 1-based line it is about; a "Batched feedback" block holds one item per "- " bullet.'
foreach ($f in $files) {
    $t = (Get-Content -Raw -LiteralPath $f.FullName -ErrorAction SilentlyContinue)
    if ($t) {
        $out += ''
        $out += $t.Trim()
    }
    Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue
}
if ($out.Count -gt 1) { $out -join "`n" }
exit 0
