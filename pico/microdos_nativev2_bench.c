/*
 * microDOS Native v2 Phase 2C benchmark.
 *
 * Production questions:
 *   1. Does backward flag liveness recover the Phase-2A compute speed?
 *   2. What does the exact same native memory region cost against the actual
 *      8 MiB PSRAM path used by the 1 MiB DOS guest?
 */
#include "microdos/native_v2.h"

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 300000
#endif

#define SRAM_GUEST_BYTES (64u * 1024u)
#define PSRAM_GUEST_BYTES (1u << 20)
#define NV2_RUNS_REG 512u
#define NV2_RUNS_MEM 128u

static uint8_t __attribute__((aligned(16))) g_sram_guest[SRAM_GUEST_BYTES];
static uint8_t __uninitialized_psram("md_nv2_psram_guest")
    __attribute__((aligned(16))) g_psram_guest[PSRAM_GUEST_BYTES];

static MdX86 g_cpu;

static const uint8_t kLoop[] = {
    0xB8,0x00,0x00,
    0xB9,0x00,0x80,
    0x40,
    0x49,
    0x75,0xFC
};

static const uint8_t kRegmix[] = {
    0xB8,0x01,0x00,
    0xBB,0x03,0x00,
    0xB9,0x00,0x80,
    0x31,0xD2,
    0x03,0xC3,
    0x33,0xD8,
    0x03,0xD0,
    0x49,
    0x75,0xF7
};

static const uint8_t kMemmix[] = {
    0xBE,0x00,0x80,
    0xBF,0x00,0x00,
    0xB9,0x00,0x40,
    0x31,0xC0,
    0x8B,0x04,
    0x83,0xC0,0x01,
    0x89,0x05,
    0x8D,0x74,0x02,
    0x8D,0x7D,0x02,
    0x83,0xC8,0x00,
    0x49,
    0x75,0xED
};

typedef struct Work {
    const char *name;
    const uint8_t *image;
    size_t image_size;
    uint64_t guest_per_run;
    unsigned runs;
    MdNativeV2Code code;
} Work;

static Work g_work[3];

static void flush_now(void)
{
    stdio_flush();
}

static int set_clock_and_psram(void)
{
#if MICRODOS_PICO_SYS_KHZ > 0
    if (!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ, false))
        return 0;

    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                               PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != 0)
        return 0;

    if (psram_reinitialize() != 0)
        return 0;
#endif

    return 1;
}

static uint32_t hash_low_half(const uint8_t *mem)
{
    uint32_t h = 2166136261u;
    unsigned i;

    for (i = 0u; i < 0x8000u; ++i)
        h = (h ^ mem[i]) * 16777619u;

    return h;
}

static void prepare_memory(uint8_t *mem)
{
    unsigned i;

    memset(mem, 0, SRAM_GUEST_BYTES);

    for (i = 0u; i < 0x4000u; ++i) {
        const uint16_t v =
            (uint16_t)((i * 109u + 0x1234u) ^ (i >> 3));
        const unsigned at = 0x8000u + i * 2u;
        mem[at] = (uint8_t)v;
        mem[at + 1u] = (uint8_t)(v >> 8);
    }
}

static uint32_t expected_memory_hash(const uint8_t *mem, uint16_t *last_ax)
{
    uint32_t h = 2166136261u;
    unsigned i;

    *last_ax = 0u;

    for (i = 0u; i < 0x4000u; ++i) {
        const unsigned at = 0x8000u + i * 2u;
        uint16_t v =
            (uint16_t)((uint16_t)mem[at] |
                       ((uint16_t)mem[at + 1u] << 8));

        v = (uint16_t)(v + 1u);
        *last_ax = v;

        h = (h ^ (uint8_t)v) * 16777619u;
        h = (h ^ (uint8_t)(v >> 8)) * 16777619u;
    }

    return h;
}

static void ref_regmix(uint16_t *ax_out, uint16_t *bx_out, uint16_t *dx_out,
                       unsigned *cf_out)
{
    uint16_t ax = 1u, bx = 3u, dx = 0u, cx = 0x8000u;
    unsigned cf = 0u;

    do {
        uint32_t wide;

        wide = (uint32_t)ax + bx;
        ax = (uint16_t)wide;
        cf = (wide >> 16) & 1u;

        bx = (uint16_t)(bx ^ ax);
        cf = 0u;

        wide = (uint32_t)dx + ax;
        dx = (uint16_t)wide;
        cf = (wide >> 16) & 1u;

        cx = (uint16_t)(cx - 1u);
    } while (cx != 0u);

    *ax_out = ax;
    *bx_out = bx;
    *dx_out = dx;
    *cf_out = cf;
}

