@echo off
setlocal EnableDelayedExpansion

REM Priority 1: Environment variables (ANDROID_NDK_HOME / ANDROID_NDK_ROOT / NDK_DIR)
if defined ANDROID_NDK_HOME (
    set "RESOLVED_NDK=%ANDROID_NDK_HOME%"
) else if defined ANDROID_NDK_ROOT (
    set "RESOLVED_NDK=%ANDROID_NDK_ROOT%"
) else if defined NDK_DIR (
    set "RESOLVED_NDK=%NDK_DIR%"
) else if exist "%LOCALAPPDATA%\Android\Sdk\ndk" (
    for /f "delims=" %%D in ('dir /b /ad /o-n "%LOCALAPPDATA%\Android\Sdk\ndk"') do (
        if exist "%LOCALAPPDATA%\Android\Sdk\ndk\%%D\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android34-clang.cmd" (
            set "RESOLVED_NDK=%LOCALAPPDATA%\Android\Sdk\ndk\%%D"
            goto :ndk_found
        )
    )
)

:ndk_found
if not defined RESOLVED_NDK (
    set "RESOLVED_NDK=C:\Users\yuand\AppData\Local\Android\Sdk\ndk\26.0.10792818"
)

set "CC=%RESOLVED_NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android34-clang.cmd"
if not exist "%CC%" (
    where aarch64-linux-android34-clang.cmd >nul 2>&1
    if !ERRORLEVEL! equ 0 (
        set "CC=aarch64-linux-android34-clang.cmd"
    )
)

echo [BUILD] Using NDK Toolchain: %CC%
if not exist bin mkdir bin

echo [BUILD] 1/2 Compiling AstraThermald daemon...
call "%CC%" -O2 -Wall -Wextra -Iinclude ^
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

if !ERRORLEVEL! neq 0 (
    echo [BUILD] FAILED building astra_thermald!
    exit /b !ERRORLEVEL!
)

echo [BUILD] 2/2 Compiling AstraLauncher standalone supervisor...
call "%CC%" -O2 -Wall -Wextra -Iinclude ^
    src\astra_launcher.c ^
    -o bin\astra_launcher

if !ERRORLEVEL! neq 0 (
    echo [BUILD] FAILED building astra_launcher!
    exit /b !ERRORLEVEL!
)

echo [BUILD] SUCCESS! Outputs: bin\astra_thermald, bin\astra_launcher
endlocal
