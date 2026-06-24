#requires -Version 5.1
<#
.SYNOPSIS
  Drain in-editor replies (the IDE -> Claude half of the feedback bridge).
.DESCRIPTION
  Replying in ImGui-IDE (clicking a toast, a gutter change-dot, a Dev Tools change,
  or right-clicking a line number, then typing a message) writes it to
  <APPDATA>\ImGuiColorTextEdit\replies\. This prints each queued reply (oldest
  first) and deletes it, so Claude can read and act on it. A reply may begin with a
  [path:line] prefix naming the file/line the feedback is about.
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
