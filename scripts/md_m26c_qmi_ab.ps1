param(
    [switch]$Build,
    [string]$Port = "COM5",
    [string]$BuildDir = ".\build-pico\out"
)
$ErrorActionPreference = "Stop"
$cases = @(
    @{ Target="microdos_pico_m25_nv2_qmi_sram"; Label="m26c-qmi-sram160" },
    @{ Target="microdos_pico_m25_nv2_exec_qmi"; Label="m26c-qmi-exec160" }
)
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$phaseRows = @()
foreach ($c in $cases) {
    if ($Build) {
        Write-Host "`n=== BUILD $($c.Target) ==="
        cmake --build $BuildDir --target $c.Target
        if ($LASTEXITCODE -ne 0) { throw "build failed: $($c.Target)" }
    }
    $uf2 = Join-Path $BuildDir ($c.Target + ".uf2")
    if (!(Test-Path $uf2)) { throw "missing UF2: $uf2 (use -Build)" }
    Write-Host "`n============================================================"
    Write-Host "M26c QMI ATTRIBUTION: $($c.Label)"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 -Uf2 $uf2 -Label $c.Label
    if ($LASTEXITCODE -ne 0) { throw "splitbench failed: $($c.Label) rc=$LASTEXITCODE" }
    $log = Get-ChildItem .\logs\engine-splitbench-$($c.Label)-*.txt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $log) { throw "could not find splitbench log for $($c.Label)" }
    $qcsv = Join-Path .\logs ("m26c-qmi-{0}-{1}.csv" -f $c.Label,$stamp)
    python .\scripts\md_m26c_qmi_extract.py $log.FullName --label $c.Label --csv $qcsv
    if ($LASTEXITCODE -ne 0) { throw "QMI extraction failed: $($c.Label)" }

    # Clean Phase-3 REP run so QMI attribution can be compared on the string
    # workload in isolation as well as on DOS2TEST / full MDSTRESS.
    $picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
    if (!(Test-Path $picotool)) { throw "picotool not found: $picotool" }
    try { & $picotool reboot -f -u; Start-Sleep -Milliseconds 800 } catch {}
    & $picotool load -v -x $uf2
    if ($LASTEXITCODE -ne 0) { throw "phase3 flash failed: $($c.Label)" }
    Start-Sleep -Milliseconds 1000
    $base = Join-Path .\logs ("qmi-rep-phase3-{0}-{1}" -f $c.Label,$stamp)
    python .\scripts\mdstress_capture.py $Port --phases 3 --label $c.Label --output $base
    if ($LASTEXITCODE -ne 0) { throw "REP phase capture failed: $($c.Label)" }
    $j = Get-Content ($base + ".json") -Raw | ConvertFrom-Json
    $p = $j.phases[0]
    $phaseRows += [pscustomobject]@{
        Label=$c.Label; Active_s=[double]$p.active_s; MIPS=[double]$p.mips;
        RepElementsM_s=[double]$p.rep_elements_mps; RepTrafficMiB_s=[double]$p.rep_traffic_mib_s;
        XipAccesses=[uint64]$p.xip_accesses; XipMisses=[uint64]$p.xip_misses; XipMissPerGuest=[double]$p.xip_miss_per_guest
    }
}
Write-Host "`n=== M26c REP/QMI ENDPOINT SUMMARY ==="
$phaseRows | Format-Table -AutoSize
$csv = Join-Path .\logs ("m26c-qmi-rep-{0}.csv" -f $stamp)
$phaseRows | Export-Csv $csv -NoTypeInformation
Write-Host "saved: $csv"
Write-Host ""
Write-Host "The [qmi] lines are exclusive categories. 'unattributed' is the remaining XIP/QMI traffic outside all scoped execution categories."
