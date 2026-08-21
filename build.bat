@echo off
REM Build script for Windows with MinGW (MSYS2)
REM Run this from MSYS2 MinGW64 terminal or Windows cmd after setting up MSYS2 environment

setlocal enabledelayedexpansion

set BUILD_DIR=build_windows
set STANDALONE=OFF
set SIGN=OFF
set SIGN_CERT=
set SIGN_PASSWORD=
set TIMESTAMP_URL=http://timestamp.digicert.com

REM Parse arguments
:parse_args
if "%~1"=="" goto build
if /i "%~1"=="--standalone" set STANDALONE=ON
if /i "%~1"=="-s" set STANDALONE=ON
if /i "%~1"=="--sign" set SIGN=ON
if /i "%~1"=="--sign-cert" (
    set SIGN_CERT=%~2
    shift
)
if /i "%~1"=="--sign-password" (
    set SIGN_PASSWORD=%~2
    shift
)
if /i "%~1"=="--timestamp-url" (
    set TIMESTAMP_URL=%~2
    shift
)
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
    cmake -Wno-deprecated .. -G Ninja -DBUILD_STANDALONE=ON
) else (
    echo Building with dynamic libraries
    cmake -Wno-deprecated .. -G Ninja
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

if "%SIGN%"=="ON" (
    if "%SIGN_CERT%"=="" (
        if defined MGVWR_SIGN_CERT_THUMBPRINT set SIGN_CERT=%MGVWR_SIGN_CERT_THUMBPRINT%
        if defined MGVWR_SIGN_CERT_PATH set SIGN_CERT=%MGVWR_SIGN_CERT_PATH%
    )
    if "%SIGN_PASSWORD%"=="" (
        if defined MGVWR_SIGN_CERT_PASSWORD set SIGN_PASSWORD=%MGVWR_SIGN_CERT_PASSWORD%
    )
    if "%SIGN_CERT%"=="" (
        echo.
        echo Signing requested, but no certificate was supplied.
        echo Set --sign-cert <thumbprint|pfx-path> or environment MGVWR_SIGN_CERT_THUMBPRINT / MGVWR_SIGN_CERT_PATH.
        echo Example: build.bat --sign --sign-cert "ABC123..."
        echo Example: build.bat --sign --sign-cert "C:\certs\app.pfx" --sign-password "secret"
    ) else (
        echo.
        echo Signing executable...
        powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign_release.ps1" -AppPath "%~dp0%BUILD_DIR%\mgvwr.exe" -CertThumbprint "%SIGN_CERT%" -CertPassword "%SIGN_PASSWORD%" -TimestampUrl "%TIMESTAMP_URL%"
        if errorlevel 1 (
            echo Signing failed.
            cd ..
            pause
            exit /b 1
        )
        echo Signed executable: %BUILD_DIR%\mgvwr.exe
    )
)

cd ..
pause
