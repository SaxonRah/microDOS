#ifndef MICRODOS_X86_H
#define MICRODOS_X86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MD_X86_AX = 0,
    MD_X86_CX = 1,
    MD_X86_DX = 2,
    MD_X86_BX = 3,
    MD_X86_SP = 4,
    MD_X86_BP = 5,
    MD_X86_SI = 6,
    MD_X86_DI = 7
};

enum {
    MD_X86_FLAG_CF = 0x0001u,
    MD_X86_FLAG_PF = 0x0004u,
    MD_X86_FLAG_AF = 0x0010u,
    MD_X86_FLAG_ZF = 0x0040u,
    MD_X86_FLAG_SF = 0x0080u,
    MD_X86_FLAG_TF = 0x0100u,
    MD_X86_FLAG_IF = 0x0200u,
    MD_X86_FLAG_DF = 0x0400u,
    MD_X86_FLAG_OF = 0x0800u,
    MD_X86_FLAG_ALWAYS1 = 0x0002u
};

/* Guest physical address width. 20 bits (1 MiB) is the 8086 and the
   default everywhere. Smaller widths exist only for memory-placement
   experiments (M21.1 A/B firmware: a 128 KiB guest that fits in SRAM);
   addresses then alias modulo the smaller space. Mask and size are derived
   from one number so they can never disagree. */
#ifndef MD_X86_ADDRESS_BITS
#define MD_X86_ADDRESS_BITS 20u
#endif
#define MD_X86_ADDRESS_SPACE (1u << MD_X86_ADDRESS_BITS)
#define MD_X86_ADDRESS_MASK (MD_X86_ADDRESS_SPACE - 1u)

/* 4 KiB executable-page tracking keeps the metadata small enough for RP2350
   internal SRAM while still invalidating only the region touched by self-
   modifying code. There are exactly 256 pages in the 8086 20-bit space. */
#define MD_X86_CODE_PAGE_SHIFT 12u
#define MD_X86_CODE_PAGE_SIZE (1u << MD_X86_CODE_PAGE_SHIFT)
#define MD_X86_CODE_PAGE_MASK (MD_X86_CODE_PAGE_SIZE - 1u)
#define MD_X86_CODE_PAGE_COUNT (MD_X86_ADDRESS_SPACE / MD_X86_CODE_PAGE_SIZE)

/* Byte-exact AOT invalidation (M13, reworked in M15).
   Each attachment of a generated image owns one guard. `base` is the linear
   address of image byte 0 and `code_bits` has one bit per image byte that
   belongs to a compiled instruction. Writes to other bytes of the image
   (variables next to code) leave it valid.

   Guards live in a table owned by MdRuntime (see md_runtime_aot_attach), so
   resetting or re-initialising a runtime drops every attachment with it and
   two runtimes never share attachment state. */
#ifndef MD_AOT_ATTACH_SLOTS
#define MD_AOT_ATTACH_SLOTS 8u
#endif

/* M17 chunk-granular invalidation: a store to a compiled byte clears only the
   valid bit of its 64-byte chunk, so data reuse of one region (e.g. DOS
   turning its one-time init code into buffers) disables just the blocks
   there. Generated blocks check the chunks they span on entry. */
#define MD_AOT_CHUNK_SHIFT 6u
#define MD_AOT_MAX_CHUNKS (0x10000u >> MD_AOT_CHUNK_SHIFT)

typedef struct MdAotGuard {
    uint32_t base;
    uint32_t size;
    const uint8_t *code_bits;
    const void *program;        /* identity of the generated image */
    uint32_t invalidations;     /* compiled-byte stores seen */
    uint32_t epoch;             /* advances on every such store */
    uint32_t live_chunks;       /* chunks that hold compiled code and are
                                   still valid; the attachment dies at 0 */
    uint32_t last_use;          /* LRU clock for slot replacement */
    uint16_t segment;
    uint8_t valid;              /* attachment is live (whole-image switch) */
    uint8_t in_use;             /* slot holds an attachment */
    uint8_t chunk_ok[MD_AOT_MAX_CHUNKS / 8u];   /* per-64-byte validity */
} MdAotGuard;

