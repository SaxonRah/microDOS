param(
    [int]$CaptureSeconds = 30,
    [string]$Root = "C:\microDOS"
)

$ErrorActionPreference = "Stop"
$Root = [System.IO.Path]::GetFullPath($Root)

$normal = Join-Path $Root "build-pi0w\out\kernel8.img"
$bench  = Join-Path $Root "build-pi0w\out\kernel8_bench.img"
$saved  = Join-Path $Root "build-pi0w\out\kernel8_dos_saved.img"

if (-not (Test-Path $normal)) { throw "missing normal Pi image: $normal" }
if (-not (Test-Path $bench))  { throw "missing benchmark Pi image: $bench" }

Set-Location $Root

Copy-Item $normal $saved -Force
try {
    Copy-Item $bench $normal -Force
    Write-Host "Benchmark image staged as kernel8.img"
    Write-Host "The DOS kernel image will be restored automatically."
    Write-Host ""

    & .\md_pi0w_run.ps1 -NoBuild -CaptureSeconds $CaptureSeconds
}
finally {
    if (Test-Path $saved) {
        Copy-Item $saved $normal -Force
        Remove-Item $saved -Force
        Write-Host ""
        Write-Host "Restored normal DOS kernel8.img"
    }
}
