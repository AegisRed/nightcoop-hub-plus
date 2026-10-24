@echo off
setlocal
cd /d "%~dp0"
title NightCoop - One-click Windows installation
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0nightcoop-windows.ps1" -Action install
set "NIGHTCOOP_EXIT=%ERRORLEVEL%"
echo.
if not "%NIGHTCOOP_EXIT%"=="0" (
  echo Setup paused or failed. Follow the instructions above, then run this file again.
) else (
  echo NightCoop host setup completed. Keep this window for the address instructions.
)
echo Press any key to close this window.
pause >nul
exit /b %NIGHTCOOP_EXIT%
