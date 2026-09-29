#include "msdos2_boot.h"

#include <string.h>

/* DOS 2.x device header layout from DEVSYM.ASM. */
enum {
    MD_DEV_NEXT_OFF = 0,
    MD_DEV_NEXT_SEG = 2,
    MD_DEV_ATTR = 4,
    MD_DEV_STRATEGY = 6,
    MD_DEV_INTERRUPT = 8,
    MD_DEV_NAME = 10,
    MD_DEV_HEADER_SIZE = 18
};

/* Static request header / INIT packet offsets. */
enum {
    MD_REQ_LEN = 0,
    MD_REQ_UNIT = 1,
    MD_REQ_FUNC = 2,
    MD_REQ_STATUS = 3,
    MD_REQ_MEDIA = 13,
    MD_REQ_TRANSFER = 14,
    MD_REQ_COUNT = 18,
    MD_REQ_START = 20,
    MD_REQ_INIT_UNITS = 13,
    MD_REQ_INIT_BREAK = 14,
    MD_REQ_INIT_BPB = 18
};

enum {
    MD_DEV_ATTR_CHAR = 0x8000u,
    MD_DEV_ATTR_CONSOLE = 0x0003u,
    MD_DEV_ATTR_CLOCK = 0x0008u,
    MD_DEV_ATTR_BLOCK_FAT_ID = 0x2000u,
    MD_DEV_STATUS_DONE = 0x0100u,
    MD_DEV_STATUS_BUSY = 0x0200u,
    MD_DEV_STATUS_ERROR = 0x8000u
};

enum {
    MD_DEV_INIT = 0u,
    MD_DEV_READ = 4u,
    MD_DEV_READ_ND = 5u,
    MD_DEV_INPUT_STATUS = 6u,
    MD_DEV_INPUT_FLUSH = 7u,
    MD_DEV_WRITE = 8u,
    MD_DEV_WRITE_VERIFY = 9u,
    MD_DEV_OUTPUT_STATUS = 10u,
    MD_DEV_OUTPUT_FLUSH = 11u,
    MD_DEV_IOCTL_WRITE = 12u
};

static void md_read_far(const MdX86 *cpu, uint16_t segment, uint16_t offset,
                        uint16_t *target_offset, uint16_t *target_segment)
{
    *target_offset = md_x86_read16(cpu, segment, offset);
    *target_segment = md_x86_read16(cpu, segment, (uint16_t)(offset + 2u));
}

static void md_write_far(MdX86 *cpu, uint16_t segment, uint16_t offset,
                         uint16_t target_offset, uint16_t target_segment)
{
    md_x86_write16(cpu, segment, offset, target_offset);
    md_x86_write16(cpu, segment, (uint16_t)(offset + 2u), target_segment);
}

static void md_write_device_header(MdX86 *cpu, uint16_t segment, uint16_t offset,
                                   uint16_t next_offset, uint16_t next_segment,
                                   uint16_t attributes, const char name[8])
{
    unsigned i;
    md_write_far(cpu, segment, (uint16_t)(offset + MD_DEV_NEXT_OFF),
                 next_offset, next_segment);
    md_x86_write16(cpu, segment, (uint16_t)(offset + MD_DEV_ATTR), attributes);
    md_x86_write16(cpu, segment, (uint16_t)(offset + MD_DEV_STRATEGY),
                   MD_MSDOS2_STRATEGY_OFFSET);
    md_x86_write16(cpu, segment, (uint16_t)(offset + MD_DEV_INTERRUPT),
                   MD_MSDOS2_INTERRUPT_OFFSET);
    for (i = 0; i < 8u; ++i) {
        md_x86_write8(cpu, segment, (uint16_t)(offset + MD_DEV_NAME + i),
                      (uint8_t)name[i]);
    }
}

static bool md_known_device_offset(uint16_t offset)
{
    return offset == MD_MSDOS2_CON_OFFSET ||
           offset == MD_MSDOS2_AUX_OFFSET ||
           offset == MD_MSDOS2_PRN_OFFSET ||
           offset == MD_MSDOS2_CLOCK_OFFSET ||
           offset == MD_MSDOS2_DISK_OFFSET;
}

