param(
    [switch]$Build,
    [string]$Port = "COM5",
    [string]$BuildDir = ".\build-pico\out"
)
$ErrorActionPreference = "Stop"

$cases = @(
    @{ Target="microdos_pico_m25_nv2";        Label="m26-sram160" },
    @{ Target="microdos_pico_m25_nv2_xip";    Label="m26-xip160" },
    @{ Target="microdos_pico_m25_nv2_hot";    Label="m26-hot160" },
    @{ Target="microdos_pico_m25_nv2_hot256"; Label="m26-hot256" }
)

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$results = @()
$skipped = @()

foreach ($c in $cases) {
    $ok = $true
    if ($Build) {
        Write-Host "`n=== BUILD $($c.Target) ==="
        cmake --build $BuildDir --target $c.Target
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "build failed: $($c.Target); skipping this case"
            $skipped += $c.Label
            $ok = $false
        }
    }
    if (!$ok) { continue }

    $uf2 = Join-Path $BuildDir ($c.Target + ".uf2")
    if (!(Test-Path $uf2)) {
        Write-Warning "missing $uf2; skipping $($c.Label) (use -Build)"
        $skipped += $c.Label
        continue
    }

    Write-Host "`n============================================================"
    Write-Host "M26 PLACEMENT: $($c.Label)"
    Write-Host "============================================================"

    # md_engine_splitbench.ps1 predates the inserted [xip]/[rep] lines and its
    # old end-of-run regex therefore exits 3 after a perfectly good capture.
    # Exit 3 means only "summary parser did not match" for this experiment;
    # the raw benchmark itself is valid, so continue to the dedicated Phase-3
    # capture.  Other failures remain fatal for this case.
    & .\scripts\md_engine_splitbench.ps1 -Uf2 $uf2 -Label $c.Label
    $splitRc = $LASTEXITCODE
    if ($splitRc -eq 3) {
        Write-Warning "splitbench raw capture completed but legacy summary parser did not match M26 [xip]/[rep] lines; continuing"
    } elseif ($splitRc -ne 0) {
        Write-Warning "splitbench failed: $($c.Label) rc=$splitRc; continuing with next case"
        $skipped += $c.Label
        continue
    }

    # Reflash/restart for a clean Phase-3-only measurement.  This gives REP
    # elements/sec and traffic MiB/s over the REP workload itself rather than
    # averaging the REP work over all nine MDSTRESS phases.
    $picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
    if (!(Test-Path $picotool)) {
        throw "picotool not found: $picotool"
    }
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 800
    } catch {}
    & $picotool load -v -x $uf2
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "phase3 flash failed: $($c.Label); continuing"
        $skipped += $c.Label
        continue
    }
    Start-Sleep -Milliseconds 1000

    $base = Join-Path .\logs ("rep-phase3-{0}-{1}" -f $c.Label,$stamp)
    python .\scripts\mdstress_capture.py $Port --phases 3 --label $c.Label --output $base
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "REP phase capture failed: $($c.Label); continuing with next case"
        $skipped += $c.Label
        continue
    }

    $j = Get-Content ($base + ".json") -Raw | ConvertFrom-Json
    $p = $j.phases[0]
    $results += [pscustomobject]@{
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

Write-Host "`n=== M26 PLACEMENT + REP SUMMARY ==="
if ($results.Count) {
    $results | Format-Table -AutoSize
    $csv = Join-Path .\logs ("m26-placement-rep-{0}.csv" -f $stamp)
    $results | Export-Csv $csv -NoTypeInformation
    Write-Host "saved: $csv"
} else {
    Write-Warning "no placement cases completed"
}
if ($skipped.Count) {
    Write-Host "skipped: $($skipped -join ', ')"
}
