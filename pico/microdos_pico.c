/* microDOS for Raspberry Pi Pico 2 / Pimoroni Pico Plus 2 (M20.2).
 *
 * Released MS-DOS 2.0 kernel + COMMAND.COM.  Static AOT remains first tier;
 * the optional M20 runtime translator owns segments with no matching static
 * image.  Ctrl+] reports exact time accounting plus static-AOT/JIT/interpreter
 * instruction partitioning and JIT exit hot sites.
 */
#include "md_dos2_system.h"
#ifndef MICRODOS_PICO_DOS2TEST_AOT
#define MICRODOS_PICO_DOS2TEST_AOT 1
#endif
#if MICRODOS_PICO_DOS2TEST_AOT
#include "dos2test_recomp.h"
#endif
#if MICRODOS_PICO_KERNEL_AOT
#include "msdos2_recomp.h"
#endif
#ifndef MICRODOS_PICO_JIT
#define MICRODOS_PICO_JIT 0
#endif
#if MICRODOS_PICO_JIT
#include "microdos/jit.h"
#endif

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Translation-only telemetry is absent from the minimal interpreter runtime. */
#if MICRODOS_TRANSLATION_SUPPORT
#define MD_RUNTIME_AOT_EVICTIONS(rt) ((rt)->aot_evictions)
#else
#define MD_RUNTIME_AOT_EVICTIONS(rt) 0u
#endif

/* Guest RAM spans the whole guest address space (1 MiB by default;
   M21.1 A/B firmware uses a 128 KiB space). MICRODOS_PICO_GUEST_SRAM places
   it in SRAM instead of PSRAM, for measuring the PSRAM penalty. */
#ifndef MICRODOS_PICO_GUEST_SRAM
#define MICRODOS_PICO_GUEST_SRAM 0
#endif
#define MD_GUEST_BYTES MD_X86_ADDRESS_SPACE
#define MD_DISK_BYTES (720u * 512u)
#define MD_SLICE 200000u
#define MD_IDLE_POLLS 256u

extern const uint8_t md_blob_msdos_sys[], md_blob_msdos_sys_end[];
extern const uint8_t md_blob_disk[], md_blob_disk_end[];

#if MICRODOS_PICO_GUEST_SRAM
static uint8_t g_guest[MD_GUEST_BYTES] __attribute__((aligned(16)));
#else
static uint8_t __uninitialized_psram("md_guest") __attribute__((aligned(16)))
    g_guest[MD_GUEST_BYTES];
#endif
static uint8_t __uninitialized_psram("md_disk") __attribute__((aligned(16)))
    g_disk[MD_DISK_BYTES];

static MdDos2System g_sys;
#if MICRODOS_PICO_CACHE
static MdBlockCache g_cache;
#endif
#if MICRODOS_PICO_DOS2TEST_AOT
static const MdAotProgram *const g_programs[] = { &md_recomp_dos2test_program };
#define MD_PICO_PROGRAM_COUNT 1u
#else
static const MdAotProgram *const *const g_programs = NULL;   /* DOS2TEST runs interpreted/JIT */
#define MD_PICO_PROGRAM_COUNT 0u
#endif

typedef struct PicoConsole {
    bool have_pending;
    uint8_t pending;
    bool last_was_cr;
    bool stats_requested;
    uint32_t idle_polls;
    uint32_t idle_sleeps;
} PicoConsole;

static PicoConsole g_con;
static uint32_t g_disk_writes;

#ifndef MICRODOS_PICO_CACHE
#define MICRODOS_PICO_CACHE 1
#endif
#ifndef MICRODOS_PICO_CODE_IN_SRAM
#define MICRODOS_PICO_CODE_IN_SRAM 0
#endif
#ifndef MICRODOS_PICO_KERNEL_AOT
#define MICRODOS_PICO_KERNEL_AOT 0
#endif

