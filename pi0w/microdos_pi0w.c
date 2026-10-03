/* microDOS bare-metal frontend for Raspberry Pi Zero 2 W.
 *
 * No Linux, no hosted libc, no filesystem. Raspberry Pi firmware loads this
 * image as kernel8.img and execution stays bare metal thereafter.
 *
 * Console: mini UART on GPIO14/15 at 115200 8N1.
 * Disk:    embedded 360 KiB FAT12 image copied to RAM; writes last until reset.
 * CPU:     AArch64 Cortex-A53. Static dosrecomp AOT compiles as native AArch64 C.
 *
 * The shared runtime-JIT core/router is common with Pico. On AArch64,
 * jit_core.c selects the v1 native backend: simple direct prefixes are emitted
 * as A64 machine code, while the first resident shapes use emitted A64
 * tail-call trampolines into the proven shared region helpers.
 */
#include "md_dos2_system.h"
#include "dos2test_recomp.h"
#include "msdos2_recomp.h"
#include "microdos/ops.h"
#include "microdos/jit.h"

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

#define MD_GUEST_BYTES (1u << 20)
#define MD_DISK_BYTES  (720u * 512u)
#define MD_SLICE       200000u

extern const uint8_t md_blob_msdos_sys[], md_blob_msdos_sys_end[];
extern const uint8_t md_blob_disk[], md_blob_disk_end[];

static uint8_t g_guest[MD_GUEST_BYTES] __attribute__((aligned(64)));
static uint8_t g_disk[MD_DISK_BYTES] __attribute__((aligned(64)));

static MdDos2System g_sys;
static const MdAotProgram *const g_programs[] = {
    &md_recomp_dos2test_program
};

typedef struct Pi0Console {
    bool have_pending;
    uint8_t pending;
    bool last_was_cr;
    bool stats_requested;
} Pi0Console;

static Pi0Console g_con;
static uint32_t g_disk_writes;

static uint64_t g_perf_last_ticks;
static uint64_t g_perf_last_instructions;
static uint64_t g_perf_last_aot_instructions;
static uint64_t g_perf_last_kernel_aot_instructions;
static uint64_t g_perf_last_jit_instructions;
static uint64_t g_perf_last_jit_native_instructions;
static uint64_t g_perf_last_jit_fallback_instructions;
static bool g_perf_valid;

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

    /* Enable mini UART. */
    mmio_write(AUX_ENABLES, mmio_read(AUX_ENABLES) | 1u);
    mmio_write(AUX_MU_CNTL, 0u);
    mmio_write(AUX_MU_IER, 0u);
    mmio_write(AUX_MU_LCR, 3u);      /* 8-bit mode */
    mmio_write(AUX_MU_MCR, 0u);
    mmio_write(AUX_MU_IIR, 0xC6u);   /* clear FIFOs */
    mmio_write(AUX_MU_BAUD, 270u);   /* 115200 at core_freq=250 MHz */

    /* GPIO14/15 -> ALT5 = mini UART TXD1/RXD1. */
    r = mmio_read(GPFSEL1);
    r &= ~((7u << 12) | (7u << 15));
    r |=  ((2u << 12) | (2u << 15));
    mmio_write(GPFSEL1, r);

    /* BCM2837-style pull disable sequence. */
    mmio_write(GPPUD, 0u);
    delay_cycles(150u);
    mmio_write(GPPUDCLK0, (1u << 14) | (1u << 15));
    delay_cycles(150u);
    mmio_write(GPPUDCLK0, 0u);

    mmio_write(AUX_MU_CNTL, 3u);     /* enable RX + TX */
}

static bool uart_rx_ready(void)
{
    return (mmio_read(AUX_MU_LSR) & 0x01u) != 0u;
}

static uint8_t uart_getc(void)
{
    while (!uart_rx_ready()) { }
    return (uint8_t)mmio_read(AUX_MU_IO);
}

static bool uart_getc_nonblock(uint8_t *value)
{
    if (!uart_rx_ready()) return false;
    *value = (uint8_t)mmio_read(AUX_MU_IO);
    return true;
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

static void uart_put_hex16(uint16_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int shift;
    for (shift = 12; shift >= 0; shift -= 4)
        uart_putc_raw((uint8_t)hex[(value >> shift) & 0x0Fu]);
}

static void uart_put_hex64(uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int shift;
    for (shift = 60; shift >= 0; shift -= 4)
        uart_putc_raw((uint8_t)hex[(value >> shift) & 0x0Fu]);
}

static inline uint64_t pi0_read_currentel(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return value;
}

static inline uint64_t pi0_read_sctlr(unsigned el)
{
    uint64_t value = 0u;

    switch (el) {
        case 1u:
            __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(value));
            break;
        case 2u:
            __asm__ volatile("mrs %0, SCTLR_EL2" : "=r"(value));
            break;
        case 3u:
            __asm__ volatile("mrs %0, SCTLR_EL3" : "=r"(value));
            break;
        default:
            break;
    }

    return value;
}

