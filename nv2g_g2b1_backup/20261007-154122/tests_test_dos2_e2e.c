/* M14 end-to-end test: boot the released MS-DOS 2.0 through the portable
 * system loop (the same code the Pico 2 runs), answer the date/time prompts,
 * run DOS2TEST at A>, and require "ALL TESTS PASSED".
 *
 * usage: microdos_dos2_e2e MSDOS.SYS disk.img [--no-aot] [--no-cache] [--no-kernel-aot]
 *        microdos_dos2_e2e MSDOS.SYS disk.img --profile-kernel OUT.entries
 *
 * --profile-kernel (M17) runs the same session single-stepped in the
 * interpreter and writes every kernel offset reached by a non-sequential
 * transfer (the block entries static analysis cannot find: IVT handlers,
 * push/ret dispatch, device-return paths). It also reports whether any
 * executed kernel code byte differs from MSDOS.SYS at the end.
 */
#include "md_dos2_system.h"
#include "microdos/decode.h"
#include "dos2test_recomp.h"
#include "msdos2_recomp.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef MICRODOS_ENABLE_JIT
/* M20.4: the runtime JIT in the full DOS session. Built for ARM and run under
   qemu-arm (tools/jit_qemu_check.sh) this executes the generated Thumb-2
   with the Pico's exact JIT configuration and reports the same counters as
   the firmware's [jit] block. */
#include "microdos/jit.h"
#if defined(__linux__)
#include <sys/mman.h>
#endif
#ifndef MICRODOS_SYSTEM_JIT_CODE_BYTES
#define MICRODOS_SYSTEM_JIT_CODE_BYTES (24u * 1024u)
#endif
static MdJit g_e2e_jit;

static void e2e_attach_jit(MdDos2System *sys)
{
    uint8_t *code = NULL;
#if defined(__linux__)
    void *p = mmap(NULL, MICRODOS_SYSTEM_JIT_CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) code = (uint8_t *)p;
#endif
    if (code == NULL) code = (uint8_t *)calloc(1u, MICRODOS_SYSTEM_JIT_CODE_BYTES);
    md_jit_init(&g_e2e_jit, code, MICRODOS_SYSTEM_JIT_CODE_BYTES);
    md_dos2_system_set_jit(sys, &g_e2e_jit);
}

static void e2e_report_jit(const MdDos2System *sys)
{
    const MdJit *j = &g_e2e_jit;
    unsigned i;
    printf("[jit] native=%llu fallback=%llu control=%llu compile=%llu hit=%llu miss=%llu invalid=%llu flush=%llu\n",
           (unsigned long long)j->direct_instructions, (unsigned long long)j->fallback_instructions,
           (unsigned long long)j->control_instructions, (unsigned long long)j->compiles,
           (unsigned long long)j->hits, (unsigned long long)j->misses,
           (unsigned long long)j->invalidations, (unsigned long long)j->flushes);
    printf("[jit] fallback-reason zero=%llu cold=%llu budget=%llu compile=%llu  resident=%llu/%llu  code=%lu/%lu B  bios-bypass=%llu\n",
           (unsigned long long)j->zero_progress_fallbacks, (unsigned long long)j->cold_fallbacks,
           (unsigned long long)j->budget_fallbacks, (unsigned long long)j->compile_fail_fallbacks,
           (unsigned long long)j->resident_regions, (unsigned long long)j->resident_instructions,
           (unsigned long)j->code_used, (unsigned long)j->code_size,
           (unsigned long long)j->bios_bypass_instructions);
    printf("[jit] tiers: jit-owned=%llu jit-native=%llu jit-fallback=%llu\n",
           (unsigned long long)sys->jit_instructions, (unsigned long long)sys->jit_native_instructions,
           (unsigned long long)sys->jit_fallback_instructions);
    for (i = 0; i < MD_JIT_HOT_SITES; ++i) {
        const MdJitHotSite *h = &j->hot_sites[i];
        if (h->count == 0u) continue;
        printf("[jit]   %8llu  %04X:%04X op=%02X -> %04X:%04X  %s\n", (unsigned long long)h->count,
               h->cs, h->ip, h->opcode, h->dst_cs, h->dst_ip, md_jit_exit_reason_name(h->reason));
    }
}
#endif
#include <string.h>
#include <time.h>

