param(
    [switch]$Build,
    [string]$Port = "COM5",
    [string]$BuildDir = ".\build-pico\out",
    [string]$Target = "microdos_pico_m27_rep_generic_qmi"
)
$ErrorActionPreference = "Stop"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$label = "m27c-generic-rep-v2"
New-Item -ItemType Directory -Force .\logs | Out-Null

if ($Build) {
    Write-Host "`n=== M27c BUILD: $Target ==="
    & cmake --build $BuildDir --target $Target
    if ($LASTEXITCODE -ne 0) { throw "build failed rc=$LASTEXITCODE" }
}

$uf2 = Join-Path $BuildDir ($Target + ".uf2")
if (!(Test-Path $uf2)) { throw "missing $uf2 -- run with -Build" }

$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
if (!(Test-Path $picotool)) { throw "picotool not found: $picotool" }

Write-Host "`n=== M27c GENERIC REP CLEAN FLASH ==="
try { & $picotool reboot -f -u; Start-Sleep -Milliseconds 800 } catch {}
& $picotool load -v -x $uf2
if ($LASTEXITCODE -ne 0) { throw "picotool load failed rc=$LASTEXITCODE" }
Start-Sleep -Milliseconds 1000

$base = ".\logs\m27c-generic-rep-$stamp"
Write-Host "`n=== M27c GENERIC REP PHASE 3 ==="
python .\scripts\mdstress_capture.py $Port --phases 3 --label $label --output $base
if ($LASTEXITCODE -ne 0) { throw "capture failed rc=$LASTEXITCODE" }

Write-Host "`n============================================================"
Write-Host "M27c GENERIC REP RESULT"
Write-Host "============================================================"
$profile = Select-String -Path ($base + ".txt") -Pattern '^\[rep-prof\]'
if ($profile) { $profile | ForEach-Object { Write-Host $_.Line } }
else { Write-Warning "no [rep-prof] lines found" }

$j = Get-Content ($base + ".json") -Raw | ConvertFrom-Json
$p = $j.phases[0]
Write-Host ("active_s                     {0:N3}" -f [double]$p.active_s)
Write-Host ("REP elements M/s             {0:N3}" -f [double]$p.rep_elements_mps)
Write-Host ("end-to-end REP traffic MiB/s {0:N3}" -f [double]$p.rep_traffic_mib_s)
Write-Host ("checksum                     {0}" -f $p.checksum)
Write-Host "`nVALIDATION: use the final interval [rep-prof] line. fast elements + scalar elements must equal total [rep] elements."
Write-Host "When they match, rep-only traffic is a valid generic-REP measurement."
Write-Host ("saved: {0}.txt/.json/.csv" -f $base)
