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
 * Keys: Ctrl+] prints runtime statistics (it is not passed to DOS).
 */
#include "md_dos2_system.h"
#include "dos2test_recomp.h"

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
    size_t i;
    con->idle_polls = 0u;
    for (i = 0; i < size; ++i) putchar_raw((int)data[i]);
    stdio_flush();
}

static bool con_peek(void *user, uint8_t *value)
{
    PicoConsole *con = (PicoConsole *)user;
    if (!con->have_pending) {
        if (!pico_fetch(con, false, &con->pending)) {
            if (++con->idle_polls >= MD_IDLE_POLLS) {
                ++con->idle_sleeps;
                sleep_us(500);                    /* idle prompt: stop spinning */
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
    return pico_fetch(con, true, value);
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
    memcpy(data, g_disk + off, size);
    return true;
}

static bool disk_write(void *user, uint32_t sector, const uint8_t *data, size_t size)
{
    const size_t off = (size_t)sector * size;
    (void)user;
    if (size != 512u || off + size > MD_DISK_BYTES) return false;
    g_con.idle_polls = 0u;
    memcpy(g_disk + off, data, size);
    ++g_disk_writes;
    return true;
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

static void md_stats(uint64_t start_us)
{
    const MdRuntime *rt = &g_sys.runtime;
    const uint64_t us = time_us_64() - start_us;
    const double mips = us != 0u ? (double)rt->instructions / (double)us : 0.0;

    md_say("\n[stats] uptime=%.1fs instructions=%llu avg=%.2f MIPS\n",
           (double)us / 1e6, (unsigned long long)rt->instructions, mips);
    md_say("[stats] aot: %s attaches=%lu enters=%lu compiled=%llu\n",
           g_programs[0]->name, (unsigned long)g_sys.aot_attaches,
           (unsigned long)g_sys.aot_enters, (unsigned long long)rt->aot_instructions);
    md_say("[stats] cache: hits=%llu misses=%llu invalidations=%llu fallback=%llu\n",
           (unsigned long long)g_cache.hits, (unsigned long long)g_cache.misses,
           (unsigned long long)g_cache.invalidations,
           (unsigned long long)g_cache.fallback_instructions);
    md_say("[stats] console polls=%lu idle-sleeps=%lu disk sector writes=%lu (RAM only)\n",
           (unsigned long)g_sys.boot.console_poll_calls, (unsigned long)g_con.idle_sleeps,
           (unsigned long)g_disk_writes);
    md_say("[stats] CS:IP=%04X:%04X\n", rt->cpu.cs, rt->cpu.ip);
}

int main(void)
{
    const size_t kernel_size = (size_t)(md_blob_msdos_sys_end - md_blob_msdos_sys);
    const size_t disk_size = (size_t)(md_blob_disk_end - md_blob_disk);
    uint64_t start_us;
    MdStopReason stop = MD_STOP_NONE;

#if MICRODOS_PICO_SYS_KHZ > 0
    set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ, true);
#endif
    stdio_init_all();
    while (!stdio_usb_connected()) sleep_ms(20);    /* wait for a terminal */
    sleep_ms(300);

    md_say("\nmicroDOS for Pico 2 (Pimoroni Pico Plus 2)\n");
    md_say("  clk_sys: %lu MHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
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

    md_dos2_system_init(&g_sys, g_guest, &g_cache);
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
    md_dos2_system_set_aot(&g_sys, g_programs, 1u, true);

    if (!md_dos2_system_start(&g_sys, md_blob_msdos_sys, kernel_size)) {
        md_say("microDOS: embedded MSDOS.SYS rejected (%lu bytes); halting.\n",
               (unsigned long)kernel_size);
        for (;;) sleep_ms(1000);
    }
    md_say("  kernel:  MSDOS.SYS %lu bytes\n", (unsigned long)kernel_size);
    md_say("  aot:     %s (%lu compiled, %lu holes)\n", g_programs[0]->name,
           (unsigned long)g_programs[0]->compiled_instructions,
           (unsigned long)g_programs[0]->hole_instructions);
    md_say("  keys:    Ctrl+] -> statistics\n");

    start_us = time_us_64();
    for (;;) {
        stop = md_dos2_system_run(&g_sys, MD_SLICE);
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
