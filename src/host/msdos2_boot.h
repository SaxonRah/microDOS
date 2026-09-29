#ifndef MICRODOS_MSDOS2_BOOT_H
#define MICRODOS_MSDOS2_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/runtime.h"

#define MD_MSDOS2_DEFAULT_DOS_SEGMENT 0x1000u
#define MD_MSDOS2_DEFAULT_BIOS_SEGMENT 0x0800u
#define MD_MSDOS2_DEFAULT_STACK_SEGMENT 0x9000u
#define MD_MSDOS2_DEFAULT_MEMORY_PARAGRAPHS 0xA000u

#define MD_MSDOS2_CON_OFFSET   0x0000u
#define MD_MSDOS2_AUX_OFFSET   0x0020u
#define MD_MSDOS2_PRN_OFFSET   0x0040u
#define MD_MSDOS2_CLOCK_OFFSET 0x0060u
#define MD_MSDOS2_DISK_OFFSET  0x0080u

#define MD_MSDOS2_STRATEGY_OFFSET 0x0100u
#define MD_MSDOS2_INTERRUPT_OFFSET 0x0110u
#define MD_MSDOS2_RETURN_OFFSET 0x0120u
#define MD_MSDOS2_BPB_TABLE_OFFSET 0x0300u
#define MD_MSDOS2_BPB_OFFSET 0x0320u

#define MD_MSDOS2_NATIVE_STRATEGY_INT 0xF0u
#define MD_MSDOS2_NATIVE_DEVICE_INT 0xF1u
#define MD_MSDOS2_NATIVE_RETURN_INT 0xF2u

typedef void (*MdMsdos2ConsoleWriteHook)(void *user, const uint8_t *data, size_t size);
typedef bool (*MdMsdos2ConsolePeekHook)(void *user, uint8_t *value);
typedef bool (*MdMsdos2ConsoleReadHook)(void *user, uint8_t *value);
typedef void (*MdMsdos2ConsoleFlushHook)(void *user);

typedef struct MdMsdos2ConsoleIo {
    MdMsdos2ConsoleWriteHook write;
    MdMsdos2ConsolePeekHook peek;
    MdMsdos2ConsoleReadHook read;
    MdMsdos2ConsoleFlushHook flush;
    void *user;
} MdMsdos2ConsoleIo;

typedef struct MdMsdos2Boot {
    uint16_t dos_segment;
    uint16_t bios_segment;
    uint16_t stack_segment;
    uint16_t memory_paragraphs;

    uint16_t request_segment;
    uint16_t request_offset;
    bool have_request;
    bool entered_dosinit;
    bool returned_from_dosinit;

    uint32_t strategy_calls;
    uint32_t device_calls;
    uint32_t init_calls;
    uint32_t unknown_device_calls;
    uint32_t console_poll_calls;
    uint32_t console_write_calls;
    uint32_t console_bytes_written;

    MdMsdos2ConsoleIo console;

    uint16_t last_device_offset;
    uint8_t last_request_function;
} MdMsdos2Boot;

void md_msdos2_boot_init(MdMsdos2Boot *boot);
void md_msdos2_boot_install_devices(MdRuntime *runtime, MdMsdos2Boot *boot);
void md_msdos2_boot_prepare_cpu(MdRuntime *runtime, MdMsdos2Boot *boot,
                                const uint8_t *image, size_t size);
bool md_msdos2_boot_interrupt(MdRuntime *runtime, uint8_t vector, void *user);
const char *md_msdos2_device_name(uint16_t offset);

#endif
