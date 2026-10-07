#include "microdos/native3.h"

#include <stdio.h>
#include <string.h>

/*
 * All large objects are static.
 *
 * Windows/MSVC's default test-thread stack is commonly ~1 MiB. The original
 * omnibus test put a 1 MiB guest array plus MdRuntime/MdNative3 objects on the
 * stack, so the test could fault before reaching main-test output.
 */
static uint8_t g_mem_a[1u << 20];
static uint8_t g_mem_b[1u << 20];
static uint8_t g_code_a[64u * 1024u];
static uint8_t g_code_b[64u * 1024u];
static uint8_t g_blob[8192];

static MdRuntime g_rt_a;
static MdRuntime g_rt_b;
static MdNative3 g_n3_a;
static MdNative3 g_n3_b;

static int exact_run(void)
{
    static const uint8_t p[] = {
        0xB8,0x01,0x00, 0xBB,0x03,0x00, 0xB9,0x90,0x01,
        0x03,0xC3, 0x33,0xD8, 0x49, 0x75,0xF9, 0xF4
    };
    MdHooks h;
    MdN3RunResult rr;
    unsigned guard = 0u;

    puts("native3 exact BEGIN");

    memset(&h, 0, sizeof(h));
    memset(g_mem_a, 0, sizeof(g_mem_a));
    memset(g_mem_b, 0, sizeof(g_mem_b));
    memset(&g_rt_a, 0, sizeof(g_rt_a));
    memset(&g_rt_b, 0, sizeof(g_rt_b));
    memset(&g_n3_a, 0, sizeof(g_n3_a));

    memcpy(g_mem_a + 0x100, p, sizeof(p));
    memcpy(g_mem_b + 0x100, p, sizeof(p));

    md_runtime_init(&g_rt_a, g_mem_a, &h);
    md_runtime_init(&g_rt_b, g_mem_b, &h);

    g_rt_a.cpu.cs = g_rt_b.cpu.cs = 0u;
    g_rt_a.cpu.ip = g_rt_b.cpu.ip = 0x100u;
    g_rt_a.cpu.ss = g_rt_b.cpu.ss = 0u;
    g_rt_a.cpu.r[MD_X86_SP] = g_rt_b.cpu.r[MD_X86_SP] = 0xFFFEu;

    md_native3_init(&g_n3_a, g_code_a, sizeof(g_code_a));

    while (g_rt_a.stop_reason == MD_STOP_NONE && guard++ < 1000u) {
        if (!md_native3_run(&g_n3_a, &g_rt_a, 4096u, &rr)) break;
    }

    guard = 0u;
    while (g_rt_b.stop_reason == MD_STOP_NONE && guard++ < 100000u)
        (void)md_interp_step(&g_rt_b);

    if (g_rt_a.cpu.r[MD_X86_AX] != g_rt_b.cpu.r[MD_X86_AX] ||
        g_rt_a.cpu.r[MD_X86_CX] != g_rt_b.cpu.r[MD_X86_CX] ||
        g_rt_a.cpu.r[MD_X86_BX] != g_rt_b.cpu.r[MD_X86_BX] ||
        g_rt_a.cpu.ip != g_rt_b.cpu.ip ||
        md_x86_flags(&g_rt_a.cpu) != md_x86_flags(&g_rt_b.cpu)) {
        printf("native3 exact FAIL "
               "AX=%04x/%04x BX=%04x/%04x CX=%04x/%04x IP=%04x/%04x "
               "FLAGS=%04x/%04x\n",
               g_rt_a.cpu.r[MD_X86_AX], g_rt_b.cpu.r[MD_X86_AX],
               g_rt_a.cpu.r[MD_X86_BX], g_rt_b.cpu.r[MD_X86_BX],
               g_rt_a.cpu.r[MD_X86_CX], g_rt_b.cpu.r[MD_X86_CX],
               g_rt_a.cpu.ip, g_rt_b.cpu.ip,
               md_x86_flags(&g_rt_a.cpu), md_x86_flags(&g_rt_b.cpu));
        return 0;
    }

    puts("native3 exact PASS");
    return 1;
}

static int cache_roundtrip(void)
{
    MdHooks h;
    MdN3Prewarm e;
    size_t n;

    puts("native3 cache BEGIN");

    memset(&h, 0, sizeof(h));
    memset(g_mem_a, 0, sizeof(g_mem_a));
    memset(&g_rt_a, 0, sizeof(g_rt_a));
    memset(&g_n3_a, 0, sizeof(g_n3_a));
    memset(&g_n3_b, 0, sizeof(g_n3_b));

    g_mem_a[0x100] = 0xB9; g_mem_a[0x101] = 10; g_mem_a[0x102] = 0;
    g_mem_a[0x103] = 0x49;
    g_mem_a[0x104] = 0x75; g_mem_a[0x105] = 0xFD;
    g_mem_a[0x106] = 0xF4;

    md_runtime_init(&g_rt_a, g_mem_a, &h);
    g_rt_a.cpu.cs = 0u;
    g_rt_a.cpu.ip = 0x100u;
    g_rt_a.cpu.ss = 0u;
    g_rt_a.cpu.r[MD_X86_SP] = 0xFFFEu;

    md_native3_init(&g_n3_a, g_code_a, sizeof(g_code_a));
    e.cs = 0u;
    e.ip = 0x100u;

    if (md_native3_prewarm(&g_n3_a, &g_rt_a, &e, 1u) != 1u) {
        puts("native3 cache prewarm FAIL");
        return 0;
    }

    n = md_native3_cache_export(&g_n3_a, g_blob, sizeof(g_blob));
    if (n == 0u) {
        puts("native3 cache export FAIL");
        return 0;
    }

    md_native3_init(&g_n3_b, g_code_b, sizeof(g_code_b));
    if (!md_native3_cache_import(&g_n3_b, &g_rt_a, g_blob, n)) {
        puts("native3 cache import FAIL");
        return 0;
    }

    puts("native3 cache PASS");
    return 1;
}

int main(void)
{
    int ok = 1;

    puts("=== Native-3 host semantic gate ===");

    ok &= exact_run();
    ok &= cache_roundtrip();

    printf("native3 tests %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
