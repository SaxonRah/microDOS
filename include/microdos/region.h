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
 * counters.  The caller owns admission/accounting and must add the returned
 * retired-instruction count.  A return value of zero means "not admitted";
 * guest state is unchanged.
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