/* Exact host-time accounting plus execution-tier snapshots. */
typedef struct PicoPerf {
    uint64_t run_us;
    uint64_t idle_sleep_us;
    uint64_t console_out_us;
    uint64_t input_wait_us;
    uint64_t disk_us;
    uint64_t instructions;
    uint64_t aot_instructions;
    uint64_t jit_owned_instructions;
    uint64_t jit_native_instructions;
    uint64_t jit_fallback_instructions;
    uint64_t bios_interpreted_instructions;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_invalidations;
    uint64_t cache_fallback;
    uint32_t idle_sleeps;
    uint64_t at_us;
#if MICRODOS_PICO_JIT
    uint64_t jit_lookups, jit_hits, jit_misses, jit_compiles, jit_entries;
    uint64_t jit_direct, jit_fallback, jit_invalidations, jit_flushes, jit_boundary;
    uint64_t jit_local_edges, jit_helper_sites;
    uint64_t jit_resident_regions, jit_resident_entries, jit_resident_instructions;
    uint64_t jit_generic_regions, jit_generic_entries, jit_generic_instructions;
    uint64_t jit_cfg_regions, jit_cfg_entries, jit_cfg_instructions, jit_cfg_edges;
    uint64_t jit_native_returns, jit_cs_change_exits, jit_stop_exits;
    uint64_t jit_compile_fb, jit_budget_fb, jit_zero_fb, jit_cold_fb;
    uint64_t jit_control, jit_bios_bypass;
    uint64_t jit_exit_reason[MD_JIT_EXIT_REASON_COUNT];
    MdJitHotSite jit_hot[MD_JIT_HOT_SITES];
    size_t jit_code_used, jit_code_size;
#endif
} PicoPerf;

static PicoPerf g_perf;
static PicoPerf g_perf_mark;

static void md_say(const char *fmt, ...)
{
    char line[240];
    va_list ap;
    size_t n;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (n = 0; line[n] != '\0'; ++n) {
        if (line[n] == '\n') putchar_raw('\r');
        putchar_raw(line[n]);
    }
    stdio_flush();
}

/* ---- console ---------------------------------------------------------- */
static bool pico_fetch(PicoConsole *con, bool block, uint8_t *value)
{
    for (;;) {
        const int ch = block ? getchar() : getchar_timeout_us(0);
        if (ch < 0) return false;
        if (ch == 0x1D) {
            con->stats_requested = true;
            if (!block) return false;
            continue;
        }
        if (ch == '\n' && con->last_was_cr) {
            con->last_was_cr = false;
            continue;
        }
        con->last_was_cr = (ch == '\r');
        *value = (uint8_t)(ch == '\n' ? '\r' : ch);
        return true;
    }
}

static void con_write(void *user, const uint8_t *data, size_t size)
{
    PicoConsole *con = (PicoConsole *)user;
    const uint64_t t0 = time_us_64();
    size_t i;
    con->idle_polls = 0u;
    for (i = 0; i < size; ++i) putchar_raw((int)data[i]);
    stdio_flush();
    g_perf.console_out_us += time_us_64() - t0;
}

static bool con_peek(void *user, uint8_t *value)
{
    PicoConsole *con = (PicoConsole *)user;
    if (!con->have_pending) {
        if (!pico_fetch(con, false, &con->pending)) {
            if (++con->idle_polls >= MD_IDLE_POLLS) {
                const uint64_t t0 = time_us_64();
                ++con->idle_sleeps;
                sleep_us(500);
                g_perf.idle_sleep_us += time_us_64() - t0;
            }
            return false;
        }
        con->have_pending = true;
    }
    con->idle_polls = 0u;
    *value = con->pending;
    return true;
}

static bool con_read(void *user, uint8_t *value)
{
    PicoConsole *con = (PicoConsole *)user;
    con->idle_polls = 0u;
    if (con->have_pending) {
        *value = con->pending;
        con->have_pending = false;
        return true;
    }
    {
        const uint64_t t0 = time_us_64();
        const bool ok = pico_fetch(con, true, value);
        g_perf.input_wait_us += time_us_64() - t0;
        return ok;
    }
}

static void con_flush(void *user)
{
    PicoConsole *con = (PicoConsole *)user;
    con->have_pending = false;
}

