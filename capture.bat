@echo off
setlocal

set "ROOT=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\capture_run.ps1" %*
set "RC=%ERRORLEVEL%"

endlocal & exit /b %RC%
