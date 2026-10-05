#ifndef MICRODOS_OPS_H
#define MICRODOS_OPS_H

#include <stdint.h>
#include "microdos/x86.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared by the interpreter and dosrecomp-generated code. */
static inline unsigned md_x86_even_parity8(uint8_t value)
{
    value ^= (uint8_t)(value >> 4);
    value &= 0x0Fu;
    return (0x9669u >> value) & 1u;
}

/* SZP on the raw word; only used by eager (materialised) paths. */
static inline void md_x86_set_szp8(MdX86 *cpu, uint8_t value)
{
    /* Branchless: SF (0x80) is bit 7 of the byte itself, ZF is bit 6,
       PF is bit 2. */
    unsigned f = cpu->flags_raw & (0xFFFFu ^ (MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF));
    f |= (unsigned)value & 0x80u;
    f |= (unsigned)(value == 0u) << 6;
    f |= md_x86_even_parity8(value) << 2;
    cpu->flags_raw = (uint16_t)f;
}

static inline void md_x86_set_szp16(MdX86 *cpu, uint16_t value)
{
    unsigned f = cpu->flags_raw & (0xFFFFu ^ (MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF));
    f |= ((unsigned)value >> 8) & 0x80u;
    f |= (unsigned)(value == 0u) << 6;
    f |= md_x86_even_parity8((uint8_t)value) << 2;
    cpu->flags_raw = (uint16_t)f;
}

/* M18: the hot ALU operations only record their inputs and result; flags
   are derived on demand (x86.h). Results are unchanged. */
static inline uint8_t md_x86_add8(MdX86 *cpu, uint8_t a, uint8_t b)
{
    const uint8_t result = (uint8_t)(a + b);
    md_x86_lazy(cpu, MD_LAZY_ADD8, a, b, result);
    return result;
}

static inline uint16_t md_x86_add16(MdX86 *cpu, uint16_t a, uint16_t b)
{
    const uint16_t result = (uint16_t)(a + b);
    md_x86_lazy(cpu, MD_LAZY_ADD16, a, b, result);
    return result;
}

static inline uint8_t md_x86_adc8(MdX86 *cpu, uint8_t a, uint8_t b)
{
    const unsigned carry = (unsigned)md_x86_cf(cpu);
    const uint8_t result = (uint8_t)(a + b + carry);
    cpu->lazy_carry = (uint8_t)carry;
    md_x86_lazy(cpu, MD_LAZY_ADC8, a, b, result);
    return result;
}

static inline uint16_t md_x86_adc16(MdX86 *cpu, uint16_t a, uint16_t b)
{
    const unsigned carry = (unsigned)md_x86_cf(cpu);
    const uint16_t result = (uint16_t)(a + b + carry);
    cpu->lazy_carry = (uint8_t)carry;
    md_x86_lazy(cpu, MD_LAZY_ADC16, a, b, result);
    return result;
}

static inline uint8_t md_x86_sub8(MdX86 *cpu, uint8_t a, uint8_t b)
{
    const uint8_t result = (uint8_t)(a - b);
    md_x86_lazy(cpu, MD_LAZY_SUB8, a, b, result);
    return result;
}

static inline uint16_t md_x86_sub16(MdX86 *cpu, uint16_t a, uint16_t b)
{
    const uint16_t result = (uint16_t)(a - b);
    md_x86_lazy(cpu, MD_LAZY_SUB16, a, b, result);
    return result;
}

static inline uint8_t md_x86_sbb8(MdX86 *cpu, uint8_t a, uint8_t b)
{
    const unsigned borrow = (unsigned)md_x86_cf(cpu);
    const uint8_t result = (uint8_t)(a - b - borrow);
    cpu->lazy_carry = (uint8_t)borrow;
    md_x86_lazy(cpu, MD_LAZY_SBB8, a, b, result);
    return result;
}

static inline uint16_t md_x86_sbb16(MdX86 *cpu, uint16_t a, uint16_t b)
{
    const unsigned borrow = (unsigned)md_x86_cf(cpu);
    const uint16_t result = (uint16_t)(a - b - borrow);
    cpu->lazy_carry = (uint8_t)borrow;
    md_x86_lazy(cpu, MD_LAZY_SBB16, a, b, result);
    return result;
}

static inline uint8_t md_x86_logic8(MdX86 *cpu, uint8_t value)
{
    md_x86_lazy(cpu, MD_LAZY_LOGIC8, 0u, 0u, value);
    return value;
}

static inline uint16_t md_x86_logic16(MdX86 *cpu, uint16_t value)
{
    md_x86_lazy(cpu, MD_LAZY_LOGIC16, 0u, 0u, value);
    return value;
}

