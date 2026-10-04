#include "microdos/runtime.h"
#include "microdos/ops.h"

#include <string.h>

#if MICRODOS_TRANSLATION_SUPPORT
static uint32_t md_next_epoch(uint32_t value)
{
    ++value;
    return value != 0u ? value : 1u;
}
#endif

static void md_runtime_bind_tracking(MdRuntime *runtime)
{
#if MICRODOS_TRANSLATION_SUPPORT
    runtime->cpu.code_page_generation = runtime->code_page_generation;
    runtime->cpu.code_page_executable = runtime->code_page_executable;
    runtime->cpu.code_write_epoch = &runtime->code_write_epoch;
    runtime->cpu.aot_guards = runtime->aot_slots;
    memset(runtime->aot_page_owner, 0, sizeof(runtime->aot_page_owner));
    runtime->cpu.aot_page_owner = runtime->aot_page_owner;
    memset(runtime->aot_live_bits, 0, sizeof(runtime->aot_live_bits));
    runtime->aot_live_pool_used = 0u;
    runtime->cpu.aot_live_bits = runtime->aot_live_bits;
    runtime->cpu.aot_guard_count = 0u;
#else
    runtime->cpu.code_page_generation = NULL;
    runtime->cpu.code_page_executable = NULL;
    runtime->cpu.code_write_epoch = NULL;
    runtime->cpu.aot_guards = NULL;
    runtime->cpu.aot_guard_count = 0u;
    runtime->cpu.aot_page_owner = NULL;
    runtime->cpu.aot_live_bits = NULL;
#endif
}


void md_runtime_reset(MdRuntime *runtime)
{
    uint8_t *memory = runtime->cpu.memory;
    MdHooks hooks = runtime->hooks;
#if MICRODOS_TRANSLATION_SUPPORT
    MdBlockCache *block_cache = runtime->block_cache;
    const uint32_t next_code_epoch = md_next_epoch(runtime->code_epoch);
#endif

    memset(runtime, 0, sizeof(*runtime));
    runtime->cpu.memory = memory;
    md_x86_set_flags(&runtime->cpu, MD_X86_FLAG_ALWAYS1);
    runtime->hooks = hooks;
#if MICRODOS_TRANSLATION_SUPPORT
    runtime->block_cache = block_cache;
    runtime->code_epoch = next_code_epoch;
    runtime->code_write_epoch = 1u;
#endif
    md_runtime_bind_tracking(runtime);
}


void md_runtime_init(MdRuntime *runtime, uint8_t *memory, const MdHooks *hooks)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->cpu.memory = memory;
    md_x86_set_flags(&runtime->cpu, MD_X86_FLAG_ALWAYS1);
#if MICRODOS_TRANSLATION_SUPPORT
    runtime->code_epoch = 1u;
    runtime->code_write_epoch = 1u;
#endif
    if (hooks != NULL) {
        runtime->hooks = *hooks;
    }
    md_runtime_bind_tracking(runtime);
}


void md_runtime_set_block_cache(MdRuntime *runtime, MdBlockCache *cache)
{
#if MICRODOS_TRANSLATION_SUPPORT
    runtime->block_cache = cache;
#else
    (void)runtime;
    (void)cache;
#endif
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
    /* M21.1b: no TRANSLATED marking here. Page generations exist for the
       decoded-block cache and the JIT, which mark exactly the pages they
       translate; compiled images mark their own pages AOT on attach. */
    (void)size;
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

    /* M21.1b: see md_runtime_load_raw (no TRANSLATED marking by loaders). */

    runtime->cpu.cs = segment;
    runtime->cpu.ds = segment;
    runtime->cpu.es = segment;
    runtime->cpu.ss = segment;
    runtime->cpu.ip = 0x0100u;
    runtime->cpu.r[MD_X86_SP] = 0xFFFEu;
}

void md_runtime_invalidate_code(MdRuntime *runtime)
{
#if MICRODOS_TRANSLATION_SUPPORT
    runtime->code_epoch = md_next_epoch(runtime->code_epoch);
    runtime->code_write_epoch = md_next_epoch(runtime->code_write_epoch);
#else
    (void)runtime;
#endif
}


void md_runtime_mark_code_range(MdRuntime *runtime, uint16_t segment,
                                uint16_t offset, size_t size)
{
#if MICRODOS_TRANSLATION_SUPPORT
    uint32_t address = md_x86_linear(segment, offset);

    while (size != 0u) {
        const unsigned page = md_x86_code_page(address);
        const size_t in_page = (size_t)(address & MD_X86_CODE_PAGE_MASK);
        size_t chunk = (size_t)MD_X86_CODE_PAGE_SIZE - in_page;
        if (chunk > size) chunk = size;

        runtime->code_page_executable[page] |= MD_X86_PAGE_TRANSLATED;
        address = (address + (uint32_t)chunk) & MD_X86_ADDRESS_MASK;
        size -= chunk;
    }
#else
    (void)runtime;
    (void)segment;
    (void)offset;
    (void)size;
#endif
}


