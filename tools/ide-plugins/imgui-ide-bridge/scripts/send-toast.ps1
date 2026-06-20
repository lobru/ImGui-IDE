#requires -Version 5.1
<#
.SYNOPSIS
  Send a toast to a running ImGui-IDE instance.
.DESCRIPTION
  Drops a small text file into the IDE's toast inbox (<APPDATA>\ImGuiColorTextEdit\toasts\).
  The IDE polls that folder (~5 Hz), shows the message as a toast, then deletes the file.
  File payload format: "<severity>|<message>"  (severity = info|warn|error|success|ok).
#>
param(
    [Parameter(Mandatory = $true)][string]$Message,
    [ValidateSet('info', 'warn', 'warning', 'error', 'err', 'success', 'ok')]
    [string]$Severity = 'info'
)

$dir = Join-Path $env:APPDATA 'ImGuiColorTextEdit\toasts'
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$stamp = (Get-Date).ToString('yyyyMMddHHmmssfff')
$file  = Join-Path $dir ("{0}_{1}.txt" -f $stamp, (Get-Random -Maximum 99999))
$payload = "$Severity|$Message"

# UTF-8 WITHOUT BOM — a BOM would corrupt the severity prefix the IDE parses.
[System.IO.File]::WriteAllText($file, $payload, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "toast queued ($Severity): $Message"
