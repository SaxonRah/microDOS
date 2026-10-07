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
#ifndef MICRODOS_PICO_M25
#define MICRODOS_PICO_M25 0           /* M25 general Thumb-2 translator */
#endif
#if MICRODOS_PICO_M25
#include "microdos/translate.h"
#ifndef MICRODOS_PICO_M25_ARENA
#define MICRODOS_PICO_M25_ARENA (32u * 1024u)
#endif
#endif
#ifndef MICRODOS_PICO_NATIVE_V2
#define MICRODOS_PICO_NATIVE_V2 0
#endif
#ifndef MICRODOS_NATIVE_V2_BACKEDGE_PROFILE
#define MICRODOS_NATIVE_V2_BACKEDGE_PROFILE 0
#endif
#if defined(MICRODOS_ENABLE_NATIVE_V2G)
#include "microdos/native_v2g.h"
#endif
#if MICRODOS_PICO_JIT
#include "microdos/jit.h"
#endif

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "hardware/structs/xip_ctrl.h"
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
#if MICRODOS_PICO_M25
/* aligned to its own size: translated code forms host addresses with one BFI */
static uint8_t __uninitialized_psram("md_guest") __attribute__((aligned(MD_GUEST_BYTES)))
    g_guest[MD_GUEST_BYTES];
#else
static uint8_t __uninitialized_psram("md_guest") __attribute__((aligned(16)))
    g_guest[MD_GUEST_BYTES];
#endif
#endif
static uint8_t __uninitialized_psram("md_disk") __attribute__((aligned(16)))
    g_disk[MD_DISK_BYTES];

static MdDos2System g_sys;
#if MICRODOS_PICO_M25
static MdTranslator g_tr;
static uint8_t __attribute__((aligned(8))) g_tr_arena[MICRODOS_PICO_M25_ARENA];
#endif
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
#ifndef MICRODOS_PICO_HOT_CODE
#define MICRODOS_PICO_HOT_CODE 0
#endif
#ifndef MICRODOS_PICO_EXEC_HOT_CODE
#define MICRODOS_PICO_EXEC_HOT_CODE 0
#endif
#if MICRODOS_PICO_CODE_IN_SRAM
#define MD_PICO_CODE_PLACEMENT "all SRAM (copy_to_ram)"
#elif MICRODOS_PICO_EXEC_HOT_CODE
#define MD_PICO_CODE_PLACEMENT "execution SRAM + cold flash XIP"
#elif MICRODOS_PICO_HOT_CODE
#define MD_PICO_CODE_PLACEMENT "hot SRAM + cold flash XIP"
#else
#define MD_PICO_CODE_PLACEMENT "all flash XIP"
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
    uint64_t rep_instructions;
    uint64_t rep_elements;
    uint64_t rep_payload_bytes;
    uint64_t rep_memory_bytes;
    uint64_t rep_op_instructions[10];
    uint64_t rep_op_elements[10];
    uint64_t xip_accesses;
    uint64_t xip_hits;
    uint64_t qmi_accesses[MD_QMI_CATEGORY_COUNT];
    uint64_t qmi_misses[MD_QMI_CATEGORY_COUNT];
    uint32_t qmi_stack_overflows;
    uint64_t aot_instructions;
    uint64_t jit_owned_instructions;
    uint64_t jit_native_instructions;
    uint64_t jit_fallback_instructions;
    uint64_t native_v2_instructions;
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
    unsigned rep_i;
    p->instructions = g_sys.runtime.instructions;
    p->rep_instructions = g_sys.runtime.rep_instructions;
    p->rep_elements = g_sys.runtime.rep_elements;
    p->rep_payload_bytes = g_sys.runtime.rep_payload_bytes;
    p->rep_memory_bytes = g_sys.runtime.rep_memory_bytes;
    for (rep_i = 0u; rep_i < 10u; ++rep_i) {
        p->rep_op_instructions[rep_i] = g_sys.runtime.rep_op_instructions[rep_i];
        p->rep_op_elements[rep_i] = g_sys.runtime.rep_op_elements[rep_i];
    }
    p->xip_accesses = xip_ctrl_hw->ctr_acc;
    p->xip_hits = xip_ctrl_hw->ctr_hit;
    for (rep_i = 0u; rep_i < MD_QMI_CATEGORY_COUNT; ++rep_i) {
        p->qmi_accesses[rep_i] = g_sys.runtime.qmi_accesses[rep_i];
        p->qmi_misses[rep_i] = g_sys.runtime.qmi_misses[rep_i];
    }
    p->qmi_stack_overflows = g_sys.runtime.qmi_stack_overflows;
    p->aot_instructions = g_sys.runtime.aot_instructions;
    p->jit_owned_instructions = g_sys.jit_instructions;
    p->jit_native_instructions = g_sys.jit_native_instructions;
    p->jit_fallback_instructions = g_sys.jit_fallback_instructions;