static inline uint64_t pi0_timer_ticks(void)
{
    uint64_t value;
    __asm__ volatile(
        "isb\n"
        "mrs %0, CNTPCT_EL0"
        : "=r"(value)
        :
        : "memory");
    return value;
}

static inline uint64_t pi0_timer_hz(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(value));
    return value;
}

static void pi0_perf_reset(void)
{
    g_perf_last_ticks = pi0_timer_ticks();
    g_perf_last_instructions = g_sys.runtime.instructions;
    g_perf_last_aot_instructions = g_sys.runtime.aot_instructions;
    g_perf_last_kernel_aot_instructions = g_sys.kernel_aot_instructions;
    g_perf_last_jit_instructions = g_sys.jit_instructions;
    g_perf_last_jit_native_instructions = g_sys.jit_native_instructions;
    g_perf_last_jit_fallback_instructions = g_sys.jit_fallback_instructions;
    g_perf_valid = true;
}

static void pi0_print_cpu_control_state(void)
{
    const uint64_t currentel = pi0_read_currentel();
    const unsigned el = (unsigned)((currentel >> 2) & 3u);
    uint64_t sctlr = 0u;

    uart_puts("[CPU] CurrentEL raw = 0x");
    uart_put_hex64(currentel);
    uart_putc('\n');

    uart_puts("[CPU] EL            = ");
    uart_put_u64(el);
    uart_putc('\n');

    if (el >= 1u && el <= 3u) {
        sctlr = pi0_read_sctlr(el);

        uart_puts("[CPU] SCTLR_EL");
        uart_put_u64(el);
        uart_puts("     = 0x");
        uart_put_hex64(sctlr);
        uart_putc('\n');

        uart_puts("[CPU] SCTLR.A       = ");
        uart_put_u64((sctlr >> 1) & 1u);
        uart_putc('\n');
    } else {
        uart_puts("[CPU] SCTLR         = unavailable at EL0\n");
    }

    uart_putc('\n');
}

static bool pi0_fetch(Pi0Console *con, bool block, uint8_t *value)
{
    for (;;) {
        uint8_t ch;
        if (block) ch = uart_getc();
        else if (!uart_getc_nonblock(&ch)) return false;

        if (ch == 0x1Du) { /* Ctrl+] => bare-metal statistics */
            con->stats_requested = true;
            if (!block) return false;
            continue;
        }
        if (ch == '\n' && con->last_was_cr) {
            con->last_was_cr = false;
            continue;
        }
        con->last_was_cr = (ch == '\r');
        *value = ch == '\n' ? '\r' : ch;
        return true;
    }
}

static void con_write(void *user, const uint8_t *data, size_t size)
{
    Pi0Console *con = (Pi0Console *)user;
    size_t i;
    (void)con;
    for (i = 0u; i < size; ++i) uart_putc_raw(data[i]);
}

static bool con_peek(void *user, uint8_t *value)
{
    Pi0Console *con = (Pi0Console *)user;
    if (!con->have_pending) {
        if (!pi0_fetch(con, false, &con->pending)) return false;
        con->have_pending = true;
    }
    *value = con->pending;
    return true;
}

static bool con_read(void *user, uint8_t *value)
{
    Pi0Console *con = (Pi0Console *)user;

    if (con->have_pending) {
        *value = con->pending;
        con->have_pending = false;
        return true;
    }

    return pi0_fetch(con, true, value);
}

static void con_flush(void *user)
{
    Pi0Console *con = (Pi0Console *)user;
    con->have_pending = false;
    while (uart_rx_ready()) (void)mmio_read(AUX_MU_IO);
}

static bool disk_read(void *user, uint32_t sector, uint8_t *data, size_t size)
{
    const size_t off = (size_t)sector * size;
    (void)user;
    if (size != 512u || off > MD_DISK_BYTES || MD_DISK_BYTES - off < size) return false;
    memcpy(data, g_disk + off, size);
    return true;
}

static bool disk_write(void *user, uint32_t sector, const uint8_t *data, size_t size)
{
    const size_t off = (size_t)sector * size;
    (void)user;
    if (size != 512u || off > MD_DISK_BYTES || MD_DISK_BYTES - off < size) return false;
    memcpy(g_disk + off, data, size);
    ++g_disk_writes;
    return true;
}


