#include "microdos/native_v2g.h"
#include "microdos/decode.h"
#include "microdos/hot_code.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef MD_NATIVE_V2_G_META_BYTES
#define MD_NATIVE_V2_G_META_BYTES 96u
#endif

#define NV2G_MAGIC 0x47u
#define NV2G_PHASE_G1 17u
#define NV2G_MAX_OPS 64u
#define NV2G_MAX_EXITS 8u
#define NV2G_TAG_BUDGET 0xFEu
#define NV2G_TAG_FALLBACK 0xFFu

/* ARM condition codes used by 16-bit Thumb B<cond>. */
enum {
    G_EQ = 0u, G_NE = 1u, G_CS = 2u, G_CC = 3u,
    G_MI = 4u, G_PL = 5u, G_VS = 6u, G_VC = 7u,
    G_HI = 8u, G_LS = 9u, G_GE = 10u, G_LT = 11u,
    G_GT = 12u, G_LE = 13u, G_AL = 14u
};

typedef enum GKind {
    G_BAD = 0,
    G_MOV_R16_IMM,
    G_MOV_R8_IMM,
    G_MOV_RR16,
    G_MOV_RR8,
    G_LOAD16,
    G_LOAD8,
    G_ALU16_RR,
    G_ALU16_RI,
    G_ALU16_RM,
    G_ALU8_RR,
    G_ALU8_RI,
    G_ALU8_RM,
    G_TEST16_RI,
    G_TEST8_RI,
    G_INC16,
    G_DEC16,
    G_NOT16,
    G_NEG16,
    G_JCC,
    G_JMP,
    G_LOOP,
    G_JCXZ,
    G_NOP
} GKind;

typedef enum GRecipeKind {
    GR_PRESERVE = 0,
    GR_ADD16_RR,
    GR_ADD16_RI,
    GR_SUB16_RR,
    GR_SUB16_RI,
    GR_CMP16_RR,
    GR_CMP16_RI,
    GR_LOGIC16_R,
    GR_TEST16_RI,
    GR_ADD8_RR,
    GR_ADD8_RI,
    GR_SUB8_RR,
    GR_SUB8_RI,
    GR_CMP8_RR,
    GR_CMP8_RI,
    GR_LOGIC8_R,
    GR_TEST8_RI,
    GR_INC16,
    GR_DEC16
} GRecipeKind;

typedef struct GMem {
    int16_t disp;
    uint16_t direct;
    uint8_t mod;
    uint8_t rm;
    uint8_t is_direct;
    uint8_t uses_ss;
} GMem;

typedef struct GRecipe {
    uint16_t ip;
    uint16_t imm;
    uint8_t kind;
    uint8_t dst;
    uint8_t src;
    uint8_t _pad;
} GRecipe;

typedef struct GOp {
    uint16_t ip;
    uint16_t next_ip;
    uint16_t target;
    uint16_t imm;
    GMem mem;
    GRecipe branch_recipe;
    uint8_t kind;
    uint8_t alu;
    uint8_t dst;
    uint8_t src;
    uint8_t width;
    uint8_t cc;
    uint8_t external;
    uint8_t latch;
    uint8_t guard_tag;
    uint8_t exit_tag;
    uint8_t fall_tag;
    uint8_t arm_cond;
} GOp;

typedef struct GMeta {
    uint8_t magic;
    uint8_t exit_count;
    uint8_t max_ops;
    uint8_t needs_memory;
    uint8_t uses_ss_word;
    uint8_t uses_ds_word;
    uint8_t _pad0[2];
    GRecipe exits[NV2G_MAX_EXITS];
    GRecipe budget;
} GMeta;

_Static_assert(sizeof(GMeta) <= MD_NATIVE_V2_G_META_BYTES,
               "NV2-G metadata exceeds MdNativeV2Code::g_meta");

typedef struct TBuf {
    uint8_t *p;
    size_t cap;
    size_t at;
    int failed;
} TBuf;

typedef struct GPatch {
    size_t at;
    uint16_t target_ip;
    uint8_t cond;
    uint8_t is_exit;
    uint8_t exit_tag;
} GPatch;

static const uint8_t kArmReg[8] = {0u,1u,2u,3u,4u,5u,6u,7u};

static MdNativeV2GStats g_nv2g_stats;

/* -------------------------------------------------------------------------
 * Minimal Thumb-2 encoder used only by the G-1 general-loop backend.
 * ------------------------------------------------------------------------- */

static void t16(TBuf *b, uint16_t v)
{
    if (b->failed) return;
    if (b->at + 2u > b->cap) { b->failed = 1; return; }
    b->p[b->at++] = (uint8_t)v;
    b->p[b->at++] = (uint8_t)(v >> 8);
}

static void t32(TBuf *b, uint16_t a, uint16_t c)
{
    t16(b, a); t16(b, c);
}

static int tpatch16(TBuf *b, size_t at, uint16_t v)
{
    if (b->failed || at + 2u > b->cap) return 0;
    b->p[at] = (uint8_t)v;
    b->p[at + 1u] = (uint8_t)(v >> 8);
    return 1;
}


static uint16_t tmovhi(unsigned rd, unsigned rm)
{
    return (uint16_t)(0x4600u | ((rd & 8u) << 4) |
                      ((rm & 15u) << 3) | (rd & 7u));
}

static uint16_t tuxth(unsigned rd, unsigned rm)
{
    return (uint16_t)(0xB280u | ((rm & 7u) << 3) | (rd & 7u));
}

static void tmovw(TBuf *b, unsigned rd, uint16_t imm)
{
    const unsigned i = (imm >> 11) & 1u;
    const unsigned imm4 = (imm >> 12) & 15u;
    const unsigned imm3 = (imm >> 8) & 7u;
    const unsigned imm8 = imm & 255u;
    t32(b, (uint16_t)(0xF240u | (i << 10) | imm4),
        (uint16_t)((imm3 << 12) | ((rd & 15u) << 8) | imm8));
}

static void tldr_w_imm(TBuf *b, unsigned rt, unsigned rn, unsigned imm12)
{
    t32(b, (uint16_t)(0xF8D0u | (rn & 15u)),
        (uint16_t)(((rt & 15u) << 12) | (imm12 & 0xfffu)));
}

static void tldrh_w_imm(TBuf *b, unsigned rt, unsigned rn, unsigned imm12)
{
    t32(b, (uint16_t)(0xF8B0u | (rn & 15u)),
        (uint16_t)(((rt & 15u) << 12) | (imm12 & 0xfffu)));
}

static void tstrh_w_imm(TBuf *b, unsigned rt, unsigned rn, unsigned imm12)
{
    t32(b, (uint16_t)(0xF8A0u | (rn & 15u)),
        (uint16_t)(((rt & 15u) << 12) | (imm12 & 0xfffu)));
}

static void tldrb_w_reg(TBuf *b, unsigned rt, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xF810u | (rn & 15u)),
        (uint16_t)(((rt & 15u) << 12) | (rm & 15u)));
}

static void tldrh_w_reg(TBuf *b, unsigned rt, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xF830u | (rn & 15u)),
        (uint16_t)(((rt & 15u) << 12) | (rm & 15u)));
}