static int compile_all(void)
{
    unsigned i;

    memset(g_work, 0, sizeof(g_work));

    g_work[0].name = "loop";
    g_work[0].image = kLoop;
    g_work[0].image_size = sizeof(kLoop);
    g_work[0].guest_per_run = 2u + 3u * 32768u;
    g_work[0].runs = NV2_RUNS_REG;

    g_work[1].name = "regmix";
    g_work[1].image = kRegmix;
    g_work[1].image_size = sizeof(kRegmix);
    g_work[1].guest_per_run = 4u + 5u * 32768u;
    g_work[1].runs = NV2_RUNS_REG;

    g_work[2].name = "memmix";
    g_work[2].image = kMemmix;
    g_work[2].image_size = sizeof(kMemmix);
    g_work[2].guest_per_run = 4u + 8u * 16384u;
    g_work[2].runs = NV2_RUNS_MEM;

    for (i = 0u; i < 3u; ++i) {
        const MdNativeV2Status st =
            md_native_v2_compile_8086(
                g_work[i].image, g_work[i].image_size,
                0x0100u, 0x0100u, &g_work[i].code);

        printf("[native-v2] compile %s=%s ops=%u bytes=%u "
               "entryCF=%u CFsites=%u Zsites=%u mem=%u store=%u\n",
               g_work[i].name,
               md_native_v2_status_name(st),
               (unsigned)g_work[i].code.op_count,
               (unsigned)g_work[i].code.size,
               (unsigned)g_work[i].code.needs_entry_cf,
               (unsigned)g_work[i].code.cf_sites,
               (unsigned)g_work[i].code.z_sites,
               (unsigned)g_work[i].code.needs_memory,
               (unsigned)g_work[i].code.has_store);
        flush_now();

        if (st != MD_NATIVE_V2_OK) return 0;
    }

    return 1;
}

static void print_fixed3(uint64_t milli)
{
    printf("%llu.%03llu",
           (unsigned long long)(milli / 1000u),
           (unsigned long long)(milli % 1000u));
}

static void print_perf(const char *label, uint64_t total_guest, uint64_t elapsed)
{
    const uint64_t mips =
        (total_guest * 1000u) / (elapsed ? elapsed : 1u);
    const uint64_t cycles =
        ((uint64_t)MICRODOS_PICO_SYS_KHZ * 1000u) / mips;

    printf("[native-v2] %-12s ", label);
    print_fixed3(mips);
    printf(" MIPS  ");
    print_fixed3(cycles);
    printf(" host-cycles/guest\n");
    flush_now();
}

static int run_loop(Work *w)
{
    uint64_t start, elapsed;
    unsigned i;

    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.flags_raw = 1u;

    if (md_native_v2_execute(&g_cpu, &w->code) == MD_NATIVE_V2_EXEC_FALLBACK)
        return 0;

    if (g_cpu.r[MD_X86_AX] != 0x8000u ||
        g_cpu.r[MD_X86_CX] != 0u ||
        g_cpu.ip != 0x010Au ||
        md_x86_cf(&g_cpu) != 1 ||
        md_x86_zf(&g_cpu) != 1) {
        printf("[native-v2] loop FAIL handoff\n");
        flush_now();
        return 0;
    }

    start = time_us_64();
    for (i = 0u; i < w->runs; ++i)
        (void)md_native_v2_execute(&g_cpu, &w->code);
    elapsed = time_us_64() - start;

    printf("[native-v2] loop handoff PASS CF=%d ZF=%d\n",
           md_x86_cf(&g_cpu), md_x86_zf(&g_cpu));
    print_perf("loop", (uint64_t)w->runs * w->guest_per_run, elapsed);
    return 1;
}

static int run_regmix(Work *w)
{
    uint16_t ax, bx, dx;
    unsigned expected_cf;
    uint64_t start, elapsed;
    unsigned i;

    ref_regmix(&ax, &bx, &dx, &expected_cf);

    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.flags_raw = 1u;

    if (md_native_v2_execute(&g_cpu, &w->code) == MD_NATIVE_V2_EXEC_FALLBACK)
        return 0;

    if (g_cpu.r[MD_X86_AX] != ax ||
        g_cpu.r[MD_X86_BX] != bx ||
        g_cpu.r[MD_X86_CX] != 0u ||
        g_cpu.r[MD_X86_DX] != dx ||
        g_cpu.ip != 0x0114u ||
        md_x86_cf(&g_cpu) != (int)expected_cf ||
        md_x86_zf(&g_cpu) != 1) {
        printf("[native-v2] regmix FAIL CF=%d/%u ZF=%d\n",
               md_x86_cf(&g_cpu), expected_cf, md_x86_zf(&g_cpu));
        flush_now();
        return 0;
    }

    start = time_us_64();
    for (i = 0u; i < w->runs; ++i)
        (void)md_native_v2_execute(&g_cpu, &w->code);
    elapsed = time_us_64() - start;

    printf("[native-v2] regmix handoff PASS CF=%d ZF=%d\n",
           md_x86_cf(&g_cpu), md_x86_zf(&g_cpu));
    print_perf("regmix", (uint64_t)w->runs * w->guest_per_run, elapsed);
    return 1;
}

