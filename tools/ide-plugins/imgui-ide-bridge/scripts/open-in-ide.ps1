#requires -Version 5.1
<#
.SYNOPSIS
  Open a project folder in ImGui-IDE (like `code .` for VS Code).
.DESCRIPTION
  Launches ImGui-IDE rooted at -Project (defaults to the current directory) via its
  --project argument, which opens the nav panel at that folder without forcing a tab.
  Set the IMGUI_IDE_EXE environment variable to override the editor path.
#>
param(
    [string]$Project = (Get-Location).Path
)

$exe = $env:IMGUI_IDE_EXE
if (-not $exe) {
    $exe = 'C:\Users\lbatv\source\repos\ImGuiColorTextEdit\example\out\build\x64-Release\ImGui-IDE.exe'
}
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Error "ImGui-IDE.exe not found at '$exe'. Build it, or set IMGUI_IDE_EXE to the editor path."
    exit 1
}
$Project = (Resolve-Path -LiteralPath $Project).Path
Start-Process -FilePath $exe -ArgumentList @('--project', $Project) | Out-Null
Write-Host "opened in ImGui-IDE: $Project"
