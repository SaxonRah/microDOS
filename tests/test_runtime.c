#include "microdos/block_cache.h"
#include "microdos/ops.h"
#include "microdos/runtime.h"
#include "hello_recomp.h"
#include "hybrid_recomp.h"
#include "loop_recomp.h"
#include "selfmod_recomp.h"
#include "memloop_recomp.h"
#include "dos2test_recomp.h"
#include "xchgself_recomp.h"
#include "divfault_recomp.h"
#include "eager_flags_ref.h"
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
    CHECK(cache.region_entries == 1u);
    CHECK(cache.region_instructions == expected - 4u); /* MOV CX + first DEC/JNZ + HLT stay outside the resident region */

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

/* M13: attach-mode guard is byte-exact. hello.com is code at 0100-010B and
   a string at 010C-0122; writing the string must not disable compiled code,
   writing an instruction byte must. */
static void test_aot_guard_byte_exact(uint8_t *memory)
{
    MdRuntime runtime;
    MdHooks hooks = {0};
    const MdAotProgram *prog = &md_recomp_hello_program;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);

    CHECK(!prog->attach(&runtime, 0x2000u));             /* nothing loaded yet */
    CHECK(!prog->ready(&runtime, 0x2000u));

    /* Load the real bytes the way DOS would (tracked writes), then attach. */
    {
        static const uint8_t hello[] = {
            0xB4, 0x09, 0xBA, 0x0C, 0x01, 0xCD, 0x21, 0xB8, 0x00, 0x4C, 0xCD, 0x21,
            'H','e','l','l','o',' ','f','r','o','m',' ','m','i','c','r','o','D','O','S','!',
            0x0D, 0x0A, '$'
        };
        unsigned i;
        for (i = 0; i < sizeof(hello); ++i) md_x86_write8(&runtime.cpu, 0x2000u, (uint16_t)(0x0100u + i), hello[i]);
    }
    CHECK(prog->attach(&runtime, 0x2000u));
    CHECK(prog->ready(&runtime, 0x2000u));
    CHECK(prog->is_entry(0x0100u));
    CHECK(!prog->is_entry(0x0101u));
    CHECK(prog->compiled_instructions == 5u);
    CHECK(prog->hole_instructions == 0u);

    md_x86_write8(&runtime.cpu, 0x2000u, 0x0110u, 'X');   /* data byte */
    CHECK(prog->ready(&runtime, 0x2000u));
    CHECK(prog->block_ok(&runtime, 0x2000u, 0x0100u));

    /* Enter compiled code at 0100: MOV AH,9 / MOV DX,010C / INT 21h. With an
       empty IVT the INT leaves the segment, so enter() returns NONE after
       exactly three compiled instructions. */
    runtime.cpu.cs = 0x2000u;
    runtime.cpu.ip = 0x0100u;
    runtime.cpu.ss = 0x3000u;
    runtime.cpu.r[MD_X86_SP] = 0xFFFEu;
    CHECK(prog->enter(&runtime, 100u) == MD_STOP_NONE);
    CHECK(runtime.aot_instructions == 3u);
    CHECK(runtime.cpu.r[MD_X86_DX] == 0x010Cu);
    CHECK(md_x86_get_reg8(&runtime.cpu, 4u) == 0x09u);

    md_x86_write8(&runtime.cpu, 0x2000u, 0x0102u, 0x90u);   /* instruction byte */
    CHECK(!prog->block_ok(&runtime, 0x2000u, 0x0100u));
    runtime.cpu.cs = 0x2000u;
    runtime.cpu.ip = 0x0100u;
    {
        const uint64_t before = runtime.aot_instructions;
        CHECK(prog->enter(&runtime, 100u) == MD_STOP_NONE);
        CHECK(runtime.aot_instructions == before);         /* refused */
    }
}

/* hello.com bytes: code 0100-010B, '$'-terminated string 010C-0122. */
static const uint8_t kHelloImage[] = {
    0xB4, 0x09, 0xBA, 0x0C, 0x01, 0xCD, 0x21, 0xB8, 0x00, 0x4C, 0xCD, 0x21,
    'H','e','l','l','o',' ','f','r','o','m',' ','m','i','c','r','o','D','O','S','!',
    0x0D, 0x0A, '$'
};

static void test_load_hello(MdRuntime *rt, uint16_t segment)
{
    unsigned i;
    for (i = 0; i < sizeof(kHelloImage); ++i) {
        md_x86_write8(&rt->cpu, segment, (uint16_t)(0x0100u + i), kHelloImage[i]);
    }
}

/* M15 BUG 1: a word store must be checked byte by byte against AOT guards,
   including when both bytes sit on the same 4 KiB page. */
