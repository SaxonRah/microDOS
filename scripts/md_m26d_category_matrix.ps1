param(
    [switch]$Build,
    [string]$Port = "COM5",
    [string]$BuildDir = ".\build-pico\out"
)
$ErrorActionPreference = "Stop"

$cases = @(
    @{ Target="microdos_pico_m25_nv2_exec_qmi";        Label="m26d-exec-control" },
    @{ Target="microdos_pico_m25_nv2_exec_comp_qmi";   Label="m26d-exec-compiler" },
    @{ Target="microdos_pico_m25_nv2_exec_interp_qmi"; Label="m26d-exec-interp" },
    @{ Target="microdos_pico_m25_nv2_exec_dos_qmi";    Label="m26d-exec-dos" },
    @{ Target="microdos_pico_m25_nv2_exec_combo_qmi";  Label="m26d-exec-combo" }
)
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$rows = @()

function Get-QmiRow($csvPath, $interval, $category) {
    $all = Import-Csv $csvPath
    return $all | Where-Object { $_.Interval -eq $interval -and $_.Category -eq $category } | Select-Object -First 1
}

foreach ($c in $cases) {
    $ramBytes = 0
    if ($Build) {
        Write-Host "`n=== BUILD $($c.Target) ==="
        $buildLog = Join-Path .\logs ("m26d-build-{0}-{1}.txt" -f $c.Label,$stamp)
        New-Item -ItemType Directory -Force .\logs | Out-Null
        # Windows PowerShell turns redirected native stderr (2>&1) into
        # ErrorRecord objects.  With $ErrorActionPreference="Stop", harmless
        # CMake status lines written to stderr can terminate the script before
        # we ever inspect cmake's real process exit code.  Merge the streams in
        # cmd.exe instead, then let PowerShell consume one ordinary text stream.
        $cmakeExe = (Get-Command cmake.exe -ErrorAction Stop).Source
        $cmdLine = '"{0}" --build "{1}" --target "{2}" 2>&1' -f $cmakeExe, $BuildDir, $c.Target
        $buildOut = @(& cmd.exe /d /s /c $cmdLine | Tee-Object -FilePath $buildLog)
        $buildRc = $LASTEXITCODE
        if ($buildRc -ne 0) { throw "build failed: $($c.Target) rc=$buildRc (see $buildLog)" }
        $joined = ($buildOut | Out-String)
        $m = [regex]::Match($joined, '(?m)^\s*RAM:\s+([0-9]+)\s+B')
        if ($m.Success) { $ramBytes = [uint64]$m.Groups[1].Value }
    }

    $uf2 = Join-Path $BuildDir ($c.Target + ".uf2")
    if (!(Test-Path $uf2)) { throw "missing UF2: $uf2 (use -Build)" }

    Write-Host "`n============================================================"
    Write-Host "M26d CATEGORY: $($c.Label)"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 -Uf2 $uf2 -Label $c.Label
    if ($LASTEXITCODE -ne 0) { throw "splitbench failed: $($c.Label) rc=$LASTEXITCODE" }

    $splitCsv = Get-ChildItem .\logs\engine-splitbench-$($c.Label)-*.csv | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $log = Get-ChildItem .\logs\engine-splitbench-$($c.Label)-*.txt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $splitCsv -or $null -eq $log) { throw "missing splitbench outputs for $($c.Label)" }

    $qcsv = Join-Path .\logs ("m26d-qmi-{0}-{1}.csv" -f $c.Label,$stamp)
    python .\scripts\md_m26c_qmi_extract.py $log.FullName --label $c.Label --csv $qcsv
    if ($LASTEXITCODE -ne 0) { throw "QMI extraction failed: $($c.Label)" }

    $perf = Import-Csv $splitCsv.FullName
    $d1 = $perf | Where-Object { $_.Interval -eq 'DOS2TEST #1' } | Select-Object -First 1
    $d2 = $perf | Where-Object { $_.Interval -eq 'DOS2TEST #2' } | Select-Object -First 1
    $d3 = $perf | Where-Object { $_.Interval -eq 'DOS2TEST #3' } | Select-Object -First 1
    $md = $perf | Where-Object { $_.Interval -eq 'MDSTRESS AUTO' } | Select-Object -First 1

    $qi = Get-QmiRow $qcsv 'DOS2TEST #3' 'interp'
    $qt = Get-QmiRow $qcsv 'DOS2TEST #3' 'translate'
    $qs = Get-QmiRow $qcsv 'DOS2TEST #3' 'step'
    $qd = Get-QmiRow $qcsv 'DOS2TEST #3' 'dos-int'
    $qn = Get-QmiRow $qcsv 'DOS2TEST #3' 'm25-native'

    # Clean phase-3 REP endpoint.
    $picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
    if (!(Test-Path $picotool)) { throw "picotool not found: $picotool" }
    try { & $picotool reboot -f -u; Start-Sleep -Milliseconds 800 } catch {}
    & $picotool load -v -x $uf2
    if ($LASTEXITCODE -ne 0) { throw "phase3 flash failed: $($c.Label)" }
    Start-Sleep -Milliseconds 1000
    $base = Join-Path .\logs ("m26d-rep-phase3-{0}-{1}" -f $c.Label,$stamp)
    python .\scripts\mdstress_capture.py $Port --phases 3 --label $c.Label --output $base
    if ($LASTEXITCODE -ne 0) { throw "REP phase capture failed: $($c.Label)" }
    $j = Get-Content ($base + ".json") -Raw | ConvertFrom-Json
    $p = $j.phases[0]

    $rows += [pscustomobject]@{
        Label=$c.Label
        RAM_KiB=[math]::Round($ramBytes / 1024.0,1)
        DOS1=[double]$d1.MIPS
        DOS2=[double]$d2.MIPS
        DOS3=[double]$d3.MIPS
        MDSTRESS=[double]$md.MIPS
        REP_MiBs=[double]$p.rep_traffic_mib_s
        D3_InterpMissG=if($qi){[double]$qi.MissPerGuest}else{0}
        D3_TranslateMissG=if($qt){[double]$qt.MissPerGuest}else{0}
        D3_StepMissG=if($qs){[double]$qs.MissPerGuest}else{0}
        D3_DosMissG=if($qd){[double]$qd.MissPerGuest}else{0}
        D3_M25MissG=if($qn){[double]$qn.MissPerGuest}else{0}
    }
}

Write-Host "`n=== M26d CATEGORY ISOLATION SUMMARY ==="
$rows | Format-Table -AutoSize
$csv = Join-Path .\logs ("m26d-category-summary-{0}.csv" -f $stamp)
$rows | Export-Csv $csv -NoTypeInformation
Write-Host "saved: $csv"
Write-Host ""
Write-Host "Interpretation: compare each one-category build against m26d-exec-control."
Write-Host "The combined build shows how close the measured hot working set gets to all-SRAM performance."
