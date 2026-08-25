@echo off
setlocal enabledelayedexpansion

rem ============================================================
rem  x64dbg-mcp 构建脚本
rem  1) 探测 MinGW-w64 工具链（winlibs，须同时含 x86_64 与 i686）
rem  2) 用 gendef/dlltool 从 release 目录的 DLL 生成导入库
rem  3) 编译 x64dbg_mcp.dp64 与 x64dbg_mcp.dp32
rem ============================================================

set ROOT=%~dp0
set SDK=%ROOT%..\pluginsdk
set RELEASE=%ROOT%..\release
set SRC=%ROOT%src
set BUILD=%ROOT%build
set JANSSON=%ROOT%third_party\jansson-2.14\src

if not exist "%SDK%" (echo [ERROR] pluginsdk not found & exit /b 1)
if not exist "%RELEASE%\x64\x64dbg.dll" (echo [ERROR] release\x64 not found & exit /b 1)
if not exist "%RELEASE%\x32\x32dbg.dll" (echo [ERROR] release\x32 not found & exit /b 1)

rem ---------- 探测编译器 ----------
set "GCC64="
set "GCC32="

set "WG=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe"

for %%P in (
  "%WG%\mingw64\bin\x86_64-w64-mingw32-gcc.exe"
  "%LOCALAPPDATA%\Programs\WinLibs\mingw64\bin\x86_64-w64-mingw32-gcc.exe"
  "C:\Program Files\WinLibs\mingw64\bin\x86_64-w64-mingw32-gcc.exe"
  "C:\mingw64\bin\x86_64-w64-mingw32-gcc.exe"
  "C:\msys64\mingw64\bin\x86_64-w64-mingw32-gcc.exe"
) do if not defined GCC64 if exist "%%~P" set "GCC64=%%~P"

for %%P in (
  "%WG%\i686\mingw32\bin\i686-w64-mingw32-gcc.exe"
  "%LOCALAPPDATA%\Programs\WinLibs\mingw32\bin\i686-w64-mingw32-gcc.exe"
  "C:\Program Files\WinLibs\mingw32\bin\i686-w64-mingw32-gcc.exe"
  "C:\mingw32\bin\i686-w64-mingw32-gcc.exe"
  "C:\msys64\mingw32\bin\i686-w64-mingw32-gcc.exe"
) do if not defined GCC32 if exist "%%~P" set "GCC32=%%~P"

rem 兜底：PATH 中查找
if not defined GCC64 for /f "delims=" %%i in ('where x86_64-w64-mingw32-gcc 2^>nul') do if not defined GCC64 set "GCC64=%%i"
if not defined GCC32 for /f "delims=" %%i in ('where i686-w64-mingw32-gcc 2^>nul') do if not defined GCC32 set "GCC32=%%i"

if not defined GCC64 (echo [ERROR] x86_64-w64-mingw32-gcc not found & exit /b 1)
if not defined GCC32 (echo [ERROR] i686-w64-mingw32-gcc not found & exit /b 1)

for %%i in ("%GCC64%") do set "BIN64=%%~dpi"
for %%i in ("%GCC32%") do set "BIN32=%%~dpi"
set "GENDEF=%BIN64%gendef.exe"
set "DLLTOOL64=%BIN64%dlltool.exe"
set "DLLTOOL32=%BIN32%dlltool.exe"

if not exist "%GENDEF%" (echo [ERROR] gendef not found & exit /b 1)

rem ---------- PATH 处理 ----------
rem 各工具链 bin 目录里有同名 DLL（libiconv/libintl 等），若 PATH 中先出现
rem 其他架构的目录，32/64 位 cc1.exe 会加载错误位数的 DLL 而静默失败
rem (0xC000007B)。因此每个架构阶段前，把对应 bin 目录置于 PATH 最前。
set "PATH=%BIN64%;%PATH%"

echo [1/4] Generating import libraries...
mkdir "%BUILD%\lib64" 2>nul
mkdir "%BUILD%\lib32" 2>nul
mkdir "%BUILD%\obj64" 2>nul
mkdir "%BUILD%\obj32" 2>nul

cd /d "%BUILD%"

rem --- x64 导入库 ---
"%GENDEF%" "%RELEASE%\x64\x64dbg.dll"
"%DLLTOOL64%" -d x64dbg.def -D x64dbg.dll -l "%BUILD%\lib64\libx64dbg.a"
"%GENDEF%" "%RELEASE%\x64\x64bridge.dll"
"%DLLTOOL64%" -d x64bridge.def -D x64bridge.dll -l "%BUILD%\lib64\libx64bridge.a"

rem --- x86 导入库 ---
set "PATH=%BIN32%;%PATH%"
"%GENDEF%" "%RELEASE%\x32\x32dbg.dll"
"%DLLTOOL32%" -d x32dbg.def -D x32dbg.dll -l "%BUILD%\lib32\libx32dbg.a"
"%GENDEF%" "%RELEASE%\x32\x32bridge.dll"
"%DLLTOOL32%" -d x32bridge.def -D x32bridge.dll -l "%BUILD%\lib32\libx32bridge.a"

