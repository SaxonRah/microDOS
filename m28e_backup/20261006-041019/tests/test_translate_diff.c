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
/* 1: translate every block on first sight (maximum translated coverage);
   0: tiered, as in firmware (needs MICRODOS_ENABLE_BACKEDGE_EXIT to tier). */
static int g_td_eager = 1;

/* Reference interpreter run with md_interp_run()'s plain contract: with
   back-edge exits compiled in, md_interp_run() also returns at loop heads,
   so keep calling it until the budget is used or a real stop occurs. */
static MdStopReason td_ref_run(MdRuntime *r, uint64_t budget)
{
    const uint64_t start = r->instructions;
    MdStopReason st;
#if MD_INTERP_BACKEDGE_EXIT
    /* saturate the suppression filter: no back-edge exits at all */
    r->native_v2_suppress_bloom[0] = 0xFFFFFFFFu;
    r->native_v2_suppress_bloom[1] = 0xFFFFFFFFu;
#endif
    for (;;) {
        st = md_interp_run(r, budget - (r->instructions - start));
        if (st != MD_STOP_NONE) return st;
#if MD_INTERP_BACKEDGE_EXIT
        r->native_v2_backedge_hit = 0u;
#endif
    }
}

static int td_init(MdTranslator *t, MdRuntime *b, uint8_t *arena, uint32_t size)
{
    const int ok = md_tr_init(t, b, arena, size);
    t->eager = (uint32_t)g_td_eager;
    return ok;
}
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
    const unsigned t = rn(62u);
    if (t >= 40u) {
        switch (t) {
            case 40: case 41: {                         /* string ops, DF both ways */
                if (rn(3u) == 0u) put(p, 0xFCu + rn(2u));
                if (rn(4u) == 0u) put(p, 0x26u + 8u * rn(4u));
                if (rn(6u) == 0u) { put(p, 0xB9u); put(p, rn(6u)); put(p, 0x00); put(p, 0xF3u); }
                { static const uint8_t so[6] = { 0xA4, 0xA5, 0xAA, 0xAB, 0xAC, 0xAD }; put(p, so[rn(6u)]); }
                break;
            }
            case 42: put(p, 0x06u + 8u * rn(4u)); break;          /* PUSH sreg */
            case 43: put(p, rn(2u) ? 0x07u : 0x1Fu); break;       /* POP ES/DS */
            case 44: put(p, 0x8Cu); modrm_any(p, rn(4u)); break;   /* MOV r/m,sreg */
            case 45: put(p, 0x8Eu); modrm_any(p, rn(2u) ? 0u : 3u); break;   /* MOV ES/DS,r/m */
            case 46: put(p, 0x86u + rn(2u)); modrm_any(p, rn(8u)); break;    /* XCHG */
            case 47: case 48: {                         /* F6/F7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV */
                const unsigned w = rn(2u), ext = rn(8u);
                put(p, 0xF6u + w); modrm_any(p, ext);
                if (ext <= 1u) { put(p, rn(256)); if (w) put(p, rn(256)); }
                break;
            }
            case 49: case 50: put(p, 0xD0u + rn(4u)); modrm_any(p, rn(8u)); break;   /* shifts */
            case 51: put(p, 0xE0u + rn(2u)); put(p, (uint8_t)(-(int)rn(16u))); break; /* LOOPNZ/Z */
            case 52: put(p, 0xCDu); put(p, rn(8u)); break;        /* INT n */
            case 53: put(p, 0xCFu); break;                        /* IRET */
            case 54: put(p, rn(2u) ? 0xCCu : 0xCEu); break;       /* INT3 / INTO */
            case 55: put(p, 0xFFu); put(p, 0xC0u | ((rn(2u) ? 4u : 2u) << 3) | rn(8u)); break; /* JMP/CALL r16 */
            case 56: put(p, 0xEAu); put(p, rn(code_len_hint)); put(p, 0x01); put(p, 0x00); put(p, 0x10); break; /* JMP far */
            case 57: put(p, 0x9Au); put(p, rn(code_len_hint)); put(p, 0x01); put(p, 0x00); put(p, 0x10); break; /* CALL far */
            case 58: put(p, rn(2u) ? 0xCBu : 0xCAu); if (p->b[p->n - 1u] == 0xCAu) { put(p, rn(8u)); put(p, 0); } break;
            case 59: put(p, 0xFCu + rn(2u)); break;               /* CLD/STD */
            case 60: put(p, 0x16u); break;                        /* PUSH SS */
            default: put(p, 0xF5u); break;
        }
        return;
    }
    switch (t) {
    case 999: break;
    }
    {
    const unsigned t2 = t;
    (void)t2;
    }
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
    for (i = 0; i < 256u; ++i) {                 /* vectors land inside the program */
        rt->cpu.memory[4u * i] = (uint8_t)(0x00u + (i * 7u) % 0x90u);
        rt->cpu.memory[4u * i + 1u] = 0x01u;
        rt->cpu.memory[4u * i + 2u] = 0x00u;
        rt->cpu.memory[4u * i + 3u] = 0x10u;
    }
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
        if (!td_init(&T, &B, arena, arena_size)) {
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
        (void)td_ref_run(&A, budget);
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
                td_init(&T, &B, arena, arena_size);
                (void)td_ref_run(&A, mid); (void)md_tr_run(&T, mid);
                if (compare(&A, &B)) hi = mid; else lo = mid;
            }
            memset(mem_a, 0, MD_X86_ADDRESS_SPACE);
            md_runtime_init(&A, mem_a, &hooks); setup(&A, &p, regs, flags, data);
            (void)td_ref_run(&A, lo);
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
            td_init(&T, &B, arena, arena_size);
            (void)td_ref_run(&A, hi); (void)md_tr_run(&T, hi);
            d = (unsigned)compare(&A, &B);
            printf("  prog:");
            for (i = 0; i < 24u && i < p.n; ++i) printf(" %02X", p.b[i]);
            printf("\n  A: %04X:%04X es=%04X ds=%04X ss=%04X sp=%04X  B: %04X:%04X es=%04X ds=%04X ss=%04X sp=%04X\n",
                   A.cpu.cs, A.cpu.ip, A.cpu.es, A.cpu.ds, A.cpu.ss, A.cpu.r[4],
                   B.cpu.cs, B.cpu.ip, B.cpu.es, B.cpu.ds, B.cpu.ss, B.cpu.r[4]);
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
    printf("[translate-diff] %s cases=%u seed=%08X %s fails=%u native=%.1f%% chains=%llu instr/episode=%.1f\n",
           g_td_eager ? "eager" : "tiered", cases, (unsigned)seed, unaligned ? "unaligned" : "aligned", fails,
           total ? 100.0 * (double)native / (double)total : 0.0,
           (unsigned long long)chains, episodes ? (double)native / (double)episodes : 0.0);
    return fails;
}