static void print_stats(void)
{
    const uint64_t now_ticks = pi0_timer_ticks();
    const uint64_t timer_hz = pi0_timer_hz();
    const uint64_t now_instructions = g_sys.runtime.instructions;
    const uint64_t now_aot = g_sys.runtime.aot_instructions;
    const uint64_t now_kernel_aot = g_sys.kernel_aot_instructions;
    const uint64_t now_jit = g_sys.jit_instructions;
    const uint64_t now_jit_native = g_sys.jit_native_instructions;
    const uint64_t now_jit_fallback = g_sys.jit_fallback_instructions;

    uart_puts("\n");

    if (g_perf_valid && timer_hz != 0u && now_ticks > g_perf_last_ticks) {
        const uint64_t delta_ticks = now_ticks - g_perf_last_ticks;
        const uint64_t delta_instructions =
            now_instructions >= g_perf_last_instructions
                ? now_instructions - g_perf_last_instructions
                : 0u;
        const uint64_t delta_aot =
            now_aot >= g_perf_last_aot_instructions
                ? now_aot - g_perf_last_aot_instructions
                : 0u;
        const uint64_t delta_kernel_aot =
            now_kernel_aot >= g_perf_last_kernel_aot_instructions
                ? now_kernel_aot - g_perf_last_kernel_aot_instructions
                : 0u;
        const uint64_t delta_app_aot =
            delta_aot >= delta_kernel_aot
                ? delta_aot - delta_kernel_aot
                : 0u;
        const uint64_t delta_jit =
            now_jit >= g_perf_last_jit_instructions
                ? now_jit - g_perf_last_jit_instructions
                : 0u;
        const uint64_t delta_jit_ref =
            now_jit_native >= g_perf_last_jit_native_instructions
                ? now_jit_native - g_perf_last_jit_native_instructions
                : 0u;
        const uint64_t delta_jit_fallback =
            now_jit_fallback >= g_perf_last_jit_fallback_instructions
                ? now_jit_fallback - g_perf_last_jit_fallback_instructions
                : 0u;
        const uint64_t accounted =
            delta_aot + delta_jit <= delta_instructions
                ? delta_aot + delta_jit
                : delta_instructions;
        const uint64_t delta_interp = delta_instructions - accounted;
        const uint64_t elapsed_us =
            (delta_ticks * 1000000u) / timer_hz;
        const uint64_t mips_milli =
            elapsed_us != 0u
                ? (delta_instructions * 1000u) / elapsed_us
                : 0u;

        uart_puts("[perf] interval ");
        uart_put_u64(elapsed_us);
        uart_puts(" us  instructions ");
        uart_put_u64(delta_instructions);
        uart_puts("  ");
        uart_put_fixed3(mips_milli);
        uart_puts(" MIPS\n");

        uart_puts("[perf] tiers aot=");
        uart_put_u64(delta_aot);
        uart_puts(" kernel=");
        uart_put_u64(delta_kernel_aot);
        uart_puts(" app=");
        uart_put_u64(delta_app_aot);
        uart_puts(" jit=");
        uart_put_u64(delta_jit);
        uart_puts(" native=");
        uart_put_u64(delta_jit_ref);
        uart_puts(" jit-fallback=");
        uart_put_u64(delta_jit_fallback);
        uart_puts(" interp=");
        uart_put_u64(delta_interp);
        uart_putc('\n');
    } else {
        uart_puts("[perf] interval unavailable\n");
    }

    uart_puts("[pi0w] total instructions=");
    uart_put_u64(now_instructions);
    uart_puts(" aot=");
    uart_put_u64(now_aot);
    uart_puts(" kernel-aot=");
    uart_put_u64(now_kernel_aot);
    uart_puts(" jit=");
    uart_put_u64(now_jit);
    uart_puts(" native=");
    uart_put_u64(now_jit_native);
    uart_puts(" jit-fallback=");
    uart_put_u64(now_jit_fallback);
    uart_puts(" bios=");
    uart_put_u64(g_sys.bios_interpreted_instructions);
    uart_puts(" attaches=");
    uart_put_u64(g_sys.aot_attaches);
    uart_puts(" enters=");
    uart_put_u64(g_sys.aot_enters);
    uart_puts(" disk-writes=");
    uart_put_u64(g_disk_writes);
    uart_puts(" CS:IP=");
    uart_put_hex16(g_sys.runtime.cpu.cs);
    uart_putc_raw(':');
    uart_put_hex16(g_sys.runtime.cpu.ip);
    uart_putc('\n');

    if (g_sys.jit != NULL) {
        uart_puts("[jit-a64] lookups=");
        uart_put_u64(g_sys.jit->lookups);
        uart_puts(" hits=");
        uart_put_u64(g_sys.jit->hits);
        uart_puts(" misses=");
        uart_put_u64(g_sys.jit->misses);
        uart_puts(" compiles=");
        uart_put_u64(g_sys.jit->compiles);
        uart_puts(" entries=");
        uart_put_u64(g_sys.jit->native_entries);
        uart_puts(" resident=");
        uart_put_u64(g_sys.jit->resident_entries);
        uart_puts(" code=");
        uart_put_u64(g_sys.jit->code_used);
        uart_puts(" helpers=");
        uart_put_u64(g_sys.jit->helper_sites);
        uart_puts(" direct=");
        uart_put_u64(g_sys.jit->direct_instructions);
        uart_puts(" flushes=");
        uart_put_u64(g_sys.jit->flushes);
        uart_putc('\n');
    }

    g_perf_last_ticks = now_ticks;
    g_perf_last_instructions = now_instructions;
    g_perf_last_aot_instructions = now_aot;
    g_perf_last_kernel_aot_instructions = now_kernel_aot;
    g_perf_last_jit_instructions = now_jit;
    g_perf_last_jit_native_instructions = now_jit_native;
    g_perf_last_jit_fallback_instructions = now_jit_fallback;
    g_perf_valid = true;
}

