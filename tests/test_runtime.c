#include "microdos/block_cache.h"
#include "microdos/ops.h"
#include "microdos/runtime.h"
#include "hello_recomp.h"
#include "hybrid_recomp.h"
#include "loop_recomp.h"
#include "selfmod_recomp.h"
#include "host_dos.h"
#include "msdos2_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static const uint8_t kHello[] = {
    0xB4,0x09,0xBA,0x0C,0x01,0xCD,0x21,0xB8,0x00,0x4C,0xCD,0x21,
    'H','e','l','l','o',' ','f','r','o','m',' ','m','i','c','r','o','D','O','S','!',13,10,'$'
};
static const uint8_t kLoop[] = {0xB9,0xFF,0xFF,0x49,0x75,0xFD,0xF4};
static const uint8_t kHybrid[] = {0xB8,0x34,0x12,0x74,0x03,0x89,0xC3,0x90,0xF4};
static const uint8_t kPatchAx[] = {0xB8,0x11,0x11,0xF4};

static const uint8_t kSegmentOps[] = {
    0xB8,0x34,0x12,       /* mov ax,1234h */
    0x8E,0xD8,            /* mov ds,ax */
    0x1E,                 /* push ds */
    0xB8,0x78,0x56,       /* mov ax,5678h */
    0x8E,0xC0,            /* mov es,ax */
    0x07,                 /* pop es */
    0x8C,0xC0,            /* mov ax,es */
    0xF4                  /* hlt */
};

static const uint8_t kGroup1Ops[] = {
    0xC7,0x06,0x00,0x02,0x34,0x12, /* mov word [0200h],1234h */
    0xC6,0x06,0x02,0x02,0x80,      /* mov byte [0202h],80h */
    0xB8,0xFF,0x00,                 /* mov ax,00ffh */
    0x33,0x06,0x00,0x02,            /* xor ax,[0200h] -> 12cbh */
    0x83,0xC0,0xFF,                 /* add ax,-1 -> 12cah */
    0x83,0xE8,0x0A,                 /* sub ax,10 -> 12c0h */
    0x80,0x3E,0x02,0x02,0x80,      /* cmp byte [0202h],80h */
    0x74,0x03,                      /* jz good */
    0xB8,0xAD,0xDE,                 /* must be skipped */
    0xF4                            /* good: hlt */
};

static const uint8_t kStringOps[] = {
    0xB8,0x00,0x20,                 /* mov ax,2000h */
    0x8E,0xD8,                      /* mov ds,ax */
    0xB8,0x00,0x30,                 /* mov ax,3000h */
    0x8E,0xC0,                      /* mov es,ax */
    0xBE,0x00,0x01,                 /* mov si,0100h */
    0xBF,0x00,0x02,                 /* mov di,0200h */
    0xB9,0x04,0x00,                 /* mov cx,4 */
    0xFC,                           /* cld */
    0x3E,0xF3,0xA4,                 /* ds: rep movsb (multiple prefixes) */
    0x26,0xC6,0x06,0x02,0x02,0x99,/* mov byte es:[0202h],99h */
    0xBE,0x00,0x01,                 /* mov si,0100h */
    0xBF,0x00,0x02,                 /* mov di,0200h */
    0xB9,0x04,0x00,                 /* mov cx,4 */
    0xF3,0xA6,                      /* repe cmpsb; stop at changed byte */
    0xBE,0x01,0x02,                 /* mov si,0201h */
    0x26,0xAC,                      /* es: lodsb */
    0xBF,0x04,0x02,                 /* mov di,0204h */
    0xAA,                           /* stosb */
    0xFD,                           /* std */
    0xB8,0xEF,0xBE,                 /* mov ax,beefh */
    0xBF,0x08,0x02,                 /* mov di,0208h */
    0xAB,                           /* stosw, backwards */
    0xFC,                           /* cld */
    0xF4                            /* hlt */
};

static const uint8_t kScasOps[] = {
    0xB8,0x00,0x30,                 /* mov ax,3000h */
    0x8E,0xC0,                      /* mov es,ax */
    0xBF,0x00,0x02,                 /* mov di,0200h */
    0xB0,0x99,                      /* mov al,99h */
    0xB9,0x04,0x00,                 /* mov cx,4 */
    0xFC,                           /* cld */
    0xF2,0xAE,                      /* repne scasb */
    0xF4                            /* hlt */
};

static const uint8_t kLoopOps[] = {
    0xB9,0x03,0x00,                 /* mov cx,3 */
    0xB8,0x00,0x00,                 /* mov ax,0 */
    0x40,                           /* again: inc ax */
    0xE2,0xFD,                      /* loop again */
    0xE3,0x03,                      /* jcxz done */
    0xB8,0xAD,0xDE,                 /* must be skipped */
    0xF4                            /* done: hlt */
};

static const uint8_t kLoopzOps[] = {
    0xB9,0x02,0x00,                 /* mov cx,2 */
    0xB8,0x00,0x00,                 /* mov ax,0 */
    0x3D,0x00,0x00,                 /* cmp ax,0 -> ZF=1 */
    0xE1,0xFE,                      /* loopz self */
    0xF4
};

static const uint8_t kLoopnzOps[] = {
    0xB9,0x02,0x00,                 /* mov cx,2 */
    0xB8,0x00,0x00,                 /* mov ax,0 */
    0x3D,0x01,0x00,                 /* cmp ax,1 -> ZF=0 */
    0xE0,0xFE,                      /* loopnz self */
    0xF4
};


static const uint8_t kCoreControlOps[] = {
    0xB0,0x80,                      /* mov al,80h */
    0x98,                           /* cbw -> ax=ff80 */
    0x99,                           /* cwd -> dx=ffff */
    0xF9,                           /* stc */
    0x9C,                           /* pushf */
    0xF8,                           /* clc */
    0x9D,                           /* popf: restore CF */
    0x72,0x03,                      /* jc good */
    0xB8,0xAD,0xDE,                 /* skipped */
    0xFA,                           /* good: cli */
    0xFB,                           /* sti */
    0xA9,0x80,0xFF,                 /* test ax,ff80h */
    0xF4
};

static const uint8_t kAddressingOps[] = {
    0xC5,0x1E,0x00,0x02,            /* lds bx,[0200h] */
    0x2E,0xC4,0x06,0x04,0x02,       /* cs: les ax,[0204h] */
    0x8D,0x36,0x34,0x12,            /* lea si,[1234h] */
    0x93,                           /* xchg ax,bx */
    0x50,                           /* push ax */
    0x8F,0x06,0x08,0x02,            /* pop word [0208h] */
    0xF4
};

static const uint8_t kGroup3Unsigned[] = {
    0xB0,0x12,                      /* mov al,12h */
    0xB3,0x10,                      /* mov bl,10h */
    0xF6,0xE3,                      /* mul bl -> ax=0120h */
    0xBB,0x03,0x00,                 /* mov bx,3 */
    0xF7,0xF3,                      /* div bx -> ax=0060h, dx=0 */
    0xF4
};

static const uint8_t kGroup3Signed[] = {
    0xB0,0xF6,                      /* mov al,-10 */
    0xB3,0x03,                      /* mov bl,3 */
    0xF6,0xEB,                      /* imul bl -> ax=-30 */
    0xF6,0xFB,                      /* idiv bl -> al=-10, ah=0 */
    0xF4
};

