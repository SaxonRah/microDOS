/*
 * microDOS M25 on-device translator check (RP2350 / Cortex-M33).
 *
 *   1. Differential test: random 8086 programs run through md_interp_run()
 *      and md_tr_run() from identical state; every register, flag, retired
 *      count, stop reason and all 1 MiB of guest memory must match.
 *      Guest RAM lives in PSRAM, tested both 1 MiB-aligned (BFI addressing)
 *      and unaligned (UBFX+ADD addressing). Generated code runs from SRAM.
 *   2. Benchmark: fixed loops timed on the hardware timer, interpreter vs
 *      translator, guest RAM in PSRAM (the production layout).
 *
 * Output goes to USB CDC. The run starts when the host opens the port
 * (DTR), prints "[translate-diff] COMPLETE result=PASS|FAIL" at the end, and
 * repeats when 'R' is received. scripts/md_translate_diff_pico.ps1 builds,
 * flashes, captures and logs it.
 */
#define MD_TRANSLATE_DIFF_NO_MAIN 1
#define MD_TRANSLATE_DIFF_PROGRESS 250u
#include "test_translate_diff.c"

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 300000
#endif
#ifndef MD_TD_CASES
#define MD_TD_CASES 1000u               /* per memory layout */
#endif
#define MD_TD_ARENA_BYTES (64u * 1024u)
#define MD_TD_BENCH_INSNS 3000000u

/* Interpreter-side guest RAM, and a 1 MiB-aligned 2 MiB region for the
   translator side (aligned at +0, unaligned at +16). 3 MiB of 8 MiB PSRAM. */
static uint8_t __uninitialized_psram("md_td_a") __attribute__((aligned(16)))
    g_mem_a[MD_X86_ADDRESS_SPACE];
static uint8_t __uninitialized_psram("md_td_b") __attribute__((aligned(MD_X86_ADDRESS_SPACE)))
    g_mem_b[2u * MD_X86_ADDRESS_SPACE];

/* Translation arena: ordinary SRAM, executable on the RP2350 (Native v2
   runs its generated code the same way). */
static uint8_t __attribute__((aligned(8))) g_arena[MD_TD_ARENA_BYTES];

static int set_clock_and_psram(void)
{
#if MICRODOS_PICO_SYS_KHZ > 0
    if (!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ, false)) return 0;
    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                               PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != 0) return 0;
    if (psram_reinitialize() != 0) return 0;
#endif
    return 1;
}

static int psram_ok(void)
{
    return psram_is_available() &&
           psram_check_address(&g_mem_a[0]) &&
           psram_check_address(&g_mem_a[MD_X86_ADDRESS_SPACE - 1u]) &&
           psram_check_address(&g_mem_b[0]) &&
           psram_check_address(&g_mem_b[2u * MD_X86_ADDRESS_SPACE - 1u]);
}

/* ---- benchmark ------------------------------------------------------------ */

typedef struct Bench {
    const char *name;
    const uint8_t *code;
    size_t size;
} Bench;

/* l: dec cx / jnz l / jmp l */
static const uint8_t kLoop[] = { 0x49, 0x75, 0xFD, 0xEB, 0xFB };
/* mov si,3000h / mov cx,1000h / xor dx,dx /
   l: mov ax,[si] / add dx,ax / add si,2 / dec cx / jnz l / jmp start */
static const uint8_t kMemsum[] = {
    0xBE, 0x00, 0x30, 0xB9, 0x00, 0x10, 0x31, 0xD2,
    0x8B, 0x04, 0x01, 0xC2, 0x83, 0xC6, 0x02, 0x49, 0x75, 0xF6, 0xEB, 0xEC
};
/* mov si,0 / l: mov al,[si] / inc si / cmp al,0FFh / jne l / jmp start
   (scans zeroed memory: the byte-scanner shape of DOS string loops) */
static const uint8_t kScan[] = {
    0xBE, 0x00, 0x00, 0x8A, 0x04, 0x46, 0x3C, 0xFF, 0x75, 0xF9, 0xEB, 0xF4
};
/* l: call f / dec cx / jnz l / jmp l / f: add bx,ax / inc ax / ret */
static const uint8_t kCallmix[] = {
    0xE8, 0x05, 0x00, 0x49, 0x75, 0xFA, 0xEB, 0xF8, 0x01, 0xC3, 0x40, 0xC3
};

static const Bench kBench[] = {
    { "dec-jnz loop", kLoop, sizeof(kLoop) },
    { "memory-sum loop", kMemsum, sizeof(kMemsum) },
    { "call/ret loop", kCallmix, sizeof(kCallmix) },
    { "byte-scan loop", kScan, sizeof(kScan) },
};

