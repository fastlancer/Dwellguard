@echo off
setlocal EnableExtensions
title DwellGuard launcher

rem ===== EDIT THIS ONE LINE =====
set "TUNNEL_TOKEN=eyJhIjoiNjBhY2Y0NDQ3MjhhNWQzZjM5OGU1MWZjODU4ZjNkNWYiLCJ0IjoiYWFkZWVlNTctNzVkNi00YjlhLTg0NzItYTkwMTJiMjY3YTJhIiwicyI6Ik5EWXdNVFU0WVRZdE1UQTFNaTAwTlRGbExXRTRPR0l0TWpjelpqVmhORGszT0dVd1lXSmxZMll4WTJZdE1ERTFPQzAwTlRVNUxXSXpPR0V0WkRFME1EUTNaVE0xT0RnMyJ9"
rem ==============================

rem Always work in the folder this file is in
cd /d "%~dp0"
echo Working folder: %CD%
echo.

if not exist "server.js" (
  echo [ERROR] server.js not found in this folder.
  echo Put this .bat file INSIDE the dwellguard folder, next to server.js.
  pause
  exit /b 1
)

where node >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Node.js is not installed or not in PATH.
  echo Install the LTS version from nodejs.org, then reopen this file.
  pause
  exit /b 1
)

for /f %%v in ('node -p "process.versions.node"') do set "NODEVER=%%v"
for /f "delims=." %%m in ("%NODEVER%") do set "NODEMAJOR=%%m"
echo Node version: %NODEVER%
if %NODEMAJOR% LSS 22 (
  echo [ERROR] DwellGuard needs Node 22.13 or newer. You have %NODEVER%.
  echo Install the current LTS from nodejs.org, then reopen this file.
  pause
  exit /b 1
)

if not exist "node_modules" (
  echo node_modules missing - running npm install...
  call npm install
  if errorlevel 1 (
    echo [ERROR] npm install failed. Scroll up for the "npm error" lines.
    pause
    exit /b 1
  )
)

echo Starting the website server in its own window...
start "DwellGuard server" cmd /k "cd /d "%~dp0" && node server.js"

echo Waiting for http://127.0.0.1:3000 to answer...
set /a TRIES=0
:waitloop
set /a TRIES+=1
curl -s -o nul http://127.0.0.1:3000/login.html
if not errorlevel 1 goto serverup
if %TRIES% GEQ 30 goto serverfail
timeout /t 1 /nobreak >nul
goto waitloop

:serverfail
echo.
echo [ERROR] The website did not start within 30 seconds.
echo Look at the window titled "DwellGuard server" - it shows the real error.
echo Not starting the tunnel, because there is nothing for it to connect to.
pause
exit /b 1

:serverup
echo Website is answering on http://127.0.0.1:3000
echo.

if "%TUNNEL_TOKEN%"=="PASTE_YOUR_CLOUDFLARE_TUNNEL_TOKEN_HERE" (
  echo [WARNING] No tunnel token set. Website runs locally only.
  goto openbrowser
)

where cloudflared >nul 2>nul
if errorlevel 1 (
  echo [WARNING] cloudflared not found in PATH. Website runs locally only.
  goto openbrowser
)

echo Starting Cloudflare tunnel in its own window...
start "Cloudflare tunnel" cmd /k "cloudflared tunnel run --token %TUNNEL_TOKEN%"

:openbrowser
start "" http://127.0.0.1:3000
echo.
echo Done. Keep the two other windows open. Close them to stop DwellGuard.
pause
