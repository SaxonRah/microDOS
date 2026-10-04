#include "microdos/native_v2.h"
#include "microdos/decode.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum MdNv2Kind {
    MD_NV2_MOV_R16_IMM = 0,
    MD_NV2_MOV_RR16,
    MD_NV2_INC_R16,
    MD_NV2_DEC_R16,
    MD_NV2_ALU_RR16,
    MD_NV2_GRP1_R16_IMM8,
    MD_NV2_ALU_ACC_IMM16,
    MD_NV2_TEST_R16_IMM16,
    MD_NV2_ROT_R16_1,
    MD_NV2_SHIFT_R16_1,
    MD_NV2_NOT_R16,
    MD_NV2_NEG_R16,
    MD_NV2_MUL_R16,
    MD_NV2_DIV_R16,
    MD_NV2_MOV_R16_MEM16,
    MD_NV2_MOV_MEM16_R16,
    MD_NV2_LEA_INDEX_DISP8,
    MD_NV2_LODSW,
    MD_NV2_JZ,
    MD_NV2_JNZ,
    MD_NV2_JB,
    MD_NV2_JMP,
    MD_NV2_LOOP_CX,
    MD_NV2_NOP
} MdNv2Kind;

typedef struct MdNv2Op {
    uint16_t ip;
    uint16_t next_ip;
    uint16_t target;
    uint16_t imm;
    uint8_t kind;
    uint8_t dst;
    uint8_t src;
    uint8_t aux;
    uint8_t need_cf;
    uint8_t need_z;
    uint8_t retire_dynamic;
} MdNv2Op;

typedef struct MdThumbBuf {
    uint8_t *bytes;
    size_t capacity;
    size_t at;
    int failed;
} MdThumbBuf;

typedef struct MdBranchPatch {
    size_t at;
    uint16_t target_ip;
    uint8_t cond;
} MdBranchPatch;

/*
 * Pico-first register convention:
 *
 *   r0 AX    r1 CX    r2 DX    r3 BX
 *   r4 SP    r5 BP    r6 SI    r7 DI
 *
 *   r8  MdX86 architectural frame
 *   r9  guest memory host pointer
 *   r10 cached DS << 4
 *   r11 virtual x86 CF (0/1)
 *   r12 effective-address / arithmetic scratch
 *
 * No generated hot operation calls a C semantics helper.
 */
static const uint8_t md_nv2_arm_reg[8] = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u
};

static void th16(MdThumbBuf *b, uint16_t hw)
{
    if (b->failed) return;
    if (b->at + 2u > b->capacity) {
        b->failed = 1;
        return;
    }
    b->bytes[b->at++] = (uint8_t)hw;
    b->bytes[b->at++] = (uint8_t)(hw >> 8);
}

static void th32(MdThumbBuf *b, uint16_t hw1, uint16_t hw2)
{
    th16(b, hw1);
    th16(b, hw2);
}

static int th_patch16(MdThumbBuf *b, size_t at, uint16_t hw)
{
    if (b->failed || at + 2u > b->capacity) {
        b->failed = 1;
        return 0;
    }
    b->bytes[at] = (uint8_t)hw;
    b->bytes[at + 1u] = (uint8_t)(hw >> 8);
    return 1;
}

static uint16_t th_movs(unsigned rd, unsigned imm)
{
    return (uint16_t)(0x2000u | ((rd & 7u) << 8) | (imm & 0xFFu));
}

static uint16_t th_mov_hi(unsigned rd, unsigned rm)
{
    return (uint16_t)(0x4600u |
                      ((rd & 8u) << 4) |
                      ((rm & 15u) << 3) |
                      (rd & 7u));
}

static uint16_t th_add_hi(unsigned rd, unsigned rm)
{
    return (uint16_t)(0x4400u |
                      ((rd & 8u) << 4) |
                      ((rm & 15u) << 3) |
                      (rd & 7u));
}

static uint16_t th_lsl_imm(unsigned rd, unsigned rm, unsigned imm)
{
    return (uint16_t)(((imm & 31u) << 6) |
                      ((rm & 7u) << 3) |
                      (rd & 7u));
}

static uint16_t th_add_imm(unsigned rd, unsigned imm)
{
    return (uint16_t)(0x3000u | ((rd & 7u) << 8) | (imm & 0xFFu));
}

static uint16_t th_sub_imm(unsigned rd, unsigned imm)
{
    return (uint16_t)(0x3800u | ((rd & 7u) << 8) | (imm & 0xFFu));
}

static uint16_t th_cmp_imm(unsigned rn, unsigned imm)
{
    return (uint16_t)(0x2800u | ((rn & 7u) << 8) | (imm & 0xFFu));
}

static uint16_t th_cmp_reg(unsigned rn, unsigned rm)
{
    return (uint16_t)(0x4280u | ((rm & 7u) << 3) | (rn & 7u));
}

static uint16_t th_and_reg(unsigned rd, unsigned rm)
{
    return (uint16_t)(0x4000u | ((rm & 7u) << 3) | (rd & 7u));
}

static uint16_t th_eor_reg(unsigned rd, unsigned rm)
{
    return (uint16_t)(0x4040u | ((rm & 7u) << 3) | (rd & 7u));
}

static uint16_t th_orr_reg(unsigned rd, unsigned rm)
{
    return (uint16_t)(0x4300u | ((rm & 7u) << 3) | (rd & 7u));
}

static uint16_t th_uxth(unsigned rd, unsigned rm)
{
    return (uint16_t)(0xB280u | ((rm & 7u) << 3) | (rd & 7u));
}

static uint16_t th_ldrh(unsigned rt, unsigned rn, unsigned off)
{
    return (uint16_t)(0x8800u |
                      (((off >> 1) & 31u) << 6) |
                      ((rn & 7u) << 3) |
                      (rt & 7u));
}

static uint16_t th_strh(unsigned rt, unsigned rn, unsigned off)
{
    return (uint16_t)(0x8000u |
                      (((off >> 1) & 31u) << 6) |
                      ((rn & 7u) << 3) |
                      (rt & 7u));
}

static uint16_t th_strb(unsigned rt, unsigned rn, unsigned off)
{
    return (uint16_t)(0x7000u |
                      ((off & 31u) << 6) |
                      ((rn & 7u) << 3) |
                      (rt & 7u));
}

static int th_offset_ok_h(unsigned off)
{
    return (off & 1u) == 0u && off <= 62u;
}

static int th_offset_ok_b(unsigned off)
{
    return off <= 31u;
}

static void th_load_imm16(MdThumbBuf *b, unsigned rd, uint16_t value)
{
    const unsigned hi = (unsigned)(value >> 8);
    const unsigned lo = (unsigned)(value & 0xFFu);

    if (hi == 0u) {
        th16(b, th_movs(rd, lo));
    } else {
        th16(b, th_movs(rd, hi));
        th16(b, th_lsl_imm(rd, rd, 8u));
        if (lo != 0u) th16(b, th_add_imm(rd, lo));
    }
}

/* Thumb-2 wide helpers. These forms do not update APSR unless explicitly CMP. */
static void th_ldr_w_imm(MdThumbBuf *b, unsigned rt, unsigned rn, unsigned imm12)
{
    th32(b,
         (uint16_t)(0xF8D0u | (rn & 15u)),
         (uint16_t)(((rt & 15u) << 12) | (imm12 & 0x0FFFu)));
}

static void th_ldrh_w_imm(MdThumbBuf *b, unsigned rt, unsigned rn, unsigned imm12)
{
    th32(b,
         (uint16_t)(0xF8B0u | (rn & 15u)),
         (uint16_t)(((rt & 15u) << 12) | (imm12 & 0x0FFFu)));
}

static void th_strh_w_imm(MdThumbBuf *b, unsigned rt, unsigned rn, unsigned imm12)
{
    th32(b,
         (uint16_t)(0xF8A0u | (rn & 15u)),
         (uint16_t)(((rt & 15u) << 12) | (imm12 & 0x0FFFu)));
}

static void th_ldrh_w_reg(MdThumbBuf *b, unsigned rt, unsigned rn, unsigned rm)
{
    th32(b,
         (uint16_t)(0xF830u | (rn & 15u)),
         (uint16_t)(((rt & 15u) << 12) | (rm & 15u)));
}

static void th_strh_w_reg(MdThumbBuf *b, unsigned rt, unsigned rn, unsigned rm)
{
    th32(b,
         (uint16_t)(0xF820u | (rn & 15u)),
         (uint16_t)(((rt & 15u) << 12) | (rm & 15u)));
}

