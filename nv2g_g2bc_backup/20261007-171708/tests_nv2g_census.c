/*
 * NV2-G hot-loop census.
 *
 * Boots the released MS-DOS 2.0 exactly like microdos_dos2_e2e (same disk
 * image, same scripted keys), runs DOS2TEST single-stepped through the
 * canonical interpreter, and records every short backward branch the way the
 * Native-v2 runtime sees it (interpreter back-edge -> admission request).
 *
 * For each loop root it reports:
 *   - dynamic weight: guest instructions retired per iteration, summed over
 *     every observed iteration (callees included: that is the time spent
 *     inside the loop from the interpreter's point of view);
 *   - who would take it today: Native-v2 special compiler, NV2-G, or nobody;
 *   - EVERY NV2-G blocker in the loop body, not only the first one the
 *     firmware's reject counters see.
 *
 * The summary ranks features by the weight they would unlock alone. This is
 * the input for choosing the next NV2-G slice.
 *
 * usage: microdos_nv2g_census MSDOS.SYS disk.img [--top N] [--run "CMD"]...
 *   default workload: DOS2TEST. Each --run types CMD at the next A> prompt
 *   (a ' in CMD is typed as ", e.g. --run "FIND /C 'MOV' FIND.ASM");
 *   the census ends at the A> after the last command.
 *
 * Weights (guest instructions):
 *   body  - executed inside the loop's own byte span [root, latch end) while
 *           it is the innermost active loop (what NV2-G could run natively);
 *   incl  - per-iteration totals between consecutive back-edges, callees
 *           included, iterations longer than CENSUS_ITER_CAP ignored (so a
 *           dispatcher tail-jump such as MSDOS 1000:068E is not a "loop").
 */
#include <stddef.h>
struct E2eStep;
static const struct E2eStep *census_script(void);
static size_t census_script_n(void);
#define E2E_SCRIPT census_script()
#define E2E_SCRIPT_COUNT census_script_n()
#define main md_e2e_main_unused
#include "test_dos2_e2e.c"
#undef main

#define CENSUS_MAX_STEPS 16u
static E2eStep g_steps[CENSUS_MAX_STEPS];
static char g_step_keys[CENSUS_MAX_STEPS][96];
static size_t g_nsteps;
static const struct E2eStep *census_script(void) { return g_steps; }
static size_t census_script_n(void) { return g_nsteps; }

#define CENSUS_BUDGET 800000000ull
#define CENSUS_ITER_CAP 4096u

#include "../src/runtime/native_v2g.c"
#include "microdos/native_v2.h"
#include "microdos/native_v2_runtime.h"

#define CENSUS_MAX_ROOTS 4096u
#define CENSUS_WINDOW MD_NATIVE_V2_RT_MAX_GUEST_BYTES
#define CENSUS_MAX_BLOCKERS 8u

typedef enum CensusOwner {
    OWN_NONE = 0,
    OWN_NV2_COUNTED,
    OWN_NV2_LOCALCALL,
    OWN_NV2_REP,
    OWN_NV2G
} CensusOwner;

typedef struct Root {
    uint16_t cs, ip;
    uint16_t end;            /* latch next_ip (0 = unknown) */
    uint64_t hits;
    uint64_t weight;         /* incl, capped */
    uint64_t body;
    uint8_t used;
    uint8_t owner;
    uint8_t nblk;
    uint8_t g_reason;        /* first NV2-G reject category, see kReason */
    char blk[CENSUS_MAX_BLOCKERS][12];
    uint8_t bytes[64];
    uint8_t nbytes;
} Root;

static Root g_roots[CENSUS_MAX_ROOTS];
static unsigned g_nroots;

static const char *const kReason[] = {
    "ok", "badarg", "region", "decode", "control", "opcode",
    "cfg", "flags", "memory", "exits", "emit", "?"
};

static Root *census_root(uint16_t cs, uint16_t ip, int create)
{
    uint32_t h = ((uint32_t)cs * 0x9E37u) ^ ((uint32_t)ip * 0x85EBu);
    unsigned i;
    for (i = 0u; i < CENSUS_MAX_ROOTS; ++i) {
        Root *r = &g_roots[(h + i) & (CENSUS_MAX_ROOTS - 1u)];
        if (!r->used) {
            if (!create) return NULL;
            r->used = 1u; r->cs = cs; r->ip = ip;
            ++g_nroots;
            return r;
        }
        if (r->cs == cs && r->ip == ip) return r;
    }
    return NULL;
}

