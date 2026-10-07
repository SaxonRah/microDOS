#ifndef MICRODOS_NATIVE_V2_H
#define MICRODOS_NATIVE_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/x86.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MD_NATIVE_V2_CODE_BYTES 1024u
#define MD_NATIVE_V2_MAX_OPS 128u
#define MD_NATIVE_V2_EXEC_FALLBACK 0xFFFFFFFFu
#define MD_NATIVE_V2_EXEC_SIDE_EXIT 0xFFFFFFFEu

typedef enum MdNativeV2Status {
    MD_NATIVE_V2_OK = 0,
    MD_NATIVE_V2_BAD_ARGUMENT,
    MD_NATIVE_V2_DECODE_ERROR,
    MD_NATIVE_V2_UNSUPPORTED,
    MD_NATIVE_V2_TOO_LARGE,
    MD_NATIVE_V2_BRANCH_RANGE
} MdNativeV2Status;

typedef struct MdNativeV2Code {
    uint32_t _align_word;
    uint8_t bytes[MD_NATIVE_V2_CODE_BYTES];
    uint16_t size;
    uint16_t op_count;
    uint16_t start_ip;
    uint16_t end_ip;
    uint8_t has_local_loop;
    uint8_t phase;
    uint8_t needs_memory;
    uint8_t has_store;
    uint8_t requires_safe_ds_word;
    uint8_t exit_flags_reg;
    uint8_t needs_entry_cf;
    uint8_t cf_sites;
    uint8_t z_sites;

    /* Phase 3D production LOOP support. */
    uint8_t loop_terminal;       /* 0x75 = DEC/JNZ, 0xE2 = LOOP */
    uint8_t exit_lazy_op;        /* lazy flags reconstructed after LOOP */
    uint8_t exit_flag_dst;
    uint8_t exit_flag_src;       /* 0..7 register, 0xFF = immediate */
    uint16_t exit_flag_imm;
    uint8_t requires_df_clear;   /* LODSW fast path assumes forward strings */

    /*
     * Phase 3F store proof:
     * exactly one MOV [BX+SI],r16 per LOOP iteration, BX invariant,
     * SI advances by +2 exactly once, CX is the LOOP counter.
     */
    uint8_t safe_store_bx_si_loop;

    /*
     * M24.6 reusable byte-string store proof.
     * A generic STOSB instruction in a linear E2 counted loop may execute
     * natively only after runtime proves its exact ES:DI byte span is
     * forward, non-wrapping, disjoint from executing code, and untracked.
     */
    uint8_t safe_stosb_loop;

    /*
     * M24.6b generic counted-loop side exit.
     *
     * One forward JZ/JNZ/JB may leave an E2 counted loop before LOOP.
     * Full iterations still retire op_count operations; side_exit_ops is
     * the number of guest instructions executed in the partial exit
     * iteration, including the taken conditional branch.
     *
     * side_exit_flags:
     *   0 = none
     *   1 = CMP r8,r8
     *   2 = CMP r8,imm8
     *   3 = CMP r8,[DS mod=00]
     */
    uint16_t side_exit_target;
    uint8_t side_exit_ops;
    uint8_t side_exit_flags;
    uint8_t side_exit_dst;
    uint8_t side_exit_src;
    uint8_t side_exit_imm;

    /*
     * CFG retirement modes:
     *
     *   0: fixed retirement = iterations * op_count
     *   1: Phase-3G optional-op correction:
     *        iterations * retire_base_ops + returned_dynamic_ops
     *   2: Phase-3H full non-memory CFG counter:
     *        returned_dynamic_ops is the exact guest retirement count
     *        and retire_base_ops is zero.
     */
    uint8_t dynamic_retire;
    uint8_t retire_base_ops;

    /*
     * Phase 3I guarded unsigned MUL/DIV pair.
     * The compiler proves the multiplier/divisor source registers are
     * invariant across the counted loop. Runtime admits native execution
     * only when 0 < divisor and multiplier < divisor, which guarantees that
     * (AX * multiplier) / divisor always fits in the 16-bit quotient.
     */
    uint8_t requires_safe_muldiv;
    uint8_t muldiv_mul_reg;
    uint8_t muldiv_div_reg;

    /*
     * Exit flag reconstruction modes.
     * 0 = ordinary lazy producer reconstruction.
     * 1 = ADD AX,imm16 -> ROL AX,1 -> ... -> ROR reg,1 -> LOOP:
     *     SZAP come from the ADD; CF/OF come from the final ROR.
     */
    uint8_t exit_flags_mode;
    uint8_t exit_rot_reg;

    /*
     * Phase 3J FLAGS/stack loop.
     * Generated code keeps one exact materialized FLAGS word in r11 while
     * PUSHF/LAHF/SAHF/POPF execute natively through SS:SP.
     */
    uint8_t needs_entry_flags;
    uint8_t requires_safe_ss_word;
    uint8_t safe_stack_pushpop;

    /*
     * Scheduler chunking mode:
     *   0 = not chunkable
     *   1 = E2 LOOP; body does not read CX
     *   2 = terminal DEC r16 / JNZ header; body does not otherwise read or
     *       write the counter. Runtime repairs the real DEC lazy flags after
     *       each partial native chunk.
     */
    uint8_t chunkable_loop;

    /*
     * Phase 3M local CALL/RET graph lowering.
     *
     * The first production graph is a static near-CALL chain inside one CS
     * segment, ending in an E2 LOOP at the hot caller. Guest CALL/PUSH/POP/RET
     * stack effects are executed against SS:SP; no C helper is called from
     * the generated hot loop.
     *
     * guest_span_* lists every non-contiguous guest-code span whose bytes
     * must match before native re-entry. Span 0 is always the loop caller.
     */
    uint8_t local_call_graph;
    uint8_t call_stack_bytes;
    uint8_t guest_span_count;
    uint8_t guest_span_len[4];
    uint16_t guest_span_ip[4];

    /*
     * Phase 3P REP/string loop. The first production region is the
     * MDSTRESS Phase-3 outer loop containing REP STOSW/MOVSW/CMPSW/SCASW.
     * It executes the REP micro-loops directly in Thumb-2 while preserving
     * architectural CX/SI/DI and the final DEC/JNZ flags.
     *
     * Direct string stores are allowed only after runtime proves DS==ES,
     * forward DF, no 16-bit offset wrap, and no overlap between either
     * destination span and the currently executing compiled guest bytes.
     */
    uint8_t safe_rep_string_loop;
    uint8_t requires_ds_eq_es;
    uint16_t rep_src_off;
    uint16_t rep_dst_off;
    uint16_t rep_words;
} MdNativeV2Code;

