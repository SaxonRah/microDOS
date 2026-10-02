#ifndef MICRODOS_REGION_H
#define MICRODOS_REGION_H

#include <stdint.h>
#include "microdos/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M21 shared resident regions.
 *
 * These helpers execute a proven hot region against the canonical MdRuntime
 * state, but do not update runtime->instructions or any tier-specific
 * counters. The caller owns admission/accounting and must add the returned
 * retired-instruction count.
 *
 * M21.1c admits as many WHOLE region iterations as fit in `budget`. A return
 * value of zero means that not even one whole iteration was admitted and
 * guest state is unchanged. On a partial admission cpu->ip remains at the
 * region entry; on completion it advances to exit_ip. This lets cache/AOT
 * callers preserve exact guest-instruction budgets without dropping back to
 * one-instruction execution for an otherwise hot counted loop.
 */
uint32_t md_region_try_dec_jnz(MdRuntime *runtime, unsigned reg,
                               uint16_t entry_ip, uint16_t exit_ip,
                               uint32_t budget);

uint32_t md_region_try_lodsw_add_dx_ax_loop(MdRuntime *runtime,
                                            uint16_t entry_ip,
                                            uint16_t exit_ip,
                                            uint32_t budget);

#ifdef __cplusplus
}
#endif

#endif