static void test_aot_guard_word_writes(uint8_t *memory)
{
    MdRuntime rt;
    MdHooks hooks = {0};
    const MdAotProgram *prog = &md_recomp_hello_program;

    /* data (PSP 00FF, outside the image) + code (0100), same page:
       this is the case that used to be missed. */
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, memory, &hooks);
    test_load_hello(&rt, 0x2000u);
    CHECK(prog->attach(&rt, 0x2000u));
    CHECK(md_x86_code_page(md_x86_linear(0x2000u, 0x00FFu)) ==
          md_x86_code_page(md_x86_linear(0x2000u, 0x0100u)));
    md_x86_write16(&rt.cpu, 0x2000u, 0x00FFu, 0x1234u);
    CHECK(!prog->block_ok(&rt, 0x2000u, 0x0100u));

    /* code (010B, last byte of INT 21h) + data (010C), same page */
    md_runtime_init(&rt, memory, &hooks);
    test_load_hello(&rt, 0x2000u);
    CHECK(prog->attach(&rt, 0x2000u));
    md_x86_write16(&rt.cpu, 0x2000u, 0x010Bu, 0x21CDu);
    CHECK(!prog->block_ok(&rt, 0x2000u, 0x0100u));

    /* data + data inside the image: stays valid. M21.1b page flags: on a
       page that only holds compiled (AOT) code the generation is untouched
       (nothing validates by page there); once the cache/JIT has translated
       code on the page (TRANSLATED), it advances exactly once. */
    md_runtime_init(&rt, memory, &hooks);
    test_load_hello(&rt, 0x2000u);
    CHECK(prog->attach(&rt, 0x2000u));
    {
        const unsigned page = md_x86_code_page(md_x86_linear(0x2000u, 0x0110u));
        uint32_t gen;
        rt.code_page_executable[page] = MD_X86_PAGE_AOT;     /* compiled only */
        gen = rt.code_page_generation[page];
        md_x86_write16(&rt.cpu, 0x2000u, 0x0110u, 0x5858u);
        CHECK(prog->ready(&rt, 0x2000u));
        CHECK(rt.code_page_generation[page] == gen);
        md_runtime_mark_code_range(&rt, 0x2000u, 0x0100u, 16u); /* + translated */
        gen = rt.code_page_generation[page];
        md_x86_write16(&rt.cpu, 0x2000u, 0x0110u, 0x5959u);
        CHECK(prog->ready(&rt, 0x2000u));
        CHECK(rt.code_page_generation[page] == gen + 1u);
    }

    /* cross-page: segment 20F0 puts 00FF at linear 20FFF (page 20h) and
       0100 at 21000 (page 21h). Data byte on one page, code on the next. */
    md_runtime_init(&rt, memory, &hooks);
    test_load_hello(&rt, 0x20F0u);
    CHECK(prog->attach(&rt, 0x20F0u));
    CHECK(md_x86_code_page(md_x86_linear(0x20F0u, 0x00FFu)) + 1u ==
          md_x86_code_page(md_x86_linear(0x20F0u, 0x0100u)));
    md_x86_write16(&rt.cpu, 0x20F0u, 0x00FFu, 0xB4B4u);
    CHECK(!prog->block_ok(&rt, 0x20F0u, 0x0100u));

    /* byte stores: data keeps it valid, code invalidates it */
    md_runtime_init(&rt, memory, &hooks);
    test_load_hello(&rt, 0x2000u);
    CHECK(prog->attach(&rt, 0x2000u));
    md_x86_write8(&rt.cpu, 0x2000u, 0x0115u, 'Y');
    CHECK(prog->ready(&rt, 0x2000u));
    CHECK(prog->block_ok(&rt, 0x2000u, 0x0100u));
    md_x86_write8(&rt.cpu, 0x2000u, 0x0100u, 0xB4u);
    CHECK(!prog->block_ok(&rt, 0x2000u, 0x0100u));
}

/* M15 BUG 2 + slot hardening: attachments belong to one runtime and die
   with its reset/init; the table evicts least-recently-used entries. */
static void test_aot_attachment_lifetime(uint8_t *memory)
{
    MdRuntime a, b;
    MdHooks hooks = {0};
    const MdAotProgram *prog = &md_recomp_hello_program;
    uint8_t *memory_b = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    unsigned i;

    CHECK(memory_b != NULL);
    if (memory_b == NULL) return;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&a, memory, &hooks);
    md_runtime_init(&b, memory_b, &hooks);
    test_load_hello(&a, 0x2000u);
    test_load_hello(&b, 0x2000u);

    CHECK(prog->attach(&a, 0x2000u));
    CHECK(prog->ready(&a, 0x2000u));
    CHECK(!prog->ready(&b, 0x2000u));          /* same bytes, other runtime */
    CHECK(prog->attach(&b, 0x2000u));
    CHECK(prog->ready(&b, 0x2000u));

    md_x86_write8(&a.cpu, 0x2000u, 0x0100u, 0x90u);   /* A only */
    CHECK(!prog->block_ok(&a, 0x2000u, 0x0100u));
    CHECK(prog->ready(&b, 0x2000u));

    md_runtime_reset(&b);                       /* reset drops attachments */
    CHECK(!prog->ready(&b, 0x2000u));
    test_load_hello(&b, 0x2000u);
    CHECK(prog->attach(&b, 0x2000u));
    CHECK(prog->ready(&b, 0x2000u));
    md_runtime_init(&b, memory_b, &hooks);      /* so does init */
    CHECK(!prog->ready(&b, 0x2000u));

    /* A reset runtime must not run compiled code via enter() either. */
    b.cpu.cs = 0x2000u;
    b.cpu.ip = 0x0100u;
    CHECK(prog->enter(&b, 100u) == MD_STOP_NONE);
    CHECK(b.aot_instructions == 0u);

    /* LRU: attach MD_AOT_ATTACH_SLOTS + 1 copies; the oldest is evicted and
       falls back to interpretation, the rest stay attached. */
    md_runtime_init(&a, memory, &hooks);
    for (i = 0; i <= MD_AOT_ATTACH_SLOTS; ++i) {
        const uint16_t seg = (uint16_t)(0x3000u + i * 0x100u);
        test_load_hello(&a, seg);
        CHECK(prog->attach(&a, seg));
    }
    CHECK(a.aot_evictions == 1u);
    CHECK(!prog->ready(&a, 0x3000u));
    for (i = 1; i <= MD_AOT_ATTACH_SLOTS; ++i) {
        CHECK(prog->ready(&a, (uint16_t)(0x3000u + i * 0x100u)));
    }
    free(memory_b);
}

