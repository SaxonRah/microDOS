param(
    [int]$RandomCases = 4000,
    [string]$Seed = "0x4E563247",
    [int]$DirectedStates = 64
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    throw "WSL is required for qemu-arm. Install WSL/Ubuntu, then install gcc-arm-linux-gnueabihf and qemu-user."
}

$linuxRepo = (& wsl.exe wslpath -a ($Repo -replace '\\','/')).Trim()
if (-not $linuxRepo) {
    # wslpath accepts the Windows path directly on normal WSL installs.
    $linuxRepo = (& wsl.exe wslpath -a $Repo).Trim()
}
if (-not $linuxRepo) {
    throw "Could not translate repo path into WSL path."
}

Write-Host ""
Write-Host "============================================================"
Write-Host "NV2-G QEMU-ARM DIFFERENTIAL"
Write-Host "============================================================"
Write-Host "repo: $Repo"
Write-Host "WSL:  $linuxRepo"
Write-Host "random cases: $RandomCases"
Write-Host "seed: $Seed"
Write-Host "directed states: $DirectedStates"
Write-Host ""

& wsl.exe bash -lc "cd '$linuxRepo' && sh scripts/md_nativev2g_diff.sh '$RandomCases' '$Seed' '$DirectedStates'"
if ($LASTEXITCODE -ne 0) {
    throw "NV2-G differential failed with exit code $LASTEXITCODE"
}