#if MICRODOS_PICO_M25
    /* reported in the native-v2 column so existing log parsers keep working
       (M25 translated + Native v2 loops when both are enabled) */
    p->native_v2_instructions = g_tr.stats.native_instructions
#if MICRODOS_PICO_NATIVE_V2
        + g_sys.native_v2_instructions
#endif
        ;
#elif MICRODOS_PICO_NATIVE_V2
    p->native_v2_instructions = g_sys.native_v2_instructions;
#else
    p->native_v2_instructions = 0u;
#endif
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
    const uint64_t nv2 = md_u64_delta(now->native_v2_instructions, from->native_v2_instructions);
    const uint64_t bios = md_u64_delta(now->bios_interpreted_instructions, from->bios_interpreted_instructions);
    const uint64_t native_total = aot + jnative + nv2;
    const uint64_t interp = instr > native_total ? instr - native_total : 0u;
    const uint64_t interp_other = interp > bios ? interp - bios : 0u;
    const uint64_t rep_instr = md_u64_delta(now->rep_instructions, from->rep_instructions);
    const uint64_t rep_elem = md_u64_delta(now->rep_elements, from->rep_elements);
    const uint64_t rep_payload = md_u64_delta(now->rep_payload_bytes, from->rep_payload_bytes);
    const uint64_t rep_memory = md_u64_delta(now->rep_memory_bytes, from->rep_memory_bytes);
    const uint64_t xip_acc = md_u64_delta(now->xip_accesses, from->xip_accesses);
    const uint64_t xip_hit = md_u64_delta(now->xip_hits, from->xip_hits);
    const uint64_t xip_miss = xip_acc > xip_hit ? xip_acc - xip_hit : 0u;
    uint64_t qmi_acc_sum = 0u, qmi_miss_sum = 0u;
    unsigned qmi_i;

    md_say("[perf] --- %s ---\n", label);
    md_say("[perf] wall %9.3f s   in-guest-loop %9.3f s\n", (double)wall / 1e6, (double)run / 1e6);
    md_say("[perf] active %7.3f s   idle-sleep %.3f s   console-out %.3f s   input-wait %.3f s   disk %.3f s\n",
           (double)active / 1e6, (double)sleep / 1e6, (double)out / 1e6, (double)in / 1e6, (double)disk / 1e6);
    md_say("[perf] instructions %llu   active %.3f MIPS\n",
           (unsigned long long)instr, active ? (double)instr/(double)active : 0.0);
    md_say("[xip] accesses %llu  hits %llu  misses %llu  hit-rate %.2f%%  misses/guest %.4f\n",
           (unsigned long long)xip_acc, (unsigned long long)xip_hit,
           (unsigned long long)xip_miss, xip_acc ? 100.0 * (double)xip_hit / (double)xip_acc : 0.0,
           instr ? (double)xip_miss / (double)instr : 0.0);
    {
        static const char *const qn[MD_QMI_CATEGORY_COUNT] = {
            "interp", "m25-native", "translate", "native-v2", "step", "dos-int"
        };
        for (qmi_i = 0u; qmi_i < MD_QMI_CATEGORY_COUNT; ++qmi_i) {
            const uint64_t qa = md_u64_delta(now->qmi_accesses[qmi_i], from->qmi_accesses[qmi_i]);
            const uint64_t qm = md_u64_delta(now->qmi_misses[qmi_i], from->qmi_misses[qmi_i]);
            qmi_acc_sum += qa; qmi_miss_sum += qm;
            md_say("[qmi] %-10s accesses %llu misses %llu access/guest %.4f miss/guest %.4f\n",
                   qn[qmi_i], (unsigned long long)qa, (unsigned long long)qm,
                   instr ? (double)qa / (double)instr : 0.0,
                   instr ? (double)qm / (double)instr : 0.0);
        }
        md_say("[qmi] attributed accesses %llu (%.1f%%) misses %llu (%.1f%%) stack-overflow %lu\n",
               (unsigned long long)qmi_acc_sum, xip_acc ? 100.0*(double)qmi_acc_sum/(double)xip_acc : 0.0,
               (unsigned long long)qmi_miss_sum, xip_miss ? 100.0*(double)qmi_miss_sum/(double)xip_miss : 0.0,
               (unsigned long)(now->qmi_stack_overflows - from->qmi_stack_overflows));
        md_say("[qmi] unattributed accesses %llu misses %llu\n",
               (unsigned long long)(xip_acc > qmi_acc_sum ? xip_acc - qmi_acc_sum : 0u),
               (unsigned long long)(xip_miss > qmi_miss_sum ? xip_miss - qmi_miss_sum : 0u));
    }
    md_say("[rep] instructions %llu  elements %llu  payload %llu B  traffic %llu B\n",
           (unsigned long long)rep_instr, (unsigned long long)rep_elem,
           (unsigned long long)rep_payload, (unsigned long long)rep_memory);
    if (active != 0u && rep_elem != 0u) {
        md_say("[rep] rate elements %.3f M/s  payload %.3f MiB/s  traffic %.3f MiB/s  1.44MB-eq %.3f/s\n",
               (double)rep_elem / (double)active,
               (double)rep_payload * 1.0e6 / (double)active / 1048576.0,
               (double)rep_memory * 1.0e6 / (double)active / 1048576.0,
               (double)rep_payload * 1.0e6 / (double)active / 1474560.0);
    }
    {
        static const char *const rn[10] = {
            "MOVSB","MOVSW","CMPSB","CMPSW","STOSB",
            "STOSW","LODSB","LODSW","SCASB","SCASW"
        };
        unsigned ri;
        md_say("[rep] elements by op:");
        for (ri = 0u; ri < 10u; ++ri) {
            const uint64_t d = md_u64_delta(now->rep_op_elements[ri], from->rep_op_elements[ri]);
            if (d != 0u) md_say(" %s=%llu", rn[ri], (unsigned long long)d);
        }
        md_say("\n");
    }
    md_say("[perf] tiers: static-aot %llu (%.1f%%)   old-jit %llu (%.1f%%)   native-v2 %llu (%.1f%%)   interpreted %llu (%.1f%%)\n",
           (unsigned long long)aot, instr ? 100.0*(double)aot/(double)instr : 0.0,
           (unsigned long long)jnative, instr ? 100.0*(double)jnative/(double)instr : 0.0,
           (unsigned long long)nv2, instr ? 100.0*(double)nv2/(double)instr : 0.0,
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
    md_say("\n[perf] config: M23, clk %lu MHz, code %s, block cache %s, kernel AOT %s, old JIT %s, Native v2 %s, guest %s\n",
           (unsigned long)(clock_get_hz(clk_sys)/1000000u),
           MD_PICO_CODE_PLACEMENT,
           MICRODOS_PICO_CACHE?"ON":"OFF", g_sys.kernel_attached?"ON":"OFF",
           MICRODOS_PICO_JIT?"ON":"OFF", MICRODOS_PICO_NATIVE_V2?"ON":"OFF",
           MICRODOS_PICO_GUEST_SRAM?"SRAM":"PSRAM");
#if MICRODOS_PICO_M25
    md_say("[perf] note: M25 translator ON; the native-v2 tier column counts M25 translated instructions\n");
#endif
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
#if MICRODOS_PICO_M25
    {
        const MdTrStats *t = &g_tr.stats;
        md_say("[m25] native=%llu interp=%llu episodes=%lu translations=%lu untranslatable=%lu chains=%lu flushes=%lu code=%lu B\n",
               (unsigned long long)t->native_instructions, (unsigned long long)t->interp_instructions,
               (unsigned long)t->episodes, (unsigned long)t->translations, (unsigned long)t->untranslatable,
               (unsigned long)t->chains, (unsigned long)t->flushes, (unsigned long)t->code_bytes);
        md_say("[m25] exits edge=%lu dynamic=%lu budget=%lu invalid=%lu store=%lu   live-pages=%lu fallback-pages=%lu latches=%lu\n",
               (unsigned long)t->exit_edge, (unsigned long)t->exit_dynamic, (unsigned long)t->exit_budget,
               (unsigned long)t->exit_invalid, (unsigned long)t->exit_store, (unsigned long)t->live_pages,
               (unsigned long)t->live_fallback_pages, (unsigned long)t->deferred_latches);
        md_say("[m25] tiering backedge-exits=%lu suppressed=%lu in-block-steps=%lu  bytes/block=%lu  unguarded-chains=%lu\n",
               (unsigned long)t->backedge_exits, (unsigned long)t->suppressed, (unsigned long)t->step_ops,
               (unsigned long)(t->translations ? t->code_bytes / t->translations : 0u),
               (unsigned long)t->chains_unguarded);
        md_say("[m25] steps executed=%lu rep=%lu helper-calls=%lu   loop-hook runs=%lu instr=%llu\n",
               (unsigned long)t->step_execs, (unsigned long)t->step_rep, (unsigned long)t->helper_calls,
               (unsigned long)t->hook_runs, (unsigned long long)t->hook_instructions);
        {
            const double tot = t->cyc_total ? (double)t->cyc_total : 1.0;
            md_say("[m25] cycles: total %.3f s  native %.1f%% (of which steps %.1f%%)  interp %.1f%%  translate %.1f%%  dispatch/other %.1f%%\n",
                   (double)t->cyc_total / (double)clock_get_hz(clk_sys),
                   100.0 * (double)t->cyc_native / tot, 100.0 * (double)t->cyc_step / tot,
                   100.0 * (double)t->cyc_interp / tot, 100.0 * (double)t->cyc_translate / tot,
                   100.0 * (double)(t->cyc_total - t->cyc_native - t->cyc_interp - t->cyc_translate) / tot);
        }
        {
            /* top stepped opcodes by executions: the next lowering targets */
            uint8_t taken[256];
            unsigned k, i, best;
            memset(taken, 0, sizeof(taken));
            md_say("[m25] top steps:");
            for (k = 0u; k < 10u; ++k) {
                best = 256u;
                for (i = 0u; i < 256u; ++i)
                    if (!taken[i] && g_tr.step_hist[i] != 0u &&
                        (best == 256u || g_tr.step_hist[i] > g_tr.step_hist[best])) best = i;
                if (best == 256u) break;
                taken[best] = 1u;
                md_say(" %02X:%lu", best, (unsigned long)g_tr.step_hist[best]);
            }
            md_say("\n");
        }
    }
#endif
#if MICRODOS_INT_PROFILE
    {
        const MdIntProfile *p = md_int_profile_counts();
        uint8_t taken[256];
        uint8_t site_taken[MD_INT_PROFILE_SITE_SLOTS];
        unsigned k, i, best;
        md_say("[intprof] guest-cd total=%llu site-drops=%lu\n",
               (unsigned long long)p->total, (unsigned long)p->site_drops);
        memset(taken, 0, sizeof(taken));
        md_say("[intprof] vectors:");
        for (k = 0u; k < 12u; ++k) {
            best = 256u;
            for (i = 0u; i < 256u; ++i)
                if (!taken[i] && p->vector[i] != 0u &&
                    (best == 256u || p->vector[i] > p->vector[best])) best = i;
            if (best == 256u) break;
            taken[best] = 1u;
            md_say(" %02X:%lu", best, (unsigned long)p->vector[best]);
        }
        md_say("\n");
#define MD_PRINT_AH_TOP(tag, arr, limit) do { \
            memset(taken, 0, sizeof(taken)); \
            md_say("[intprof] " tag ":"); \
            for (k = 0u; k < (limit); ++k) { \
                best = 256u; \
                for (i = 0u; i < 256u; ++i) \
                    if (!taken[i] && (arr)[i] != 0u && \
                        (best == 256u || (arr)[i] > (arr)[best])) best = i; \
                if (best == 256u) break; \
                taken[best] = 1u; \
                md_say(" %02X:%lu", best, (unsigned long)(arr)[best]); \
            } \
            md_say("\n"); \
        } while (0)
        MD_PRINT_AH_TOP("int21-ah", p->int21_ah, 20u);
        MD_PRINT_AH_TOP("int10-ah", p->int10_ah, 12u);
        MD_PRINT_AH_TOP("int13-ah", p->int13_ah, 12u);
        MD_PRINT_AH_TOP("int16-ah", p->int16_ah, 12u);
        MD_PRINT_AH_TOP("int33-axlo", p->int33_ax_low, 12u);
#undef MD_PRINT_AH_TOP
        memset(site_taken, 0, sizeof(site_taken));
        md_say("[intprof] sites:");
        for (k = 0u; k < 16u; ++k) {
            best = MD_INT_PROFILE_SITE_SLOTS;
            for (i = 0u; i < MD_INT_PROFILE_SITE_SLOTS; ++i)
                if (!site_taken[i] && p->site[i].count != 0u &&
                    (best == MD_INT_PROFILE_SITE_SLOTS || p->site[i].count > p->site[best].count)) best = i;
            if (best == MD_INT_PROFILE_SITE_SLOTS) break;
            site_taken[best] = 1u;
            md_say(" %04X:%04X/CD%02X/AX%04X:%lu", p->site[best].cs, p->site[best].ip,
                   p->site[best].vector, p->site[best].ax, (unsigned long)p->site[best].count);
        }
        md_say("\n");
        md_int_profile_reset();
    }
