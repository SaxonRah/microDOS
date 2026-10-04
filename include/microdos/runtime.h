#ifndef MICRODOS_RUNTIME_H
#define MICRODOS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/x86.h"

#ifndef MICRODOS_TRANSLATION_SUPPORT
#define MICRODOS_TRANSLATION_SUPPORT 1
#endif

#ifndef MD_INTERP_OPCODE_PROFILE
#define MD_INTERP_OPCODE_PROFILE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdStopReason {
    MD_STOP_NONE = 0,
    MD_STOP_EXIT,
    MD_STOP_HALT,
    MD_STOP_BUDGET,
    MD_STOP_FAULT
} MdStopReason;

struct MdRuntime;
#ifndef MD_AOT_LIVE_PAGES
#define MD_AOT_LIVE_PAGES 8u    /* 4 KiB: DOS kernel (5 pages) + a compiled program; beyond that, pages fall back to the exact guard path */
#endif

typedef struct MdRuntime MdRuntime;
struct MdBlockCache;
typedef struct MdBlockCache MdBlockCache;

typedef bool (*MdInterruptHook)(MdRuntime *runtime, uint8_t vector, void *user);
typedef uint8_t (*MdPortIn8Hook)(MdRuntime *runtime, uint16_t port, void *user);
typedef void (*MdPortOut8Hook)(MdRuntime *runtime, uint16_t port, uint8_t value, void *user);

typedef struct MdHooks {
    MdInterruptHook interrupt;
    MdPortIn8Hook in8;
    MdPortOut8Hook out8;
    void *user;
} MdHooks;

struct MdRuntime {
    MdX86 cpu;
    MdHooks hooks;
    uint64_t instructions;
    /* Subset of `instructions` retired by generated AOT code. */
    uint64_t aot_instructions;
    MdStopReason stop_reason;
    uint32_t fault_linear;
    uint8_t fault_opcode;
    uint8_t exit_code;

#if MICRODOS_TRANSLATION_SUPPORT
    /* code_epoch invalidates an entire external cache after image/reset events.
       Page generations handle normal self-modifying-code invalidation. */
    uint32_t code_epoch;
    uint32_t code_write_epoch;
    uint32_t code_page_generation[MD_X86_CODE_PAGE_COUNT];
    uint8_t code_page_executable[MD_X86_CODE_PAGE_COUNT];

    /* Optional caller-owned decoded cache. Generated AOT uses this when it must
       leave compiled code and execute an unknown/modified region. */
    MdBlockCache *block_cache;

    /* Attachments of generated images (M15). Cleared by init/reset, so an
       attachment can never outlive the runtime it was made for. */
    MdAotGuard aot_slots[MD_AOT_ATTACH_SLOTS];
    uint8_t aot_page_owner[MD_X86_CODE_PAGE_COUNT];   /* M21.1b, see MdX86 */
    /* M21.1b live-code bitmaps: per page a pointer into a small pool (one
       bit per byte of a 4 KiB page); pages beyond the pool use an all-ones
       bitmap, which simply means "always take the guard path". */
    uint8_t *aot_live_bits[MD_X86_CODE_PAGE_COUNT];
    uint8_t aot_live_pool[MD_AOT_LIVE_PAGES][MD_X86_CODE_PAGE_SIZE / 8u];
    uint8_t aot_live_pool_used;
    uint32_t aot_use_clock;
    uint32_t aot_evictions;

#endif
};

void md_runtime_init(MdRuntime *runtime, uint8_t *memory, const MdHooks *hooks);
void md_runtime_reset(MdRuntime *runtime);
void md_runtime_load_com(MdRuntime *runtime, const uint8_t *data, size_t size, uint16_t segment);
void md_runtime_load_raw(MdRuntime *runtime, const uint8_t *data, size_t size,
                         uint16_t segment, uint16_t offset);
void md_runtime_request_exit(MdRuntime *runtime, uint8_t exit_code);
void md_runtime_set_block_cache(MdRuntime *runtime, MdBlockCache *cache);

/* Coarse invalidation remains available for image replacement and platform code
   that cannot identify the touched page. Normal guest writes invalidate only
   executable pages through the MdX86 write helpers. */
void md_runtime_invalidate_code(MdRuntime *runtime);

