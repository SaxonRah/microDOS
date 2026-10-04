param(
    [int]$CaptureSeconds = 300,
    [int]$SliceInstructions = 10000
)

$ErrorActionPreference = "Stop"

$Repo = (Get-Location).Path
$CMake = Join-Path $Repo "pico\CMakeLists.txt"
$PicoMain = Join-Path $Repo "pico\microdos_pico.c"
$Nv2Runtime = Join-Path $Repo "src\runtime\native_v2_runtime.c"
$GeneratedAot = Join-Path $Repo "build-host\generated\msdos2_recomp.c"
$Bench = Join-Path $Repo "md_nativev2_phase3p_completion_splitbench.ps1"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
$Uf2 = Join-Path $Repo "build-pico\out\microdos_pico_nativev2.uf2"
$TempDir = Join-Path $Repo "build-pico\phase4f"
$FlashAot = Join-Path $TempDir "msdos2_recomp_flash.c"

foreach ($p in @($CMake, $PicoMain, $Nv2Runtime, $GeneratedAot, $Bench)) {
    if (-not (Test-Path $p)) { throw "Required file not found: $p" }
}
if ($SliceInstructions -lt 1000) { throw "SliceInstructions must be >= 1000." }

$CMakeBytes = [IO.File]::ReadAllBytes($CMake)
$PicoBytes = [IO.File]::ReadAllBytes($PicoMain)
$Nv2Bytes = [IO.File]::ReadAllBytes($Nv2Runtime)
$CMakeText = [Text.Encoding]::UTF8.GetString($CMakeBytes)
$PicoText = [Text.Encoding]::UTF8.GetString($PicoBytes)
$Nv2Text = [Text.Encoding]::UTF8.GetString($Nv2Bytes)
$AotText = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($GeneratedAot))
$Utf8NoBom = New-Object Text.UTF8Encoding($false)

function Replace-ExactlyOnce([string]$Text, [string]$Old, [string]$New, [string]$What) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Could not find marker for $What. No production source files have been written." }
    $second = $Text.IndexOf($Old, $first + $Old.Length, [StringComparison]::Ordinal)
    if ($second -ge 0) { throw "Marker for $What was not unique. No production source files have been written." }
    return $Text.Substring(0, $first) + $New + $Text.Substring($first + $Old.Length)
}

function Configure-Production {
    Write-Host "Configuring restored production tree..."
    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Production Pico reconfigure failed with exit code $LASTEXITCODE."
    }
}

# Normalize after previous experiments.
Configure-Production

# -------------------------------------------------------------------------
# Build a TEMPORARY copy of the generated MSDOS.SYS AOT translation unit
# whose own code and large constant tables live in .flashdata.*.
#
# pico/md_pico_blobs.S.in already documents that .flashdata stays in flash
# even for copy_to_ram images.  Production generated source is never edited.
# -------------------------------------------------------------------------

$AotFlash = $AotText

$injectMarker = '#include "microdos/runtime.h"'
if ($AotFlash.IndexOf($injectMarker, [StringComparison]::Ordinal) -lt 0) {
    throw "Generated AOT source does not contain expected runtime.h include."
}

$placementMacros = @'

/*
 * Phase 4F diagnostic placement.
 * .flashdata.* remains XIP-flash resident in Pico COPY_TO_RAM binaries.
 */
#define MD_AOT_XIP_TEXT   __attribute__((section(".flashdata.md_aot_text"), noinline))
#define MD_AOT_XIP_RODATA __attribute__((section(".flashdata.md_aot_rodata")))

'@

$AotFlash = Replace-ExactlyOnce `
    $AotFlash `
    $injectMarker `
    ($injectMarker + $placementMacros) `
    "generated AOT include injection"

# Move the five large/important generated constant tables to flash.
$roPatterns = @(
    '(?m)^static const uint8_t (md_image)\[',
    '(?m)^static const uint8_t (md_code_bits)\[',
    '(?m)^static const uint16_t (md_entry_ip)\[',
    '(?m)^static const uint16_t (md_entry_range)\[',
    '(?m)^static const uint16_t (md_bucket)\['
)

