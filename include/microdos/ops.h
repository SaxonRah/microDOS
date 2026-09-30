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
    cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF);
    if (value == 0u) cpu->flags_raw |= MD_X86_FLAG_ZF;
    if ((value & 0x80u) != 0u) cpu->flags_raw |= MD_X86_FLAG_SF;
    if (md_x86_even_parity8(value)) cpu->flags_raw |= MD_X86_FLAG_PF;
}

static inline void md_x86_set_szp16(MdX86 *cpu, uint16_t value)
{
    cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF);
    if (value == 0u) cpu->flags_raw |= MD_X86_FLAG_ZF;
    if ((value & 0x8000u) != 0u) cpu->flags_raw |= MD_X86_FLAG_SF;
    if (md_x86_even_parity8((uint8_t)value)) cpu->flags_raw |= MD_X86_FLAG_PF;
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

static inline uint8_t md_x86_shift8(MdX86 *cpu, unsigned operation,
                                    uint8_t value, unsigned count)
{
    md_x86_flags_materialize(cpu);   /* shifts stay eager (M18) */
    uint8_t result = value;
    const uint8_t original = value;
    unsigned i;

    count &= 0xFFu; /* 8086 uses the full CL count; it does not mask to 5 bits. */
    if (count == 0u) return value;

    for (i = 0u; i < count; ++i) {
        const unsigned old_cf = (cpu->flags_raw & MD_X86_FLAG_CF) != 0u;
        unsigned new_cf = 0u;
        switch (operation & 7u) {
            case 0u: /* ROL */
                new_cf = (result >> 7) & 1u;
                result = (uint8_t)((uint8_t)(result << 1) | (uint8_t)new_cf);
                break;
            case 1u: /* ROR */
                new_cf = result & 1u;
                result = (uint8_t)((result >> 1) | (uint8_t)(new_cf << 7));
                break;
            case 2u: /* RCL */
                new_cf = (result >> 7) & 1u;
                result = (uint8_t)((uint8_t)(result << 1) | (uint8_t)old_cf);
                break;
            case 3u: /* RCR */
                new_cf = result & 1u;
                result = (uint8_t)((result >> 1) | (uint8_t)(old_cf << 7));
                break;
            case 4u: /* SHL/SAL */
            case 6u: /* undocumented 8086 alias */
                new_cf = (result >> 7) & 1u;
                result = (uint8_t)(result << 1);
                break;
            case 5u: /* SHR */
                new_cf = result & 1u;
                result = (uint8_t)(result >> 1);
                break;
            default: /* SAR */
                new_cf = result & 1u;
                result = (uint8_t)((result >> 1) | (result & 0x80u));
                break;
        }
        cpu->flags_raw = (uint16_t)((cpu->flags_raw & (uint16_t)~MD_X86_FLAG_CF) |
                                (new_cf ? MD_X86_FLAG_CF : 0u));
    }

    if ((operation & 7u) >= 4u) md_x86_set_szp8(cpu, result);

    if (count == 1u) {
        cpu->flags_raw &= (uint16_t)~MD_X86_FLAG_OF;
        switch (operation & 7u) {
            case 0u: case 2u: case 4u: case 6u:
                if ((((result >> 7) & 1u) ^ ((cpu->flags_raw & MD_X86_FLAG_CF) != 0u)) != 0u)
                    cpu->flags_raw |= MD_X86_FLAG_OF;
                break;
            case 1u: case 3u:
                if ((((result >> 7) ^ (result >> 6)) & 1u) != 0u)
                    cpu->flags_raw |= MD_X86_FLAG_OF;
                break;
            case 5u:
                if ((original & 0x80u) != 0u) cpu->flags_raw |= MD_X86_FLAG_OF;
                break;
            default: /* SAR */
                break;
        }
    }
    return result;
}

