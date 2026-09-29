@echo off
rem ===========================================================================
rem microDOS - single entry point for host/runtime development.
rem
rem   .\md.bat build host
rem   .\md.bat run host
rem   .\md.bat test
rem   .\md.bat clean
rem
rem Pico 2 becomes a build target after the host interpreter/AOT equivalence
rem layer is stable. Keeping the runtime platform-neutral is intentional.
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "MD_ROOT=%CD%"

set "CMD=%~1"
if "%CMD%"=="" set "CMD=help"
shift /1

if /i "%CMD%"=="build" goto build
if /i "%CMD%"=="run" goto run
if /i "%CMD%"=="test" goto test
if /i "%CMD%"=="bench" goto bench
if /i "%CMD%"=="clean" goto clean
if /i "%CMD%"=="help" goto help
if /i "%CMD%"=="-h" goto help
if /i "%CMD%"=="--help" goto help

echo ERROR: unknown command "%CMD%".
exit /b 1

:build
if /i not "%~1"=="host" (
    echo ERROR: current bootstrap build target is "host".
    exit /b 1
)
cmake -S "%MD_ROOT%" -B "%MD_ROOT%\build-host" -DMD_BUILD_HOST=ON -DMD_BUILD_TESTS=ON || exit /b 1
cmake --build "%MD_ROOT%\build-host" --config Release || exit /b 1
exit /b 0

:run
if /i not "%~1"=="host" (
    echo ERROR: current bootstrap run target is "host".
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

:build_host_if_needed
if not exist "%MD_ROOT%\build-host\CMakeCache.txt" (
    cmake -S "%MD_ROOT%" -B "%MD_ROOT%\build-host" -DMD_BUILD_HOST=ON -DMD_BUILD_TESTS=ON || exit /b 1
)
cmake --build "%MD_ROOT%\build-host" --config Release || exit /b 1
exit /b 0

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

:clean
if exist "%MD_ROOT%\build-host" rmdir /s /q "%MD_ROOT%\build-host"
echo clean.
exit /b 0

:help
echo microDOS build and run driver.
echo.
echo   .\md.bat build host
echo   .\md.bat run host
echo   .\md.bat test
echo   .\md.bat bench [rounds]
echo   .\md.bat clean
exit /b 0
