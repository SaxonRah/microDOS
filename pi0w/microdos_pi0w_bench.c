/*
 * microDOS Raspberry Pi Zero 2 W bare-metal benchmark.
 *
 * Uses the same guest workload bytes and generated AOT fixtures as the RP2350
 * benchmark, but runs from Pi Normal-cacheable RAM and times with CNTPCT_EL0.
 *
 * Engines:
 *   step, threaded, cs-run, decoded block cache, static AOT, runtime JIT
 *
 * JIT rows:
 *   jit-cold  - reset translator and include compilation in each sample
 *   jit-warm  - prime once, restore guest/runtime state, reuse translations
 *
 * Every row checks guest result correctness. A fast wrong result prints FAIL.
 */

#include "microdos/block_cache.h"
#include "microdos/jit.h"
#include "microdos/runtime.h"
#include "loop_recomp.h"
#include "memloop_recomp.h"
#include "checksum_recomp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PERIPHERAL_BASE 0x3F000000UL
#define GPIO_BASE       (PERIPHERAL_BASE + 0x00200000UL)
#define AUX_BASE        (PERIPHERAL_BASE + 0x00215000UL)

#define GPFSEL1         (GPIO_BASE + 0x04UL)
#define GPPUD           (GPIO_BASE + 0x94UL)
#define GPPUDCLK0       (GPIO_BASE + 0x98UL)
#define AUX_ENABLES     (AUX_BASE + 0x04UL)
#define AUX_MU_IO       (AUX_BASE + 0x40UL)
#define AUX_MU_IER      (AUX_BASE + 0x44UL)
#define AUX_MU_IIR      (AUX_BASE + 0x48UL)
#define AUX_MU_LCR      (AUX_BASE + 0x4CUL)
#define AUX_MU_MCR      (AUX_BASE + 0x50UL)
#define AUX_MU_LSR      (AUX_BASE + 0x54UL)
#define AUX_MU_CNTL     (AUX_BASE + 0x60UL)
#define AUX_MU_BAUD     (AUX_BASE + 0x68UL)

#define BENCH_GUEST_BYTES (64u * 1024u)
#define JIT_CODE_BYTES    (32u * 1024u)
#define RUN_BUDGET        10000000u

static uint8_t g_guest[BENCH_GUEST_BYTES] __attribute__((aligned(64)));
static uint8_t g_backup[BENCH_GUEST_BYTES] __attribute__((aligned(64)));
static uint8_t g_jit_code[JIT_CODE_BYTES] __attribute__((aligned(64)));

static MdRuntime g_rt;
static MdBlockCache g_cache;
static MdJit g_jit;

static const uint8_t kLoop[] = {
    0xB9,0xFF,0xFF, 0x49, 0x75,0xFD, 0xF4
};

static const uint8_t kMemloop[] = {
    0xB9,0x00,0x80, 0xBE,0x00,0x80, 0x8A,0x04, 0x04,0x03, 0x88,0x04,
    0x83,0xC6,0x61, 0x81,0xCE,0x00,0x80, 0x49,0x75,0xF0, 0xF4
};

static const uint8_t kChecksum[] = {
    0xBE,0x00,0x20,
    0xB9,0x00,0x40,
    0x31,0xD2,
    0xFC,
    0xAD,
    0x03,0xD0,
    0xE2,0xFB,
    0xF4
};

static const uint8_t kRegionmix[] = {
    0xB9,0x00,0x80,
    0xBE,0x00,0x80,
    0xBB,0x34,0x12,
    0x8A,0x04,
    0x04,0x03,
    0x81,0xF3,0x57,0x13,
    0x83,0xC3,0x05,
    0x88,0x04,
    0x83,0xC6,0x61,
    0x81,0xCE,0x00,0x80,
    0x49,
    0x75,0xE9,
    0xF4
};

static const uint8_t kBranchmix[] = {
    0xB9,0x00,0x80,
    0xB8,0x00,0x00,
    0xBB,0x00,0x00,
    0x83,0xF0,0x01,
    0x83,0xF8,0x00,
    0x74,0x03,
    0x83,0xC3,0x03,
    0x83,0xCB,0x00,
    0x49,
    0x75,0xEF,
    0xF4
};

static const uint8_t kCallmix[] = {
    0xB9,0x00,0x80,
    0xBB,0x00,0x00,
    0xE8,0x04,0x00,
    0x49,
    0x75,0xFA,
    0xF4,
    0x83,0xC3,0x03,
    0xC3
};

