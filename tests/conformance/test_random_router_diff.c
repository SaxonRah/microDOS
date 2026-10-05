/*
 * Deterministic randomized differential test.
 *
 * Generates bounded real-mode 8086 programs and compares the canonical
 * interpreter against the production DOS execution router/JIT policy after
 * every scheduler slice.  This is intentionally independent of DOS2TEST.
 */

#include "md_dos2_system.h"
#include "microdos/jit.h"
#include "microdos/runtime.h"
#include "microdos/x86.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#define CODE_SEG 0x1000u
#define DATA_SEG 0x3000u
#define EXTRA_SEG 0x4000u
#define STACK_SEG 0x5000u
#define CODE_OFF 0x0100u
#define MAX_PROGRAM 4096u

static uint32_t rng_state = 0xC0FFEE11u;

static uint32_t rng32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static uint16_t rng16(void) { return (uint16_t)rng32(); }

typedef struct Emit {
    uint8_t bytes[MAX_PROGRAM];
    size_t len;
} Emit;

static void e8(Emit *e, uint8_t v)
{
    if (e->len >= sizeof(e->bytes)) {
        fprintf(stderr, "random-diff: program overflow\n");
        exit(2);
    }
    e->bytes[e->len++] = v;
}

static void e16(Emit *e, uint16_t v)
{
    e8(e, (uint8_t)v);
    e8(e, (uint8_t)(v >> 8));
}

static void mov_r16_imm(Emit *e, unsigned reg, uint16_t v)
{
    e8(e, (uint8_t)(0xB8u + (reg & 7u)));
    e16(e, v);
}

static void alu_r16_imm(Emit *e, unsigned ext, unsigned reg, uint16_t v)
{
    e8(e, 0x81u);
    e8(e, (uint8_t)(0xC0u | ((ext & 7u) << 3) | (reg & 7u)));
    e16(e, v);
}

static void emit_safe_op(Emit *e)
{
    unsigned reg = rng32() & 7u;
    uint16_t imm = rng16();
    uint16_t off = (uint16_t)(0x0200u + (rng32() & 0x03FEu));

    switch (rng32() % 18u) {
        case 0: mov_r16_imm(e, reg, imm); break;
        case 1: alu_r16_imm(e, 0u, reg, imm); break; /* ADD */
        case 2: alu_r16_imm(e, 5u, reg, imm); break; /* SUB */
        case 3: alu_r16_imm(e, 6u, reg, imm); break; /* XOR */
        case 4: alu_r16_imm(e, 7u, reg, imm); break; /* CMP */
        case 5: e8(e, (uint8_t)(0x40u + reg)); break; /* INC */
        case 6: e8(e, (uint8_t)(0x48u + reg)); break; /* DEC */
        case 7:
            e8(e, (uint8_t)(0x50u + reg));            /* PUSH */
            e8(e, (uint8_t)(0x58u + reg));            /* POP same */
            break;
        case 8:
            e8(e, 0xA3u); e16(e, off);                /* MOV [disp],AX */
            break;
        case 9:
            e8(e, 0xA1u); e16(e, off);                /* MOV AX,[disp] */
            break;
        case 10:
            e8(e, 0x26u); e8(e, 0xA1u); e16(e, off); /* ES:MOV AX,[disp] */
            break;
        case 11:
            e8(e, 0xD1u); e8(e, (uint8_t)(0xC0u | reg)); /* ROL r16,1 */
            break;
        case 12:
            e8(e, 0xD1u); e8(e, (uint8_t)(0xE8u | reg)); /* SHR r16,1 */
            break;
        case 13:
            e8(e, 0xA9u); e16(e, imm);                /* TEST AX,imm */
            break;
        case 14:
            e8(e, 0x9Cu); e8(e, 0x9Du);               /* PUSHF/POPF */
            break;
        case 15:
            e8(e, (uint8_t)(0x90u + (1u + (rng32() % 7u)))); /* XCHG AX,r */
            break;
        case 16:
            e8(e, (rng32() & 1u) ? 0xF8u : 0xF9u);    /* CLC/STC */
            break;
        default:
            e8(e, 0xF5u);                             /* CMC */
            break;
    }
}