/* M15: memloop.com (the Pico memory benchmark) must leave identical memory,
   registers and instruction counts in all three engines. */
static uint32_t test_window_sum(const uint8_t *memory)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0x8000u; i < 0x10000u; ++i) h = (h ^ memory[i]) * 16777619u;
    return h;
}

static void test_memloop_engines(uint8_t *memory)
{
    static const uint8_t kMemloop[] = {
        0xB9,0x00,0x80, 0xBE,0x00,0x80, 0x8A,0x04, 0x04,0x03, 0x88,0x04,
        0x83,0xC6,0x61, 0x81,0xCE,0x00,0x80, 0x49, 0x75,0xF0, 0xF4
    };
    MdRuntime rt;
    MdHooks hooks = {0};
    MdBlockCache cache;
    uint32_t sum_interp, sum_cache, sum_aot;
    uint16_t si_interp;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, memory, &hooks);
    md_runtime_load_com(&rt, kMemloop, sizeof(kMemloop), 0x0000u);
    CHECK(md_interp_run(&rt, 1000000u) == MD_STOP_HALT);
    CHECK(rt.instructions == 229379u);
    sum_interp = test_window_sum(memory);
    si_interp = rt.cpu.r[MD_X86_SI];

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, memory, &hooks);
    md_block_cache_init(&cache);
    md_runtime_load_com(&rt, kMemloop, sizeof(kMemloop), 0x0000u);
    CHECK(md_interp_run_cached(&rt, &cache, 1000000u) == MD_STOP_HALT);
    CHECK(rt.instructions == 229379u);
    sum_cache = test_window_sum(memory);
    CHECK(rt.cpu.r[MD_X86_SI] == si_interp);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, memory, &hooks);
    CHECK(md_recomp_memloop(&rt, 0x0000u, 1000000u) == MD_STOP_HALT);
    CHECK(rt.instructions == 229379u);
    CHECK(rt.aot_instructions == 229379u);
    sum_aot = test_window_sum(memory);
    CHECK(rt.cpu.r[MD_X86_SI] == si_interp);

    CHECK(sum_interp == sum_cache);
    CHECK(sum_interp == sum_aot);
    CHECK(sum_interp != test_window_sum((const uint8_t *)memset(memory, 0, MD_X86_ADDRESS_SPACE)));
}

/* M16: md_interp_run_until_cs_change() runs near control flow and the new
   threaded fast paths without stopping, and returns MD_STOP_NONE exactly
   after the instruction that changed CS. */
static void test_run_until_cs_change(uint8_t *memory)
{
    /* 1000:0100  B9 03 00        mov cx,3
                  E8 02 00        call +2 (near)        -> 0108
                  EB 09           jmp  +9               -> 0111
       1000:0108  50              push ax
                  58              pop  ax
                  49              dec  cx
                  75 FB           jnz  -5               -> 0108 (x3)
                  C3              ret                   -> 0106
       1000:0106  (jmp) -> 0111
       1000:0111  9A 00 00 00 20  call far 2000:0000
       2000:0000  F4              hlt                                    */
    static const uint8_t code[] = {
        0xB9,0x03,0x00, 0xE8,0x02,0x00, 0xEB,0x09,
        0x50, 0x58, 0x49, 0x75,0xFB, 0xC3,
        0x90,0x90,0x90,
        0x9A,0x00,0x00,0x00,0x20
    };
    MdRuntime rt;
    MdHooks hooks = {0};
    unsigned i;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, memory, &hooks);
    for (i = 0; i < sizeof(code); ++i) md_x86_write8(&rt.cpu, 0x1000u, (uint16_t)(0x0100u + i), code[i]);
    md_x86_write8(&rt.cpu, 0x2000u, 0x0000u, 0xF4u);
    rt.cpu.cs = 0x1000u; rt.cpu.ip = 0x0100u;
    rt.cpu.ss = 0x3000u; rt.cpu.r[MD_X86_SP] = 0xFFFEu;

    CHECK(md_interp_run_until_cs_change(&rt, 1000u) == MD_STOP_NONE);
    CHECK(rt.stop_reason == MD_STOP_NONE);
    CHECK(rt.cpu.cs == 0x2000u && rt.cpu.ip == 0x0000u);
    /* mov, call, 3x(push,pop,dec,jnz), ret, jmp, call far = 17 */
    CHECK(rt.instructions == 17u);
    CHECK(rt.cpu.r[MD_X86_CX] == 0u);

    /* same CS from here on: runs to HLT like md_interp_run */
    CHECK(md_interp_run_until_cs_change(&rt, 1000u) == MD_STOP_HALT);
    CHECK(rt.instructions == 18u);

    /* budget exhaustion without a CS change still reports BUDGET */
    md_runtime_init(&rt, memory, &hooks);
    rt.cpu.cs = 0x1000u; rt.cpu.ip = 0x0100u;
    rt.cpu.ss = 0x3000u; rt.cpu.r[MD_X86_SP] = 0xFFFEu;
    CHECK(md_interp_run_until_cs_change(&rt, 5u) == MD_STOP_BUDGET);
    CHECK(rt.instructions == 5u);
}

