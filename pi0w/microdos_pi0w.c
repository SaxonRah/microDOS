/* microDOS bare-metal frontend for Raspberry Pi Zero 2 W.
 *
 * No Linux, no hosted libc, no filesystem. Raspberry Pi firmware loads this
 * image as kernel8.img and execution stays bare metal thereafter.
 *
 * Console: mini UART on GPIO14/15 at 115200 8N1.
 * Disk:    embedded 360 KiB FAT12 image copied to RAM; writes last until reset.
 * CPU:     AArch64 Cortex-A53. Static dosrecomp AOT compiles as native AArch64 C.
 *
 * The RP2350 Thumb-2 runtime JIT is intentionally not linked here; an AArch64
 * runtime JIT is a separate backend.
 */
#include "md_dos2_system.h"
#include "dos2test_recomp.h"
#include "msdos2_recomp.h"

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

static uint32_t g_boot_int_trace_count;
static uint32_t g_con_read_count;

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

static void uart_put_hex16(uint16_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int shift;
    for (shift = 12; shift >= 0; shift -= 4)
        uart_putc_raw((uint8_t)hex[(value >> shift) & 0x0Fu]);
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
    bool ok;

    ++g_con_read_count;

    uart_puts("\n[CONDBG] con_read ENTER #");
    uart_put_u64(g_con_read_count);
    uart_puts(" CS:IP=");
    uart_put_hex16(g_sys.runtime.cpu.cs);
    uart_putc_raw(':');
    uart_put_hex16(g_sys.runtime.cpu.ip);
    uart_puts(" ins=");
    uart_put_u64(g_sys.runtime.instructions);
    uart_puts(" rxready=");
    uart_put_u64(uart_rx_ready() ? 1u : 0u);
    uart_putc('\n');

    if (con->have_pending) {
        *value = con->pending;
        con->have_pending = false;

        uart_puts("[CONDBG] con_read RETURN pending value=");
        uart_put_hex16((uint16_t)*value);
        uart_putc('\n');
        return true;
    }

    uart_puts("[CONDBG] blocking in pi0_fetch(true)\n");
    ok = pi0_fetch(con, true, value);

    uart_puts("[CONDBG] con_read RETURN ok=");
    uart_put_u64(ok ? 1u : 0u);
    uart_puts(" value=");
    uart_put_hex16((uint16_t)*value);
    uart_putc('\n');

    return ok;
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

static bool pi0_boot_interrupt_debug(MdRuntime *runtime,
                                     uint8_t vector,
                                     void *user)
{
    MdMsdos2Boot *boot = (MdMsdos2Boot *)user;
    bool trace = false;
    bool handled;

    if (vector >= MD_MSDOS2_NATIVE_STRATEGY_INT &&
        vector <= MD_MSDOS2_NATIVE_POSTINIT_STDIO_FAIL_INT &&
        g_boot_int_trace_count < 128u) {
        uint8_t req = 0xFFu;

        ++g_boot_int_trace_count;
        trace = true;

        if (boot->have_request) {
            /* DOS 2 request header byte 2 is the command/function. */
            req = md_x86_read8(&runtime->cpu,
                               boot->request_segment,
                               (uint16_t)(boot->request_offset + 2u));
        }

        uart_puts("\n[INTDBG] ENTER #");
        uart_put_u64(g_boot_int_trace_count);
        uart_puts(" vec=");
        uart_put_hex16((uint16_t)vector);
        uart_puts(" CS:IP=");
        uart_put_hex16(runtime->cpu.cs);
        uart_putc_raw(':');
        uart_put_hex16(runtime->cpu.ip);
        uart_puts(" ins=");
        uart_put_u64(runtime->instructions);
        uart_puts(" dev=");
        uart_put_hex16(boot->last_device_offset);
        uart_puts(" req=");
        uart_put_hex16((uint16_t)req);
        uart_puts(" have=");
        uart_put_u64(boot->have_request ? 1u : 0u);
        uart_putc('\n');
    }

    handled = md_msdos2_boot_interrupt(runtime, vector, user);

    if (trace) {
        uart_puts("[INTDBG] RETURN vec=");
        uart_put_hex16((uint16_t)vector);
        uart_puts(" handled=");
        uart_put_u64(handled ? 1u : 0u);
        uart_puts(" stop=");
        uart_put_u64((uint64_t)runtime->stop_reason);
        uart_puts(" CS:IP=");
        uart_put_hex16(runtime->cpu.cs);
        uart_putc_raw(':');
        uart_put_hex16(runtime->cpu.ip);
        uart_puts(" ins=");
        uart_put_u64(runtime->instructions);
        uart_puts(" lastdev=");
        uart_put_hex16(boot->last_device_offset);
        uart_puts(" lastreq=");
        uart_put_hex16((uint16_t)boot->last_request_function);
        uart_putc('\n');
    }

    return handled;
}

static void print_stats(void)
{
    uart_puts("\n[pi0w] instructions=");
    uart_put_u64(g_sys.runtime.instructions);
    uart_puts(" aot=");
    uart_put_u64(g_sys.runtime.aot_instructions);
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
}

void kernel_main(void)
{
    const size_t kernel_size =
        (size_t)(md_blob_msdos_sys_end - md_blob_msdos_sys);
    const size_t disk_size =
        (size_t)(md_blob_disk_end - md_blob_disk);
    MdStopReason stop = MD_STOP_NONE;

    uart_init();

    uart_puts("\nmicroDOS - Raspberry Pi Zero 2 W bare metal\n");
    uart_puts("  cpu:     Cortex-A53 / AArch64\n");
    uart_puts("  console: GPIO14/15 mini UART, 115200 8N1\n");
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
     * Stage 4: initialize portable DOS/runtime system.
     *
     * With M23 this also exercises the execution-router
     * initialization path, so this checkpoint is important.
     */
    {
        MdHooks hooks;

        uart_puts("[04a] memset MdDos2System BEGIN\n");

        memset(&g_sys, 0, sizeof(g_sys));

        uart_puts("[04a] memset MdDos2System OK\n");


        uart_puts("[04b] md_msdos2_boot_init BEGIN\n");

        md_msdos2_boot_init(&g_sys.boot);
        g_sys.boot.continue_after_dosinit = true;

        uart_puts("[04b] md_msdos2_boot_init OK\n");


        uart_puts("[04c] md_exec_router_init BEGIN\n");

        md_exec_router_init(&g_sys.router);

        uart_puts("[04c] md_exec_router_init OK\n");


        uart_puts("[04d] hooks setup BEGIN\n");

        memset(&hooks, 0, sizeof(hooks));
        hooks.interrupt = pi0_boot_interrupt_debug;
        hooks.user = &g_sys.boot;

        uart_puts("[04d] hooks setup OK\n");


        uart_puts("[04e1] runtime memset BEGIN\n");
        memset(&g_sys.runtime, 0, sizeof(g_sys.runtime));
        uart_puts("[04e1] runtime memset OK\n");

        uart_puts("[04e2] cpu.memory BEGIN\n");
        g_sys.runtime.cpu.memory = g_guest;
        uart_puts("[04e2] cpu.memory OK\n");

        uart_puts("[04e3] flags BEGIN\n");
        md_x86_set_flags(&g_sys.runtime.cpu, MD_X86_FLAG_ALWAYS1);
        uart_puts("[04e3] flags OK\n");

        uart_puts("[04e4] epochs BEGIN\n");

        uart_puts("[04e4a] runtime address = ");
        uart_put_u64((uint64_t)(uintptr_t)&g_sys.runtime);
        uart_puts("\n[04e4a] code_epoch address = ");
        uart_put_u64((uint64_t)(uintptr_t)&g_sys.runtime.code_epoch);
        uart_puts("\n[04e4a] code_write_epoch address = ");
        uart_put_u64((uint64_t)(uintptr_t)&g_sys.runtime.code_write_epoch);
        uart_puts("\n");

        uart_puts("[04e4b] code_epoch store BEGIN\n");
        *(volatile uint32_t *)&g_sys.runtime.code_epoch = 1u;
        __asm__ volatile("dmb sy" ::: "memory");
        uart_puts("[04e4b] code_epoch store OK, readback=");
        uart_put_u64((uint64_t)*(volatile uint32_t *)&g_sys.runtime.code_epoch);
        uart_puts("\n");

        uart_puts("[04e4c] code_write_epoch store BEGIN\n");
        *(volatile uint32_t *)&g_sys.runtime.code_write_epoch = 1u;
        __asm__ volatile("dmb sy" ::: "memory");
        uart_puts("[04e4c] code_write_epoch store OK, readback=");
        uart_put_u64((uint64_t)*(volatile uint32_t *)&g_sys.runtime.code_write_epoch);
        uart_puts("\n");

        uart_puts("[04e4] epochs OK\n");

        uart_puts("[04e5] hooks copy BEGIN\n");
        g_sys.runtime.hooks = hooks;
        uart_puts("[04e5] hooks copy OK\n");

        /* Manual expansion of md_runtime_bind_tracking(). */

        uart_puts("[04e6] generation ptr BEGIN\n");
        g_sys.runtime.cpu.code_page_generation =
            g_sys.runtime.code_page_generation;
        uart_puts("[04e6] generation ptr OK\n");

        uart_puts("[04e7] executable ptr BEGIN\n");
        g_sys.runtime.cpu.code_page_executable =
            g_sys.runtime.code_page_executable;
        uart_puts("[04e7] executable ptr OK\n");

        uart_puts("[04e8] write epoch ptr BEGIN\n");
        g_sys.runtime.cpu.code_write_epoch =
            &g_sys.runtime.code_write_epoch;
        uart_puts("[04e8] write epoch ptr OK\n");

        uart_puts("[04e9] AOT guard ptr BEGIN\n");
        g_sys.runtime.cpu.aot_guards =
            g_sys.runtime.aot_slots;
        uart_puts("[04e9] AOT guard ptr OK\n");

        uart_puts("[04e10] owner memset BEGIN\n");
        memset(g_sys.runtime.aot_page_owner,
            0,
            sizeof(g_sys.runtime.aot_page_owner));
        uart_puts("[04e10] owner memset OK\n");

        uart_puts("[04e11] owner ptr BEGIN\n");
        g_sys.runtime.cpu.aot_page_owner =
            g_sys.runtime.aot_page_owner;
        uart_puts("[04e11] owner ptr OK\n");

        uart_puts("[04e12] live bits memset BEGIN\n");
        memset(g_sys.runtime.aot_live_bits,
            0,
            sizeof(g_sys.runtime.aot_live_bits));
        uart_puts("[04e12] live bits memset OK\n");

        uart_puts("[04e13] live pool BEGIN\n");
        g_sys.runtime.aot_live_pool_used = 0u;
        uart_puts("[04e13] live pool OK\n");

        uart_puts("[04e14] live bits ptr BEGIN\n");
        g_sys.runtime.cpu.aot_live_bits =
            g_sys.runtime.aot_live_bits;
        uart_puts("[04e14] live bits ptr OK\n");

        uart_puts("[04e15] guard count BEGIN\n");
        g_sys.runtime.cpu.aot_guard_count = 0u;
        uart_puts("[04e15] guard count OK\n");

        uart_puts("[04e] manual md_runtime_init OK\n");


        uart_puts("[04f] cache setup BEGIN\n");

        g_sys.cache = NULL;

        uart_puts("[04f] cache setup OK\n");
    }

    uart_puts("[04] manual md_dos2_system_init OK\n");

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
     * BRING-UP TEST:
     *
     * Intentionally disable ALL static AOT.
     *
     * We want the first Pi boot to prove that the portable
     * interpreter + DOS environment works independently of
     * generated native AOT.
     */
    uart_puts("[06] disabling AOT for bring-up\n");

    md_dos2_system_set_aot(&g_sys, NULL, 0u, false);

    uart_puts("[06] interpreter-only mode selected\n");

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
    uart_puts("  AOT:     OFF - diagnostic interpreter boot\n");
    uart_puts("  JIT:     OFF\n");
    uart_puts("  keys:    Ctrl+] prints execution statistics\n");
    uart_puts("\n");

    /*
     * Stage 8 diagnostic:
     *
     * Do NOT enter md_dos2_system_run() yet.  The previous trace proved that
     * all bootstrap/device interrupts through instruction 90 return normally,
     * then the direct run loop stops making observable progress.
     *
     * Step the canonical interpreter one instruction at a time instead.  This
     * tells us whether one instruction semantic itself blocks, or whether the
     * GNU direct-threaded run loop is the failing layer.
     */
    uart_puts("[08] STEP diagnostic BEGIN\n");
    uart_puts("     tracing instructions 80 through 300\n");

    while (g_sys.runtime.stop_reason == MD_STOP_NONE &&
           g_sys.runtime.instructions < 300u) {
        MdRuntime *rt = &g_sys.runtime;
        const uint64_t before = rt->instructions;
        const uint16_t cs = rt->cpu.cs;
        const uint16_t ip = rt->cpu.ip;
        const uint8_t opcode = md_x86_read8(&rt->cpu, cs, ip);

        if (opcode == 0xF6u) {
            uint8_t modrm;
            unsigned j;

            uart_puts("\n[F6DBG] exact live guest state before F6\n");
            uart_puts("[F6DBG] CS:IP=");
            uart_put_hex16(cs);
            uart_putc_raw(':');
            uart_put_hex16(ip);
            uart_puts(" ins=");
            uart_put_u64(before + 1u);
            uart_putc('\n');

            uart_puts("[F6DBG] bytes:");
            for (j = 0u; j < 12u; ++j) {
                const uint8_t b = md_x86_read8(&rt->cpu, cs, (uint16_t)(ip + j));
                uart_putc_raw(' ');
                uart_put_hex16((uint16_t)b);
            }
            uart_putc('\n');

            modrm = md_x86_read8(&rt->cpu, cs, (uint16_t)(ip + 1u));
            uart_puts("[F6DBG] modrm=");
            uart_put_hex16((uint16_t)modrm);
            uart_puts(" mod=");
            uart_put_u64((uint64_t)(modrm >> 6));
            uart_puts(" ext=");
            uart_put_u64((uint64_t)((modrm >> 3) & 7u));
            uart_puts(" rm=");
            uart_put_u64((uint64_t)(modrm & 7u));
            uart_putc('\n');

            uart_puts("[F6DBG] AX=");
            uart_put_hex16(rt->cpu.r[MD_X86_AX]);
            uart_puts(" BX=");
            uart_put_hex16(rt->cpu.r[MD_X86_BX]);
            uart_puts(" CX=");
            uart_put_hex16(rt->cpu.r[MD_X86_CX]);
            uart_puts(" DX=");
            uart_put_hex16(rt->cpu.r[MD_X86_DX]);
            uart_puts(" SP=");
            uart_put_hex16(rt->cpu.r[MD_X86_SP]);
            uart_puts(" BP=");
            uart_put_hex16(rt->cpu.r[MD_X86_BP]);
            uart_puts(" SI=");
            uart_put_hex16(rt->cpu.r[MD_X86_SI]);
            uart_puts(" DI=");
            uart_put_hex16(rt->cpu.r[MD_X86_DI]);
            uart_putc('\n');

            uart_puts("[F6DBG] CS=");
            uart_put_hex16(rt->cpu.cs);
            uart_puts(" DS=");
            uart_put_hex16(rt->cpu.ds);
            uart_puts(" ES=");
            uart_put_hex16(rt->cpu.es);
            uart_puts(" SS=");
            uart_put_hex16(rt->cpu.ss);
            uart_puts(" FLAGS=");
            uart_put_hex16(md_x86_flags(&rt->cpu));
            uart_putc('\n');

            uart_puts("[F6DBG] HALT before executing F6\n");

            for (;;) {
                __asm__ volatile("wfe");
            }
        }

        if (before >= 80u) {
            uart_puts("\n[STEP] BEFORE ins=");
            uart_put_u64(before + 1u);
            uart_puts(" CS:IP=");
            uart_put_hex16(cs);
            uart_putc_raw(':');
            uart_put_hex16(ip);
            uart_puts(" OP=");
            uart_put_hex16((uint16_t)opcode);

            uart_puts(" AX=");
            uart_put_hex16(rt->cpu.r[MD_X86_AX]);
            uart_puts(" BX=");
            uart_put_hex16(rt->cpu.r[MD_X86_BX]);
            uart_puts(" CX=");
            uart_put_hex16(rt->cpu.r[MD_X86_CX]);
            uart_puts(" DX=");
            uart_put_hex16(rt->cpu.r[MD_X86_DX]);
            uart_puts(" SI=");
            uart_put_hex16(rt->cpu.r[MD_X86_SI]);
            uart_puts(" DI=");
            uart_put_hex16(rt->cpu.r[MD_X86_DI]);
            uart_puts(" DS=");
            uart_put_hex16(rt->cpu.ds);
            uart_puts(" ES=");
            uart_put_hex16(rt->cpu.es);
            uart_puts(" SS:SP=");
            uart_put_hex16(rt->cpu.ss);
            uart_putc_raw(':');
            uart_put_hex16(rt->cpu.r[MD_X86_SP]);
            uart_putc('\n');
        }

        stop = md_interp_step(rt);

        if (before >= 80u) {
            uart_puts("[STEP] AFTER  ins=");
            uart_put_u64(rt->instructions);
            uart_puts(" stop=");
            uart_put_u64((uint64_t)stop);
            uart_puts(" CS:IP=");
            uart_put_hex16(rt->cpu.cs);
            uart_putc_raw(':');
            uart_put_hex16(rt->cpu.ip);
            uart_putc('\n');
        }
    }

    uart_puts("\n[08] STEP diagnostic END\n");
    uart_puts("     instructions = ");
    uart_put_u64(g_sys.runtime.instructions);
    uart_puts("\n     stop         = ");
    uart_put_u64((uint64_t)g_sys.runtime.stop_reason);
    uart_puts("\n     CS:IP        = ");
    uart_put_hex16(g_sys.runtime.cpu.cs);
    uart_putc_raw(':');
    uart_put_hex16(g_sys.runtime.cpu.ip);
    uart_putc('\n');

    print_stats();

    for (;;) {
        __asm__ volatile("wfe");
    }
}