typedef struct MdX86 {
    uint16_t r[8];
    uint16_t es;
    uint16_t cs;
    uint16_t ss;
    uint16_t ds;
    uint16_t ip;
    /* M18 lazy flags. `flags_raw` always holds the non-arithmetic bits
       (TF/IF/DF/...) and holds OSZAPC only while `lazy_op` is MD_LAZY_NONE.
       Otherwise OSZAPC are derived on demand from the last flag-producing
       ALU operation. Never read flags_raw's OSZAPC bits directly: use
       md_x86_flags() / md_x86_cf() ... (the field was renamed from `flags`
       so the compiler finds every access). */
    uint16_t flags_raw;
    uint8_t lazy_op;
    uint8_t lazy_carry;       /* ADC/SBB carry-in, or INC/DEC preserved CF */
    uint16_t lazy_a;
    uint16_t lazy_b;
    uint16_t lazy_res;
    uint8_t *memory;

    /* Optional runtime-owned code tracking. Standalone MdX86 users may leave
       these NULL; ordinary reads/writes then remain simple memory accesses. */
    uint32_t *code_page_generation;
    uint8_t *code_page_executable;
    uint32_t *code_write_epoch;

    /* Optional generated-image guards (see MdAotGuard), runtime-owned. */
    MdAotGuard *aot_guards;
    unsigned aot_guard_count;
    /* M21.1b: per-code-page owner of live compiled code: 0 = none,
       k = only guard slot k-1, 0xFF = several. Conservative: never 0 for a
       page a live guard covers (a stale entry naming a dead guard is
       harmless). NULL = scan every guard (standalone MdX86 users). */
    const uint8_t *aot_page_owner;
    /* M21.1b: per-code-page merged bitmap of LIVE compiled bytes (one bit
       per byte; NULL = none on this page). A store whose bit is clear needs
       no guard work at all. Maintained by the runtime: built on attach,
       cleared per chunk as chunks die (re-derived from other live guards).
       NULL table = always take the guard path (standalone MdX86 users). */
    uint8_t **aot_live_bits;
    /* M25: per-code-page bitmap of bytes covered by translated blocks (one
       bit per byte; NULL = page-granular). Only consulted for pages flagged
       MD_X86_PAGE_TRBYTES. Owned by the translator (translate.c). */
    uint8_t **tr_live_bits;
} MdX86;

/* ---- M18 lazy flags ---------------------------------------------------- */

enum {
    MD_LAZY_NONE = 0,
    MD_LAZY_ADD8, MD_LAZY_ADD16,
    MD_LAZY_ADC8, MD_LAZY_ADC16,
    MD_LAZY_SUB8, MD_LAZY_SUB16,
    MD_LAZY_SBB8, MD_LAZY_SBB16,
    MD_LAZY_LOGIC8, MD_LAZY_LOGIC16,
    MD_LAZY_INC8, MD_LAZY_INC16,
    MD_LAZY_DEC8, MD_LAZY_DEC16
};

#define MD_X86_FLAGS_OSZAPC (0x0001u | 0x0004u | 0x0010u | 0x0040u | 0x0080u | 0x0800u)

static inline int md_lazy_wide(unsigned op)
{
    return (op & 1u) == 0u;   /* even enum values are the 16-bit forms */
}

static inline unsigned md_lazy_sign(unsigned op)
{
    /* even op (16-bit) -> 0x8000, odd op (8-bit) -> 0x80, without a branch */
    return 0x80u << ((~op & 1u) << 3);
}

static inline unsigned md_lazy_mask(unsigned op)
{
    return md_lazy_wide(op) ? 0xFFFFu : 0xFFu;
}

static inline int md_x86_cf(const MdX86 *cpu)
{
    const unsigned a = cpu->lazy_a, b = cpu->lazy_b, r = cpu->lazy_res;
    switch (cpu->lazy_op) {
        case MD_LAZY_NONE: return (cpu->flags_raw & 0x0001u) != 0u;
        case MD_LAZY_ADD8: case MD_LAZY_ADD16: return r < a;
        case MD_LAZY_ADC8: case MD_LAZY_ADC16: return cpu->lazy_carry ? r <= a : r < a;
        case MD_LAZY_SUB8: case MD_LAZY_SUB16: return a < b;
        case MD_LAZY_SBB8: case MD_LAZY_SBB16: return cpu->lazy_carry ? a <= b : a < b;
        case MD_LAZY_LOGIC8: case MD_LAZY_LOGIC16: return 0;
        default: return cpu->lazy_carry != 0u;           /* INC/DEC keep CF */
    }
}

