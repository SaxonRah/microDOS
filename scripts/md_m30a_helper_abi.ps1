param(
    [int]$DiffSeconds = 2600,
    [int]$CaptureSeconds = 360,
    [switch]$SkipDiff,
    [switch]$SkipControl
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repo "build-pico\out"
$controlTarget = "microdos_pico_m29b_kint_qmi"
$candidateTarget = "microdos_pico_m30a_helperabi_qmi"
$controlUf2 = Join-Path $buildDir "$controlTarget.uf2"
$candidateUf2 = Join-Path $buildDir "$candidateTarget.uf2"

function Latest-Log([string]$label) {
    Get-ChildItem ".\logs\engine-splitbench-$label-*.txt" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Show-Summary([string]$name, [string]$path) {
    Write-Host ""
    Write-Host "----- $name -----"
    Get-Content $path |
        Select-String `
            '^=== ENGINE INTERVAL SUMMARY:', `
            '^BOOT\s', `
            '^DOS2TEST #', `
            '^MDSTRESS AUTO', `
            '^\[m25\] native=', `
            '^\[m25\] exits ', `
            '^\[m25\] cycles:'
}

Push-Location $repo
try {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30a HOST TESTS"
    Write-Host "============================================================"
    & .\md.bat test all
    if ($LASTEXITCODE -ne 0) { throw "host tests failed: $LASTEXITCODE" }

    if (-not $SkipDiff) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M30a FULL RP2350 TRANSLATOR DIFFERENTIAL"
        Write-Host "============================================================"
        & .\scripts\md_translate_diff_pico.ps1 `
            -Build `
            -Label "M30a-helper-abi" `
            -CaptureSeconds $DiffSeconds
        if ($LASTEXITCODE -ne 0) {
            throw "M30a differential failed: $LASTEXITCODE"
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30a A/B BUILD"
    Write-Host "============================================================"
    & cmake -S .\pico -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

    & cmake --build $buildDir --target $controlTarget $candidateTarget
    if ($LASTEXITCODE -ne 0) { throw "A/B build failed: $LASTEXITCODE" }

    if (-not $SkipControl) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M30a CONTROL: M29b"
        Write-Host "============================================================"
        & .\scripts\md_engine_splitbench.ps1 `
            -Uf2 $controlUf2 `
            -Label "M30a-control-m29b" `
            -CaptureSeconds $CaptureSeconds
        if ($LASTEXITCODE -ne 0) { throw "M29b control splitbench failed: $LASTEXITCODE" }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30a CANDIDATE: GENERIC HELPER ABI"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $candidateUf2 `
        -Label "M30a-helper-abi" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) { throw "M30a splitbench failed: $LASTEXITCODE" }

    $candidateLog = Latest-Log "M30a-helper-abi"
    if (-not $candidateLog) { throw "M30a candidate log not found" }

    $controlLog = $null
    if (-not $SkipControl) {
        $controlLog = Latest-Log "M30a-control-m29b"
        if (-not $controlLog) { throw "M30a control log not found" }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30a RESULT"
    Write-Host "============================================================"
    if ($controlLog) { Show-Summary "M29b SAME-RUN CONTROL" $controlLog.FullName }
    Show-Summary "M30a HELPER ABI" $candidateLog.FullName

    Write-Host ""
    Write-Host "Historical M29b reference:"
    Write-Host "  BOOT          2.635 MIPS"
    Write-Host "  DOS2TEST #1   2.524 MIPS"
    Write-Host "  DOS2TEST #2   2.306 MIPS"
    Write-Host "  DOS2TEST #3   1.987 MIPS"
    Write-Host "  MDSTRESS     15.128 MIPS"
    Write-Host ""
    Write-Host "Key secondary signal: compare translations / flushes / code= bytes."
    Write-Host "A good M30a result should reduce generated code and preferably flush churn."
    Write-Host "candidate saved: $($candidateLog.FullName)"
    if ($controlLog) { Write-Host "control saved:   $($controlLog.FullName)" }
}
finally {
    Pop-Location
}
