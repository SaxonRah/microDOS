#include "md_dos2_system.h"

#include <string.h>

#define MD_DOS2_KERNEL_SIZE 16690u
#define MD_DOS2_AOT_CHUNK 65536u

void md_dos2_system_init(MdDos2System *sys, uint8_t *memory, MdBlockCache *cache)
{
    MdHooks hooks;

    memset(sys, 0, sizeof(*sys));
    md_msdos2_boot_init(&sys->boot);
    sys->boot.continue_after_dosinit = true;

    memset(&hooks, 0, sizeof(hooks));
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &sys->boot;
    md_runtime_init(&sys->runtime, memory, &hooks);

    sys->cache = cache;
    if (cache != NULL) {
        md_block_cache_init(cache);
        md_runtime_set_block_cache(&sys->runtime, cache);
    }
}

void md_dos2_system_set_aot(MdDos2System *sys, const MdAotProgram *const *programs,
                            size_t count, bool enabled)
{
    sys->aot_programs = programs;
    sys->aot_program_count = count;
    sys->aot_enabled = enabled && count != 0u;
}

bool md_dos2_system_start(MdDos2System *sys, const uint8_t *msdos_sys, size_t size)
{
    if (msdos_sys == NULL || size != MD_DOS2_KERNEL_SIZE ||
        msdos_sys[0] != 0xE9u || msdos_sys[1] != 0x78u || msdos_sys[2] != 0x3Eu) {
        return false;
    }
    md_msdos2_boot_prepare_cpu(&sys->runtime, &sys->boot, msdos_sys, size);
    return true;
}

/* Programs only ever run outside the kernel and OEM/device segments. */
static bool md_user_segment(const MdDos2System *sys, uint16_t cs)
{
    return cs != sys->boot.dos_segment && cs != sys->boot.bios_segment;
}

/* Returns the program whose compiled code may run at the current CS:IP,
   attaching it first when DOS has just started it at XXXX:0100. */
static const MdAotProgram *md_aot_candidate(MdDos2System *sys)
{
    MdRuntime *rt = &sys->runtime;
    const uint16_t cs = rt->cpu.cs;
    size_t i;

    for (i = 0; i < sys->aot_program_count; ++i) {
        const MdAotProgram *prog = sys->aot_programs[i];
        if (prog->ready(rt, cs)) return prog;
        if (rt->cpu.ip == 0x0100u && prog->attach(rt, cs)) {
            ++sys->aot_attaches;
            return prog;
        }
    }
    return NULL;
}

/* Cache stop predicate: hand control back when compiled code could run here
   (or DOS just started something at 0100h that might be ours). */
static bool md_aot_stop(const MdRuntime *rt, void *user)
{
    const MdDos2System *sys = (const MdDos2System *)user;
    const uint16_t cs = rt->cpu.cs;
    size_t i;

    if (!md_user_segment(sys, cs)) return false;
    if (rt->cpu.ip == 0x0100u) return true;
    for (i = 0; i < sys->aot_program_count; ++i) {
        const MdAotProgram *prog = sys->aot_programs[i];
        if (prog->is_entry(rt->cpu.ip) && prog->ready(rt, cs)) return true;
    }
    return false;
}

MdStopReason md_dos2_system_run(MdDos2System *sys, uint64_t budget)
{
    MdRuntime *rt = &sys->runtime;
    const uint64_t start = rt->instructions;

    while (rt->stop_reason == MD_STOP_NONE) {
        const uint64_t used = rt->instructions - start;
        uint64_t left;

        if (used >= budget) break;
        left = budget - used;

        if (sys->aot_enabled && md_aot_stop(rt, sys)) {
            const MdAotProgram *prog = md_aot_candidate(sys);
            if (prog != NULL && prog->is_entry(rt->cpu.ip)) {
                const uint64_t before = rt->instructions;
                const MdStopReason st =
                    prog->enter(rt, left < MD_DOS2_AOT_CHUNK ? left : MD_DOS2_AOT_CHUNK);
                ++sys->aot_enters;
                sys->aot_last_program = prog;
                sys->aot_last_segment = rt->cpu.cs;
                if (st != MD_STOP_NONE) break;
                if (rt->instructions != before) continue;
            }
            /* Not compiled here (or no progress): one canonical instruction
               guarantees progress past the predicate's trigger point. */
            (void)md_interp_step(rt);
            continue;
        }

        if (sys->cache != NULL) {
            (void)md_interp_run_cached_until(rt, sys->cache, left,
                                             sys->aot_enabled ? md_aot_stop : NULL, sys);
            if (rt->stop_reason == MD_STOP_BUDGET) {
                rt->stop_reason = MD_STOP_NONE;       /* slice boundary, not a stop */
                break;
            }
        } else {
            (void)md_interp_step(rt);
        }
    }
    return rt->stop_reason;
}
