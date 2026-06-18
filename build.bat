@echo off
setlocal

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build"
set "QT_DIR=C:\Qt\6.11.1\msvc2022_64"

echo === PA_Spiral_PAM Build Script ===

if not exist "%QT_DIR%" (
    echo [ERROR] Qt not found: %QT_DIR%
    exit /b 1
)

if "%~1"=="" (
    set "MODE=Release"
) else (
    set "MODE=%~1"
)

if /i "%MODE%"=="clean" (
    echo [CLEAN] Removing build directory...
    rmdir /s /q "%BUILD_DIR%" 2>nul
    echo Done.
    exit /b 0
)

if /i "%MODE%"=="dist" (
    echo [DIST] Building Release + packaging...
    call "%PROJECT_DIR%build.bat" Release
    if errorlevel 1 exit /b 1
    call "%PROJECT_DIR%deploy.bat"
    exit /b 0
)

echo [CONFIG] Mode=%MODE%
echo [CONFIG] Qt=%QT_DIR%

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo [CONFIG] Running cmake...
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%"
if errorlevel 1 (
    echo [ERROR] CMake configure failed
    exit /b 1
)

echo [BUILD] Compiling %MODE%...
cmake --build "%BUILD_DIR%" --config %MODE%
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

echo [OK] %BUILD_DIR%\%MODE%\PA_Spiral_PAM.exe
