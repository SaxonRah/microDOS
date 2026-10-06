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
#ifndef MD_TR_HEAT_SLOTS
#define MD_TR_HEAT_SLOTS 512u        /* hotness counters (power of 2) */
#endif
#ifndef MD_TR_HOT_THRESHOLD
#define MD_TR_HOT_THRESHOLD 2u       /* back-edge / exit hits before translating */
#endif
#ifndef MD_TR_LIVE_PAGES
#define MD_TR_LIVE_PAGES 16u        /* pages tracked byte-exactly (512 B each) */
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
    uint32_t live_pages;            /* pages tracked byte-exactly */
    uint32_t live_fallback_pages;   /* pool exhausted: page-granular */
    uint32_t deferred_latches;      /* self-loops with deferred flag writes */
    uint32_t step_ops;              /* in-block interpreter steps translated */
    uint32_t backedge_exits;        /* interpreter returned at a loop head */
    uint32_t suppressed;            /* loop heads that cannot be translated */
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
    uint32_t inline_dispatch;       /* RET looks up blocks in generated code */
    /* 0 = tiered (default): cold code runs in the threaded interpreter and
       a block is translated once its start is hot. 1 = translate every
       block on first sight (differential testing). */
    uint32_t eager;
    uint8_t heat[MD_TR_HEAT_SLOTS];
    MdTrBlock blocks[MD_TR_SLOTS];
    /* M25 byte-exact SMC tracking: rt->cpu.tr_live_bits points here. */
    uint8_t *live_table[MD_X86_CODE_PAGE_COUNT];
    uint8_t live_pool[MD_TR_LIVE_PAGES][(1u << MD_X86_CODE_PAGE_SHIFT) / 8u];
    uint32_t live_used;
    MdTrStats stats;
} MdTranslator;

/* arena must be executable and at least 4 KiB. Returns 0 if the host
   cannot execute Thumb-2 (translation is then disabled). */
int md_tr_init(MdTranslator *tr, MdRuntime *rt, uint8_t *arena, uint32_t arena_size);

/* Drop every translation (e.g. after md_runtime_reset). */
void md_tr_flush(MdTranslator *tr);

/* Run up to `budget` guest instructions, mixing translated execution and
   the canonical interpreter. Same observable result as md_interp_run(rt,
   budget). Tiering needs the interpreter's back-edge exit
   (MICRODOS_ENABLE_BACKEDGE_EXIT or Native v2); without it the translator
   behaves as eager and steps untranslatable instructions one at a time. */
MdStopReason md_tr_run(MdTranslator *tr, uint64_t budget);

#ifdef __cplusplus
}
#endif

#endif
