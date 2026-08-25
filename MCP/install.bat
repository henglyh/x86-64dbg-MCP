@echo off
rem ============================================================
rem  x64dbg-mcp install script
rem  Copies build outputs into the release plugins directories
rem ============================================================
setlocal
set ROOT=%~dp0
set RELEASE=%ROOT%..\release

if not exist "%ROOT%build\x64dbg_mcp.dp64" (echo [ERROR] build\x64dbg_mcp.dp64 not found, run build.bat first & exit /b 1)
if not exist "%ROOT%build\x64dbg_mcp.dp32" (echo [ERROR] build\x64dbg_mcp.dp32 not found, run build.bat first & exit /b 1)

mkdir "%RELEASE%\x64\plugins" 2>nul
mkdir "%RELEASE%\x32\plugins" 2>nul

copy /y "%ROOT%build\x64dbg_mcp.dp64" "%RELEASE%\x64\plugins\" >nul || (echo [ERROR] copy dp64 failed - is x64dbg running? & exit /b 1)
copy /y "%ROOT%build\x64dbg_mcp.dp32" "%RELEASE%\x32\plugins\" >nul || (echo [ERROR] copy dp32 failed - is x32dbg running? & exit /b 1)

echo [OK] installed:
echo   %RELEASE%\x64\plugins\x64dbg_mcp.dp64
echo   %RELEASE%\x32\plugins\x64dbg_mcp.dp32
echo Restart x96dbg to load the plugin.
exit /b 0