/* M17: invalidation is per 64-byte chunk. A store to one compiled byte of
   DOS2TEST disables only the blocks in that chunk; blocks elsewhere in the
   same copy stay runnable, and the attachment itself stays live. */
static void test_aot_chunk_invalidation(uint8_t *memory)
{
    MdRuntime rt;
    MdHooks hooks = {0};
    const MdAotProgram *prog = &md_recomp_dos2test_program;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, memory, &hooks);
    /* standalone entry with a zero budget: loads, attaches, runs nothing */
    CHECK(md_recomp_dos2test(&rt, 0x2000u, 0u) == MD_STOP_BUDGET);
    CHECK(prog->ready(&rt, 0x2000u));
    CHECK(prog->block_ok(&rt, 0x2000u, 0x0258u));    /* t_version */
    CHECK(prog->block_ok(&rt, 0x2000u, 0x0B26u));    /* t_exec_command */
    CHECK(!prog->block_ok(&rt, 0x2000u, 0x0259u));   /* not an entry */

    md_x86_write8(&rt.cpu, 0x2000u, 0x0258u, 0xB4u);  /* same byte value */
    CHECK(prog->ready(&rt, 0x2000u));                 /* attachment lives */
    CHECK(!prog->block_ok(&rt, 0x2000u, 0x0258u));    /* this chunk is off */
    CHECK(prog->block_ok(&rt, 0x2000u, 0x0B26u));     /* far chunk still on */

    /* overwriting every compiled byte (DOS loading another program over
       this copy) kills the attachment, so hosts stop treating the segment
       as compiled code */
    {
        MdRuntime r2;
        uint32_t i;
        md_runtime_init(&r2, memory, &hooks);
        CHECK(md_recomp_dos2test(&r2, 0x2000u, 0u) == MD_STOP_BUDGET);
        CHECK(prog->ready(&r2, 0x2000u));
        for (i = 0x0100u; i < 0x0100u + 4448u; ++i) md_x86_write8(&r2.cpu, 0x2000u, (uint16_t)i, 0x90u);
        CHECK(!prog->ready(&r2, 0x2000u));
        md_runtime_init(&rt, memory, &hooks);
        CHECK(md_recomp_dos2test(&rt, 0x2000u, 0u) == MD_STOP_BUDGET);
        md_x86_write8(&rt.cpu, 0x2000u, 0x0258u, 0xB4u);
    }

    /* entering at a disabled block makes no compiled progress */
    rt.stop_reason = MD_STOP_NONE;          /* clear the zero-budget stop above */
    rt.cpu.cs = 0x2000u;
    rt.cpu.ip = 0x0258u;
    rt.cpu.ss = 0x3000u;
    rt.cpu.r[MD_X86_SP] = 0xFFFEu;
    {
        const uint64_t before = rt.aot_instructions;
        CHECK(prog->enter(&rt, 100u) == MD_STOP_NONE);
        CHECK(rt.aot_instructions == before);
    }
}

/* M17 regression: XCHG [0100h],AH stores into a compiled byte. Compiled
   code must finish the instruction (load AH) before handing over. */
static void test_aot_store_completes_instruction(uint8_t *memory)
{
    MdRuntime rt;
    MdHooks hooks = {0};
    MdBlockCache cache;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&rt, memory, &hooks);
    md_block_cache_init(&cache);
    md_runtime_set_block_cache(&rt, &cache);
    CHECK(md_recomp_xchgself(&rt, 0x1000u, 100u) == MD_STOP_HALT);
    CHECK(md_x86_get_reg8(&rt.cpu, 4u) == 0xB4u);
    CHECK(md_x86_read8(&rt.cpu, 0x1000u, 0x0100u) == 0x42u);
    CHECK(rt.instructions == 3u);
}

/* M18: MUL/DIV compiled through the shared interpreter core; the divide
   fault must stop both engines identically. */