foreach ($pat in $roPatterns) {
    $m = [regex]::Matches($AotFlash, $pat)
    if ($m.Count -ne 1) {
        throw "Expected exactly one generated AOT table for pattern '$pat'; found $($m.Count)."
    }
    $AotFlash = [regex]::Replace(
        $AotFlash,
        $pat,
        { param($x) $x.Value.Replace($x.Groups[1].Value, "MD_AOT_XIP_RODATA $($x.Groups[1].Value)") },
        1
    )
}

# Move every generated md_* function definition in this translation unit to
# flash.  This includes md_body, block lookup/guard helpers, attach/entry
# helpers and the public standalone generated runner.  The regex is restricted
# to top-level definitions whose name begins "md_".
$fnPattern = '(?m)^(static\s+)?(bool|int|MdStopReason|MdAotGuard\s*\*)\s+(md_[A-Za-z0-9_]+)\s*\('
$fnMatches = [regex]::Matches($AotFlash, $fnPattern)
if ($fnMatches.Count -lt 8) {
    throw "Generated AOT function matcher found only $($fnMatches.Count) functions; refusing unsafe placement."
}

$AotFlash = [regex]::Replace(
    $AotFlash,
    $fnPattern,
    {
        param($m)
        $storage = $m.Groups[1].Value
        $rtype = $m.Groups[2].Value
        $name = $m.Groups[3].Value
        return "${storage}MD_AOT_XIP_TEXT $rtype $name("
    }
)

# The exported MdAotProgram descriptor contains function pointers into the
# flash-resident body, so keep the descriptor itself in flash too.
$programPattern = '(?m)^const MdAotProgram ([A-Za-z0-9_]+_program)\s*='
$pm = [regex]::Matches($AotFlash, $programPattern)
if ($pm.Count -ne 1) {
    throw "Expected exactly one MdAotProgram descriptor; found $($pm.Count)."
}
$AotFlash = [regex]::Replace(
    $AotFlash,
    $programPattern,
    'const MdAotProgram MD_AOT_XIP_RODATA $1 =',
    1
)

# Exact per-block AOT execution profiler.  This support code is intentionally
# inserted AFTER XIP function annotation, so the small profiler itself remains
# in normal copy_to_ram SRAM while the generated kernel body stays in XIP.
$aotProfiler = @'
#define MD_AOT_PROFILE_SLOTS 4096u

static uint32_t g_md_aot_prof_keys[MD_AOT_PROFILE_SLOTS];
static uint32_t g_md_aot_prof_entries[MD_AOT_PROFILE_SLOTS];
static uint32_t g_md_aot_prof_guest[MD_AOT_PROFILE_SLOTS];
static uint32_t g_md_aot_prof_used;
static uint32_t g_md_aot_prof_dropped;

static void md_aot_prof_hit(uint16_t ip, uint32_t guest)
{
    const uint32_t key = (uint32_t)ip + 1u;
    uint32_t slot = ((key * 2654435761u) >> 20) & (MD_AOT_PROFILE_SLOTS - 1u);
    uint32_t probe;

    for (probe = 0u; probe < MD_AOT_PROFILE_SLOTS; ++probe) {
        if (g_md_aot_prof_keys[slot] == key) {
            if (g_md_aot_prof_entries[slot] != UINT32_MAX)
                ++g_md_aot_prof_entries[slot];
            if (UINT32_MAX - g_md_aot_prof_guest[slot] >= guest)
                g_md_aot_prof_guest[slot] += guest;
            else
                g_md_aot_prof_guest[slot] = UINT32_MAX;
            return;
        }
        if (g_md_aot_prof_keys[slot] == 0u) {
            g_md_aot_prof_keys[slot] = key;
            g_md_aot_prof_entries[slot] = 1u;
            g_md_aot_prof_guest[slot] = guest;
            ++g_md_aot_prof_used;
            return;
        }
        slot = (slot + 1u) & (MD_AOT_PROFILE_SLOTS - 1u);
    }
    ++g_md_aot_prof_dropped;
}

