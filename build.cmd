@echo off
::=============================================================================
::  Builds the RadioVoice APPLICATION  ->  build\bin\RadioVoice.exe
::
::  For the kernel driver, use driver\build-driver.cmd instead.
::
::  Usage:
::      build.cmd              release build, stripped   (this is the normal one)
::      build.cmd reldbg       release plus debug symbols, for stack traces
::      build.cmd debug        unoptimised, with assertions
::      build.cmd msvc         Visual Studio toolchain instead of MinGW
::      build.cmd no-vst3      without the plugin host (no copyleft dependency)
::
::  The first run downloads Dear ImGui, nlohmann/json and the VST3 SDK, so it
::  needs network access and takes about half a minute longer.
::=============================================================================

setlocal
cd /d "%~dp0"

set PRESET=mingw
set OUTDIR=build
if /i "%~1"=="reldbg"  ( set PRESET=mingw-reldbg & set OUTDIR=build-reldbg )
if /i "%~1"=="debug"   ( set PRESET=mingw-debug  & set OUTDIR=build-debug  )
if /i "%~1"=="msvc"    ( set PRESET=msvc         & set OUTDIR=build-msvc   )
if /i "%~1"=="no-vst3" ( set PRESET=no-vst3      & set OUTDIR=build-novst3 )

echo.
echo === RadioVoice application ^(preset: %PRESET%^) ===
echo.

where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo [X] cmake not found. Install it, for example:
    echo        scoop install cmake ninja mingw git
    goto :fail
)

cmake --preset %PRESET%
if %errorlevel% neq 0 goto :fail

cmake --build --preset %PRESET%
if %errorlevel% neq 0 goto :fail

echo.
echo ================================================================
echo  Built: %OUTDIR%\bin\RadioVoice.exe
echo ================================================================
exit /b 0

:fail
echo.
echo Build failed - see the output above.
exit /b 1