static void test_aot_muldiv_fault(uint8_t *memory)
{
    static const uint8_t kProg[] = {
        0xB8,0xD2,0x04, 0xB3,0x0A, 0xF6,0xE3, 0xB9,0x07,0x00, 0x31,0xD2,
        0xF7,0xF1, 0x30,0xDB, 0xF6,0xF3, 0xF4
    };
    MdRuntime a, b;
    MdHooks hooks = {0};

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&a, memory, &hooks);
    md_runtime_load_com(&a, kProg, sizeof(kProg), 0x1000u);
    CHECK(md_interp_run(&a, 100u) == MD_STOP_FAULT);

    {
        static uint8_t mem_b[MD_X86_ADDRESS_SPACE];
        md_runtime_init(&b, mem_b, &hooks);
        CHECK(md_recomp_divfault(&b, 0x1000u, 100u) == MD_STOP_FAULT);
        CHECK(b.aot_instructions > 0u);
    }
    CHECK(a.instructions == b.instructions);
    CHECK(a.instructions == 8u);
    CHECK(a.cpu.ip == b.cpu.ip);
    CHECK(a.fault_linear == b.fault_linear);
    CHECK(a.fault_opcode == b.fault_opcode);
    /* MUL BL multiplies AL only: 0xD2 * 10 = 2100; 2100 / 7 = 300 r 0 */
    CHECK(a.cpu.r[MD_X86_AX] == b.cpu.r[MD_X86_AX] && a.cpu.r[MD_X86_AX] == 300u);
    CHECK(a.cpu.r[MD_X86_DX] == b.cpu.r[MD_X86_DX] && a.cpu.r[MD_X86_DX] == 0u);
    CHECK(md_x86_flags(&a.cpu) == md_x86_flags(&b.cpu));
}

/* M18: lazy flags must produce exactly the pre-M18 eager flags. Compared
   against the verbatim eager reference (eager_flags_ref.h): full FLAGS word
   after materialisation, every condition code before materialisation, and
   chains where ADC/SBB/INC/DEC take CF from a still-lazy previous result. */
static uint32_t g_lf_seed = 12345u;
static uint32_t lf_rand(void)
{
    g_lf_seed = g_lf_seed * 1664525u + 1013904223u;
    return g_lf_seed >> 8;
}

static int lf_check(const MdX86 *lazy_cpu, uint16_t ref_flags, uint16_t lazy_result, uint16_t ref_result)
{
    MdX86 copy = *lazy_cpu;
    RefCpu rc;
    unsigned cc;
    int ok = lazy_result == ref_result;
    rc.flags = ref_flags;
    for (cc = 0; cc < 16u; ++cc) {
        if ((md_x86_condition(lazy_cpu, cc) != 0) != (ref_x86_condition(&rc, cc) != 0)) ok = 0;
    }
    if (md_x86_flags(&copy) != ref_flags) ok = 0;
    return ok;
}

/* one lazy op vs the eager reference; `prev` selects the state that CF is
   read from: 0 = materialised flags, 1 = a lazy SUB/ADD result */
static int lf_one(unsigned op, int wide, unsigned a, unsigned b, uint16_t base, int prev_kind, unsigned pa, unsigned pb)
{
    MdX86 c;
    RefCpu r;
    unsigned lr, rr;
    memset(&c, 0, sizeof(c));
    md_x86_set_flags(&c, base);
    r.flags = base;
    if (prev_kind == 1) {              /* leave a lazy SUB behind (CF = pa < pb) */
        if (wide) { (void)md_x86_sub16(&c, (uint16_t)pa, (uint16_t)pb); (void)ref_x86_sub16(&r, (uint16_t)pa, (uint16_t)pb); }
        else { (void)md_x86_sub8(&c, (uint8_t)pa, (uint8_t)pb); (void)ref_x86_sub8(&r, (uint8_t)pa, (uint8_t)pb); }
    } else if (prev_kind == 2) {       /* leave a lazy ADD behind */
        if (wide) { (void)md_x86_add16(&c, (uint16_t)pa, (uint16_t)pb); (void)ref_x86_add16(&r, (uint16_t)pa, (uint16_t)pb); }
        else { (void)md_x86_add8(&c, (uint8_t)pa, (uint8_t)pb); (void)ref_x86_add8(&r, (uint8_t)pa, (uint8_t)pb); }
    }
    if (op <= 7u) {
        if (wide) { lr = md_x86_alu16(&c, op, (uint16_t)a, (uint16_t)b); rr = ref_x86_alu16(&r, op, (uint16_t)a, (uint16_t)b); }
        else { lr = md_x86_alu8(&c, op, (uint8_t)a, (uint8_t)b); rr = ref_x86_alu8(&r, op, (uint8_t)a, (uint8_t)b); }
        if (op == 7u) { lr = 0u; rr = 0u; }
    } else {                           /* 8 = INC, 9 = DEC: eager = add/sub 1 + CF restore */
        const uint16_t cf = r.flags & MD_X86_FLAG_CF;
        if (wide) {
            lr = op == 8u ? md_x86_inc16(&c, (uint16_t)a) : md_x86_dec16(&c, (uint16_t)a);
            rr = op == 8u ? ref_x86_add16(&r, (uint16_t)a, 1u) : ref_x86_sub16(&r, (uint16_t)a, 1u);
        } else {
            lr = op == 8u ? md_x86_inc8(&c, (uint8_t)a) : md_x86_dec8(&c, (uint8_t)a);
            rr = op == 8u ? ref_x86_add8(&r, (uint8_t)a, 1u) : ref_x86_sub8(&r, (uint8_t)a, 1u);
        }
        r.flags = (uint16_t)((r.flags & (uint16_t)~MD_X86_FLAG_CF) | cf);
    }
    return lf_check(&c, r.flags, (uint16_t)lr, (uint16_t)rr);
}