const uint32_t *md_msdos2_aot_prof_keys(void) { return g_md_aot_prof_keys; }
const uint32_t *md_msdos2_aot_prof_entries(void) { return g_md_aot_prof_entries; }
const uint32_t *md_msdos2_aot_prof_guest(void) { return g_md_aot_prof_guest; }
uint32_t md_msdos2_aot_prof_slots(void) { return MD_AOT_PROFILE_SLOTS; }
uint32_t md_msdos2_aot_prof_used(void) { return g_md_aot_prof_used; }
uint32_t md_msdos2_aot_prof_dropped(void) { return g_md_aot_prof_dropped; }

void md_msdos2_aot_prof_reset(void)
{
    memset(g_md_aot_prof_keys, 0, sizeof(g_md_aot_prof_keys));
    memset(g_md_aot_prof_entries, 0, sizeof(g_md_aot_prof_entries));
    memset(g_md_aot_prof_guest, 0, sizeof(g_md_aot_prof_guest));
    g_md_aot_prof_used = 0u;
    g_md_aot_prof_dropped = 0u;
}

#define MD_AOT_PROF_HIT(ip_, n_) md_aot_prof_hit((uint16_t)(ip_), (uint32_t)(n_))

'@

# md_body() was already annotated by the XIP-placement pass above, so insert
# the SRAM-resident profiler immediately before the annotated function.
$annotatedBodyMarker = "static MD_AOT_XIP_TEXT MdStopReason md_body("
$AotFlash = Replace-ExactlyOnce `
    $AotFlash `
    $annotatedBodyMarker `
    ($aotProfiler + $annotatedBodyMarker) `
    "AOT profiler support"

# Avoid a multiline/line-ending-sensitive match here.  The tail statement is
# emitted exactly once by dosrecomp inside MD_AOT_BLOCK; insert one profiling
# call immediately before it while preserving the C preprocessor continuation.
$aotBlockTail = '        remaining -= (n_); done += (n_); wep = guard->epoch; } while (0)'
$aotBlockTailProfiled = @'
        MD_AOT_PROF_HIT((ip_), (n_)); \
        remaining -= (n_); done += (n_); wep = guard->epoch; } while (0)
'@
$AotFlash = Replace-ExactlyOnce `
    $AotFlash `
    $aotBlockTail `
    $aotBlockTailProfiled `
    "MD_AOT_BLOCK profiler hook"

New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
[IO.File]::WriteAllText($FlashAot, $AotFlash, $Utf8NoBom)
Write-Host "Created temporary flash-resident AOT source:"
Write-Host "  $FlashAot"
Write-Host "  functions marked for XIP flash: $($fnMatches.Count)"

