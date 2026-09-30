/* Reference copy of the pre-M18 EAGER flag helpers, kept verbatim (names
   prefixed ref_) so tests can prove the lazy implementation computes
   exactly the same flags. Operates on a plain uint16_t flags word. */
#ifndef MICRODOS_EAGER_FLAGS_REF_H
#define MICRODOS_EAGER_FLAGS_REF_H

#include <stdint.h>
#include "microdos/x86.h"
typedef struct RefCpu { uint16_t flags; } RefCpu;

#ifdef __cplusplus
extern "C" {
#endif

/* Shared by the interpreter and dosrecomp-generated code. */
static inline unsigned ref_x86_even_parity8(uint8_t value)
{
    value ^= (uint8_t)(value >> 4);
    value &= 0x0Fu;
    return (0x9669u >> value) & 1u;
}

static inline void ref_x86_set_szp8(RefCpu *cpu, uint8_t value)
{
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF);
    if (value == 0u) cpu->flags |= MD_X86_FLAG_ZF;
    if ((value & 0x80u) != 0u) cpu->flags |= MD_X86_FLAG_SF;
    if (ref_x86_even_parity8(value)) cpu->flags |= MD_X86_FLAG_PF;
}

static inline void ref_x86_set_szp16(RefCpu *cpu, uint16_t value)
{
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF);
    if (value == 0u) cpu->flags |= MD_X86_FLAG_ZF;
    if ((value & 0x8000u) != 0u) cpu->flags |= MD_X86_FLAG_SF;
    if (ref_x86_even_parity8((uint8_t)value)) cpu->flags |= MD_X86_FLAG_PF;
}

