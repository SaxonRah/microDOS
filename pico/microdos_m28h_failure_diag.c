/*
 * M28h v2 targeted regression gate.
 * Runs the exact seven random cases that exposed the v1 mid-block fault bug.
 */
#define MD_TRANSLATE_DIFF_NO_MAIN 1
#define MD_TRANSLATE_DIFF_PROGRESS 1000000u
#include "test_translate_diff.c"

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 300000
#endif
#define MD_M28H_DIAG_ARENA_BYTES (64u * 1024u)

static uint8_t __uninitialized_psram("md_m28h_diag_a") __attribute__((aligned(16)))
    g_diag_mem_a[MD_X86_ADDRESS_SPACE];
static uint8_t __uninitialized_psram("md_m28h_diag_b") __attribute__((aligned(MD_X86_ADDRESS_SPACE)))
    g_diag_mem_b[2u * MD_X86_ADDRESS_SPACE];
static uint8_t __attribute__((aligned(8))) g_diag_arena[MD_M28H_DIAG_ARENA_BYTES];

static int diag_clock_psram(void)
{
#if MICRODOS_PICO_SYS_KHZ > 0
    if (!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ, false)) return 0;
    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                               PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != 0) return 0;
    if (psram_reinitialize() != 0) return 0;
#endif
    return psram_is_available() &&
           psram_check_address(&g_diag_mem_a[0]) &&
           psram_check_address(&g_diag_mem_a[MD_X86_ADDRESS_SPACE - 1u]) &&
           psram_check_address(&g_diag_mem_b[0]) &&
           psram_check_address(&g_diag_mem_b[2u * MD_X86_ADDRESS_SPACE - 1u]);
}

static unsigned run_one(const char *mode, int eager, uint32_t seed,
                        int unaligned, int only)
{
    unsigned fails;
    g_td_eager = eager;
    printf("\n[m28h-diag] BEGIN mode=%s seed=%08X case=%d unaligned=%d\n",
           mode, (unsigned)seed, only, unaligned);
    stdio_flush();

    fails = md_translate_diff_run((unsigned)only + 1u, seed, unaligned, only,
                                  g_diag_mem_a, g_diag_mem_b,
                                  g_diag_arena, MD_M28H_DIAG_ARENA_BYTES);

    printf("[m28h-diag] END mode=%s case=%d fails=%u\n", mode, only, fails);
    stdio_flush();
    return fails;
}

static void run_diag(void)
{
    static const int eager_cases[] = { 9, 32, 42, 48, 50 };
    static const int tiered_cases[] = { 957, 970 };
    unsigned fails = 0u;
    unsigned i;

    printf("\n[m28h-diag] microDOS M28h v2 targeted regression gate\n");
    printf("[m28h-diag] clock=%u kHz psram=%s arena=%p (%u bytes)\n",
           (unsigned)(clock_get_hz(clk_sys) / 1000u),
           psram_is_available() ? "OK" : "FAIL",
           (void *)g_diag_arena, (unsigned)sizeof(g_diag_arena));
    stdio_flush();

    for (i = 0u; i < sizeof(eager_cases) / sizeof(eager_cases[0]); ++i)
        fails += run_one("eager-aligned", 1, 0x4D32355Bu, 0, eager_cases[i]);

    for (i = 0u; i < sizeof(tiered_cases) / sizeof(tiered_cases[0]); ++i)
        fails += run_one("tiered-unaligned", 0, 0x0BADF00Du, 1, tiered_cases[i]);

    printf("\n[m28h-diag] COMPLETE result=%s fails=%u\n",
           fails ? "FAIL" : "PASS", fails);
    printf("[m28h-diag] send R to run again\n");
    stdio_flush();
}

int main(void)
{
    const int ok = diag_clock_psram();
    int ch;
    stdio_init_all();

    for (;;) {
        while (!stdio_usb_connected()) sleep_ms(50);
        sleep_ms(500);

        if (!ok) {
            printf("[m28h-diag] COMPLETE result=FAIL clock-or-psram\n");
            stdio_flush();
        } else {
            run_diag();
        }

        do {
            ch = getchar_timeout_us(100000);
        } while (ch != 'R' && ch != 'r' && stdio_usb_connected());
    }
}
