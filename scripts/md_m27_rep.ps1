param(
    [switch]$Build,
    [switch]$Full,
    [string]$Port = "COM5",
    [string]$BuildDir = ".\build-pico\out",
    [string]$Target = "microdos_pico_m25_nv2_exec_combo_qmi"
)
$ErrorActionPreference = "Stop"

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$label = "m27a-rep-bulk"
New-Item -ItemType Directory -Force .\logs | Out-Null

if ($Build) {
    Write-Host "`n=== M27a BUILD: $Target ==="
    $buildLog = ".\logs\m27a-build-$stamp.txt"
    & cmake --build $BuildDir --target $Target 2>&1 | Tee-Object -FilePath $buildLog
    if ($LASTEXITCODE -ne 0) { throw "build failed rc=$LASTEXITCODE; see $buildLog" }
    Write-Host "saved: $buildLog"
}

$uf2 = Join-Path $BuildDir ($Target + ".uf2")
if (!(Test-Path $uf2)) {
    throw "missing $uf2 -- run with -Build"
}

if ($Full) {
    Write-Host "`n=== M27a FULL ENGINE SPLITBENCH ==="
    & .\scripts\md_engine_splitbench.ps1 -Uf2 $uf2 -Label $label
    $splitRc = $LASTEXITCODE
    if ($splitRc -eq 3) {
        Write-Warning "legacy splitbench summary parser did not match the extra diagnostic lines; raw capture is still valid"
    } elseif ($splitRc -ne 0) {
        throw "splitbench failed rc=$splitRc"
    }
}

$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
if (!(Test-Path $picotool)) { throw "picotool not found: $picotool" }

Write-Host "`n=== M27a CLEAN PHASE-3 FLASH ==="
try {
    & $picotool reboot -f -u
    Start-Sleep -Milliseconds 800
} catch {}
& $picotool load -v -x $uf2
if ($LASTEXITCODE -ne 0) { throw "picotool load failed rc=$LASTEXITCODE" }
Start-Sleep -Milliseconds 1000

$base = ".\logs\m27a-rep-phase3-$stamp"
Write-Host "`n=== M27a REP PHASE 3 CAPTURE ==="
python .\scripts\mdstress_capture.py $Port --phases 3 --label $label --output $base
if ($LASTEXITCODE -ne 0) { throw "REP phase capture failed rc=$LASTEXITCODE" }

$jsonPath = $base + ".json"
$j = Get-Content $jsonPath -Raw | ConvertFrom-Json
$p = $j.phases[0]
$traffic = [double]$p.rep_traffic_mib_s
$payload = [double]$p.rep_payload_mib_s
$elements = [double]$p.rep_elements_mps
$baseline = 3.548
$ratio = if ($baseline -gt 0) { $traffic / $baseline } else { 0.0 }

Write-Host "`n============================================================"
Write-Host "M27a REP BULK RESULT"
Write-Host "============================================================"
Write-Host ("active_s             {0:N3}" -f [double]$p.active_s)
Write-Host ("REP elements M/s     {0:N3}" -f $elements)
Write-Host ("REP payload MiB/s    {0:N3}" -f $payload)
Write-Host ("REP traffic MiB/s    {0:N3}" -f $traffic)
Write-Host ("vs M26d combo 3.548x {0:N2}x" -f $ratio)
Write-Host ("XIP misses           {0}" -f [uint64]$p.xip_misses)
Write-Host ("XIP miss/guest       {0:N4}" -f [double]$p.xip_miss_per_guest)
Write-Host ("checksum             {0}" -f $p.checksum)
if ($traffic -ge 16.0) {
    Write-Host "RESULT: >=16 MiB/s second target reached."
} elseif ($traffic -ge 8.0) {
    Write-Host "RESULT: >=8 MiB/s first target reached."
} else {
    Write-Host "RESULT: below 8 MiB/s first target; paste the capture and we will profile the remaining cost."
}
Write-Host "saved: $jsonPath"
