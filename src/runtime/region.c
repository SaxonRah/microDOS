#include "microdos/region.h"
#include "microdos/ops.h"

uint32_t md_region_try_dec_jnz(MdRuntime *runtime, unsigned reg,
                               uint16_t entry_ip, uint16_t exit_ip,
                               uint32_t budget)
{
    MdX86 *cpu;
    uint32_t trips;
    uint32_t retired;

    if (runtime == NULL || runtime->stop_reason != MD_STOP_NONE) return 0u;
    cpu = &runtime->cpu;
    if (cpu->ip != entry_ip || reg >= 8u) return 0u;

    trips = cpu->r[reg] != 0u ? (uint32_t)cpu->r[reg] : 65536u;
    retired = trips * 2u;
    if (budget < retired) return 0u;

    /*
     * DEC preserves CF.  Every other arithmetic flag after the whole loop is
     * exactly the flag state of the final 1 -> 0 DEC, so one canonical lazy
     * DEC is sufficient.  md_x86_dec16 captures the incoming CF before
     * replacing the lazy arithmetic state.
     */
    cpu->r[reg] = md_x86_dec16(cpu, 1u);
    cpu->ip = exit_ip;
    return retired;
}

static uint16_t md_region_read16_linear(const MdX86 *cpu, uint32_t linear)
{
    const uint32_t lo = linear & MD_X86_ADDRESS_MASK;
    const uint32_t hi = (lo + 1u) & MD_X86_ADDRESS_MASK;
    return (uint16_t)((uint16_t)cpu->memory[lo] |
                      ((uint16_t)cpu->memory[hi] << 8));
}

uint32_t md_region_try_lodsw_add_dx_ax_loop(MdRuntime *runtime,
                                            uint16_t entry_ip,
                                            uint16_t exit_ip,
                                            uint32_t budget)
{
    MdX86 *cpu;
    uint32_t trips;
    uint32_t retired;
    uint32_t base;
    uint16_t si, dx, ax;
    uint16_t delta;
    uint32_t i;

    if (runtime == NULL || runtime->stop_reason != MD_STOP_NONE) return 0u;
    cpu = &runtime->cpu;
    if (cpu->memory == NULL || cpu->ip != entry_ip) return 0u;

    trips = cpu->r[MD_X86_CX] != 0u ? (uint32_t)cpu->r[MD_X86_CX] : 65536u;
    retired = trips * 3u;  /* LODSW + ADD DX,AX + LOOP */
    if (budget < retired) return 0u;

    base = ((uint32_t)cpu->ds << 4) & MD_X86_ADDRESS_MASK;
    si = cpu->r[MD_X86_SI];
    dx = cpu->r[MD_X86_DX];
    delta = (cpu->flags_raw & MD_X86_FLAG_DF) != 0u
              ? (uint16_t)-2
              : (uint16_t)2u;

    /*
     * Keep AX/DX/SI/CX resident in C locals.  All non-final ADDs may be raw:
     * only the final ADD's flags are architecturally visible because LODSW
     * and LOOP do not modify FLAGS.  The final ADD uses the canonical lazy
     * helper, preserving exact 8086 flag semantics.
     */
    for (i = 1u; i < trips; ++i) {
        const uint32_t linear = (base + (uint32_t)si) & MD_X86_ADDRESS_MASK;
        ax = md_region_read16_linear(cpu, linear);
        dx = (uint16_t)(dx + ax);
        si = (uint16_t)(si + delta);
    }

    {
        const uint32_t linear = (base + (uint32_t)si) & MD_X86_ADDRESS_MASK;
        ax = md_region_read16_linear(cpu, linear);
        dx = md_x86_add16(cpu, dx, ax);
        si = (uint16_t)(si + delta);
    }

    cpu->r[MD_X86_AX] = ax;
    cpu->r[MD_X86_CX] = 0u;
    cpu->r[MD_X86_DX] = dx;
    cpu->r[MD_X86_SI] = si;
    cpu->ip = exit_ip;
    return retired;
}
