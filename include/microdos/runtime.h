#ifndef MICRODOS_RUNTIME_H
#define MICRODOS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/x86.h"

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

    /* code_epoch invalidates an entire external cache after image/reset events.
       Page generations handle normal self-modifying-code invalidation. */
    uint32_t code_epoch;
    uint32_t code_write_epoch;
    uint32_t code_page_generation[MD_X86_CODE_PAGE_COUNT];
    uint8_t code_page_executable[MD_X86_CODE_PAGE_COUNT];

    /* Optional caller-owned decoded cache. Generated AOT uses this when it must
       leave compiled code and execute an unknown/modified region. */
    MdBlockCache *block_cache;
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

/* Link a generated-image guard into the write-tracking path (idempotent). */
void md_runtime_register_aot_guard(MdRuntime *runtime, MdAotGuard *guard);
void md_runtime_unregister_aot_guard(MdRuntime *runtime, MdAotGuard *guard);

MdStopReason md_interp_step(MdRuntime *runtime);
MdStopReason md_interp_run(MdRuntime *runtime, uint64_t instruction_budget);
const char *md_stop_reason_name(MdStopReason reason);

#ifdef __cplusplus
}
#endif

#endif