#define E2E_OUT_MAX (64u * 1024u)
#define E2E_BUDGET 60000000ull

typedef struct E2eStep { const char *after; const char *keys; } E2eStep;

static const E2eStep kScript[] = {
    { "Enter new date", "\r" },
    { "Enter new time", "\r" },
    { "A>", "DOS2TEST\r" },
};

typedef struct E2e {
    char out[E2E_OUT_MAX];
    size_t out_len;
    size_t step;
    size_t search_from;
    const char *keys;           /* released keys not yet consumed */
    uint8_t *disk;
    size_t disk_size;
    bool quiet;
} E2e;

/* Tools that #include this harness (nv2g_census) may supply their own script. */
#ifndef E2E_SCRIPT
#define E2E_SCRIPT kScript
#define E2E_SCRIPT_COUNT (sizeof(kScript) / sizeof(kScript[0]))
#endif

static void e2e_release(E2e *e)
{
    if ((e->keys == NULL || *e->keys == '\0') &&
        e->step < E2E_SCRIPT_COUNT) {
        const char *hit;
        e->out[e->out_len] = '\0';
        hit = strstr(e->out + e->search_from, E2E_SCRIPT[e->step].after);
        if (hit != NULL) {
            e->search_from = (size_t)(hit - e->out) + strlen(E2E_SCRIPT[e->step].after);
            e->keys = E2E_SCRIPT[e->step].keys;
            ++e->step;
        }
    }
}

static void con_write(void *user, const uint8_t *data, size_t size)
{
    E2e *e = (E2e *)user;
    size_t i;
    for (i = 0; i < size && e->out_len + 1u < E2E_OUT_MAX; ++i) e->out[e->out_len++] = (char)data[i];
    if (!e->quiet) fwrite(data, 1u, size, stdout);
}
static bool con_peek(void *user, uint8_t *v)
{
    E2e *e = (E2e *)user;
    e2e_release(e);
    if (e->keys == NULL || *e->keys == '\0') return false;
    *v = (uint8_t)*e->keys;
    return true;
}
static bool con_read(void *user, uint8_t *v)
{
    E2e *e = (E2e *)user;
    if (!con_peek(user, v)) return false;
    ++e->keys;
    return true;
}
static void con_flush(void *user) { (void)user; }

static bool disk_read(void *user, uint32_t sector, uint8_t *data, size_t size)
{
    E2e *e = (E2e *)user;
    if ((size_t)sector * size + size > e->disk_size) return false;
    memcpy(data, e->disk + (size_t)sector * size, size);
    return true;
}
static bool disk_write(void *user, uint32_t sector, const uint8_t *data, size_t size)
{
    E2e *e = (E2e *)user;
    if ((size_t)sector * size + size > e->disk_size) return false;
    memcpy(e->disk + (size_t)sector * size, data, size);
    return true;
}

#define E2E_KERNEL_SEG 0x1000u

