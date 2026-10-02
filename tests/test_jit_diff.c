/* JIT differential fixtures (M20.3).
 *
 * Every benchmark fixture runs twice: once in the canonical interpreter and
 * once through the runtime JIT. Registers, segment registers, IP, the
 * architectural FLAGS word, all 1 MiB of guest memory, the stop reason and
 * the retired-instruction count must be identical. Each fixture also states
 * the JIT shape it exists to prove (zero interpreter fallback, a resident
 * region formed, native control transfers).
 *
 * Built for the host, this checks the JIT's decode/region logic and its C
 * reference semantics. Built for ARM and run under qemu-arm (see
 * CMakeLists.txt / tests/README), it executes the emitted Thumb-2 code
 * itself, so code-generation bugs surface before a UF2 reaches hardware.
 */
#include "microdos/jit.h"
#include "microdos/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

#define JIT_CODE_BYTES (64u * 1024u)

/* Executable buffer for generated code. On Linux (including qemu-arm user
   mode) map it executable; elsewhere a plain array is enough because the
   host path never executes generated code. */
static uint8_t *jit_code_buffer(void)
{
    static uint8_t *buf;
    if (buf != NULL) return buf;
#if defined(__linux__)
    {
        void *p = mmap(NULL, JIT_CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { buf = (uint8_t *)p; return buf; }
    }
#endif
    buf = (uint8_t *)calloc(1u, JIT_CODE_BYTES);
    return buf;
}

typedef struct Fixture {
    const char *name;
    const uint8_t *bytes;
    size_t size;
    void (*setup)(MdRuntime *rt);      /* optional extra guest state */
    int max_fallback;                  /* -1: not checked (feature not in the JIT yet) */
    int want_resident;
    int want_control;                  /* native CALL/RET/INT count > 0 */
} Fixture;

/* ---- fixtures (byte-identical to pico/microdos_bench.c) --------------- */

static const uint8_t kLoop[] = { 0xB9, 0xFF, 0xFF, 0x49, 0x75, 0xFD, 0xF4 };
static const uint8_t kMemloop[] = {
    0xB9,0x00,0x80, 0xBE,0x00,0x80, 0x8A,0x04, 0x04,0x03, 0x88,0x04,
    0x83,0xC6,0x61, 0x81,0xCE,0x00,0x80, 0x49,0x75,0xF0, 0xF4
};
static const uint8_t kRegionmix[] = {
    0xB9,0x00,0x80, 0xBE,0x00,0x80, 0xBB,0x34,0x12, 0x8A,0x04, 0x04,0x03,
    0x81,0xF3,0x57,0x13, 0x83,0xC3,0x05, 0x88,0x04, 0x83,0xC6,0x61,
    0x81,0xCE,0x00,0x80, 0x49, 0x75,0xE9, 0xF4
};
static const uint8_t kBranchmix[] = {
    0xB9,0x00,0x80, 0xB8,0x00,0x00, 0xBB,0x00,0x00, 0x83,0xF0,0x01,
    0x83,0xF8,0x00, 0x74,0x03, 0x83,0xC3,0x03, 0x83,0xCB,0x00, 0x49,
    0x75,0xEF, 0xF4
};
static const uint8_t kCallmix[] = {
    0xB9,0x00,0x80, 0xBB,0x00,0x00, 0xE8,0x04,0x00, 0x49, 0x75,0xFA, 0xF4,
    0x83,0xC3,0x03, 0xC3
};

/* M20.3 (tests/test_jit.c): LODSW / ADD DX,AX / LOOP over a small table */
static const uint8_t kLodsLoop[] = { 0xAD, 0x01,0xC2, 0xE2,0xFB, 0xF4 };
static void setup_lods_fwd(MdRuntime *rt)
{
    unsigned i;
    for (i = 0; i < 64u; ++i) md_x86_write16(&rt->cpu, rt->cpu.cs, (uint16_t)(0x0200u + i * 2u), (uint16_t)(i * 7u + 1u));
    rt->cpu.ds = rt->cpu.cs;
    rt->cpu.r[MD_X86_CX] = 64u;
    rt->cpu.r[MD_X86_DX] = 0u;
    rt->cpu.r[MD_X86_SI] = 0x0200u;
}
static void setup_lods_back(MdRuntime *rt)
{
    setup_lods_fwd(rt);
    rt->cpu.r[MD_X86_SI] = 0x027Eu;
    rt->cpu.flags_raw |= MD_X86_FLAG_DF;
}

/* LODSB / LOOPNZ: stops early on the first zero byte (ZF from OR AL,AL) */
static const uint8_t kLodsbScan[] = { 0xAC, 0x08,0xC0, 0xE0,0xFB, 0xF4 };
static void setup_lodsb_scan(MdRuntime *rt)
{
    unsigned i;
    for (i = 0; i < 100u; ++i) md_x86_write8(&rt->cpu, rt->cpu.cs, (uint16_t)(0x0300u + i), (uint8_t)(i == 70u ? 0u : i + 1u));
    rt->cpu.ds = rt->cpu.cs;
    rt->cpu.r[MD_X86_CX] = 100u;
    rt->cpu.r[MD_X86_SI] = 0x0300u;
}

/* DEC/LOOPZ: LOOPZ continues while ZF=1 and CX!=0 (CMP AL,AL forces ZF=1) */
static const uint8_t kLoopz[] = { 0x40, 0x38,0xC0, 0xE1,0xFB, 0xF4 };
static void setup_loopz(MdRuntime *rt) { rt->cpu.r[MD_X86_CX] = 300u; }

/* JCXZ skip + LOOP with CX starting at 0 (LOOP wraps to FFFF iterations) */
static const uint8_t kJcxz[] = { 0xE3,0x02, 0x41, 0x41, 0x43, 0xE2,0xFD, 0xF4 };
static void setup_jcxz(MdRuntime *rt) { rt->cpu.r[MD_X86_CX] = 0u; }

/* Shape expectations apply to native Thumb-2 runs only (the host reference
   path executes blocks op by op and reports different fallback counts).
   memloop/regionmix: their first block starts at the setup instructions, and
   the resident loop is built for entry at the loop head, so the very first
   MOV CX is interpreted once; every iteration after that is native.
   M20.3: LODS/ALU/LOOP fixtures expect zero fallback; LODS+LOOP bodies
   expect a resident region. */
/* COMMAND.COM 2.0 transient checksum, real encodings (ADD DX,AX = 03 D0),
   DS != 0:  mov ax,2000h / mov ds,ax / mov si,100h / mov cx,28AFh / cld /
   shr cx,1 / xor dx,dx / L: lodsw / add dx,ax / loop L / hlt */
static const uint8_t kChecksum[] = {
    0xB8,0x00,0x20, 0x8E,0xD8, 0xBE,0x00,0x01, 0xB9,0xAF,0x28, 0xFC, 0xD1,0xE9,
    0x33,0xD2, 0xAD, 0x03,0xD0, 0xE2,0xFB, 0xF4
};
static void setup_checksum(MdRuntime *rt)
{
    unsigned i;
    for (i = 0; i < 0x28B0u; ++i) md_x86_write8(&rt->cpu, 0x2000u, (uint16_t)(0x0100u + i), (uint8_t)(i * 37u + 11u));
}

/* LODSW loop across the 1 MiB boundary: DS=FFFFh, SI=0007h -> linear FFFF7h;
   word reads at FFFFFh take their high byte from 00000h. */
static const uint8_t kWrapLoop[] = { 0xAD, 0x01,0xC2, 0xE2,0xFB, 0xF4 };
static void setup_wrap(MdRuntime *rt)
{
    unsigned i;
    for (i = 0; i < 16u; ++i) rt->cpu.memory[0xFFFF0u + i] = (uint8_t)(0xA0u + i);
    for (i = 0; i < 16u; ++i) rt->cpu.memory[i] = (uint8_t)(0x10u + i);
    rt->cpu.ds = 0xFFFFu;
    rt->cpu.r[MD_X86_SI] = 0x0007u;
    rt->cpu.r[MD_X86_CX] = 8u;
}

/* CX=0 at entry: 65536 iterations, SI wraps at 64 KiB */
static void setup_cx0(MdRuntime *rt)
{
    rt->cpu.ds = 0x3000u;
    rt->cpu.r[MD_X86_SI] = 0xFF00u;
    rt->cpu.r[MD_X86_CX] = 0u;
    {
        unsigned i;
        for (i = 0; i < 0x10000u; i += 97u) md_x86_write8(&rt->cpu, 0x3000u, (uint16_t)i, (uint8_t)i);
    }
}

/* ALU mix: every operand placement, final flags from CMP.
   L: lodsw / xor bx,ax (31 C3: memory dest, resident src) /
      sub dx,si (2B D6: memory dest, resident src) /
      add ax,bx (03 C3: resident dest, memory src) /
      cmp ax,bx (39 D8: resident dest, memory src, flags only) / loop L / hlt */
static const uint8_t kAluMix[] = { 0xAD, 0x31,0xC3, 0x2B,0xD6, 0x03,0xC3, 0x39,0xD8, 0xE2,0xF5, 0xF4 };
static void setup_alumix(MdRuntime *rt)
{
    unsigned i;
    for (i = 0; i < 0x400u; ++i) md_x86_write8(&rt->cpu, rt->cpu.cs, (uint16_t)(0x0400u + i), (uint8_t)(i * 13u + 5u));
    rt->cpu.ds = rt->cpu.cs;
    rt->cpu.r[MD_X86_SI] = 0x0400u;
    rt->cpu.r[MD_X86_CX] = 0x180u;
    rt->cpu.r[MD_X86_BX] = 0x1234u;
    rt->cpu.r[MD_X86_DX] = 0x8001u;
}

static const Fixture kFixtures[] = {
    { "loop",       kLoop,       sizeof(kLoop),       NULL,              0, 1, 0 },
    { "memloop",    kMemloop,    sizeof(kMemloop),    NULL,              1, 1, 0 },
    { "regionmix",  kRegionmix,  sizeof(kRegionmix),  NULL,              1, 1, 0 },
    { "branchmix",  kBranchmix,  sizeof(kBranchmix),  NULL,              0, 1, 0 },
    { "callmix",    kCallmix,    sizeof(kCallmix),    NULL,              0, 0, 1 },
    { "lodsw+loop", kLodsLoop,   sizeof(kLodsLoop),   setup_lods_fwd,    0, 1, 0 },
    { "lodsw+loop std", kLodsLoop, sizeof(kLodsLoop), setup_lods_back,   0, 1, 0 },
    { "lodsb+loopnz", kLodsbScan, sizeof(kLodsbScan), setup_lodsb_scan,  0, 0, 0 },
    { "inc+loopz",  kLoopz,      sizeof(kLoopz),      setup_loopz,       0, 0, 0 },
    { "jcxz+loop",  kJcxz,       sizeof(kJcxz),       setup_jcxz,        0, 0, 0 },
    /* 3 = MOV DS,AX / CLD / SHR CX,1 in the one-time setup (not JIT ops yet) */
    { "checksum",   kChecksum,   sizeof(kChecksum),   setup_checksum,    3, 1, 0 },
    { "lodsw wrap", kWrapLoop,   sizeof(kWrapLoop),   setup_wrap,        0, 1, 0 },
    { "lodsw cx=0", kWrapLoop,   sizeof(kWrapLoop),   setup_cx0,         0, 1, 0 },
    { "alu mix",    kAluMix,     sizeof(kAluMix),     setup_alumix,      0, 1, 0 },
};

static void load(MdRuntime *rt, uint8_t *mem, const Fixture *f)
{
    MdHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    memset(mem, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(rt, mem, &hooks);
    md_runtime_load_com(rt, f->bytes, f->size, 0x1000u);
    if (f->setup != NULL) f->setup(rt);
}

static uint16_t arch_flags(const MdX86 *c)
{
    MdX86 copy = *c;
    return md_x86_flags(&copy);
}

static void run_fixture(const Fixture *f, int verbose)
{
    static MdRuntime a, b;
    static MdJit jit;
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *code = jit_code_buffer();
    MdStopReason sa, sb;
    int same_regs, same_mem;

    if (ma == NULL || mb == NULL || code == NULL) { CHECK(0); free(ma); free(mb); return; }

    load(&a, ma, f);
    sa = md_interp_run(&a, 20000000u);

    load(&b, mb, f);
    memset(code, 0, JIT_CODE_BYTES);
    md_jit_init(&jit, code, JIT_CODE_BYTES);
    sb = md_jit_run(&jit, &b, 20000000u);

    same_regs = memcmp(a.cpu.r, b.cpu.r, sizeof(a.cpu.r)) == 0 &&
                a.cpu.cs == b.cpu.cs && a.cpu.ds == b.cpu.ds && a.cpu.es == b.cpu.es &&
                a.cpu.ss == b.cpu.ss && a.cpu.ip == b.cpu.ip &&
                arch_flags(&a.cpu) == arch_flags(&b.cpu);
    same_mem = memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0;

    if (verbose || !same_regs || !same_mem || sa != sb || a.instructions != b.instructions) {
        printf("[jit-diff] %-15s interp: %s instr=%llu AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X F=%04X\n",
               f->name, md_stop_reason_name(sa), (unsigned long long)a.instructions,
               a.cpu.r[0], a.cpu.r[3], a.cpu.r[1], a.cpu.r[2], a.cpu.r[6], arch_flags(&a.cpu));
        printf("[jit-diff] %-15s jit:    %s instr=%llu AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X F=%04X mem=%s\n",
               f->name, md_stop_reason_name(sb), (unsigned long long)b.instructions,
               b.cpu.r[0], b.cpu.r[3], b.cpu.r[1], b.cpu.r[2], b.cpu.r[6], arch_flags(&b.cpu),
               same_mem ? "same" : "DIFFERENT");
    }
    printf("[jit-diff] %-15s fallback=%llu resident=%llu cfg=%llu control=%llu compiles=%llu  %s\n",
           f->name, (unsigned long long)jit.fallback_instructions,
           (unsigned long long)jit.resident_regions, (unsigned long long)jit.cfg_regions,
           (unsigned long long)jit.control_instructions, (unsigned long long)jit.compiles,
#if defined(__arm__) || defined(__thumb__)
           "(native Thumb-2)"
#else
           "(host reference path)"
#endif
           );

    CHECK(sa == MD_STOP_HALT);
    CHECK(sa == sb);
    CHECK(same_regs);
    CHECK(same_mem);
    CHECK(a.instructions == b.instructions);
#if defined(__arm__) || defined(__thumb__)
    /* M22 changed a single warm-up fallback into a bounded burst. Preserve
       zero-fallback requirements; nonzero allowances admit that one burst. */
    if (f->max_fallback >= 0) {
        const uint64_t limit = (uint64_t)f->max_fallback +
            (f->max_fallback > 0 ? MD_JIT_ZERO_ESCAPE_BURST - 1u : 0u);
        CHECK(jit.fallback_instructions <= limit);
    }
    if (f->want_resident) CHECK(jit.resident_regions >= 1u);
    if (f->want_control) CHECK(jit.control_instructions > 0u);
#endif
    if (failures) fprintf(stderr, "  (in fixture %s)\n", f->name);

    free(ma);
    free(mb);
}

/* Budget slicing: the JIT runs in slices of `slice` instructions; after each
   slice the interpreter advances by exactly the retired count and the CPU
   states must match. Proves that budget exits (including mid-loop exits of
   resident regions) stop on exact architectural instruction boundaries. */
static void run_sliced(const Fixture *f, unsigned slice)
{
    static MdRuntime a, b;
    static MdJit jit;
    uint8_t *ma = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *mb = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    uint8_t *code = jit_code_buffer();
    unsigned long steps = 0, bad = 0;
    if (ma == NULL || mb == NULL || code == NULL) { CHECK(0); free(ma); free(mb); return; }
    load(&a, ma, f);
    load(&b, mb, f);
    memset(code, 0, JIT_CODE_BYTES);
    md_jit_init(&jit, code, JIT_CODE_BYTES);
    while (b.stop_reason == MD_STOP_NONE && steps < 4000000ul) {
        const uint64_t before = b.instructions;
        uint64_t got;
        (void)md_jit_run(&jit, &b, slice);
        if (b.stop_reason == MD_STOP_BUDGET) b.stop_reason = MD_STOP_NONE;
        got = b.instructions - before;
        if (got > slice) ++bad;
        if (got != 0u) {
            (void)md_interp_run(&a, got);
            if (a.stop_reason == MD_STOP_BUDGET) a.stop_reason = MD_STOP_NONE;
        }
        if (memcmp(a.cpu.r, b.cpu.r, sizeof(a.cpu.r)) != 0 || a.cpu.ip != b.cpu.ip ||
            a.cpu.cs != b.cpu.cs || a.cpu.ds != b.cpu.ds ||
            arch_flags(&a.cpu) != arch_flags(&b.cpu) || a.instructions != b.instructions) {
            if (bad < 3u) {
                printf("[jit-slice] %s slice=%u diverged at instr=%llu: interp IP=%04X CX=%04X SI=%04X F=%04X | jit IP=%04X CX=%04X SI=%04X F=%04X\n",
                       f->name, slice, (unsigned long long)b.instructions,
                       a.cpu.ip, a.cpu.r[1], a.cpu.r[6], arch_flags(&a.cpu),
                       b.cpu.ip, b.cpu.r[1], b.cpu.r[6], arch_flags(&b.cpu));
            }
            ++bad;
            break;
        }
        ++steps;
    }
    CHECK(bad == 0u);
    CHECK(b.stop_reason == MD_STOP_HALT);
    CHECK(memcmp(ma, mb, MD_X86_ADDRESS_SPACE) == 0);
    if (bad != 0u) fprintf(stderr, "  (sliced %s, slice=%u)\n", f->name, slice);
    free(ma);
    free(mb);
}

int main(int argc, char **argv)
{
    size_t i;
    const int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    for (i = 0; i < sizeof(kFixtures) / sizeof(kFixtures[0]); ++i) {
        const int before = failures;
        run_fixture(&kFixtures[i], verbose);
        if (failures != before) fprintf(stderr, "[jit-diff] %s: %d check(s) failed\n", kFixtures[i].name, failures - before);
    }
    for (i = 0; i < sizeof(kFixtures) / sizeof(kFixtures[0]); ++i) {
        run_sliced(&kFixtures[i], 7u);
        run_sliced(&kFixtures[i], 50u);
    }
    printf("[jit-slice] budget slicing (7, 50) checked on %zu fixtures\n", sizeof(kFixtures) / sizeof(kFixtures[0]));
    if (failures != 0) {
        fprintf(stderr, "microDOS JIT differential: %d failure(s)\n", failures);
        return 1;
    }
    puts("microDOS JIT differential: ok");
    return 0;
}
