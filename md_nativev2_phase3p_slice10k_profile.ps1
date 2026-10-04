param(
    [int]$CaptureSeconds = 300,
    [int]$SliceInstructions = 10000
)

$ErrorActionPreference = "Stop"

$Repo = (Get-Location).Path
$PicoMain = Join-Path $Repo "pico\microdos_pico.c"
$Bench = Join-Path $Repo "md_nativev2_phase3p_completion_splitbench.ps1"

if (-not (Test-Path $PicoMain)) {
    throw "Cannot find $PicoMain. Run this script from C:\microDOS."
}
if (-not (Test-Path $Bench)) {
    throw "Cannot find $Bench. Copy md_nativev2_phase3p_completion_splitbench.ps1 into C:\microDOS first."
}
if ($SliceInstructions -lt 1000) {
    throw "SliceInstructions must be >= 1000."
}

$OriginalBytes = [System.IO.File]::ReadAllBytes($PicoMain)
$OriginalText = [System.Text.Encoding]::UTF8.GetString($OriginalBytes)

$pattern = '(?m)^#define\s+MD_SLICE\s+200000u\s*$'
if ($OriginalText -notmatch $pattern) {
    # Also accept an already parameterized/default form, but refuse unknown edits.
    $pattern = '(?m)^#define\s+MD_SLICE\s+\d+u\s*$'
    $matches = [regex]::Matches($OriginalText, $pattern)
    if ($matches.Count -ne 1) {
        throw "Could not uniquely locate '#define MD_SLICE ...u' in pico\microdos_pico.c. Source was not modified."
    }
}

$replacement = "#define MD_SLICE $($SliceInstructions)u"
$ProfileText = [regex]::Replace($OriginalText, $pattern, $replacement, 1)

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

Write-Host "=== Phase 3P scheduler-slice profile ==="
Write-Host "Production source: $PicoMain"
Write-Host "Temporary MD_SLICE: $SliceInstructions instructions"
Write-Host "The source file will be restored byte-for-byte after the profiling firmware is built."
Write-Host ""

try {
    [System.IO.File]::WriteAllText($PicoMain, $ProfileText, $Utf8NoBom)

    Write-Host "Building profiling firmware..."
    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Profiling firmware build failed with exit code $LASTEXITCODE."
    }

    $Uf2 = Join-Path $Repo "build-pico\out\microdos_pico_nativev2.uf2"
    if (-not (Test-Path $Uf2)) {
        throw "Build succeeded but $Uf2 was not found."
    }

    Write-Host ""
    Write-Host "Profiling firmware built successfully."
}
finally {
    [System.IO.File]::WriteAllBytes($PicoMain, $OriginalBytes)
    Write-Host "Restored pico\microdos_pico.c byte-for-byte."
}

Write-Host ""
Write-Host "Running completion-boundary benchmark using the temporary $SliceInstructions-instruction-slice firmware..."
& $Bench -CaptureSeconds $CaptureSeconds
$BenchExit = $LASTEXITCODE

Write-Host ""
Write-Host "Rebuilding production Phase 3P firmware with the restored source..."
& cmake --build .\build-pico\out --target microdos_pico_nativev2
if ($LASTEXITCODE -ne 0) {
    throw "Profile completed, but production firmware rebuild failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "Reflashing restored production Phase 3P firmware..."
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
$Uf2 = Join-Path $Repo "build-pico\out\microdos_pico_nativev2.uf2"
try {
    & $picotool reboot -f -u
    Start-Sleep -Milliseconds 1000
} catch {}
& $picotool load -v -x $Uf2
if ($LASTEXITCODE -ne 0) {
    throw "Production UF2 rebuild succeeded, but reflashing it failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "=== Slice profile complete ==="
Write-Host "Source restored byte-for-byte."
Write-Host "Production 200000-instruction-slice firmware rebuilt and reflashed."
Write-Host ""

if ($BenchExit -ne 0) {
    exit $BenchExit
}
