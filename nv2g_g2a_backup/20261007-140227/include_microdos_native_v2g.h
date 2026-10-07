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

typedef struct MdNativeV2GStats {
    uint64_t attempts;
    uint64_t compiles;
    uint64_t reject_bad_argument;
    uint64_t reject_region;
    uint64_t reject_decode;
    uint64_t reject_control;
    uint64_t reject_opcode;
    uint64_t reject_cfg;
    uint64_t reject_flags;
    uint64_t reject_memory;
    uint64_t reject_exits;
    uint64_t reject_emit;

    /* G-1D exact first-reject profile. Counters are intentionally cumulative
       and read-only; they do not affect admission or generated code. */
    uint32_t reject_opcode_byte[256];
    uint32_t reject_control_opcode[256];
    uint32_t reject_prefix_byte[256];

    uint32_t reject_control_prefix;
    uint32_t reject_control_far;
    uint32_t reject_control_call;
    uint32_t reject_control_indirect_call;
    uint32_t reject_control_indirect_jump;
    uint32_t reject_control_return;
    uint32_t reject_control_stop;
} MdNativeV2GStats;

/* Cumulative since boot/process start. Read-only telemetry. */
const MdNativeV2GStats *md_native_v2g_stats(void);

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
