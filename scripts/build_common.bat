@echo off
rem Scripts - the one step the build scripts here share.
rem
rem Called, never run on its own:
rem
rem   call scripts\build_common.bat :setup
rem   call scripts\build_common.bat :close   <exe>
rem   call scripts\build_common.bat :restore <exe>
rem   call scripts\build_common.bat :cpc     <what> <sources> <out> <flags>
rem
rem :setup finds the bundled cpc.exe and makes bin\ if it is not there yet.
rem :close stops the copies of the one exe named that are running out of the
rem path given, and :restore starts that same exe again the way it was started;
rem both are build_guard.ps1, which matches a whole path and so leaves a copy
rem running from anywhere else alone, and which remembers what it stopped in a
rem file of that exe's own - so a build here can only ever close and start the
rem program it is the build of. build_crypto.bat uses neither: nothing on this
rem PC runs that one from bin\.
rem
rem :cpc compiles one exe and adds its name to FAILED when it did not link.
rem FAILED lives in the caller - the build script reads it back - so there is
rem no setlocal in this file.

if /i "%~1"==":setup"   goto :setup
if /i "%~1"==":close"   goto :close
if /i "%~1"==":restore" goto :restore
if /i "%~1"==":cpc"     goto :cpc

echo build_common: no such step: %1
exit /b 2

:setup
if not exist bin mkdir bin
if defined TOOL_CPC (
    set "CPC=%TOOL_CPC%"
    exit /b 0
)
set "CPC=cpc"
if exist "%CD%\cpc.exe" set "CPC=%CD%\cpc.exe"
exit /b 0

:close
if "%~2"=="" (
    echo build_common: close needs the exe it is about to overwrite
    exit /b 2
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_guard.ps1" close "%~2"
if errorlevel 1 echo build_guard could not do its job - a locked exe will fail below
exit /b 0

:restore
if "%~2"=="" (
    echo build_common: restore needs the exe this build closed
    exit /b 2
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_guard.ps1" restore "%~2"
exit /b 0

:cpc
set "WHAT=%~2"
set "SRC=%~3"
set "OUT=%~4"
set "FLAGS=%~5"
set "LOG=%TEMP%\tools_cpc.log"

"%CPC%" %SRC% -o "%OUT%" %TOOL_INCLUDES% -I src %FLAGS% >"%LOG%" 2>&1
if not errorlevel 1 goto :cpc_said

rem cpc, with a second try when - and only when - it fell over inside its own
rem headers. The bundled compiler unpacks its include and lib runtime to
rem %LOCALAPPDATA%\cpc\<version>, and anything else on this PC using that same
rem version re-unpacks it, so a compile can read a tree that is halfway written
rem and report nonsense like a syntax error in winnt.h. The error names that
rem folder, which is how this tells that apart from a real fault in our source.
findstr /c:"AppData/Local/cpc/" "%LOG%" >nul
if errorlevel 1 goto :cpc_failed

echo   cpc fell over inside its own headers:
type "%LOG%"
echo   that is what it does while something else on this PC re-unpacks
echo   %LOCALAPPDATA%\cpc. Running that step once more.
del "%LOG%" 2>nul
"%CPC%" %SRC% -o "%OUT%" %TOOL_INCLUDES% -I src %FLAGS% >>"%LOG%" 2>&1
if errorlevel 1 goto :cpc_failed
echo   it built that time

:cpc_said
rem Warnings on a build that worked still deserve the light of day.
type "%LOG%" 2>nul
del "%LOG%" 2>nul
exit /b 0

:cpc_failed
type "%LOG%"
del "%LOG%" 2>nul
set "FAILED=%FAILED% %WHAT%"
exit /b 1
