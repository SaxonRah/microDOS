/*
 * Focused NV2-G G-2B2/G-2C qemu differential.
 *
 * This intentionally runs in addition to tests/test_native_v2g_diff.c.
 * It targets the two new semantics:
 *   - path-dependent FLAGS merges whose producers are exact NLM states;
 *   - one bounded static near CALL/C3-RET leaf with exact guest stack effects.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "microdos/runtime.h"
#include "microdos/native_v2g.h"

#define TEST_SEG 0x1000u
#define TEST_IP  0x0100u

static uint32_t g_seed = 0x42324332u;

static uint32_t rnd32(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static void put16(uint8_t *mem, uint16_t seg, uint16_t off, uint16_t v)
{
    uint32_t p = md_x86_linear(seg, off);
    mem[p] = (uint8_t)v;
    mem[(p + 1u) & (MD_X86_ADDRESS_SPACE - 1u)] = (uint8_t)(v >> 8);
}

static void setup_runtime(MdRuntime *rt, uint8_t *mem,
                          const uint8_t *program, size_t program_size)
{
    MdHooks hooks;
    unsigned i;

    memset(&hooks, 0, sizeof(hooks));
    memset(mem, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(rt, mem, &hooks);
    md_runtime_load_com(rt, program, program_size, TEST_SEG);

    rt->cpu.cs = TEST_SEG;
    rt->cpu.ds = TEST_SEG;
    rt->cpu.es = TEST_SEG;
    rt->cpu.ss = TEST_SEG;
    rt->cpu.ip = TEST_IP;
    rt->cpu.r[MD_X86_SP] = 0xF000u;
    rt->cpu.r[MD_X86_BP] = 0x3000u;
    rt->cpu.r[MD_X86_SI] = 0x3200u;
    rt->cpu.r[MD_X86_DI] = 0x3300u;
    rt->cpu.r[MD_X86_CX] = 2u;
    rt->cpu.r[MD_X86_DX] = 0x3202u;

    for (i = 0u; i < 8u; ++i) {
        if (i != MD_X86_SP && i != MD_X86_BP &&
            i != MD_X86_SI && i != MD_X86_DI &&
            i != MD_X86_CX && i != MD_X86_DX)
            rt->cpu.r[i] = (uint16_t)rnd32();
    }

    md_x86_set_flags(&rt->cpu,
        (uint16_t)(MD_X86_FLAG_ALWAYS1 | (rnd32() & 0x08D5u)));
    rt->cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
    rt->instructions = 0u;
    rt->stop_reason = MD_STOP_NONE;
}

static void clone_runtime(MdRuntime *dst, uint8_t *dst_mem,
                          const MdRuntime *src, const uint8_t *src_mem)
{
    MdHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    memcpy(dst_mem, src_mem, MD_X86_ADDRESS_SPACE);
    md_runtime_init(dst, dst_mem, &hooks);
    memcpy(dst->cpu.r, src->cpu.r, sizeof(src->cpu.r));
    dst->cpu.es = src->cpu.es;
    dst->cpu.cs = src->cpu.cs;
    dst->cpu.ss = src->cpu.ss;
    dst->cpu.ds = src->cpu.ds;
    dst->cpu.ip = src->cpu.ip;
    dst->cpu.flags_raw = src->cpu.flags_raw;
    dst->cpu.lazy_op = src->cpu.lazy_op;
    dst->cpu.lazy_carry = src->cpu.lazy_carry;
    dst->cpu.lazy_a = src->cpu.lazy_a;
    dst->cpu.lazy_b = src->cpu.lazy_b;
    dst->cpu.lazy_res = src->cpu.lazy_res;
    dst->instructions = src->instructions;
    dst->stop_reason = src->stop_reason;
}

static unsigned compare_state(MdRuntime *a, MdRuntime *b)
{
    MdX86 ca = a->cpu;
    MdX86 cb = b->cpu;
    if (memcmp(a->cpu.r, b->cpu.r, sizeof(a->cpu.r)) != 0) return 1u;
    if (a->cpu.cs != b->cpu.cs || a->cpu.ds != b->cpu.ds ||
        a->cpu.es != b->cpu.es || a->cpu.ss != b->cpu.ss ||
        a->cpu.ip != b->cpu.ip) return 2u;
    if (md_x86_flags(&ca) != md_x86_flags(&cb)) return 3u;
    if (a->stop_reason != b->stop_reason) return 4u;
    if (memcmp(a->cpu.memory, b->cpu.memory, MD_X86_ADDRESS_SPACE) != 0)
        return 5u;
    return 0u;
}

static void dump_state(const char *name, unsigned kind,
                       MdRuntime *a, MdRuntime *b,
                       uint32_t budget, uint32_t retired)
{
    MdX86 ca = a->cpu, cb = b->cpu;
    unsigned i;
    printf("[nv2g-bc] MISMATCH %s kind=%u budget=%u retired=%u\n",
           name, kind, (unsigned)budget, (unsigned)retired);
    printf("  interp ip=%04X flags=%04X regs", a->cpu.ip, md_x86_flags(&ca));
    for (i = 0u; i < 8u; ++i) printf(" %04X", a->cpu.r[i]);
    printf("\n");
    printf("  native ip=%04X flags=%04X regs", b->cpu.ip, md_x86_flags(&cb));
    for (i = 0u; i < 8u; ++i) printf(" %04X", b->cpu.r[i]);
    printf("\n");
}

static int compile_program(const uint8_t *program, size_t size,
                           MdNativeV2Code *code, uint8_t *memory)
{
    size_t guest_size = 0u;
    MdNativeV2Status st;
    size_t i;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    for (i = 0u; i < size; ++i)
        memory[md_x86_linear(TEST_SEG, (uint16_t)(TEST_IP + i))] = program[i];

    memset(code, 0, sizeof(*code));
    st = md_native_v2g_compile_loop_graph(
        memory, TEST_SEG, TEST_IP, code, &guest_size);
    if (st != MD_NATIVE_V2_OK) {
        printf("[nv2g-bc] compile failed status=%d size=%u\n",
               (int)st, (unsigned)size);
        return 0;
    }
#if defined(__GNUC__)
    __builtin___clear_cache(
        (char *)&code->bytes[0],
        (char *)&code->bytes[0] + code->size);
#endif
    return 1;
}

/*
 * Prefix oracle: execute native once, then interpret exactly the number of
 * guest instructions the native return value says retired.
 */