$FlashAotCmake = $FlashAot.Replace('\', '/')

# -------------------------------------------------------------------------
# Same proven Phase 4C hybrid configuration + minimal one-kernel metadata.
# Only change under test: generated kernel-AOT code/table placement.
# -------------------------------------------------------------------------

$PicoHybrid = [regex]::Replace(
    $PicoText,
    '(?m)^#define\s+MD_SLICE\s+\d+u\s*$',
    "#define MD_SLICE $($SliceInstructions)u",
    1
)
if ($PicoHybrid -eq $PicoText) {
    throw "Could not locate MD_SLICE in pico\microdos_pico.c."
}

$aotReport = @'
extern const uint32_t *md_msdos2_aot_prof_keys(void);
extern const uint32_t *md_msdos2_aot_prof_entries(void);
extern const uint32_t *md_msdos2_aot_prof_guest(void);
extern uint32_t md_msdos2_aot_prof_slots(void);
extern uint32_t md_msdos2_aot_prof_used(void);
extern uint32_t md_msdos2_aot_prof_dropped(void);
extern void md_msdos2_aot_prof_reset(void);

static void md_aot_hot_report_and_reset(void)
{
    enum { TOPN = 48 };
    const uint32_t *keys = md_msdos2_aot_prof_keys();
    const uint32_t *entries = md_msdos2_aot_prof_entries();
    const uint32_t *guest = md_msdos2_aot_prof_guest();
    uint32_t top_guest[TOPN] = {0};
    uint32_t top_entries[TOPN] = {0};
    uint16_t top_ip[TOPN] = {0};
    uint64_t total_entries = 0u;
    uint64_t total_guest = 0u;
    uint32_t i;
    unsigned rank;

    for (i = 0u; i < md_msdos2_aot_prof_slots(); ++i) {
        unsigned j;
        if (keys[i] == 0u || guest[i] == 0u) continue;
        total_entries += entries[i];
        total_guest += guest[i];

        for (j = 0u; j < TOPN; ++j) {
            if (guest[i] > top_guest[j]) {
                unsigned k;
                for (k = TOPN - 1u; k > j; --k) {
                    top_guest[k] = top_guest[k - 1u];
                    top_entries[k] = top_entries[k - 1u];
                    top_ip[k] = top_ip[k - 1u];
                }
                top_guest[j] = guest[i];
                top_entries[j] = entries[i];
                top_ip[j] = (uint16_t)(keys[i] - 1u);
                break;
            }
        }
    }

    md_say("[aot-hot] total-entries=%llu total-guest=%llu used=%lu dropped=%lu\n",
           (unsigned long long)total_entries,
           (unsigned long long)total_guest,
           (unsigned long)md_msdos2_aot_prof_used(),
           (unsigned long)md_msdos2_aot_prof_dropped());

    for (rank = 0u; rank < TOPN && top_guest[rank] != 0u; ++rank) {
        uint32_t permille = 0u;
        if (total_guest != 0u)
            permille = (uint32_t)(((uint64_t)top_guest[rank] * 1000u) / total_guest);

        md_say("[aot-hot] %02u ip=%04X entries=%lu guest=%lu share=%lu.%lu%%\n",
               rank + 1u,
               top_ip[rank],
               (unsigned long)top_entries[rank],
               (unsigned long)top_guest[rank],
               (unsigned long)(permille / 10u),
               (unsigned long)(permille % 10u));
    }

    md_msdos2_aot_prof_reset();
}
'@

$PicoHybrid = Replace-ExactlyOnce `
    $PicoHybrid `
    "static void md_stats(uint64_t start_us)" `
    ($aotReport + "static void md_stats(uint64_t start_us)") `
    "AOT hot-block reporter"

$PicoHybrid = Replace-ExactlyOnce `
    $PicoHybrid `
    "    g_perf_mark=now;" `
    "    md_aot_hot_report_and_reset();`r`n    g_perf_mark=now;" `
    "AOT hot-block report call"

$HybridBlock = @"

# ---- TEMPORARY Phase 4F: XIP kernel AOT hot-block profiler -----------------
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
        PROPERTY COMPILE_DEFINITIONS "`$`{_md_nv2_defs}")

    target_sources(microdos_pico_nativev2 PRIVATE
        "`$`{MD_ROOT}/src/runtime/x86_block_cache.c"
        "$FlashAotCmake")

    # Profiling only: instrumenting every AOT block makes md_body() enormous.
    # GCC 14.2 can ICE in RTL sched1 on that function at -O2. MIPS from this
    # run are intentionally irrelevant, so compile only this temporary
    # generated translation unit at -O0 and disable both scheduling passes.
    set_source_files_properties("$FlashAotCmake" PROPERTIES
        COMPILE_OPTIONS "-O0;-fno-schedule-insns;-fno-schedule-insns2")
endif()
# ---- END TEMPORARY Phase 4F ----------------------------------------------
"@

$CMakeHybrid = $CMakeText + $HybridBlock