int md_runtime_aot_find(const MdRuntime *runtime, const void *program, uint16_t segment)
{
#if MICRODOS_TRANSLATION_SUPPORT
    unsigned i;
    for (i = 0; i < MD_AOT_ATTACH_SLOTS; ++i) {
        const MdAotGuard *g = &runtime->aot_slots[i];
        if (g->in_use && g->program == program && g->segment == segment) return (int)i;
    }
    return -1;
#else
    (void)runtime;
    (void)program;
    (void)segment;
    return -1;
#endif
}


void md_runtime_aot_touch(MdRuntime *runtime, int slot)
{
#if MICRODOS_TRANSLATION_SUPPORT
    if (slot >= 0 && (unsigned)slot < MD_AOT_ATTACH_SLOTS) {
        runtime->aot_slots[slot].last_use = ++runtime->aot_use_clock;
    }
#else
    (void)runtime;
    (void)slot;
#endif
}


void md_x86_note_aot_slow(MdX86 *cpu, uint32_t a)
{
    md_x86_note_aot_guards(cpu, a);
}

/* All-ones bitmap for pages beyond the pool: "always take the guard path". */
static uint8_t g_md_live_all_ones[MD_X86_CODE_PAGE_SIZE / 8u];

static int md_guard_live_code_byte(const MdAotGuard *g, uint32_t a)
{
    uint32_t off, chunk;
    if (!g->in_use || !g->valid) return 0;
    off = (a - g->base) & MD_X86_ADDRESS_MASK;
    if (off >= g->size || ((g->code_bits[off >> 3] >> (off & 7u)) & 1u) == 0u) return 0;
    chunk = off >> MD_AOT_CHUNK_SHIFT;
    return ((g->chunk_ok[chunk >> 3] >> (chunk & 7u)) & 1u) != 0u;
}

#if MICRODOS_TRANSLATION_SUPPORT
static void md_set_live_bit(MdRuntime *rt, uint32_t a)
{
    const unsigned page = (unsigned)(a >> MD_X86_CODE_PAGE_SHIFT);
    uint8_t *bm = rt->aot_live_bits[page];
    if (bm == NULL) {
        if (rt->aot_live_pool_used < MD_AOT_LIVE_PAGES) {
            bm = rt->aot_live_pool[rt->aot_live_pool_used++];
            memset(bm, 0, MD_X86_CODE_PAGE_SIZE / 8u);
        } else {
            memset(g_md_live_all_ones, 0xFF, sizeof(g_md_live_all_ones));
            bm = g_md_live_all_ones;
        }
        rt->aot_live_bits[page] = bm;
    }
    if (bm != g_md_live_all_ones) bm[(a & MD_X86_CODE_PAGE_MASK) >> 3] |= (uint8_t)(1u << (a & 7u));
}
#endif


#if MICRODOS_TRANSLATION_SUPPORT
static void md_runtime_aot_rebuild_live_bits(MdRuntime *runtime)
{
    unsigned i;
    memset(runtime->aot_live_bits, 0, sizeof(runtime->aot_live_bits));
    runtime->aot_live_pool_used = 0u;
    for (i = 0; i < MD_AOT_ATTACH_SLOTS; ++i) {
        const MdAotGuard *g = &runtime->aot_slots[i];
        uint32_t off;
        if (!g->in_use || !g->valid) continue;
        for (off = 0; off < g->size; ++off) {
            if (((g->code_bits[off >> 3] >> (off & 7u)) & 1u) != 0u &&
                md_guard_live_code_byte(g, (g->base + off) & MD_X86_ADDRESS_MASK)) {
                md_set_live_bit(runtime, (g->base + off) & MD_X86_ADDRESS_MASK);
            }
        }
    }
}
#endif


/* A chunk of `guard` died: clear its live bits, then re-derive any bit that
   another live guard still needs (overlapping attachments stay correct). */
void md_x86_aot_chunk_died(MdX86 *cpu, const MdAotGuard *guard, uint32_t chunk)
{
    const uint32_t first = chunk << MD_AOT_CHUNK_SHIFT;
    uint32_t off;
    unsigned i;
    for (off = first; off < first + (1u << MD_AOT_CHUNK_SHIFT) && off < guard->size; ++off) {
        const uint32_t a = (guard->base + off) & MD_X86_ADDRESS_MASK;
        uint8_t *bm = cpu->aot_live_bits[a >> MD_X86_CODE_PAGE_SHIFT];
        int keep = 0;
        if (bm == NULL || bm == g_md_live_all_ones) continue;
        if (((guard->code_bits[off >> 3] >> (off & 7u)) & 1u) == 0u) continue;
        for (i = 0; i < cpu->aot_guard_count && !keep; ++i) {
            if (&cpu->aot_guards[i] != guard) keep = md_guard_live_code_byte(&cpu->aot_guards[i], a);
        }
        if (!keep) bm[(a & MD_X86_CODE_PAGE_MASK) >> 3] &= (uint8_t)~(1u << (a & 7u));
    }
}