static const uint8_t kGroup3Logic[] = {
    0xB8,0x34,0x12,                 /* mov ax,1234h */
    0xF7,0xD0,                      /* not ax -> edcbh */
    0xF7,0xD8,                      /* neg ax -> 1235h */
    0xF7,0xC0,0x35,0x12,            /* test ax,1235h */
    0xF4
};

static const uint8_t kGroup45Memory[] = {
    0xC7,0x06,0x00,0x02,0xFF,0x00, /* mov word [0200h],00ffh */
    0xFF,0x06,0x00,0x02,            /* inc word [0200h] */
    0xFF,0x0E,0x00,0x02,            /* dec word [0200h] */
    0xFF,0x36,0x00,0x02,            /* push word [0200h] */
    0x58,                           /* pop ax */
    0xC6,0x06,0x02,0x02,0x7F,      /* mov byte [0202h],7fh */
    0xFE,0x06,0x02,0x02,            /* inc byte [0202h] */
    0xFE,0x0E,0x02,0x02,            /* dec byte [0202h] */
    0xF4
};

static const uint8_t kNearIndirectCall[] = {
    0xBB,0x0B,0x01,                 /* mov bx,010bh */
    0xB8,0x00,0x00,                 /* mov ax,0 */
    0xFF,0xD3,                      /* call bx */
    0xF4,                           /* return here */
    0x90,0x90,                      /* padding */
    0x40,                           /* 010b: inc ax */
    0xC3                            /* ret */
};

static const uint8_t kNearIndirectJump[] = {
    0xBB,0x08,0x01,                 /* mov bx,0108h */
    0xFF,0xE3,                      /* jmp bx */
    0xB8,0xAD,0xDE,                 /* skipped */
    0xF4                            /* 0108 */
};

static const uint8_t kFarCall[] = {
    0xB8,0x00,0x00,                 /* mov ax,0 */
    0x9A,0x00,0x01,0x00,0x20,       /* call far 2000:0100 */
    0xF4
};

static const uint8_t kIretOps[] = {0xCF,0xF4};
static const uint8_t kPushSpOps[] = {0x54,0x58,0xF4};
static const uint8_t kAamAadOps[] = {0xB0,0x2A,0xD4,0x0A,0xD5,0x0A,0xF4};
static const uint8_t kShiftOps[] = {
    0xB0,0x81,0xD0,0xE0,            /* shl al,1 -> 02h */
    0xB1,0x01,0xD2,0xC8,            /* ror al,cl -> 01h */
    0xB8,0x00,0x80,0xD1,0xF8,       /* sar ax,1 -> c000h */
    0xF4
};

static void test_address_wrap(uint8_t *memory)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.memory = memory;
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_x86_write8(&cpu, 0xFFFFu, 0x0010u, 0xA5u);
    CHECK(memory[0] == 0xA5u);
    CHECK(md_x86_linear(0xFFFFu, 0x0010u) == 0u);
}

static void test_reg_alias(uint8_t *memory)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.memory = memory;
    cpu.r[MD_X86_AX] = 0x1234u;
    CHECK(md_x86_get_reg8(&cpu, 0u) == 0x34u);
    CHECK(md_x86_get_reg8(&cpu, 4u) == 0x12u);
    md_x86_set_reg8(&cpu, 4u, 0xABu);
    CHECK(cpu.r[MD_X86_AX] == 0xAB34u);
}

static MdHooks make_host_hooks(MdHostDos *host)
{
    MdHooks hooks;
    hooks.interrupt = md_host_dos_interrupt;
    hooks.in8 = NULL;
    hooks.out8 = NULL;
    hooks.user = host;
    return hooks;
}

static MdStopReason run_hello_interp(uint8_t *memory, char *out, size_t cap,
                                     uint64_t *instructions)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    MdStopReason stop;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, out, cap);
    hooks = make_host_hooks(&host);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kHello, sizeof(kHello), 0x1000u);
    stop = md_interp_run(&runtime, 1000u);
    *instructions = runtime.instructions;
    CHECK(runtime.exit_code == 0u);
    return stop;
}

static MdStopReason run_hello_cache(uint8_t *memory, char *out, size_t cap,
                                    uint64_t *instructions, MdBlockCache *cache)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    MdStopReason stop;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, out, cap);
    hooks = make_host_hooks(&host);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kHello, sizeof(kHello), 0x1000u);
    md_block_cache_init(cache);
    stop = md_interp_run_cached(&runtime, cache, 1000u);
    *instructions = runtime.instructions;
    CHECK(runtime.exit_code == 0u);
    return stop;
}

static MdStopReason run_hello_aot(uint8_t *memory, char *out, size_t cap,
                                  uint64_t *instructions, MdBlockCache *cache)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    MdStopReason stop;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, out, cap);
    hooks = make_host_hooks(&host);
    md_runtime_init(&runtime, memory, &hooks);
    md_block_cache_init(cache);
    md_runtime_set_block_cache(&runtime, cache);
    stop = md_recomp_hello(&runtime, 0x1000u, 1000u);
    *instructions = runtime.instructions;
    CHECK(runtime.exit_code == 0u);
    return stop;
}

static void test_interp_cache_aot(uint8_t *memory)
{
    char interp_out[256];
    char cache_out[256];
    char aot_out[256];
    uint64_t interp_n = 0u;
    uint64_t cache_n = 0u;
    uint64_t aot_n = 0u;
    MdBlockCache cache;
    MdBlockCache aot_cache;

    CHECK(run_hello_interp(memory, interp_out, sizeof(interp_out), &interp_n) == MD_STOP_EXIT);
    CHECK(run_hello_cache(memory, cache_out, sizeof(cache_out), &cache_n, &cache) == MD_STOP_EXIT);
    CHECK(run_hello_aot(memory, aot_out, sizeof(aot_out), &aot_n, &aot_cache) == MD_STOP_EXIT);
    CHECK(strcmp(interp_out, "Hello from microDOS!\r\n") == 0);
    CHECK(strcmp(interp_out, cache_out) == 0);
    CHECK(strcmp(interp_out, aot_out) == 0);
    CHECK(interp_n == 5u);
    CHECK(cache_n == 5u);
    CHECK(aot_n == 5u);
    CHECK(cache.fallback_instructions == 0u);
    CHECK(aot_cache.fallback_instructions == 0u);
}

static void test_loop_cache(uint8_t *memory)
{
    MdRuntime interp;
    MdRuntime cached;
    MdRuntime aot;
    MdHooks hooks = {0};
    MdBlockCache cache;
    MdBlockCache aot_cache;
    uint64_t expected;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&interp, memory, &hooks);
    md_runtime_load_com(&interp, kLoop, sizeof(kLoop), 0x1000u);
    CHECK(md_interp_run(&interp, 200000u) == MD_STOP_HALT);
    expected = interp.instructions;
    CHECK(expected == 131072u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&cached, memory, &hooks);
    md_runtime_load_com(&cached, kLoop, sizeof(kLoop), 0x1000u);
    md_block_cache_init(&cache);
    CHECK(md_interp_run_cached(&cached, &cache, 200000u) == MD_STOP_HALT);
    CHECK(cached.cpu.r[MD_X86_CX] == 0u);
    CHECK(cached.instructions == expected);
    CHECK(cache.fallback_instructions == 0u);
    CHECK(cache.hits > cache.misses);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&aot, memory, &hooks);
    md_block_cache_init(&aot_cache);
    md_runtime_set_block_cache(&aot, &aot_cache);
    CHECK(md_recomp_loop(&aot, 0x1000u, 200000u) == MD_STOP_HALT);
    CHECK(aot.cpu.r[MD_X86_CX] == 0u);
    CHECK(aot.instructions == expected);
}

