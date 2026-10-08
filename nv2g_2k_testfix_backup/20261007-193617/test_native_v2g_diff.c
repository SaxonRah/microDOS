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

/* G-2A: lodsb / or al,al / jnz loop. */
static const uint8_t kLodsb[] = {
    0xAC,
    0x0A,0xC0,
    0x75,0xFB
};

/* G-2A: producer before guarded STOSB, then counted latch. */
static const uint8_t kStosb[] = {
    0x04,0x01,
    0xAA,
    0xE2,0xFB
};

/* G-2A: general guarded MOV [BX+SI],AX. */
static const uint8_t kMovStore16[] = {
    0x05,0x01,0x00,
    0x89,0x00,
    0x83,0xC6,0x02,
    0xE2,0xF6
};

/* G-2A: register PUSH/POP with exact 8086 stack semantics. */
static const uint8_t kPushPop[] = {
    0x05,0x01,0x00,
    0x50,
    0x5B,
    0xE2,0xF9
};

/* G-2A: accumulator moffs store through the same generic store guard. */
static const uint8_t kMoffsStore[] = {
    0x05,0x01,0x00,
    0xA3,0x00,0x30,
    0xE2,0xF8
};

/*
 * G-2B0: the real DOS shapes. Every guarded op sits ahead of the iteration's
 * flag producer, so its guard is hoisted to the loop header. (The G-2A
 * fixtures above all insert an artificial producer first.)
 */
static const uint8_t kStrcpy[] = {        /* lodsb/stosb/or al,al/jnz */
    0xAC, 0xAA, 0x0A,0xC0, 0x75,0xFA
};
static const uint8_t kStosbPre[] = {      /* stosb/inc ax/cmp ax,dx/jne */
    0xAA, 0x40, 0x3B,0xC2, 0x75,0xFA
};
static const uint8_t kStoswPre[] = {      /* stosw/inc ax/cmp ax,dx/jne */
    0xAB, 0x40, 0x3B,0xC2, 0x75,0xFA
};
static const uint8_t kStore16Pre[] = {    /* mov [bx+si],ax/add si,2/cmp si,3040h/jne */
    0x89,0x00, 0x83,0xC6,0x02, 0x81,0xFE,0x40,0x30, 0x75,0xF5
};
static const uint8_t kStore8DispPre[] = { /* mov [di+7],al/inc di/cmp di,3040h/jne */
    0x88,0x45,0x07, 0x47, 0x81,0xFF,0x40,0x30, 0x75,0xF6
};
static const uint8_t kPushPre[] = {       /* push ax/inc ax/cmp ax,dx/jne */
    0x50, 0x40, 0x3B,0xC2, 0x75,0xFA
};
static const uint8_t kPopPre[] = {        /* pop bx/cmp bx,dx/jne */
    0x5B, 0x3B,0xDA, 0x75,0xFB
};
static const uint8_t kLodswPre[] = {      /* lodsw/cmp ax,dx/jne */
    0xAD, 0x3B,0xC2, 0x75,0xFB
};
static const uint8_t kMoffsPre[] = {      /* mov [3000h],ax/add ax,1/cmp ax,dx/jne */
    0xA3,0x00,0x30, 0x05,0x01,0x00, 0x3B,0xC2, 0x75,0xF6
};
static const uint8_t kStoswLoopnz[] = {   /* stosw/cmp ax,dx/loopnz */
    0xAB, 0x3B,0xC2, 0xE0,0xFB
};
/* Hoisted STOSB plus an ordinary (post-producer) guarded store. */
static const uint8_t kMixedStores[] = {   /* stosb/add bx,1/mov [bx],al/cmp bx,dx/jne */
    0xAA, 0x83,0xC3,0x01, 0x88,0x07, 0x3B,0xDA, 0x75,0xF6
};
/* Hoisted word load in a scanner with a side exit. */
static const uint8_t kLoadScanPre[] = {   /* mov ax,[si]/cmp ax,dx/je out/add si,2/cmp si,3040h/jne */
    0x8B,0x04, 0x3B,0xC2, 0x74,0x09, 0x83,0xC6,0x02,
    0x81,0xFE,0x40,0x30, 0x75,0xF1
};