/* Mark bytes as executable so later guest writes to their 4 KiB pages invalidate
   decoded blocks and, conservatively, any generated AOT using the image. */
void md_runtime_mark_code_range(MdRuntime *runtime, uint16_t segment,
                                uint16_t offset, size_t size);

/* Returns true when a host/native hook handled the interrupt. If it did not,
   the function performs real-mode 8086 interrupt-vector dispatch through the IVT. */
bool md_runtime_interrupt(MdRuntime *runtime, uint8_t vector);

/* Generated-image attachments (see MdAotGuard). An attachment is keyed by
   (program identity, segment). attach() reuses that key's slot, else a free
   slot, else evicts the least recently used one (the evicted copy simply
   runs interpreted). It arms the guard valid. find() returns the slot index
   or -1; the slot may exist but be invalid after a code write. */
int md_runtime_aot_find(const MdRuntime *runtime, const void *program, uint16_t segment);
MdAotGuard *md_runtime_aot_attach(MdRuntime *runtime, const void *program, uint16_t segment,
                                  uint32_t base, uint32_t size, const uint8_t *code_bits);
/* Marks the slot most recently used (call when entering compiled code). */
void md_runtime_aot_touch(MdRuntime *runtime, int slot);

/* M17 out-of-line helpers for compact generated code (MD_AOT_COMPACT):
   one shared copy instead of the store/guard/flag logic inlined at every
   compiled instruction. Semantics are exactly the inline versions'. */
void md_aot_store8(MdX86 *cpu, uint16_t segment, uint16_t offset, uint8_t value);
void md_aot_store16(MdX86 *cpu, uint16_t segment, uint16_t offset, uint16_t value);
void md_aot_push(MdX86 *cpu, uint16_t value);
void md_aot_push_reg(MdX86 *cpu, unsigned reg);
uint8_t md_aot_alu8(MdX86 *cpu, unsigned operation, uint8_t lhs, uint8_t rhs);
uint16_t md_aot_alu16(MdX86 *cpu, unsigned operation, uint16_t lhs, uint16_t rhs);
uint8_t md_aot_shift8(MdX86 *cpu, unsigned operation, uint8_t value, unsigned count);
uint16_t md_aot_shift16(MdX86 *cpu, unsigned operation, uint16_t value, unsigned count);
uint8_t md_aot_incdec8(MdX86 *cpu, uint8_t value, int dec);     /* CF preserved */
uint16_t md_aot_incdec16(MdX86 *cpu, uint16_t value, int dec);  /* CF preserved */
int md_aot_condition(const MdX86 *cpu, unsigned cc);
int md_aot_chunks_ok_ol(const MdAotGuard *guard, uint32_t first, uint32_t last);

/* M18: interpreter semantics exported for compiled code, so the generator
   never re-implements them. `ext` is ModR/M.reg (4 MUL .. 7 IDIV); a divide
   fault stops the runtime exactly as the interpreter does. The string op
   takes the raw prefix bytes (0 = none; 26/2E/36/3E; F2/F3). */
void md_interp_muldiv(MdRuntime *runtime, uint8_t opcode, unsigned ext, uint16_t operand,
                      uint16_t ip_before);
void md_interp_string_op(MdRuntime *runtime, uint8_t opcode, uint8_t segment_prefix,
                         uint8_t repeat_prefix);

#if MD_INTERP_OPCODE_PROFILE
const uint32_t *md_interp_opcode_profile_counts(void);
const uint32_t *md_interp_unpref_modrm_profile_counts(void);
const uint32_t *md_interp_prefix_profile_counts(void);
const uint32_t *md_interp_hot_modrm_profile_counts(void);
void md_interp_opcode_profile_reset(void);
#endif
MdStopReason md_interp_step(MdRuntime *runtime);
MdStopReason md_interp_run(MdRuntime *runtime, uint64_t instruction_budget);
/* Like md_interp_run(), but also returns (with MD_STOP_NONE) as soon as CS
   changes: far jumps/calls/returns, INT and IRET. The DOS system loop uses it
   to run the kernel and non-compiled programs at threaded speed while still
   stopping wherever compiled code could take over (M16). */
MdStopReason md_interp_run_until_cs_change(MdRuntime *runtime, uint64_t instruction_budget);
const char *md_stop_reason_name(MdStopReason reason);

#ifdef __cplusplus
}
#endif

#endif
