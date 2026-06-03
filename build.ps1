<#
.SYNOPSIS
    Build the ImGuiColorTextEdit example application.

.PARAMETER Config
    Build configuration: Debug (default) or Release.

.PARAMETER Clean
    Remove the build directory before configuring.

.PARAMETER Run
    Launch the built executable after a successful build.
5
.EXAMPLE
    .\build.ps1
    .\build.ps1 -Config Release
    .\build.ps1 -Clean -Run
#>

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [switch]$Clean,
    [switch]$Run,
    [switch]$Shared        # Build TextEditor as a DLL instead of a static lib
)

$ErrorActionPreference = "Stop"
$Root     = $PSScriptRoot
$BuildDir = "$Root\example\out\build\x64-$Config"
$ExePath  = "$BuildDir\example.exe"

# ── locate Visual Studio ──────────────────────────────────────────────────────
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Error "vswhere.exe not found at:`n  $vsWhere`nInstall Visual Studio 2019 or later."
}

$vsPath    = & $vsWhere -latest -requires Microsoft.VisualCpp.Tools.HostX64.TargetX64 -property installationPath
$vcvarsall = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsall)) {
    Write-Error "vcvarsall.bat not found. Make sure the C++ Desktop workload is installed."
}

# ── find cmake ────────────────────────────────────────────────────────────────
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCmd) { $cmakeCmd.Source } else { $null }
if (-not $cmake) {
    # VS ships cmake under Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
    $cmake = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmake)) {
        Write-Error "cmake not found. Add CMake to PATH or install the CMake component in VS."
    }
}

# ── clean ─────────────────────────────────────────────────────────────────────
if ($Clean) {
    if (Test-Path $BuildDir) {
        Write-Host "Cleaning $BuildDir ..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $BuildDir
    }
    # Wipe FetchContent subbuild/build dirs — they store the CMake generator in
    # CMakeCache.txt and will cause a "generator mismatch" error if they were
    # previously configured with a different generator (e.g. VS 17 vs Ninja).
    $depsDir = "$Root\example\deps"
    foreach ($sub in @("sdl\sdl-subbuild","sdl\sdl-build",
                       "imgui\imgui-subbuild","imgui\imgui-build",
                       "imguifiledialog\imguifd-subbuild","imguifiledialog\imguifd-build")) {
        $p = "$depsDir\$sub"
        if (Test-Path $p) {
            Write-Host "Cleaning dep cache: $p ..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force $p
        }
    }
    # Also remove the top-level deps CMakeCache if present
    $topCache = "$depsDir\CMakeCache.txt"
    if (Test-Path $topCache) { Remove-Item -Force $topCache }

    # Wipe any in-source CMake artifacts (created if someone ran cmake in example/ directly)
    foreach ($stale in @("$Root\example\CMakeCache.txt",
                         "$Root\example\CMakeFiles",
                         "$Root\example\out\CMakeCache.txt",
                         "$Root\example\out\CMakeFiles",
                         "$Root\example\out\deps",
                         "$Root\example\_deps")) {
        if (Test-Path $stale) {
            Write-Host "Removing stale artifact: $stale ..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force $stale
        }
    }
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# ── configure + build via a temp batch file (so vcvarsall env is inherited) ──
$bat = [IO.Path]::GetTempFileName() -replace '\.tmp$', '.bat'
@"
@echo off
call "$vcvarsall" x64 || exit /b 1
cd /d "$BuildDir"
"$cmake" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DTEXTEDITOR_BUILD_SHARED=$($Shared.IsPresent.ToString().ToUpper()) "$Root\example" || exit /b 1
ninja || exit /b 1
"@ | Set-Content -Path $bat -Encoding ASCII

Write-Host "Configuring and building ($Config) ..." -ForegroundColor Cyan
$buildOut = & cmd /c $bat 2>&1
$buildOut | ForEach-Object { Write-Host $_ }
$code = $LASTEXITCODE

# Self-host workflow: if the only thing that failed is the linker not being able
# to overwrite a running example.exe (LNK1168), close the running instance and
# retry once. Lets you rebuild the editor from inside the editor.
if ($code -ne 0 -and ($buildOut -match 'LNK1168')) {
    Write-Host "`nexample.exe is running and locked its own output — closing it and retrying ..." -ForegroundColor Yellow
    Get-Process example -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
    $buildOut = & cmd /c $bat 2>&1
    $buildOut | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
}
Remove-Item $bat -Force

if ($code -ne 0) {
    Write-Host "`nBuild FAILED (exit $code)." -ForegroundColor Red
    exit $code
}

Write-Host "`nBuild succeeded: $ExePath" -ForegroundColor Green

if ($Run) {
    Write-Host "Launching (with dev-cmd env so MSVC %INCLUDE% is set) ..." -ForegroundColor Cyan
    # Run inside a cmd.exe that has vcvarsall applied. That makes %INCLUDE%,
    # %VCToolsInstallDir%, %WindowsSdkDir%, etc. visible to the editor process
    # — which is what lets `#include <vector>` resolve via the editor's
    # "Go to File" menu without any extra config.
    $runBat = [IO.Path]::GetTempFileName() -replace '\.tmp$', '.bat'
@"
@echo off
call "$vcvarsall" x64 >nul || exit /b 1
"$ExePath" %*
"@ | Set-Content -Path $runBat -Encoding ASCII
    cmd /c $runBat
    Remove-Item $runBat -Force
}
