/* microDOS RP2350 benchmark firmware (M15 -> M21.1).
 *
 * Separates the costs that full-DOS numbers mix together:
 *   - where the ARM execution code runs (flash XIP vs SRAM: build variant)
 *   - where guest memory lives (SRAM vs PSRAM: both measured here)
 *   - which engine runs it (step, threaded, CS-run, block cache, AOT, JIT)
 *   - JIT cold cost versus steady-state translated-block reuse (M21.1)
 *
 * M20.2 keeps the resident/CFG fast paths and adds direct native control helpers.
 * regionmix proves generic straight-line bodies; branchmix proves a forward CFG
 * edge; callmix repeatedly crosses near CALL/RET boundaries with zero interpreter
 * fallback once the blocks are translated.
 *
 * Workloads are loaded at segment 0, so every guest access stays inside
 * linear 0000-FFFF and a 64 KiB SRAM buffer is a complete guest for them.
 *   loop     CX=FFFF DEC/JNZ, register-only (131071 guest instructions)
 *   memloop  RMW over a 32 KiB window, stride 97 (229379 guest instructions)
 *   regionmix generic resident-loop stress (294916 incl. HLT)
 *   branchmix bounded-CFG proof with alternating forward JZ (212996 incl. HLT)
 *   callmix  32768 near CALL/RET pairs (163843 incl. HLT)
 *
 * Every row checks that the guest produced the reference result, so a fast
 * wrong answer is reported as FAIL, not as a speedup.
 */
#include "microdos/block_cache.h"
#include "microdos/jit.h"
#include "microdos/runtime.h"
#include "loop_recomp.h"
#include "memloop_recomp.h"
#include "checksum_recomp.h"

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef MICRODOS_PICO_CODE_IN_SRAM
#define MICRODOS_PICO_CODE_IN_SRAM 0
#endif

#define SRAM_GUEST_BYTES (64u * 1024u)
#define MD_JIT_CODE_BYTES (32u * 1024u)

static uint8_t __attribute__((aligned(16))) g_sram_guest[SRAM_GUEST_BYTES];
static uint8_t __uninitialized_psram("md_bench_guest") __attribute__((aligned(16)))
    g_psram_guest[1u << 20];

/* Executable SRAM arena. RP2350 SRAM is executable; the JIT backend issues
 * DSB/ISB after filling a block before calling it through a Thumb pointer. */
static uint8_t __attribute__((aligned(16))) g_jit_code[MD_JIT_CODE_BYTES];

static MdRuntime g_rt;
static MdBlockCache g_cache;
static MdJit g_jit;

static const uint8_t kLoop[] = { 0xB9, 0xFF, 0xFF, 0x49, 0x75, 0xFD, 0xF4 };
static const uint8_t kMemloop[] = {
    0xB9,0x00,0x80, 0xBE,0x00,0x80, 0x8A,0x04, 0x04,0x03, 0x88,0x04,
    0x83,0xC6,0x61, 0x81,0xCE,0x00,0x80, 0x49,0x75,0xF0, 0xF4
};

/* M21 primary convergence workload: 32 KiB of real guest-memory traffic. */
static const uint8_t kChecksum[] = {
    0xBE,0x00,0x20,             /* mov si,2000h */
    0xB9,0x00,0x40,             /* mov cx,4000h */
    0x31,0xD2,                  /* xor dx,dx */
    0xFC,                       /* cld */
    0xAD,                       /* L: lodsw */
    0x03,0xD0,                  /* add dx,ax */
    0xE2,0xFB,                  /* loop L */
    0xF4                        /* hlt */
};

/* M20: not an AOT fixture.  It exists specifically to prove the generic
 * resident compiler rather than the old exact loop/memloop recognizers. */
static const uint8_t kRegionmix[] = {
    0xB9,0x00,0x80,             /* mov cx,8000h */
    0xBE,0x00,0x80,             /* mov si,8000h */
    0xBB,0x34,0x12,             /* mov bx,1234h */
    0x8A,0x04,                  /* mov al,[si] */
    0x04,0x03,                  /* add al,3 */
    0x81,0xF3,0x57,0x13,       /* xor bx,1357h */
    0x83,0xC3,0x05,            /* add bx,5 */
    0x88,0x04,                  /* mov [si],al */
    0x83,0xC6,0x61,            /* add si,97 */
    0x81,0xCE,0x00,0x80,       /* or si,8000h */
    0x49,                       /* dec cx */
    0x75,0xE9,                  /* jnz back to mov al,[si] */
    0xF4                        /* hlt */
};


/* M20.1 bounded-CFG proof.  The forward JZ skips ADD BX,3 every other
 * iteration, then both paths reconverge at OR BX,0 before DEC CX/JNZ. */
