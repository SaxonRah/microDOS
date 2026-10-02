#include "msdos2_boot.h"

#include <assert.h>
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

/* Static request header and command-specific DOS 2 request offsets. */
enum {
    MD_REQ_LEN = 0,
    MD_REQ_UNIT = 1,
    MD_REQ_FUNC = 2,
    MD_REQ_STATUS = 3,
    MD_REQ_MEDIA = 13,
    MD_REQ_MEDIA_RESULT = 14,
    MD_REQ_TRANSFER = 14,
    MD_REQ_COUNT = 18,
    MD_REQ_START = 20,
    MD_REQ_BPB = 18,
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
    MD_DEV_MEDIA_CHECK = 1u,
    MD_DEV_BUILD_BPB = 2u,
    MD_DEV_IOCTL_READ = 3u,
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

enum {
    MD_DEV_ERR_UNKNOWN_UNIT = 1u,
    MD_DEV_ERR_NOT_READY = 2u,
    MD_DEV_ERR_UNKNOWN_COMMAND = 3u,
    MD_DEV_ERR_SECTOR_NOT_FOUND = 8u,
    MD_DEV_ERR_WRITE_FAULT = 10u,
    MD_DEV_ERR_READ_FAULT = 11u
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
    boot->disk.sector_size = 512u;
    boot->disk.sector_count = 720u;
}

static void md_install_postinit_program(MdRuntime *runtime, const MdMsdos2Boot *boot)
{
    MdX86 *cpu = &runtime->cpu;
    const uint16_t s = boot->bios_segment;
    uint16_t p = MD_MSDOS2_POSTINIT_OFFSET;
    static const char kPath[] = "A:\\COMMAND.COM";
    static const char kConDev[] = "\\DEV\\CON";
    static const char kAuxDev[] = "\\DEV\\AUX";
    static const char kPrnDev[] = "\\DEV\\PRN";
    uint16_t jc_open_con;
    uint16_t jc_dup1;
    uint16_t jc_dup2;
    uint16_t stdio_fail;
    unsigned i;

#define EMIT8(v) md_x86_write8(cpu, s, p++, (uint8_t)(v))
#define EMIT16(v) do { const uint16_t md_v_ = (uint16_t)(v); EMIT8(md_v_ & 0xffu); EMIT8(md_v_ >> 8); } while (0)
#define PATCH_JC(at, target) \
    md_x86_write8(cpu, s, (at), (uint8_t)((target) - (uint16_t)((at) + 1u)))

    /* M12.3: recreate SYSINIT's standard-handle setup (SYSINIT.ASM, just
       before GOSET). DOSINIT only leaves JFN 0/1/2 pointing at a bootstrap
       SFT entry 0 that is good enough for DOS's internal console messages
       but was never created by $Open, so its sf_FCB.fcb_RECSIZ is not 1.
       Handle I/O is record I/O in DOS 2 ($Write -> random block write), so
       an unopened entry turns a 16-byte AH=40h write into 16*128 bytes.
       SYSINIT therefore closes the bootstrap handles, OPENs \DEV\CON (which
       sets RECSIZ=1, "byte io only") and XDUPs it to STDOUT/STDERR. */
    EMIT8(0x0Eu);                    /* PUSH CS */
    EMIT8(0x1Fu);                    /* POP DS */
    EMIT8(0x33u); EMIT8(0xDBu);      /* XOR BX,BX */
    EMIT8(0xB4u); EMIT8(0x3Eu);      /* MOV AH,CLOSE */
    EMIT8(0xCDu); EMIT8(0x21u);      /* close standard input */
    EMIT8(0xBBu); EMIT16(2u);        /* MOV BX,2 */
    EMIT8(0xB9u); EMIT16(MD_MSDOS2_SYSINIT_FILES); /* MOV CX,[FILES] */
    /* RCCLLOOP: close everybody but standard output. */
    EMIT8(0xB4u); EMIT8(0x3Eu);      /* MOV AH,CLOSE */
    EMIT8(0xCDu); EMIT8(0x21u);      /* INT 21h (errors ignored, as SYSINIT) */
    EMIT8(0x43u);                    /* INC BX */
    EMIT8(0xE2u); EMIT8(0xF9u);      /* LOOP RCCLLOOP */

    EMIT8(0xBAu); EMIT16(MD_MSDOS2_CONDEV_OFFSET); /* MOV DX,CONDEV */
    EMIT8(0xB8u); EMIT16(0x3D02u);   /* MOV AX,3D02h  OPEN read/write */
    EMIT8(0xF9u);                    /* STC */
    EMIT8(0xCDu); EMIT8(0x21u);      /* INT 21h */
    EMIT8(0x72u); jc_open_con = p; EMIT8(0x00u); /* JC stdio_fail */
    EMIT8(0x50u);                    /* PUSH AX */
    EMIT8(0xBBu); EMIT16(1u);        /* MOV BX,1 */
    EMIT8(0xB4u); EMIT8(0x3Eu);      /* MOV AH,CLOSE */
    EMIT8(0xCDu); EMIT8(0x21u);      /* close standard output */
    EMIT8(0x58u);                    /* POP AX */
    EMIT8(0x8Bu); EMIT8(0xD8u);      /* MOV BX,AX  new CON handle */
    EMIT8(0xB4u); EMIT8(0x45u);      /* MOV AH,XDUP */
    EMIT8(0xCDu); EMIT8(0x21u);      /* dup to 1, STDOUT */
    EMIT8(0x72u); jc_dup1 = p; EMIT8(0x00u); /* JC stdio_fail */
    EMIT8(0xB4u); EMIT8(0x45u);      /* MOV AH,XDUP */
    EMIT8(0xCDu); EMIT8(0x21u);      /* dup to 2, STDERR */
    EMIT8(0x72u); jc_dup2 = p; EMIT8(0x00u); /* JC stdio_fail */

    /* GOAUX2: AUX (read/write) and PRN (write only). SYSINIT's OPEN_DEV
       falls back to NUL on failure; these are optional for COMMAND.COM, so
       failure is simply ignored here. */
    EMIT8(0xBAu); EMIT16(MD_MSDOS2_AUXDEV_OFFSET); /* MOV DX,AUXDEV */
    EMIT8(0xB8u); EMIT16(0x3D02u);   /* MOV AX,3D02h */
    EMIT8(0xF9u);                    /* STC */
    EMIT8(0xCDu); EMIT8(0x21u);
    EMIT8(0xBAu); EMIT16(MD_MSDOS2_PRNDEV_OFFSET); /* MOV DX,PRNDEV */
    EMIT8(0xB8u); EMIT16(0x3D01u);   /* MOV AX,3D01h */
    EMIT8(0xF9u);                    /* STC */
    EMIT8(0xCDu); EMIT8(0x21u);

    /* Match the EXEC path in Microsoft's SYSINIT.ASM: DS=ES=SYSINIT,
       DS:DX -> COMMAND.COM, ES:BX -> Exec0, AX=4B00h. */
    EMIT8(0x0Eu);                    /* PUSH CS */
    EMIT8(0x1Fu);                    /* POP DS */
    EMIT8(0x0Eu);                    /* PUSH CS */
    EMIT8(0x07u);                    /* POP ES */
    EMIT8(0xBAu); EMIT16(MD_MSDOS2_POSTINIT_PATH_OFFSET); /* MOV DX,path */
    EMIT8(0xBBu); EMIT16(MD_MSDOS2_EXEC_BLOCK_OFFSET);    /* MOV BX,Exec0 */
    EMIT8(0xB8u); EMIT16(0x4B00u);   /* MOV AX,4B00h */
    EMIT8(0xCDu); EMIT8(0x21u);      /* EXEC; success transfers to child. */
    EMIT8(0x72u); EMIT8(0x03u);      /* JC exec_failed */
    EMIT8(0xCDu); EMIT8(MD_MSDOS2_NATIVE_POSTINIT_OK_INT); /* child returned */
    EMIT8(0xF4u);
    EMIT8(0xCDu); EMIT8(MD_MSDOS2_NATIVE_POSTINIT_FAIL_INT);
    EMIT8(0xF4u);

    stdio_fail = p;
    EMIT8(0xCDu); EMIT8(MD_MSDOS2_NATIVE_POSTINIT_STDIO_FAIL_INT);
    EMIT8(0xF4u);

    PATCH_JC(jc_open_con, stdio_fail);
    PATCH_JC(jc_dup1, stdio_fail);
    PATCH_JC(jc_dup2, stdio_fail);

    /* The program must not run into the path string that follows it. */
    assert(p <= MD_MSDOS2_POSTINIT_PATH_OFFSET);
    assert((uint16_t)(stdio_fail - (jc_open_con + 1u)) < 0x80u);

#undef PATCH_JC
#undef EMIT16
#undef EMIT8

    for (i = 0u; i < sizeof(kPath); ++i) {
        md_x86_write8(cpu, s, (uint16_t)(MD_MSDOS2_POSTINIT_PATH_OFFSET + i),
                      (uint8_t)kPath[i]);
    }
    for (i = 0u; i < sizeof(kConDev); ++i) {
        md_x86_write8(cpu, s, (uint16_t)(MD_MSDOS2_CONDEV_OFFSET + i), (uint8_t)kConDev[i]);
        md_x86_write8(cpu, s, (uint16_t)(MD_MSDOS2_AUXDEV_OFFSET + i), (uint8_t)kAuxDev[i]);
        md_x86_write8(cpu, s, (uint16_t)(MD_MSDOS2_PRNDEV_OFFSET + i), (uint8_t)kPrnDev[i]);
    }

    /* DOS 2 Exec0: environment segment, command-tail FAR ptr, FCB1 FAR ptr,
       FCB2 FAR ptr. Environment 0 means inherit the current PDB environment. */
    md_x86_write16(cpu, s, MD_MSDOS2_EXEC_BLOCK_OFFSET + 0u, 0u);
    md_write_far(cpu, s, MD_MSDOS2_EXEC_BLOCK_OFFSET + 2u,
                 MD_MSDOS2_COMMAND_TAIL_OFFSET, s);
    md_write_far(cpu, s, MD_MSDOS2_EXEC_BLOCK_OFFSET + 6u,
                 MD_MSDOS2_FCB1_OFFSET, s);
    md_write_far(cpu, s, MD_MSDOS2_EXEC_BLOCK_OFFSET + 10u,
                 MD_MSDOS2_FCB2_OFFSET, s);

    /* COMMAND.COM's permanent-shell tail: length=2, "/P", CR. EXEC copies
       all 128 bytes into the child's PSP at offset 80h. */
    for (i = 0u; i < 128u; ++i) {
        md_x86_write8(cpu, s, (uint16_t)(MD_MSDOS2_COMMAND_TAIL_OFFSET + i), 0u);
    }
    md_x86_write8(cpu, s, MD_MSDOS2_COMMAND_TAIL_OFFSET + 0u, 2u);
    md_x86_write8(cpu, s, MD_MSDOS2_COMMAND_TAIL_OFFSET + 1u, (uint8_t)'/');
    md_x86_write8(cpu, s, MD_MSDOS2_COMMAND_TAIL_OFFSET + 2u, (uint8_t)'P');
    md_x86_write8(cpu, s, MD_MSDOS2_COMMAND_TAIL_OFFSET + 3u, 0x0Du);

    /* Default FCBs: drive 0, blank/zero name. DOS only needs the first 12
       bytes from each pointer while constructing the child PSP. */
    for (i = 0u; i < 16u; ++i) {
        md_x86_write8(cpu, s, (uint16_t)(MD_MSDOS2_FCB1_OFFSET + i), 0u);
        md_x86_write8(cpu, s, (uint16_t)(MD_MSDOS2_FCB2_OFFSET + i), 0u);
    }

    md_runtime_mark_code_range(runtime, s, MD_MSDOS2_POSTINIT_OFFSET,
                               (uint32_t)(p - MD_MSDOS2_POSTINIT_OFFSET));
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

    /* One-entry BPB pointer table and conventional 360 KiB FAT12 BPB. */
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_TABLE_OFFSET, MD_MSDOS2_BPB_OFFSET);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 0u, 512u);
    md_x86_write8(cpu, seg, MD_MSDOS2_BPB_OFFSET + 2u, 2u);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 3u, 1u);
    md_x86_write8(cpu, seg, MD_MSDOS2_BPB_OFFSET + 5u, 2u);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 6u, 112u);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 8u, 720u);
    md_x86_write8(cpu, seg, MD_MSDOS2_BPB_OFFSET + 10u, 0xFDu);
    md_x86_write16(cpu, seg, MD_MSDOS2_BPB_OFFSET + 11u, 2u);

    md_install_postinit_program(runtime, boot);

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
    md_x86_set_flags(cpu, (uint16_t)(MD_X86_FLAG_ALWAYS1 | MD_X86_FLAG_IF));

    /* Recreate SYSINIT's FAR CALL MSDOS stack frame. RETF pops IP then CS. */
    md_x86_push(cpu, boot->bios_segment);
    md_x86_push(cpu, MD_MSDOS2_RETURN_OFFSET);
}

