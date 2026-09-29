#include "microdos/block_cache.h"
#include "microdos/runtime.h"
#include "hello_recomp.h"
#include "host_dos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t kHelloCom[] = {
    0xB4,0x09, 0xBA,0x0C,0x01, 0xCD,0x21, 0xB8,0x00,0x4C, 0xCD,0x21,
    'H','e','l','l','o',' ','f','r','o','m',' ','m','i','c','r','o','D','O','S','!',13,10,'$'
};

static MdHooks host_hooks(MdHostDos *host)
{
    MdHooks hooks;
    hooks.interrupt = md_host_dos_interrupt;
    hooks.in8 = NULL;
    hooks.out8 = NULL;
    hooks.user = host;
    return hooks;
}

static int run_interp(uint8_t *memory, char *output, size_t capacity)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    MdStopReason stop;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, output, capacity);
    hooks = host_hooks(&host);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kHelloCom, sizeof(kHelloCom), 0x1000u);
    stop = md_interp_run(&runtime, 1000u);
    printf("interp: stop=%s instructions=%llu exit=%u\n",
           md_stop_reason_name(stop),
           (unsigned long long)runtime.instructions,
           (unsigned)runtime.exit_code);
    return stop == MD_STOP_EXIT && runtime.exit_code == 0u ? 0 : 1;
}

static int run_cache(uint8_t *memory, char *output, size_t capacity)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    MdBlockCache cache;
    MdStopReason stop;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, output, capacity);
    hooks = host_hooks(&host);
    md_runtime_init(&runtime, memory, &hooks);
    md_runtime_load_com(&runtime, kHelloCom, sizeof(kHelloCom), 0x1000u);
    md_block_cache_init(&cache);
    stop = md_interp_run_cached(&runtime, &cache, 1000u);
    printf("cache:  stop=%s instructions=%llu exit=%u blocks=%llu fallback=%llu\n",
           md_stop_reason_name(stop),
           (unsigned long long)runtime.instructions,
           (unsigned)runtime.exit_code,
           (unsigned long long)cache.decodes,
           (unsigned long long)cache.fallback_instructions);
    return stop == MD_STOP_EXIT && runtime.exit_code == 0u ? 0 : 1;
}

static int run_recomp(uint8_t *memory, char *output, size_t capacity)
{
    MdRuntime runtime;
    MdHostDos host;
    MdHooks hooks;
    MdStopReason stop;

    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_host_dos_init(&host, output, capacity);
    hooks = host_hooks(&host);
    md_runtime_init(&runtime, memory, &hooks);
    stop = md_recomp_hello(&runtime, 0x1000u, 1000u);
    printf("recomp: stop=%s instructions=%llu exit=%u\n",
           md_stop_reason_name(stop),
           (unsigned long long)runtime.instructions,
           (unsigned)runtime.exit_code);
    return stop == MD_STOP_EXIT && runtime.exit_code == 0u ? 0 : 1;
}

int main(void)
{
    uint8_t *memory = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    char interp_output[256];
    char cache_output[256];
    char recomp_output[256];
    int failed = 0;

    if (memory == NULL) {
        fprintf(stderr, "could not allocate 1 MiB guest address space\n");
        return 1;
    }

    failed |= run_interp(memory, interp_output, sizeof(interp_output));
    failed |= run_cache(memory, cache_output, sizeof(cache_output));
    failed |= run_recomp(memory, recomp_output, sizeof(recomp_output));

    printf("output: %s", interp_output);
    if (strcmp(interp_output, cache_output) != 0 ||
        strcmp(interp_output, recomp_output) != 0) {
        fprintf(stderr, "interpreter/cache/AOT output mismatch\n");
        failed = 1;
    }

    free(memory);
    return failed;
}