static const uint8_t kBranchmix[] = {
    0xB9,0x00,0x80,             /* mov cx,8000h */
    0xB8,0x00,0x00,             /* mov ax,0 */
    0xBB,0x00,0x00,             /* mov bx,0 */
    0x83,0xF0,0x01,             /* xor ax,1 */
    0x83,0xF8,0x00,             /* cmp ax,0 */
    0x74,0x03,                  /* jz skip_add */
    0x83,0xC3,0x03,             /* add bx,3 */
    0x83,0xCB,0x00,             /* skip_add: or bx,0 (CF=0) */
    0x49,                       /* dec cx */
    0x75,0xEF,                  /* jnz back to xor ax,1 */
    0xF4                        /* hlt */
};

/* M20.2 control-transfer proof. Every iteration executes CALL -> ADD BX,3 ->
 * RET before DEC/JNZ. 32768 * 3 wraps BX to 8000h. */
static const uint8_t kCallmix[] = {
    0xB9,0x00,0x80,             /* mov cx,8000h */
    0xBB,0x00,0x00,             /* mov bx,0 */
    0xE8,0x04,0x00,             /* call sub */
    0x49,                       /* dec cx */
    0x75,0xFA,                  /* jnz call */
    0xF4,                       /* hlt */
    0x83,0xC3,0x03,             /* sub: add bx,3 */
    0xC3                        /* ret */
};
typedef enum { ENG_STEP, ENG_THREADED, ENG_CSRUN, ENG_CACHE, ENG_AOT, ENG_JIT } Engine;
static const char *const kEngine[] = { "step", "threaded", "cs-run", "cache", "aot", "jit-cold" };

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 0
#endif

static bool bench_set_clock(void)
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

static void say(const char *fmt, ...)
{
    char line[220];
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

static uint32_t window_hash(const uint8_t *mem)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0x8000u; i < 0x10000u; ++i) h = (h ^ mem[i]) * 16777619u;
    return h;
}

/* M21.1 warm-JIT measurement.
 *
 * The normal JIT rows deliberately include md_jit_init() + translation on
 * every sample. For the warm row we prime once, then restore the exact
 * post-load architectural/runtime state and the first 64 KiB of guest memory
 * before each timed execution while keeping g_jit and its translated blocks.
 *
 * g_psram_guest is 1 MiB and benchmark guests use only its first 64 KiB, so
 * the second 64 KiB is a safe untimed snapshot buffer for both SRAM and PSRAM
 * guest runs.
 */
typedef void (*WarmPrepareFn)(uint8_t *mem);
typedef uint32_t (*WarmCheckFn)(const uint8_t *mem);

typedef struct WarmJitDelta {
    uint64_t compiles;
    uint64_t hits;
    uint64_t misses;
    uint64_t entries;
    uint64_t direct;
    uint64_t fallback;
    uint64_t resident_entries;
    uint64_t resident_instructions;
} WarmJitDelta;

static void warm_restore_runtime(const MdRuntime *snapshot, uint8_t *mem)
{
    g_rt = *snapshot;

    /* md_runtime_init/reset normally establish these self-pointers. A struct
       snapshot preserves their values, but rebind them explicitly so this
       benchmark remains correct if the snapshot's storage ever changes. */
    g_rt.cpu.memory = mem;
    g_rt.cpu.code_page_generation = g_rt.code_page_generation;
    g_rt.cpu.code_page_executable = g_rt.code_page_executable;
    g_rt.cpu.code_write_epoch = &g_rt.code_write_epoch;
    g_rt.cpu.aot_guards = g_rt.aot_slots;
}

