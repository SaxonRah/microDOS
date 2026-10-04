param(
    [int]$CaptureSeconds = 300,
    [int]$SliceInstructions = 10000
)

$ErrorActionPreference = "Stop"

$Repo = (Get-Location).Path
$CMake = Join-Path $Repo "pico\CMakeLists.txt"
$PicoMain = Join-Path $Repo "pico\microdos_pico.c"
$Nv2Runtime = Join-Path $Repo "src\runtime\native_v2_runtime.c"
$Bench = Join-Path $Repo "md_nativev2_phase3p_completion_splitbench.ps1"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
$Uf2 = Join-Path $Repo "build-pico\out\microdos_pico_nativev2.uf2"

foreach ($p in @($CMake, $PicoMain, $Nv2Runtime, $Bench)) {
    if (-not (Test-Path $p)) { throw "Required file not found: $p" }
}
if ($SliceInstructions -lt 1000) { throw "SliceInstructions must be >= 1000." }

$CMakeBytes = [IO.File]::ReadAllBytes($CMake)
$PicoBytes = [IO.File]::ReadAllBytes($PicoMain)
$Nv2Bytes = [IO.File]::ReadAllBytes($Nv2Runtime)
$CMakeText = [Text.Encoding]::UTF8.GetString($CMakeBytes)
$PicoText = [Text.Encoding]::UTF8.GetString($PicoBytes)
$Nv2Text = [Text.Encoding]::UTF8.GetString($Nv2Bytes)
$Utf8NoBom = New-Object Text.UTF8Encoding($false)

function Replace-ExactlyOnce([string]$Text, [string]$Old, [string]$New, [string]$What) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Could not find marker for $What. No source files have been written." }
    $second = $Text.IndexOf($Old, $first + $Old.Length, [StringComparison]::Ordinal)
    if ($second -ge 0) { throw "Marker for $What was not unique. No source files have been written." }
    return $Text.Substring(0, $first) + $New + $Text.Substring($first + $Old.Length)
}

function Configure-Production {
    Write-Host "Configuring restored production tree..."
    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Production Pico reconfigure failed with exit code $LASTEXITCODE."
    }
}

# Normalize after any prior temporary experiment.
Configure-Production

$PicoHybrid = [regex]::Replace(
    $PicoText,
    '(?m)^#define\s+MD_SLICE\s+\d+u\s*$',
    "#define MD_SLICE $($SliceInstructions)u",
    1
)
if ($PicoHybrid -eq $PicoText) {
    throw "Could not locate MD_SLICE in pico\microdos_pico.c."
}

$HybridBlock = @'

# ---- TEMPORARY Phase 4D: kernel-AOT + Native-v2 store coexistence --------
if(TARGET microdos_pico_nativev2)
    get_target_property(_md_nv2_defs microdos_pico_nativev2 COMPILE_DEFINITIONS)
    if(NOT _md_nv2_defs)
        set(_md_nv2_defs "")
    endif()

    list(REMOVE_ITEM _md_nv2_defs
        "MICRODOS_PICO_KERNEL_AOT=0"
        "MICRODOS_PICO_KERNEL_AOT=1"
        "MICRODOS_PICO_DOS2TEST_AOT=0"
        "MICRODOS_PICO_DOS2TEST_AOT=1"
        "MICRODOS_SYSTEM_ENABLE_AOT=0"
        "MICRODOS_SYSTEM_ENABLE_AOT=1"
        "MICRODOS_TRANSLATION_SUPPORT=0"
        "MICRODOS_TRANSLATION_SUPPORT=1"
        "MD_X86_TRACK_WRITES=0"
        "MD_X86_TRACK_WRITES=1")

    list(APPEND _md_nv2_defs
        "MICRODOS_PICO_KERNEL_AOT=1"
        "MICRODOS_PICO_DOS2TEST_AOT=0"
        "MICRODOS_SYSTEM_ENABLE_AOT=1"
        "MICRODOS_TRANSLATION_SUPPORT=1"
        "MD_X86_TRACK_WRITES=1"
        "MD_AOT_ATTACH_SLOTS=1"
        "MD_AOT_LIVE_PAGES=5")

    set_property(TARGET microdos_pico_nativev2
        PROPERTY COMPILE_DEFINITIONS "${_md_nv2_defs}")

    target_sources(microdos_pico_nativev2 PRIVATE
        "${MD_ROOT}/src/runtime/x86_block_cache.c"
        "${MD_GENERATED}/msdos2_recomp.c")

    # The generated kernel compiler explicitly supports a compact mode:
    # shared out-of-line ALU/shift/condition/chunk helpers instead of
    # duplicating those semantics throughout the generated body.  Apply -Os
    # only to the generated kernel source; the interpreter and Native-v2 hot
    # paths retain their normal -O2 build.
    set_source_files_properties("${MD_GENERATED}/msdos2_recomp.c" PROPERTIES
        COMPILE_DEFINITIONS "MD_AOT_COMPACT=1"
        COMPILE_OPTIONS "-Os")