/* ---- disk ------------------------------------------------------------- */
static bool disk_read(void *user, uint32_t sector, uint8_t *data, size_t size)
{
    const size_t off = (size_t)sector * size;
    (void)user;
    if (size != 512u || off + size > MD_DISK_BYTES) return false;
    g_con.idle_polls = 0u;
    {
        const uint64_t t0 = time_us_64();
        memcpy(data, g_disk + off, size);
        g_perf.disk_us += time_us_64() - t0;
    }
    return true;
}

static bool disk_write(void *user, uint32_t sector, const uint8_t *data, size_t size)
{
    const size_t off = (size_t)sector * size;
    (void)user;
    if (size != 512u || off + size > MD_DISK_BYTES) return false;
    g_con.idle_polls = 0u;
    {
        const uint64_t t0 = time_us_64();
        memcpy(g_disk + off, data, size);
        g_perf.disk_us += time_us_64() - t0;
    }
    ++g_disk_writes;
    return true;
}

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 0
#endif

static bool md_pico_set_clock(void)
{
#if MICRODOS_PICO_SYS_KHZ > 0
    if (!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ, false)) return false;
    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ, PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != 0) return false;
    return psram_reinitialize() == 0;
#else
    return true;
#endif
}

static bool md_psram_ok(void)
{
    volatile uint32_t *g = (volatile uint32_t *)g_guest;
    volatile uint32_t *d = (volatile uint32_t *)g_disk;
    uint32_t i;
    if (!psram_is_available()) { md_say("  psram:   NOT AVAILABLE\n"); return false; }
    if ((!MICRODOS_PICO_GUEST_SRAM &&
         (!psram_check_address(&g_guest[0]) || !psram_check_address(&g_guest[MD_GUEST_BYTES - 1u]))) ||
        !psram_check_address(&g_disk[0]) || !psram_check_address(&g_disk[MD_DISK_BYTES - 1u])) {
        md_say("  psram:   buffers not inside PSRAM (%p, %p)\n", (void *)g_guest, (void *)g_disk);
        return false;
    }
    for (i = 0; i < MD_GUEST_BYTES / 4u; i += 1024u) g[i] = 0xA5000000u ^ i;
    for (i = 0; i < MD_DISK_BYTES / 4u; i += 1024u) d[i] = 0x5A000000u ^ i;
    for (i = 0; i < MD_GUEST_BYTES / 4u; i += 1024u)
        if (g[i] != (0xA5000000u ^ i)) return false;
    for (i = 0; i < MD_DISK_BYTES / 4u; i += 1024u)
        if (d[i] != (0x5A000000u ^ i)) return false;
    return true;
}

