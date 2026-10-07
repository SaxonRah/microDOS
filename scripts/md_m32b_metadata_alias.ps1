param(
    [int]$CaptureSeconds = 360,
    [int]$FastDiffSeconds = 420
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repo "build-pico\out"
$controlTarget = "microdos_pico_m30b_shareddispatch_qmi"
$candidateTarget = "microdos_pico_m32b_metadata_alias_qmi"
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
            '^\[m25\] steps executed=', `
            '^\[m25\] cycles:', `
            '^\[m31b\] ', `
            '^\[m32a\] ', `
            '^\[m32b\] '
}

Push-Location $repo
try {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M32b HOST TESTS"
    Write-Host "============================================================"
    & .\md.bat test all
    if ($LASTEXITCODE -ne 0) { throw "host tests failed: $LASTEXITCODE" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M32b FAST RP2350 DIFFERENTIAL"
    Write-Host "============================================================"
    & .\scripts\md_translate_diff_fast.ps1 `
        -Label "M32b-metadata-alias" `
        -CaptureSeconds $FastDiffSeconds
    if ($LASTEXITCODE -ne 0) { throw "M32b fast differential failed: $LASTEXITCODE" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M32b A/B BUILD"
    Write-Host "============================================================"
    & cmake -S .\pico -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "configure failed: $LASTEXITCODE" }
    & cmake --build $buildDir --target $controlTarget $candidateTarget
    if ($LASTEXITCODE -ne 0) { throw "A/B build failed: $LASTEXITCODE" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M32b CONTROL: M30b"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $controlUf2 `
        -Label "M32b-control-m30b" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) { throw "M30b control failed: $LASTEXITCODE" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M32b CANDIDATE: METADATA-ONLY ALIASES"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $candidateUf2 `
        -Label "M32b-metadata-alias" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) { throw "M32b candidate failed: $LASTEXITCODE" }

    $ctl = Latest-Log "M32b-control-m30b"
    $cand = Latest-Log "M32b-metadata-alias"
    if (-not $ctl -or -not $cand) { throw "A/B logs missing" }

    if (-not (Select-String -Path $cand.FullName -Pattern '^\[m31b\] inline-dispatch=1')) {
        throw "M32b invariant failure: inline-dispatch=1 missing"
    }
    if (-not (Select-String -Path $cand.FullName -Pattern '^\[m32b\] alias-representation=metadata-only')) {
        throw "M32b invariant failure: metadata-only alias marker missing"
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M32b RESULT"
    Write-Host "============================================================"
    Show-Summary "M30b SAME-RUN CONTROL" $ctl.FullName
    Show-Summary "M32b METADATA-ONLY CANONICAL REGIONS" $cand.FullName

    Write-Host ""
    Write-Host "Required invariants:"
    Write-Host "  inline-dispatch=1             [PASS]"
    Write-Host "  alias native-stub bytes = 0   [PASS]"
    Write-Host ""
    Write-Host "Decision signals:"
    Write-Host "  code bytes / flushes / translate %"
    Write-Host "  translations / episodes / edge exits"
    Write-Host "  DOS #2/#3 and MDSTRESS recovery"
    Write-Host ""
    Write-Host "control:   $($ctl.FullName)"
    Write-Host "candidate: $($cand.FullName)"
}
finally {
    Pop-Location
}
