param(
    [int]$TargetedSeconds = 900,
    [int]$DiffSeconds = 2400,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

Push-Location $repo
try {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28h v2 HOST TESTS"
    Write-Host "============================================================"
    & .\md.bat test all
    if ($LASTEXITCODE -ne 0) { throw "host tests failed: $LASTEXITCODE" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28h v2 TARGETED REGRESSION GATE"
    Write-Host "============================================================"
    & .\scripts\md_m28h_failure_diag.ps1 `
        -Build `
        -Flash `
        -CaptureSeconds $TargetedSeconds
    if ($LASTEXITCODE -ne 0) {
        throw "targeted M28h v2 regression gate failed: $LASTEXITCODE"
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28h v2 FULL TRANSLATOR DIFFERENTIAL"
    Write-Host "============================================================"
    & .\scripts\md_translate_diff_pico.ps1 `
        -Build `
        -Label "M28h-v2-budget-fix" `
        -CaptureSeconds $DiffSeconds
    if ($LASTEXITCODE -ne 0) {
        throw "full translator differential failed: $LASTEXITCODE"
    }

    $target = "microdos_pico_m28h_combo224_qmi"
    $uf2 = ".\build-pico\out\$target.uf2"

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28h v2 PRODUCTION BUILD 224 KiB"
    Write-Host "============================================================"
    & cmake --build .\build-pico\out --target $target
    if ($LASTEXITCODE -ne 0) { throw "production build failed: $LASTEXITCODE" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28h v2 SPLITBENCH"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $uf2 `
        -Label "M28h-v2-224" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) { throw "splitbench failed: $LASTEXITCODE" }

    $log = Get-ChildItem .\logs\engine-splitbench-M28h-v2-224-*.txt |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $log) { throw "M28h v2 splitbench log not found" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28h v2 RESULT"
    Write-Host "============================================================"
    Get-Content $log.FullName |
        Select-String `
            '^=== ENGINE INTERVAL SUMMARY:', `
            '^BOOT\s', `
            '^DOS2TEST #', `
            '^MDSTRESS AUTO', `
            '^\[m25\] native=', `
            '^\[m25\] cycles:', `
            '^\[m25\] top steps:'

    Write-Host ""
    Write-Host "M28f 224 KiB baseline:"
    Write-Host "  BOOT          2.250 MIPS"
    Write-Host "  DOS2TEST #1   2.205 MIPS"
    Write-Host "  DOS2TEST #2   2.022 MIPS"
    Write-Host "  DOS2TEST #3   1.782 MIPS"
    Write-Host "  MDSTRESS     12.860 MIPS"
    Write-Host ""
    Write-Host "Frozen M27:"
    Write-Host "  BOOT          2.079 MIPS"
    Write-Host "  DOS2TEST #1   2.083 MIPS"
    Write-Host "  DOS2TEST #2   2.003 MIPS"
    Write-Host "  DOS2TEST #3   1.883 MIPS"
    Write-Host "  MDSTRESS     13.068 MIPS"
    Write-Host ""
    Write-Host "saved: $($log.FullName)"
}
finally {
    Pop-Location
}
