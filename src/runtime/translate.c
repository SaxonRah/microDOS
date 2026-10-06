/*
 * M25.1 general 8086 -> Thumb-2 translator (prototype).
 *
 * Native register ABI inside translated code:
 *
 *   r4  AX   r5  CX   r6  DX   r7  BX      (low registers: 16-bit encodings)
 *   r8  SP   r9  BP   r10 SI   r11 DI
 *   r12 MdRuntime* (== &rt->cpu)           invariant at every block boundary
 *   lr  guest RAM base                      invariant at every block boundary
 *   r0-r3 scratch; [sp] = remaining instruction budget
 *
 * Guest registers hold the 16-bit value in bits 0..15; bits 16..31 are
 * undefined ("upper-half garbage" invariant). Consumers use LSL #16/#24
 * operand forms, UXTH or STRH, so ALU results never need re-zero-extending.
 *
 * r4-r11 are AAPCS callee-saved, so C helpers (BLX) keep guest registers
 * resident; r12 and lr are reloaded with MOVW/MOVT after every helper.
 *
 * Flags: translated code keeps the canonical lazy representation
 * (lazy_op/a/b/res) and writes it only for producers whose flags can be
 * observed: the last producer of a block, producers followed by a possible
 * side exit, or a flag reader. A Jcc adjacent to its producer branches on
 * ARM flags computed from operands shifted into the top bits, which makes
 * ARM N/Z/C/V equal to the 8086 SF/ZF/(inverted)CF/OF of the 8- or 16-bit
 * operation.
 */

#include "microdos/translate.h"

#include <stddef.h>
#include <string.h>

#include "microdos/decode.h"
#include "microdos/ops.h"
#include "thumb2_emit.h"

#if defined(__arm__) && defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
#define MD_TR_HOST_THUMB2 1
#else
#define MD_TR_HOST_THUMB2 0
#endif

/* ---- constants ------------------------------------------------------------ */

enum { MD_TR_EXIT_EDGE = 1, MD_TR_EXIT_DYNAMIC, MD_TR_EXIT_BUDGET,
       MD_TR_EXIT_INVALID, MD_TR_EXIT_STORE };

static const uint8_t kG[8] = { 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u };
#define RCPU T2_R12
#define RMEM T2_LR
#define NOREG 0xFFu

#define OFF_R(i)   ((uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, r) + 2u * (i)))
#define OFF_IP     ((uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, ip)))
#define OFF_LOP    ((uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, lazy_op)))
#define OFF_LCARRY ((uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, lazy_carry)))
#define OFF_LA     ((uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, lazy_a)))
#define OFF_LB     ((uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, lazy_b)))
#define OFF_LRES   ((uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, lazy_res)))
#if MICRODOS_TRANSLATION_SUPPORT
#define OFF_GEN    ((uint32_t)offsetof(MdRuntime, code_page_generation))
#define OFF_EXEC   ((uint32_t)offsetof(MdRuntime, code_page_executable))
#endif

static uint32_t md_tr_seg_off(unsigned seg)   /* 0 ES, 1 CS, 2 SS, 3 DS */
{
    switch (seg & 3u) {
        case 0u: return (uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, es));
        case 1u: return (uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, cs));
        case 2u: return (uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, ss));
        default: return (uint32_t)(offsetof(MdRuntime, cpu) + offsetof(MdX86, ds));
    }
}

/* ---- IR ------------------------------------------------------------------- */

enum { OPK_NONE = 0, OPK_R16, OPK_R8, OPK_IMM, OPK_MEM };

typedef struct MdTrOperand {
    uint8_t kind;
    uint8_t reg;
    uint16_t imm;
} MdTrOperand;

typedef struct MdTrEa {
    uint8_t base;      /* guest reg index or NOREG (direct address) */
    uint8_t index;     /* guest reg index or NOREG */
    uint8_t seg;       /* 0 ES 1 CS 2 SS 3 DS */
    uint8_t _pad;
    uint16_t disp;
} MdTrEa;

enum {
    K_ALU = 1, K_INC, K_DEC, K_MOV, K_LEA, K_XCHG, K_CBW, K_CWD, K_NOP,
    K_PUSH, K_POP, K_JCC, K_JMP, K_LOOP, K_JCXZ, K_CALL, K_RET
};

/* lazy classes */
enum { CL_NONE = 0, CL_ADD, CL_SUB, CL_LOGIC, CL_INC, CL_DEC };

/* carry source for INC/DEC lazy writes */
enum { CS_NONE = 0, CS_INCOMING };

typedef struct MdTrOp {
    uint16_t ip, next_ip, target, imm;
    uint8_t kind, width, alu, nowrite;
    uint8_t cls, emit_lazy, fused, carry_src, carry_width, emit_carry, cc, defer_lazy;
    MdTrOperand dst, src;
    MdTrEa ea;
} MdTrOp;

typedef struct MdTrStub {
    uint32_t patch_at;
    uint16_t ip;
    uint8_t cond;      /* T2_AL = unconditional B.W */
    uint8_t kind;
    uint16_t addback;
} MdTrStub;

typedef struct MdTrCtx {
    MdTranslator *tr;
    MdRuntime *rt;
    MdT2Buf b;
    MdTrOp ops[MD_TR_MAX_OPS];
    unsigned n;
    MdTrStub stubs[MD_TR_MAX_OPS * 3u + 4u];
    unsigned nstubs;
    uint16_t cs, ip0, end_ip;
    uint8_t page[2];
    unsigned page_count;
    int track;           /* store tracking present */
    uint32_t ops_at;     /* first op after the guard (self-loop latch target) */
    int defer;           /* self-loop latch with a deferred lazy write */
    unsigned def_ra, def_rb, def_rres;   /* deferred producer's registers */
} MdTrCtx;

/* ---- C helpers called from generated code ---------------------------------- */

static uint32_t md_tr_h_load16(MdRuntime *rt, uint32_t seg, uint32_t off)
{
    return md_x86_read16(&rt->cpu, (uint16_t)seg, (uint16_t)off);
}

#if MICRODOS_TRANSLATION_SUPPORT
#define MD_TR_WRITE_EPOCH(rt) ((rt)->code_write_epoch)
#else
#define MD_TR_WRITE_EPOCH(rt) 0u
#endif

static uint32_t md_tr_h_store8(MdRuntime *rt, uint32_t seg, uint32_t off, uint32_t v)
{
    const uint32_t e = MD_TR_WRITE_EPOCH(rt);
    md_x86_write8(&rt->cpu, (uint16_t)seg, (uint16_t)off, (uint8_t)v);
    return MD_TR_WRITE_EPOCH(rt) != e;
}

static uint32_t md_tr_h_store16(MdRuntime *rt, uint32_t seg, uint32_t off, uint32_t v)
{
    const uint32_t e = MD_TR_WRITE_EPOCH(rt);
    md_x86_write16(&rt->cpu, (uint16_t)seg, (uint16_t)off, (uint16_t)v);
    return MD_TR_WRITE_EPOCH(rt) != e;
}

static uint32_t md_tr_h_cond(MdRuntime *rt, uint32_t cc)
{
    return md_x86_condition(&rt->cpu, cc) ? 1u : 0u;
}

static void md_tr_h_capture_cf(MdRuntime *rt)
{
    rt->cpu.lazy_carry = (uint8_t)md_x86_cf(&rt->cpu);
}

static uint32_t md_tr_addr(const void *p) { return (uint32_t)(uintptr_t)p; }

/* ---- emission helpers ------------------------------------------------------ */

static void em_reload(MdTrCtx *c)
{
    t2_mov32(&c->b, RCPU, md_tr_addr(c->rt));
    t2_mov32(&c->b, RMEM, md_tr_addr(c->rt->cpu.memory));
}

static void em_call(MdTrCtx *c, const void *fn)
{
    t2_mov32(&c->b, RCPU, md_tr_addr(fn));
    t2_blx(&c->b, RCPU);
    em_reload(c);
}

static void em_add_imm(MdTrCtx *c, unsigned rd, unsigned rn, uint32_t imm)
{
    imm &= 0xFFFFu;
    if (imm == 0u) { if (rd != rn) t2_mov(&c->b, rd, rn); return; }
    if (imm < 4096u) { t2_addw(&c->b, rd, rn, imm); return; }
    if (0x10000u - imm < 4096u) { t2_subw(&c->b, rd, rn, 0x10000u - imm); return; }
    t2_movw(&c->b, rd == rn ? T2_R3 : rd, imm);
    t2_dp_reg(&c->b, T2_ADD, 0u, rd, rn, rd == rn ? T2_R3 : rd, T2_LSL, 0u);
}

static void em_stub(MdTrCtx *c, unsigned cond, unsigned kind, uint16_t ip, unsigned addback)
{
    MdTrStub *s;
    if (c->nstubs >= sizeof(c->stubs) / sizeof(c->stubs[0])) { c->b.failed = 1; return; }
    s = &c->stubs[c->nstubs++];
    s->patch_at = t2_b_fwd(&c->b);
    s->ip = ip;
    s->cond = (uint8_t)cond;
    s->kind = (uint8_t)kind;
    s->addback = (uint16_t)addback;
}

static unsigned md_tr_bits(void) { return MD_X86_ADDRESS_BITS; }

/* Effective address: roff = 16-bit offset, rlin = (seg << 4) + offset
   (not yet wrapped to the address space). Uses only roff and rlin. */
