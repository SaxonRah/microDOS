param(
    [switch]$Build,
    [string]$Port = "COM5",
    [string]$BuildDir = ".\build-pico\out"
)

$ErrorActionPreference = "Stop"

# Same-run A/B: exact all-SRAM/160K control vs M26b EXEC-SRAM/160K.
$cases = @(
    @{ Target="microdos_pico_m25_nv2";      Label="m26b-sram160-control" },
    @{ Target="microdos_pico_m25_nv2_exec"; Label="m26b-exec160" }
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
    Write-Host "M26b EXEC PLACEMENT: $($c.Label)"
    Write-Host "============================================================"

    & .\scripts\md_engine_splitbench.ps1 -Uf2 $uf2 -Label $c.Label
    if ($LASTEXITCODE -ne 0) { throw "splitbench failed: $($c.Label) rc=$LASTEXITCODE" }

    # Clean boot for REP/string Phase 3 so the REP throughput denominator is
    # the string workload itself, not all nine MDSTRESS phases.
    $picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
    if (!(Test-Path $picotool)) { throw "picotool not found: $picotool" }
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 800
    } catch {}
    & $picotool load -v -x $uf2
    if ($LASTEXITCODE -ne 0) { throw "phase3 flash failed: $($c.Label)" }
    Start-Sleep -Milliseconds 1000

    $base = Join-Path .\logs ("rep-phase3-{0}-{1}" -f $c.Label,$stamp)
    python .\scripts\mdstress_capture.py $Port --phases 3 --label $c.Label --output $base
    if ($LASTEXITCODE -ne 0) { throw "REP phase capture failed: $($c.Label)" }

    $j = Get-Content ($base + ".json") -Raw | ConvertFrom-Json
    $p = $j.phases[0]
    $phaseRows += [pscustomobject]@{
        Label = $c.Label
        Active_s = [double]$p.active_s
        MIPS = [double]$p.mips
        RepElementsM_s = [double]$p.rep_elements_mps
        RepPayloadMiB_s = [double]$p.rep_payload_mib_s
        RepTrafficMiB_s = [double]$p.rep_traffic_mib_s
        Floppy144_s = [double]$p.rep_floppy144_s
        XipAccesses = [uint64]$p.xip_accesses
        XipMisses = [uint64]$p.xip_misses
        XipHitPct = [double]$p.xip_hit_pct
        XipMissPerGuest = [double]$p.xip_miss_per_guest
    }
}

Write-Host "`n=== M26b EXEC-SRAM REP SUMMARY ==="
$phaseRows | Format-Table -AutoSize
$csv = Join-Path .\logs ("m26b-exec-rep-{0}.csv" -f $stamp)
$phaseRows | Export-Csv $csv -NoTypeInformation
Write-Host "saved: $csv"
Write-Host ""
Write-Host "Compare the two engine-splitbench CSV/log pairs for DOS2TEST and MDSTRESS."
