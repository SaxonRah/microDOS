param(
    [int]$CaptureSeconds = 360,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"

$variants = @(
    @{ KiB = 192; Target = "microdos_pico_m28f_combo192_qmi"; Label = "M28g-arena192" },
    @{ KiB = 224; Target = "microdos_pico_m28f_combo224_qmi"; Label = "M28g-arena224" },
    @{ KiB = 256; Target = "microdos_pico_m28f_combo256_qmi"; Label = "M28g-arena256" }
)

$results = @()

Write-Host ""
Write-Host "============================================================"
Write-Host "M28g CMAKE REGENERATE"
Write-Host "============================================================"

# Re-run CMake generation so newly-added matrix targets are guaranteed to be
# present in the Ninja graph before the first --target build.
& cmake -S .\pico -B .\build-pico\out
if ($LASTEXITCODE -ne 0) {
    throw "CMake regenerate failed: $LASTEXITCODE"
}

foreach ($v in $variants) {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M28g BUILD $($v.KiB) KiB"
    Write-Host "============================================================"

    & cmake --build .\build-pico\out --target $v.Target
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for $($v.KiB) KiB: $LASTEXITCODE"
    }

    $elf = ".\build-pico\out\$($v.Target).elf"
    $uf2 = ".\build-pico\out\$($v.Target).uf2"
    if (-not (Test-Path $uf2)) {
        throw "UF2 not found: $uf2"
    }

    if ($BuildOnly) {
        continue
    }

    Write-Host ""
    Write-Host "=== M28g SPLITBENCH $($v.KiB) KiB ==="

    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $uf2 `
        -Label $v.Label `
        -CaptureSeconds $CaptureSeconds

    if ($LASTEXITCODE -ne 0) {
        throw "Splitbench failed for $($v.KiB) KiB: $LASTEXITCODE"
    }

    $log = Get-ChildItem ".\logs\engine-splitbench-$($v.Label)-*.txt" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $log) {
        throw "No splitbench log found for $($v.KiB) KiB"
    }

    $summary = Get-Content $log.FullName |
        Select-String `
            '^=== ENGINE INTERVAL SUMMARY:', `
            '^BOOT\s', `
            '^DOS2TEST #', `
            '^MDSTRESS AUTO', `
            '^\[m25\] native=', `
            '^\[m25\] cycles:', `
            '^\[m25\] top steps:'

    $results += [pscustomobject]@{
        KiB = $v.KiB
        Log = $log.FullName
        Summary = ($summary | ForEach-Object { $_.Line }) -join "`n"
    }
}

if ($BuildOnly) {
    Write-Host ""
    Write-Host "M28g build-only complete."
    exit 0
}

Write-Host ""
Write-Host "============================================================"
Write-Host "M28g ARENA MATRIX RESULT"
Write-Host "============================================================"

Write-Host ""
Write-Host "160 KiB M28f control:"
Write-Host "  BOOT          2.250 MIPS"
Write-Host "  DOS2TEST #1   2.151 MIPS"
Write-Host "  DOS2TEST #2   1.883 MIPS"
Write-Host "  DOS2TEST #3   1.551 MIPS"
Write-Host "  MDSTRESS     12.388 MIPS"

Write-Host ""
Write-Host "Frozen M27 reference:"
Write-Host "  BOOT          2.079 MIPS"
Write-Host "  DOS2TEST #1   2.083 MIPS"
Write-Host "  DOS2TEST #2   2.003 MIPS"
Write-Host "  DOS2TEST #3   1.883 MIPS"
Write-Host "  MDSTRESS     13.068 MIPS"

foreach ($r in $results) {
    Write-Host ""
    Write-Host "------------------------------------------------------------"
    Write-Host "$($r.KiB) KiB"
    Write-Host "------------------------------------------------------------"
    Write-Host $r.Summary
    Write-Host "log: $($r.Log)"
}

Write-Host ""
Write-Host "Compare especially:"
Write-Host "  - DOS2TEST #2/#3 and MDSTRESS MIPS"
Write-Host "  - [m25] translations and flushes"
Write-Host "  - translate% and dispatch/other%"
Write-Host ""
Write-Host "Paste the complete M28g ARENA MATRIX RESULT block plus all three build memory-region blocks."
