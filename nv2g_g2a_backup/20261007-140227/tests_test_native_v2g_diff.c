/*
 * NV2-G qemu-arm differential test.
 *
 * Executes generated NV2-G Thumb-2 under qemu-arm and compares it against
 * the canonical microDOS interpreter from identical state.
 *
 * This is intentionally a G-1 verification harness. It tests the current
 * natural-loop compiler directly rather than the runtime admission cascade.
 * Existing Native-v2 special compilers are not involved.
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
#define DATA_OFF 0x3000u

typedef struct Nv2gProgram {
    const char *name;
    const uint8_t *code;
    size_t size;
} Nv2gProgram;

static uint32_t g_rng = 0x4E563247u;

static uint32_t rng32(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static uint16_t rng16(void)
{
    return (uint16_t)rng32();
}

static unsigned rn(unsigned n)
{
    return n ? (unsigned)(rng32() % n) : 0u;
}

/*
 * Directed G-1 programs. All end at a direct backward latch to 0100h.
 */

/* mov al,[si] / inc si / cmp al,0 / jne loop */
static const uint8_t kScan8[] = {
    0x8A,0x04,
    0x46,
    0x3C,0x00,
    0x75,0xF9
};

/*
 * Two architectural side exits:
 *   mov al,[si]
 *   cmp al,' ' / je end
 *   cmp al,9   / je end
 *   inc si
 *   cmp si,3020h
 *   jne loop
 */
static const uint8_t kMultiExit[] = {
    0x8A,0x04,
    0x3C,0x20,
    0x74,0x0B,
    0x3C,0x09,
    0x74,0x07,
    0x46,
    0x81,0xFE,0x20,0x30,
    0x75,0xEF
};

/* add ax,3 / cmp ax,40h / jl loop */
static const uint8_t kSignedJl[] = {
    0x83,0xC0,0x03,
    0x83,0xF8,0x40,
    0x7C,0xF8
};

/*
 * General DS ModR/M word load with disp8. OR establishes an exit recipe
 * before the word-load wrap guard.
 */
static const uint8_t kDsDispLoad[] = {
    0x0B,0xD2,             /* or dx,dx */
    0x8B,0x40,0x05,        /* mov ax,[bx+si+5] */
    0x03,0xD0,             /* add dx,ax */
    0x83,0xC6,0x02,        /* add si,2 */
    0x81,0xFE,0x40,0x30,   /* cmp si,3040h */
    0x75,0xF0              /* jne 0100h */
};

/* General SS-default [bp+di+disp8] word load. */
static const uint8_t kSsDispLoad[] = {
    0x0B,0xD2,             /* or dx,dx */
    0x8B,0x43,0x05,        /* mov ax,[bp+di+5] */
    0x03,0xD0,             /* add dx,ax */
    0x47,                  /* inc di */
    0x81,0xFF,0x40,0x30,   /* cmp di,3040h */
    0x75,0xF2              /* jne 0100h */
};

/*
 * JCXZ side exit with no FLAGS dependency, followed by an ADD/JNZ latch.
 * Target 0109h is exactly the region fall-through.
 */
static const uint8_t kJcxz[] = {
    0x89,0xDB,             /* mov bx,bx */
    0xE3,0x05,             /* jcxz end */
    0x83,0xC0,0x01,        /* add ax,1 */
    0x75,0xF7              /* jne 0100h */
};

/* cmp ax,20h / loopz 0100h */
static const uint8_t kLoopz[] = {
    0x3D,0x20,0x00,
    0xE1,0xFB
};

/* cmp ax,20h / loopnz 0100h */
static const uint8_t kLoopnz[] = {
    0x3D,0x20,0x00,
    0xE0,0xFB
};

static const Nv2gProgram kDirected[] = {
    { "scan8",       kScan8,       sizeof(kScan8)       },
    { "multi-exit",  kMultiExit,   sizeof(kMultiExit)   },
    { "signed-jl",   kSignedJl,    sizeof(kSignedJl)    },
    { "ds-disp-load",kDsDispLoad,  sizeof(kDsDispLoad)  },
    { "ss-disp-load",kSsDispLoad,  sizeof(kSsDispLoad)  },
    { "jcxz",        kJcxz,        sizeof(kJcxz)        },
    { "loopz",       kLoopz,       sizeof(kLoopz)       },
    { "loopnz",      kLoopnz,      sizeof(kLoopnz)      }
};