/*
 * M24.4: lazy ZF fast path.
 *
 * Every lazy 8-bit ALU producer stores a uint8_t result through md_x86_lazy(),
 * which zero-extends it into lazy_res. 16-bit producers already store the full
 * uint16_t result. Therefore a pending lazy result needs no width-dependent
 * mask for ZF: zero is zero at either width.
 *
 * This removes md_lazy_mask()/width testing from hot JZ/JNZ, LOOPZ/NZ and
 * REP compare termination while preserving the raw-FLAGS path exactly.
 */
static inline int md_x86_zf(const MdX86 *cpu)
{
    if (cpu->lazy_op == MD_LAZY_NONE) return (cpu->flags_raw & 0x0040u) != 0u;
    return cpu->lazy_res == 0u;
}

static inline int md_x86_sf(const MdX86 *cpu)
{
    if (cpu->lazy_op == MD_LAZY_NONE) return (cpu->flags_raw & 0x0080u) != 0u;
    return (cpu->lazy_res & md_lazy_sign(cpu->lazy_op)) != 0u;
}

static inline int md_x86_of(const MdX86 *cpu)
{
    const unsigned a = cpu->lazy_a, b = cpu->lazy_b, r = cpu->lazy_res;
    const unsigned sign = md_lazy_sign(cpu->lazy_op);
    switch (cpu->lazy_op) {
        case MD_LAZY_NONE: return (cpu->flags_raw & 0x0800u) != 0u;
        case MD_LAZY_ADD8: case MD_LAZY_ADD16:
        case MD_LAZY_ADC8: case MD_LAZY_ADC16:
        case MD_LAZY_INC8: case MD_LAZY_INC16:           /* INC: b == 1 */
            return ((~(a ^ b) & (a ^ r)) & sign) != 0u;
        case MD_LAZY_SUB8: case MD_LAZY_SUB16:
        case MD_LAZY_SBB8: case MD_LAZY_SBB16:
        case MD_LAZY_DEC8: case MD_LAZY_DEC16:           /* DEC: b == 1 */
            return (((a ^ b) & (a ^ r)) & sign) != 0u;
        default: return 0;                               /* logic */
    }
}

/* Folds a pending lazy result into flags_raw (same values the eager
   helpers used to compute, proven by tests/test_runtime.c). */
static inline void md_x86_flags_materialize(MdX86 *cpu)
{
    const unsigned op = cpu->lazy_op;
    const unsigned r = cpu->lazy_res;
    unsigned f;
    unsigned p;
    if (op == MD_LAZY_NONE) return;
    f = cpu->flags_raw & (0xFFFFu ^ MD_X86_FLAGS_OSZAPC);   /* no truncating cast (MSVC C4310) */
    /* Every flag lands in its bit position with shifts/ORs, no branches. */
    f |= (unsigned)md_x86_cf(cpu);                               /* CF bit 0 */
    p = (r ^ (r >> 4)) & 0x0Fu;
    f |= ((0x9669u >> p) & 1u) << 2;                             /* PF bit 2 */
    if (op != MD_LAZY_LOGIC8 && op != MD_LAZY_LOGIC16)
        f |= (cpu->lazy_a ^ cpu->lazy_b ^ r) & 0x10u;            /* AF bit 4 */
    f |= (unsigned)(r == 0u) << 6;                               /* ZF bit 6 */
    f |= (unsigned)((r & md_lazy_sign(op)) != 0u) << 7;          /* SF bit 7 */
    f |= (unsigned)md_x86_of(cpu) << 11;                         /* OF bit 11 */
    cpu->flags_raw = (uint16_t)f;
    cpu->lazy_op = MD_LAZY_NONE;
}

/* The architectural FLAGS word (materialises). */
static inline uint16_t md_x86_flags(MdX86 *cpu)
{
    md_x86_flags_materialize(cpu);
    return cpu->flags_raw;
}