typedef enum Engine {
    ENG_STEP,
    ENG_THREADED,
    ENG_CSRUN,
    ENG_CACHE,
    ENG_AOT,
    ENG_JIT
} Engine;

static const char *const kEngine[] = {
    "step", "threaded", "cs-run", "cache", "aot", "jit-cold"
};

typedef MdStopReason (*AotRunFn)(MdRuntime *, uint16_t, uint64_t);

typedef struct Workload {
    const char *name;
    const uint8_t *image;
    size_t image_size;
    void (*prepare)(uint8_t *);
    uint32_t (*check)(const uint8_t *);
    AotRunFn aot;
} Workload;

typedef struct JitSnapshot {
    MdRuntime runtime;
    uint64_t compiles;
    uint64_t hits;
    uint64_t misses;
    uint64_t entries;
    uint64_t direct;
    uint64_t fallback;
    uint64_t resident_entries;
    uint64_t resident_instructions;
} JitSnapshot;

static inline void mmio_write(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

static inline uint32_t mmio_read(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}

static void delay_cycles(unsigned count)
{
    while (count-- != 0u) __asm__ volatile("nop");
}

static void uart_init(void)
{
    uint32_t r;

    mmio_write(AUX_ENABLES, mmio_read(AUX_ENABLES) | 1u);
    mmio_write(AUX_MU_CNTL, 0u);
    mmio_write(AUX_MU_IER, 0u);
    mmio_write(AUX_MU_LCR, 3u);
    mmio_write(AUX_MU_MCR, 0u);
    mmio_write(AUX_MU_IIR, 0xC6u);
    mmio_write(AUX_MU_BAUD, 270u);

    r = mmio_read(GPFSEL1);
    r &= ~((7u << 12) | (7u << 15));
    r |=  ((2u << 12) | (2u << 15));
    mmio_write(GPFSEL1, r);

    mmio_write(GPPUD, 0u);
    delay_cycles(150u);
    mmio_write(GPPUDCLK0, (1u << 14) | (1u << 15));
    delay_cycles(150u);
    mmio_write(GPPUDCLK0, 0u);

    mmio_write(AUX_MU_CNTL, 3u);
}

static void uart_putc_raw(uint8_t ch)
{
    while ((mmio_read(AUX_MU_LSR) & 0x20u) == 0u) { }
    mmio_write(AUX_MU_IO, ch);
}

static void uart_putc(uint8_t ch)
{
    if (ch == '\n') uart_putc_raw('\r');
    uart_putc_raw(ch);
}

static void uart_puts(const char *s)
{
    while (*s != '\0') uart_putc((uint8_t)*s++);
}

static void uart_put_u64(uint64_t value)
{
    char buf[24];
    unsigned n = 0u;

    if (value == 0u) {
        uart_putc_raw('0');
        return;
    }

    while (value != 0u && n < sizeof(buf)) {
        buf[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (n != 0u) uart_putc_raw((uint8_t)buf[--n]);
}

static void uart_put_fixed3(uint64_t milli_value)
{
    const uint64_t whole = milli_value / 1000u;
    const unsigned frac = (unsigned)(milli_value % 1000u);

    uart_put_u64(whole);
    uart_putc_raw('.');
    uart_putc_raw((uint8_t)('0' + ((frac / 100u) % 10u)));
    uart_putc_raw((uint8_t)('0' + ((frac / 10u) % 10u)));
    uart_putc_raw((uint8_t)('0' + (frac % 10u)));
}

static inline uint64_t timer_ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, CNTPCT_EL0" : "=r"(v) :: "memory");
    return v;
}

static inline uint64_t timer_hz(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v;
}

static uint64_t ticks_to_us(uint64_t ticks)
{
    const uint64_t hz = timer_hz();
    return hz != 0u ? (ticks * 1000000u) / hz : 0u;
}

static uint32_t window_hash(const uint8_t *mem)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0x8000u; i < 0x10000u; ++i)
        h = (h ^ mem[i]) * 16777619u;
    return h;
}

static void checksum_fill(uint8_t *mem)
{
    uint32_t i;
    for (i = 0u; i < 0x8000u; ++i)
        mem[0x2000u + i] = (uint8_t)(i * 37u + 11u);
}

static uint32_t check_loop(const uint8_t *mem)
{
    (void)mem;
    return (uint32_t)g_rt.cpu.r[MD_X86_CX] + 1u;
}

