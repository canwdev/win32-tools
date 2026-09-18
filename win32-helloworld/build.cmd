@echo off
setlocal
rem win32-helloworld build script (32-bit i686 / msvcrt / Windows 7 compatible)
rem usage: build.cmd        -> build hello.exe
rem        build.cmd run    -> build, then run it

set "CC=i686-w64-mingw32-gcc"
where %CC% >nul 2>nul
if errorlevel 1 set "CC=D:\Projects\tools\mingw32\bin\i686-w64-mingw32-gcc.exe"

echo [build] %CC%
"%CC%" -O2 -Wall -Wextra -mwindows -static -s -o hello.exe hello.c
if errorlevel 1 goto fail

echo [ok] hello.exe
if /i "%~1"=="run" start "" hello.exe
exit /b 0

:fail
echo [fail] compile failed
exit /b 1