/* Replace the whole FLAGS word (POPF, IRET, SAHF-merged values, tests). */
static inline void md_x86_set_flags(MdX86 *cpu, uint16_t value)
{
    cpu->lazy_op = MD_LAZY_NONE;
    cpu->flags_raw = value;
}

/* Set/clear individual OSZAPC bits (CLC/STC/CMC, MUL/DIV, native services). */
static inline void md_x86_update_flags(MdX86 *cpu, uint16_t clear_mask, uint16_t set_mask)
{
    md_x86_flags_materialize(cpu);
    cpu->flags_raw = (uint16_t)((cpu->flags_raw & (uint16_t)~clear_mask) | set_mask);
}

static inline void md_x86_lazy(MdX86 *cpu, unsigned op, unsigned a, unsigned b, unsigned r)
{
    cpu->lazy_op = (uint8_t)op;
    cpu->lazy_a = (uint16_t)a;
    cpu->lazy_b = (uint16_t)b;
    cpu->lazy_res = (uint16_t)r;
}

static inline uint32_t md_x86_linear(uint16_t segment, uint16_t offset)
{
    return ((((uint32_t)segment) << 4) + (uint32_t)offset) & MD_X86_ADDRESS_MASK;
}

static inline unsigned md_x86_code_page(uint32_t address)
{
    return (unsigned)((address & MD_X86_ADDRESS_MASK) >> MD_X86_CODE_PAGE_SHIFT);
}

/* True when every chunk covering image offsets [first, last] is valid. */
static inline int md_aot_chunks_ok(const MdAotGuard *guard, uint32_t first, uint32_t last)
{
    uint32_t c;
    for (c = first >> MD_AOT_CHUNK_SHIFT; c <= (last >> MD_AOT_CHUNK_SHIFT); ++c) {
        if (((guard->chunk_ok[c >> 3] >> (c & 7u)) & 1u) == 0u) return 0;
    }
    return 1;
}

/* M21.1b page flags (code_page_executable[]): who must hear about stores.
   TRANSLATED: the decoded-block cache or the JIT translated code on this
   page; its generation (and the global code-write epoch) must change on
   every store. AOT: compiled code may be live here; stores are checked
   against the live-code bitmap / guards. Compiled images set only AOT, so
   kernel stack/data stores no longer pay for generation bumps nobody reads. */
#define MD_X86_PAGE_TRANSLATED 0x01u
#define MD_X86_PAGE_AOT 0x02u
/* M25: the page holds translated blocks tracked byte-exactly through
   tr_live_bits; only a store to a covered byte bumps the page generation. */
#define MD_X86_PAGE_TRBYTES 0x04u

/* Pure-interpreter targets have nothing translated to invalidate.  Compile
   store tracking out entirely there: self-modifying code remains naturally
   coherent because the next interpreter fetch reads guest memory directly. */
#ifndef MD_X86_TRACK_WRITES
#define MD_X86_TRACK_WRITES 1
#endif

static inline unsigned md_x86_page_flags(const MdX86 *cpu, uint32_t address)
{
    if (cpu->code_page_generation == NULL || cpu->code_page_executable == NULL) return 0u;
    return cpu->code_page_executable[md_x86_code_page(address)];
}

static inline int md_x86_page_executable(const MdX86 *cpu, uint32_t address)
{
    return md_x86_page_flags(cpu, address) != 0u;
}

/* Page-granular bookkeeping for the decoded-block cache and the global code
   write epoch. Call at most once per touched executable page per store. */
static inline void md_x86_note_page_write(MdX86 *cpu, uint32_t address)
{
    const unsigned page = md_x86_code_page(address);
    uint32_t next;

    next = cpu->code_page_generation[page] + 1u;
    if (next == 0u) next = 1u;
    cpu->code_page_generation[page] = next;

    if (cpu->code_write_epoch != NULL) {
        next = *cpu->code_write_epoch + 1u;
        if (next == 0u) next = 1u;
        *cpu->code_write_epoch = next;
    }
}

/* Byte-exact AOT check. Must be called for EVERY modified byte that lies on
   an executable page, including both bytes of a same-page word store (M15
   bug fix: the second byte used to be skipped unless it crossed a page). */