static void th_add_w_reg(MdThumbBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    th32(b,
         (uint16_t)(0xEB00u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void th_sub_w_reg(MdThumbBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    th32(b,
         (uint16_t)(0xEBA0u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void th_mul_w(MdThumbBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    th32(b,
         (uint16_t)(0xFB00u | (rn & 15u)),
         (uint16_t)(0xF000u |
                    ((rd & 15u) << 8) |
                    (rm & 15u)));
}

static void th_udiv_w(MdThumbBuf *b, unsigned rd, unsigned rn, unsigned rm)
{
    th32(b,
         (uint16_t)(0xFBB0u | (rn & 15u)),
         (uint16_t)(0xF0F0u |
                    ((rd & 15u) << 8) |
                    (rm & 15u)));
}

static void th_mls_w(MdThumbBuf *b,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm,
                     unsigned ra)
{
    th32(b,
         (uint16_t)(0xFB00u | (rn & 15u)),
         (uint16_t)(((ra & 15u) << 12) |
                    ((rd & 15u) << 8) |
                    0x0010u |
                    (rm & 15u)));
}

static void th_add_w_imm(MdThumbBuf *b, unsigned rd, unsigned rn, unsigned imm8)
{
    th32(b,
         (uint16_t)(0xF100u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (imm8 & 0xFFu)));
}

static void th_sub_w_imm(MdThumbBuf *b, unsigned rd, unsigned rn, unsigned imm8)
{
    th32(b,
         (uint16_t)(0xF1A0u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (imm8 & 0xFFu)));
}

/* MOVW Rd,#imm16, valid for low or high registers. */
static void th_movw(MdThumbBuf *b, unsigned rd, uint16_t imm)
{
    const unsigned i = (imm >> 11) & 1u;
    const unsigned imm4 = (imm >> 12) & 0xFu;
    const unsigned imm3 = (imm >> 8) & 7u;
    const unsigned imm8 = imm & 0xFFu;

    th32(b,
         (uint16_t)(0xF240u | (i << 10) | imm4),
         (uint16_t)((imm3 << 12) |
                    ((rd & 15u) << 8) |
                    imm8));
}

static void th_shift_w_imm(MdThumbBuf *b,
                           unsigned type,
                           unsigned rd,
                           unsigned rm,
                           unsigned imm)
{
    const unsigned imm3 = (imm >> 2) & 7u;
    const unsigned imm2 = imm & 3u;

    th32(b, 0xEA4Fu,
         (uint16_t)((imm3 << 12) |
                    ((rd & 15u) << 8) |
                    (imm2 << 6) |
                    ((type & 3u) << 4) |
                    (rm & 15u)));
}

static void th_orr_w_reg(MdThumbBuf *b,
                         unsigned rd,
                         unsigned rn,
                         unsigned rm)
{
    th32(b,
         (uint16_t)(0xEA40u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void th_orr_w_lsl(MdThumbBuf *b,
                         unsigned rd,
                         unsigned rn,
                         unsigned rm,
                         unsigned imm)
{
    const unsigned imm3 = (imm >> 2) & 7u;
    const unsigned imm2 = imm & 3u;

    th32(b,
         (uint16_t)(0xEA40u | (rn & 15u)),
         (uint16_t)((imm3 << 12) |
                    ((rd & 15u) << 8) |
                    (imm2 << 6) |
                    (rm & 15u)));
}

static void th_eor_w_reg(MdThumbBuf *b,
                         unsigned rd,
                         unsigned rn,
                         unsigned rm)
{
    th32(b,
         (uint16_t)(0xEA80u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void th_and_w_reg(MdThumbBuf *b,
                         unsigned rd,
                         unsigned rn,
                         unsigned rm)
{
    th32(b,
         (uint16_t)(0xEA00u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void th_bic_w_reg(MdThumbBuf *b,
                         unsigned rd,
                         unsigned rn,
                         unsigned rm)
{
    th32(b,
         (uint16_t)(0xEA20u | (rn & 15u)),
         (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static uint16_t th_sxtb(unsigned rd, unsigned rm)
{
    return (uint16_t)(0xB240u | ((rm & 7u) << 3) | (rd & 7u));
}


static void th_mvn_w_reg(MdThumbBuf *b, unsigned rd, unsigned rm)
{
    th32(b, 0xEA6Fu,
         (uint16_t)(((rd & 15u) << 8) | (rm & 15u)));
}

static void th_rsb_w_zero(MdThumbBuf *b, unsigned rd, unsigned rn)
{
    th32(b,
         (uint16_t)(0xF1C0u | (rn & 15u)),
         (uint16_t)((rd & 15u) << 8));
}

static void th_cmp_w_imm(MdThumbBuf *b, unsigned rn, unsigned imm8)
{
    th32(b,
         (uint16_t)(0xF1B0u | (rn & 15u)),
         (uint16_t)(0x0F00u | (imm8 & 0xFFu)));
}

static void th_tst_w_reg(MdThumbBuf *b, unsigned rn, unsigned rm)
{
    th32(b,
         (uint16_t)(0xEA10u | (rn & 15u)),
         (uint16_t)(0x0F00u | (rm & 15u)));
}

/* UBFX Rd,Rn,#16,#1. Used to capture 8086 16-bit carry/borrow. */
static void th_cf_from_bit16(MdThumbBuf *b, unsigned rn)
{
    th32(b,
         (uint16_t)(0xF3C0u | (rn & 15u)),
         0x4B00u); /* Rd=r11, lsb=16, width=1 */
}

/* UBFX r12,r12,#0,#20: physical 8086 20-bit address wrap. */
static void th_wrap20_r12(MdThumbBuf *b)
{
    th32(b, 0xF3CCu, 0x0C13u);
}

static void th_cf_zero(MdThumbBuf *b)
{
    /* MOVS.W r11,#0; later CMP re-establishes guest Z for JZ/JNZ. */
    th32(b, 0xF05Fu, 0x0B00u);
}

static size_t th_emit_bcond_placeholder(MdThumbBuf *b, unsigned cond)
{
    const size_t at = b->at;
    th16(b, (uint16_t)(0xD000u | ((cond & 0xFu) << 8)));
    return at;
}

static int th_patch_bcond(MdThumbBuf *b, size_t at, unsigned cond, size_t target)
{
    const intptr_t delta = (intptr_t)target - (intptr_t)(at + 4u);
    intptr_t imm;

    if ((delta & 1) != 0) return 0;
    imm = delta / 2;
    if (imm < -128 || imm > 127) return 0;

    return th_patch16(
        b, at,
        (uint16_t)(0xD000u |
                   ((cond & 0xFu) << 8) |
                   ((uint8_t)imm)));
}

static size_t th_emit_b_placeholder(MdThumbBuf *b)
{
    const size_t at = b->at;
    th16(b, 0xE000u);
    return at;
}

static int th_patch_b(MdThumbBuf *b, size_t at, size_t target)
{
    const intptr_t delta = (intptr_t)target - (intptr_t)(at + 4u);
    intptr_t imm;

    if ((delta & 1) != 0) return 0;
    imm = delta / 2;
    if (imm < -1024 || imm > 1023) return 0;

    return th_patch16(
        b, at,
        (uint16_t)(0xE000u | ((uint16_t)imm & 0x07FFu)));
}

static int md_nv2_find_op_ip(const MdNv2Op *ops, unsigned count, uint16_t ip)
{
    unsigned i;
    for (i = 0u; i < count; ++i) {
        if (ops[i].ip == ip) return (int)i;
    }
    return -1;
}

static int md_nv2_alu_supported(unsigned alu)
{
    return alu == 0u || alu == 1u || alu == 2u || alu == 4u ||
           alu == 5u || alu == 6u || alu == 7u;
}

static int md_nv2_ds_mod0_rm_supported(unsigned rm)
{
    /*
     * 8086 mod=00 DS-default addressing forms.
     * BP-based forms default to SS and stay in fallback for now.
     */
    return rm == 0u || rm == 1u || rm == 4u ||
           rm == 5u || rm == 7u;
}

static int md_nv2_mem_index_from_rm(unsigned rm)
{
    if (rm == 4u) return MD_X86_SI;
    if (rm == 5u) return MD_X86_DI;
    return -1;
}


static int md_nv2_op_defines_cf(const MdNv2Op *op)
{
    return op->kind == MD_NV2_ALU_RR16 ||
           op->kind == MD_NV2_GRP1_R16_IMM8 ||
           op->kind == MD_NV2_ALU_ACC_IMM16 ||
           op->kind == MD_NV2_TEST_R16_IMM16 ||
           op->kind == MD_NV2_ROT_R16_1 ||
           op->kind == MD_NV2_SHIFT_R16_1 ||
           op->kind == MD_NV2_NEG_R16 ||
           op->kind == MD_NV2_MUL_R16;
}

static int md_nv2_op_consumes_cf(const MdNv2Op *op)
{
    return (op->kind == MD_NV2_ALU_RR16 && op->aux == 2u) ||
           op->kind == MD_NV2_JB;
}

static int md_nv2_op_defines_z(const MdNv2Op *op)
{
    return op->kind == MD_NV2_INC_R16 ||
           op->kind == MD_NV2_DEC_R16 ||
           op->kind == MD_NV2_ALU_RR16 ||
           op->kind == MD_NV2_GRP1_R16_IMM8 ||
           op->kind == MD_NV2_ALU_ACC_IMM16 ||
           op->kind == MD_NV2_TEST_R16_IMM16 ||
           op->kind == MD_NV2_SHIFT_R16_1 ||
           op->kind == MD_NV2_NEG_R16;
}

/*
 * Phase 2C: backward flag liveness.
 *
 * Phase 2B maintained virtual CF and native Z after every flag-producing
 * operation. That was correct but regmix showed the cost clearly:
 *   2A  2.800 cycles/guest
 *   2B  3.601 cycles/guest
 *
 * The pass now covers the measured small-CFG subset too:
 *   - JZ/JNZ consume native Z.
 *   - ADC and JB consume virtual x86 CF in r11.
 *   - DEC/JNZ exits keep CF live through DEC for lazy handoff.
 *   - LOOP exits reconstruct the final arithmetic producer from resident
 *     registers, so CF is not live merely because the region exits.
 *
 * Most intermediate flag work is still dead and should not be emitted.
 */
static int md_nv2_op_writes_reg(const MdNv2Op *op, unsigned reg)
{
    switch ((MdNv2Kind)op->kind) {
        case MD_NV2_MOV_R16_IMM:
        case MD_NV2_MOV_RR16:
        case MD_NV2_INC_R16:
        case MD_NV2_DEC_R16:
        case MD_NV2_MOV_R16_MEM16:
        case MD_NV2_LEA_INDEX_DISP8:
            return op->dst == reg;

        case MD_NV2_ALU_RR16:
            return op->aux != 7u && op->dst == reg;

        case MD_NV2_GRP1_R16_IMM8:
            return op->aux != 7u && op->dst == reg;

        case MD_NV2_ALU_ACC_IMM16:
            return op->aux != 7u && reg == MD_X86_AX;

        case MD_NV2_ROT_R16_1:
        case MD_NV2_SHIFT_R16_1:
        case MD_NV2_NEG_R16:
        case MD_NV2_NOT_R16:
            return op->dst == reg;

        case MD_NV2_MUL_R16:
        case MD_NV2_DIV_R16:
            return reg == MD_X86_AX || reg == MD_X86_DX;

        case MD_NV2_TEST_R16_IMM16:
            return 0;

        case MD_NV2_LODSW:
            return reg == MD_X86_AX || reg == MD_X86_SI;

        case MD_NV2_LOOP_CX:
            return reg == MD_X86_CX;

        default:
            return 0;
    }
}

static int md_nv2_mem_rm_reads_reg(unsigned rm, unsigned reg)
{
    switch (rm & 7u) {
        case 0u: return reg == MD_X86_BX || reg == MD_X86_SI;
        case 1u: return reg == MD_X86_BX || reg == MD_X86_DI;
        case 4u: return reg == MD_X86_SI;
        case 5u: return reg == MD_X86_DI;
        case 7u: return reg == MD_X86_BX;
        default: return 0;
    }
}

static int md_nv2_op_reads_reg(const MdNv2Op *op, unsigned reg)
{
    switch ((MdNv2Kind)op->kind) {
        case MD_NV2_MOV_R16_IMM:
        case MD_NV2_JZ:
        case MD_NV2_JNZ:
        case MD_NV2_JB:
        case MD_NV2_JMP:
        case MD_NV2_NOP:
            return 0;

        case MD_NV2_MOV_RR16:
            return op->src == reg;

        case MD_NV2_INC_R16:
        case MD_NV2_DEC_R16:
        case MD_NV2_ROT_R16_1:
        case MD_NV2_SHIFT_R16_1:
        case MD_NV2_NOT_R16:
        case MD_NV2_NEG_R16:
        case MD_NV2_TEST_R16_IMM16:
            return op->dst == reg;

        case MD_NV2_ALU_RR16:
            return op->dst == reg || op->src == reg;

        case MD_NV2_GRP1_R16_IMM8:
            return op->dst == reg;

        case MD_NV2_ALU_ACC_IMM16:
            return reg == MD_X86_AX;

        case MD_NV2_MUL_R16:
            return reg == MD_X86_AX || op->src == reg;

        case MD_NV2_DIV_R16:
            return reg == MD_X86_AX || reg == MD_X86_DX || op->src == reg;

        case MD_NV2_MOV_R16_MEM16:
            return md_nv2_mem_rm_reads_reg(op->aux, reg);

        case MD_NV2_MOV_MEM16_R16:
            return op->src == reg || md_nv2_mem_rm_reads_reg(op->aux, reg);

        case MD_NV2_LEA_INDEX_DISP8:
            return op->src == reg;

        case MD_NV2_LODSW:
            return reg == MD_X86_SI;

        case MD_NV2_LOOP_CX:
            return reg == MD_X86_CX;

        default:
            return 1;
    }
}

static uint8_t md_nv2_chunk_mode(const MdNv2Op *ops,
                                  unsigned count,
                                  uint8_t loop_terminal)
{
    unsigned i;

    if (count < 2u)
        return 0u;

    if (loop_terminal == 0xE2u) {
        for (i = 0u; i + 1u < count; ++i) {
            if (md_nv2_op_reads_reg(&ops[i], MD_X86_CX))
                return 0u;
        }

        return ops[count - 1u].kind == MD_NV2_LOOP_CX ? 1u : 0u;
    }

    if (loop_terminal == 0x75u &&
        ops[count - 1u].kind == MD_NV2_JNZ &&
        ops[count - 1u].target == ops[0].ip &&
        ops[count - 2u].kind == MD_NV2_DEC_R16) {
        const unsigned counter = ops[count - 2u].dst & 7u;

        for (i = 0u; i + 2u < count; ++i) {
            if (md_nv2_op_reads_reg(&ops[i], counter) ||
                md_nv2_op_writes_reg(&ops[i], counter)) {
                return 0u;
            }
        }

        return 2u;
    }

    return 0u;
}

static uint8_t md_nv2_safe_store_bx_si_loop(const MdNv2Op *ops,
                                             unsigned count,
                                             uint8_t loop_terminal)
{
    unsigned stores = 0u;
    unsigned si_steps = 0u;
    unsigned i;
    int saw_store = 0;

    if (loop_terminal != 0xE2u || count < 3u)
        return 0u;

    for (i = 0u; i < count; ++i) {
        const MdNv2Op *op = &ops[i];

        if (op->kind == MD_NV2_LOOP_CX)
            continue;

        if (op->kind == MD_NV2_MOV_MEM16_R16) {
            if ((op->aux & 7u) != 0u)
                return 0u;
            ++stores;
            saw_store = 1;
            continue;
        }

        if (md_nv2_op_writes_reg(op, MD_X86_BX) ||
            md_nv2_op_writes_reg(op, MD_X86_CX)) {
            return 0u;
        }

        if (md_nv2_op_writes_reg(op, MD_X86_SI)) {
            if (!saw_store ||
                op->kind != MD_NV2_GRP1_R16_IMM8 ||
                op->aux != 0u ||
                op->dst != MD_X86_SI ||
                op->imm != 2u) {
                return 0u;
            }
            ++si_steps;
        }
    }

    return (uint8_t)(stores == 1u && si_steps == 1u);
}

static int md_nv2_prove_muldiv_pair(const MdNv2Op *ops,
                                    unsigned count,
                                    uint8_t *mul_reg_out,
                                    uint8_t *div_reg_out)
{
    int mul_i = -1;
    int div_i = -1;
    unsigned i;
    uint8_t mr = 0u;
    uint8_t dr = 0u;

    for (i = 0u; i < count; ++i) {
        if (ops[i].kind == MD_NV2_MUL_R16) {
            if (mul_i >= 0)
                return 0;
            mul_i = (int)i;
            mr = ops[i].src;
        } else if (ops[i].kind == MD_NV2_DIV_R16) {
            if (div_i >= 0)
                return 0;
            div_i = (int)i;
            dr = ops[i].src;
        }
    }

    if (mul_i < 0 && div_i < 0)
        return -1; /* no MUL/DIV pair in this region */

    if (mul_i < 0 || div_i != mul_i + 1)
        return 0;

    /*
     * The proof relies on both operands staying invariant for the complete
     * counted loop. AX and DX are written by MUL/DIV, and CX is the LOOP
     * counter, so none can be a guarded source register.
     */
    if (mr == MD_X86_AX || mr == MD_X86_DX || mr == MD_X86_CX ||
        dr == MD_X86_AX || dr == MD_X86_DX || dr == MD_X86_CX ||
        mr == dr) {
        return 0;
    }

    for (i = 0u; i < count; ++i) {
        if ((int)i == mul_i || (int)i == div_i)
            continue;
        if (md_nv2_op_writes_reg(&ops[i], mr) ||
            md_nv2_op_writes_reg(&ops[i], dr)) {
            return 0;
        }
    }

    if (mul_reg_out != NULL) *mul_reg_out = mr;
    if (div_reg_out != NULL) *div_reg_out = dr;
    return 1;
}

/*
 * Phase 3I exit shape:
 *
 *   ADD AX,imm16
 *   ROL AX,1
 *   ROR r16,1
 *   LOOP
 *
 * ROL/ROR preserve SZAP. Therefore SZAP at region exit come from the ADD,
 * while CF/OF come from the final ROR. The final AX value can be inverted
 * by one ROR to recover the ADD result exactly.
 */
static int md_nv2_match_add_rol_ror_exit(const MdNv2Op *ops,
                                         unsigned count,
                                         uint16_t *imm_out,
                                         uint8_t *ror_reg_out)
{
    const MdNv2Op *add;
    const MdNv2Op *rol;
    const MdNv2Op *ror;
    const MdNv2Op *loop;

    if (count < 4u)
        return 0;

    add = &ops[count - 4u];
    rol = &ops[count - 3u];
    ror = &ops[count - 2u];
    loop = &ops[count - 1u];

    if (add->kind != MD_NV2_ALU_ACC_IMM16 ||
        add->aux != 0u ||
        add->dst != MD_X86_AX ||
        rol->kind != MD_NV2_ROT_R16_1 ||
        rol->aux != 0u ||
        rol->dst != MD_X86_AX ||
        ror->kind != MD_NV2_ROT_R16_1 ||
        ror->aux != 1u ||
        loop->kind != MD_NV2_LOOP_CX) {
        return 0;
    }

    if (imm_out != NULL) *imm_out = add->imm;
    if (ror_reg_out != NULL) *ror_reg_out = ror->dst;
    return 1;
}

static int md_nv2_mark_dynamic_retire(MdNv2Op *ops,
                                        unsigned count,
                                        uint8_t needs_memory,
                                        uint8_t loop_terminal,
                                        uint8_t *retire_base_ops_out)
{
    unsigned branch_index = 0u;
    unsigned target_index = 0u;
    unsigned internal_edges = 0u;
    unsigned i;

    if (retire_base_ops_out == NULL)
        return -1;

    *retire_base_ops_out = (uint8_t)count;

    for (i = 0u; i < count; ++i) {
        ops[i].retire_dynamic = 0u;

        if ((ops[i].kind == MD_NV2_JZ ||
             ops[i].kind == MD_NV2_JB ||
             ops[i].kind == MD_NV2_JMP) &&
            ops[i].target != ops[0].ip) {
            int ti = md_nv2_find_op_ip(ops, count, ops[i].target);
            if (ti < 0)
                return -1;
            branch_index = i;
            target_index = (unsigned)ti;
            ++internal_edges;
        }
    }

    if (internal_edges == 0u)
        return 0;

    /*
     * Preserve Phase 3G's cheap one-optional-instruction correction.
     */
    if (internal_edges == 1u &&
        !needs_memory &&
        loop_terminal == 0x75u &&
        ops[branch_index].kind == MD_NV2_JZ &&
        target_index == branch_index + 2u &&
        target_index < count) {
        ops[branch_index + 1u].retire_dynamic = 1u;
        *retire_base_ops_out = (uint8_t)(count - 1u);
        return 1;
    }

    /*
     * Phase 3H: general non-memory CFG retirement. r9 counts every actually
     * executed guest operation, including conditional/unconditional branches
     * and the terminal counted branch. The runtime still admits by the
     * conservative maximum iterations * op_count.
     */
    if (needs_memory)
        return -1;

    for (i = 0u; i < count; ++i)
        ops[i].retire_dynamic = 1u;

    *retire_base_ops_out = 0u;
    return 2;
}

static uint8_t md_nv2_mark_flag_liveness(MdNv2Op *ops, unsigned count,
                                         unsigned exit_cf_live,
                                         uint8_t *cf_sites_out,
                                         uint8_t *z_sites_out)
{
    unsigned cf_live = exit_cf_live != 0u;
    unsigned z_live = 0u;
    unsigned cf_sites = 0u;
    unsigned z_sites = 0u;
    unsigned i = count;

    while (i != 0u) {
        MdNv2Op *op = &ops[--i];

        op->need_cf = 0u;
        op->need_z = 0u;

        if (op->kind == MD_NV2_JZ || op->kind == MD_NV2_JNZ)
            z_live = 1u;

        if (md_nv2_op_defines_z(op)) {
            if (z_live) {
                op->need_z = 1u;
                ++z_sites;
            }
            z_live = 0u;
        }

        if (md_nv2_op_defines_cf(op)) {
            if (cf_live) {
                op->need_cf = 1u;
                ++cf_sites;
            }
            cf_live = 0u;
        }

        if (md_nv2_op_consumes_cf(op))
            cf_live = 1u;

        /* INC/DEC preserve CF, so cf_live deliberately flows through them. */
    }

    *cf_sites_out = (uint8_t)cf_sites;
    *z_sites_out = (uint8_t)z_sites;
    return (uint8_t)(cf_live != 0u);
}

static MdNativeV2Status md_nv2_lower(const uint8_t *image,
                                     size_t image_size,
                                     uint16_t image_base,
                                     uint16_t entry_ip,
                                     MdNv2Op *ops,
                                     unsigned *count_out,
                                     uint16_t *end_ip_out,
                                     uint8_t *has_loop_out,
                                     uint8_t *needs_memory_out,
                                     uint8_t *has_store_out,
                                     uint8_t *exit_flags_reg_out,
                                     uint8_t *needs_entry_cf_out,
                                     uint8_t *cf_sites_out,
                                     uint8_t *z_sites_out,
                                     uint8_t *loop_terminal_out,
                                     uint8_t *exit_lazy_op_out,
                                     uint8_t *exit_flag_dst_out,
                                     uint8_t *exit_flag_src_out,
                                     uint16_t *exit_flag_imm_out,
                                     uint8_t *requires_df_clear_out)
{
    const uint32_t image_end32 =
        (uint32_t)image_base + (uint32_t)image_size;
    uint16_t ip = entry_ip;
    unsigned count = 0u;
    unsigned conditional_count = 0u;
    int last_flag_kind = -1;
    uint8_t last_flag_reg = 0u;
    uint8_t has_loop = 0u;
    uint8_t needs_memory = 0u;
    uint8_t has_store = 0u;
    uint8_t loop_terminal = 0u;
    uint8_t exit_lazy_op = MD_LAZY_NONE;
    uint8_t exit_flag_dst = 0u;
    uint8_t exit_flag_src = 0xFFu;
    uint16_t exit_flag_imm = 0u;
    uint8_t requires_df_clear = 0u;
    int last_flag_index = -1;

    if (image == NULL || ops == NULL || count_out == NULL ||
        end_ip_out == NULL || has_loop_out == NULL ||
        needs_memory_out == NULL || has_store_out == NULL ||
        exit_flags_reg_out == NULL || needs_entry_cf_out == NULL ||
        cf_sites_out == NULL || z_sites_out == NULL ||
        loop_terminal_out == NULL || exit_lazy_op_out == NULL ||
        exit_flag_dst_out == NULL || exit_flag_src_out == NULL ||
        exit_flag_imm_out == NULL || requires_df_clear_out == NULL) {
        return MD_NATIVE_V2_BAD_ARGUMENT;
    }

    if (image_size == 0u || image_end32 > 0x10000u ||
        entry_ip < image_base || (uint32_t)entry_ip >= image_end32) {
        return MD_NATIVE_V2_BAD_ARGUMENT;
    }

    while ((uint32_t)ip < image_end32) {
        MdDecodedInstruction inst;
        MdNv2Op *op;
        const size_t off = (size_t)(uint16_t)(ip - image_base);
        const uint8_t *q = image + off;
        uint8_t opcode;

        if (count >= MD_NATIVE_V2_MAX_OPS)
            return MD_NATIVE_V2_TOO_LARGE;

        if (!md_decode_8086(image, image_size, image_base, ip, &inst) ||
            !inst.valid_8086) {
            return MD_NATIVE_V2_DECODE_ERROR;
        }

        if (inst.prefix_count != 0u)
            return MD_NATIVE_V2_UNSUPPORTED;

        opcode = inst.opcode;
        op = &ops[count];
        memset(op, 0, sizeof(*op));
        op->ip = inst.ip;
        op->next_ip = inst.next_ip;
        op->target = inst.target;

        if ((opcode & 0xF8u) == 0xB8u) {
            if (inst.length != 3u || off + 2u >= image_size)
                return MD_NATIVE_V2_DECODE_ERROR;

            op->kind = MD_NV2_MOV_R16_IMM;
            op->dst = (uint8_t)(opcode & 7u);
            op->imm = (uint16_t)((uint16_t)q[1] |
                                 ((uint16_t)q[2] << 8));
        } else if ((opcode == 0x89u || opcode == 0x8Bu) &&
                   inst.has_modrm) {
            const unsigned mod = inst.modrm >> 6;
            const unsigned mreg = (inst.modrm >> 3) & 7u;
            const unsigned mrm = inst.modrm & 7u;

            if (mod == 3u) {
                op->kind = MD_NV2_MOV_RR16;
                if (opcode == 0x8Bu) {
                    op->dst = (uint8_t)mreg;
                    op->src = (uint8_t)mrm;
                } else {
                    op->dst = (uint8_t)mrm;
                    op->src = (uint8_t)mreg;
                }
            } else if (mod == 0u && md_nv2_ds_mod0_rm_supported(mrm)) {
                needs_memory = 1u;
                op->aux = (uint8_t)mrm;
                if (opcode == 0x8Bu) {
                    op->kind = MD_NV2_MOV_R16_MEM16;
                    op->dst = (uint8_t)mreg;
                } else {
                    op->kind = MD_NV2_MOV_MEM16_R16;
                    op->src = (uint8_t)mreg;
                    has_store = 1u;
                }
            } else {
                return MD_NATIVE_V2_UNSUPPORTED;
            }
        } else if (opcode == 0x8Du && inst.has_modrm &&
                   (inst.modrm >> 6) == 1u) {
            const unsigned mreg = (inst.modrm >> 3) & 7u;
            const unsigned mrm = inst.modrm & 7u;
            const int idx = md_nv2_mem_index_from_rm(mrm);

            if (idx < 0 || off + 2u >= image_size)
                return MD_NATIVE_V2_UNSUPPORTED;

            op->kind = MD_NV2_LEA_INDEX_DISP8;
            op->dst = (uint8_t)mreg;
            op->src = (uint8_t)idx;
            op->imm = (uint16_t)(int16_t)(int8_t)q[2];
        } else if ((opcode & 0xF8u) == 0x40u) {
            op->kind = MD_NV2_INC_R16;
            op->dst = (uint8_t)(opcode & 7u);
            last_flag_kind = MD_NV2_INC_R16;
            last_flag_reg = op->dst;
            last_flag_index = (int)count;
        } else if ((opcode & 0xF8u) == 0x48u) {
            op->kind = MD_NV2_DEC_R16;
            op->dst = (uint8_t)(opcode & 7u);
            last_flag_kind = MD_NV2_DEC_R16;
            last_flag_reg = op->dst;
            last_flag_index = (int)count;
        } else if (opcode <= 0x3Bu &&
                   (opcode & 1u) != 0u &&
                   ((opcode & 7u) == 1u || (opcode & 7u) == 3u) &&
                   inst.has_modrm &&
                   (inst.modrm >> 6) == 3u) {
            const unsigned alu = (opcode >> 3) & 7u;
            const unsigned mreg = (inst.modrm >> 3) & 7u;
            const unsigned mrm = inst.modrm & 7u;

            if (!md_nv2_alu_supported(alu))
                return MD_NATIVE_V2_UNSUPPORTED;

            op->kind = MD_NV2_ALU_RR16;
            op->aux = (uint8_t)alu;

            if (opcode & 2u) {
                op->dst = (uint8_t)mreg;
                op->src = (uint8_t)mrm;
            } else {
                op->dst = (uint8_t)mrm;
                op->src = (uint8_t)mreg;
            }

            last_flag_kind = MD_NV2_ALU_RR16;
            last_flag_reg = op->dst;
            last_flag_index = (int)count;
        } else if (opcode == 0x05u || opcode == 0x35u ||
                   opcode == 0x3Du) {
            if (inst.length != 3u || off + 2u >= image_size)
                return MD_NATIVE_V2_DECODE_ERROR;

            op->kind = MD_NV2_ALU_ACC_IMM16;
            op->aux = opcode == 0x05u ? 0u :
                      opcode == 0x35u ? 6u : 7u;
            op->dst = MD_X86_AX;
            op->imm = (uint16_t)((uint16_t)q[1] |
                                 ((uint16_t)q[2] << 8));

            last_flag_kind = MD_NV2_ALU_ACC_IMM16;
            last_flag_reg = MD_X86_AX;
            last_flag_index = (int)count;
        } else if (opcode == 0x83u &&
                   inst.has_modrm &&
                   (inst.modrm >> 6) == 3u) {
            const unsigned alu = (inst.modrm >> 3) & 7u;
            const int8_t simm = (int8_t)q[2];

            /*
             * Phase 2B keeps sign-negative immediates in fallback until the
             * full carry model is generalized. OR is admitted only for imm=0.
             */
            if (!((alu == 0u || alu == 5u || alu == 7u) && simm >= 0) &&
                !(alu == 1u && simm == 0)) {
                return MD_NATIVE_V2_UNSUPPORTED;
            }

            op->kind = MD_NV2_GRP1_R16_IMM8;
            op->aux = (uint8_t)alu;
            op->dst = (uint8_t)(inst.modrm & 7u);
            op->imm = (uint16_t)(uint8_t)simm;

            last_flag_kind = MD_NV2_GRP1_R16_IMM8;
            last_flag_reg = op->dst;
            last_flag_index = (int)count;
        } else if (opcode == 0xA9u) { /* TEST AX,imm16 */
            if (inst.length != 3u || off + 2u >= image_size)
                return MD_NATIVE_V2_DECODE_ERROR;

            op->kind = MD_NV2_TEST_R16_IMM16;
            op->dst = MD_X86_AX;
            op->imm = (uint16_t)((uint16_t)q[1] |
                                 ((uint16_t)q[2] << 8));
            last_flag_kind = MD_NV2_TEST_R16_IMM16;
            last_flag_reg = MD_X86_AX;
            last_flag_index = (int)count;
        } else if (opcode == 0xF7u &&
                   inst.has_modrm &&
                   (inst.modrm >> 6) == 3u) {
            const unsigned ext = (inst.modrm >> 3) & 7u;

            if (ext == 0u) { /* TEST r16,imm16 */
                if (inst.length != 4u || off + 3u >= image_size)
                    return MD_NATIVE_V2_DECODE_ERROR;

                op->kind = MD_NV2_TEST_R16_IMM16;
                op->dst = (uint8_t)(inst.modrm & 7u);
                op->imm = (uint16_t)((uint16_t)q[2] |
                                     ((uint16_t)q[3] << 8));
                last_flag_kind = MD_NV2_TEST_R16_IMM16;
                last_flag_reg = op->dst;
                last_flag_index = (int)count;
            } else if (ext == 2u) { /* NOT r16 */
                op->kind = MD_NV2_NOT_R16;
                op->dst = (uint8_t)(inst.modrm & 7u);
            } else if (ext == 3u) { /* NEG r16 */
                op->kind = MD_NV2_NEG_R16;
                op->dst = (uint8_t)(inst.modrm & 7u);
                last_flag_kind = MD_NV2_NEG_R16;
                last_flag_reg = op->dst;
                last_flag_index = (int)count;
            } else if (ext == 4u) { /* MUL r16 */
                op->kind = MD_NV2_MUL_R16;
                op->src = (uint8_t)(inst.modrm & 7u);
                last_flag_kind = MD_NV2_MUL_R16;
                last_flag_reg = MD_X86_AX;
                last_flag_index = (int)count;
            } else if (ext == 6u) { /* DIV r16 */
                op->kind = MD_NV2_DIV_R16;
                op->src = (uint8_t)(inst.modrm & 7u);

                /*
                 * DIV leaves FLAGS undefined on hardware; microDOS preserves
                 * the incoming arithmetic flags. Do not replace the previous
                 * flag producer here.
                 */
            } else {
                return MD_NATIVE_V2_UNSUPPORTED;
            }
        } else if (opcode == 0xD1u &&
                   inst.has_modrm &&
                   (inst.modrm >> 6) == 3u) {
            const unsigned ext = (inst.modrm >> 3) & 7u;

            if (ext == 0u || ext == 1u) {
                op->kind = MD_NV2_ROT_R16_1;
                op->aux = (uint8_t)ext;
            } else if (ext == 5u) {
                op->kind = MD_NV2_SHIFT_R16_1; /* SHR */
                op->aux = 5u;
            } else {
                return MD_NATIVE_V2_UNSUPPORTED;
            }

            op->dst = (uint8_t)(inst.modrm & 7u);
            last_flag_kind = op->kind;
            last_flag_reg = op->dst;
            last_flag_index = (int)count;
        } else if (opcode == 0xADu) {
            needs_memory = 1u;
            requires_df_clear = 1u;
            op->kind = MD_NV2_LODSW;
        } else if (opcode == 0xE2u) {
            ++conditional_count;

            if (conditional_count > 8u ||
                inst.flow != MD_DECODE_FLOW_CONDITIONAL ||
                inst.target < entry_ip ||
                (uint32_t)inst.target >= image_end32) {
                return MD_NATIVE_V2_UNSUPPORTED;
            }

            op->kind = MD_NV2_LOOP_CX;
            has_loop = 1u;
            loop_terminal = 0xE2u;
        } else if (opcode == 0x72u || opcode == 0x74u ||
                   opcode == 0x75u) {
            ++conditional_count;

            if (conditional_count > 8u ||
                inst.flow != MD_DECODE_FLOW_CONDITIONAL ||
                inst.target < entry_ip ||
                (uint32_t)inst.target >= image_end32) {
                return MD_NATIVE_V2_UNSUPPORTED;
            }

            op->kind = opcode == 0x72u ? MD_NV2_JB :
                       opcode == 0x74u ? MD_NV2_JZ : MD_NV2_JNZ;

            if (opcode == 0x75u && inst.target < inst.next_ip) {
                if (loop_terminal != 0u)
                    return MD_NATIVE_V2_UNSUPPORTED;
                has_loop = 1u;
                loop_terminal = opcode;
            } else if (inst.target <= inst.next_ip) {
                return MD_NATIVE_V2_UNSUPPORTED;
            }
        } else if (opcode == 0xEBu &&
                   inst.flow == MD_DECODE_FLOW_JUMP) {
            if (inst.target <= inst.next_ip ||
                inst.target < entry_ip ||
                (uint32_t)inst.target >= image_end32) {
                return MD_NATIVE_V2_UNSUPPORTED;
            }
            op->kind = MD_NV2_JMP;
        } else if (opcode == 0x90u) {
            op->kind = MD_NV2_NOP;
        } else {
            return MD_NATIVE_V2_UNSUPPORTED;
        }

        ++count;
        ip = inst.next_ip;
    }

    if (loop_terminal == 0xE2u) {
        const MdNv2Op *f;

        if (count < 2u || last_flag_index != (int)count - 2)
            return MD_NATIVE_V2_UNSUPPORTED;

        f = &ops[last_flag_index];
        if (f->kind == MD_NV2_ALU_RR16 &&
            (f->aux == 0u || f->aux == 1u || f->aux == 4u ||
             f->aux == 5u || f->aux == 6u)) {
            if ((f->aux == 0u || f->aux == 5u) && f->dst == f->src)
                return MD_NATIVE_V2_UNSUPPORTED;
            exit_lazy_op =
                (f->aux == 0u) ? MD_LAZY_ADD16 :
                (f->aux == 5u) ? MD_LAZY_SUB16 : MD_LAZY_LOGIC16;
            exit_flag_dst = f->dst;
            exit_flag_src = f->src;
        } else if (f->kind == MD_NV2_GRP1_R16_IMM8 &&
                   (f->aux == 0u || f->aux == 1u || f->aux == 5u)) {
            exit_lazy_op =
                (f->aux == 0u) ? MD_LAZY_ADD16 :
                (f->aux == 5u) ? MD_LAZY_SUB16 : MD_LAZY_LOGIC16;
            exit_flag_dst = f->dst;
            exit_flag_src = 0xFFu;
            exit_flag_imm = f->imm;
        } else if (f->kind == MD_NV2_ALU_ACC_IMM16 &&
                   f->aux == 0u) {
            exit_lazy_op = MD_LAZY_ADD16;
            exit_flag_dst = MD_X86_AX;
            exit_flag_src = 0xFFu;
            exit_flag_imm = f->imm;
        } else {
            uint16_t special_imm = 0u;
            uint8_t special_ror = 0u;

            if (!md_nv2_match_add_rol_ror_exit(
                    ops, count, &special_imm, &special_ror)) {
                return MD_NATIVE_V2_UNSUPPORTED;
            }

            (void)special_ror;
            exit_lazy_op = MD_LAZY_ADD16;
            exit_flag_dst = MD_X86_AX;
            exit_flag_src = 0xFFu;
            exit_flag_imm = special_imm;
        }

        *needs_entry_cf_out =
            md_nv2_mark_flag_liveness(ops, count, 0u,
                                       cf_sites_out, z_sites_out);
    } else {
        if (last_flag_kind != MD_NV2_DEC_R16 || last_flag_reg == MD_X86_AX)
            return MD_NATIVE_V2_UNSUPPORTED;
        *needs_entry_cf_out =
            md_nv2_mark_flag_liveness(ops, count, 1u, cf_sites_out, z_sites_out);
    }

    *count_out = count;
    *end_ip_out = ip;
    *has_loop_out = has_loop;
    *needs_memory_out = needs_memory;
    *has_store_out = has_store;
    *exit_flags_reg_out = last_flag_reg;
    *loop_terminal_out = loop_terminal;
    *exit_lazy_op_out = exit_lazy_op;
    *exit_flag_dst_out = exit_flag_dst;
    *exit_flag_src_out = exit_flag_src;
    *exit_flag_imm_out = exit_flag_imm;
    *requires_df_clear_out = requires_df_clear;
    return MD_NATIVE_V2_OK;
}

static void md_nv2_emit_truncate(MdThumbBuf *b, unsigned rd, int need_z)
{
    th16(b, th_uxth(rd, rd));
    if (need_z) th16(b, th_cmp_imm(rd, 0u));
}

static void md_nv2_emit_logic_flags(MdThumbBuf *b, unsigned rd,
                                    int need_cf, int need_z)
{
    if (need_cf) th_cf_zero(b);
    if (need_z) th16(b, th_cmp_imm(rd, 0u));
}

static void md_nv2_emit_ds_index_addr(MdThumbBuf *b, unsigned index_reg)
{
    th16(b, th_mov_hi(12u, 10u));       /* r12 = DS base */
    th16(b, th_add_hi(12u, index_reg)); /* + SI/DI, does not set APSR */
    th_wrap20_r12(b);
}

static void md_nv2_emit_ds_mod0_addr(MdThumbBuf *b, unsigned rm)
{
    th16(b, th_mov_hi(12u, 10u)); /* r12 = DS base */

    switch (rm & 7u) {
        case 0u: /* [BX+SI] */
            th16(b, th_add_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
            th16(b, th_add_hi(12u, md_nv2_arm_reg[MD_X86_SI]));
            break;

        case 1u: /* [BX+DI] */
            th16(b, th_add_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
            th16(b, th_add_hi(12u, md_nv2_arm_reg[MD_X86_DI]));
            break;

        case 4u: /* [SI] */
            th16(b, th_add_hi(12u, md_nv2_arm_reg[MD_X86_SI]));
            break;

        case 5u: /* [DI] */
            th16(b, th_add_hi(12u, md_nv2_arm_reg[MD_X86_DI]));
            break;

        case 7u: /* [BX] */
            th16(b, th_add_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
            break;

        default:
            b->failed = 1;
            return;
    }

    th_wrap20_r12(b);
}

static void md_nv2_emit_exit_regs_only(MdThumbBuf *b)
{
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    unsigned i;

    if (!th_offset_ok_h(roff)) {
        b->failed = 1;
        return;
    }

    th16(b, 0xB401u);
    th16(b, th_mov_hi(0u, 8u));
    for (i = 1u; i < 8u; ++i)
        th16(b, th_strh(md_nv2_arm_reg[i], 0u, roff + i * 2u));
    th16(b, 0xBC02u);
    th16(b, th_strh(1u, 0u, roff));
}

static void md_nv2_emit_exit_dec_flags(MdThumbBuf *b, unsigned flag_reg)
{
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    const unsigned ipoff = (unsigned)offsetof(MdX86, ip);
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    const unsigned aoff = (unsigned)offsetof(MdX86, lazy_a);
    const unsigned boff = (unsigned)offsetof(MdX86, lazy_b);
    const unsigned resoff = (unsigned)offsetof(MdX86, lazy_res);
    unsigned i;

    if (!th_offset_ok_h(roff) || !th_offset_ok_h(ipoff) ||
        !th_offset_ok_b(opoff) || !th_offset_ok_b(coff) ||
        !th_offset_ok_h(aoff) || !th_offset_ok_h(boff) ||
        !th_offset_ok_h(resoff)) {
        b->failed = 1;
        return;
    }

    /* Save guest AX; r0 becomes MdX86*. */
    th16(b, 0xB401u);             /* push {r0} */
    th16(b, th_mov_hi(0u, 8u));

    /* Spill guest CX..DI first. */
    for (i = 1u; i < 8u; ++i)
        th16(b, th_strh(md_nv2_arm_reg[i], 0u, roff + i * 2u));

    /*
     * Recreate the canonical lazy DEC16 state:
     *   carry = virtual CF preserved by DEC
     *   a     = result + 1
     *   b     = 1
     *   res   = result
     */
    th16(b, th_mov_hi(2u, 11u));
    th16(b, th_strb(2u, 0u, coff));

    th16(b, th_movs(2u, MD_LAZY_DEC16));
    th16(b, th_strb(2u, 0u, opoff));

    th16(b, th_mov_hi(2u, md_nv2_arm_reg[flag_reg]));
    th16(b, th_strh(2u, 0u, resoff));
    th16(b, th_add_imm(2u, 1u));
    th16(b, th_uxth(2u, 2u));
    th16(b, th_strh(2u, 0u, aoff));

    th16(b, th_movs(2u, 1u));
    th16(b, th_strh(2u, 0u, boff));

    /* Restore saved guest AX into scratch r1 and spill it. */
    th16(b, 0xBC02u);             /* pop {r1} */
    th16(b, th_strh(1u, 0u, roff));

    /* IP and return value are emitted by the caller. */
}

/* Phase 3J: update exact x86 FLAGS in r11 after DEC BX.
 * r3=result, bit0 of r11 already holds CF preserved from NEG.
 * r7/r12 are scratch. */
static void md_nv2_emit_phase7_dec_flags(MdThumbBuf *b)
{
    /* Clear OF/SF/ZF/AF/PF; preserve CF + non-arithmetic bits. */
    th_movw(b, 7u, 0x08D4u);
    th_bic_w_reg(b, 11u, 11u, 7u);

    /* SF = result bit15 -> FLAGS bit7. */
    th16(b, th_mov_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
    th_shift_w_imm(b, 1u, 12u, 12u, 8u);
    th_movw(b, 7u, 0x0080u);
    th_and_w_reg(b, 12u, 12u, 7u);
    th_orr_w_reg(b, 11u, 11u, 12u);

    /* ZF = !result, branchlessly. */
    th16(b, th_mov_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
    th_rsb_w_zero(b, 7u, 12u);
    th_orr_w_reg(b, 12u, 12u, 7u);
    th_shift_w_imm(b, 1u, 12u, 12u, 31u);
    th_movw(b, 7u, 1u);
    th_eor_w_reg(b, 12u, 12u, 7u);
    th_shift_w_imm(b, 0u, 12u, 12u, 6u);
    th_orr_w_reg(b, 11u, 11u, 12u);

    /* AF for DEC iff result low nibble is F. */
    th16(b, th_mov_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
    th_movw(b, 7u, 0x000Fu);
    th_and_w_reg(b, 12u, 12u, 7u);
    th_add_w_imm(b, 12u, 12u, 1u);
    th_shift_w_imm(b, 1u, 12u, 12u, 4u);
    th_shift_w_imm(b, 0u, 12u, 12u, 4u);
    th_orr_w_reg(b, 11u, 11u, 12u);

    /* PF = even parity of result low byte. */
    th16(b, th_mov_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
    th_movw(b, 7u, 0x00FFu);
    th_and_w_reg(b, 12u, 12u, 7u);
    th16(b, th_mov_hi(7u, 12u));
    th_shift_w_imm(b, 1u, 7u, 7u, 4u);
    th_eor_w_reg(b, 12u, 12u, 7u);
    th16(b, th_mov_hi(7u, 12u));
    th_shift_w_imm(b, 1u, 7u, 7u, 2u);
    th_eor_w_reg(b, 12u, 12u, 7u);
    th16(b, th_mov_hi(7u, 12u));
    th_shift_w_imm(b, 1u, 7u, 7u, 1u);
    th_eor_w_reg(b, 12u, 12u, 7u);
    th_movw(b, 7u, 1u);
    th_and_w_reg(b, 12u, 12u, 7u);
    th_eor_w_reg(b, 12u, 12u, 7u);
    th_shift_w_imm(b, 0u, 12u, 12u, 2u);
    th_orr_w_reg(b, 11u, 11u, 12u);

    /* OF for DEC iff result == 7FFFh. */
    th16(b, th_mov_hi(12u, md_nv2_arm_reg[MD_X86_BX]));
    th_movw(b, 7u, 0x7FFFu);
    th_eor_w_reg(b, 12u, 12u, 7u);
    th_rsb_w_zero(b, 7u, 12u);
    th_orr_w_reg(b, 12u, 12u, 7u);
    th_shift_w_imm(b, 1u, 12u, 12u, 31u);
    th_movw(b, 7u, 1u);
    th_eor_w_reg(b, 12u, 12u, 7u);
    th_shift_w_imm(b, 0u, 12u, 12u, 11u);
    th_orr_w_reg(b, 11u, 11u, 12u);
}

/* -------------------------------------------------------------------------
 * Phase 3M: local CALL/RET graph lowering for the measured MDSTRESS Phase-4
 * inner loop.
 *
 * The guest graph is static and entirely within the current CS segment:
 *
 *     caller:  call proc_a
 *              loop caller
 *
 *     proc_a -> proc_b -> proc_c
 *
 * The generated Thumb body is flattened, but every x86 CALL/PUSH/POP/RET
 * still performs its exact guest SS:SP memory effect. This keeps the guest
 * stack architecturally visible while avoiding C semantic helpers and host
 * dispatch inside the hot loop. Runtime separately guards the complete
 * 16-byte stack window against every compiled guest-code span.
 * ------------------------------------------------------------------------- */

static uint8_t md_nv2_guest8(const uint8_t *memory, uint16_t cs, uint16_t ip)
{
    return memory[md_x86_linear(cs, ip)];
}

static uint16_t md_nv2_guest16(const uint8_t *memory, uint16_t cs, uint16_t ip)
{
    const uint8_t lo = md_nv2_guest8(memory, cs, ip);
    const uint8_t hi = md_nv2_guest8(memory, cs, (uint16_t)(ip + 1u));
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static uint16_t md_nv2_rel16_target(uint16_t next_ip, uint16_t disp)
{
    return (uint16_t)(next_ip + (int16_t)disp);
}

static int md_nv2_match_bytes(const uint8_t *memory,
                              uint16_t cs,
                              uint16_t ip,
                              const uint8_t *pattern,
                              size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (md_nv2_guest8(memory, cs, (uint16_t)(ip + (uint16_t)i)) !=
            pattern[i]) {
            return 0;
        }
    }
    return 1;
}

static void md_nv2_call_stack_addr(MdThumbBuf *b)
{
    th16(b, th_mov_hi(12u, 10u));
    th16(b, th_add_hi(12u, 4u));
    th_wrap20_r12(b);
}

static void md_nv2_call_push_reg(MdThumbBuf *b, unsigned reg)
{
    th_sub_w_imm(b, 4u, 4u, 2u);
    th16(b, th_uxth(4u, 4u));
    md_nv2_call_stack_addr(b);
    th_strh_w_reg(b, reg, 9u, 12u);
}

static void md_nv2_call_push_imm(MdThumbBuf *b, uint16_t value)
{
    th_sub_w_imm(b, 4u, 4u, 2u);
    th16(b, th_uxth(4u, 4u));
    th_movw(b, 11u, value);
    md_nv2_call_stack_addr(b);
    th_strh_w_reg(b, 11u, 9u, 12u);
}

static void md_nv2_call_pop_reg(MdThumbBuf *b, unsigned reg)
{
    md_nv2_call_stack_addr(b);
    th_ldrh_w_reg(b, reg, 9u, 12u);
    th_add_w_imm(b, 4u, 4u, 2u);
    th16(b, th_uxth(4u, 4u));
}

static void md_nv2_call_ret_pop(MdThumbBuf *b)
{
    md_nv2_call_stack_addr(b);
    th_ldrh_w_reg(b, 11u, 9u, 12u);
    th_add_w_imm(b, 4u, 4u, 2u);
    th16(b, th_uxth(4u, 4u));
}

static MdNativeV2Status md_nv2_compile_phase4_call_loop(
    const uint8_t *memory,
    uint16_t cs,
    uint16_t entry_ip,
    uint16_t proc_a,
    uint16_t proc_b,
    uint16_t proc_c,
    MdNativeV2Code *out)
{
    MdThumbBuf b;
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    const unsigned ssoff = (unsigned)offsetof(MdX86, ss);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned ipoff = (unsigned)offsetof(MdX86, ip);
    size_t loop_native;
    size_t loop_exit_branch;
    size_t loop_back_branch;
    size_t loop_done;
    unsigned i;

    (void)memory;
    (void)cs;

    if (out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    if (!th_offset_ok_h(roff) || !th_offset_ok_h(ipoff) ||
        ssoff > 0x0FFFu || moff > 0x0FFFu)
        return MD_NATIVE_V2_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    b.bytes = out->bytes;
    b.capacity = sizeof(out->bytes);
    b.at = 0u;
    b.failed = 0;

    /* Resident guest GPRs plus guest memory and SS base. */
    th16(&b, th_mov_hi(8u, 0u));
    for (i = 1u; i < 8u; ++i) {
        if (!th_offset_ok_h(roff + i * 2u))
            return MD_NATIVE_V2_UNSUPPORTED;
        th16(&b, th_ldrh(md_nv2_arm_reg[i], 0u, roff + i * 2u));
    }
    th_ldr_w_imm(&b, 9u, 8u, moff);
    th_ldrh_w_imm(&b, 10u, 8u, ssoff);
    th32(&b, 0xEA4Fu, 0x1A0Au); /* lsl.w r10,r10,#4 */
    th_ldrh_w_imm(&b, 0u, 8u, roff);

    loop_native = b.at;

    /* caller: CALL proc_a -- push guest return IP 0351-equivalent. */
    md_nv2_call_push_imm(&b, (uint16_t)(entry_ip + 3u));

    /* proc_a: push ax, bx, dx; CALL proc_b. */
    md_nv2_call_push_reg(&b, 0u);
    md_nv2_call_push_reg(&b, 3u);
    md_nv2_call_push_reg(&b, 2u);
    md_nv2_call_push_imm(&b, (uint16_t)(proc_a + 6u));

    /* proc_b: push si, di; CALL proc_c. */
    md_nv2_call_push_reg(&b, 6u);
    md_nv2_call_push_reg(&b, 7u);
    md_nv2_call_push_imm(&b, (uint16_t)(proc_b + 5u));

    /* proc_c: add ax,1; adc bx,ax; xor si,bx; rol di,1. */
    th_add_w_imm(&b, 0u, 0u, 1u);
    th_cf_from_bit16(&b, 0u);
    th16(&b, th_uxth(0u, 0u));

    th_add_w_reg(&b, 3u, 3u, 0u);
    th_add_w_reg(&b, 3u, 3u, 11u);
    th16(&b, th_uxth(3u, 3u));

    th16(&b, th_eor_reg(6u, 3u));
    th16(&b, th_uxth(6u, 6u));

    th_shift_w_imm(&b, 1u, 12u, 7u, 15u);
    th_shift_w_imm(&b, 0u, 7u, 7u, 1u);
    th_orr_w_reg(&b, 7u, 7u, 12u);
    th16(&b, th_uxth(7u, 7u));

    /* RET proc_c; proc_b: xchg si,di; pop di; pop si; RET. */
    md_nv2_call_ret_pop(&b);
    th16(&b, th_mov_hi(12u, 6u));
    th16(&b, th_mov_hi(6u, 7u));
    th16(&b, th_mov_hi(7u, 12u));
    md_nv2_call_pop_reg(&b, 7u);
    md_nv2_call_pop_reg(&b, 6u);
    md_nv2_call_ret_pop(&b);

    /* proc_a: pop dx,bx,ax; inc ax; xor bx,ax; RET. */
    md_nv2_call_pop_reg(&b, 2u);
    md_nv2_call_pop_reg(&b, 3u);
    md_nv2_call_pop_reg(&b, 0u);
    th_add_w_imm(&b, 0u, 0u, 1u);
    th16(&b, th_uxth(0u, 0u));
    th16(&b, th_eor_reg(3u, 0u));
    th16(&b, th_uxth(3u, 3u));
    md_nv2_call_ret_pop(&b);

    /* caller: LOOP entry_ip. LOOP itself preserves FLAGS. */
    th_sub_w_imm(&b, 1u, 1u, 1u);
    th16(&b, th_uxth(1u, 1u));
    th16(&b, th_cmp_imm(1u, 0u));
    loop_exit_branch = th_emit_bcond_placeholder(&b, 0u); /* BEQ done */
    loop_back_branch = th_emit_b_placeholder(&b);
    loop_done = b.at;

    if (!th_patch_bcond(&b, loop_exit_branch, 0u, loop_done) ||
        !th_patch_b(&b, loop_back_branch, loop_native)) {
        return MD_NATIVE_V2_BRANCH_RANGE;
    }

    md_nv2_emit_exit_regs_only(&b);
    th16(&b, th_mov_hi(0u, 8u));
    th_load_imm16(&b, 1u, (uint16_t)(entry_ip + 5u));
    th16(&b, th_strh(1u, 0u, ipoff));
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0x4770u);

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    out->_align_word = 0x4E563242u;
    out->size = (uint16_t)b.at;
    out->op_count = 24u;
    out->start_ip = entry_ip;
    out->end_ip = (uint16_t)(entry_ip + 5u);
    out->has_local_loop = 1u;
    out->phase = 12u;
    out->needs_memory = 1u;
    out->has_store = 1u;
    out->requires_safe_ss_word = 1u;
    out->loop_terminal = 0xE2u;
    out->exit_lazy_op = MD_LAZY_LOGIC16;
    out->exit_flag_dst = MD_X86_BX;
    out->exit_flag_src = 0xFFu;
    out->retire_base_ops = 24u;
    out->chunkable_loop = 1u;
    out->local_call_graph = 1u;
    out->call_stack_bytes = 16u;
    out->guest_span_count = 4u;
    out->guest_span_ip[0] = entry_ip;
    out->guest_span_len[0] = 5u;
    out->guest_span_ip[1] = proc_a;
    out->guest_span_len[1] = 13u;
    out->guest_span_ip[2] = proc_b;
    out->guest_span_len[2] = 10u;
    out->guest_span_ip[3] = proc_c;
    out->guest_span_len[3] = 10u;
    return MD_NATIVE_V2_OK;
}

bool md_native_v2_find_local_call_loop_entry(const uint8_t *memory,
                                              uint16_t cs,
                                              uint16_t observed_ip,
                                              uint16_t *entry_ip_out,
                                              uint8_t *prefix_ops_out,
                                              uint8_t *zero_counter_out)
{
    uint16_t entry_ip;
    uint8_t prefix_ops = 0u;
    uint8_t zero_counter = 0u;

    if (memory == NULL || entry_ip_out == NULL ||
        prefix_ops_out == NULL || zero_counter_out == NULL) {
        return false;
    }

    entry_ip = observed_ip;

    /* Direct root: CALL rel16 ; LOOP -5 back to the CALL. */
    if (md_nv2_guest8(memory, cs, observed_ip) == 0xE8u &&
        md_nv2_guest8(memory, cs, (uint16_t)(observed_ip + 3u)) == 0xE2u &&
        md_nv2_guest8(memory, cs, (uint16_t)(observed_ip + 4u)) == 0xFBu) {
        /* already the native graph root */
    } else if (observed_ip <= 0xFFF9u &&
               md_nv2_guest8(memory, cs, observed_ip) == 0x33u &&
               md_nv2_guest8(memory, cs, (uint16_t)(observed_ip + 1u)) == 0xC9u &&
               md_nv2_guest8(memory, cs, (uint16_t)(observed_ip + 2u)) == 0xE8u &&
               md_nv2_guest8(memory, cs, (uint16_t)(observed_ip + 5u)) == 0xE2u &&
               md_nv2_guest8(memory, cs, (uint16_t)(observed_ip + 6u)) == 0xFBu) {
        /*
         * Outer Phase-4 header: XOR CX,CX is a one-shot prelude. Its FLAGS
         * are dead before the first consumer because proc_c begins with ADD,
         * while its CX=0 result means 65,536 LOOP iterations.
         */
        entry_ip = (uint16_t)(observed_ip + 2u);
        prefix_ops = 1u;
        zero_counter = 1u;
    } else {
        return false;
    }

    *entry_ip_out = entry_ip;
    *prefix_ops_out = prefix_ops;
    *zero_counter_out = zero_counter;
    return true;
}

static int md_nv2_match_xchg_si_di(const uint8_t *memory,
                                    uint16_t cs,
                                    uint16_t ip)
{
    const uint8_t opcode = md_nv2_guest8(memory, cs, ip);
    const uint8_t modrm = md_nv2_guest8(memory, cs, (uint16_t)(ip + 1u));

    /*
     * XCHG r/m16,r16 is symmetric. NASM currently emits 87 F7 for
     * `xchg si,di`, while the original Phase-3M synthetic fixture used
     * the equally valid 87 FE encoding. Admit either exact SI/DI form.
     */
    return opcode == 0x87u && (modrm == 0xF7u || modrm == 0xFEu);
}

static int md_nv2_match_add_ax_1(const uint8_t *memory,
                                  uint16_t cs,
                                  uint16_t ip)
{
    /*
     * Both encodings are 8086-equivalent and three bytes long:
     *
     *   05 01 00    ADD AX,0001h       (NASM's MDSTRESS.COM)
     *   83 C0 01    ADD AX,+1          (original synthetic fixture)
     *
     * Keeping both prevents the graph proof from depending on an
     * assembler's choice of canonical immediate encoding.
     */
    if (md_nv2_guest8(memory, cs, ip) == 0x05u &&
        md_nv2_guest8(memory, cs, (uint16_t)(ip + 1u)) == 0x01u &&
        md_nv2_guest8(memory, cs, (uint16_t)(ip + 2u)) == 0x00u) {
        return 1;
    }

    return md_nv2_guest8(memory, cs, ip) == 0x83u &&
           md_nv2_guest8(memory, cs, (uint16_t)(ip + 1u)) == 0xC0u &&
           md_nv2_guest8(memory, cs, (uint16_t)(ip + 2u)) == 0x01u;
}

MdNativeV2Status md_native_v2_compile_local_call_loop(const uint8_t *memory,
                                                       uint16_t cs,
                                                       uint16_t entry_ip,
                                                       MdNativeV2Code *out,
                                                       uint8_t *counter_reg_out)
{
    static const uint8_t a_tail[7] = {
        0x5Au,0x5Bu,0x58u,0x40u,0x33u,0xD8u,0xC3u
    };
    static const uint8_t b_after_xchg[3] = {
        0x5Fu,0x5Eu,0xC3u
    };
    static const uint8_t c_after_add[7] = {
        0x13u,0xD8u,0x33u,0xF3u,0xD1u,0xC7u,0xC3u
    };
    uint16_t proc_a;
    uint16_t proc_b;
    uint16_t proc_c;

    if (memory == NULL || out == NULL || counter_reg_out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    /* Root must be exactly: CALL rel16 ; LOOP back to the CALL. */
    if (md_nv2_guest8(memory, cs, entry_ip) != 0xE8u ||
        md_nv2_guest8(memory, cs, (uint16_t)(entry_ip + 3u)) != 0xE2u ||
        md_nv2_guest8(memory, cs, (uint16_t)(entry_ip + 4u)) != 0xFBu) {
        return MD_NATIVE_V2_UNSUPPORTED;
    }

    proc_a = md_nv2_rel16_target(
        (uint16_t)(entry_ip + 3u),
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 1u)));

    /* proc_a: PUSH AX/BX/DX; CALL proc_b; POP DX/BX/AX; INC AX; XOR BX,AX; RET */
    if (md_nv2_guest8(memory, cs, proc_a) != 0x50u ||
        md_nv2_guest8(memory, cs, (uint16_t)(proc_a + 1u)) != 0x53u ||
        md_nv2_guest8(memory, cs, (uint16_t)(proc_a + 2u)) != 0x52u ||
        md_nv2_guest8(memory, cs, (uint16_t)(proc_a + 3u)) != 0xE8u ||
        !md_nv2_match_bytes(memory, cs, (uint16_t)(proc_a + 6u),
                            a_tail, sizeof(a_tail))) {
        return MD_NATIVE_V2_UNSUPPORTED;
    }
    proc_b = md_nv2_rel16_target(
        (uint16_t)(proc_a + 6u),
        md_nv2_guest16(memory, cs, (uint16_t)(proc_a + 4u)));

    /* proc_b: PUSH SI/DI; CALL proc_c; XCHG SI,DI; POP DI/SI; RET */
    if (md_nv2_guest8(memory, cs, proc_b) != 0x56u ||
        md_nv2_guest8(memory, cs, (uint16_t)(proc_b + 1u)) != 0x57u ||
        md_nv2_guest8(memory, cs, (uint16_t)(proc_b + 2u)) != 0xE8u ||
        !md_nv2_match_xchg_si_di(memory, cs, (uint16_t)(proc_b + 5u)) ||
        !md_nv2_match_bytes(memory, cs, (uint16_t)(proc_b + 7u),
                            b_after_xchg, sizeof(b_after_xchg))) {
        return MD_NATIVE_V2_UNSUPPORTED;
    }
    proc_c = md_nv2_rel16_target(
        (uint16_t)(proc_b + 5u),
        md_nv2_guest16(memory, cs, (uint16_t)(proc_b + 3u)));

    if (!md_nv2_match_add_ax_1(memory, cs, proc_c) ||
        !md_nv2_match_bytes(memory, cs, (uint16_t)(proc_c + 3u),
                            c_after_add, sizeof(c_after_add))) {
        return MD_NATIVE_V2_UNSUPPORTED;
    }

    /* Keep the first production graph non-overlapping and bounded. */
    if ((uint16_t)(proc_a + 13u) < proc_a ||
        (uint16_t)(proc_b + 10u) < proc_b ||
        (uint16_t)(proc_c + 10u) < proc_c)
        return MD_NATIVE_V2_UNSUPPORTED;

    *counter_reg_out = MD_X86_CX;
    return md_nv2_compile_phase4_call_loop(
        memory, cs, entry_ip, proc_a, proc_b, proc_c, out);
}

/* -------------------------------------------------------------------------
 * Phase 3P: native REP/string execution for MDSTRESS Phase 3.
 *
 * Measured guest shape (46 bytes, 18 retired instructions per outer pass):
 *
 *   mov ax,bp
 *   mov di,SRC       / mov cx,N / rep stosw
 *   mov si,SRC       / mov di,DST / mov cx,N / rep movsw
 *   mov si,SRC       / mov di,DST / mov cx,N / repe cmpsw
 *   mov ax,SCAN      / mov di,DST / mov cx,N / repne scasw
 *   dec bp
 *   jnz outer
 *
 * The REP instructions are single guest instructions but perform thousands of
 * word accesses. The interpreter therefore understates their cost badly in
 * retired-instruction profiles. This emitter keeps all guest GPRs resident and
 * runs each REP micro-loop directly in Thumb-2 with no C helper calls.
 *
 * The first production proof deliberately requires DS==ES and DF=0 at runtime.
 * That is exactly the MDSTRESS setup (PUSH DS / POP ES / CLD), and it lets the
 * generated body use one cached segment base while the runtime proves both
 * direct-store spans cannot overlap the executing guest code.
 * ------------------------------------------------------------------------- */

static int md_nv2_match_rep_string_loop(const uint8_t *memory,
                                         uint16_t cs,
                                         uint16_t entry_ip,
                                         uint16_t *src_out,
                                         uint16_t *dst_out,
                                         uint16_t *words_out,
                                         uint16_t *scan_out)
{
    uint16_t src;
    uint16_t dst;
    uint16_t words;
    uint16_t scan;
    int8_t rel;

    if (memory == NULL || src_out == NULL || dst_out == NULL ||
        words_out == NULL || scan_out == NULL)
        return 0;

    /* Keep runtime capture contiguous; production target is nowhere near wrap. */
    if (entry_ip > 0xFFD1u)
        return 0;

    if (md_nv2_guest8(memory, cs, entry_ip + 0u) != 0x8Bu ||
        md_nv2_guest8(memory, cs, entry_ip + 1u) != 0xC5u || /* mov ax,bp */
        md_nv2_guest8(memory, cs, entry_ip + 2u) != 0xBFu || /* mov di,src */
        md_nv2_guest8(memory, cs, entry_ip + 5u) != 0xB9u || /* mov cx,n */
        md_nv2_guest8(memory, cs, entry_ip + 8u) != 0xF3u ||
        md_nv2_guest8(memory, cs, entry_ip + 9u) != 0xABu || /* rep stosw */
        md_nv2_guest8(memory, cs, entry_ip + 10u) != 0xBEu ||
        md_nv2_guest8(memory, cs, entry_ip + 13u) != 0xBFu ||
        md_nv2_guest8(memory, cs, entry_ip + 16u) != 0xB9u ||
        md_nv2_guest8(memory, cs, entry_ip + 19u) != 0xF3u ||
        md_nv2_guest8(memory, cs, entry_ip + 20u) != 0xA5u || /* rep movsw */
        md_nv2_guest8(memory, cs, entry_ip + 21u) != 0xBEu ||
        md_nv2_guest8(memory, cs, entry_ip + 24u) != 0xBFu ||
        md_nv2_guest8(memory, cs, entry_ip + 27u) != 0xB9u ||
        md_nv2_guest8(memory, cs, entry_ip + 30u) != 0xF3u ||
        md_nv2_guest8(memory, cs, entry_ip + 31u) != 0xA7u || /* repe cmpsw */
        md_nv2_guest8(memory, cs, entry_ip + 32u) != 0xB8u ||
        md_nv2_guest8(memory, cs, entry_ip + 35u) != 0xBFu ||
        md_nv2_guest8(memory, cs, entry_ip + 38u) != 0xB9u ||
        md_nv2_guest8(memory, cs, entry_ip + 41u) != 0xF2u ||
        md_nv2_guest8(memory, cs, entry_ip + 42u) != 0xAFu || /* repne scasw */
        md_nv2_guest8(memory, cs, entry_ip + 43u) != 0x4Du || /* dec bp */
        md_nv2_guest8(memory, cs, entry_ip + 44u) != 0x75u) { /* jnz */
        return 0;
    }

    src = md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 3u));
    words = md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 6u));
    dst = md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 14u));
    scan = md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 33u));

    if (words == 0u || (src & 1u) != 0u || (dst & 1u) != 0u)
        return 0;

    /* All four setup groups must use the same buffers/count. */
    if (md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 11u)) != src ||
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 17u)) != words ||
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 22u)) != src ||
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 25u)) != dst ||
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 28u)) != words ||
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 36u)) != dst ||
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 39u)) != words) {
        return 0;
    }

    rel = (int8_t)md_nv2_guest8(memory, cs, (uint16_t)(entry_ip + 45u));
    if ((uint16_t)(entry_ip + 46u + rel) != entry_ip)
        return 0;

    *src_out = src;
    *dst_out = dst;
    *words_out = words;
    *scan_out = scan;
    return 1;
}

static void md_nv2_rep_addr(MdThumbBuf *b, unsigned index_reg)
{
    /* Phase-3P runtime proves DS==ES and no 20-bit wrap. */
    th16(b, th_mov_hi(12u, 10u));
    th16(b, th_add_hi(12u, index_reg));
}

static void md_nv2_rep_advance(MdThumbBuf *b, unsigned index_reg)
{
    th_add_w_imm(b, index_reg, index_reg, 2u);
    th16(b, th_uxth(index_reg, index_reg));
}

static void md_nv2_rep_dec_cx(MdThumbBuf *b)
{
    th_sub_w_imm(b, md_nv2_arm_reg[MD_X86_CX],
                 md_nv2_arm_reg[MD_X86_CX], 1u);
    th16(b, th_uxth(md_nv2_arm_reg[MD_X86_CX],
                    md_nv2_arm_reg[MD_X86_CX]));
}

static MdNativeV2Status md_nv2_compile_phase3_rep_loop(
    uint16_t entry_ip,
    uint16_t src_off,
    uint16_t dst_off,
    uint16_t words,
    uint16_t scan_value,
    MdNativeV2Code *out)
{
    MdThumbBuf b;
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned ipoff = (unsigned)offsetof(MdX86, ip);
    size_t outer_native;
    size_t stos_native, stos_back;
    size_t movs_native, movs_back;
    size_t cmps_native, cmps_mismatch, cmps_back, cmps_done_jump, cmps_done;
    size_t scas_native, scas_match, scas_back, scas_done_jump, scas_done;
    size_t outer_exit, outer_back, outer_done;
    unsigned i;

    if (out == NULL || words == 0u)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    if (!th_offset_ok_h(roff) || !th_offset_ok_h(ipoff) ||
        dsoff > 0x0FFFu || moff > 0x0FFFu)
        return MD_NATIVE_V2_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    b.bytes = out->bytes;
    b.capacity = sizeof(out->bytes);
    b.at = 0u;
    b.failed = 0;

    /* Resident GPRs, guest memory pointer, and the shared DS/ES base. */
    th16(&b, th_mov_hi(8u, 0u));
    for (i = 1u; i < 8u; ++i) {
        if (!th_offset_ok_h(roff + i * 2u))
            return MD_NATIVE_V2_UNSUPPORTED;
        th16(&b, th_ldrh(md_nv2_arm_reg[i], 0u, roff + i * 2u));
    }
    th_ldr_w_imm(&b, 9u, 8u, moff);
    th_ldrh_w_imm(&b, 10u, 8u, dsoff);
    th32(&b, 0xEA4Fu, 0x1A0Au); /* lsl.w r10,r10,#4 */
    th_ldrh_w_imm(&b, 0u, 8u, roff);

    outer_native = b.at;

    /* mov ax,bp ; mov di,SRC ; mov cx,N */
    th16(&b, th_mov_hi(0u, md_nv2_arm_reg[MD_X86_BP]));
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_DI], src_off);
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_CX], words);

    /* REP STOSW: ES==DS, forward. */
    stos_native = b.at;
    md_nv2_rep_addr(&b, md_nv2_arm_reg[MD_X86_DI]);
    th_strh_w_reg(&b, md_nv2_arm_reg[MD_X86_AX], 9u, 12u);
    md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_DI]);
    md_nv2_rep_dec_cx(&b);
    th16(&b, th_cmp_imm(md_nv2_arm_reg[MD_X86_CX], 0u));
    stos_back = th_emit_bcond_placeholder(&b, 1u); /* BNE */
    if (!th_patch_bcond(&b, stos_back, 1u, stos_native))
        return MD_NATIVE_V2_BRANCH_RANGE;

    /* mov si,SRC ; mov di,DST ; mov cx,N ; REP MOVSW. */
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_SI], src_off);
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_DI], dst_off);
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_CX], words);
    th16(&b, 0xB401u); /* host push {r0}: preserve guest AX */
    movs_native = b.at;
    md_nv2_rep_addr(&b, md_nv2_arm_reg[MD_X86_SI]);
    th_ldrh_w_reg(&b, 0u, 9u, 12u);
    md_nv2_rep_addr(&b, md_nv2_arm_reg[MD_X86_DI]);
    th_strh_w_reg(&b, 0u, 9u, 12u);
    md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_SI]);
    md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_DI]);
    md_nv2_rep_dec_cx(&b);
    th16(&b, th_cmp_imm(md_nv2_arm_reg[MD_X86_CX], 0u));
    movs_back = th_emit_bcond_placeholder(&b, 1u);
    if (!th_patch_bcond(&b, movs_back, 1u, movs_native))
        return MD_NATIVE_V2_BRANCH_RANGE;
    th16(&b, 0xBC01u); /* host pop {r0} */

    /* mov si,SRC ; mov di,DST ; mov cx,N ; REPE CMPSW. */
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_SI], src_off);
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_DI], dst_off);
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_CX], words);
    th16(&b, 0xB405u); /* host push {r0,r2}: preserve AX,DX */
    cmps_native = b.at;
    md_nv2_rep_addr(&b, md_nv2_arm_reg[MD_X86_SI]);
    th_ldrh_w_reg(&b, 0u, 9u, 12u);
    md_nv2_rep_addr(&b, md_nv2_arm_reg[MD_X86_DI]);
    th_ldrh_w_reg(&b, 2u, 9u, 12u);
    th16(&b, th_cmp_reg(0u, 2u));
    cmps_mismatch = th_emit_bcond_placeholder(&b, 1u); /* BNE mismatch */

    /* Equal iteration: indexes/CX advance, then repeat while CX!=0. */
    md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_SI]);
    md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_DI]);
    md_nv2_rep_dec_cx(&b);
    th16(&b, th_cmp_imm(md_nv2_arm_reg[MD_X86_CX], 0u));
    cmps_back = th_emit_bcond_placeholder(&b, 1u);
    cmps_done_jump = th_emit_b_placeholder(&b);

    /* Mismatch still consumes the just-compared element, then REP stops. */
    {
        const size_t mismatch_at = b.at;
        md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_SI]);
        md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_DI]);
        md_nv2_rep_dec_cx(&b);
        cmps_done = b.at;

        if (!th_patch_bcond(&b, cmps_mismatch, 1u, mismatch_at) ||
            !th_patch_bcond(&b, cmps_back, 1u, cmps_native) ||
            !th_patch_b(&b, cmps_done_jump, cmps_done)) {
            return MD_NATIVE_V2_BRANCH_RANGE;
        }
    }
    th16(&b, 0xBC05u); /* host pop {r0,r2} */

    /* mov ax,SCAN ; mov di,DST ; mov cx,N ; REPNE SCASW. */
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_AX], scan_value);
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_DI], dst_off);
    th_load_imm16(&b, md_nv2_arm_reg[MD_X86_CX], words);
    th16(&b, 0xB404u); /* host push {r2}: preserve guest DX */
    scas_native = b.at;
    md_nv2_rep_addr(&b, md_nv2_arm_reg[MD_X86_DI]);
    th_ldrh_w_reg(&b, 2u, 9u, 12u);
    th16(&b, th_cmp_reg(0u, 2u));
    scas_match = th_emit_bcond_placeholder(&b, 0u); /* BEQ match */

    /* Non-match: consume element, continue while CX != 0. */
    md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_DI]);
    md_nv2_rep_dec_cx(&b);
    th16(&b, th_cmp_imm(md_nv2_arm_reg[MD_X86_CX], 0u));
    scas_back = th_emit_bcond_placeholder(&b, 1u);
    scas_done_jump = th_emit_b_placeholder(&b);

    {
        const size_t match_at = b.at;
        md_nv2_rep_advance(&b, md_nv2_arm_reg[MD_X86_DI]);
        md_nv2_rep_dec_cx(&b);
        scas_done = b.at;

        if (!th_patch_bcond(&b, scas_match, 0u, match_at) ||
            !th_patch_bcond(&b, scas_back, 1u, scas_native) ||
            !th_patch_b(&b, scas_done_jump, scas_done)) {
            return MD_NATIVE_V2_BRANCH_RANGE;
        }
    }

    /* Final SCASW CF = borrow from AX - last memory word. */
    th16(&b, th_mov_hi(12u, 0u));
    th_sub_w_reg(&b, 12u, 12u, 2u);
    th_cf_from_bit16(&b, 12u);
    th16(&b, 0xBC04u); /* host pop {r2} */

    /* DEC BP / JNZ outer. DEC preserves the SCASW CF held in r11. */
    th_sub_w_imm(&b, md_nv2_arm_reg[MD_X86_BP],
                 md_nv2_arm_reg[MD_X86_BP], 1u);
    th16(&b, th_uxth(md_nv2_arm_reg[MD_X86_BP],
                     md_nv2_arm_reg[MD_X86_BP]));
    th16(&b, th_cmp_imm(md_nv2_arm_reg[MD_X86_BP], 0u));
    outer_exit = th_emit_bcond_placeholder(&b, 0u);
    outer_back = th_emit_b_placeholder(&b);
    outer_done = b.at;

    if (!th_patch_bcond(&b, outer_exit, 0u, outer_done) ||
        !th_patch_b(&b, outer_back, outer_native)) {
        return MD_NATIVE_V2_BRANCH_RANGE;
    }

    md_nv2_emit_exit_dec_flags(&b, MD_X86_BP);
    th16(&b, th_mov_hi(0u, 8u));
    th_load_imm16(&b, 1u, (uint16_t)(entry_ip + 46u));
    th16(&b, th_strh(1u, 0u, ipoff));
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0x4770u); /* bx lr */

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    out->_align_word = 0x4E563242u;
    out->size = (uint16_t)b.at;
    out->op_count = 18u;
    out->start_ip = entry_ip;
    out->end_ip = (uint16_t)(entry_ip + 46u);
    out->has_local_loop = 1u;
    out->phase = 13u;
    out->needs_memory = 1u;
    out->has_store = 1u;
    out->requires_safe_ds_word = 1u;
    out->requires_df_clear = 1u;
    out->exit_flags_reg = MD_X86_BP;
    out->loop_terminal = 0x75u;
    out->exit_lazy_op = MD_LAZY_DEC16;
    out->exit_flag_dst = MD_X86_BP;
    out->exit_flag_src = 0xFFu;
    out->retire_base_ops = 18u;
    out->chunkable_loop = 0u; /* body reads BP via MOV AX,BP */
    out->safe_rep_string_loop = 1u;
    out->requires_ds_eq_es = 1u;
    out->rep_src_off = src_off;
    out->rep_dst_off = dst_off;
    out->rep_words = words;
    return MD_NATIVE_V2_OK;
}