static uint32_t check_memloop(const uint8_t *mem)
{
    return window_hash(mem);
}

static uint32_t check_checksum(const uint8_t *mem)
{
    uint32_t h;
    (void)mem;
    h = ((uint32_t)g_rt.cpu.r[MD_X86_DX] << 16) |
        (uint32_t)g_rt.cpu.r[MD_X86_AX];
    h ^= ((uint32_t)g_rt.cpu.r[MD_X86_SI] << 1);
    h ^= g_rt.cpu.r[MD_X86_CX];
    return h != 0u ? h : 1u;
}

static uint32_t check_regionmix(const uint8_t *mem)
{
    uint32_t h = window_hash(mem);
    h ^= ((uint32_t)g_rt.cpu.r[MD_X86_BX] << 16) |
         (uint32_t)g_rt.cpu.r[MD_X86_AX];
    h ^= ((uint32_t)g_rt.cpu.r[MD_X86_SI] << 1);
    return h != 0u ? h : 1u;
}

static uint32_t check_branchmix(const uint8_t *mem)
{
    uint32_t h;
    (void)mem;
    h = ((uint32_t)g_rt.cpu.r[MD_X86_BX] << 16) |
        (uint32_t)g_rt.cpu.r[MD_X86_AX];
    h ^= g_rt.cpu.r[MD_X86_CX];
    return h != 0u ? h : 1u;
}

static uint32_t check_callmix(const uint8_t *mem)
{
    uint32_t h;
    (void)mem;
    h = ((uint32_t)g_rt.cpu.r[MD_X86_BX] << 16) |
        (uint32_t)g_rt.cpu.r[MD_X86_CX];
    h ^= g_rt.cpu.r[MD_X86_SP];
    return h != 0u ? h : 1u;
}

static const Workload kWorkloads[] = {
    { "loop",      kLoop,      sizeof(kLoop),      NULL,          check_loop,      md_recomp_loop },
    { "memloop",   kMemloop,   sizeof(kMemloop),   NULL,          check_memloop,   md_recomp_memloop },
    { "checksum",  kChecksum,  sizeof(kChecksum),  checksum_fill, check_checksum,  md_recomp_checksum },
    { "regionmix", kRegionmix, sizeof(kRegionmix), NULL,          check_regionmix, NULL },
    { "branchmix", kBranchmix, sizeof(kBranchmix), NULL,          check_branchmix, NULL },
    { "callmix",   kCallmix,   sizeof(kCallmix),   NULL,          check_callmix,   NULL }
};

static void restore_runtime(const MdRuntime *snapshot)
{
    g_rt = *snapshot;
    g_rt.cpu.memory = g_guest;
    g_rt.cpu.code_page_generation = g_rt.code_page_generation;
    g_rt.cpu.code_page_executable = g_rt.code_page_executable;
    g_rt.cpu.code_write_epoch = &g_rt.code_write_epoch;
    g_rt.cpu.aot_guards = g_rt.aot_slots;
    g_rt.cpu.aot_page_owner = g_rt.aot_page_owner;
    g_rt.cpu.aot_live_bits = g_rt.aot_live_bits;
}

static uint64_t run_once(const Workload *w, Engine eng, uint64_t *us, uint32_t *check)
{
    MdHooks hooks;
    MdStopReason st = MD_STOP_NONE;
    uint64_t t0, t1;

    memset(&hooks, 0, sizeof(hooks));
    memset(g_guest, 0, sizeof(g_guest));
    md_runtime_init(&g_rt, g_guest, &hooks);

    if (eng == ENG_CACHE) {
        md_block_cache_init(&g_cache);
        md_runtime_set_block_cache(&g_rt, &g_cache);
    }

    if (eng == ENG_JIT)
        md_jit_init(&g_jit, g_jit_code, sizeof(g_jit_code));

    if (eng != ENG_AOT)
        md_runtime_load_com(&g_rt, w->image, w->image_size, 0x0000u);

    if (w->prepare != NULL) w->prepare(g_guest);

    t0 = timer_ticks();

    switch (eng) {
        case ENG_STEP:
            while (g_rt.stop_reason == MD_STOP_NONE)
                (void)md_interp_step(&g_rt);
            st = g_rt.stop_reason;
            break;

        case ENG_THREADED:
            st = md_interp_run(&g_rt, RUN_BUDGET);
            break;

        case ENG_CSRUN:
            st = md_interp_run_until_cs_change(&g_rt, RUN_BUDGET);
            break;

        case ENG_CACHE:
            st = md_interp_run_cached(&g_rt, &g_cache, RUN_BUDGET);
            break;

        case ENG_AOT:
            if (w->aot != NULL)
                st = w->aot(&g_rt, 0x0000u, RUN_BUDGET);
            else
                st = MD_STOP_FAULT;
            break;

        case ENG_JIT:
            st = md_jit_run(&g_jit, &g_rt, RUN_BUDGET);
            break;
    }

    t1 = timer_ticks();

    *us = ticks_to_us(t1 - t0);
    *check = st == MD_STOP_HALT ? w->check(g_guest) : 0u;
    return g_rt.instructions;
}

