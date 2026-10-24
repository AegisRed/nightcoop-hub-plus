@echo off
setlocal
cd /d "%~dp0"
title NightCoop - Guest PC Firewall setup
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0nightcoop-windows.ps1" -Action firewall
set "NIGHTCOOP_EXIT=%ERRORLEVEL%"
echo.
if not "%NIGHTCOOP_EXIT%"=="0" (
  echo Firewall setup failed or UAC was declined. Read the message above.
) else (
  echo Guest PC rules installed. Docker is NOT required here.
)
echo Press any key to close this window.
pause >nul
exit /b %NIGHTCOOP_EXIT%
