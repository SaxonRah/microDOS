#ifndef MICRODOS_THUMB2_EMIT_H
#define MICRODOS_THUMB2_EMIT_H

/*
 * M25 shared Thumb-2 (ARMv7-M / ARMv8-M Mainline) encoders.
 *
 * Translation-time only: nothing here runs per guest instruction. Every
 * encoder appends to an MdT2Buf and silently marks the buffer failed on
 * overflow, so callers check `failed` once after a whole block.
 *
 * All 32-bit encodings are the T32 forms from the ARMv7-M ARM. Instructions
 * that must not disturb APSR (lazy-flag stores between a flag-setting compare
 * and its conditional branch) use the non-S forms: MOVW/MOVT, MOV.W imm with
 * S=0, ADD/SUB/AND/ORR/EOR.W with S=0, BFI/UBFX/UXT*, LDR/STR.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct MdT2Buf {
    uint8_t *base;      /* write pointer base == execution address */
    uint32_t size;
    uint32_t at;
    int failed;
} MdT2Buf;

enum {
    T2_R0 = 0, T2_R1, T2_R2, T2_R3, T2_R4, T2_R5, T2_R6, T2_R7,
    T2_R8, T2_R9, T2_R10, T2_R11, T2_R12, T2_SP = 13, T2_LR = 14, T2_PC = 15
};

/* Data-processing opcodes (bits 8:5 of hw1). */
enum {
    T2_AND = 0, T2_BIC = 1, T2_ORR = 2, T2_ORN = 3, T2_EOR = 4,
    T2_ADD = 8, T2_ADC = 10, T2_SBC = 11, T2_SUB = 13, T2_RSB = 14
};

enum { T2_LSL = 0, T2_LSR = 1, T2_ASR = 2, T2_ROR = 3 };

enum {
    T2_EQ = 0, T2_NE, T2_CS, T2_CC, T2_MI, T2_PL, T2_VS, T2_VC,
    T2_HI, T2_LS, T2_GE, T2_LT, T2_GT, T2_LE, T2_AL
};

/* Load/store opcode bases. Immediate (imm12) forms. */
enum {
    T2_STRB_I = 0xF880u, T2_STRH_I = 0xF8A0u, T2_STR_I = 0xF8C0u,
    T2_LDRB_I = 0xF890u, T2_LDRH_I = 0xF8B0u, T2_LDR_I = 0xF8D0u
};

static inline uint32_t t2_here(const MdT2Buf *b) { return b->at; }

static inline void t2_h16(MdT2Buf *b, uint32_t hw)
{
    if (b->failed || b->at + 2u > b->size) { b->failed = 1; return; }
    b->base[b->at] = (uint8_t)hw;
    b->base[b->at + 1u] = (uint8_t)(hw >> 8);
    b->at += 2u;
}

static inline void t2_h32(MdT2Buf *b, uint32_t hw1, uint32_t hw2)
{
    t2_h16(b, hw1);
    t2_h16(b, hw2);
}

static inline void t2_put32_at(MdT2Buf *b, uint32_t at, uint32_t hw1, uint32_t hw2)
{
    if (b->failed || at + 4u > b->size) { b->failed = 1; return; }
    b->base[at] = (uint8_t)hw1;
    b->base[at + 1u] = (uint8_t)(hw1 >> 8);
    b->base[at + 2u] = (uint8_t)hw2;
    b->base[at + 3u] = (uint8_t)(hw2 >> 8);
}

/* ---- moves ------------------------------------------------------------- */

static inline void t2_movw(MdT2Buf *b, unsigned rd, uint32_t imm16)
{
    t2_h32(b, 0xF240u | (((imm16 >> 11) & 1u) << 10) | ((imm16 >> 12) & 0xFu),
              (((imm16 >> 8) & 7u) << 12) | (rd << 8) | (imm16 & 0xFFu));
}

static inline void t2_movt(MdT2Buf *b, unsigned rd, uint32_t imm16)
{
    t2_h32(b, 0xF2C0u | (((imm16 >> 11) & 1u) << 10) | ((imm16 >> 12) & 0xFu),
              (((imm16 >> 8) & 7u) << 12) | (rd << 8) | (imm16 & 0xFFu));
}

static inline void t2_mov32(MdT2Buf *b, unsigned rd, uint32_t v)
{
    t2_movw(b, rd, v & 0xFFFFu);
    if ((v >> 16) != 0u) t2_movt(b, rd, v >> 16);
}

/* MOV rd, rm (16-bit, any registers, flags untouched). */
static inline void t2_mov(MdT2Buf *b, unsigned rd, unsigned rm)
{
    t2_h16(b, 0x4600u | ((rd & 8u) << 4) | (rm << 3) | (rd & 7u));
}

/* ---- data processing, shifted register --------------------------------- */