static void test_lazy_flags_equivalence(uint8_t *memory)
{
    static const uint16_t bases[] = {
        (uint16_t)(MD_X86_FLAG_ALWAYS1),
        (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF),
        (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF | MD_X86_FLAG_ZF | MD_X86_FLAG_OF |
                   MD_X86_FLAG_AF | MD_X86_FLAG_PF | MD_X86_FLAG_SF | MD_X86_FLAG_DF | MD_X86_FLAG_IF)
    };
    static const uint16_t edges[] = { 0u, 1u, 2u, 0x0Fu, 0x10u, 0x7Fu, 0x80u, 0xFFu, 0x100u,
                                      0x7FFFu, 0x8000u, 0x8001u, 0xFFFEu, 0xFFFFu };
    unsigned op, a, b, bi, pk, i, j;
    unsigned long failures = 0, cases = 0;
    (void)memory;

    /* 8-bit: exhaustive over a, b for every op and every starting CF */
    for (op = 0; op <= 9u; ++op)
        for (bi = 0; bi < 3u; ++bi)
            for (a = 0; a < 256u; ++a)
                for (b = 0; b < 256u; ++b) {
                    if (op >= 8u && b != 0u) break;      /* INC/DEC have no b */
                    if (!lf_one(op, 0, a, b, bases[bi], 0, 0, 0)) ++failures;
                    ++cases;
                }
    /* 8-bit chains: CF comes from a lazy SUB/ADD (both borrow outcomes) */
    for (op = 0; op <= 9u; ++op)
        for (pk = 1; pk <= 2u; ++pk)
            for (i = 0; i < 4u; ++i)
                for (a = 0; a < 256u; a += 3u)
                    for (b = 0; b < 256u; b += 5u) {
                        const unsigned pa = i < 2u ? 0x10u : 0xF0u, pb = (i & 1u) ? 0x20u : 0x05u;
                        if (!lf_one(op, 0, a, b, bases[0], (int)pk, pa, pb)) ++failures;
                        ++cases;
                    }
    /* 16-bit: all edge pairs, both starting CFs and chains, plus random */
    for (op = 0; op <= 9u; ++op) {
        for (i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i)
            for (j = 0; j < sizeof(edges) / sizeof(edges[0]); ++j)
                for (bi = 0; bi < 3u; ++bi)
                    for (pk = 0; pk <= 2u; ++pk) {
                        if (!lf_one(op, 1, edges[i], edges[j], bases[bi], (int)pk, edges[j], edges[i])) ++failures;
                        ++cases;
                    }
        for (i = 0; i < 40000u; ++i) {
            const unsigned ra = lf_rand() & 0xFFFFu, rb = lf_rand() & 0xFFFFu;
            if (!lf_one(op, 1, ra, rb, bases[lf_rand() % 3u], (int)(lf_rand() % 3u),
                        lf_rand() & 0xFFFFu, lf_rand() & 0xFFFFu)) ++failures;
            ++cases;
        }
    }
    CHECK(failures == 0u);
    CHECK(cases > 1000000u);
    if (failures != 0u) printf("lazy flags: %lu of %lu cases differ from eager\n", failures, cases);
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
    md_x86_set_flags(&cpu, (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF));
    CHECK(md_x86_adc8(&cpu, 0x7Fu, 0x00u) == 0x80u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_OF) != 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_CF) == 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_AF) != 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_SF) != 0u);

    md_x86_set_flags(&cpu, (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF));
    CHECK(md_x86_adc16(&cpu, 0xFFFFu, 0x0000u) == 0x0000u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_CF) != 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_ZF) != 0u);

    md_x86_set_flags(&cpu, (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF));
    CHECK(md_x86_sbb8(&cpu, 0x80u, 0x00u) == 0x7Fu);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_OF) != 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_CF) == 0u);

    md_x86_set_flags(&cpu, (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF | MD_X86_FLAG_OF | MD_X86_FLAG_AF));
    CHECK(md_x86_logic16(&cpu, (uint16_t)(0x55AAu ^ 0xFFFFu)) == 0xAA55u);
    CHECK((md_x86_flags(&cpu) & (MD_X86_FLAG_CF | MD_X86_FLAG_OF | MD_X86_FLAG_AF)) == 0u);
}

static void test_all_jcc_conditions(void)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));

    md_x86_set_flags(&cpu, MD_X86_FLAG_OF);
    CHECK(md_x86_condition(&cpu, 0x0u));
    CHECK(!md_x86_condition(&cpu, 0x1u));

    md_x86_set_flags(&cpu, MD_X86_FLAG_CF);
    CHECK(md_x86_condition(&cpu, 0x2u));
    CHECK(!md_x86_condition(&cpu, 0x3u));

    md_x86_set_flags(&cpu, MD_X86_FLAG_ZF);
    CHECK(md_x86_condition(&cpu, 0x4u));
    CHECK(!md_x86_condition(&cpu, 0x5u));
    CHECK(md_x86_condition(&cpu, 0x6u));
    CHECK(!md_x86_condition(&cpu, 0x7u));

    md_x86_set_flags(&cpu, MD_X86_FLAG_SF);
    CHECK(md_x86_condition(&cpu, 0x8u));
    CHECK(!md_x86_condition(&cpu, 0x9u));
    CHECK(md_x86_condition(&cpu, 0xCu));
    CHECK(!md_x86_condition(&cpu, 0xDu));
    CHECK(md_x86_condition(&cpu, 0xEu));
    CHECK(!md_x86_condition(&cpu, 0xFu));

    md_x86_set_flags(&cpu, MD_X86_FLAG_PF);
    CHECK(md_x86_condition(&cpu, 0xAu));
    CHECK(!md_x86_condition(&cpu, 0xBu));

    md_x86_set_flags(&cpu, 0u);
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
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_ZF) != 0u);

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
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_ZF) == 0u);
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_DF) == 0u);
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
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_ZF) != 0u);
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
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_ZF) != 0u);
    CHECK(runtime.instructions == 6u);

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kLoopnzOps, sizeof(kLoopnzOps), 0x1000u);
    CHECK(md_interp_run(&runtime, 16u) == MD_STOP_HALT);
    CHECK(runtime.cpu.r[MD_X86_CX] == 0u);
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_ZF) == 0u);
    CHECK(runtime.instructions == 6u);
}

