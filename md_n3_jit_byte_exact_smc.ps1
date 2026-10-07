param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"

function Read-Normalized([string]$Path) {
    return ([IO.File]::ReadAllText($Path) -replace "`r`n", "`n")
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $enc = [System.Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($Path, $Text, $enc)
}

function Replace-Once(
    [string]$Text,
    [string]$Old,
    [string]$New,
    [string]$Label
) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) {
        throw "anchor not found: $Label"
    }

    $second = $Text.IndexOf(
        $Old,
        $first + $Old.Length,
        [StringComparison]::Ordinal
    )

    if ($second -ge 0) {
        throw "anchor is not unique: $Label"
    }

    return $Text.Substring(0, $first) +
        $New +
        $Text.Substring($first + $Old.Length)
}

$JitH  = Join-Path $Repo "include\microdos\jit.h"
$JitC  = Join-Path $Repo "src\runtime\jit_core.c"
$PicoC = Join-Path $Repo "pico\microdos_pico.c"
$CMake = Join-Path $Repo "pico\CMakeLists.txt"

foreach ($p in @($JitH, $JitC, $PicoC, $CMake)) {
    if (-not (Test-Path $p)) {
        throw "missing file: $p"
    }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backup = Join-Path $Repo "native3_jit_byte_smc_backup\$stamp"
New-Item -ItemType Directory -Force -Path $backup | Out-Null

Copy-Item $JitH  (Join-Path $backup "jit.h") -Force
Copy-Item $JitC  (Join-Path $backup "jit_core.c") -Force
Copy-Item $PicoC (Join-Path $backup "microdos_pico.c") -Force
Copy-Item $CMake (Join-Path $backup "CMakeLists.txt") -Force

Write-Host "backup: $backup"

# ===========================================================================
# include/microdos/jit.h
# ===========================================================================

$h = Read-Normalized $JitH

$old = @'
#ifndef MD_JIT_PROFILE
#define MD_JIT_PROFILE 1
#endif
'@

$new = @'
#ifndef MD_JIT_PROFILE
#define MD_JIT_PROFILE 1
#endif

/*
 * Optional M25-style byte-exact SMC tracking for the runtime JIT.
 *
 * The historical JIT marks every 4 KiB page containing a translated block as
 * MD_X86_PAGE_TRANSLATED, so any data/stack store sharing that page bumps the
 * generation and invalidates JIT/N3 metadata. With this switch enabled, JIT
 * source bytes use MD_X86_PAGE_TRBYTES + a per-byte bitmap, just like the M25
 * translator. Pool exhaustion or coexistence with another bitmap owner falls
 * back conservatively to the old page-granular tracking.
 */
#ifndef MD_JIT_BYTE_EXACT_TRACKING
#define MD_JIT_BYTE_EXACT_TRACKING 0
#endif

#ifndef MD_JIT_LIVE_PAGES
#define MD_JIT_LIVE_PAGES 16u
#endif
'@

$h = Replace-Once $h $old $new "add JIT byte-exact tracking config"

$old = @'
#if MD_JIT_LEGACY_HOTNESS
    MdJitHotness hotness[MD_JIT_HOTNESS_SLOTS];
#endif
    uint8_t last_lookup_cold;
};
'@

$new = @'
#if MD_JIT_LEGACY_HOTNESS
    MdJitHotness hotness[MD_JIT_HOTNESS_SLOTS];
#endif

#if MD_JIT_BYTE_EXACT_TRACKING && MICRODOS_TRANSLATION_SUPPORT
    /*
     * One bit per guest byte for pages containing JIT source code.
     * cpu.tr_live_bits points at live_table while this JIT owns the byte-exact
     * tracker. 16 pages cost 8 KiB and cover the normal DOS/JIT working set.
     */
    uint8_t *live_table[MD_X86_CODE_PAGE_COUNT];
    uint8_t live_pool[MD_JIT_LIVE_PAGES][MD_X86_CODE_PAGE_SIZE / 8u];
    uint16_t live_used;
    uint16_t live_fallback_pages;
#endif

    uint8_t last_lookup_cold;
};
'@

$h = Replace-Once $h $old $new "add JIT live bitmap storage"