/* Profile-guided discovery for the kernel image (see header comment). */
static int e2e_profile(MdDos2System *sys, E2e *e, const uint8_t *kernel, size_t kernel_size,
                       const char *out_path)
{
    static uint8_t target[0x10000];
    static uint8_t executed[0x10000];
    MdRuntime *rt = &sys->runtime;
    uint8_t *kmem = rt->cpu.memory + ((uint32_t)E2E_KERNEL_SEG << 4);
    FILE *fp;
    uint32_t i, targets = 0, exec_count = 0, changed = 0;

    while (rt->instructions < E2E_BUDGET && rt->stop_reason == MD_STOP_NONE) {
        const uint16_t cs = rt->cpu.cs, ip = rt->cpu.ip;
        MdDecodedInstruction d;
        const int in_kernel = cs == E2E_KERNEL_SEG;
        int have = 0;
        if (in_kernel && ip < kernel_size &&
            md_decode_8086(kmem, kernel_size, 0x0000u, ip, &d)) {
            uint32_t b;
            have = 1;
            for (b = ip; b < (uint32_t)ip + d.length && b < kernel_size; ++b) executed[b] = 1u;
        }
        (void)md_interp_step(rt);
        if (rt->cpu.cs == E2E_KERNEL_SEG && rt->cpu.ip < kernel_size) {
            if (!in_kernel || !have || rt->cpu.ip != d.next_ip) target[rt->cpu.ip] = 1u;
        }
        if ((rt->instructions & 0xFFFFu) == 0u) {
            e->out[e->out_len] = '\0';
            if (strstr(e->out, "ALL TESTS PASSED") != NULL) break;
        }
    }
    e->out[e->out_len] = '\0';

    for (i = 0; i < kernel_size; ++i) {
        if (target[i]) ++targets;
        if (executed[i]) {
            ++exec_count;
            if (kmem[i] != kernel[i]) ++changed;
        }
    }
    printf("\n[profile] instructions=%llu kernel code bytes executed=%lu transfer targets=%lu\n",
           (unsigned long long)rt->instructions, (unsigned long)exec_count, (unsigned long)targets);
    printf("[profile] executed kernel code bytes that differ from MSDOS.SYS at the end: %lu\n",
           (unsigned long)changed);
    for (i = 0; i < kernel_size; ++i) {
        if (executed[i] && kmem[i] != kernel[i]) {
            uint32_t j = i;
            while (j + 1u < kernel_size && executed[j + 1u] && kmem[j + 1u] != kernel[j + 1u]) ++j;
            printf("[profile]   changed code %04lX-%04lX\n", (unsigned long)i, (unsigned long)j);
            i = j;
        }
    }

    fp = fopen(out_path, "w");
    if (fp == NULL) return 2;
    fprintf(fp, "# MSDOS.SYS (MS-DOS 2.0 release) dosrecomp entry points, profile-guided.\n"
                "# Generated by: microdos_dos2_e2e MSDOS.SYS disk.img --profile-kernel FILE\n"
                "# (a DOS2TEST session at A>, single-stepped in the interpreter). Each line is\n"
                "# a kernel offset reached by a non-sequential transfer. The INT 21h\n"
                "# dispatch table is added from the binary: PUSH CS:[BX+DISPATCH] at 0647h\n"
                "# reads 88 near pointers (functions 00h-57h) at 06AAh.\n"
                "table 0x06AA 88\n");
    for (i = 0; i < kernel_size; ++i) if (target[i]) fprintf(fp, "0x%04lX\n", (unsigned long)i);
    fclose(fp);
    printf("[profile] wrote %s\n", out_path);
    return strstr(e->out, "passed: 25   failed: 0") != NULL ? 0 : 1;
}

/* M17 lockstep differential: system A (compiled kernel + DOS2TEST) and
   system B (pure interpreter) advance exactly one guest instruction per
   iteration (md_dos2_system_run with a budget of 1) with identical scripted
   input; CPU state is compared after every instruction and memory
   periodically. Reports the first divergence. */
/* Architectural FLAGS from a copy (lazy flags materialise on read). */
static uint16_t e2e_flags(const MdX86 *c)
{
    MdX86 copy = *c;
    return md_x86_flags(&copy);
}

static int e2e_same_cpu(const MdX86 *a, const MdX86 *b)
{
    return memcmp(a->r, b->r, sizeof(a->r)) == 0 && a->es == b->es && a->cs == b->cs &&
           a->ss == b->ss && a->ds == b->ds && a->ip == b->ip && e2e_flags(a) == e2e_flags(b);
}

static void e2e_dump_cpu(const char *tag, const MdX86 *c)
{
    printf("[lockstep] %s CS:IP=%04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X F=%04X\n",
           tag, c->cs, c->ip, c->r[0], c->r[3], c->r[1], c->r[2], c->r[6], c->r[7], c->r[5], c->r[4],
           c->ds, c->es, c->ss, e2e_flags(c));
}

static const MdAotProgram *const g_programs[] = { &md_recomp_dos2test_program };

/* One unit of A: a whole compiled-code entry if one can run here, else one
   interpreted instruction. B then runs exactly as many instructions. */
static MdStopReason e2e_unit_a(MdDos2System *a)
{
    MdRuntime *rt = &a->runtime;
    const uint16_t cs = rt->cpu.cs, ip = rt->cpu.ip;
    const MdAotProgram *prog = NULL;
    if (cs == a->boot.dos_segment) {
        if (a->kernel_attached && a->kernel_program->block_ok(rt, cs, ip)) prog = a->kernel_program;
    } else if (cs != a->boot.bios_segment) {
        if (ip == 0x0100u && !g_programs[0]->ready(rt, cs)) (void)g_programs[0]->attach(rt, cs);
        if (g_programs[0]->block_ok(rt, cs, ip)) prog = g_programs[0];
    }
    if (prog != NULL) {
        const uint64_t before = rt->instructions;
        const MdStopReason st = prog->enter(rt, 65536u);
        if (st != MD_STOP_NONE || rt->instructions != before) return st;
    }
    return md_interp_step(rt);
}

