#include "microdos/block_cache.h"
#include "microdos/runtime.h"
#include "loop_recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const uint8_t kLoopCom[] = {0xB9,0xFF,0xFF,0x49,0x75,0xFD,0xF4};

static double elapsed_seconds(clock_t begin, clock_t end)
{
    return (double)(end - begin) / (double)CLOCKS_PER_SEC;
}

static int bench_interp(unsigned long rounds, uint8_t *memory)
{
    unsigned long i;
    uint64_t total = 0u;
    MdRuntime runtime;
    MdHooks hooks = {0};
    clock_t begin;
    clock_t end;
    double seconds;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    begin = clock();
    for (i = 0; i < rounds; ++i) {
        md_runtime_load_com(&runtime, kLoopCom, sizeof(kLoopCom), 0x1000u);
        if (md_interp_run(&runtime, 200000u) != MD_STOP_HALT) return 1;
        total += runtime.instructions;
    }
    end = clock();
    seconds = elapsed_seconds(begin, end);
    printf("interp rounds=%lu guest_instructions=%llu seconds=%.6f MIPS=%.2f\n",
           rounds, (unsigned long long)total, seconds,
           seconds > 0.0 ? ((double)total / seconds) / 1e6 : 0.0);
    return 0;
}

static int bench_cache(unsigned long rounds, uint8_t *memory)
{
    unsigned long i;
    uint64_t total = 0u;
    MdRuntime runtime;
    MdHooks hooks = {0};
    MdBlockCache cache;
    clock_t begin;
    clock_t end;
    double seconds;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    md_block_cache_init(&cache);

    begin = clock();
    for (i = 0; i < rounds; ++i) {
        md_runtime_load_com(&runtime, kLoopCom, sizeof(kLoopCom), 0x1000u);
        if (md_interp_run_cached(&runtime, &cache, 200000u) != MD_STOP_HALT) return 1;
        total += runtime.instructions;
    }
    end = clock();
    seconds = elapsed_seconds(begin, end);
    printf("cache  rounds=%lu guest_instructions=%llu seconds=%.6f MIPS=%.2f hits=%llu misses=%llu decodes=%llu fallback=%llu\n",
           rounds, (unsigned long long)total, seconds,
           seconds > 0.0 ? ((double)total / seconds) / 1e6 : 0.0,
           (unsigned long long)cache.hits,
           (unsigned long long)cache.misses,
           (unsigned long long)cache.decodes,
           (unsigned long long)cache.fallback_instructions);
    return 0;
}

static int bench_aot(unsigned long rounds, uint8_t *memory)
{
    unsigned long i;
    uint64_t total = 0u;
    MdRuntime runtime;
    MdHooks hooks = {0};
    clock_t begin;
    clock_t end;
    double seconds;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);
    begin = clock();
    for (i = 0; i < rounds; ++i) {
        if (md_recomp_loop(&runtime, 0x1000u, 200000u) != MD_STOP_HALT) return 1;
        total += runtime.instructions;
    }
    end = clock();
    seconds = elapsed_seconds(begin, end);
    printf("aot    rounds=%lu guest_instructions=%llu seconds=%.6f MIPS=%.2f\n",
           rounds, (unsigned long long)total, seconds,
           seconds > 0.0 ? ((double)total / seconds) / 1e6 : 0.0);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long rounds = 1000ul;
    uint8_t *memory;
    int failed = 0;

    if (argc > 1) {
        rounds = strtoul(argv[1], NULL, 10);
        if (rounds == 0ul) rounds = 1ul;
    }

    memory = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    if (memory == NULL) return 2;

    failed |= bench_interp(rounds, memory);
    failed |= bench_cache(rounds, memory);
    failed |= bench_aot(rounds, memory);

    free(memory);
    return failed;
}
