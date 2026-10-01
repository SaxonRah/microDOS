#include "microdos/jit.h"

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

#if defined(__arm__) || defined(__thumb__)
#define JIT_SHAPE_CHECK(expr) CHECK(expr)
#else
#define JIT_SHAPE_CHECK(expr) ((void)0)
#endif

static void test_command_string_loop(int backwards)
{
    static const uint8_t program[] = {
        0xAD,                   /* lodsw */
        0x01,0xC2,             /* add dx,ax */
        0xE2,0xFB,             /* loop 0100h */
        0xF4                    /* hlt */
    };
    uint8_t *memory = (uint8_t *)calloc(MD_X86_ADDRESS_SPACE, 1u);
    uint8_t code[4096];
    MdHooks hooks;
    MdRuntime runtime;
    MdJit jit;
    MdStopReason st;
    uint16_t segment = 0x1000u;
    unsigned i;

    CHECK(memory != NULL);
    if (memory == NULL) return;

    memset(&hooks, 0, sizeof(hooks));
    memset(code, 0, sizeof(code));
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, program, sizeof(program), segment);

    for (i = 0u; i < 4u; ++i)
        md_x86_write16(&runtime.cpu, segment, (uint16_t)(0x0200u + i * 2u),
                       (uint16_t)(i + 1u));

    runtime.cpu.r[MD_X86_CX] = 4u;
    runtime.cpu.r[MD_X86_DX] = 0u;
    runtime.cpu.r[MD_X86_SI] = backwards ? 0x0206u : 0x0200u;
    if (backwards) runtime.cpu.flags_raw |= MD_X86_FLAG_DF;
    else runtime.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;

    md_jit_init(&jit, code, sizeof(code));
    st = md_jit_run(&jit, &runtime, 1000u);

    CHECK(st == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_CX] == 0u);
    CHECK(runtime.cpu.r[MD_X86_DX] == 10u);
    CHECK(runtime.cpu.r[MD_X86_AX] == (backwards ? 1u : 4u));
    CHECK(runtime.cpu.r[MD_X86_SI] == (backwards ? 0x01FEu : 0x0208u));
    CHECK(runtime.instructions == 13u);
    /* Code-shape checks apply to native Thumb-2 (RP2350, qemu-arm). On a
       64-bit host some MdRuntime offsets exceed Thumb immediate ranges, so
       the reference path legitimately interprets e.g. the final HLT. */
    JIT_SHAPE_CHECK(jit.fallback_instructions == 0u);
    JIT_SHAPE_CHECK(jit.resident_regions >= 1u);

    free(memory);
}

static void test_lodsw_wrap(int backwards)
{
    static const uint8_t program[] = { 0xAD, 0xF4 };
    uint8_t *memory = (uint8_t *)calloc(MD_X86_ADDRESS_SPACE, 1u);
    uint8_t code[4096];
    MdHooks hooks;
    MdRuntime runtime;
    MdJit jit;
    MdStopReason st;

    CHECK(memory != NULL);
    if (memory == NULL) return;

    memset(&hooks, 0, sizeof(hooks));
    memset(code, 0, sizeof(code));
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, program, sizeof(program), 0x1000u);

    runtime.cpu.ds = 0xFFFFu;
    runtime.cpu.r[MD_X86_SI] = 0x000Fu; /* linear FFFFFh */
    memory[0xFFFFFu] = 0x34u;
    memory[0x00000u] = 0x12u;           /* read16 physical wrap */
    if (backwards) runtime.cpu.flags_raw |= MD_X86_FLAG_DF;
    else runtime.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;

    md_jit_init(&jit, code, sizeof(code));
    st = md_jit_run(&jit, &runtime, 100u);

    CHECK(st == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x1234u);
    CHECK(runtime.cpu.r[MD_X86_SI] == (backwards ? 0x000Du : 0x0011u));
    CHECK(runtime.instructions == 2u);
    JIT_SHAPE_CHECK(jit.fallback_instructions == 0u);

    free(memory);
}

int main(void)
{
    test_command_string_loop(0);
    test_command_string_loop(1);
    test_lodsw_wrap(0);
    test_lodsw_wrap(1);

    if (failures != 0) {
        fprintf(stderr, "microDOS M20.3 JIT tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("microDOS M20.3 JIT tests: ok");
    return 0;
}
