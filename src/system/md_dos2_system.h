#ifndef MICRODOS_DOS2_SYSTEM_H
#define MICRODOS_DOS2_SYSTEM_H

/* Platform-neutral MS-DOS 2.0 system loop (M14).
 *
 * Owns the boot contract, runs the guest through the decoded-block cache,
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
#include "msdos2_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdDos2System {
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdBlockCache *cache;                 /* optional; NULL = step interpreter */

    const MdAotProgram *const *aot_programs;
    size_t aot_program_count;
    bool aot_enabled;

    uint32_t aot_attaches;
    uint32_t aot_enters;
    uint16_t aot_last_segment;
    const MdAotProgram *aot_last_program;
} MdDos2System;

/* Zero-initialises the system, binds `memory` (1 MiB guest space, caller
   owned, e.g. PSRAM) and the optional block cache. Fill sys->boot.console,
   sys->boot.disk and clock fields afterwards, then call start. */
void md_dos2_system_init(MdDos2System *sys, uint8_t *memory, MdBlockCache *cache);

void md_dos2_system_set_aot(MdDos2System *sys, const MdAotProgram *const *programs,
                            size_t count, bool enabled);

/* Loads the released MSDOS.SYS image and prepares the SYSINIT contract.
   Returns false if the image is not the expected MS-DOS 2.0 kernel. */
bool md_dos2_system_start(MdDos2System *sys, const uint8_t *msdos_sys, size_t size);

/* Runs up to `budget` guest instructions. Returns MD_STOP_NONE when the
   slice was used up normally; any other value is a real stop. */
MdStopReason md_dos2_system_run(MdDos2System *sys, uint64_t budget);

#ifdef __cplusplus
}
#endif

#endif
