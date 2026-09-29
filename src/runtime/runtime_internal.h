#ifndef MICRODOS_RUNTIME_INTERNAL_H
#define MICRODOS_RUNTIME_INTERNAL_H

#include <stdint.h>
#include "microdos/ops.h"
#include "microdos/runtime.h"

static inline uint8_t md_fetch8(MdRuntime *runtime)
{
    const uint8_t value = md_x86_read8(&runtime->cpu, runtime->cpu.cs, runtime->cpu.ip);
    runtime->cpu.ip = (uint16_t)(runtime->cpu.ip + 1u);
    return value;
}

static inline uint16_t md_fetch16(MdRuntime *runtime)
{
    const uint16_t value = md_x86_read16(&runtime->cpu, runtime->cpu.cs, runtime->cpu.ip);
    runtime->cpu.ip = (uint16_t)(runtime->cpu.ip + 2u);
    return value;
}

#endif
