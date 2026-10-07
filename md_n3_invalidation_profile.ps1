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

$Header = Join-Path $Repo "include\microdos\native3.h"
$Source = Join-Path $Repo "src\runtime\native3.c"
$PicoC  = Join-Path $Repo "pico\microdos_pico.c"
$CMake  = Join-Path $Repo "pico\CMakeLists.txt"

foreach ($p in @($Header, $Source, $PicoC, $CMake)) {
    if (-not (Test-Path $p)) {
        throw "missing file: $p"
    }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backup = Join-Path $Repo "native3_invalidation_profile_backup\$stamp"
New-Item -ItemType Directory -Force -Path $backup | Out-Null

Copy-Item $Header (Join-Path $backup "native3.h") -Force
Copy-Item $Source (Join-Path $backup "native3.c") -Force
Copy-Item $PicoC  (Join-Path $backup "microdos_pico.c") -Force
Copy-Item $CMake  (Join-Path $backup "CMakeLists.txt") -Force

Write-Host "backup: $backup"

# ===========================================================================
# Restore the profile build to the proven capacity baseline:
#   N3 sites = 256
#   NV2 runtime slots = 32
# ===========================================================================

$c = Read-Normalized $CMake

$pattern = '(?ms)(target_compile_definitions\(microdos_pico_native3_profile PRIVATE\s+MICRODOS_ENABLE_NATIVE3=1\s+MD_N3_PROFILE=1\s+MD_JIT_PROFILE=1\s+MD_JIT_LEGACY_HOTNESS=0\s+MD_JIT_BLOCK_SLOTS=256\s+)(?:MD_N3_SITE_SLOTS=\d+\s+)?(?:MD_NATIVE_V2_RT_SLOTS=\d+\s+)?(MD_X86_TRACK_WRITES=1\s+"MICRODOS_NATIVE3_CODE_BYTES=\(128u\*1024u\)"\))'

if ($c -notmatch $pattern) {
    throw "Native-3 profile target anchor not found. No files changed."
}

$replacement =
    '${1}' +
    "MD_N3_SITE_SLOTS=256`n    " +
    "MD_NATIVE_V2_RT_SLOTS=32`n    " +
    '${2}'

$c = [regex]::Replace($c, $pattern, $replacement, 1)
Write-Utf8NoBom $CMake $c

# ===========================================================================
# include/microdos/native3.h
# ===========================================================================

$h = Read-Normalized $Header

$old = @'
    uint64_t cache_imported, cache_exported;
    uint64_t smc_rejects, budget_rejects;
'@

$new = @'
    uint64_t cache_imported, cache_exported;
    uint64_t smc_rejects, budget_rejects;

    /*
     * Native-3 site-cache diagnostics.
     *
     * cold_misses, site_collisions and invalidations are mutually exclusive
     * reasons for an n3_site() miss.  The invalid_* counters further split
     * freshness failures by which tracked 4 KiB guest code page changed.
     *
     * epoch_resets counts whole-engine code_epoch changes after initial
     * attachment; normal self-modifying writes should normally appear in
     * page-generation invalidation counters instead.
     */
    uint64_t cold_misses, site_collisions;
    uint64_t invalid_page0_only, invalid_page1_only, invalid_both_pages;
    uint64_t epoch_resets;
'@

$h = Replace-Once $h $old $new "extend MdN3Stats invalidation diagnostics"
Write-Utf8NoBom $Header $h

# ===========================================================================
# src/runtime/native3.c
# ===========================================================================

$s = Read-Normalized $Source

$old = @'
static int n3_fresh(const MdN3Site *s, const MdRuntime *rt)
{
    if (s->state == MD_N3_SITE_EMPTY) return 0;
    if (s->page_count && rt->code_page_generation[s->page0] != s->gen0) return 0;
    if (s->page_count > 1u && rt->code_page_generation[s->page1] != s->gen1) return 0;
    return 1;
}

static MdN3Site *n3_site(MdNative3 *n3, MdRuntime *rt, uint16_t cs, uint16_t ip)
{
    MdN3Site *s = &n3->site[n3_hash(cs, ip)];
    N3STAT_INC(n3, lookups);
    if (s->state != MD_N3_SITE_EMPTY && s->cs == cs && s->ip == ip) {
        if (n3_fresh(s, rt)) { N3STAT_INC(n3, hits); return s; }
        memset(s, 0, sizeof(*s));
        N3STAT_INC(n3, invalidations);
    } else if (s->state != MD_N3_SITE_EMPTY) {
        memset(s, 0, sizeof(*s));
    }
    N3STAT_INC(n3, misses);
    s->cs = cs; s->ip = ip;
    s->signature = n3_signature(rt, cs, ip);
    n3_pages(s, rt, cs, ip);
    s->state = MD_N3_SITE_SEEN;
    return s;
}
'@

$new = @'
/*
 * Return a bit mask describing why a cached site is stale:
 *   bit 0 = first tracked 4 KiB guest code page changed
 *   bit 1 = second tracked guest code page changed
 *
 * n3_pages() currently tracks the 32-byte site window, so most sites occupy
 * one page and cross-page sites occupy exactly two.
 */
static unsigned n3_stale_mask(const MdN3Site *s, const MdRuntime *rt)
{
    unsigned mask = 0u;

    if (s->state == MD_N3_SITE_EMPTY)
        return 3u;

    if (s->page_count &&
        rt->code_page_generation[s->page0] != s->gen0)
        mask |= 1u;

    if (s->page_count > 1u &&
        rt->code_page_generation[s->page1] != s->gen1)
        mask |= 2u;

    return mask;
}

static int n3_fresh(const MdN3Site *s, const MdRuntime *rt)
{
    return s->state != MD_N3_SITE_EMPTY &&
           n3_stale_mask(s, rt) == 0u;
}

static MdN3Site *n3_site(MdNative3 *n3, MdRuntime *rt, uint16_t cs, uint16_t ip)
{
    MdN3Site *s = &n3->site[n3_hash(cs, ip)];

    N3STAT_INC(n3, lookups);

    if (s->state != MD_N3_SITE_EMPTY &&
        s->cs == cs &&
        s->ip == ip) {
        const unsigned stale = n3_stale_mask(s, rt);

        if (stale == 0u) {
            N3STAT_INC(n3, hits);
            return s;
        }

        if (stale == 1u)
            N3STAT_INC(n3, invalid_page0_only);
        else if (stale == 2u)
            N3STAT_INC(n3, invalid_page1_only);
        else
            N3STAT_INC(n3, invalid_both_pages);

        memset(s, 0, sizeof(*s));
        N3STAT_INC(n3, invalidations);
    } else if (s->state != MD_N3_SITE_EMPTY) {
        /*
         * Direct-map replacement of an unrelated CS:IP.  This is capacity /
         * hash collision churn, not guest self-modifying code.
         */
        N3STAT_INC(n3, site_collisions);
        memset(s, 0, sizeof(*s));
    } else {
        N3STAT_INC(n3, cold_misses);
    }

    N3STAT_INC(n3, misses);

    s->cs = cs;
    s->ip = ip;
    s->signature = n3_signature(rt, cs, ip);
    n3_pages(s, rt, cs, ip);
    s->state = MD_N3_SITE_SEEN;

    return s;
}
'@

$s = Replace-Once $s $old $new "instrument n3_site miss reasons"

$old = @'
    if (n3->seen_code_epoch != rt->code_epoch) {
        md_jit_reset(&n3->jit); md_native_v2_runtime_init(&n3->nv2);
        memset(n3->site,0,sizeof(n3->site)); memset(n3->shadow,0,sizeof(n3->shadow));
        n3->shadow_top=n3->shadow_count=0; n3->seen_code_epoch=rt->code_epoch;
    }
'@

$new = @'
    if (n3->seen_code_epoch != rt->code_epoch) {
        /*
         * seen_code_epoch==0 is the first attachment after md_native3_init(),
         * not an invalidation event.  Count only subsequent whole-engine
         * resets so page-local SMC and image/reset churn remain distinguishable.
         */
        if (n3->seen_code_epoch != 0u)
            N3STAT_INC(n3, epoch_resets);

        md_jit_reset(&n3->jit);
        md_native_v2_runtime_init(&n3->nv2);
        memset(n3->site,0,sizeof(n3->site));
        memset(n3->shadow,0,sizeof(n3->shadow));
        n3->shadow_top=n3->shadow_count=0;
        n3->seen_code_epoch=rt->code_epoch;
    }
'@

$s = Replace-Once $s $old $new "instrument Native-3 epoch resets"

Write-Utf8NoBom $Source $s

# ===========================================================================
# pico/microdos_pico.c
# Add the new profile line immediately after the existing Native-3 lookup line.
# ===========================================================================

$p = Read-Normalized $PicoC

$old = @'
        md_say("[native3] lookup hit/miss=%llu/%llu compiles=%llu "
               "reject=%llu invalid=%llu shadow=%llu/%llu/%llu "
               "smc=%llu\n",
               (unsigned long long)(n3 ? n3->hits : 0u),
               (unsigned long long)(n3 ? n3->misses : 0u),
               (unsigned long long)(n3 ? n3->compiles : 0u),
               (unsigned long long)(n3 ? n3->rejects : 0u),
               (unsigned long long)(n3 ? n3->invalidations : 0u),
               (unsigned long long)(n3 ? n3->shadow_pushes : 0u),
               (unsigned long long)(n3 ? n3->shadow_hits : 0u),
               (unsigned long long)(n3 ? n3->shadow_misses : 0u),
               (unsigned long long)(n3 ? n3->smc_rejects : 0u));

'@

$new = @'
        md_say("[native3] lookup hit/miss=%llu/%llu compiles=%llu "
               "reject=%llu invalid=%llu shadow=%llu/%llu/%llu "
               "smc=%llu\n",
               (unsigned long long)(n3 ? n3->hits : 0u),
               (unsigned long long)(n3 ? n3->misses : 0u),
               (unsigned long long)(n3 ? n3->compiles : 0u),
               (unsigned long long)(n3 ? n3->rejects : 0u),
               (unsigned long long)(n3 ? n3->invalidations : 0u),
               (unsigned long long)(n3 ? n3->shadow_pushes : 0u),
               (unsigned long long)(n3 ? n3->shadow_hits : 0u),
               (unsigned long long)(n3 ? n3->shadow_misses : 0u),
               (unsigned long long)(n3 ? n3->smc_rejects : 0u));

        md_say("[native3-inv] cold=%llu collision=%llu "
               "p0=%llu p1=%llu both=%llu epoch-reset=%llu "
               "code-epoch=%lu write-epoch=%lu\n",
               (unsigned long long)(n3 ? n3->cold_misses : 0u),
               (unsigned long long)(n3 ? n3->site_collisions : 0u),
               (unsigned long long)(n3 ? n3->invalid_page0_only : 0u),
               (unsigned long long)(n3 ? n3->invalid_page1_only : 0u),
               (unsigned long long)(n3 ? n3->invalid_both_pages : 0u),
               (unsigned long long)(n3 ? n3->epoch_resets : 0u),
               (unsigned long)g_sys.runtime.code_epoch,
               (unsigned long)g_sys.runtime.code_write_epoch);

'@

$p = Replace-Once $p $old $new "print Native-3 invalidation diagnostics"
Write-Utf8NoBom $PicoC $p

Write-Host ""
Write-Host "Applied Native-3 invalidation diagnostics."
Write-Host "Profile capacity restored to:"
Write-Host "  N3 site slots: 256"
Write-Host "  NV2 slots:     32"
Write-Host ""
Write-Host "Changed:"
Write-Host "  include\microdos\native3.h"
Write-Host "  src\runtime\native3.c"
Write-Host "  pico\microdos_pico.c"
Write-Host "  pico\CMakeLists.txt"
Write-Host ""
Write-Host "Validate:"
Write-Host '  .\md.bat build host'
Write-Host '  ctest --test-dir .\build-host -C Release -R "^native3$" -V'
Write-Host '  .\scripts\md_native3_dos_profile.ps1'