/*
 * Directed programs for paths random code rarely reaches: self-loop latches
 * with deferred flag writes (including CF preserved across INC/DEC), data
 * stores to the code page (byte-exact tracking: no invalidation), real
 * self-modifying code inside a running loop, and deep CALL/RET (inline
 * dispatch). Each runs at many budgets so budget exits land everywhere.
 */
typedef struct DirectedProg { const char *name; const uint8_t *code; size_t size; } DirectedProg;

/* stc / mov cx,7 / l: dec cx / jnz l / pushf / pop ax / hlt */
static const uint8_t kDecLoopCF[] = { 0xF9, 0xB9, 0x07, 0x00, 0x49, 0x75, 0xFD, 0x9C, 0x58, 0xF4 };
/* mov si,0 / l: inc si / cmp si,40h / jb l / pushf / pop ax / hlt */
static const uint8_t kCmpLoop[] = { 0xBE, 0x00, 0x00, 0x46, 0x83, 0xFE, 0x40, 0x72, 0xFA, 0x9C, 0x58, 0xF4 };
/* mov cx,30 / mov bx,0F00h / l: add ax,bx / dec cx / jnz l / pushf / pop dx / hlt */
static const uint8_t kAddDecLoop[] = { 0xB9, 0x1E, 0x00, 0xBB, 0x00, 0x0F, 0x01, 0xD8, 0x49, 0x75, 0xFB, 0x9C, 0x5A, 0xF4 };
/* data on the code page: mov si,180h / mov cx,40h / mov al,5Ah /
   l: mov [si],al / inc si / inc al / dec cx / jnz l / hlt */