static uint64_t run_jit_warm(uint8_t *mem,
                             const uint8_t *image, size_t image_size,
                             WarmPrepareFn prepare, WarmCheckFn check_fn,
                             uint64_t *us_best, uint32_t *check,
                             WarmJitDelta *delta)
{
    MdHooks hooks;
    MdRuntime snapshot;
    uint8_t *backup = g_psram_guest + SRAM_GUEST_BYTES;
    MdStopReason st;
    uint64_t c0, h0, m0, e0, d0, f0, re0, ri0;
    uint64_t best = UINT64_MAX;
    uint64_t best_instr = 0u;
    uint32_t best_check = 0u;
    int rep;

    memset(&hooks, 0, sizeof(hooks));
    memset(mem, 0, SRAM_GUEST_BYTES);
    md_runtime_init(&g_rt, mem, &hooks);
    md_jit_init(&g_jit, g_jit_code, sizeof(g_jit_code));
    md_runtime_load_com(&g_rt, image, image_size, 0x0000u);
    if (prepare != NULL) prepare(mem);

    memcpy(backup, mem, SRAM_GUEST_BYTES);
    snapshot = g_rt;

    /* Untimed prime: translate every block reached by one complete run. */
    st = md_jit_run(&g_jit, &g_rt, 10000000u);
    if (st != MD_STOP_HALT || (check_fn != NULL && check_fn(mem) == 0u)) {
        *us_best = 0u;
        *check = 0u;
        memset(delta, 0, sizeof(*delta));
        return g_rt.instructions;
    }

    c0 = g_jit.compiles;
    h0 = g_jit.hits;
    m0 = g_jit.misses;
    e0 = g_jit.native_entries;
    d0 = g_jit.direct_instructions;
    f0 = g_jit.fallback_instructions;
    re0 = g_jit.resident_entries;
    ri0 = g_jit.resident_instructions;

    for (rep = 0; rep < 3; ++rep) {
        uint64_t t0, t1, us;
        uint32_t value;

        memcpy(mem, backup, SRAM_GUEST_BYTES);
        warm_restore_runtime(&snapshot, mem);

        t0 = time_us_64();
        st = md_jit_run(&g_jit, &g_rt, 10000000u);
        t1 = time_us_64();

        us = t1 - t0;
        value = st == MD_STOP_HALT && check_fn != NULL ? check_fn(mem) : 0u;
        if (us < best) {
            best = us;
            best_instr = g_rt.instructions;
            best_check = value;
        }
    }

    delta->compiles = g_jit.compiles - c0;
    delta->hits = g_jit.hits - h0;
    delta->misses = g_jit.misses - m0;
    delta->entries = g_jit.native_entries - e0;
    delta->direct = g_jit.direct_instructions - d0;
    delta->fallback = g_jit.fallback_instructions - f0;
    delta->resident_entries = g_jit.resident_entries - re0;
    delta->resident_instructions = g_jit.resident_instructions - ri0;

    *us_best = best;
    *check = best_check;
    return best_instr;
}

static void print_jit_warm_delta(const WarmJitDelta *d)
{
    say("[jit-warm] delta/3-runs: compile=%llu hit=%llu miss=%llu entry=%llu direct=%llu fallback=%llu resident-entry=%llu resident-instr=%llu code=%lu B\n",
        (unsigned long long)d->compiles,
        (unsigned long long)d->hits,
        (unsigned long long)d->misses,
        (unsigned long long)d->entries,
        (unsigned long long)d->direct,
        (unsigned long long)d->fallback,
        (unsigned long long)d->resident_entries,
        (unsigned long long)d->resident_instructions,
        (unsigned long)g_jit.code_used);
}

/* One timed run. Returns guest instructions, fills us and the result check. */
static uint64_t run_once(uint8_t *mem, int memloop, Engine eng, uint64_t *us, uint32_t *check)
{
    MdHooks hooks;
    uint64_t t0, t1;
    MdStopReason st = MD_STOP_NONE;

    memset(&hooks, 0, sizeof(hooks));
    memset(mem, 0, SRAM_GUEST_BYTES);          /* the only range these touch */
    md_runtime_init(&g_rt, mem, &hooks);
    if (eng == ENG_CACHE) {
        md_block_cache_init(&g_cache);
        md_runtime_set_block_cache(&g_rt, &g_cache);
    }
    if (eng == ENG_JIT) md_jit_init(&g_jit, g_jit_code, sizeof(g_jit_code));

    if (eng != ENG_AOT) {
        if (memloop) md_runtime_load_com(&g_rt, kMemloop, sizeof(kMemloop), 0x0000u);
        else md_runtime_load_com(&g_rt, kLoop, sizeof(kLoop), 0x0000u);
    }

    t0 = time_us_64();
    switch (eng) {
        case ENG_STEP:
            while (g_rt.stop_reason == MD_STOP_NONE) (void)md_interp_step(&g_rt);
            st = g_rt.stop_reason;
            break;
        case ENG_THREADED:
            st = md_interp_run(&g_rt, 10000000u);
            break;
        case ENG_CSRUN:                 /* M16 DOS loop engine; CS never changes here */
            st = md_interp_run_until_cs_change(&g_rt, 10000000u);
            break;
        case ENG_CACHE:
            st = md_interp_run_cached(&g_rt, &g_cache, 10000000u);
            break;
        case ENG_AOT:
            st = memloop ? md_recomp_memloop(&g_rt, 0x0000u, 10000000u)
                         : md_recomp_loop(&g_rt, 0x0000u, 10000000u);
            break;
        case ENG_JIT:
            st = md_jit_run(&g_jit, &g_rt, 10000000u);
            break;
    }
    t1 = time_us_64();

    *us = t1 - t0;
    *check = (st == MD_STOP_HALT) ? (memloop ? window_hash(mem) : g_rt.cpu.r[MD_X86_CX] + 1u) : 0u;
    return g_rt.instructions;
}