static void md_perf_snapshot(PicoPerf *p)
{
    p->instructions = g_sys.runtime.instructions;
    p->aot_instructions = g_sys.runtime.aot_instructions;
    p->jit_owned_instructions = g_sys.jit_instructions;
    p->jit_native_instructions = g_sys.jit_native_instructions;
    p->jit_fallback_instructions = g_sys.jit_fallback_instructions;
    p->bios_interpreted_instructions = g_sys.bios_interpreted_instructions;
#if MICRODOS_PICO_CACHE
    p->cache_hits = g_cache.hits;
    p->cache_misses = g_cache.misses;
    p->cache_invalidations = g_cache.invalidations;
    p->cache_fallback = g_cache.fallback_instructions;
#else
    p->cache_hits = 0u;
    p->cache_misses = 0u;
    p->cache_invalidations = 0u;
    p->cache_fallback = 0u;
#endif
    p->idle_sleeps = g_con.idle_sleeps;
    p->at_us = time_us_64();
#if MICRODOS_PICO_JIT
    if (g_sys.jit != NULL) {
        const MdJit *j = g_sys.jit;
        unsigned i;
        p->jit_lookups=j->lookups; p->jit_hits=j->hits; p->jit_misses=j->misses;
        p->jit_compiles=j->compiles; p->jit_entries=j->native_entries;
        p->jit_direct=j->direct_instructions; p->jit_fallback=j->fallback_instructions;
        p->jit_invalidations=j->invalidations; p->jit_flushes=j->flushes; p->jit_boundary=j->boundary_fallbacks;
        p->jit_local_edges=j->local_edges; p->jit_helper_sites=j->helper_sites;
        p->jit_resident_regions=j->resident_regions; p->jit_resident_entries=j->resident_entries; p->jit_resident_instructions=j->resident_instructions;
        p->jit_generic_regions=j->generic_regions; p->jit_generic_entries=j->generic_entries; p->jit_generic_instructions=j->generic_instructions;
        p->jit_cfg_regions=j->cfg_regions; p->jit_cfg_entries=j->cfg_entries; p->jit_cfg_instructions=j->cfg_instructions; p->jit_cfg_edges=j->cfg_internal_edges;
        p->jit_native_returns=j->native_returns; p->jit_cs_change_exits=j->cs_change_exits; p->jit_stop_exits=j->stop_exits;
        p->jit_compile_fb=j->compile_fail_fallbacks; p->jit_budget_fb=j->budget_fallbacks; p->jit_zero_fb=j->zero_progress_fallbacks; p->jit_cold_fb=j->cold_fallbacks;
        p->jit_control=j->control_instructions; p->jit_bios_bypass=j->bios_bypass_instructions;
        for (i=0;i<MD_JIT_EXIT_REASON_COUNT;++i) p->jit_exit_reason[i]=j->exit_reason[i];
        for (i=0;i<MD_JIT_HOT_SITES;++i) p->jit_hot[i]=j->hot_sites[i];
        p->jit_code_used=j->code_used; p->jit_code_size=j->code_size;
    }
#endif
}

static uint64_t md_u64_delta(uint64_t a, uint64_t b) { return a >= b ? a - b : 0u; }

static void md_perf_report(const char *label, const PicoPerf *now, const PicoPerf *from, uint64_t since_us)
{
    const uint64_t wall = now->at_us - since_us;
    const uint64_t run = md_u64_delta(now->run_us, from->run_us);
    const uint64_t sleep = md_u64_delta(now->idle_sleep_us, from->idle_sleep_us);
    const uint64_t out = md_u64_delta(now->console_out_us, from->console_out_us);
    const uint64_t in = md_u64_delta(now->input_wait_us, from->input_wait_us);
    const uint64_t disk = md_u64_delta(now->disk_us, from->disk_us);
    const uint64_t host = sleep + out + in + disk;
    const uint64_t active = run > host ? run - host : 0u;
    const uint64_t instr = md_u64_delta(now->instructions, from->instructions);
    const uint64_t aot = md_u64_delta(now->aot_instructions, from->aot_instructions);
    const uint64_t jnative = md_u64_delta(now->jit_native_instructions, from->jit_native_instructions);
    const uint64_t jfallback = md_u64_delta(now->jit_fallback_instructions, from->jit_fallback_instructions);
    const uint64_t jowned = md_u64_delta(now->jit_owned_instructions, from->jit_owned_instructions);
    const uint64_t bios = md_u64_delta(now->bios_interpreted_instructions, from->bios_interpreted_instructions);
    const uint64_t interp = instr > aot + jnative ? instr - aot - jnative : 0u;
    const uint64_t interp_other = interp > bios ? interp - bios : 0u;

    md_say("[perf] --- %s ---\n", label);
    md_say("[perf] wall %9.3f s   in-guest-loop %9.3f s\n", (double)wall / 1e6, (double)run / 1e6);
    md_say("[perf] active %7.3f s   idle-sleep %.3f s   console-out %.3f s   input-wait %.3f s   disk %.3f s\n",
           (double)active / 1e6, (double)sleep / 1e6, (double)out / 1e6, (double)in / 1e6, (double)disk / 1e6);
    md_say("[perf] instructions %llu   active %.3f MIPS\n",
           (unsigned long long)instr, active ? (double)instr/(double)active : 0.0);
    md_say("[perf] tiers: static-aot %llu (%.1f%%)   jit-native %llu (%.1f%%)   interpreted %llu (%.1f%%)\n",
           (unsigned long long)aot, instr ? 100.0*(double)aot/(double)instr : 0.0,
           (unsigned long long)jnative, instr ? 100.0*(double)jnative/(double)instr : 0.0,
           (unsigned long long)interp, instr ? 100.0*(double)interp/(double)instr : 0.0);
    md_say("[perf] interpreted split: bios %llu   other %llu\n",
           (unsigned long long)bios, (unsigned long long)interp_other);
    md_say("[perf] jit-owned %llu (fallback interpreted %llu)   cache h/m/i/f %llu/%llu/%llu/%llu   idle-sleeps %lu\n",
           (unsigned long long)jowned, (unsigned long long)jfallback,
           (unsigned long long)md_u64_delta(now->cache_hits,from->cache_hits),
           (unsigned long long)md_u64_delta(now->cache_misses,from->cache_misses),
           (unsigned long long)md_u64_delta(now->cache_invalidations,from->cache_invalidations),
           (unsigned long long)md_u64_delta(now->cache_fallback,from->cache_fallback),
           (unsigned long)(now->idle_sleeps - from->idle_sleeps));
}

