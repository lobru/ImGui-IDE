@echo off
:: Wrapper — forwards all args to build.ps1
:: Usage: build [Debug|Release] [-Clean] [-Run]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