static void md_device_status(MdRuntime *runtime, const MdMsdos2Boot *boot, uint16_t status)
{
    md_x86_write16(&runtime->cpu, boot->request_segment,
                   (uint16_t)(boot->request_offset + MD_REQ_STATUS), status);
}

static void md_device_error(MdRuntime *runtime, const MdMsdos2Boot *boot, uint8_t code)
{
    md_device_status(runtime, boot,
                     (uint16_t)(MD_DEV_STATUS_ERROR | MD_DEV_STATUS_DONE | code));
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

    if (count != 0u) {
        for (i = 0u; i < count; ++i) {
            const uint8_t value = md_x86_read8(cpu, data_segment,
                                                (uint16_t)(data_offset + i));
            if (value == (uint8_t)'$') {
                ++boot->console_dollar_writes;
                if (!boot->console_first_dollar_valid) {
                    boot->console_first_dollar_valid = true;
                    boot->console_first_dollar_data_segment = data_segment;
                    boot->console_first_dollar_data_offset = (uint16_t)(data_offset + i);
                    boot->console_first_dollar_count = count;
                    boot->console_first_dollar_request_segment = boot->request_segment;
                    boot->console_first_dollar_request_offset = boot->request_offset;

                    /* The native F1 interrupt executes inside the shared device
                       trampoline. The far CALL made by DOS still has its return
                       IP:CS at SS:SP, so retain that call-site boundary too. */
                    boot->console_first_dollar_return_offset =
                        md_x86_read16(cpu, cpu->ss, cpu->r[MD_X86_SP]);
                    boot->console_first_dollar_return_segment =
                        md_x86_read16(cpu, cpu->ss,
                                      (uint16_t)(cpu->r[MD_X86_SP] + 2u));
                }
            }
        }
    }

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

    ++boot->console_read_calls;
    for (i = 0u; i < count; ++i) {
        uint8_t value;
        if (!boot->console.read(boot->console.user, &value)) return false;
        md_x86_write8(cpu, data_segment, (uint16_t)(data_offset + i), value);
        ++boot->console_bytes_read;
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

static bool md_clock_service(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    MdX86 *cpu = &runtime->cpu;
    uint16_t data_offset;
    uint16_t data_segment;

    if (boot->last_request_function != MD_DEV_READ &&
        boot->last_request_function != MD_DEV_WRITE &&
        boot->last_request_function != MD_DEV_WRITE_VERIFY) {
        return false;
    }

    md_read_far(cpu, boot->request_segment,
                (uint16_t)(boot->request_offset + MD_REQ_TRANSFER),
                &data_offset, &data_segment);

    if (boot->last_request_function == MD_DEV_READ) {
        ++boot->clock_read_calls;
        md_x86_write16(cpu, data_segment, data_offset, boot->clock_days);
        md_x86_write8(cpu, data_segment, (uint16_t)(data_offset + 2u), boot->clock_minutes);
        md_x86_write8(cpu, data_segment, (uint16_t)(data_offset + 3u), boot->clock_hours);
        md_x86_write8(cpu, data_segment, (uint16_t)(data_offset + 4u), boot->clock_hundredths);
        md_x86_write8(cpu, data_segment, (uint16_t)(data_offset + 5u), boot->clock_seconds);
    } else {
        ++boot->clock_write_calls;
        boot->clock_days = md_x86_read16(cpu, data_segment, data_offset);
        boot->clock_minutes = md_x86_read8(cpu, data_segment, (uint16_t)(data_offset + 2u));
        boot->clock_hours = md_x86_read8(cpu, data_segment, (uint16_t)(data_offset + 3u));
        boot->clock_hundredths = md_x86_read8(cpu, data_segment, (uint16_t)(data_offset + 4u));
        boot->clock_seconds = md_x86_read8(cpu, data_segment, (uint16_t)(data_offset + 5u));
    }

    md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
    return true;
}

static bool md_disk_transfer(MdRuntime *runtime, MdMsdos2Boot *boot, bool write)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t unit = md_x86_read8(cpu, boot->request_segment,
                                      (uint16_t)(boot->request_offset + MD_REQ_UNIT));
    uint16_t data_offset;
    uint16_t data_segment;
    uint16_t count;
    uint16_t start;
    uint16_t sector_index;
    uint16_t i;
    uint8_t sector[512];

    if (unit != 0u) {
        md_device_error(runtime, boot, MD_DEV_ERR_UNKNOWN_UNIT);
        return true;
    }
    if (boot->disk.sector_size != sizeof(sector) || boot->disk.sector_count == 0u) {
        md_device_error(runtime, boot, MD_DEV_ERR_NOT_READY);
        return true;
    }

    md_read_far(cpu, boot->request_segment,
                (uint16_t)(boot->request_offset + MD_REQ_TRANSFER),
                &data_offset, &data_segment);
    count = md_x86_read16(cpu, boot->request_segment,
                          (uint16_t)(boot->request_offset + MD_REQ_COUNT));
    start = md_x86_read16(cpu, boot->request_segment,
                          (uint16_t)(boot->request_offset + MD_REQ_START));

    if ((uint32_t)start + count > boot->disk.sector_count) {
        md_device_error(runtime, boot, MD_DEV_ERR_SECTOR_NOT_FOUND);
        return true;
    }

    if (write) ++boot->disk_write_calls;
    else ++boot->disk_read_calls;

    for (sector_index = 0u; sector_index < count; ++sector_index) {
        const uint32_t lba = (uint32_t)start + sector_index;
        const uint16_t guest_base = (uint16_t)(data_offset +
                                  (uint16_t)(sector_index * boot->disk.sector_size));

        if (write) {
            if (!boot->disk.writable || boot->disk.write == NULL) {
                md_device_error(runtime, boot, MD_DEV_ERR_WRITE_FAULT);
                return true;
            }
            for (i = 0u; i < boot->disk.sector_size; ++i) {
                sector[i] = md_x86_read8(cpu, data_segment,
                                         (uint16_t)(guest_base + i));
            }
            if (!boot->disk.write(boot->disk.user, lba, sector, boot->disk.sector_size)) {
                md_device_error(runtime, boot, MD_DEV_ERR_WRITE_FAULT);
                return true;
            }
            ++boot->disk_sectors_written;
        } else {
            if (boot->disk.read == NULL ||
                !boot->disk.read(boot->disk.user, lba, sector, boot->disk.sector_size)) {
                md_device_error(runtime, boot, MD_DEV_ERR_READ_FAULT);
                return true;
            }
            md_x86_write_block(cpu, data_segment, guest_base, sector, boot->disk.sector_size);
            ++boot->disk_sectors_read;
        }
    }

    md_x86_write16(cpu, boot->request_segment,
                   (uint16_t)(boot->request_offset + MD_REQ_COUNT), count);
    md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
    return true;
}

static bool md_disk_service(MdRuntime *runtime, MdMsdos2Boot *boot)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t unit = md_x86_read8(cpu, boot->request_segment,
                                      (uint16_t)(boot->request_offset + MD_REQ_UNIT));

    switch (boot->last_request_function) {
        case MD_DEV_MEDIA_CHECK:
            ++boot->disk_media_checks;
            if (unit != 0u) {
                md_device_error(runtime, boot, MD_DEV_ERR_UNKNOWN_UNIT);
            } else {
                /* Fixed host image: media has not changed. */
                md_x86_write8(cpu, boot->request_segment,
                              (uint16_t)(boot->request_offset + MD_REQ_MEDIA_RESULT), 1u);
                md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
            }
            return true;

        case MD_DEV_BUILD_BPB:
            ++boot->disk_bpb_calls;
            if (unit != 0u) {
                md_device_error(runtime, boot, MD_DEV_ERR_UNKNOWN_UNIT);
            } else {
                md_x86_write8(cpu, boot->request_segment,
                              (uint16_t)(boot->request_offset + MD_REQ_MEDIA), 0xFDu);
                md_write_far(cpu, boot->request_segment,
                             (uint16_t)(boot->request_offset + MD_REQ_BPB),
                             MD_MSDOS2_BPB_OFFSET, boot->bios_segment);
                md_device_status(runtime, boot, MD_DEV_STATUS_DONE);
            }
            return true;

        case MD_DEV_READ:
            return md_disk_transfer(runtime, boot, false);
        case MD_DEV_WRITE:
        case MD_DEV_WRITE_VERIFY:
            return md_disk_transfer(runtime, boot, true);
        case MD_DEV_IOCTL_READ:
        case MD_DEV_IOCTL_WRITE:
            md_device_error(runtime, boot, MD_DEV_ERR_UNKNOWN_COMMAND);
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
                md_device_error(runtime, boot, MD_DEV_ERR_UNKNOWN_COMMAND);
            }
            return true;
        }

        boot->last_request_function = md_x86_read8(cpu, boot->request_segment,
                                                   (uint16_t)(boot->request_offset + MD_REQ_FUNC));
        if (boot->last_request_function == MD_DEV_INIT) {
            md_device_init(runtime, boot);
        } else if (boot->last_device_offset == MD_MSDOS2_CON_OFFSET &&
                   md_console_service(runtime, boot)) {
            /* Handled by the DOS 2 character-device contract. */
        } else if (boot->last_device_offset == MD_MSDOS2_CLOCK_OFFSET &&
                   md_clock_service(runtime, boot)) {
            /* DOS 2 CLOCK$ six-byte date/time packet. */
        } else if (boot->last_device_offset == MD_MSDOS2_DISK_OFFSET &&
                   md_disk_service(runtime, boot)) {
            /* Handled by the DOS 2 block-device contract. */
        } else {
            ++boot->unknown_device_calls;
            md_device_error(runtime, boot, MD_DEV_ERR_UNKNOWN_COMMAND);
        }
        return true;
    }

    if (vector == MD_MSDOS2_NATIVE_RETURN_INT) {
        boot->returned_from_dosinit = true;
        if (boot->continue_after_dosinit) {
            boot->postinit_started = true;
            cpu->cs = boot->bios_segment;
            cpu->ip = MD_MSDOS2_POSTINIT_OFFSET;
        } else {
            runtime->stop_reason = MD_STOP_HALT;
        }
        return true;
    }

    if (vector == MD_MSDOS2_NATIVE_POSTINIT_OK_INT) {
        /* A successful EXEC normally does not return until the child exits.
           Reaching this hook therefore means COMMAND.COM returned. */
        boot->postinit_completed = true;
        boot->postinit_succeeded = true;
        boot->postinit_error = 0u;
        runtime->stop_reason = MD_STOP_HALT;
        return true;
    }

    if (vector == MD_MSDOS2_NATIVE_POSTINIT_STDIO_FAIL_INT) {
        /* OPEN \DEV\CON or an XDUP failed: COMMAND.COM would otherwise run
           on the bootstrap SFT entry and every handle write would be
           multiplied by the default 128-byte record size. */
        boot->postinit_completed = true;
        boot->postinit_succeeded = false;
        boot->postinit_stdio_failed = true;
        boot->postinit_error = cpu->r[MD_X86_AX];
        runtime->stop_reason = MD_STOP_HALT;
        return true;
    }

    if (vector == MD_MSDOS2_NATIVE_POSTINIT_FAIL_INT) {
        boot->postinit_completed = true;
        boot->postinit_succeeded = false;
        boot->postinit_error = cpu->r[MD_X86_AX];
        runtime->stop_reason = MD_STOP_HALT;
        return true;
    }

    return false;
}