#if MICRODOS_PICO_JIT
static uint64_t md_hot_previous(const PicoPerf *from, const MdJitHotSite *site)
{
    unsigned i;
    for (i=0;i<MD_JIT_HOT_SITES;++i) {
        const MdJitHotSite *p=&from->jit_hot[i];
        if (p->cs==site->cs && p->ip==site->ip && p->dst_cs==site->dst_cs && p->dst_ip==site->dst_ip &&
            p->opcode==site->opcode && p->reason==site->reason) return p->count;
    }
    return 0u;
}

static void md_jit_report(const char *label, const PicoPerf *now, const PicoPerf *from)
{
    unsigned i;
    md_say("[jit] --- %s ---\n", label);
    md_say("[jit] native=%llu fallback=%llu control=%llu entry=%llu compile=%llu hit=%llu miss=%llu return=%llu cs-exit=%llu stop=%llu\n",
           (unsigned long long)md_u64_delta(now->jit_direct,from->jit_direct),
           (unsigned long long)md_u64_delta(now->jit_fallback,from->jit_fallback),
           (unsigned long long)md_u64_delta(now->jit_control,from->jit_control),
           (unsigned long long)md_u64_delta(now->jit_entries,from->jit_entries),
           (unsigned long long)md_u64_delta(now->jit_compiles,from->jit_compiles),
           (unsigned long long)md_u64_delta(now->jit_hits,from->jit_hits),
           (unsigned long long)md_u64_delta(now->jit_misses,from->jit_misses),
           (unsigned long long)md_u64_delta(now->jit_native_returns,from->jit_native_returns),
           (unsigned long long)md_u64_delta(now->jit_cs_change_exits,from->jit_cs_change_exits),
           (unsigned long long)md_u64_delta(now->jit_stop_exits,from->jit_stop_exits));
    md_say("[jit] fallback-reason compile=%llu budget=%llu zero=%llu cold=%llu   invalid=%llu flush=%llu boundary=%llu bios-bypass=%llu\n",
           (unsigned long long)md_u64_delta(now->jit_compile_fb,from->jit_compile_fb),
           (unsigned long long)md_u64_delta(now->jit_budget_fb,from->jit_budget_fb),
           (unsigned long long)md_u64_delta(now->jit_zero_fb,from->jit_zero_fb),
           (unsigned long long)md_u64_delta(now->jit_cold_fb,from->jit_cold_fb),
           (unsigned long long)md_u64_delta(now->jit_invalidations,from->jit_invalidations),
           (unsigned long long)md_u64_delta(now->jit_flushes,from->jit_flushes),
           (unsigned long long)md_u64_delta(now->jit_boundary,from->jit_boundary),
           (unsigned long long)md_u64_delta(now->jit_bios_bypass,from->jit_bios_bypass));
    md_say("[jit] resident=%llu/%llu generic=%llu/%llu cfg=%llu/%llu cfg-edges=%llu   code=%lu/%lu B\n",
           (unsigned long long)md_u64_delta(now->jit_resident_entries,from->jit_resident_entries),
           (unsigned long long)md_u64_delta(now->jit_resident_instructions,from->jit_resident_instructions),
           (unsigned long long)md_u64_delta(now->jit_generic_entries,from->jit_generic_entries),
           (unsigned long long)md_u64_delta(now->jit_generic_instructions,from->jit_generic_instructions),
           (unsigned long long)md_u64_delta(now->jit_cfg_entries,from->jit_cfg_entries),
           (unsigned long long)md_u64_delta(now->jit_cfg_instructions,from->jit_cfg_instructions),
           (unsigned long long)md_u64_delta(now->jit_cfg_edges,from->jit_cfg_edges),
           (unsigned long)now->jit_code_used, (unsigned long)now->jit_code_size);
    md_say("[jit] exit histogram:");
    for (i=1;i<MD_JIT_EXIT_REASON_COUNT;++i) {
        const uint64_t d=md_u64_delta(now->jit_exit_reason[i],from->jit_exit_reason[i]);
        if (d) md_say(" %s=%llu",md_jit_exit_reason_name(i),(unsigned long long)d);
    }
    md_say("\n");
    md_say("[jit] hot sites (interval delta; ~ means space-saving candidate):\n");
    for (i=0;i<MD_JIT_HOT_SITES;++i) {
        const MdJitHotSite *site=&now->jit_hot[i];
        uint64_t prev, delta;
        if (!site->count) continue;
        prev=md_hot_previous(from,site); delta=site->count>=prev?site->count-prev:site->count;
        if (!delta) continue;
        md_say("[jit]   ~%8llu  %04X:%04X op=%02X -> %04X:%04X  %s\n",
               (unsigned long long)delta,site->cs,site->ip,site->opcode,
               site->dst_cs,site->dst_ip,md_jit_exit_reason_name(site->reason));
    }
}
#endif