void kernel_main(void)
{
    const size_t kernel_size =
        (size_t)(md_blob_msdos_sys_end - md_blob_msdos_sys);
    const size_t disk_size =
        (size_t)(md_blob_disk_end - md_blob_disk);
    MdStopReason stop = MD_STOP_NONE;

    uart_init();
    pi0_print_cpu_control_state();

    uart_puts("\nmicroDOS - Raspberry Pi Zero 2 W bare metal\n");
    uart_puts("  cpu:     Cortex-A53 / AArch64\n");
    uart_puts("  console: GPIO14/15 mini UART, 115200 8N1\n");
    uart_puts("  timer:   ARM generic physical counter, ");
    uart_put_u64(pi0_timer_hz());
    uart_puts(" Hz\n");
    uart_puts("  host OS: none\n\n");

    /*
     * Stage 1: verify embedded blobs.
     */
    uart_puts("[01] checking embedded blobs\n");

    uart_puts("     MSDOS.SYS bytes = ");
    uart_put_u64(kernel_size);
    uart_putc('\n');

    uart_puts("     disk bytes      = ");
    uart_put_u64(disk_size);
    uart_putc('\n');

    if (kernel_size != 16690u) {
        uart_puts("FATAL: embedded MSDOS.SYS size mismatch\n");
        for (;;) __asm__ volatile("wfe");
    }

    if (disk_size != MD_DISK_BYTES) {
        uart_puts("FATAL: embedded disk size mismatch\n");
        for (;;) __asm__ volatile("wfe");
    }

    uart_puts("[01] blobs OK\n");

    /*
     * Stage 2: guest RAM.
     */
    uart_puts("[02] clearing 1 MiB guest RAM\n");

    memset(g_guest, 0, sizeof(g_guest));

    uart_puts("[02] guest RAM OK\n");

    /*
     * Stage 3: RAM disk.
     */
    uart_puts("[03] copying 360 KiB disk image\n");

    memcpy(g_disk, md_blob_disk, sizeof(g_disk));

    uart_puts("[03] disk image OK\n");

    /*
     * Stage 4: initialize the portable DOS/runtime system through the real
     * platform-neutral initializer.  This installs the normal
     * md_msdos2_boot_interrupt hook.
     */
    uart_puts("[04] md_dos2_system_init BEGIN\n");

    md_dos2_system_init(&g_sys, g_guest, NULL);

    uart_puts("[04] md_dos2_system_init OK\n");

    /*
     * Stage 5: platform callbacks.
     */
    uart_puts("[05] installing console/disk callbacks\n");

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

    /*
     * Deterministic initial DOS clock.
     */
    g_sys.boot.clock_days = 1162u;
    g_sys.boot.clock_hours = 12u;

    uart_puts("[05] callbacks OK\n");

    /*
     * Static AOT test.
     *
     * These are the same generated AOT programs used by the portable
     * microDOS system layer on Pico.  On Pi Zero 2 W they are compiled by
     * the AArch64 cross-compiler into native Cortex-A53 code.
     *
     * Runtime JIT remains disabled for this test.
     */
    uart_puts("[06] enabling static AOT\n");

    md_dos2_system_set_kernel_aot(&g_sys, &md_recomp_msdos2_program);
    md_dos2_system_set_aot(
        &g_sys,
        g_programs,
        sizeof(g_programs) / sizeof(g_programs[0]),
        true);

    uart_puts("[06] static AOT selected; AArch64 native JIT v1 enabled\n");

    /*
     * Stage 7: load MSDOS.SYS and construct the synthetic
     * SYSINIT/BIOS environment.
     */
    uart_puts("[07] md_dos2_system_start BEGIN\n");

    if (!md_dos2_system_start(&g_sys,
                              md_blob_msdos_sys,
                              kernel_size)) {
        uart_puts("FATAL: MSDOS.SYS rejected\n");
        for (;;) __asm__ volatile("wfe");
    }

    uart_puts("[07] md_dos2_system_start OK\n");

    uart_puts("\n");
    uart_puts("  guest:   1 MiB RAM\n");
    uart_puts("  disk:    360 KiB FAT12 RAM disk\n");

    uart_puts("  AOT:     MSDOS.SYS ");
    uart_put_u64(md_recomp_msdos2_program.compiled_instructions);
    uart_puts(" compiled, ");
    uart_put_u64(md_recomp_msdos2_program.hole_instructions);
    uart_puts(" holes, ");
    uart_puts(g_sys.kernel_attached ? "attached\n" : "NOT ATTACHED\n");

    uart_puts("  AOT:     ");
    uart_puts(md_recomp_dos2test_program.name);
    uart_puts(" ");
    uart_put_u64(md_recomp_dos2test_program.compiled_instructions);
    uart_puts(" compiled, ");
    uart_put_u64(md_recomp_dos2test_program.hole_instructions);
    uart_puts(" holes\n");

    uart_puts("  JIT:     AArch64 native v1 - emitted A64 direct prefixes + resident trampolines\n");
    uart_puts("  keys:    Ctrl+] prints rolling MIPS + AOT/JIT/native tier statistics\n");
    uart_puts("\n");

    /*
     * Stage 8:
     *
     * The Pi memory-type failure is now understood and fixed at startup:
     *
     *   - EL2 stage-1 MMU enabled by start.S.
     *   - ordinary RAM is mapped as Normal memory.
     *   - 0x3F000000..0x3FFFFFFF remains Device-nGnRnE for peripherals.
     *   - GCC uses its normal AArch64 code generation; -mstrict-align is OFF.
     *
     * The real md_interp_step() Group-3/F6 path has been proven through
     * instruction 300 under that configuration.  Stop single-stepping here
     * and return to the platform-neutral DOS system loop.
     *
     * Static AOT remains highest priority for MSDOS.SYS and DOS2TEST.COM.
     * MICRODOS_ENABLE_JIT adds the shared execution router/JIT core for other
     * segments. On AArch64, jit_core.c now selects the v1 native emitter:
     * simple direct prefixes execute as generated A64, while the first two
     * resident shapes execute through emitted A64 tail-call trampolines into
     * the already-proven shared region helpers.
     */
    uart_puts("[08] static AOT + AArch64 native JIT v1 run BEGIN\n");
    uart_puts("     md_dos2_system_run() slices = 50000 instructions\n");
    uart_puts("     Ctrl+] = rolling MIPS + AOT/JIT/native tier sample\n");

    pi0_perf_reset();

    {
        const uint64_t slice_budget = 50000u;

        for (;;) {
            /*
             * Ctrl+] is consumed by the console shim and requests a statistics
             * snapshot.  There is no automatic periodic stats output.
             */
            if (g_con.stats_requested) {
                g_con.stats_requested = false;
                print_stats();
            }

            stop = md_dos2_system_run(&g_sys, slice_budget);

            if (stop != MD_STOP_NONE) {
                uart_puts("\n[08] static AOT + AArch64 native JIT v1 run STOP\n");
                uart_puts("     stop         = ");
                uart_put_u64((uint64_t)stop);
                uart_puts("\n     instructions = ");
                uart_put_u64(g_sys.runtime.instructions);
                uart_puts("\n     CS:IP        = ");
                uart_put_hex16(g_sys.runtime.cpu.cs);
                uart_putc_raw(':');
                uart_put_hex16(g_sys.runtime.cpu.ip);
                uart_putc('\n');
                print_stats();
                break;
            }
        }
    }

    for (;;) {
        __asm__ volatile("wfe");
    }
}