static void test_cached_fallback(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    MdBlockCache cache;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kHybrid, sizeof(kHybrid), 0x1000u);
    md_block_cache_init(&cache);
    CHECK(md_interp_run_cached(&runtime, &cache, 100u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x1234u);
    CHECK(runtime.cpu.r[MD_X86_BX] == 0x1234u);
    CHECK(runtime.instructions == 5u);
    CHECK(cache.fallback_instructions == 1u);
}

static void test_page_code_invalidation(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    MdBlockCache cache;
    uint64_t old_decodes;
    uint64_t old_invalidations;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kPatchAx, sizeof(kPatchAx), 0x1000u);
    md_block_cache_init(&cache);
    CHECK(md_interp_run_cached(&runtime, &cache, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x1111u);

    old_decodes = cache.decodes;
    old_invalidations = cache.invalidations;
    md_x86_write16(&runtime.cpu, 0x1000u, 0x0101u, 0x2222u);

    runtime.stop_reason = MD_STOP_NONE;
    runtime.instructions = 0u;
    runtime.cpu.cs = 0x1000u;
    runtime.cpu.ds = 0x1000u;
    runtime.cpu.es = 0x1000u;
    runtime.cpu.ss = 0x1000u;
    runtime.cpu.ip = 0x0100u;
    runtime.cpu.r[MD_X86_SP] = 0xFFFEu;
    CHECK(md_interp_run_cached(&runtime, &cache, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x2222u);
    CHECK(cache.decodes > old_decodes);
    CHECK(cache.invalidations > old_invalidations);
}

static void test_hybrid_aot_cache_handoff(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    MdBlockCache cache;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_block_cache_init(&cache);
    md_runtime_set_block_cache(&runtime, &cache);
    CHECK(md_recomp_hybrid(&runtime, 0x1000u, 100u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x1234u);
    CHECK(runtime.cpu.r[MD_X86_BX] == 0x1234u);
    CHECK(runtime.instructions == 5u);
    CHECK(cache.fallback_instructions == 1u);
    CHECK(cache.decodes >= 1u);
}

static void test_aot_self_modifying_code(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    MdBlockCache cache;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_block_cache_init(&cache);
    md_runtime_set_block_cache(&runtime, &cache);

    CHECK(md_recomp_selfmod(&runtime, 0x1000u, 100u) == MD_STOP_HALT);
    CHECK(runtime.instructions == 3u);
    CHECK(md_x86_read8(&runtime.cpu, 0x1000u, 0x0105u) == 0xF4u);
    CHECK(runtime.cpu.ip == 0x0106u);
    CHECK(cache.decodes >= 1u);
}

static void test_budget(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    MdBlockCache cache;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kLoop, sizeof(kLoop), 0x1000u);
    md_block_cache_init(&cache);
    CHECK(md_interp_run_cached(&runtime, &cache, 3u) == MD_STOP_BUDGET);
    CHECK(runtime.instructions == 3u);
}


static void test_phase_a_flag_semantics(void)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.flags = (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF);
    CHECK(md_x86_adc8(&cpu, 0x7Fu, 0x00u) == 0x80u);
    CHECK((cpu.flags & MD_X86_FLAG_OF) != 0u);
    CHECK((cpu.flags & MD_X86_FLAG_CF) == 0u);
    CHECK((cpu.flags & MD_X86_FLAG_AF) != 0u);
    CHECK((cpu.flags & MD_X86_FLAG_SF) != 0u);

    cpu.flags = (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF);
    CHECK(md_x86_adc16(&cpu, 0xFFFFu, 0x0000u) == 0x0000u);
    CHECK((cpu.flags & MD_X86_FLAG_CF) != 0u);
    CHECK((cpu.flags & MD_X86_FLAG_ZF) != 0u);

    cpu.flags = (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF);
    CHECK(md_x86_sbb8(&cpu, 0x80u, 0x00u) == 0x7Fu);
    CHECK((cpu.flags & MD_X86_FLAG_OF) != 0u);
    CHECK((cpu.flags & MD_X86_FLAG_CF) == 0u);

    cpu.flags = (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF | MD_X86_FLAG_OF | MD_X86_FLAG_AF);
    CHECK(md_x86_logic16(&cpu, (uint16_t)(0x55AAu ^ 0xFFFFu)) == 0xAA55u);
    CHECK((cpu.flags & (MD_X86_FLAG_CF | MD_X86_FLAG_OF | MD_X86_FLAG_AF)) == 0u);
}

static void test_all_jcc_conditions(void)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));

    cpu.flags = MD_X86_FLAG_OF;
    CHECK(md_x86_condition(&cpu, 0x0u));
    CHECK(!md_x86_condition(&cpu, 0x1u));

    cpu.flags = MD_X86_FLAG_CF;
    CHECK(md_x86_condition(&cpu, 0x2u));
    CHECK(!md_x86_condition(&cpu, 0x3u));

    cpu.flags = MD_X86_FLAG_ZF;
    CHECK(md_x86_condition(&cpu, 0x4u));
    CHECK(!md_x86_condition(&cpu, 0x5u));
    CHECK(md_x86_condition(&cpu, 0x6u));
    CHECK(!md_x86_condition(&cpu, 0x7u));

    cpu.flags = MD_X86_FLAG_SF;
    CHECK(md_x86_condition(&cpu, 0x8u));
    CHECK(!md_x86_condition(&cpu, 0x9u));
    CHECK(md_x86_condition(&cpu, 0xCu));
    CHECK(!md_x86_condition(&cpu, 0xDu));
    CHECK(md_x86_condition(&cpu, 0xEu));
    CHECK(!md_x86_condition(&cpu, 0xFu));

    cpu.flags = MD_X86_FLAG_PF;
    CHECK(md_x86_condition(&cpu, 0xAu));
    CHECK(!md_x86_condition(&cpu, 0xBu));

    cpu.flags = 0u;
    CHECK(!md_x86_condition(&cpu, 0x6u));
    CHECK(md_x86_condition(&cpu, 0x7u));
    CHECK(!md_x86_condition(&cpu, 0xEu));
    CHECK(md_x86_condition(&cpu, 0xFu));
}

static void test_segment_register_ops(uint8_t *memory)
{
    MdRuntime runtime;
    MdRuntime cached;
    MdHooks hooks = {0};
    MdBlockCache cache;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kSegmentOps, sizeof(kSegmentOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.instructions == 8u);
    CHECK(runtime.cpu.ds == 0x1234u);
    CHECK(runtime.cpu.es == 0x1234u);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x1234u);
    CHECK(runtime.cpu.r[MD_X86_SP] == 0xFFFEu);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&cached, memory, &hooks);
    md_runtime_load_com(&cached, kSegmentOps, sizeof(kSegmentOps), 0x1000u);
    md_block_cache_init(&cache);
    CHECK(md_interp_run_cached(&cached, &cache, 32u) == MD_STOP_HALT);
    CHECK(cached.instructions == runtime.instructions);
    CHECK(cached.cpu.ds == runtime.cpu.ds);
    CHECK(cached.cpu.es == runtime.cpu.es);
    CHECK(cached.cpu.r[MD_X86_AX] == runtime.cpu.r[MD_X86_AX]);
    CHECK(cache.fallback_instructions != 0u);
}

