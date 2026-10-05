param(
    [int]$CaptureSeconds = 300,
    [int]$SliceInstructions = 10000,
    [ValidateSet(16,24,32,48)]
    [int]$HotBlocks = 48,
    [ValidateRange(0,4)]
    [int]$ClosureDepth = 3
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
$TempDir = Join-Path $Repo "build-pico\phase4j"
$FlashAot = Join-Path $TempDir "msdos2_recomp_flash.c"

$RecoveryDir = Join-Path $TempDir "recovery"
$RecoveryCMake = Join-Path $RecoveryDir "CMakeLists.txt.production"
$RecoveryPico = Join-Path $RecoveryDir "microdos_pico.c.production"
$RecoveryNv2 = Join-Path $RecoveryDir "native_v2_runtime.c.production"

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

function Replace-RegexExactlyOnce([string]$Text, [string]$Pattern, [string]$New, [string]$What) {
    $matches = [regex]::Matches($Text, $Pattern)
    if ($matches.Count -ne 1) {
        throw "Regex marker for $What matched $($matches.Count) times; expected exactly one. No production source files have been written."
    }
    $m = $matches[0]
    return $Text.Substring(0, $m.Index) + $New + $Text.Substring($m.Index + $m.Length)
}

function Configure-Production {
    Write-Host "Configuring restored production tree..."
    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Production Pico reconfigure failed with exit code $LASTEXITCODE."
    }
}

# Validate the current tree BEFORE blessing it as "production" recovery state.
# If a previous crash left a torn experimental file behind, fail here without
# overwriting the last known recovery copies with damaged bytes.
# Normalize after previous experiments.
Configure-Production

# Only after the production tree successfully configures do we persist exact
# byte-for-byte recovery copies. These survive PowerShell termination/reboot/BSOD.
New-Item -ItemType Directory -Force -Path $RecoveryDir | Out-Null
[IO.File]::WriteAllBytes($RecoveryCMake, $CMakeBytes)
[IO.File]::WriteAllBytes($RecoveryPico, $PicoBytes)
[IO.File]::WriteAllBytes($RecoveryNv2, $Nv2Bytes)
Write-Host "Validated persistent production recovery copies:"
Write-Host "  $RecoveryDir"

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
 * Phase 4J diagnostic placement.
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

# -------------------------------------------------------------------------
# Phase 4J: profile-guided hot block split.
#
# The Phase 4F exact profiler found the same top 48 kernel blocks in all
# three DOS2TEST runs.  Together they retire about 70% of kernel AOT guest
# instructions while containing only 263 guest instructions of block bodies.
#
# Keep the complete generated md_body() in XIP as the correctness/reference
# body.  Duplicate only the selected hot blocks into an SRAM-resident helper.
# Cold -> hot edges are redirected through md_dispatch.  Hot -> hot edges
# stay direct inside the SRAM helper.  Hot -> cold edges return the exact
# remaining/done/write-epoch state to the XIP body.
# -------------------------------------------------------------------------

$HotRanked = @(
    "0694","1019","0680","0FF6","37DD","104C","068E","1129",
    "1081","1074","1FAD","383B","113A","105A","1032","19AD",
    "0A3F","37F1","1039","19B6","11B4","332B","108D","37F9",
    "1092","3342","2507","1FD6","05B8","3836","200F","37F3",
    "37C8","37CD","37D2","3800","0A30","1040","0A68","33F7",
    "19BD","247C","1146","2DA1","05F0","1016","1126","2DEE"
)
$HotSeedIps = @($HotRanked | Select-Object -First $HotBlocks)

function Get-AotBlockText([string]$Text, [string]$Ip) {
    $pattern = "(?ms)^md_block_${Ip}:\r?\n.*?(?=^md_block_[0-9A-Fa-f]{4}:|^#undef MD_AOT_FLUSH)"
    $m = [regex]::Match($Text, $pattern)
    if (-not $m.Success) {
        throw "Could not extract generated AOT block md_block_$Ip."
    }
    return $m.Value.TrimEnd()
}

# Save the annotated-but-not-yet-rewritten body as our source for the SRAM
# duplicates and for graph expansion.
$AotForHotExtraction = $AotFlash