static void em_ea(MdTrCtx *c, const MdTrEa *ea, unsigned roff, unsigned rlin)
{
    MdT2Buf *b = &c->b;
    if (ea->base == NOREG) {
        t2_movw(b, roff, ea->disp);
    } else {
        unsigned src = kG[ea->base];
        const int16_t d = (int16_t)ea->disp;
        if (ea->index != NOREG) {
            t2_dp_reg(b, T2_ADD, 0u, roff, src, kG[ea->index], T2_LSL, 0u);
            src = roff;
        }
        if (d > 0 && d < 4096) t2_addw(b, roff, src, (uint32_t)d);
        else if (d < 0 && d > -4096) t2_subw(b, roff, src, (uint32_t)(-d));
        else if (d != 0) {
            t2_movw(b, rlin, ea->disp);
            t2_dp_reg(b, T2_ADD, 0u, roff, src, rlin, T2_LSL, 0u);
        } else if (src != roff) {
            t2_uxth(b, roff, src);
            goto have_off;
        }
        t2_uxth(b, roff, roff);
    }
have_off:
    t2_ldst(b, T2_LDRH_I, rlin, RCPU, md_tr_seg_off(ea->seg));
    t2_dp_reg(b, T2_ADD, 0u, rlin, roff, rlin, T2_LSL, 4u);
}

/* Host pointer for linear rlin. Returns the register holding it. */
static unsigned em_hostptr(MdTrCtx *c, unsigned rlin, unsigned rtmp)
{
    const unsigned bits = md_tr_bits();
    if (c->tr->mem_aligned) {
        t2_bfi(&c->b, RMEM, rlin, 0u, bits);   /* base | (lin & mask) in one op */
        return RMEM;
    }
    t2_ubfx(&c->b, rtmp, rlin, 0u, bits);
    t2_dp_reg(&c->b, T2_ADD, 0u, rtmp, rtmp, RMEM, T2_LSL, 0u);
    return rtmp;
}

/* Load width-bit value at EA into rd (r0 or r1, or a pinned reg).
   Clobbers r0-r3 on the slow path. */
static void em_load(MdTrCtx *c, const MdTrEa *ea, unsigned width, unsigned rd)
{
    MdT2Buf *b = &c->b;
    em_ea(c, ea, T2_R2, T2_R3);
    if (width == 8u) {
        const unsigned p = em_hostptr(c, T2_R3, rd);
        t2_ldst(b, T2_LDRB_I, rd, p, 0u);
        return;
    } else {
        const unsigned bits = md_tr_bits();
        uint32_t j_slow1, j_slow2, j_done;
        unsigned p;
        t2_movw(b, rd, 0xFFFFu);
        t2_cmp_reg(b, T2_R2, rd, T2_LSL, 0u);
        j_slow1 = t2_b_fwd(b);                          /* beq slow: offset wraps */
        t2_dp_imm(b, T2_ADD, 0u, rd, T2_R3, 1u);
        t2_dp_reg(b, T2_ORR, 1u, rd, T2_PC, rd, T2_LSL, 32u - bits);   /* lsls */
        j_slow2 = t2_b_fwd(b);                          /* beq slow: linear wraps */
        p = em_hostptr(c, T2_R3, rd);
        t2_ldst(b, T2_LDRH_I, rd, p, 0u);
        j_done = t2_b_fwd(b);
        t2_patch_bcc(b, j_slow1, T2_EQ, t2_here(b));
        t2_patch_bcc(b, j_slow2, T2_EQ, t2_here(b));
        t2_mov(b, T2_R0, RCPU);
        t2_ldst(b, T2_LDRH_I, T2_R1, RCPU, md_tr_seg_off(ea->seg));
        em_call(c, (const void *)md_tr_h_load16);
        if (rd != T2_R0) t2_mov(b, rd, T2_R0);
        t2_patch_b(b, j_done, t2_here(b));
    }
}

/* Store rv (not r2/r3/rtmp) to the EA. op_index identifies the guest
   instruction for the side-exit stub (exit ip = exit_ip). */
static void em_store(MdTrCtx *c, const MdTrEa *ea, unsigned width, unsigned rv,
                     unsigned rtmp, unsigned k, uint16_t exit_ip)
{
    MdT2Buf *b = &c->b;
    const unsigned bits = md_tr_bits();
    uint32_t slow[4];
    unsigned ns = 0u, i, p;
    uint32_t j_done;

    em_ea(c, ea, T2_R2, T2_R3);
    if (width == 16u) {
        t2_movw(b, rtmp, 0xFFFFu);
        t2_cmp_reg(b, T2_R2, rtmp, T2_LSL, 0u);
        slow[ns++] = t2_b_fwd(b);                                   /* EQ */
        t2_dp_imm(b, T2_ADD, 0u, rtmp, T2_R3, 1u);
        t2_dp_reg(b, T2_ORR, 1u, rtmp, T2_PC, rtmp, T2_LSL, 32u - bits);
        slow[ns++] = t2_b_fwd(b);                                   /* EQ */
    }
#if MICRODOS_TRANSLATION_SUPPORT
    if (c->track) {
        if (width == 16u) {
            /* lin and lin+1 on different 4 KiB pages: bits >= 12 differ */
            t2_dp_imm(b, T2_ADD, 0u, rtmp, T2_R3, 1u);
            t2_dp_reg(b, T2_EOR, 0u, rtmp, rtmp, T2_R3, T2_LSL, 0u);
            t2_dp_reg(b, T2_ORR, 1u, rtmp, T2_PC, rtmp, T2_LSR, 12u);   /* lsrs */
            slow[ns++] = t2_b_fwd(b);                               /* NE */
        }
        t2_ubfx(b, rtmp, T2_R3, MD_X86_CODE_PAGE_SHIFT, bits - MD_X86_CODE_PAGE_SHIFT);
        t2_dp_reg(b, T2_ADD, 0u, rtmp, RCPU, rtmp, T2_LSL, 0u);
        t2_ldst(b, T2_LDRB_I, rtmp, rtmp, OFF_EXEC);
        t2_dp_imm(b, T2_SUB, 1u, T2_PC, rtmp, 0u);                     /* cmp #0 */
        slow[ns++] = t2_b_fwd(b);                                   /* NE */
    }
#endif
    p = em_hostptr(c, T2_R3, rtmp);
    t2_ldst(b, width == 16u ? T2_STRH_I : T2_STRB_I, rv, p, 0u);
    j_done = t2_b_fwd(b);
    for (i = 0u; i < ns; ++i) {
        /* first two (16-bit wrap checks) are EQ; page checks are NE */
        const unsigned cond = (width == 16u && i < 2u) ? T2_EQ : T2_NE;
        t2_patch_bcc(b, slow[i], cond, t2_here(b));
    }
    /* slow path: helper(rt, seg, off, value) -> nonzero if translated code
       may have been modified; then leave translated execution. */
    t2_mov(b, T2_R3, rv);
    t2_mov(b, T2_R0, RCPU);
    t2_ldst(b, T2_LDRH_I, T2_R1, RCPU, md_tr_seg_off(ea->seg));
    em_call(c, width == 16u ? (const void *)md_tr_h_store16 : (const void *)md_tr_h_store8);
    t2_dp_imm(b, T2_SUB, 1u, T2_PC, T2_R0, 0u);
    em_stub(c, T2_NE, MD_TR_EXIT_STORE, exit_ip, c->n - k);
    t2_patch_b(b, j_done, t2_here(b));
}

static unsigned r8_lsb(unsigned reg) { return (reg & 4u) ? 8u : 0u; }

/* Lazy write. a/b registers hold zero-extended (8-bit) or low-16-valid
   (16-bit) values; NOREG means 0 (logic) or the constant 1 (inc/dec b). */
static void em_lazy(MdTrCtx *c, unsigned lazy_op, unsigned ra, unsigned rb, unsigned rres,
                    int b_is_one)
{
    MdT2Buf *b = &c->b;
    if (ra == NOREG || (rb == NOREG && !b_is_one)) t2_movi(b, T2_R3, 0u);
    t2_ldst(b, T2_STRH_I, ra == NOREG ? T2_R3 : ra, RCPU, OFF_LA);
    if (b_is_one) {
        t2_movi(b, T2_R3, 1u);
        t2_ldst(b, T2_STRH_I, T2_R3, RCPU, OFF_LB);
    } else {
        t2_ldst(b, T2_STRH_I, rb == NOREG ? T2_R3 : rb, RCPU, OFF_LB);
    }
    t2_ldst(b, T2_STRH_I, rres, RCPU, OFF_LRES);
    t2_movi(b, T2_R3, lazy_op);
    t2_ldst(b, T2_STRB_I, T2_R3, RCPU, OFF_LOP);
}

static unsigned lazy_code(unsigned cls, unsigned width)
{
    const unsigned w16 = width == 16u;
    switch (cls) {
        case CL_ADD:   return w16 ? MD_LAZY_ADD16 : MD_LAZY_ADD8;
        case CL_SUB:   return w16 ? MD_LAZY_SUB16 : MD_LAZY_SUB8;
        case CL_LOGIC: return w16 ? MD_LAZY_LOGIC16 : MD_LAZY_LOGIC8;
        case CL_INC:   return w16 ? MD_LAZY_INC16 : MD_LAZY_INC8;
        default:       return w16 ? MD_LAZY_DEC16 : MD_LAZY_DEC8;
    }
}

