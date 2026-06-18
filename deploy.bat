@echo off
setlocal enabledelayedexpansion

set QT_DIR=C:\Qt\6.11.1\msvc2022_64
set PROJECT_DIR=%~dp0
set SRC_EXE=%PROJECT_DIR%build\Release\PA_Spiral_PAM.exe
set DIST_DIR=%PROJECT_DIR%dist

if not exist "%SRC_EXE%" (
    echo [ERROR] %SRC_EXE% not found
    echo Build Release first: cmake --build build --config Release
    exit /b 1
)

echo [1/4] Cleaning dist...
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

echo [2/4] Copying exe...
copy "%SRC_EXE%" "%DIST_DIR%\" > nul

echo [3/4] Running windeployqt...
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations "%DIST_DIR%\PA_Spiral_PAM.exe"

echo [4/4] Copying VC runtime...

set "VC_TOOLS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
set "CRT_DIR="

for /d %%d in ("%VC_TOOLS%\*") do (
    for /d %%e in ("%%d\Redist\x64\Microsoft.VC*.CRT") do set "CRT_DIR=%%e"
)

if not "%CRT_DIR%"=="" (
    echo   from: %CRT_DIR%
    copy "%CRT_DIR%\vcruntime140.dll"         "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\vcruntime140_1.dll"       "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\msvcp140.dll"             "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\msvcp140_1.dll"           "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\msvcp140_2.dll"           "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\msvcp140_atomic_wait.dll" "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\msvcp140_codecvt_ids.dll" "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\concrt140.dll"            "%DIST_DIR%\" > nul 2>&1
    copy "%CRT_DIR%\vccorlib140.dll"          "%DIST_DIR%\" > nul 2>&1
) else (
    echo [WARN] VC runtime not found, install vc_redist.x64.exe on target machine
)

if exist "%DIST_DIR%\translations" rmdir /s /q "%DIST_DIR%\translations"

echo.
echo === Deploy complete: %DIST_DIR% ===
dir "%DIST_DIR%" /b