# Expand the profiled seed set through direct generated-block control-flow
# edges.  This keeps adjacent successors with the hot blocks and reduces
# SRAM<->XIP handoffs without copying arbitrary cold kernel code.
$HotSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($ip in $HotSeedIps) { [void]$HotSet.Add($ip) }

$Frontier = @($HotSeedIps)
$ClosureAddedPerDepth = New-Object System.Collections.Generic.List[int]

for ($depth = 1; $depth -le $ClosureDepth; ++$depth) {
    $NextSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    foreach ($ip in $Frontier) {
        $block = Get-AotBlockText $AotForHotExtraction $ip
        foreach ($m in [regex]::Matches($block, 'goto md_block_([0-9A-Fa-f]{4});')) {
            $target = $m.Groups[1].Value.ToUpperInvariant()
            if ($HotSet.Add($target)) {
                [void]$NextSet.Add($target)
            }
        }
    }

    $ClosureAddedPerDepth.Add($NextSet.Count)
    $Frontier = @($NextSet)

    if ($Frontier.Count -eq 0) {
        break
    }
}

$HotIps = @(
    $HotSet |
    Sort-Object { [Convert]::ToInt32($_, 16) }
)
$ColdTargets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$HotColdEdgeKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$HotColdEdges = New-Object System.Collections.Generic.List[object]
$HotBlockCopies = New-Object System.Collections.Generic.List[string]

foreach ($ip in $HotIps) {
    $block = Get-AotBlockText $AotForHotExtraction $ip
    $sourceIp = $ip

    # Instrument exact hot->cold edges. Hot->hot edges remain direct.
    $block = [regex]::Replace(
        $block,
        'goto md_block_([0-9A-Fa-f]{4});',
        {
            param($m)
            $target = $m.Groups[1].Value.ToUpperInvariant()
            if ($HotSet.Contains($target)) {
                return "goto md_block_$target;"
            }

            [void]$ColdTargets.Add($target)
            $edgeKey = "${sourceIp}_${target}"
            if ($HotColdEdgeKeys.Add($edgeKey)) {
                $HotColdEdges.Add([pscustomobject]@{
                    Source = $sourceIp
                    Target = $target
                    Key    = $edgeKey
                })
            }
            return "goto md_hot_to_cold_${sourceIp}_${target};"
        }
    )

    $HotBlockCopies.Add($block)
}

$HotEntryCases = ($HotIps | ForEach-Object {
    "        case 0x{0}u: return 1;" -f $_
}) -join "`n"

$HotGotoCases = ($HotIps | ForEach-Object {
    "        case 0x{0}u: goto md_block_{0};" -f $_
}) -join "`n"

$ColdTargetList = @($ColdTargets | Sort-Object)

$HotColdEdgeList = @(
    $HotColdEdges |
    Sort-Object `
        @{ Expression = { [Convert]::ToInt32($_.Source, 16) } }, `
        @{ Expression = { [Convert]::ToInt32($_.Target, 16) } }
)

$EdgeCount = $HotColdEdgeList.Count
if ($EdgeCount -le 0) {
    throw "Phase 4J edge profiler found no hot->cold edges at closure depth $ClosureDepth."
}

$EdgeSrcInit = ($HotColdEdgeList | ForEach-Object { "0x$($_.Source)u" }) -join ", "
$EdgeDstInit = ($HotColdEdgeList | ForEach-Object { "0x$($_.Target)u" }) -join ", "

