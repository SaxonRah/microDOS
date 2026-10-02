/* M21.1b store-path equivalence.
 *
 * The tracked-store path was restructured for speed (per-page AOT owner map,
 * out-of-line slow path, bulk block stores). This test replays the same
 * random store sequence through the new path and through the pre-M21.1b
 * algorithm (kept verbatim below as the reference: page checks per byte and
 * a scan of every guard slot) and requires identical results after every
 * operation:
 *   - guest memory,
 *   - every guard's valid / chunk_ok / live_chunks / epoch / invalidations
 *     (invalidations = stores that hit a live compiled byte, M21.1b),
 *   - which code pages had their generation changed, and whether the global
 *     code-write epoch changed (generation values may differ for block
 *     stores, which bump once per page instead of once per byte; only
 *     "changed" is observable to the cache/JIT validity checks).
 */
#include "microdos/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)

/* ---- reference (pre-M21.1b) ------------------------------------------- */
static void ref_note_aot(MdX86 *cpu, uint32_t address)
{
    const uint32_t a = address & MD_X86_ADDRESS_MASK;
    unsigned i;
    for (i = 0; i < cpu->aot_guard_count; ++i) {
        MdAotGuard *guard = &cpu->aot_guards[i];
        uint32_t off;
        if (!guard->valid) continue;
        off = (a - guard->base) & MD_X86_ADDRESS_MASK;
        if (off < guard->size && ((guard->code_bits[off >> 3] >> (off & 7u)) & 1u) != 0u) {
            const uint32_t chunk = off >> MD_AOT_CHUNK_SHIFT;
            const uint8_t bit = (uint8_t)(1u << (chunk & 7u));
            if ((guard->chunk_ok[chunk >> 3] & bit) != 0u) {
                guard->chunk_ok[chunk >> 3] &= (uint8_t)~bit;
                if (guard->live_chunks != 0u && --guard->live_chunks == 0u) guard->valid = 0u;
                ++guard->epoch;
                ++guard->invalidations;   /* M21.1b meaning: live hits only */
            }
        }
    }
}
/* Reference with the M21.1b two-flag rule: generation bumps only on
   TRANSLATED pages; AOT/guard checks on any flagged page. */
static void ref_write8(MdX86 *cpu, uint32_t address, uint8_t value)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    const unsigned f = md_x86_page_flags(cpu, a0);
    cpu->memory[a0] = value;
    if (f & MD_X86_PAGE_TRANSLATED) md_x86_note_page_write(cpu, a0);
    if (f != 0u) ref_note_aot(cpu, a0);
}
static void ref_write16(MdX86 *cpu, uint32_t address, uint16_t value)
{
    const uint32_t a0 = address & MD_X86_ADDRESS_MASK;
    const uint32_t a1 = (a0 + 1u) & MD_X86_ADDRESS_MASK;
    const unsigned p0 = md_x86_code_page(a0), p1 = md_x86_code_page(a1);
    const unsigned f0 = md_x86_page_flags(cpu, a0);
    const unsigned f1 = p1 == p0 ? f0 : md_x86_page_flags(cpu, a1);
    cpu->memory[a0] = (uint8_t)value;
    cpu->memory[a1] = (uint8_t)(value >> 8);
    if (f0 & MD_X86_PAGE_TRANSLATED) md_x86_note_page_write(cpu, a0);
    if ((f1 & MD_X86_PAGE_TRANSLATED) && p1 != p0) md_x86_note_page_write(cpu, a1);
    if (f0 != 0u) ref_note_aot(cpu, a0);
    if (f1 != 0u) ref_note_aot(cpu, a1);
}

/* ---- harness ------------------------------------------------------------ */
static uint32_t g_seed = 0x1234567u;
static uint32_t rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 1; }

#define NGUARD 4
static uint8_t g_bits[NGUARD][8192];
static const uint32_t kBase[NGUARD] = { 0x10000u, 0x10800u, 0x23FC0u, 0x9D1F0u };  /* 0/1 share a page */
static const uint32_t kSize[NGUARD] = { 16690u, 3000u, 600u, 9000u };