static void test_group1_mov_imm_xor_and_jcc(uint8_t *memory)
{
    MdRuntime runtime;
    MdRuntime cached;
    MdHooks hooks = {0};
    MdBlockCache cache;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kGroup1Ops, sizeof(kGroup1Ops), 0x1000u);
    CHECK(md_interp_run(&runtime, 64u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x12C0u);
    CHECK(md_x86_read16(&runtime.cpu, 0x1000u, 0x0200u) == 0x1234u);
    CHECK(md_x86_read8(&runtime.cpu, 0x1000u, 0x0202u) == 0x80u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_ZF) != 0u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&cached, memory, &hooks);
    md_runtime_load_com(&cached, kGroup1Ops, sizeof(kGroup1Ops), 0x1000u);
    md_block_cache_init(&cache);
    CHECK(md_interp_run_cached(&cached, &cache, 64u) == MD_STOP_HALT);
    CHECK(cached.instructions == runtime.instructions);
    CHECK(cached.cpu.r[MD_X86_AX] == runtime.cpu.r[MD_X86_AX]);
    CHECK(md_x86_read16(&cached.cpu, 0x1000u, 0x0200u) == 0x1234u);
    CHECK(md_x86_read8(&cached.cpu, 0x1000u, 0x0202u) == 0x80u);
    CHECK(cache.fallback_instructions != 0u);
}

static void test_prefix_string_and_direction_ops(uint8_t *memory)
{
    MdRuntime runtime;
    MdRuntime cached;
    MdHooks hooks = {0};
    MdBlockCache cache;
    unsigned i;
    static const uint8_t source[4] = {0x11u, 0x22u, 0x33u, 0x44u};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kStringOps, sizeof(kStringOps), 0x1000u);
    for (i = 0u; i < 4u; ++i) md_x86_write8(&runtime.cpu, 0x2000u, (uint16_t)(0x0100u + i), source[i]);
    CHECK(md_interp_run(&runtime, 128u) == MD_STOP_HALT);
    CHECK(runtime.instructions == 24u);
    CHECK(runtime.cpu.ds == 0x2000u);
    CHECK(runtime.cpu.es == 0x3000u);
    CHECK(runtime.cpu.r[MD_X86_CX] == 1u);
    CHECK(runtime.cpu.r[MD_X86_SI] == 0x0202u);
    CHECK(runtime.cpu.r[MD_X86_DI] == 0x0206u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_ZF) == 0u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_DF) == 0u);
    CHECK(md_x86_read8(&runtime.cpu, 0x3000u, 0x0200u) == 0x11u);
    CHECK(md_x86_read8(&runtime.cpu, 0x3000u, 0x0201u) == 0x22u);
    CHECK(md_x86_read8(&runtime.cpu, 0x3000u, 0x0202u) == 0x99u);
    CHECK(md_x86_read8(&runtime.cpu, 0x3000u, 0x0203u) == 0x44u);
    CHECK(md_x86_read8(&runtime.cpu, 0x3000u, 0x0204u) == 0x22u);
    CHECK(md_x86_read16(&runtime.cpu, 0x3000u, 0x0208u) == 0xBEEFu);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&cached, memory, &hooks);
    md_runtime_load_com(&cached, kStringOps, sizeof(kStringOps), 0x1000u);
    for (i = 0u; i < 4u; ++i) md_x86_write8(&cached.cpu, 0x2000u, (uint16_t)(0x0100u + i), source[i]);
    md_block_cache_init(&cache);
    CHECK(md_interp_run_cached(&cached, &cache, 128u) == MD_STOP_HALT);
    CHECK(cached.instructions == runtime.instructions);
    CHECK(cached.cpu.r[MD_X86_CX] == runtime.cpu.r[MD_X86_CX]);
    CHECK(cached.cpu.r[MD_X86_SI] == runtime.cpu.r[MD_X86_SI]);
    CHECK(cached.cpu.r[MD_X86_DI] == runtime.cpu.r[MD_X86_DI]);
    CHECK(md_x86_read16(&cached.cpu, 0x3000u, 0x0208u) == 0xBEEFu);
    CHECK(cache.fallback_instructions != 0u);
}

static void test_repne_scas(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kScasOps, sizeof(kScasOps), 0x1000u);
    md_x86_write8(&runtime.cpu, 0x3000u, 0x0200u, 0x11u);
    md_x86_write8(&runtime.cpu, 0x3000u, 0x0201u, 0x22u);
    md_x86_write8(&runtime.cpu, 0x3000u, 0x0202u, 0x99u);
    md_x86_write8(&runtime.cpu, 0x3000u, 0x0203u, 0x44u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.instructions == 8u);
    CHECK(runtime.cpu.r[MD_X86_CX] == 1u);
    CHECK(runtime.cpu.r[MD_X86_DI] == 0x0203u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_ZF) != 0u);
}

static void test_loop_family(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kLoopOps, sizeof(kLoopOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 3u);
    CHECK(runtime.cpu.r[MD_X86_CX] == 0u);
    CHECK(runtime.instructions == 10u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kLoopzOps, sizeof(kLoopzOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_CX] == 0u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_ZF) != 0u);
    CHECK(runtime.instructions == 6u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kLoopnzOps, sizeof(kLoopnzOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_CX] == 0u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_ZF) == 0u);
    CHECK(runtime.instructions == 6u);
}

static void test_shift_rotate_semantics(void)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.flags = MD_X86_FLAG_ALWAYS1;

    CHECK(md_x86_shift8(&cpu, 4u, 0x81u, 1u) == 0x02u);
    CHECK((cpu.flags & MD_X86_FLAG_CF) != 0u);
    CHECK((cpu.flags & MD_X86_FLAG_OF) != 0u);

    cpu.flags = (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF);
    CHECK(md_x86_shift8(&cpu, 2u, 0x80u, 1u) == 0x01u); /* RCL through carry */
    CHECK((cpu.flags & MD_X86_FLAG_CF) != 0u);

    cpu.flags = MD_X86_FLAG_ALWAYS1;
    CHECK(md_x86_shift16(&cpu, 7u, 0x8001u, 1u) == 0xC000u);
    CHECK((cpu.flags & MD_X86_FLAG_CF) != 0u);
    CHECK((cpu.flags & MD_X86_FLAG_OF) == 0u);
    CHECK((cpu.flags & MD_X86_FLAG_SF) != 0u);
}

static void test_m7_control_and_flags(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kCoreControlOps, sizeof(kCoreControlOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 64u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0xFF80u);
    CHECK(runtime.cpu.r[MD_X86_DX] == 0xFFFFu);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_IF) != 0u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_SF) != 0u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_ZF) == 0u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_CF) == 0u); /* TEST clears CF. */
}

