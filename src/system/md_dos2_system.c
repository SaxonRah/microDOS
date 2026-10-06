#include "md_dos2_system.h"
#include "microdos/hot_code.h"

#ifdef MICRODOS_ENABLE_JIT
#include "microdos/jit.h"
#ifndef MICRODOS_SYSTEM_JIT_CODE_BYTES
#define MICRODOS_SYSTEM_JIT_CODE_BYTES (24u * 1024u)
#endif
#if defined(_MSC_VER)
__declspec(align(16)) static uint8_t g_md_system_jit_code[MICRODOS_SYSTEM_JIT_CODE_BYTES];
#else
static uint8_t g_md_system_jit_code[MICRODOS_SYSTEM_JIT_CODE_BYTES] __attribute__((aligned(16)));
#endif
static MdJit g_md_system_jit;
#endif

#include <string.h>

#ifndef MICRODOS_SYSTEM_ENABLE_TRANSLATOR
#define MICRODOS_SYSTEM_ENABLE_TRANSLATOR 0
#endif
#if MICRODOS_SYSTEM_ENABLE_TRANSLATOR
#include "microdos/translate.h"
#endif

#ifndef MICRODOS_SYSTEM_ENABLE_AOT
#define MICRODOS_SYSTEM_ENABLE_AOT 1
#endif
#ifndef MICRODOS_SYSTEM_ENABLE_CACHE
#define MICRODOS_SYSTEM_ENABLE_CACHE 1
#endif

#define MD_DOS2_KERNEL_SIZE 16690u
#define MD_DOS2_AOT_CHUNK 65536u

void md_dos2_system_init(MdDos2System *sys, uint8_t *memory, MdBlockCache *cache)
{
    MdHooks hooks;

    memset(sys, 0, sizeof(*sys));
    md_exec_router_init(&sys->router);
    md_msdos2_boot_init(&sys->boot);
    sys->boot.continue_after_dosinit = true;

    memset(&hooks, 0, sizeof(hooks));
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &sys->boot;
    md_runtime_init(&sys->runtime, memory, &hooks);
#ifdef MICRODOS_ENABLE_NATIVE_V2
    md_native_v2_runtime_init(&sys->native_v2);
#endif

#ifdef MICRODOS_ENABLE_JIT
    md_jit_init(&g_md_system_jit, g_md_system_jit_code, sizeof(g_md_system_jit_code));
    sys->jit = &g_md_system_jit;
#endif

#if MICRODOS_SYSTEM_ENABLE_CACHE
    sys->cache = cache;
    if (cache != NULL) {
        md_block_cache_init(cache);
        md_runtime_set_block_cache(&sys->runtime, cache);
    }
#else
    (void)cache;
    sys->cache = NULL;
#endif
}

void md_dos2_system_set_aot(MdDos2System *sys, const MdAotProgram *const *programs,
                            size_t count, bool enabled)
{
    sys->aot_programs = programs;
    sys->aot_program_count = enabled ? count : 0u;
    sys->aot_enabled = enabled && (count != 0u || sys->kernel_program != NULL);
    if (!enabled) sys->kernel_program = NULL;
}

void md_dos2_system_set_jit(MdDos2System *sys, MdJit *jit)
{
    if (sys != NULL) {
        sys->jit = jit;
        md_exec_router_init(&sys->router);
    }
}

bool md_dos2_system_start(MdDos2System *sys, const uint8_t *msdos_sys, size_t size)
{
    if (msdos_sys == NULL || size != MD_DOS2_KERNEL_SIZE ||
        msdos_sys[0] != 0xE9u || msdos_sys[1] != 0x78u || msdos_sys[2] != 0x3Eu) {
        return false;
    }
    md_exec_router_init(&sys->router);
#ifdef MICRODOS_ENABLE_NATIVE_V2
    md_native_v2_runtime_init(&sys->native_v2);
    sys->native_v2_instructions = 0u;
#endif
#ifdef MICRODOS_ENABLE_JIT
    if (sys->jit != NULL) md_jit_reset(sys->jit);
#endif
    md_msdos2_boot_prepare_cpu(&sys->runtime, &sys->boot, msdos_sys, size);
#if MICRODOS_SYSTEM_ENABLE_AOT
    if (sys->aot_enabled && sys->kernel_program != NULL) {
        sys->kernel_attached = sys->kernel_program->attach(&sys->runtime, sys->boot.dos_segment);
        if (sys->kernel_attached) ++sys->aot_attaches;
    }
#endif
    return true;
}