/* x86 cc -> ARM cond for a producer class, or -1 if not fusable. */
static int fuse_cond(unsigned cls, unsigned cc)
{
    static const int8_t sub[16] = { T2_VS, T2_VC, T2_CC, T2_CS, T2_EQ, T2_NE, T2_LS, T2_HI,
                                    T2_MI, T2_PL, -1, -1, T2_LT, T2_GE, T2_LE, T2_GT };
    static const int8_t add[16] = { T2_VS, T2_VC, T2_CS, T2_CC, T2_EQ, T2_NE, -1, -1,
                                    T2_MI, T2_PL, -1, -1, T2_LT, T2_GE, T2_LE, T2_GT };
    static const int8_t incdec[16] = { T2_VS, T2_VC, -1, -1, T2_EQ, T2_NE, -1, -1,
                                       T2_MI, T2_PL, -1, -1, T2_LT, T2_GE, T2_LE, T2_GT };
    cc &= 15u;
    switch (cls) {
        case CL_SUB: case CL_LOGIC: return sub[cc];
        case CL_ADD: return add[cc];
        case CL_INC: case CL_DEC: return incdec[cc];
        default: return -1;
    }
}

static void em_carry_capture(MdTrCtx *c, const MdTrOp *op)
{
    MdT2Buf *b = &c->b;
    switch (op->carry_src) {
        case CS_INCOMING: {
            /* lazy_op INC8..DEC16 already carries CF in lazy_carry */
            uint32_t skip;
            t2_ldst(b, T2_LDRB_I, T2_R0, RCPU, OFF_LOP);
            t2_dp_imm(b, T2_SUB, 0u, T2_R0, T2_R0, MD_LAZY_INC8);
            t2_dp_imm(b, T2_SUB, 1u, T2_PC, T2_R0, 3u);
            skip = t2_b_fwd(b);
            t2_mov(b, T2_R0, RCPU);
            em_call(c, (const void *)md_tr_h_capture_cf);
            t2_patch_bcc(b, skip, T2_LS, t2_here(b));
            break;
        }
        default:
            break;
    }
}

/* Read a non-memory operand into a register (pinned reg for R16). */
static unsigned em_operand(MdTrCtx *c, const MdTrOperand *o, unsigned width, unsigned scratch)
{
    switch (o->kind) {
        case OPK_R16: return kG[o->reg];
        case OPK_R8:
            t2_ubfx(&c->b, scratch, kG[o->reg & 3u], r8_lsb(o->reg), 8u);
            return scratch;
        default:
            t2_movi(&c->b, scratch, width == 16u ? o->imm : (o->imm & 0xFFu));
            return scratch;
    }
}

/* Write lazy_carry = CF of this producer, computed from its operands, so a
   later INC/DEC lazy write needs neither this producer's lazy fields nor a
   reload. Uses r3 and APSR. */
static void em_producer_carry(MdTrCtx *c, unsigned cls, unsigned sh, unsigned ra, unsigned rb)
{
    MdT2Buf *b = &c->b;
    if (cls == CL_LOGIC) {
        t2_movi(b, T2_R3, 0u);
    } else {
        t2_mov_sh(b, T2_R3, ra, T2_LSL, sh);
        if (cls == CL_ADD) {
            t2_cmn_reg(b, T2_R3, rb, T2_LSL, sh);          /* C = carry */
            t2_movi(b, T2_R3, 0u);
            t2_dp_imm(b, T2_ADC, 0u, T2_R3, T2_R3, 0u);
        } else {
            t2_cmp_reg(b, T2_R3, rb, T2_LSL, sh);          /* C = !borrow */
            t2_movi(b, T2_R3, 0u);
            t2_dp_imm(b, T2_SBC, 0u, T2_R3, T2_R3, 0u);    /* -borrow */
            t2_dp_imm(b, T2_AND, 0u, T2_R3, T2_R3, 1u);
        }
    }
    t2_ldst(b, T2_STRB_I, T2_R3, RCPU, OFF_LCARRY);
}

static void em_alu(MdTrCtx *c, const MdTrOp *op, unsigned k)
{
    MdT2Buf *b = &c->b;
    const unsigned w = op->width, sh = w == 16u ? 16u : 24u;
    const int writes = !op->nowrite;
    const int lazy = op->emit_lazy, fused = op->fused;
    static const uint8_t dp[8] = { T2_ADD, T2_ORR, 0, 0, T2_AND, T2_SUB, T2_EOR, T2_SUB };
    unsigned ra, rb, rres;

    if (!writes && !lazy && !fused && !op->emit_carry) return;   /* dead CMP/TEST */

    if (op->dst.kind == OPK_MEM) em_load(c, &op->ea, w, T2_R0);
    if (op->src.kind == OPK_MEM) em_load(c, &op->ea, w, T2_R1);

    /* a */
    if (op->dst.kind == OPK_MEM) ra = T2_R0;
    else if (op->dst.kind == OPK_R16) {
        ra = kG[op->dst.reg];
        if (writes && (fused || lazy || op->emit_carry) && op->cls != CL_LOGIC) { t2_mov(b, T2_R0, ra); ra = T2_R0; }
    } else ra = em_operand(c, &op->dst, w, T2_R0);

    /* b */
    if (op->src.kind == OPK_MEM) rb = T2_R1;
    else {
        rb = em_operand(c, &op->src, w, T2_R1);
        if (op->src.kind == OPK_R16 && op->dst.kind == OPK_R16 &&
            op->src.reg == op->dst.reg && writes) {
            t2_mov(b, T2_R1, rb);
            rb = T2_R1;
        }
    }

    /* compute */
    if (op->dst.kind == OPK_R16 && writes) {
        rres = kG[op->dst.reg];
        t2_dp_reg(b, dp[op->alu], 0u, rres, rres, rb, T2_LSL, 0u);
    } else {
        rres = T2_R2;
        t2_dp_reg(b, dp[op->alu], 0u, rres, ra, rb, T2_LSL, 0u);
        if (w == 8u) t2_uxtb(b, rres, rres);
    }
    if (op->dst.kind == OPK_R8 && writes)
        t2_bfi(b, kG[op->dst.reg & 3u], rres, r8_lsb(op->dst.reg), 8u);

    if (op->emit_carry) em_producer_carry(c, op->cls, w == 16u ? 16u : 24u, ra, rb);

    if (op->defer_lazy) {                 /* written on the latch's exits */
        c->def_ra = op->cls == CL_LOGIC ? NOREG : ra;
        c->def_rb = op->cls == CL_LOGIC ? NOREG : rb;
        c->def_rres = rres;
    } else if (lazy) {
        if (op->cls == CL_LOGIC) em_lazy(c, lazy_code(op->cls, w), NOREG, NOREG, rres, 0);
        else em_lazy(c, lazy_code(op->cls, w), ra, rb, rres, 0);
    }

    if (op->dst.kind == OPK_MEM && writes) {
        t2_mov(b, T2_R0, rres);
        em_store(c, &op->ea, w, T2_R0, T2_R1, k, op->next_ip);
    }

    if (fused) {
        /* rres may be pinned (in place) or r2; r3 is free here */
        if (op->cls == CL_LOGIC) {
            t2_mov_sh(b, T2_R3, rres, T2_LSL, sh);
            t2_dp_imm(b, T2_SUB, 1u, T2_PC, T2_R3, 0u);
        } else {
            t2_mov_sh(b, T2_R3, ra, T2_LSL, sh);
            if (op->cls == CL_ADD) t2_cmn_reg(b, T2_R3, rb, T2_LSL, sh);
            else t2_cmp_reg(b, T2_R3, rb, T2_LSL, sh);
        }
    }
}

static void em_incdec(MdTrCtx *c, const MdTrOp *op, unsigned k)
{
    MdT2Buf *b = &c->b;
    const unsigned w = op->width, sh = w == 16u ? 16u : 24u;
    const unsigned arith = op->kind == K_INC ? T2_ADD : T2_SUB;
    const int lazy = op->emit_lazy, fused = op->fused;
    unsigned ra, rres;

    if (lazy && !op->defer_lazy) em_carry_capture(c, op);

    if (op->dst.kind == OPK_MEM) { em_load(c, &op->ea, w, T2_R0); ra = T2_R0; }
    else if (op->dst.kind == OPK_R16) {
        ra = kG[op->dst.reg];
        if (lazy || fused) { t2_mov(b, T2_R0, ra); ra = T2_R0; }
    } else ra = em_operand(c, &op->dst, w, T2_R0);

    if (op->dst.kind == OPK_R16) {
        rres = kG[op->dst.reg];
        t2_dp_imm(b, arith, 0u, rres, rres, 1u);
    } else {
        rres = T2_R2;
        t2_dp_imm(b, arith, 0u, rres, ra, 1u);
        if (w == 8u) t2_uxtb(b, rres, rres);
        if (op->dst.kind == OPK_R8) t2_bfi(b, kG[op->dst.reg & 3u], rres, r8_lsb(op->dst.reg), 8u);
    }
    if (op->defer_lazy) c->def_rres = rres;
    else if (lazy) em_lazy(c, lazy_code(op->cls, w), ra, NOREG, rres, 1);
    if (op->dst.kind == OPK_MEM) {
        t2_mov(b, T2_R0, rres);
        em_store(c, &op->ea, w, T2_R0, T2_R1, k, op->next_ip);
    }
    if (fused) {
        t2_mov_sh(b, T2_R3, ra, T2_LSL, sh);
        t2_dp_imm(b, arith, 1u, T2_R3, T2_R3, 1u << sh);
    }
}