static uint32_t loop_warm_check(const uint8_t *mem)
{
    (void)mem;
    return g_rt.cpu.r[MD_X86_CX] + 1u;
}

static uint32_t memloop_warm_check(const uint8_t *mem)
{
    return window_hash(mem);
}


static void print_jit_stats(void);

static void checksum_fill(uint8_t *mem)
{
    uint32_t i;
    for (i = 0u; i < 0x8000u; ++i)
        mem[0x2000u + i] = (uint8_t)(i * 37u + 11u);
}

static uint32_t checksum_check(void)
{
    uint32_t h = ((uint32_t)g_rt.cpu.r[MD_X86_DX] << 16) |
                 (uint32_t)g_rt.cpu.r[MD_X86_AX];
    h ^= ((uint32_t)g_rt.cpu.r[MD_X86_SI] << 1);
    h ^= g_rt.cpu.r[MD_X86_CX];
    return h != 0u ? h : 1u;
}

static uint32_t checksum_warm_check(const uint8_t *mem)
{
    (void)mem;
    return checksum_check();
}

static uint64_t run_checksum_once(uint8_t *mem, Engine eng, uint64_t *us, uint32_t *check)
{
    MdHooks hooks;
    MdStopReason st = MD_STOP_NONE;
    uint64_t t0, t1;

    memset(&hooks, 0, sizeof(hooks));
    memset(mem, 0, SRAM_GUEST_BYTES);
    md_runtime_init(&g_rt, mem, &hooks);
    if (eng == ENG_CACHE) {
        md_block_cache_init(&g_cache);
        md_runtime_set_block_cache(&g_rt, &g_cache);
    }
    if (eng == ENG_JIT) md_jit_init(&g_jit, g_jit_code, sizeof(g_jit_code));
    if (eng != ENG_AOT) md_runtime_load_com(&g_rt, kChecksum, sizeof(kChecksum), 0x0000u);
    checksum_fill(mem);

    t0 = time_us_64();
    switch (eng) {
        case ENG_STEP:
            while (g_rt.stop_reason == MD_STOP_NONE) (void)md_interp_step(&g_rt);
            st = g_rt.stop_reason;
            break;
        case ENG_THREADED: st = md_interp_run(&g_rt, 10000000u); break;
        case ENG_CSRUN: st = md_interp_run_until_cs_change(&g_rt, 10000000u); break;
        case ENG_CACHE: st = md_interp_run_cached(&g_rt, &g_cache, 10000000u); break;
        case ENG_AOT: st = md_recomp_checksum(&g_rt, 0x0000u, 10000000u); break;
        case ENG_JIT: st = md_jit_run(&g_jit, &g_rt, 10000000u); break;
    }
    t1 = time_us_64();

    *us = t1 - t0;
    *check = st == MD_STOP_HALT ? checksum_check() : 0u;
    return g_rt.instructions;
}

static void bench_checksum(void)
{
    static const char *const kGuest[] = { "SRAM ", "PSRAM" };
    uint8_t *const guests[2] = { g_sram_guest, g_psram_guest };
    uint32_t reference = 0u;
    int g, e;

    say("[bench] checksum is the M21 AOT/JIT/resident-interpreter convergence workload.\n");
    for (g = 0; g < 2; ++g) {
        for (e = ENG_STEP; e <= ENG_JIT; ++e) {
            uint64_t us_best = UINT64_MAX, instr = 0u;
            uint32_t check = 0u;
            int rep;
            for (rep = 0; rep < 3; ++rep) {
                uint64_t us;
                instr = run_checksum_once(guests[g], (Engine)e, &us, &check);
                if (us < us_best) us_best = us;
            }
            if (reference == 0u) reference = check;
            say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                "checksum", kGuest[g], kEngine[e],
                (unsigned long long)instr, (unsigned long long)us_best,
                us_best ? (double)instr / (double)us_best : 0.0,
                (check != 0u && check == reference) ? "ok" : "FAIL");
            if (e == ENG_CACHE) {
                say("[cache] region-entry=%llu region-instr=%llu\n",
                    (unsigned long long)g_cache.region_entries,
                    (unsigned long long)g_cache.region_instructions);
            }
            if (e == ENG_JIT) {
                WarmJitDelta delta;
                uint64_t warm_us;
                uint32_t warm_check;
                uint64_t warm_instr;
                print_jit_stats();
                warm_instr = run_jit_warm(guests[g], kChecksum, sizeof(kChecksum),
                                          checksum_fill, checksum_warm_check,
                                          &warm_us, &warm_check, &delta);
                say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                    "checksum", kGuest[g], "jit-warm",
                    (unsigned long long)warm_instr, (unsigned long long)warm_us,
                    warm_us ? (double)warm_instr / (double)warm_us : 0.0,
                    (warm_check != 0u && warm_check == reference) ? "ok" : "FAIL");
                print_jit_warm_delta(&delta);
            }
        }
    }
}

