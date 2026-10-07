param(
    [switch]$BuildHost,
    [int]$CaptureSeconds = 360
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $repo
try {
    if ($BuildHost) {
        & .\md.bat build host
        if ($LASTEXITCODE -ne 0) { throw "host build failed: $LASTEXITCODE" }
    }

    Write-Host "=== M29a INT PROFILE BUILD ==="
    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & cmake --build .\build-pico\out --target microdos_pico_m29a_intprofile
    if ($LASTEXITCODE -ne 0) { throw "M29a build failed: $LASTEXITCODE" }

    $uf2 = ".\build-pico\out\microdos_pico_m29a_intprofile.uf2"
    Write-Host "=== M29a INT PROFILE SPLITBENCH ==="
    & .\scripts\md_engine_splitbench.ps1 `
        -Uf2 $uf2 `
        -Label "M29a-int-profile" `
        -CaptureSeconds $CaptureSeconds
    if ($LASTEXITCODE -ne 0) { throw "M29a splitbench failed: $LASTEXITCODE" }

    $log = Get-ChildItem .\logs\engine-splitbench-M29a-int-profile-*.txt |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $log) { throw "M29a log not found" }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "M29a INT PROFILE RESULT"
    Write-Host "============================================================"
    Get-Content $log.FullName |
        Select-String `
            '^=== ENGINE INTERVAL SUMMARY:', `
            '^BOOT\s', `
            '^DOS2TEST #', `
            '^MDSTRESS AUTO', `
            '^\[intprof\]', `
            '^\[m25\] top steps:'
    Write-Host ""
    Write-Host "saved: $($log.FullName)"
}
finally {
    Pop-Location
}
