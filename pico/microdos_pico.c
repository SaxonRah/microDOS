/* microDOS for Raspberry Pi Pico 2 / Pimoroni Pico Plus 2 (M14).
 *
 * Boots the released MS-DOS 2.0 kernel and COMMAND.COM through the same
 * portable system loop the desktop end-to-end test uses. Everything the
 * guest sees lives in PSRAM; the console is USB CDC serial.
 *
 *   guest memory  1 MiB, PSRAM (uninitialised section, zeroed at boot)
 *   disk          360 KiB FAT12 image embedded in flash, copied to PSRAM;
 *                 DOS writes land in the PSRAM copy and are lost at reset
 *   kernel        MSDOS.SYS embedded in flash
 *   AOT           DOS2TEST.COM compiled by dosrecomp, attached when DOS runs it
 *
 * Keys: Ctrl+] prints runtime statistics (it is not passed to DOS): totals
 * since boot and the interval since the previous Ctrl+]. Press it right
 * before and right after a program to measure just that program.
 *
 * Build variants (pico/CMakeLists.txt):
 *   MICRODOS_PICO_CACHE        1 = decoded-block cache, 0 = canonical stepping
 *   MICRODOS_PICO_CODE_IN_SRAM 1 = copy_to_ram binary (informational here)
 */
#include "md_dos2_system.h"
#include "dos2test_recomp.h"
#if MICRODOS_PICO_KERNEL_AOT
#include "msdos2_recomp.h"
#endif

#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MD_GUEST_BYTES (1u << 20)
#define MD_DISK_BYTES (720u * 512u)
#define MD_SLICE 200000u
#define MD_IDLE_POLLS 256u

extern const uint8_t md_blob_msdos_sys[], md_blob_msdos_sys_end[];
extern const uint8_t md_blob_disk[], md_blob_disk_end[];

static uint8_t __uninitialized_psram("md_guest") __attribute__((aligned(16)))
    g_guest[MD_GUEST_BYTES];
static uint8_t __uninitialized_psram("md_disk") __attribute__((aligned(16)))
    g_disk[MD_DISK_BYTES];

static MdDos2System g_sys;
static MdBlockCache g_cache;

static const MdAotProgram *const g_programs[] = { &md_recomp_dos2test_program };

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

/* M15 exact time accounting. Everything is measured, nothing estimated:
   active = time inside md_dos2_system_run minus the host work it calls
   back into (idle sleeps, console output, blocking input, disk copies). */
typedef struct PicoPerf {
    uint64_t run_us;
    uint64_t idle_sleep_us;
    uint64_t console_out_us;
    uint64_t input_wait_us;
    uint64_t disk_us;
    uint64_t instructions;
    uint64_t aot_instructions;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_invalidations;
    uint64_t cache_fallback;
    uint32_t idle_sleeps;
    uint64_t at_us;
} PicoPerf;

static PicoPerf g_perf;          /* running totals */
static PicoPerf g_perf_mark;     /* snapshot at the previous Ctrl+] */

/* ---- host text (CR LF explicit: stdio CRLF translation is off) --------- */