# -------------------------------------------------------------------------
# Retain the successful Phase 4C Native-v2 / AOT store coexistence rule.
# -------------------------------------------------------------------------

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
$Nv2Hybrid = Replace-ExactlyOnce $Nv2Hybrid $oldStoreGuard $newStoreGuard "streaming store AOT guard"

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
$Nv2Hybrid = Replace-ExactlyOnce $Nv2Hybrid $oldRepGuard $newRepGuard "REP/string AOT guard"

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
$Nv2Hybrid = Replace-ExactlyOnce $Nv2Hybrid $oldStackGuard $newStackGuard "stack push/pop AOT guard"

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
$Nv2Hybrid = Replace-ExactlyOnce $Nv2Hybrid $oldCallGuard $newCallGuard "CALL graph AOT guard"

$oldExecute = @'
    native_rc = md_native_v2_execute(cpu, &slot->code);
'@
$newExecute = @'
    if (slot->code.has_store) {
        uint8_t *saved_code_page_executable = cpu->code_page_executable;
        cpu->code_page_executable = NULL;
        native_rc = md_native_v2_execute(cpu, &slot->code);
        cpu->code_page_executable = saved_code_page_executable;
    } else {
        native_rc = md_native_v2_execute(cpu, &slot->code);
    }
'@
$Nv2Hybrid = Replace-ExactlyOnce $Nv2Hybrid $oldExecute $newExecute "Native-v2 tracked-store entry"

$HybridBuilt = $false
$BenchExit = 0

try {
    [IO.File]::WriteAllText($CMake, $CMakeHybrid, $Utf8NoBom)
    [IO.File]::WriteAllText($PicoMain, $PicoHybrid, $Utf8NoBom)
    [IO.File]::WriteAllText($Nv2Runtime, $Nv2Hybrid, $Utf8NoBom)

    Write-Host ""
    Write-Host "=== Phase 4F v3 XIP kernel-AOT hot-block profiler ==="
    Write-Host "Temporary scheduler slice: $SliceInstructions"
    Write-Host "Kernel AOT mode: FAST / non-compact, XIP"
    Write-Host "Kernel AOT placement: .flashdata.* (XIP flash)"
    Write-Host "Interpreter + Native-v2 placement: production copy_to_ram SRAM"
    Write-Host "AOT metadata: 1 attachment slot, 5 live-page bitmaps"
    Write-Host "Phase 4C store coexistence: retained"
    Write-Host "AOT profiler: exact block entries + guest-retirement weight, top 48"
    Write-Host ""

    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Temporary Pico reconfigure failed with exit code $LASTEXITCODE."
    }

    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Phase 4F hybrid firmware build failed with exit code $LASTEXITCODE."
    }

    if (-not (Test-Path $Uf2)) {
        throw "Phase 4F build completed but UF2 was not found: $Uf2"
    }

    $HybridBuilt = $true
}
finally {
    [IO.File]::WriteAllBytes($CMake, $CMakeBytes)
    [IO.File]::WriteAllBytes($PicoMain, $PicoBytes)
    [IO.File]::WriteAllBytes($Nv2Runtime, $Nv2Bytes)
    Write-Host "Restored CMakeLists.txt, microdos_pico.c, and native_v2_runtime.c byte-for-byte."

    if (-not $HybridBuilt) {
        Write-Host "Phase 4F v3 did not build; restoring production build state..."
        & cmake -S .\pico -B .\build-pico\out
        if ($LASTEXITCODE -eq 0) {
            & cmake --build .\build-pico\out --target microdos_pico_nativev2
        }
    }
}

if (-not $HybridBuilt) { exit 1 }

Write-Host ""
Write-Host "Running completion-boundary workload on Phase 4F firmware..."
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
Write-Host "=== Phase 4F v3 v2 complete ==="
Write-Host "Production sources restored byte-for-byte."
Write-Host "Production Phase 3P rebuilt and reflashed."
Write-Host "Temporary flash-AOT source remains only under build-pico\phase4f."
Write-Host "Send back the log containing [aot-hot] lines."
Write-Host ""

if ($BenchExit -ne 0) { exit $BenchExit }