static uint32_t regionmix_check(const uint8_t *mem)
{
    uint32_t h = window_hash(mem);
    h ^= ((uint32_t)g_rt.cpu.r[MD_X86_BX] << 16) | g_rt.cpu.r[MD_X86_AX];
    h ^= ((uint32_t)g_rt.cpu.r[MD_X86_SI] << 1);
    return h != 0u ? h : 1u;
}

static uint64_t run_regionmix_once(uint8_t *mem, Engine eng, uint64_t *us, uint32_t *check)
{
    MdHooks hooks;
    MdStopReason st = MD_STOP_NONE;
    uint64_t t0, t1;

    memset(&hooks, 0, sizeof(hooks));
    memset(mem, 0, SRAM_GUEST_BYTES);
    md_runtime_init(&g_rt, mem, &hooks);
    if (eng == ENG_CACHE) {
        md_block_cache_init(&g_cache);
        md_runtime_set_block_cache(&g_rt, &g_cache);
    }
    if (eng == ENG_JIT) md_jit_init(&g_jit, g_jit_code, sizeof(g_jit_code));
    md_runtime_load_com(&g_rt, kRegionmix, sizeof(kRegionmix), 0x0000u);

    t0 = time_us_64();
    switch (eng) {
        case ENG_STEP:
            while (g_rt.stop_reason == MD_STOP_NONE) (void)md_interp_step(&g_rt);
            st = g_rt.stop_reason;
            break;
        case ENG_THREADED: st = md_interp_run(&g_rt, 10000000u); break;
        case ENG_CSRUN: st = md_interp_run_until_cs_change(&g_rt, 10000000u); break;
        case ENG_CACHE: st = md_interp_run_cached(&g_rt, &g_cache, 10000000u); break;
        case ENG_JIT: st = md_jit_run(&g_jit, &g_rt, 10000000u); break;
        case ENG_AOT: break;              /* no static-AOT regionmix fixture yet */
    }
    t1 = time_us_64();
    *us = t1 - t0;
    *check = st == MD_STOP_HALT ? regionmix_check(mem) : 0u;
    return g_rt.instructions;
}

static void print_jit_stats(void)
{
    say("[jit]   compile=%llu hit=%llu miss=%llu entry=%llu direct=%llu fallback=%llu invalid=%llu flush=%llu code=%lu B local-edges=%llu helper-sites=%llu\n",
        (unsigned long long)g_jit.compiles,
        (unsigned long long)g_jit.hits,
        (unsigned long long)g_jit.misses,
        (unsigned long long)g_jit.native_entries,
        (unsigned long long)g_jit.direct_instructions,
        (unsigned long long)g_jit.fallback_instructions,
        (unsigned long long)g_jit.invalidations,
        (unsigned long long)g_jit.flushes,
        (unsigned long)g_jit.code_used,
        (unsigned long long)g_jit.local_edges,
        (unsigned long long)g_jit.helper_sites);
    say("[jit]   resident-regions=%llu resident-entry=%llu resident-instr=%llu generic-regions=%llu generic-entry=%llu generic-instr=%llu\n",
        (unsigned long long)g_jit.resident_regions,
        (unsigned long long)g_jit.resident_entries,
        (unsigned long long)g_jit.resident_instructions,
        (unsigned long long)g_jit.generic_regions,
        (unsigned long long)g_jit.generic_entries,
        (unsigned long long)g_jit.generic_instructions);
    say("[jit]   cfg-regions=%llu cfg-entry=%llu cfg-instr=%llu cfg-edges=%llu returns=%llu cs-exit=%llu control=%llu compile-fb=%llu budget-fb=%llu zero-fb=%llu cold-fb=%llu\n",
        (unsigned long long)g_jit.cfg_regions,
        (unsigned long long)g_jit.cfg_entries,
        (unsigned long long)g_jit.cfg_instructions,
        (unsigned long long)g_jit.cfg_internal_edges,
        (unsigned long long)g_jit.native_returns,
        (unsigned long long)g_jit.cs_change_exits,
        (unsigned long long)g_jit.control_instructions,
        (unsigned long long)g_jit.compile_fail_fallbacks,
        (unsigned long long)g_jit.budget_fallbacks,
        (unsigned long long)g_jit.zero_progress_fallbacks,
        (unsigned long long)g_jit.cold_fallbacks);
}

