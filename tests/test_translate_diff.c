/*
 * M25 translator differential test.
 *
 * Random 8086 programs (lowered and non-lowered instructions mixed, so block
 * boundaries, helper paths, side exits and chaining are all exercised) run
 * once through md_interp_run() and once through md_tr_run() from identical
 * state with identical budgets. Registers, segments, IP, the full FLAGS
 * word, retired-instruction count, stop reason and all guest memory must
 * match exactly.
 *
 * Needs a Thumb-2 host (RP2350, or qemu-arm on a desktop):
 *   arm-linux-gnueabihf-gcc -mthumb -O2 ... && qemu-arm ./test_translate_diff [cases] [seed] [unaligned]
 *
 * The Pico firmware (pico/microdos_translate_diff.c) includes this file with
 * MD_TRANSLATE_DIFF_NO_MAIN defined and calls md_translate_diff_run() itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "microdos/runtime.h"
#include "microdos/translate.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

#ifndef MD_TRANSLATE_DIFF_PROGRESS
#define MD_TRANSLATE_DIFF_PROGRESS 500u
#endif

static uint32_t g_rng;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
static unsigned rn(unsigned n) { return n ? rnd() % n : 0u; }

typedef struct Prog { uint8_t b[600]; unsigned n; } Prog;
static void put(Prog *p, unsigned v) { if (p->n < sizeof(p->b)) p->b[p->n++] = (uint8_t)v; }

/* memory ModR/M with a modest displacement; registers are kept pointing near
   the data area by the initial state, but nothing forbids wild addresses */
static void modrm_mem(Prog *p, unsigned reg)
{
    const unsigned rm = rn(8u);
    const unsigned mod = rn(3u);
    if (mod == 0u && rm == 6u) { put(p, (reg << 3) | 6u); put(p, 0x00); put(p, 0x30 + rn(0x10)); return; }
    put(p, (mod << 6) | (reg << 3) | rm);
    if (mod == 1u) put(p, rn(256));
    if (mod == 2u) { put(p, rn(256)); put(p, rn(4) == 0 ? 0xFF : rn(0x20)); }
}

static void modrm_any(Prog *p, unsigned reg)
{
    if (rn(2u)) put(p, 0xC0u | (reg << 3) | rn(8u));
    else modrm_mem(p, reg);
}

static void gen_insn(Prog *p, unsigned code_len_hint)
{
    const unsigned t = rn(40u);
    switch (t) {
        case 0: case 1: case 2: case 3:            /* ALU r/m,r both widths/dirs */
            put(p, (rn(8u) << 3) | rn(4u)); modrm_any(p, rn(8u)); break;
        case 4: case 5:                            /* ALU acc,imm */
            { const unsigned w = rn(2u); put(p, (rn(8u) << 3) | 4u | w); put(p, rn(256)); if (w) put(p, rn(256)); }
            break;
        case 6: case 7:                            /* group 1 */
            { const unsigned o = 0x80u + rn(4u); put(p, o); modrm_any(p, rn(8u)); put(p, rn(256)); if (o == 0x81u) put(p, rn(256)); }
            break;
        case 8: put(p, 0x84u + rn(2u)); modrm_any(p, rn(8u)); break;
        case 9: put(p, 0xA8u); put(p, rn(256)); break;
        case 10: case 11: put(p, 0x88u + rn(4u)); modrm_any(p, rn(8u)); break;
        case 12: put(p, 0xB0u + rn(8u)); put(p, rn(256)); break;
        case 13: put(p, 0xB8u + rn(8u)); put(p, rn(256)); put(p, rn(256)); break;
        case 14: case 15: put(p, 0x40u + rn(16u)); break;      /* INC/DEC r16 */
        case 16: put(p, 0xFEu + rn(2u)); modrm_any(p, rn(2u)); break;
        case 17: put(p, 0x50u + rn(8u)); break;
        case 18: put(p, 0x58u + rn(8u)); break;
        case 19: put(p, 0x90u + rn(8u)); break;                 /* NOP/XCHG */
        case 20: put(p, 0x98u + rn(2u)); break;                 /* CBW/CWD */
        case 21: put(p, 0x8Du); modrm_mem(p, rn(8u)); break;    /* LEA */
        case 22: case 23: case 24:                              /* Jcc short */
            put(p, 0x70u + rn(16u)); put(p, rn(3u) ? rn(12u) : (uint8_t)(-(int)rn(24u)));
            break;
        case 25: put(p, 0xE2u); put(p, (uint8_t)(-(int)rn(20u))); break;   /* LOOP back */
        case 26: put(p, 0xE3u); put(p, rn(10u)); break;
        case 27: put(p, 0xEBu); put(p, rn(8u)); break;
        case 28: {                                              /* CALL near */
            const unsigned target = rn(code_len_hint);
            const int rel = (int)target - (int)(p->n + 3u);
            put(p, 0xE8u); put(p, (unsigned)rel & 0xFFu); put(p, ((unsigned)rel >> 8) & 0xFFu);
            break;
        }
        case 29: put(p, 0xC3u); break;
        /* not lowered: flag readers/writers and others, exercising exits */
        case 30: put(p, 0xF8u + rn(2u)); break;                 /* CLC/STC */
        case 31: put(p, 0xF5u); break;                          /* CMC */
        case 32: put(p, 0x9Cu); break;                          /* PUSHF */
        case 33: put(p, 0x9Fu); break;                          /* LAHF */
        case 34: put(p, 0x10u + rn(4u)); put(p, 0xC0u | rn(64u)); break;   /* ADC */
        case 35: put(p, 0x18u + rn(4u)); put(p, 0xC0u | rn(64u)); break;   /* SBB */
        case 36: put(p, 0xD1u); put(p, 0xC0u | (rn(8u) << 3) | rn(8u)); break; /* shifts */
        case 37: put(p, 0xF7u); put(p, 0xD0u | rn(16u)); break; /* NOT/NEG */
        case 38: put(p, 0x26u + 8u * rn(4u)); put(p, 0x8Bu); modrm_mem(p, rn(8u)); break; /* seg override */
        default: put(p, 0x9Eu); break;                          /* SAHF */
    }
}

