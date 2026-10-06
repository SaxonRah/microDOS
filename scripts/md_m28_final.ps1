param(
    [switch]$Build,
    [switch]$FullDiff,
    [int]$DiffSeconds = 2400,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$target = "microdos_pico_m25_nv2_exec_combo_qmi"
$buildDir = Join-Path $repo "build-pico\out"
$uf2 = Join-Path $buildDir "$target.uf2"

Push-Location $repo
try {
    if ($FullDiff) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M28 FINAL FULL TRANSLATOR DIFFERENTIAL"
        Write-Host "============================================================"
        & .\scripts\md_translate_diff_pico.ps1 `
            -Build `
            -Label "M28-final" `
            -CaptureSeconds $DiffSeconds
        if ($LASTEXITCODE -ne 0) {
            throw "M28 final full differential failed: $LASTEXITCODE"
        }
    }

    if ($Build) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M28 FINAL PRODUCTION BUILD"
        Write-Host "============================================================"
        & cmake -S .\pico -B $buildDir
        if ($LASTEXITCODE -ne 0) {
            throw "CMake regenerate failed: $LASTEXITCODE"
        }
        & cmake --build $buildDir --target $target
        if ($LASTEXITCODE -ne 0) {
            throw "M28 final build failed: $LASTEXITCODE"
        }
    }

    if (-not (Test-Path $uf2)) {
        throw "M28 final UF2 not found: $uf2 (use -Build)"
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28 FINAL SPLITBENCH"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $uf2 `
        -Label "M28-final-224" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) {
        throw "M28 final splitbench failed: $LASTEXITCODE"
    }

    $log = Get-ChildItem .\logs\engine-splitbench-M28-final-224-*.txt |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $log) {
        throw "M28 final splitbench log not found"
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28 FINAL RESULT"
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
    Write-Host "Validated M28h v2 reference:"
    Write-Host "  BOOT          2.532 MIPS"
    Write-Host "  DOS2TEST #1   2.463 MIPS"
    Write-Host "  DOS2TEST #2   2.260 MIPS"
    Write-Host "  DOS2TEST #3   1.972 MIPS"
    Write-Host "  MDSTRESS     15.200 MIPS"
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
