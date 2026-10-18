@echo off
setlocal enabledelayedexpansion

if "%NDK_DIR%"=="" set NDK_DIR=C:\Users\yuand\AppData\Local\Android\Sdk\ndk\26.0.10792818
if not exist "%NDK_DIR%" (
    if not "%ANDROID_NDK_HOME%"=="" set NDK_DIR=%ANDROID_NDK_HOME%
    if not "%ANDROID_NDK_ROOT%"=="" set NDK_DIR=%ANDROID_NDK_ROOT%
)

set CC=%NDK_DIR%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android34-clang.cmd
if not exist "%CC%" set CC=aarch64-linux-android34-clang

echo [BUILD] Building AstraThermald (SM8650) with GPL-3.0...
echo [BUILD] Compiler: %CC%

if not exist bin mkdir bin

call "%CC%" -O2 -Wall -Wextra -pthread -Iinclude ^
    src\main.c ^
    src\sensor_hub.c ^
    src\virtual_synth.c ^
    src\safety_guard.c ^
    src\arbiter_voting.c ^
    src\takeover_ctrl.c ^
    src\hook_injector.c ^
    src\conf_parser.c ^
    crypto\aes128.c ^
    -o bin\astra_thermald

if %ERRORLEVEL% equ 0 (
    echo [BUILD] SUCCESS: bin\astra_thermald
) else (
    echo [BUILD] FAILED with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

endlocal
