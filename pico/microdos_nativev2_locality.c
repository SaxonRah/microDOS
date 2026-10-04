/*
 * microDOS Native v2 Phase 2D
 *
 * PSRAM/XIP locality map.
 *
 * The native compiler is unchanged. This benchmark measures the same
 * read/ALU region against internal SRAM and the real Pico Plus 2 PSRAM over
 * working sets from 512 bytes through 64 KiB.
 *
 * The firmware is built copy_to_ram, so the XIP cache is free to cache PSRAM.
 */
#include "microdos/native_v2.h"

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 300000
#endif

#define NV2_LOCAL_BYTES (64u * 1024u)
#define NV2_PSRAM_BYTES (1024u * 1024u)

static uint8_t __attribute__((aligned(16))) g_sram[NV2_LOCAL_BYTES];
static uint8_t __uninitialized_psram("nv2_locality_psram")
    __attribute__((aligned(16))) g_psram[NV2_PSRAM_BYTES];

static MdX86 g_cpu;

typedef struct LocalityCase {
    uint32_t bytes;
    uint16_t words;
    uint16_t expected_dx;
    MdNativeV2Code code;
} LocalityCase;

static LocalityCase g_case[] = {
    {   512u,   256u, 0u, {0} },
    {  2048u,  1024u, 0u, {0} },
    {  8192u,  4096u, 0u, {0} },
    { 16384u,  8192u, 0u, {0} },
    { 32768u, 16384u, 0u, {0} },
    { 65536u, 32768u, 0u, {0} },
};

#define CASE_COUNT (sizeof(g_case) / sizeof(g_case[0]))

static void flush_now(void)
{
    stdio_flush();
}

static bool set_clock_and_psram(void)
{
#if MICRODOS_PICO_SYS_KHZ > 0
    if (!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ, false))
        return false;

    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                               PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != 0)
        return false;

    if (psram_reinitialize() != 0)
        return false;
#endif
    return true;
}

static uint16_t value_at(unsigned word)
{
    return (uint16_t)(((word * 109u + 0x1234u) ^ (word >> 3)) & 0xFFFFu);
}

static void prepare_buffer(uint8_t *mem, size_t bytes)
{
    unsigned i;
    const unsigned words = (unsigned)(bytes / 2u);

    for (i = 0u; i < words; ++i) {
        const uint16_t v = value_at(i);
        mem[i * 2u] = (uint8_t)v;
        mem[i * 2u + 1u] = (uint8_t)(v >> 8);
    }
}

static uint16_t expected_sum(unsigned words)
{
    uint16_t sum = 0u;
    unsigned i;

    for (i = 0u; i < words; ++i)
        sum = (uint16_t)(sum + value_at(i));

    return sum;
}

static size_t build_image(uint8_t *image, uint16_t words)
{
    /*
     * 0100  mov si,0000
     * 0103  mov cx,words
     * 0106  xor dx,dx
     * 0108  L: mov ax,[si]
     * 010A     add dx,ax
     * 010C     lea si,[si+2]
     * 010F     dec cx
     * 0110     jnz L       (-10 / F6)
     */
    image[0]  = 0xBE; image[1]  = 0x00; image[2]  = 0x00;
    image[3]  = 0xB9; image[4]  = (uint8_t)words;
                       image[5]  = (uint8_t)(words >> 8);
    image[6]  = 0x31; image[7]  = 0xD2;
    image[8]  = 0x8B; image[9]  = 0x04;
    image[10] = 0x03; image[11] = 0xD0;
    image[12] = 0x8D; image[13] = 0x74; image[14] = 0x02;
    image[15] = 0x49;
    image[16] = 0x75; image[17] = 0xF6;
    return 18u;
}

static int compile_cases(void)
{
    unsigned i;

    for (i = 0u; i < CASE_COUNT; ++i) {
        uint8_t image[18];
        const size_t n = build_image(image, g_case[i].words);
        const MdNativeV2Status st =
            md_native_v2_compile_8086(image, n, 0x0100u, 0x0100u,
                                      &g_case[i].code);

        g_case[i].expected_dx = expected_sum(g_case[i].words);

        printf("[P2D] compile %5lu B = %-12s ops=%u native=%u B\n",
               (unsigned long)g_case[i].bytes,
               md_native_v2_status_name(st),
               (unsigned)g_case[i].code.op_count,
               (unsigned)g_case[i].code.size);
        flush_now();

        if (st != MD_NATIVE_V2_OK)
            return 0;
    }

    return 1;
}

static uint32_t choose_runs(uint64_t guest_per_run)
{
    const uint64_t target_guest = 20000000ull;
    uint64_t runs = target_guest / guest_per_run;

    if (runs < 64u) runs = 64u;
    if (runs > 20000u) runs = 20000u;
    return (uint32_t)runs;
}

