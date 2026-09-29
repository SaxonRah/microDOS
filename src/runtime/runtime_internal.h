#ifndef MICRODOS_RUNTIME_INTERNAL_H
#define MICRODOS_RUNTIME_INTERNAL_H

#include <stdint.h>

#include "microdos/runtime.h"

static inline uint8_t md_fetch8(MdRuntime *runtime)
{
    const uint8_t value = md_x86_read8(&runtime->cpu, runtime->cpu.cs, runtime->cpu.ip);
    runtime->cpu.ip = (uint16_t)(runtime->cpu.ip + 1u);
    return value;
}

static inline uint16_t md_fetch16(MdRuntime *runtime)
{
    const uint16_t value = md_x86_read16(&runtime->cpu, runtime->cpu.cs, runtime->cpu.ip);
    runtime->cpu.ip = (uint16_t)(runtime->cpu.ip + 2u);
    return value;
}

static inline unsigned md_even_parity8(uint8_t value)
{
    value ^= (uint8_t)(value >> 4);
    value &= 0x0Fu;
    return (0x9669u >> value) & 1u;
}

static inline void md_set_szp8(MdX86 *cpu, uint8_t value)
{
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF);
    if (value == 0u) cpu->flags |= MD_X86_FLAG_ZF;
    if (value & 0x80u) cpu->flags |= MD_X86_FLAG_SF;
    if (md_even_parity8(value)) cpu->flags |= MD_X86_FLAG_PF;
}

static inline void md_set_szp16(MdX86 *cpu, uint16_t value)
{
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_PF);
    if (value == 0u) cpu->flags |= MD_X86_FLAG_ZF;
    if (value & 0x8000u) cpu->flags |= MD_X86_FLAG_SF;
    if (md_even_parity8((uint8_t)value)) cpu->flags |= MD_X86_FLAG_PF;
}

static inline uint8_t md_add8(MdX86 *cpu, uint8_t a, uint8_t b)
{
    const uint16_t wide = (uint16_t)a + (uint16_t)b;
    const uint8_t result = (uint8_t)wide;
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (wide & 0x100u) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if (((~(a ^ b) & (a ^ result)) & 0x80u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    md_set_szp8(cpu, result);
    return result;
}

static inline uint16_t md_add16(MdX86 *cpu, uint16_t a, uint16_t b)
{
    const uint32_t wide = (uint32_t)a + (uint32_t)b;
    const uint16_t result = (uint16_t)wide;
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (wide & 0x10000u) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if (((~(a ^ b) & (a ^ result)) & 0x8000u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    md_set_szp16(cpu, result);
    return result;
}

static inline uint8_t md_sub8(MdX86 *cpu, uint8_t a, uint8_t b)
{
    const uint8_t result = (uint8_t)(a - b);
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (a < b) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if ((((a ^ b) & (a ^ result)) & 0x80u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    md_set_szp8(cpu, result);
    return result;
}

static inline uint16_t md_sub16(MdX86 *cpu, uint16_t a, uint16_t b)
{
    const uint16_t result = (uint16_t)(a - b);
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_CF | MD_X86_FLAG_AF | MD_X86_FLAG_OF);
    if (a < b) cpu->flags |= MD_X86_FLAG_CF;
    if (((a ^ b ^ result) & 0x10u) != 0u) cpu->flags |= MD_X86_FLAG_AF;
    if ((((a ^ b) & (a ^ result)) & 0x8000u) != 0u) cpu->flags |= MD_X86_FLAG_OF;
    md_set_szp16(cpu, result);
    return result;
}

#endif