static void blk_add(Root *r, const char *name)
{
    unsigned i;
    for (i = 0u; i < r->nblk; ++i)
        if (strcmp(r->blk[i], name) == 0) return;
    if (r->nblk < CENSUS_MAX_BLOCKERS) {
        strncpy(r->blk[r->nblk], name, sizeof(r->blk[0]) - 1u);
        r->blk[r->nblk][sizeof(r->blk[0]) - 1u] = '\0';
        ++r->nblk;
    }
}

/* Coarse names for opcodes NV2-G does not lower. */
static void op_name(const uint8_t *q, unsigned op, char *out, size_t n)
{
    const unsigned modrm = q[1];
    const unsigned reg = (modrm >> 3) & 7u;
    const int mem = (modrm >> 6) != 3u;
    const char *s = NULL;

    switch (op) {
        case 0xA4: case 0xA5: s = "MOVS1"; break;
        case 0xA6: case 0xA7: s = "CMPS1"; break;
        case 0xAE: case 0xAF: s = "SCAS1"; break;
        case 0x06: case 0x0E: case 0x16: case 0x1E: s = "PUSHseg"; break;
        case 0x07: case 0x17: case 0x1F: s = "POPseg"; break;
        case 0x8C: case 0x8E: s = "MOVseg"; break;
        case 0x86: case 0x87: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: s = "XCHG"; break;
        case 0xD0: case 0xD1: s = "SHIFT1"; break;
        case 0xD2: case 0xD3: s = "SHIFTCL"; break;
        case 0x98: s = "CBW"; break;
        case 0x99: s = "CWD"; break;
        case 0x8D: s = "LEA"; break;
        case 0xD7: s = "XLAT"; break;
        case 0xC4: case 0xC5: s = "LxS"; break;
        case 0x9C: case 0x9D: s = "PUSHF"; break;
        case 0x9E: case 0x9F: s = "SAHF"; break;
        case 0xF8: case 0xF9: case 0xF5: s = "CLC/STC"; break;
        case 0xFA: case 0xFB: s = "CLI/STI"; break;
        case 0xFC: case 0xFD: s = "CLD/STD"; break;
        case 0xA8: case 0xA9: s = "TESTacc"; break;
        case 0x84: case 0x85: s = "TESTrm"; break;
        case 0xC6: case 0xC7: s = "MOVm,imm"; break;
        case 0x80: case 0x81: case 0x82: case 0x83:
            s = mem ? (reg == 7u ? "CMPm,imm" : "ALUm,imm") : NULL; break;
        case 0xFE:
            s = mem ? "INCDECm8" : "INCDECr8"; break;
        case 0xFF:
            s = reg <= 1u ? "INCDECm16" : reg == 6u ? "PUSHm" : "FFgrp"; break;
        case 0xF6: case 0xF7:
            s = reg == 0u ? (mem ? "TESTm,imm" : "TESTr,imm") :
                reg == 2u ? "NOT" : reg == 3u ? "NEG" : "MULDIV"; break;
        default:
            if (op <= 0x3Fu && ((op & 7u) <= 5u) &&
                ((op >> 3) == 2u || (op >> 3) == 3u)) s = "ADC/SBB";
            else if (op <= 0x3Fu && (op & 7u) <= 3u && mem) s = "ALUm,r";
            break;
    }
    if (s != NULL) snprintf(out, n, "%s", s);
    else snprintf(out, n, "op%02X", op);
}

/* Linear walk of the loop body from root to its latch: every blocker. */
static void census_blockers(Root *r, const uint8_t *image, size_t size)
{
    uint16_t ip = r->ip;
    unsigned guard = 0u;

    while (guard++ < 64u) {
        MdDecodedInstruction d, d2;
        GOp o;
        const size_t off = (size_t)(uint16_t)(ip - r->ip);
        char nm[16];
        unsigned pi;

        if (off >= size) { blk_add(r, "toolong"); return; }
        if (!md_decode_8086(image, size, r->ip, ip, &d) || !d.valid_8086) {
            blk_add(r, "decode"); return;
        }
        for (pi = 0u; pi < d.prefix_count; ++pi) {
            const unsigned p = d.prefixes[pi];
            blk_add(r, (p == 0xF2u || p == 0xF3u) ? "REP" :
                       (p == 0xF0u) ? "LOCK" : "SEG");
        }
        if (d.far_control) blk_add(r, "FAR");
        else if (d.flow == MD_DECODE_FLOW_CALL) blk_add(r, "CALL");
        else if (d.flow == MD_DECODE_FLOW_INDIRECT_CALL) blk_add(r, "ICALL");
        else if (d.flow == MD_DECODE_FLOW_INDIRECT_JUMP) blk_add(r, "IJMP");
        else if (d.flow == MD_DECODE_FLOW_RETURN) blk_add(r, "RET");
        else if (d.flow == MD_DECODE_FLOW_STOP)
            blk_add(r, (d.opcode == 0xCDu || d.opcode == 0xCCu) ? "INT" : "STOP");
        else {
            d2 = d;
            d2.ip = (uint16_t)(d.ip + d.prefix_count);
            d2.prefix_count = 0u;
            if (!g_decode_op(image, size, r->ip, &d2, &o)) {
                op_name(image + (size_t)(uint16_t)(d2.ip - r->ip),
                        d.opcode, nm, sizeof(nm));
                blk_add(r, nm);
            }
        }
        if ((d.flow == MD_DECODE_FLOW_CONDITIONAL ||
             d.flow == MD_DECODE_FLOW_JUMP) && d.target == r->ip) {
            r->end = d.next_ip;
            return;
        }
        ip = d.next_ip;
    }
    blk_add(r, "toolong");
}

