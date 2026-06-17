@echo off
REM ============================================================
REM  HLAE fork - launch script
REM  Runs the staging build produced by build.bat
REM ============================================================

cd /d "%~dp0"

set "EXE=build\staging-release\bin\HLAE.exe"

if not exist "%EXE%" (
    echo HLAE.exe not found at "%EXE%".
    echo You need to compile first - run build.bat.
    pause
    exit /b 1
)

start "" "%EXE%"