static void attach_all(MdRuntime *rt)
{
    unsigned g;
    for (g = 0; g < NGUARD; ++g) {
        (void)md_runtime_aot_attach(rt, &g_bits[g], (uint16_t)(0x1000u + g), kBase[g], kSize[g], g_bits[g]);
        /* guards 0-2 also have translated (cache/JIT) code on their pages;
           guard 3's pages are AOT-only */
        if (g != 3u) md_runtime_mark_code_range(rt, (uint16_t)(kBase[g] >> 4), (uint16_t)(kBase[g] & 15u), kSize[g]);
    }
}

static int same_guards(const MdRuntime *a, const MdRuntime *b)
{
    unsigned i;
    for (i = 0; i < MD_AOT_ATTACH_SLOTS; ++i) {
        const MdAotGuard *x = &a->aot_slots[i], *y = &b->aot_slots[i];
        if (x->valid != y->valid || x->live_chunks != y->live_chunks || x->epoch != y->epoch ||
            x->invalidations != y->invalidations || memcmp(x->chunk_ok, y->chunk_ok, sizeof(x->chunk_ok)) != 0)
            return 0;
    }
    return 1;
}

static int same_gen_changes(const MdRuntime *a, const MdRuntime *b,
                            const uint32_t *before_a, const uint32_t *before_b)
{
    unsigned i;
    for (i = 0; i < MD_X86_CODE_PAGE_COUNT; ++i) {
        const int ca = a->cpu.code_page_generation[i] != before_a[i];
        const int cb = b->cpu.code_page_generation[i] != before_b[i];
        if (ca != cb) return 0;
    }
    return 1;
}

/* Deterministic overlap case: guard A has code on every byte of its first
   chunk; guard B, attached over the same bytes, has code only at byte 10.
   A store to byte 1 (code for A only) kills A's chunk; B's chunk must stay
   live and a later store to byte 10 must still kill it. Guards that die
   must never hide another guard's live code (md_x86_aot_chunk_died). */
static void test_overlap_rederive(void)
{
    static MdRuntime rt;
    static uint8_t bits_a[16], bits_b[16];
    uint8_t *mem = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdHooks hooks;
    const uint32_t base = 0x30000u;
    MdAotGuard *ga, *gb;
    memset(bits_a, 0, sizeof(bits_a));
    memset(bits_b, 0, sizeof(bits_b));
    memset(bits_a, 0xFF, 8u);                /* bytes 0..63: code for A */
    bits_b[1] = 0x04u;                       /* byte 10: code for B */
    memset(&hooks, 0, sizeof(hooks));
    md_runtime_init(&rt, mem, &hooks);
    ga = md_runtime_aot_attach(&rt, bits_a, 0x3000u, base, 128u, bits_a);
    gb = md_runtime_aot_attach(&rt, bits_b, 0x3001u, base, 128u, bits_b);
    md_runtime_mark_code_range(&rt, 0x3000u, 0u, 128u);
    CHECK(ga->valid && gb->valid);
    md_x86_write8_linear(&rt.cpu, base + 1u, 0x90u);     /* kills A's chunk only */
    CHECK(!ga->valid);                                    /* A had one chunk */
    CHECK(gb->valid);
    md_x86_write8_linear(&rt.cpu, base + 10u, 0x90u);    /* B's code byte */
    CHECK(!gb->valid);                                    /* must be detected */
    free(mem);
}

