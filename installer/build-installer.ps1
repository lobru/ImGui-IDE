<#
.SYNOPSIS
    Build the ImGui-IDE release exe, then compile the Windows installer.

.DESCRIPTION
    Runs build.ps1 -Config Release (so example.exe is current), then invokes Inno
    Setup's ISCC on ImGui-IDE.iss to produce installer/output/ImGui-IDE-Setup-*.exe.
    A failed build because a running instance holds example.exe is tolerated — the
    installer just packages whatever example.exe is already built.

.PARAMETER SkipBuild
    Skip the C++ build and only (re)compile the installer from the existing exe.
#>
param([switch]$SkipBuild)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path $here -Parent

if (-not $SkipBuild) {
    Write-Host "Building release ..." -ForegroundColor Cyan
    try { & "$root\build.ps1" -Config Release } catch { }
    # build.ps1 exits non-zero if a running instance locks example.exe; the previous
    # exe is still on disk, so press on and let the installer package it.
}

$iscc = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $iscc) {
    Write-Error "Inno Setup 6 (ISCC.exe) not found. Install from https://jrsoftware.org/isdl.php"
    exit 1
}

Write-Host "Compiling installer ..." -ForegroundColor Cyan
& $iscc "$here\ImGui-IDE.iss"
if ($LASTEXITCODE -ne 0) { Write-Error "ISCC failed ($LASTEXITCODE)"; exit $LASTEXITCODE }

$out = Get-ChildItem "$here\output\*.exe" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($out) { Write-Host "`nInstaller: $($out.FullName)" -ForegroundColor Green }