static void setup(MdRuntime *rt, const Prog *p, const uint16_t *regs, uint16_t flags, const uint8_t *data)
{
    unsigned i;
    md_runtime_load_com(rt, p->b, p->n, 0x1000u);
    for (i = 0; i < 8; ++i) rt->cpu.r[i] = regs[i];
    md_x86_set_flags(&rt->cpu, flags);
    for (i = 0; i < 0x1000u; ++i) rt->cpu.memory[0x13000u + i] = data[i];
}

static int compare(const MdRuntime *a, const MdRuntime *b)
{
    unsigned i;
    MdX86 ca = a->cpu, cb = b->cpu;
    if (memcmp(a->cpu.r, b->cpu.r, sizeof(a->cpu.r)) != 0) return 1;
    if (a->cpu.es != b->cpu.es || a->cpu.cs != b->cpu.cs || a->cpu.ss != b->cpu.ss ||
        a->cpu.ds != b->cpu.ds || a->cpu.ip != b->cpu.ip) return 2;
    if (md_x86_flags(&ca) != md_x86_flags(&cb)) return 3;
    if (a->instructions != b->instructions) return 4;
    if (a->stop_reason != b->stop_reason) return 5;
    (void)i;
    if (memcmp(a->cpu.memory, b->cpu.memory, MD_X86_ADDRESS_SPACE) != 0) return 6;
    return 0;
}

/*
 * mem_a:      MD_X86_ADDRESS_SPACE bytes for the interpreter runtime.
 * b_region:   2 * MD_X86_ADDRESS_SPACE bytes, aligned to MD_X86_ADDRESS_SPACE;
 *             the translator runtime uses it at +0 (aligned: BFI addressing)
 *             or at +16 (unaligned: UBFX + ADD addressing).
 * arena:      executable translation arena.
 * Returns the number of mismatching cases.
 */