MdNativeV2Status md_native_v2_compile_8086(const uint8_t *image,
                                           size_t image_size,
                                           uint16_t image_base,
                                           uint16_t entry_ip,
                                           MdNativeV2Code *out);

/*
 * Production admission helper for Phase 3A.
 *
 * `image` points at guest CS:entry_ip. The helper accepts exact counted
 * loops ending in either:
 *
 *     dec counter          or        loop entry
 *     jnz entry
 *
 * For LOOP, CX may not be written in the body. For DEC/JNZ, the selected
 * counter register may not be written anywhere except the terminating DEC.
 */
MdNativeV2Status md_native_v2_compile_counted_loop(const uint8_t *image,
                                                   size_t max_size,
                                                   uint16_t entry_ip,
                                                   MdNativeV2Code *out,
                                                   size_t *guest_size_out,
                                                   uint8_t *counter_reg_out);

/*
 * Phase 3M: compile a bounded static local near-CALL/RET graph rooted at a
 * counted loop. `memory` is the complete 1 MiB guest physical address space;
 * code is read with normal 8086 CS:IP 20-bit wrapping rules.
 */
MdNativeV2Status md_native_v2_compile_local_call_loop(const uint8_t *memory,
                                                       uint16_t cs,
                                                       uint16_t entry_ip,
                                                       MdNativeV2Code *out,
                                                       uint8_t *counter_reg_out);

/*
 * Phase 3P: compile the measured REP/string outer loop. The matcher accepts
 * the semantic MDSTRESS shape and records the source/destination/count so the
 * runtime can prove direct-store safety before native entry.
 */
MdNativeV2Status md_native_v2_compile_rep_string_loop(const uint8_t *memory,
                                                       uint16_t cs,
                                                       uint16_t entry_ip,
                                                       MdNativeV2Code *out,
                                                       uint8_t *counter_reg_out);

/*
 * Phase 3N admission bridge. Native-v2 admission is driven by observed
 * backward-edge headers. MDSTRESS Phase 4 presents the outer header
 *
 *     XOR CX,CX
 *     CALL proc_a
 *     LOOP CALL
 *
 * even though the compilable counted graph begins two bytes later at CALL.
 * Return that real graph entry plus the one architecturally consumed prelude
 * instruction. The caller must not consume the prelude unless native entry
 * actually succeeds.
 */
bool md_native_v2_find_local_call_loop_entry(const uint8_t *memory,
                                              uint16_t cs,
                                              uint16_t observed_ip,
                                              uint16_t *entry_ip_out,
                                              uint8_t *prefix_ops_out,
                                              uint8_t *zero_counter_out);

bool md_native_v2_available(void);

/*
 * Returns MD_NATIVE_V2_EXEC_FALLBACK when the region must not execute
 * natively under the current runtime state.
 */
uint32_t md_native_v2_execute(MdX86 *cpu, const MdNativeV2Code *code);

const char *md_native_v2_status_name(MdNativeV2Status status);

#ifdef __cplusplus
}
#endif

#endif
