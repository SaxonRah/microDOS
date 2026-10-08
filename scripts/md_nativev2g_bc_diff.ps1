param()

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    throw "WSL is required. Install WSL/Ubuntu plus gcc-arm-linux-gnueabihf and qemu-user."
}

$linuxRepo = (& wsl.exe wslpath -a ($Repo -replace '\\','/')).Trim()
if (-not $linuxRepo) {
    $linuxRepo = (& wsl.exe wslpath -a $Repo).Trim()
}
if (-not $linuxRepo) {
    throw "Could not translate repo path into WSL path."
}

Write-Host ""
Write-Host "============================================================"
Write-Host "NV2-G G-2B2/G-2C FOCUSED DIFFERENTIAL"
Write-Host "============================================================"
Write-Host "repo: $Repo"
Write-Host "WSL:  $linuxRepo"
Write-Host ""

& wsl.exe bash -lc "cd '$linuxRepo' && sh scripts/md_nativev2g_bc_diff.sh"
if ($LASTEXITCODE -ne 0) {
    throw "G-2B2/G-2C differential failed with exit code $LASTEXITCODE"
}
