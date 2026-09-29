/* M14 end-to-end test: boot the released MS-DOS 2.0 through the portable
 * system loop (the same code the Pico 2 runs), answer the date/time prompts,
 * run DOS2TEST at A>, and require "ALL TESTS PASSED".
 *
 * usage: microdos_dos2_e2e MSDOS.SYS disk.img [--no-aot] [--no-cache]
 */
#include "md_dos2_system.h"
#include "dos2test_recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
} E2e;

static void e2e_release(E2e *e)
{
    if ((e->keys == NULL || *e->keys == '\0') &&
        e->step < sizeof(kScript) / sizeof(kScript[0])) {
        const char *hit;
        e->out[e->out_len] = '\0';
        hit = strstr(e->out + e->search_from, kScript[e->step].after);
        if (hit != NULL) {
            e->search_from = (size_t)(hit - e->out) + strlen(kScript[e->step].after);
            e->keys = kScript[e->step].keys;
            ++e->step;
        }
    }
}

static void con_write(void *user, const uint8_t *data, size_t size)
{
    E2e *e = (E2e *)user;
    size_t i;
    for (i = 0; i < size && e->out_len + 1u < E2E_OUT_MAX; ++i) e->out[e->out_len++] = (char)data[i];
    fwrite(data, 1u, size, stdout);
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

int main(int argc, char **argv)
{
    static const MdAotProgram *const programs[] = { &md_recomp_dos2test_program };
    static E2e e;
    static MdDos2System sys;
    static MdBlockCache cache;
    uint8_t *memory, *kernel;
    size_t kernel_size = 0;
    bool aot = true, use_cache = true;
    int i;
    MdStopReason st = MD_STOP_NONE;

    if (argc < 3) { fprintf(stderr, "usage: %s MSDOS.SYS disk.img [--no-aot] [--no-cache]\n", argv[0]); return 2; }
    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--no-aot") == 0) aot = false;
        else if (strcmp(argv[i], "--no-cache") == 0) use_cache = false;
    }
    kernel = load(argv[1], &kernel_size);
    e.disk = load(argv[2], &e.disk_size);
    memory = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    if (kernel == NULL || e.disk == NULL || memory == NULL) { fprintf(stderr, "e2e: cannot load inputs\n"); return 2; }

    md_dos2_system_init(&sys, memory, use_cache ? &cache : NULL);
    sys.boot.console.write = con_write;
    sys.boot.console.peek = con_peek;
    sys.boot.console.read = con_read;
    sys.boot.console.flush = con_flush;
    sys.boot.console.user = &e;
    sys.boot.disk.read = disk_read;
    sys.boot.disk.write = disk_write;
    sys.boot.disk.user = &e;
    sys.boot.disk.sector_size = 512u;
    sys.boot.disk.sector_count = 720u;
    sys.boot.disk.writable = true;
    sys.boot.clock_days = 1162u;            /* 1983-03-08, fixed for reproducibility */
    sys.boot.clock_hours = 12u;
    md_dos2_system_set_aot(&sys, programs, 1u, aot);
    if (!md_dos2_system_start(&sys, kernel, kernel_size)) { fprintf(stderr, "e2e: bad MSDOS.SYS\n"); return 2; }

    while (sys.runtime.instructions < E2E_BUDGET) {
        st = md_dos2_system_run(&sys, 100000u);
        if (st != MD_STOP_NONE) break;
        e.out[e.out_len] = '\0';
        if (strstr(e.out, "ALL TESTS PASSED") != NULL || strstr(e.out, "SOME TESTS FAILED") != NULL) break;
    }
    e.out[e.out_len] = '\0';

    printf("\n[e2e] aot=%s cache=%s instructions=%llu aot_instructions=%llu attaches=%u stop=%s\n",
           aot ? "on" : "off", use_cache ? "on" : "off",
           (unsigned long long)sys.runtime.instructions,
           (unsigned long long)sys.runtime.aot_instructions,
           (unsigned)sys.aot_attaches, md_stop_reason_name(st));
    if (use_cache) {
        printf("[e2e] cache hits=%llu misses=%llu decodes=%llu invalidations=%llu fallback=%llu\n",
               (unsigned long long)cache.hits, (unsigned long long)cache.misses,
               (unsigned long long)cache.decodes, (unsigned long long)cache.invalidations,
               (unsigned long long)cache.fallback_instructions);
    }

    if (strstr(e.out, "passed: 25   failed: 0") == NULL) { fprintf(stderr, "[e2e] FAIL: DOS2TEST did not pass\n"); return 1; }
    if (aot && (sys.aot_attaches < 2u || sys.runtime.aot_instructions == 0u)) {
        fprintf(stderr, "[e2e] FAIL: compiled DOS2TEST was not used\n");
        return 1;
    }
    if (!aot && sys.runtime.aot_instructions != 0u) { fprintf(stderr, "[e2e] FAIL: AOT ran while disabled\n"); return 1; }
    puts("[e2e] PASS");
    return 0;
}
