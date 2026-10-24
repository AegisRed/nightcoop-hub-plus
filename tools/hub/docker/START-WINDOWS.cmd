@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0nightcoop-windows.ps1"
echo.
echo NightCoop setup closed. Press any key to close this window.
pause >nul