static unsigned long long g_trace_unit = 0;   /* --lockstep-trace N */

static int e2e_lockstep(MdDos2System *a, MdDos2System *b, E2e *ea, E2e *eb)
{
    uint64_t n = 0;
    MdX86 before_a;
    for (;;) {
        const uint64_t ia = a->runtime.instructions;
        MdStopReason sa, sb;
        before_a = a->runtime.cpu;
        sa = e2e_unit_a(a);
        if (g_trace_unit != 0u && n + 1u == g_trace_unit) {
            uint64_t k;
            for (k = 0; k < a->runtime.instructions - ia; ++k) {
                unsigned j;
                printf("[trace] B %04X:%04X ", b->runtime.cpu.cs, b->runtime.cpu.ip);
                for (j = 0; j < 6u; ++j) printf("%02X ", md_x86_read8(&b->runtime.cpu, b->runtime.cpu.cs, (uint16_t)(b->runtime.cpu.ip + j)));
                printf(" AX=%04X F=%04X\n", b->runtime.cpu.r[0], e2e_flags(&b->runtime.cpu));
                (void)md_interp_step(&b->runtime);
            }
            sb = b->runtime.stop_reason;
        } else
        sb = md_interp_run(&b->runtime, a->runtime.instructions - ia);
        if (sb == MD_STOP_BUDGET) { b->runtime.stop_reason = MD_STOP_NONE; sb = MD_STOP_NONE; }
        ++n;
        if (sa != sb || !e2e_same_cpu(&a->runtime.cpu, &b->runtime.cpu) ||
            ((n & 0x3FFu) == 0u && memcmp(a->runtime.cpu.memory, b->runtime.cpu.memory, MD_X86_ADDRESS_SPACE) != 0)) {
            uint32_t i;
            printf("\n[lockstep] DIVERGENCE at unit %llu, instruction %llu (A aot=%llu, unit length %llu)\n",
                   (unsigned long long)n, (unsigned long long)a->runtime.instructions,
                   (unsigned long long)a->runtime.aot_instructions,
                   (unsigned long long)(a->runtime.instructions - ia));
            e2e_dump_cpu("before ", &before_a);
            e2e_dump_cpu("A(aot) ", &a->runtime.cpu);
            e2e_dump_cpu("B(int) ", &b->runtime.cpu);
            printf("[lockstep] code at before CS:IP:");
            for (i = 0; i < 8u; ++i) printf(" %02X", md_x86_read8(&b->runtime.cpu, before_a.cs, (uint16_t)(before_a.ip + i)));
            printf("\n");
            for (i = 0; i < MD_X86_ADDRESS_SPACE; ++i) {
                if (a->runtime.cpu.memory[i] != b->runtime.cpu.memory[i]) {
                    printf("[lockstep] first memory difference at %05lX: A=%02X B=%02X\n",
                           (unsigned long)i, a->runtime.cpu.memory[i], b->runtime.cpu.memory[i]);
                    break;
                }
            }
            return 1;
        }
        if (sa != MD_STOP_NONE) break;
        if ((n & 0xFFFFu) == 0u) {
            ea->out[ea->out_len] = '\0';
            if (strstr(ea->out, "ALL TESTS PASSED") != NULL || strstr(ea->out, "SOME TESTS FAILED") != NULL) break;
        }
        if (n > E2E_BUDGET) break;
    }
    (void)eb;
    ea->out[ea->out_len] = '\0';
    printf("\n[lockstep] no divergence in %llu units (A aot=%llu)\n",
           (unsigned long long)n, (unsigned long long)a->runtime.aot_instructions);
    if (strstr(ea->out, "passed: 25   failed: 0") == NULL) {
        printf("[lockstep] FAIL: DOS2TEST did not pass in the compiled system\n");
        return 1;
    }
    if (a->runtime.aot_instructions == 0u) {
        printf("[lockstep] FAIL: no compiled code ran\n");
        return 1;
    }
    puts("[lockstep] PASS");
    return 0;
}