static void em_mov(MdTrCtx *c, const MdTrOp *op, unsigned k)
{
    MdT2Buf *b = &c->b;
    const unsigned w = op->width;
    if (op->dst.kind == OPK_R16) {
        if (op->src.kind == OPK_R16) t2_mov(b, kG[op->dst.reg], kG[op->src.reg]);
        else if (op->src.kind == OPK_IMM) t2_movw(b, kG[op->dst.reg], op->src.imm);
        else em_load(c, &op->ea, 16u, kG[op->dst.reg]);
    } else if (op->dst.kind == OPK_R8) {
        unsigned r;
        if (op->src.kind == OPK_MEM) { em_load(c, &op->ea, 8u, T2_R0); r = T2_R0; }
        else r = em_operand(c, &op->src, 8u, T2_R0);
        t2_bfi(b, kG[op->dst.reg & 3u], r, r8_lsb(op->dst.reg), 8u);
    } else {
        const unsigned v = em_operand(c, &op->src, w, T2_R0);
        em_store(c, &op->ea, w, v, v == T2_R0 ? T2_R1 : T2_R0, k, op->next_ip);
    }
}

/* Lazy write of the deferred producer (the op before the latch Jcc). Its
   registers are still live here: nothing but the fused compare ran since. */
static void em_deferred_lazy(MdTrCtx *c, const MdTrOp *p)
{
    const unsigned code = lazy_code(p->cls, p->width);
    if (p->kind == K_INC || p->kind == K_DEC) {
        /* the loop never wrote lazy state, so memory still holds the state
           from before the loop: capture CF from it, then rebuild a = res -/+ 1 */
        if (p->carry_src == CS_INCOMING) em_carry_capture(c, p);
        t2_dp_imm(&c->b, p->kind == K_INC ? T2_SUB : T2_ADD, 0u, T2_R0, c->def_rres, 1u);
        em_lazy(c, code, T2_R0, NOREG, c->def_rres, 1);
    } else {
        em_lazy(c, code, c->def_ra, c->def_rb, c->def_rres, 0);
    }
}

/* Indirect transfer (RET): r1 = target IP. Probe the block table from
   generated code, exactly as md_tr_hash()/md_tr_lookup() would, and jump
   to the target's guard (which re-checks generations and the budget).
   On a miss, leave through the dynamic exit; C translates the target. */
static void em_dispatch(MdTrCtx *c)
{
    MdT2Buf *b = &c->b;
    MdTranslator *tr = c->tr;
    uint32_t miss1, miss2;
    unsigned slot_bits = 0u;
    while ((1u << slot_bits) < MD_TR_SLOTS) ++slot_bits;

    if (tr->inline_dispatch) {
        t2_movw(b, T2_R2, c->cs);                                       /* near RET: same CS */
        t2_dp_reg(b, T2_ADD, 0u, T2_R3, T2_R1, T2_R2, T2_LSL, 4u);       /* x = (cs<<4)+ip */
        t2_dp_reg(b, T2_EOR, 0u, T2_R3, T2_R3, T2_R3, T2_LSR, 9u);       /* x ^ (x>>9) */
        t2_ubfx(b, T2_R3, T2_R3, 0u, slot_bits);
        t2_mov32(b, T2_R0, md_tr_addr(tr->blocks));
        t2_dp_reg(b, T2_ADD, 0u, T2_R0, T2_R0, T2_R3, T2_LSL, 5u);       /* 32-byte entries */
        t2_ldst(b, T2_LDR_I, T2_R3, T2_R0, (uint32_t)offsetof(MdTrBlock, cs));
        t2_dp_reg(b, T2_ORR, 0u, T2_R2, T2_R2, T2_R1, T2_LSL, 16u);      /* cs | ip<<16 */
        t2_cmp_reg(b, T2_R3, T2_R2, T2_LSL, 0u);
        miss1 = t2_b_fwd(b);
        t2_ldst(b, T2_LDRB_I, T2_R3, T2_R0, (uint32_t)offsetof(MdTrBlock, state));
        t2_dp_imm(b, T2_SUB, 1u, T2_PC, T2_R3, 1u);
        miss2 = t2_b_fwd(b);
        t2_ldst(b, T2_LDR_I, T2_R3, T2_R0, (uint32_t)offsetof(MdTrBlock, entry));
        t2_mov32(b, T2_R2, md_tr_addr(tr->arena) | 1u);
        t2_dp_reg(b, T2_ADD, 0u, T2_R3, T2_R3, T2_R2, T2_LSL, 0u);
        t2_bx(b, T2_R3);
        t2_patch_bcc(b, miss1, T2_NE, t2_here(b));
        t2_patch_bcc(b, miss2, T2_NE, t2_here(b));
    }
    t2_mov32(b, T2_R0, (uint32_t)MD_TR_EXIT_DYNAMIC << 24);
    t2_b_to(b, tr->exit_off);
}

static void em_ea_sp(MdTrEa *ea)
{
    ea->base = MD_X86_SP;
    ea->index = NOREG;
    ea->seg = 2u;
    ea->disp = 0u;
}

static void em_op(MdTrCtx *c, unsigned i)
{
    MdT2Buf *b = &c->b;
    const MdTrOp *op = &c->ops[i];
    const unsigned k = i + 1u;
    MdTrEa sp;
    em_ea_sp(&sp);

    switch (op->kind) {
        case K_ALU: em_alu(c, op, k); break;
        case K_INC: case K_DEC: em_incdec(c, op, k); break;
        case K_MOV: em_mov(c, op, k); break;
        case K_NOP: break;
        case K_LEA:
            em_ea(c, &op->ea, T2_R2, T2_R3);
            t2_mov(b, kG[op->dst.reg], T2_R2);
            break;
        case K_XCHG:
            t2_mov(b, T2_R0, kG[0]);
            t2_mov(b, kG[0], kG[op->src.reg]);
            t2_mov(b, kG[op->src.reg], T2_R0);
            break;
        case K_CBW:
            t2_sxtb(b, T2_R0, kG[0]);
            t2_bfi(b, kG[0], T2_R0, 0u, 16u);
            break;
        case K_CWD:
            t2_sbfx(b, kG[2], kG[0], 15u, 1u);
            break;
        case K_PUSH:
            t2_dp_imm(b, T2_SUB, 0u, kG[MD_X86_SP], kG[MD_X86_SP], 2u);
            em_store(c, &sp, 16u, kG[op->src.reg], T2_R0, k, op->next_ip);
            break;
        case K_POP:
            em_load(c, &sp, 16u, T2_R0);
            t2_dp_imm(b, T2_ADD, 0u, kG[MD_X86_SP], kG[MD_X86_SP], 2u);
            t2_mov(b, kG[op->dst.reg], T2_R0);
            break;
        case K_JCC: {
            int cond = -1;
            if (op->fused) cond = fuse_cond(c->ops[i - 1u].cls, op->cc);
            if (c->defer && i + 1u == c->n) {
                /* self-loop latch: the producer's lazy write happens only on
                   the two ways out (loop exit, budget exhausted) */
                const MdTrOp *p = &c->ops[i - 1u];
                uint32_t j_taken, j_budget;
                j_taken = t2_b_fwd(b);
                em_deferred_lazy(c, p);
                em_stub(c, T2_AL, MD_TR_EXIT_EDGE, op->next_ip, 0u);
                t2_patch_bcc(b, j_taken, (unsigned)cond, t2_here(b));
                t2_ldst(b, T2_LDR_I, T2_R3, T2_SP, 0u);
                t2_dp_imm(b, T2_SUB, 1u, T2_R3, T2_R3, c->n);
                j_budget = t2_b_fwd(b);
                t2_ldst(b, T2_STR_I, T2_R3, T2_SP, 0u);
                t2_b_to(b, c->ops_at);
                t2_patch_bcc(b, j_budget, T2_LT, t2_here(b));
                em_deferred_lazy(c, p);
                em_stub(c, T2_AL, MD_TR_EXIT_BUDGET, c->ip0, 0u);
                break;
            }
            if (cond < 0) {
                t2_mov(b, T2_R0, RCPU);
                t2_movi(b, T2_R1, op->cc);
                em_call(c, (const void *)md_tr_h_cond);
                t2_dp_imm(b, T2_SUB, 1u, T2_PC, T2_R0, 0u);
                cond = T2_NE;
            }
            em_stub(c, (unsigned)cond, MD_TR_EXIT_EDGE, op->target, 0u);
            em_stub(c, T2_AL, MD_TR_EXIT_EDGE, op->next_ip, 0u);
            break;
        }
        case K_JMP:
            em_stub(c, T2_AL, MD_TR_EXIT_EDGE, op->target, 0u);
            break;
        case K_LOOP:
            t2_dp_imm(b, T2_SUB, 0u, kG[1], kG[1], 1u);
            t2_dp_reg(b, T2_ORR, 1u, T2_R0, T2_PC, kG[1], T2_LSL, 16u);
            em_stub(c, T2_NE, MD_TR_EXIT_EDGE, op->target, 0u);
            em_stub(c, T2_AL, MD_TR_EXIT_EDGE, op->next_ip, 0u);
            break;
        case K_JCXZ:
            t2_dp_reg(b, T2_ORR, 1u, T2_R0, T2_PC, kG[1], T2_LSL, 16u);
            em_stub(c, T2_EQ, MD_TR_EXIT_EDGE, op->target, 0u);
            em_stub(c, T2_AL, MD_TR_EXIT_EDGE, op->next_ip, 0u);
            break;
        case K_CALL:
            t2_movw(b, T2_R1, op->next_ip);
            t2_dp_imm(b, T2_SUB, 0u, kG[MD_X86_SP], kG[MD_X86_SP], 2u);
            em_store(c, &sp, 16u, T2_R1, T2_R0, k, op->target);
            em_stub(c, T2_AL, MD_TR_EXIT_EDGE, op->target, 0u);
            break;
        case K_RET: {
            em_load(c, &sp, 16u, T2_R1);
            em_add_imm(c, kG[MD_X86_SP], kG[MD_X86_SP], 2u + op->imm);
            em_dispatch(c);
            break;
        }
        default:
            b->failed = 1;
            break;
    }
}