static uint64_t run_jit_warm(const Workload *w, uint64_t *us_best, uint32_t *check,
                             JitSnapshot *delta)
{
    MdHooks hooks;
    MdRuntime snapshot;
    MdStopReason st;
    JitSnapshot before;
    uint64_t best = UINT64_MAX;
    uint64_t best_instr = 0u;
    uint32_t best_check = 0u;
    int rep;

    memset(&hooks, 0, sizeof(hooks));
    memset(g_guest, 0, sizeof(g_guest));
    md_runtime_init(&g_rt, g_guest, &hooks);
    md_jit_init(&g_jit, g_jit_code, sizeof(g_jit_code));
    md_runtime_load_com(&g_rt, w->image, w->image_size, 0x0000u);
    if (w->prepare != NULL) w->prepare(g_guest);

    memcpy(g_backup, g_guest, sizeof(g_backup));
    snapshot = g_rt;

    st = md_jit_run(&g_jit, &g_rt, RUN_BUDGET);
    if (st != MD_STOP_HALT || w->check(g_guest) == 0u) {
        *us_best = 0u;
        *check = 0u;
        memset(delta, 0, sizeof(*delta));
        return g_rt.instructions;
    }

    before.compiles = g_jit.compiles;
    before.hits = g_jit.hits;
    before.misses = g_jit.misses;
    before.entries = g_jit.native_entries;
    before.direct = g_jit.direct_instructions;
    before.fallback = g_jit.fallback_instructions;
    before.resident_entries = g_jit.resident_entries;
    before.resident_instructions = g_jit.resident_instructions;

    for (rep = 0; rep < 3; ++rep) {
        uint64_t t0, t1, us;
        uint32_t value;

        memcpy(g_guest, g_backup, sizeof(g_backup));
        restore_runtime(&snapshot);

        t0 = timer_ticks();
        st = md_jit_run(&g_jit, &g_rt, RUN_BUDGET);
        t1 = timer_ticks();

        us = ticks_to_us(t1 - t0);
        value = st == MD_STOP_HALT ? w->check(g_guest) : 0u;

        if (us < best) {
            best = us;
            best_instr = g_rt.instructions;
            best_check = value;
        }
    }

    delta->compiles = g_jit.compiles - before.compiles;
    delta->hits = g_jit.hits - before.hits;
    delta->misses = g_jit.misses - before.misses;
    delta->entries = g_jit.native_entries - before.entries;
    delta->direct = g_jit.direct_instructions - before.direct;
    delta->fallback = g_jit.fallback_instructions - before.fallback;
    delta->resident_entries = g_jit.resident_entries - before.resident_entries;
    delta->resident_instructions = g_jit.resident_instructions - before.resident_instructions;

    *us_best = best;
    *check = best_check;
    return best_instr;
}

static void print_row(const char *workload, const char *engine,
                      uint64_t instr, uint64_t us, uint32_t ok)
{
    uint64_t mips_milli = us != 0u ? (instr * 1000u) / us : 0u;

    uart_puts("[bench] ");
    uart_puts(workload);
    uart_puts(" ");
    uart_puts(engine);
    uart_puts(" instr=");
    uart_put_u64(instr);
    uart_puts(" us=");
    uart_put_u64(us);
    uart_puts(" MIPS=");
    uart_put_fixed3(mips_milli);
    uart_puts(ok ? " ok\n" : " FAIL\n");
}

