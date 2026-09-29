@echo off
setlocal EnableExtensions
cd /d "%~dp0\.."
set "MD_ROOT=%CD%"
set "MD_MSDOS_DIR=%MD_ROOT%\third_party\msdos"
set "MD_MSDOS_URL=https://github.com/microsoft/MS-DOS.git"
set "MD_MSDOS_SHA=2d04cacc5322951f187bb17e017c12920ac8ebe2"
set "MD_MSDOS_SYS_BLOB=803eb7004b3e3d0093d955565beff7ec39f5b8c4"
set "MD_COMMAND_COM_BLOB=820f39322f00e0cecd52568ac5505b8d1611b552"

where git >nul 2>nul || (
    echo ERROR: git was not found on PATH.
    exit /b 1
)

if not exist "%MD_ROOT%\third_party" mkdir "%MD_ROOT%\third_party" || exit /b 1

if not exist "%MD_MSDOS_DIR%\.git" (
    if exist "%MD_MSDOS_DIR%" rmdir /s /q "%MD_MSDOS_DIR%"
    echo === cloning Microsoft MS-DOS reference tree ===
    git clone --filter=blob:none --no-checkout "%MD_MSDOS_URL%" "%MD_MSDOS_DIR%" || exit /b 1
)

rem The upstream repository is archived, but pin the exact tree anyway so binary
rem analysis stays reproducible even if a mirror or future unarchive changes HEAD.
git -C "%MD_MSDOS_DIR%" fetch origin || exit /b 1
git -C "%MD_MSDOS_DIR%" sparse-checkout init --cone >nul 2>nul
git -C "%MD_MSDOS_DIR%" sparse-checkout set v2.0 || exit /b 1
git -C "%MD_MSDOS_DIR%" checkout --detach "%MD_MSDOS_SHA%" || exit /b 1

for /f "delims=" %%S in ('git -C "%MD_MSDOS_DIR%" rev-parse HEAD') do set "ACTUAL=%%S"
if /i not "%ACTUAL%"=="%MD_MSDOS_SHA%" (
    echo ERROR: MS-DOS checkout is at %ACTUAL%, expected %MD_MSDOS_SHA%.
    exit /b 1
)

if not exist "%MD_MSDOS_DIR%\v2.0\bin\MSDOS.SYS" (
    echo ERROR: pinned checkout is missing v2.0\bin\MSDOS.SYS.
    exit /b 1
)
if not exist "%MD_MSDOS_DIR%\v2.0\bin\COMMAND.COM" (
    echo ERROR: pinned checkout is missing v2.0\bin\COMMAND.COM.
    exit /b 1
)
if not exist "%MD_MSDOS_DIR%\v2.0\source\MSHEAD.ASM" (
    echo ERROR: pinned checkout is missing v2.0\source\MSHEAD.ASM.
    exit /b 1
)

for /f "delims=" %%S in ('git -C "%MD_MSDOS_DIR%" rev-parse "%MD_MSDOS_SHA%:v2.0/bin/MSDOS.SYS"') do set "ACTUAL_MSDOS_BLOB=%%S"
for /f "delims=" %%S in ('git -C "%MD_MSDOS_DIR%" rev-parse "%MD_MSDOS_SHA%:v2.0/bin/COMMAND.COM"') do set "ACTUAL_COMMAND_BLOB=%%S"
if /i not "%ACTUAL_MSDOS_BLOB%"=="%MD_MSDOS_SYS_BLOB%" (
    echo ERROR: MSDOS.SYS blob mismatch: %ACTUAL_MSDOS_BLOB%
    exit /b 1
)
if /i not "%ACTUAL_COMMAND_BLOB%"=="%MD_COMMAND_COM_BLOB%" (
    echo ERROR: COMMAND.COM blob mismatch: %ACTUAL_COMMAND_BLOB%
    exit /b 1
)

for %%F in ("%MD_MSDOS_DIR%\v2.0\bin\MSDOS.SYS") do set "MSDOS_SIZE=%%~zF"
for %%F in ("%MD_MSDOS_DIR%\v2.0\bin\COMMAND.COM") do set "COMMAND_SIZE=%%~zF"
if not "%MSDOS_SIZE%"=="16690" (
    echo ERROR: MSDOS.SYS size mismatch: %MSDOS_SIZE%
    exit /b 1
)
if not "%COMMAND_SIZE%"=="15480" (
    echo ERROR: COMMAND.COM size mismatch: %COMMAND_SIZE%
    exit /b 1
)

echo MS-DOS 2.0 reference tree ready.
echo   commit: %MD_MSDOS_SHA%
echo   path:   %MD_MSDOS_DIR%
exit /b 0
