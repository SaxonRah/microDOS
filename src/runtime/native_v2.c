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
    MD_NV2_MOV_R16_MEM16,
    MD_NV2_MOV_MEM16_R16,
    MD_NV2_LEA_INDEX_DISP8,
    MD_NV2_JZ,
    MD_NV2_JNZ,
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
    return alu == 0u || alu == 1u || alu == 4u ||
           alu == 5u || alu == 6u || alu == 7u;
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
           op->kind == MD_NV2_GRP1_R16_IMM8;
}

static int md_nv2_op_defines_z(const MdNv2Op *op)
{
    return op->kind == MD_NV2_INC_R16 ||
           op->kind == MD_NV2_DEC_R16 ||
           op->kind == MD_NV2_ALU_RR16 ||
           op->kind == MD_NV2_GRP1_R16_IMM8;
}

/*
 * Phase 2C: backward flag liveness.
 *
 * Phase 2B maintained virtual CF and native Z after every flag-producing
 * operation. That was correct but regmix showed the cost clearly:
 *   2A  2.800 cycles/guest
 *   2B  3.601 cycles/guest
 *
 * In the current native subset:
 *   - JZ/JNZ are the only in-region Z consumers.
 *   - no instruction consumes CF in-region yet.
 *   - the region exit needs the CF preserved through the final DEC so the
 *     canonical lazy DEC16 state can be reconstructed.
 *
 * Therefore most intermediate flag work is dead and should not be emitted.
 */
static uint8_t md_nv2_mark_flag_liveness(MdNv2Op *ops, unsigned count,
                                         uint8_t *cf_sites_out,
                                         uint8_t *z_sites_out)
{
    unsigned cf_live = 1u; /* final DEC preserves this into the exit state */
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
                                     uint8_t *z_sites_out)
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

    if (image == NULL || ops == NULL || count_out == NULL ||
        end_ip_out == NULL || has_loop_out == NULL ||
        needs_memory_out == NULL || has_store_out == NULL ||
        exit_flags_reg_out == NULL || needs_entry_cf_out == NULL ||
        cf_sites_out == NULL || z_sites_out == NULL) {
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
            } else if (mod == 0u && md_nv2_mem_index_from_rm(mrm) >= 0) {
                needs_memory = 1u;
                op->aux = (uint8_t)md_nv2_mem_index_from_rm(mrm);
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
        } else if ((opcode & 0xF8u) == 0x48u) {
            op->kind = MD_NV2_DEC_R16;
            op->dst = (uint8_t)(opcode & 7u);
            last_flag_kind = MD_NV2_DEC_R16;
            last_flag_reg = op->dst;
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
        } else if (opcode == 0x74u || opcode == 0x75u) {
            ++conditional_count;

            if (conditional_count > 1u ||
                inst.flow != MD_DECODE_FLOW_CONDITIONAL ||
                inst.target < entry_ip ||
                (uint32_t)inst.target >= image_end32) {
                return MD_NATIVE_V2_UNSUPPORTED;
            }

            op->kind = opcode == 0x74u ? MD_NV2_JZ : MD_NV2_JNZ;
            has_loop = 1u;
        } else if (opcode == 0x90u) {
            op->kind = MD_NV2_NOP;
        } else {
            return MD_NATIVE_V2_UNSUPPORTED;
        }

        ++count;
        ip = inst.next_ip;
    }

    /*
     * Phase 2B's interpreter-handoff proof is intentionally strict:
     * the last flag producer must be DEC r16, and not AX (r0 is used as the
     * architectural frame during exit). This covers the dominant DOS loop
     * shape while giving exact full lazy flags at the boundary.
     */
    if (last_flag_kind != MD_NV2_DEC_R16 || last_flag_reg == MD_X86_AX)
        return MD_NATIVE_V2_UNSUPPORTED;

    *needs_entry_cf_out =
        md_nv2_mark_flag_liveness(ops, count, cf_sites_out, z_sites_out);

    *count_out = count;
    *end_ip_out = ip;
    *has_loop_out = has_loop;
    *needs_memory_out = needs_memory;
    *has_store_out = has_store;
    *exit_flags_reg_out = last_flag_reg;
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
                                    MdNativeV2Code *out)
{
    MdThumbBuf b;
    MdBranchPatch patches[4];
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

    th_ldr_w_imm(&b, 9u, 8u, moff);
    th_ldrh_w_imm(&b, 10u, 8u, dsoff);
    th32(&b, 0xEA4Fu, 0x1A0Au); /* lsl.w r10,r10,#4 */

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

            case MD_NV2_MOV_R16_MEM16:
                md_nv2_emit_ds_index_addr(
                    &b, md_nv2_arm_reg[op->aux & 7u]);
                th_ldrh_w_reg(&b, dst, 9u, 12u);
                break;

            case MD_NV2_MOV_MEM16_R16:
                md_nv2_emit_ds_index_addr(
                    &b, md_nv2_arm_reg[op->aux & 7u]);
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

            case MD_NV2_JZ:
            case MD_NV2_JNZ:
                if (!z_valid || patch_count >= 4u)
                    return MD_NATIVE_V2_UNSUPPORTED;

                patches[patch_count].at =
                    th_emit_bcond_placeholder(
                        &b, op->kind == MD_NV2_JZ ? 0u : 1u);
                patches[patch_count].target_ip = op->target;
                patches[patch_count].cond =
                    (uint8_t)(op->kind == MD_NV2_JZ ? 0u : 1u);
                ++patch_count;
                break;

            case MD_NV2_NOP:
                break;

            default:
                return MD_NATIVE_V2_UNSUPPORTED;
        }
    }

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    md_nv2_emit_exit_dec_flags(&b, exit_flags_reg);

    th_load_imm16(&b, 1u, end_ip);
    th16(&b, th_strh(1u, 0u, ipoff));
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0x4770u); /* bx lr */

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    for (i = 0u; i < patch_count; ++i) {
        const int target_index =
            md_nv2_find_op_ip(ops, count, patches[i].target_ip);

        if (target_index < 0 ||
            !th_patch_bcond(&b,
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
    MdNativeV2Status status;

    if (out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    status = md_nv2_lower(image, image_size, image_base, entry_ip,
                          ops, &count, &end_ip, &has_loop,
                          &needs_memory, &has_store, &exit_flags_reg,
                          &needs_entry_cf, &cf_sites, &z_sites);

    if (status != MD_NATIVE_V2_OK) {
        memset(out, 0, sizeof(*out));
        return status;
    }

    return md_nv2_emit(ops, count, entry_ip, end_ip, has_loop,
                       needs_memory, has_store, exit_flags_reg,
                       needs_entry_cf, cf_sites, z_sites, out);
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
     * LDRH/STRH direct fast path is correct for all offsets when DS <= EFFFh:
     * DS<<4 + FFFFh <= FFFEFh, so a word cannot straddle the 1 MiB wrap.
     * Higher DS values stay in the interpreter until Phase 2C emits an edge
     * slow path.
     */
    if (code->requires_safe_ds_word && cpu->ds > 0xEFFFu)
        return MD_NATIVE_V2_EXEC_FALLBACK;

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
    if (code->needs_entry_cf)
        md_x86_flags_materialize(cpu);

    __asm volatile("dsb sy\n\tisb sy" ::: "memory");

    entry = ((uintptr_t)&code->bytes[0]) | (uintptr_t)1u;
    return md_nv2_call_thumb(cpu, entry);
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
