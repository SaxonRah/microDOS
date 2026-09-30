/* microDOS RP2350 benchmark firmware (M15).
 *
 * Separates the three costs that full-DOS numbers mix together:
 *   - where the ARM interpreter code runs (flash XIP vs SRAM: build variant)
 *   - where guest memory lives (SRAM vs PSRAM: both measured here)
 *   - which engine runs it (step, threaded interpreter, block cache, AOT)
 *
 * Workloads are loaded at segment 0, so every guest access stays inside
 * linear 0000-FFFF and a 64 KiB SRAM buffer is a complete guest for them.
 *   loop     CX=FFFF DEC/JNZ, register-only (131071 guest instructions)
 *   memloop  RMW over a 32 KiB window, stride 97 (229379 guest instructions)
 *
 * Every row checks that the guest produced the reference result, so a fast
 * wrong answer is reported as FAIL, not as a speedup.
 */
#include "microdos/block_cache.h"
#include "microdos/runtime.h"
#include "loop_recomp.h"
#include "memloop_recomp.h"

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

static uint8_t __attribute__((aligned(16))) g_sram_guest[SRAM_GUEST_BYTES];
static uint8_t __uninitialized_psram("md_bench_guest") __attribute__((aligned(16)))
    g_psram_guest[1u << 20];

static MdRuntime g_rt;
static MdBlockCache g_cache;

static const uint8_t kLoop[] = { 0xB9, 0xFF, 0xFF, 0x49, 0x75, 0xFD, 0xF4 };
static const uint8_t kMemloop[] = {
    0xB9,0x00,0x80, 0xBE,0x00,0x80, 0x8A,0x04, 0x04,0x03, 0x88,0x04,
    0x83,0xC6,0x61, 0x81,0xCE,0x00,0x80, 0x49, 0x75,0xF0, 0xF4
};

typedef enum { ENG_STEP, ENG_THREADED, ENG_CSRUN, ENG_CACHE, ENG_AOT } Engine;
static const char *const kEngine[] = { "step", "threaded", "cs-run", "cache", "aot" };

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
    char line[200];
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
    }
    t1 = time_us_64();

    *us = t1 - t0;
    *check = (st == MD_STOP_HALT) ? (memloop ? window_hash(mem) : g_rt.cpu.r[MD_X86_CX] + 1u) : 0u;
    return g_rt.instructions;
}

static void bench(void)
{
    static const char *const kGuest[] = { "SRAM ", "PSRAM" };
    uint8_t *const guests[2] = { g_sram_guest, g_psram_guest };
    uint32_t reference[2] = { 0u, 0u };
    int g, w, e;

    say("\n[bench] microDOS RP2350 benchmark: clk %lu MHz, code %s, psram %lu KiB\n",
        (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
        MICRODOS_PICO_CODE_IN_SRAM ? "SRAM (copy_to_ram)" : "flash XIP",
        (unsigned long)(psram_get_size() / 1024u));
    say("[bench] %-8s %-5s %-8s %9s %10s %9s  %s\n",
        "workload", "guest", "engine", "instr", "us", "MIPS", "result");

    for (w = 0; w < 2; ++w) {
        for (g = 0; g < 2; ++g) {
            for (e = ENG_STEP; e <= ENG_AOT; ++e) {
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
            }
        }
    }
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
