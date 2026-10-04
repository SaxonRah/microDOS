param(
    [int]$CaptureSeconds = 240,
    [int]$SliceInstructions = 10000
)

$ErrorActionPreference = "Stop"

$Repo = (Get-Location).Path
$PicoMain = Join-Path $Repo "pico\microdos_pico.c"
$Bench = Join-Path $Repo "md_nativev2_phase3p_completion_splitbench.ps1"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not (Test-Path $PicoMain)) { throw "Cannot find $PicoMain" }
if (-not (Test-Path $Bench)) { throw "Cannot find $Bench" }

$OriginalBytes = [IO.File]::ReadAllBytes($PicoMain)
$Text = [Text.Encoding]::UTF8.GetString($OriginalBytes)
$Utf8NoBom = New-Object Text.UTF8Encoding($false)

function Replace-ExactlyOnce([string]$Text, [string]$Old, [string]$New, [string]$What) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Could not find marker for $What. Source not modified." }
    $second = $Text.IndexOf($Old, $first + $Old.Length, [StringComparison]::Ordinal)
    if ($second -ge 0) { throw "Marker for $What was not unique. Source not modified." }
    return $Text.Substring(0, $first) + $New + $Text.Substring($first + $Old.Length)
}

$Profile = [regex]::Replace(
    $Text,
    '(?m)^#define\s+MD_SLICE\s+\d+u\s*$',
    "#define MD_SLICE $($SliceInstructions)u",
    1
)
if ($Profile -eq $Text) { throw "Could not locate MD_SLICE. Source not modified." }

$dumpFn = @'
static void md_dump_guest_bytes(uint16_t seg, uint16_t start, unsigned count)
{
    unsigned i;
    md_say("[hot-bytes] %04X:%04X len=%u", seg, start, count);
    for (i = 0u; i < count; ++i) {
        if ((i & 15u) == 0u)
            md_say("\n[hot-bytes] %04X:%04X ", seg, (uint16_t)(start + i));
        md_say("%02X ", md_x86_read8(&g_sys.runtime.cpu, seg, (uint16_t)(start + i)));
    }
    md_say("\n");
}

static void md_dump_phase4_hot_windows(void)
{
    md_say("[hot-bytes] --- phase4 kernel windows ---\n");
    md_dump_guest_bytes(0x1000u, 0x0670u, 0x50u);
    md_dump_guest_bytes(0x1000u, 0x1120u, 0x40u);
    md_dump_guest_bytes(0x1000u, 0x37E0u, 0x30u);
    md_dump_guest_bytes(0x0800u, 0x00F0u, 0x40u);
}

'@

$Profile = Replace-ExactlyOnce `
    $Profile `
    "static void md_stats(uint64_t start_us)" `
    ($dumpFn + "static void md_stats(uint64_t start_us)") `
    "stats function"

$Profile = Replace-ExactlyOnce `
    $Profile `
    "    g_perf_mark=now;" `
    "    md_dump_phase4_hot_windows();`r`n    g_perf_mark=now;" `
    "stats mark"

$Built = $false
try {
    [IO.File]::WriteAllText($PicoMain, $Profile, $Utf8NoBom)

    Write-Host "=== Phase 4 hot-byte dump build ==="
    Write-Host "Temporary MD_SLICE: $SliceInstructions"
    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Hot-byte profiling firmware build failed with exit code $LASTEXITCODE."
    }
    $Built = $true
}
finally {
    [IO.File]::WriteAllBytes($PicoMain, $OriginalBytes)
    Write-Host "Restored pico\microdos_pico.c byte-for-byte."
}

if (-not $Built) { exit 1 }

Write-Host ""
Write-Host "Running workload; look for [hot-bytes] lines..."
& $Bench -CaptureSeconds $CaptureSeconds
$BenchExit = $LASTEXITCODE

Write-Host ""
Write-Host "Rebuilding production Phase 3P firmware..."
& cmake --build .\build-pico\out --target microdos_pico_nativev2
if ($LASTEXITCODE -ne 0) {
    throw "Production firmware rebuild failed with exit code $LASTEXITCODE."
}

$Uf2 = Join-Path $Repo "build-pico\out\microdos_pico_nativev2.uf2"
Write-Host "Reflashing production Phase 3P..."
try {
    & $Picotool reboot -f -u
    Start-Sleep -Milliseconds 1000
} catch {}
& $Picotool load -v -x $Uf2
if ($LASTEXITCODE -ne 0) {
    throw "Production firmware rebuild succeeded, but reflashing failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "=== Phase 4 hot-byte dump complete ==="
Write-Host "Production source restored and production Phase 3P rebuilt/reflashed."
Write-Host "Send back the log containing [hot-bytes] lines."
Write-Host ""

if ($BenchExit -ne 0) { exit $BenchExit }
