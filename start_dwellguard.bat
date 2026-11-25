@echo off
title DwellGuard Server
cd /d "%~dp0"

echo =====================================
echo          DWELLGUARD SERVER
echo =====================================
echo.

where node >nul 2>nul
if errorlevel 1 (
    echo ERROR: Node.js was not found.
    echo Install Node.js LTS, then reopen this file.
    echo.
    pause
    exit /b 1
)

if not exist "server.js" (
    echo ERROR: server.js was not found.
    echo Place this BAT file in your DwellGuard project folder,
    echo in the same folder as server.js.
    echo.
    pause
    exit /b 1
)

if not exist "node_modules" (
    echo Dependencies are missing. Installing them now...
    call npm.cmd install
    if errorlevel 1 (
        echo.
        echo ERROR: npm install failed. Review the messages above.
        pause
        exit /b 1
    )
)

echo Starting DwellGuard...
echo Keep this window open while you want the website online.
echo Open http://localhost:3000 in your browser.
echo Press Ctrl+C to stop the server.
echo.

call npm.cmd start

echo.
echo DwellGuard server has stopped.
pause
