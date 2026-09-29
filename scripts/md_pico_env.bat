@echo off
rem microDOS Pico SDK environment (same lookup order as microrender/microconsole).

setlocal EnableExtensions

rem SDK path. Preserve override.
if not "%PICO_SDK_PATH%"=="" goto have_sdk

if exist "%USERPROFILE%\.pico-sdk\sdk\2.3.0\pico_sdk_init.cmake" (
    set "PICO_SDK_PATH=%USERPROFILE%\.pico-sdk\sdk\2.3.0"
    goto have_sdk
)

echo ERROR: Pico SDK 2.3.0 is required and was not found under:
echo   %USERPROFILE%\.pico-sdk\sdk\2.3.0
echo Or set PICO_SDK_PATH explicitly to a Pico SDK 2.3.0 checkout.
exit /b 1

:have_sdk
if not exist "%PICO_SDK_PATH%\pico_sdk_init.cmake" (
    echo ERROR: PICO_SDK_PATH does not look valid:
    echo   %PICO_SDK_PATH%
    exit /b 1
)

rem Toolchain path. Preserve override.
if not "%PICO_TOOLCHAIN_PATH%"=="" goto have_toolchain

if exist "%USERPROFILE%\.pico-sdk\toolchain\14_2_Rel1\bin\arm-none-eabi-gcc.exe" (
    set "PICO_TOOLCHAIN_PATH=%USERPROFILE%\.pico-sdk\toolchain\14_2_Rel1"
    goto have_toolchain
)

if exist "%USERPROFILE%\.pico-sdk\toolchain\13_3_Rel1\bin\arm-none-eabi-gcc.exe" (
    set "PICO_TOOLCHAIN_PATH=%USERPROFILE%\.pico-sdk\toolchain\13_3_Rel1"
    goto have_toolchain
)

rem Fallback to PATH if available.
where arm-none-eabi-gcc.exe >nul 2>nul
if not errorlevel 1 (
    for /f "delims=" %%G in ('where arm-none-eabi-gcc.exe 2^>nul') do (
        if not defined GCC_PATH set "GCC_PATH=%%~dpG"
    )
    if defined GCC_PATH (
        for %%D in ("%GCC_PATH%\..") do set "PICO_TOOLCHAIN_PATH=%%~fD"
        goto have_toolchain
    )
)

echo ERROR: arm-none-eabi-gcc was not found.
echo Checked:
echo   %USERPROFILE%\.pico-sdk\toolchain\14_2_Rel1\bin\arm-none-eabi-gcc.exe
echo   %USERPROFILE%\.pico-sdk\toolchain\13_3_Rel1\bin\arm-none-eabi-gcc.exe
echo.
echo You can set it manually, for example:
echo   set PICO_TOOLCHAIN_PATH=%USERPROFILE%\.pico-sdk\toolchain\14_2_Rel1
exit /b 1

:have_toolchain
if not exist "%PICO_TOOLCHAIN_PATH%\bin\arm-none-eabi-gcc.exe" (
    echo ERROR: PICO_TOOLCHAIN_PATH does not contain bin\arm-none-eabi-gcc.exe:
    echo   %PICO_TOOLCHAIN_PATH%
    exit /b 1
)

rem Ninja. Preserve override.
if not "%NINJA_EXE%"=="" goto have_ninja

where ninja.exe >nul 2>nul
if not errorlevel 1 (
    for /f "delims=" %%N in ('where ninja.exe 2^>nul') do (
        if not defined NINJA_EXE set "NINJA_EXE=%%N"
    )
)

if "%NINJA_EXE%"=="" if exist "%USERPROFILE%\.pico-sdk\ninja\v1.12.1\ninja.exe" (
    set "NINJA_EXE=%USERPROFILE%\.pico-sdk\ninja\v1.12.1\ninja.exe"
)

:have_ninja
if "%NINJA_EXE%"=="" (
    echo ERROR: Ninja was not found.
    exit /b 1
)

if not exist "%NINJA_EXE%" (
    echo ERROR: NINJA_EXE does not exist:
    echo   %NINJA_EXE%
    exit /b 1
)

rem Board. Preserve override.
if "%PICO_BOARD%"=="" (
    set "PICO_BOARD=pimoroni_pico_plus2_rp2350"
)

echo PICO_SDK_PATH=%PICO_SDK_PATH%
echo PICO_TOOLCHAIN_PATH=%PICO_TOOLCHAIN_PATH%
echo PICO_BOARD=%PICO_BOARD%
echo NINJA_EXE=%NINJA_EXE%

endlocal & (
    set "PICO_SDK_PATH=%PICO_SDK_PATH%"
    set "PICO_TOOLCHAIN_PATH=%PICO_TOOLCHAIN_PATH%"
    set "PICO_BOARD=%PICO_BOARD%"
    set "NINJA_EXE=%NINJA_EXE%"
)
exit /b 0