/* One guard, one byte: the exact M15/M17 semantics. */
/* Clears the merged live bits of one dead chunk (runtime.c); bits another
   live guard still needs are re-derived there. */
void md_x86_aot_chunk_died(MdX86 *cpu, const MdAotGuard *guard, uint32_t chunk);

/* One guard, one byte: the exact M15/M17 semantics. `invalidations` counts
   stores that hit a live compiled byte (M21.1b; it is a statistic only). */
static inline void md_x86_aot_check_guard(MdX86 *cpu, MdAotGuard *guard, uint32_t a)
{
    uint32_t off;
    if (!guard->valid) return;
    off = (a - guard->base) & MD_X86_ADDRESS_MASK;
    if (off < guard->size &&
        ((guard->code_bits[off >> 3] >> (off & 7u)) & 1u) != 0u) {
        const uint32_t chunk = off >> MD_AOT_CHUNK_SHIFT;
        const uint8_t bit = (uint8_t)(1u << (chunk & 7u));
        if ((guard->chunk_ok[chunk >> 3] & bit) != 0u) {
            guard->chunk_ok[chunk >> 3] &= (uint8_t)~bit;
            /* Every compiled chunk overwritten (e.g. DOS loaded another
               program here): the attachment is dead, so hosts stop treating
               this segment as compiled code. */
            if (guard->live_chunks != 0u && --guard->live_chunks == 0u) guard->valid = 0u;
            /* M17b: only a chunk that goes valid -> invalid can affect a
               running block (blocks are checked on entry), so only that
               advances the epoch. */
            ++guard->epoch;
            ++guard->invalidations;
            if (cpu->aot_live_bits != NULL) md_x86_aot_chunk_died(cpu, guard, chunk);
        }
    }
}

/* Guard work for a store that may hit live compiled code (runtime.c). */
void md_x86_note_aot_slow(MdX86 *cpu, uint32_t a);

#if defined(__GNUC__) || defined(__clang__)
#define MD_X86_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define MD_X86_ALWAYS_INLINE inline
#endif

/* M21.1b: one bit test inline; a clear bit means "not a live compiled
   byte" (stack, data, DOS buffers in dead init code), which is almost
   every store. Only live hits reach the out-of-line guard path. */
static MD_X86_ALWAYS_INLINE void md_x86_note_aot_write(MdX86 *cpu, uint32_t address)
{
    const uint32_t a = address & MD_X86_ADDRESS_MASK;
    if (cpu->aot_live_bits != NULL) {
        const uint8_t *bm = cpu->aot_live_bits[a >> MD_X86_CODE_PAGE_SHIFT];
        if (bm == NULL) return;
        if (((bm[(a & MD_X86_CODE_PAGE_MASK) >> 3] >> (a & 7u)) & 1u) == 0u) return;
    }
    md_x86_note_aot_slow(cpu, a);
}

static inline void md_x86_note_aot_guards(MdX86 *cpu, uint32_t a)
{
    unsigned i;
    if (cpu->aot_page_owner != NULL) {
        const uint8_t owner = cpu->aot_page_owner[a >> MD_X86_CODE_PAGE_SHIFT];
        if (owner == 0u) return;
        if (owner != 0xFFu) {
            md_x86_aot_check_guard(cpu, &cpu->aot_guards[owner - 1u], a);
            return;
        }
    }
    for (i = 0; i < cpu->aot_guard_count; ++i) md_x86_aot_check_guard(cpu, &cpu->aot_guards[i], a);
}

/* M25 byte-exact check: a store to data that merely shares a page with
   translated code (the normal .COM layout) neither bumps the page
   generation nor invalidates anything. */
static inline void md_x86_note_tr_write(MdX86 *cpu, uint32_t address)
{
    const uint32_t a = address & MD_X86_ADDRESS_MASK;
    const uint8_t *bm = cpu->tr_live_bits != NULL ? cpu->tr_live_bits[a >> MD_X86_CODE_PAGE_SHIFT] : NULL;
    if (bm != NULL && ((bm[(a & MD_X86_CODE_PAGE_MASK) >> 3] >> (a & 7u)) & 1u) == 0u) return;
    md_x86_note_page_write(cpu, a);
}