static void bench_regionmix(void)
{
    static const char *const kGuest[] = { "SRAM ", "PSRAM" };
    uint8_t *const guests[2] = { g_sram_guest, g_psram_guest };
    int g, e;

    say("[bench] regionmix has no static-AOT row; it is the M20 generic-region proof.\n");
    for (g = 0; g < 2; ++g) {
        uint32_t reference = 0u;
        for (e = ENG_STEP; e <= ENG_JIT; ++e) {
            uint64_t us_best = UINT64_MAX, instr = 0u;
            uint32_t check = 0u;
            int rep;
            if (e == ENG_AOT) continue;
            for (rep = 0; rep < 3; ++rep) {
                uint64_t us;
                instr = run_regionmix_once(guests[g], (Engine)e, &us, &check);
                if (us < us_best) us_best = us;
            }
            if (reference == 0u) reference = check;
            say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                "regionmix", kGuest[g], kEngine[e],
                (unsigned long long)instr, (unsigned long long)us_best,
                us_best ? (double)instr / (double)us_best : 0.0,
                (check != 0u && check == reference) ? "ok" : "FAIL");
            if (e == ENG_JIT) {
                WarmJitDelta delta;
                uint64_t warm_us;
                uint32_t warm_check;
                uint64_t warm_instr;
                print_jit_stats();
                warm_instr = run_jit_warm(guests[g], kRegionmix, sizeof(kRegionmix),
                                          NULL, regionmix_check,
                                          &warm_us, &warm_check, &delta);
                say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                    "regionmix", kGuest[g], "jit-warm",
                    (unsigned long long)warm_instr, (unsigned long long)warm_us,
                    warm_us ? (double)warm_instr / (double)warm_us : 0.0,
                    (warm_check != 0u && warm_check == reference) ? "ok" : "FAIL");
                print_jit_warm_delta(&delta);
            }
        }
    }
}

static uint32_t branchmix_check(void)
{
    uint32_t h = ((uint32_t)g_rt.cpu.r[MD_X86_BX] << 16) | g_rt.cpu.r[MD_X86_AX];
    h ^= g_rt.cpu.r[MD_X86_CX];
    return h != 0u ? h : 1u;
}

static uint32_t branchmix_warm_check(const uint8_t *mem)
{
    (void)mem;
    return branchmix_check();
}

static uint64_t run_branchmix_once(uint8_t *mem, Engine eng, uint64_t *us, uint32_t *check)
{
    MdHooks hooks;
    MdStopReason st = MD_STOP_NONE;
    uint64_t t0, t1;
    memset(&hooks, 0, sizeof(hooks));
    memset(mem, 0, SRAM_GUEST_BYTES);
    md_runtime_init(&g_rt, mem, &hooks);
    if (eng == ENG_CACHE) { md_block_cache_init(&g_cache); md_runtime_set_block_cache(&g_rt, &g_cache); }
    if (eng == ENG_JIT) md_jit_init(&g_jit, g_jit_code, sizeof(g_jit_code));
    md_runtime_load_com(&g_rt, kBranchmix, sizeof(kBranchmix), 0x0000u);
    t0 = time_us_64();
    switch (eng) {
        case ENG_STEP: while (g_rt.stop_reason == MD_STOP_NONE) (void)md_interp_step(&g_rt); st = g_rt.stop_reason; break;
        case ENG_THREADED: st = md_interp_run(&g_rt, 10000000u); break;
        case ENG_CSRUN: st = md_interp_run_until_cs_change(&g_rt, 10000000u); break;
        case ENG_CACHE: st = md_interp_run_cached(&g_rt, &g_cache, 10000000u); break;
        case ENG_JIT: st = md_jit_run(&g_jit, &g_rt, 10000000u); break;
        case ENG_AOT: break;
    }
    t1 = time_us_64();
    *us = t1 - t0;
    *check = st == MD_STOP_HALT ? branchmix_check() : 0u;
    return g_rt.instructions;
}

