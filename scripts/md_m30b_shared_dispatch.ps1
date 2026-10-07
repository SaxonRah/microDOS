param(
    [int]$DiffSeconds = 2600,
    [int]$CaptureSeconds = 360,
    [switch]$SkipDiff,
    [switch]$SkipControl
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repo "build-pico\out"

$controlTarget = "microdos_pico_m30a_helperabi_qmi"
$candidateTarget = "microdos_pico_m30b_shareddispatch_qmi"
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
            '^\[m25\] tiering ', `
            '^\[m25\] cycles:'
}

Push-Location $repo
try {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30b HOST TESTS"
    Write-Host "============================================================"
    & .\md.bat test all
    if ($LASTEXITCODE -ne 0) { throw "host tests failed: $LASTEXITCODE" }

    if (-not $SkipDiff) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M30b FULL RP2350 TRANSLATOR DIFFERENTIAL"
        Write-Host "============================================================"
        & .\scripts\md_translate_diff_pico.ps1 `
            -Build `
            -Label "M30b-shared-dispatch" `
            -CaptureSeconds $DiffSeconds
        if ($LASTEXITCODE -ne 0) {
            throw "M30b differential failed: $LASTEXITCODE"
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30b A/B BUILD"
    Write-Host "============================================================"
    & cmake -S .\pico -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

    & cmake --build $buildDir --target $controlTarget $candidateTarget
    if ($LASTEXITCODE -ne 0) { throw "A/B build failed: $LASTEXITCODE" }

    if (-not $SkipControl) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M30b CONTROL: M30a"
        Write-Host "============================================================"
        & .\scripts\md_engine_splitbench.ps1 `
            -Uf2 $controlUf2 `
            -Label "M30b-control-m30a" `
            -CaptureSeconds $CaptureSeconds
        if ($LASTEXITCODE -ne 0) { throw "M30a control splitbench failed: $LASTEXITCODE" }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30b CANDIDATE: SHARED DISPATCH"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $candidateUf2 `
        -Label "M30b-shared-dispatch" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) { throw "M30b splitbench failed: $LASTEXITCODE" }

    $candidateLog = Latest-Log "M30b-shared-dispatch"
    if (-not $candidateLog) { throw "M30b candidate log not found" }

    $controlLog = $null
    if (-not $SkipControl) {
        $controlLog = Latest-Log "M30b-control-m30a"
        if (-not $controlLog) { throw "M30a control log not found" }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M30b RESULT"
    Write-Host "============================================================"
    if ($controlLog) { Show-Summary "M30a SAME-RUN CONTROL" $controlLog.FullName }
    Show-Summary "M30b SHARED DISPATCH" $candidateLog.FullName

    Write-Host ""
    Write-Host "M30a accepted reference from previous run:"
    Write-Host "  BOOT          2.634 MIPS"
    Write-Host "  DOS2TEST #1   2.567 MIPS"
    Write-Host "  DOS2TEST #2   2.389 MIPS"
    Write-Host "  DOS2TEST #3   2.103 MIPS"
    Write-Host "  MDSTRESS     15.261 MIPS"
    Write-Host ""
    Write-Host "Primary signals:"
    Write-Host "  1) DOS #1/#2/#3 MIPS"
    Write-Host "  2) generated code= bytes"
    Write-Host "  3) translations + flushes"
    Write-Host "  4) dispatch/other cycle share"
    Write-Host "  5) MDSTRESS only after considering same-run noise"
    Write-Host ""
    Write-Host "candidate saved: $($candidateLog.FullName)"
    if ($controlLog) { Write-Host "control saved:   $($controlLog.FullName)" }
}
finally {
    Pop-Location
}
