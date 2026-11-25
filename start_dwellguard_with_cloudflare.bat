@echo off
setlocal EnableExtensions
title DwellGuard Launcher

REM Always work from the folder that contains this file (and server.js).
cd /d "%~dp0"

REM ====================================================================
REM  SETTINGS - the only part you may need to edit
REM ====================================================================
REM Address to open. Use EXACTLY the hostname you set in Cloudflare:
REM dwellguard.casa   or   www.dwellguard.casa
set "SITE_URL=https://dwellguard.casa"
set "LOCAL_URL=http://127.0.0.1:3000"

REM Name and ID of the device. The ID must match DEVICE_ID in the ESP-01 config.h.
set "DEVICE_NAME=Living room sensor"
set "DEVICE_ID=DEV-4F2A-9B7C"

REM To close account creation, remove the word REM at the start of the next line.
REM set "ALLOW_REGISTRATION=false"

REM Cloudflare tunnel token. Best: create a file called cloudflare-token.txt
REM next to this BAT file and paste ONLY the token in it (one line, plain text).
REM Or paste the token between the quotes below.
set "TUNNEL_TOKEN="
REM ====================================================================

echo ==========================================
echo          DWELLGUARD ONE-CLICK START
echo ==========================================
echo.

REM ---------- 1. Node.js (version 22.13 or newer) ----------
where node >nul 2>nul
if errorlevel 1 goto :err_node
node -e "const v=process.versions.node.split('.').map(Number);process.exit(v[0]>22||(v[0]===22&&v[1]>=13)?0:1)"
if errorlevel 1 goto :err_node_old

REM ---------- 2. website files ----------
if not exist "server.js" goto :err_server
if not exist "deviceApi.js" goto :err_old_files

REM ---------- 3. curl (built into Windows 10 and 11) ----------
where curl >nul 2>nul
if errorlevel 1 goto :err_curl

REM ---------- 4. Cloudflare tunnel token ----------
if not defined TUNNEL_TOKEN if exist "%~dp0cloudflare-token.txt" set /p TUNNEL_TOKEN=<"%~dp0cloudflare-token.txt"
if not defined TUNNEL_TOKEN goto :err_token
echo(%TUNNEL_TOKEN%|findstr /i /c:"PASTE" >nul
if not errorlevel 1 goto :err_token
echo(%TUNNEL_TOKEN%|findstr /c:" " >nul
if not errorlevel 1 goto :err_token_format

REM ---------- 5. cloudflared.exe ----------
set "CLOUDFLARED="
if exist "%~dp0cloudflared.exe" set "CLOUDFLARED=%~dp0cloudflared.exe"
if not defined CLOUDFLARED for /f "delims=" %%I in ('where cloudflared 2^>nul') do if not defined CLOUDFLARED set "CLOUDFLARED=%%I"
if not defined CLOUDFLARED goto :err_cloudflared

REM ---------- 6. install packages on the first run ----------
if not exist "node_modules" (
    echo Installing DwellGuard packages - first run only...
    call npm.cmd install
    if errorlevel 1 goto :err_npm
)

REM ---------- 7. website server ----------
curl -s -f -o nul -m 2 "%LOCAL_URL%/login.html"
if not errorlevel 1 (
    echo The DwellGuard server is already running.
    goto :server_up
)
echo Starting the DwellGuard server...
start "DwellGuard Server" /D "%~dp0" cmd /k npm.cmd start
set /a TRIES=0
:wait_server
timeout /t 1 /nobreak >nul
curl -s -f -o nul -m 2 "%LOCAL_URL%/login.html"
if not errorlevel 1 goto :server_up
set /a TRIES+=1
if %TRIES% LSS 40 goto :wait_server
goto :err_server_start
:server_up
echo Server is running at %LOCAL_URL%
echo.

REM ---------- 8. Cloudflare tunnel ----------
tasklist /FI "IMAGENAME eq cloudflared.exe" 2>nul | find /I "cloudflared.exe" >nul
if not errorlevel 1 (
    echo The Cloudflare tunnel is already running.
    goto :tunnel_up
)
echo Starting the Cloudflare tunnel...
start "DwellGuard Cloudflare Tunnel" cmd /k ""%CLOUDFLARED%" tunnel run --token %TUNNEL_TOKEN%"
:tunnel_up
echo.

REM ---------- 9. wait until the public address answers ----------
echo Waiting for %SITE_URL% to answer, up to about a minute...
set /a TRIES=0
:wait_site
set "CODE=000"
for /f %%C in ('curl -s -o nul -m 5 -w "%%{http_code}" "%SITE_URL%/login.html"') do set "CODE=%%C"
if "%CODE:~0,1%"=="2" goto :site_up
if "%CODE:~0,1%"=="3" goto :site_up
if "%CODE:~0,1%"=="4" goto :site_up
set /a TRIES+=1
if %TRIES% GEQ 30 goto :site_slow
timeout /t 2 /nobreak >nul
goto :wait_site

:site_up
echo The website is online.
echo Opening %SITE_URL%
start "" "%SITE_URL%"
goto :done

:site_slow
echo.
echo The public address did not answer yet. Last answer code: %CODE%
echo   - 000 : the address does not exist or is not reachable. Check SITE_URL above
echo           and the public hostname in the Cloudflare dashboard.
echo   - 502 / 530 : the tunnel is up but cannot reach the server. In Cloudflare set the
echo           tunnel service to  http://127.0.0.1:3000
echo Also read the two black windows for red error text.
echo.
echo Opening the local address instead: %LOCAL_URL%
start "" "%LOCAL_URL%"
goto :done

:done
echo.
echo DwellGuard is running. Keep the two black windows open: closing them stops the website.
echo On the very first start the server window prints a DEVICE TOKEN box. Copy it for the ESP-01.
echo.
pause
endlocal
exit /b 0

REM ====================================================================
REM  Error messages
REM ====================================================================
:err_node
echo ERROR: Node.js was not found.
echo Install the LTS version from https://nodejs.org and run this file again.
goto :fail

:err_node_old
echo ERROR: your Node.js is too old. DwellGuard needs version 22.13 or newer.
for /f %%V in ('node -v') do echo You have %%V
echo Install the latest LTS from https://nodejs.org and run this file again.
goto :fail

:err_server
echo ERROR: server.js was not found.
echo Put this file in the same folder as server.js:
echo %~dp0
goto :fail

:err_old_files
echo ERROR: deviceApi.js is missing, so the website files are old.
echo Unzip the newest dwellguard.zip into this folder and run this file again.
goto :fail

:err_curl
echo ERROR: curl.exe was not found. It is part of Windows 10 and 11.
echo Update Windows, or start the server and the tunnel by hand.
goto :fail

:err_token
echo ERROR: the Cloudflare tunnel token is not set.
echo Create a plain text file called cloudflare-token.txt in this folder:
echo %~dp0
echo and paste only the token in it. It is the long text starting with eyJ that
echo Cloudflare shows for your tunnel in the Zero Trust dashboard.
goto :fail

:err_token_format
echo ERROR: the token contains a space. Paste ONLY the token, not the whole command.
echo It is the long text starting with eyJ.
goto :fail

:err_cloudflared
echo ERROR: cloudflared.exe was not found.
echo Put cloudflared.exe in this folder, or add it to PATH.
goto :fail

:err_npm
echo ERROR: npm install failed. Read the messages above.
goto :fail

:err_server_start
echo ERROR: the server did not start within 40 seconds.
echo Look at the window called DwellGuard Server for the red error text.
goto :fail

:fail
echo.
pause
endlocal
exit /b 1
