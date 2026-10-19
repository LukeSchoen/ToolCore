@echo off
rem Build the endpoint with Visual Studio's C compiler.
setlocal EnableExtensions
cd /d "%~dp0"

set "ROOT=%CD%\"
set "PLATFORM=%~1"
if not defined PLATFORM set "PLATFORM=x64"
if /i "%PLATFORM%"=="x86" set "PLATFORM=Win32"
if /i not "%PLATFORM%"=="x64" if /i not "%PLATFORM%"=="Win32" (
    echo Usage: build_msvc.cmd [x64^|x86]
    exit /b 2
)

set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if defined ProgramFiles(x86) if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
        if not defined MSBUILD set "MSBUILD=%%I"
    )
)

rem Also support Build Tools installations and a Developer Command Prompt.
if not defined MSBUILD if defined MSBuildToolsPath if exist "%MSBuildToolsPath%\MSBuild.exe" (
    set "MSBUILD=%MSBuildToolsPath%\MSBuild.exe"
)
if not defined MSBUILD (
    for /f "delims=" %%I in ('where msbuild 2^>nul') do (
        if not defined MSBUILD set "MSBUILD=%%I"
    )
)
if not defined MSBUILD for %%V in (2022 2019 2017) do (
    for %%E in (Enterprise Professional Community BuildTools) do (
        if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\MSBuild\Current\Bin\MSBuild.exe" (
            set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\MSBuild\Current\Bin\MSBuild.exe"
        )
        if not defined MSBUILD if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\%%E\MSBuild\Current\Bin\MSBuild.exe" (
            set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\%%E\MSBuild\Current\Bin\MSBuild.exe"
        )
    )
)

if not defined MSBUILD (
    echo MSBuild was not found.
    echo Install Visual Studio or Visual Studio Build Tools with the C++ workload.
    exit /b 1
)

echo Using MSBuild: %MSBUILD%
echo Configuration: Release^|%PLATFORM%

set "FAILED="
call "%ROOT%scripts\build_common.bat" :close "bin\tool.exe"
echo.
echo Building ToolCore\bin\tool.exe...
"%MSBUILD%" "%ROOT%ToolCore.vcxproj" /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=%PLATFORM%
if errorlevel 1 set "FAILED=1"
echo.

if defined FAILED echo Did not build: tool.exe
if not defined FAILED echo Built ToolCore\bin\tool.exe
echo.

call "%ROOT%scripts\build_common.bat" :restore "bin\tool.exe"
if defined FAILED exit /b 1
exit /b 0