static void test_m7_addressing_and_far_loads(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kAddressingOps, sizeof(kAddressingOps), 0x1000u);
    md_x86_write16(&runtime.cpu, 0x1000u, 0x0200u, 0x3456u);
    md_x86_write16(&runtime.cpu, 0x1000u, 0x0202u, 0x2000u);
    md_x86_write16(&runtime.cpu, 0x1000u, 0x0204u, 0x789Au);
    md_x86_write16(&runtime.cpu, 0x1000u, 0x0206u, 0x3000u);
    CHECK(md_interp_run(&runtime, 64u) == MD_STOP_HALT);
    CHECK(runtime.cpu.ds == 0x2000u);
    CHECK(runtime.cpu.es == 0x3000u);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x3456u);
    CHECK(runtime.cpu.r[MD_X86_BX] == 0x789Au);
    CHECK(runtime.cpu.r[MD_X86_SI] == 0x1234u);
    CHECK(md_x86_read16(&runtime.cpu, 0x2000u, 0x0208u) == 0x3456u);
}

static void test_m7_group3(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kGroup3Unsigned, sizeof(kGroup3Unsigned), 0x1000u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x0060u);
    CHECK(runtime.cpu.r[MD_X86_DX] == 0u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kGroup3Signed, sizeof(kGroup3Signed), 0x1000u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x00F6u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kGroup3Logic, sizeof(kGroup3Logic), 0x1000u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x1235u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_ZF) == 0u);
}

static void test_m7_group45_and_indirect_control(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kGroup45Memory, sizeof(kGroup45Memory), 0x1000u);
    CHECK(md_interp_run(&runtime, 64u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x00FFu);
    CHECK(md_x86_read16(&runtime.cpu, 0x1000u, 0x0200u) == 0x00FFu);
    CHECK(md_x86_read8(&runtime.cpu, 0x1000u, 0x0202u) == 0x7Fu);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kNearIndirectCall, sizeof(kNearIndirectCall), 0x1000u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 1u);
    CHECK(runtime.cpu.r[MD_X86_SP] == 0xFFFEu);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kNearIndirectJump, sizeof(kNearIndirectJump), 0x1000u);
    CHECK(md_interp_run(&runtime, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0u);
}

static void test_m7_far_call_iret_and_push_sp(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kFarCall, sizeof(kFarCall), 0x1000u);
    md_x86_write8(&runtime.cpu, 0x2000u, 0x0100u, 0x40u); /* inc ax */
    md_x86_write8(&runtime.cpu, 0x2000u, 0x0101u, 0xCBu); /* retf */
    CHECK(md_interp_run(&runtime, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 1u);
    CHECK(runtime.cpu.cs == 0x1000u);
    CHECK(runtime.cpu.r[MD_X86_SP] == 0xFFFEu);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kIretOps, sizeof(kIretOps), 0x1000u);
    runtime.cpu.r[MD_X86_SP] = 0xFFF8u;
    md_x86_write16(&runtime.cpu, 0x1000u, 0xFFF8u, 0x0101u);
    md_x86_write16(&runtime.cpu, 0x1000u, 0xFFFAu, 0x1000u);
    md_x86_write16(&runtime.cpu, 0x1000u, 0xFFFCu,
                   (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF | MD_X86_FLAG_IF));
    CHECK(md_interp_run(&runtime, 4u) == MD_STOP_HALT);
    CHECK((runtime.cpu.flags & (MD_X86_FLAG_CF | MD_X86_FLAG_IF)) ==
          (MD_X86_FLAG_CF | MD_X86_FLAG_IF));
    CHECK(runtime.cpu.r[MD_X86_SP] == 0xFFFEu);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kPushSpOps, sizeof(kPushSpOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 8u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0xFFFCu); /* original 8086 PUSH SP quirk */
    CHECK(runtime.cpu.r[MD_X86_SP] == 0xFFFEu);
}

static void test_m7_shift_and_adjust(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};

    test_shift_rotate_semantics();

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kShiftOps, sizeof(kShiftOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 32u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0xC000u);
    CHECK((runtime.cpu.flags & MD_X86_FLAG_SF) != 0u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kAamAadOps, sizeof(kAamAadOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_AX] == 0x002Au);
}



static void test_relative_jump_fetch_sequencing(uint8_t *memory)
{
    /* The released MS-DOS 2.0 image starts E9 78 3E, which must land at 3E7B:
       target = IP-after-immediate (0003) + 3E78. Keep this exact case because
       combining cpu->ip and md_fetch16() in one C expression is unsequenced. */
    static const uint8_t kDosEntryJump[] = {0xE9u,0x78u,0x3Eu};
    static const uint8_t kShortJump[] = {0xEBu,0x02u,0x90u,0x90u,0xF4u};
    MdRuntime runtime;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_raw(&runtime, kDosEntryJump, sizeof(kDosEntryJump), 0x1000u, 0u);
    CHECK(md_interp_step(&runtime) == MD_STOP_NONE);
    CHECK(runtime.cpu.ip == 0x3E7Bu);
    CHECK(runtime.instructions == 1u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_raw(&runtime, kShortJump, sizeof(kShortJump), 0x1000u, 0x0100u);
    CHECK(md_interp_run(&runtime, 4u) == MD_STOP_HALT);
    CHECK(runtime.cpu.ip == 0x0105u);
    CHECK(runtime.instructions == 2u); /* JMP + HLT */
}

static void test_raw_loader_and_msdos2_bootstrap(uint8_t *memory)
{
    /* A one-instruction synthetic "kernel" that immediately RETFs back to the
       synthetic SYSINIT return trampoline. This tests the raw loader, OEM
       register contract, device-chain construction, and far-return framing
       without requiring Microsoft's binary in the normal unit-test suite. */
    static const uint8_t kReturnKernel[] = {0xCBu};
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_msdos2_boot_init(&boot);
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_prepare_cpu(&runtime, &boot, kReturnKernel,
                               sizeof(kReturnKernel));

    CHECK(runtime.cpu.cs == boot.dos_segment);
    CHECK(runtime.cpu.ip == 0u);
    CHECK(runtime.cpu.ds == boot.bios_segment);
    CHECK(runtime.cpu.r[MD_X86_SI] == MD_MSDOS2_CON_OFFSET);
    CHECK(runtime.cpu.r[MD_X86_DX] == boot.memory_paragraphs);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment,
                        MD_MSDOS2_DISK_OFFSET + 4u) == 0x2000u);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment,
                        MD_MSDOS2_BPB_OFFSET) == 512u);

    CHECK(md_interp_run(&runtime, 8u) == MD_STOP_HALT);
    CHECK(boot.returned_from_dosinit);
    CHECK(runtime.instructions == 2u); /* RETF + native return INT */
}


static void test_msdos2_native_device_init(uint8_t *memory)
{
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdHooks hooks = {0};
    const uint16_t req_seg = 0x1200u;
    const uint16_t req_off = 0x0200u;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_msdos2_boot_init(&boot);
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_install_devices(&runtime, &boot);

    runtime.cpu.ds = boot.bios_segment;
    runtime.cpu.r[MD_X86_SI] = MD_MSDOS2_DISK_OFFSET;
    runtime.cpu.es = req_seg;
    runtime.cpu.r[MD_X86_BX] = req_off;
    md_x86_write8(&runtime.cpu, req_seg, req_off + 0u, 26u);
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 0u);

    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
    CHECK(md_x86_read8(&runtime.cpu, req_seg, req_off + 13u) == 1u);
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 18u) == MD_MSDOS2_BPB_TABLE_OFFSET);
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 20u) == boot.bios_segment);
    CHECK(boot.init_calls == 1u);
    CHECK(boot.unknown_device_calls == 0u);
}


