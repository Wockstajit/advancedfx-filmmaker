@echo off
setlocal
REM ============================================================
REM  CS2 camera-path LIVE - one click
REM  1) Launches CS2 with netcon at 1920x1080 (only if CS2 is
REM     not already running, so it won't kill a running game).
REM  2) Opens the browser dashboard (live marker count, speed
REM     mode, segment progress, console stream + drive buttons).
REM
REM  Usage:
REM    live.bat                 (CS2 1920x1080, netcon 29010, web 8765)
REM ============================================================

cd /d "%~dp0"

set "PORT=29010"
set "WEBPORT=8765"
set "WIDTH=1920"
set "HEIGHT=1080"

if not exist "misc\cs2-live.ps1" (
    echo cs2-live.ps1 not found ^(expected misc\cs2-live.ps1^).
    pause
    exit /b 1
)

REM --- launch CS2 only if it isn't already up ----------------
tasklist /FI "IMAGENAME eq cs2.exe" 2>NUL | find /I "cs2.exe" >NUL
if errorlevel 1 (
    echo Launching CS2 with netcon at %WIDTH%x%HEIGHT% on port %PORT% ...
    powershell -ExecutionPolicy Bypass -File "misc\launch-cs2-netcon.ps1" -Port %PORT% -Width %WIDTH% -Height %HEIGHT%
) else (
    echo CS2 already running - leaving it as is ^(close it first if you want 1920x1080^).
)

REM --- open the live dashboard (auto-retries until netcon is up)
powershell -ExecutionPolicy Bypass -File "misc\cs2-live.ps1" -Port %PORT% -WebPort %WEBPORT%

echo.
echo Dashboard server stopped.
pause