Write-Utf8NoBom $JitH $h

# ===========================================================================
# src/runtime/jit_core.c
# ===========================================================================

$c = Read-Normalized $JitC

$anchor = @'
static int md_jit_block_current(const MdJitBlock *block, const MdRuntime *runtime)
'@

$tracking = @'
#if MD_JIT_BYTE_EXACT_TRACKING && MICRODOS_TRANSLATION_SUPPORT

/*
 * Bind this JIT as the byte-exact translated-code bitmap owner when no other
 * translator already owns cpu.tr_live_bits. Native-3 does not instantiate the
 * M25 translator, so its normal profile path takes this branch.
 */
static int md_jit_bind_live(MdJit *jit, MdRuntime *runtime)
{
    if (runtime->cpu.tr_live_bits == NULL)
        runtime->cpu.tr_live_bits = jit->live_table;

    return runtime->cpu.tr_live_bits == jit->live_table;
}

/*
 * Mark only the guest bytes actually decoded into this JIT block.
 *
 * A page already carrying MD_X86_PAGE_TRANSLATED remains page-granular:
 * another cache/explicit code-range marker may depend on that semantics.
 * Likewise, if the bitmap pool is exhausted, fall back to the historical
 * page-granular mark. Both cases remain fully conservative.
 */
static void md_jit_mark_code_range(MdJit *jit,
                                   MdRuntime *runtime,
                                   uint16_t cs,
                                   uint16_t ip,
                                   size_t len)
{
    size_t i;

    if (!md_jit_bind_live(jit, runtime)) {
        md_runtime_mark_code_range(runtime, cs, ip, len);
        return;
    }

    for (i = 0u; i < len; ++i) {
        const uint32_t a =
            md_x86_linear(cs, (uint16_t)(ip + i)) & MD_X86_ADDRESS_MASK;
        const unsigned page = md_x86_code_page(a);
        uint8_t *bm = jit->live_table[page];

        if (bm == NULL) {
            /*
             * Never try to convert a page that is already conservatively
             * tracked by another subsystem. We do not know that subsystem's
             * exact live-byte set.
             */
            if ((runtime->code_page_executable[page] &
                 MD_X86_PAGE_TRANSLATED) != 0u) {
                continue;
            }

            if (jit->live_used >= MD_JIT_LIVE_PAGES) {
                if ((runtime->code_page_executable[page] &
                     MD_X86_PAGE_TRANSLATED) == 0u) {
                    runtime->code_page_executable[page] |=
                        MD_X86_PAGE_TRANSLATED;
                    ++jit->live_fallback_pages;
                }
                continue;
            }

            bm = jit->live_pool[jit->live_used++];
            memset(bm, 0, sizeof(jit->live_pool[0]));
            jit->live_table[page] = bm;
            runtime->code_page_executable[page] |= MD_X86_PAGE_TRBYTES;
        }

        bm[(a & MD_X86_CODE_PAGE_MASK) >> 3] |=
            (uint8_t)(1u << (a & 7u));
    }
}

#else

static void md_jit_mark_code_range(MdJit *jit,
                                   MdRuntime *runtime,
                                   uint16_t cs,
                                   uint16_t ip,
                                   size_t len)
{
    (void)jit;
    md_runtime_mark_code_range(runtime, cs, ip, len);
}

#endif

static int md_jit_block_current(const MdJitBlock *block, const MdRuntime *runtime)
'@

$c = Replace-Once $c $anchor $tracking "insert JIT byte-exact tracker"

$old = @'
    if (!md_jit_decode_block(jit, runtime, cs, start_ip, block, NULL)) return NULL;
    md_runtime_mark_code_range(runtime, cs, start_ip, block->source_bytes);
    block->code_epoch = runtime->code_epoch;
'@

$new = @'
    if (!md_jit_decode_block(jit, runtime, cs, start_ip, block, NULL)) return NULL;
    md_jit_mark_code_range(jit, runtime, cs, start_ip, block->source_bytes);
    block->code_epoch = runtime->code_epoch;
'@

$c = Replace-Once $c $old $new "use exact JIT code marking"

