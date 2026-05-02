@echo off
setlocal
title DayZ - Refresh Mods and Restart

cd /d "C:\GameServers\dayz"

echo.
echo  ==================================================
echo    DayZ - Refresh Workshop Mods and Restart Server
echo  ==================================================
echo.

echo [1/4] Stopping DayZServer_x64.exe (if running)...
taskkill /IM DayZServer_x64.exe /F >nul 2>&1
if %ERRORLEVEL%==0 (
    echo       stopped.
) else (
    echo       not running.
)

echo.
echo [2/4] Waiting 5s for file handles to release...
timeout /t 5 /nobreak >nul

echo.
echo [3/4] Running update_mods.ps1 (SteamCMD download + copy + keys)...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0update_mods.ps1"
if errorlevel 1 (
    echo.
    echo  ERROR: update_mods.ps1 failed. Server NOT restarted.
    echo  Check the output above, then re-run this script.
    pause
    exit /b 1
)

echo.
echo [4/4] Starting DayZ server via start_server.bat...
call "%~dp0start_server.bat"

exit /b 0