static void test_shift_rotate_semantics(void)
{
    MdX86 cpu;
    memset(&cpu, 0, sizeof(cpu));
    md_x86_set_flags(&cpu, MD_X86_FLAG_ALWAYS1);

    CHECK(md_x86_shift8(&cpu, 4u, 0x81u, 1u) == 0x02u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_CF) != 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_OF) != 0u);

    md_x86_set_flags(&cpu, (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_CF));
    CHECK(md_x86_shift8(&cpu, 2u, 0x80u, 1u) == 0x01u); /* RCL through carry */
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_CF) != 0u);

    md_x86_set_flags(&cpu, MD_X86_FLAG_ALWAYS1);
    CHECK(md_x86_shift16(&cpu, 7u, 0x8001u, 1u) == 0xC000u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_CF) != 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_OF) == 0u);
    CHECK((md_x86_flags(&cpu) & MD_X86_FLAG_SF) != 0u);
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
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_IF) != 0u);
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_SF) != 0u);
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_ZF) == 0u);
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_CF) == 0u); /* TEST clears CF. */
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
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_ZF) == 0u);
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
    CHECK((md_x86_flags(&runtime.cpu) & (MD_X86_FLAG_CF | MD_X86_FLAG_IF)) ==
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
    CHECK((md_x86_flags(&runtime.cpu) & MD_X86_FLAG_SF) != 0u);

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

static bool test_far_streq(const MdX86 *cpu, uint16_t seg, uint16_t off, const char *text)
{
    for (;;) {
        const uint8_t b = md_x86_read8(cpu, seg, off++);
        if (b != (uint8_t)*text) {
            return false;
        }
        if (b == 0u) {
            return true;
        }
        ++text;
    }
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
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_OFFSET + 2u) == 0x33u);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_OFFSET + 3u) == 0xDBu);
    CHECK(md_x86_read8(&runtime.cpu, boot.bios_segment, MD_MSDOS2_POSTINIT_PATH_OFFSET) == (uint8_t)'A');
    CHECK(test_far_streq(&runtime.cpu, boot.bios_segment, MD_MSDOS2_CONDEV_OFFSET, "\\DEV\\CON"));
    CHECK(test_far_streq(&runtime.cpu, boot.bios_segment, MD_MSDOS2_AUXDEV_OFFSET, "\\DEV\\AUX"));
    CHECK(test_far_streq(&runtime.cpu, boot.bios_segment, MD_MSDOS2_PRNDEV_OFFSET, "\\DEV\\PRN"));

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

/* M12.3: the postinit program must reproduce SYSINIT's standard-handle
   setup before EXEC. The fake DOS below records every INT 21h function so
   the exact order can be checked. */
typedef struct TestPostinitEnv {
    MdMsdos2Boot boot;
    unsigned execs;
    uint8_t calls[32];
    uint16_t call_bx[32];
    unsigned call_count;
    unsigned next_dup;
    bool fail_open_con;
} TestPostinitEnv;