typedef struct TestConsoleCapture {
    uint8_t data[64];
    size_t size;
    uint8_t peek_value;
    bool have_peek;
    const uint8_t *read_data;
    size_t read_size;
    size_t read_pos;
    unsigned flushes;
} TestConsoleCapture;

static void test_console_write_cb(void *user, const uint8_t *data, size_t size)
{
    TestConsoleCapture *cap = (TestConsoleCapture *)user;
    size_t room = sizeof(cap->data) - cap->size;
    if (size > room) size = room;
    memcpy(cap->data + cap->size, data, size);
    cap->size += size;
}

static bool test_console_peek_cb(void *user, uint8_t *value)
{
    TestConsoleCapture *cap = (TestConsoleCapture *)user;
    if (!cap->have_peek) return false;
    *value = cap->peek_value;
    return true;
}

static bool test_console_read_cb(void *user, uint8_t *value)
{
    TestConsoleCapture *cap = (TestConsoleCapture *)user;
    if (cap->read_pos >= cap->read_size) return false;
    *value = cap->read_data[cap->read_pos++];
    return true;
}

static void test_console_flush_cb(void *user)
{
    TestConsoleCapture *cap = (TestConsoleCapture *)user;
    ++cap->flushes;
    cap->have_peek = false;
}

static void test_msdos2_console_device_contract(uint8_t *memory)
{
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdHooks hooks = {0};
    TestConsoleCapture cap;
    const uint16_t req_seg = 0x1200u;
    const uint16_t req_off = 0x0200u;
    const uint16_t data_seg = 0x1300u;
    const uint16_t data_off = 0x0040u;
    static const uint8_t text[] = {'D','O','S','!','\r','\n'};
    unsigned i;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    memset(&cap, 0, sizeof(cap));
    md_msdos2_boot_init(&boot);
    boot.console.write = test_console_write_cb;
    boot.console.peek = test_console_peek_cb;
    boot.console.read = test_console_read_cb;
    boot.console.flush = test_console_flush_cb;
    boot.console.user = &cap;
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_install_devices(&runtime, &boot);

    runtime.cpu.ds = boot.bios_segment;
    runtime.cpu.r[MD_X86_SI] = MD_MSDOS2_CON_OFFSET;
    runtime.cpu.es = req_seg;
    runtime.cpu.r[MD_X86_BX] = req_off;

    /* Function 5: no character pending is BUSY|DONE, not error 8103. */
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 5u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0300u);
    CHECK(boot.unknown_device_calls == 0u);
    CHECK(boot.console_poll_calls == 1u);

    /* Non-destructive input returns the pending character in byte 13. */
    cap.have_peek = true;
    cap.peek_value = (uint8_t)'X';
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 5u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
    CHECK(md_x86_read8(&runtime.cpu, req_seg, req_off + 13u) == (uint8_t)'X');

    /* Function 4: destructive read consumes exactly COUNT bytes. */
    {
        static const uint8_t input[] = {'V','E','R','\r'};
        cap.read_data = input;
        cap.read_size = sizeof(input);
        cap.read_pos = 0u;
        md_x86_write16(&runtime.cpu, req_seg, req_off + 14u, data_off);
        md_x86_write16(&runtime.cpu, req_seg, req_off + 16u, data_seg);
        md_x86_write16(&runtime.cpu, req_seg, req_off + 18u, (uint16_t)sizeof(input));
        md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 4u);
        CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
        CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
        CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
        for (i = 0u; i < sizeof(input); ++i) {
            CHECK(md_x86_read8(&runtime.cpu, data_seg, (uint16_t)(data_off + i)) == input[i]);
        }
        CHECK(boot.console_read_calls == 1u);
        CHECK(boot.console_bytes_read == sizeof(input));
    }

    /* Function 10: console output status is immediately ready. */
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 10u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);

    /* Function 8: transfer address/count are the DOS 2 DRDWR request layout. */
    for (i = 0u; i < sizeof(text); ++i) {
        md_x86_write8(&runtime.cpu, data_seg, (uint16_t)(data_off + i), text[i]);
    }
    md_x86_write16(&runtime.cpu, req_seg, req_off + 14u, data_off);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 16u, data_seg);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 18u, (uint16_t)sizeof(text));
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 8u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
    CHECK(cap.size == sizeof(text));
    CHECK(memcmp(cap.data, text, sizeof(text)) == 0);
    CHECK(boot.console_write_calls == 1u);
    CHECK(boot.console_bytes_written == sizeof(text));

    /* M12.1 diagnostic latch records the first literal '$' reaching CON,
       including the transfer location and the far device-call return frame. */
    runtime.cpu.ss = 0x1400u;
    runtime.cpu.r[MD_X86_SP] = 0x0100u;
    md_x86_write16(&runtime.cpu, runtime.cpu.ss, 0x0100u, 0x4567u);
    md_x86_write16(&runtime.cpu, runtime.cpu.ss, 0x0102u, 0x2345u);
    md_x86_write8(&runtime.cpu, data_seg, data_off, (uint8_t)'$');
    md_x86_write16(&runtime.cpu, req_seg, req_off + 14u, data_off);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 16u, data_seg);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 18u, 1u);
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 8u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(boot.console_dollar_writes == 1u);
    CHECK(boot.console_first_dollar_valid);
    CHECK(boot.console_first_dollar_data_segment == data_seg);
    CHECK(boot.console_first_dollar_data_offset == data_off);
    CHECK(boot.console_first_dollar_count == 1u);
    CHECK(boot.console_first_dollar_request_segment == req_seg);
    CHECK(boot.console_first_dollar_request_offset == req_off);
    CHECK(boot.console_first_dollar_return_segment == 0x2345u);
    CHECK(boot.console_first_dollar_return_offset == 0x4567u);

    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 7u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(cap.flushes == 1u);
    CHECK(!cap.have_peek);
    CHECK(boot.unknown_device_calls == 0u);
}



static void test_msdos2_clock_device_contract(uint8_t *memory)
{
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdHooks hooks = {0};
    const uint16_t req_seg = 0x1200u;
    const uint16_t req_off = 0x0200u;
    const uint16_t data_seg = 0x1300u;
    const uint16_t data_off = 0x0040u;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_msdos2_boot_init(&boot);
    boot.clock_days = 17000u;
    boot.clock_hours = 12u;
    boot.clock_minutes = 34u;
    boot.clock_seconds = 56u;
    boot.clock_hundredths = 78u;
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_install_devices(&runtime, &boot);

    runtime.cpu.r[MD_X86_SI] = MD_MSDOS2_CLOCK_OFFSET;
    runtime.cpu.es = req_seg;
    runtime.cpu.r[MD_X86_BX] = req_off;
    md_x86_write16(&runtime.cpu, req_seg, req_off + 14u, data_off);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 16u, data_seg);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 18u, 6u);
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 4u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
    CHECK(md_x86_read16(&runtime.cpu, data_seg, data_off) == 17000u);
    CHECK(md_x86_read8(&runtime.cpu, data_seg, data_off + 2u) == 34u);
    CHECK(md_x86_read8(&runtime.cpu, data_seg, data_off + 3u) == 12u);
    CHECK(md_x86_read8(&runtime.cpu, data_seg, data_off + 4u) == 78u);
    CHECK(md_x86_read8(&runtime.cpu, data_seg, data_off + 5u) == 56u);
    CHECK(boot.clock_read_calls == 1u);

    md_x86_write16(&runtime.cpu, data_seg, data_off, 18000u);
    md_x86_write8(&runtime.cpu, data_seg, data_off + 2u, 2u);
    md_x86_write8(&runtime.cpu, data_seg, data_off + 3u, 1u);
    md_x86_write8(&runtime.cpu, data_seg, data_off + 4u, 4u);
    md_x86_write8(&runtime.cpu, data_seg, data_off + 5u, 3u);
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 8u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(boot.clock_days == 18000u);
    CHECK(boot.clock_hours == 1u);
    CHECK(boot.clock_minutes == 2u);
    CHECK(boot.clock_seconds == 3u);
    CHECK(boot.clock_hundredths == 4u);
    CHECK(boot.clock_write_calls == 1u);
    CHECK(boot.unknown_device_calls == 0u);
}

