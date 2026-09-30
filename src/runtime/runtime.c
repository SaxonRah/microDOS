#include "microdos/runtime.h"
#include "microdos/ops.h"

#include <string.h>

static uint32_t md_next_epoch(uint32_t value)
{
    ++value;
    return value != 0u ? value : 1u;
}

static void md_runtime_bind_tracking(MdRuntime *runtime)
{
    runtime->cpu.code_page_generation = runtime->code_page_generation;
    runtime->cpu.code_page_executable = runtime->code_page_executable;
    runtime->cpu.code_write_epoch = &runtime->code_write_epoch;
    runtime->cpu.aot_guards = runtime->aot_slots;
    /* High-water mark of used slots: 0 until something attaches, so writes
       to executable pages (e.g. the DOS kernel stack) skip the guard walk. */
    runtime->cpu.aot_guard_count = 0u;
}

void md_runtime_reset(MdRuntime *runtime)
{
    uint8_t *memory = runtime->cpu.memory;
    MdHooks hooks = runtime->hooks;
    MdBlockCache *block_cache = runtime->block_cache;
    const uint32_t next_code_epoch = md_next_epoch(runtime->code_epoch);

    memset(runtime, 0, sizeof(*runtime));
    runtime->cpu.memory = memory;
    md_x86_set_flags(&runtime->cpu, MD_X86_FLAG_ALWAYS1);
    runtime->hooks = hooks;
    runtime->block_cache = block_cache;
    runtime->code_epoch = next_code_epoch;
    runtime->code_write_epoch = 1u;
    md_runtime_bind_tracking(runtime);
}

void md_runtime_init(MdRuntime *runtime, uint8_t *memory, const MdHooks *hooks)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->cpu.memory = memory;
    md_x86_set_flags(&runtime->cpu, MD_X86_FLAG_ALWAYS1);
    runtime->code_epoch = 1u;
    runtime->code_write_epoch = 1u;
    if (hooks != NULL) {
        runtime->hooks = *hooks;
    }
    md_runtime_bind_tracking(runtime);
}

void md_runtime_set_block_cache(MdRuntime *runtime, MdBlockCache *cache)
{
    runtime->block_cache = cache;
}

void md_runtime_load_raw(MdRuntime *runtime, const uint8_t *data, size_t size,
                         uint16_t segment, uint16_t offset)
{
    size_t i;
    size_t max_size = MD_X86_ADDRESS_SPACE;
    const uint32_t start = md_x86_linear(segment, offset);

    md_runtime_reset(runtime);
    if (size > max_size) size = max_size;
    for (i = 0; i < size; ++i) {
        md_x86_write8_linear(&runtime->cpu, start + (uint32_t)i, data[i]);
    }

    runtime->cpu.cs = segment;
    runtime->cpu.ip = offset;
    runtime->cpu.ss = segment;
    runtime->cpu.r[MD_X86_SP] = 0xFFFEu;
    md_runtime_mark_code_range(runtime, segment, offset, size);
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

    md_runtime_mark_code_range(runtime, segment, 0x0100u, size);

    runtime->cpu.cs = segment;
    runtime->cpu.ds = segment;
    runtime->cpu.es = segment;
    runtime->cpu.ss = segment;
    runtime->cpu.ip = 0x0100u;
    runtime->cpu.r[MD_X86_SP] = 0xFFFEu;
}

void md_runtime_invalidate_code(MdRuntime *runtime)
{
    runtime->code_epoch = md_next_epoch(runtime->code_epoch);
    runtime->code_write_epoch = md_next_epoch(runtime->code_write_epoch);
}

void md_runtime_mark_code_range(MdRuntime *runtime, uint16_t segment,
                                uint16_t offset, size_t size)
{
    uint32_t address = md_x86_linear(segment, offset);

    while (size != 0u) {
        const unsigned page = md_x86_code_page(address);
        const size_t in_page = (size_t)(address & MD_X86_CODE_PAGE_MASK);
        size_t chunk = (size_t)MD_X86_CODE_PAGE_SIZE - in_page;
        if (chunk > size) chunk = size;

        runtime->code_page_executable[page] = 1u;
        address = (address + (uint32_t)chunk) & MD_X86_ADDRESS_MASK;
        size -= chunk;
    }
}

int md_runtime_aot_find(const MdRuntime *runtime, const void *program, uint16_t segment)
{
    unsigned i;
    for (i = 0; i < MD_AOT_ATTACH_SLOTS; ++i) {
        const MdAotGuard *g = &runtime->aot_slots[i];
        if (g->in_use && g->program == program && g->segment == segment) return (int)i;
    }
    return -1;
}