#endif
#if defined(MICRODOS_ENABLE_NATIVE3) && MD_N3_PROFILE
    {
        const MdN3Stats *n3 = md_native3_stats(&g_sys.native3);
        const MdNativeV2Runtime *nv = &g_sys.native3.nv2;

        md_say("[native3] owned=%llu retired=%llu native=%llu interp=%llu "
               "entries=%llu jit=%llu/%llu nv2=%llu/%llu\n",
               (unsigned long long)g_sys.native3_instructions,
               (unsigned long long)(n3 ? n3->retired : 0u),
               (unsigned long long)(n3 ? n3->native_retired : 0u),
               (unsigned long long)(n3 ? n3->interp_retired : 0u),
               (unsigned long long)(n3 ? n3->entries : 0u),
               (unsigned long long)(n3 ? n3->jit_entries : 0u),
               (unsigned long long)(n3 ? n3->jit_retired : 0u),
               (unsigned long long)(n3 ? n3->nv2_entries : 0u),
               (unsigned long long)(n3 ? n3->nv2_retired : 0u));

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

#if MD_JIT_BYTE_EXACT_TRACKING && MICRODOS_TRANSLATION_SUPPORT
        md_say("[native3-jit-smc] exact-pages=%u fallback-pages=%u\n",
               (unsigned)g_sys.native3.jit.live_used,
               (unsigned)g_sys.native3.jit.live_fallback_pages);
#endif

        md_say("[native3-nv2] retired=%llu entries=%llu lookups=%llu "
               "hit/miss=%llu/%llu probes=%llu compiles=%llu "
               "compile-reject=%llu stack-guard=%llu budget=%llu short=%llu\n",
               (unsigned long long)nv->retired,
               (unsigned long long)nv->entries,
               (unsigned long long)nv->lookups,
               (unsigned long long)nv->cache_hits,
               (unsigned long long)nv->cache_misses,
               (unsigned long long)nv->probes,
               (unsigned long long)nv->compiles,
               (unsigned long long)nv->compile_rejects,
               (unsigned long long)nv->stack_guard_rejects,
               (unsigned long long)nv->budget_rejects,
               (unsigned long long)nv->short_rejects);
    }