static void md_say(const char *fmt, ...)
{
    char line[192];
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

/* ---- console callbacks -------------------------------------------------- */

static bool pico_fetch(PicoConsole *con, bool block, uint8_t *value)
{
    for (;;) {
        const int ch = block ? getchar() : getchar_timeout_us(0);
        if (ch < 0) return false;
        if (ch == 0x1D) {                         /* Ctrl+]: host stats */
            con->stats_requested = true;
            if (!block) return false;
            continue;
        }
        if (ch == '\n' && con->last_was_cr) {     /* CR LF from terminal = one Enter */
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
                sleep_us(500);                    /* idle prompt: stop spinning */
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

/* ---- disk callbacks: PSRAM copy of the flash image ---------------------- */

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

/* ---- clock --------------------------------------------------------------- */

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 0
#endif

/* M16: optional higher clk_sys. The SDK initialises PSRAM before main() for
   the boot clock, so after changing clk_sys the QMI divider/rxdelay/select
   timings are recomputed from the new clock and PSRAM is re-initialised.
   Nothing lives in PSRAM yet at this point. Same approach microconsole uses
   for 300 MHz on this board, plus the PSRAM retime. */
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

/* ---- boot checks -------------------------------------------------------- */

static bool md_psram_ok(void)
{
    volatile uint32_t *g = (volatile uint32_t *)g_guest;
    volatile uint32_t *d = (volatile uint32_t *)g_disk;
    uint32_t i;

    if (!psram_is_available()) { md_say("  psram:   NOT AVAILABLE\n"); return false; }
    if (!psram_check_address(&g_guest[0]) || !psram_check_address(&g_guest[MD_GUEST_BYTES - 1u]) ||
        !psram_check_address(&g_disk[0]) || !psram_check_address(&g_disk[MD_DISK_BYTES - 1u])) {
        md_say("  psram:   buffers not inside PSRAM (%p, %p)\n", (void *)g_guest, (void *)g_disk);
        return false;
    }
    /* one word per 4 KiB page, both buffers */
    for (i = 0; i < MD_GUEST_BYTES / 4u; i += 1024u) g[i] = 0xA5000000u ^ i;
    for (i = 0; i < MD_DISK_BYTES / 4u; i += 1024u) d[i] = 0x5A000000u ^ i;
    for (i = 0; i < MD_GUEST_BYTES / 4u; i += 1024u) {
        if (g[i] != (0xA5000000u ^ i)) { md_say("  psram:   guest verify failed at word %lu\n", (unsigned long)i); return false; }
    }
    for (i = 0; i < MD_DISK_BYTES / 4u; i += 1024u) {
        if (d[i] != (0x5A000000u ^ i)) { md_say("  psram:   disk verify failed at word %lu\n", (unsigned long)i); return false; }
    }
    return true;
}

static void md_perf_snapshot(PicoPerf *p)
{
    p->instructions = g_sys.runtime.instructions;
    p->aot_instructions = g_sys.runtime.aot_instructions;
    p->cache_hits = g_cache.hits;
    p->cache_misses = g_cache.misses;
    p->cache_invalidations = g_cache.invalidations;
    p->cache_fallback = g_cache.fallback_instructions;
    p->idle_sleeps = g_con.idle_sleeps;
    p->at_us = time_us_64();
}

static void md_perf_report(const char *label, const PicoPerf *now, const PicoPerf *from, uint64_t since_us)
{
    const uint64_t wall = now->at_us - since_us;
    const uint64_t run = now->run_us - from->run_us;
    const uint64_t sleep = now->idle_sleep_us - from->idle_sleep_us;
    const uint64_t out = now->console_out_us - from->console_out_us;
    const uint64_t in = now->input_wait_us - from->input_wait_us;
    const uint64_t disk = now->disk_us - from->disk_us;
    const uint64_t host = sleep + out + in + disk;
    const uint64_t active = run > host ? run - host : 0u;
    const uint64_t instr = now->instructions - from->instructions;

    md_say("[perf] --- %s ---\n", label);
    md_say("[perf] wall %9.3f s   in-guest-loop %9.3f s\n", (double)wall / 1e6, (double)run / 1e6);
    md_say("[perf] active %7.3f s   idle-sleep %.3f s   console-out %.3f s   input-wait %.3f s   disk %.3f s\n",
           (double)active / 1e6, (double)sleep / 1e6, (double)out / 1e6, (double)in / 1e6, (double)disk / 1e6);
    md_say("[perf] instructions %llu (aot %llu)   active %.3f MIPS\n",
           (unsigned long long)instr,
           (unsigned long long)(now->aot_instructions - from->aot_instructions),
           active != 0u ? (double)instr / (double)active : 0.0);
    md_say("[perf] cache hits %llu misses %llu invalidations %llu fallback %llu   idle-sleeps %lu\n",
           (unsigned long long)(now->cache_hits - from->cache_hits),
           (unsigned long long)(now->cache_misses - from->cache_misses),
           (unsigned long long)(now->cache_invalidations - from->cache_invalidations),
           (unsigned long long)(now->cache_fallback - from->cache_fallback),
           (unsigned long)(now->idle_sleeps - from->idle_sleeps));
}

static void md_stats(uint64_t start_us)
{
    PicoPerf now = g_perf;
    PicoPerf zero;
    memset(&zero, 0, sizeof(zero));
    md_perf_snapshot(&now);

    md_say("\n[perf] config: M18, clk %lu MHz, code %s, block cache %s, kernel AOT %s, guest PSRAM\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
           MICRODOS_PICO_CODE_IN_SRAM ? "SRAM (copy_to_ram)" : "flash XIP",
           MICRODOS_PICO_CACHE ? "ON" : "OFF",
           g_sys.kernel_attached ? "ON" : "OFF");
    md_perf_report("since boot", &now, &zero, start_us);
    md_perf_report("since previous Ctrl+]", &now, &g_perf_mark,
                   g_perf_mark.at_us != 0u ? g_perf_mark.at_us : start_us);
    md_say("[perf] aot: kernel compiled %llu (%.1f%% of all)   DOS2TEST compiled %llu   attached-segment steps %llu\n",
           (unsigned long long)g_sys.kernel_aot_instructions,
           now.instructions ? 100.0 * (double)g_sys.kernel_aot_instructions / (double)now.instructions : 0.0,
           (unsigned long long)(now.aot_instructions - g_sys.kernel_aot_instructions),
           (unsigned long long)g_sys.attached_steps);
    md_say("[perf] aot: attaches=%lu enters=%lu evictions=%lu   disk writes %lu (RAM only)   CS:IP=%04X:%04X\n",
           (unsigned long)g_sys.aot_attaches,
           (unsigned long)g_sys.aot_enters, (unsigned long)g_sys.runtime.aot_evictions,
           (unsigned long)g_disk_writes, g_sys.runtime.cpu.cs, g_sys.runtime.cpu.ip);
    g_perf_mark = now;
}

int main(void)
{
    const size_t kernel_size = (size_t)(md_blob_msdos_sys_end - md_blob_msdos_sys);
    const size_t disk_size = (size_t)(md_blob_disk_end - md_blob_disk);
    uint64_t start_us;
    MdStopReason stop = MD_STOP_NONE;

    const bool clock_ok = md_pico_set_clock();
    stdio_init_all();
    while (!stdio_usb_connected()) sleep_ms(20);    /* wait for a terminal */
    sleep_ms(300);

    md_say("\nmicroDOS for Pico 2 (Pimoroni Pico Plus 2)\n");
    md_say("  clk_sys: %lu MHz%s\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
           MICRODOS_PICO_SYS_KHZ > 0 ? (clock_ok ? " (raised; PSRAM retimed)" : " (REQUESTED CLOCK FAILED)") : "");
    md_say("  psram:   %lu KiB (sdk available=%d)\n",
           (unsigned long)(psram_get_size() / 1024u), psram_is_available() ? 1 : 0);

    if (!md_psram_ok()) {
        md_say("microDOS: PSRAM check failed; halting.\n");
        for (;;) sleep_ms(1000);
    }
    md_say("  guest:   1 MiB at %p (PSRAM, page check ok)\n", (void *)g_guest);

    if (disk_size != MD_DISK_BYTES) {
        md_say("microDOS: embedded disk is %lu bytes, expected %lu; halting.\n",
               (unsigned long)disk_size, (unsigned long)MD_DISK_BYTES);
        for (;;) sleep_ms(1000);
    }
    memset(g_guest, 0, MD_GUEST_BYTES);
    memcpy(g_disk, md_blob_disk, MD_DISK_BYTES);
    md_say("  disk:    360 KiB image copied from flash to PSRAM (writes are lost at reset)\n");

    md_dos2_system_init(&g_sys, g_guest, MICRODOS_PICO_CACHE ? &g_cache : NULL);
    g_sys.boot.console.write = con_write;
    g_sys.boot.console.peek = con_peek;
    g_sys.boot.console.read = con_read;
    g_sys.boot.console.flush = con_flush;
    g_sys.boot.console.user = &g_con;
    g_sys.boot.disk.read = disk_read;
    g_sys.boot.disk.write = disk_write;
    g_sys.boot.disk.user = NULL;
    g_sys.boot.disk.sector_size = 512u;
    g_sys.boot.disk.sector_count = 720u;
    g_sys.boot.disk.writable = true;
    g_sys.boot.clock_days = 1162u;          /* 1983-03-08; set it at the DOS prompt */
    g_sys.boot.clock_hours = 12u;
#if MICRODOS_PICO_KERNEL_AOT
    md_dos2_system_set_kernel_aot(&g_sys, &md_recomp_msdos2_program);   /* M17 */
#endif
    md_dos2_system_set_aot(&g_sys, g_programs, 1u, true);

    if (!md_dos2_system_start(&g_sys, md_blob_msdos_sys, kernel_size)) {
        md_say("microDOS: embedded MSDOS.SYS rejected (%lu bytes); halting.\n",
               (unsigned long)kernel_size);
        for (;;) sleep_ms(1000);
    }
    md_say("  kernel:  MSDOS.SYS %lu bytes\n", (unsigned long)kernel_size);
#if MICRODOS_PICO_KERNEL_AOT
    md_say("  aot:     MSDOS.SYS kernel (%lu compiled, %lu holes) %s\n",
           (unsigned long)md_recomp_msdos2_program.compiled_instructions,
           (unsigned long)md_recomp_msdos2_program.hole_instructions,
           g_sys.kernel_attached ? "attached" : "NOT ATTACHED (image mismatch)");
#else
    md_say("  aot:     MSDOS.SYS kernel: not compiled in this firmware\n");
#endif
    md_say("  aot:     %s (%lu compiled, %lu holes)\n", g_programs[0]->name,
           (unsigned long)g_programs[0]->compiled_instructions,
           (unsigned long)g_programs[0]->hole_instructions);
    md_say("  config:  code %s, block cache %s\n",
           MICRODOS_PICO_CODE_IN_SRAM ? "SRAM (copy_to_ram)" : "flash XIP",
           MICRODOS_PICO_CACHE ? "ON" : "OFF");
    md_say("  keys:    Ctrl+] -> statistics (since boot + since previous Ctrl+])\n");

    start_us = time_us_64();
    for (;;) {
        const uint64_t t0 = time_us_64();
        stop = md_dos2_system_run(&g_sys, MD_SLICE);
        g_perf.run_us += time_us_64() - t0;
        if (g_con.stats_requested) {
            g_con.stats_requested = false;
            md_stats(start_us);
        }
        if (stop != MD_STOP_NONE) break;
    }

    md_say("\n[microDOS] guest stopped: %s\n", md_stop_reason_name(stop));
    if (stop == MD_STOP_FAULT) {
        md_say("[microDOS] fault linear=%05lX opcode=%02X\n",
               (unsigned long)g_sys.runtime.fault_linear, g_sys.runtime.fault_opcode);
    }
    md_stats(start_us);
    for (;;) sleep_ms(1000);
}