static void print_jit_stats(const char *tag)
{
    uart_puts("[jit-");
    uart_puts(tag);
    uart_puts("] compile=");
    uart_put_u64(g_jit.compiles);
    uart_puts(" hit=");
    uart_put_u64(g_jit.hits);
    uart_puts(" miss=");
    uart_put_u64(g_jit.misses);
    uart_puts(" entry=");
    uart_put_u64(g_jit.native_entries);
    uart_puts(" direct=");
    uart_put_u64(g_jit.direct_instructions);
    uart_puts(" fallback=");
    uart_put_u64(g_jit.fallback_instructions);
    uart_puts(" resident-entry=");
    uart_put_u64(g_jit.resident_entries);
    uart_puts(" resident-instr=");
    uart_put_u64(g_jit.resident_instructions);
    uart_puts(" code=");
    uart_put_u64(g_jit.code_used);
    uart_puts(" helpers=");
    uart_put_u64(g_jit.helper_sites);
    uart_puts(" flush=");
    uart_put_u64(g_jit.flushes);
    uart_putc('\n');
}

static void print_warm_delta(const JitSnapshot *d)
{
    uart_puts("[jit-warm-delta] compile=");
    uart_put_u64(d->compiles);
    uart_puts(" hit=");
    uart_put_u64(d->hits);
    uart_puts(" miss=");
    uart_put_u64(d->misses);
    uart_puts(" entry=");
    uart_put_u64(d->entries);
    uart_puts(" direct=");
    uart_put_u64(d->direct);
    uart_puts(" fallback=");
    uart_put_u64(d->fallback);
    uart_puts(" resident-entry=");
    uart_put_u64(d->resident_entries);
    uart_puts(" resident-instr=");
    uart_put_u64(d->resident_instructions);
    uart_putc('\n');
}

static void run_benchmarks(void)
{
    size_t wi;

    uart_puts("\n[bench] microDOS Pi Zero 2 W native benchmark\n");
    uart_puts("[bench] timer-hz=");
    uart_put_u64(timer_hz());
    uart_puts(" guest=64KiB RAM jit-arena=32KiB\n");
    uart_puts("[bench] same workload bytes as Pico M21 benchmark\n");
    uart_puts("[bench] best-of-3 per cold engine; warm JIT primes once and reuses translations\n\n");

    for (wi = 0u; wi < sizeof(kWorkloads) / sizeof(kWorkloads[0]); ++wi) {
        const Workload *w = &kWorkloads[wi];
        uint32_t reference = 0u;
        int e;

        uart_puts("[workload] ");
        uart_puts(w->name);
        uart_putc('\n');

        for (e = ENG_STEP; e <= ENG_JIT; ++e) {
            uint64_t best_us = UINT64_MAX;
            uint64_t best_instr = 0u;
            uint32_t best_check = 0u;
            int rep;

            if (e == ENG_AOT && w->aot == NULL) continue;

            for (rep = 0; rep < 3; ++rep) {
                uint64_t us;
                uint64_t instr;
                uint32_t check;

                instr = run_once(w, (Engine)e, &us, &check);
                if (us < best_us) {
                    best_us = us;
                    best_instr = instr;
                    best_check = check;
                }
            }

            if (reference == 0u && best_check != 0u) reference = best_check;

            print_row(w->name, kEngine[e], best_instr, best_us,
                      best_check != 0u && best_check == reference);

            if (e == ENG_JIT) {
                JitSnapshot delta;
                uint64_t warm_us;
                uint64_t warm_instr;
                uint32_t warm_check;

                print_jit_stats("cold");

                warm_instr = run_jit_warm(w, &warm_us, &warm_check, &delta);
                print_row(w->name, "jit-warm", warm_instr, warm_us,
                          warm_check != 0u && warm_check == reference);
                print_jit_stats("warm");
                print_warm_delta(&delta);
            }
        }

        uart_putc('\n');
    }

    uart_puts("[bench] COMPLETE\n");
}

void kernel_main(void)
{
    uint64_t currentel;
    uint64_t sctlr;

    uart_init();

    currentel = 0u;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(currentel));
    __asm__ volatile("mrs %0, SCTLR_EL2" : "=r"(sctlr));

    uart_puts("\n[microDOS Pi benchmark]\n");
    uart_puts("[cpu] CurrentEL=");
    uart_put_u64((currentel >> 2) & 3u);
    uart_puts(" SCTLR_EL2=0x");
    {
        static const char hex[] = "0123456789ABCDEF";
        int shift;
        for (shift = 60; shift >= 0; shift -= 4)
            uart_putc_raw((uint8_t)hex[(sctlr >> shift) & 0x0fu]);
    }
    uart_putc('\n');

    run_benchmarks();

    for (;;) __asm__ volatile("wfe");
}
