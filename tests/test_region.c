#include "microdos/block_cache.h"
#include "microdos/region.h"
#include "microdos/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        ++failures; \
    } \
} while (0)

static uint16_t arch_flags(const MdX86 *cpu)
{
    MdX86 copy = *cpu;
    return md_x86_flags(&copy);
}

static int same_machine(const MdRuntime *a, const MdRuntime *b)
{
    return memcmp(a->cpu.r, b->cpu.r, sizeof(a->cpu.r)) == 0 &&
           a->cpu.cs == b->cpu.cs &&
           a->cpu.ds == b->cpu.ds &&
           a->cpu.es == b->cpu.es &&
           a->cpu.ss == b->cpu.ss &&
           a->cpu.ip == b->cpu.ip &&
           arch_flags(&a->cpu) == arch_flags(&b->cpu);
}

static void setup_dec(MdRuntime *rt, uint8_t *mem, unsigned initial)
{
    static const uint8_t code[] = { 0x49, 0x75, 0xFD, 0xF4 }; /* DEC CX; JNZ 0100; HLT */
    MdHooks hooks = {0};

    md_runtime_init(rt, mem, &hooks);
    md_runtime_load_com(rt, code, sizeof(code), 0x1000u);
    rt->cpu.r[MD_X86_CX] = (uint16_t)initial;
    md_x86_set_flags(&rt->cpu,
        (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF | MD_X86_FLAG_IF));
}

static void test_dec_jnz(unsigned initial)
{
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    const uint32_t trips = initial != 0u ? initial : 65536u;
    const uint32_t retired = trips * 2u;
    uint32_t got;

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    setup_dec(&a, ma, initial);
    setup_dec(&b, mb, initial);

    CHECK(md_interp_run(&a, retired) == MD_STOP_BUDGET);
    a.stop_reason = MD_STOP_NONE;

    got = md_region_try_dec_jnz(&b, MD_X86_CX, 0x0100u, 0x0103u, retired);
    CHECK(got == retired);
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);

    free(ma);
    free(mb);
}

static void test_dec_partial(unsigned initial, uint32_t budget)
{
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    MdX86 before;
    uint32_t got;

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    setup_dec(&a, ma, initial);
    setup_dec(&b, mb, initial);

    /* Less than one whole DEC/JNZ iteration must be a true no-op. */
    before = b.cpu;
    got = md_region_try_dec_jnz(&b, MD_X86_CX, 0x0100u, 0x0103u, 1u);
    CHECK(got == 0u);
    CHECK(memcmp(before.r, b.cpu.r, sizeof(before.r)) == 0);
    CHECK(before.ip == b.cpu.ip);
    CHECK(arch_flags(&before) == arch_flags(&b.cpu));

    CHECK(md_interp_run(&a, budget) == MD_STOP_BUDGET);
    a.stop_reason = MD_STOP_NONE;

    got = md_region_try_dec_jnz(&b, MD_X86_CX, 0x0100u, 0x0103u, budget);
    CHECK(got == budget);
    CHECK(b.cpu.ip == 0x0100u);       /* partial region remains at loop head */
    CHECK(same_machine(&a, &b));
    CHECK((arch_flags(&b.cpu) & MD_X86_FLAG_CF) != 0u); /* DEC preserved CF */

    free(ma);
    free(mb);
}

static void fill_words(MdRuntime *rt, uint16_t ds, uint16_t first, unsigned count)
{
    unsigned i;
    for (i = 0u; i < count; ++i) {
        md_x86_write16(&rt->cpu, ds, (uint16_t)(first + i * 2u),
                       (uint16_t)(0x113u + i * 0x91u));
    }
}

static void setup_lodsw(MdRuntime *rt, uint8_t *mem, int backwards, unsigned count)
{
    static const uint8_t code[] = { 0xAD, 0x03, 0xD0, 0xE2, 0xFB, 0xF4 };
    MdHooks hooks = {0};

    md_runtime_init(rt, mem, &hooks);
    md_runtime_load_com(rt, code, sizeof(code), 0x1000u);
    rt->cpu.ds = 0x2345u;
    rt->cpu.r[MD_X86_CX] = (uint16_t)count;
    rt->cpu.r[MD_X86_DX] = 0x8123u;
    rt->cpu.r[MD_X86_SI] = backwards
        ? (uint16_t)(0x0200u + (count - 1u) * 2u)
        : 0x0200u;
    if (backwards) rt->cpu.flags_raw |= MD_X86_FLAG_DF;
    fill_words(rt, rt->cpu.ds, 0x0200u, count);
}

static void test_lodsw_loop(int backwards)
{
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    const unsigned count = 64u;
    const uint32_t retired = count * 3u;
    uint32_t got;

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    setup_lodsw(&a, ma, backwards, count);
    setup_lodsw(&b, mb, backwards, count);

    CHECK(md_interp_run(&a, retired) == MD_STOP_BUDGET);
    a.stop_reason = MD_STOP_NONE;

    got = md_region_try_lodsw_add_dx_ax_loop(&b, 0x0100u, 0x0105u, retired);
    CHECK(got == retired);
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);

    free(ma);
    free(mb);
}