static unsigned run_exact(const char *name,
                          const uint8_t *program, size_t size,
                          MdNativeV2Code *code,
                          uint8_t *ma, uint8_t *mb,
                          void (*prep)(MdX86 *, unsigned),
                          unsigned variant,
                          uint32_t budget)
{
    MdRuntime a, b;
    uint32_t rem, retired, i;
    unsigned d;

    setup_runtime(&a, ma, program, size);
    if (prep != NULL) prep(&a.cpu, variant);
    clone_runtime(&b, mb, &a, ma);

    rem = md_native_v2g_execute(&b.cpu, code, budget);
    if (rem == MD_NATIVE_V2_EXEC_FALLBACK || rem > budget) {
        printf("[nv2g-bc] %s unexpected fallback variant=%u budget=%u\n",
               name, variant, (unsigned)budget);
        return 1u;
    }

    retired = budget - rem;
    for (i = 0u; i < retired; ++i)
        (void)md_interp_step(&a);

    d = compare_state(&a, &b);
    if (d != 0u) {
        dump_state(name, d, &a, &b, budget, retired);
        return 1u;
    }

    printf("[nv2g-bc] %-24s variant=%u retired=%u PASS\n",
           name, variant, (unsigned)retired);
    return 0u;
}

/* Exact MASM census root 21A5:00E3, rebased at TEST_IP. */
static const uint8_t kMergeExit[] = {
    0x8B,0x7E,0xF4, 0x03,0x7E,0x00, 0x8B,0x15,
    0x8B,0x7E,0xF4, 0x03,0x7E,0xFE, 0x3A,0x15, 0x74,0x1B,
    0x8B,0x7E,0xF4, 0x03,0x7E,0x00, 0x8B,0x15,
    0x8B,0x7E,0xF4, 0x03,0x7E,0xFE, 0x3A,0x15,
    0xB9,0x00,0x00, 0x73,0x01, 0x41, 0x88,0x4E,0xF6,
    0xEB,0x0D,
    0x8B,0x56,0xF4, 0x42, 0x89,0x56,0xF4, 0x4A,
    0x3B,0x56,0xF0, 0x75,0xC6
};

