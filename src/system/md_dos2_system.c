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
    sys->aot_program_count = enabled ? count : 0u;
    sys->aot_enabled = enabled && (count != 0u || sys->kernel_program != NULL);
    if (!enabled) sys->kernel_program = NULL;
}

bool md_dos2_system_start(MdDos2System *sys, const uint8_t *msdos_sys, size_t size)
{
    if (msdos_sys == NULL || size != MD_DOS2_KERNEL_SIZE ||
        msdos_sys[0] != 0xE9u || msdos_sys[1] != 0x78u || msdos_sys[2] != 0x3Eu) {
        return false;
    }
    md_msdos2_boot_prepare_cpu(&sys->runtime, &sys->boot, msdos_sys, size);
    if (sys->aot_enabled && sys->kernel_program != NULL) {
        sys->kernel_attached = sys->kernel_program->attach(&sys->runtime, sys->boot.dos_segment);
        if (sys->kernel_attached) ++sys->aot_attaches;
    }
    return true;
}

void md_dos2_system_set_kernel_aot(MdDos2System *sys, const MdAotProgram *kernel)
{
    sys->kernel_program = kernel;
    if (kernel != NULL) sys->aot_enabled = true;
}

/* Returns the program whose compiled code may run at the current CS:IP,
   attaching it first when DOS has just started it at XXXX:0100. */
static const MdAotProgram *md_aot_candidate(MdDos2System *sys)
{
    MdRuntime *rt = &sys->runtime;
    const uint16_t cs = rt->cpu.cs;
    size_t i;

    if (cs == sys->boot.dos_segment) {
        return sys->kernel_attached ? sys->kernel_program : NULL;
    }
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

    if (cs == sys->boot.dos_segment) {
        return sys->kernel_attached && sys->kernel_program->block_ok(rt, cs, rt->cpu.ip);
    }
    if (cs == sys->boot.bios_segment) return false;
    if (rt->cpu.ip == 0x0100u) return true;
    for (i = 0; i < sys->aot_program_count; ++i) {
        if (sys->aot_programs[i]->block_ok(rt, cs, rt->cpu.ip)) return true;
    }
    return false;
}

/* The compiled program attached at the current CS, attaching one if DOS has
   just started it at XXXX:0100. NULL when CS holds no live attachment. */
static const MdAotProgram *md_aot_here(MdDos2System *sys)
{
    MdRuntime *rt = &sys->runtime;
    const uint16_t cs = rt->cpu.cs;
    size_t i;

    if (cs == sys->boot.dos_segment) {
        return sys->kernel_attached && sys->kernel_program->ready(rt, cs) ? sys->kernel_program : NULL;
    }
    if (cs == sys->boot.bios_segment) return NULL;
    /* fast path: the program we entered last, at the same segment */
    if (sys->aot_last_program != NULL && sys->aot_last_segment == cs &&
        sys->aot_last_program->ready(rt, cs)) {
        return sys->aot_last_program;
    }
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

MdStopReason md_dos2_system_run(MdDos2System *sys, uint64_t budget)
{
    MdRuntime *rt = &sys->runtime;
    const uint64_t start = rt->instructions;

    while (rt->stop_reason == MD_STOP_NONE) {
        const uint64_t used = rt->instructions - start;
        uint64_t left;

        if (used >= budget) break;
        left = budget - used;

        if (sys->cache != NULL) {
            /* Cache mode keeps the M14 shape: predicate-driven hand-off. */
            if (sys->aot_enabled && md_aot_stop(rt, sys)) {
                const MdAotProgram *prog = md_aot_candidate(sys);
                if (prog != NULL && prog->block_ok(rt, rt->cpu.cs, rt->cpu.ip)) {
                    const uint64_t before = rt->instructions;
                    const uint64_t before_aot = rt->aot_instructions;
                    const MdStopReason st =
                        prog->enter(rt, left < MD_DOS2_AOT_CHUNK ? left : MD_DOS2_AOT_CHUNK);
                    ++sys->aot_enters;
                    if (prog == sys->kernel_program) {
                        sys->kernel_aot_instructions += rt->aot_instructions - before_aot;
                    }
                    if (st != MD_STOP_NONE) break;
                    if (rt->instructions != before) continue;
                }
                (void)md_interp_step(rt);
                continue;
            }
            (void)md_interp_run_cached_until(rt, sys->cache, left,
                                             sys->aot_enabled ? md_aot_stop : NULL, sys);
            if (rt->stop_reason == MD_STOP_BUDGET) {
                rt->stop_reason = MD_STOP_NONE;       /* slice boundary, not a stop */
                break;
            }
            continue;
        }

        if (sys->aot_enabled) {
            /* M18 cheap re-entry: in a segment holding a live attachment just
               enter; enter() itself refuses when no compiled block can run at
               CS:IP (not an entry, invalid chunk), and only then is one
               instruction interpreted. No separate predicate/block_ok pass. */
            const MdAotProgram *prog = md_aot_here(sys);
            if (prog != NULL) {
                const uint64_t before = rt->instructions;
                const uint64_t before_aot = rt->aot_instructions;
                const uint16_t entered_cs = rt->cpu.cs;
                const MdStopReason st =
                    prog->enter(rt, left < MD_DOS2_AOT_CHUNK ? left : MD_DOS2_AOT_CHUNK);
                ++sys->aot_enters;
                if (prog == sys->kernel_program) {
                    sys->kernel_aot_instructions += rt->aot_instructions - before_aot;
                } else {
                    sys->aot_last_program = prog;
                    sys->aot_last_segment = entered_cs;
                }
                if (st != MD_STOP_NONE) break;
                if (rt->instructions == before) {
                    (void)md_interp_step(rt);
                    ++sys->attached_steps;
                }
                continue;
            }
        }

        /* M16: kernel, COMMAND.COM and non-compiled programs run on the
           threaded interpreter. Compiled code can only become reachable
           through a far transfer, INT or IRET, all of which change CS, so
           returning on a CS change loses no AOT entry. */
        (void)md_interp_run_until_cs_change(rt, left);
        if (rt->stop_reason == MD_STOP_BUDGET) {
            rt->stop_reason = MD_STOP_NONE;
            break;
        }
    }
    return rt->stop_reason;
}