static void tadd_reg(TBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xEB00u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void tsub_reg(TBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xEBA0u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void tadds_reg(TBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xEB10u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void tsubs_reg(TBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xEBB0u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void tadd_imm(TBuf *b, unsigned rd, unsigned rn, unsigned imm8)
{
    t32(b, (uint16_t)(0xF100u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (imm8 & 255u)));
}

static void tsub_imm(TBuf *b, unsigned rd, unsigned rn, unsigned imm8)
{
    t32(b, (uint16_t)(0xF1A0u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (imm8 & 255u)));
}

static void tand_reg(TBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xEA00u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void torr_reg(TBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xEA40u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void teor_reg(TBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    t32(b, (uint16_t)(0xEA80u | (rn & 15u)),
        (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void tmvn_reg(TBuf *b, unsigned rd, unsigned rm)
{
    t32(b, 0xEA6Fu, (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void trsb0(TBuf *b, unsigned rd, unsigned rn)
{
    t32(b, (uint16_t)(0xF1C0u | (rn & 15u)),
        (uint16_t)((rd & 15u) << 8));
}

static void tshift(TBuf *b, unsigned type, unsigned rd, unsigned rm, unsigned imm)
{
    const unsigned imm3 = (imm >> 2) & 7u;
    const unsigned imm2 = imm & 3u;
    t32(b, 0xEA4Fu,
        (uint16_t)((imm3 << 12) | ((rd & 15u) << 8) |
                   (imm2 << 6) | ((type & 3u) << 4) | (rm & 15u)));
}

static void tuxtb_any(TBuf *b, unsigned rd)
{
    if (rd < 8u) {
        t16(b, (uint16_t)(0xB2C0u | ((rd & 7u) << 3) | (rd & 7u)));
    } else {
        /* 16-bit UXTB cannot name r8-r15. LSL/LSR.W is flag-neutral. */
        tshift(b, 0u, rd, rd, 24u);
        tshift(b, 1u, rd, rd, 24u);
    }
}

static void tuxth_any(TBuf *b, unsigned rd)
{
    if (rd < 8u) {
        t16(b, tuxth(rd, rd));
    } else {
        /* 16-bit UXTH cannot name r8-r15. LSL/LSR.W is flag-neutral. */
        tshift(b, 0u, rd, rd, 16u);
        tshift(b, 1u, rd, rd, 16u);
    }
}

static void tcmp_imm(TBuf *b, unsigned rn, unsigned imm8)
{
    t32(b, (uint16_t)(0xF1B0u | (rn & 15u)),
        (uint16_t)(0x0F00u | (imm8 & 255u)));
}

static void tcmp_reg_shift(TBuf *b, unsigned rn, unsigned rm, unsigned lsl)
{
    const unsigned imm3 = (lsl >> 2) & 7u;
    const unsigned imm2 = lsl & 3u;
    /* CMP.W Rn,Rm,LSL #n = SUBS PC,Rn,Rm,LSL #n */
    t32(b, (uint16_t)(0xEBB0u | (rn & 15u)),
        (uint16_t)(0x0F00u | (imm3 << 12) | (imm2 << 6) | (rm & 15u)));
}

static void twrap20(TBuf *b, unsigned rd)
{
    /* UBFX Rd,Rd,#0,#20. */
    t32(b, (uint16_t)(0xF3C0u | (rd & 15u)),
        (uint16_t)(((rd & 15u) << 8) | 0x0013u));
}

static size_t tbcc(TBuf *b, unsigned cond)
{
    const size_t at = b->at;
    /*
     * Long conditional branch veneer:
     *   B<inverse cond> +0   ; skip the following B when condition is false
     *   B target
     *
     * The unconditional Thumb B reaches +/-2 KiB, comfortably covering the
     * complete 1 KiB NV2 code buffer and all G-1 exit stubs. This avoids the
     * +/-256-byte limit of the 16-bit conditional encoding.
     */
    t16(b, (uint16_t)(0xD000u | (((cond ^ 1u) & 15u) << 8)));
    t16(b, 0xE000u);
    return at;
}

static size_t tb(TBuf *b)
{
    const size_t at = b->at;
    t16(b, 0xE000u);
    return at;
}

static int tpatch_b(TBuf *b, size_t at, size_t target);

static int tpatch_bcc(TBuf *b, size_t at, unsigned cond, size_t target)
{
    /*
     * tbcc() emits:
     *   at+0: B<inverse cond> +0  -> PC (at+4), skipping the B below
     *   at+2: B target
     */
    if (cond >= 14u) return 0;
    if (!tpatch16(b, at,
                  (uint16_t)(0xD000u |
                             (((cond ^ 1u) & 15u) << 8)))) {
        return 0;
    }
    return tpatch_b(b, at + 2u, target);
}

static int tpatch_b(TBuf *b, size_t at, size_t target)
{
    const intptr_t delta = (intptr_t)target - (intptr_t)(at + 4u);
    intptr_t imm;
    if ((delta & 1) != 0) return 0;
    imm = delta / 2;
    if (imm < -1024 || imm > 1023) return 0;
    return tpatch16(b, at, (uint16_t)(0xE000u | ((uint16_t)imm & 0x7ffu)));
}

static unsigned r8_parent(unsigned r) { return r & 3u; }

static void emit_r8_to(TBuf *b, unsigned rd, unsigned r8)
{
    const unsigned parent = kArmReg[r8_parent(r8)];
    if (r8 < 4u) {
        t16(b, tmovhi(rd, parent));
        tuxtb_any(b, rd);
    } else {
        /* UBFX Rd,parent,#8,#8. */
        t32(b, (uint16_t)(0xF3C0u | (parent & 15u)),
            (uint16_t)(0x2000u | ((rd & 15u) << 8) | 0x0007u));
    }
}

static void emit_r8_from(TBuf *b, unsigned r8, unsigned src)
{
    const unsigned parent = kArmReg[r8_parent(r8)];
    if (r8 < 4u) {
        /* BFI parent,src,#0,#8 */
        t32(b, (uint16_t)(0xF360u | (src & 15u)),
            (uint16_t)(((parent & 15u) << 8) | 0x0007u));
    } else {
        /* BFI parent,src,#8,#8 */
        t32(b, (uint16_t)(0xF360u | (src & 15u)),
            (uint16_t)(0x2000u | ((parent & 15u) << 8) | 0x000Fu));
    }
}

/* -------------------------------------------------------------------------
 * Decode / natural-loop model.
 * ------------------------------------------------------------------------- */

static int g_parse_mem(const uint8_t *q, size_t avail,
                       const MdDecodedInstruction *d,
                       GMem *m, size_t *imm_at)
{
    const unsigned mod = d->modrm >> 6;
    const unsigned rm = d->modrm & 7u;
    size_t at = 2u;

    if (!d->has_modrm || mod == 3u || avail < 2u) return 0;
    memset(m, 0, sizeof(*m));
    m->mod = (uint8_t)mod;
    m->rm = (uint8_t)rm;
    m->uses_ss = (uint8_t)(rm == 2u || rm == 3u || (rm == 6u && mod != 0u));

    if (mod == 0u && rm == 6u) {
        if (avail < 4u) return 0;
        m->is_direct = 1u;
        m->direct = (uint16_t)((uint16_t)q[2] | ((uint16_t)q[3] << 8));
        at = 4u;
    } else if (mod == 1u) {
        if (avail < 3u) return 0;
        m->disp = (int8_t)q[2];
        at = 3u;
    } else if (mod == 2u) {
        if (avail < 4u) return 0;
        m->disp = (int16_t)((uint16_t)q[2] | ((uint16_t)q[3] << 8));
        at = 4u;
    }

    if (imm_at != NULL) *imm_at = at;
    return 1;
}

static int g_decode_op(const uint8_t *image, size_t image_size,
                       uint16_t base, const MdDecodedInstruction *d,
                       GOp *o)
{
    const size_t off = (size_t)(uint16_t)(d->ip - base);
    const uint8_t *q = image + off;
    const size_t avail = image_size - off;
    const unsigned op = d->opcode;

    memset(o, 0, sizeof(*o));
    o->ip = d->ip; o->next_ip = d->next_ip; o->target = d->target;
    o->guard_tag = 0xffu;

    if (d->prefix_count != 0u) return 0; /* G-2 owns prefixes. */

    if ((op & 0xf8u) == 0xb8u && avail >= 3u) {
        o->kind = G_MOV_R16_IMM; o->dst = (uint8_t)(op & 7u);
        o->imm = (uint16_t)((uint16_t)q[1] | ((uint16_t)q[2] << 8));
        return 1;
    }
    if ((op & 0xf8u) == 0xb0u && avail >= 2u) {
        o->kind = G_MOV_R8_IMM; o->dst = (uint8_t)(op & 7u); o->imm = q[1];
        return 1;
    }

    if ((op == 0x88u || op == 0x8au || op == 0x89u || op == 0x8bu) && d->has_modrm) {
        const unsigned mod = d->modrm >> 6;
        const unsigned reg = (d->modrm >> 3) & 7u;
        const unsigned rm = d->modrm & 7u;
        const unsigned w = op & 1u;
        const unsigned dir = (op >> 1) & 1u;
        if (mod == 3u) {
            if (w) {
                o->kind = G_MOV_RR16;
                o->dst = (uint8_t)(dir ? reg : rm);
                o->src = (uint8_t)(dir ? rm : reg);
            } else {
                o->kind = G_MOV_RR8;
                o->dst = (uint8_t)(dir ? reg : rm);
                o->src = (uint8_t)(dir ? rm : reg);
            }
            return 1;
        }
        if (!dir) return 0; /* stores arrive in G-2 */
        if (!g_parse_mem(q, avail, d, &o->mem, NULL)) return 0;
        o->kind = (uint8_t)(w ? G_LOAD16 : G_LOAD8);
        o->width = (uint8_t)(w ? 16u : 8u);
        o->dst = (uint8_t)reg;
        return 1;
    }

    if (op <= 0x3bu && (op & 7u) <= 3u && d->has_modrm) {
        const unsigned alu = (op >> 3) & 7u;
        const unsigned w = op & 1u;
        const unsigned dir = (op >> 1) & 1u;
        const unsigned mod = d->modrm >> 6;
        const unsigned reg = (d->modrm >> 3) & 7u;
        const unsigned rm = d->modrm & 7u;
        if (!(alu == 0u || alu == 1u || alu == 4u || alu == 5u ||
              alu == 6u || alu == 7u)) return 0; /* ADC/SBB are G-3. */
        o->alu = (uint8_t)alu; o->width = (uint8_t)(w ? 16u : 8u);
        if (mod == 3u) {
            o->kind = (uint8_t)(w ? G_ALU16_RR : G_ALU8_RR);
            o->dst = (uint8_t)(dir ? reg : rm);
            o->src = (uint8_t)(dir ? rm : reg);
            return 1;
        }
        /* G-1 accepts memory only as a load/source. */
        if (!dir) return 0;
        if (!g_parse_mem(q, avail, d, &o->mem, NULL)) return 0;
        o->kind = (uint8_t)(w ? G_ALU16_RM : G_ALU8_RM);
        o->dst = (uint8_t)reg;
        return 1;
    }

    if (op == 0x04u || op == 0x0cu || op == 0x24u || op == 0x2cu ||
        op == 0x34u || op == 0x3cu) {
        if (avail < 2u) return 0;
        o->kind = G_ALU8_RI; o->width = 8u; o->dst = 0u;
        o->alu = (uint8_t)((op >> 3) & 7u); o->imm = q[1];
        return 1;
    }
    if (op == 0x05u || op == 0x0du || op == 0x25u || op == 0x2du ||
        op == 0x35u || op == 0x3du) {
        if (avail < 3u) return 0;
        o->kind = G_ALU16_RI; o->width = 16u; o->dst = MD_X86_AX;
        o->alu = (uint8_t)((op >> 3) & 7u);
        o->imm = (uint16_t)((uint16_t)q[1] | ((uint16_t)q[2] << 8));
        return 1;
    }

    if ((op == 0x80u || op == 0x81u || op == 0x83u) && d->has_modrm &&
        (d->modrm >> 6) == 3u) {
        const unsigned alu = (d->modrm >> 3) & 7u;
        if (!(alu == 0u || alu == 1u || alu == 4u || alu == 5u ||
              alu == 6u || alu == 7u)) return 0;
        o->alu = (uint8_t)alu; o->dst = (uint8_t)(d->modrm & 7u);
        if (op == 0x80u) {
            if (avail < 3u) return 0;
            o->kind = G_ALU8_RI; o->width = 8u; o->imm = q[2];
        } else if (op == 0x81u) {
            if (avail < 4u) return 0;
            o->kind = G_ALU16_RI; o->width = 16u;
            o->imm = (uint16_t)((uint16_t)q[2] | ((uint16_t)q[3] << 8));
        } else {
            if (avail < 3u) return 0;
            o->kind = G_ALU16_RI; o->width = 16u;
            o->imm = (uint16_t)(int16_t)(int8_t)q[2];
        }
        return 1;
    }

    if (op == 0xa8u && avail >= 2u) {
        o->kind = G_TEST8_RI; o->dst = 0u; o->width = 8u; o->imm = q[1];
        return 1;
    }
    if (op == 0xa9u && avail >= 3u) {
        o->kind = G_TEST16_RI; o->dst = MD_X86_AX; o->width = 16u;
        o->imm = (uint16_t)((uint16_t)q[1] | ((uint16_t)q[2] << 8));
        return 1;
    }
    if ((op == 0xf6u || op == 0xf7u) && d->has_modrm &&
        (d->modrm >> 6) == 3u && ((d->modrm >> 3) & 7u) == 0u) {
        o->dst = (uint8_t)(d->modrm & 7u);
        if (op == 0xf6u) {
            if (avail < 3u) return 0;
            o->kind = G_TEST8_RI; o->width = 8u; o->imm = q[2];
        } else {
            if (avail < 4u) return 0;
            o->kind = G_TEST16_RI; o->width = 16u;
            o->imm = (uint16_t)((uint16_t)q[2] | ((uint16_t)q[3] << 8));
        }
        return 1;
    }

    if ((op & 0xf8u) == 0x40u) {
        o->kind = G_INC16; o->dst = (uint8_t)(op & 7u); o->width = 16u; return 1;
    }
    if ((op & 0xf8u) == 0x48u) {
        o->kind = G_DEC16; o->dst = (uint8_t)(op & 7u); o->width = 16u; return 1;
    }
    if (op == 0xf7u && d->has_modrm && (d->modrm >> 6) == 3u) {
        const unsigned ext = (d->modrm >> 3) & 7u;
        if (ext == 2u) { o->kind = G_NOT16; o->dst = (uint8_t)(d->modrm & 7u); return 1; }
        if (ext == 3u) { o->kind = G_NEG16; o->dst = (uint8_t)(d->modrm & 7u); return 1; }
        return 0;
    }

    if ((op & 0xf0u) == 0x70u && d->flow == MD_DECODE_FLOW_CONDITIONAL) {
        o->kind = G_JCC; o->cc = (uint8_t)(op & 15u); return 1;
    }
    if (op >= 0xe0u && op <= 0xe2u && d->flow == MD_DECODE_FLOW_CONDITIONAL) {
        o->kind = G_LOOP; o->cc = (uint8_t)(op - 0xe0u); return 1;
    }
    if (op == 0xe3u && d->flow == MD_DECODE_FLOW_CONDITIONAL) {
        o->kind = G_JCXZ; return 1;
    }
    if (op == 0xebu && d->flow == MD_DECODE_FLOW_JUMP) {
        o->kind = G_JMP; return 1;
    }
    if (op == 0x90u) { o->kind = G_NOP; return 1; }

    return 0;
}

static int g_find_ip(const GOp *ops, unsigned n, uint16_t ip)
{
    unsigned i;
    for (i = 0u; i < n; ++i) if (ops[i].ip == ip) return (int)i;
    return -1;
}

static uint8_t g_write_mask(const GOp *o)
{
    switch ((GKind)o->kind) {
        case G_MOV_R16_IMM: case G_MOV_RR16: case G_LOAD16:
        case G_INC16: case G_DEC16: case G_NOT16: case G_NEG16:
            return (uint8_t)(1u << (o->dst & 7u));
        case G_MOV_R8_IMM: case G_MOV_RR8: case G_LOAD8:
            return (uint8_t)(1u << r8_parent(o->dst));
        case G_ALU16_RR: case G_ALU16_RI: case G_ALU16_RM:
            return o->alu == 7u ? 0u : (uint8_t)(1u << (o->dst & 7u));
        case G_ALU8_RR: case G_ALU8_RI: case G_ALU8_RM:
            return o->alu == 7u ? 0u : (uint8_t)(1u << r8_parent(o->dst));
        case G_LOOP:
            return (uint8_t)(1u << MD_X86_CX);
        default:
            return 0u;
    }
}

static int g_is_producer(const GOp *o)
{
    switch ((GKind)o->kind) {
        case G_ALU16_RR: case G_ALU16_RI: case G_ALU16_RM:
        case G_ALU8_RR: case G_ALU8_RI: case G_ALU8_RM:
        case G_TEST16_RI: case G_TEST8_RI:
        case G_INC16: case G_DEC16: case G_NEG16:
            return 1;
        default:
            return 0;
    }
}

static uint8_t g_required_mask(const GOp *p)
{
    uint8_t m = 0u;
    switch ((GKind)p->kind) {
        case G_ALU16_RR:
            m |= (uint8_t)(1u << (p->dst & 7u));
            if (p->alu == 0u || p->alu == 5u || p->alu == 7u)
                m |= (uint8_t)(1u << (p->src & 7u));
            break;
        case G_ALU16_RI:
        case G_TEST16_RI:
        case G_INC16: case G_DEC16:
            m |= (uint8_t)(1u << (p->dst & 7u));
            break;
        case G_ALU8_RR:
            m |= (uint8_t)(1u << r8_parent(p->dst));
            if (p->alu == 0u || p->alu == 5u || p->alu == 7u)
                m |= (uint8_t)(1u << r8_parent(p->src));
            break;
        case G_ALU8_RI:
        case G_TEST8_RI:
            m |= (uint8_t)(1u << r8_parent(p->dst));
            break;
        default:
            break;
    }
    return m;
}

static int g_recipe_from_producer(const GOp *p, GRecipe *r)
{
    memset(r, 0, sizeof(*r));
    if (p == NULL) { r->kind = GR_PRESERVE; return 1; }
    r->dst = p->dst; r->src = p->src; r->imm = p->imm;
    switch ((GKind)p->kind) {
        case G_ALU16_RR:
            if ((p->alu == 0u || p->alu == 5u) && p->dst == p->src)
                return 0;
            r->kind = (uint8_t)(p->alu == 0u ? GR_ADD16_RR :
                                p->alu == 5u ? GR_SUB16_RR :
                                p->alu == 7u ? GR_CMP16_RR :
                                (p->alu == 1u || p->alu == 4u || p->alu == 6u) ? GR_LOGIC16_R : 0u);
            return r->kind != 0u;
        case G_ALU16_RI:
            r->kind = (uint8_t)(p->alu == 0u ? GR_ADD16_RI :
                                p->alu == 5u ? GR_SUB16_RI :
                                p->alu == 7u ? GR_CMP16_RI :
                                (p->alu == 1u || p->alu == 4u || p->alu == 6u) ? GR_LOGIC16_R : 0u);
            return r->kind != 0u;
        case G_TEST16_RI: r->kind = GR_TEST16_RI; return 1;
        case G_ALU8_RR:
            if ((p->alu == 0u || p->alu == 5u) && p->dst == p->src)
                return 0;
            r->kind = (uint8_t)(p->alu == 0u ? GR_ADD8_RR :
                                p->alu == 5u ? GR_SUB8_RR :
                                p->alu == 7u ? GR_CMP8_RR :
                                (p->alu == 1u || p->alu == 4u || p->alu == 6u) ? GR_LOGIC8_R : 0u);
            return r->kind != 0u;
        case G_ALU8_RI:
            r->kind = (uint8_t)(p->alu == 0u ? GR_ADD8_RI :
                                p->alu == 5u ? GR_SUB8_RI :
                                p->alu == 7u ? GR_CMP8_RI :
                                (p->alu == 1u || p->alu == 4u || p->alu == 6u) ? GR_LOGIC8_R : 0u);
            return r->kind != 0u;
        case G_TEST8_RI: r->kind = GR_TEST8_RI; return 1;
        case G_INC16: r->kind = GR_INC16; return 1;
        case G_DEC16: r->kind = GR_DEC16; return 1;
        default: return 0;
    }
}

static int g_cond_for(unsigned recipe_kind, unsigned cc)
{
    static const int8_t sub[16] = {G_VS,G_VC,G_CC,G_CS,G_EQ,G_NE,G_LS,G_HI,G_MI,G_PL,-1,-1,G_LT,G_GE,G_LE,G_GT};
    static const int8_t add[16] = {G_VS,G_VC,G_CS,G_CC,G_EQ,G_NE,-1,-1,G_MI,G_PL,-1,-1,G_LT,G_GE,G_LE,G_GT};
    static const int8_t incdec[16] = {G_VS,G_VC,-1,-1,G_EQ,G_NE,-1,-1,G_MI,G_PL,-1,-1,G_LT,G_GE,G_LE,G_GT};
    cc &= 15u;
    switch ((GRecipeKind)recipe_kind) {
        case GR_ADD16_RR: case GR_ADD16_RI: case GR_ADD8_RR: case GR_ADD8_RI:
            return add[cc];
        case GR_INC16: case GR_DEC16:
            return incdec[cc];
        case GR_SUB16_RR: case GR_SUB16_RI: case GR_CMP16_RR: case GR_CMP16_RI:
        case GR_LOGIC16_R: case GR_TEST16_RI:
        case GR_SUB8_RR: case GR_SUB8_RI: case GR_CMP8_RR: case GR_CMP8_RI:
        case GR_LOGIC8_R: case GR_TEST8_RI:
            return sub[cc];
        default:
            return -1;
    }
}

/* One-iteration producer dataflow. Backedges to op0 deliberately do not feed
 * the entry state: a G-1 flag-consuming branch must have a producer in the
 * current iteration. Merges with different producers are rejected. */
static int g_flag_dataflow(const GOp *ops, unsigned n,
                           int16_t *in_prod, uint8_t *in_clobber)
{
    uint8_t seen[NV2G_MAX_OPS];
    unsigned changed = 1u, pass = 0u;
    unsigned i;
    memset(seen, 0, sizeof(seen));
    for (i = 0u; i < n; ++i) { in_prod[i] = -2; in_clobber[i] = 0u; }
    in_prod[0] = -1; seen[0] = 1u;

    while (changed && pass++ < NV2G_MAX_OPS * 2u) {
        changed = 0u;
        for (i = 0u; i < n; ++i) {
            int16_t prod;
            uint8_t clob;
            int succ[2] = {-1,-1};
            unsigned ns = 0u, k;
            if (!seen[i]) continue;
            prod = in_prod[i]; clob = in_clobber[i];
            if (g_is_producer(&ops[i])) { prod = (int16_t)i; clob = 0u; }
            else clob |= g_write_mask(&ops[i]);

            switch ((GKind)ops[i].kind) {
                case G_JCC: case G_JCXZ: case G_LOOP:
                    if (!ops[i].external && !ops[i].latch) succ[ns++] = g_find_ip(ops,n,ops[i].target);
                    if (i + 1u < n) succ[ns++] = (int)(i + 1u);
                    break;
                case G_JMP:
                    if (!ops[i].external && !ops[i].latch) succ[ns++] = g_find_ip(ops,n,ops[i].target);
                    break;
                default:
                    if (i + 1u < n) succ[ns++] = (int)(i + 1u);
                    break;
            }

            for (k = 0u; k < ns; ++k) {
                const int s = succ[k];
                if (s < 0) return 0;
                if (!seen[s]) {
                    seen[s] = 1u; in_prod[s] = prod; in_clobber[s] = clob; changed = 1u;
                } else {
                    if (in_prod[s] != prod) return 0;
                    if ((uint8_t)(in_clobber[s] | clob) != in_clobber[s]) {
                        in_clobber[s] |= clob; changed = 1u;
                    }
                }
            }
        }
    }
    return 1;
}

static int g_exit_recipe_ok(const GRecipe *r)
{
    /*
     * INC/DEC preserve the incoming x86 CF. G-1A can fuse their N/Z/V
     * result directly into a branch, but an architectural exit would also
     * need the pre-producer CF. Keep those exits conservative until the
     * G-1 differential harness proves the dedicated carry snapshot path.
     */
    return r->kind != GR_INC16 && r->kind != GR_DEC16;
}

static int g_add_exit(GMeta *m, uint16_t ip, const GOp *producer,
                      uint8_t clobber, uint8_t *tag_out)
{
    GRecipe *r;
    if (m->exit_count >= NV2G_MAX_EXITS) return 0;
    if (producer != NULL && (clobber & g_required_mask(producer)) != 0u) return 0;
    r = &m->exits[m->exit_count];
    if (!g_recipe_from_producer(producer, r) || !g_exit_recipe_ok(r)) return 0;
    r->ip = ip;
    *tag_out = m->exit_count++;
    return 1;
}

/* -------------------------------------------------------------------------
 * Emission helpers.
 * ------------------------------------------------------------------------- */

static void emit_ea(TBuf *b, const GMem *m, unsigned width,
                    size_t guard_patch_at[1], int *needs_guard,
                    unsigned ssoff)
{
    unsigned a = 0xffu, c = 0xffu;
    *needs_guard = 0;
    if (m->is_direct) {
        tmovw(b, 12u, m->direct);
    } else {
        switch (m->rm & 7u) {
            case 0u: a = MD_X86_BX; c = MD_X86_SI; break;
            case 1u: a = MD_X86_BX; c = MD_X86_DI; break;
            case 2u: a = MD_X86_BP; c = MD_X86_SI; break;
            case 3u: a = MD_X86_BP; c = MD_X86_DI; break;
            case 4u: a = MD_X86_SI; break;
            case 5u: a = MD_X86_DI; break;
            case 6u: a = MD_X86_BP; break;
            case 7u: a = MD_X86_BX; break;
        }
        t16(b, tmovhi(12u, kArmReg[a]));
        if (c != 0xffu) tadd_reg(b, 12u, 12u, kArmReg[c]);
        if (m->disp != 0) {
            const unsigned mag = (unsigned)(m->disp < 0 ? -m->disp : m->disp);
            if (mag <= 255u) {
                if (m->disp > 0) tadd_imm(b, 12u, 12u, mag);
                else tsub_imm(b, 12u, 12u, mag);
            } else {
                tmovw(b, 14u, (uint16_t)mag);
                if (m->disp > 0) tadd_reg(b, 12u, 12u, 14u);
                else tsub_reg(b, 12u, 12u, 14u);
            }
        }
        tuxth_any(b, 12u);
    }

    if (width == 16u) {
        tmovw(b, 14u, 0xffffu);
        tcmp_reg_shift(b, 12u, 14u, 0u);
        guard_patch_at[0] = tbcc(b, G_EQ);
        *needs_guard = 1;
    }

    if (m->uses_ss) {
        tldrh_w_imm(b, 14u, 8u, ssoff);
        tshift(b, 0u, 14u, 14u, 4u);
        tadd_reg(b, 12u, 12u, 14u);
    } else {
        tadd_reg(b, 12u, 12u, 10u);
    }
    twrap20(b, 12u);
}

static void emit_budget_dec(TBuf *b) { tsub_imm(b, 11u, 11u, 1u); }

static void emit_recipe_flags(TBuf *b, const GRecipe *r)
{
    unsigned dst;

    if (r->kind == GR_PRESERVE)
        return;

    /*
     * Rebuild the x86 producer in a scratch register, shifted so the ARM
     * N/Z/C/V bits describe the 8086 operand width. The destination guest
     * register already contains the producer result, so ADD/SUB recipes
     * recover the original left operand from result +/- right operand.
     */
    switch ((GRecipeKind)r->kind) {
        case GR_ADD16_RR:
        case GR_SUB16_RR:
        case GR_CMP16_RR:
            dst = kArmReg[r->dst & 7u];
            t16(b, tmovhi(12u, dst));
            t16(b, tmovhi(14u, kArmReg[r->src & 7u]));
            if (r->kind == GR_ADD16_RR)
                tsub_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB16_RR)
                tadd_reg(b, 12u, 12u, 14u);
            tshift(b, 0u, 12u, 12u, 16u);
            tshift(b, 0u, 14u, 14u, 16u);
            if (r->kind == GR_ADD16_RR)
                tadds_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB16_RR)
                tsubs_reg(b, 12u, 12u, 14u);
            else
                tcmp_reg_shift(b, 12u, 14u, 0u);
            return;

        case GR_ADD16_RI:
        case GR_SUB16_RI:
        case GR_CMP16_RI:
            dst = kArmReg[r->dst & 7u];
            t16(b, tmovhi(12u, dst));
            tmovw(b, 14u, r->imm);
            if (r->kind == GR_ADD16_RI)
                tsub_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB16_RI)
                tadd_reg(b, 12u, 12u, 14u);
            tshift(b, 0u, 12u, 12u, 16u);
            tshift(b, 0u, 14u, 14u, 16u);
            if (r->kind == GR_ADD16_RI)
                tadds_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB16_RI)
                tsubs_reg(b, 12u, 12u, 14u);
            else
                tcmp_reg_shift(b, 12u, 14u, 0u);
            return;

        case GR_LOGIC16_R:
            t16(b, tmovhi(12u, kArmReg[r->dst & 7u]));
            tshift(b, 0u, 12u, 12u, 16u);
            tcmp_imm(b, 12u, 0u);
            return;

        case GR_TEST16_RI:
            t16(b, tmovhi(12u, kArmReg[r->dst & 7u]));
            tmovw(b, 14u, r->imm);
            tand_reg(b, 12u, 12u, 14u);
            tshift(b, 0u, 12u, 12u, 16u);
            tcmp_imm(b, 12u, 0u);
            return;

        case GR_INC16:
        case GR_DEC16:
            t16(b, tmovhi(12u, kArmReg[r->dst & 7u]));
            if (r->kind == GR_INC16)
                tsub_imm(b, 12u, 12u, 1u);
            else
                tadd_imm(b, 12u, 12u, 1u);
            tshift(b, 0u, 12u, 12u, 16u);
            tmovw(b, 14u, 1u);
            tshift(b, 0u, 14u, 14u, 16u);
            if (r->kind == GR_INC16)
                tadds_reg(b, 12u, 12u, 14u);
            else
                tsubs_reg(b, 12u, 12u, 14u);
            return;

        case GR_ADD8_RR:
        case GR_SUB8_RR:
        case GR_CMP8_RR:
            emit_r8_to(b, 12u, r->dst);
            emit_r8_to(b, 14u, r->src);
            if (r->kind == GR_ADD8_RR)
                tsub_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB8_RR)
                tadd_reg(b, 12u, 12u, 14u);
            tshift(b, 0u, 12u, 12u, 24u);
            tshift(b, 0u, 14u, 14u, 24u);
            if (r->kind == GR_ADD8_RR)
                tadds_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB8_RR)
                tsubs_reg(b, 12u, 12u, 14u);
            else
                tcmp_reg_shift(b, 12u, 14u, 0u);
            return;

        case GR_ADD8_RI:
        case GR_SUB8_RI:
        case GR_CMP8_RI:
            emit_r8_to(b, 12u, r->dst);
            tmovw(b, 14u, (uint16_t)(r->imm & 0xffu));
            if (r->kind == GR_ADD8_RI)
                tsub_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB8_RI)
                tadd_reg(b, 12u, 12u, 14u);
            tshift(b, 0u, 12u, 12u, 24u);
            tshift(b, 0u, 14u, 14u, 24u);
            if (r->kind == GR_ADD8_RI)
                tadds_reg(b, 12u, 12u, 14u);
            else if (r->kind == GR_SUB8_RI)
                tsubs_reg(b, 12u, 12u, 14u);
            else
                tcmp_reg_shift(b, 12u, 14u, 0u);
            return;

        case GR_LOGIC8_R:
            emit_r8_to(b, 12u, r->dst);
            tshift(b, 0u, 12u, 12u, 24u);
            tcmp_imm(b, 12u, 0u);
            return;

        case GR_TEST8_RI:
            emit_r8_to(b, 12u, r->dst);
            tmovw(b, 14u, (uint16_t)(r->imm & 0xffu));
            tand_reg(b, 12u, 12u, 14u);
            tshift(b, 0u, 12u, 12u, 24u);
            tcmp_imm(b, 12u, 0u);
            return;

        default:
            b->failed = 1;
            return;
    }
}

static void emit_store_regs(TBuf *b, unsigned roff)
{
    unsigned i;
    for (i = 0u; i < 8u; ++i)
        tstrh_w_imm(b, kArmReg[i], 8u, roff + i * 2u);
}

static void emit_return_tag(TBuf *b, unsigned roff, unsigned tag)
{
    emit_store_regs(b, roff);
    t16(b, tmovhi(0u,11u));
    tmovw(b,12u,(uint16_t)tag);
    tshift(b,0u,12u,12u,24u);
    torr_reg(b,0u,0u,12u);
    t16(b,0xBD00u); /* pop {pc}; LR was saved at region entry */
}

static int emit_simple_alu(TBuf *b, const GOp *o)
{
    unsigned dst, src;
    if (o->width == 16u) {
        dst = kArmReg[o->dst & 7u]; src = kArmReg[o->src & 7u];
        if (o->kind == G_ALU16_RR) {
            switch (o->alu) {
                case 0u: tadd_reg(b,dst,dst,src); break;
                case 1u: torr_reg(b,dst,dst,src); break;
                case 4u: tand_reg(b,dst,dst,src); break;
                case 5u: tsub_reg(b,dst,dst,src); break;
                case 6u: teor_reg(b,dst,dst,src); break;
                case 7u: break;
                default: return 0;
            }
            if (o->alu != 7u) tuxth_any(b, dst);
            return 1;
        }
        if (o->kind == G_ALU16_RI) {
            tmovw(b,14u,o->imm);
            switch (o->alu) {
                case 0u: tadd_reg(b,dst,dst,14u); break;
                case 1u: torr_reg(b,dst,dst,14u); break;
                case 4u: tand_reg(b,dst,dst,14u); break;
                case 5u: tsub_reg(b,dst,dst,14u); break;
                case 6u: teor_reg(b,dst,dst,14u); break;
                case 7u: break;
                default: return 0;
            }
            if (o->alu != 7u) tuxth_any(b, dst);
            return 1;
        }
    } else {
        emit_r8_to(b,12u,o->dst);
        if (o->kind == G_ALU8_RR) emit_r8_to(b,14u,o->src);
        else tmovw(b,14u,(uint16_t)(o->imm & 0xffu));
        switch (o->alu) {
            case 0u: tadd_reg(b,12u,12u,14u); break;
            case 1u: torr_reg(b,12u,12u,14u); break;
            case 4u: tand_reg(b,12u,12u,14u); break;
            case 5u: tsub_reg(b,12u,12u,14u); break;
            case 6u: teor_reg(b,12u,12u,14u); break;
            case 7u: return 1;
            default: return 0;
        }
        emit_r8_from(b,o->dst,12u);
        return 1;
    }
    return 0;
}

static MdNativeV2Status g_emit(GOp *ops, unsigned n, uint16_t entry_ip,
                               uint16_t end_ip, GMeta *m, MdNativeV2Code *out)
{
    TBuf b;
    size_t op_at[NV2G_MAX_OPS], exit_at[NV2G_MAX_EXITS], budget_at;
    GPatch patch[64];
    unsigned np = 0u, i;
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned ssoff = (unsigned)offsetof(MdX86, ss);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    size_t loop_top;

    memset(out, 0, sizeof(*out));
    b.p = out->bytes;
    b.cap = sizeof(out->bytes);
    b.at = 0u;
    b.failed = 0;

    /* Save LR so r14 can be a second scratch register inside the region. */
    t16(&b, 0xB500u); /* push {lr} */
    t16(&b, tmovhi(8u, 0u));
    t16(&b, tmovhi(11u, 2u)); /* r2 = scheduler budget */
    for (i = 1u; i < 8u; ++i)
        tldrh_w_imm(&b, kArmReg[i], 8u, roff + i * 2u);
    if (m->needs_memory)
        tldr_w_imm(&b, 9u, 8u, moff);
    if (m->needs_memory) {
        tldrh_w_imm(&b, 10u, 8u, dsoff);
        tshift(&b, 0u, 10u, 10u, 4u);
    }
    tldrh_w_imm(&b, 0u, 8u, roff);

    /*
     * G-1A spends one budget unit per actually executed guest instruction.
     * The conservative header check prevents a partial iteration: max_ops is
     * an upper bound on every path through this single natural loop.
     */
    loop_top = b.at;
    tcmp_imm(&b, 11u, m->max_ops);
    if (np >= 64u) return MD_NATIVE_V2_TOO_LARGE;
    patch[np].at = tbcc(&b, G_CC); /* budget < max_ops */
    patch[np].cond = G_CC;
    patch[np].is_exit = 1u;
    patch[np].exit_tag = NV2G_TAG_BUDGET;
    patch[np].target_ip = entry_ip;
    ++np;

    for (i = 0u; i < n && !b.failed; ++i) {
        GOp *o = &ops[i];
        op_at[i] = b.at;

        switch ((GKind)o->kind) {
            case G_MOV_R16_IMM:
                tmovw(&b, kArmReg[o->dst & 7u], o->imm);
                break;

            case G_MOV_R8_IMM:
                tmovw(&b, 12u, (uint16_t)(o->imm & 0xffu));
                emit_r8_from(&b, o->dst, 12u);
                break;

            case G_MOV_RR16:
                t16(&b, tmovhi(kArmReg[o->dst & 7u], kArmReg[o->src & 7u]));
                break;

            case G_MOV_RR8:
                emit_r8_to(&b, 12u, o->src);
                emit_r8_from(&b, o->dst, 12u);
                break;

            case G_LOAD16:
            case G_LOAD8:
            case G_ALU16_RM:
            case G_ALU8_RM: {
                size_t guard_at = 0u;
                int needs_guard = 0;
                const unsigned w =
                    (o->kind == G_LOAD16 || o->kind == G_ALU16_RM) ? 16u : 8u;

                emit_ea(&b, &o->mem, w, &guard_at, &needs_guard, ssoff);
                if (needs_guard) {
                    if (o->guard_tag == 0xffu || np >= 64u)
                        return MD_NATIVE_V2_UNSUPPORTED;
                    patch[np].at = guard_at;
                    patch[np].cond = G_EQ;
                    patch[np].is_exit = 1u;
                    patch[np].exit_tag = o->guard_tag;
                    patch[np].target_ip = o->ip;
                    ++np;
                }

                if (w == 16u)
                    tldrh_w_reg(&b, 14u, 9u, 12u);
                else
                    tldrb_w_reg(&b, 14u, 9u, 12u);

                if (o->kind == G_LOAD16) {
                    t16(&b, tmovhi(kArmReg[o->dst & 7u], 14u));
                } else if (o->kind == G_LOAD8) {
                    emit_r8_from(&b, o->dst, 14u);
                } else if (o->kind == G_ALU16_RM) {
                    const unsigned d = kArmReg[o->dst & 7u];
                    switch (o->alu) {
                        case 0u: tadd_reg(&b, d, d, 14u); break;
                        case 1u: torr_reg(&b, d, d, 14u); break;
                        case 4u: tand_reg(&b, d, d, 14u); break;
                        case 5u: tsub_reg(&b, d, d, 14u); break;
                        case 6u: teor_reg(&b, d, d, 14u); break;
                        case 7u: break; /* CMP: value unchanged */
                        default: return MD_NATIVE_V2_UNSUPPORTED;
                    }
                    if (o->alu != 7u)
                        tuxth_any(&b, d);
                } else {
                    emit_r8_to(&b, 12u, o->dst);
                    switch (o->alu) {
                        case 0u: tadd_reg(&b, 12u, 12u, 14u); break;
                        case 1u: torr_reg(&b, 12u, 12u, 14u); break;
                        case 4u: tand_reg(&b, 12u, 12u, 14u); break;
                        case 5u: tsub_reg(&b, 12u, 12u, 14u); break;
                        case 6u: teor_reg(&b, 12u, 12u, 14u); break;
                        case 7u: break;
                        default: return MD_NATIVE_V2_UNSUPPORTED;
                    }
                    if (o->alu != 7u)
                        emit_r8_from(&b, o->dst, 12u);
                }
                break;
            }

            case G_ALU16_RR:
            case G_ALU16_RI:
            case G_ALU8_RR:
            case G_ALU8_RI:
                if (!emit_simple_alu(&b, o))
                    return MD_NATIVE_V2_UNSUPPORTED;
                break;

            case G_TEST16_RI:
            case G_TEST8_RI:
                /* No architectural value changes; flags are fused on demand. */
                break;

            case G_INC16:
                tadd_imm(&b, kArmReg[o->dst & 7u], kArmReg[o->dst & 7u], 1u);
                tuxth_any(&b, kArmReg[o->dst & 7u]);
                break;

            case G_DEC16:
                tsub_imm(&b, kArmReg[o->dst & 7u], kArmReg[o->dst & 7u], 1u);
                tuxth_any(&b, kArmReg[o->dst & 7u]);
                break;

            case G_NOT16:
                tmvn_reg(&b, kArmReg[o->dst & 7u], kArmReg[o->dst & 7u]);
                tuxth_any(&b, kArmReg[o->dst & 7u]);
                break;

            case G_NEG16:
                trsb0(&b, kArmReg[o->dst & 7u], kArmReg[o->dst & 7u]);
                tuxth_any(&b, kArmReg[o->dst & 7u]);
                break;

            case G_JCC:
            case G_JMP:
            case G_LOOP:
            case G_JCXZ:
            case G_NOP:
                break;

            default:
                return MD_NATIVE_V2_UNSUPPORTED;
        }

        /* This guest instruction has now completed. */
        emit_budget_dec(&b);

        switch ((GKind)o->kind) {
            case G_JCC:
                emit_recipe_flags(&b, &o->branch_recipe);
                if (b.failed || np >= 64u)
                    return MD_NATIVE_V2_UNSUPPORTED;
                patch[np].at = tbcc(&b, o->arm_cond);
                patch[np].cond = o->arm_cond;
                patch[np].is_exit = o->external;
                patch[np].exit_tag = o->exit_tag;
                patch[np].target_ip = o->target;
                ++np;
                break;

            case G_JMP:
                if (np >= 64u) return MD_NATIVE_V2_TOO_LARGE;
                patch[np].at = tb(&b);
                patch[np].cond = 0xffu;
                patch[np].is_exit = o->external;
                patch[np].exit_tag = o->exit_tag;
                patch[np].target_ip = o->target;
                ++np;
                break;

            case G_JCXZ:
                tcmp_imm(&b, kArmReg[MD_X86_CX], 0u);
                if (np >= 64u) return MD_NATIVE_V2_TOO_LARGE;
                patch[np].at = tbcc(&b, G_EQ);
                patch[np].cond = G_EQ;
                patch[np].is_exit = o->external;
                patch[np].exit_tag = o->exit_tag;
                patch[np].target_ip = o->target;
                ++np;
                break;

            case G_LOOP:
                tsub_imm(&b, kArmReg[MD_X86_CX], kArmReg[MD_X86_CX], 1u);
                tuxth_any(&b, kArmReg[MD_X86_CX]);
                tcmp_imm(&b, kArmReg[MD_X86_CX], 0u);

                if (o->cc == 2u) { /* LOOP */
                    if (np >= 64u) return MD_NATIVE_V2_TOO_LARGE;
                    patch[np].at = tbcc(&b, G_NE);
                    patch[np].cond = G_NE;
                    patch[np].is_exit = o->external;
                    patch[np].exit_tag = o->exit_tag;
                    patch[np].target_ip = o->target;
                    ++np;
                } else {
                    /* LOOPZ/LOOPNZ: CX==0 always falls through. */
                    if (np + 2u > 64u) return MD_NATIVE_V2_TOO_LARGE;
                    patch[np].at = tbcc(&b, G_EQ);
                    patch[np].cond = G_EQ;
                    patch[np].is_exit = o->latch ? 1u : 0u;
                    patch[np].exit_tag = o->fall_tag;
                    patch[np].target_ip = o->next_ip;
                    ++np;

                    emit_recipe_flags(&b, &o->branch_recipe);
                    if (b.failed) return MD_NATIVE_V2_UNSUPPORTED;
                    patch[np].at = tbcc(&b, o->cc == 1u ? G_EQ : G_NE);
                    patch[np].cond = (uint8_t)(o->cc == 1u ? G_EQ : G_NE);
                    patch[np].is_exit = o->external;
                    patch[np].exit_tag = o->exit_tag;
                    patch[np].target_ip = o->target;
                    ++np;
                }
                break;

            default:
                break;
        }
    }

    /* Conditional latches have a fall-through architectural exit. */
    if (ops[n - 1u].kind != G_JMP) {
        if (ops[n - 1u].fall_tag >= m->exit_count)
            return MD_NATIVE_V2_UNSUPPORTED;
        if (np >= 64u)
            return MD_NATIVE_V2_TOO_LARGE;
        patch[np].at = tb(&b);
        patch[np].cond = 0xffu;
        patch[np].is_exit = 1u;
        patch[np].exit_tag = ops[n - 1u].fall_tag;
        patch[np].target_ip = end_ip;
        ++np;
    }

    for (i = 0u; i < m->exit_count; ++i) {
        exit_at[i] = b.at;
        emit_return_tag(&b, roff, i);
    }
    budget_at = b.at;
    emit_return_tag(&b, roff, NV2G_TAG_BUDGET);

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    for (i = 0u; i < np; ++i) {
        size_t target;
        if (patch[i].is_exit) {
            if (patch[i].exit_tag == NV2G_TAG_BUDGET) {
                target = budget_at;
            } else {
                if (patch[i].exit_tag >= m->exit_count)
                    return MD_NATIVE_V2_BRANCH_RANGE;
                target = exit_at[patch[i].exit_tag];
            }
        } else if (patch[i].target_ip == entry_ip) {
            target = loop_top;
        } else {
            const int ti = g_find_ip(ops, n, patch[i].target_ip);
            if (ti < 0)
                return MD_NATIVE_V2_BRANCH_RANGE;
            target = op_at[(unsigned)ti];
        }

        if (patch[i].cond == 0xffu) {
            if (!tpatch_b(&b, patch[i].at, target))
                return MD_NATIVE_V2_BRANCH_RANGE;
        } else if (!tpatch_bcc(&b, patch[i].at, patch[i].cond, target)) {
            return MD_NATIVE_V2_BRANCH_RANGE;
        }
    }

    out->_align_word = 0x4E563247u; /* NV2G */
    out->size = (uint16_t)b.at;
    out->op_count = (uint16_t)n;
    out->start_ip = entry_ip;
    out->end_ip = end_ip;
    out->has_local_loop = 1u;
    out->phase = NV2G_PHASE_G1;
    out->needs_memory = m->needs_memory;
    out->has_store = 0u;
    out->dynamic_retire = 3u;
    out->retire_base_ops = 0u;
    memcpy(out->g_meta, m, sizeof(*m));
    return MD_NATIVE_V2_OK;
}

/* -------------------------------------------------------------------------
 * Public compiler.
 *
 * G-1 first cut deliberately supports the general region model, budget ABI,
 * multi-exit CFG, full non-parity Jcc analysis, and general ModR/M loads.
 * The emitter currently admits JCC only when it can be fused by the narrow
 * branch path below; LOOPZ/NZ and INC/DEC-fed branches remain conservative
 * rejects until the qemu differential harness is in place. Existing special
 * compilers still own all previously-fast MDSTRESS shapes.
 * ------------------------------------------------------------------------- */

const MdNativeV2GStats *md_native_v2g_stats(void)
{
    return &g_nv2g_stats;
}

MdNativeV2Status md_native_v2g_compile_loop(const uint8_t *image,
                                             size_t max_size,
                                             uint16_t entry_ip,
                                             MdNativeV2Code *out,
                                             size_t *guest_size_out)
{
    GOp ops[NV2G_MAX_OPS];
    int16_t in_prod[NV2G_MAX_OPS];
    uint8_t in_clob[NV2G_MAX_OPS];
    GMeta m;
    unsigned n = 0u, i;
    uint16_t ip = entry_ip, end_ip = entry_ip;
    int found_latch = 0;
    int any_producer = 0;

    ++g_nv2g_stats.attempts;

    if (image == NULL || out == NULL || guest_size_out == NULL ||
        max_size < 2u || max_size > 64u) {
        ++g_nv2g_stats.reject_bad_argument;
        return MD_NATIVE_V2_BAD_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));
    memset(&m, 0, sizeof(m));
    m.magic = NV2G_MAGIC;

    /* Decode one natural loop, ending at the first direct latch to entry_ip. */
    while (n < NV2G_MAX_OPS) {
        MdDecodedInstruction d;
        const size_t used = (size_t)(uint16_t)(ip - entry_ip);

        if (used >= max_size) {
            ++g_nv2g_stats.reject_region;
            return MD_NATIVE_V2_UNSUPPORTED;
        }
        if (!md_decode_8086(image, max_size, entry_ip, ip, &d) ||
            !d.valid_8086) {
            ++g_nv2g_stats.reject_decode;
            return MD_NATIVE_V2_DECODE_ERROR;
        }
        if (d.prefix_count != 0u || d.far_control ||
            d.flow == MD_DECODE_FLOW_CALL ||
            d.flow == MD_DECODE_FLOW_INDIRECT_CALL ||
            d.flow == MD_DECODE_FLOW_INDIRECT_JUMP ||
            d.flow == MD_DECODE_FLOW_RETURN ||
            d.flow == MD_DECODE_FLOW_STOP) {
            unsigned pi;
            ++g_nv2g_stats.reject_control;
            ++g_nv2g_stats.reject_control_opcode[d.opcode];

            if (d.prefix_count != 0u) {
                ++g_nv2g_stats.reject_control_prefix;
                for (pi = 0u; pi < d.prefix_count; ++pi)
                    ++g_nv2g_stats.reject_prefix_byte[d.prefixes[pi]];
            } else if (d.far_control) {
                ++g_nv2g_stats.reject_control_far;
            } else {
                switch (d.flow) {
                    case MD_DECODE_FLOW_CALL:
                        ++g_nv2g_stats.reject_control_call;
                        break;
                    case MD_DECODE_FLOW_INDIRECT_CALL:
                        ++g_nv2g_stats.reject_control_indirect_call;
                        break;
                    case MD_DECODE_FLOW_INDIRECT_JUMP:
                        ++g_nv2g_stats.reject_control_indirect_jump;
                        break;
                    case MD_DECODE_FLOW_RETURN:
                        ++g_nv2g_stats.reject_control_return;
                        break;
                    case MD_DECODE_FLOW_STOP:
                        ++g_nv2g_stats.reject_control_stop;
                        break;
                    default:
                        break;
                }
            }
            return MD_NATIVE_V2_UNSUPPORTED;
        }
        if (!g_decode_op(image, max_size, entry_ip, &d, &ops[n])) {
            ++g_nv2g_stats.reject_opcode;
            ++g_nv2g_stats.reject_opcode_byte[d.opcode];
            return MD_NATIVE_V2_UNSUPPORTED;
        }

        if (g_is_producer(&ops[n]))
            any_producer = 1;
        if (ops[n].kind == G_LOAD16 || ops[n].kind == G_LOAD8 ||
            ops[n].kind == G_ALU16_RM || ops[n].kind == G_ALU8_RM) {
            m.needs_memory = 1u;
        }

        ++n;
        ip = d.next_ip;

        if ((d.flow == MD_DECODE_FLOW_CONDITIONAL ||
             d.flow == MD_DECODE_FLOW_JUMP) &&
            d.target == entry_ip) {
            found_latch = 1;
            ops[n - 1u].latch = 1u;
            end_ip = d.next_ip;
            break;
        }

        /* G-1 is one natural loop; nested/earlier backedges stay fallback. */
        if ((d.flow == MD_DECODE_FLOW_CONDITIONAL ||
             d.flow == MD_DECODE_FLOW_JUMP) &&
            d.target < entry_ip) {
            ++g_nv2g_stats.reject_cfg;
            return MD_NATIVE_V2_UNSUPPORTED;
        }
    }

    if (!found_latch || n == 0u) {
        ++g_nv2g_stats.reject_region;
        return MD_NATIVE_V2_UNSUPPORTED;
    }

    *guest_size_out = (size_t)(uint16_t)(end_ip - entry_ip);
    if (*guest_size_out == 0u || *guest_size_out > max_size) {
        ++g_nv2g_stats.reject_region;
        return MD_NATIVE_V2_UNSUPPORTED;
    }

    /* Classify internal CFG edges vs architectural side exits. */
    for (i = 0u; i < n; ++i) {
        if (ops[i].kind == G_JCC || ops[i].kind == G_JCXZ ||
            ops[i].kind == G_LOOP || ops[i].kind == G_JMP) {
            if (ops[i].latch)
                continue;
            if (ops[i].target < entry_ip || ops[i].target >= end_ip) {
                ops[i].external = 1u;
            } else if (g_find_ip(ops, n, ops[i].target) < 0) {
                ++g_nv2g_stats.reject_cfg;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            /* G-1 models conditional side exits; external JMP is not a loop. */
            if (ops[i].kind == G_JMP && ops[i].external) {
                ++g_nv2g_stats.reject_cfg;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
        }
    }

    if (!g_flag_dataflow(ops, n, in_prod, in_clob)) {
        ++g_nv2g_stats.reject_flags;
        return MD_NATIVE_V2_UNSUPPORTED;
    }

    /* Prepare fused condition recipes for every flag-consuming control op. */
    for (i = 0u; i < n; ++i) {
        const GOp *p = in_prod[i] >= 0 ? &ops[(unsigned)in_prod[i]] : NULL;

        if (ops[i].kind == G_JCC) {
            GRecipe rr;
            int cond;
            if (p == NULL || (in_clob[i] & g_required_mask(p)) != 0u ||
                !g_recipe_from_producer(p, &rr)) {
                ++g_nv2g_stats.reject_flags;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            cond = g_cond_for(rr.kind, ops[i].cc);
            if (cond < 0) {
                ++g_nv2g_stats.reject_flags;
                return MD_NATIVE_V2_UNSUPPORTED; /* parity and unfusable flags */
            }
            ops[i].branch_recipe = rr;
            ops[i].arm_cond = (uint8_t)cond;
        } else if (ops[i].kind == G_LOOP && ops[i].cc != 2u) {
            /* LOOPNZ/LOOPZ consume ZF while decrementing CX without changing it. */
            GRecipe rr;
            int cond;
            if (p == NULL || (in_clob[i] & g_required_mask(p)) != 0u ||
                !g_recipe_from_producer(p, &rr)) {
                ++g_nv2g_stats.reject_flags;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            cond = g_cond_for(rr.kind, ops[i].cc == 1u ? 4u : 5u);
            if (cond < 0) {
                ++g_nv2g_stats.reject_flags;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            ops[i].branch_recipe = rr;
            ops[i].arm_cond = (uint8_t)cond;
        }
    }

    /*
     * Allocate exact exit recipes. A 16-bit wrap guard exits before the load,
     * so its retirement/IP are exact. If that guard can occur before the first
     * flag producer of an iteration, a later-iteration exit would need the
     * previous iteration's flags; conservatively reject that case in G-1A.
     */
    for (i = 0u; i < n; ++i) {
        const GOp *p = in_prod[i] >= 0 ? &ops[(unsigned)in_prod[i]] : NULL;
        uint8_t clob = in_clob[i];

        if (ops[i].external) {
            uint8_t tag;
            if (ops[i].kind == G_LOOP)
                clob |= (uint8_t)(1u << MD_X86_CX);
            if (!g_add_exit(&m, ops[i].target, p, clob, &tag)) {
                ++g_nv2g_stats.reject_exits;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            ops[i].exit_tag = tag;
        }

        if (ops[i].kind == G_LOAD16 || ops[i].kind == G_ALU16_RM) {
            uint8_t tag;
            if (p == NULL && any_producer) {
                ++g_nv2g_stats.reject_memory;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            if (!g_add_exit(&m, ops[i].ip, p, in_clob[i], &tag)) {
                ++g_nv2g_stats.reject_memory;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            ops[i].guard_tag = tag;
            if (ops[i].mem.uses_ss)
                m.uses_ss_word = 1u;
            else
                m.uses_ds_word = 1u;
        }
    }

    /* Latch fall-through is the normal region exit; JMP has no fall-through. */
    {
        const unsigned li = n - 1u;
        const GOp *p = in_prod[li] >= 0 ? &ops[(unsigned)in_prod[li]] : NULL;
        uint8_t clob = in_clob[li];

        ops[li].fall_tag = 0xffu;
        if (ops[li].kind == G_LOOP)
            clob |= (uint8_t)(1u << MD_X86_CX);

        if (ops[li].kind != G_JMP) {
            uint8_t tag;
            if (!g_add_exit(&m, end_ip, p, clob, &tag)) {
                ++g_nv2g_stats.reject_exits;
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            ops[li].fall_tag = tag;
        }

        /* Budget exit occurs at the next loop header, after a full iteration. */
        if (p != NULL && (clob & g_required_mask(p)) != 0u) {
            ++g_nv2g_stats.reject_flags;
            return MD_NATIVE_V2_UNSUPPORTED;
        }
        if (!g_recipe_from_producer(p, &m.budget) ||
            !g_exit_recipe_ok(&m.budget)) {
            ++g_nv2g_stats.reject_flags;
            return MD_NATIVE_V2_UNSUPPORTED;
        }
        m.budget.ip = entry_ip;
    }

    /* LOOPZ/NZ CX==0 falls through immediately using the latch exit recipe. */
    for (i = 0u; i < n; ++i) {
        if (ops[i].kind == G_LOOP && ops[i].cc != 2u) {
            if (ops[i].latch) {
                ops[i].fall_tag = ops[n - 1u].fall_tag;
            } else {
                /* Non-latch LOOPZ/NZ simply continues at next_ip when CX==0. */
                ops[i].fall_tag = 0xffu;
            }
        }
    }

    m.max_ops = (uint8_t)n;
    m.magic = NV2G_MAGIC;

    {
        const MdNativeV2Status st =
            g_emit(ops, n, entry_ip, end_ip, &m, out);
        if (st != MD_NATIVE_V2_OK) {
            ++g_nv2g_stats.reject_emit;
            memset(out, 0, sizeof(*out));
            return st;
        }
    }

    out->requires_safe_ds_word = m.uses_ds_word;
    out->requires_safe_ss_word = m.uses_ss_word;
    out->needs_memory = m.needs_memory;
    ++g_nv2g_stats.compiles;
    return MD_NATIVE_V2_OK;
}

/* -------------------------------------------------------------------------
 * Exit flag reconstruction. No C helper is called inside a native loop;
 * recipes run once after a region exits.
 * ------------------------------------------------------------------------- */

#if defined(__arm__) || defined(__thumb__)
static uint8_t g_get8(const MdX86 *cpu,unsigned r){return md_x86_get_reg8(cpu,r&7u);}

static void g_finish_recipe(MdX86 *cpu,const GRecipe *r)
{
    uint16_t a=0,b=0,res=0;
    uint8_t a8=0,b8=0,res8=0;
    switch((GRecipeKind)r->kind){
        case GR_PRESERVE: break;
        case GR_ADD16_RR: res=cpu->r[r->dst&7u];b=cpu->r[r->src&7u];a=(uint16_t)(res-b);md_x86_lazy(cpu,MD_LAZY_ADD16,a,b,res);cpu->lazy_carry=0;break;
        case GR_ADD16_RI: res=cpu->r[r->dst&7u];b=r->imm;a=(uint16_t)(res-b);md_x86_lazy(cpu,MD_LAZY_ADD16,a,b,res);cpu->lazy_carry=0;break;
        case GR_SUB16_RR: res=cpu->r[r->dst&7u];b=cpu->r[r->src&7u];a=(uint16_t)(res+b);md_x86_lazy(cpu,MD_LAZY_SUB16,a,b,res);cpu->lazy_carry=0;break;
        case GR_SUB16_RI: res=cpu->r[r->dst&7u];b=r->imm;a=(uint16_t)(res+b);md_x86_lazy(cpu,MD_LAZY_SUB16,a,b,res);cpu->lazy_carry=0;break;
        case GR_CMP16_RR: a=cpu->r[r->dst&7u];b=cpu->r[r->src&7u];res=(uint16_t)(a-b);md_x86_lazy(cpu,MD_LAZY_SUB16,a,b,res);cpu->lazy_carry=0;break;
        case GR_CMP16_RI: a=cpu->r[r->dst&7u];b=r->imm;res=(uint16_t)(a-b);md_x86_lazy(cpu,MD_LAZY_SUB16,a,b,res);cpu->lazy_carry=0;break;
        case GR_LOGIC16_R: res=cpu->r[r->dst&7u];md_x86_lazy(cpu,MD_LAZY_LOGIC16,0,0,res);cpu->lazy_carry=0;break;
        case GR_TEST16_RI: a=cpu->r[r->dst&7u];res=(uint16_t)(a&r->imm);md_x86_lazy(cpu,MD_LAZY_LOGIC16,0,0,res);cpu->lazy_carry=0;break;
        case GR_ADD8_RR: res8=g_get8(cpu,r->dst);b8=g_get8(cpu,r->src);a8=(uint8_t)(res8-b8);md_x86_lazy(cpu,MD_LAZY_ADD8,a8,b8,res8);cpu->lazy_carry=0;break;
        case GR_ADD8_RI: res8=g_get8(cpu,r->dst);b8=(uint8_t)r->imm;a8=(uint8_t)(res8-b8);md_x86_lazy(cpu,MD_LAZY_ADD8,a8,b8,res8);cpu->lazy_carry=0;break;
        case GR_SUB8_RR: res8=g_get8(cpu,r->dst);b8=g_get8(cpu,r->src);a8=(uint8_t)(res8+b8);md_x86_lazy(cpu,MD_LAZY_SUB8,a8,b8,res8);cpu->lazy_carry=0;break;
        case GR_SUB8_RI: res8=g_get8(cpu,r->dst);b8=(uint8_t)r->imm;a8=(uint8_t)(res8+b8);md_x86_lazy(cpu,MD_LAZY_SUB8,a8,b8,res8);cpu->lazy_carry=0;break;
        case GR_CMP8_RR: a8=g_get8(cpu,r->dst);b8=g_get8(cpu,r->src);res8=(uint8_t)(a8-b8);md_x86_lazy(cpu,MD_LAZY_SUB8,a8,b8,res8);cpu->lazy_carry=0;break;
        case GR_CMP8_RI: a8=g_get8(cpu,r->dst);b8=(uint8_t)r->imm;res8=(uint8_t)(a8-b8);md_x86_lazy(cpu,MD_LAZY_SUB8,a8,b8,res8);cpu->lazy_carry=0;break;
        case GR_LOGIC8_R: res8=g_get8(cpu,r->dst);md_x86_lazy(cpu,MD_LAZY_LOGIC8,0,0,res8);cpu->lazy_carry=0;break;
        case GR_TEST8_RI: a8=g_get8(cpu,r->dst);res8=(uint8_t)(a8&(uint8_t)r->imm);md_x86_lazy(cpu,MD_LAZY_LOGIC8,0,0,res8);cpu->lazy_carry=0;break;
        case GR_INC16: res=cpu->r[r->dst&7u];md_x86_flags_materialize(cpu);{uint8_t cf=(cpu->flags_raw&MD_X86_FLAG_CF)?1u:0u;md_x86_lazy(cpu,MD_LAZY_INC16,(uint16_t)(res-1u),1u,res);cpu->lazy_carry=cf;}break;
        case GR_DEC16: res=cpu->r[r->dst&7u];md_x86_flags_materialize(cpu);{uint8_t cf=(cpu->flags_raw&MD_X86_FLAG_CF)?1u:0u;md_x86_lazy(cpu,MD_LAZY_DEC16,(uint16_t)(res+1u),1u,res);cpu->lazy_carry=cf;}break;
        default: break;
    }
}
#endif

int md_native_v2g_is_code(const MdNativeV2Code *code)
{
    const GMeta *m;
    if(!code||code->dynamic_retire!=3u||code->phase!=NV2G_PHASE_G1)return 0;
    m=(const GMeta *)code->g_meta;
    return m->magic==NV2G_MAGIC;
}

#if defined(__arm__) || defined(__thumb__)
__attribute__((naked,noinline))
static uint32_t g_call_thumb(MdX86 *cpu,uintptr_t entry,uint32_t budget)
{
    (void)cpu;(void)entry;(void)budget;
    __asm volatile(
        "push {r4-r7, lr}\n"
        "sub sp, sp, #20\n"
        "str.w r8,  [sp, #0]\n"
        "str.w r9,  [sp, #4]\n"
        "str.w r10, [sp, #8]\n"
        "str.w r11, [sp, #12]\n"
        "blx r1\n"
        "ldr.w r8,  [sp, #0]\n"
        "ldr.w r9,  [sp, #4]\n"
        "ldr.w r10, [sp, #8]\n"
        "ldr.w r11, [sp, #12]\n"
        "add sp, sp, #20\n"
        "pop {r4-r7, pc}\n");
}
#endif

uint32_t MD_HOT_FUNC(md_native_v2g_execute)(MdX86 *cpu,
                                             const MdNativeV2Code *code,
                                             uint32_t budget)
{
#if defined(__arm__) || defined(__thumb__)
    const GMeta *m;
    uint32_t rc,remaining,tag;
    uintptr_t entry;
    if(!cpu||!code||!md_native_v2g_is_code(code)||code->size==0u||budget==0u||budget>0x00ffffffu)return MD_NATIVE_V2_EXEC_FALLBACK;
    if(code->needs_memory&&!cpu->memory)return MD_NATIVE_V2_EXEC_FALLBACK;
    if(code->requires_safe_ds_word&&cpu->ds>0xefffu)return MD_NATIVE_V2_EXEC_FALLBACK;
    if(code->requires_safe_ss_word&&cpu->ss>0xefffu)return MD_NATIVE_V2_EXEC_FALLBACK;
    m=(const GMeta *)code->g_meta;
    /*
     * The generated loop header's budget exit represents the state after at
     * least one complete iteration and therefore carries the latch producer's
     * FLAGS recipe. If the caller cannot afford even one max-length path,
     * return in C without entering native code: architecturally nothing ran,
     * so registers/FLAGS/IP must remain untouched.
     */
    if (budget < m->max_ops) return budget;
    __asm volatile("dsb sy\n\tisb sy":::"memory");
    entry=((uintptr_t)&code->bytes[0])|(uintptr_t)1u;
    rc=g_call_thumb(cpu,entry,budget);
    tag=rc>>24;remaining=rc&0x00ffffffu;
    if(remaining>budget)return MD_NATIVE_V2_EXEC_FALLBACK;
    if(tag==NV2G_TAG_BUDGET){g_finish_recipe(cpu,&m->budget);cpu->ip=code->start_ip;return remaining;}
    if(tag>=m->exit_count)return MD_NATIVE_V2_EXEC_FALLBACK;
    g_finish_recipe(cpu,&m->exits[tag]);cpu->ip=m->exits[tag].ip;return remaining;
#else
    (void)cpu;(void)code;(void)budget;
    return MD_NATIVE_V2_EXEC_FALLBACK;
#endif
}