static unsigned g_reason_from_delta(const MdNativeV2GStats *a,
                                    const MdNativeV2GStats *b)
{
    if (b->compiles != a->compiles) return 0u;
    if (b->reject_bad_argument != a->reject_bad_argument) return 1u;
    if (b->reject_region != a->reject_region) return 2u;
    if (b->reject_decode != a->reject_decode) return 3u;
    if (b->reject_control != a->reject_control) return 4u;
    if (b->reject_opcode != a->reject_opcode) return 5u;
    if (b->reject_cfg != a->reject_cfg) return 6u;
    if (b->reject_flags != a->reject_flags) return 7u;
    if (b->reject_memory != a->reject_memory) return 8u;
    if (b->reject_exits != a->reject_exits) return 9u;
    if (b->reject_emit != a->reject_emit) return 10u;
    return 11u;
}

/* Same cascade order as md_native_v2_runtime_try_execute. */
static void census_classify(Root *r, const MdX86 *cpu)
{
    const uint32_t linear = md_x86_linear(r->cs, r->ip);
    size_t window = CENSUS_WINDOW;
    const uint8_t *guest = cpu->memory + linear;
    MdNativeV2Code code;
    size_t gs = 0u;
    uint8_t creg = 0xffu;
    MdNativeV2GStats before;

    if (window > (size_t)(0x10000u - r->ip)) window = 0x10000u - r->ip;
    if (window > (size_t)(MD_X86_ADDRESS_SPACE - linear))
        window = MD_X86_ADDRESS_SPACE - linear;
    r->nbytes = (uint8_t)(window < 64u ? window : 64u);
    memcpy(r->bytes, guest, r->nbytes);

    if (md_native_v2_compile_counted_loop(guest, window, r->ip, &code, &gs, &creg)
        == MD_NATIVE_V2_OK) { r->owner = OWN_NV2_COUNTED; }
    else if (md_native_v2_compile_local_call_loop(cpu->memory, r->cs, r->ip, &code, &creg)
        == MD_NATIVE_V2_OK) { r->owner = OWN_NV2_LOCALCALL; }
    else if (md_native_v2_compile_rep_string_loop(cpu->memory, r->cs, r->ip, &code, &creg)
        == MD_NATIVE_V2_OK) { r->owner = OWN_NV2_REP; }

    before = *md_native_v2g_stats();
    if (md_native_v2g_compile_loop(guest, window, r->ip, &code, &gs) == MD_NATIVE_V2_OK) {
        if (r->owner == OWN_NONE) r->owner = OWN_NV2G;
        r->g_reason = 0u;
    } else {
        r->g_reason = (uint8_t)g_reason_from_delta(&before, md_native_v2g_stats());
    }
    census_blockers(r, guest, window);
}

static int cmp_weight(const void *pa, const void *pb)
{
    const Root *a = *(const Root *const *)pa, *b = *(const Root *const *)pb;
    if (a->weight != b->weight) return (a->weight < b->weight) - (a->weight > b->weight);
    return (a->body < b->body) - (a->body > b->body);
}

static const char *owner_name(unsigned o)
{
    switch (o) {
        case OWN_NV2_COUNTED: return "NV2-counted";
        case OWN_NV2_LOCALCALL: return "NV2-call";
        case OWN_NV2_REP: return "NV2-rep";
        case OWN_NV2G: return "NV2-G";
        default: return "-";
    }
}