void md_runtime_aot_touch(MdRuntime *runtime, int slot)
{
    if (slot >= 0 && (unsigned)slot < MD_AOT_ATTACH_SLOTS) {
        runtime->aot_slots[slot].last_use = ++runtime->aot_use_clock;
    }
}

MdAotGuard *md_runtime_aot_attach(MdRuntime *runtime, const void *program, uint16_t segment,
                                  uint32_t base, uint32_t size, const uint8_t *code_bits)
{
    int slot = md_runtime_aot_find(runtime, program, segment);
    MdAotGuard *g;
    unsigned i;

    if (slot < 0) {
        for (i = 0; i < MD_AOT_ATTACH_SLOTS; ++i) {
            if (!runtime->aot_slots[i].in_use) { slot = (int)i; break; }
        }
    }
    if (slot < 0) {
        slot = 0;
        for (i = 1; i < MD_AOT_ATTACH_SLOTS; ++i) {
            if (runtime->aot_slots[i].last_use < runtime->aot_slots[slot].last_use) slot = (int)i;
        }
        ++runtime->aot_evictions;
    }
    g = &runtime->aot_slots[slot];
    g->valid = 0u;                 /* never half-armed while fields change */
    g->base = base;
    g->size = size;
    g->code_bits = code_bits;
    g->program = program;
    g->segment = segment;
    g->in_use = 1u;
    g->last_use = ++runtime->aot_use_clock;
    g->epoch = 0u;
    memset(g->chunk_ok, 0xFF, sizeof(g->chunk_ok));
    g->live_chunks = 0u;
    if (size <= 0x10000u) {
        uint32_t c;
        const uint32_t bytes_per_chunk_map = (1u << MD_AOT_CHUNK_SHIFT) / 8u;   /* 8 */
        for (c = 0; c * (1u << MD_AOT_CHUNK_SHIFT) < size; ++c) {
            uint32_t k, any = 0u;
            for (k = 0; k < bytes_per_chunk_map; ++k) {
                const uint32_t idx = c * bytes_per_chunk_map + k;
                if (idx < (size + 7u) / 8u) any |= code_bits[idx];
            }
            if (any != 0u) ++g->live_chunks;
        }
    }
    g->valid = size <= 0x10000u && g->live_chunks != 0u;   /* chunk map covers <= 64 KiB */
    if ((unsigned)slot + 1u > runtime->cpu.aot_guard_count) {
        runtime->cpu.aot_guard_count = (unsigned)slot + 1u;
    }
    return g;
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
    md_x86_push(cpu, md_x86_flags(cpu));   /* the guest sees the full word */
    md_x86_push(cpu, cpu->cs);
    md_x86_push(cpu, cpu->ip);
    cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_IF | MD_X86_FLAG_TF);   /* never lazy */
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

/* ---- M17 out-of-line helpers for compact generated code ---------------- */

void md_aot_store8(MdX86 *cpu, uint16_t segment, uint16_t offset, uint8_t value)
{
    md_x86_write8(cpu, segment, offset, value);
}

void md_aot_store16(MdX86 *cpu, uint16_t segment, uint16_t offset, uint16_t value)
{
    md_x86_write16(cpu, segment, offset, value);
}

void md_aot_push(MdX86 *cpu, uint16_t value)
{
    md_x86_push(cpu, value);
}

void md_aot_push_reg(MdX86 *cpu, unsigned reg)
{
    md_x86_push_reg(cpu, reg);
}

uint8_t md_aot_alu8(MdX86 *cpu, unsigned operation, uint8_t lhs, uint8_t rhs)
{
    return md_x86_alu8(cpu, operation, lhs, rhs);
}

uint16_t md_aot_alu16(MdX86 *cpu, unsigned operation, uint16_t lhs, uint16_t rhs)
{
    return md_x86_alu16(cpu, operation, lhs, rhs);
}

uint8_t md_aot_shift8(MdX86 *cpu, unsigned operation, uint8_t value, unsigned count)
{
    return md_x86_shift8(cpu, operation, value, count);
}

uint16_t md_aot_shift16(MdX86 *cpu, unsigned operation, uint16_t value, unsigned count)
{
    return md_x86_shift16(cpu, operation, value, count);
}

uint8_t md_aot_incdec8(MdX86 *cpu, uint8_t value, int dec)
{
    return dec ? md_x86_dec8(cpu, value) : md_x86_inc8(cpu, value);
}

uint16_t md_aot_incdec16(MdX86 *cpu, uint16_t value, int dec)
{
    return dec ? md_x86_dec16(cpu, value) : md_x86_inc16(cpu, value);
}

int md_aot_condition(const MdX86 *cpu, unsigned cc)
{
    return md_x86_condition(cpu, cc);
}

int md_aot_chunks_ok_ol(const MdAotGuard *guard, uint32_t first, uint32_t last)
{
    return md_aot_chunks_ok(guard, first, last);
}