/* INC/DEC: like ADD/SUB 1 but CF is preserved (captured once, here). */
static inline uint8_t md_x86_inc8(MdX86 *cpu, uint8_t a)
{
    const uint8_t result = (uint8_t)(a + 1u);
    cpu->lazy_carry = (uint8_t)md_x86_cf(cpu);
    md_x86_lazy(cpu, MD_LAZY_INC8, a, 1u, result);
    return result;
}

static inline uint16_t md_x86_inc16(MdX86 *cpu, uint16_t a)
{
    const uint16_t result = (uint16_t)(a + 1u);
    cpu->lazy_carry = (uint8_t)md_x86_cf(cpu);
    md_x86_lazy(cpu, MD_LAZY_INC16, a, 1u, result);
    return result;
}

static inline uint8_t md_x86_dec8(MdX86 *cpu, uint8_t a)
{
    const uint8_t result = (uint8_t)(a - 1u);
    cpu->lazy_carry = (uint8_t)md_x86_cf(cpu);
    md_x86_lazy(cpu, MD_LAZY_DEC8, a, 1u, result);
    return result;
}

static inline uint16_t md_x86_dec16(MdX86 *cpu, uint16_t a)
{
    const uint16_t result = (uint16_t)(a - 1u);
    cpu->lazy_carry = (uint8_t)md_x86_cf(cpu);
    md_x86_lazy(cpu, MD_LAZY_DEC16, a, 1u, result);
    return result;
}

/*
 * Shifts and rotates in closed form.
 *
 * The 8086 does not mask CL, so a count can be up to 255. Instead of one
 * loop iteration (and one switch dispatch) per bit, every operation is
 * computed in O(1) with a wider intermediate:
 *
 *   ROL/ROR  rotate by count mod width; CF is the bit that landed at the
 *            LSB (ROL) or MSB (ROR).
 *   RCL/RCR  rotate the (width+1)-bit value CF:operand by count mod
 *            (width+1); CF is the top bit of the result.
 *   SHL/SHR  shift a 32-bit copy by min(count, width+1); CF is the last
 *            bit shifted out (zero once count exceeds width).
 *   SAR      shift a sign-extended copy by min(count, width).
 *
 * Flag behaviour is identical to the previous per-bit loop, including the
 * count==1-only OF update and the SETMO/SETMOC (/6) quirk.
 */
static inline uint8_t md_x86_shift8(MdX86 *cpu, unsigned operation,
                                    uint8_t value, unsigned count)
{
    const unsigned op = operation & 7u;
    const unsigned v = value;
    unsigned result;
    unsigned cf;

    md_x86_flags_materialize(cpu);   /* shifts stay eager (M18) */

    count &= 0xFFu; /* 8086 uses the full CL count; it does not mask to 5 bits. */
    if (count == 0u) return value;

    switch (op) {
        case 0u: { /* ROL */
            const unsigned r = count & 7u;
            result = ((v << r) | (v >> ((8u - r) & 7u))) & 0xFFu;
            cf = result & 1u;
            break;
        }
        case 1u: { /* ROR */
            const unsigned r = count & 7u;
            result = ((v >> r) | (v << ((8u - r) & 7u))) & 0xFFu;
            cf = result >> 7;
            break;
        }
        case 2u: { /* RCL: 9-bit rotate of CF:value */
            const unsigned r = count % 9u;
            unsigned x = v | ((unsigned)(cpu->flags_raw & MD_X86_FLAG_CF) << 8);
            x = ((x << r) | (x >> (9u - r))) & 0x1FFu;
            result = x & 0xFFu;
            cf = x >> 8;
            break;
        }
        case 3u: { /* RCR */
            const unsigned r = count % 9u;
            unsigned x = v | ((unsigned)(cpu->flags_raw & MD_X86_FLAG_CF) << 8);
            x = ((x >> r) | (x << (9u - r))) & 0x1FFu;
            result = x & 0xFFu;
            cf = x >> 8;
            break;
        }
        case 4u: { /* SHL/SAL */
            const uint32_t w = (uint32_t)v << (count > 9u ? 9u : count);
            result = w & 0xFFu;
            cf = (w >> 8) & 1u;
            break;
        }
        case 5u: { /* SHR */
            const unsigned c = count > 9u ? 9u : count;
            result = v >> c;
            cf = (v >> (c - 1u)) & 1u;
            break;
        }
        case 6u: /*
                  * Original-8086 undocumented Group-2 /6 is SETMO/SETMOC, not
                  * SHL: D0 /6 -> FFh; D2 /6 -> unchanged if CL==0, else FFh.
                  * Deterministic flags: CF/AF/OF clear, PF/SF set, ZF clear.
                  */
            cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_CF |
                                          MD_X86_FLAG_AF |
                                          MD_X86_FLAG_OF);
            md_x86_set_szp8(cpu, 0xFFu);
            return 0xFFu;
        default: { /* SAR */
            const unsigned c = count > 8u ? 8u : count;
            const unsigned s = v | (0u - (v & 0x80u));   /* sign-extend */
            result = (s >> c) & 0xFFu;
            cf = (s >> (c - 1u)) & 1u;
            break;
        }
    }

    cpu->flags_raw = (uint16_t)((cpu->flags_raw & (uint16_t)~MD_X86_FLAG_CF) | cf);
    if (op >= 4u) md_x86_set_szp8(cpu, (uint8_t)result);

    if (count == 1u) {
        unsigned of;
        switch (op) {
            case 0u: case 2u: case 4u: of = (result >> 7) ^ cf; break;
            case 1u: case 3u:          of = ((result >> 7) ^ (result >> 6)) & 1u; break;
            case 5u:                   of = v >> 7; break;
            default:                   of = 0u; break;   /* SAR */
        }
        cpu->flags_raw = (uint16_t)((cpu->flags_raw & (uint16_t)~MD_X86_FLAG_OF) | (of << 11));
    }
    return (uint8_t)result;
}

