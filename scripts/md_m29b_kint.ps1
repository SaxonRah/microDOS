param(
    [int]$DiffSeconds = 2400,
    [int]$CaptureSeconds = 360,
    [switch]$SkipDiff,
    [switch]$Profile
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$target = "microdos_pico_m29b_kint_qmi"
$buildDir = Join-Path $repo "build-pico\out"
$uf2 = Join-Path $buildDir "$target.uf2"

Push-Location $repo
try {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M29b HOST TESTS"
    Write-Host "============================================================"
    & .\md.bat test all
    if ($LASTEXITCODE -ne 0) { throw "host tests failed: $LASTEXITCODE" }

    if (-not $SkipDiff) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M29b FULL RP2350 TRANSLATOR DIFFERENTIAL"
        Write-Host "============================================================"
        & .\scripts\md_translate_diff_pico.ps1 `
            -Build `
            -Label "M29b-generic-kint" `
            -CaptureSeconds $DiffSeconds
        if ($LASTEXITCODE -ne 0) { throw "M29b translator differential failed: $LASTEXITCODE" }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M29b PRODUCTION BUILD"
    Write-Host "============================================================"
    & cmake -S .\pico -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & cmake --build $buildDir --target $target
    if ($LASTEXITCODE -ne 0) { throw "M29b build failed: $LASTEXITCODE" }
    if (-not (Test-Path $uf2)) { throw "M29b UF2 missing: $uf2" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M29b PRODUCTION SPLITBENCH"
    Write-Host "============================================================"
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $uf2 `
        -Label "M29b-kint" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) { throw "M29b splitbench failed: $LASTEXITCODE" }

    $log = Get-ChildItem .\logs\engine-splitbench-M29b-kint-*.txt |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $log) { throw "M29b splitbench log not found" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M29b RESULT"
    Write-Host "============================================================"
    Get-Content $log.FullName |
        Select-String `
            '^=== ENGINE INTERVAL SUMMARY:', `
            '^BOOT\s', `
            '^DOS2TEST #', `
            '^MDSTRESS AUTO', `
            '^\[m25\] native=', `
            '^\[m25\] cycles:', `
            '^\[m25\] top steps:'

    Write-Host ""
    Write-Host "Frozen M28 reference:"
    Write-Host "  BOOT          2.532 MIPS"
    Write-Host "  DOS2TEST #1   2.463 MIPS"
    Write-Host "  DOS2TEST #2   2.260 MIPS"
    Write-Host "  DOS2TEST #3   1.972 MIPS"
    Write-Host "  MDSTRESS     15.200 MIPS"

    if ($Profile) {
        $ptarget = "microdos_pico_m29a_intprofile"
        $puf2 = Join-Path $buildDir "$ptarget.uf2"

        Write-Host ""
        Write-Host "============================================================"
        Write-Host "M29b OPTIONAL INT PROFILE"
        Write-Host "============================================================"
        & cmake --build $buildDir --target $ptarget
        if ($LASTEXITCODE -ne 0) { throw "profile target build failed: $LASTEXITCODE" }

        & .\scripts\md_engine_splitbench.ps1 `
            -Uf2 $puf2 `
            -Label "M29b-kint-profile" `
            -CaptureSeconds $CaptureSeconds
        if ($LASTEXITCODE -ne 0) { throw "profile splitbench failed: $LASTEXITCODE" }

        $plog = Get-ChildItem .\logs\engine-splitbench-M29b-kint-profile-*.txt |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1

        Write-Host ""
        Write-Host "M29b INT PROFILE CHECK"
        Get-Content $plog.FullName |
            Select-String '^\[intprof\]', '^\[m25\] top steps:'
        Write-Host "profile saved: $($plog.FullName)"
    }

    Write-Host ""
    Write-Host "production saved: $($log.FullName)"
}
finally {
    Pop-Location
}