typedef struct TestDiskImage {
    uint8_t data[4u * 512u];
    unsigned reads;
    unsigned writes;
} TestDiskImage;

static bool test_disk_read_cb(void *user, uint32_t sector, uint8_t *data, size_t size)
{
    TestDiskImage *disk = (TestDiskImage *)user;
    if (size != 512u || sector >= 4u) return false;
    memcpy(data, disk->data + (size_t)sector * 512u, 512u);
    ++disk->reads;
    return true;
}

static bool test_disk_write_cb(void *user, uint32_t sector, const uint8_t *data, size_t size)
{
    TestDiskImage *disk = (TestDiskImage *)user;
    if (size != 512u || sector >= 4u) return false;
    memcpy(disk->data + (size_t)sector * 512u, data, 512u);
    ++disk->writes;
    return true;
}

static void test_msdos2_disk_device_contract(uint8_t *memory)
{
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdHooks hooks = {0};
    TestDiskImage disk;
    const uint16_t req_seg = 0x1200u;
    const uint16_t req_off = 0x0200u;
    const uint16_t data_seg = 0x1300u;
    const uint16_t data_off = 0x0100u;
    unsigned i;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    memset(&disk, 0, sizeof(disk));
    for (i = 0u; i < sizeof(disk.data); ++i) disk.data[i] = (uint8_t)(i ^ (i >> 8));

    md_msdos2_boot_init(&boot);
    boot.disk.read = test_disk_read_cb;
    boot.disk.write = test_disk_write_cb;
    boot.disk.user = &disk;
    boot.disk.sector_size = 512u;
    boot.disk.sector_count = 4u;
    boot.disk.writable = true;
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_install_devices(&runtime, &boot);

    runtime.cpu.ds = boot.bios_segment;
    runtime.cpu.r[MD_X86_SI] = MD_MSDOS2_DISK_OFFSET;
    runtime.cpu.es = req_seg;
    runtime.cpu.r[MD_X86_BX] = req_off;
    md_x86_write8(&runtime.cpu, req_seg, req_off + 1u, 0u);

    /* MEDIA CHECK: a fixed image is unchanged. */
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 1u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
    CHECK(md_x86_read8(&runtime.cpu, req_seg, req_off + 14u) == 1u);

    /* BUILD BPB returns the same BPB installed during INIT. */
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 2u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 18u) == MD_MSDOS2_BPB_OFFSET);
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 20u) == boot.bios_segment);

    /* READ two sectors starting at LBA 1 into guest memory. */
    md_x86_write16(&runtime.cpu, req_seg, req_off + 14u, data_off);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 16u, data_seg);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 18u, 2u);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 20u, 1u);
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 4u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
    CHECK(disk.reads == 2u);
    for (i = 0u; i < 1024u; ++i) {
        CHECK(md_x86_read8(&runtime.cpu, data_seg, (uint16_t)(data_off + i)) == disk.data[512u + i]);
    }

    /* WRITE one sector back through the same DOS 2 request layout. */
    for (i = 0u; i < 512u; ++i) {
        md_x86_write8(&runtime.cpu, data_seg, (uint16_t)(data_off + i), (uint8_t)(0xA5u ^ i));
    }
    md_x86_write16(&runtime.cpu, req_seg, req_off + 18u, 1u);
    md_x86_write16(&runtime.cpu, req_seg, req_off + 20u, 3u);
    md_x86_write8(&runtime.cpu, req_seg, req_off + 2u, 8u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_STRATEGY_INT, &boot));
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_DEVICE_INT, &boot));
    CHECK(md_x86_read16(&runtime.cpu, req_seg, req_off + 3u) == 0x0100u);
    CHECK(disk.writes == 1u);
    for (i = 0u; i < 512u; ++i) CHECK(disk.data[3u * 512u + i] == (uint8_t)(0xA5u ^ i));

    CHECK(boot.disk_media_checks == 1u);
    CHECK(boot.disk_bpb_calls == 1u);
    CHECK(boot.disk_read_calls == 1u);
    CHECK(boot.disk_sectors_read == 2u);
    CHECK(boot.disk_write_calls == 1u);
    CHECK(boot.disk_sectors_written == 1u);
    CHECK(boot.unknown_device_calls == 0u);
}

static void test_msdos2_postinit_continuation(uint8_t *memory)
{
    MdRuntime runtime;
    MdMsdos2Boot boot;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_msdos2_boot_init(&boot);
    boot.continue_after_dosinit = true;
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_install_devices(&runtime, &boot);

    runtime.cpu.cs = boot.bios_segment;
    runtime.cpu.ip = (uint16_t)(MD_MSDOS2_RETURN_OFFSET + 2u);
    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_RETURN_INT, &boot));
    CHECK(boot.returned_from_dosinit);
    CHECK(boot.postinit_started);
    CHECK(runtime.stop_reason == MD_STOP_NONE);
    CHECK(runtime.cpu.cs == boot.bios_segment);
    CHECK(runtime.cpu.ip == MD_MSDOS2_POSTINIT_OFFSET);

    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_OFFSET + 0u) == 0x0Eu);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_OFFSET + 1u) == 0x1Fu);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_OFFSET + 2u) == 0x0Eu);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_OFFSET + 3u) == 0x07u);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_PATH_OFFSET) == (uint8_t)'A');

    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment, MD_MSDOS2_EXEC_BLOCK_OFFSET + 0u) == 0u);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment, MD_MSDOS2_EXEC_BLOCK_OFFSET + 2u) == MD_MSDOS2_COMMAND_TAIL_OFFSET);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment, MD_MSDOS2_EXEC_BLOCK_OFFSET + 4u) == boot.bios_segment);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment, MD_MSDOS2_EXEC_BLOCK_OFFSET + 6u) == MD_MSDOS2_FCB1_OFFSET);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment, MD_MSDOS2_EXEC_BLOCK_OFFSET + 8u) == boot.bios_segment);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment, MD_MSDOS2_EXEC_BLOCK_OFFSET + 10u) == MD_MSDOS2_FCB2_OFFSET);
    CHECK(md_x86_read16(&runtime.cpu, boot.bios_segment, MD_MSDOS2_EXEC_BLOCK_OFFSET + 12u) == boot.bios_segment);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_COMMAND_TAIL_OFFSET + 0u) == 2u);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_COMMAND_TAIL_OFFSET + 1u) == (uint8_t)'/');
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_COMMAND_TAIL_OFFSET + 2u) == (uint8_t)'P');
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_COMMAND_TAIL_OFFSET + 3u) == 0x0Du);
}