void md_dos2_system_set_kernel_aot(MdDos2System *sys, const MdAotProgram *kernel)
{
    sys->kernel_program = kernel;
    if (kernel != NULL) sys->aot_enabled = true;
}

/* Returns the program whose compiled code may run at the current CS:IP,
   attaching it first when DOS has just started it at XXXX:0100. */
#if MICRODOS_SYSTEM_ENABLE_AOT || MICRODOS_SYSTEM_ENABLE_CACHE
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

#endif

#if MICRODOS_SYSTEM_ENABLE_TRANSLATOR && defined(MICRODOS_ENABLE_NATIVE_V2)
/* M25 F: Native v2 gets each loop head first; M25 runs what it rejects. */
static int MD_EXEC_HOT_FUNC(md_dos2_nv2_loop_hook)(void *user, MdRuntime *rt, uint64_t budget)
{
    MdDos2System *sys = (MdDos2System *)user;
    MdNativeV2RunResult result;
    if (!md_native_v2_runtime_try_execute(&sys->native_v2, rt, budget, &result)) return 0;
    sys->native_v2_instructions += result.retired;
    if (result.rep_string_loop && result.rep_words != 0u) {
        const uint64_t outer = result.iterations;
        const uint64_t each = outer * (uint64_t)result.rep_words;
        const unsigned opidx[4] = { 5u, 1u, 3u, 9u }; /* STOSW MOVSW CMPSW SCASW */
        unsigned i;
        rt->rep_instructions += outer * 4u;
        rt->rep_elements += each * 4u;
        rt->rep_payload_bytes += each * 8u;            /* four 16-bit streams */
        rt->rep_memory_bytes += each * 12u;            /* 2 + 4 + 4 + 2 B */
        for (i = 0u; i < 4u; ++i) {
            rt->rep_op_instructions[opidx[i]] += outer;
            rt->rep_op_elements[opidx[i]] += each;
        }
    }
    return 1;
}
#endif