const char *md_msdos2_device_name(uint16_t offset)
{
    switch (offset) {
        case MD_MSDOS2_CON_OFFSET: return "CON";
        case MD_MSDOS2_AUX_OFFSET: return "AUX";
        case MD_MSDOS2_PRN_OFFSET: return "PRN";
        case MD_MSDOS2_CLOCK_OFFSET: return "CLOCK";
        case MD_MSDOS2_DISK_OFFSET: return "DISK";
        default: return "UNKNOWN";
    }
}

void md_msdos2_boot_init(MdMsdos2Boot *boot)
{
    memset(boot, 0, sizeof(*boot));
    boot->dos_segment = MD_MSDOS2_DEFAULT_DOS_SEGMENT;
    boot->bios_segment = MD_MSDOS2_DEFAULT_BIOS_SEGMENT;
    boot->stack_segment = MD_MSDOS2_DEFAULT_STACK_SEGMENT;
    boot->memory_paragraphs = MD_MSDOS2_DEFAULT_MEMORY_PARAGRAPHS;
}

void md_msdos2_boot_install_devices(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    MdX86 *cpu = &runtime->cpu;
    const uint16_t seg = boot->bios_segment;
    static const char kCon[8] = {'C','O','N',' ',' ',' ',' ',' '};
    static const char kAux[8] = {'A','U','X',' ',' ',' ',' ',' '};
    static const char kPrn[8] = {'P','R','N',' ',' ',' ',' ',' '};
    static const char kClock[8] = {'C','L','O','C','K',' ',' ',' '};
    static const char kDisk[8] = {1,0,0,0,0,0,0,0};

    md_write_device_header(cpu, seg, MD_MSDOS2_CON_OFFSET,
                           MD_MSDOS2_AUX_OFFSET, seg,
                           (uint16_t)(MD_DEV_ATTR_CHAR | MD_DEV_ATTR_CONSOLE), kCon);
    md_write_device_header(cpu, seg, MD_MSDOS2_AUX_OFFSET,
                           MD_MSDOS2_PRN_OFFSET, seg,
                           MD_DEV_ATTR_CHAR, kAux);
    md_write_device_header(cpu, seg, MD_MSDOS2_PRN_OFFSET,
                           MD_MSDOS2_CLOCK_OFFSET, seg,
                           MD_DEV_ATTR_CHAR, kPrn);
    md_write_device_header(cpu, seg, MD_MSDOS2_CLOCK_OFFSET,
                           MD_MSDOS2_DISK_OFFSET, seg,
                           (uint16_t)(MD_DEV_ATTR_CHAR | MD_DEV_ATTR_CLOCK), kClock);
    md_write_device_header(cpu, seg, MD_MSDOS2_DISK_OFFSET,
                           0xFFFFu, 0xFFFFu,
                           MD_DEV_ATTR_BLOCK_FAT_ID, kDisk);

    /* Shared native strategy and interrupt trampolines. RETF returns from the
       far device-driver calls DOS performs through the header. */
    md_x86_write8(cpu, seg, MD_MSDOS2_STRATEGY_OFFSET + 0u, 0xCDu);
    md_x86_write8(cpu, seg, MD_MSDOS2_STRATEGY_OFFSET + 1u, MD_MSDOS2_NATIVE_STRATEGY_INT);
    md_x86_write8(cpu, seg, MD_MSDOS2_STRATEGY_OFFSET + 2u, 0xCBu);

    md_x86_write8(cpu, seg, MD_MSDOS2_INTERRUPT_OFFSET + 0u, 0xCDu);
    md_x86_write8(cpu, seg, MD_MSDOS2_INTERRUPT_OFFSET + 1u, MD_MSDOS2_NATIVE_DEVICE_INT);
    md_x86_write8(cpu, seg, MD_MSDOS2_INTERRUPT_OFFSET + 2u, 0xCBu);

    /* Synthetic SYSINIT return address. */
    md_x86_write8(cpu, seg, MD_MSDOS2_RETURN_OFFSET + 0u, 0xCDu);
    md_x86_write8(cpu, seg, MD_MSDOS2_RETURN_OFFSET + 1u, MD_MSDOS2_NATIVE_RETURN_INT);
    md_x86_write8(cpu, seg, MD_MSDOS2_RETURN_OFFSET + 2u, 0xF4u);

    /* One-entry BPB pointer table and a conventional 360 KiB FAT12 BPB.
       DOS 2.x consumes the 13-byte BPB beginning with bytes/sector. */
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_TABLE_OFFSET, MD_MSDOS2_BPB_OFFSET);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 0u, 512u);
    md_x86_write8(cpu, seg, MD_MSDOS2_BPB_OFFSET + 2u, 2u);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 3u, 1u);
    md_x86_write8(cpu, seg, MD_MSDOS2_BPB_OFFSET + 5u, 2u);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 6u, 112u);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 8u, 720u);
    md_x86_write8(cpu, seg, MD_MSDOS2_BPB_OFFSET + 10u, 0xFDu);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 11u, 2u);

    md_runtime_mark_code_range(runtime, seg, MD_MSDOS2_STRATEGY_OFFSET, 3u);
    md_runtime_mark_code_range(runtime, seg, MD_MSDOS2_INTERRUPT_OFFSET, 3u);
    md_runtime_mark_code_range(runtime, seg, MD_MSDOS2_RETURN_OFFSET, 3u);
}

