param(
    [Parameter(Position=0)]
    [ValidateSet("help","build","firmware","deploy","ports","serial","clean","all")]
    [string]$Command = "help",

    [string]$ToolchainBin = "C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-aarch64-none-elf\bin",
    [string]$BootDrive = "",
    [string]$SerialPort = "COM5"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$Root = (Get-Location).Path
$Build = Join-Path $Root "build-pi0w"
$Out = Join-Path $Build "out"
$Boot = Join-Path $Build "boot"

function Add-Toolchain {
    if ($ToolchainBin -ne "") {
        $env:PATH = "$ToolchainBin;$env:PATH"
    }

    $gcc = Get-Command aarch64-none-elf-gcc.exe -ErrorAction SilentlyContinue
    if (-not $gcc) {
        $gcc = Get-Command aarch64-none-elf-gcc -ErrorAction SilentlyContinue
    }
    if (-not $gcc) {
        throw @"
aarch64-none-elf-gcc was not found.

Install the Arm GNU Toolchain for AArch64 bare-metal (aarch64-none-elf),
then either put its bin directory on PATH or pass:

  -ToolchainBin "C:\path\to\arm-gnu-toolchain\bin"
"@
    }
    Write-Host "AArch64 toolchain: $($gcc.Source)"
}

function Find-Ninja {
    $n = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($n) { return $n.Source }

    $candidate = Join-Path $HOME ".pico-sdk\ninja\v1.12.1\ninja.exe"
    if (Test-Path $candidate) { return $candidate }

    throw "ninja.exe not found (PATH or ~/.pico-sdk/ninja/v1.12.1)."
}

function Build-HostPrereqs {
    if (!(Test-Path (Join-Path $Root "third_party\msdos\v2.0\bin\MSDOS.SYS")) -or
        !(Test-Path (Join-Path $Root "third_party\msdos\v2.0\bin\COMMAND.COM"))) {
        throw "Pinned MS-DOS binaries are missing. Run: .\md.bat deps msdos"
    }

    & cmd /c ".\md.bat build host"
    if ($LASTEXITCODE -ne 0) { throw "Host prerequisite build failed." }

    New-Item -ItemType Directory -Force -Path $Build | Out-Null
    $mkfat = Join-Path $Root "build-host\Release\mkfat12.exe"
    if (!(Test-Path $mkfat)) {
        $mkfat = Join-Path $Root "build-host\mkfat12.exe"
    }
    if (!(Test-Path $mkfat)) { throw "mkfat12.exe was not produced." }

    & $mkfat `
        --command (Join-Path $Root "third_party\msdos\v2.0\bin\COMMAND.COM") `
        --add (Join-Path $Root "tests\dos2\DOS2TEST.COM") DOS2TEST.COM `
        --add (Join-Path $Root "tests\dos2\MDSTRESS.COM") MDSTRESS.COM `
        --output (Join-Path $Build "msdos2.img")
    if ($LASTEXITCODE -ne 0) { throw "FAT12 image build failed." }
}

function Get-Firmware {
    New-Item -ItemType Directory -Force -Path $Boot | Out-Null

    $base = "https://raw.githubusercontent.com/raspberrypi/firmware/master/boot"
    $files = @(
        "bootcode.bin",
        "start.elf",
        "fixup.dat",
        "bcm2710-rpi-zero-2-w.dtb"
    )

    foreach ($f in $files) {
        $dst = Join-Path $Boot $f
        if (!(Test-Path $dst)) {
            Write-Host "Downloading Raspberry Pi firmware: $f"
            & curl.exe -L --fail --silent --show-error "$base/$f" -o $dst
            if ($LASTEXITCODE -ne 0) { throw "Could not download $f" }
        }
    }
}

function Build-BareMetal {
    Add-Toolchain
    Build-HostPrereqs
    Get-Firmware

    $ninja = Find-Ninja
    Write-Host "Ninja: $ninja"

    & cmake `
        -S (Join-Path $Root "pi0w") `
        -B $Out `
        -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $Root 'pi0w\toolchain-aarch64-none-elf.cmake')" `
        "-DMICRODOS_HOST_BUILD=$(Join-Path $Root 'build-host')" `
        "-DMICRODOS_PI0W_DISK=$(Join-Path $Build 'msdos2.img')" `
        "-DMICRODOS_PI0W_KERNEL=$(Join-Path $Root 'third_party\msdos\v2.0\bin\MSDOS.SYS')"

    if ($LASTEXITCODE -ne 0) { throw "Bare-metal CMake configure failed." }

    & cmake --build $Out
    if ($LASTEXITCODE -ne 0) { throw "Bare-metal build failed." }

    Copy-Item -Force (Join-Path $Out "kernel8.img") (Join-Path $Boot "kernel8.img")
    Copy-Item -Force (Join-Path $Root "pi0w\config.txt") (Join-Path $Boot "config.txt")

    Write-Host ""
    Write-Host "Bare-metal boot payload:"
    Get-ChildItem $Boot | Format-Table Name,Length
    Write-Host ""
    Write-Host "kernel8.img is a freestanding AArch64 image; Linux is not involved."
}

function Deploy-Boot {
    if ($BootDrive -eq "") {
        throw "Pass the FAT32 boot volume, for example: -BootDrive E:\"
    }

    $dst = [System.IO.Path]::GetFullPath($BootDrive)
    if (!(Test-Path $dst)) { throw "Boot drive does not exist: $dst" }
    if (!(Test-Path (Join-Path $Boot "kernel8.img"))) {
        throw "No build-pi0w\boot\kernel8.img. Run build first."
    }

    Write-Host "Copying bare-metal boot payload to $dst"
    Copy-Item -Force (Join-Path $Boot "*") $dst
    Write-Host "Done."
}

switch ($Command) {
    "build"    { Build-BareMetal }
    "firmware" { Get-Firmware }
    "deploy"   { Deploy-Boot }
    "ports" {
        & python -m serial.tools.list_ports -v
        exit $LASTEXITCODE
    }
    "serial" {
        & python -m serial.tools.miniterm $SerialPort 115200 --exit-char 24
        exit $LASTEXITCODE
    }
    "clean" {
        if (Test-Path $Build) { Remove-Item -Recurse -Force $Build }
        Write-Host "clean."
    }
    "all" {
        Build-BareMetal
        Deploy-Boot
    }
    default {
@"
microDOS Raspberry Pi Zero 2 W BARE-METAL build chain

Build:
  .\md_pi0w_baremetal.ps1 build

Build with an explicit Arm GNU Toolchain:
  .\md_pi0w_baremetal.ps1 build -ToolchainBin "C:\ArmGNU\bin"

Deploy to an already-formatted FAT32 SD boot volume:
  .\md_pi0w_baremetal.ps1 deploy -BootDrive E:\

Build + deploy:
  .\md_pi0w_baremetal.ps1 all -BootDrive E:\

List serial adapters:
  .\md_pi0w_baremetal.ps1 ports

Open the UART terminal:
  .\md_pi0w_baremetal.ps1 serial -SerialPort COM5

Equivalent:
  python -m serial.tools.miniterm COM5 115200 --exit-char 24

UART wiring (3.3 V USB-TTL adapter):
  Pi pin 8  / GPIO14 / TX  -> adapter RX
  Pi pin 10 / GPIO15 / RX  <- adapter TX
  Pi pin 6  / GND          -- adapter GND

Do NOT connect a 5 V UART signal to the Pi.

The Zero 2 W micro-USB data port is NOT used for console in this first
bare-metal target. Making that port enumerate as a Windows COM device requires
a bare-metal DWC2 USB-device + CDC-ACM stack; GPIO UART works immediately and
uses the same miniterm workflow as the Pico.
"@ | Write-Host
    }
}
