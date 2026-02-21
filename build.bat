@echo off
REM Build script for Windows with MinGW (MSYS2)
REM Run this from MSYS2 MinGW64 terminal or Windows cmd after setting up MSYS2 environment

setlocal enabledelayedexpansion

set BUILD_DIR=build_windows
set STANDALONE=OFF

REM Parse arguments
:parse_args
if "%~1"=="" goto build
if /i "%~1"=="--standalone" set STANDALONE=ON
if /i "%~1"=="-s" set STANDALONE=ON
shift
goto parse_args

:build
REM Clean and create build directory
if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

REM Configure with CMake
echo Configuring build...
if "%STANDALONE%"=="ON" (
    echo Building standalone executable (static linking)
    cmake .. -G Ninja -DBUILD_STANDALONE=ON
) else (
    echo Building with dynamic libraries
    cmake .. -G Ninja
)

if errorlevel 1 (
    echo CMake configuration failed!
    cd ..
    pause
    exit /b 1
)

REM Build
echo Building...
ninja

if errorlevel 1 (
    echo Build failed!
    cd ..
    pause
    exit /b 1
)

echo.
echo Build complete!
echo Executable: %BUILD_DIR%\mgvwr.exe
if "%STANDALONE%"=="ON" (
    echo Mode: Standalone (fully static)
) else (
    echo Mode: Dynamic linking (requires DLLs)
)

cd ..
pause