static unsigned md_translate_diff_run(unsigned cases, uint32_t seed, int unaligned, int only,
                                      uint8_t *mem_a, uint8_t *b_region,
                                      uint8_t *arena, uint32_t arena_size)
{
    static MdRuntime A, B;
    static MdTranslator T;
    static uint8_t data[0x1000];
    uint8_t *mem_b = b_region + (unaligned ? 16u : 0u);
    MdHooks hooks;
    unsigned c, fails = 0u;
    uint64_t native = 0, total = 0, episodes = 0, chains = 0;

    memset(&hooks, 0, sizeof(hooks));
    g_rng = seed;
    for (c = 0; c < cases; ++c) {
        Prog p;
        uint16_t regs[8];
        uint16_t flags;
        uint64_t budget;
        unsigned i, d;
        p.n = 0;
        while (p.n < 160u + rn(200u)) gen_insn(&p, 160u);
        put(&p, 0xF4u);                                     /* HLT */
        for (i = 0; i < 8; ++i) regs[i] = (uint16_t)rnd();
        regs[3] = (uint16_t)(0x3000u + rn(0x400u));        /* BX */
        regs[5] = (uint16_t)(0x3000u + rn(0x400u));        /* BP */
        regs[6] = (uint16_t)(0x3000u + rn(0x400u));        /* SI */
        regs[7] = (uint16_t)(0x3000u + rn(0x400u));        /* DI */
        regs[4] = (uint16_t)(0xF000u + rn(0x800u));        /* SP */
        if (rn(50u) == 0u) regs[6] = 0xFFFFu;              /* offset-wrap edge */
        flags = (uint16_t)(0x0002u | (rnd() & 0x08D5u));
        for (i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)rnd();
        budget = 1u + rn(rn(4u) == 0u ? 50000u : 3000u);

        memset(mem_a, 0, MD_X86_ADDRESS_SPACE);
        memset(mem_b, 0, MD_X86_ADDRESS_SPACE);
        md_runtime_init(&A, mem_a, &hooks);
        md_runtime_init(&B, mem_b, &hooks);
        setup(&A, &p, regs, flags, data);
        setup(&B, &p, regs, flags, data);
        if (!md_tr_init(&T, &B, arena, arena_size)) {
            printf("[translate-diff] translator unavailable on this host\n");
            return cases;
        }

        if (only >= 0 && (int)c != only) continue;
        if (only >= 0) {
#if !defined(MD_TRANSLATE_DIFF_NO_MAIN)
            FILE *f = fopen("translate_diff_case.bin", "wb");
            if (f != NULL) { fwrite(p.b, 1, p.n, f); fclose(f); }
#endif
            printf("case %u budget %llu regs", c, (unsigned long long)budget);
            for (i = 0; i < 8; ++i) printf(" %04X", regs[i]);
            printf(" flags %04X\n", flags);
        }
        (void)md_interp_run(&A, budget);
        (void)md_tr_run(&T, budget);
        d = (unsigned)compare(&A, &B);
        if (d != 0u && only >= 0) {
            /* bisect the first diverging budget */
            uint64_t lo = 0, hi = budget;
            while (hi - lo > 1u) {
                const uint64_t mid = (lo + hi) / 2u;
                memset(mem_a, 0, MD_X86_ADDRESS_SPACE); memset(mem_b, 0, MD_X86_ADDRESS_SPACE);
                md_runtime_init(&A, mem_a, &hooks); md_runtime_init(&B, mem_b, &hooks);
                setup(&A, &p, regs, flags, data); setup(&B, &p, regs, flags, data);
                md_tr_init(&T, &B, arena, arena_size);
                (void)md_interp_run(&A, mid); (void)md_tr_run(&T, mid);
                if (compare(&A, &B)) hi = mid; else lo = mid;
            }
            memset(mem_a, 0, MD_X86_ADDRESS_SPACE);
            md_runtime_init(&A, mem_a, &hooks); setup(&A, &p, regs, flags, data);
            (void)md_interp_run(&A, lo);
            {
                const uint32_t lin = md_x86_linear(A.cpu.cs, A.cpu.ip);
                MdX86 ca = A.cpu;
                printf("first divergence at budget %llu: before it cs:ip=%04X:%04X bytes",
                       (unsigned long long)hi, A.cpu.cs, A.cpu.ip);
                for (i = 0; i < 8; ++i) printf(" %02X", A.cpu.memory[(lin + i) & (MD_X86_ADDRESS_SPACE - 1u)]);
                printf("\n  regs ax=%04X cx=%04X dx=%04X bx=%04X sp=%04X bp=%04X si=%04X di=%04X fl=%04X lazy=%u\n",
                       A.cpu.r[0], A.cpu.r[1], A.cpu.r[2], A.cpu.r[3], A.cpu.r[4], A.cpu.r[5], A.cpu.r[6], A.cpu.r[7],
                       md_x86_flags(&ca), A.cpu.lazy_op);
            }
            memset(mem_a, 0, MD_X86_ADDRESS_SPACE); memset(mem_b, 0, MD_X86_ADDRESS_SPACE);
            md_runtime_init(&A, mem_a, &hooks); md_runtime_init(&B, mem_b, &hooks);
            setup(&A, &p, regs, flags, data); setup(&B, &p, regs, flags, data);
            md_tr_init(&T, &B, arena, arena_size);
            (void)md_interp_run(&A, hi); (void)md_tr_run(&T, hi);
            d = (unsigned)compare(&A, &B);
            {
                unsigned s2;
                printf("  B stats: native=%llu interp=%llu translations=%u invalid=%u store=%u chains=%u\n",
                       (unsigned long long)T.stats.native_instructions, (unsigned long long)T.stats.interp_instructions,
                       (unsigned)T.stats.translations, (unsigned)T.stats.exit_invalid,
                       (unsigned)T.stats.exit_store, (unsigned)T.stats.chains);
                for (s2 = 0; s2 < MD_TR_SLOTS; ++s2) {
                    const MdTrBlock *bk = &T.blocks[s2];
                    if (bk->state && bk->cs == 0u)
                        printf("  block %04X:%04X state=%u ops=%u pages=%u p0=%u gen0=%u now=%u\n", bk->cs, bk->ip,
                               bk->state, bk->ops, bk->page_count, bk->page[0], (unsigned)bk->gen[0],
                               (unsigned)B.code_page_generation[bk->page[0]]);
                }
            }
        }
        native += T.stats.native_instructions;
        total += B.instructions;
        episodes += T.stats.episodes;
        chains += T.stats.chains;
        if (((c + 1u) % MD_TRANSLATE_DIFF_PROGRESS) == 0u) {
            printf("[translate-diff] progress %u/%u fails=%u\n", c + 1u, cases, fails);
            fflush(stdout);
        }
        if (d != 0u) {
            if (fails++ < 5u) {
                MdX86 ca = A.cpu, cb = B.cpu;
                printf("MISMATCH case %u kind %u budget %llu\n", c, d, (unsigned long long)budget);
                printf("  interp: ip=%04X ax=%04X cx=%04X dx=%04X bx=%04X sp=%04X bp=%04X si=%04X di=%04X fl=%04X n=%llu st=%d\n",
                       A.cpu.ip, A.cpu.r[0], A.cpu.r[1], A.cpu.r[2], A.cpu.r[3], A.cpu.r[4], A.cpu.r[5],
                       A.cpu.r[6], A.cpu.r[7], md_x86_flags(&ca), (unsigned long long)A.instructions, A.stop_reason);
                printf("  transl: ip=%04X ax=%04X cx=%04X dx=%04X bx=%04X sp=%04X bp=%04X si=%04X di=%04X fl=%04X n=%llu st=%d\n",
                       B.cpu.ip, B.cpu.r[0], B.cpu.r[1], B.cpu.r[2], B.cpu.r[3], B.cpu.r[4], B.cpu.r[5],
                       B.cpu.r[6], B.cpu.r[7], md_x86_flags(&cb), (unsigned long long)B.instructions, B.stop_reason);
            }
        }
    }
    printf("[translate-diff] cases=%u seed=%08X %s fails=%u native=%.1f%% chains=%llu instr/episode=%.1f\n",
           cases, (unsigned)seed, unaligned ? "unaligned" : "aligned", fails,
           total ? 100.0 * (double)native / (double)total : 0.0,
           (unsigned long long)chains, episodes ? (double)native / (double)episodes : 0.0);
    return fails;
}

#if !defined(MD_TRANSLATE_DIFF_NO_MAIN)
int main(int argc, char **argv)
{
    const unsigned cases = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 20000u;
    const uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x4D32355Bu;
    const int unaligned = argc > 3 ? atoi(argv[3]) : 0;
    const int only = argc > 4 ? atoi(argv[4]) : -1;
    uint8_t *mem_a = malloc(MD_X86_ADDRESS_SPACE);
    uint8_t *raw_b, *b_region, *arena;
#if defined(__linux__)
    raw_b = mmap(NULL, 3u * MD_X86_ADDRESS_SPACE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    arena = mmap(NULL, 256u * 1024u, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    raw_b = malloc(3u * MD_X86_ADDRESS_SPACE);
    arena = malloc(256u * 1024u);
#endif
    b_region = (uint8_t *)(((uintptr_t)raw_b + MD_X86_ADDRESS_SPACE - 1u) &
                           ~(uintptr_t)(MD_X86_ADDRESS_SPACE - 1u));
    return md_translate_diff_run(cases, seed, unaligned, only, mem_a, b_region,
                                 arena, 256u * 1024u) != 0u;
}
#endif