typedef struct TestPostinitEnv {
    MdMsdos2Boot boot;
    unsigned execs;
} TestPostinitEnv;

static bool test_postinit_interrupt_hook(MdRuntime *runtime, uint8_t vector, void *user)
{
    TestPostinitEnv *env = (TestPostinitEnv *)user;
    MdX86 *cpu = &runtime->cpu;

    if (vector == 0x21u) {
        const uint8_t ah = md_x86_get_reg8(cpu, 4u);
        const uint8_t al = md_x86_get_reg8(cpu, 0u);
        if (ah == 0x4Bu && al == 0u) {
            const uint16_t seg = env->boot.bios_segment;
            CHECK(cpu->ds == seg);
            CHECK(cpu->es == seg);
            CHECK(cpu->r[MD_X86_DX] == MD_MSDOS2_POSTINIT_PATH_OFFSET);
            CHECK(cpu->r[MD_X86_BX] == MD_MSDOS2_EXEC_BLOCK_OFFSET);
            CHECK(md_x86_read8(cpu, seg, MD_MSDOS2_POSTINIT_PATH_OFFSET) == (uint8_t)'A');
            CHECK(md_x86_read16(cpu, seg, MD_MSDOS2_EXEC_BLOCK_OFFSET) == 0u);
            CHECK(md_x86_read16(cpu, seg, MD_MSDOS2_EXEC_BLOCK_OFFSET + 2u) == MD_MSDOS2_COMMAND_TAIL_OFFSET);
            CHECK(md_x86_read16(cpu, seg, MD_MSDOS2_EXEC_BLOCK_OFFSET + 4u) == seg);
            CHECK(md_x86_read8(cpu, seg, MD_MSDOS2_COMMAND_TAIL_OFFSET) == 2u);
            CHECK(md_x86_read8(cpu, seg, MD_MSDOS2_COMMAND_TAIL_OFFSET + 1u) == (uint8_t)'/');
            CHECK(md_x86_read8(cpu, seg, MD_MSDOS2_COMMAND_TAIL_OFFSET + 2u) == (uint8_t)'P');
            CHECK(md_x86_read8(cpu, seg, MD_MSDOS2_COMMAND_TAIL_OFFSET + 3u) == 0x0Du);

            /* Simulate DOS EXEC transferring to a freshly built COM PSP. */
            md_x86_write8(cpu, 0x3000u, 0x0000u, 0xCDu);
            md_x86_write8(cpu, 0x3000u, 0x0001u, 0x20u);
            md_x86_write8(cpu, 0x3000u, 0x0080u, 2u);
            md_x86_write8(cpu, 0x3000u, 0x0081u, (uint8_t)'/');
            md_x86_write8(cpu, 0x3000u, 0x0082u, (uint8_t)'P');
            md_x86_write8(cpu, 0x3000u, 0x0083u, 0x0Du);
            md_x86_write8(cpu, 0x3000u, MD_MSDOS2_COMMAND_ENTRY_OFFSET, 0xF4u);
            cpu->cs = 0x3000u;
            cpu->ds = 0x3000u;
            cpu->es = 0x3000u;
            cpu->ss = 0x3000u;
            cpu->ip = MD_MSDOS2_COMMAND_ENTRY_OFFSET;
            cpu->r[MD_X86_SP] = 0xFFFEu;
            cpu->flags &= (uint16_t)~MD_X86_FLAG_CF;
            ++env->execs;
            return true;
        }
        cpu->flags |= MD_X86_FLAG_CF;
        cpu->r[MD_X86_AX] = 1u;
        return true;
    }

    return md_msdos2_boot_interrupt(runtime, vector, &env->boot);
}

static void test_msdos2_postinit_program_execution(uint8_t *memory)
{
    MdRuntime runtime;
    TestPostinitEnv env;
    MdHooks hooks = {0};
    MdStopReason reason;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    memset(&env, 0, sizeof(env));
    md_msdos2_boot_init(&env.boot);
    env.boot.continue_after_dosinit = true;
    hooks.interrupt = test_postinit_interrupt_hook;
    hooks.user = &env;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_install_devices(&runtime, &env.boot);
    runtime.cpu.ss = env.boot.stack_segment;
    runtime.cpu.r[MD_X86_SP] = 0xFFFEu;
    runtime.cpu.flags = MD_X86_FLAG_ALWAYS1;

    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_RETURN_INT, &env.boot));
    reason = md_interp_run(&runtime, 100u);
    CHECK(reason == MD_STOP_HALT);
    CHECK(env.execs == 1u);
    CHECK(runtime.cpu.cs == 0x3000u);
    CHECK(runtime.cpu.ds == 0x3000u);
    CHECK(runtime.cpu.es == 0x3000u);
    CHECK(runtime.cpu.ss == 0x3000u);
    CHECK(runtime.cpu.ip == (uint16_t)(MD_MSDOS2_COMMAND_ENTRY_OFFSET + 1u));
}


static void test_ivt(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    runtime.cpu.cs = 0x1234u;
    runtime.cpu.ip = 0x5678u;
    runtime.cpu.ss = 0x2000u;
    runtime.cpu.r[MD_X86_SP] = 0x1000u;
    runtime.cpu.flags = MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_IF | MD_X86_FLAG_TF;
    md_x86_write16_linear(&runtime.cpu, 0x30u * 4u, 0x1111u);
    md_x86_write16_linear(&runtime.cpu, 0x30u * 4u + 2u, 0x2222u);
    CHECK(!md_runtime_interrupt(&runtime, 0x30u));
    CHECK(runtime.cpu.cs == 0x2222u);
    CHECK(runtime.cpu.ip == 0x1111u);
    CHECK(runtime.cpu.r[MD_X86_SP] == 0x0FFAu);
    CHECK(md_x86_read16(&runtime.cpu, 0x2000u, 0x0FFAu) == 0x5678u);
}

int main(void)
{
    uint8_t *memory = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    if (memory == NULL) return 2;

    test_address_wrap(memory);
    test_reg_alias(memory);
    test_interp_cache_aot(memory);
    test_loop_cache(memory);
    test_cached_fallback(memory);
    test_page_code_invalidation(memory);
    test_hybrid_aot_cache_handoff(memory);
    test_aot_self_modifying_code(memory);
    test_budget(memory);
    test_phase_a_flag_semantics();
    test_all_jcc_conditions();
    test_segment_register_ops(memory);
    test_group1_mov_imm_xor_and_jcc(memory);
    test_prefix_string_and_direction_ops(memory);
    test_repne_scas(memory);
    test_loop_family(memory);
    test_m7_control_and_flags(memory);
    test_m7_addressing_and_far_loads(memory);
    test_m7_group3(memory);
    test_m7_group45_and_indirect_control(memory);
    test_m7_far_call_iret_and_push_sp(memory);
    test_m7_shift_and_adjust(memory);
    test_relative_jump_fetch_sequencing(memory);
    test_raw_loader_and_msdos2_bootstrap(memory);
    test_msdos2_native_device_init(memory);
    test_msdos2_console_device_contract(memory);
    test_msdos2_clock_device_contract(memory);
    test_msdos2_disk_device_contract(memory);
    test_msdos2_postinit_continuation(memory);
    test_msdos2_postinit_program_execution(memory);
    test_ivt(memory);

    free(memory);
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("microDOS runtime + DOS 2 phase-C core tests passed");
    return 0;
}
