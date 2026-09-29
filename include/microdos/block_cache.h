#ifndef MICRODOS_BLOCK_CACHE_H
#define MICRODOS_BLOCK_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "microdos/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compact fixed-size decoded-block cache.
 *
 * The cache is caller-owned so a Pico target can place it deliberately in
 * internal SRAM or PSRAM. There is no allocation in the execution path.
 */
#ifndef MD_BLOCK_CACHE_SLOTS
#define MD_BLOCK_CACHE_SLOTS 128u
#endif
#ifndef MD_BLOCK_MAX_OPS
#define MD_BLOCK_MAX_OPS 16u
#endif

#if (MD_BLOCK_CACHE_SLOTS == 0u) || ((MD_BLOCK_CACHE_SLOTS & (MD_BLOCK_CACHE_SLOTS - 1u)) != 0u)
#error MD_BLOCK_CACHE_SLOTS must be a power of two
#endif

typedef enum MdDecodedKind {
    MD_DOP_INVALID = 0,
    MD_DOP_FALLBACK,
    MD_DOP_MOV_R8_IMM,
    MD_DOP_MOV_R16_IMM,
    MD_DOP_INC_R16,
    MD_DOP_DEC_R16,
    MD_DOP_DEC_JNZ,
    MD_DOP_PUSH_R16,
    MD_DOP_POP_R16,
    MD_DOP_ADD_AL_IMM,
    MD_DOP_ADD_AX_IMM,
    MD_DOP_SUB_AL_IMM,
    MD_DOP_SUB_AX_IMM,
    MD_DOP_CMP_AL_IMM,
    MD_DOP_CMP_AX_IMM,
    MD_DOP_MOV_AL_MOFFS,
    MD_DOP_MOV_AX_MOFFS,
    MD_DOP_MOV_MOFFS_AL,
    MD_DOP_MOV_MOFFS_AX,
    MD_DOP_JZ,
    MD_DOP_JNZ,
    MD_DOP_NOP,
    MD_DOP_INT,
    MD_DOP_CALL,
    MD_DOP_JMP,
    MD_DOP_RET,
    MD_DOP_HLT
} MdDecodedKind;

typedef struct MdDecodedOp {
    uint16_t next_ip;
    uint16_t arg;
    uint8_t kind;
    uint8_t reg;
    uint8_t opcode;
    uint8_t guest_count;
} MdDecodedOp;

typedef struct MdDecodedBlock {
    uint32_t epoch;
    uint32_t page_generation[2];
    uint16_t cs;
    uint16_t ip;
    uint8_t code_page[2];
    uint8_t page_count;
    uint8_t count;
    uint8_t fallback;
    uint8_t guest_count;
    uint8_t may_write;
    uint8_t reserved;
    MdDecodedOp ops[MD_BLOCK_MAX_OPS];
} MdDecodedBlock;

struct MdBlockCache {
    MdDecodedBlock slots[MD_BLOCK_CACHE_SLOTS];
    uint64_t hits;
    uint64_t misses;
    uint64_t decodes;
    uint64_t invalidations;
    uint64_t fallback_instructions;
};

typedef bool (*MdCacheStopPredicate)(const MdRuntime *runtime, void *user);

void md_block_cache_init(MdBlockCache *cache);
void md_block_cache_clear_stats(MdBlockCache *cache);

/*
 * Run through predecoded 8086 basic blocks. Unsupported instructions fall back
 * to the canonical single-instruction interpreter and immediately re-enter the
 * cache at the resulting CS:IP.
 */
MdStopReason md_interp_run_cached(MdRuntime *runtime, MdBlockCache *cache,
                                  uint64_t instruction_budget);

/* Hybrid/AOT entry point. Returns MD_STOP_NONE, without executing the current
   instruction, as soon as stop_predicate says the current CS:IP belongs to a
   caller-owned execution path. Otherwise behaves like md_interp_run_cached(). */
MdStopReason md_interp_run_cached_until(MdRuntime *runtime, MdBlockCache *cache,
                                        uint64_t instruction_budget,
                                        MdCacheStopPredicate stop_predicate,
                                        void *stop_user);

#ifdef __cplusplus
}
#endif

#endif
