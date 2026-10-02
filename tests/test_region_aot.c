#include "microdos/runtime.h"
#include "checksum_recomp.h"

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

static const uint8_t kChecksum[] = {
    0xBE,0x00,0x20, 0xB9,0x00,0x40, 0x31,0xD2, 0xFC,
    0xAD, 0x03,0xD0, 0xE2,0xFB, 0xF4
};

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

static void fill(uint8_t *mem)
{
    uint32_t i;
    for (i = 0u; i < 0x8000u; ++i)
        mem[0x2000u + i] = (uint8_t)(i * 37u + 11u);
}

static void test_full_standalone(void)
{
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    MdHooks hooks = {0};

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    md_runtime_init(&a, ma, &hooks);
    md_runtime_load_com(&a, kChecksum, sizeof(kChecksum), 0x0000u);
    fill(ma);
    CHECK(md_interp_run(&a, 1000000u) == MD_STOP_HALT);

    md_runtime_init(&b, mb, &hooks);
    fill(mb);
    CHECK(md_recomp_checksum(&b, 0x0000u, 1000000u) == MD_STOP_HALT);

    CHECK(a.instructions == 49157u);
    CHECK(a.instructions == b.instructions);
    CHECK(b.aot_instructions == b.instructions);
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);

    free(ma);
    free(mb);
}

/*
 * M21.1c regression: generated AOT must trust the shared region helper's IP.
 * A 25-instruction budget is the four-instruction checksum prologue plus
 * seven complete 3-instruction loop iterations. The helper therefore makes
 * partial progress and intentionally leaves IP at the loop head (0109h).
 */
static void test_partial_attach(void)
{
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime a, b;
    MdHooks hooks = {0};
    MdStopReason st;

    CHECK(ma != NULL && mb != NULL);
    if (ma == NULL || mb == NULL) { free(ma); free(mb); return; }

    md_runtime_init(&a, ma, &hooks);
    md_runtime_load_com(&a, kChecksum, sizeof(kChecksum), 0x0000u);
    fill(ma);

    md_runtime_init(&b, mb, &hooks);
    md_runtime_load_com(&b, kChecksum, sizeof(kChecksum), 0x0000u);
    fill(mb);
    CHECK(md_recomp_checksum_program.attach(&b, 0x0000u));

    CHECK(md_interp_run(&a, 25u) == MD_STOP_BUDGET);
    a.stop_reason = MD_STOP_NONE;

    st = md_recomp_checksum_program.enter(&b, 25u);
    CHECK(st == MD_STOP_NONE);
    CHECK(b.stop_reason == MD_STOP_NONE);
    CHECK(a.instructions == 25u && b.instructions == 25u);
    CHECK(b.aot_instructions == 25u);
    CHECK(b.cpu.ip == 0x0109u);
    CHECK(b.cpu.r[MD_X86_CX] == (uint16_t)(0x4000u - 7u));
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);

    /* Both execution paths must continue from that partial region state. */
    CHECK(md_interp_run(&a, 1000000u) == MD_STOP_HALT);
    CHECK(md_recomp_checksum_program.enter(&b, 1000000u) == MD_STOP_HALT);

    CHECK(a.instructions == 49157u);
    CHECK(a.instructions == b.instructions);
    CHECK(b.aot_instructions == b.instructions);
    CHECK(same_machine(&a, &b));
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);

    free(ma);
    free(mb);
}

int main(void)
{
    test_full_standalone();
    test_partial_attach();

    if (failures != 0) {
        fprintf(stderr, "microDOS M21 AOT region test: %d failure(s)\n", failures);
        return 1;
    }
    puts("microDOS M21 AOT region test: ok");
    return 0;
}
