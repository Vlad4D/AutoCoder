@echo off
setlocal enabledelayedexpansion

REM ---------------------------------------------------------------
REM  build_update_and_restart.bat — Build + restart AutoCoder
REM
REM  Builds AutoCoder from source, then kills the running instance,
REM  copies the fresh binary to Debug2Release\, and launches it.
REM ---------------------------------------------------------------

set SRC_BUILD=build\default
set BUILD_CFG=Debug
set SRC_EXE=%SRC_BUILD%\%BUILD_CFG%\AutoCoder.exe
set SRC_PDB=%SRC_BUILD%\%BUILD_CFG%\AutoCoder.pdb
set RUN_DIR=%SRC_BUILD%\Debug2Release
set RUN_EXE=%RUN_DIR%\AutoCoder.exe
set RUN_PDB=%RUN_DIR%\AutoCoder.pdb
set CMAKE=cmake

REM ----- Find cmake -----
where %CMAKE% >nul 2>nul
if errorlevel 1 (
    for %%p in (
        "C:\Program Files\CMake\bin\cmake.exe"
        "C:\Program Files (x86)\CMake\bin\cmake.exe"
    ) do if exist %%p set "CMAKE=%%~dpnxs"
)

REM ----- Bootstrap VS 2022 x64 env if needed -----
where cl.exe >nul 2>nul
if errorlevel 1 (
    for %%f in (
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) do if exist %%f (
        echo [build] Loading Visual Studio 2022 x64 environment ...
        call "%%~f" >nul
        goto :env_loaded
    )
    echo [build] WARNING: cl.exe not found -- run from a Developer Command Prompt.
)
:env_loaded

REM ----- Build -----
echo [build] Building AutoCoder ...
"%CMAKE%" --build "%SRC_BUILD%" --config %BUILD_CFG% 2>&1
if errorlevel 1 (
    echo [build] Build failed. Fix errors and re-run.
    pause
    exit /b 1
)
echo [build] Build succeeded.

REM ----- Ensure RUN_DIR exists -----
if not exist "%RUN_DIR%" mkdir "%RUN_DIR%"

REM ----- Give windeployqt a moment to release file handles -----
timeout /t 2 /nobreak >nul

REM ----- Kill any running AutoCoder.exe -----
echo [run] Terminating AutoCoder.exe gracefully ...
taskkill /im AutoCoder.exe >nul 2>nul
timeout /t 2 /nobreak >nul
tasklist /fi "imagename eq AutoCoder.exe" 2>nul | find "AutoCoder.exe" >nul
if not errorlevel 1 (
    echo [run] Force terminating AutoCoder.exe ...
    taskkill /f /im AutoCoder.exe >nul 2>nul
    timeout /t 1 /nobreak >nul
)

REM ----- Copy binary to run directory (retry a few times) -----
echo [run] Copying binary to %RUN_DIR% ...
set RETRIES=3
:retry_copy
copy /y "%SRC_EXE%" "%RUN_EXE%" >nul 2>nul
if errorlevel 1 (
    set /a RETRIES-=1
    if !RETRIES! gtr 0 (
        echo [run] File locked, retrying in 2 seconds ...
        timeout /t 2 /nobreak >nul
        goto :retry_copy
    )
    echo [run] Copy failed after retries — make sure AutoCoder is closed.
    pause
    exit /b 1
)

if exist "%SRC_PDB%" (
    copy /y "%SRC_PDB%" "%RUN_PDB%" >nul 2>nul
)

REM ----- Launch the new instance -----
echo [run] Starting AutoCoder from %RUN_DIR% ...
start "" /d "%~dp0." "%RUN_EXE%"

echo [run] Done.
exit /b 0