static inline uint16_t md_x86_shift16(MdX86 *cpu, unsigned operation,
                                      uint16_t value, unsigned count)
{
    md_x86_flags_materialize(cpu);   /* shifts stay eager (M18) */
    uint16_t result = value;
    const uint16_t original = value;
    unsigned i;

    count &= 0xFFu;
    if (count == 0u) return value;

    for (i = 0u; i < count; ++i) {
        const unsigned old_cf = (cpu->flags_raw & MD_X86_FLAG_CF) != 0u;
        unsigned new_cf = 0u;
        switch (operation & 7u) {
            case 0u: /* ROL */
                new_cf = (result >> 15) & 1u;
                result = (uint16_t)((uint16_t)(result << 1) | (uint16_t)new_cf);
                break;
            case 1u: /* ROR */
                new_cf = result & 1u;
                result = (uint16_t)((result >> 1) | (uint16_t)(new_cf << 15));
                break;
            case 2u: /* RCL */
                new_cf = (result >> 15) & 1u;
                result = (uint16_t)((uint16_t)(result << 1) | (uint16_t)old_cf);
                break;
            case 3u: /* RCR */
                new_cf = result & 1u;
                result = (uint16_t)((result >> 1) | (uint16_t)(old_cf << 15));
                break;
            case 4u: /* SHL/SAL */
            case 6u:
                new_cf = (result >> 15) & 1u;
                result = (uint16_t)(result << 1);
                break;
            case 5u: /* SHR */
                new_cf = result & 1u;
                result = (uint16_t)(result >> 1);
                break;
            default: /* SAR */
                new_cf = result & 1u;
                result = (uint16_t)((result >> 1) | (result & 0x8000u));
                break;
        }
        cpu->flags_raw = (uint16_t)((cpu->flags_raw & (uint16_t)~MD_X86_FLAG_CF) |
                                (new_cf ? MD_X86_FLAG_CF : 0u));
    }

    if ((operation & 7u) >= 4u) md_x86_set_szp16(cpu, result);

    if (count == 1u) {
        cpu->flags_raw &= (uint16_t)~MD_X86_FLAG_OF;
        switch (operation & 7u) {
            case 0u: case 2u: case 4u: case 6u:
                if ((((result >> 15) & 1u) ^ ((cpu->flags_raw & MD_X86_FLAG_CF) != 0u)) != 0u)
                    cpu->flags_raw |= MD_X86_FLAG_OF;
                break;
            case 1u: case 3u:
                if ((((result >> 15) ^ (result >> 14)) & 1u) != 0u)
                    cpu->flags_raw |= MD_X86_FLAG_OF;
                break;
            case 5u:
                if ((original & 0x8000u) != 0u) cpu->flags_raw |= MD_X86_FLAG_OF;
                break;
            default:
                break;
        }
    }
    return result;
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

static inline int md_x86_condition(const MdX86 *cpu, unsigned cc)
{
    /* M18: direct lazy predicates; only JP/JNP needs the parity bit. */
    switch (cc & 0x0Fu) {
        case 0x0u: return md_x86_of(cpu);
        case 0x1u: return !md_x86_of(cpu);
        case 0x2u: return md_x86_cf(cpu);
        case 0x3u: return !md_x86_cf(cpu);
        case 0x4u: return md_x86_zf(cpu);
        case 0x5u: return !md_x86_zf(cpu);
        case 0x6u: return md_x86_cf(cpu) || md_x86_zf(cpu);
        case 0x7u: return !md_x86_cf(cpu) && !md_x86_zf(cpu);
        case 0x8u: return md_x86_sf(cpu);
        case 0x9u: return !md_x86_sf(cpu);
        case 0xAu: case 0xBu: {
            MdX86 tmp = *cpu;
            const int pf = (md_x86_flags(&tmp) & MD_X86_FLAG_PF) != 0u;
            return (cc & 1u) ? !pf : pf;
        }
        case 0xCu: return md_x86_sf(cpu) != md_x86_of(cpu);
        case 0xDu: return md_x86_sf(cpu) == md_x86_of(cpu);
        case 0xEu: return md_x86_zf(cpu) || md_x86_sf(cpu) != md_x86_of(cpu);
        default:   return !md_x86_zf(cpu) && md_x86_sf(cpu) == md_x86_of(cpu);
    }
}

#ifdef __cplusplus
}
#endif
#endif