static int run_memmix_one(Work *w, const char *label, uint8_t *memory)
{
    uint16_t last_ax;
    uint32_t expected_hash, got_hash;
    uint64_t start, elapsed;
    unsigned i;

    prepare_memory(memory);
    expected_hash = expected_memory_hash(memory, &last_ax);

    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.memory = memory;
    g_cpu.ds = 0u;
    g_cpu.flags_raw = 1u;

    if (md_native_v2_execute(&g_cpu, &w->code) == MD_NATIVE_V2_EXEC_FALLBACK) {
        printf("[native-v2] %s unexpected fallback\n", label);
        flush_now();
        return 0;
    }

    got_hash = hash_low_half(memory);

    if (got_hash != expected_hash ||
        g_cpu.r[MD_X86_AX] != last_ax ||
        g_cpu.r[MD_X86_CX] != 0u ||
        g_cpu.r[MD_X86_SI] != 0u ||
        g_cpu.r[MD_X86_DI] != 0x8000u ||
        g_cpu.ip != 0x011Eu ||
        md_x86_cf(&g_cpu) != 0 ||
        md_x86_zf(&g_cpu) != 1) {
        printf("[native-v2] %s FAIL hash=%08lX/%08lX "
               "AX=%04X/%04X CX=%04X SI=%04X DI=%04X CF=%d ZF=%d IP=%04X\n",
               label,
               (unsigned long)got_hash,
               (unsigned long)expected_hash,
               g_cpu.r[MD_X86_AX], last_ax,
               g_cpu.r[MD_X86_CX],
               g_cpu.r[MD_X86_SI],
               g_cpu.r[MD_X86_DI],
               md_x86_cf(&g_cpu),
               md_x86_zf(&g_cpu),
               g_cpu.ip);
        flush_now();
        return 0;
    }

    start = time_us_64();
    for (i = 0u; i < w->runs; ++i)
        (void)md_native_v2_execute(&g_cpu, &w->code);
    elapsed = time_us_64() - start;

    printf("[native-v2] %s PASS hash=%08lX CF=%d ZF=%d\n",
           label, (unsigned long)got_hash,
           md_x86_cf(&g_cpu), md_x86_zf(&g_cpu));
    print_perf(label, (uint64_t)w->runs * w->guest_per_run, elapsed);
    return 1;
}

int main(void)
{
    const int clock_psram_ok = set_clock_and_psram();
    int compiled = 0;
    absolute_time_t hello_at;

    stdio_init_all();
    hello_at = get_absolute_time();

    for (;;) {
        int ch;

        if (absolute_time_diff_us(get_absolute_time(), hello_at) <= 0) {
            printf("\n[NV2-P2C] microDOS Native v2 Phase 2C\n");
            printf("[NV2-P2C] clock=%lu kHz psram=%s addr=%s\n",
                   (unsigned long)MICRODOS_PICO_SYS_KHZ,
                   clock_psram_ok && psram_is_available() ? "OK" : "FAILED",
                   psram_check_address(&g_psram_guest[0]) &&
                   psram_check_address(&g_psram_guest[PSRAM_GUEST_BYTES - 1u])
                       ? "OK" : "BAD");
            flush_now();
            hello_at = make_timeout_time_ms(1000);
        }

        ch = getchar_timeout_us(10000);
        if (ch < 0) continue;

        if (!compiled) {
            printf("[native-v2] COMPILE BEGIN\n");
            flush_now();

            compiled =
                clock_psram_ok &&
                psram_is_available() &&
                psram_check_address(&g_psram_guest[0]) &&
                psram_check_address(&g_psram_guest[PSRAM_GUEST_BYTES - 1u]) &&
                compile_all();

            printf("[native-v2] %s\n",
                   compiled ? "READY - send R" : "COMPILE/PSRAM FAILED");
            flush_now();
            continue;
        }

        if (ch == 'R' || ch == 'r' || ch == '\r' || ch == '\n') {
            const int ok =
                run_loop(&g_work[0]) &&
                run_regmix(&g_work[1]) &&
                run_memmix_one(&g_work[2], "mem-sram", g_sram_guest) &&
                run_memmix_one(&g_work[2], "mem-psram", g_psram_guest);

            printf("[native-v2] PHASE2C %s\n", ok ? "PASS" : "FAIL");
            printf("[native-v2] READY - send R\n");
            flush_now();
        }
    }
}