MdNativeV2Status md_native_v2_compile_rep_string_loop(const uint8_t *memory,
                                                       uint16_t cs,
                                                       uint16_t entry_ip,
                                                       MdNativeV2Code *out,
                                                       uint8_t *counter_reg_out)
{
    uint16_t src_off;
    uint16_t dst_off;
    uint16_t words;
    uint16_t scan_value;

    if (memory == NULL || out == NULL || counter_reg_out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    if (!md_nv2_match_rep_string_loop(memory, cs, entry_ip,
                                       &src_off, &dst_off,
                                       &words, &scan_value)) {
        return MD_NATIVE_V2_UNSUPPORTED;
    }

    *counter_reg_out = MD_X86_BP;
    return md_nv2_compile_phase3_rep_loop(entry_ip, src_off, dst_off,
                                           words, scan_value, out);
}

/* Dedicated helper-free emitter for the measured Phase-7 rare-opcode loop.
 * Guest DI is temporarily saved on the host stack so r7 can be a second
 * scratch register for exact FLAGS synthesis. */
static MdNativeV2Status md_nv2_compile_phase7_flags_loop(
    uint16_t entry_ip, MdNativeV2Code *out)
{
    MdThumbBuf b;
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    const unsigned ssoff = (unsigned)offsetof(MdX86, ss);
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned ipoff = (unsigned)offsetof(MdX86, ip);
    size_t loop_native;
    size_t loop_exit_branch;
    size_t loop_back_branch;
    size_t loop_done;
    unsigned i;

    if (out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    if (!th_offset_ok_h(roff) || !th_offset_ok_h(ipoff) ||
        !th_offset_ok_h(foff) || !th_offset_ok_b(opoff) ||
        ssoff > 0x0FFFu || moff > 0x0FFFu)
        return MD_NATIVE_V2_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    b.bytes = out->bytes;
    b.capacity = sizeof(out->bytes);
    b.at = 0u;
    b.failed = 0;

    th16(&b, th_mov_hi(8u, 0u));
    for (i = 1u; i < 8u; ++i) {
        if (!th_offset_ok_h(roff + i * 2u))
            return MD_NATIVE_V2_UNSUPPORTED;
        th16(&b, th_ldrh(md_nv2_arm_reg[i], 0u, roff + i * 2u));
    }

    th_ldr_w_imm(&b, 9u, 8u, moff);
    th_ldrh_w_imm(&b, 10u, 8u, ssoff);
    th32(&b, 0xEA4Fu, 0x1A0Au); /* lsl.w r10,r10,#4 */
    th_ldrh_w_imm(&b, 11u, 8u, foff);
    th_movw(&b, 12u, MD_X86_FLAG_ALWAYS1);
    th_orr_w_reg(&b, 11u, 11u, 12u);
    th_ldrh_w_imm(&b, 0u, 8u, roff);

    th16(&b, 0xB480u); /* push {r7}: save guest DI */
    loop_native = b.at;

    /* XCHG AX,BX. */
    th16(&b, th_mov_hi(12u, 0u));
    th16(&b, th_mov_hi(0u, 3u));
    th16(&b, th_mov_hi(3u, 12u));

    /* PUSHF. */
    th_sub_w_imm(&b, 4u, 4u, 2u);
    th16(&b, th_uxth(4u, 4u));
    th16(&b, th_mov_hi(12u, 10u));
    th16(&b, th_add_hi(12u, 4u));
    th_wrap20_r12(&b);
    th_strh_w_reg(&b, 11u, 9u, 12u);

    /* LAHF. */
    th16(&b, th_mov_hi(12u, 11u));
    th_movw(&b, 7u, 0x00D5u);
    th_and_w_reg(&b, 12u, 12u, 7u);
    th_add_w_imm(&b, 12u, 12u, 2u);
    th16(&b, (uint16_t)(0xB2C0u | (0u << 3) | 0u)); /* uxtb r0,r0 */
    th_shift_w_imm(&b, 0u, 12u, 12u, 8u);
    th_orr_w_reg(&b, 0u, 0u, 12u);
    th16(&b, th_uxth(0u, 0u));

    /* XOR AH,5Ah. Its produced flags are overwritten before any consumer. */
    th_movw(&b, 12u, 0x5A00u);
    th_eor_w_reg(&b, 0u, 0u, 12u);
    th16(&b, th_uxth(0u, 0u));

    /* SAHF. */
    th16(&b, th_mov_hi(12u, 0u));
    th_shift_w_imm(&b, 1u, 12u, 12u, 8u);
    th_movw(&b, 7u, 0x00D5u);
    th_and_w_reg(&b, 12u, 12u, 7u);
    th_bic_w_reg(&b, 11u, 11u, 7u);
    th_orr_w_reg(&b, 11u, 11u, 12u);

    /* POPF. */
    th16(&b, th_mov_hi(12u, 10u));
    th16(&b, th_add_hi(12u, 4u));
    th_wrap20_r12(&b);
    th_ldrh_w_reg(&b, 11u, 9u, 12u);
    th_add_w_imm(&b, 4u, 4u, 2u);
    th16(&b, th_uxth(4u, 4u));

    /* NOT AX. */
    th_mvn_w_reg(&b, 0u, 0u);
    th16(&b, th_uxth(0u, 0u));

    /* NEG BX; retain only its CF because INC/DEC overwrite the rest. */
    th_rsb_w_zero(&b, 3u, 3u);
    th16(&b, th_uxth(3u, 3u));
    th_shift_w_imm(&b, 1u, 11u, 11u, 1u);
    th_shift_w_imm(&b, 0u, 11u, 11u, 1u);
    th16(&b, th_mov_hi(12u, 3u));
    th_rsb_w_zero(&b, 7u, 12u);
    th_orr_w_reg(&b, 12u, 12u, 7u);
    th_shift_w_imm(&b, 1u, 12u, 12u, 31u);
    th_orr_w_reg(&b, 11u, 11u, 12u);

    /* CBW; CWD. */
    th16(&b, th_sxtb(0u, 0u));
    th16(&b, th_uxth(0u, 0u));
    th_shift_w_imm(&b, 2u, 2u, 0u, 15u);
    th16(&b, th_uxth(2u, 2u));

    /* XCHG DX,SI. */
    th16(&b, th_mov_hi(12u, 2u));
    th16(&b, th_mov_hi(2u, 6u));
    th16(&b, th_mov_hi(6u, 12u));

    /* INC AX; then DEC BX, whose flags survive LOOP. */
    th_add_w_imm(&b, 0u, 0u, 1u);
    th16(&b, th_uxth(0u, 0u));
    th_sub_w_imm(&b, 3u, 3u, 1u);
    th16(&b, th_uxth(3u, 3u));
    md_nv2_emit_phase7_dec_flags(&b);

    /* LOOP preserves x86 FLAGS in r11. */
    th_sub_w_imm(&b, 1u, 1u, 1u);
    th16(&b, th_uxth(1u, 1u));
    th16(&b, th_cmp_imm(1u, 0u));
    loop_exit_branch = th_emit_bcond_placeholder(&b, 0u); /* BEQ done */
    loop_back_branch = th_emit_b_placeholder(&b);         /* B loop */
    loop_done = b.at;
    if (!th_patch_bcond(&b, loop_exit_branch, 0u, loop_done) ||
        !th_patch_b(&b, loop_back_branch, loop_native)) {
        return MD_NATIVE_V2_BRANCH_RANGE;
    }

    th16(&b, 0xBC80u); /* pop {r7}: restore guest DI */

    /* Spill GPRs, exact materialized FLAGS, and clear lazy state. */
    th16(&b, 0xB401u); /* push {r0} */
    th16(&b, th_mov_hi(0u, 8u));
    for (i = 1u; i < 8u; ++i)
        th16(&b, th_strh(md_nv2_arm_reg[i], 0u, roff + i * 2u));
    th_strh_w_imm(&b, 11u, 0u, foff);
    th16(&b, th_movs(2u, MD_LAZY_NONE));
    th16(&b, th_strb(2u, 0u, opoff));
    th16(&b, 0xBC02u); /* pop {r1}: saved guest AX */
    th16(&b, th_strh(1u, 0u, roff));

    th_load_imm16(&b, 1u, (uint16_t)(entry_ip + 20u));
    th16(&b, th_strh(1u, 0u, ipoff));
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0x4770u);

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    out->_align_word = 0x4E563242u;
    out->size = (uint16_t)b.at;
    out->op_count = 14u;
    out->start_ip = entry_ip;
    out->end_ip = (uint16_t)(entry_ip + 20u);
    out->has_local_loop = 1u;
    out->phase = 11u;
    out->needs_memory = 1u;
    out->has_store = 1u;
    out->loop_terminal = 0xE2u;
    out->retire_base_ops = 14u;
    out->needs_entry_flags = 1u;
    out->requires_safe_ss_word = 1u;
    out->safe_stack_pushpop = 1u;
    out->chunkable_loop = 1u;
    return MD_NATIVE_V2_OK;
}

static MdNativeV2Status md_nv2_emit(const MdNv2Op *ops,
                                    unsigned count,
                                    uint16_t start_ip,
                                    uint16_t end_ip,
                                    uint8_t has_loop,
                                    uint8_t needs_memory,
                                    uint8_t has_store,
                                    uint8_t exit_flags_reg,
                                    uint8_t needs_entry_cf,
                                    uint8_t cf_sites,
                                    uint8_t z_sites,
                                    uint8_t loop_terminal,
                                    uint8_t exit_lazy_op,
                                    uint8_t exit_flag_dst,
                                    uint8_t exit_flag_src,
                                    uint16_t exit_flag_imm,
                                    uint8_t requires_df_clear,
                                    uint8_t dynamic_retire,
                                    uint8_t retire_base_ops,
                                    MdNativeV2Code *out)
{
    MdThumbBuf b;
    MdBranchPatch patches[8];
    size_t op_native[MD_NATIVE_V2_MAX_OPS];
    unsigned patch_count = 0u;
    unsigned i;
    int z_valid = 0;
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned ipoff = (unsigned)offsetof(MdX86, ip);

    if (out == NULL || ops == NULL || count == 0u)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    memset(out, 0, sizeof(*out));

    b.bytes = out->bytes;
    b.capacity = sizeof(out->bytes);
    b.at = 0u;
    b.failed = 0;

    if (!th_offset_ok_h(ipoff) || moff > 0x0FFFu ||
        dsoff > 0x0FFFu || foff > 0x0FFFu) {
        return MD_NATIVE_V2_UNSUPPORTED;
    }

    /*
     * Entry: all eight guest GPRs become resident, then runtime state goes to
     * high registers. r0 remains MdX86* until AX is loaded last.
     */
    th16(&b, th_mov_hi(8u, 0u));

    for (i = 1u; i < 8u; ++i) {
        if (!th_offset_ok_h(roff + i * 2u))
            return MD_NATIVE_V2_UNSUPPORTED;
        th16(&b, th_ldrh(md_nv2_arm_reg[i], 0u, roff + i * 2u));
    }

    if (dynamic_retire) {
        if (needs_memory)
            return MD_NATIVE_V2_UNSUPPORTED;
        th_movw(&b, 9u, 0u); /* dynamic optional-op execution counter */
    } else if (needs_memory) {
        th_ldr_w_imm(&b, 9u, 8u, moff);
    }

    if (needs_memory) {
        th_ldrh_w_imm(&b, 10u, 8u, dsoff);
        th32(&b, 0xEA4Fu, 0x1A0Au); /* lsl.w r10,r10,#4 */
    }

    if (needs_entry_cf) {
        th_ldrh_w_imm(&b, 11u, 8u, foff);
        th32(&b, 0xF3CBu, 0x0B00u); /* ubfx r11,r11,#0,#1 */
    }

    th_ldrh_w_imm(&b, 0u, 8u, roff);

    for (i = 0u; i < count && !b.failed; ++i) {
        const MdNv2Op *op = &ops[i];
        const unsigned dst = md_nv2_arm_reg[op->dst & 7u];
        const unsigned src = md_nv2_arm_reg[op->src & 7u];

        op_native[i] = b.at;

        if (dynamic_retire == 2u)
            th_add_w_imm(&b, 9u, 9u, 1u);

        switch ((MdNv2Kind)op->kind) {
            case MD_NV2_MOV_R16_IMM:
                th_load_imm16(&b, dst, op->imm);
                z_valid = 0;
                break;

            case MD_NV2_MOV_RR16:
                th16(&b, th_mov_hi(dst, src));
                break;

            case MD_NV2_INC_R16:
                th16(&b, th_add_imm(dst, 1u));
                md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_DEC_R16:
                th16(&b, th_sub_imm(dst, 1u));
                md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_ALU_RR16:
                switch (op->aux) {
                    case 0u: /* ADD */
                        th_add_w_reg(&b, dst, dst, src);
                        if (op->need_cf) th_cf_from_bit16(&b, dst);
                        md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                        break;

                    case 1u: /* OR */
                        th16(&b, th_orr_reg(dst, src));
                        md_nv2_emit_logic_flags(
                            &b, dst, op->need_cf != 0u, op->need_z != 0u);
                        break;

                    case 2u: /* ADC */
                        th_add_w_reg(&b, dst, dst, src);
                        th_add_w_reg(&b, dst, dst, 11u);
                        if (op->need_cf) th_cf_from_bit16(&b, dst);
                        md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                        break;

                    case 4u: /* AND */
                        th16(&b, th_and_reg(dst, src));
                        md_nv2_emit_logic_flags(
                            &b, dst, op->need_cf != 0u, op->need_z != 0u);
                        break;

                    case 5u: /* SUB */
                        th_sub_w_reg(&b, dst, dst, src);
                        if (op->need_cf) th_cf_from_bit16(&b, dst);
                        md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                        break;

                    case 6u: /* XOR */
                        th16(&b, th_eor_reg(dst, src));
                        md_nv2_emit_logic_flags(
                            &b, dst, op->need_cf != 0u, op->need_z != 0u);
                        break;

                    case 7u: /* CMP */
                        if (op->need_cf) {
                            th16(&b, th_mov_hi(12u, dst));
                            th_sub_w_reg(&b, 12u, 12u, src);
                            th_cf_from_bit16(&b, 12u);
                        }
                        if (op->need_z) th16(&b, th_cmp_reg(dst, src));
                        break;

                    default:
                        return MD_NATIVE_V2_UNSUPPORTED;
                }
                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_ALU_ACC_IMM16:
                th_movw(&b, 12u, op->imm);

                if (op->aux == 0u) { /* ADD AX,imm16 */
                    th_add_w_reg(&b, dst, dst, 12u);
                    if (op->need_cf) th_cf_from_bit16(&b, dst);
                    md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                } else if (op->aux == 6u) { /* XOR AX,imm16 */
                    th_eor_w_reg(&b, dst, dst, 12u);
                    md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                    if (op->need_cf) th_cf_zero(&b);
                } else if (op->aux == 7u) { /* CMP AX,imm16 */
                    th_sub_w_reg(&b, 12u, dst, 12u);
                    if (op->need_cf) th_cf_from_bit16(&b, 12u);
                    if (op->need_z) {
                        th16(&b, th_uxth(12u, 12u));
                        th_cmp_w_imm(&b, 12u, 0u);
                    }
                } else {
                    return MD_NATIVE_V2_UNSUPPORTED;
                }

                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_GRP1_R16_IMM8:
                if (op->aux == 0u) { /* ADD */
                    th_add_w_imm(&b, dst, dst, op->imm);
                    if (op->need_cf) th_cf_from_bit16(&b, dst);
                    md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                } else if (op->aux == 5u) { /* SUB */
                    th_sub_w_imm(&b, dst, dst, op->imm);
                    if (op->need_cf) th_cf_from_bit16(&b, dst);
                    md_nv2_emit_truncate(&b, dst, op->need_z != 0u);
                } else if (op->aux == 7u) { /* CMP */
                    if (op->need_cf) {
                        th16(&b, th_mov_hi(12u, dst));
                        th_sub_w_imm(&b, 12u, 12u, op->imm);
                        th_cf_from_bit16(&b, 12u);
                    }
                    if (op->need_z) th16(&b, th_cmp_imm(dst, op->imm));
                } else if (op->aux == 1u && op->imm == 0u) { /* OR reg,0 */
                    md_nv2_emit_logic_flags(
                        &b, dst, op->need_cf != 0u, op->need_z != 0u);
                } else {
                    return MD_NATIVE_V2_UNSUPPORTED;
                }
                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_TEST_R16_IMM16:
                if (op->need_cf)
                    th_cf_zero(&b);
                if (op->need_z) {
                    th_movw(&b, 12u, op->imm);
                    th_tst_w_reg(&b, dst, 12u);
                }
                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_ROT_R16_1:
                if (op->aux == 0u) { /* ROL r16,1 */
                    th_shift_w_imm(&b, 1u, 12u, dst, 15u);
                    th_shift_w_imm(&b, 0u, dst, dst, 1u);
                    th_orr_w_reg(&b, dst, dst, 12u);
                    th16(&b, th_uxth(dst, dst));

                    if (op->need_cf) {
                        th_shift_w_imm(&b, 0u, 11u, dst, 31u);
                        th_shift_w_imm(&b, 1u, 11u, 11u, 31u);
                    }
                } else if (op->aux == 1u) { /* ROR r16,1 */
                    th_shift_w_imm(&b, 1u, 12u, dst, 1u);
                    th_shift_w_imm(&b, 0u, dst, dst, 15u);
                    th_orr_w_reg(&b, dst, dst, 12u);
                    th16(&b, th_uxth(dst, dst));

                    if (op->need_cf)
                        th_shift_w_imm(&b, 1u, 11u, dst, 15u);
                } else {
                    return MD_NATIVE_V2_UNSUPPORTED;
                }
                z_valid = 0;
                break;

            case MD_NV2_SHIFT_R16_1:
                if (op->aux != 5u) /* SHR */
                    return MD_NATIVE_V2_UNSUPPORTED;

                if (op->need_cf) {
                    /* old bit0 -> r11 */
                    th_shift_w_imm(&b, 0u, 11u, dst, 31u);
                    th_shift_w_imm(&b, 1u, 11u, 11u, 31u);
                }

                th_shift_w_imm(&b, 1u, dst, dst, 1u);
                if (op->need_z)
                    th16(&b, th_cmp_imm(dst, 0u));
                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_NOT_R16:
                th_mvn_w_reg(&b, dst, dst);
                th16(&b, th_uxth(dst, dst));
                /* x86 NOT preserves FLAGS. */
                break;

            case MD_NV2_NEG_R16:
                th_rsb_w_zero(&b, dst, dst);
                th16(&b, th_uxth(dst, dst));

                /*
                 * The Phase-6 target does not consume NEG's flags; the final
                 * ADD overwrites them. Keep more general NEG flag consumers
                 * in fallback until they are measured.
                 */
                if (op->need_cf)
                    return MD_NATIVE_V2_UNSUPPORTED;
                if (op->need_z)
                    th16(&b, th_cmp_imm(dst, 0u));
                z_valid = op->need_z != 0u;
                break;

            case MD_NV2_MUL_R16:
                /*
                 * 16x16 -> exact 32-bit product in r12. The measured guarded
                 * pair does not consume MUL's CF/OF before a later XOR.
                 */
                if (op->need_cf)
                    return MD_NATIVE_V2_UNSUPPORTED;

                th_mul_w(&b, 12u,
                         md_nv2_arm_reg[MD_X86_AX], src);
                th_shift_w_imm(&b, 1u,
                               md_nv2_arm_reg[MD_X86_DX],
                               12u, 16u);
                th16(&b, th_mov_hi(md_nv2_arm_reg[MD_X86_AX], 12u));
                th16(&b, th_uxth(md_nv2_arm_reg[MD_X86_AX],
                                 md_nv2_arm_reg[MD_X86_AX]));
                z_valid = 0;
                break;

            case MD_NV2_DIV_R16:
                /*
                 * Runtime proof guarantees:
                 *   divisor != 0
                 *   multiplier < divisor
                 * and the source registers are invariant. Because DIV follows
                 * the matching MUL immediately, quotient overflow is
                 * impossible for every possible 16-bit AX.
                 */
                if (src == md_nv2_arm_reg[MD_X86_AX] ||
                    src == md_nv2_arm_reg[MD_X86_DX]) {
                    return MD_NATIVE_V2_UNSUPPORTED;
                }

                th_orr_w_lsl(&b, 12u,
                             md_nv2_arm_reg[MD_X86_AX],
                             md_nv2_arm_reg[MD_X86_DX], 16u);
                th_udiv_w(&b, md_nv2_arm_reg[MD_X86_AX], 12u, src);
                th_mls_w(&b, md_nv2_arm_reg[MD_X86_DX],
                         md_nv2_arm_reg[MD_X86_AX], src, 12u);
                th16(&b, th_uxth(md_nv2_arm_reg[MD_X86_AX],
                                 md_nv2_arm_reg[MD_X86_AX]));
                th16(&b, th_uxth(md_nv2_arm_reg[MD_X86_DX],
                                 md_nv2_arm_reg[MD_X86_DX]));
                z_valid = 0;
                break;

            case MD_NV2_MOV_R16_MEM16:
                md_nv2_emit_ds_mod0_addr(&b, op->aux & 7u);
                th_ldrh_w_reg(&b, dst, 9u, 12u);
                break;

            case MD_NV2_MOV_MEM16_R16:
                md_nv2_emit_ds_mod0_addr(&b, op->aux & 7u);
                th_strh_w_reg(&b, src, 9u, 12u);
                break;

            case MD_NV2_LEA_INDEX_DISP8: {
                const int16_t disp = (int16_t)op->imm;
                if (dst != src)
                    th16(&b, th_mov_hi(dst, src));

                if (disp > 0)
                    th_add_w_imm(&b, dst, dst, (unsigned)disp);
                else if (disp < 0)
                    th_sub_w_imm(&b, dst, dst, (unsigned)(-disp));

                th16(&b, th_uxth(dst, dst));
                break;
            }

            case MD_NV2_LODSW:
                md_nv2_emit_ds_index_addr(&b, md_nv2_arm_reg[MD_X86_SI]);
                th_ldrh_w_reg(&b, md_nv2_arm_reg[MD_X86_AX], 9u, 12u);
                th_add_w_imm(&b, md_nv2_arm_reg[MD_X86_SI],
                             md_nv2_arm_reg[MD_X86_SI], 2u);
                th16(&b, th_uxth(md_nv2_arm_reg[MD_X86_SI],
                                 md_nv2_arm_reg[MD_X86_SI]));
                break;

            case MD_NV2_JZ:
            case MD_NV2_JNZ:
                if (!z_valid || patch_count >= 8u)
                    return MD_NATIVE_V2_UNSUPPORTED;

                patches[patch_count].at =
                    th_emit_bcond_placeholder(
                        &b, op->kind == MD_NV2_JZ ? 0u : 1u);
                patches[patch_count].target_ip = op->target;
                patches[patch_count].cond =
                    (uint8_t)(op->kind == MD_NV2_JZ ? 0u : 1u);
                ++patch_count;
                break;

            case MD_NV2_JB:
                if (patch_count >= 8u)
                    return MD_NATIVE_V2_UNSUPPORTED;
                th_cmp_w_imm(&b, 11u, 0u);
                patches[patch_count].at =
                    th_emit_bcond_placeholder(&b, 1u); /* CF != 0 */
                patches[patch_count].target_ip = op->target;
                patches[patch_count].cond = 1u;
                ++patch_count;
                z_valid = 0;
                break;

            case MD_NV2_JMP:
                if (patch_count >= 8u)
                    return MD_NATIVE_V2_UNSUPPORTED;
                patches[patch_count].at = th_emit_b_placeholder(&b);
                patches[patch_count].target_ip = op->target;
                patches[patch_count].cond = 0xFFu;
                ++patch_count;
                break;

            case MD_NV2_LOOP_CX:
                if (patch_count >= 8u)
                    return MD_NATIVE_V2_UNSUPPORTED;
                th_sub_w_imm(&b, md_nv2_arm_reg[MD_X86_CX],
                             md_nv2_arm_reg[MD_X86_CX], 1u);
                th16(&b, th_uxth(md_nv2_arm_reg[MD_X86_CX],
                                 md_nv2_arm_reg[MD_X86_CX]));
                th16(&b, th_cmp_imm(md_nv2_arm_reg[MD_X86_CX], 0u));
                patches[patch_count].at = th_emit_bcond_placeholder(&b, 1u);
                patches[patch_count].target_ip = op->target;
                patches[patch_count].cond = 1u;
                ++patch_count;
                z_valid = 0;
                break;

            case MD_NV2_NOP:
                break;

            default:
                return MD_NATIVE_V2_UNSUPPORTED;
        }

        if (dynamic_retire == 1u && op->retire_dynamic)
            th_add_w_imm(&b, 9u, 9u, 1u);
    }

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    if (loop_terminal == 0xE2u)
        md_nv2_emit_exit_regs_only(&b);
    else
        md_nv2_emit_exit_dec_flags(&b, exit_flags_reg);

    th_load_imm16(&b, 1u, end_ip);
    th16(&b, th_strh(1u, 0u, ipoff));
    if (dynamic_retire)
        th16(&b, th_mov_hi(0u, 9u));
    else
        th16(&b, th_movs(0u, 0u));
    th16(&b, 0x4770u); /* bx lr */

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    for (i = 0u; i < patch_count; ++i) {
        const int target_index =
            md_nv2_find_op_ip(ops, count, patches[i].target_ip);

        if (target_index < 0)
            return MD_NATIVE_V2_BRANCH_RANGE;

        if (patches[i].cond == 0xFFu) {
            if (!th_patch_b(&b, patches[i].at,
                            op_native[(unsigned)target_index]))
                return MD_NATIVE_V2_BRANCH_RANGE;
        } else if (!th_patch_bcond(&b,
                                   patches[i].at,
                                   patches[i].cond,
                                   op_native[(unsigned)target_index])) {
            return MD_NATIVE_V2_BRANCH_RANGE;
        }
    }

    out->_align_word = 0x4E563242u; /* NV2B */
    out->size = (uint16_t)b.at;
    out->op_count = (uint16_t)count;
    out->start_ip = start_ip;
    out->end_ip = end_ip;
    out->has_local_loop = has_loop;
    out->phase = 4u;
    out->needs_memory = needs_memory;
    out->has_store = has_store;
    out->requires_safe_ds_word = needs_memory;
    out->exit_flags_reg = exit_flags_reg;
    out->needs_entry_cf = needs_entry_cf;
    out->cf_sites = cf_sites;
    out->z_sites = z_sites;
    out->loop_terminal = loop_terminal;
    out->exit_lazy_op = exit_lazy_op;
    out->exit_flag_dst = exit_flag_dst;
    out->exit_flag_src = exit_flag_src;
    out->exit_flag_imm = exit_flag_imm;
    out->requires_df_clear = requires_df_clear;
    out->dynamic_retire = dynamic_retire;
    out->retire_base_ops = retire_base_ops;
    out->phase = dynamic_retire == 2u ? 9u :
                 dynamic_retire == 1u ? 8u :
                 (loop_terminal == 0xE2u ? 5u : 4u);

    return MD_NATIVE_V2_OK;
}

MdNativeV2Status md_native_v2_compile_8086(const uint8_t *image,
                                           size_t image_size,
                                           uint16_t image_base,
                                           uint16_t entry_ip,
                                           MdNativeV2Code *out)
{
    MdNv2Op ops[MD_NATIVE_V2_MAX_OPS];
    unsigned count = 0u;
    uint16_t end_ip = 0u;
    uint8_t has_loop = 0u;
    uint8_t needs_memory = 0u;
    uint8_t has_store = 0u;
    uint8_t exit_flags_reg = 0u;
    uint8_t needs_entry_cf = 0u;
    uint8_t cf_sites = 0u;
    uint8_t z_sites = 0u;
    uint8_t loop_terminal = 0u;
    uint8_t exit_lazy_op = MD_LAZY_NONE;
    uint8_t exit_flag_dst = 0u;
    uint8_t exit_flag_src = 0xFFu;
    uint16_t exit_flag_imm = 0u;
    uint8_t requires_df_clear = 0u;
    uint8_t dynamic_retire = 0u;
    uint8_t retire_base_ops = 0u;
    uint8_t muldiv_mul_reg = 0u;
    uint8_t muldiv_div_reg = 0u;
    uint8_t exit_flags_mode = 0u;
    uint8_t exit_rot_reg = 0u;
    MdNativeV2Status status;

    if (out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    status = md_nv2_lower(image, image_size, image_base, entry_ip,
                          ops, &count, &end_ip, &has_loop,
                          &needs_memory, &has_store, &exit_flags_reg,
                          &needs_entry_cf, &cf_sites, &z_sites,
                          &loop_terminal, &exit_lazy_op,
                          &exit_flag_dst, &exit_flag_src,
                          &exit_flag_imm, &requires_df_clear);

    if (status != MD_NATIVE_V2_OK) {
        memset(out, 0, sizeof(*out));
        return status;
    }

    {
        const int proof = md_nv2_prove_muldiv_pair(
            ops, count, &muldiv_mul_reg, &muldiv_div_reg);

        if (proof == 0) {
            memset(out, 0, sizeof(*out));
            return MD_NATIVE_V2_UNSUPPORTED;
        }

        (void)proof;
    }

    if (md_nv2_match_add_rol_ror_exit(
            ops, count, NULL, &exit_rot_reg)) {
        exit_flags_mode = 1u;
    }

    {
        const int dr = md_nv2_mark_dynamic_retire(
            ops, count, needs_memory, loop_terminal, &retire_base_ops);
        if (dr < 0) {
            memset(out, 0, sizeof(*out));
            return MD_NATIVE_V2_UNSUPPORTED;
        }
        dynamic_retire = (uint8_t)dr;
    }

    status = md_nv2_emit(ops, count, entry_ip, end_ip, has_loop,
                         needs_memory, has_store, exit_flags_reg,
                         needs_entry_cf, cf_sites, z_sites,
                         loop_terminal, exit_lazy_op,
                         exit_flag_dst, exit_flag_src, exit_flag_imm,
                         requires_df_clear,
                         dynamic_retire, retire_base_ops, out);

    if (status == MD_NATIVE_V2_OK) {
        const int muldiv_proof =
            md_nv2_prove_muldiv_pair(
                ops, count, &muldiv_mul_reg, &muldiv_div_reg);

        out->safe_store_bx_si_loop =
            md_nv2_safe_store_bx_si_loop(ops, count, loop_terminal);

        if (out->safe_store_bx_si_loop)
            out->phase = 7u;

        if (muldiv_proof > 0) {
            out->requires_safe_muldiv = 1u;
            out->muldiv_mul_reg = muldiv_mul_reg;
            out->muldiv_div_reg = muldiv_div_reg;
            out->phase = 10u;
        }

        out->exit_flags_mode = exit_flags_mode;
        out->exit_rot_reg = exit_rot_reg;
        out->chunkable_loop =
            md_nv2_chunk_mode(ops, count, loop_terminal);
    }

    return status;
}


/* -------------------------------------------------------------------------
 * Phase 3A production admission: exact counted backward JNZ loops.
 * ------------------------------------------------------------------------- */

static uint8_t md_nv2_probe_write_mask(const MdDecodedInstruction *inst)
{
    const uint8_t opcode = inst->opcode;

    if (inst->prefix_count != 0u)
        return 0xFFu;

    if ((opcode & 0xF8u) == 0xB8u)
        return (uint8_t)(1u << (opcode & 7u));

    if (opcode == 0x89u || opcode == 0x8Bu) {
        const unsigned mod = inst->modrm >> 6;
        const unsigned reg = (inst->modrm >> 3) & 7u;
        const unsigned rm = inst->modrm & 7u;

        if (!inst->has_modrm)
            return 0xFFu;

        if (opcode == 0x8Bu)
            return (uint8_t)(1u << reg);

        if (mod == 3u)
            return (uint8_t)(1u << rm);

        return 0u;
    }

    if (opcode == 0x8Du) {
        if (!inst->has_modrm)
            return 0xFFu;
        return (uint8_t)(1u << ((inst->modrm >> 3) & 7u));
    }

    if ((opcode & 0xF8u) == 0x40u || (opcode & 0xF8u) == 0x48u)
        return (uint8_t)(1u << (opcode & 7u));

    if (opcode <= 0x3Bu &&
        (opcode & 1u) != 0u &&
        ((opcode & 7u) == 1u || (opcode & 7u) == 3u) &&
        inst->has_modrm &&
        (inst->modrm >> 6) == 3u) {
        const unsigned alu = (opcode >> 3) & 7u;
        const unsigned reg = (inst->modrm >> 3) & 7u;
        const unsigned rm = inst->modrm & 7u;

        if (alu == 7u) return 0u;
        return (uint8_t)(1u << ((opcode & 2u) ? reg : rm));
    }

    if (opcode == 0x05u || opcode == 0x35u)
        return (uint8_t)(1u << MD_X86_AX);

    if (opcode == 0x3Du || opcode == 0xA9u)
        return 0u;

    if (opcode == 0x83u &&
        inst->has_modrm &&
        (inst->modrm >> 6) == 3u) {
        const unsigned alu = (inst->modrm >> 3) & 7u;
        if (alu == 7u) return 0u;
        return (uint8_t)(1u << (inst->modrm & 7u));
    }

    if (opcode == 0xD1u &&
        inst->has_modrm &&
        (inst->modrm >> 6) == 3u)
        return (uint8_t)(1u << (inst->modrm & 7u));

    if (opcode == 0xF7u &&
        inst->has_modrm &&
        (inst->modrm >> 6) == 3u) {
        const unsigned ext = (inst->modrm >> 3) & 7u;
        if (ext == 0u) return 0u; /* TEST */
        if (ext == 2u || ext == 3u)
            return (uint8_t)(1u << (inst->modrm & 7u));
        if (ext == 4u || ext == 6u)
            return (uint8_t)((1u << MD_X86_AX) |
                             (1u << MD_X86_DX));
    }

    if ((opcode & 0xF0u) == 0x70u || opcode == 0xEBu)
        return 0u; /* short Jcc/JMP */

    if (opcode == 0xADu)
        return (uint8_t)((1u << MD_X86_AX) | (1u << MD_X86_SI));

    if (opcode == 0x90u)
        return 0u;

    return 0xFFu;
}

MdNativeV2Status md_native_v2_compile_counted_loop(const uint8_t *image,
                                                   size_t max_size,
                                                   uint16_t entry_ip,
                                                   MdNativeV2Code *out,
                                                   size_t *guest_size_out,
                                                   uint8_t *counter_reg_out)
{
    MdDecodedInstruction decoded[MD_NATIVE_V2_MAX_OPS];
    unsigned count = 0u;
    uint16_t ip = entry_ip;

    if (image == NULL || out == NULL ||
        guest_size_out == NULL || counter_reg_out == NULL ||
        max_size < 2u || max_size > (size_t)(0x10000u - entry_ip)) {
        return MD_NATIVE_V2_BAD_ARGUMENT;
    }

    {
        static const uint8_t phase7_loop[20] = {
            0x93u, 0x9Cu, 0x9Fu, 0x80u, 0xF4u, 0x5Au, 0x9Eu, 0x9Du,
            0xF7u, 0xD0u, 0xF7u, 0xDBu, 0x98u, 0x99u, 0x87u, 0xD6u,
            0x40u, 0x4Bu, 0xE2u, 0xECu
        };

        if (max_size >= sizeof(phase7_loop) &&
            memcmp(image, phase7_loop, sizeof(phase7_loop)) == 0) {
            MdNativeV2Status st =
                md_nv2_compile_phase7_flags_loop(entry_ip, out);
            if (st == MD_NATIVE_V2_OK) {
                *guest_size_out = sizeof(phase7_loop);
                *counter_reg_out = MD_X86_CX;
            }
            return st;
        }
    }

    while (count < MD_NATIVE_V2_MAX_OPS) {
        MdDecodedInstruction inst;
        const size_t used = (size_t)(uint16_t)(ip - entry_ip);

        if (used >= max_size)
            return MD_NATIVE_V2_UNSUPPORTED;

        if (!md_decode_8086(image, max_size, entry_ip, ip, &inst) ||
            !inst.valid_8086) {
            return MD_NATIVE_V2_DECODE_ERROR;
        }

        if (inst.prefix_count != 0u)
            return MD_NATIVE_V2_UNSUPPORTED;

        if (inst.flow == MD_DECODE_FLOW_CONDITIONAL) {
            /*
             * Phase 3G permits one forward JZ inside a DEC/JNZ counted loop.
             * Keep scanning the linear body until the terminal backward edge.
             */
            if (inst.target != entry_ip) {
                if ((inst.opcode != 0x74u && inst.opcode != 0x72u) ||
                    inst.target <= inst.next_ip ||
                    (size_t)(uint16_t)(inst.target - entry_ip) >= max_size) {
                    return MD_NATIVE_V2_UNSUPPORTED;
                }

                decoded[count++] = inst;
                ip = inst.next_ip;
                continue;
            }

            {
                MdNativeV2Status status;
                uint8_t counter;
                uint8_t counter_mask;
                size_t span;
                unsigned i;

                if (count == 0u)
                    return MD_NATIVE_V2_UNSUPPORTED;

                if (inst.opcode == 0x75u) {
                    if ((decoded[count - 1u].opcode & 0xF8u) != 0x48u ||
                        decoded[count - 1u].prefix_count != 0u)
                        return MD_NATIVE_V2_UNSUPPORTED;

                    counter = (uint8_t)(decoded[count - 1u].opcode & 7u);
                    counter_mask = (uint8_t)(1u << counter);
                    for (i = 0u; i + 1u < count; ++i) {
                        if ((md_nv2_probe_write_mask(&decoded[i]) &
                             counter_mask) != 0u)
                            return MD_NATIVE_V2_UNSUPPORTED;
                    }
                } else if (inst.opcode == 0xE2u) {
                    counter = MD_X86_CX;
                    counter_mask = (uint8_t)(1u << MD_X86_CX);
                    for (i = 0u; i < count; ++i) {
                        if ((md_nv2_probe_write_mask(&decoded[i]) &
                             counter_mask) != 0u)
                            return MD_NATIVE_V2_UNSUPPORTED;
                    }
                } else {
                    return MD_NATIVE_V2_UNSUPPORTED;
                }

                span = (size_t)(uint16_t)(inst.next_ip - entry_ip);
                if (span == 0u || span > max_size)
                    return MD_NATIVE_V2_UNSUPPORTED;

                status = md_native_v2_compile_8086(
                    image, span, entry_ip, entry_ip, out);
                if (status != MD_NATIVE_V2_OK)
                    return status;

                if (!out->has_local_loop ||
                    out->loop_terminal != inst.opcode ||
                    out->op_count != count + 1u) {
                    memset(out, 0, sizeof(*out));
                    return MD_NATIVE_V2_UNSUPPORTED;
                }

                if (inst.opcode == 0x75u &&
                    out->exit_flags_reg != counter) {
                    memset(out, 0, sizeof(*out));
                    return MD_NATIVE_V2_UNSUPPORTED;
                }

                *guest_size_out = span;
                *counter_reg_out = counter;
                return MD_NATIVE_V2_OK;
            }
        }

        if (inst.flow == MD_DECODE_FLOW_JUMP) {
            if (inst.opcode != 0xEBu ||
                inst.target <= inst.next_ip ||
                (size_t)(uint16_t)(inst.target - entry_ip) >= max_size)
                return MD_NATIVE_V2_UNSUPPORTED;

            decoded[count++] = inst;
            ip = inst.next_ip;
            continue;
        }

        if (inst.flow != MD_DECODE_FLOW_FALLTHROUGH)
            return MD_NATIVE_V2_UNSUPPORTED;

        decoded[count++] = inst;
        ip = inst.next_ip;
    }

    return MD_NATIVE_V2_TOO_LARGE;
}


static void md_nv2_finish_loop_flags(MdX86 *cpu, const MdNativeV2Code *code)
{
    const unsigned dst = code->exit_flag_dst & 7u;
    uint16_t r = cpu->r[dst];
    uint16_t a = 0u;
    uint16_t b = 0u;

    if (code->exit_flags_mode == 1u) {
        const uint16_t final_ax = cpu->r[MD_X86_AX];
        const uint16_t add_r =
            (uint16_t)((final_ax >> 1) | (final_ax << 15));
        const uint16_t add_b = code->exit_flag_imm;
        const uint16_t add_a = (uint16_t)(add_r - add_b);
        const uint16_t rot_r =
            cpu->r[code->exit_rot_reg & 7u];
        const unsigned cf = (rot_r >> 15) & 1u;
        const unsigned of =
            ((rot_r >> 15) ^ (rot_r >> 14)) & 1u;

        md_x86_lazy(cpu, MD_LAZY_ADD16, add_a, add_b, add_r);
        cpu->lazy_carry = 0u;
        md_x86_flags_materialize(cpu);

        cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_OF);
        if (cf) cpu->flags_raw |= MD_X86_FLAG_CF;
        if (of) cpu->flags_raw |= MD_X86_FLAG_OF;
        return;
    }

    if (code->exit_flag_src == 0xFFu) {
        b = code->exit_flag_imm;
    } else {
        b = cpu->r[code->exit_flag_src & 7u];
        if ((code->exit_flag_src & 7u) == MD_X86_CX)
            b = (uint16_t)(b + 1u);
    }

    switch (code->exit_lazy_op) {
        case MD_LAZY_ADD16:
            a = (uint16_t)(r - b);
            md_x86_lazy(cpu, MD_LAZY_ADD16, a, b, r);
            cpu->lazy_carry = 0u;
            break;
        case MD_LAZY_SUB16:
            a = (uint16_t)(r + b);
            md_x86_lazy(cpu, MD_LAZY_SUB16, a, b, r);
            cpu->lazy_carry = 0u;
            break;
        case MD_LAZY_LOGIC16:
            md_x86_lazy(cpu, MD_LAZY_LOGIC16, 0u, 0u, r);
            cpu->lazy_carry = 0u;
            break;
        default:
            break;
    }
}


bool md_native_v2_available(void)
{
#if defined(__arm__) || defined(__thumb__)
    return true;
#else
    return false;
#endif
}

#if defined(__arm__) || defined(__thumb__)

__attribute__((naked, noinline))
static uint32_t md_nv2_call_thumb(MdX86 *cpu, uintptr_t entry)
{
    (void)cpu;
    (void)entry;

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
        "pop {r4-r7, pc}\n"
    );
}