static int verify_one(LocalityCase *c, uint8_t *memory)
{
    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.memory = memory;
    g_cpu.ds = 0u;
    g_cpu.flags_raw = 0u;

    if (md_native_v2_execute(&g_cpu, &c->code) ==
        MD_NATIVE_V2_EXEC_FALLBACK) {
        printf("[P2D] unexpected native fallback for %lu bytes\n",
               (unsigned long)c->bytes);
        flush_now();
        return 0;
    }

    if (g_cpu.r[MD_X86_DX] != c->expected_dx ||
        g_cpu.r[MD_X86_CX] != 0u ||
        g_cpu.r[MD_X86_SI] != (uint16_t)c->bytes ||
        g_cpu.ip != 0x0112u ||
        !md_x86_zf(&g_cpu)) {
        printf("[P2D] verify FAIL %lu B "
               "DX=%04X/%04X CX=%04X SI=%04X IP=%04X ZF=%d\n",
               (unsigned long)c->bytes,
               g_cpu.r[MD_X86_DX], c->expected_dx,
               g_cpu.r[MD_X86_CX],
               g_cpu.r[MD_X86_SI],
               g_cpu.ip,
               md_x86_zf(&g_cpu));
        flush_now();
        return 0;
    }

    return 1;
}

static int measure(const char *where, LocalityCase *c, uint8_t *memory)
{
    const uint64_t guest_per_run = 3ull + 5ull * c->words;
    const uint32_t runs = choose_runs(guest_per_run);
    uint64_t start_us, elapsed_us, total_guest;
    uint64_t mips_milli, cycles_milli;
    uint32_t i;

    if (!verify_one(c, memory))
        return 0;

    /*
     * One untimed second pass deliberately warms the hardware cache for the
     * selected working set. This benchmark is about steady-state DOS locality,
     * not first-touch latency.
     */
    if (!verify_one(c, memory))
        return 0;

    start_us = time_us_64();
    for (i = 0u; i < runs; ++i)
        (void)md_native_v2_execute(&g_cpu, &c->code);
    elapsed_us = time_us_64() - start_us;

    total_guest = guest_per_run * runs;
    if (elapsed_us == 0u) elapsed_us = 1u;

    mips_milli = (total_guest * 1000u) / elapsed_us;
    cycles_milli =
        ((uint64_t)MICRODOS_PICO_SYS_KHZ * 1000u) / mips_milli;

    printf("[P2D] %-5s %5lu B  runs=%5lu  %3llu.%03llu MIPS  "
           "%2llu.%03llu cyc/guest\n",
           where,
           (unsigned long)c->bytes,
           (unsigned long)runs,
           (unsigned long long)(mips_milli / 1000u),
           (unsigned long long)(mips_milli % 1000u),
           (unsigned long long)(cycles_milli / 1000u),
           (unsigned long long)(cycles_milli % 1000u));
    flush_now();

    return 1;
}

static int run_locality(void)
{
    unsigned i;

    prepare_buffer(g_sram, sizeof(g_sram));
    prepare_buffer(g_psram, NV2_LOCAL_BYTES);

    printf("[P2D] SRAM  addr=%p\n", (void *)g_sram);
    printf("[P2D] PSRAM addr=%p check=%s size=%lu\n",
           (void *)g_psram,
           psram_check_address(g_psram) ? "OK" : "BAD",
           (unsigned long)psram_get_size());
    flush_now();

    printf("[P2D] ---- internal SRAM ----\n");
    flush_now();
    for (i = 0u; i < CASE_COUNT; ++i)
        if (!measure("SRAM", &g_case[i], g_sram)) return 0;

    printf("[P2D] ---- cached XIP/QMI PSRAM ----\n");
    flush_now();
    for (i = 0u; i < CASE_COUNT; ++i)
        if (!measure("PSRAM", &g_case[i], g_psram)) return 0;

    return 1;
}

int main(void)
{
    const bool clock_ok = set_clock_and_psram();
    int compiled = 0;
    absolute_time_t hello_at;

    stdio_init_all();
    hello_at = get_absolute_time();

    for (;;) {
        int ch;

        if (absolute_time_diff_us(get_absolute_time(), hello_at) <= 0) {
            printf("\n[NV2-P2D] PSRAM locality map\n");
            printf("[NV2-P2D] clock=%lu kHz status=%s psram=%s\n",
                   (unsigned long)MICRODOS_PICO_SYS_KHZ,
                   clock_ok ? "OK" : "FAILED",
                   psram_is_available() ? "OK" : "MISSING");
            flush_now();
            hello_at = make_timeout_time_ms(1000);
        }

        ch = getchar_timeout_us(10000);
        if (ch < 0) continue;

        if (!compiled) {
            printf("[P2D] COMPILE BEGIN\n");
            flush_now();
            compiled = compile_cases();
            printf("[P2D] %s\n",
                   compiled ? "READY - send R" : "COMPILE FAILED");
            flush_now();
            continue;
        }

        if (ch == 'R' || ch == 'r' || ch == '\r' || ch == '\n') {
            const int ok = run_locality();
            printf("[P2D] PHASE2D %s\n", ok ? "PASS" : "FAIL");
            printf("[P2D] READY - send R\n");
            flush_now();
        }
    }
}