static inline uint16_t md_x86_shift16(MdX86 *cpu, unsigned operation,
                                      uint16_t value, unsigned count)
{
    const unsigned op = operation & 7u;
    const uint32_t v = value;
    uint32_t result;
    unsigned cf;

    md_x86_flags_materialize(cpu);   /* shifts stay eager (M18) */

    count &= 0xFFu;
    if (count == 0u) return value;

    switch (op) {
        case 0u: { /* ROL */
            const unsigned r = count & 15u;
            result = ((v << r) | (v >> ((16u - r) & 15u))) & 0xFFFFu;
            cf = (unsigned)(result & 1u);
            break;
        }
        case 1u: { /* ROR */
            const unsigned r = count & 15u;
            result = ((v >> r) | (v << ((16u - r) & 15u))) & 0xFFFFu;
            cf = (unsigned)(result >> 15);
            break;
        }
        case 2u: { /* RCL: 17-bit rotate of CF:value */
            const unsigned r = count % 17u;
            uint32_t x = v | ((uint32_t)(cpu->flags_raw & MD_X86_FLAG_CF) << 16);
            x = ((x << r) | (x >> (17u - r))) & 0x1FFFFu;
            result = x & 0xFFFFu;
            cf = (unsigned)(x >> 16);
            break;
        }
        case 3u: { /* RCR */
            const unsigned r = count % 17u;
            uint32_t x = v | ((uint32_t)(cpu->flags_raw & MD_X86_FLAG_CF) << 16);
            x = ((x >> r) | (x << (17u - r))) & 0x1FFFFu;
            result = x & 0xFFFFu;
            cf = (unsigned)(x >> 16);
            break;
        }
        case 4u: { /* SHL/SAL */
            const uint32_t w = v << (count > 17u ? 17u : count);
            result = w & 0xFFFFu;
            cf = (unsigned)((w >> 16) & 1u);
            break;
        }
        case 5u: { /* SHR */
            const unsigned c = count > 17u ? 17u : count;
            result = v >> c;
            cf = (unsigned)((v >> (c - 1u)) & 1u);
            break;
        }
        case 6u: /* SETMO/SETMOC, see the byte helper above. */
            cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_CF |
                                          MD_X86_FLAG_AF |
                                          MD_X86_FLAG_OF);
            md_x86_set_szp16(cpu, 0xFFFFu);
            return 0xFFFFu;
        default: { /* SAR */
            const unsigned c = count > 16u ? 16u : count;
            const uint32_t s = v | (0u - (v & 0x8000u));   /* sign-extend */
            result = (s >> c) & 0xFFFFu;
            cf = (unsigned)((s >> (c - 1u)) & 1u);
            break;
        }
    }

    cpu->flags_raw = (uint16_t)((cpu->flags_raw & (uint16_t)~MD_X86_FLAG_CF) | cf);
    if (op >= 4u) md_x86_set_szp16(cpu, (uint16_t)result);

    if (count == 1u) {
        unsigned of;
        switch (op) {
            case 0u: case 2u: case 4u: of = (unsigned)(result >> 15) ^ cf; break;
            case 1u: case 3u:          of = (unsigned)((result >> 15) ^ (result >> 14)) & 1u; break;
            case 5u:                   of = (unsigned)(v >> 15); break;
            default:                   of = 0u; break;   /* SAR */
        }
        cpu->flags_raw = (uint16_t)((cpu->flags_raw & (uint16_t)~MD_X86_FLAG_OF) | (of << 11));
    }
    return (uint16_t)result;
}

/* ALU group dispatch shared by the interpreter and generated code:
   0 ADD, 1 OR, 2 ADC, 3 SBB, 4 AND, 5 SUB, 6 XOR, 7 CMP (result discarded). */