static const uint8_t kSamePageStore[] = {
    0xBE, 0x80, 0x01, 0xB9, 0x40, 0x00, 0xB0, 0x5A,
    0x88, 0x04, 0x46, 0xFE, 0xC0, 0x49, 0x75, 0xF8, 0xF4
};
/* self-modifying loop: mov cx,20 / xor dx,dx /
   l: mov [patch+1],cl / patch: mov al,00 / add dl,al / dec cx / jnz l / hlt
   (patch at 0x10C; its immediate byte at 0x10D) */
static const uint8_t kSmcLoop[] = {
    0xB9, 0x14, 0x00, 0x31, 0xD2,
    0x88, 0x0E, 0x0D, 0x01,        /* 105: mov [010Dh],cl */
    0xB0, 0x00,                    /* 109: mov al,00 -- wait: fixed below */
    0x00, 0xC2, 0x49, 0x75, 0xF5, 0xF4
};
/* recursion: mov cx,40 / call f / hlt / f: dec cx / jz done / call f / done: ret */
static const uint8_t kRecurse[] = {
    0xB9, 0x28, 0x00, 0xE8, 0x01, 0x00, 0xF4,
    0x49, 0x74, 0x03, 0xE8, 0xFA, 0xFF, 0xC3
};
/* byte compare scanner: mov si,200h / l: mov al,[si] / inc si / cmp al,0 / jne l / hlt,
   with a NUL 37 bytes in */
static const uint8_t kScan8[] = { 0xBE, 0x00, 0x02, 0x8A, 0x04, 0x46, 0x3C, 0x00, 0x75, 0xF9, 0xF4 };

/*
 * M28b far-pointer directed case.
 *
 * Prepare two m16:16 pointers, then deliberately use the destination register
 * as part of the effective address:
 *
 *     LES DI,[DI]
 *     LDS BX,[BX]
 *
 * A lowering that writes DI/BX before fetching the segment word will fail.
 */
static const uint8_t kLesLds[] = {
    0xBB, 0x00, 0x02,                         /* mov bx,0200h */
    0xBF, 0x10, 0x02,                         /* mov di,0210h */
    0xC7, 0x06, 0x00, 0x02, 0x56, 0x34,       /* word [0200]=3456h */
    0xC7, 0x06, 0x02, 0x02, 0x9A, 0x78,       /* word [0202]=789Ah */
    0xC7, 0x06, 0x10, 0x02, 0x11, 0x11,       /* word [0210]=1111h */
    0xC7, 0x06, 0x12, 0x02, 0x22, 0x22,       /* word [0212]=2222h */
    0xC4, 0x3D,                               /* les di,[di] */
    0xC5, 0x1F,                               /* lds bx,[bx] */
    0xF4                                      /* hlt */
};

/*
 * M28c control-step-only directed case.
 *
 * EA is an unlowered far JMP, therefore a K_CSTEP and a terminator. In eager
 * mode this program begins with a one-op control-only translated block. The
 * canonical interpreter performs the far transfer; M25 must then redispatch
 * at 1000:0108 and remain bit-exact with the reference interpreter.
 */
static const uint8_t kControlOnly[] = {
    0xEA, 0x08, 0x01, 0x00, 0x10,             /* jmp far 1000:0108h */
    0x90, 0x90, 0x90,                         /* unreachable padding */
    0xB8, 0x34, 0x12,                         /* mov ax,1234h */
    0xF4                                      /* hlt */
};

