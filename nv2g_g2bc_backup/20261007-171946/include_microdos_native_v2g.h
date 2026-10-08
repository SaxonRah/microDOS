#ifndef MICRODOS_NATIVE_V2G_H
#define MICRODOS_NATIVE_V2G_H

#include <stddef.h>
#include <stdint.h>

#include "microdos/native_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NV2-G G-2A: measured coverage slice of the general natural-loop
 * compiler described by docs/NATIVE_V2G_DESIGN.md.
 *
 * Existing Native-v2 special compilers stay first in the runtime cascade.
 * NV2-G is called only after they reject an interpreter-observed back-edge.
 *
 * Implemented through G-2A:
 *   - one natural loop rooted at the observed back-edge target;
 *   - r11 scheduler-budget ABI, dynamic_retire = 3;
 *   - internal forward CFG edges and up to eight architectural exits;
 *   - all non-parity Jcc conditions when producer fusion is provable;
 *   - JCXZ, LOOP, LOOPZ and LOOPNZ;
 *   - general 8086 ModR/M byte/word loads with DS/SS default segments;
 *   - exact exit lazy-flag recipes for the supported producer subset;
 *   - guarded MOV byte/word stores (including A2/A3 moffs stores);
 *   - single LODSB/LODSW and STOSB/STOSW with a DF==0 entry guard;
 *   - PUSH/POP r16 with original-8086 PUSH SP semantics.
 *
 * G-2A store guards exit before the instruction on 16-bit offset wrap,
 * overlap with the currently executing guest-code span, or a tracked page.
 *
 * G-2B0: a guard ahead of the iteration's first flag producer is hoisted to
 * the loop header when its address registers are not written earlier in the
 * body (the normal DOS shape: store first, compare at the latch). A failure
 * there is the exact budget-exit state; on the first pass it retires nothing
 * and execute returns FALLBACK. Unhoistable guards remain a conservative
 * reject. JCXZ/LOOP side exits ahead of the first producer are rejected
 * unless provably first-iteration-only (JCXZ in a loop that never writes CX).
 *
 * G-2B1 (measured on MASM/SORT/FIND/CHKDSK with tests/nv2g_census.c):
 *   - native lazy materialization (NLM): a producer whose FLAGS cannot be
 *     rebuilt from registers writes the canonical MdX86 lazy state right
 *     after it executes; exits then preserve cpu FLAGS and fused branches
 *     read lazy_a/lazy_b back. Covers memory-operand producers (CMP r,[m]),
 *     overwritten producer operands, carried FLAGS for unhoistable guards
 *     and JCXZ/LOOP/JMP exits ahead of the iteration's first producer, and
 *     INC/DEC exits (CF source found through INC/DEC chains);
 *   - MOV r/m,imm (C6/C7) and external JMP side exits;
 *   - compact layout (shared exit epilogue, no inline tracked-page check)
 *     used only when the normal layout overflows the code buffer; execute
 *     refuses such code when a code-page tracker is present.
 *
 * Still deferred:
 *   - segment overrides and SCAS/CMPS single -> G-2B;
 *   - REP string forms stay with the existing special compiler;
 *   - ADC/SBB, CL shifts, CBW/CWD, XCHG -> G-3;
 *   - CALL/RET/INT/far/indirect control stays outside the general loop
 *     (the largest remaining census item: SORT's read loop);
 *   - different producers merging at a join (MASM 21A5:00E3);
 *   - parity Jcc, and INC/DEC exits whose incoming CF cannot be placed in
 *     cpu state (e.g. an INC/DEC latch producer with carried FLAGS).
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

    /* G-2B0: guards moved to the loop header (counted per compiled op,
       including attempts that later reject for another reason). */
    uint32_t hoisted_guards;
} MdNativeV2GStats;

/* Cumulative since boot/process start. Read-only telemetry. */
const MdNativeV2GStats *md_native_v2g_stats(void);

/*
 * Execute an NV2-G region with `budget` guest instructions available.
 * Returns unconsumed budget, or MD_NATIVE_V2_EXEC_FALLBACK before native entry
 * when the current runtime state cannot safely execute the region.
 */
uint32_t md_native_v2g_execute(MdX86 *cpu,
                               const MdNativeV2Code *code,
                               uint32_t budget);

/* True when `code` contains an NV2-G region. */
int md_native_v2g_is_code(const MdNativeV2Code *code);

#ifdef __cplusplus
}
#endif

#endif
