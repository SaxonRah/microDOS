#ifndef MICRODOS_JIT_H
#define MICRODOS_JIT_H

#include <stddef.h>
#include <stdint.h>

#include "microdos/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M20.2 runtime-native translation.
 *
 * M19.0 proved that the RP2350 can discover 8086 blocks, emit Thumb into
 * executable SRAM and run them correctly, but its per-instruction BLX into C
 * helpers was slower than the threaded interpreter.
 *
 * M19.2 keeps M19.1 direct Thumb/chaining and adds resident native regions:
 * hot guest registers stay in ARM registers across loop iterations, x86 lazy
 * flag state is deferred until region exit, and exact-budget guards move out
 * of the hot backedge. The existing M19.1 lowering remains the safe fallback.
 *
 * The hot path therefore has:
 *
 *   - simple 8086 operations are lowered directly to Thumb-2;
 *   - guest register/flag state is updated directly in MdRuntime;
 *   - backward direct edges whose target is already inside the translated
 *     region become native Thumb branches;
 *   - unsupported instructions exit at their exact CS:IP and execute through
 *     the canonical interpreter;
 *   - memory stores use an inlined "page executable?" fast path and only call
 *     C when a store can invalidate translated/AOT code.
 *
 * M20 keeps the proven loop.com/memloop.com specializations and adds a generic
 * counted-loop region compiler. M20.1 extends discovery through bounded forward
 * conditional edges and can emit a multi-block resident CFG. M20.2 uses the first
 * real COMMAND.COM profile to add native control-transfer helpers, a small hotness
 * gate for the full DOS build, BIOS-tier isolation, and source->destination transfer
 * profiling so the 24 KiB code arena is spent on application hot code instead of
 * synthetic BIOS/boundary churn. Unsupported shapes still use the canonical path.
 */

#ifndef MD_JIT_BLOCK_SLOTS
#define MD_JIT_BLOCK_SLOTS 128u
#endif

#ifndef MD_JIT_MAX_OPS
#define MD_JIT_MAX_OPS 24u
#endif

#ifndef MD_JIT_DECODE_WINDOW
#define MD_JIT_DECODE_WINDOW 192u
#endif

#ifndef MD_JIT_HOT_SITES
#define MD_JIT_HOT_SITES 8u
#endif

#ifndef MD_JIT_HOTNESS_SLOTS
#define MD_JIT_HOTNESS_SLOTS 64u
#endif

#ifndef MD_JIT_HOT_THRESHOLD
#define MD_JIT_HOT_THRESHOLD 1u
#endif

typedef enum MdJitExitReason {
    MD_JIT_EXIT_NONE = 0,
    MD_JIT_EXIT_COMPILE_FAIL,
    MD_JIT_EXIT_BUDGET_FALLBACK,
    MD_JIT_EXIT_ZERO_PROGRESS,
    MD_JIT_EXIT_COLD_FALLBACK,
    MD_JIT_EXIT_NATIVE_RETURN,
    MD_JIT_EXIT_CS_CHANGE,
    MD_JIT_EXIT_STOP,
    MD_JIT_EXIT_REASON_COUNT
} MdJitExitReason;

typedef struct MdJitHotSite {
    uint64_t count;
    uint16_t cs;
    uint16_t ip;
    uint16_t dst_cs;
    uint16_t dst_ip;
    uint8_t opcode;
    uint8_t reason;
} MdJitHotSite;

typedef struct MdJitHotness {
    uint16_t cs;
    uint16_t ip;
    uint8_t count;
    uint8_t valid;
} MdJitHotness;

typedef enum MdJitOpKind {
    MD_JIT_OP_FALLBACK = 0,
    MD_JIT_OP_MOV_R8_IMM,
    MD_JIT_OP_MOV_R16_IMM,
    MD_JIT_OP_INC_R16,
    MD_JIT_OP_DEC_R16,
    MD_JIT_OP_ALU_ACC_IMM,
    MD_JIT_OP_GRP1_R16_IMM,
    MD_JIT_OP_MOV_AL_SI,
    MD_JIT_OP_MOV_SI_AL,
    MD_JIT_OP_JCC,
    MD_JIT_OP_JMP,
    MD_JIT_OP_CALL_NEAR,
    MD_JIT_OP_RET_NEAR,
    MD_JIT_OP_RET_NEAR_IMM,
    MD_JIT_OP_RET_FAR,
    MD_JIT_OP_RET_FAR_IMM,
    MD_JIT_OP_INT,
    MD_JIT_OP_IRET,
    MD_JIT_OP_NOP,
    MD_JIT_OP_HLT
} MdJitOpKind;