static void bench_branchmix(void)
{
    static const char *const kGuest[] = { "SRAM ", "PSRAM" };
    uint8_t *const guests[2] = { g_sram_guest, g_psram_guest };
    int g, e;
    say("[bench] branchmix has no static-AOT row; it is the M20.1 bounded-CFG proof.\n");
    for (g = 0; g < 2; ++g) {
        uint32_t reference = 0u;
        for (e = ENG_STEP; e <= ENG_JIT; ++e) {
            uint64_t us_best = UINT64_MAX, instr = 0u;
            uint32_t check = 0u;
            int rep;
            if (e == ENG_AOT) continue;
            for (rep = 0; rep < 3; ++rep) {
                uint64_t us;
                instr = run_branchmix_once(guests[g], (Engine)e, &us, &check);
                if (us < us_best) us_best = us;
            }
            if (reference == 0u) reference = check;
            say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                "branchmix", kGuest[g], kEngine[e],
                (unsigned long long)instr, (unsigned long long)us_best,
                us_best ? (double)instr / (double)us_best : 0.0,
                (check != 0u && check == reference) ? "ok" : "FAIL");
            if (e == ENG_JIT) {
                WarmJitDelta delta;
                uint64_t warm_us;
                uint32_t warm_check;
                uint64_t warm_instr;
                print_jit_stats();
                warm_instr = run_jit_warm(guests[g], kBranchmix, sizeof(kBranchmix),
                                          NULL, branchmix_warm_check,
                                          &warm_us, &warm_check, &delta);
                say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                    "branchmix", kGuest[g], "jit-warm",
                    (unsigned long long)warm_instr, (unsigned long long)warm_us,
                    warm_us ? (double)warm_instr / (double)warm_us : 0.0,
                    (warm_check != 0u && warm_check == reference) ? "ok" : "FAIL");
                print_jit_warm_delta(&delta);
            }
        }
    }
}

static uint32_t callmix_check(void)
{
    uint32_t h = ((uint32_t)g_rt.cpu.r[MD_X86_BX] << 16) | g_rt.cpu.r[MD_X86_CX];
    h ^= g_rt.cpu.r[MD_X86_SP];
    return h != 0u ? h : 1u;
}

static uint32_t callmix_warm_check(const uint8_t *mem)
{
    (void)mem;
    return callmix_check();
}

static uint64_t run_callmix_once(uint8_t *mem, Engine eng, uint64_t *us, uint32_t *check)
{
    MdHooks hooks; MdStopReason st=MD_STOP_NONE; uint64_t t0,t1;
    memset(&hooks,0,sizeof(hooks)); memset(mem,0,SRAM_GUEST_BYTES);
    md_runtime_init(&g_rt,mem,&hooks);
    if(eng==ENG_CACHE){md_block_cache_init(&g_cache);md_runtime_set_block_cache(&g_rt,&g_cache);}
    if(eng==ENG_JIT)md_jit_init(&g_jit,g_jit_code,sizeof(g_jit_code));
    md_runtime_load_com(&g_rt,kCallmix,sizeof(kCallmix),0x0000u);
    t0=time_us_64();
    switch(eng){
        case ENG_STEP: while(g_rt.stop_reason==MD_STOP_NONE)(void)md_interp_step(&g_rt); st=g_rt.stop_reason; break;
        case ENG_THREADED: st=md_interp_run(&g_rt,10000000u); break;
        case ENG_CSRUN: st=md_interp_run_until_cs_change(&g_rt,10000000u); break;
        case ENG_CACHE: st=md_interp_run_cached(&g_rt,&g_cache,10000000u); break;
        case ENG_JIT: st=md_jit_run(&g_jit,&g_rt,10000000u); break;
        case ENG_AOT: break;
    }
    t1=time_us_64(); *us=t1-t0; *check=st==MD_STOP_HALT?callmix_check():0u;
    return g_rt.instructions;
}

static void bench_callmix(void)
{
    static const char *const kGuest[]={"SRAM ","PSRAM"};
    uint8_t *const guests[2]={g_sram_guest,g_psram_guest};
    int g,e;
    say("[bench] callmix has no static-AOT row; it is the M20.2 CALL/RET proof.\n");
    for(g=0;g<2;++g){
        uint32_t reference=0u;
        for(e=ENG_STEP;e<=ENG_JIT;++e){
            uint64_t us_best=UINT64_MAX,instr=0u; uint32_t check=0u; int rep;
            if(e==ENG_AOT)continue;
            for(rep=0;rep<3;++rep){uint64_t us;instr=run_callmix_once(guests[g],(Engine)e,&us,&check);if(us<us_best)us_best=us;}
            if(reference==0u)reference=check;
            say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n","callmix",kGuest[g],kEngine[e],
                (unsigned long long)instr,(unsigned long long)us_best,us_best?(double)instr/(double)us_best:0.0,
                (check!=0u&&check==reference)?"ok":"FAIL");
            if(e==ENG_JIT){
                WarmJitDelta delta; uint64_t warm_us,warm_instr; uint32_t warm_check;
                print_jit_stats();
                warm_instr=run_jit_warm(guests[g],kCallmix,sizeof(kCallmix),NULL,callmix_warm_check,
                                       &warm_us,&warm_check,&delta);
                say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n","callmix",kGuest[g],"jit-warm",
                    (unsigned long long)warm_instr,(unsigned long long)warm_us,
                    warm_us?(double)warm_instr/(double)warm_us:0.0,
                    (warm_check!=0u&&warm_check==reference)?"ok":"FAIL");
                print_jit_warm_delta(&delta);
            }
        }
    }
}