/* ---- front end: 8086 bytes -> IR ------------------------------------------- */

static int parse_modrm(const uint8_t *p, size_t avail, unsigned seg_override,
                       MdTrOperand *rm_out, MdTrEa *ea, unsigned width, unsigned *consumed)
{
    const unsigned modrm = p[0];
    const unsigned mod = modrm >> 6, rm = modrm & 7u;
    static const uint8_t base[8]  = { 3, 3, 5, 5, 6, 7, 5, 3 };
    static const uint8_t index[8] = { 6, 7, 6, 7, NOREG, NOREG, NOREG, NOREG };
    *consumed = 1u;
    if (mod == 3u) {
        rm_out->kind = width == 16u ? OPK_R16 : OPK_R8;
        rm_out->reg = (uint8_t)rm;
        return 1;
    }
    rm_out->kind = OPK_MEM;
    ea->seg = 3u;
    ea->disp = 0u;
    if (mod == 0u && rm == 6u) {
        if (avail < 3u) return 0;
        ea->base = NOREG;
        ea->index = NOREG;
        ea->disp = (uint16_t)(p[1] | (p[2] << 8));
        *consumed = 3u;
    } else {
        ea->base = base[rm];
        ea->index = index[rm];
        if (rm == 2u || rm == 3u || rm == 6u) ea->seg = 2u;
        if (mod == 1u) {
            if (avail < 2u) return 0;
            ea->disp = (uint16_t)(int16_t)(int8_t)p[1];
            *consumed = 2u;
        } else if (mod == 2u) {
            if (avail < 3u) return 0;
            ea->disp = (uint16_t)(p[1] | (p[2] << 8));
            *consumed = 3u;
        }
    }
    if (seg_override != 0u) ea->seg = (uint8_t)((seg_override >> 3) & 3u);
    return 1;
}

static unsigned alu_class(unsigned alu)
{
    switch (alu & 7u) {
        case 0u: return CL_ADD;
        case 5u: case 7u: return CL_SUB;
        case 1u: case 4u: case 6u: return CL_LOGIC;
        default: return CL_NONE;                  /* ADC/SBB: not lowered yet */
    }
}

/* Fills *op from one decoded instruction; returns 0 if not lowered. */
static int md_tr_parse(const uint8_t *p, size_t avail, const MdDecodedInstruction *in, MdTrOp *op)
{
    unsigned seg = 0u, i, pc = in->prefix_count;
    const uint8_t opc = in->opcode;
    unsigned used, w;
    const uint8_t *q;
    size_t left;

    memset(op, 0, sizeof(*op));
    op->ip = in->ip;
    op->next_ip = in->next_ip;
    op->target = in->target;
    for (i = 0u; i < pc; ++i) {
        const uint8_t pf = in->prefixes[i];
        if ((pf & 0xE7u) == 0x26u) seg = pf;
        else return 0;                            /* REP / LOCK: interpreter */
    }
    q = p + pc + 1u;                              /* byte after the opcode */
    left = avail > pc + 1u ? avail - pc - 1u : 0u;

    /* ALU r/m,r and r,r/m */
    if (opc <= 0x3Bu && (opc & 4u) == 0u) {
        const unsigned alu = (opc >> 3) & 7u, dir = (opc >> 1) & 1u;
        MdTrOperand rmo, rego;
        if (alu_class(alu) == CL_NONE || left < 1u) return 0;
        w = (opc & 1u) ? 16u : 8u;
        if (!parse_modrm(q, left, seg, &rmo, &op->ea, w, &used)) return 0;
        rego.kind = w == 16u ? OPK_R16 : OPK_R8;
        rego.reg = (uint8_t)((q[0] >> 3) & 7u);
        rego.imm = 0u;
        op->kind = K_ALU; op->width = (uint8_t)w; op->alu = (uint8_t)alu;
        op->dst = dir ? rego : rmo;
        op->src = dir ? rmo : rego;
        op->nowrite = alu == 7u;
        op->cls = (uint8_t)alu_class(alu);
        return 1;
    }
    /* ALU acc,imm */
    if (opc <= 0x3Du && (opc & 6u) == 4u) {
        const unsigned alu = (opc >> 3) & 7u;
        if (alu_class(alu) == CL_NONE) return 0;
        w = (opc & 1u) ? 16u : 8u;
        op->kind = K_ALU; op->width = (uint8_t)w; op->alu = (uint8_t)alu;
        op->dst.kind = w == 16u ? OPK_R16 : OPK_R8; op->dst.reg = 0u;
        op->src.kind = OPK_IMM;
        op->src.imm = (uint16_t)(w == 16u ? (q[0] | (q[1] << 8)) : q[0]);
        op->nowrite = alu == 7u;
        op->cls = (uint8_t)alu_class(alu);
        return 1;
    }
    switch (opc) {
        case 0x80: case 0x81: case 0x82: case 0x83: {
            const unsigned alu = (q[0] >> 3) & 7u;
            const uint8_t *imm;
            if (alu_class(alu) == CL_NONE) return 0;
            w = (opc == 0x81u || opc == 0x83u) ? 16u : 8u;
            if (!parse_modrm(q, left, seg, &op->dst, &op->ea, w, &used)) return 0;
            imm = q + used;
            op->kind = K_ALU; op->width = (uint8_t)w; op->alu = (uint8_t)alu;
            op->src.kind = OPK_IMM;
            if (opc == 0x81u) op->src.imm = (uint16_t)(imm[0] | (imm[1] << 8));
            else if (opc == 0x83u) op->src.imm = (uint16_t)(int16_t)(int8_t)imm[0];
            else op->src.imm = imm[0];
            op->nowrite = alu == 7u;
            op->cls = (uint8_t)alu_class(alu);
            return 1;
        }
        case 0x84: case 0x85:
            w = (opc & 1u) ? 16u : 8u;
            if (!parse_modrm(q, left, seg, &op->dst, &op->ea, w, &used)) return 0;
            op->kind = K_ALU; op->width = (uint8_t)w; op->alu = 4u; op->nowrite = 1u;
            op->src.kind = w == 16u ? OPK_R16 : OPK_R8;
            op->src.reg = (uint8_t)((q[0] >> 3) & 7u);
            op->cls = CL_LOGIC;
            return 1;
        case 0xA8: case 0xA9:
            w = (opc & 1u) ? 16u : 8u;
            op->kind = K_ALU; op->width = (uint8_t)w; op->alu = 4u; op->nowrite = 1u;
            op->dst.kind = w == 16u ? OPK_R16 : OPK_R8;
            op->src.kind = OPK_IMM;
            op->src.imm = (uint16_t)(w == 16u ? (q[0] | (q[1] << 8)) : q[0]);
            op->cls = CL_LOGIC;
            return 1;
        case 0x88: case 0x89: case 0x8A: case 0x8B: {
            MdTrOperand rmo, rego;
            w = (opc & 1u) ? 16u : 8u;
            if (!parse_modrm(q, left, seg, &rmo, &op->ea, w, &used)) return 0;
            rego.kind = w == 16u ? OPK_R16 : OPK_R8;
            rego.reg = (uint8_t)((q[0] >> 3) & 7u);
            rego.imm = 0u;
            op->kind = K_MOV; op->width = (uint8_t)w;
            op->dst = (opc & 2u) ? rego : rmo;
            op->src = (opc & 2u) ? rmo : rego;
            return 1;
        }
        case 0xC6: case 0xC7:
            if (((q[0] >> 3) & 7u) != 0u) return 0;
            w = (opc & 1u) ? 16u : 8u;
            if (!parse_modrm(q, left, seg, &op->dst, &op->ea, w, &used)) return 0;
            op->kind = K_MOV; op->width = (uint8_t)w;
            op->src.kind = OPK_IMM;
            op->src.imm = (uint16_t)(w == 16u ? (q[used] | (q[used + 1u] << 8)) : q[used]);
            return 1;
        case 0x8D:
            if ((q[0] >> 6) == 3u) return 0;
            if (!parse_modrm(q, left, seg, &op->src, &op->ea, 16u, &used)) return 0;
            op->kind = K_LEA; op->width = 16u;
            op->dst.kind = OPK_R16; op->dst.reg = (uint8_t)((q[0] >> 3) & 7u);
            return 1;
        case 0xFE: case 0xFF: {
            const unsigned ext = (q[0] >> 3) & 7u;
            if (ext > 1u) return 0;
            w = opc == 0xFFu ? 16u : 8u;
            if (!parse_modrm(q, left, seg, &op->dst, &op->ea, w, &used)) return 0;
            op->kind = ext == 0u ? K_INC : K_DEC; op->width = (uint8_t)w;
            op->cls = ext == 0u ? CL_INC : CL_DEC;
            return 1;
        }
        case 0x90: op->kind = K_NOP; return 1;
        case 0x98: op->kind = K_CBW; return 1;
        case 0x99: op->kind = K_CWD; return 1;
        case 0xE2: op->kind = K_LOOP; return 1;
        case 0xE3: op->kind = K_JCXZ; return 1;
        case 0xEB: case 0xE9: op->kind = K_JMP; return 1;
        case 0xE8: op->kind = K_CALL; return 1;
        case 0xC3: case 0xC1: op->kind = K_RET; return 1;
        case 0xC2: case 0xC0: op->kind = K_RET; op->imm = (uint16_t)(q[0] | (q[1] << 8)); return 1;
        default: break;
    }
    if (opc >= 0xB0u && opc <= 0xB7u) {
        op->kind = K_MOV; op->width = 8u;
        op->dst.kind = OPK_R8; op->dst.reg = (uint8_t)(opc & 7u);
        op->src.kind = OPK_IMM; op->src.imm = q[0];
        return 1;
    }
    if (opc >= 0xB8u && opc <= 0xBFu) {
        op->kind = K_MOV; op->width = 16u;
        op->dst.kind = OPK_R16; op->dst.reg = (uint8_t)(opc & 7u);
        op->src.kind = OPK_IMM; op->src.imm = (uint16_t)(q[0] | (q[1] << 8));
        return 1;
    }
    if (opc >= 0x40u && opc <= 0x4Fu) {
        op->kind = opc < 0x48u ? K_INC : K_DEC; op->width = 16u;
        op->dst.kind = OPK_R16; op->dst.reg = (uint8_t)(opc & 7u);
        op->cls = opc < 0x48u ? CL_INC : CL_DEC;
        return 1;
    }
    if (opc >= 0x50u && opc <= 0x57u) {
        op->kind = K_PUSH; op->src.kind = OPK_R16; op->src.reg = (uint8_t)(opc & 7u);
        return 1;
    }
    if (opc >= 0x58u && opc <= 0x5Fu && opc != 0x5Cu) {
        op->kind = K_POP; op->dst.kind = OPK_R16; op->dst.reg = (uint8_t)(opc & 7u);
        return 1;
    }
    if (opc >= 0x91u && opc <= 0x97u) {
        op->kind = K_XCHG; op->src.kind = OPK_R16; op->src.reg = (uint8_t)(opc & 7u);
        return 1;
    }
    if (opc >= 0x60u && opc <= 0x7Fu) {      /* 60-6F alias 70-7F on the 8086 */
        op->kind = K_JCC; op->cc = (uint8_t)(opc & 15u);
        return 1;
    }
    return 0;
}

