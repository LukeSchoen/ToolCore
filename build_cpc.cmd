@echo off
rem Build the endpoint with cpc: connection and local controls only.
setlocal
cd /d "%~dp0"
set "ROOT=%CD%\"
set "FAILED="
set "TOOL_CPC="
set "TOOL_INCLUDES="

call "%ROOT%scripts\build_common.bat" :setup
call "%ROOT%scripts\build_common.bat" :close "bin\tool.exe"
echo.

echo Building ToolCore\bin\tool.exe...
call "%ROOT%scripts\build_common.bat" :cpc tool.exe "src\client.c src\common.c src\crypto.c src\audio.c src\mirror.c src\TextPaste.c src\tool_features_none.c" "bin\tool.exe" "-lws2_32 -ladvapi32 -luser32 -lgdi32 -lole32 -lshell32"

echo.
if defined FAILED echo Did not build:%FAILED%
if not defined FAILED echo Built ToolCore\bin\tool.exe
echo.

call "%ROOT%scripts\build_common.bat" :restore "bin\tool.exe"
if defined FAILED exit /b 1
exit /b 0
