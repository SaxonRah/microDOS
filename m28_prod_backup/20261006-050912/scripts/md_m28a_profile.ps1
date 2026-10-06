param(
    [switch]$Build,
    [int]$CaptureSeconds = 300
)

$ErrorActionPreference = "Stop"
$target = "microdos_pico_m28a_profile_qmi"
$uf2 = ".\build-pico\out\$target.uf2"

if ($Build) {
    Write-Host "=== M28a BUILD: $target ==="
    cmake --build .\build-pico\out --target $target
    if ($LASTEXITCODE -ne 0) { throw "M28a build failed: $LASTEXITCODE" }
}

if (-not (Test-Path $uf2)) { throw "M28a UF2 not found: $uf2" }

& .\scripts\md_engine_splitbench.ps1 `
    -Uf2 $uf2 `
    -Label "M28a-profile" `
    -CaptureSeconds $CaptureSeconds
if ($LASTEXITCODE -ne 0) { throw "M28a splitbench failed: $LASTEXITCODE" }

$log = Get-ChildItem .\logs\engine-splitbench-M28a-profile-*.txt |
    Sort-Object LastWriteTime |
    Select-Object -Last 1
if (-not $log) { throw "M28a splitbench log not found" }

Write-Host ""
Write-Host "============================================================"
Write-Host "M28a INTERPRETER FALLBACK PROFILE"
Write-Host "============================================================"
Get-Content $log.FullName | Where-Object {
    $_ -match '^=== SPLIT INTERVAL:' -or
    $_ -match '^\[m28\]' -or
    $_ -match '^\[m25\] cycles:' -or
    $_ -match '^\[m25\] top steps:'
} | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "saved: $($log.FullName)"
Write-Host "Paste the M28a INTERPRETER FALLBACK PROFILE block."
