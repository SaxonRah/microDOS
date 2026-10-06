param(
    [switch]$SkipDiff,
    [int]$DiffSeconds = 2400,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "============================================================"
Write-Host "M28h CMAKE REGENERATE"
Write-Host "============================================================"
& cmake -S .\pico -B .\build-pico\out
if ($LASTEXITCODE -ne 0) { throw "CMake regenerate failed: $LASTEXITCODE" }

if (-not $SkipDiff) {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28h TRANSLATOR DIFFERENTIAL CHECK"
    Write-Host "============================================================"
    & .\scripts\md_translate_diff_pico.ps1 `
        -Build `
        -Label "M28h-group3-control" `
        -CaptureSeconds $DiffSeconds
    if ($LASTEXITCODE -ne 0) {
        throw "M28h translator differential test failed: $LASTEXITCODE"
    }
}

$target = "microdos_pico_m28h_combo224_qmi"
$uf2 = ".\build-pico\out\$target.uf2"

Write-Host ""
Write-Host "============================================================"
Write-Host "M28h BUILD 224 KiB"
Write-Host "============================================================"
& cmake --build .\build-pico\out --target $target
if ($LASTEXITCODE -ne 0) { throw "M28h production build failed: $LASTEXITCODE" }
if (-not (Test-Path $uf2)) { throw "M28h UF2 not found: $uf2" }

Write-Host ""
Write-Host "============================================================"
Write-Host "M28h SPLITBENCH"
Write-Host "============================================================"
& .\scripts\md_engine_splitbench.ps1 `
    -Uf2 $uf2 `
    -Label "M28h-group3-control-224" `
    -CaptureSeconds $CaptureSeconds
if ($LASTEXITCODE -ne 0) { throw "M28h splitbench failed: $LASTEXITCODE" }

$log = Get-ChildItem .\logs\engine-splitbench-M28h-group3-control-224-*.txt |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $log) { throw "M28h splitbench log not found" }

Write-Host ""
Write-Host "============================================================"
Write-Host "M28h GROUP3 + CONTROL RESULT"
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
Write-Host "224 KiB M28f baseline:"
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
Write-Host "Expected top-step changes:"
Write-Host "  F6/F7 collapse except intentionally canonical prefixed cases."
Write-Host "  FF collapses to PUSH aliases / unusual invalid forms."
Write-Host "  CB and CF disappear."
Write-Host "  CD intentionally remains canonical."
Write-Host ""
Write-Host "saved: $($log.FullName)"