static void prep_merge(MdX86 *c, unsigned variant)
{
    /* BP frame values used by the real MASM loop. */
    put16(c->memory, c->ss, 0x2FF4u, 0x3100u); /* [bp-0Ch] */
    put16(c->memory, c->ss, 0x3000u, 0x0000u); /* [bp+0] */
    put16(c->memory, c->ss, 0x2FFEu, 0x0010u); /* [bp-2] */
    put16(c->memory, c->ss, 0x2FF0u, 0x3200u); /* [bp-10h] */

    /* First load is DS:3100, compare byte is DS:3110. */
    c->memory[md_x86_linear(c->ds, 0x3101u)] = 0x32u;
    if (variant == 0u) {
        /* First CMP equal -> take JE to common tail. */
        c->memory[md_x86_linear(c->ds, 0x3100u)] = 0x44u;
        c->memory[md_x86_linear(c->ds, 0x3110u)] = 0x44u;
    } else if (variant == 1u) {
        /* Both CMPs not equal, unsigned-below -> INC CX producer wins merge. */
        c->memory[md_x86_linear(c->ds, 0x3100u)] = 0x10u;
        c->memory[md_x86_linear(c->ds, 0x3110u)] = 0x20u;
    } else {
        /* Both CMPs not equal, unsigned-above -> CMP producer wins merge. */
        c->memory[md_x86_linear(c->ds, 0x3100u)] = 0x30u;
        c->memory[md_x86_linear(c->ds, 0x3110u)] = 0x20u;
    }
}

/*
 * Caller span 0100-0109; leaf deliberately begins beyond the old 64-byte window:
 *   call 0150
 *   jz   010C      ; architectural side exit using callee CMP flags
 *   mov  [si],al   ; exercises caller-store protection of callee span
 *   inc  si
 *   cmp  si,dx
 *   jne  0100
 *
 * 010C-014F are padding. Leaf 0150-0153:
 *   cmp ax,1234h
 *   ret
 */
static void make_call_leaf(uint8_t *p, size_t n)
{
    size_t i;
    if (n < 0x54u) return;
    for (i = 0u; i < n; ++i) p[i] = 0x90u;

    /* CALL target 0150h: rel16 = 0150 - 0103 = 004Dh. */
    p[0x00] = 0xE8u; p[0x01] = 0x4Du; p[0x02] = 0x00u;
    p[0x03] = 0x74u; p[0x04] = 0x07u; /* jz 010Ch */
    p[0x05] = 0x88u; p[0x06] = 0x04u; /* mov [si],al */
    p[0x07] = 0x46u;                   /* inc si */
    p[0x08] = 0x3Bu; p[0x09] = 0xF2u; /* cmp si,dx */
    p[0x0A] = 0x75u; p[0x0B] = 0xF4u; /* jne 0100h */

    p[0x50] = 0x3Du; p[0x51] = 0x34u; p[0x52] = 0x12u; /* cmp ax,1234h */
    p[0x53] = 0xC3u;                                      /* ret */
}

static void prep_call(MdX86 *c, unsigned variant)
{
    c->r[MD_X86_SP] = 0xF000u;
    c->r[MD_X86_SI] = variant == 4u
        ? (uint16_t)(TEST_IP + 0x50u) : 0x3200u;
    c->r[MD_X86_DX] = variant == 4u
        ? (uint16_t)(TEST_IP + 0x55u)
        : (uint16_t)(0x3201u + (variant & 3u));
    c->r[MD_X86_AX] = variant == 0u ? 0x1234u : 0x7777u;
}

static unsigned run_call_fallbacks(const uint8_t *program, size_t size,
                                   MdNativeV2Code *code,
                                   uint8_t *ma, uint8_t *mb)
{
    static uint8_t pages[MD_X86_CODE_PAGE_COUNT];
    MdRuntime a, b;
    uint32_t rem;
    unsigned d, fails = 0u;
    uint32_t stack_lin;

    /* Tracked CALL return slot must refuse native entry without mutation. */
    setup_runtime(&a, ma, program, size);
    prep_call(&a.cpu, 1u);
    clone_runtime(&b, mb, &a, ma);
    memset(pages, 0, sizeof(pages));
    stack_lin = md_x86_linear(
        b.cpu.ss, (uint16_t)(b.cpu.r[MD_X86_SP] - 2u));
    pages[stack_lin >> MD_X86_CODE_PAGE_SHIFT] = 1u;
    b.cpu.code_page_executable = pages;
    rem = md_native_v2g_execute(&b.cpu, code, 64u);
    b.cpu.code_page_executable = NULL;
    d = compare_state(&a, &b);
    if (rem != MD_NATIVE_V2_EXEC_FALLBACK || d != 0u) {
        printf("[nv2g-bc] tracked-call-stack FAILED fallback=%d kind=%u\n",
               rem == MD_NATIVE_V2_EXEC_FALLBACK, d);
        ++fails;
    } else {
        printf("[nv2g-bc] tracked-call-stack       PASS (entry fallback)\n");
    }

    /* SP underflow is outside the bounded stack proof. */
    setup_runtime(&a, ma, program, size);
    prep_call(&a.cpu, 1u);
    a.cpu.r[MD_X86_SP] = 1u;
    clone_runtime(&b, mb, &a, ma);
    rem = md_native_v2g_execute(&b.cpu, code, 64u);
    d = compare_state(&a, &b);
    if (rem != MD_NATIVE_V2_EXEC_FALLBACK || d != 0u) {
        printf("[nv2g-bc] call-sp-underflow FAILED fallback=%d kind=%u\n",
               rem == MD_NATIVE_V2_EXEC_FALLBACK, d);
        ++fails;
    } else {
        printf("[nv2g-bc] call-sp-underflow        PASS (entry fallback)\n");
    }

    /* Return slot overlapping caller code must also refuse entry. */
    setup_runtime(&a, ma, program, size);
    prep_call(&a.cpu, 1u);
    a.cpu.r[MD_X86_SP] = (uint16_t)(TEST_IP + 2u);
    clone_runtime(&b, mb, &a, ma);
    rem = md_native_v2g_execute(&b.cpu, code, 64u);
    d = compare_state(&a, &b);
    if (rem != MD_NATIVE_V2_EXEC_FALLBACK || d != 0u) {
        printf("[nv2g-bc] call-code-overlap FAILED fallback=%d kind=%u\n",
               rem == MD_NATIVE_V2_EXEC_FALLBACK, d);
        ++fails;
    } else {
        printf("[nv2g-bc] call-code-overlap        PASS (entry fallback)\n");
    }

    return fails;
}