static int md_tr_is_terminator(unsigned kind)
{
    return kind >= K_JCC;
}

static int md_tr_writes_memory(const MdTrOp *op)
{
    switch (op->kind) {
        case K_ALU: return op->dst.kind == OPK_MEM && !op->nowrite;
        case K_INC: case K_DEC: case K_MOV: return op->dst.kind == OPK_MEM;
        case K_PUSH: case K_CALL: return 1;
        default: return 0;
    }
}

static void md_tr_analyze_carry(MdTrCtx *c);
static void md_tr_analyze_defer(MdTrCtx *c);

/* Flag liveness: which producers must write the lazy state, Jcc fusion,
   and the carry source of INC/DEC lazy writes. */
static void md_tr_analyze(MdTrCtx *c)
{
    int p = -1;
    unsigned i;
    for (i = 0u; i < c->n; ++i) {
        MdTrOp *op = &c->ops[i];
        if (op->cls != CL_NONE) p = (int)i;
        if (c->track && md_tr_writes_memory(op) && p >= 0) c->ops[p].emit_lazy = 1u;
        if (op->kind == K_JCC) {
            if (i > 0u && (int)i - 1 == p && c->ops[p].dst.kind != OPK_MEM &&
                fuse_cond(c->ops[p].cls, op->cc) >= 0) {
                op->fused = 1u;
                c->ops[p].fused = 1u;
            }
            if (p >= 0) c->ops[p].emit_lazy = 1u;
        }
    }
    if (p >= 0) c->ops[p].emit_lazy = 1u;                      /* live-out */

    md_tr_analyze_carry(c);
    md_tr_analyze_defer(c);
}

static void md_tr_analyze_carry(MdTrCtx *c)
{
    unsigned i;
    for (i = 0u; i < c->n; ++i) {
        MdTrOp *op = &c->ops[i];
        int j;
        if ((op->kind != K_INC && op->kind != K_DEC) || !op->emit_lazy) continue;
        op->carry_src = CS_INCOMING;
        for (j = (int)i - 1; j >= 0; --j) {
            MdTrOp *q = &c->ops[j];
            if (q->cls == CL_NONE) continue;
            if (q->cls == CL_INC || q->cls == CL_DEC) {
                if (q->emit_lazy) { op->carry_src = CS_NONE; break; }
                continue;
            }
            /* q captures CF into lazy_carry from its own registers. Safe:
               any exit between q and op writes a lazy op (q's or an
               INC/DEC's) consistent with that lazy_carry. */
            q->emit_carry = 1u;
            op->carry_src = CS_NONE;
            break;
        }
    }
}

/* Self-loop latch with a deferred lazy write: the block ends in a fused Jcc
   back to its own start, and the producer right before it writes the lazy
   state only on the exits. Legal when the flags are dead on loop entry: the
   first producer comes before any possible side exit and does not read the
   incoming CF (the producer itself may: memory still holds the pre-loop
   state, which is exactly what its deferred capture needs). */
static void md_tr_analyze_defer(MdTrCtx *c)
{
    MdTrOp *last, *p;
    unsigned i;
    c->defer = 0;
    if (c->n < 2u) return;
    last = &c->ops[c->n - 1u];
    p = &c->ops[c->n - 2u];
    if (last->kind != K_JCC || !last->fused || last->target != c->ip0) return;
    if (p->cls == CL_NONE || p->dst.kind == OPK_MEM || p->emit_carry) return;
    if ((p->kind == K_INC || p->kind == K_DEC) && p->dst.kind != OPK_R16) return;
    for (i = 0u; i + 1u < c->n; ++i) {
        const MdTrOp *op = &c->ops[i];
        if (op->cls != CL_NONE) {
            if (op != p && (op->kind == K_INC || op->kind == K_DEC) &&
                op->emit_lazy && op->carry_src == CS_INCOMING) return;
            break;
        }
        if (c->track && md_tr_writes_memory(op)) return;
    }
    p->defer_lazy = 1u;
    c->defer = 1;
    ++c->tr->stats.deferred_latches;
}

/* ---- byte-exact code tracking ------------------------------------------------ */

/* Marks the guest bytes of a block as translated. Pages get a byte bitmap
   from a small pool (MD_X86_PAGE_TRBYTES: only stores to covered bytes bump
   the page generation). When the pool is exhausted the page falls back to
   page-granular MD_X86_PAGE_TRANSLATED tracking, which is always correct. */
#if MICRODOS_TRANSLATION_SUPPORT
static void md_tr_mark_live(MdTranslator *tr, uint16_t cs, uint16_t ip, size_t len)
{
    MdRuntime *rt = tr->rt;
    size_t i;
    for (i = 0u; i < len; ++i) {
        const uint32_t a = md_x86_linear(cs, (uint16_t)(ip + i)) & MD_X86_ADDRESS_MASK;
        const unsigned page = (unsigned)(a >> MD_X86_CODE_PAGE_SHIFT);
        uint8_t *bm = tr->live_table[page];
        if (bm == NULL) {
            if (rt->code_page_executable[page] & MD_X86_PAGE_TRANSLATED) continue;
            if (tr->live_used >= MD_TR_LIVE_PAGES) {
                rt->code_page_executable[page] |= MD_X86_PAGE_TRANSLATED;
                ++tr->stats.live_fallback_pages;
                continue;
            }
            bm = tr->live_pool[tr->live_used++];
            memset(bm, 0, sizeof(tr->live_pool[0]));
            tr->live_table[page] = bm;
            rt->code_page_executable[page] |= MD_X86_PAGE_TRBYTES;
            ++tr->stats.live_pages;
        }
        bm[(a & MD_X86_CODE_PAGE_MASK) >> 3] |= (uint8_t)(1u << (a & 7u));
    }
}
#endif

/* ---- arena / trampolines --------------------------------------------------- */

static void md_tr_sync(MdTranslator *tr, uint32_t from, uint32_t to)
{
#if MD_TR_HOST_THUMB2
#if defined(__linux__)
    __builtin___clear_cache((char *)tr->arena + from, (char *)tr->arena + to);
#else
    (void)tr; (void)from; (void)to;
    __asm volatile("dsb 0xF\n\tisb 0xF" ::: "memory");
#endif
#else
    (void)tr; (void)from; (void)to;
#endif
}