static uint8_t *load(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    long n;
    uint8_t *d;
    if (fp == NULL) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    d = (uint8_t *)malloc((size_t)n);
    if (d == NULL || fread(d, 1u, (size_t)n, fp) != (size_t)n) { fclose(fp); free(d); return NULL; }
    fclose(fp);
    *size = (size_t)n;
    return d;
}

static bool e2e_setup(MdDos2System *sys, E2e *e, uint8_t *memory, MdBlockCache *cache,
                      const uint8_t *kernel, size_t kernel_size, bool aot, bool kernel_aot)
{
    md_dos2_system_init(sys, memory, cache);
    sys->boot.console.write = con_write;
    sys->boot.console.peek = con_peek;
    sys->boot.console.read = con_read;
    sys->boot.console.flush = con_flush;
    sys->boot.console.user = e;
    sys->boot.disk.read = disk_read;
    sys->boot.disk.write = disk_write;
    sys->boot.disk.user = e;
    sys->boot.disk.sector_size = 512u;
    sys->boot.disk.sector_count = 720u;
    sys->boot.disk.writable = true;
    sys->boot.clock_days = 1162u;            /* 1983-03-08, fixed for reproducibility */
    sys->boot.clock_hours = 12u;
    if (aot && kernel_aot) md_dos2_system_set_kernel_aot(sys, &md_recomp_msdos2_program);
    md_dos2_system_set_aot(sys, g_programs, 1u, aot);
    return md_dos2_system_start(sys, kernel, kernel_size);
}

int main(int argc, char **argv)
{
    static const MdAotProgram *const programs[] = { &md_recomp_dos2test_program };
    static E2e e;
    static MdDos2System sys;
    static MdBlockCache cache;
    uint8_t *memory, *kernel;
    size_t kernel_size = 0;
    bool aot = true, use_cache = true, kernel_aot = true;
    const char *profile_out = NULL;
    bool lockstep = false;
    int i;
    MdStopReason st = MD_STOP_NONE;

    if (argc < 3) { fprintf(stderr, "usage: %s MSDOS.SYS disk.img [--no-aot] [--no-cache]\n", argv[0]); return 2; }
    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--no-aot") == 0) aot = false;
        else if (strcmp(argv[i], "--no-cache") == 0) use_cache = false;
        else if (strcmp(argv[i], "--no-kernel-aot") == 0) kernel_aot = false;
        else if (strcmp(argv[i], "--profile-kernel") == 0 && i + 1 < argc) profile_out = argv[++i];
        else if (strcmp(argv[i], "--lockstep") == 0) lockstep = true;
        else if (strcmp(argv[i], "--lockstep-trace") == 0 && i + 1 < argc) { lockstep = true; g_trace_unit = strtoull(argv[++i], NULL, 0); }
    }
    kernel = load(argv[1], &kernel_size);
    e.disk = load(argv[2], &e.disk_size);
    memory = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    if (kernel == NULL || e.disk == NULL || memory == NULL) { fprintf(stderr, "e2e: cannot load inputs\n"); return 2; }

    if (lockstep) {
        static E2e eb;
        static MdDos2System sb;
        uint8_t *memory_b = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
        eb.disk = load(argv[2], &eb.disk_size);
        if (memory_b == NULL || eb.disk == NULL) return 2;
        e.quiet = eb.quiet = true;
        if (!e2e_setup(&sys, &e, memory, NULL, kernel, kernel_size, true, kernel_aot) ||
            !e2e_setup(&sb, &eb, memory_b, NULL, kernel, kernel_size, false, false)) return 2;
        return e2e_lockstep(&sys, &sb, &e, &eb);
    }
    (void)programs;
    if (!e2e_setup(&sys, &e, memory, use_cache ? &cache : NULL, kernel, kernel_size, aot, kernel_aot)) {
        fprintf(stderr, "e2e: bad MSDOS.SYS\n");
        return 2;
    }
#ifdef MICRODOS_ENABLE_JIT
    e2e_attach_jit(&sys);