int main(int argc, char **argv)
{
    static E2e e;
    static MdDos2System sys;
    static Root *sorted[CENSUS_MAX_ROOTS];
    uint8_t *memory, *kernel;
    size_t kernel_size = 0u;
    MdRuntime *rt;
    unsigned i, n = 0u, top = 40u;
    uint64_t total_w = 0u, owned_w = 0u, g_w = 0u, last_n = 0u;
    Root *last = NULL;

    if (argc < 3) {
        fprintf(stderr, "usage: %s MSDOS.SYS disk.img [--top N] [--run CMD]...\n", argv[0]);
        return 2;
    }
    g_steps[0].after = "Enter new date"; g_steps[0].keys = "\r";
    g_steps[1].after = "Enter new time"; g_steps[1].keys = "\r";
    g_nsteps = 2u;
    for (i = 3u; i < (unsigned)argc; ++i) {
        if (strcmp(argv[i], "--top") == 0 && i + 1u < (unsigned)argc) {
            top = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--run") == 0 && i + 1u < (unsigned)argc &&
                   g_nsteps < CENSUS_MAX_STEPS) {
            char *k;
            snprintf(g_step_keys[g_nsteps], sizeof(g_step_keys[0]), "%s\r", argv[++i]);
            /* ' becomes " so shells need not escape DOS quotes (FIND /C 'MOV'). */
            for (k = g_step_keys[g_nsteps]; *k != '\0'; ++k)
                if (*k == '\'') *k = '"';
            g_steps[g_nsteps].after = "A>";
            g_steps[g_nsteps].keys = g_step_keys[g_nsteps];
            ++g_nsteps;
        }
    }
    if (g_nsteps == 2u) {
        g_steps[2].after = "A>"; g_steps[2].keys = "DOS2TEST\r";
        g_nsteps = 3u;
    }
    kernel = load(argv[1], &kernel_size);
    e.disk = load(argv[2], &e.disk_size);
    memory = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    if (kernel == NULL || e.disk == NULL || memory == NULL) return 2;
    e.quiet = true;
    if (!e2e_setup(&sys, &e, memory, NULL, kernel, kernel_size, false, false))
        return 2;
    rt = &sys.runtime;

    while (rt->instructions < CENSUS_BUDGET && rt->stop_reason == MD_STOP_NONE) {
        const uint16_t cs = rt->cpu.cs, ip = rt->cpu.ip;
        const uint32_t lin = md_x86_linear(cs, ip);
        MdDecodedInstruction d;
        const int have = md_decode_8086(rt->cpu.memory + lin, 16u, ip, ip, &d);

        if (last != NULL && cs == last->cs && ip >= last->ip &&
            (last->end == 0u || ip < last->end))
            ++last->body;

        (void)md_interp_step(rt);

        if (have && rt->cpu.cs == cs && rt->cpu.ip < ip &&
            (uint16_t)(ip - rt->cpu.ip) < CENSUS_WINDOW &&
            (d.flow == MD_DECODE_FLOW_CONDITIONAL ||
             d.flow == MD_DECODE_FLOW_JUMP) &&
            d.target == rt->cpu.ip) {
            Root *r = census_root(cs, rt->cpu.ip, 1);
            if (r != NULL) {
                if (r->hits == 0u) census_classify(r, &rt->cpu);
                ++r->hits;
                if (last == r && rt->instructions - last_n <= CENSUS_ITER_CAP)
                    r->weight += rt->instructions - last_n;
                last = r;
                last_n = rt->instructions;
            }
        }
        if ((rt->instructions & 0xFFFFu) == 0u) {
            e.out[e.out_len] = '\0';
            if (e.step == g_nsteps && (e.keys == NULL || *e.keys == '\0') &&
                strstr(e.out + e.search_from, "A>") != NULL) break;
        }
    }
    e.out[e.out_len] = '\0';
    if (e.step != g_nsteps || strstr(e.out + e.search_from, "A>") == NULL) {
        fprintf(stderr, "[census] workload did not finish (step %u/%u, %llu instr)\n%s\n",
                (unsigned)e.step, (unsigned)g_nsteps,
                (unsigned long long)rt->instructions, e.out);
        return 1;
    }
    if (strstr(e.out, "SOME TESTS FAILED") != NULL) {
        fprintf(stderr, "[census] DOS2TEST failed\n");
        return 1;
    }

    for (i = 0u; i < CENSUS_MAX_ROOTS; ++i)
        if (g_roots[i].used) {
            sorted[n++] = &g_roots[i];
            total_w += g_roots[i].body;
            if (g_roots[i].owner != OWN_NONE) owned_w += g_roots[i].body;
            if (g_roots[i].owner == OWN_NV2G) g_w += g_roots[i].body;
        }
    qsort(sorted, n, sizeof(sorted[0]), cmp_weight);

    printf("[census] session instructions=%llu loop roots=%u in-loop body=%llu (%.1f%% of session)\n",
           (unsigned long long)rt->instructions, n, (unsigned long long)total_w,
           rt->instructions ? 100.0 * (double)total_w / (double)rt->instructions : 0.0);
    printf("[census] loop body owned today: %.1f%% of session (NV2-G alone %.1f%%)\n\n",
           rt->instructions ? 100.0 * (double)owned_w / (double)rt->instructions : 0.0,
           rt->instructions ? 100.0 * (double)g_w / (double)rt->instructions : 0.0);

    printf("rank root       hits      incl  %%sess      body  %%sess owner       G-first  blockers | bytes\n");
    for (i = 0u; i < n && i < top; ++i) {
        const Root *r = sorted[i];
        const double sess = rt->instructions ? (double)rt->instructions : 1.0;
        unsigned k;
        printf("%4u %04X:%04X %7llu %9llu %5.1f%% %9llu %5.1f%% %-11s %-8s ",
               i + 1u, r->cs, r->ip, (unsigned long long)r->hits,
               (unsigned long long)r->weight, 100.0 * (double)r->weight / sess,
               (unsigned long long)r->body, 100.0 * (double)r->body / sess,
               owner_name(r->owner), kReason[r->g_reason]);
        for (k = 0u; k < r->nblk; ++k) printf("%s%s", k ? "," : "", r->blk[k]);
        if (r->nblk == 0u) printf("(none)");
        printf(" |");
        for (k = 0u; k < 12u; ++k) printf(" %02X", r->bytes[k]);
        printf("\n");
    }

    /* Full loop bytes for the ranked rows (for offline disassembly). */
    for (i = 0u; i < n && i < top; ++i) {
        const Root *r = sorted[i];
        unsigned k, len = r->end ? (unsigned)(uint16_t)(r->end - r->ip) : r->nbytes;
        if (len > r->nbytes) len = r->nbytes;
        printf("[loop] %04X:%04X", r->cs, r->ip);
        for (k = 0u; k < len; ++k) printf(" %02X", r->bytes[k]);
        printf("\n");
    }

    /* Single-feature unlock potential over loops nobody owns today. */
    {
        typedef struct Feat {
            char name[12];
            uint64_t sole_i, sole_b, any_i, any_b;
            unsigned sole_n, any_n;
        } Feat;
        static Feat f[128];
        const double sess = rt->instructions ? (double)rt->instructions : 1.0;
        unsigned nf = 0u, j, k;
        uint64_t nb_b = 0u;
        for (i = 0u; i < n; ++i) {
            const Root *r = sorted[i];
            if (r->owner != OWN_NONE) continue;
            if (r->nblk == 0u) { nb_b += r->body; continue; }
            for (k = 0u; k < r->nblk; ++k) {
                for (j = 0u; j < nf; ++j) if (strcmp(f[j].name, r->blk[k]) == 0) break;
                if (j == nf && nf < 128u) { memcpy(f[nf].name, r->blk[k], 12); ++nf; }
                if (j < 128u) {
                    f[j].any_i += r->weight; f[j].any_b += r->body; ++f[j].any_n;
                    if (r->nblk == 1u) {
                        f[j].sole_i += r->weight; f[j].sole_b += r->body; ++f[j].sole_n;
                    }
                }
            }
        }
        for (j = 0u; j < nf; ++j)
            for (k = j + 1u; k < nf; ++k)
                if (f[k].sole_i > f[j].sole_i ||
                    (f[k].sole_i == f[j].sole_i && f[k].any_i > f[j].any_i)) {
                    Feat t = f[j]; f[j] = f[k]; f[k] = t;
                }
        printf("\n[census] unowned loops with no decode-level blocker: body %.2f%% of session\n",
               100.0 * (double)nb_b / sess);
        printf("[census] %% of session   | sole blocker: incl  body (loops) | present in: incl  body (loops)\n");
        for (j = 0u; j < nf; ++j)
            printf("[census] %-12s |        %6.2f%% %5.2f%% (%3u) |      %6.2f%% %5.2f%% (%3u)\n",
                   f[j].name,
                   100.0 * (double)f[j].sole_i / sess, 100.0 * (double)f[j].sole_b / sess, f[j].sole_n,
                   100.0 * (double)f[j].any_i / sess, 100.0 * (double)f[j].any_b / sess, f[j].any_n);
    }
    return 0;
}