static void md_tr_emit_trampolines(MdTranslator *tr)
{
    MdT2Buf b = { tr->arena, tr->arena_size, 0u, 0 };
    unsigned i;
    MdRuntime *rt = tr->rt;

    tr->enter_off = t2_here(&b);                /* uint32_t enter(entry|1, budget) */
    t2_push(&b, 0x4FF0u);                       /* r4-r11, lr */
    t2_sub_sp(&b, 12u);
    t2_ldst(&b, T2_STR_I, T2_R1, T2_SP, 0u);
    t2_mov32(&b, RCPU, md_tr_addr(rt));
    for (i = 0u; i < 8u; ++i) t2_ldst(&b, T2_LDRH_I, kG[i], RCPU, OFF_R(i));
    t2_mov32(&b, RMEM, md_tr_addr(rt->cpu.memory));
    t2_bx(&b, T2_R0);
    while (b.at & 3u) t2_h16(&b, 0xBF00u);

    tr->exit_off = t2_here(&b);                 /* r0 = info, r1 = next ip */
    for (i = 0u; i < 8u; ++i) t2_ldst(&b, T2_STRH_I, kG[i], RCPU, OFF_R(i));
    t2_ldst(&b, T2_STRH_I, T2_R1, RCPU, OFF_IP);
    t2_ldst(&b, T2_LDR_I, T2_R2, T2_SP, 0u);
    t2_mov32(&b, T2_R3, md_tr_addr((const void *)&tr->remaining));
    t2_ldst(&b, T2_STR_I, T2_R2, T2_R3, 0u);
    t2_add_sp(&b, 12u);
    t2_pop(&b, 0x8FF0u);                        /* r4-r11, pc */
    while (b.at & 3u) t2_h16(&b, 0xBF00u);

    tr->arena_used = b.at;
    md_tr_sync(tr, 0u, b.at);
}

void md_tr_flush(MdTranslator *tr)
{
#if MICRODOS_TRANSLATION_SUPPORT
    unsigned page;
    for (page = 0u; page < MD_X86_CODE_PAGE_COUNT; ++page) {
        if (tr->live_table[page] != NULL) {
            tr->rt->code_page_executable[page] &= (uint8_t)(0xFFu ^ MD_X86_PAGE_TRBYTES);   /* no truncating cast (MSVC C4310) */
            tr->live_table[page] = NULL;
        }
    }
    tr->live_used = 0u;
#endif
    memset(tr->blocks, 0, sizeof(tr->blocks));
    if (tr->arena != NULL) md_tr_emit_trampolines(tr);
#if MICRODOS_TRANSLATION_SUPPORT
    tr->epoch = tr->rt->code_epoch;
#endif
    ++tr->stats.flushes;
}

int md_tr_init(MdTranslator *tr, MdRuntime *rt, uint8_t *arena, uint32_t arena_size)
{
    memset(tr, 0, sizeof(*tr));
    tr->rt = rt;
#if !MD_TR_HOST_THUMB2 || !MICRODOS_TRANSLATION_SUPPORT
    (void)arena; (void)arena_size;
    return 0;
#else
    if (arena == NULL || arena_size < 4096u || arena_size > 0xFFFFFFu) return 0;
    if (OFF_EXEC + MD_X86_CODE_PAGE_COUNT > 4095u) return 0;
    tr->arena = arena;
    tr->arena_size = arena_size;
    tr->mem_aligned = (md_tr_addr(rt->cpu.memory) & (MD_X86_ADDRESS_SPACE - 1u)) == 0u;
    /* inline RET dispatch reads cs|ip as one word from 32-byte entries */
    tr->inline_dispatch = sizeof(MdTrBlock) == 32u &&
                          (offsetof(MdTrBlock, cs) & 3u) == 0u &&
                          offsetof(MdTrBlock, ip) == offsetof(MdTrBlock, cs) + 2u &&
                          (MD_TR_SLOTS & (MD_TR_SLOTS - 1u)) == 0u;
    rt->cpu.tr_live_bits = tr->live_table;
    md_tr_flush(tr);
    tr->stats.flushes = 0u;
    return 1;
#endif
}

/* ---- block translation ------------------------------------------------------ */

static unsigned md_tr_hash(uint16_t cs, uint16_t ip)
{
    const uint32_t x = ((uint32_t)cs << 4) + ip;
    return (unsigned)((x ^ (x >> 9)) & (MD_TR_SLOTS - 1u));
}

#if MICRODOS_TRANSLATION_SUPPORT
static int md_tr_block_fresh(const MdTranslator *tr, const MdTrBlock *blk)
{
    unsigned i;
    for (i = 0u; i < blk->page_count; ++i)
        if (tr->rt->code_page_generation[blk->page[i]] != blk->gen[i]) return 0;
    return 1;
}

/* Adds a code page to the block's (at most two) pages. Takes the array and
   count explicitly so GCC's object-size analysis sees the real bound. */
static int md_tr_add_page(uint8_t pages[2], unsigned *count, unsigned page)
{
    if (*count >= 1u && pages[0] == page) return 1;
    if (*count >= 2u && pages[1] == page) return 1;
    if (*count >= 2u || page >= MD_X86_CODE_PAGE_COUNT) return 0;
    pages[*count] = (uint8_t)page;
    *count += 1u;
    return 1;
}

/* Returns 1 and fills blk on success; 0 if the first instruction is not
   lowered (blk becomes an untranslatable marker). */
static int md_tr_translate(MdTranslator *tr, MdTrBlock *blk, uint16_t cs, uint16_t ip, int retry)
{
    static MdTrCtx ctx;                         /* compile scratch, not per block */
    MdTrCtx *c = &ctx;
    MdRuntime *rt = tr->rt;
    uint8_t win[112];
    size_t wlen = sizeof(win), i;
    uint16_t cur = ip;
    unsigned s;

    c->n = 0u;
    c->nstubs = 0u;
    c->page_count = 0u;
    c->tr = tr;
    c->rt = rt;
    c->cs = cs;
    c->ip0 = ip;
    c->track = rt->cpu.code_page_executable != NULL;
    if ((size_t)(0x10000u - ip) < wlen) wlen = (size_t)(0x10000u - ip);
    {
        const uint32_t lin = md_x86_linear(cs, ip) & MD_X86_ADDRESS_MASK;
        if (lin + wlen <= MD_X86_ADDRESS_SPACE) memcpy(win, rt->cpu.memory + lin, wlen);
        else for (i = 0u; i < wlen; ++i) win[i] = md_x86_read8(&rt->cpu, cs, (uint16_t)(ip + i));
    }

    memset(blk, 0, sizeof(*blk));
    blk->cs = cs;
    blk->ip = ip;

    while (c->n < MD_TR_MAX_OPS) {
        MdDecodedInstruction in;
        MdTrOp *op = &c->ops[c->n];
        const size_t at = (size_t)(cur - ip);
        unsigned p0, p1;
        if (at + 8u > wlen) break;               /* keep whole instructions in the window */
        if (!md_decode_8086(win, wlen, ip, cur, &in) || !in.valid_8086) break;
        if ((size_t)(in.next_ip - ip) > wlen || in.next_ip <= cur) break;
        p0 = md_x86_code_page(md_x86_linear(cs, cur));
        p1 = md_x86_code_page(md_x86_linear(cs, (uint16_t)(in.next_ip - 1u)));
        if (!md_tr_add_page(c->page, &c->page_count, p0) ||
            !md_tr_add_page(c->page, &c->page_count, p1)) break;
        if (!md_tr_parse(win + at, wlen - at, &in, op)) break;
        ++c->n;
        cur = in.next_ip;
        if (md_tr_is_terminator(op->kind)) break;
    }
    c->end_ip = cur;

    if (c->n == 0u) {
        const unsigned page = md_x86_code_page(md_x86_linear(cs, ip));
        blk->state = 2u;
        blk->page[0] = (uint8_t)page;
        blk->page_count = 1u;
        blk->gen[0] = rt->code_page_generation[page];
        ++tr->stats.untranslatable;
        return 0;
    }

    md_tr_analyze(c);
    md_tr_mark_live(tr, cs, ip, (size_t)(cur - ip));

    c->b.base = tr->arena;
    c->b.size = tr->arena_size;
    c->b.at = tr->arena_used;
    c->b.failed = 0;
    blk->entry = c->b.at;

    /* guard: code-page generations, then the budget */
    for (s = 0u; s < c->page_count; ++s) {
        const uint32_t g = rt->code_page_generation[c->page[s]];
        t2_ldst(&c->b, T2_LDR_I, T2_R0, RCPU, OFF_GEN + 4u * c->page[s]);
        if (!t2_dp_imm(&c->b, T2_SUB, 1u, T2_PC, T2_R0, g)) {
            t2_mov32(&c->b, T2_R1, g);
            t2_cmp_reg(&c->b, T2_R0, T2_R1, T2_LSL, 0u);
        }
        em_stub(c, T2_NE, MD_TR_EXIT_INVALID, ip, 0u);
        blk->page[s] = c->page[s];
        blk->gen[s] = g;
    }
    blk->body = t2_here(&c->b);
    blk->page_count = (uint8_t)c->page_count;
    t2_ldst(&c->b, T2_LDR_I, T2_R0, T2_SP, 0u);
    t2_dp_imm(&c->b, T2_SUB, 1u, T2_R0, T2_R0, c->n);
    em_stub(c, T2_LT, MD_TR_EXIT_BUDGET, ip, 0u);
    t2_ldst(&c->b, T2_STR_I, T2_R0, T2_SP, 0u);
    c->ops_at = t2_here(&c->b);

    for (s = 0u; s < c->n; ++s) em_op(c, s);
    if (!md_tr_is_terminator(c->ops[c->n - 1u].kind))
        em_stub(c, T2_AL, MD_TR_EXIT_EDGE, cur, 0u);

    /* exit stubs: movw/movt r0 = kind<<24 | stub offset (patchable first
       word), r1 = next ip, optional budget add-back, B.W common exit */
    for (s = 0u; s < c->nstubs; ++s) {
        const MdTrStub *st = &c->stubs[s];
        const uint32_t at = t2_here(&c->b);
        const uint32_t info = ((uint32_t)st->kind << 24) | at;
        if (st->cond == T2_AL) t2_patch_b(&c->b, st->patch_at, at);
        else t2_patch_bcc(&c->b, st->patch_at, st->cond, at);
        t2_movw(&c->b, T2_R0, info & 0xFFFFu);
        t2_movt(&c->b, T2_R0, info >> 16);
        t2_movw(&c->b, T2_R1, st->ip);
        if (st->addback != 0u) {
            t2_ldst(&c->b, T2_LDR_I, T2_R2, T2_SP, 0u);
            t2_addw(&c->b, T2_R2, T2_R2, st->addback);
            t2_ldst(&c->b, T2_STR_I, T2_R2, T2_SP, 0u);
        }
        t2_b_to(&c->b, tr->exit_off);
    }

    if (c->b.failed) {
        if (retry) { md_tr_flush(tr); return md_tr_translate(tr, blk, cs, ip, 0); }
        blk->state = 2u;
        return 0;
    }
    blk->end = c->b.at;
    md_tr_sync(tr, blk->entry, c->b.at);
    tr->stats.code_bytes += c->b.at - tr->arena_used;
    tr->arena_used = c->b.at;
    blk->state = 1u;
    blk->ops = (uint8_t)c->n;
    ++tr->stats.translations;
    return 1;
}