static unsigned md_translate_directed(uint8_t *mem_a, uint8_t *b_region, int unaligned,
                                      uint8_t *arena, uint32_t arena_size)
{
    static MdRuntime A, B;
    static MdTranslator T;
    static uint8_t smc[sizeof(kSmcLoop)];
    DirectedProg progs[9];
    uint8_t *mem_b = b_region + (unaligned ? 16u : 0u);
    MdHooks hooks;
    unsigned k, fails = 0u, runs = 0u;
    uint32_t latches = 0, live = 0, invalid = 0, store_exits = 0;
    uint64_t budget;

    /* kSmcLoop as written above has the patch target off by the prefix
       bytes; build the real layout: the store hits the immediate of the
       mov al,imm8 that follows it inside the same loop block. */
    memcpy(smc, kSmcLoop, sizeof(smc));
    smc[7] = 0x0A; smc[8] = 0x01;             /* mov [010Ah],cl -> imm of mov al at 0109h */
    smc[15] = (uint8_t)(0x05 - 0x10);          /* jnz back to l (0105h) */

    progs[0] = (DirectedProg){ "dec-loop-cf", kDecLoopCF, sizeof(kDecLoopCF) };
    progs[1] = (DirectedProg){ "cmp-loop", kCmpLoop, sizeof(kCmpLoop) };
    progs[2] = (DirectedProg){ "add-dec-loop", kAddDecLoop, sizeof(kAddDecLoop) };
    progs[3] = (DirectedProg){ "same-page-store", kSamePageStore, sizeof(kSamePageStore) };
    progs[4] = (DirectedProg){ "smc-loop", smc, sizeof(smc) };
    progs[5] = (DirectedProg){ "recurse", kRecurse, sizeof(kRecurse) };
    progs[6] = (DirectedProg){ "scan8", kScan8, sizeof(kScan8) };
    progs[7] = (DirectedProg){ "les-lds-self-ea", kLesLds, sizeof(kLesLds) };
    progs[8] = (DirectedProg){ "control-step-only", kControlOnly, sizeof(kControlOnly) };

    memset(&hooks, 0, sizeof(hooks));
    for (k = 0u; k < 9u; ++k) {
        for (budget = 1u; budget <= 400u; budget += (budget < 64u ? 1u : 7u)) {
            unsigned d;
            memset(mem_a, 0, MD_X86_ADDRESS_SPACE);
            memset(mem_b, 0, MD_X86_ADDRESS_SPACE);
            md_runtime_init(&A, mem_a, &hooks);
            md_runtime_init(&B, mem_b, &hooks);
            md_runtime_load_com(&A, progs[k].code, progs[k].size, 0x1000u);
            md_runtime_load_com(&B, progs[k].code, progs[k].size, 0x1000u);
            A.cpu.memory[0x10200u + 37u] = 0u; B.cpu.memory[0x10200u + 37u] = 0u;
            {
                unsigned i;
                for (i = 0; i < 37u; ++i) {
                    A.cpu.memory[0x10200u + i] = (uint8_t)(i + 1u);
                    B.cpu.memory[0x10200u + i] = (uint8_t)(i + 1u);
                }
            }
            A.cpu.r[0] = B.cpu.r[0] = 0x1234u;
            if (!td_init(&T, &B, arena, arena_size)) return 1u;
            (void)td_ref_run(&A, budget);
            (void)md_tr_run(&T, budget);
            d = (unsigned)compare(&A, &B);
            ++runs;
            latches += T.stats.deferred_latches;
            live += T.stats.live_pages;
            invalid += T.stats.exit_invalid;
            store_exits += T.stats.exit_store;
            if (k == 3u && budget > 200u && (T.stats.exit_invalid != 0u || T.stats.exit_store != 0u)) {
                printf("[translate-directed] same-page-store invalidated translated code (invalid=%u store=%u)\n",
                       (unsigned)T.stats.exit_invalid, (unsigned)T.stats.exit_store);
                ++fails;
            }
            if (d != 0u) {
                MdX86 ca = A.cpu, cb = B.cpu;
                if (fails++ < 8u)
                    printf("[translate-directed] MISMATCH %s budget=%llu kind=%u ip %04X/%04X ax %04X/%04X cx %04X/%04X fl %04X/%04X n %llu/%llu\n",
                           progs[k].name, (unsigned long long)budget, d, A.cpu.ip, B.cpu.ip,
                           A.cpu.r[0], B.cpu.r[0], A.cpu.r[1], B.cpu.r[1],
                           md_x86_flags(&ca), md_x86_flags(&cb),
                           (unsigned long long)A.instructions, (unsigned long long)B.instructions);
            }
        }
    }
    printf("[translate-directed] %s %s runs=%u fails=%u deferred-latches=%u live-pages=%u invalid-exits=%u store-exits=%u\n",
           g_td_eager ? "eager" : "tiered", unaligned ? "unaligned" : "aligned", runs, fails, (unsigned)latches, (unsigned)live,
           (unsigned)invalid, (unsigned)store_exits);
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
    {
        unsigned fails = 0u;
        int mode;
        for (mode = 1; mode >= 0; --mode) {          /* eager, then tiered */
            g_td_eager = mode;
            fails += md_translate_directed(mem_a, b_region, unaligned, arena, 256u * 1024u);
            fails += md_translate_diff_run(cases, seed, unaligned, only, mem_a, b_region,
                                           arena, 256u * 1024u);
        }
        return fails != 0u;
    }
}
#endif