$HotColdRedirectsList = New-Object System.Collections.Generic.List[string]
for ($edgeIndex = 0; $edgeIndex -lt $HotColdEdgeList.Count; ++$edgeIndex) {
    $edge = $HotColdEdgeList[$edgeIndex]
    $HotColdRedirectsList.Add(@"
md_hot_to_cold_$($edge.Source)_$($edge.Target):
    ++md_hot_edge_hits[$edgeIndex];
    cpu->ip = 0x$($edge.Target)u;
    goto md_dispatch;
"@)
}
$HotColdRedirects = $HotColdRedirectsList -join "`n"

$HotBlocksText = $HotBlockCopies -join "`n`n"

$HotPrototypes = @"
static uint32_t md_hot_edge_hits[$EdgeCount];
static const uint16_t md_hot_edge_src[$EdgeCount] = { $EdgeSrcInit };
static const uint16_t md_hot_edge_dst[$EdgeCount] = { $EdgeDstInit };

/*
 * Phase 4J profiler for the control-flow class Phase 4I could not see:
 * a hot copied block writes cpu->ip and jumps to md_dispatch.  If that IP is
 * not in the SRAM hot set, md_hot_body() returns through md_hot_out to the
 * original XIP dispatcher.  Profile the cold target IP at that boundary.
 *
 * 128 open-addressed slots are deliberately small (~1 KiB) and are reset at
 * each Ctrl+] boundary by the Pico reporting hook.
 */
static uint16_t md_hot_out_ip[128];
static uint32_t md_hot_out_hits[128];
static uint32_t md_hot_out_total;
static uint32_t md_hot_out_overflow;

unsigned md_aot_hot_edge_count(void);
uint16_t md_aot_hot_edge_source(unsigned index);
uint16_t md_aot_hot_edge_target(unsigned index);
uint32_t md_aot_hot_edge_hits(unsigned index);
void md_aot_hot_edge_reset(void);

unsigned md_aot_hot_out_slot_count(void);
uint16_t md_aot_hot_out_ip(unsigned index);
uint32_t md_aot_hot_out_hits(unsigned index);
uint32_t md_aot_hot_out_total(void);
uint32_t md_aot_hot_out_overflow(void);
void md_aot_hot_out_reset(void);

static int md_hot_is_entry(uint16_t ip);
static MdStopReason md_hot_body(MdRuntime *runtime,
                                uint16_t segment,
                                MdAotGuard *guard,
                                uint32_t *remaining_io,
                                uint32_t *done_io,
                                uint32_t *wep_io,
                                int *fallback_out);

"@

$annotatedBodyMarker = "static MD_AOT_XIP_TEXT MdStopReason md_body("
$AotFlash = Replace-ExactlyOnce `
    $AotFlash `
    $annotatedBodyMarker `
    ($HotPrototypes + $annotatedBodyMarker) `
    "hot-body forward declarations"

# In the original XIP md_body, turn every direct edge to one of the selected
# labels into a redirect label.  This is deliberately a single goto statement,
# so generated one-line conditionals retain their exact semantics.
foreach ($ip in $HotIps) {
    $AotFlash = $AotFlash.Replace(
        "goto md_block_$ip;",
        "goto md_sram_redirect_$ip;"
    )
}

$ColdToHotRedirects = ($HotIps | ForEach-Object {
@"
md_sram_redirect_$($_):
    cpu->ip = 0x$($_)u;
    goto md_dispatch;
"@
}) -join "`n"

$AotFlash = Replace-ExactlyOnce `
    $AotFlash `
    "md_fallback:" `
    ($ColdToHotRedirects + "`nmd_fallback:") `
    "cold-to-hot redirect labels"

# Route hot IPs to the SRAM helper before the original generated entry table.
$dispatchPattern = 'wep\s*=\s*guard->epoch;\s*switch\s*\(md_find\(cpu->ip\)\)\s*\{'
$dispatchReplacement = @'
    wep = guard->epoch;
    if (md_hot_is_entry(cpu->ip)) {
        int hot_fallback_ = 0;
        const MdStopReason hot_stop_ =
            md_hot_body(runtime, segment, guard,
                        &remaining, &done, &wep, &hot_fallback_);
        if (hot_stop_ != MD_STOP_NONE) return hot_stop_;
        if (hot_fallback_) goto md_fallback;
        goto md_dispatch;
    }
    switch (md_find(cpu->ip)) {
'@
$AotFlash = Replace-RegexExactlyOnce `
    $AotFlash `
    $dispatchPattern `
    $dispatchReplacement `
    "XIP md_dispatch hot routing"

$HotHelper = @"
static int md_hot_is_entry(uint16_t ip)
{
    switch (ip) {
$HotEntryCases
        default: return 0;
    }
}

static void md_hot_out_profile_hit(uint16_t ip)
{
    unsigned probe;
    unsigned slot = (((unsigned)ip) ^ (((unsigned)ip) >> 7)) & 127u;

    ++md_hot_out_total;

    for (probe = 0u; probe < 128u; ++probe) {
        const unsigned index = (slot + probe) & 127u;

        if (md_hot_out_hits[index] == 0u) {
            md_hot_out_ip[index] = ip;
            md_hot_out_hits[index] = 1u;
            return;
        }

        if (md_hot_out_ip[index] == ip) {
            ++md_hot_out_hits[index];
            return;
        }
    }

    ++md_hot_out_overflow;
}

static MdStopReason md_hot_body(MdRuntime *runtime,
                                uint16_t segment,
                                MdAotGuard *guard,
                                uint32_t *remaining_io,
                                uint32_t *done_io,
                                uint32_t *wep_io,
                                int *fallback_out)
{
    MdX86 *cpu = &runtime->cpu;
    uint32_t remaining = *remaining_io;
    uint32_t done = *done_io;
    uint32_t wep = *wep_io;

    *fallback_out = 0;

md_dispatch:
    if (runtime->stop_reason != MD_STOP_NONE) {
        MD_AOT_FLUSH();
        *remaining_io = remaining;
        *done_io = done;
        *wep_io = wep;
        return runtime->stop_reason;
    }
    if (!guard->valid || cpu->cs != segment)
        goto md_fallback;

    wep = guard->epoch;
    switch (cpu->ip) {
$HotGotoCases
        default: goto md_hot_out;
    }

$HotBlocksText

$HotColdRedirects

md_hot_out:
    md_hot_out_profile_hit(cpu->ip);
    *remaining_io = remaining;
    *done_io = done;
    *wep_io = wep;
    return MD_STOP_NONE;

md_fallback:
    *remaining_io = remaining;
    *done_io = done;
    *wep_io = wep;
    *fallback_out = 1;
    return MD_STOP_NONE;
}

unsigned md_aot_hot_edge_count(void)
{
    return ${EdgeCount}u;
}

uint16_t md_aot_hot_edge_source(unsigned index)
{
    return index < ${EdgeCount}u ? md_hot_edge_src[index] : 0u;
}

uint16_t md_aot_hot_edge_target(unsigned index)
{
    return index < ${EdgeCount}u ? md_hot_edge_dst[index] : 0u;
}

uint32_t md_aot_hot_edge_hits(unsigned index)
{
    return index < ${EdgeCount}u ? md_hot_edge_hits[index] : 0u;
}

void md_aot_hot_edge_reset(void)
{
    unsigned i;
    for (i = 0u; i < ${EdgeCount}u; ++i)
        md_hot_edge_hits[i] = 0u;
}

unsigned md_aot_hot_out_slot_count(void)
{
    return 128u;
}

uint16_t md_aot_hot_out_ip(unsigned index)
{
    return index < 128u ? md_hot_out_ip[index] : 0u;
}

uint32_t md_aot_hot_out_hits(unsigned index)
{
    return index < 128u ? md_hot_out_hits[index] : 0u;
}

uint32_t md_aot_hot_out_total(void)
{
    return md_hot_out_total;
}

uint32_t md_aot_hot_out_overflow(void)
{
    return md_hot_out_overflow;
}

void md_aot_hot_out_reset(void)
{
    unsigned i;

    for (i = 0u; i < 128u; ++i) {
        md_hot_out_ip[i] = 0u;
        md_hot_out_hits[i] = 0u;
    }

    md_hot_out_total = 0u;
    md_hot_out_overflow = 0u;
}

"@

# dosrecomp emits the macro undefs immediately BEFORE md_body()'s closing
# brace.  Move that brace ahead of the undefs, insert md_hot_body() as a
# separate function while the generated macros are still defined, then emit
# the original undef sequence.  This gives the SRAM body the exact same AOT
# accounting/store/chunk macros without creating a nested C function.
$bodyTailPattern = '#undef\s+MD_AOT_FLUSH\s*#undef\s+MD_AOT_BLOCK\s*#undef\s+MD_AOT_WCHK\s*#undef\s+MD_AOT_WCHK_T\s*#undef\s+MD_AOT_HOLE\s*#undef\s+MD_AOT_STOPCHK\s*\}'
$bodyTailReplacement = @"
}

$HotHelper
#undef MD_AOT_FLUSH
#undef MD_AOT_BLOCK
#undef MD_AOT_WCHK
#undef MD_AOT_WCHK_T
#undef MD_AOT_HOLE
#undef MD_AOT_STOPCHK
"@
$AotFlash = Replace-RegexExactlyOnce `
    $AotFlash `
    $bodyTailPattern `
    $bodyTailReplacement `
    "SRAM hot-body insertion"

New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
[IO.File]::WriteAllText($FlashAot, $AotFlash, $Utf8NoBom)
Write-Host "Created temporary split-placement AOT source:"
Write-Host "  $FlashAot"
Write-Host "  functions marked for XIP flash: $($fnMatches.Count)"
Write-Host "  profiled seed blocks: $HotBlocks"
Write-Host "  CFG closure depth: $ClosureDepth"
Write-Host "  CFG additions by depth: $($ClosureAddedPerDepth -join ', ')"
Write-Host "  final SRAM hot blocks: $($HotIps.Count)"
Write-Host "  remaining hot->cold redirect targets: $($ColdTargetList.Count)"
Write-Host "  instrumented hot->cold edges: $EdgeCount"`nWrite-Host "  indirect md_hot_out target profiler slots: 128"

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

$PicoStatsMarker = 'static void md_stats(uint64_t start_us)'
$PicoEdgeDecl = @'
unsigned md_aot_hot_edge_count(void);
uint16_t md_aot_hot_edge_source(unsigned index);
uint16_t md_aot_hot_edge_target(unsigned index);
uint32_t md_aot_hot_edge_hits(unsigned index);
void md_aot_hot_edge_reset(void);

unsigned md_aot_hot_out_slot_count(void);
uint16_t md_aot_hot_out_ip(unsigned index);
uint32_t md_aot_hot_out_hits(unsigned index);
uint32_t md_aot_hot_out_total(void);
uint32_t md_aot_hot_out_overflow(void);
void md_aot_hot_out_reset(void);

static void md_aot_hot_flow_report(void)
{
    unsigned i;
    unsigned edge_count = md_aot_hot_edge_count();
    unsigned out_slots = md_aot_hot_out_slot_count();
    uint64_t edge_total = 0u;

    for (i = 0u; i < edge_count; ++i)
        edge_total += (uint64_t)md_aot_hot_edge_hits(i);

    md_say("[aot-edge] total-crossings=%llu edges=%u\n",
           (unsigned long long)edge_total, edge_count);

    for (i = 0u; i < edge_count; ++i) {
        uint32_t hits = md_aot_hot_edge_hits(i);
        if (hits == 0u)
            continue;
        md_say("[aot-edge] %04X -> %04X hits=%lu\n",
               md_aot_hot_edge_source(i),
               md_aot_hot_edge_target(i),
               (unsigned long)hits);
    }

    md_say("[aot-dispatch] total-exits=%lu overflow=%lu slots=%u\n",
           (unsigned long)md_aot_hot_out_total(),
           (unsigned long)md_aot_hot_out_overflow(),
           out_slots);

    for (i = 0u; i < out_slots; ++i) {
        uint32_t hits = md_aot_hot_out_hits(i);
        if (hits == 0u)
            continue;
        md_say("[aot-dispatch] target=%04X hits=%lu\n",
               md_aot_hot_out_ip(i),
               (unsigned long)hits);
    }

    md_aot_hot_edge_reset();
    md_aot_hot_out_reset();
}

'@

$PicoHybrid = Replace-ExactlyOnce `
    $PicoHybrid `
    $PicoStatsMarker `
    ($PicoEdgeDecl + $PicoStatsMarker) `
    "Pico hot-flow profiler declarations"

$PerfMarkMarker = '    g_perf_mark=now;'
$PicoHybrid = Replace-ExactlyOnce `
    $PicoHybrid `
    $PerfMarkMarker `
    ("    md_aot_hot_flow_report();`r`n" + $PerfMarkMarker) `
    "Pico hot-flow report hook"

$HybridBlock = @"

# ---- TEMPORARY Phase 4J: indirect hot-dispatch target profiler ----------------
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
endif()
# ---- END TEMPORARY Phase 4J ------------------------------------------------
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

# Structural preflight on the generated temporary AOT source.  Fail before
# touching production source if any split-body landmark is missing/duplicated.
$PreflightChecks = @(
    # Prototype + definition are both expected for these two functions.
    @{ Name = "hot helper declarations";   Pattern = 'static MdStopReason md_hot_body\('; Expected = 2 },
    @{ Name = "hot helper definition";     Pattern = 'static MdStopReason md_hot_body\([^;]*\)\s*\{'; Expected = 1 },
    @{ Name = "entry selector declarations"; Pattern = 'static int md_hot_is_entry\('; Expected = 2 },
    @{ Name = "entry selector definition"; Pattern = 'static int md_hot_is_entry\(uint16_t ip\)\s*\{'; Expected = 1 },

    @{ Name = "cold dispatcher";            Pattern = 'switch \(md_find\(cpu->ip\)\)'; Expected = 1 },
    @{ Name = "hot dispatcher";             Pattern = 'switch \(cpu->ip\)'; Expected = 1 },
    @{ Name = "hot redirect 0694";          Pattern = 'md_sram_redirect_0694:'; Expected = 1 },

    # Original XIP label + duplicated SRAM label.
    @{ Name = "hot block 0694 copies";      Pattern = 'md_block_0694:'; Expected = 2 },

    # Membership switch must return true, not jump across function scope.
    @{ Name = "entry case 0694";            Pattern = 'case 0x0694u:\s*return 1;'; Expected = 1 },

    # Hot dispatcher must jump to the SRAM-local duplicated block.
    @{ Name = "dispatcher case 0694";       Pattern = 'case 0x0694u:\s*goto md_block_0694;'; Expected = 1 },
    @{ Name = "edge count accessor";          Pattern = 'unsigned md_aot_hot_edge_count\(void\)\s*\{'; Expected = 1 },
    @{ Name = "edge reset accessor";          Pattern = 'void md_aot_hot_edge_reset\(void\)\s*\{'; Expected = 1 },
    @{ Name = "edge hit counters";            Pattern = 'static uint32_t md_hot_edge_hits\['; Expected = 1 },
    @{ Name = "hot-out profile helper";         Pattern = 'static void md_hot_out_profile_hit\(uint16_t ip\)\s*\{'; Expected = 1 },
    @{ Name = "hot-out instrumentation";        Pattern = 'md_hot_out:\s*md_hot_out_profile_hit\(cpu->ip\);'; Expected = 1 },
    @{ Name = "hot-out total accessor";         Pattern = 'uint32_t md_aot_hot_out_total\(void\)\s*\{'; Expected = 1 },
    @{ Name = "hot-out reset accessor";         Pattern = 'void md_aot_hot_out_reset\(void\)\s*\{'; Expected = 1 },

    @{ Name = "macro undef tail"; Pattern = '#undef MD_AOT_FLUSH\s*#undef MD_AOT_BLOCK\s*#undef MD_AOT_WCHK\s*#undef MD_AOT_WCHK_T\s*#undef MD_AOT_HOLE\s*#undef MD_AOT_STOPCHK'; Expected = 1 }
)

foreach ($check in $PreflightChecks) {
    $count = [regex]::Matches(
        $AotFlash,
        $check.Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Singleline
    ).Count

    if ($count -ne $check.Expected) {
        throw "Phase 4J preflight '$($check.Name)' found $count occurrences; expected $($check.Expected)."
    }
}

foreach ($edge in $HotColdEdgeList) {
    $edgeLabel = "md_hot_to_cold_$($edge.Source)_$($edge.Target):"
    if ([regex]::Matches($AotFlash, [regex]::Escape($edgeLabel)).Count -ne 1) {
        throw "Phase 4J preflight missing/duplicated edge redirect $($edge.Source)->$($edge.Target)."
    }
}

foreach ($ip in $HotIps) {
    $labelCount = [regex]::Matches($AotFlash, "md_block_${ip}:").Count
    if ($labelCount -ne 2) {
        throw "Phase 4J preflight hot block $ip has $labelCount labels; expected XIP+SRAM copies."
    }
    $casePattern = "case 0x${ip}u:\s*return 1;"
    if ([regex]::Matches($AotFlash, $casePattern).Count -ne 1) {
        throw "Phase 4J preflight hot-entry case missing/duplicated for $ip."
    }
}

$hotDef = [regex]::Match(
    $AotFlash,
    'static MdStopReason md_hot_body\([^;]*\)\s*\{',
    [System.Text.RegularExpressions.RegexOptions]::Singleline
)
$undefPos = $AotFlash.IndexOf("#undef MD_AOT_FLUSH", [StringComparison]::Ordinal)

if (-not $hotDef.Success -or $undefPos -lt 0 -or $hotDef.Index -gt $undefPos) {
    throw "Phase 4J preflight ordering failed: hot helper definition must precede macro undef tail."
}

Write-Host "Phase 4J generated-source preflight: PASS"

$HybridBuilt = $false
$BenchExit = 0

try {
    [IO.File]::WriteAllText($CMake, $CMakeHybrid, $Utf8NoBom)
    [IO.File]::WriteAllText($PicoMain, $PicoHybrid, $Utf8NoBom)
    [IO.File]::WriteAllText($Nv2Runtime, $Nv2Hybrid, $Utf8NoBom)

    Write-Host ""
    Write-Host "=== Phase 4J v1 indirect hot-dispatch target profiler A/B ==="
    Write-Host "Temporary scheduler slice: $SliceInstructions"
    Write-Host "Kernel AOT mode: FAST / non-compact"
    Write-Host "Kernel AOT cold body/tables: XIP flash"
    Write-Host "Kernel AOT hot body seed: top $HotBlocks profiled blocks"
    Write-Host "Kernel AOT hot CFG closure depth: $ClosureDepth"
    Write-Host "Kernel AOT final SRAM block count: $($HotIps.Count)"
    Write-Host "Instrumented hot->cold edges: $EdgeCount"`n    Write-Host "Indirect md_hot_out target profiler slots: 128"
    Write-Host "Profiler note: MIPS includes profiling overhead; use [aot-edge] and [aot-dispatch] hit counts for placement decisions."
    Write-Host "Interpreter + Native-v2: production copy_to_ram SRAM"
    Write-Host "AOT metadata: 1 attachment slot, 5 live-page bitmaps"
    Write-Host "Phase 4C store coexistence: retained"
    Write-Host ""

    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Temporary Pico reconfigure failed with exit code $LASTEXITCODE."
    }

    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Phase 4J indirect-dispatch profiler firmware build failed with exit code $LASTEXITCODE."
    }

    if (-not (Test-Path $Uf2)) {
        throw "Phase 4J build completed but UF2 was not found: $Uf2"
    }

    $HybridBuilt = $true
}
finally {
    [IO.File]::WriteAllBytes($CMake, [IO.File]::ReadAllBytes($RecoveryCMake))
    [IO.File]::WriteAllBytes($PicoMain, [IO.File]::ReadAllBytes($RecoveryPico))
    [IO.File]::WriteAllBytes($Nv2Runtime, [IO.File]::ReadAllBytes($RecoveryNv2))
    Write-Host "Restored CMakeLists.txt, microdos_pico.c, and native_v2_runtime.c byte-for-byte from persistent recovery copies."

    if (-not $HybridBuilt) {
        Write-Host "Phase 4J did not build; restoring production build state..."
        & cmake -S .\pico -B .\build-pico\out
        if ($LASTEXITCODE -eq 0) {
            & cmake --build .\build-pico\out --target microdos_pico_nativev2
        }
    }
}

if (-not $HybridBuilt) { exit 1 }

Write-Host ""
Write-Host "Running completion-boundary workload on Phase 4J indirect-dispatch profiler firmware..."
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
Write-Host "=== Phase 4J v1 complete ==="
Write-Host "Production sources restored byte-for-byte."
Write-Host "Production Phase 3P rebuilt and reflashed."
Write-Host "Temporary CFG-split AOT source remains only under build-pico\phase4j."
Write-Host "Send back the full console/log output."
Write-Host ""

if ($BenchExit -ne 0) { exit $BenchExit }
