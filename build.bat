@echo off
setlocal
cd /d "%~dp0"
if not exist "isofile" mkdir "isofile"
if not exist "isofile\.gitkeep" type nul > "isofile\.gitkeep"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set "code=%errorlevel%"
echo.
if "%code%"=="0" (
  echo Finished. The program is in the dist\windows folder.
) else (
  echo The build did not finish. Read the message above, or open build-log.txt.
)
echo.
pause
exit /b %code%