static const Nv2gProgram kDirected[] = {
    { "scan8",       kScan8,       sizeof(kScan8)       },
    { "multi-exit",  kMultiExit,   sizeof(kMultiExit)   },
    { "signed-jl",   kSignedJl,    sizeof(kSignedJl)    },
    { "ds-disp-load",kDsDispLoad,  sizeof(kDsDispLoad)  },
    { "ss-disp-load",kSsDispLoad,  sizeof(kSsDispLoad)  },
    { "jcxz",        kJcxz,        sizeof(kJcxz)        },
    { "loopz",       kLoopz,       sizeof(kLoopz)       },
    { "loopnz",      kLoopnz,      sizeof(kLoopnz)      },
    { "lodsb",       kLodsb,       sizeof(kLodsb)       },
    { "stosb",       kStosb,       sizeof(kStosb)       },
    { "mov-store16", kMovStore16,  sizeof(kMovStore16)  },
    { "push-pop",    kPushPop,     sizeof(kPushPop)     },
    { "moffs-store", kMoffsStore,  sizeof(kMoffsStore)  },
    { "strcpy-lods-stos", kStrcpy,   sizeof(kStrcpy)      },
    { "stosb-pre",   kStosbPre,    sizeof(kStosbPre)    },
    { "stosw-pre",   kStoswPre,    sizeof(kStoswPre)    },
    { "store16-pre", kStore16Pre,  sizeof(kStore16Pre)  },
    { "store8-disp-pre", kStore8DispPre, sizeof(kStore8DispPre) },
    { "push-pre",    kPushPre,     sizeof(kPushPre)     },
    { "pop-pre",     kPopPre,      sizeof(kPopPre)      },
    { "lodsw-pre",   kLodswPre,    sizeof(kLodswPre)    },
    { "moffs-pre",   kMoffsPre,    sizeof(kMoffsPre)    },
    { "stosw-loopnz", kStoswLoopnz, sizeof(kStoswLoopnz) },
    { "mixed-stos-stores", kMixedStores, sizeof(kMixedStores) },
    { "load-scan-pre", kLoadScanPre, sizeof(kLoadScanPre) }
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

            /* G-2 single string fast path is intentionally DF==0 only. */
            if (strstr(name, "lods") != NULL || strstr(name, "stos") != NULL) {
                a.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
                b.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
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

    printf("[nv2g-diff] directed %-17s states=%u runs=%u fails=%u ops=%u bytes=%u\n",
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


static unsigned run_g2a_guard_checks(MdNativeV2Code *compiled,
                                     uint8_t *mem_a,
                                     uint8_t *mem_b)
{
    MdRuntime a, b;
    uint32_t rem_b;
    unsigned d;
    unsigned fails = 0u;
    size_t guest_size = 0u;
    uint8_t page_flags[MD_X86_CODE_PAGE_COUNT];

    /* DF==1 must reject single-string native entry without changing state. */
    memset(compiled, 0, sizeof(*compiled));
    if (md_native_v2g_compile_loop(
            kLodsb, sizeof(kLodsb), TEST_IP, compiled, &guest_size) !=
        MD_NATIVE_V2_OK) {
        printf("[nv2g-diff] G-2A DF guard compile FAILED\n");
        return 1u;
    }
    setup_runtime(&a, mem_a, kLodsb, sizeof(kLodsb), 0xDFA1u);
    setup_runtime(&b, mem_b, kLodsb, sizeof(kLodsb), 0xDFA1u);
    memcpy(b.cpu.r, a.cpu.r, sizeof(a.cpu.r));
    b.cpu.es=a.cpu.es; b.cpu.cs=a.cpu.cs; b.cpu.ss=a.cpu.ss; b.cpu.ds=a.cpu.ds;
    b.cpu.ip=a.cpu.ip; b.cpu.flags_raw=a.cpu.flags_raw;
    b.cpu.lazy_op=a.cpu.lazy_op; b.cpu.lazy_a=a.cpu.lazy_a; b.cpu.lazy_b=a.cpu.lazy_b;
    b.cpu.lazy_res=a.cpu.lazy_res; b.cpu.lazy_carry=a.cpu.lazy_carry;
    memcpy(mem_b, mem_a, MD_X86_ADDRESS_SPACE);
    a.cpu.flags_raw |= MD_X86_FLAG_DF;
    b.cpu.flags_raw |= MD_X86_FLAG_DF;
    rem_b = md_native_v2g_execute(&b.cpu, compiled, 32u);
    if (rem_b != MD_NATIVE_V2_EXEC_FALLBACK ||
        compare_state(&a, &b, 32u, 32u) != 0u) {
        printf("[nv2g-diff] G-2A DF guard FAILED\n");
        ++fails;
    } else {
        printf("[nv2g-diff] G-2A DF guard PASS\n");
    }

    /*
     * Own-code store: native executes the preceding ADD, then exits before
     * MOV [BX+SI],AX. The reference performs exactly that ADD and stops at
     * the store IP.
     */
    memset(compiled, 0, sizeof(*compiled));
    guest_size = 0u;
    if (md_native_v2g_compile_loop(
            kMovStore16, sizeof(kMovStore16), TEST_IP,
            compiled, &guest_size) != MD_NATIVE_V2_OK) {
        printf("[nv2g-diff] G-2A store guard compile FAILED\n");
        return fails + 1u;
    }
#if defined(__GNUC__)
    __builtin___clear_cache(
        (char *)&compiled->bytes[0],
        (char *)&compiled->bytes[0] + compiled->size);
#endif

    setup_runtime(&a, mem_a, kMovStore16, sizeof(kMovStore16), 0xC0DEu);
    setup_runtime(&b, mem_b, kMovStore16, sizeof(kMovStore16), 0xC0DEu);
    memcpy(b.cpu.r, a.cpu.r, sizeof(a.cpu.r));
    b.cpu.es=a.cpu.es; b.cpu.cs=a.cpu.cs; b.cpu.ss=a.cpu.ss; b.cpu.ds=a.cpu.ds;
    b.cpu.ip=a.cpu.ip; b.cpu.flags_raw=a.cpu.flags_raw;
    b.cpu.lazy_op=a.cpu.lazy_op; b.cpu.lazy_a=a.cpu.lazy_a; b.cpu.lazy_b=a.cpu.lazy_b;
    b.cpu.lazy_res=a.cpu.lazy_res; b.cpu.lazy_carry=a.cpu.lazy_carry;
    memcpy(mem_b, mem_a, MD_X86_ADDRESS_SPACE);
    a.cpu.r[MD_X86_BX] = b.cpu.r[MD_X86_BX] = TEST_IP;
    a.cpu.r[MD_X86_SI] = b.cpu.r[MD_X86_SI] = 0u;
    a.cpu.r[MD_X86_AX] = b.cpu.r[MD_X86_AX] = 0x1111u;
    (void)md_interp_step(&a);
    rem_b = md_native_v2g_execute(&b.cpu, compiled, 32u);
    d = compare_state(&a, &b, 31u, rem_b);
    if (d != 0u) {
        printf("[nv2g-diff] G-2A own-code store guard FAILED kind=%u rem=%u\n",
               d, rem_b);
        ++fails;
    } else {
        printf("[nv2g-diff] G-2A own-code store guard PASS\n");
    }

    /* A nonzero tracked-page flag must produce the same pre-store exit. */
    setup_runtime(&a, mem_a, kMovStore16, sizeof(kMovStore16), 0x7A6Bu);
    setup_runtime(&b, mem_b, kMovStore16, sizeof(kMovStore16), 0x7A6Bu);
    memcpy(b.cpu.r, a.cpu.r, sizeof(a.cpu.r));
    b.cpu.es=a.cpu.es; b.cpu.cs=a.cpu.cs; b.cpu.ss=a.cpu.ss; b.cpu.ds=a.cpu.ds;
    b.cpu.ip=a.cpu.ip; b.cpu.flags_raw=a.cpu.flags_raw;
    b.cpu.lazy_op=a.cpu.lazy_op; b.cpu.lazy_a=a.cpu.lazy_a; b.cpu.lazy_b=a.cpu.lazy_b;
    b.cpu.lazy_res=a.cpu.lazy_res; b.cpu.lazy_carry=a.cpu.lazy_carry;
    memcpy(mem_b, mem_a, MD_X86_ADDRESS_SPACE);
    a.cpu.r[MD_X86_BX] = b.cpu.r[MD_X86_BX] = DATA_OFF;
    a.cpu.r[MD_X86_SI] = b.cpu.r[MD_X86_SI] = 0u;
    a.cpu.r[MD_X86_AX] = b.cpu.r[MD_X86_AX] = 0x2222u;
    memset(page_flags, 0, sizeof(page_flags));
    page_flags[md_x86_code_page(md_x86_linear(TEST_SEG, DATA_OFF))] = 1u;
    b.cpu.code_page_executable = page_flags;
    (void)md_interp_step(&a);
    rem_b = md_native_v2g_execute(&b.cpu, compiled, 32u);
    b.cpu.code_page_executable = NULL; /* not architectural state */
    d = compare_state(&a, &b, 31u, rem_b);
    if (d != 0u) {
        printf("[nv2g-diff] G-2A tracked-page store guard FAILED kind=%u rem=%u\n",
               d, rem_b);
        ++fails;
    } else {
        printf("[nv2g-diff] G-2A tracked-page store guard PASS\n");
    }

    return fails;
}

/* -------------------------------------------------------------------------
 * G-2B0 hoisted-guard firing checks.
 *
 * Prefix oracle: if native returns with `retired` instructions consumed, the
 * interpreter must reach the identical architectural state (registers, IP,
 * FLAGS, memory) after exactly `retired` steps. If native returns FALLBACK,
 * no state may have changed. Each case also asserts the expected retirement
 * so a guard that never fires (or fires early) is caught.
 * ------------------------------------------------------------------------- */

static void sync_runtime_b(const MdRuntime *a, MdRuntime *b,
                           const uint8_t *mem_a, uint8_t *mem_b)
{
    memcpy(b->cpu.r, a->cpu.r, sizeof(a->cpu.r));
    b->cpu.es = a->cpu.es; b->cpu.cs = a->cpu.cs;
    b->cpu.ss = a->cpu.ss; b->cpu.ds = a->cpu.ds;
    b->cpu.ip = a->cpu.ip;
    b->cpu.flags_raw = a->cpu.flags_raw;
    b->cpu.lazy_op = a->cpu.lazy_op;
    b->cpu.lazy_a = a->cpu.lazy_a;
    b->cpu.lazy_b = a->cpu.lazy_b;
    b->cpu.lazy_res = a->cpu.lazy_res;
    b->cpu.lazy_carry = a->cpu.lazy_carry;
    memcpy(mem_b, mem_a, MD_X86_ADDRESS_SPACE);
}

static void put8(MdX86 *c, uint16_t seg, uint16_t off, uint8_t v)
{
    c->memory[md_x86_linear(seg, off)] = v;
}

static void prep_strcpy_owncode_it3(MdX86 *c)
{   /* iterations 1,2 store at 00FE/00FF; iteration 3 would hit 0100 (code). */
    unsigned i;
    for (i = 0u; i < 16u; ++i) put8(c, TEST_SEG, (uint16_t)(0x3100u + i), 0x41u);
    c->r[MD_X86_SI] = 0x3100u;
    c->r[MD_X86_DI] = (uint16_t)(TEST_IP - 2u);
}
static void prep_strcpy_owncode_it1(MdX86 *c)
{
    prep_strcpy_owncode_it3(c);
    c->r[MD_X86_DI] = (uint16_t)(TEST_IP + 1u);
}
static void prep_strcpy_tracked_it3(MdX86 *c)
{   /* iteration 3 store reaches linear 14000h (flagged page). */
    prep_strcpy_owncode_it3(c);
    c->r[MD_X86_DI] = 0x3FFEu;
}
static void prep_stosw_wrap_it2(MdX86 *c)
{
    c->r[MD_X86_DI] = 0xFFFDu;
    c->r[MD_X86_DX] = (uint16_t)(c->r[MD_X86_AX] + 0x100u);
}
static void prep_stosw_wrap_it1(MdX86 *c)
{
    prep_stosw_wrap_it2(c);
    c->r[MD_X86_DI] = 0xFFFFu;
}
static void prep_push_wrap_it2(MdX86 *c)
{
    c->r[MD_X86_SP] = 0x0003u;
    c->r[MD_X86_DX] = (uint16_t)(c->r[MD_X86_AX] + 0x100u);
}
static void prep_pop_wrap_it2(MdX86 *c)
{
    c->r[MD_X86_SP] = 0xFFFDu;
    put8(c, TEST_SEG, 0xFFFDu, 0x34u);
    put8(c, TEST_SEG, 0xFFFEu, 0x12u);
    c->r[MD_X86_DX] = 0x5555u;
}
static void prep_lodsw_wrap_it2(MdX86 *c)
{
    c->r[MD_X86_SI] = 0xFFFDu;
    put8(c, TEST_SEG, 0xFFFDu, 0x34u);
    put8(c, TEST_SEG, 0xFFFEu, 0x12u);
    c->r[MD_X86_DX] = 0x5555u;
}
static void prep_mixed_inner_guard(MdX86 *c)
{   /* hoisted STOSB stays safe; the post-producer MOV [BX] hits code on it3. */
    c->r[MD_X86_DI] = 0x3000u;
    c->r[MD_X86_BX] = (uint16_t)(TEST_IP - 3u);
    c->r[MD_X86_DX] = 0x7777u;
}

typedef struct PrefixCase {
    const char *name;
    const uint8_t *code;
    size_t size;
    void (*prep)(MdX86 *);
    long tracked_lin;          /* -1: no tracker */
    int expect_fallback;
    uint32_t expect_retired;
} PrefixCase;

static const PrefixCase kPrefixCases[] = {
    { "strcpy own-code it3",  kStrcpy, sizeof(kStrcpy),
      prep_strcpy_owncode_it3, -1, 0, 8u },
    { "strcpy own-code it1",  kStrcpy, sizeof(kStrcpy),
      prep_strcpy_owncode_it1, -1, 1, 0u },
    { "strcpy tracked it3",   kStrcpy, sizeof(kStrcpy),
      prep_strcpy_tracked_it3, 0x14000L, 0, 8u },
    { "stosw wrap it2",       kStoswPre, sizeof(kStoswPre),
      prep_stosw_wrap_it2, -1, 0, 4u },
    { "stosw wrap it1",       kStoswPre, sizeof(kStoswPre),
      prep_stosw_wrap_it1, -1, 1, 0u },
    { "push wrap it2",        kPushPre, sizeof(kPushPre),
      prep_push_wrap_it2, -1, 0, 4u },
    { "pop wrap it2",         kPopPre, sizeof(kPopPre),
      prep_pop_wrap_it2, -1, 0, 3u },
    { "lodsw wrap it2",       kLodswPre, sizeof(kLodswPre),
      prep_lodsw_wrap_it2, -1, 0, 3u },
    { "mixed inner guard it3", kMixedStores, sizeof(kMixedStores),
      prep_mixed_inner_guard, -1, 0, 12u }
};

static unsigned run_g2b0_guard_checks(MdNativeV2Code *compiled,
                                      uint8_t *mem_a,
                                      uint8_t *mem_b)
{
    static uint8_t page_flags[MD_X86_CODE_PAGE_COUNT];
    unsigned ci, fails = 0u;
    const uint32_t budget = 64u;

    for (ci = 0u; ci < sizeof(kPrefixCases)/sizeof(kPrefixCases[0]); ++ci) {
        const PrefixCase *pc = &kPrefixCases[ci];
        MdRuntime a, b;
        uint32_t rem, retired = 0u, k;
        unsigned d;
        int ok = 1;

        memset(compiled, 0, sizeof(*compiled));
        if (!compile_region(pc->code, pc->size, compiled)) {
            printf("[nv2g-diff] G-2B0 %s compile FAILED\n", pc->name);
            ++fails;
            continue;
        }

        setup_runtime(&a, mem_a, pc->code, pc->size, 0x2B00u + ci);
        setup_runtime(&b, mem_b, pc->code, pc->size, 0x2B00u + ci);
        sync_runtime_b(&a, &b, mem_a, mem_b);
        a.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
        b.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
        pc->prep(&a.cpu);
        pc->prep(&b.cpu);

        if (pc->tracked_lin >= 0) {
            memset(page_flags, 0, sizeof(page_flags));
            page_flags[md_x86_code_page((uint32_t)pc->tracked_lin)] = 1u;
            b.cpu.code_page_executable = page_flags;
        }
        rem = md_native_v2g_execute(&b.cpu, compiled, budget);
        b.cpu.code_page_executable = NULL; /* not architectural state */

        if (rem == MD_NATIVE_V2_EXEC_FALLBACK) {
            d = compare_state(&a, &b, 0u, 0u);
            ok = pc->expect_fallback && d == 0u;
        } else {
            retired = budget - rem;
            for (k = 0u; k < retired; ++k)
                (void)md_interp_step(&a);
            d = compare_state(&a, &b, rem, rem);
            ok = !pc->expect_fallback && d == 0u &&
                 retired == pc->expect_retired;
        }

        if (!ok) {
            printf("[nv2g-diff] G-2B0 %s FAILED kind=%u fallback=%d retired=%u expect=%u\n",
                   pc->name, d, rem == MD_NATIVE_V2_EXEC_FALLBACK,
                   (unsigned)retired, (unsigned)pc->expect_retired);
            dump_mismatch(pc->name, d, budget, rem, rem, &a, &b);
            ++fails;
        } else {
            printf("[nv2g-diff] G-2B0 %-24s PASS (%s)\n", pc->name,
                   rem == MD_NATIVE_V2_EXEC_FALLBACK ? "entry fallback" :
                   "exact exit");
        }
    }
    return fails;
}

/* -------------------------------------------------------------------------
 * G-2B0 random memory loops: stores, string ops and stack ops ahead of the
 * final producer, in the real DOS order. Pointers start in a safe window, so
 * no guard fires and the strict reference schedule applies. Loops whose
 * guards cannot be hoisted (e.g. two STOSB in one body) legitimately reject;
 * those are counted, and a minimum compile ratio is enforced.
 * ------------------------------------------------------------------------- */

static void make_random_mem_loop(ProgBuf *p)
{
    static const uint8_t cc[] = {
        0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,12u,13u,14u,15u
    };
    static const uint8_t data_reg[] = { MD_X86_AX, MD_X86_CX, MD_X86_DX };
    unsigned i, branch_at;
    const unsigned body_ops = 1u + rn(6u);
    int rel;

    p->n = 0u;
    for (i = 0u; i < body_ops; ++i) {
        const unsigned r = data_reg[rn(3u)];
        const unsigned r4 = rn(4u); /* AX CX DX BX as store source */
        switch (rn(12u)) {
            case 0u: pb(p, 0xB8u + r); pw(p, rng16()); break;       /* mov r,imm */
            case 1u: pb(p, 0x83u); pb(p, 0xC0u | r);                 /* add r,imm8 */
                     pb(p, 1u + rn(31u)); break;
            case 2u: pb(p, 0x89u); pb(p, 0x44u | (r4 << 3));          /* mov [si+d8],r16 */
                     pb(p, rn(0x40u)); break;
            case 3u: pb(p, 0x88u); pb(p, 0x01u | (r4 << 3)); break;   /* mov [bx+di],r8 */
            case 4u: pb(p, 0xAAu); break;                            /* stosb */
            case 5u: pb(p, 0xABu); break;                            /* stosw */
            case 6u: pb(p, 0xACu); break;                            /* lodsb */
            case 7u: pb(p, 0xADu); break;                            /* lodsw */
            case 8u: pb(p, 0x50u + r4); break;                       /* push r */
            case 9u: pb(p, 0x58u + r); break;                        /* pop r */
            case 10u: pb(p, 0x8Bu); pb(p, 0x02u | (r << 3)); break;  /* mov r,[bp+si] */
            default: pb(p, 0xA3u); pw(p, (uint16_t)(0x3100u + rn(0x40u))); break; /* moffs */
        }
    }
    pb(p, 0x81u);                                   /* cmp r16,imm16 */
    pb(p, 0xF8u | rn(8u));
    pw(p, rng16());
    branch_at = p->n;
    pb(p, 0x70u + cc[rn((unsigned)(sizeof(cc)/sizeof(cc[0])))]);
    pb(p, 0u);
    rel = -(int)p->n;
    p->b[branch_at + 1u] = (uint8_t)rel;
}

static unsigned run_random_mem(unsigned cases,
                               MdNativeV2Code *compiled,
                               uint8_t *mem_a,
                               uint8_t *mem_b)
{
    static const uint16_t budget_set[] = {
        1u,3u,7u,15u,31u,63u,127u,255u
    };
    unsigned c, fails = 0u, compiled_cases = 0u, rejected = 0u;

    for (c = 0u; c < cases; ++c) {
        ProgBuf p;
        MdRuntime a, b;
        uint32_t budget, rem_a, rem_b;
        unsigned d;
        size_t guest_size = 0u;

        make_random_mem_loop(&p);
        memset(compiled, 0, sizeof(*compiled));
        if (md_native_v2g_compile_loop(p.b, p.n, TEST_IP, compiled,
                                       &guest_size) != MD_NATIVE_V2_OK) {
            ++rejected;
            continue;
        }
        ++compiled_cases;
#if defined(__GNUC__)
        __builtin___clear_cache((char *)&compiled->bytes[0],
                                (char *)&compiled->bytes[0] + compiled->size);
#endif
        budget = budget_set[rn((unsigned)(sizeof(budget_set)/sizeof(budget_set[0])))];
        setup_runtime(&a, mem_a, p.b, p.n, c * 7u + 3u);
        setup_runtime(&b, mem_b, p.b, p.n, c * 7u + 3u);
        sync_runtime_b(&a, &b, mem_a, mem_b);
        a.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
        b.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;

        rem_a = ref_run_region(&a, compiled, budget);
        rem_b = md_native_v2g_execute(&b.cpu, compiled, budget);
        if (rem_b == MD_NATIVE_V2_EXEC_FALLBACK) {
            printf("[nv2g-diff] random-mem unexpected fallback case=%u\n", c);
            ++fails;
        } else {
            d = compare_state(&a, &b, rem_a, rem_b);
            if (d != 0u) {
                if (fails < 12u) {
                    unsigned j;
                    printf("[nv2g-diff] random-mem case=%u bytes:", c);
                    for (j = 0u; j < p.n; ++j) printf(" %02X", p.b[j]);
                    printf("\n");
                    dump_mismatch("random-mem", d, budget, rem_a, rem_b, &a, &b);
                }
                ++fails;
            }
        }
        if (fails >= 20u) break;
    }

    printf("[nv2g-diff] random-mem cases=%u compiled=%u rejected=%u fails=%u\n",
           cases, compiled_cases, rejected, fails);
    if (cases >= 100u && compiled_cases * 2u < cases) {
        printf("[nv2g-diff] random-mem compile ratio too low (%u/%u)\n",
               compiled_cases, cases);
        ++fails;
    }
    return fails;
}

/* -------------------------------------------------------------------------
 * G-2B1: native lazy materialization (NLM), carried FLAGS, MOV r/m,imm,
 * INC/DEC CF chains and the compact layout, on the real loops that the
 * NV2-G hot-loop census ranked highest in MASM / FIND (MS-DOS 2.0 tools).
 * ------------------------------------------------------------------------- */

static const uint8_t kMasmFill[] = {      /* MASM 14DE:1BA6 */
    0x8B,0x3E,0x7E,0x03, 0xC6,0x85,0xD0,0x17,0x20, 0xA1,0x7E,0x03, 0x40,
    0xA3,0x7E,0x03, 0x3D,0x20,0x00, 0x75,0xEB
};
static const uint8_t kMasmSwap[] = {      /* MASM 14DE:1CF2 (compact layout) */
    0x8B,0x7E,0xFC, 0x8B,0x95,0x89,0x01, 0x88,0x56,0xFA, 0x8B,0x95,0xD0,0x17,
    0x88,0x95,0x89,0x01, 0x8B,0x56,0xFA, 0x88,0x95,0xD0,0x17, 0x8B,0x56,0xFC,
    0x42, 0x89,0x56,0xFC, 0x83,0xFA,0x20, 0x75,0xDB
};
static const uint8_t kMasmGather[] = {    /* MASM 14DE:BA60 (INC->DEC CF chain) */
    0x8B,0x7E,0xF8, 0x8B,0x7D,0x04, 0x03,0x3E,0x7E,0x03, 0x8B,0x15,
    0x8B,0x3E,0x7E,0x03, 0x88,0x95,0x05,0x03, 0xA1,0x7E,0x03, 0x40,
    0xA3,0x7E,0x03, 0x48, 0x3B,0x46,0xF6, 0x75,0xDF
};
static const uint8_t kMasmScatter[] = {   /* MASM 14DE:128B */
    0x8B,0x3E,0x7E,0x03, 0x8B,0x95,0xD0,0x17, 0x8B,0x76,0x00, 0x03,0x3C,
    0x88,0x15, 0xA1,0x7E,0x03, 0x40, 0xA3,0x7E,0x03, 0x48, 0x3B,0x46,0xFA,
    0x75,0xE4
};
static const uint8_t kScanMem[] = {       /* 217D:027A cmp ax,[si]/je/add si,4/loopne */
    0x3B,0x04, 0x74,0x06, 0x83,0xC6,0x04, 0xE0,0xF7
};
static const uint8_t kJcxzCarried[] = {   /* jcxz end/sub cx,1/cmp ax,1234h/jne */
    0xE3,0x08, 0x83,0xE9,0x01, 0x3D,0x34,0x12, 0x75,0xF6
};
static const uint8_t kIncExitCarried[] = { /* inc si/mov [si],al/cmp si,dx/jne */
    0x46, 0x88,0x04, 0x3B,0xF2, 0x75,0xF9
};
static const uint8_t kExtJmp[] = {        /* cmp al,[si]/je +3/inc si/jmp out... */
    0x3A,0x04,             /* cmp al,[si]   */
    0x74,0x03,             /* je  0107      */
    0x46,                  /* inc si        */
    0xEB,0x05,             /* jmp 010C (external) */
    0x83,0xC6,0x02,        /* 0107: add si,2 */
    0x75,0xF4              /* jne 0100      */
};

typedef struct Nv2gPrefixProg { const char *name; const uint8_t *code; size_t size; } Nv2gPrefixProg;
static const Nv2gPrefixProg kG2b1Programs[] = {
    { "masm-fill",       kMasmFill,       sizeof(kMasmFill)       },
    { "masm-swap",       kMasmSwap,       sizeof(kMasmSwap)       },
    { "masm-gather",     kMasmGather,     sizeof(kMasmGather)     },
    { "masm-scatter",    kMasmScatter,    sizeof(kMasmScatter)    },
    { "scan-mem-loopne", kScanMem,        sizeof(kScanMem)        },
    { "jcxz-carried",    kJcxzCarried,    sizeof(kJcxzCarried)    },
    { "inc-exit-carried", kIncExitCarried, sizeof(kIncExitCarried) },
    { "ext-jmp",         kExtJmp,         sizeof(kExtJmp)         }
};

/*
 * Prefix-oracle run: native result must equal the interpreter after exactly
 * the instructions native retired; FALLBACK must leave state untouched.
 * Returns 1 on mismatch. *retired_out receives native retirement.
 */
static unsigned prefix_check(const char *name, MdRuntime *a, MdRuntime *b,
                             const MdNativeV2Code *code, uint32_t budget,
                             uint32_t *retired_out, int quiet)
{
    uint32_t rem, retired, k;
    unsigned d;
    rem = md_native_v2g_execute(&b->cpu, code, budget);
    if (rem == MD_NATIVE_V2_EXEC_FALLBACK) {
        *retired_out = 0u;
        d = compare_state(a, b, 0u, 0u);
    } else {
        if (rem > budget) { printf("MISMATCH %s rem>budget\n", name); return 1u; }
        retired = budget - rem;
        *retired_out = retired;
        for (k = 0u; k < retired; ++k) (void)md_interp_step(a);
        d = compare_state(a, b, rem, rem);
    }
    if (d != 0u && !quiet)
        dump_mismatch(name, d, budget, 0u, 0u, a, b);
    return d != 0u;
}

static unsigned run_g2b1_directed(unsigned states, MdNativeV2Code *compiled,
                                  uint8_t *mem_a, uint8_t *mem_b)
{
    static const uint16_t kB[] = { 1u,3u,7u,15u,16u,31u,64u,127u,255u };
    unsigned pi, s, bi, fails = 0u;
    for (pi = 0u; pi < sizeof(kG2b1Programs)/sizeof(kG2b1Programs[0]); ++pi) {
        const Nv2gPrefixProg *pg = &kG2b1Programs[pi];
        unsigned pf = 0u, runs = 0u;
        uint64_t native = 0u, offered = 0u;
        if (!compile_region(pg->code, pg->size, compiled)) {
            printf("[nv2g-diff] G-2B1 %s compile FAILED\n", pg->name);
            ++fails; continue;
        }
        for (s = 0u; s < states; ++s) {
            for (bi = 0u; bi < sizeof(kB)/sizeof(kB[0]); ++bi) {
                MdRuntime a, b;
                uint32_t got = 0u;
                const uint32_t salt = 0xB100u + pi * 977u + s * 31u + bi;
                setup_runtime(&a, mem_a, pg->code, pg->size, salt);
                setup_runtime(&b, mem_b, pg->code, pg->size, salt);
                sync_runtime_b(&a, &b, mem_a, mem_b);
                /* Pascal-style frame/counter cells: small counters so loops
                   run, BP-relative pointers random (guards fire often). */
                a.cpu.memory[md_x86_linear(TEST_SEG, 0x037Eu)] = (uint8_t)rn(0x20u);
                a.cpu.memory[md_x86_linear(TEST_SEG, 0x037Fu)] = (uint8_t)(rn(4u) == 0u ? 0xE9u : 0u);
                if ((s & 3u) == 0u) a.cpu.r[MD_X86_CX] = (uint16_t)(1u + rn(4u));
                a.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
                sync_runtime_b(&a, &b, mem_a, mem_b);
                pf += prefix_check(pg->name, &a, &b, compiled, kB[bi], &got, pf >= 4u);
                native += got; offered += kB[bi]; ++runs;
            }
        }
        printf("[nv2g-diff] G-2B1 %-17s runs=%u fails=%u native=%.0f%% bytes=%u%s\n",
               pg->name, runs, pf, offered ? 100.0 * (double)native / (double)offered : 0.0,
               (unsigned)compiled->size,
               ((const uint8_t *)compiled->g_meta)[9] ? " (compact)" : "");
        if (native == 0u) { printf("[nv2g-diff] G-2B1 %s never ran natively\n", pg->name); ++pf; }
        fails += pf;
    }
    return fails;
}

static void prep_masm_fill_it3(MdX86 *c)
{   /* DI=[037E]; byte store at DI+17D0: 00FE, 00FF, then 0100 = own code. */
    c->memory[md_x86_linear(TEST_SEG, 0x037Eu)] = 0x2Eu;
    c->memory[md_x86_linear(TEST_SEG, 0x037Fu)] = 0xE9u;
}
static void prep_masm_fill_it1(MdX86 *c)
{
    c->memory[md_x86_linear(TEST_SEG, 0x037Eu)] = 0x30u;
    c->memory[md_x86_linear(TEST_SEG, 0x037Fu)] = 0xE9u;
}
static void prep_jcxz_carried(MdX86 *c)
{
    c->r[MD_X86_CX] = 2u;
    c->r[MD_X86_AX] = 0x4321u;
}
static void prep_inc_exit_it3(MdX86 *c)
{
    c->r[MD_X86_SI] = (uint16_t)(TEST_IP - 3u);
    c->r[MD_X86_DX] = 0x5555u;
}
static void prep_scan_mem_hit3(MdX86 *c)
{
    c->r[MD_X86_SI] = 0x3100u;
    c->r[MD_X86_CX] = 10u;
    c->r[MD_X86_AX] = 0xBEEFu;
    c->memory[md_x86_linear(TEST_SEG, 0x3100u)] = 0x00u;
    c->memory[md_x86_linear(TEST_SEG, 0x3104u)] = 0x00u;
    c->memory[md_x86_linear(TEST_SEG, 0x3108u)] = 0xEFu;
    c->memory[md_x86_linear(TEST_SEG, 0x3109u)] = 0xBEu;
}

static const PrefixCase kG2b1Cases[] = {
    { "masm-fill carried it3",  kMasmFill, sizeof(kMasmFill),
      prep_masm_fill_it3, -1, 0, 15u },
    { "masm-fill guard it1",    kMasmFill, sizeof(kMasmFill),
      prep_masm_fill_it1, -1, 0, 1u },
    { "jcxz carried it3",       kJcxzCarried, sizeof(kJcxzCarried),
      prep_jcxz_carried, -1, 0, 9u },
    { "inc-exit CF carried it3", kIncExitCarried, sizeof(kIncExitCarried),
      prep_inc_exit_it3, -1, 0, 9u },
    { "cmp-mem je exit it3",    kScanMem, sizeof(kScanMem),
      prep_scan_mem_hit3, -1, 0, 10u },
    /* Compact layout carries no inline tracked-page check: entry refused. */
    { "compact + tracker",      kMasmSwap, sizeof(kMasmSwap),
      prep_jcxz_carried, 0x30000L, 1, 0u }
};

static unsigned run_g2b1_guard_checks(MdNativeV2Code *compiled,
                                      uint8_t *mem_a, uint8_t *mem_b)
{
    static uint8_t page_flags[MD_X86_CODE_PAGE_COUNT];
    unsigned ci, fails = 0u;
    const uint32_t budget = 64u;
    for (ci = 0u; ci < sizeof(kG2b1Cases)/sizeof(kG2b1Cases[0]); ++ci) {
        const PrefixCase *pc = &kG2b1Cases[ci];
        MdRuntime a, b;
        uint32_t rem, retired = 0u, k;
        unsigned d;
        int ok;
        memset(compiled, 0, sizeof(*compiled));
        if (!compile_region(pc->code, pc->size, compiled)) {
            printf("[nv2g-diff] G-2B1 %s compile FAILED\n", pc->name);
            ++fails; continue;
        }
        setup_runtime(&a, mem_a, pc->code, pc->size, 0x2B10u + ci);
        setup_runtime(&b, mem_b, pc->code, pc->size, 0x2B10u + ci);
        sync_runtime_b(&a, &b, mem_a, mem_b);
        a.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
        pc->prep(&a.cpu);
        sync_runtime_b(&a, &b, mem_a, mem_b);
        if (pc->tracked_lin >= 0) {
            memset(page_flags, 0, sizeof(page_flags));
            page_flags[md_x86_code_page((uint32_t)pc->tracked_lin)] = 1u;
            b.cpu.code_page_executable = page_flags;
        }
        rem = md_native_v2g_execute(&b.cpu, compiled, budget);
        b.cpu.code_page_executable = NULL;
        if (rem == MD_NATIVE_V2_EXEC_FALLBACK) {
            d = compare_state(&a, &b, 0u, 0u);
            ok = pc->expect_fallback && d == 0u;
        } else {
            retired = budget - rem;
            for (k = 0u; k < retired; ++k) (void)md_interp_step(&a);
            d = compare_state(&a, &b, rem, rem);
            ok = !pc->expect_fallback && d == 0u && retired == pc->expect_retired;
        }
        if (!ok) {
            printf("[nv2g-diff] G-2B1 %s FAILED kind=%u fallback=%d retired=%u expect=%u\n",
                   pc->name, d, rem == MD_NATIVE_V2_EXEC_FALLBACK,
                   (unsigned)retired, (unsigned)pc->expect_retired);
            dump_mismatch(pc->name, d, budget, rem, rem, &a, &b);
            ++fails;
        } else {
            printf("[nv2g-diff] G-2B1 %-24s PASS (%s)\n", pc->name,
                   rem == MD_NATIVE_V2_EXEC_FALLBACK ? "entry fallback" : "exact exit");
        }
    }
    return fails;
}

/*
 * G-2B1 random NLM loops: memory-operand producers, pointers reloaded from
 * random memory (stores land anywhere, including the loop's own code and
 * offset FFFF, so guards and carried-FLAGS exits fire constantly), MOV
 * r/m,imm, INC/DEC producers and CF chains, memory-operand latch producers.
 * Checked with the prefix oracle.
 */
static void make_random_nlm_loop(ProgBuf *p)
{
    static const uint8_t data_reg[] = { MD_X86_AX, MD_X86_CX, MD_X86_DX };
    static const uint8_t alu_rm16[] = { 0x03u, 0x2Bu, 0x3Bu, 0x23u, 0x0Bu, 0x33u };
    static const uint8_t alu_rm8[] = { 0x02u, 0x2Au, 0x3Au, 0x22u, 0x0Au, 0x32u };
    static const uint8_t cc_any[] = { 0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,12u,13u,14u,15u };
    static const uint8_t cc_incdec[] = { 0u,1u,4u,5u,8u,9u,12u,13u,14u,15u };
    unsigned i, branch_at;
    const unsigned body_ops = 1u + rn(7u);
    unsigned latch_kind;
    int rel;

    p->n = 0u;
    for (i = 0u; i < body_ops; ++i) {
        const unsigned r = data_reg[rn(3u)];
        const unsigned r8 = rn(8u) & 3u;   /* AL CL DL BL */
        switch (rn(13u)) {
            case 0u: pb(p, 0xB8u + r); pw(p, rng16()); break;
            case 1u: pb(p, alu_rm16[rn(6u)]); pb(p, 0x44u | (r << 3)); pb(p, rn(0x40u)); break;
            case 2u: pb(p, alu_rm8[rn(6u)]); pb(p, 0x01u | (r8 << 3)); break;   /* op r8,[bx+di] */
            case 3u: pb(p, 0x8Bu); pb(p, 0x7Eu); pb(p, rn(0x40u)); break;      /* mov di,[bp+d8] */
            case 4u: pb(p, 0x89u); pb(p, 0x45u | (r << 3)); pb(p, rn(0x40u)); break; /* mov [di+d8],r */
            case 5u: pb(p, 0xC6u); pb(p, 0x05u); pb(p, rn(256u)); break;        /* mov byte [di],imm */
            case 6u: pb(p, 0xC7u); pb(p, 0x45u); pb(p, rn(0x40u)); pw(p, rng16()); break;
            case 7u: pb(p, 0x40u + r); break;                                   /* inc r */
            case 8u: pb(p, 0x48u + r); break;                                   /* dec r */
            case 9u: pb(p, 0xAAu); break;                                       /* stosb */
            case 10u: pb(p, 0x81u); pb(p, 0xF8u | r); pw(p, rng16()); break;    /* cmp r,imm */
            case 11u: pb(p, 0x8Bu); pb(p, 0x3Eu); pw(p, 0x3000u + (uint16_t)(rn(0x40u) * 2u)); break; /* mov di,[moffs] */
            default: pb(p, 0x8Au); pb(p, 0x05u | (r8 << 3)); break;            /* mov r8,[di] */
        }
    }
    latch_kind = rn(4u);
    if (latch_kind == 0u) {
        pb(p, 0x81u); pb(p, 0xF8u | data_reg[rn(3u)]); pw(p, rng16());
    } else if (latch_kind == 1u) {
        pb(p, 0x3Bu); pb(p, 0x44u | (data_reg[rn(3u)] << 3)); pb(p, rn(0x40u));
    } else if (latch_kind == 2u) {
        pb(p, 0x49u);                                                           /* dec cx */
    } else {
        pb(p, 0x05u); pw(p, (uint16_t)(1u + rn(0x200u)));                        /* add ax,imm */
    }
    branch_at = p->n;
    if (latch_kind == 2u) pb(p, 0x70u + cc_incdec[rn((unsigned)sizeof(cc_incdec))]);
    else pb(p, 0x70u + cc_any[rn((unsigned)sizeof(cc_any))]);
    pb(p, 0u);
    rel = -(int)p->n;
    p->b[branch_at + 1u] = (uint8_t)rel;
}

static unsigned run_random_nlm(unsigned cases, MdNativeV2Code *compiled,
                               uint8_t *mem_a, uint8_t *mem_b)
{
    static const uint16_t budget_set[] = { 1u,3u,7u,15u,31u,63u,127u,255u };
    unsigned c, fails = 0u, compiled_cases = 0u, rejected = 0u, compact = 0u;
    uint64_t native = 0u, offered = 0u;
    for (c = 0u; c < cases; ++c) {
        ProgBuf p;
        MdRuntime a, b;
        size_t guest_size = 0u;
        uint32_t budget, got = 0u;
        make_random_nlm_loop(&p);
        memset(compiled, 0, sizeof(*compiled));
        if (md_native_v2g_compile_loop(p.b, p.n, TEST_IP, compiled,
                                       &guest_size) != MD_NATIVE_V2_OK) {
            ++rejected; continue;
        }
        ++compiled_cases;
        if (((const uint8_t *)compiled->g_meta)[9]) ++compact;
#if defined(__GNUC__)
        __builtin___clear_cache((char *)&compiled->bytes[0],
                                (char *)&compiled->bytes[0] + compiled->size);
#endif
        budget = budget_set[rn((unsigned)(sizeof(budget_set)/sizeof(budget_set[0])))];
        setup_runtime(&a, mem_a, p.b, p.n, c * 13u + 5u);
        setup_runtime(&b, mem_b, p.b, p.n, c * 13u + 5u);
        a.cpu.flags_raw &= (uint16_t)~MD_X86_FLAG_DF;
        if ((c & 3u) == 0u) a.cpu.r[MD_X86_DI] = (uint16_t)(TEST_IP - 2u + rn(8u));
        if ((c & 7u) == 1u) a.cpu.r[MD_X86_DI] = (uint16_t)(0xFFFCu + rn(4u));
        sync_runtime_b(&a, &b, mem_a, mem_b);
        if (prefix_check("random-nlm", &a, &b, compiled, budget, &got, fails >= 8u)) {
            if (fails < 8u) {
                unsigned j;
                printf("[nv2g-diff] random-nlm case=%u bytes:", c);
                for (j = 0u; j < p.n; ++j) printf(" %02X", p.b[j]);
                printf("\n");
            }
            ++fails;
        }
        native += got; offered += budget;
        if (fails >= 20u) break;
    }
    printf("[nv2g-diff] random-nlm cases=%u compiled=%u (compact %u) rejected=%u fails=%u native=%.0f%%\n",
           cases, compiled_cases, compact, rejected, fails,
           offered ? 100.0 * (double)native / (double)offered : 0.0);
    if (cases >= 100u && compiled_cases * 2u < cases) {
        printf("[nv2g-diff] random-nlm compile ratio too low\n");
        ++fails;
    }
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

    fails += run_g2a_guard_checks(compiled, mem_a, mem_b);
    fails += run_g2b0_guard_checks(compiled, mem_a, mem_b);
    fails += run_g2b1_directed(directed_states, compiled, mem_a, mem_b);
    fails += run_g2b1_guard_checks(compiled, mem_a, mem_b);

    fails += run_random(
        random_cases,
        compiled,
        mem_a,
        mem_b);

    fails += run_random_mem(
        random_cases,
        compiled,
        mem_a,
        mem_b);

    fails += run_random_nlm(random_cases, compiled, mem_a, mem_b);

    printf("[nv2g-diff] RESULT %s mismatches=%u\n",
           fails ? "FAIL" : "PASS", fails);

    free(mem_a);
    free(mem_b);
    return fails ? 1 : 0;
}