MdStopReason MD_EXEC_HOT_FUNC(md_dos2_system_run)(MdDos2System *sys, uint64_t budget)
{
    MdRuntime *rt = &sys->runtime;

#if MICRODOS_SYSTEM_ENABLE_TRANSLATOR
    if (sys->translator != NULL) {
#ifdef MICRODOS_ENABLE_NATIVE_V2
        sys->translator->loop_hook = md_dos2_nv2_loop_hook;
        sys->translator->loop_user = sys;
#endif
        /* Same contract as md_interp_run(): exact budget, same stop reasons. */
        const MdStopReason st = md_tr_run(sys->translator, budget);
        if (st == MD_STOP_BUDGET) {
            rt->stop_reason = MD_STOP_NONE;
            return MD_STOP_NONE;
        }
        return st;
    }
#endif

#ifdef MICRODOS_ENABLE_NATIVE_V2
    rt->native_v2_backedge_hit = 0u;
    rt->native_v2_suppress_bloom[0] = 0u;
    rt->native_v2_suppress_bloom[1] = 0u;
#endif

#if !MICRODOS_SYSTEM_ENABLE_AOT && !MICRODOS_SYSTEM_ENABLE_CACHE && !defined(MICRODOS_ENABLE_JIT) && !defined(MICRODOS_ENABLE_NATIVE_V2)
    MdStopReason st = md_interp_run(rt, budget);
    if (st == MD_STOP_BUDGET) {
        rt->stop_reason = MD_STOP_NONE;
        return MD_STOP_NONE;
    }
    return st;
#else
    const uint64_t start = rt->instructions;

    while (rt->stop_reason == MD_STOP_NONE) {
        const uint64_t used = rt->instructions - start;
        uint64_t left;

        if (used >= budget) break;
        left = budget - used;

#ifdef MICRODOS_ENABLE_NATIVE_V2
        if (rt->native_v2_backedge_hit) {
            const unsigned bit =
                ((unsigned)rt->native_v2_backedge_cs ^
                 (unsigned)rt->native_v2_backedge_ip) & 63u;
            const uint32_t mask = (uint32_t)1u << (bit & 31u);
            MdNativeV2RunResult nv2_result;

            rt->native_v2_backedge_hit = 0u;

            if (rt->cpu.cs != sys->boot.bios_segment &&
                md_native_v2_runtime_try_execute(
                    &sys->native_v2, rt, left, &nv2_result)) {
                sys->native_v2_instructions += nv2_result.retired;
                continue;
            }

            rt->native_v2_suppress_bloom[bit >> 5] |= mask;
        }
#endif

#if MICRODOS_SYSTEM_ENABLE_CACHE
        if (sys->cache != NULL) {
            /* Cache mode keeps the M14 shape: predicate-driven hand-off. */
            if (sys->aot_enabled && md_aot_stop(rt, sys)) {
                const MdAotProgram *prog = md_aot_candidate(sys);
                if (prog != NULL && prog->block_ok(rt, rt->cpu.cs, rt->cpu.ip)) {
                    const uint64_t before = rt->instructions;
                    const uint64_t before_aot = rt->aot_instructions;
                    const MdStopReason st =
                        prog->enter(rt, left < MD_DOS2_AOT_CHUNK ? left : MD_DOS2_AOT_CHUNK);
                    md_exec_router_record(&sys->router, MD_EXEC_TIER_AOT, rt->aot_instructions - before_aot);
                    md_exec_router_record(&sys->router, MD_EXEC_TIER_INTERP,
                                          rt->instructions - before - (rt->aot_instructions - before_aot));
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
                rt->stop_reason = MD_STOP_NONE;
                break;
            }
            continue;
        }
#endif

#if MICRODOS_SYSTEM_ENABLE_AOT
        if (sys->aot_enabled) {
            /* Static AOT remains the highest-priority native tier. */
            const MdAotProgram *prog = md_aot_here(sys);
            if (prog != NULL) {
                const uint64_t before = rt->instructions;
                const uint64_t before_aot = rt->aot_instructions;
                const uint16_t entered_cs = rt->cpu.cs;
                const MdStopReason st =
                    prog->enter(rt, left < MD_DOS2_AOT_CHUNK ? left : MD_DOS2_AOT_CHUNK);
                md_exec_router_record(&sys->router, MD_EXEC_TIER_AOT, rt->aot_instructions - before_aot);
                md_exec_router_record(&sys->router, MD_EXEC_TIER_INTERP,
                                      rt->instructions - before - (rt->aot_instructions - before_aot));
                ++sys->aot_enters;
                if (prog == sys->kernel_program) {
                    sys->kernel_aot_instructions += rt->aot_instructions - before_aot;
                } else {
                    sys->aot_last_program = prog;
                    sys->aot_last_segment = entered_cs;
                }
                if (st != MD_STOP_NONE) break;
                if (rt->instructions == before) {
                    /* Keep AOT hole/invalid-chunk behaviour conservative for
                       now; M20 JIT is for segments with no static image. */
                    (void)md_interp_step(rt);
                    md_exec_router_record(&sys->router, MD_EXEC_TIER_INTERP, 1u);
                    ++sys->attached_steps;
                }
                continue;
            }
        }
#endif

#ifdef MICRODOS_ENABLE_JIT
        if (sys->jit != NULL && rt->cpu.cs == sys->boot.bios_segment) {
            /* M20.2: the synthetic BIOS is a tiny INT/RETF trampoline layer,
               not application code. Profiling showed it consumed most JIT
               zero-progress sites and repeatedly polluted the 24 KiB arena.
               Keep it on the canonical threaded path until its CS changes. */
            const uint64_t before = rt->instructions;
            (void)md_interp_run_until_cs_change(rt, left);
            md_exec_router_record(&sys->router, MD_EXEC_TIER_INTERP, rt->instructions - before);
            sys->bios_interpreted_instructions += rt->instructions - before;
            sys->jit->bios_bypass_instructions += rt->instructions - before;
            if (rt->stop_reason == MD_STOP_BUDGET) {
                rt->stop_reason = MD_STOP_NONE;
                break;
            }
            if (rt->stop_reason != MD_STOP_NONE) break;
            continue;
        }

        if (sys->jit != NULL) {
            MdExecSite *site = md_exec_router_lookup(&sys->router, rt->cpu.cs, rt->cpu.ip);
            uint64_t before;
            uint64_t q;
            MdStopReason st;
            if (site->mode == MD_EXEC_JIT_REGION) {
                MdJitRunResult result;
                st = md_jit_run_region(sys->jit, rt, left, &result);
                sys->jit_instructions += result.retired;
                sys->jit_native_instructions += result.native;
                sys->jit_fallback_instructions += result.fallback;
                md_exec_router_record(&sys->router, MD_EXEC_TIER_JIT, result.retired);
                md_exec_router_jit_feedback(&sys->router, site, &result);
                if (st != MD_STOP_NONE) break;
                if (result.retired != 0u) continue;
                /* Refused budget/guard or invalidated code: canonical quantum,
                   never retry native at the unchanged IP without progress. */
            }
            q = left < MD_EXEC_INTERP_QUANTUM ? left : MD_EXEC_INTERP_QUANTUM;
            before = rt->instructions;
            st = md_interp_run_until_cs_change(rt, q);
            md_exec_router_record(&sys->router, MD_EXEC_TIER_INTERP, rt->instructions - before);
            if (st == MD_STOP_BUDGET) rt->stop_reason = MD_STOP_NONE;
            else if (st != MD_STOP_NONE) break;
            /* Sample only real quanta or CS boundaries, not arbitrary tiny
               caller slices. Profile/test slicing must not manufacture heat. */
            if (rt->instructions - before == MD_EXEC_INTERP_QUANTUM || st == MD_STOP_NONE) {
                site = md_exec_router_lookup(&sys->router, rt->cpu.cs, rt->cpu.ip);
                md_exec_router_sample(site);
#if MD_EXEC_ENABLE_PROMOTION
                if (rt->cpu.cs != sys->boot.bios_segment &&
#if MICRODOS_SYSTEM_ENABLE_AOT
                    !(sys->aot_enabled && md_aot_here(sys) != NULL) &&
#endif
                    md_exec_router_should_probe(site)) {
                    MdJitProbe probe;
                    if (md_jit_probe(sys->jit, rt, site->cs, site->ip, &probe) &&
                        md_exec_router_accept_probe(&probe, MD_EXEC_ENABLE_DIRECT != 0) &&
                        md_jit_prepare_region(sys->jit, rt))
                        md_exec_router_promote(&sys->router, site);
                    else md_exec_router_reject(&sys->router, site);
                }
#endif
            }
            continue;
        }
#endif

        {
            const uint64_t before = rt->instructions;
            const int was_bios = rt->cpu.cs == sys->boot.bios_segment;
            (void)md_interp_run_until_cs_change(rt, left);
            md_exec_router_record(&sys->router, MD_EXEC_TIER_INTERP,
                                  rt->instructions - before);
#ifdef MICRODOS_ENABLE_NATIVE_V2
            if (was_bios)
                sys->bios_interpreted_instructions +=
                    rt->instructions - before;
#endif
        }
        if (rt->stop_reason == MD_STOP_BUDGET) {
            rt->stop_reason = MD_STOP_NONE;
            break;
        }
    }
    return rt->stop_reason;
#endif
}