int main(void)
{
    uint8_t *ma = NULL, *mb = NULL;
    MdNativeV2Code *code = NULL;
    uint8_t call_program[0x54u];
    unsigned fails = 0u;
    unsigned v;

    ma = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    mb = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    if (ma == NULL || mb == NULL) {
        fprintf(stderr, "guest allocation failed\n");
        return 2;
    }

#if defined(__linux__)
    {
        long page = sysconf(_SC_PAGESIZE);
        size_t rounded;
        if (page <= 0) page = 4096;
        rounded = (sizeof(MdNativeV2Code) + (size_t)page - 1u) &
                  ~((size_t)page - 1u);
        code = (MdNativeV2Code *)mmap(
            NULL, rounded,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1, 0);
        if (code == MAP_FAILED) code = NULL;
    }
#else
    code = (MdNativeV2Code *)malloc(sizeof(*code));
#endif
    if (code == NULL) {
        fprintf(stderr, "executable code allocation failed\n");
        return 2;
    }

    printf("== NV2-G G-2B2/G-2C focused differential ==\n");
    make_call_leaf(call_program, sizeof(call_program));

    if (!compile_program(kMergeExit, sizeof(kMergeExit), code, ma)) {
        ++fails;
    } else {
        for (v = 0u; v < 3u; ++v)
            fails += run_exact(
                "G-2B2 MASM merge", kMergeExit, sizeof(kMergeExit),
                code, ma, mb, prep_merge, v, 96u);
    }

    if (!compile_program(call_program, sizeof(call_program), code, ma)) {
        ++fails;
    } else {
        if (!code->local_call_graph ||
            code->call_stack_bytes != 2u ||
            code->guest_span_count != 2u ||
            code->guest_span_ip[0] != TEST_IP ||
            code->guest_span_ip[1] != (uint16_t)(TEST_IP + 0x50u)) {
            printf("[nv2g-bc] G-2C metadata FAILED local=%u depth=%u spans=%u "
                   "span0=%04X span1=%04X\n",
                   (unsigned)code->local_call_graph,
                   (unsigned)code->call_stack_bytes,
                   (unsigned)code->guest_span_count,
                   code->guest_span_ip[0], code->guest_span_ip[1]);
            ++fails;
        } else {
            printf("[nv2g-bc] G-2C metadata             PASS "
                   "(caller=%u callee=%u native-bytes=%u)\n",
                   (unsigned)code->guest_span_len[0],
                   (unsigned)code->guest_span_len[1],
                   (unsigned)code->size);
        }

        for (v = 0u; v < 5u; ++v)
            fails += run_exact(
                "G-2C CALL/RET", call_program, sizeof(call_program),
                code, ma, mb, prep_call, v, 96u);

        fails += run_call_fallbacks(
            call_program, sizeof(call_program), code, ma, mb);
    }

    printf("[nv2g-bc] RESULT %s mismatches=%u\n",
           fails ? "FAIL" : "PASS", fails);

    free(ma);
    free(mb);
    return fails ? 1 : 0;
}
