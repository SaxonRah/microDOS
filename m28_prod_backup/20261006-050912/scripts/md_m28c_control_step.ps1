param(
    [switch]$Build,
    [switch]$SkipDiff,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"
$target = "microdos_pico_m28a_profile_qmi"
$uf2 = ".\build-pico\out\$target.uf2"

if (-not $SkipDiff) {
    Write-Host ""
    Write-Host "=== M28c TRANSLATOR DIFFERENTIAL CHECK ==="
    & .\scripts\md_translate_diff_pico.ps1 `
        -Build `
        -Label "M28c-control-step"
    if ($LASTEXITCODE -ne 0) {
        throw "M28c translator differential test failed: $LASTEXITCODE"
    }
}

if ($Build) {
    Write-Host ""
    Write-Host "=== M28c BUILD: $target ==="
    & cmake --build .\build-pico\out --target $target
    if ($LASTEXITCODE -ne 0) {
        throw "M28c profile build failed: $LASTEXITCODE"
    }
}

if (-not (Test-Path $uf2)) {
    throw "M28c profile UF2 not found: $uf2 (use -Build)"
}

Write-Host ""
Write-Host "=== M28c SPLITBENCH ==="
& .\scripts\md_engine_splitbench.ps1 `
    -Uf2 $uf2 `
    -Label "M28c-control-step" `
    -CaptureSeconds $CaptureSeconds
if ($LASTEXITCODE -ne 0) {
    throw "M28c splitbench failed: $LASTEXITCODE"
}

$log = Get-ChildItem .\logs\engine-splitbench-M28c-control-step-*.txt |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $log) { throw "M28c splitbench log not found" }

Write-Host ""
Write-Host "============================================================"
Write-Host "M28c CONTROL-STEP RESULT"
Write-Host "============================================================"

$lines = Get-Content $log.FullName
$show = $false
foreach ($line in $lines) {
    if ($line -match '^=== SPLIT INTERVAL: ') {
        $show = $true
        Write-Host $line
        continue
    }
    if (-not $show) { continue }
    if ($line -match '^\[m25\] cycles:' -or
        $line -match '^\[m25\] top steps:' -or
        $line -match '^\[m28\] fallback ' -or
        $line -match '^\[m28\] untrans classes' -or
        $line -match '^\[m28\] episode-' -or
        $line -match '^\[m28\] top parse-miss:' -or
        $line -match '^\[m28\] top untrans-head:' -or
        $line -match '^\[m28\] site0[1-6] ') {
        Write-Host $line
    }
}

Write-Host ""
Write-Host "saved: $($log.FullName)"
Write-Host "Expected: 0800:0100/CD disappears from reason=untrans; CD should move into top steps."
Write-Host "Paste the M28c CONTROL-STEP RESULT block."