static void setup_runtime(MdRuntime *rt,
                          uint8_t *memory,
                          const uint8_t *program,
                          size_t program_size,
                          uint32_t salt)
{
    MdHooks hooks;
    unsigned i;
    uint32_t lin;

    memset(&hooks, 0, sizeof(hooks));
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(rt, memory, &hooks);
    md_runtime_load_com(rt, program, program_size, TEST_SEG);

    rt->cpu.ds = TEST_SEG;
    rt->cpu.es = TEST_SEG;
    rt->cpu.ss = TEST_SEG;
    rt->cpu.cs = TEST_SEG;
    rt->cpu.ip = TEST_IP;

    for (i = 0u; i < 8u; ++i)
        rt->cpu.r[i] = (uint16_t)(rng16() ^ (uint16_t)(salt * (i + 1u)));

    /*
     * Keep effective addresses in a known data window and away from FFFFh.
     * Guard-specific edge cases are separate from execution equivalence.
     */
    rt->cpu.r[MD_X86_BX] = (uint16_t)(0x3000u + rn(0x20u));
    rt->cpu.r[MD_X86_BP] = (uint16_t)(0x3000u + rn(0x20u));
    rt->cpu.r[MD_X86_SI] = (uint16_t)(0x3000u + rn(0x20u));
    rt->cpu.r[MD_X86_DI] = (uint16_t)(0x3000u + rn(0x20u));
    rt->cpu.r[MD_X86_SP] = 0xF000u;

    /*
     * Give LOOP-family fixtures useful counts often, but still include
     * CX=0/1 boundary states in randomized runs.
     */
    switch (salt & 7u) {
        case 0u: rt->cpu.r[MD_X86_CX] = 0u; break;
        case 1u: rt->cpu.r[MD_X86_CX] = 1u; break;
        case 2u: rt->cpu.r[MD_X86_CX] = 2u; break;
        default: rt->cpu.r[MD_X86_CX] = (uint16_t)(2u + rn(40u)); break;
    }

    md_x86_set_flags(&rt->cpu,
        (uint16_t)(0x0002u | (rng16() & 0x08D5u)));

    lin = md_x86_linear(TEST_SEG, DATA_OFF);
    for (i = 0u; i < 0x200u; ++i)
        memory[(lin + i) & (MD_X86_ADDRESS_SPACE - 1u)] =
            (uint8_t)(rng32() ^ (salt + i * 17u));

    /* Deterministic scanner exits. */
    memory[md_x86_linear(TEST_SEG, 0x3010u)] = 0x20u;
    memory[md_x86_linear(TEST_SEG, 0x3018u)] = 0x09u;
    memory[md_x86_linear(TEST_SEG, 0x3020u)] = 0x00u;

    rt->instructions = 0u;
    rt->stop_reason = MD_STOP_NONE;
}

/*
 * Reference scheduling contract for dynamic_retire=3:
 *
 * G-1A refuses to begin an iteration unless remaining >= op_count, where
 * op_count is its conservative max path length. Once an iteration begins,
 * each actually executed instruction consumes one budget unit. Execution
 * stops at an architectural side exit or at the loop root between iterations.
 */
static uint32_t ref_run_region(MdRuntime *rt,
                               const MdNativeV2Code *code,
                               uint32_t budget)
{
    uint32_t remaining = budget;
    const uint16_t start = code->start_ip;
    const uint16_t end = code->end_ip;
    unsigned safety = 0u;

    while (rt->stop_reason == MD_STOP_NONE &&
           rt->cpu.cs == TEST_SEG &&
           rt->cpu.ip == start &&
           remaining >= code->op_count) {

        for (;;) {
            if (remaining == 0u)
                return remaining;

            (void)md_interp_step(rt);
            --remaining;

            if (rt->stop_reason != MD_STOP_NONE ||
                rt->cpu.cs != TEST_SEG)
                return remaining;

            if (rt->cpu.ip == start)
                break;

            if (rt->cpu.ip < start || rt->cpu.ip >= end)
                return remaining;

            if (++safety > 1000000u) {
                fprintf(stderr, "reference safety trip\n");
                return remaining;
            }
        }
    }

    return remaining;
}

static unsigned compare_state(const MdRuntime *a,
                              const MdRuntime *b,
                              uint32_t rem_a,
                              uint32_t rem_b)
{
    MdX86 ca = a->cpu;
    MdX86 cb = b->cpu;

    if (rem_a != rem_b) return 1u;
    if (memcmp(a->cpu.r, b->cpu.r, sizeof(a->cpu.r)) != 0) return 2u;
    if (a->cpu.cs != b->cpu.cs ||
        a->cpu.ds != b->cpu.ds ||
        a->cpu.es != b->cpu.es ||
        a->cpu.ss != b->cpu.ss ||
        a->cpu.ip != b->cpu.ip) return 3u;
    if (md_x86_flags(&ca) != md_x86_flags(&cb)) return 4u;
    if (a->stop_reason != b->stop_reason) return 5u;
    if (memcmp(a->cpu.memory, b->cpu.memory, MD_X86_ADDRESS_SPACE) != 0)
        return 6u;
    return 0u;
}

