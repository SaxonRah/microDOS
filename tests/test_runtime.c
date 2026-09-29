#include "microdos/runtime.h"
#include "host_dos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static const uint8_t kHelloCom[] = {
    0xB4, 0x09,
    0xBA, 0x0C, 0x01,
    0xCD, 0x21,
    0xB8, 0x00, 0x4C,
    0xCD, 0x21,
    'H','e','l','l','o',' ','f','r','o','m',' ','m','i','c','r','o','D','O','S','!',13,10,'$'
};

static void test_address_wrap(uint8_t *memory)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.memory = memory;
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_x86_write8(&cpu, 0xFFFFu, 0x0010u, 0xA5u);
    CHECK(memory[0] == 0xA5u);
    CHECK(md_x86_linear(0xFFFFu, 0x0010u) == 0u);
}

static void test_reg8_aliasing(uint8_t *memory)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.memory = memory;
    cpu.r[MD_X86_AX] = 0x1234u;
    CHECK(md_x86_get_reg8(&cpu, 0u) == 0x34u);
    CHECK(md_x86_get_reg8(&cpu, 4u) == 0x12u);
    md_x86_set_reg8(&cpu, 4u, 0xABu);
    CHECK(cpu.r[MD_X86_AX] == 0xAB34u);
    md_x86_set_reg8(&cpu, 0u, 0xCDu);
    CHECK(cpu.r[MD_X86_AX] == 0xABCDu);
}

static MdStopReason run_interp(uint8_t *memory, char *output, size_t output_size, uint64_t *instructions)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, output, output_size);
    hooks.interrupt = md_host_dos_interrupt;
    hooks.in8 = NULL;
    hooks.out8 = NULL;
    hooks.user = &host;
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kHelloCom, sizeof(kHelloCom), 0x1000u);
    {
        MdStopReason stop = md_interp_run(&runtime, 1000u);
        *instructions = runtime.instructions;
        CHECK(runtime.exit_code == 0u);
        return stop;
    }
}

static MdStopReason run_recomp(uint8_t *memory, char *output, size_t output_size, uint64_t *instructions)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, output, output_size);
    hooks.interrupt = md_host_dos_interrupt;
    hooks.in8 = NULL;
    hooks.out8 = NULL;
    hooks.user = &host;
    md_runtime_init(&runtime, memory, &hooks);
    {
        MdStopReason stop = md_recomp_hello(&runtime, 0x1000u);
        *instructions = runtime.instructions;
        CHECK(runtime.exit_code == 0u);
        return stop;
    }
}

static void test_interp_vs_recomp(uint8_t *memory)
{
    char interp_output[256];
    char recomp_output[256];
    uint64_t interp_instructions = 0;
    uint64_t recomp_instructions = 0;
    const MdStopReason interp_stop = run_interp(memory, interp_output, sizeof(interp_output), &interp_instructions);
    const MdStopReason recomp_stop = run_recomp(memory, recomp_output, sizeof(recomp_output), &recomp_instructions);

    CHECK(interp_stop == MD_STOP_EXIT);
    CHECK(recomp_stop == MD_STOP_EXIT);
    CHECK(strcmp(interp_output, "Hello from microDOS!\r\n") == 0);
    CHECK(strcmp(interp_output, recomp_output) == 0);
    CHECK(interp_instructions == 5u);
    CHECK(recomp_instructions == 5u);
}

static void test_real_ivt_fallback(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    runtime.cpu.cs = 0x1234u;
    runtime.cpu.ip = 0x5678u;
    runtime.cpu.ss = 0x2000u;
    runtime.cpu.r[MD_X86_SP] = 0x1000u;
    runtime.cpu.flags = (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_IF | MD_X86_FLAG_TF);
    md_x86_write16_linear(&runtime.cpu, 0x30u * 4u, 0x1111u);
    md_x86_write16_linear(&runtime.cpu, 0x30u * 4u + 2u, 0x2222u);

    CHECK(!md_runtime_interrupt(&runtime, 0x30u));
    CHECK(runtime.cpu.cs == 0x2222u);
    CHECK(runtime.cpu.ip == 0x1111u);
    CHECK((runtime.cpu.flags & (MD_X86_FLAG_IF | MD_X86_FLAG_TF)) == 0u);
    CHECK(runtime.cpu.r[MD_X86_SP] == 0x0FFAu);
    CHECK(md_x86_read16(&runtime.cpu, 0x2000u, 0x0FFAu) == 0x5678u);
    CHECK(md_x86_read16(&runtime.cpu, 0x2000u, 0x0FFCu) == 0x1234u);
}

int main(void)
{
    uint8_t *memory = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    if (memory == NULL) return 2;

    test_address_wrap(memory);
    test_reg8_aliasing(memory);
    test_interp_vs_recomp(memory);
    test_real_ivt_fallback(memory);

    free(memory);
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("microDOS runtime tests passed");
    return 0;
}
