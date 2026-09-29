#include "microdos/runtime.h"

#include <string.h>

void md_runtime_reset(MdRuntime *runtime)
{
    uint8_t *memory = runtime->cpu.memory;
    MdHooks hooks = runtime->hooks;
    memset(runtime, 0, sizeof(*runtime));
    runtime->cpu.memory = memory;
    runtime->cpu.flags = MD_X86_FLAG_ALWAYS1;
    runtime->hooks = hooks;
}

void md_runtime_init(MdRuntime *runtime, uint8_t *memory, const MdHooks *hooks)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->cpu.memory = memory;
    runtime->cpu.flags = MD_X86_FLAG_ALWAYS1;
    if (hooks != NULL) {
        runtime->hooks = *hooks;
    }
}

void md_runtime_load_com(MdRuntime *runtime, const uint8_t *data, size_t size, uint16_t segment)
{
    size_t i;
    md_runtime_reset(runtime);

    /* A COM image begins at offset 0100h in a single segment. A minimal PSP-sized
       zero region is sufficient for the first validation programs; the real DOS
       PSP builder will replace this when EXEC lands. */
    for (i = 0; i < 0x100u; ++i) {
        md_x86_write8(&runtime->cpu, segment, (uint16_t)i, 0u);
    }

    if (size > 0xFF00u) {
        size = 0xFF00u;
    }
    for (i = 0; i < size; ++i) {
        md_x86_write8(&runtime->cpu, segment, (uint16_t)(0x0100u + i), data[i]);
    }

    runtime->cpu.cs = segment;
    runtime->cpu.ds = segment;
    runtime->cpu.es = segment;
    runtime->cpu.ss = segment;
    runtime->cpu.ip = 0x0100u;
    runtime->cpu.r[MD_X86_SP] = 0xFFFEu;
}

void md_runtime_request_exit(MdRuntime *runtime, uint8_t exit_code)
{
    runtime->exit_code = exit_code;
    runtime->stop_reason = MD_STOP_EXIT;
}

bool md_runtime_interrupt(MdRuntime *runtime, uint8_t vector)
{
    MdX86 *cpu = &runtime->cpu;
    const uint32_t ivt = (uint32_t)vector * 4u;

    if (runtime->hooks.interrupt != NULL &&
        runtime->hooks.interrupt(runtime, vector, runtime->hooks.user)) {
        return true;
    }

    /* Real 8086 INT semantics for anything the native layer does not claim. */
    md_x86_push(cpu, cpu->flags);
    md_x86_push(cpu, cpu->cs);
    md_x86_push(cpu, cpu->ip);
    cpu->flags &= (uint16_t)~(MD_X86_FLAG_IF | MD_X86_FLAG_TF);
    cpu->ip = md_x86_read16_linear(cpu, ivt);
    cpu->cs = md_x86_read16_linear(cpu, ivt + 2u);
    return false;
}

const char *md_stop_reason_name(MdStopReason reason)
{
    switch (reason) {
        case MD_STOP_NONE: return "none";
        case MD_STOP_EXIT: return "exit";
        case MD_STOP_HALT: return "halt";
        case MD_STOP_BUDGET: return "budget";
        case MD_STOP_FAULT: return "fault";
        default: return "unknown";
    }
}