static void dump_mismatch(const char *name,
                          unsigned kind,
                          uint32_t budget,
                          uint32_t rem_a,
                          uint32_t rem_b,
                          const MdRuntime *a,
                          const MdRuntime *b)
{
    MdX86 ca = a->cpu;
    MdX86 cb = b->cpu;
    unsigned i;

    printf("MISMATCH %s kind=%u budget=%u remaining=%u/%u\n",
           name, kind, budget, rem_a, rem_b);
    printf("  interp ip=%04X flags=%04X regs",
           a->cpu.ip, md_x86_flags(&ca));
    for (i = 0u; i < 8u; ++i) printf(" %04X", a->cpu.r[i]);
    printf("\n");
    printf("  nv2g   ip=%04X flags=%04X regs",
           b->cpu.ip, md_x86_flags(&cb));
    for (i = 0u; i < 8u; ++i) printf(" %04X", b->cpu.r[i]);
    printf("\n");
}

static int compile_region(const uint8_t *program,
                          size_t program_size,
                          MdNativeV2Code *code)
{
    size_t guest_size = 0u;
    const MdNativeV2Status st =
        md_native_v2g_compile_loop(
            program,
            program_size,
            TEST_IP,
            code,
            &guest_size);

    if (st != MD_NATIVE_V2_OK) {
        printf("compile reject: %s status=%d\n",
               program_size ? "program" : "empty",
               (int)st);
        return 0;
    }

    if (!md_native_v2g_is_code(code) ||
        code->dynamic_retire != 3u ||
        guest_size == 0u) {
        printf("compile produced invalid G metadata\n");
        return 0;
    }

#if defined(__GNUC__)
    __builtin___clear_cache(
        (char *)&code->bytes[0],
        (char *)&code->bytes[0] + code->size);
#endif

    return 1;
}

static unsigned run_compiled_case(const char *name,
                                  const uint8_t *program,
                                  size_t program_size,
                                  unsigned state_count,
                                  MdNativeV2Code *compiled,
                                  uint8_t *mem_a,
                                  uint8_t *mem_b)
{
    static const uint16_t kBudgets[] = {
        1u,2u,3u,4u,5u,6u,7u,8u,
        9u,10u,15u,16u,17u,31u,32u,33u,
        63u,64u,65u,127u,128u,129u,255u,256u
    };
    unsigned s, bi, fails = 0u;

    if (!compile_region(program, program_size, compiled)) {
        printf("[nv2g-diff] directed compile FAILED: %s\n", name);
        return 1u;
    }

    for (s = 0u; s < state_count; ++s) {
        for (bi = 0u; bi < sizeof(kBudgets)/sizeof(kBudgets[0]); ++bi) {
            MdRuntime a, b;
            uint32_t rem_a, rem_b;
            unsigned d;
            const uint32_t salt =
                (uint32_t)(s * 131u + bi * 17u + program_size);

            setup_runtime(&a, mem_a, program, program_size, salt);
            /*
             * Re-seed before B setup so both runtimes receive identical
             * randomized architectural state and data.
             */
            {
                const uint32_t saved = g_rng;
                g_rng ^= 0xA5A5A5A5u;
                setup_runtime(&b, mem_b, program, program_size, salt);
                /*
                 * setup_runtime consumes RNG. Copy the full architectural
                 * starting state/data from A to B to make identity explicit.
                 */
                memcpy(b.cpu.r, a.cpu.r, sizeof(a.cpu.r));
                b.cpu.es = a.cpu.es; b.cpu.cs = a.cpu.cs;
                b.cpu.ss = a.cpu.ss; b.cpu.ds = a.cpu.ds;
                b.cpu.ip = a.cpu.ip;
                b.cpu.flags_raw = a.cpu.flags_raw;
                b.cpu.lazy_op = a.cpu.lazy_op;
                b.cpu.lazy_a = a.cpu.lazy_a;
                b.cpu.lazy_b = a.cpu.lazy_b;
                b.cpu.lazy_res = a.cpu.lazy_res;
                b.cpu.lazy_carry = a.cpu.lazy_carry;
                memcpy(mem_b, mem_a, MD_X86_ADDRESS_SPACE);
                g_rng = saved;
            }

            rem_a = ref_run_region(&a, compiled, kBudgets[bi]);
            rem_b = md_native_v2g_execute(
                &b.cpu, compiled, kBudgets[bi]);

            if (rem_b == MD_NATIVE_V2_EXEC_FALLBACK) {
                printf("MISMATCH %s unexpected native fallback budget=%u\n",
                       name, (unsigned)kBudgets[bi]);
                ++fails;
                continue;
            }

            d = compare_state(&a, &b, rem_a, rem_b);
            if (d != 0u) {
                if (fails < 12u)
                    dump_mismatch(
                        name, d, kBudgets[bi],
                        rem_a, rem_b, &a, &b);
                ++fails;
            }
        }
    }

    printf("[nv2g-diff] directed %-14s states=%u runs=%u fails=%u ops=%u bytes=%u\n",
           name,
           state_count,
           state_count *
               (unsigned)(sizeof(kBudgets)/sizeof(kBudgets[0])),
           fails,
           (unsigned)compiled->op_count,
           (unsigned)compiled->size);

    return fails;
}