#if MICRODOS_PICO_JIT && MD_EXEC_PROFILE
static void md_router_report(const MdExecRouter *router)
{
    const uint64_t total = router->interp_instructions + router->aot_instructions + router->jit_instructions;
    unsigned top[4] = { MD_EXEC_SITE_SLOTS, MD_EXEC_SITE_SLOTS, MD_EXEC_SITE_SLOTS, MD_EXEC_SITE_SLOTS };
    unsigned i, j, k;
    md_say("[router] interp=%llu aot=%llu jit=%llu promotions=%lu reject=%lu demote=%lu unstable=%lu\n",
           (unsigned long long)router->interp_instructions, (unsigned long long)router->aot_instructions,
           (unsigned long long)router->jit_instructions, (unsigned long)router->promotions,
           (unsigned long)router->rejections, (unsigned long)router->demotions,
           (unsigned long)router->unstable_demotions);
    md_say("[router] entries=%lu switches/1k=%llu avg-jit-run=%llu promotion=%s direct=%s\n",
           (unsigned long)router->tier_entries,
           (unsigned long long)(total ? (uint64_t)router->tier_switches * 1000u / total : 0u),
           (unsigned long long)(router->jit_entries ? router->jit_instructions / router->jit_entries : 0u),
           MD_EXEC_ENABLE_PROMOTION ? "ON" : "OFF", MD_EXEC_ENABLE_DIRECT ? "ON" : "OFF");
    for (i = 0u; i < MD_EXEC_SITE_SLOTS; ++i) {
        const MdExecSite *site = &router->site[i];
        unsigned score = site->heat + site->penalty + site->cooldown;
        if (score == 0u) continue;
        for (j = 0u; j < 4u; ++j) {
            if (top[j] == MD_EXEC_SITE_SLOTS || score >
                (unsigned)(router->site[top[j]].heat + router->site[top[j]].penalty + router->site[top[j]].cooldown)) {
                for (k = 3u; k > j; --k) top[k] = top[k - 1u];
                top[j] = i;
                break;
            }
        }
    }
    for (i = 0u; i < 4u && top[i] != MD_EXEC_SITE_SLOTS; ++i) {
        const MdExecSite *site = &router->site[top[i]];
        md_say("[router] %04X:%04X mode=%s heat=%u penalty=%u cooldown=%u\n",
               site->cs, site->ip, md_exec_mode_name(site->mode), site->heat, site->penalty, site->cooldown);
    }
}
#endif

