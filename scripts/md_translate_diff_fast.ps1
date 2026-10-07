param(
    [int]$CaptureSeconds = 420,
    [string]$Label = "fast-dev",
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

Push-Location $repo
try {
    Write-Host "=== FAST RP2350 TRANSLATOR DIFFERENTIAL BUILD ==="
    & cmake --build .\build-pico\out --target microdos_pico_translate_diff_fast
    if ($LASTEXITCODE -ne 0) {
        throw "fast diff build failed: $LASTEXITCODE"
    }

    Write-Host ""
    Write-Host "=== FAST RP2350 TRANSLATOR DIFFERENTIAL RUN ==="
    Write-Host "  directed: 24 boundary budgets per current directed program"
    Write-Host "  random:   100 eager + 100 tiered"
    Write-Host "  purpose:  development gate; full gate still required before commit/freeze"
    Write-Host ""

    $args = @{
        Uf2 = ".\build-pico\out\microdos_pico_translate_diff_fast.uf2"
        Label = $Label
        CaptureSeconds = $CaptureSeconds
    }

    if ($NoFlash) {
        $args.NoFlash = $true
    }

    & .\scripts\md_translate_diff_pico.ps1 @args
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
