param(
    [switch]$Build,
    [switch]$Full,
    [string]$Port = "COM5",
    [string]$BuildDir = ".\build-pico\out",
    [string]$Target = "microdos_pico_m25_nv2_exec_combo_qmi"
)
$ErrorActionPreference = "Stop"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$label = "m27b-rep-profile"
New-Item -ItemType Directory -Force .\logs | Out-Null

if ($Build) {
    Write-Host "`n=== M27b BUILD: $Target ==="
    & cmake --build $BuildDir --target $Target
    if ($LASTEXITCODE -ne 0) { throw "build failed rc=$LASTEXITCODE" }
}
$uf2 = Join-Path $BuildDir ($Target + ".uf2")
if (!(Test-Path $uf2)) { throw "missing $uf2 -- run with -Build" }

if ($Full) {
    Write-Host "`n=== M27b FULL ENGINE SPLITBENCH (optional) ==="
    & .\scripts\md_engine_splitbench.ps1 -Uf2 $uf2 -Label $label
    $rc=$LASTEXITCODE
    if ($rc -ne 0 -and $rc -ne 3) { throw "splitbench failed rc=$rc" }
}

$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
if (!(Test-Path $picotool)) { throw "picotool not found: $picotool" }
Write-Host "`n=== M27b CLEAN PHASE-3 FLASH ==="
try { & $picotool reboot -f -u; Start-Sleep -Milliseconds 800 } catch {}
& $picotool load -v -x $uf2
if ($LASTEXITCODE -ne 0) { throw "picotool load failed rc=$LASTEXITCODE" }
Start-Sleep -Milliseconds 1000

$base = ".\logs\m27b-rep-profile-$stamp"
Write-Host "`n=== M27b REP PHASE 3 PROFILE ==="
python .\scripts\mdstress_capture.py $Port --phases 3 --label $label --output $base
if ($LASTEXITCODE -ne 0) { throw "capture failed rc=$LASTEXITCODE" }

Write-Host "`n============================================================"
Write-Host "M27b REP PROFILE LINES"
Write-Host "============================================================"
$profile = Select-String -Path ($base + ".txt") -Pattern '^\[rep-prof\]'
if ($profile) { $profile | ForEach-Object { Write-Host $_.Line } }
else { Write-Warning "no [rep-prof] lines found; paste the full capture" }
$j=Get-Content ($base+".json") -Raw | ConvertFrom-Json
$p=$j.phases[0]
Write-Host ("end-to-end active_s          {0:N3}" -f [double]$p.active_s)
Write-Host ("end-to-end REP traffic MiB/s {0:N3}" -f [double]$p.rep_traffic_mib_s)
Write-Host ("checksum                     {0}" -f $p.checksum)
Write-Host "`nPaste the M27b REP PROFILE LINES plus the final phase summary."
Write-Host ("saved: {0}.txt/.json/.csv" -f $base)
