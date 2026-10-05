#ifndef MICRODOS_TRANSLATE_H
#define MICRODOS_TRANSLATE_H

/*
 * M25 general 8086 -> Thumb-2 translator.
 *
 * Translated blocks keep AX..DI resident in r4..r11 for a whole native
 * episode, chain block-to-block through patched exit stubs, and leave
 * translated execution only for budget, self-modifying code, indirect
 * control flow, or an instruction the translator does not lower. Anything
 * the translator does not lower is executed by the canonical interpreter
 * (md_interp_step), so translation never rejects a site outright.
 *
 * Execution requires a Thumb-2 host (RP2350 / Cortex-M33, or any ARMv7
 * Thumb-2 target such as qemu-arm). Elsewhere md_tr_run() simply runs the
 * interpreter, so the module always links.
 */

#include <stdint.h>

#include "microdos/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MD_TR_SLOTS
#define MD_TR_SLOTS 256u            /* direct-mapped block table (power of 2) */
#endif
#ifndef MD_TR_MAX_OPS
#define MD_TR_MAX_OPS 32u
#endif

typedef struct MdTrBlock {
    uint32_t entry;                 /* arena offset of guard entry */
    uint32_t body;                  /* after the page-generation checks */
    uint32_t end;                   /* end of the block's code (incl. stubs) */
    uint32_t gen[2];                /* code-page generations at translation */
    uint16_t cs;
    uint16_t ip;
    uint8_t page[2];
    uint8_t page_count;
    uint8_t state;                  /* 0 empty, 1 translated, 2 not translatable */
    uint8_t ops;                    /* guest instructions in the block */
    uint8_t _pad;
} MdTrBlock;

typedef struct MdTrStats {
    uint64_t native_instructions;   /* retired inside translated code */
    uint64_t interp_instructions;   /* retired by md_interp_step fallback */
    uint32_t episodes;              /* native entries from C */
    uint32_t translations;
    uint32_t untranslatable;
    uint32_t chains;                /* exit stubs patched to direct branches */
    uint32_t exit_edge, exit_dynamic, exit_budget, exit_invalid, exit_store;
    uint32_t flushes;
    uint32_t code_bytes;
} MdTrStats;

typedef struct MdTranslator {
    MdRuntime *rt;
    uint8_t *arena;                 /* executable, written in place */
    uint32_t arena_size;
    uint32_t arena_used;
    uint32_t enter_off;             /* entry trampoline */
    uint32_t exit_off;              /* common exit */
    uint32_t mem_aligned;           /* guest RAM aligned to its own size */
    uint32_t epoch;                 /* rt->code_epoch seen at last flush */
    volatile uint32_t remaining;    /* written by the common exit */
    MdTrBlock blocks[MD_TR_SLOTS];
    MdTrStats stats;
} MdTranslator;

/* arena must be executable and at least 4 KiB. Returns 0 if the host
   cannot execute Thumb-2 (translation is then disabled). */
int md_tr_init(MdTranslator *tr, MdRuntime *rt, uint8_t *arena, uint32_t arena_size);

/* Drop every translation (e.g. after md_runtime_reset). */
void md_tr_flush(MdTranslator *tr);

/* Run up to `budget` guest instructions, mixing translated execution and
   md_interp_step(). Same observable result as md_interp_run(rt, budget). */
MdStopReason md_tr_run(MdTranslator *tr, uint64_t budget);

#ifdef __cplusplus
}
#endif

#endif
