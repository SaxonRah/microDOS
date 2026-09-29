#include "host_dos.h"

#include <stdint.h>

static void md_host_putc(MdHostDos *host, char ch)
{
    if (host->capacity != 0u && host->length + 1u < host->capacity) {
        host->output[host->length] = ch;
        host->output[host->length + 1u] = '\0';
    }
    ++host->length;
}

void md_host_dos_init(MdHostDos *host, char *output, size_t capacity)
{
    host->output = output;
    host->capacity = capacity;
    host->length = 0u;
    if (capacity != 0u) output[0] = '\0';
}

bool md_host_dos_interrupt(MdRuntime *runtime, uint8_t vector, void *user)
{
    MdHostDos *host = (MdHostDos *)user;
    MdX86 *cpu = &runtime->cpu;
    const uint8_t ah = md_x86_get_reg8(cpu, 4u);

    if (vector != 0x21u) return false;

    switch (ah) {
        case 0x02u:
            md_host_putc(host, (char)md_x86_get_reg8(cpu, 2u));
            return true;

        case 0x09u: {
            uint16_t offset = cpu->r[MD_X86_DX];
            for (;;) {
                const uint8_t ch = md_x86_read8(cpu, cpu->ds, offset++);
                if (ch == '$') break;
                md_host_putc(host, (char)ch);
            }
            return true;
        }

        case 0x4Cu:
            md_runtime_request_exit(runtime, md_x86_get_reg8(cpu, 0u));
            return true;

        default:
            return false;
    }
}
