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
    Write-Host "=== M28e TRANSLATOR DIFFERENTIAL CHECK ==="
    & .\scripts\md_translate_diff_pico.ps1 `
        -Build `
        -Label "M28e-moffs"
    if ($LASTEXITCODE -ne 0) {
        throw "M28e translator differential test failed: $LASTEXITCODE"
    }
}

if ($Build) {
    Write-Host ""
    Write-Host "=== M28e BUILD: $target ==="
    & cmake --build .\build-pico\out --target $target
    if ($LASTEXITCODE -ne 0) {
        throw "M28e profile build failed: $LASTEXITCODE"
    }
}

if (-not (Test-Path $uf2)) {
    throw "M28e profile UF2 not found: $uf2 (use -Build)"
}

Write-Host ""
Write-Host "=== M28e MOFFS SPLITBENCH ==="
& .\scripts\md_engine_splitbench.ps1 `
    -Uf2 $uf2 `
    -Label "M28e-moffs" `
    -CaptureSeconds $CaptureSeconds
if ($LASTEXITCODE -ne 0) {
    throw "M28e splitbench failed: $LASTEXITCODE"
}

$log = Get-ChildItem .\logs\engine-splitbench-M28e-moffs-*.txt |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $log) {
    throw "M28e splitbench log not found"
}

Write-Host ""
Write-Host "============================================================"
Write-Host "M28e MOFFS RESULT"
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
        $line -match '^\[m28\] site0[1-9] ' -or
        $line -match '^\[m28d\] ') {
        Write-Host $line
    }
}

Write-Host ""
Write-Host "saved: $($log.FullName)"
Write-Host "Expected:"
Write-Host "  - 1000:113A disappears from reason=untrans"
Write-Host "  - A0/A1/A2/A3 disappear or collapse in top parse-miss"
Write-Host "  - 1000:19CB and 1000:0655 should become translatable or materially change"
Write-Host "  - DOS2TEST throughput should improve if M28c+M28e closes the INT return path"
Write-Host "Paste the M28e MOFFS RESULT block."
