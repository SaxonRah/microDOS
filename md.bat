@echo off
rem ===========================================================================
rem microDOS - single entry point for host/runtime/recompiler development.
rem
rem   .\md.bat deps msdos
rem   .\md.bat build host
rem   .\md.bat build pico           (Pimoroni Pico Plus 2 firmware, .uf2)
rem   .\md.bat run host
rem   .\md.bat run dos2 [budget]    (interactive; Ctrl+] exits; 0/omitted = unlimited)
rem   .\md.bat test
rem   .\md.bat bench [rounds]
rem   .\md.bat recomp input.com name [code-end]
rem   .\md.bat analyze [dos2^|msdos^|command]
rem   .\md.bat image dos2
rem   .\md.bat boot msdos2 [budget]
rem   .\md.bat clean [all]
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "MD_ROOT=%CD%"
set "CMD=%~1"
if "%CMD%"=="" set "CMD=help"
shift /1

if /i "%CMD%"=="deps"   goto deps
if /i "%CMD%"=="build"  goto build
if /i "%CMD%"=="run"    goto run
if /i "%CMD%"=="test"   goto test
if /i "%CMD%"=="bench"  goto bench
if /i "%CMD%"=="recomp" goto recomp
if /i "%CMD%"=="analyze" goto analyze
if /i "%CMD%"=="image"  goto image
if /i "%CMD%"=="boot"   goto boot
if /i "%CMD%"=="clean"  goto clean
if /i "%CMD%"=="help"   goto help
if /i "%CMD%"=="-h"     goto help
if /i "%CMD%"=="--help" goto help

echo ERROR: unknown command "%CMD%".
exit /b 1


:deps
if /i "%~1"=="msdos" (
    call "%MD_ROOT%\scripts\md_msdos_deps.bat"
    exit /b %ERRORLEVEL%
)
echo Usage:
echo   .\md.bat deps msdos
exit /b 1

:build
if /i "%~1"=="pico" goto build_pico
if /i not "%~1"=="host" (
    echo ERROR: build targets are "host" and "pico".
    exit /b 1
)
cmake -S "%MD_ROOT%" -B "%MD_ROOT%\build-host" -DMD_BUILD_HOST=ON -DMD_BUILD_TESTS=ON || exit /b 1
cmake --build "%MD_ROOT%\build-host" --config Release || exit /b 1
exit /b 0

:build_pico
rem Pico 2 / Pimoroni Pico Plus 2 firmware. Needs the desktop tools first
rem (dosrecomp generates DOS2TEST's C, mkfat12 builds the embedded disk).
call :build_host_if_needed || exit /b 1
if not exist "%MD_ROOT%\third_party\msdos\v2.0\bin\MSDOS.SYS" (
    echo ERROR: MS-DOS 2.0 binaries are missing. Run: .\md.bat deps msdos
    exit /b 1
)
call "%MD_ROOT%\scripts\md_pico_env.bat" || exit /b 1
if not exist "%MD_ROOT%\build-pico" mkdir "%MD_ROOT%\build-pico" || exit /b 1
set "MKFAT_EXE=%MD_ROOT%\build-host\mkfat12.exe"
if exist "%MD_ROOT%\build-host\Release\mkfat12.exe" set "MKFAT_EXE=%MD_ROOT%\build-host\Release\mkfat12.exe"
"%MKFAT_EXE%" --command "%MD_ROOT%\third_party\msdos\v2.0\bin\COMMAND.COM" --add "%MD_ROOT%\tests\dos2\DOS2TEST.COM" DOS2TEST.COM --output "%MD_ROOT%\build-pico\pico_disk.img" || exit /b 1
cmake -S "%MD_ROOT%\pico" -B "%MD_ROOT%\build-pico\out" -G Ninja "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%" "-DMICRODOS_HOST_BUILD=%MD_ROOT%\build-host" "-DMICRODOS_PICO_DISK=%MD_ROOT%\build-pico\pico_disk.img" || exit /b 1
cmake --build "%MD_ROOT%\build-pico\out" || exit /b 1
echo.
echo Firmware in %MD_ROOT%\build-pico\out\ :
echo   microdos_pico.uf2           DOS, compiled kernel, 300 MHz ^(default^)
echo   microdos_pico_150.uf2       DOS, compiled kernel, 150 MHz
echo   microdos_pico_nokernel.uf2  DOS, interpreted kernel, 300 MHz ^(A/B^)
echo   microdos_bench.uf2          benchmark matrix, 300 MHz
echo   microdos_bench_150.uf2      benchmark matrix, 150 MHz
echo Hold BOOTSEL while plugging in the Pico Plus 2, then copy a .uf2 to the RP2350 drive.
exit /b 0

:run
if /i "%~1"=="host" goto run_host
if /i "%~1"=="dos2" goto run_dos2
echo Usage:
echo   .\md.bat run host
echo   .\md.bat run dos2 [budget]    ^(interactive; Ctrl+] exits; 0/omitted = unlimited^)
exit /b 1

:run_host
call :build_host_if_needed || exit /b 1
if exist "%MD_ROOT%\build-host\Release\microdos_host.exe" (
    "%MD_ROOT%\build-host\Release\microdos_host.exe"
) else (
    "%MD_ROOT%\build-host\microdos_host.exe"
)
exit /b %ERRORLEVEL%