static inline uint8_t md_x86_alu8(MdX86 *cpu, unsigned operation, uint8_t lhs, uint8_t rhs)
{
    switch (operation & 7u) {
        case 0u: return md_x86_add8(cpu, lhs, rhs);
        case 1u: return md_x86_logic8(cpu, (uint8_t)(lhs | rhs));
        case 2u: return md_x86_adc8(cpu, lhs, rhs);
        case 3u: return md_x86_sbb8(cpu, lhs, rhs);
        case 4u: return md_x86_logic8(cpu, (uint8_t)(lhs & rhs));
        case 5u: return md_x86_sub8(cpu, lhs, rhs);
        case 6u: return md_x86_logic8(cpu, (uint8_t)(lhs ^ rhs));
        default: return md_x86_sub8(cpu, lhs, rhs);
    }
}

static inline uint16_t md_x86_alu16(MdX86 *cpu, unsigned operation, uint16_t lhs, uint16_t rhs)
{
    switch (operation & 7u) {
        case 0u: return md_x86_add16(cpu, lhs, rhs);
        case 1u: return md_x86_logic16(cpu, (uint16_t)(lhs | rhs));
        case 2u: return md_x86_adc16(cpu, lhs, rhs);
        case 3u: return md_x86_sbb16(cpu, lhs, rhs);
        case 4u: return md_x86_logic16(cpu, (uint16_t)(lhs & rhs));
        case 5u: return md_x86_sub16(cpu, lhs, rhs);
        case 6u: return md_x86_logic16(cpu, (uint16_t)(lhs ^ rhs));
        default: return md_x86_sub16(cpu, lhs, rhs);
    }
}

/* Lazy PF: parity of the low result byte, no flag materialisation. */
static inline int md_x86_pf(const MdX86 *cpu)
{
    if (cpu->lazy_op == MD_LAZY_NONE) return (cpu->flags_raw & MD_X86_FLAG_PF) != 0u;
    return (int)md_x86_even_parity8((uint8_t)cpu->lazy_res);
}

/* SF != OF ("signed less than"). After SUB/CMP/SCAS/CMPS this is exactly a
   signed compare of the two inputs, so the overflow algebra disappears. */
static inline int md_x86_lt(const MdX86 *cpu)
{
    switch (cpu->lazy_op) {
        case MD_LAZY_SUB8:  return (int8_t)(uint8_t)cpu->lazy_a < (int8_t)(uint8_t)cpu->lazy_b;
        case MD_LAZY_SUB16: return (int16_t)cpu->lazy_a < (int16_t)cpu->lazy_b;
        case MD_LAZY_LOGIC8: case MD_LAZY_LOGIC16:   /* OF = 0, so lt = SF */
            return (cpu->lazy_res & md_lazy_sign(cpu->lazy_op)) != 0u;
        default: return md_x86_sf(cpu) != md_x86_of(cpu);
    }
}

/* CF | ZF ("unsigned below or equal"). After SUB/CMP: a <= b. */
static inline int md_x86_be(const MdX86 *cpu)
{
    if (cpu->lazy_op == MD_LAZY_SUB8 || cpu->lazy_op == MD_LAZY_SUB16)
        return cpu->lazy_a <= cpu->lazy_b;
    return md_x86_cf(cpu) || md_x86_zf(cpu);
}

/* ZF | (SF != OF) ("signed less or equal"). After SUB/CMP: a <= b signed. */
static inline int md_x86_le(const MdX86 *cpu)
{
    switch (cpu->lazy_op) {
        case MD_LAZY_SUB8:  return (int8_t)(uint8_t)cpu->lazy_a <= (int8_t)(uint8_t)cpu->lazy_b;
        case MD_LAZY_SUB16: return (int16_t)cpu->lazy_a <= (int16_t)cpu->lazy_b;
        default: return md_x86_zf(cpu) || md_x86_lt(cpu);
    }
}

static inline int md_x86_condition(const MdX86 *cpu, unsigned cc)
{
    /* Even/odd condition pairs differ only in polarity, so evaluate the
       even predicate once and flip with the low bit of cc. */
    int t;
    switch ((cc >> 1) & 7u) {
        case 0u: t = md_x86_of(cpu); break;
        case 1u: t = md_x86_cf(cpu); break;
        case 2u: t = md_x86_zf(cpu); break;
        case 3u: t = md_x86_be(cpu); break;
        case 4u: t = md_x86_sf(cpu); break;
        case 5u: t = md_x86_pf(cpu); break;
        case 6u: t = md_x86_lt(cpu); break;
        default: t = md_x86_le(cpu); break;
    }
    return t ^ (int)(cc & 1u);
}

#ifdef __cplusplus
}
#endif
#endif