typedef struct ProgBuf {
    uint8_t b[64];
    unsigned n;
} ProgBuf;

static void pb(ProgBuf *p, unsigned v)
{
    if (p->n < sizeof(p->b))
        p->b[p->n++] = (uint8_t)v;
}

static void pw(ProgBuf *p, uint16_t v)
{
    pb(p, v & 0xffu);
    pb(p, v >> 8);
}

/*
 * Generate only instructions in the current G-1 accepted subset. Every
 * program ends in CMP r16,imm16 + a non-parity Jcc latch to the root.
 */
static void make_random_loop(ProgBuf *p)
{
    static const uint8_t cc[] = {
        0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,12u,13u,14u,15u
    };
    unsigned i;
    const unsigned body_ops = 1u + rn(6u);
    const unsigned cmp_reg = rn(8u);
    unsigned branch_at;
    int rel;

    p->n = 0u;

    for (i = 0u; i < body_ops; ++i) {
        const unsigned k = rn(6u);
        const unsigned r = rn(8u);
        const unsigned r2 = rn(8u);

        switch (k) {
            case 0u: /* mov r16,imm16 */
                pb(p, 0xB8u + r);
                pw(p, rng16());
                break;

            case 1u: /* add r16,imm8 */
                pb(p, 0x83u);
                pb(p, 0xC0u | r);
                pb(p, 1u + rn(31u));
                break;

            case 2u: /* xor r16,r16 */
                pb(p, 0x31u);
                pb(p, 0xC0u | (r2 << 3) | r);
                break;

            case 3u: /* mov r16,r16 */
                pb(p, 0x89u);
                pb(p, 0xC0u | (r2 << 3) | r);
                break;

            case 4u: /* byte load mov al,[si+disp8] */
                pb(p, 0x8Au);
                pb(p, 0x44u);
                pb(p, rn(16u));
                break;

            default: /* not r16 -- preserves flags */
                pb(p, 0xF7u);
                pb(p, 0xD0u | r);
                break;
        }
    }

    /* Final producer is always exact and branch-fusable. */
    pb(p, 0x81u);
    pb(p, 0xF8u | cmp_reg);
    pw(p, rng16());

    branch_at = p->n;
    pb(p, 0x70u + cc[rn((unsigned)(sizeof(cc)/sizeof(cc[0])))]);
    pb(p, 0u);

    rel = -(int)p->n;
    p->b[branch_at + 1u] = (uint8_t)rel;
}