:run_dos2
set "RUN_BUDGET=%~2"
if "%RUN_BUDGET%"=="" set "RUN_BUDGET=0"
call :ensure_dos2_image || exit /b 1
set "RUN_EXE=%MD_ROOT%\build-host\microdos_msdos2.exe"
if exist "%MD_ROOT%\build-host\Release\microdos_msdos2.exe" set "RUN_EXE=%MD_ROOT%\build-host\Release\microdos_msdos2.exe"
if not exist "%RUN_EXE%" (
    echo ERROR: microdos_msdos2 executable was not built.
    exit /b 1
)
"%RUN_EXE%" "%MD_ROOT%\third_party\msdos\v2.0\bin\MSDOS.SYS" %RUN_BUDGET% "%MD_ROOT%\build-disk\msdos2.img" "%MD_ROOT%\third_party\msdos\v2.0\bin\COMMAND.COM"
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


:analyze
set "ANALYZE_TARGET=%~1"
if "%ANALYZE_TARGET%"=="" set "ANALYZE_TARGET=dos2"
call :build_host_if_needed || exit /b 1
call "%MD_ROOT%\scripts\md_analyze.bat" "%ANALYZE_TARGET%"
exit /b %ERRORLEVEL%

:image
if /i not "%~1"=="dos2" (
    echo Usage:
    echo   .\md.bat image dos2
    exit /b 1
)
call :make_dos2_image
exit /b %ERRORLEVEL%

:ensure_dos2_image
if exist "%MD_ROOT%\build-disk\msdos2.img" exit /b 0
call :make_dos2_image
exit /b %ERRORLEVEL%

:make_dos2_image
call :build_host_if_needed || exit /b 1
if not exist "%MD_ROOT%\third_party\msdos\v2.0\bin\COMMAND.COM" (
    echo ERROR: MS-DOS 2.0 COMMAND.COM is missing.
    echo Run: .\md.bat deps msdos
    exit /b 1
)
if not exist "%MD_ROOT%\build-disk" mkdir "%MD_ROOT%\build-disk" || exit /b 1
set "MKFAT_EXE=%MD_ROOT%\build-host\mkfat12.exe"
if exist "%MD_ROOT%\build-host\Release\mkfat12.exe" set "MKFAT_EXE=%MD_ROOT%\build-host\Release\mkfat12.exe"
set "MKFAT_EXTRA="
if exist "%MD_ROOT%\tests\dos2\DOS2TEST.COM" set MKFAT_EXTRA=--add "%MD_ROOT%\tests\dos2\DOS2TEST.COM" DOS2TEST.COM
"%MKFAT_EXE%" --command "%MD_ROOT%\third_party\msdos\v2.0\bin\COMMAND.COM" %MKFAT_EXTRA% --output "%MD_ROOT%\build-disk\msdos2.img"
exit /b %ERRORLEVEL%


:boot
if /i not "%~1"=="msdos2" (
    echo Usage:
    echo   .\md.bat boot msdos2 [budget]
    exit /b 1
)
call :build_host_if_needed || exit /b 1
if not exist "%MD_ROOT%\third_party\msdos\v2.0\bin\MSDOS.SYS" (
    echo ERROR: MS-DOS 2.0 reference image is missing.
    echo Run: .\md.bat deps msdos
    exit /b 1
)
set "BOOT_BUDGET=%~2"
if "%BOOT_BUDGET%"=="" set "BOOT_BUDGET=2000000"
set "BOOT_EXE=%MD_ROOT%\build-host\microdos_msdos2.exe"
if exist "%MD_ROOT%\build-host\Release\microdos_msdos2.exe" set "BOOT_EXE=%MD_ROOT%\build-host\Release\microdos_msdos2.exe"
"%BOOT_EXE%" "%MD_ROOT%\third_party\msdos\v2.0\bin\MSDOS.SYS" %BOOT_BUDGET%
exit /b %ERRORLEVEL%

:recomp_usage
echo Usage:
echo   .\md.bat recomp input.com name [code-end]
echo   .\md.bat analyze [dos2^|msdos^|command]
echo   .\md.bat image dos2
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
if exist "%MD_ROOT%\build-analysis" rmdir /s /q "%MD_ROOT%\build-analysis"
if exist "%MD_ROOT%\build-pico" rmdir /s /q "%MD_ROOT%\build-pico"
if /i "%~1"=="all" (
    if exist "%MD_ROOT%\build-disk" rmdir /s /q "%MD_ROOT%\build-disk"
    echo clean including persistent DOS disk image.
) else (
    echo clean. persistent build-disk preserved; use ".\md.bat image dos2" to reset it.
)
exit /b 0

:help
echo microDOS build, run, and recompilation driver.
echo.
echo   .\md.bat deps msdos
echo   .\md.bat build host
echo   .\md.bat build pico          ^(Pico Plus 2 firmware; needs Pico SDK 2.3.0^)
echo   .\md.bat run host
echo   .\md.bat run dos2 [budget]    ^(interactive; Ctrl+] exits; 0/omitted = unlimited^)
echo   .\md.bat test
echo   .\md.bat bench [rounds]
echo   .\md.bat recomp input.com name [code-end]
echo   .\md.bat analyze [dos2^|msdos^|command]
echo   .\md.bat image dos2          ^(rebuild/reset FAT12 image^)
echo   .\md.bat boot msdos2 [budget]
echo   .\md.bat clean [all]     ^(default preserves build-disk; all removes it^)
echo.
echo MS-DOS 2.0 bring-up:
echo   .\md.bat deps msdos
echo   .\md.bat analyze dos2
echo   .\md.bat boot msdos2
echo   .\md.bat image dos2
echo   .\md.bat run dos2
echo.
echo dosrecomp example:
echo   .\md.bat recomp tests\programs\hello.com hello 0x10c
exit /b 0
