@echo off
::=============================================================================
::  Builds the RadioVoice application.
::
::  Usage:
::      build.cmd              release build with MinGW
::      build.cmd debug        debug build
::      build.cmd msvc         build with Visual Studio instead
::      build.cmd no-vst3      without the plugin host (no copyleft dependency)
::
::  The first run downloads Dear ImGui, nlohmann/json and the VST3 SDK, so it
::  needs network access and takes about half a minute longer.
::=============================================================================

setlocal
cd /d "%~dp0"

set PRESET=mingw
if /i "%~1"=="debug"   set PRESET=mingw-debug
if /i "%~1"=="msvc"    set PRESET=msvc
if /i "%~1"=="no-vst3" set PRESET=no-vst3

where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo [X] cmake not found. Install it, for example:  scoop install cmake ninja mingw git
    goto :fail
)

echo Configuring ^(preset: %PRESET%^)...
cmake --preset %PRESET%
if %errorlevel% neq 0 goto :fail

echo.
echo Building...
cmake --build --preset %PRESET%
if %errorlevel% neq 0 goto :fail

echo.
echo ================================================================
if /i "%PRESET%"=="msvc" (
    echo  Built: build-msvc\bin\RadioVoice.exe
) else if /i "%PRESET%"=="mingw-debug" (
    echo  Built: build-debug\bin\RadioVoice.exe
) else if /i "%PRESET%"=="no-vst3" (
    echo  Built: build-novst3\bin\RadioVoice.exe
) else (
    echo  Built: build\bin\RadioVoice.exe
)
echo ================================================================
exit /b 0

:fail
echo.
echo Build failed - see the output above.
exit /b 1
