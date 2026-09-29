#include "microdos/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 1 + (65535 * 2) + 1 = 131072 guest instructions per round. */
static const uint8_t kLoopCom[] = {
    0xB9, 0xFF, 0xFF, /* mov cx,ffffh */
    0x49,             /* dec cx */
    0x75, 0xFD,       /* jnz -3 */
    0xF4              /* hlt */
};

int main(int argc, char **argv)
{
    unsigned long rounds = 1000ul;
    unsigned long i;
    uint64_t total_instructions = 0u;
    uint8_t *memory;
    MdRuntime runtime;
    MdHooks hooks = {0};
    clock_t begin;
    clock_t end;
    double seconds;

    if (argc > 1) {
        rounds = strtoul(argv[1], NULL, 10);
        if (rounds == 0ul) rounds = 1ul;
    }

    memory = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    if (memory == NULL) return 2;
    memset(memory, 0, MD_X86_ADDRESS_SPACE);
    md_runtime_init(&runtime, memory, &hooks);

    begin = clock();
    for (i = 0; i < rounds; ++i) {
        md_runtime_load_com(&runtime, kLoopCom, sizeof(kLoopCom), 0x1000u);
        if (md_interp_run(&runtime, 200000u) != MD_STOP_HALT) {
            fprintf(stderr, "benchmark guest did not halt cleanly\n");
            free(memory);
            return 1;
        }
        total_instructions += runtime.instructions;
    }
    end = clock();

    seconds = (double)(end - begin) / (double)CLOCKS_PER_SEC;
    printf("rounds=%lu guest_instructions=%llu seconds=%.6f MIPS=%.2f\n",
           rounds,
           (unsigned long long)total_instructions,
           seconds,
           seconds > 0.0 ? ((double)total_instructions / seconds) / 1000000.0 : 0.0);

    free(memory);
    return 0;
}