static void test_lodsw_partial(int backwards)
{
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    MdX86 before;
    const unsigned count = 64u;
    const uint32_t budget = 17u * 3u;
    uint32_t got;

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    setup_lodsw(&a, ma, backwards, count);
    setup_lodsw(&b, mb, backwards, count);

    before = b.cpu;
    CHECK(md_region_try_lodsw_add_dx_ax_loop(&b, 0x0100u, 0x0105u, 2u) == 0u);
    CHECK(memcmp(before.r, b.cpu.r, sizeof(before.r)) == 0);
    CHECK(before.ip == b.cpu.ip);
    CHECK(arch_flags(&before) == arch_flags(&b.cpu));

    CHECK(md_interp_run(&a, budget) == MD_STOP_BUDGET);
    a.stop_reason = MD_STOP_NONE;

    got = md_region_try_lodsw_add_dx_ax_loop(&b, 0x0100u, 0x0105u, budget);
    CHECK(got == budget);
    CHECK(b.cpu.ip == 0x0100u);
    CHECK(b.cpu.r[MD_X86_CX] == (uint16_t)(count - 17u));
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);

    free(ma);
    free(mb);
}

static void test_lodsw_wrap(void)
{
    static const uint8_t code[] = { 0xAD, 0x03, 0xD0, 0xE2, 0xFB, 0xF4 };
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    MdHooks hooks = {0};
    const unsigned count = 8u;
    const uint32_t retired = count * 3u;
    unsigned i;

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    md_runtime_init(&a, ma, &hooks);
    md_runtime_load_com(&a, code, sizeof(code), 0x1000u);
    for (i = 0u; i < 16u; ++i) {
        ma[0xFFFF0u + i] = (uint8_t)(0xA0u + i);
        ma[i] = (uint8_t)(0x10u + i);
    }
    a.cpu.ds = 0xFFFFu;
    a.cpu.r[MD_X86_SI] = 0x0007u;
    a.cpu.r[MD_X86_CX] = count;
    a.cpu.r[MD_X86_DX] = 0x4411u;

    memcpy(mb, ma, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&b, mb, &hooks);
    md_runtime_load_com(&b, code, sizeof(code), 0x1000u);
    b.cpu.ds = 0xFFFFu;
    b.cpu.r[MD_X86_SI] = 0x0007u;
    b.cpu.r[MD_X86_CX] = count;
    b.cpu.r[MD_X86_DX] = 0x4411u;

    CHECK(md_interp_run(&a, retired) == MD_STOP_BUDGET);
    a.stop_reason = MD_STOP_NONE;
    CHECK(md_region_try_lodsw_add_dx_ax_loop(&b, 0x0100u, 0x0105u, retired) == retired);
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);

    free(ma);
    free(mb);
}

static void test_cached_checksum(void)
{
    static const uint8_t code[] = {
        0xBE,0x00,0x20, 0xB9,0x00,0x40, 0x31,0xD2, 0xFC,
        0xAD, 0x03,0xD0, 0xE2,0xFB, 0xF4
    };
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    MdHooks hooks = {0};
    MdBlockCache cache;
    unsigned i;

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    md_runtime_init(&a, ma, &hooks);
    md_runtime_load_com(&a, code, sizeof(code), 0x0000u);
    for (i = 0u; i < 0x8000u; ++i) ma[0x2000u + i] = (uint8_t)(i * 37u + 11u);

    memcpy(mb, ma, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&b, mb, &hooks);
    md_block_cache_init(&cache);
    md_runtime_set_block_cache(&b, &cache);
    md_runtime_load_com(&b, code, sizeof(code), 0x0000u);

    CHECK(md_interp_run(&a, 1000000u) == MD_STOP_HALT);
    CHECK(md_interp_run_cached(&b, &cache, 1000000u) == MD_STOP_HALT);
    CHECK(a.instructions == 49157u);
    CHECK(a.instructions == b.instructions);
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);
    CHECK(cache.region_entries >= 1u);
    CHECK(cache.region_instructions > 40000u);

    free(ma);
    free(mb);
}

int main(void)
{
    test_dec_jnz(1u);
    test_dec_jnz(123u);
    test_dec_jnz(0u);
    test_dec_partial(123u, 10u);       /* five complete DEC/JNZ iterations */
    test_dec_partial(0u, 14u);         /* proves CX=0 wrap on partial admission */

    test_lodsw_loop(0);
    test_lodsw_loop(1);
    test_lodsw_partial(0);             /* exercises contiguous fast path */
    test_lodsw_partial(1);             /* generic DF=1 path remains exact */
    test_lodsw_wrap();                 /* generic physical-wrap path remains exact */
    test_cached_checksum();

    if (failures != 0) {
        fprintf(stderr, "microDOS M21 region tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("microDOS M21 region tests: ok");
    return 0;
}
