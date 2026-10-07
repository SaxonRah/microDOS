param(
    [switch]$SkipHost,
    [switch]$SkipPico,
    [switch]$SkipPi,
    [switch]$SkipFullHost,
    [string]$HostConfig = "Release"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Invoke-Checked {
    param(
        [Parameter(Mandatory=$true)][string]$Label,
        [Parameter(Mandatory=$true)][scriptblock]$Command
    )

    Write-Host ""
    Write-Host "============================================================"
    Write-Host $Label
    Write-Host "============================================================"

    & $Command

    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

Push-Location $repo
try {
    if (-not $SkipHost) {
        Invoke-Checked "N3 HOST BUILD" {
            & .\md.bat build host
        }

        Invoke-Checked "N3 HOST DIFFERENTIAL / CACHE TEST" {
            & ctest `
                --test-dir .\build-host `
                -C $HostConfig `
                -R "^native3$" `
                --output-on-failure
        }

        # Also run the executable directly. This makes the useful PASS/FAIL
        # lines visible even when CTest captures normal successful output.
        $native3Exe = Join-Path $repo "build-host\$HostConfig\microdos_native3_tests.exe"
        if (-not (Test-Path $native3Exe)) {
            throw "Native-3 test executable not found: $native3Exe"
        }

        Invoke-Checked "N3 HOST TEST EXECUTABLE" {
            & $native3Exe
        }

        if (-not $SkipFullHost) {
            Invoke-Checked "FULL HOST REGRESSION SUITE" {
                & ctest `
                    --test-dir .\build-host `
                    -C $HostConfig `
                    --output-on-failure
            }
        }
    }

    if (-not $SkipPico) {
        Invoke-Checked "N3 PICO CONFIGURE" {
            & cmake -S .\pico -B .\build-pico\out
        }

        Invoke-Checked "N3 PICO BUILD" {
            & cmake --build .\build-pico\out --target `
                microdos_pico_native3 `
                microdos_pico_native3_bench
        }

        $picoDos = Join-Path $repo "build-pico\out\microdos_pico_native3.uf2"
        $picoBench = Join-Path $repo "build-pico\out\microdos_pico_native3_bench.uf2"

        if (-not (Test-Path $picoDos)) {
            throw "Pico Native-3 DOS UF2 missing: $picoDos"
        }
        if (-not (Test-Path $picoBench)) {
            throw "Pico Native-3 benchmark UF2 missing: $picoBench"
        }

        Write-Host ""
        Write-Host "Pico Native-3 outputs:"
        Write-Host "  DOS:   $picoDos"
        Write-Host "  Gate:  $picoBench"
        Write-Host ""
        Write-Host "Flash the Gate UF2 first."
        Write-Host "Required performance line: GATE40=PASS"
    }

    if (-not $SkipPi) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "N3 PI ZERO 2 W BUILD"
        Write-Host "============================================================"

        $piBuild = Join-Path $repo "build-pi0w"

        if (-not (Test-Path $piBuild)) {
            Write-Host "build-pi0w is not configured."
            Write-Host "Run the existing Pi bare-metal configure/build script once,"
            Write-Host "then rerun this script with -SkipHost -SkipPico."
        }
        else {
            & cmake --build $piBuild --target `
                microdos_pi0w_native3.elf `
                microdos_pi0w_native3_bench.elf

            if ($LASTEXITCODE -ne 0) {
                throw "N3 Pi Zero 2 W build failed with exit code $LASTEXITCODE"
            }

            $piDos = Join-Path $piBuild "kernel8_native3.img"
            $piBench = Join-Path $piBuild "kernel8_native3_bench.img"

            if (-not (Test-Path $piDos)) {
                throw "Pi Native-3 DOS image missing: $piDos"
            }
            if (-not (Test-Path $piBench)) {
                throw "Pi Native-3 benchmark image missing: $piBench"
            }

            Write-Host ""
            Write-Host "Pi Zero 2 W Native-3 outputs:"
            Write-Host "  DOS:   $piDos"
            Write-Host "  Gate:  $piBench"
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "NATIVE-3 OMNIBUS BUILD COMPLETE"
    Write-Host "============================================================"
    Write-Host "Host correctness passed."
    Write-Host "Hardware performance criterion remains >= 40.000 MIPS."
}
finally {
    Pop-Location
}
