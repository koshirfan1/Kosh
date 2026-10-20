@echo off
setlocal enabledelayedexpansion

echo ─────────────────────────────────────────────────────────────
echo  Khan Language — Windows Build Script
echo ─────────────────────────────────────────────────────────────

:: Check for GCC
where gcc >nul 2>1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] gcc not found. Please install MinGW-w64 and add it to your PATH.
    pause
    exit /b 1
)

:: Compilation Flags
set CFLAGS=-std=c11 -Wall -Wextra -O2 -Isrc -D_POSIX_C_SOURCE=200809L -Wno-implicit-function-declaration -Wno-builtin-declaration-mismatch -Wno-cast-function-type
set LDFLAGS=-lm -lwinhttp -lshell32 -lws2_32 -ladvapi32

:: Source Files
set SRCS=src/lexer.c src/parser.c src/ast.c src/chunk.c src/value.c src/compiler.c src/vm.c src/vm_libs.c src/interpreter.c src/khan_stdlib.c src/json_lib.c src/datetime_lib.c src/requests_lib.c src/webi_lib.c src/sqlite_lib.c src/vision_lib.c src/vision_cv.c src/vision_cascade.c src/main.c
set KH_SRCS=src/kh.c

echo [1/2] Building khan.exe (Interpreter)...
gcc %CFLAGS% %SRCS% -o khan.exe %LDFLAGS%
if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] Built khan.exe
) else (
    echo [ERROR] Failed to build khan.exe
    pause
    exit /b %ERRORLEVEL%
)

echo [2/2] Building kh.exe (Package Manager)...
gcc %CFLAGS% %KH_SRCS% -o kh.exe %LDFLAGS%
if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] Built kh.exe
) else (
    echo [ERROR] Failed to build kh.exe
    pause
    exit /b %ERRORLEVEL%
)

echo ─────────────────────────────────────────────────────────────
echo  Build Complete!
echo  Run 'khan examples/hello.kh' to get started.
echo ─────────────────────────────────────────────────────────────
pause