void md_msdos2_boot_prepare_cpu(MdRuntime *runtime, MdMsdos2Boot *boot,
                                const uint8_t *image, size_t size)
{
    MdX86 *cpu;

    md_runtime_load_raw(runtime, image, size, boot->dos_segment, 0u);
    md_msdos2_boot_install_devices(runtime, boot);

    cpu = &runtime->cpu;
    cpu->cs = boot->dos_segment;
    cpu->ip = 0u;
    cpu->ds = boot->bios_segment;
    cpu->r[MD_X86_SI] = MD_MSDOS2_CON_OFFSET;
    cpu->r[MD_X86_DX] = boot->memory_paragraphs;
    cpu->ss = boot->stack_segment;
    cpu->r[MD_X86_SP] = 0xFFFEu;
    cpu->flags = (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_IF);

    /* Recreate SYSINIT's FAR CALL MSDOS stack frame. RETF must pop IP then CS. */
    md_x86_push(cpu, boot->bios_segment);
    md_x86_push(cpu, MD_MSDOS2_RETURN_OFFSET);
}

static void md_device_status(MdRuntime *runtime, const MdMsdos2Boot *boot, uint16_t status)
{
    md_x86_write16(&runtime->cpu, boot->request_segment,
                   (uint16_t)(boot->request_offset + MD_REQ_STATUS), status);
}


static void md_console_write(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    MdX86 *cpu = &runtime->cpu;
    uint16_t data_offset;
    uint16_t data_segment;
    uint16_t count;
    uint16_t i;

    md_read_far(cpu, boot->request_segment,
                (uint16_t)(boot->request_offset + MD_REQ_TRANSFER),
                &data_offset, &data_segment);
    count = md_x86_read16(cpu, boot->request_segment,
                          (uint16_t)(boot->request_offset + MD_REQ_COUNT));

    ++boot->console_write_calls;
    boot->console_bytes_written += count;

    if (boot->console.write != NULL && count != 0u) {
        uint8_t chunk[128];
        uint16_t done = 0u;
        while (done < count) {
            const uint16_t remaining = (uint16_t)(count - done);
            const uint16_t take = remaining > (uint16_t)sizeof(chunk)
                                ? (uint16_t)sizeof(chunk) : remaining;
            for (i = 0u; i < take; ++i) {
                chunk[i] = md_x86_read8(cpu, data_segment,
                                        (uint16_t)(data_offset + done + i));
            }
            boot->console.write(boot->console.user, chunk, take);
            done = (uint16_t)(done + take);
        }
    }

    md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
}

static void md_console_read_nondestructive(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    uint8_t value = 0u;
    ++boot->console_poll_calls;

    if (boot->console.peek != NULL && boot->console.peek(boot->console.user, &value)) {
        md_x86_write8(&runtime->cpu, boot->request_segment,
                      (uint16_t)(boot->request_offset + MD_REQ_MEDIA), value);
        md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
    } else {
        /* DOS 2 sample drivers report BUSY|DONE for an empty non-destructive
           console read. This is not an error and must not set bit 15. */
        md_device_status(runtime, boot, (uint16_t)(MD_DEV_STATUS_BUSY | MD_DEV_STATUS_DONE));
    }
}

