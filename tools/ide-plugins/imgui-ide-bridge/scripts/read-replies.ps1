#requires -Version 5.1
<#
.SYNOPSIS
  Drain in-editor toast replies (the IDE -> Claude half of the feedback bridge).
.DESCRIPTION
  Clicking a toast in ImGui-IDE writes its text to <APPDATA>\ImGuiColorTextEdit\replies\.
  This prints each queued reply (oldest first) and deletes it, so Claude can read
  and act on what the user clicked in the editor.
#>
$dir = Join-Path $env:APPDATA 'ImGuiColorTextEdit\replies'
if (-not (Test-Path $dir)) { Write-Host '(no replies)'; return }
$files = Get-ChildItem -LiteralPath $dir -Filter *.txt -ErrorAction SilentlyContinue | Sort-Object Name
if (-not $files) { Write-Host '(no replies)'; return }
foreach ($f in $files) {
    $text = Get-Content -Raw -LiteralPath $f.FullName
    Write-Host ("- " + $text.Trim())
    Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue
}