static void emit_loop(Emit *e)
{
    const uint16_t count = (uint16_t)(2u + (rng32() & 63u));
    mov_r16_imm(e, MD_X86_CX, count);
    e8(e, 0x40u);       /* INC AX */
    e8(e, 0xE2u);       /* LOOP */
    e8(e, 0xFDu);       /* back to INC AX */
}

static void emit_callret(Emit *e)
{
    /* CALL +2 -> INC BX; RET.  The fallthrough JMP skips the subroutine. */
    e8(e, 0xE8u); e16(e, 0x0002u);
    e8(e, 0xEBu); e8(e, 0x02u);
    e8(e, 0x43u);
    e8(e, 0xC3u);
}

static void emit_string(Emit *e)
{
    const uint16_t count = (uint16_t)(1u + (rng32() & 31u));
    mov_r16_imm(e, MD_X86_SI, (uint16_t)(0x0600u + (rng32() & 0x003Fu)));
    mov_r16_imm(e, MD_X86_DI, (uint16_t)(0x0700u + (rng32() & 0x003Fu)));
    mov_r16_imm(e, MD_X86_CX, count);
    e8(e, 0xFCu);       /* CLD */
    e8(e, 0xF3u); e8(e, (rng32() & 1u) ? 0xA4u : 0xA5u); /* REP MOVSB/W */
}

static void make_program(Emit *e)
{
    unsigned i;
    memset(e, 0, sizeof(*e));

    mov_r16_imm(e, MD_X86_AX, rng16());
    mov_r16_imm(e, MD_X86_BX, rng16());
    mov_r16_imm(e, MD_X86_DX, rng16());
    mov_r16_imm(e, MD_X86_BP, rng16());

    for (i = 0u; i < 40u + (rng32() % 80u); ++i) {
        const uint32_t kind = rng32() % 16u;
        if (kind == 0u) emit_loop(e);
        else if (kind == 1u) emit_callret(e);
        else if (kind == 2u) emit_string(e);
        else emit_safe_op(e);
    }
    e8(e, 0xF4u);       /* HLT */
}

static uint16_t flags_of(MdX86 *cpu)
{
    return md_x86_flags(cpu);
}

static int equal_state(const MdRuntime *a_const, const MdRuntime *b_const,
                       const uint8_t *ma, const uint8_t *mb,
                       unsigned case_no, unsigned slice_no)
{
    MdRuntime *a = (MdRuntime *)(uintptr_t)a_const;
    MdRuntime *b = (MdRuntime *)(uintptr_t)b_const;
    if (memcmp(a->cpu.r, b->cpu.r, sizeof(a->cpu.r)) != 0 ||
        a->cpu.cs != b->cpu.cs || a->cpu.ip != b->cpu.ip ||
        a->cpu.ds != b->cpu.ds || a->cpu.es != b->cpu.es ||
        a->cpu.ss != b->cpu.ss ||
        flags_of(&a->cpu) != flags_of(&b->cpu) ||
        a->instructions != b->instructions ||
        a->stop_reason != b->stop_reason ||
        memcmp(ma, mb, MD_X86_ADDRESS_SPACE) != 0) {
        fprintf(stderr,
                "[random-diff] mismatch case=%u slice=%u ins=%llu/%llu "
                "CS:IP=%04X:%04X/%04X:%04X F=%04X/%04X stop=%d/%d\n",
                case_no, slice_no,
                (unsigned long long)a->instructions,
                (unsigned long long)b->instructions,
                a->cpu.cs, a->cpu.ip, b->cpu.cs, b->cpu.ip,
                flags_of(&a->cpu), flags_of(&b->cpu),
                (int)a->stop_reason, (int)b->stop_reason);
        return 0;
    }
    return 1;
}