static inline uint8_t ref_x86_add8(RefCpu *cpu, uint8_t a, uint8_t b)
{
    const uint16_t wide = (uint16_t)a + (uint16_t)b;
    const uint8_t result = (uint8_t)wide;
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if ((wide & 0x100u) != 0u) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if (((~(a ^ b) & (a ^ result)) & 0x80u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp8(cpu, result);
    return result;
}

static inline uint16_t ref_x86_add16(RefCpu *cpu, uint16_t a, uint16_t b)
{
    const uint32_t wide = (uint32_t)a + (uint32_t)b;
    const uint16_t result = (uint16_t)wide;
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if ((wide & 0x10000u) != 0u) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if (((~(a ^ b) & (a ^ result)) & 0x8000u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp16(cpu, result);
    return result;
}

static inline uint8_t ref_x86_adc8(RefCpu *cpu, uint8_t a, uint8_t b)
{
    const unsigned carry = (cpu->flags & MD_X86_FLAG_CF) != 0u;
    const uint16_t wide = (uint16_t)((uint16_t)a + (uint16_t)b + (uint16_t)carry);
    const int16_t signed_wide = (int16_t)(int8_t)a + (int16_t)(int8_t)b + (int16_t)carry;
    const uint8_t result = (uint8_t)wide;
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (wide > 0xFFu) cpu->flags |= MD_X86_FLAG_CF;
    if (((a & 0x0Fu) + (b & 0x0Fu) + carry) > 0x0Fu) cpu->flags |= MD_X86_FLAG_AF;
    if (signed_wide < -128 || signed_wide > 127) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp8(cpu, result);
    return result;
}

static inline uint16_t ref_x86_adc16(RefCpu *cpu, uint16_t a, uint16_t b)
{
    const unsigned carry = (cpu->flags & MD_X86_FLAG_CF) != 0u;
    const uint32_t wide = (uint32_t)a + (uint32_t)b + carry;
    const int32_t signed_wide = (int32_t)(int16_t)a + (int32_t)(int16_t)b + (int32_t)carry;
    const uint16_t result = (uint16_t)wide;
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (wide > 0xFFFFu) cpu->flags |= MD_X86_FLAG_CF;
    if (((a & 0x0Fu) + (b & 0x0Fu) + carry) > 0x0Fu) cpu->flags |= MD_X86_FLAG_AF;
    if (signed_wide < -32768 || signed_wide > 32767) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp16(cpu, result);
    return result;
}

static inline uint8_t ref_x86_sub8(RefCpu *cpu, uint8_t a, uint8_t b)
{
    const uint8_t result = (uint8_t)(a - b);
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (a < b) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if ((((a ^ b) & (a ^ result)) & 0x80u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp8(cpu, result);
    return result;
}

static inline uint16_t ref_x86_sub16(RefCpu *cpu, uint16_t a, uint16_t b)
{
    const uint16_t result = (uint16_t)(a - b);
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (a < b) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if ((((a ^ b) & (a ^ result)) & 0x8000u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp16(cpu, result);
    return result;
}

static inline uint8_t ref_x86_sbb8(RefCpu *cpu, uint8_t a, uint8_t b)
{
    const unsigned borrow = (cpu->flags & MD_X86_FLAG_CF) != 0u;
    const uint16_t rhs = (uint16_t)((uint16_t)b + (uint16_t)borrow);
    const int16_t signed_wide = (int16_t)(int8_t)a - (int16_t)(int8_t)b - (int16_t)borrow;
    const uint8_t result = (uint8_t)((uint16_t)a - rhs);
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if ((uint16_t)a < rhs) cpu->flags |= MD_X86_FLAG_CF;
    if ((a & 0x0Fu) < ((b & 0x0Fu) + borrow)) cpu->flags |= MD_X86_FLAG_AF;
    if (signed_wide < -128 || signed_wide > 127) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp8(cpu, result);
    return result;
}

static inline uint16_t ref_x86_sbb16(RefCpu *cpu, uint16_t a, uint16_t b)
{
    const unsigned borrow = (cpu->flags & MD_X86_FLAG_CF) != 0u;
    const uint32_t rhs = (uint32_t)b + borrow;
    const int32_t signed_wide = (int32_t)(int16_t)a - (int32_t)(int16_t)b - (int32_t)borrow;
    const uint16_t result = (uint16_t)((uint32_t)a - rhs);
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if ((uint32_t)a < rhs) cpu->flags |= MD_X86_FLAG_CF;
    if ((a & 0x0Fu) < ((b & 0x0Fu) + borrow)) cpu->flags |= MD_X86_FLAG_AF;
    if (signed_wide < -32768 || signed_wide > 32767) cpu->flags |= MD_X86_FLAG_OF;
    ref_x86_set_szp16(cpu, result);
    return result;
}

static inline uint8_t ref_x86_logic8(RefCpu *cpu, uint8_t value)
{
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_OF | MD_X86_FLAG_AF);
    ref_x86_set_szp8(cpu, value);
    return value;
}

static inline uint16_t ref_x86_logic16(RefCpu *cpu, uint16_t value)
{
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_OF | MD_X86_FLAG_AF);
    ref_x86_set_szp16(cpu, value);
    return value;
}

static inline uint8_t ref_x86_shift8(RefCpu *cpu, unsigned operation,
                                    uint8_t value, unsigned count)
{
    uint8_t result = value;
    const uint8_t original = value;
    unsigned i;

    count &= 0xFFu; /* 8086 uses the full CL count; it does not mask to 5 bits. */
    if (count == 0u) return value;

    for (i = 0u; i < count; ++i) {
        const unsigned old_cf = (cpu->flags & MD_X86_FLAG_CF) != 0u;
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
        cpu->flags = (uint16_t)((cpu->flags & (uint16_t)~MD_X86_FLAG_CF) |
                                (new_cf ? MD_X86_FLAG_CF : 0u));
    }

    if ((operation & 7u) >= 4u) ref_x86_set_szp8(cpu, result);

    if (count == 1u) {
        cpu->flags &= (uint16_t)~MD_X86_FLAG_OF;
        switch (operation & 7u) {
            case 0u: case 2u: case 4u: case 6u:
                if ((((result >> 7) & 1u) ^ ((cpu->flags & MD_X86_FLAG_CF) != 0u)) != 0u)
                    cpu->flags |= MD_X86_FLAG_OF;
                break;
            case 1u: case 3u:
                if ((((result >> 7) ^ (result >> 6)) & 1u) != 0u)
                    cpu->flags |= MD_X86_FLAG_OF;
                break;
            case 5u:
                if ((original & 0x80u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
                break;
            default: /* SAR */
                break;
        }
    }
    return result;
}

static inline uint16_t ref_x86_shift16(RefCpu *cpu, unsigned operation,
                                      uint16_t value, unsigned count)
{
    uint16_t result = value;
    const uint16_t original = value;
    unsigned i;

    count &= 0xFFu;
    if (count == 0u) return value;

    for (i = 0u; i < count; ++i) {
        const unsigned old_cf = (cpu->flags & MD_X86_FLAG_CF) != 0u;
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
        cpu->flags = (uint16_t)((cpu->flags & (uint16_t)~MD_X86_FLAG_CF) |
                                (new_cf ? MD_X86_FLAG_CF : 0u));
    }

    if ((operation & 7u) >= 4u) ref_x86_set_szp16(cpu, result);

    if (count == 1u) {
        cpu->flags &= (uint16_t)~MD_X86_FLAG_OF;
        switch (operation & 7u) {
            case 0u: case 2u: case 4u: case 6u:
                if ((((result >> 15) & 1u) ^ ((cpu->flags & MD_X86_FLAG_CF) != 0u)) != 0u)
                    cpu->flags |= MD_X86_FLAG_OF;
                break;
            case 1u: case 3u:
                if ((((result >> 15) ^ (result >> 14)) & 1u) != 0u)
                    cpu->flags |= MD_X86_FLAG_OF;
                break;
            case 5u:
                if ((original & 0x8000u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
                break;
            default:
                break;
        }
    }
    return result;
}

/* ALU group dispatch shared by the interpreter and generated code:
   0 ADD, 1 OR, 2 ADC, 3 SBB, 4 AND, 5 SUB, 6 XOR, 7 CMP (result discarded). */
static inline uint8_t ref_x86_alu8(RefCpu *cpu, unsigned operation, uint8_t lhs, uint8_t rhs)
{
    switch (operation & 7u) {
        case 0u: return ref_x86_add8(cpu, lhs, rhs);
        case 1u: return ref_x86_logic8(cpu, (uint8_t)(lhs | rhs));
        case 2u: return ref_x86_adc8(cpu, lhs, rhs);
        case 3u: return ref_x86_sbb8(cpu, lhs, rhs);
        case 4u: return ref_x86_logic8(cpu, (uint8_t)(lhs & rhs));
        case 5u: return ref_x86_sub8(cpu, lhs, rhs);
        case 6u: return ref_x86_logic8(cpu, (uint8_t)(lhs ^ rhs));
        default: return ref_x86_sub8(cpu, lhs, rhs);
    }
}

static inline uint16_t ref_x86_alu16(RefCpu *cpu, unsigned operation, uint16_t lhs, uint16_t rhs)
{
    switch (operation & 7u) {
        case 0u: return ref_x86_add16(cpu, lhs, rhs);
        case 1u: return ref_x86_logic16(cpu, (uint16_t)(lhs | rhs));
        case 2u: return ref_x86_adc16(cpu, lhs, rhs);
        case 3u: return ref_x86_sbb16(cpu, lhs, rhs);
        case 4u: return ref_x86_logic16(cpu, (uint16_t)(lhs & rhs));
        case 5u: return ref_x86_sub16(cpu, lhs, rhs);
        case 6u: return ref_x86_logic16(cpu, (uint16_t)(lhs ^ rhs));
        default: return ref_x86_sub16(cpu, lhs, rhs);
    }
}

static inline int ref_x86_condition(const RefCpu *cpu, unsigned cc)
{
    const unsigned cf = (cpu->flags & MD_X86_FLAG_CF) != 0u;
    const unsigned pf = (cpu->flags & MD_X86_FLAG_PF) != 0u;
    const unsigned zf = (cpu->flags & MD_X86_FLAG_ZF) != 0u;
    const unsigned sf = (cpu->flags & MD_X86_FLAG_SF) != 0u;
    const unsigned of = (cpu->flags & MD_X86_FLAG_OF) != 0u;

    switch (cc & 0x0Fu) {
        case 0x0u: return of != 0u;
        case 0x1u: return of == 0u;
        case 0x2u: return cf != 0u;
        case 0x3u: return cf == 0u;
        case 0x4u: return zf != 0u;
        case 0x5u: return zf == 0u;
        case 0x6u: return cf != 0u || zf != 0u;
        case 0x7u: return cf == 0u && zf == 0u;
        case 0x8u: return sf != 0u;
        case 0x9u: return sf == 0u;
        case 0xAu: return pf != 0u;
        case 0xBu: return pf == 0u;
        case 0xCu: return sf != of;
        case 0xDu: return sf == of;
        case 0xEu: return zf != 0u || sf != of;
        default:   return zf == 0u && sf == of;
    }
}

#ifdef __cplusplus
}
#endif
#endif