static void md_stats(uint64_t start_us)
{
    PicoPerf now=g_perf, zero;
    memset(&zero,0,sizeof(zero));
    md_perf_snapshot(&now);
    md_say("\n[perf] config: M23, clk %lu MHz, code %s, block cache %s, kernel AOT %s, runtime JIT %s, guest %s\n",
           (unsigned long)(clock_get_hz(clk_sys)/1000000u),
           MICRODOS_PICO_CODE_IN_SRAM?"SRAM (copy_to_ram)":"flash XIP",
           MICRODOS_PICO_CACHE?"ON":"OFF", g_sys.kernel_attached?"ON":"OFF",
           MICRODOS_PICO_JIT?"ON":"OFF",
           MICRODOS_PICO_GUEST_SRAM?"SRAM":"PSRAM");
    md_perf_report("since boot",&now,&zero,start_us);
    md_perf_report("since previous Ctrl+]",&now,&g_perf_mark,g_perf_mark.at_us?g_perf_mark.at_us:start_us);
    md_say("[perf] aot: kernel compiled %llu   non-kernel AOT %llu   attached-segment steps %llu\n",
           (unsigned long long)g_sys.kernel_aot_instructions,
           (unsigned long long)(now.aot_instructions-g_sys.kernel_aot_instructions),
           (unsigned long long)g_sys.attached_steps);
    md_say("[perf] aot: attaches=%lu enters=%lu evictions=%lu   disk writes %lu   CS:IP=%04X:%04X\n",
           (unsigned long)g_sys.aot_attaches,(unsigned long)g_sys.aot_enters,
           (unsigned long)MD_RUNTIME_AOT_EVICTIONS(&g_sys.runtime),(unsigned long)g_disk_writes,
           g_sys.runtime.cpu.cs,g_sys.runtime.cpu.ip);
#if MICRODOS_PICO_JIT
    if (g_sys.jit) {
#if MD_EXEC_PROFILE
        md_router_report(&g_sys.router);
#endif
        md_jit_report("since boot",&now,&zero);
        md_jit_report("since previous Ctrl+]",&now,&g_perf_mark);
    }
#endif
    g_perf_mark=now;
}