static unsigned run_random(unsigned cases,
                           MdNativeV2Code *compiled,
                           uint8_t *mem_a,
                           uint8_t *mem_b)
{
    unsigned c, fails = 0u, compiled_cases = 0u;
    static const uint16_t budget_set[] = {
        1u,3u,7u,15u,31u,63u,127u,255u
    };

    for (c = 0u; c < cases; ++c) {
        ProgBuf p;
        MdRuntime a, b;
        uint32_t budget, rem_a, rem_b;
        unsigned d;
        size_t guest_size = 0u;
        MdNativeV2Status st;

        make_random_loop(&p);

        memset(compiled, 0, sizeof(*compiled));
        st = md_native_v2g_compile_loop(
            p.b, p.n, TEST_IP, compiled, &guest_size);

        if (st != MD_NATIVE_V2_OK) {
            printf("[nv2g-diff] random generator produced reject case=%u status=%d\n",
                   c, (int)st);
            ++fails;
            if (fails >= 20u) break;
            continue;
        }
        ++compiled_cases;

#if defined(__GNUC__)
        __builtin___clear_cache(
            (char *)&compiled->bytes[0],
            (char *)&compiled->bytes[0] + compiled->size);
#endif

        budget = budget_set[rn(
            (unsigned)(sizeof(budget_set)/sizeof(budget_set[0])))];

        setup_runtime(&a, mem_a, p.b, p.n, c + 0x1000u);
        setup_runtime(&b, mem_b, p.b, p.n, c + 0x1000u);

        memcpy(b.cpu.r, a.cpu.r, sizeof(a.cpu.r));
        b.cpu.es = a.cpu.es; b.cpu.cs = a.cpu.cs;
        b.cpu.ss = a.cpu.ss; b.cpu.ds = a.cpu.ds;
        b.cpu.ip = a.cpu.ip;
        b.cpu.flags_raw = a.cpu.flags_raw;
        b.cpu.lazy_op = a.cpu.lazy_op;
        b.cpu.lazy_a = a.cpu.lazy_a;
        b.cpu.lazy_b = a.cpu.lazy_b;
        b.cpu.lazy_res = a.cpu.lazy_res;
        b.cpu.lazy_carry = a.cpu.lazy_carry;
        memcpy(mem_b, mem_a, MD_X86_ADDRESS_SPACE);

        rem_a = ref_run_region(&a, compiled, budget);
        rem_b = md_native_v2g_execute(&b.cpu, compiled, budget);

        if (rem_b == MD_NATIVE_V2_EXEC_FALLBACK) {
            printf("[nv2g-diff] random unexpected fallback case=%u budget=%u\n",
                   c, budget);
            ++fails;
            continue;
        }

        d = compare_state(&a, &b, rem_a, rem_b);
        if (d != 0u) {
            if (fails < 12u)
                dump_mismatch("random", d, budget, rem_a, rem_b, &a, &b);
            ++fails;
        }

        if (((c + 1u) % 500u) == 0u) {
            printf("[nv2g-diff] random progress %u/%u fails=%u\n",
                   c + 1u, cases, fails);
            fflush(stdout);
        }
    }

    printf("[nv2g-diff] random cases=%u compiled=%u fails=%u seed=%08X\n",
           cases, compiled_cases, fails, g_rng);
    return fails;
}

int main(int argc, char **argv)
{
    unsigned directed_states = 64u;
    unsigned random_cases = 4000u;
    uint32_t seed = 0x4E563247u;
    uint8_t *mem_a = NULL;
    uint8_t *mem_b = NULL;
    MdNativeV2Code *compiled = NULL;
    unsigned fails = 0u;
    unsigned i;

    if (argc > 1) random_cases = (unsigned)strtoul(argv[1], NULL, 0);
    if (argc > 2) seed = (uint32_t)strtoul(argv[2], NULL, 0);
    if (argc > 3) directed_states = (unsigned)strtoul(argv[3], NULL, 0);
    g_rng = seed;

    mem_a = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    mem_b = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    if (mem_a == NULL || mem_b == NULL) {
        fprintf(stderr, "guest-memory allocation failed\n");
        return 2;
    }

#if defined(__linux__)
    {
        long page = sysconf(_SC_PAGESIZE);
        size_t bytes = sizeof(MdNativeV2Code);
        size_t rounded;
        if (page <= 0) page = 4096;
        rounded = (bytes + (size_t)page - 1u) &
                  ~((size_t)page - 1u);
        compiled = (MdNativeV2Code *)mmap(
            NULL, rounded,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1, 0);
        if (compiled == MAP_FAILED)
            compiled = NULL;
    }
#else
    compiled = (MdNativeV2Code *)malloc(sizeof(*compiled));
#endif

    if (compiled == NULL) {
        fprintf(stderr, "executable code allocation failed\n");
        return 2;
    }

    printf("== NV2-G qemu differential ==\n");
    printf("random_cases=%u seed=%08X directed_states=%u\n",
           random_cases, seed, directed_states);

    for (i = 0u; i < sizeof(kDirected)/sizeof(kDirected[0]); ++i) {
        memset(compiled, 0, sizeof(*compiled));
        fails += run_compiled_case(
            kDirected[i].name,
            kDirected[i].code,
            kDirected[i].size,
            directed_states,
            compiled,
            mem_a,
            mem_b);
    }

    fails += run_random(
        random_cases,
        compiled,
        mem_a,
        mem_b);

    printf("[nv2g-diff] RESULT %s mismatches=%u\n",
           fails ? "FAIL" : "PASS", fails);

    free(mem_a);
    free(mem_b);
    return fails ? 1 : 0;
}
