#include "microdos/block_cache.h"
#include "microdos/runtime.h"
#include "msdos2_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MD_MSDOS2_EXPECTED_SIZE 16690u
#define MD_DOSINIT_OFFSET 0x3E7Bu

static uint8_t *md_read_file(const char *path, size_t *size_out)
{
    FILE *fp;
    long length;
    uint8_t *data;

    *size_out = 0u;
    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL) { fclose(fp); return NULL; }
    if (length != 0 && fread(data, 1u, (size_t)length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size_out = (size_t)length;
    return data;
}


static void md_host_console_write(void *user, const uint8_t *data, size_t size)
{
    FILE *fp = (FILE *)user;
    if (size == 0u) return;
    (void)fwrite(data, 1u, size, fp);
    (void)fflush(fp);
}

static void md_dump_cpu(const MdRuntime *runtime)
{
    const MdX86 *c = &runtime->cpu;
    printf("  CS:IP=%04X:%04X  SS:SP=%04X:%04X  DS=%04X ES=%04X\n",
           c->cs, c->ip, c->ss, c->r[MD_X86_SP], c->ds, c->es);
    printf("  AX=%04X BX=%04X CX=%04X DX=%04X BP=%04X SI=%04X DI=%04X FLAGS=%04X\n",
           c->r[MD_X86_AX], c->r[MD_X86_BX], c->r[MD_X86_CX], c->r[MD_X86_DX],
           c->r[MD_X86_BP], c->r[MD_X86_SI], c->r[MD_X86_DI], c->flags);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "third_party/msdos/v2.0/bin/MSDOS.SYS";
    uint64_t budget = 2000000u;
    uint8_t *image;
    size_t image_size;
    uint8_t *memory;
    MdRuntime runtime;
    MdBlockCache cache;
    MdMsdos2Boot boot;
    MdHooks hooks;
    uint64_t steps = 0u;
    uint32_t error_traces = 0u;

    if (argc > 2) {
        budget = (uint64_t)strtoull(argv[2], NULL, 0);
        if (budget == 0u) budget = 1u;
    }

    image = md_read_file(path, &image_size);
    if (image == NULL) {
        fprintf(stderr, "microDOS: unable to read %s\n", path);
        fprintf(stderr, "run .\\md.bat deps msdos first\n");
        return 2;
    }
    if (image_size != MD_MSDOS2_EXPECTED_SIZE || image_size < 3u ||
        image[0] != 0xE9u || image[1] != 0x78u || image[2] != 0x3Eu) {
        fprintf(stderr, "microDOS: unexpected MS-DOS 2.0 image (%zu bytes, entry %02X %02X %02X)\n",
                image_size,
                image_size > 0u ? image[0] : 0u,
                image_size > 1u ? image[1] : 0u,
                image_size > 2u ? image[2] : 0u);
        free(image);
        return 2;
    }

    memory = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    if (memory == NULL) {
        free(image);
        return 2;
    }

    md_msdos2_boot_init(&boot);
    boot.console.write = md_host_console_write;
    boot.console.user = stdout;
    memset(&hooks, 0, sizeof(hooks));
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_block_cache_init(&cache);
    md_runtime_set_block_cache(&runtime, &cache);
    md_msdos2_boot_prepare_cpu(&runtime, &boot, image, image_size);

    printf("microDOS MS-DOS 2.0 bring-up\n");
    printf("  image:   %s (%zu bytes)\n", path, image_size);
    printf("  kernel:  %04X:0000 -> DOSINIT %04X:%04X\n",
           boot.dos_segment, boot.dos_segment, MD_DOSINIT_OFFSET);
    printf("  devices: %04X:%04X CON -> AUX -> PRN -> CLOCK -> DISK\n",
           boot.bios_segment, MD_MSDOS2_CON_OFFSET);
    printf("  memory:  %u paragraphs (%u KiB)\n",
           boot.memory_paragraphs, (unsigned)(boot.memory_paragraphs / 64u));
    printf("  budget:  %llu guest instructions\n", (unsigned long long)budget);

    while (runtime.stop_reason == MD_STOP_NONE && steps < budget) {
        if (!boot.entered_dosinit && runtime.cpu.cs == boot.dos_segment &&
            runtime.cpu.ip == MD_DOSINIT_OFFSET) {
            boot.entered_dosinit = true;
            printf("[boot] entered DOSINIT at %04X:%04X after %llu instructions\n",
                   runtime.cpu.cs, runtime.cpu.ip,
                   (unsigned long long)runtime.instructions);
        }

        {
            const uint32_t before_calls = boot.device_calls;
            const MdStopReason reason = md_interp_step(&runtime);
            ++steps;
            if (boot.device_calls != before_calls) {
                const uint16_t status = md_x86_read16(&runtime.cpu, boot.request_segment,
                                                       (uint16_t)(boot.request_offset + 3u));
                /* INIT and failures are interesting. Successful character I/O is
                   intentionally quiet so DOS output is readable instead of being
                   buried under one trace line per byte/status poll. */
                if (boot.last_request_function == 0u) {
                    printf("[device] %-5s func=%u request=%04X:%04X status=%04X\n",
                           md_msdos2_device_name(boot.last_device_offset),
                           boot.last_request_function,
                           boot.request_segment, boot.request_offset, status);
                } else if ((status & 0x8000u) != 0u) {
                    if (error_traces < 20u) {
                        printf("[device] %-5s func=%u request=%04X:%04X status=%04X\n",
                               md_msdos2_device_name(boot.last_device_offset),
                               boot.last_request_function,
                               boot.request_segment, boot.request_offset, status);
                    } else if (error_traces == 20u) {
                        puts("[device] further repeated error traces suppressed");
                    }
                    ++error_traces;
                }
            }
            if (reason != MD_STOP_NONE) break;
        }
    }

    if (runtime.stop_reason == MD_STOP_NONE && steps >= budget) {
        runtime.stop_reason = MD_STOP_BUDGET;
    }

    printf("\n[boot] stop=%s instructions=%llu strategy=%u device=%u init=%u unknown=%u\n",
           md_stop_reason_name(runtime.stop_reason),
           (unsigned long long)runtime.instructions,
           (unsigned)boot.strategy_calls,
           (unsigned)boot.device_calls,
           (unsigned)boot.init_calls,
           (unsigned)boot.unknown_device_calls);
    printf("[boot] console polls=%u writes=%u bytes=%u\n",
           (unsigned)boot.console_poll_calls,
           (unsigned)boot.console_write_calls,
           (unsigned)boot.console_bytes_written);
    printf("[boot] dosinit_entered=%s dosinit_returned=%s\n",
           boot.entered_dosinit ? "yes" : "no",
           boot.returned_from_dosinit ? "yes" : "no");

    if (runtime.stop_reason == MD_STOP_FAULT) {
        printf("[fault] linear=%05X opcode=%02X\n",
               (unsigned)runtime.fault_linear, runtime.fault_opcode);
    }
    md_dump_cpu(&runtime);

    free(memory);
    free(image);

    if (boot.returned_from_dosinit) return 0;
    if (runtime.stop_reason == MD_STOP_FAULT) return 3;
    if (runtime.stop_reason == MD_STOP_BUDGET) return 4;
    return 1;
}