int main(void)
{
    const size_t kernel_size=(size_t)(md_blob_msdos_sys_end-md_blob_msdos_sys);
    const size_t disk_size=(size_t)(md_blob_disk_end-md_blob_disk);
    uint64_t start_us;
    MdStopReason stop=MD_STOP_NONE;
    const bool clock_ok=md_pico_set_clock();

    stdio_init_all();
    while(!stdio_usb_connected()) sleep_ms(20);
    sleep_ms(300);
    md_say("\nmicroDOS for Pico 2 (Pimoroni Pico Plus 2)\n");
    md_say("  clk_sys: %lu MHz%s\n",(unsigned long)(clock_get_hz(clk_sys)/1000000u),
           MICRODOS_PICO_SYS_KHZ>0?(clock_ok?" (raised; PSRAM retimed)":" (REQUESTED CLOCK FAILED)"):"");
    md_say("  psram:   %lu KiB (sdk available=%d)\n",(unsigned long)(psram_get_size()/1024u),psram_is_available()?1:0);
    if(!md_psram_ok()){md_say("microDOS: PSRAM check failed; halting.\n");for(;;)sleep_ms(1000);}
    md_say("  guest:   %lu KiB at %p (%s, page check ok), DOS memory %lu KiB\n",
           (unsigned long)(MD_GUEST_BYTES / 1024u),(void*)g_guest,MICRODOS_PICO_GUEST_SRAM?"SRAM":"PSRAM",
           (unsigned long)MD_MSDOS2_DEFAULT_MEMORY_PARAGRAPHS / 64ul);
    if(disk_size!=MD_DISK_BYTES){md_say("microDOS: embedded disk is %lu bytes, expected %lu; halting.\n",(unsigned long)disk_size,(unsigned long)MD_DISK_BYTES);for(;;)sleep_ms(1000);}
    memset(g_guest,0,MD_GUEST_BYTES); memcpy(g_disk,md_blob_disk,MD_DISK_BYTES);
    md_say("  disk:    360 KiB image copied from flash to PSRAM (writes are lost at reset)\n");

#if MICRODOS_PICO_CACHE
    md_dos2_system_init(&g_sys, g_guest, &g_cache);
#else
    md_dos2_system_init(&g_sys, g_guest, NULL);
#endif
    g_sys.boot.console.write=con_write; g_sys.boot.console.peek=con_peek; g_sys.boot.console.read=con_read; g_sys.boot.console.flush=con_flush; g_sys.boot.console.user=&g_con;
    g_sys.boot.disk.read=disk_read; g_sys.boot.disk.write=disk_write; g_sys.boot.disk.user=NULL; g_sys.boot.disk.sector_size=512u; g_sys.boot.disk.sector_count=720u; g_sys.boot.disk.writable=true;
    g_sys.boot.clock_days=1162u; g_sys.boot.clock_hours=12u;
#if MICRODOS_PICO_KERNEL_AOT
    md_dos2_system_set_kernel_aot(&g_sys,&md_recomp_msdos2_program);
#endif
    md_dos2_system_set_aot(&g_sys,g_programs,MD_PICO_PROGRAM_COUNT,true);
    if(!md_dos2_system_start(&g_sys,md_blob_msdos_sys,kernel_size)){md_say("microDOS: embedded MSDOS.SYS rejected (%lu bytes); halting.\n",(unsigned long)kernel_size);for(;;)sleep_ms(1000);}
    md_say("  kernel:  MSDOS.SYS %lu bytes\n",(unsigned long)kernel_size);
#if MICRODOS_PICO_KERNEL_AOT
    md_say("  aot:     MSDOS.SYS kernel (%lu compiled, %lu holes) %s\n",(unsigned long)md_recomp_msdos2_program.compiled_instructions,(unsigned long)md_recomp_msdos2_program.hole_instructions,g_sys.kernel_attached?"attached":"NOT ATTACHED");
#else
    md_say("  aot:     MSDOS.SYS kernel: not compiled in this firmware\n");
#endif
#if MICRODOS_PICO_DOS2TEST_AOT
    md_say("  aot:     %s (%lu compiled, %lu holes)\n",g_programs[0]->name,(unsigned long)g_programs[0]->compiled_instructions,(unsigned long)g_programs[0]->hole_instructions);
#else
    md_say("  aot:     DOS2TEST.COM: not compiled in this firmware\n");
#endif
    md_say("  config:  code %s, block cache %s, runtime JIT %s\n",MICRODOS_PICO_CODE_IN_SRAM?"SRAM (copy_to_ram)":"flash XIP",MICRODOS_PICO_CACHE?"ON":"OFF",MICRODOS_PICO_JIT?"ON":"OFF");
    md_say("  keys:    Ctrl+] -> M20.2.1 tier/JIT statistics (boot + interval)\n");

    start_us=time_us_64();
    for(;;){
        const uint64_t t0=time_us_64();
        stop=md_dos2_system_run(&g_sys,MD_SLICE);
        g_perf.run_us+=time_us_64()-t0;
        if(g_con.stats_requested){g_con.stats_requested=false;md_stats(start_us);}
        if(stop!=MD_STOP_NONE)break;
    }
    md_say("\n[microDOS] guest stopped: %s\n",md_stop_reason_name(stop));
    if(stop==MD_STOP_FAULT)md_say("[microDOS] fault linear=%05lX opcode=%02X\n",(unsigned long)g_sys.runtime.fault_linear,g_sys.runtime.fault_opcode);
    md_stats(start_us);
    for(;;)sleep_ms(1000);
}