static inline void md_x86_note_code_write(MdX86 *cpu, uint32_t address)
{
    const unsigned f = md_x86_page_flags(cpu, address);
    if (f & MD_X86_PAGE_TRANSLATED) md_x86_note_page_write(cpu, address);
    if (f & MD_X86_PAGE_TRBYTES) md_x86_note_tr_write(cpu, address);
    if (f != 0u) md_x86_note_aot_write(cpu, address);
}

static inline uint8_t md_x86_read8_linear(const MdX86 *cpu, uint32_t address)
{
    return cpu->memory[address & MD_X86_ADDRESS_MASK];
}

static inline uint16_t md_x86_read16_linear(const MdX86 *cpu, uint32_t address)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    const uint32_t a1 = (a0 + 1u) & MD_X86_ADDRESS_MASK;
    return (uint16_t)((uint16_t)cpu->memory[a0] | ((uint16_t)cpu->memory[a1] << 8));
}

/* Tracked stores (M21.1b). The inline part handles the common case of a
   page holding no translated/compiled code; anything else goes to one
   shared out-of-line implementation (runtime.c), instead of the whole
   tracking sequence being inlined at every store site in generated code.
   Semantics are exactly those of md_x86_write8/16_tracked_inline below. */
void md_x86_store8_tracked(MdX86 *cpu, uint32_t a0, uint8_t value);
void md_x86_store16_tracked(MdX86 *cpu, uint32_t a0, uint16_t value);
/* Copy `len` bytes to segment:offset with exactly the effect of `len`
   md_x86_write8 calls (offset wraps within the segment), but one memcpy and
   per-page bookkeeping when the range does not wrap (disk transfers). */
void md_x86_write_block(MdX86 *cpu, uint16_t segment, uint16_t offset,
                        const uint8_t *data, uint32_t len);

static inline void md_x86_write8_tracked_inline(MdX86 *cpu, uint32_t a0, uint8_t value)
{
    cpu->memory[a0] = value;
    md_x86_note_code_write(cpu, a0);
}

static inline void md_x86_write16_tracked_inline(MdX86 *cpu, uint32_t a0, uint16_t value)
{
    const uint32_t a1 = (a0 + 1u) & MD_X86_ADDRESS_MASK;
    const unsigned p0 = md_x86_code_page(a0);
    const unsigned p1 = md_x86_code_page(a1);

    const unsigned f0 = md_x86_page_flags(cpu, a0);
    const unsigned f1 = p1 == p0 ? f0 : md_x86_page_flags(cpu, a1);

    cpu->memory[a0] = (uint8_t)value;
    cpu->memory[a1] = (uint8_t)(value >> 8);
    /* Page generations: once per touched TRANSLATED page. AOT: every byte. */
    if (f0 & MD_X86_PAGE_TRANSLATED) md_x86_note_page_write(cpu, a0);
    if ((f1 & MD_X86_PAGE_TRANSLATED) && p1 != p0) md_x86_note_page_write(cpu, a1);
    if (f0 & MD_X86_PAGE_TRBYTES) md_x86_note_tr_write(cpu, a0);
    if (f1 & MD_X86_PAGE_TRBYTES) md_x86_note_tr_write(cpu, a1);
    if (f0 != 0u) md_x86_note_aot_write(cpu, a0);
    if (f1 != 0u) md_x86_note_aot_write(cpu, a1);
}

#if MD_X86_TRACK_WRITES
static inline void md_x86_write8_linear(MdX86 *cpu, uint32_t address, uint8_t value)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    if (!md_x86_page_executable(cpu, a0)) { cpu->memory[a0] = value; return; }
    md_x86_store8_tracked(cpu, a0, value);
}

static inline void md_x86_write16_linear(MdX86 *cpu, uint32_t address, uint16_t value)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    const uint32_t a1 = (a0 + 1u) & MD_X86_ADDRESS_MASK;
    if (!md_x86_page_executable(cpu, a0) &&
        ((a0 & MD_X86_CODE_PAGE_MASK) != MD_X86_CODE_PAGE_MASK || !md_x86_page_executable(cpu, a1))) {
        cpu->memory[a0] = (uint8_t)value;
        cpu->memory[a1] = (uint8_t)(value >> 8);
        return;
    }
    md_x86_store16_tracked(cpu, a0, value);
}
#else
static inline void md_x86_write8_linear(MdX86 *cpu, uint32_t address, uint8_t value)
{
    cpu->memory[address & MD_X86_ADDRESS_MASK] = value;
}