static void bench(void)
{
    static const char *const kGuest[] = { "SRAM ", "PSRAM" };
    uint8_t *const guests[2] = { g_sram_guest, g_psram_guest };
    uint32_t reference[2] = { 0u, 0u };
    int g, w, e;

    say("\n[bench] microDOS RP2350 M21 compact-resident benchmark: clk %lu MHz, code %s, psram %lu KiB\n",
        (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
        MICRODOS_PICO_CODE_IN_SRAM ? "SRAM (copy_to_ram)" : "flash XIP",
        (unsigned long)(psram_get_size() / 1024u));
    say("[bench] JIT arena: %u KiB executable/descriptor SRAM\n",
        (unsigned)(sizeof(g_jit_code) / 1024u));
    say("[bench] JIT timing: jit-cold rebuilds translations each sample; jit-warm primes once then reuses them\n");
    say("[bench] state bytes: runtime=%u cache=%u jit=%u jit-block=%u jit-op=%u\n",
        (unsigned)sizeof(MdRuntime), (unsigned)sizeof(MdBlockCache),
        (unsigned)sizeof(MdJit), (unsigned)sizeof(MdJitBlock), (unsigned)sizeof(MdJitOp));
    say("[bench] %-8s %-5s %-8s %9s %10s %9s  %s\n",
        "workload", "guest", "engine", "instr", "us", "MIPS", "result");

    for (w = 0; w < 2; ++w) {
        for (g = 0; g < 2; ++g) {
            for (e = ENG_STEP; e <= ENG_JIT; ++e) {
                uint64_t us_best = UINT64_MAX, instr = 0u;
                uint32_t check = 0u;
                int rep;
                for (rep = 0; rep < 3; ++rep) {      /* best of 3: warm caches */
                    uint64_t us;
                    instr = run_once(guests[g], w, (Engine)e, &us, &check);
                    if (us < us_best) us_best = us;
                }
                if (reference[w] == 0u) reference[w] = check;
                say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                    w ? "memloop" : "loop", kGuest[g], kEngine[e],
                    (unsigned long long)instr, (unsigned long long)us_best,
                    us_best ? (double)instr / (double)us_best : 0.0,
                    (check != 0u && check == reference[w]) ? "ok" : "FAIL");
                if (e == ENG_JIT) {
                    WarmJitDelta delta;
                    uint64_t warm_us;
                    uint32_t warm_check;
                    uint64_t warm_instr;
                    const uint8_t *image = w ? kMemloop : kLoop;
                    const size_t image_size = w ? sizeof(kMemloop) : sizeof(kLoop);
                    WarmCheckFn check_fn = w ? memloop_warm_check : loop_warm_check;
                    print_jit_stats();
                    warm_instr = run_jit_warm(guests[g], image, image_size, NULL, check_fn,
                                              &warm_us, &warm_check, &delta);
                    say("[bench] %-8s %-5s %-8s %9llu %10llu %9.3f  %s\n",
                        w ? "memloop" : "loop", kGuest[g], "jit-warm",
                        (unsigned long long)warm_instr, (unsigned long long)warm_us,
                        warm_us ? (double)warm_instr / (double)warm_us : 0.0,
                        (warm_check != 0u && warm_check == reference[w]) ? "ok" : "FAIL");
                    print_jit_warm_delta(&delta);
                }
            }
        }
    }
    bench_checksum();
    bench_regionmix();
    bench_branchmix();
    bench_callmix();
    say("[bench] done. Press any key to run again.\n");
}

int main(void)
{
    const bool clock_ok = bench_set_clock();
    stdio_init_all();
    while (!stdio_usb_connected()) sleep_ms(20);
    sleep_ms(300);

    if (!psram_is_available() || !psram_check_address(&g_psram_guest[0]) ||
        !psram_check_address(&g_psram_guest[SRAM_GUEST_BYTES - 1u])) {
        say("[bench] PSRAM not available; halting.\n");
        for (;;) sleep_ms(1000);
    }
    if (!clock_ok) say("[bench] WARNING: requested clock %lu kHz failed\n", (unsigned long)MICRODOS_PICO_SYS_KHZ);
    for (;;) {
        bench();
        while (getchar_timeout_us(0) >= 0) { }      /* drain */
        (void)getchar();                            /* wait for a key */
    }
}