static inline void t2_dp_reg(MdT2Buf *b, unsigned op, unsigned s, unsigned rd,
                             unsigned rn, unsigned rm, unsigned type, unsigned amt)
{
    t2_h32(b, 0xEA00u | (op << 5) | (s << 4) | rn,
              (((amt >> 2) & 7u) << 12) | (rd << 8) | ((amt & 3u) << 6) |
              (type << 4) | rm);
}

/* MOV.W rd, rm, <shift> #amt   (S=0: flags untouched) */
static inline void t2_mov_sh(MdT2Buf *b, unsigned rd, unsigned rm, unsigned type, unsigned amt)
{
    t2_dp_reg(b, T2_ORR, 0u, rd, T2_PC, rm, type, amt);
}

/* CMP rn, rm, <shift>; CMN; TST */
static inline void t2_cmp_reg(MdT2Buf *b, unsigned rn, unsigned rm, unsigned type, unsigned amt)
{
    t2_dp_reg(b, T2_SUB, 1u, T2_PC, rn, rm, type, amt);
}

static inline void t2_cmn_reg(MdT2Buf *b, unsigned rn, unsigned rm, unsigned type, unsigned amt)
{
    t2_dp_reg(b, T2_ADD, 1u, T2_PC, rn, rm, type, amt);
}

/* ---- data processing, modified immediate -------------------------------- */

/* Returns the 12-bit ThumbExpandImm encoding of v, or -1. */
static inline int t2_modimm(uint32_t v)
{
    unsigned rot;
    if (v <= 0xFFu) return (int)v;
    if ((v & 0xFF00FF00u) == 0u && (v >> 16) == (v & 0xFFu)) return (int)(0x100u | (v & 0xFFu));
    if ((v & 0x00FF00FFu) == 0u && (v >> 24) == ((v >> 8) & 0xFFu)) return (int)(0x200u | ((v >> 8) & 0xFFu));
    if (((v >> 8) & 0xFFu) == (v & 0xFFu) && ((v >> 16) & 0xFFu) == (v & 0xFFu) &&
        (v >> 24) == (v & 0xFFu)) return (int)(0x300u | (v & 0xFFu));
    for (rot = 8u; rot < 32u; ++rot) {
        const uint32_t u = (v << rot) | (v >> (32u - rot));   /* undo ROR #rot */
        if (u >= 0x80u && u <= 0xFFu) return (int)((rot << 7) | (u & 0x7Fu));
    }
    return -1;
}

/* Returns 0 if imm is not encodable (caller must materialise it). */
static inline int t2_dp_imm(MdT2Buf *b, unsigned op, unsigned s, unsigned rd,
                            unsigned rn, uint32_t imm)
{
    const int e = t2_modimm(imm);
    if (e < 0) return 0;
    t2_h32(b, 0xF000u | ((((unsigned)e >> 11) & 1u) << 10) | (op << 5) | (s << 4) | rn,
              ((((unsigned)e >> 8) & 7u) << 12) | (rd << 8) | ((unsigned)e & 0xFFu));
    return 1;
}

/* ADDW / SUBW rd, rn, #imm12 (no flags). */
static inline void t2_addw(MdT2Buf *b, unsigned rd, unsigned rn, uint32_t imm12)
{
    t2_h32(b, 0xF200u | (((imm12 >> 11) & 1u) << 10) | rn,
              (((imm12 >> 8) & 7u) << 12) | (rd << 8) | (imm12 & 0xFFu));
}

static inline void t2_subw(MdT2Buf *b, unsigned rd, unsigned rn, uint32_t imm12)
{
    t2_h32(b, 0xF2A0u | (((imm12 >> 11) & 1u) << 10) | rn,
              (((imm12 >> 8) & 7u) << 12) | (rd << 8) | (imm12 & 0xFFu));
}

/* MOV.W rd, #imm (S=0) when encodable, else MOVW. Flags untouched. */
static inline void t2_movi(MdT2Buf *b, unsigned rd, uint32_t imm)
{
    if (imm > 0xFFFFu || !t2_dp_imm(b, T2_ORR, 0u, rd, T2_PC, imm)) t2_mov32(b, rd, imm);
}

/* ---- bitfield / extend --------------------------------------------------- */

static inline void t2_ubfx(MdT2Buf *b, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    t2_h32(b, 0xF3C0u | rn,
              ((lsb >> 2) << 12) | (rd << 8) | ((lsb & 3u) << 6) | (width - 1u));
}

static inline void t2_sbfx(MdT2Buf *b, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    t2_h32(b, 0xF340u | rn,
              ((lsb >> 2) << 12) | (rd << 8) | ((lsb & 3u) << 6) | (width - 1u));
}

