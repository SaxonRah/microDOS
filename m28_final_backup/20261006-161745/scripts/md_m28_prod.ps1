param(
    [switch]$Build,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"
$target = "microdos_pico_m25_nv2_exec_combo_qmi"
$uf2 = ".\build-pico\out\$target.uf2"

if ($Build) {
    Write-Host "=== M28 PRODUCTION CANDIDATE BUILD: $target ==="
    & cmake --build .\build-pico\out --target $target
    if ($LASTEXITCODE -ne 0) {
        throw "M28 production build failed: $LASTEXITCODE"
    }
}

if (-not (Test-Path $uf2)) {
    throw "M28 production UF2 not found: $uf2 (use -Build)"
}

Write-Host ""
Write-Host "=== M28 PRODUCTION SPLITBENCH ==="
& .\scripts\md_engine_splitbench.ps1 `
    -Uf2 $uf2 `
    -Label "M28-prod-bce" `
    -CaptureSeconds $CaptureSeconds
if ($LASTEXITCODE -ne 0) {
    throw "M28 production splitbench failed: $LASTEXITCODE"
}

$log = Get-ChildItem .\logs\engine-splitbench-M28-prod-bce-*.txt |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $log) {
    throw "M28 production splitbench log not found"
}

Write-Host ""
Write-Host "============================================================"
Write-Host "M28 PRODUCTION CANDIDATE RESULT"
Write-Host "============================================================"

$summary = Get-Content $log.FullName |
    Select-String `
        '^=== ENGINE INTERVAL SUMMARY:', `
        '^BOOT\s', `
        '^DOS2TEST #', `
        '^MDSTRESS AUTO', `
        '^\[m25\] cycles:', `
        '^\[m25\] top steps:'

$summary | ForEach-Object { Write-Host $_.Line }

Write-Host ""
Write-Host "M27 reference rates:"
Write-Host "  BOOT          2.079 MIPS"
Write-Host "  DOS2TEST #1   2.083 MIPS"
Write-Host "  DOS2TEST #2   2.003 MIPS"
Write-Host "  DOS2TEST #3   1.883 MIPS"
Write-Host "  MDSTRESS     13.068 MIPS"
Write-Host ""
Write-Host "saved: $($log.FullName)"
Write-Host "Paste this result block plus the build memory-region lines."
