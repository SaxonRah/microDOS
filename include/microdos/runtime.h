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
    MdStopReason stop_reason;
    uint32_t fault_linear;
    uint8_t fault_opcode;
    uint8_t exit_code;
    uint32_t code_epoch;
};

void md_runtime_init(MdRuntime *runtime, uint8_t *memory, const MdHooks *hooks);
void md_runtime_reset(MdRuntime *runtime);
void md_runtime_load_com(MdRuntime *runtime, const uint8_t *data, size_t size, uint16_t segment);
void md_runtime_request_exit(MdRuntime *runtime, uint8_t exit_code);
/* Coarse code-cache invalidation. Page-granular tracking will replace this later. */
void md_runtime_invalidate_code(MdRuntime *runtime);

/* Returns true when a host/native hook handled the interrupt. If it did not,
   the function performs real-mode 8086 interrupt-vector dispatch through the IVT. */
bool md_runtime_interrupt(MdRuntime *runtime, uint8_t vector);

MdStopReason md_interp_step(MdRuntime *runtime);
MdStopReason md_interp_run(MdRuntime *runtime, uint64_t instruction_budget);
const char *md_stop_reason_name(MdStopReason reason);

#ifdef __cplusplus
}
#endif

#endif
