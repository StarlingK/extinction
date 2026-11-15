@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set "rc=%errorlevel%"
pause
exit /b %rc%