int main(void)
{
    static MdRuntime a, b;
    static uint32_t gen_a[MD_X86_CODE_PAGE_COUNT], gen_b[MD_X86_CODE_PAGE_COUNT];
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    MdHooks hooks;
    unsigned op, g, i;
    unsigned long n_byte = 0, n_word = 0, n_block = 0, n_wrap = 0, n_owner_ff = 0, n_hits = 0;

    if (ma == NULL || mb == NULL) return 2;
    test_overlap_rederive();
    for (g = 0; g < NGUARD; ++g)
        for (i = 0; i < sizeof(g_bits[g]); ++i) g_bits[g][i] = (uint8_t)(rnd() & rnd() & 0xFFu);
    memset(&hooks, 0, sizeof(hooks));
    md_runtime_init(&a, ma, &hooks);
    md_runtime_init(&b, mb, &hooks);
    attach_all(&a);
    attach_all(&b);
    for (i = 0; i < MD_X86_CODE_PAGE_COUNT; ++i) if (a.aot_page_owner[i] == 0xFFu) ++n_owner_ff;
    CHECK(n_owner_ff >= 1u);                  /* a page shared by two guards is exercised */
    CHECK(same_guards(&a, &b));
    b.cpu.aot_page_owner = NULL;              /* the reference side uses neither map */
    b.cpu.aot_live_bits = NULL;

    for (op = 0; op < 200000u && failures == 0; ++op) {
        const unsigned g2 = rnd() % NGUARD;
        const unsigned kind = rnd() % 10u;
        uint32_t addr = (kBase[g2] + (rnd() % (kSize[g2] + 256u)) - 128u) & MD_X86_ADDRESS_MASK;
        const uint32_t ea = *a.cpu.code_write_epoch, eb = *b.cpu.code_write_epoch;
        if ((rnd() & 15u) == 0u) addr |= MD_X86_CODE_PAGE_MASK;          /* page edge */
        memcpy(gen_a, a.cpu.code_page_generation, sizeof(gen_a));
        memcpy(gen_b, b.cpu.code_page_generation, sizeof(gen_b));

        if (kind < 4u) {
            const uint8_t v = (uint8_t)rnd();
            md_x86_write8_linear(&a.cpu, addr, v);
            ref_write8(&b.cpu, addr, v);
            ++n_byte;
        } else if (kind < 8u) {
            const uint16_t v = (uint16_t)rnd();
            md_x86_write16_linear(&a.cpu, addr, v);
            ref_write16(&b.cpu, addr, v);
            ++n_word;
        } else {
            uint8_t buf[700];
            uint16_t seg = (uint16_t)((addr >> 4) - (rnd() & 0xFFu));
            uint16_t off = (uint16_t)(addr - ((uint32_t)seg << 4));
            const uint32_t len = 1u + rnd() % 700u;
            if ((rnd() & 15u) == 0u) { off = (uint16_t)(0x10000u - (rnd() % 300u) - 1u); ++n_wrap; }
            for (i = 0; i < len; ++i) buf[i] = (uint8_t)rnd();
            md_x86_write_block(&a.cpu, seg, off, buf, len);
            for (i = 0; i < len; ++i) ref_write8(&b.cpu, md_x86_linear(seg, (uint16_t)(off + i)), buf[i]);
            ++n_block;
        }
        CHECK(same_guards(&a, &b));
        CHECK(same_gen_changes(&a, &b, gen_a, gen_b));
        CHECK((*a.cpu.code_write_epoch != ea) == (*b.cpu.code_write_epoch != eb));
        if (failures) fprintf(stderr, "  diverged at op %u (kind %u, addr %05lX)\n", op, kind, (unsigned long)addr);
    }
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);
    for (g = 0; g < MD_AOT_ATTACH_SLOTS; ++g) n_hits += a.aot_slots[g].invalidations;
    printf("store tracking: %lu byte, %lu word, %lu block stores (%lu wrapping); %lu compiled-byte hits; "
           "%lu shared page(s)\n", n_byte, n_word, n_block, n_wrap, n_hits, n_owner_ff);
    CHECK(n_hits > 50u);
    free(ma);
    free(mb);
    if (failures) { fprintf(stderr, "store tracking: %d failure(s)\n", failures); return 1; }
    puts("store tracking: new path identical to the reference");
    return 0;
}
