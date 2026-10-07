#ifndef MICRODOS_NATIVE_V2G_H
#define MICRODOS_NATIVE_V2G_H

#include <stddef.h>
#include <stdint.h>

#include "microdos/native_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NV2-G G-1A: conservative vertical slice of the general natural-loop
 * compiler described by docs/NATIVE_V2G_DESIGN.md.
 *
 * Existing Native-v2 special compilers stay first in the runtime cascade.
 * G-1A is called only after they reject an interpreter-observed back-edge.
 *
 * Implemented in this slice:
 *   - one natural loop rooted at the observed back-edge target;
 *   - r11 scheduler-budget ABI, dynamic_retire = 3;
 *   - internal forward CFG edges and up to eight architectural exits;
 *   - all non-parity Jcc conditions when producer fusion is provable;
 *   - JCXZ, LOOP, LOOPZ and LOOPNZ;
 *   - general 8086 ModR/M byte/word loads with DS/SS default segments;
 *   - exact exit lazy-flag recipes for the supported producer subset.
 *
 * Still conservative / deferred exactly where proof is incomplete:
 *   - prefixes, stores, string instructions and PUSH/POP -> G-2;
 *   - ADC/SBB, CL shifts, CBW/CWD, XCHG -> G-3;
 *   - parity Jcc and exits requiring preserved INC/DEC carry are rejected;
 *   - a word-load wrap guard before this iteration's first flag producer is
 *     rejected until previous-iteration flag handoff is represented.
 */
MdNativeV2Status md_native_v2g_compile_loop(const uint8_t *image,
                                             size_t max_size,
                                             uint16_t entry_ip,
                                             MdNativeV2Code *out,
                                             size_t *guest_size_out);

/*
 * Execute a G-1A region with `budget` guest instructions available.
 * Returns unconsumed budget, or MD_NATIVE_V2_EXEC_FALLBACK before native entry
 * when the current runtime state cannot safely execute the region.
 */
uint32_t md_native_v2g_execute(MdX86 *cpu,
                               const MdNativeV2Code *code,
                               uint32_t budget);

/* True when `code` contains an NV2-G G-1A region. */
int md_native_v2g_is_code(const MdNativeV2Code *code);

#ifdef __cplusplus
}
#endif

#endif