rem 说明：jansson 以源码形式静态编译进插件（third_party\jansson-2.14），
rem 不依赖 x64dbg 附带的 jansson.dll，避免 32 位下跨 CRT 分配/释放导致的堆损坏。

mkdir "%BUILD%\obj64\jansson" 2>nul
mkdir "%BUILD%\obj32\jansson" 2>nul

echo [2/4] Compiling x64 plugin...
for %%F in (dump.c error.c hashtable.c hashtable_seed.c load.c memory.c pack_unpack.c strbuffer.c strconv.c utf.c value.c version.c) do (
  "%GCC64%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DHAVE_CONFIG_H -DUSE_WINDOWS_CRYPTOAPI -DJANSSON_STATIC ^
    -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
    "%JANSSON%\%%F" -o "%BUILD%\obj64\jansson\%%~nF.o" || goto :error
)
rem 插件编译：-Ddllimport= 使 pluginsdk/jansson/jansson.h 中无条件的
rem __declspec(dllimport) 声明失效（jansson 已静态编译进插件），
rem 对 MinGW 系统头无影响（导入库同时提供 __imp_* 与普通符号）。
"%GCC64%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DJANSSON_STATIC -Ddllimport= ^
  -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
  "%SRC%\plugin.c" -o "%BUILD%\obj64\plugin.o" || goto :error
"%GCC64%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DJANSSON_STATIC -Ddllimport= ^
  -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
  "%SRC%\mcp_server.c" -o "%BUILD%\obj64\mcp_server.o" || goto :error
"%GCC64%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DJANSSON_STATIC -Ddllimport= ^
  -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
  "%SRC%\tools.c" -o "%BUILD%\obj64\tools.o" || goto :error

"%GCC64%" -shared -static-libgcc -o "%BUILD%\x64dbg_mcp.dp64" ^
  "%BUILD%\obj64\plugin.o" "%BUILD%\obj64\mcp_server.o" "%BUILD%\obj64\tools.o" ^
  "%BUILD%\obj64\jansson\dump.o" "%BUILD%\obj64\jansson\error.o" ^
  "%BUILD%\obj64\jansson\hashtable.o" "%BUILD%\obj64\jansson\hashtable_seed.o" ^
  "%BUILD%\obj64\jansson\load.o" "%BUILD%\obj64\jansson\memory.o" ^
  "%BUILD%\obj64\jansson\pack_unpack.o" "%BUILD%\obj64\jansson\strbuffer.o" ^
  "%BUILD%\obj64\jansson\strconv.o" "%BUILD%\obj64\jansson\utf.o" ^
  "%BUILD%\obj64\jansson\value.o" "%BUILD%\obj64\jansson\version.o" ^
  -L"%BUILD%\lib64" -lx64dbg -lx64bridge -lws2_32 -ladvapi32 || goto :error

echo [3/4] Compiling x86 plugin...
for %%F in (dump.c error.c hashtable.c hashtable_seed.c load.c memory.c pack_unpack.c strbuffer.c strconv.c utf.c value.c version.c) do (
  "%GCC32%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DHAVE_CONFIG_H -DUSE_WINDOWS_CRYPTOAPI -DJANSSON_STATIC ^
    -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
    "%JANSSON%\%%F" -o "%BUILD%\obj32\jansson\%%~nF.o" || goto :error
)
"%GCC32%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DJANSSON_STATIC -Ddllimport= ^
  -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
  "%SRC%\plugin.c" -o "%BUILD%\obj32\plugin.o" || goto :error
"%GCC32%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DJANSSON_STATIC -Ddllimport= ^
  -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
  "%SRC%\mcp_server.c" -o "%BUILD%\obj32\mcp_server.o" || goto :error
"%GCC32%" -c -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DJANSSON_STATIC -Ddllimport= ^
  -I"%JANSSON%" -I"%SDK%" -I"%SDK%\jansson" -I"%SRC%" ^
  "%SRC%\tools.c" -o "%BUILD%\obj32\tools.o" || goto :error

"%GCC32%" -shared -static-libgcc -o "%BUILD%\x64dbg_mcp.dp32" ^
  "%BUILD%\obj32\plugin.o" "%BUILD%\obj32\mcp_server.o" "%BUILD%\obj32\tools.o" ^
  "%BUILD%\obj32\jansson\dump.o" "%BUILD%\obj32\jansson\error.o" ^
  "%BUILD%\obj32\jansson\hashtable.o" "%BUILD%\obj32\jansson\hashtable_seed.o" ^
  "%BUILD%\obj32\jansson\load.o" "%BUILD%\obj32\jansson\memory.o" ^
  "%BUILD%\obj32\jansson\pack_unpack.o" "%BUILD%\obj32\jansson\strbuffer.o" ^
  "%BUILD%\obj32\jansson\strconv.o" "%BUILD%\obj32\jansson\utf.o" ^
  "%BUILD%\obj32\jansson\value.o" "%BUILD%\obj32\jansson\version.o" ^
  -L"%BUILD%\lib32" -lx32dbg -lx32bridge -lws2_32 -ladvapi32 || goto :error

echo [4/4] Done.
echo   x64: %BUILD%\x64dbg_mcp.dp64
echo   x86: %BUILD%\x64dbg_mcp.dp32
exit /b 0

:error
echo [ERROR] build failed
exit /b 1
