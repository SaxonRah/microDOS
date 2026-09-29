@echo off
setlocal EnableExtensions
cd /d "%~dp0\.."
set "MD_ROOT=%CD%"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=dos2"

set "MSDOS_ROOT=%MD_ROOT%\third_party\msdos\v2.0"
if not exist "%MSDOS_ROOT%\bin\MSDOS.SYS" goto no_msdos
if not exist "%MSDOS_ROOT%\bin\COMMAND.COM" goto no_msdos

call :find_probe || exit /b 1
if not exist "%MD_ROOT%\build-analysis" mkdir "%MD_ROOT%\build-analysis" || exit /b 1

if /i "%TARGET%"=="msdos" goto analyze_msdos
if /i "%TARGET%"=="command" goto analyze_command
if /i "%TARGET%"=="dos2" goto analyze_all

echo ERROR: unknown analysis target "%TARGET%".
echo        Expected: msdos, command, or dos2.
exit /b 1

:analyze_all
call :run_msdos || exit /b 1
echo.
call :run_command || exit /b 1
exit /b 0

:analyze_msdos
call :run_msdos
exit /b %ERRORLEVEL%

:analyze_command
call :run_command
exit /b %ERRORLEVEL%

:run_msdos
echo === MS-DOS 2.0 MSDOS.SYS ===
"%DP_EXE%" ^
  --input "%MSDOS_ROOT%\bin\MSDOS.SYS" ^
  --base 0x0000 ^
  --entry 0x0000 ^
  --label "MS-DOS 2.0 MSDOS.SYS" ^
  --json "%MD_ROOT%\build-analysis\msdos-sys.json" ^
  --top 24
exit /b %ERRORLEVEL%

:run_command
echo === MS-DOS 2.0 COMMAND.COM ===
"%DP_EXE%" ^
  --input "%MSDOS_ROOT%\bin\COMMAND.COM" ^
  --base 0x0100 ^
  --entry 0x0100 ^
  --label "MS-DOS 2.0 COMMAND.COM" ^
  --json "%MD_ROOT%\build-analysis\command-com.json" ^
  --top 24
exit /b %ERRORLEVEL%

:find_probe
set "DP_EXE=%MD_ROOT%\build-host\dosprobe.exe"
if exist "%MD_ROOT%\build-host\Release\dosprobe.exe" set "DP_EXE=%MD_ROOT%\build-host\Release\dosprobe.exe"
if exist "%DP_EXE%" exit /b 0

echo dosprobe is not built; building host tools...
cmake -S "%MD_ROOT%" -B "%MD_ROOT%\build-host" -DMD_BUILD_HOST=ON -DMD_BUILD_TESTS=ON || exit /b 1
cmake --build "%MD_ROOT%\build-host" --config Release || exit /b 1
set "DP_EXE=%MD_ROOT%\build-host\dosprobe.exe"
if exist "%MD_ROOT%\build-host\Release\dosprobe.exe" set "DP_EXE=%MD_ROOT%\build-host\Release\dosprobe.exe"
if not exist "%DP_EXE%" (
    echo ERROR: dosprobe executable was not built.
    exit /b 1
)
exit /b 0

:no_msdos
echo ERROR: the pinned Microsoft MS-DOS tree is not installed.
echo.
echo Run:
echo   .\md.bat deps msdos
exit /b 1