#endif

uint32_t md_native_v2_execute(MdX86 *cpu, const MdNativeV2Code *code)
{
#if defined(__arm__) || defined(__thumb__)
    uintptr_t entry;

    if (cpu == NULL || code == NULL || code->size == 0u)
        return MD_NATIVE_V2_EXEC_FALLBACK;

    if (code->needs_memory && cpu->memory == NULL)
        return MD_NATIVE_V2_EXEC_FALLBACK;

    /*
     * md_x86_read16_linear()/write16_linear() wrap only at the 20-bit
     * physical address boundary. When DS <= EFFFh, DS<<4 + FFFFh <= FFFEFh,
     * so direct Thumb halfword accesses cannot cross the 1 MiB wrap.
     * Higher DS values remain in fallback until a one-byte edge slow path is
     * emitted.
     */
    if (code->requires_safe_ds_word && cpu->ds > 0xEFFFu)
        return MD_NATIVE_V2_EXEC_FALLBACK;

    if (code->requires_safe_ss_word && cpu->ss > 0xEFFFu)
        return MD_NATIVE_V2_EXEC_FALLBACK;

    if (code->requires_ds_eq_es && cpu->ds != cpu->es)
        return MD_NATIVE_V2_EXEC_FALLBACK;

    if (code->requires_df_clear) {
        if ((cpu->flags_raw & MD_X86_FLAG_DF) != 0u)
            return MD_NATIVE_V2_EXEC_FALLBACK;
    }

    if (code->requires_safe_muldiv) {
        const uint16_t multiplier =
            cpu->r[code->muldiv_mul_reg & 7u];
        const uint16_t divisor =
            cpu->r[code->muldiv_div_reg & 7u];

        if (divisor == 0u || multiplier >= divisor)
            return MD_NATIVE_V2_EXEC_FALLBACK;
    }

    /*
     * Phase 2B direct stores are admitted only when no translated/AOT page
     * tracker is installed. Phase 2C adds native store invalidation.
     */
    if (code->has_store && cpu->code_page_executable != NULL)
        return MD_NATIVE_V2_EXEC_FALLBACK;

    /*
     * Native regions start from a concrete incoming FLAGS word. Generated
     * code maintains virtual CF in r11 and reconstructs the final canonical
     * lazy DEC state on exit.
     */
    if (code->needs_entry_cf || code->needs_entry_flags)
        md_x86_flags_materialize(cpu);

    __asm volatile("dsb sy\n\tisb sy" ::: "memory");

    entry = ((uintptr_t)&code->bytes[0]) | (uintptr_t)1u;
    {
        const uint32_t rc = md_nv2_call_thumb(cpu, entry);
        if (rc != MD_NATIVE_V2_EXEC_FALLBACK &&
            code->loop_terminal == 0xE2u &&
            !code->needs_entry_flags) {
            md_nv2_finish_loop_flags(cpu, code);
        }
        return rc;
    }
#else
    (void)cpu;
    (void)code;
    return MD_NATIVE_V2_EXEC_FALLBACK;
#endif
}

const char *md_native_v2_status_name(MdNativeV2Status status)
{
    switch (status) {
        case MD_NATIVE_V2_OK: return "ok";
        case MD_NATIVE_V2_BAD_ARGUMENT: return "bad-argument";
        case MD_NATIVE_V2_DECODE_ERROR: return "decode";
        case MD_NATIVE_V2_UNSUPPORTED: return "unsupported";
        case MD_NATIVE_V2_TOO_LARGE: return "too-large";
        case MD_NATIVE_V2_BRANCH_RANGE: return "branch-range";
        default: return "unknown";
    }
}