#endif

#if MICRODOS_PICO_NATIVE_V2
    {
        const MdNativeV2Runtime *nv = &g_sys.native_v2;
        unsigned i;

        md_say("[native-v2] retired=%llu entries=%llu lookups=%llu hit/miss=%llu/%llu "
               "rejected-hit=%llu probes=%llu compiles=%llu\n",
               (unsigned long long)nv->retired,
               (unsigned long long)nv->entries,
               (unsigned long long)nv->lookups,
               (unsigned long long)nv->cache_hits,
               (unsigned long long)nv->cache_misses,
               (unsigned long long)nv->rejected_hits,
               (unsigned long long)nv->probes,
               (unsigned long long)nv->compiles);
        md_say("[native-v2] reject compile=%llu store=%llu store-guard=%llu stack-guard=%llu muldiv-guard=%llu stale=%llu budget=%llu short=%llu runtime=%llu\n",
               (unsigned long long)nv->compile_rejects,
               (unsigned long long)nv->store_rejects,
               (unsigned long long)nv->store_guard_rejects,
               (unsigned long long)nv->stack_guard_rejects,
               (unsigned long long)nv->muldiv_guard_rejects,
               (unsigned long long)nv->stale_code,
               (unsigned long long)nv->budget_rejects,
               (unsigned long long)nv->short_rejects,
               (unsigned long long)nv->runtime_fallbacks);
        md_say("[native-v2] chunks entries=%llu iterations=%llu\n",
               (unsigned long long)nv->chunked_entries,
               (unsigned long long)nv->chunked_iterations);

#if defined(MICRODOS_ENABLE_NATIVE_V2G)
        {
            const MdNativeV2GStats *gs = md_native_v2g_stats();
            md_say("[native-v2g] attempts=%llu compiles=%llu "
                   "badarg=%llu region=%llu decode=%llu control=%llu "
                   "opcode=%llu cfg=%llu flags=%llu memory=%llu "
                   "exits=%llu emit=%llu\n",
                   (unsigned long long)gs->attempts,
                   (unsigned long long)gs->compiles,
                   (unsigned long long)gs->reject_bad_argument,
                   (unsigned long long)gs->reject_region,
                   (unsigned long long)gs->reject_decode,
                   (unsigned long long)gs->reject_control,
                   (unsigned long long)gs->reject_opcode,
                   (unsigned long long)gs->reject_cfg,
                   (unsigned long long)gs->reject_flags,
                   (unsigned long long)gs->reject_memory,
                   (unsigned long long)gs->reject_exits,
                   (unsigned long long)gs->reject_emit);
        }
#endif

        for (i = 0u; i < MD_NATIVE_V2_RT_SLOTS; ++i) {
            const MdNativeV2RuntimeSlot *s = &nv->slot[i];

            if (s->state == MD_NV2_RT_COMPILED && s->entries != 0u) {
                md_say("[native-v2] slot%u %04X:%04X bytes=%u ops=%u "
                       "phase=%u dyn=%u base=%u chunk=%u counter=%u entries=%lu retired=%llu\n",
                       i, s->cs, s->ip,
                       (unsigned)s->guest_size,
                       (unsigned)s->code.op_count,
                       (unsigned)s->code.phase,
                       (unsigned)s->code.dynamic_retire,
                       (unsigned)s->code.retire_base_ops,
                       (unsigned)s->code.chunkable_loop,
                       (unsigned)s->counter_reg,
                       (unsigned long)s->entries,
                       (unsigned long long)s->retired);
            } else if (s->state == MD_NV2_RT_REJECTED) {
                unsigned j;
                md_say("[native-v2] reject-slot%u %04X:%04X reason=%s status=%s code=",
                       i, s->cs, s->ip,
                       md_native_v2_reject_reason_name(s->reject_reason),
                       md_native_v2_status_name((MdNativeV2Status)s->reject_status));
                for (j = 0u; j < s->reject_bytes_len; ++j)
                    md_say("%02X%s", s->reject_bytes[j],
                           j + 1u == s->reject_bytes_len ? "" : " ");
                md_say("\n");
            }
        }
    }
