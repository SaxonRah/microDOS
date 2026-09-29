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

/* M11 guest-side SYSINIT continuation. It deliberately lives in the OEM
   segment, outside the DOS arena, and invokes DOS EXEC through INT 21h. */
#define MD_MSDOS2_POSTINIT_OFFSET 0x0500u
#define MD_MSDOS2_POSTINIT_PATH_OFFSET 0x0600u
#define MD_MSDOS2_EXEC_BLOCK_OFFSET 0x0640u
#define MD_MSDOS2_COMMAND_TAIL_OFFSET 0x0660u
#define MD_MSDOS2_FCB1_OFFSET 0x0700u
#define MD_MSDOS2_FCB2_OFFSET 0x0720u
#define MD_MSDOS2_COMMAND_ENTRY_OFFSET 0x0100u

#define MD_MSDOS2_NATIVE_STRATEGY_INT 0xF0u
#define MD_MSDOS2_NATIVE_DEVICE_INT 0xF1u
#define MD_MSDOS2_NATIVE_RETURN_INT 0xF2u
#define MD_MSDOS2_NATIVE_POSTINIT_OK_INT 0xF3u
#define MD_MSDOS2_NATIVE_POSTINIT_FAIL_INT 0xF4u

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

typedef bool (*MdMsdos2DiskReadHook)(void *user, uint32_t sector,
                                      uint8_t *data, size_t size);
typedef bool (*MdMsdos2DiskWriteHook)(void *user, uint32_t sector,
                                       const uint8_t *data, size_t size);

typedef struct MdMsdos2DiskIo {
    MdMsdos2DiskReadHook read;
    MdMsdos2DiskWriteHook write;
    void *user;
    uint16_t sector_size;
    uint32_t sector_count;
    bool writable;
} MdMsdos2DiskIo;

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
    bool continue_after_dosinit;
    bool postinit_started;
    bool postinit_completed;
    bool postinit_succeeded;
    uint16_t postinit_error;

    bool command_entered;
    bool command_image_match;
    bool command_psp_valid;
    uint16_t command_segment;

    uint32_t strategy_calls;
    uint32_t device_calls;
    uint32_t init_calls;
    uint32_t unknown_device_calls;
    uint32_t console_poll_calls;
    uint32_t console_read_calls;
    uint32_t console_bytes_read;
    uint32_t console_write_calls;
    uint32_t console_bytes_written;

    /* M12.1 diagnostic latch: DOS function 09 should stop before a '$'.
       If one reaches CON after COMMAND.COM starts, keep enough device state
       to correlate it with the recent guest instruction trace. */
    uint32_t console_dollar_writes;
    bool console_first_dollar_valid;
    uint16_t console_first_dollar_data_segment;
    uint16_t console_first_dollar_data_offset;
    uint16_t console_first_dollar_count;
    uint16_t console_first_dollar_request_segment;
    uint16_t console_first_dollar_request_offset;
    uint16_t console_first_dollar_return_segment;
    uint16_t console_first_dollar_return_offset;

    uint32_t clock_read_calls;
    uint32_t clock_write_calls;
    uint16_t clock_days;
    uint8_t clock_minutes;
    uint8_t clock_hours;
    uint8_t clock_hundredths;
    uint8_t clock_seconds;

    uint32_t disk_media_checks;
    uint32_t disk_bpb_calls;
    uint32_t disk_read_calls;
    uint32_t disk_write_calls;
    uint32_t disk_sectors_read;
    uint32_t disk_sectors_written;

    MdMsdos2ConsoleIo console;
    MdMsdos2DiskIo disk;

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