static int run_case(unsigned case_no, unsigned budget, uint8_t *jit_code)
{
    Emit program;
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdRuntime ref;
    MdDos2System sys;
    MdJit jit;
    MdHooks hooks;
    unsigned i;
    unsigned slices = 0u;

    if (ma == NULL || mb == NULL) {
        free(ma); free(mb);
        return 0;
    }

    memset(&hooks, 0, sizeof(hooks));
    make_program(&program);

    for (i = 0u; i < 0x1000u; ++i) {
        ma[md_x86_linear(DATA_SEG, (uint16_t)i)] = (uint8_t)rng32();
        ma[md_x86_linear(EXTRA_SEG, (uint16_t)i)] = (uint8_t)rng32();
    }
    memcpy(mb, ma, MD_X86_ADDRESS_SPACE);

    memcpy(ma + md_x86_linear(CODE_SEG, CODE_OFF), program.bytes, program.len);
    memcpy(mb + md_x86_linear(CODE_SEG, CODE_OFF), program.bytes, program.len);

    md_runtime_init(&ref, ma, &hooks);
    md_dos2_system_init(&sys, mb, NULL);
    memset(&sys.runtime.hooks, 0, sizeof(sys.runtime.hooks));

    ref.cpu.cs = sys.runtime.cpu.cs = CODE_SEG;
    ref.cpu.ip = sys.runtime.cpu.ip = CODE_OFF;
    ref.cpu.ds = sys.runtime.cpu.ds = DATA_SEG;
    ref.cpu.es = sys.runtime.cpu.es = EXTRA_SEG;
    ref.cpu.ss = sys.runtime.cpu.ss = STACK_SEG;
    ref.cpu.r[MD_X86_SP] = sys.runtime.cpu.r[MD_X86_SP] = 0xFFF0u;
    md_x86_set_flags(&ref.cpu, MD_X86_FLAG_ALWAYS1);
    md_x86_set_flags(&sys.runtime.cpu, MD_X86_FLAG_ALWAYS1);
    sys.boot.dos_segment = 0x6000u;

    md_jit_init(&jit, jit_code, 65536u);
    md_dos2_system_set_jit(&sys, &jit);

    while (ref.stop_reason == MD_STOP_NONE && slices < 200000u) {
        (void)md_interp_run(&ref, budget);
        if (ref.stop_reason == MD_STOP_BUDGET) ref.stop_reason = MD_STOP_NONE;

        (void)md_dos2_system_run(&sys, budget);
        if (sys.runtime.stop_reason == MD_STOP_BUDGET)
            sys.runtime.stop_reason = MD_STOP_NONE;

        if (!equal_state(&ref, &sys.runtime, ma, mb, case_no, slices))
            goto fail;
        ++slices;
    }

    if (ref.stop_reason != MD_STOP_HALT ||
        sys.runtime.stop_reason != MD_STOP_HALT) {
        fprintf(stderr, "[random-diff] no clean HLT case=%u budget=%u\n",
                case_no, budget);
        goto fail;
    }

    free(ma);
    free(mb);
    return 1;

fail:
    fprintf(stderr, "[random-diff] seed-state=%08X case=%u budget=%u program-bytes=%zu\n",
            rng_state, case_no, budget, program.len);
    free(ma);
    free(mb);
    return 0;
}

int main(int argc, char **argv)
{
    static const unsigned budgets[] = {1u,2u,3u,7u,16u,31u,64u,257u,4096u};
    unsigned cases = 1000u;
    unsigned i;
    uint8_t *jit_code = NULL;

    if (argc > 1) {
        char *end = NULL;
        unsigned long v = strtoul(argv[1], &end, 0);
        if (end == NULL || *end != '\0' || v == 0u) {
            fprintf(stderr, "usage: %s [cases] [seed]\n", argv[0]);
            return 2;
        }
        cases = (unsigned)v;
    }
    if (argc > 2) {
        char *end = NULL;
        unsigned long v = strtoul(argv[2], &end, 0);
        if (end == NULL || *end != '\0') return 2;
        rng_state = (uint32_t)v;
    }

#if defined(__linux__)
    jit_code = (uint8_t *)mmap(NULL, 65536u,
                               PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (jit_code == MAP_FAILED) jit_code = NULL;
#endif
    if (jit_code == NULL) jit_code = (uint8_t *)calloc(1u, 65536u);
    if (jit_code == NULL) return 2;

    printf("[random-diff] cases=%u seed=%08X\n", cases, rng_state);
    for (i = 0u; i < cases; ++i) {
        const unsigned budget = budgets[i % (sizeof(budgets)/sizeof(budgets[0]))];
        if (!run_case(i, budget, jit_code)) {
            fprintf(stderr, "[random-diff] FAIL at case %u\n", i);
            return 1;
        }
        if ((i + 1u) % 100u == 0u)
            printf("[random-diff] %u/%u PASS\n", i + 1u, cases);
    }

    printf("[random-diff] PASS cases=%u\n", cases);
    return 0;
}
