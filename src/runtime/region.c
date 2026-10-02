#include "microdos/region.h"
#include "microdos/ops.h"

uint32_t md_region_try_dec_jnz(MdRuntime *runtime, unsigned reg,
                               uint16_t entry_ip, uint16_t exit_ip,
                               uint32_t budget)
{
    MdX86 *cpu;
    uint16_t start;
    uint16_t final_value;
    uint32_t trips;
    uint32_t run;

    if (runtime == NULL || runtime->stop_reason != MD_STOP_NONE) return 0u;
    cpu = &runtime->cpu;
    if (cpu->ip != entry_ip || reg >= 8u) return 0u;

    start = cpu->r[reg];
    trips = start != 0u ? (uint32_t)start : 65536u;
    run = budget / 2u;                  /* DEC + JNZ */
    if (run > trips) run = trips;
    if (run == 0u) return 0u;

    /*
     * Execute `run` counted iterations algebraically. DEC preserves CF, and
     * every other arithmetic flag after the admitted prefix is exactly the
     * flag state of its final DEC. One canonical lazy DEC on the value that
     * precedes the final result therefore reproduces the exact FLAGS state.
     *
     * This also handles CX=0's 65536-trip 8086 wrap naturally:
     * final_value is 16-bit modulo arithmetic and final_value+1 is the input
     * of the final DEC.
     */
    final_value = (uint16_t)(start - (uint16_t)run);
    cpu->r[reg] = md_x86_dec16(cpu, (uint16_t)(final_value + 1u));
    cpu->ip = run == trips ? exit_ip : entry_ip;
    return run * 2u;
}

static uint16_t md_region_read16_linear(const MdX86 *cpu, uint32_t linear)
{
    const uint32_t lo = linear & MD_X86_ADDRESS_MASK;
    const uint32_t hi = (lo + 1u) & MD_X86_ADDRESS_MASK;
    return (uint16_t)((uint16_t)cpu->memory[lo] |
                      ((uint16_t)cpu->memory[hi] << 8));
}

static uint16_t md_region_read16_ptr(const uint8_t *p)
{
    /* Explicit little-endian byte assembly is alignment-safe and portable. */
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t md_region_try_lodsw_add_dx_ax_loop(MdRuntime *runtime,
                                            uint16_t entry_ip,
                                            uint16_t exit_ip,
                                            uint32_t budget)
{
    MdX86 *cpu;
    uint32_t trips;
    uint32_t run;
    uint32_t base;
    uint16_t cx, si, dx, ax;
    uint16_t delta;

    if (runtime == NULL || runtime->stop_reason != MD_STOP_NONE) return 0u;
    cpu = &runtime->cpu;
    if (cpu->memory == NULL || cpu->ip != entry_ip) return 0u;

    cx = cpu->r[MD_X86_CX];
    trips = cx != 0u ? (uint32_t)cx : 65536u;
    run = budget / 3u;                  /* LODSW + ADD DX,AX + LOOP */
    if (run > trips) run = trips;
    if (run == 0u) return 0u;

    base = ((uint32_t)cpu->ds << 4) & MD_X86_ADDRESS_MASK;
    si = cpu->r[MD_X86_SI];
    dx = cpu->r[MD_X86_DX];
    delta = (cpu->flags_raw & MD_X86_FLAG_DF) != 0u
              ? (uint16_t)-2
              : (uint16_t)2u;

    /*
     * M21.1c fast path: the overwhelmingly common forward checksum/scan case
     * walks one contiguous host-memory span. Prove both 16-bit SI and guest
     * physical addressing stay contiguous for every admitted read, then
     * remove the per-word address add/mask/wrap work from the hot loop.
     *
     * All but the final ADD may accumulate without flag bookkeeping because
     * LODSW and LOOP do not modify FLAGS. The final ADD goes through the
     * canonical helper, making its lazy arithmetic flags architecturally
     * exact. `acc` cannot overflow uint32_t: at most 65535 raw uint16_t
     * operands precede the final ADD.
     */
    if (delta == 2u) {
        const uint32_t span = run * 2u;
        const uint32_t linear = (base + (uint32_t)si) & MD_X86_ADDRESS_MASK;

        if ((uint32_t)si + span <= 0x10000u &&
            linear + span <= MD_X86_ADDRESS_SPACE) {
            const uint8_t *p = cpu->memory + linear;
            uint32_t raw = run - 1u;
            uint32_t acc = dx;

            /* Four words per branch on the common long checksum path. */
            while (raw >= 4u) {
                acc += md_region_read16_ptr(p + 0u);
                acc += md_region_read16_ptr(p + 2u);
                acc += md_region_read16_ptr(p + 4u);
                acc += md_region_read16_ptr(p + 6u);
                p += 8u;
                raw -= 4u;
            }
            while (raw != 0u) {
                acc += md_region_read16_ptr(p);
                p += 2u;
                --raw;
            }

            ax = md_region_read16_ptr(p);
            dx = md_x86_add16(cpu, (uint16_t)acc, ax);
            si = (uint16_t)(si + (uint16_t)span);
        } else {
            uint32_t i;
            for (i = 1u; i < run; ++i) {
                const uint32_t linear_i = (base + (uint32_t)si) & MD_X86_ADDRESS_MASK;
                ax = md_region_read16_linear(cpu, linear_i);
                dx = (uint16_t)(dx + ax);
                si = (uint16_t)(si + delta);
            }
            ax = md_region_read16_linear(cpu, base + (uint32_t)si);
            dx = md_x86_add16(cpu, dx, ax);
            si = (uint16_t)(si + delta);
        }
    } else {
        uint32_t i;
        for (i = 1u; i < run; ++i) {
            const uint32_t linear = (base + (uint32_t)si) & MD_X86_ADDRESS_MASK;
            ax = md_region_read16_linear(cpu, linear);
            dx = (uint16_t)(dx + ax);
            si = (uint16_t)(si + delta);
        }
        ax = md_region_read16_linear(cpu, base + (uint32_t)si);
        dx = md_x86_add16(cpu, dx, ax);
        si = (uint16_t)(si + delta);
    }

    cpu->r[MD_X86_AX] = ax;
    cpu->r[MD_X86_CX] = (uint16_t)(cx - (uint16_t)run);
    cpu->r[MD_X86_DX] = dx;
    cpu->r[MD_X86_SI] = si;
    cpu->ip = run == trips ? exit_ip : entry_ip;
    return run * 3u;
}
