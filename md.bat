@echo off
rem ===========================================================================
rem microDOS - single entry point for host/runtime/recompiler development.
rem
rem   .\md.bat build host
rem   .\md.bat run host
rem   .\md.bat test
rem   .\md.bat bench [rounds]
rem   .\md.bat recomp input.com name [code-end]
rem   .\md.bat clean
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "MD_ROOT=%CD%"
set "CMD=%~1"
if "%CMD%"=="" set "CMD=help"
shift /1

if /i "%CMD%"=="build"  goto build
if /i "%CMD%"=="run"    goto run
if /i "%CMD%"=="test"   goto test
if /i "%CMD%"=="bench"  goto bench
if /i "%CMD%"=="recomp" goto recomp
if /i "%CMD%"=="clean"  goto clean
if /i "%CMD%"=="help"   goto help
if /i "%CMD%"=="-h"     goto help
if /i "%CMD%"=="--help" goto help

echo ERROR: unknown command "%CMD%".
exit /b 1

:build
if /i not "%~1"=="host" (
    echo ERROR: current build target is "host".
    exit /b 1
)
cmake -S "%MD_ROOT%" -B "%MD_ROOT%\build-host" -DMD_BUILD_HOST=ON -DMD_BUILD_TESTS=ON || exit /b 1
cmake --build "%MD_ROOT%\build-host" --config Release || exit /b 1
exit /b 0

:run
if /i not "%~1"=="host" (
    echo ERROR: current run target is "host".
    exit /b 1
)
call :build_host_if_needed || exit /b 1
if exist "%MD_ROOT%\build-host\Release\microdos_host.exe" (
    "%MD_ROOT%\build-host\Release\microdos_host.exe"
) else (
    "%MD_ROOT%\build-host\microdos_host.exe"
)
exit /b %ERRORLEVEL%

:test
call :build_host_if_needed || exit /b 1
ctest --test-dir "%MD_ROOT%\build-host" -C Release --output-on-failure
exit /b %ERRORLEVEL%

:bench
call :build_host_if_needed || exit /b 1
set "ROUNDS=%~1"
if "%ROUNDS%"=="" set "ROUNDS=1000"
if exist "%MD_ROOT%\build-host\Release\microdos_bench.exe" (
    "%MD_ROOT%\build-host\Release\microdos_bench.exe" %ROUNDS%
) else (
    "%MD_ROOT%\build-host\microdos_bench.exe" %ROUNDS%
)
exit /b %ERRORLEVEL%

:recomp
set "INPUT=%~1"
set "NAME=%~2"
set "CODE_END=%~3"
if "%INPUT%"=="" goto recomp_usage
if "%NAME%"=="" goto recomp_usage
if not exist "%INPUT%" (
    echo ERROR: input COM file not found: %INPUT%
    exit /b 1
)
call :build_host_if_needed || exit /b 1
if not exist "%MD_ROOT%\build-recomp" mkdir "%MD_ROOT%\build-recomp" || exit /b 1
set "DR_EXE=%MD_ROOT%\build-host\dosrecomp.exe"
if exist "%MD_ROOT%\build-host\Release\dosrecomp.exe" set "DR_EXE=%MD_ROOT%\build-host\Release\dosrecomp.exe"
if not exist "%DR_EXE%" (
    echo ERROR: dosrecomp executable was not built.
    exit /b 1
)
if "%CODE_END%"=="" (
    "%DR_EXE%" --input "%INPUT%" --output-c "%MD_ROOT%\build-recomp\%NAME%_recomp.c" --output-h "%MD_ROOT%\build-recomp\%NAME%_recomp.h" --symbol "md_recomp_%NAME%" --dump
) else (
    "%DR_EXE%" --input "%INPUT%" --output-c "%MD_ROOT%\build-recomp\%NAME%_recomp.c" --output-h "%MD_ROOT%\build-recomp\%NAME%_recomp.h" --symbol "md_recomp_%NAME%" --code-end "%CODE_END%" --dump
)
exit /b %ERRORLEVEL%

:recomp_usage
echo Usage:
echo   .\md.bat recomp input.com name [code-end]
echo Example:
echo   .\md.bat recomp tests\programs\hello.com hello 0x10c
exit /b 1

:build_host_if_needed
if not exist "%MD_ROOT%\build-host\CMakeCache.txt" (
    cmake -S "%MD_ROOT%" -B "%MD_ROOT%\build-host" -DMD_BUILD_HOST=ON -DMD_BUILD_TESTS=ON || exit /b 1
)
cmake --build "%MD_ROOT%\build-host" --config Release || exit /b 1
exit /b 0

:clean
if exist "%MD_ROOT%\build-host" rmdir /s /q "%MD_ROOT%\build-host"
if exist "%MD_ROOT%\build-recomp" rmdir /s /q "%MD_ROOT%\build-recomp"
echo clean.
exit /b 0

:help
echo microDOS build, run, and recompilation driver.
echo.
echo   .\md.bat build host
echo   .\md.bat run host
echo   .\md.bat test
echo   .\md.bat bench [rounds]
echo   .\md.bat recomp input.com name [code-end]
echo   .\md.bat clean
echo.
echo dosrecomp example:
echo   .\md.bat recomp tests\programs\hello.com hello 0x10c
exit /b 0