typedef struct MdJitOp {
    uint16_t ip;
    uint16_t next_ip;
    uint16_t target;
    uint16_t imm;
    uint8_t opcode;
    uint8_t kind;
    uint8_t reg;
    uint8_t aux;
} MdJitOp;

struct MdJit;
typedef struct MdJit MdJit;
struct MdJitBlock;
typedef struct MdJitBlock MdJitBlock;

typedef void (*MdJitMaterializeFn)(MdRuntime *runtime);
typedef void (*MdJitStore8SlowFn)(MdRuntime *runtime, uint32_t linear, uint8_t value);
typedef void (*MdJitExecOneFn)(MdRuntime *runtime, MdJitBlock *block, unsigned index);
typedef uint32_t (*MdJitNativeFn)(MdRuntime *runtime, MdJitBlock *block, uint32_t budget);

struct MdJitBlock {
    /* These function pointers intentionally lead the structure. Generated
       Thumb can load them with compact [r5,#imm] instructions. */
    MdJitMaterializeFn materialize;
    MdJitStore8SlowFn store8_slow;
    MdJitExecOneFn exec_one;       /* host/reference path + unsupported ops */
    MdJitNativeFn native;
    MdJit *owner;

    uint32_t code_epoch;
    uint32_t page_gen0;
    uint32_t page_gen1;
    uint32_t native_offset;

    uint16_t cs;
    uint16_t ip;
    uint16_t end_ip;
    uint16_t source_bytes;

    uint8_t page0;
    uint8_t page1;
    uint8_t page_count;
    uint8_t op_count;
    uint8_t valid;
    uint8_t direct_prefix_ops;
    uint8_t local_edges;
    uint8_t resident;
    uint8_t generic_region;
    uint8_t cfg_region;

    MdJitOp ops[MD_JIT_MAX_OPS];
};

struct MdJit {
    uint8_t *code;
    size_t code_size;
    size_t code_used;

    MdJitBlock blocks[MD_JIT_BLOCK_SLOTS];

    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t compiles;
    uint64_t native_entries;
    uint64_t direct_instructions;
    uint64_t fallback_instructions;
    uint64_t invalidations;
    uint64_t flushes;
    uint64_t boundary_fallbacks;

    /* Compile-time/native-shape counters. local_edges counts translated direct
       edges that stay inside one native region; helper_sites is the number of
       emitted slow/helper call sites, not how often they execute. */
    uint64_t local_edges;
    uint64_t helper_sites;

    /* M19.2 resident-region counters. resident_regions is compile-time shape;
       resident_entries/instructions are dynamic execution counters. */
    uint64_t resident_regions;
    uint64_t resident_entries;
    uint64_t resident_instructions;

    /* M20 generic counted-loop regions (subset of resident_*). */
    uint64_t generic_regions;
    uint64_t generic_entries;
    uint64_t generic_instructions;

    /* M20.1 bounded-CFG resident regions. */
    uint64_t cfg_regions;
    uint64_t cfg_entries;
    uint64_t cfg_instructions;
    uint64_t cfg_internal_edges;

    /* Dynamic native/JIT exit observability. */
    uint64_t native_returns;
    uint64_t cs_change_exits;
    uint64_t stop_exits;
    uint64_t compile_fail_fallbacks;
    uint64_t budget_fallbacks;
    uint64_t zero_progress_fallbacks;
    uint64_t cold_fallbacks;
    uint64_t control_instructions;
    uint64_t bios_bypass_instructions;
    uint64_t exit_reason[MD_JIT_EXIT_REASON_COUNT];
    MdJitHotSite hot_sites[MD_JIT_HOT_SITES];
    MdJitHotness hotness[MD_JIT_HOTNESS_SLOTS];
    uint8_t last_lookup_cold;
};

void md_jit_init(MdJit *jit, void *code, size_t code_size);
void md_jit_reset(MdJit *jit);

MdStopReason md_jit_run(MdJit *jit, MdRuntime *runtime, uint64_t instruction_budget);
MdStopReason md_jit_run_until_cs_change(MdJit *jit, MdRuntime *runtime,
                                         uint64_t instruction_budget);
const char *md_jit_exit_reason_name(unsigned reason);

#ifdef __cplusplus
}
#endif

#endif