static int run_bench(const Bench *bench)
{
    static MdRuntime ri, rt;
    static MdTranslator tr;
    MdHooks hooks;
    uint64_t t0, t_interp, t_tr;
    double mi, mt;
    int same;

    memset(&hooks, 0, sizeof(hooks));

    memset(g_mem_a, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&ri, g_mem_a, &hooks);
    md_runtime_load_com(&ri, bench->code, bench->size, 0x1000u);
    t0 = time_us_64();
    (void)md_interp_run(&ri, MD_TD_BENCH_INSNS);
    t_interp = time_us_64() - t0;

    memset(g_mem_b, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, g_mem_b, &hooks);
    md_runtime_load_com(&rt, bench->code, bench->size, 0x1000u);
    if (!md_tr_init(&tr, &rt, g_arena, MD_TD_ARENA_BYTES)) {
        printf("[translate-bench] %s: translator unavailable\n", bench->name);
        return 0;
    }
    t0 = time_us_64();
    (void)md_tr_run(&tr, MD_TD_BENCH_INSNS);
    t_tr = time_us_64() - t0;

    same = memcmp(ri.cpu.r, rt.cpu.r, sizeof(ri.cpu.r)) == 0 &&
           ri.cpu.ip == rt.cpu.ip && ri.instructions == rt.instructions;
    mi = t_interp ? (double)ri.instructions / (double)t_interp : 0.0;
    mt = t_tr ? (double)rt.instructions / (double)t_tr : 0.0;
    printf("[translate-bench] %-16s interp %7.2f MIPS  translated %7.2f MIPS  x%5.2f  "
           "native=%.1f%% episodes=%lu chains=%lu state=%s\n",
           bench->name, mi, mt, mi > 0.0 ? mt / mi : 0.0,
           rt.instructions ? 100.0 * (double)tr.stats.native_instructions / (double)rt.instructions : 0.0,
           (unsigned long)tr.stats.episodes, (unsigned long)tr.stats.chains,
           same ? "match" : "MISMATCH");
    stdio_flush();
    return same;
}

/* ---- main ----------------------------------------------------------------- */

static int run_all(void)
{
    unsigned fails = 0u;
    unsigned i;
    uint64_t t0 = time_us_64();

    printf("\n[translate-diff] microDOS M25 on-device check\n");
    printf("[translate-diff] clock=%lu kHz psram=%s mem_a=%p mem_b=%p arena=%p (%u bytes)\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000u), psram_ok() ? "OK" : "FAILED",
           (void *)g_mem_a, (void *)g_mem_b, (void *)g_arena, MD_TD_ARENA_BYTES);
    stdio_flush();
    if (!psram_ok()) {
        printf("[translate-diff] COMPLETE result=FAIL (PSRAM)\n");
        stdio_flush();
        return 0;
    }

    fails += md_translate_directed(g_mem_a, g_mem_b, 0, g_arena, MD_TD_ARENA_BYTES);
    stdio_flush();
    fails += md_translate_directed(g_mem_a, g_mem_b, 1, g_arena, MD_TD_ARENA_BYTES);
    stdio_flush();
    fails += md_translate_diff_run(MD_TD_CASES, 0x4D32355Bu, 0, -1, g_mem_a, g_mem_b,
                                   g_arena, MD_TD_ARENA_BYTES);
    stdio_flush();
    fails += md_translate_diff_run(MD_TD_CASES, 0x0BADF00Du, 1, -1, g_mem_a, g_mem_b,
                                   g_arena, MD_TD_ARENA_BYTES);
    stdio_flush();

    for (i = 0u; i < sizeof(kBench) / sizeof(kBench[0]); ++i)
        if (!run_bench(&kBench[i])) ++fails;

    printf("[translate-diff] elapsed %.1f s\n", (double)(time_us_64() - t0) / 1e6);
    printf("[translate-diff] COMPLETE result=%s fails=%u\n", fails ? "FAIL" : "PASS", fails);
    stdio_flush();
    return fails == 0u;
}

int main(void)
{
    const int clocks_ok = set_clock_and_psram();
    stdio_init_all();

    for (;;) {
        int ch;
        /* wait for the host to open the port (DTR) so nothing is lost */
        while (!stdio_usb_connected()) sleep_ms(50);
        sleep_ms(500);
        if (!clocks_ok) {
            printf("[translate-diff] COMPLETE result=FAIL (clock/PSRAM setup)\n");
            stdio_flush();
        } else {
            (void)run_all();
        }
        printf("[translate-diff] send R to run again\n");
        stdio_flush();
        do {
            ch = getchar_timeout_us(100000);
        } while (ch != 'R' && ch != 'r' && stdio_usb_connected());
    }
}