static bool md_console_read(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    MdX86 *cpu = &runtime->cpu;
    uint16_t data_offset;
    uint16_t data_segment;
    uint16_t count;
    uint16_t i;

    if (boot->console.read == NULL) return false;

    md_read_far(cpu, boot->request_segment,
                (uint16_t)(boot->request_offset + MD_REQ_TRANSFER),
                &data_offset, &data_segment);
    count = md_x86_read16(cpu, boot->request_segment,
                          (uint16_t)(boot->request_offset + MD_REQ_COUNT));

    for (i = 0u; i < count; ++i) {
        uint8_t value;
        if (!boot->console.read(boot->console.user, &value)) return false;
        md_x86_write8(cpu, data_segment, (uint16_t)(data_offset + i), value);
    }

    md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
    return true;
}

static bool md_console_service(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    switch (boot->last_request_function) {
        case MD_DEV_READ:
            return md_console_read(runtime, boot);
        case MD_DEV_READ_ND:
            md_console_read_nondestructive(runtime, boot);
            return true;
        case MD_DEV_INPUT_STATUS:
            md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
            return true;
        case MD_DEV_INPUT_FLUSH:
            if (boot->console.flush != NULL) boot->console.flush(boot->console.user);
            md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
            return true;
        case MD_DEV_WRITE:
        case MD_DEV_WRITE_VERIFY:
            md_console_write(runtime, boot);
            return true;
        case MD_DEV_OUTPUT_STATUS:
            md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
            return true;
        case MD_DEV_OUTPUT_FLUSH:
        case MD_DEV_IOCTL_WRITE:
            md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
            return true;
        default:
            return false;
    }
}

static void md_device_init(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    MdX86 *cpu = &runtime->cpu;

    ++boot->init_calls;
    md_device_status(runtime, boot, MD_DEV_STATUS_DONE);

    if (boot->last_device_offset == MD_MSDOS2_DISK_OFFSET) {
        md_x86_write8(cpu, boot->request_segment,
                      (uint16_t)(boot->request_offset + MD_REQ_INIT_UNITS), 1u);
        md_write_far(cpu, boot->request_segment,
                     (uint16_t)(boot->request_offset + MD_REQ_INIT_BREAK),
                     0x0400u, boot->bios_segment);
        md_write_far(cpu, boot->request_segment,
                     (uint16_t)(boot->request_offset + MD_REQ_INIT_BPB),
                     MD_MSDOS2_BPB_TABLE_OFFSET, boot->bios_segment);
    }
}

bool md_msdos2_boot_interrupt(MdRuntime *runtime, uint8_t vector, void *user)
{
    MdMsdos2Boot *boot = (MdMsdos2Boot *)user;
    MdX86 *cpu = &runtime->cpu;

    if (vector == MD_MSDOS2_NATIVE_STRATEGY_INT) {
        boot->request_segment = cpu->es;
        boot->request_offset = cpu->r[MD_X86_BX];
        boot->have_request = true;
        boot->last_device_offset = cpu->r[MD_X86_SI];
        ++boot->strategy_calls;
        return true;
    }

    if (vector == MD_MSDOS2_NATIVE_DEVICE_INT) {
        ++boot->device_calls;
        boot->last_device_offset = cpu->r[MD_X86_SI];
        if (!boot->have_request || !md_known_device_offset(boot->last_device_offset)) {
            ++boot->unknown_device_calls;
            if (boot->have_request) {
                md_device_status(runtime, boot,
                                 (uint16_t)(MD_DEV_STATUS_ERROR | MD_DEV_STATUS_DONE | 3u));
            }
            return true;
        }

        boot->last_request_function = md_x86_read8(cpu, boot->request_segment,
                                                   (uint16_t)(boot->request_offset + MD_REQ_FUNC));
        if (boot->last_request_function == MD_DEV_INIT) {
            md_device_init(runtime, boot);
        } else if (boot->last_device_offset == MD_MSDOS2_CON_OFFSET &&
                   md_console_service(runtime, boot)) {
            /* Handled by the DOS 2 character-device contract above. */
        } else {
            ++boot->unknown_device_calls;
            md_device_status(runtime, boot,
                             (uint16_t)(MD_DEV_STATUS_ERROR | MD_DEV_STATUS_DONE | 3u));
        }
        return true;
    }

    if (vector == MD_MSDOS2_NATIVE_RETURN_INT) {
        boot->returned_from_dosinit = true;
        runtime->stop_reason = MD_STOP_HALT;
        return true;
    }

    return false;
}