static bool test_postinit_interrupt_hook(MdRuntime *runtime, uint8_t vector, void *user)
{
    TestPostinitEnv *env = (TestPostinitEnv *)user;
    MdX86 *cpu = &runtime->cpu;

    if (vector == 0x21u) {
        const uint8_t ah = md_x86_get_reg8(cpu, 4u);
        const uint8_t al = md_x86_get_reg8(cpu, 0u);
        const uint16_t seg = env->boot.bios_segment;
        if (env->call_count < 32u) {
            env->calls[env->call_count] = ah;
            env->call_bx[env->call_count] = cpu->r[MD_X86_BX];
            ++env->call_count;
        }
        if (ah == 0x3Eu) {                       /* CLOSE */
            md_x86_update_flags(cpu, MD_X86_FLAG_CF, 0u);
            return true;
        }
        if (ah == 0x3Du) {                       /* OPEN */
            CHECK(cpu->ds == seg);
            if (cpu->r[MD_X86_DX] == MD_MSDOS2_CONDEV_OFFSET) {
                CHECK(al == 2u);
                CHECK(test_far_streq(cpu, seg, MD_MSDOS2_CONDEV_OFFSET, "\\DEV\\CON"));
                if (env->fail_open_con) {
                    md_x86_update_flags(cpu, 0u, MD_X86_FLAG_CF);
                    cpu->r[MD_X86_AX] = 2u;      /* file not found */
                    return true;
                }
                cpu->r[MD_X86_AX] = 0u;          /* lowest free handle */
                env->next_dup = 1u;
            } else if (cpu->r[MD_X86_DX] == MD_MSDOS2_AUXDEV_OFFSET) {
                CHECK(al == 2u);
                cpu->r[MD_X86_AX] = 3u;
            } else {
                CHECK(cpu->r[MD_X86_DX] == MD_MSDOS2_PRNDEV_OFFSET);
                CHECK(al == 1u);
                cpu->r[MD_X86_AX] = 4u;
            }
            md_x86_update_flags(cpu, MD_X86_FLAG_CF, 0u);
            return true;
        }
        if (ah == 0x45u) {                       /* XDUP */
            CHECK(cpu->r[MD_X86_BX] == 0u);
            cpu->r[MD_X86_AX] = (uint16_t)env->next_dup++;
            md_x86_update_flags(cpu, MD_X86_FLAG_CF, 0u);
            return true;
        }
        if (ah == 0x4Bu && al == 0u) {
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
            md_x86_update_flags(cpu, MD_X86_FLAG_CF, 0u);
            ++env->execs;
            return true;
        }
        md_x86_update_flags(cpu, 0u, MD_X86_FLAG_CF);
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
    md_x86_set_flags(&runtime.cpu, MD_X86_FLAG_ALWAYS1);

    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_RETURN_INT, &env.boot));
    reason = md_interp_run(&runtime, 1000u);
    CHECK(reason == MD_STOP_HALT);
    CHECK(env.execs == 1u);
    CHECK(!env.boot.postinit_stdio_failed);

    /* SYSINIT order: CLOSE 0, CLOSE 2..9, OPEN CON, CLOSE 1, XDUP, XDUP,
       OPEN AUX, OPEN PRN, EXEC. */
    {
        static const uint8_t kExpect[] = {
            0x3Eu,
            0x3Eu, 0x3Eu, 0x3Eu, 0x3Eu, 0x3Eu, 0x3Eu, 0x3Eu, 0x3Eu,
            0x3Du, 0x3Eu, 0x45u, 0x45u, 0x3Du, 0x3Du, 0x4Bu
        };
        unsigned i;
        CHECK(env.call_count == sizeof(kExpect));
        for (i = 0u; i < sizeof(kExpect) && i < env.call_count; ++i) {
            CHECK(env.calls[i] == kExpect[i]);
        }
        CHECK(env.call_bx[0] == 0u);             /* close stdin */
        for (i = 1u; i <= MD_MSDOS2_SYSINIT_FILES; ++i) {
            CHECK(env.call_bx[i] == (uint16_t)(i + 1u)); /* close 2..9 */
        }
        CHECK(env.call_bx[10] == 1u);            /* close stdout after OPEN */
    }
    CHECK(runtime.cpu.cs == 0x3000u);
    CHECK(runtime.cpu.ds == 0x3000u);
    CHECK(runtime.cpu.es == 0x3000u);
    CHECK(runtime.cpu.ss == 0x3000u);
    CHECK(runtime.cpu.ip == (uint16_t)(MD_MSDOS2_COMMAND_ENTRY_OFFSET + 1u));
}

static void test_msdos2_postinit_stdio_failure(uint8_t *memory)
{
    MdRuntime runtime;
    TestPostinitEnv env;
    MdHooks hooks = {0};
    MdStopReason reason;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    memset(&env, 0, sizeof(env));
    md_msdos2_boot_init(&env.boot);
    env.boot.continue_after_dosinit = true;
    env.fail_open_con = true;
    hooks.interrupt = test_postinit_interrupt_hook;
    hooks.user = &env;
    md_runtime_init(&runtime, memory, &hooks);
    md_msdos2_boot_install_devices(&runtime, &env.boot);
    runtime.cpu.ss = env.boot.stack_segment;
    runtime.cpu.r[MD_X86_SP] = 0xFFFEu;
    md_x86_set_flags(&runtime.cpu, MD_X86_FLAG_ALWAYS1);

    CHECK(md_msdos2_boot_interrupt(&runtime, MD_MSDOS2_NATIVE_RETURN_INT, &env.boot));
    reason = md_interp_run(&runtime, 1000u);
    CHECK(reason == MD_STOP_HALT);
    CHECK(env.execs == 0u);                      /* never EXEC on bootstrap SFT */
    CHECK(env.boot.postinit_completed);
    CHECK(!env.boot.postinit_succeeded);
    CHECK(env.boot.postinit_stdio_failed);
    CHECK(env.boot.postinit_error == 2u);
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
    md_x86_set_flags(&runtime.cpu, MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_IF | MD_X86_FLAG_TF);
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
    test_aot_guard_byte_exact(memory);
    test_aot_guard_word_writes(memory);
    test_aot_attachment_lifetime(memory);
    test_memloop_engines(memory);
    test_run_until_cs_change(memory);
    test_aot_chunk_invalidation(memory);
    test_aot_store_completes_instruction(memory);
    test_aot_muldiv_fault(memory);
    test_lazy_flags_equivalence(memory);
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
    test_msdos2_postinit_stdio_failure(memory);
    test_ivt(memory);

    free(memory);
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("microDOS runtime + DOS 2 phase-C core tests passed");
    return 0;
}
