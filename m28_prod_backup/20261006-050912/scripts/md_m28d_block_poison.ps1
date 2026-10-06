param(
    [switch]$Build,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"
$target = "microdos_pico_m28a_profile_qmi"
$uf2 = ".\build-pico\out\$target.uf2"

if ($Build) {
    Write-Host "=== M28d BUILD: $target ==="
    & cmake --build .\build-pico\out --target $target
    if ($LASTEXITCODE -ne 0) { throw "M28d profile build failed: $LASTEXITCODE" }
}
if (-not (Test-Path $uf2)) { throw "M28d profile UF2 not found: $uf2 (use -Build)" }
Write-Host ""
Write-Host "=== M28d BLOCK-POISON SPLITBENCH ==="
& .\scripts\md_engine_splitbench.ps1 -Uf2 $uf2 -Label "M28d-block-poison" -CaptureSeconds $CaptureSeconds
if ($LASTEXITCODE -ne 0) { throw "M28d splitbench failed: $LASTEXITCODE" }
$log = Get-ChildItem .\logs\engine-splitbench-M28d-block-poison-*.txt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $log) { throw "M28d splitbench log not found" }
Write-Host ""
Write-Host "============================================================"
Write-Host "M28d REJECTED-BLOCK COMPOSITION"
Write-Host "============================================================"
$lines = Get-Content $log.FullName
$show = $false
foreach ($line in $lines) {
    if ($line -match '^=== SPLIT INTERVAL: ') { $show = $true; Write-Host $line; continue }
    if (-not $show) { continue }
    if ($line -match '^\[m25\] cycles:' -or $line -match '^\[m25\] top steps:' -or
        $line -match '^\[m28\] fallback ' -or $line -match '^\[m28\] top parse-miss:' -or
        $line -match '^\[m28\] top untrans-head:' -or $line -match '^\[m28\] site0[1-6] ' -or
        $line -match '^\[m28d\] ') { Write-Host $line }
}
Write-Host ""
Write-Host "Legend: opcode/N=native, opcode/S=K_STEP, opcode/C=K_CSTEP"
Write-Host "saved: $($log.FullName)"
Write-Host "Paste the M28d REJECTED-BLOCK COMPOSITION block."
