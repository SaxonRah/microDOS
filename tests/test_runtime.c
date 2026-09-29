#include "microdos/block_cache.h"
#include "microdos/runtime.h"
#include "hello_recomp.h"
#include "hybrid_recomp.h"
#include "loop_recomp.h"
#include "selfmod_recomp.h"
#include "host_dos.h"

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
    test_ivt(memory);

    free(memory);
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("microDOS runtime + page cache + hybrid AOT tests passed");
    return 0;
}