#endif
#if MICRODOS_NATIVE_V2_BACKEDGE_PROFILE
    {
        const MdRuntime *rt = &g_sys.runtime;
        unsigned i;

        md_say("[native-v2-profile] backward-taken=%llu\n",
               (unsigned long long)rt->native_v2_backedge_hits);

        for (i = 0u; i < MD_NATIVE_V2_BACKEDGE_SLOTS; ++i) {
            const MdNativeV2BackedgeSite *s = &rt->native_v2_backedge[i];
            unsigned j;

            if (s->hits == 0u)
                continue;

            md_say("[native-v2-profile] edge%u hits=%lu op=%02X "
                   "%04X:%04X -> %04X code=",
                   i,
                   (unsigned long)s->hits,
                   (unsigned)s->opcode,
                   s->cs, s->source_ip, s->target_ip);

            for (j = 0u; j < s->bytes_len; ++j)
                md_say("%02X%s", s->bytes[j],
                       j + 1u == s->bytes_len ? "" : " ");
            md_say("\n");
        }
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
#if MICRODOS_PICO_M25
    if (md_tr_init(&g_tr, &g_sys.runtime, g_tr_arena, MICRODOS_PICO_M25_ARENA)) {
        g_sys.translator = &g_tr;
        md_say("  m25:     translator ON, %lu KiB arena at %p, guest %s\n",
               (unsigned long)(MICRODOS_PICO_M25_ARENA / 1024u), (void *)g_tr_arena,
               g_tr.mem_aligned ? "1 MiB-aligned (BFI)" : "unaligned");
    } else {
        md_say("  m25:     translator unavailable; interpreter only\n");
    }
#endif
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
    md_say("  config:  code %s, block cache %s, old JIT %s, Native v2 %s\n",
           MD_PICO_CODE_PLACEMENT,
           MICRODOS_PICO_CACHE?"ON":"OFF", MICRODOS_PICO_JIT?"ON":"OFF",
           MICRODOS_PICO_NATIVE_V2?"ON":"OFF");
#if defined(MICRODOS_ENABLE_NATIVE3)
    md_say("  n3:      Native-3 ON%s\n",
#if MD_N3_PROFILE
           " (profile counters ON)"
#else
           ""
#endif
    );
#endif
    md_say("  keys:    Ctrl+] -> tier/native statistics (boot + interval)\n");

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
