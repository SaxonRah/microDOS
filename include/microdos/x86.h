#ifndef MICRODOS_X86_H
#define MICRODOS_X86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MD_X86_AX = 0,
    MD_X86_CX = 1,
    MD_X86_DX = 2,
    MD_X86_BX = 3,
    MD_X86_SP = 4,
    MD_X86_BP = 5,
    MD_X86_SI = 6,
    MD_X86_DI = 7
};

enum {
    MD_X86_FLAG_CF = 0x0001u,
    MD_X86_FLAG_PF = 0x0004u,
    MD_X86_FLAG_AF = 0x0010u,
    MD_X86_FLAG_ZF = 0x0040u,
    MD_X86_FLAG_SF = 0x0080u,
    MD_X86_FLAG_TF = 0x0100u,
    MD_X86_FLAG_IF = 0x0200u,
    MD_X86_FLAG_DF = 0x0400u,
    MD_X86_FLAG_OF = 0x0800u,
    MD_X86_FLAG_ALWAYS1 = 0x0002u
};

#define MD_X86_ADDRESS_MASK 0x000FFFFFu
#define MD_X86_ADDRESS_SPACE (1u << 20)

typedef struct MdX86 {
    uint16_t r[8];
    uint16_t es;
    uint16_t cs;
    uint16_t ss;
    uint16_t ds;
    uint16_t ip;
    uint16_t flags;
    uint8_t *memory;
} MdX86;

static inline uint32_t md_x86_linear(uint16_t segment, uint16_t offset)
{
    return ((((uint32_t)segment) << 4) + (uint32_t)offset) & MD_X86_ADDRESS_MASK;
}

static inline uint8_t md_x86_read8_linear(const MdX86 *cpu, uint32_t address)
{
    return cpu->memory[address & MD_X86_ADDRESS_MASK];
}

static inline uint16_t md_x86_read16_linear(const MdX86 *cpu, uint32_t address)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    const uint32_t a1 = (a0 + 1u) & MD_X86_ADDRESS_MASK;
    return (uint16_t)((uint16_t)cpu->memory[a0] | ((uint16_t)cpu->memory[a1] << 8));
}

static inline void md_x86_write8_linear(MdX86 *cpu, uint32_t address, uint8_t value)
{
    cpu->memory[address & MD_X86_ADDRESS_MASK] = value;
}

static inline void md_x86_write16_linear(MdX86 *cpu, uint32_t address, uint16_t value)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    const uint32_t a1 = (a0 + 1u) & MD_X86_ADDRESS_MASK;
    cpu->memory[a0] = (uint8_t)value;
    cpu->memory[a1] = (uint8_t)(value >> 8);
}

static inline uint8_t md_x86_read8(const MdX86 *cpu, uint16_t segment, uint16_t offset)
{
    return md_x86_read8_linear(cpu, md_x86_linear(segment, offset));
}

static inline uint16_t md_x86_read16(const MdX86 *cpu, uint16_t segment, uint16_t offset)
{
    return md_x86_read16_linear(cpu, md_x86_linear(segment, offset));
}

static inline void md_x86_write8(MdX86 *cpu, uint16_t segment, uint16_t offset, uint8_t value)
{
    md_x86_write8_linear(cpu, md_x86_linear(segment, offset), value);
}

static inline void md_x86_write16(MdX86 *cpu, uint16_t segment, uint16_t offset, uint16_t value)
{
    md_x86_write16_linear(cpu, md_x86_linear(segment, offset), value);
}

static inline uint8_t md_x86_get_reg8(const MdX86 *cpu, unsigned reg)
{
    const unsigned slot = reg & 3u;
    const unsigned shift = (reg & 4u) ? 8u : 0u;
    return (uint8_t)(cpu->r[slot] >> shift);
}

static inline void md_x86_set_reg8(MdX86 *cpu, unsigned reg, uint8_t value)
{
    const unsigned slot = reg & 3u;
    const uint16_t old = cpu->r[slot];
    if (reg & 4u) {
        cpu->r[slot] = (uint16_t)((old & 0x00FFu) | ((uint16_t)value << 8));
    } else {
        cpu->r[slot] = (uint16_t)((old & 0xFF00u) | value);
    }
}

static inline void md_x86_push(MdX86 *cpu, uint16_t value)
{
    cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] - 2u);
    md_x86_write16(cpu, cpu->ss, cpu->r[MD_X86_SP], value);
}

static inline uint16_t md_x86_pop(MdX86 *cpu)
{
    const uint16_t value = md_x86_read16(cpu, cpu->ss, cpu->r[MD_X86_SP]);
    cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + 2u);
    return value;
}

#ifdef __cplusplus
}
#endif

#endif
