param(
    [switch]$Build,
    [switch]$SkipDiff,
    [int]$DiffSeconds = 1800,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"
$target = "microdos_pico_m25_nv2_exec_combo_qmi"
$uf2 = ".\build-pico\out\$target.uf2"

if (-not $SkipDiff) {
    Write-Host ""
    Write-Host "=== M28f TRANSLATOR DIFFERENTIAL CHECK ==="
    & .\scripts\md_translate_diff_pico.ps1 `
        -Build `
        -Label "M28f-hot-steps" `
        -CaptureSeconds $DiffSeconds
    if ($LASTEXITCODE -ne 0) {
        throw "M28f translator differential test failed: $LASTEXITCODE"
    }
}

if ($Build) {
    Write-Host ""
    Write-Host "=== M28f PRODUCTION BUILD: $target ==="
    & cmake --build .\build-pico\out --target $target
    if ($LASTEXITCODE -ne 0) {
        throw "M28f production build failed: $LASTEXITCODE"
    }
}

if (-not (Test-Path $uf2)) {
    throw "M28f production UF2 not found: $uf2 (use -Build)"
}

Write-Host ""
Write-Host "=== M28f PRODUCTION SPLITBENCH ==="
& .\scripts\md_engine_splitbench.ps1 `
    -Uf2 $uf2 `
    -Label "M28f-hot-steps" `
    -CaptureSeconds $CaptureSeconds
if ($LASTEXITCODE -ne 0) {
    throw "M28f splitbench failed: $LASTEXITCODE"
}

$log = Get-ChildItem .\logs\engine-splitbench-M28f-hot-steps-*.txt |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $log) {
    throw "M28f splitbench log not found"
}

Write-Host ""
Write-Host "============================================================"
Write-Host "M28f HOT-STEP RESULT"
Write-Host "============================================================"

Get-Content $log.FullName |
    Select-String `
        '^=== ENGINE INTERVAL SUMMARY:', `
        '^BOOT\s', `
        '^DOS2TEST #', `
        '^MDSTRESS AUTO', `
        '^\[m25\] cycles:', `
        '^\[m25\] top steps:'

Write-Host ""
Write-Host "M27 reference:"
Write-Host "  BOOT          2.079 MIPS"
Write-Host "  DOS2TEST #1   2.083 MIPS"
Write-Host "  DOS2TEST #2   2.003 MIPS"
Write-Host "  DOS2TEST #3   1.883 MIPS"
Write-Host "  MDSTRESS     13.068 MIPS"
Write-Host ""
Write-Host "M28 b+c+e reference:"
Write-Host "  BOOT          1.980 MIPS"
Write-Host "  DOS2TEST #1   2.011 MIPS"
Write-Host "  DOS2TEST #2   1.764 MIPS"
Write-Host "  DOS2TEST #3   1.491 MIPS"
Write-Host "  MDSTRESS     12.554 MIPS"
Write-Host ""
Write-Host "Expected top-step reductions: F8, 8F, 9C, 9D, and FF /6,/7."
Write-Host "saved: $($log.FullName)"