static int md_tr_match(const MdTranslator *tr, const MdTrBlock *blk, uint16_t cs, uint16_t ip)
{
    return blk->state != 0u && blk->cs == cs && blk->ip == ip && md_tr_block_fresh(tr, blk);
}

static uint8_t *md_tr_heat(MdTranslator *tr, uint16_t cs, uint16_t ip)
{
    const uint32_t x = ((uint32_t)cs << 4) + ip;
    return &tr->heat[(x ^ (x >> 7) ^ (x >> 13)) & (MD_TR_HEAT_SLOTS - 1u)];
}

static void md_tr_heat_bump(MdTranslator *tr, uint16_t cs, uint16_t ip)
{
    uint8_t *h = md_tr_heat(tr, cs, ip);
    if (*h != 0xFFu) ++*h;
}

static MdTrBlock *md_tr_lookup(MdTranslator *tr, uint16_t cs, uint16_t ip)
{
    MdTrBlock *blk = &tr->blocks[md_tr_hash(cs, ip)];
    if (blk->state != 0u && blk->cs == cs && blk->ip == ip && md_tr_block_fresh(tr, blk))
        return blk;
    (void)md_tr_translate(tr, blk, cs, ip, 1);
    return blk;
}
#endif

/* ---- run loop ------------------------------------------------------------------ */

MdStopReason md_tr_run(MdTranslator *tr, uint64_t budget)
{
    MdRuntime *rt = tr->rt;
#if !MD_TR_HOST_THUMB2 || !MICRODOS_TRANSLATION_SUPPORT
#if MICRODOS_TRANSLATION_SUPPORT
    (void)md_tr_lookup;              /* compiled everywhere, executed on Thumb-2 */
    (void)md_tr_match;
    (void)md_tr_heat_bump;
#endif
    return md_interp_run(rt, budget);
#else
    typedef uint32_t (*MdTrEnter)(uint32_t entry, uint32_t budget);
    if (tr->arena == NULL) return md_interp_run(rt, budget);
#if MD_INTERP_BACKEDGE_EXIT
    rt->native_v2_suppress_bloom[0] = 0u;
    rt->native_v2_suppress_bloom[1] = 0u;
#endif

    while (rt->stop_reason == MD_STOP_NONE) {
        MdTrBlock *blk;
        uint32_t chunk, info, kind, retired;
        MdTrEnter enter;
        const uint16_t cs = rt->cpu.cs, ip = rt->cpu.ip;

        if (budget == 0u) { rt->stop_reason = MD_STOP_BUDGET; break; }
        if (rt->code_epoch != tr->epoch) md_tr_flush(tr);

        blk = &tr->blocks[md_tr_hash(cs, ip)];
        if (!md_tr_match(tr, blk, cs, ip)) {
            if (tr->eager || *md_tr_heat(tr, cs, ip) >= MD_TR_HOT_THRESHOLD)
                (void)md_tr_translate(tr, blk, cs, ip, 1);
            else
                blk = NULL;                          /* cold: interpret */
        }
        if (blk == NULL || blk->state != 1u || blk->ops > budget) {
#if MD_INTERP_BACKEDGE_EXIT
            if (!tr->eager) {
                /* Cold or untranslatable code: run the threaded interpreter
                   at full speed until it reaches a loop head (taken JNZ/LOOP
                   back-edge), then let that loop head heat up. */
                const uint64_t before = rt->instructions;
                rt->native_v2_backedge_hit = 0u;
                (void)md_interp_run(rt, budget);
                retired = (uint32_t)(rt->instructions - before);
                budget -= retired;
                tr->stats.interp_instructions += retired;
                if (rt->native_v2_backedge_hit) {
                    const uint16_t bcs = rt->native_v2_backedge_cs, bip = rt->native_v2_backedge_ip;
                    MdTrBlock *t = &tr->blocks[md_tr_hash(bcs, bip)];
                    rt->native_v2_backedge_hit = 0u;
                    ++tr->stats.backedge_exits;
                    if (md_tr_match(tr, t, bcs, bip) && t->state == 2u) {
                        const unsigned bit = ((unsigned)bcs ^ (unsigned)bip) & 63u;
                        rt->native_v2_suppress_bloom[bit >> 5] |= (uint32_t)1u << (bit & 31u);
                        ++tr->stats.suppressed;
                    } else {
                        md_tr_heat_bump(tr, bcs, bip);
                    }
                }
                continue;
            }
#endif
            (void)md_interp_step(rt);
            --budget;
            ++tr->stats.interp_instructions;
            continue;
        }

        chunk = budget > 0x3FFFFFFFu ? 0x3FFFFFFFu : (uint32_t)budget;
        enter = (MdTrEnter)(uintptr_t)(md_tr_addr(tr->arena + tr->enter_off) | 1u);
        info = enter(md_tr_addr(tr->arena + blk->entry) | 1u, chunk);
        retired = chunk - tr->remaining;
        rt->instructions += retired;
        budget -= retired;
        tr->stats.native_instructions += retired;
        ++tr->stats.episodes;

        kind = info >> 24;
        switch (kind) {
            case MD_TR_EXIT_EDGE: {
                const uint32_t stub = info & 0xFFFFFFu;
                const uint32_t flushes = tr->stats.flushes;
                MdTrBlock *to;
                ++tr->stats.exit_edge;
                if (tr->eager) {
                    to = md_tr_lookup(tr, rt->cpu.cs, rt->cpu.ip);
                } else {
                    /* tiered: chain only to a target that is already hot */
                    to = &tr->blocks[md_tr_hash(rt->cpu.cs, rt->cpu.ip)];
                    if (!md_tr_match(tr, to, rt->cpu.cs, rt->cpu.ip)) {
                        md_tr_heat_bump(tr, rt->cpu.cs, rt->cpu.ip);
                        if (*md_tr_heat(tr, rt->cpu.cs, rt->cpu.ip) >= MD_TR_HOT_THRESHOLD)
                            (void)md_tr_translate(tr, to, rt->cpu.cs, rt->cpu.ip, 1);
                    }
                }
                if (to->state == 1u && md_tr_match(tr, to, rt->cpu.cs, rt->cpu.ip) &&
                    tr->stats.flushes == flushes) {
                    MdT2Buf b = { tr->arena, tr->arena_size, 0u, 0 };
                    /* A block chained to itself skips its page-generation
                       check: within one native episode its code can only
                       change through a slow store, which exits at once. */
                    const int self = stub >= to->entry && stub < to->end;
                    t2_patch_b(&b, stub, self ? to->body : to->entry);
                    md_tr_sync(tr, stub, stub + 4u);
                    ++tr->stats.chains;
                }
                break;
            }
            case MD_TR_EXIT_DYNAMIC:
                ++tr->stats.exit_dynamic;
                md_tr_heat_bump(tr, rt->cpu.cs, rt->cpu.ip);
                break;
            case MD_TR_EXIT_BUDGET:  ++tr->stats.exit_budget; break;
            case MD_TR_EXIT_INVALID: ++tr->stats.exit_invalid; break;
            case MD_TR_EXIT_STORE:   ++tr->stats.exit_store; break;
            default: break;
        }
    }
    return rt->stop_reason;
#endif
}
