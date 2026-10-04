#ifndef MICRODOS_DOS2_SYSTEM_H
#define MICRODOS_DOS2_SYSTEM_H

/* Platform-neutral MS-DOS 2.0 system loop (M14 -> M20.2).
 *
 * Owns the boot contract, runs the guest through the selected execution tier,
 * and attaches/enters dosrecomp-generated programs when DOS starts them.
 * Hosts supply only memory, console/disk/clock callbacks, and time slices.
 * Used by the Pico 2 target and by the desktop end-to-end test.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/aot.h"
#include "microdos/block_cache.h"
#include "microdos/runtime.h"
#include "microdos/exec_router.h"
#ifdef MICRODOS_ENABLE_NATIVE_V2
#include "microdos/native_v2_runtime.h"
#endif
#include "msdos2_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

struct MdJit;
typedef struct MdJit MdJit;

typedef struct MdDos2System {
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdBlockCache *cache;                 /* optional decoded cache */

    const MdAotProgram *const *aot_programs;   /* .COM programs DOS may EXEC */
    size_t aot_program_count;
    bool aot_enabled;

    /* M17: the compiled kernel image (MSDOS.SYS at dos_segment:0000). */
    const MdAotProgram *kernel_program;
    bool kernel_attached;

    /* M20: optional runtime native translator.  Static AOT always wins.  The router
       interprets unknown code first and admits only proven hot regions. */
    MdJit *jit;
    MdExecRouter router;
#ifdef MICRODOS_ENABLE_NATIVE_V2
    MdNativeV2Runtime native_v2;
    uint64_t native_v2_instructions;
#endif

    uint32_t aot_attaches;
    uint32_t aot_enters;
    uint16_t aot_last_segment;
    const MdAotProgram *aot_last_program;

    uint64_t kernel_aot_instructions;   /* compiled kernel instructions */
    uint64_t attached_steps;            /* interpreter steps inside attached
                                           AOT segments (holes/invalid chunks) */
    uint64_t jit_instructions;          /* all guest instructions while JIT owns execution */
    uint64_t jit_native_instructions;   /* subset retired by generated native code */
    uint64_t jit_fallback_instructions; /* subset interpreted inside the JIT tier */
    uint64_t bios_interpreted_instructions; /* M20.2: synthetic BIOS bypasses JIT */
} MdDos2System;

/* Zero-initialises the system, binds `memory` (1 MiB guest space, caller
   owned, e.g. PSRAM) and the optional block cache. Fill sys->boot.console,
   sys->boot.disk and clock fields afterwards, then call start. */
void md_dos2_system_init(MdDos2System *sys, uint8_t *memory, MdBlockCache *cache);

void md_dos2_system_set_aot(MdDos2System *sys, const MdAotProgram *const *programs,
                            size_t count, bool enabled);

/* Optional M20 runtime translator. The caller owns both MdJit and its
   executable code arena. Safe to leave NULL; host/M18 behaviour is unchanged. */
void md_dos2_system_set_jit(MdDos2System *sys, MdJit *jit);

/* Loads the released MSDOS.SYS image and prepares the SYSINIT contract.
   Returns false if the image is not the expected MS-DOS 2.0 kernel. */
bool md_dos2_system_start(MdDos2System *sys, const uint8_t *msdos_sys, size_t size);

/* Optional compiled MSDOS.SYS. Call before md_dos2_system_start(); it is
   attached right after the kernel image is loaded (M17). */
void md_dos2_system_set_kernel_aot(MdDos2System *sys, const MdAotProgram *kernel);

/* Runs up to `budget` guest instructions. Returns MD_STOP_NONE when the
   slice was used up normally; any other value is a real stop. */
MdStopReason md_dos2_system_run(MdDos2System *sys, uint64_t budget);

#ifdef __cplusplus
}
#endif

#endif