/* Rebuild the page -> guard owner map from the attachment slots. Called
   whenever an attachment is (re)armed; guards that die later keep their
   entries, which is safe (md_x86_aot_check_guard ignores dead guards). */
#if MICRODOS_TRANSLATION_SUPPORT
static void md_runtime_aot_rebuild_owner_map(MdRuntime *runtime)
{
    unsigned i;
    memset(runtime->aot_page_owner, 0, sizeof(runtime->aot_page_owner));
    for (i = 0; i < MD_AOT_ATTACH_SLOTS; ++i) {
        const MdAotGuard *g = &runtime->aot_slots[i];
        uint32_t page, first, last;
        if (!g->in_use || !g->valid || g->size == 0u) continue;
        first = (g->base & MD_X86_ADDRESS_MASK) >> MD_X86_CODE_PAGE_SHIFT;
        last = ((g->base + g->size - 1u) & MD_X86_ADDRESS_MASK) >> MD_X86_CODE_PAGE_SHIFT;
        for (page = first;; page = (page + 1u) % MD_X86_CODE_PAGE_COUNT) {
            uint8_t *o = &runtime->aot_page_owner[page];
            *o = (*o == 0u || *o == (uint8_t)(i + 1u)) ? (uint8_t)(i + 1u) : 0xFFu;
            if (page == last) break;
        }
    }
}
#endif


/* Out-of-line tracked stores (see x86.h). */
void md_x86_store8_tracked(MdX86 *cpu, uint32_t a0, uint8_t value)
{
    md_x86_write8_tracked_inline(cpu, a0, value);
}

void md_x86_store16_tracked(MdX86 *cpu, uint32_t a0, uint16_t value)
{
    md_x86_write16_tracked_inline(cpu, a0, value);
}

void md_x86_write_block(MdX86 *cpu, uint16_t segment, uint16_t offset,
                        const uint8_t *data, uint32_t len)
{
    const uint32_t lin = md_x86_linear(segment, offset);
    uint32_t i;
    if (len == 0u) return;
    if ((uint32_t)offset + len > 0x10000u || lin + len > MD_X86_ADDRESS_SPACE) {
        for (i = 0; i < len; ++i) md_x86_write8(cpu, segment, (uint16_t)(offset + i), data[i]);
        return;
    }
    memcpy(cpu->memory + lin, data, len);
#if MICRODOS_TRANSLATION_SUPPORT
    {
        uint32_t a = lin;
        const uint32_t end = lin + len;
        while (a < end) {
            const uint32_t page_end = (a | MD_X86_CODE_PAGE_MASK) + 1u;
            const uint32_t stop = page_end < end ? page_end : end;
            const unsigned f = md_x86_page_flags(cpu, a);
            if (f & MD_X86_PAGE_TRANSLATED) md_x86_note_page_write(cpu, a);
            if (f != 0u) {
                for (; a < stop; ++a) md_x86_note_aot_write(cpu, a);
            }
            a = stop;
        }
    }
#endif
}


MdAotGuard *md_runtime_aot_attach(MdRuntime *runtime, const void *program, uint16_t segment,
                                  uint32_t base, uint32_t size, const uint8_t *code_bits)
{
#if MICRODOS_TRANSLATION_SUPPORT
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
    g->valid = 0u;
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
        const uint32_t bytes_per_chunk_map = (1u << MD_AOT_CHUNK_SHIFT) / 8u;
        for (c = 0; c * (1u << MD_AOT_CHUNK_SHIFT) < size; ++c) {
            uint32_t k, any = 0u;
            for (k = 0; k < bytes_per_chunk_map; ++k) {
                const uint32_t idx = c * bytes_per_chunk_map + k;
                if (idx < (size + 7u) / 8u) any |= code_bits[idx];
            }
            if (any != 0u) ++g->live_chunks;
        }
    }
    g->valid = size <= 0x10000u && g->live_chunks != 0u;
    if ((unsigned)slot + 1u > runtime->cpu.aot_guard_count) {
        runtime->cpu.aot_guard_count = (unsigned)slot + 1u;
    }
    md_runtime_aot_rebuild_owner_map(runtime);
    md_runtime_aot_rebuild_live_bits(runtime);
    if (g->valid) {
        uint32_t page = (base & MD_X86_ADDRESS_MASK) >> MD_X86_CODE_PAGE_SHIFT;
        const uint32_t last = ((base + size - 1u) & MD_X86_ADDRESS_MASK) >> MD_X86_CODE_PAGE_SHIFT;
        for (;; page = (page + 1u) % MD_X86_CODE_PAGE_COUNT) {
            runtime->code_page_executable[page] |= MD_X86_PAGE_AOT;
            if (page == last) break;
        }
    }
    return g;
#else
    (void)runtime;
    (void)program;
    (void)segment;
    (void)base;
    (void)size;
    (void)code_bits;
    return NULL;
#endif
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