endif()
# ---- END TEMPORARY Phase 4D ----------------------------------------------
'@

$CMakeHybrid = $CMakeText + $HybridBlock

# Native-v2 direct stores are already range-proven by this runtime.  With
# static AOT installed, permit those direct stores only when every destination
# page has no live AOT bytes.  Native-v2's own cached code remains protected by
# its full byte-image validation on every re-entry.
$helperMarker = @'
static void md_nv2_rt_reject(MdNativeV2RuntimeSlot *slot,
'@

$helper = @'
static int md_nv2_rt_range_hits_live_aot(const MdX86 *cpu,
                                         uint32_t start,
                                         uint32_t end)
{
    unsigned first_page;
    unsigned last_page;
    unsigned page;

    if (cpu == NULL || start > end || end >= MD_X86_ADDRESS_SPACE)
        return 1;

    if (cpu->aot_live_bits == NULL)
        return 0;

    first_page = start >> MD_X86_CODE_PAGE_SHIFT;
    last_page = end >> MD_X86_CODE_PAGE_SHIFT;

    for (page = first_page; page <= last_page; ++page) {
        if (cpu->aot_live_bits[page] != NULL)
            return 1;
    }

    return 0;
}

'@

$Nv2Hybrid = Replace-ExactlyOnce `
    $Nv2Text `
    $helperMarker `
    ($helper + $helperMarker) `
    "AOT range helper"

$oldStoreGuard = @'
        if (!(data_end < code_start || data_start > code_end)) {
            ++runtime->store_guard_rejects;
            return false;
        }
'@
$newStoreGuard = @'
        if (!(data_end < code_start || data_start > code_end) ||
            md_nv2_rt_range_hits_live_aot(cpu, data_start, data_end)) {
            ++runtime->store_guard_rejects;
            return false;
        }
'@
$Nv2Hybrid = Replace-ExactlyOnce `
    $Nv2Hybrid `
    $oldStoreGuard `
    $newStoreGuard `
    "streaming store AOT guard"

$oldRepGuard = @'
        if (!(src_end < code_start || src_start > code_end) ||
            !(dst_end < code_start || dst_start > code_end)) {
            ++runtime->store_guard_rejects;
            return false;
        }
'@
$newRepGuard = @'
        if (!(src_end < code_start || src_start > code_end) ||
            !(dst_end < code_start || dst_start > code_end) ||
            md_nv2_rt_range_hits_live_aot(cpu, src_start, src_end) ||
            md_nv2_rt_range_hits_live_aot(cpu, dst_start, dst_end)) {
            ++runtime->store_guard_rejects;
            return false;
        }
'@
$Nv2Hybrid = Replace-ExactlyOnce `
    $Nv2Hybrid `
    $oldRepGuard `
    $newRepGuard `
    "REP/string AOT guard"

$oldStackGuard = @'
        if (cpu->ss > 0xEFFFu ||
            !(data_end < code_start || data_start > code_end)) {
            ++runtime->stack_guard_rejects;
            return false;
        }
'@
$newStackGuard = @'
        if (cpu->ss > 0xEFFFu ||
            !(data_end < code_start || data_start > code_end) ||
            md_nv2_rt_range_hits_live_aot(cpu, data_start, data_end)) {
            ++runtime->stack_guard_rejects;
            return false;
        }
'@
$Nv2Hybrid = Replace-ExactlyOnce `
    $Nv2Hybrid `
    $oldStackGuard `
    $newStackGuard `
    "stack push/pop AOT guard"

$oldCallGuard = @'
        if (data_end >= MD_X86_ADDRESS_SPACE ||
            md_nv2_rt_range_overlaps_code(slot, data_start, data_end)) {
            ++runtime->stack_guard_rejects;
            return false;
        }
'@
$newCallGuard = @'
        if (data_end >= MD_X86_ADDRESS_SPACE ||
            md_nv2_rt_range_overlaps_code(slot, data_start, data_end) ||
            md_nv2_rt_range_hits_live_aot(cpu, data_start, data_end)) {
            ++runtime->stack_guard_rejects;
            return false;
        }
'@
$Nv2Hybrid = Replace-ExactlyOnce `
    $Nv2Hybrid `
    $oldCallGuard `
    $newCallGuard `
    "CALL graph AOT guard"

$oldExecute = @'
    native_rc = md_native_v2_execute(cpu, &slot->code);
'@
$newExecute = @'
    /*
     * Phase 4D: md_native_v2_execute() conservatively rejects every direct
     * store whenever a page tracker exists. The exact store destinations have
     * now been proved above to miss both this Native-v2 region and every page
     * containing live AOT code. Temporarily hide only the executable-page
     * table from the low-level entry guard; restore it immediately afterward.
     *
     * Non-store native regions never need this bypass.
     */
    if (slot->code.has_store) {
        uint8_t *saved_code_page_executable = cpu->code_page_executable;
        cpu->code_page_executable = NULL;
        native_rc = md_native_v2_execute(cpu, &slot->code);
        cpu->code_page_executable = saved_code_page_executable;
    } else {
        native_rc = md_native_v2_execute(cpu, &slot->code);
    }
'@
$Nv2Hybrid = Replace-ExactlyOnce `
    $Nv2Hybrid `
    $oldExecute `
    $newExecute `
    "Native-v2 tracked-store entry"

$HybridBuilt = $false
$BenchExit = 0

try {
    [IO.File]::WriteAllText($CMake, $CMakeHybrid, $Utf8NoBom)
    [IO.File]::WriteAllText($PicoMain, $PicoHybrid, $Utf8NoBom)
    [IO.File]::WriteAllText($Nv2Runtime, $Nv2Hybrid, $Utf8NoBom)

    Write-Host ""
    Write-Host "=== Phase 4D compact kernel-AOT + Native-v2 A/B ==="
    Write-Host "Temporary scheduler slice: $SliceInstructions"
    Write-Host "Kernel AOT: ON, compact generated mode"
    Write-Host "Kernel AOT source optimization: -Os"
    Write-Host "AOT metadata: 1 attachment slot, 5 live-page bitmaps"
    Write-Host "Native-v2: ON"
    Write-Host "Kernel AOT: MD_AOT_COMPACT + -Os; Native-v2 store coexistence retained"
    Write-Host ""

    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Temporary Pico reconfigure failed with exit code $LASTEXITCODE."
    }

    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Phase 4D hybrid firmware build failed with exit code $LASTEXITCODE."
    }

    if (-not (Test-Path $Uf2)) {
        throw "Hybrid build completed but UF2 was not found: $Uf2"
    }

    $HybridBuilt = $true
}
finally {
    [IO.File]::WriteAllBytes($CMake, $CMakeBytes)
    [IO.File]::WriteAllBytes($PicoMain, $PicoBytes)
    [IO.File]::WriteAllBytes($Nv2Runtime, $Nv2Bytes)
    Write-Host "Restored CMakeLists.txt, microdos_pico.c, and native_v2_runtime.c byte-for-byte."

    if (-not $HybridBuilt) {
        Write-Host "Hybrid did not build; restoring production build state..."
        & cmake -S .\pico -B .\build-pico\out
        if ($LASTEXITCODE -eq 0) {
            & cmake --build .\build-pico\out --target microdos_pico_nativev2
        }
    }
}

if (-not $HybridBuilt) { exit 1 }

Write-Host ""
Write-Host "Running completion-boundary workload on Phase 4D firmware..."
& $Bench -CaptureSeconds $CaptureSeconds
$BenchExit = $LASTEXITCODE

Write-Host ""
Write-Host "Reconfiguring and rebuilding production Phase 3P..."
Configure-Production
& cmake --build .\build-pico\out --target microdos_pico_nativev2
if ($LASTEXITCODE -ne 0) {
    throw "Production Phase 3P rebuild failed with exit code $LASTEXITCODE."
}

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
Write-Host "=== Phase 4D complete ==="
Write-Host "Production sources restored byte-for-byte."
Write-Host "Production Phase 3P rebuilt and reflashed."
Write-Host "Send back the full console/log output."
Write-Host ""

if ($BenchExit -ne 0) { exit $BenchExit }