static inline void t2_bfi(MdT2Buf *b, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    t2_h32(b, 0xF360u | rn,
              ((lsb >> 2) << 12) | (rd << 8) | ((lsb & 3u) << 6) | (lsb + width - 1u));
}

static inline void t2_uxth(MdT2Buf *b, unsigned rd, unsigned rm) { t2_h32(b, 0xFA1Fu, 0xF080u | (rd << 8) | rm); }
static inline void t2_uxtb(MdT2Buf *b, unsigned rd, unsigned rm) { t2_h32(b, 0xFA5Fu, 0xF080u | (rd << 8) | rm); }
static inline void t2_sxtb(MdT2Buf *b, unsigned rd, unsigned rm) { t2_h32(b, 0xFA4Fu, 0xF080u | (rd << 8) | rm); }

/* ---- loads / stores ------------------------------------------------------ */

static inline void t2_ldst(MdT2Buf *b, uint32_t base_op, unsigned rt, unsigned rn, uint32_t imm12)
{
    t2_h32(b, base_op | rn, (rt << 12) | (imm12 & 0xFFFu));
}

/* ---- branches ------------------------------------------------------------ */

/* B.W (T4) / BL from `at` to `target` (byte offsets in the same buffer). */
static inline void t2_enc_b(uint32_t at, uint32_t target, int link, uint32_t *hw1, uint32_t *hw2)
{
    const int32_t off = (int32_t)target - (int32_t)(at + 4u);
    const uint32_t s = off < 0 ? 1u : 0u;
    const uint32_t i1 = ((uint32_t)off >> 23) & 1u;
    const uint32_t i2 = ((uint32_t)off >> 22) & 1u;
    const uint32_t j1 = (i1 ^ 1u) ^ s;
    const uint32_t j2 = (i2 ^ 1u) ^ s;
    *hw1 = 0xF000u | (s << 10) | (((uint32_t)off >> 12) & 0x3FFu);
    *hw2 = (link ? 0xD000u : 0x9000u) | (j1 << 13) | (j2 << 11) | (((uint32_t)off >> 1) & 0x7FFu);
}

/* B<cond>.W (T3), +-1 MiB. */
static inline void t2_enc_bcc(uint32_t at, uint32_t target, unsigned cond, uint32_t *hw1, uint32_t *hw2)
{
    const int32_t off = (int32_t)target - (int32_t)(at + 4u);
    const uint32_t s = off < 0 ? 1u : 0u;
    *hw1 = 0xF000u | (s << 10) | (cond << 6) | (((uint32_t)off >> 12) & 0x3Fu);
    *hw2 = 0x8000u | ((((uint32_t)off >> 18) & 1u) << 13) |
           ((((uint32_t)off >> 19) & 1u) << 11) | (((uint32_t)off >> 1) & 0x7FFu);
}

static inline void t2_b_to(MdT2Buf *b, uint32_t target)
{
    uint32_t h1, h2;
    t2_enc_b(b->at, target, 0, &h1, &h2);
    t2_h32(b, h1, h2);
}

/* Emits a placeholder and returns its offset for t2_patch_*. */
static inline uint32_t t2_b_fwd(MdT2Buf *b)
{
    const uint32_t at = b->at;
    t2_h32(b, 0xF3AFu, 0x8000u);   /* NOP.W until patched */
    return at;
}

static inline void t2_patch_b(MdT2Buf *b, uint32_t at, uint32_t target)
{
    uint32_t h1, h2;
    t2_enc_b(at, target, 0, &h1, &h2);
    t2_put32_at(b, at, h1, h2);
}

static inline void t2_patch_bcc(MdT2Buf *b, uint32_t at, unsigned cond, uint32_t target)
{
    uint32_t h1, h2;
    t2_enc_bcc(at, target, cond, &h1, &h2);
    t2_put32_at(b, at, h1, h2);
}

static inline void t2_bl_to(MdT2Buf *b, uint32_t target)
{
    uint32_t h1, h2;
    t2_enc_b(b->at, target, 1, &h1, &h2);
    t2_h32(b, h1, h2);
}

static inline void t2_blx(MdT2Buf *b, unsigned rm) { t2_h16(b, 0x4780u | (rm << 3)); }
static inline void t2_bx(MdT2Buf *b, unsigned rm) { t2_h16(b, 0x4700u | (rm << 3)); }

static inline void t2_push(MdT2Buf *b, uint32_t list) { t2_h32(b, 0xE92Du, list); }
static inline void t2_pop(MdT2Buf *b, uint32_t list) { t2_h32(b, 0xE8BDu, list); }
static inline void t2_sub_sp(MdT2Buf *b, unsigned imm) { t2_h16(b, 0xB080u | (imm >> 2)); }
static inline void t2_add_sp(MdT2Buf *b, unsigned imm) { t2_h16(b, 0xB000u | (imm >> 2)); }

#endif
