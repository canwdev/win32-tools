@echo off
rem ============================================================
rem  simple-hyperv-win32 build script
rem  ASCII only + CRLF line endings (cmd.exe misparses LF + UTF-8)
rem  No multi-line "if (...)" blocks: use single-line if / goto only.
rem ============================================================
setlocal
set HERE=%~dp0
set GCC=x86_64-w64-mingw32-gcc
set RC=windres
where %GCC% >nul 2>nul || set GCC=D:\Projects\tools\mingw64\bin\x86_64-w64-mingw32-gcc.exe
where %RC%  >nul 2>nul || set RC=D:\Projects\tools\mingw64\bin\windres.exe

if /i "%~1"=="clean" goto clean

echo [1/3] windres : res.rc -^> res.o
"%RC%" -O coff -i "%HERE%res.rc" -o "%HERE%res.o"
if errorlevel 1 goto fail

echo [2/3] gcc     : simplehyperv.c -^> simplehyperv.exe
"%GCC%" -O2 -Wall -Wextra -municode -mwindows -static -s -Wl,--no-insert-timestamp -o "%HERE%simplehyperv.exe" "%HERE%simplehyperv.c" "%HERE%res.o" -lcomctl32 -lshell32 -lcomdlg32 -ladvapi32 -lgdi32 -luser32
if errorlevel 1 goto fail

echo [3/3] done
for %%F in ("%HERE%simplehyperv.exe") do echo       %%~zF bytes   %%~fF

if /i "%~1"=="test"     goto test
if /i "%~1"=="selftest" goto test
if /i "%~1"=="run"      goto run
exit /b 0

:test
rem res.o must be linked in: the self-test asserts the icon resource really is
rem RT_GROUP_ICON(101). Get that wrong and the exe silently shows the default
rem icon -- compiles fine, runs fine, only your eyes notice. So assert it.
echo [test] building console self-test from the same source
"%GCC%" -DSHV_SELFTEST -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -static -s -o "%HERE%selftest.exe" "%HERE%simplehyperv.c" "%HERE%res.o" -lcomctl32 -lshell32 -lcomdlg32 -ladvapi32 -lgdi32 -luser32
if errorlevel 1 goto fail
"%HERE%selftest.exe"
if errorlevel 1 goto fail
exit /b 0

:run
start "" "%HERE%simplehyperv.exe"
exit /b 0

:clean
del /q "%HERE%res.o" "%HERE%simplehyperv.exe" "%HERE%selftest.exe" "%HERE%_shv_dbg.txt" 2>nul
echo cleaned
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1