# If the first emission flushes and re-decodes, make sure the retried source
# range is also represented. Usually it is identical and this is idempotent.
$old = @'
        block = &jit->blocks[md_jit_hash(cs, start_ip)];
        if (!md_jit_decode_block(jit, runtime, cs, start_ip, block, NULL)) return NULL;
        block->code_epoch = runtime->code_epoch;
'@

$new = @'
        block = &jit->blocks[md_jit_hash(cs, start_ip)];
        if (!md_jit_decode_block(jit, runtime, cs, start_ip, block, NULL)) return NULL;
        md_jit_mark_code_range(jit, runtime, cs, start_ip, block->source_bytes);
        block->code_epoch = runtime->code_epoch;
'@

$c = Replace-Once $c $old $new "mark retried JIT source"

Write-Utf8NoBom $JitC $c

# ===========================================================================
# pico/CMakeLists.txt
# Enable the experiment only for the instrumented Native-3 DOS profile.
# Keep the proven 256 N3 / 32 NV2 capacities.
# ===========================================================================

$cm = Read-Normalized $CMake

$pattern = '(?ms)(target_compile_definitions\(microdos_pico_native3_profile PRIVATE\s+MICRODOS_ENABLE_NATIVE3=1\s+MD_N3_PROFILE=1\s+MD_JIT_PROFILE=1\s+MD_JIT_LEGACY_HOTNESS=0\s+MD_JIT_BLOCK_SLOTS=256\s+)(?:MD_N3_SITE_SLOTS=\d+\s+)?(?:MD_NATIVE_V2_RT_SLOTS=\d+\s+)?(?:MD_JIT_BYTE_EXACT_TRACKING=\d+\s+)?(?:MD_JIT_LIVE_PAGES=\d+\s+)?(MD_X86_TRACK_WRITES=1\s+"MICRODOS_NATIVE3_CODE_BYTES=\(128u\*1024u\)"\))'

if ($cm -notmatch $pattern) {
    throw "Native-3 profile CMake target anchor not found. No CMake change made."
}

$replacement =
    '${1}' +
    "MD_N3_SITE_SLOTS=256`n    " +
    "MD_NATIVE_V2_RT_SLOTS=32`n    " +
    "MD_JIT_BYTE_EXACT_TRACKING=1`n    " +
    "MD_JIT_LIVE_PAGES=16`n    " +
    '${2}'

$cm = [regex]::Replace($cm, $pattern, $replacement, 1)
Write-Utf8NoBom $CMake $cm

# ===========================================================================
# pico/microdos_pico.c
# Add JIT exact-SMC telemetry immediately after native3-inv.
# ===========================================================================

$p = Read-Normalized $PicoC

$anchor = @'
        md_say("[native3-nv2] retired=%llu entries=%llu lookups=%llu "
'@

$telemetry = @'
#if MD_JIT_BYTE_EXACT_TRACKING && MICRODOS_TRANSLATION_SUPPORT
        md_say("[native3-jit-smc] exact-pages=%u fallback-pages=%u\n",
               (unsigned)g_sys.native3.jit.live_used,
               (unsigned)g_sys.native3.jit.live_fallback_pages);
#endif

        md_say("[native3-nv2] retired=%llu entries=%llu lookups=%llu "
'@

$p = Replace-Once $p $anchor $telemetry "add JIT SMC profile line"
Write-Utf8NoBom $PicoC $p

Write-Host ""
Write-Host "Applied Native-3 JIT byte-exact SMC experiment."
Write-Host "Profile configuration:"
Write-Host "  N3 sites:       256"
Write-Host "  NV2 slots:      32"
Write-Host "  JIT exact SMC:  ON"
Write-Host "  JIT live pages: 16 (8 KiB bitmap pool)"
Write-Host ""
Write-Host "Changed:"
Write-Host "  include\microdos\jit.h"
Write-Host "  src\runtime\jit_core.c"
Write-Host "  pico\CMakeLists.txt"
Write-Host "  pico\microdos_pico.c"
Write-Host ""
Write-Host "Run:"
Write-Host '  .\md.bat build host'
Write-Host '  ctest --test-dir .\build-host -C Release --output-on-failure'
Write-Host '  .\scripts\md_native3_dos_profile.ps1'