#endif
    if (profile_out != NULL) {
        md_dos2_system_set_aot(&sys, programs, 1u, false);
        return e2e_profile(&sys, &e, kernel, kernel_size, profile_out);
    }

    {
    const clock_t t0 = clock();
    while (sys.runtime.instructions < E2E_BUDGET) {
        st = md_dos2_system_run(&sys, 100000u);
        if (st != MD_STOP_NONE) break;
        e.out[e.out_len] = '\0';
        if (strstr(e.out, "ALL TESTS PASSED") != NULL || strstr(e.out, "SOME TESTS FAILED") != NULL) break;
    }
    {
        const double secs = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        printf("\n[e2e] loop time %.2f ms, %.1f MIPS (host)\n", secs * 1000.0,
               secs > 0.0 ? (double)sys.runtime.instructions / secs / 1e6 : 0.0);
    }
    }
    e.out[e.out_len] = '\0';

    printf("\n[e2e] aot=%s kernel-aot=%s cache=%s instructions=%llu aot_instructions=%llu attaches=%u stop=%s\n",
           aot ? "on" : "off", sys.kernel_attached ? "on" : "off", use_cache ? "on" : "off",
           (unsigned long long)sys.runtime.instructions,
           (unsigned long long)sys.runtime.aot_instructions,
           (unsigned)sys.aot_attaches, md_stop_reason_name(st));
    printf("[e2e] kernel compiled=%llu (%.1f%% of all)  DOS2TEST compiled=%llu  attached-segment steps=%llu\n",
           (unsigned long long)sys.kernel_aot_instructions,
           sys.runtime.instructions ? 100.0 * (double)sys.kernel_aot_instructions / (double)sys.runtime.instructions : 0.0,
           (unsigned long long)(sys.runtime.aot_instructions - sys.kernel_aot_instructions),
           (unsigned long long)sys.attached_steps);
    if (use_cache) {
        printf("[e2e] cache hits=%llu misses=%llu decodes=%llu invalidations=%llu fallback=%llu\n",
               (unsigned long long)cache.hits, (unsigned long long)cache.misses,
               (unsigned long long)cache.decodes, (unsigned long long)cache.invalidations,
               (unsigned long long)cache.fallback_instructions);
    }

    if (strstr(e.out, "passed: 25   failed: 0") == NULL) { fprintf(stderr, "[e2e] FAIL: DOS2TEST did not pass\n"); return 1; }
    if (aot && kernel_aot && (!sys.kernel_attached || sys.kernel_aot_instructions == 0u)) {
        fprintf(stderr, "[e2e] FAIL: compiled kernel was not used\n");
        return 1;
    }
    if (aot && (sys.aot_attaches < 2u || sys.runtime.aot_instructions == 0u)) {
        fprintf(stderr, "[e2e] FAIL: compiled DOS2TEST was not used\n");
        return 1;
    }
    if (!aot && sys.runtime.aot_instructions != 0u) { fprintf(stderr, "[e2e] FAIL: AOT ran while disabled\n"); return 1; }
#ifdef MICRODOS_ENABLE_NATIVE3
    {
        const MdN3Stats *n3 = md_native3_stats(&sys.native3);

        printf(
            "[n3] owned=%llu entries=%llu retired=%llu native=%llu interp=%llu "
            "lookups=%llu hits=%llu misses=%llu compiles=%llu invalidations=%llu\n",
            (unsigned long long)sys.native3_instructions,
            (unsigned long long)(n3 ? n3->entries : 0u),
            (unsigned long long)(n3 ? n3->retired : 0u),
            (unsigned long long)(n3 ? n3->native_retired : 0u),
            (unsigned long long)(n3 ? n3->interp_retired : 0u),
            (unsigned long long)(n3 ? n3->lookups : 0u),
            (unsigned long long)(n3 ? n3->hits : 0u),
            (unsigned long long)(n3 ? n3->misses : 0u),
            (unsigned long long)(n3 ? n3->compiles : 0u),
            (unsigned long long)(n3 ? n3->invalidations : 0u));

        if (
            n3 == NULL ||
            sys.native3_instructions == 0u ||
            n3->retired == 0u
        ) {
            fprintf(
                stderr,
                "[e2e] FAIL: Native-3 was enabled but retired no guest instructions\n");

            return 1;
        }
    }
#endif

#ifdef MICRODOS_ENABLE_JIT
    e2e_report_jit(&sys);
#endif

    puts("[e2e] PASS");
    return 0;
}