static inline void md_x86_write16_linear(MdX86 *cpu, uint32_t address, uint16_t value)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    const uint32_t a1 = (a0 + 1u) & MD_X86_ADDRESS_MASK;
    cpu->memory[a0] = (uint8_t)value;
    cpu->memory[a1] = (uint8_t)(value >> 8);
}
#endif

static inline uint8_t md_x86_read8(const MdX86 *cpu, uint16_t segment, uint16_t offset)
{
    return md_x86_read8_linear(cpu, md_x86_linear(segment, offset));
}

static inline uint16_t md_x86_read16(const MdX86 *cpu, uint16_t segment, uint16_t offset)
{
    const uint32_t a0 = md_x86_linear(segment, offset);

    /*
     * Original 8086 segmented word accesses wrap the 16-bit offset between
     * the low and high byte.  This differs from incrementing the already
     * formed 20-bit linear address when offset == FFFFh.
     */
    if (offset != 0xFFFFu)
        return md_x86_read16_linear(cpu, a0);

    return (uint16_t)(
        (uint16_t)md_x86_read8_linear(cpu, a0) |
        ((uint16_t)md_x86_read8_linear(cpu, md_x86_linear(segment, 0u)) << 8));
}

static inline void md_x86_write8(MdX86 *cpu, uint16_t segment, uint16_t offset, uint8_t value)
{
    md_x86_write8_linear(cpu, md_x86_linear(segment, offset), value);
}

static inline void md_x86_write16(MdX86 *cpu, uint16_t segment, uint16_t offset, uint16_t value)
{
    const uint32_t a0 = md_x86_linear(segment, offset);

    if (offset != 0xFFFFu) {
        md_x86_write16_linear(cpu, a0, value);
        return;
    }

    /* See md_x86_read16(): high byte is segment:0000, not linear a0+1. */
    md_x86_write8_linear(cpu, a0, (uint8_t)value);
    md_x86_write8_linear(cpu, md_x86_linear(segment, 0u), (uint8_t)(value >> 8));
}

static inline uint8_t md_x86_get_reg8(const MdX86 *cpu, unsigned reg)
{
    const unsigned slot = reg & 3u;
    const unsigned shift = (reg & 4u) ? 8u : 0u;
    return (uint8_t)(cpu->r[slot] >> shift);
}

static inline void md_x86_set_reg8(MdX86 *cpu, unsigned reg, uint8_t value)
{
    const unsigned slot = reg & 3u;
    const uint16_t old = cpu->r[slot];
    if (reg & 4u) {
        cpu->r[slot] = (uint16_t)((old & 0x00FFu) | ((uint16_t)value << 8));
    } else {
        cpu->r[slot] = (uint16_t)((old & 0xFF00u) | value);
    }
}

static inline void md_x86_push(MdX86 *cpu, uint16_t value)
{
    cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] - 2u);
    md_x86_write16(cpu, cpu->ss, cpu->r[MD_X86_SP], value);
}

static inline uint16_t md_x86_pop(MdX86 *cpu)
{
    const uint16_t value = md_x86_read16(cpu, cpu->ss, cpu->r[MD_X86_SP]);
    cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + 2u);
    return value;
}

/* The original 8086/8088 has a famous PUSH SP quirk: the value written is
   the already-decremented SP, not the pre-instruction SP used by later x86. */
static inline void md_x86_push_reg(MdX86 *cpu, unsigned reg)
{
    reg &= 7u;
    if (reg == MD_X86_SP) {
        cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] - 2u);
        md_x86_write16(cpu, cpu->ss, cpu->r[MD_X86_SP], cpu->r[MD_X86_SP]);
    } else {
        md_x86_push(cpu, cpu->r[reg]);
    }
}

#ifdef __cplusplus
}
#endif

#endif
