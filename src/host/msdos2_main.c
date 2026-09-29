#include "microdos/block_cache.h"
#include "microdos/runtime.h"
#include "msdos2_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <conio.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

#define MD_MSDOS2_EXPECTED_SIZE 16690u
#define MD_DOSINIT_OFFSET 0x3E7Bu
#define MD_FAT12_IMAGE_SIZE (720u * 512u)

#define MD_TRACE_RING_SIZE 192u
#define MD_TRACE_CODE_BYTES 6u

typedef struct MdTraceEntry {
    uint64_t instruction;
    uint16_t cs;
    uint16_t ip;
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t bp;
    uint16_t si;
    uint16_t di;
    uint16_t ss;
    uint16_t sp;
    uint16_t ds;
    uint16_t es;
    uint16_t flags;
    uint8_t code[MD_TRACE_CODE_BYTES];
} MdTraceEntry;

typedef struct MdTraceRing {
    MdTraceEntry entries[MD_TRACE_RING_SIZE];
    size_t next;
    size_t count;
} MdTraceRing;

#define MD_ORIGIN_SAMPLE_BYTES 24u

typedef struct MdCommandStringScan {
    bool valid;
    bool post_valid;
    bool match_found;
    uint32_t sequence;
    uint64_t instruction;
    uint16_t cs;
    uint16_t ip;
    uint16_t ds;
    uint16_t es;
    uint16_t di;
    uint16_t cx;
    uint16_t flags;
    uint8_t al;
    uint32_t match_distance;
    uint16_t match_offset;
    uint8_t match_byte;
    uint16_t post_di;
    uint16_t post_cx;
    uint16_t post_flags;
} MdCommandStringScan;

typedef struct MdUserWriteOrigin {
    bool valid;
    bool last_valid;
    uint32_t sequence;
    uint64_t instruction;
    uint16_t cs;
    uint16_t ip;
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t ds;
    uint16_t es;
    uint16_t flags;
    uint8_t last_byte;
    uint8_t next_byte;
    uint16_t first_count;
    uint8_t first_bytes[MD_ORIGIN_SAMPLE_BYTES];
    uint16_t end_start;
    uint16_t end_count;
    uint8_t end_bytes[MD_ORIGIN_SAMPLE_BYTES];
} MdUserWriteOrigin;

static bool md_env_enabled(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') return false;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0 && strcmp(value, "no") != 0 &&
           strcmp(value, "NO") != 0;
}

static void md_trace_record(MdTraceRing *ring, const MdRuntime *runtime)
{
    MdTraceEntry *entry = &ring->entries[ring->next];
    const MdX86 *cpu = &runtime->cpu;
    unsigned i;

    memset(entry, 0, sizeof(*entry));
    entry->instruction = runtime->instructions;
    entry->cs = cpu->cs;
    entry->ip = cpu->ip;
    entry->ax = cpu->r[MD_X86_AX];
    entry->bx = cpu->r[MD_X86_BX];
    entry->cx = cpu->r[MD_X86_CX];
    entry->dx = cpu->r[MD_X86_DX];
    entry->bp = cpu->r[MD_X86_BP];
    entry->si = cpu->r[MD_X86_SI];
    entry->di = cpu->r[MD_X86_DI];
    entry->ss = cpu->ss;
    entry->sp = cpu->r[MD_X86_SP];
    entry->ds = cpu->ds;
    entry->es = cpu->es;
    entry->flags = cpu->flags;
    for (i = 0u; i < MD_TRACE_CODE_BYTES; ++i) {
        entry->code[i] = md_x86_read8(cpu, cpu->cs, (uint16_t)(cpu->ip + i));
    }

    ring->next = (ring->next + 1u) % MD_TRACE_RING_SIZE;
    if (ring->count < MD_TRACE_RING_SIZE) ++ring->count;
}


static bool md_command_string_scan_pattern(const MdX86 *cpu)
{
    static const uint8_t pattern[] = {
        0xF2u, 0xAEu,       /* REPNZ SCASB */
        0x07u,              /* POP ES */
        0xF7u, 0xD9u,       /* NEG CX */
        0x49u, 0x49u,       /* DEC CX / DEC CX */
        0xB4u, 0x40u,       /* MOV AH,40h */
        0xCDu, 0x21u        /* INT 21h */
    };
    size_t i;
    for (i = 0u; i < sizeof(pattern); ++i) {
        if (md_x86_read8(cpu, cpu->cs, (uint16_t)(cpu->ip + (uint16_t)i)) != pattern[i]) {
            return false;
        }
    }
    return true;
}

static bool md_capture_command_string_scan_pre(MdCommandStringScan *scan,
                                                uint32_t *sequence,
                                                const MdRuntime *runtime)
{
    const MdX86 *cpu = &runtime->cpu;
    uint32_t distance;
    int32_t step;

    if (!md_command_string_scan_pattern(cpu)) return false;

    memset(scan, 0, sizeof(*scan));
    scan->valid = true;
    scan->sequence = ++(*sequence);
    scan->instruction = runtime->instructions;
    scan->cs = cpu->cs;
    scan->ip = cpu->ip;
    scan->ds = cpu->ds;
    scan->es = cpu->es;
    scan->di = cpu->r[MD_X86_DI];
    scan->cx = cpu->r[MD_X86_CX];
    scan->flags = cpu->flags;
    scan->al = md_x86_get_reg8(cpu, 0u);

    step = (cpu->flags & MD_X86_FLAG_DF) != 0u ? -1 : 1;
    for (distance = 0u; distance < (uint32_t)scan->cx; ++distance) {
        const uint16_t offset = (uint16_t)((int32_t)scan->di +
                                           step * (int32_t)distance);
        if (md_x86_read8(cpu, scan->es, offset) == scan->al) {
            scan->match_found = true;
            scan->match_distance = distance;
            scan->match_offset = offset;
            scan->match_byte = scan->al;
            break;
        }
    }
    return true;
}

static void md_capture_command_string_scan_post(MdCommandStringScan *scan,
                                                 const MdRuntime *runtime)
{
    if (!scan->valid) return;
    scan->post_valid = true;
    scan->post_di = runtime->cpu.r[MD_X86_DI];
    scan->post_cx = runtime->cpu.r[MD_X86_CX];
    scan->post_flags = runtime->cpu.flags;
}

static bool md_is_user_write_call(const MdRuntime *runtime,
                                  const MdMsdos2Boot *boot)
{
    const MdX86 *cpu = &runtime->cpu;
    if (!boot->command_entered) return false;
    if (cpu->cs == boot->dos_segment || cpu->cs == boot->bios_segment) return false;
    if (md_x86_read8(cpu, cpu->cs, cpu->ip) != 0xCDu ||
        md_x86_read8(cpu, cpu->cs, (uint16_t)(cpu->ip + 1u)) != 0x21u) {
        return false;
    }
    if ((cpu->r[MD_X86_AX] >> 8) != 0x40u) return false;
    return cpu->r[MD_X86_BX] == 1u || cpu->r[MD_X86_BX] == 2u;
}

static void md_capture_user_write_origin(MdUserWriteOrigin *origin,
                                         uint32_t *sequence,
                                         const MdRuntime *runtime)
{
    const MdX86 *cpu = &runtime->cpu;
    const uint16_t count = cpu->r[MD_X86_CX];
    uint16_t sample;
    uint16_t i;

    memset(origin, 0, sizeof(*origin));
    origin->valid = true;
    origin->sequence = ++(*sequence);
    origin->instruction = runtime->instructions;
    origin->cs = cpu->cs;
    origin->ip = cpu->ip;
    origin->ax = cpu->r[MD_X86_AX];
    origin->bx = cpu->r[MD_X86_BX];
    origin->cx = count;
    origin->dx = cpu->r[MD_X86_DX];
    origin->ds = cpu->ds;
    origin->es = cpu->es;
    origin->flags = cpu->flags;

    sample = count < (uint16_t)(MD_ORIGIN_SAMPLE_BYTES - 1u)
        ? (uint16_t)(count + 1u) : (uint16_t)MD_ORIGIN_SAMPLE_BYTES;
    origin->first_count = sample;
    for (i = 0u; i < sample; ++i) {
        origin->first_bytes[i] = md_x86_read8(cpu, cpu->ds,
                                              (uint16_t)(origin->dx + i));
    }

    origin->end_start = count > 8u ? (uint16_t)(count - 8u) : 0u;
    origin->end_count = (uint16_t)MD_ORIGIN_SAMPLE_BYTES;
    for (i = 0u; i < origin->end_count; ++i) {
        origin->end_bytes[i] = md_x86_read8(cpu, cpu->ds,
                                            (uint16_t)(origin->dx +
                                                       origin->end_start + i));
    }

    if (count != 0u) {
        origin->last_valid = true;
        origin->last_byte = md_x86_read8(cpu, cpu->ds,
                                         (uint16_t)(origin->dx + count - 1u));
        origin->next_byte = md_x86_read8(cpu, cpu->ds,
                                         (uint16_t)(origin->dx + count));
    }
}

static void md_dump_hex_ascii(const char *label, const uint8_t *bytes,
                              uint16_t count, uint16_t start)
{
    uint16_t i;
    printf("[trace] %s +%u:", label, (unsigned)start);
    for (i = 0u; i < count; ++i) printf(" %02X", bytes[i]);
    printf("  |");
    for (i = 0u; i < count; ++i) {
        const uint8_t ch = bytes[i];
        putchar(ch >= 32u && ch < 127u ? (int)ch : '.');
    }
    puts("|");
}

static void md_dump_origin_trace(const MdUserWriteOrigin *origin,
                                 const MdCommandStringScan *scan)
{
    if (origin->valid) {
        printf("[trace] originating AH=40 write #%u at %04X:%04X instruction=%llu\n",
               (unsigned)origin->sequence, origin->cs, origin->ip,
               (unsigned long long)origin->instruction);
        printf("[trace] origin AX=%04X BX=%04X CX=%04X DS:DX=%04X:%04X ES=%04X F=%04X\n",
               origin->ax, origin->bx, origin->cx, origin->ds, origin->dx,
               origin->es, origin->flags);
        if (origin->last_valid) {
            printf("[trace] origin last_requested=%02X('%c') next=%02X('%c')"
                   " terminator_included=%s next_is_terminator=%s\n",
                   origin->last_byte,
                   origin->last_byte >= 32u && origin->last_byte < 127u
                       ? origin->last_byte : '.',
                   origin->next_byte,
                   origin->next_byte >= 32u && origin->next_byte < 127u
                       ? origin->next_byte : '.',
                   origin->last_byte == (uint8_t)'$' ? "yes" : "no",
                   origin->next_byte == (uint8_t)'$' ? "yes" : "no");
        }
        md_dump_hex_ascii("origin first bytes", origin->first_bytes,
                          origin->first_count, 0u);
        md_dump_hex_ascii("origin end window", origin->end_bytes,
                          origin->end_count, origin->end_start);
    } else {
        puts("[trace] no originating user AH=40 write was captured");
    }

    if (scan->valid) {
        uint16_t expected_cx = scan->cx;
        uint16_t expected_di = scan->di;
        uint16_t derived_count = 0u;
        bool expected_valid = false;

        if (scan->match_found && scan->match_distance < (uint32_t)scan->cx) {
            const uint16_t iterations = (uint16_t)(scan->match_distance + 1u);
            const int32_t direction = (scan->flags & MD_X86_FLAG_DF) != 0u ? -1 : 1;
            expected_cx = (uint16_t)(scan->cx - iterations);
            expected_di = (uint16_t)((int32_t)scan->di +
                                     direction * (int32_t)iterations);
            expected_valid = true;
        } else if (!scan->match_found) {
            const int32_t direction = (scan->flags & MD_X86_FLAG_DF) != 0u ? -1 : 1;
            expected_cx = 0u;
            expected_di = (uint16_t)((int32_t)scan->di +
                                     direction * (int32_t)scan->cx);
            expected_valid = true;
        }

        printf("[trace] COMMAND STRING_OUT scan #%u at %04X:%04X instruction=%llu\n",
               (unsigned)scan->sequence, scan->cs, scan->ip,
               (unsigned long long)scan->instruction);
        printf("[trace] scan pre AL=%02X('%c') CX=%04X DS=%04X ES:DI=%04X:%04X F=%04X DF=%u\n",
               scan->al,
               scan->al >= 32u && scan->al < 127u ? scan->al : '.',
               scan->cx, scan->ds, scan->es, scan->di, scan->flags,
               (scan->flags & MD_X86_FLAG_DF) != 0u ? 1u : 0u);
        if (scan->match_found) {
            printf("[trace] scan first_match distance=%u ES:%04X byte=%02X\n",
                   (unsigned)scan->match_distance, scan->match_offset,
                   scan->match_byte);
        } else {
            puts("[trace] scan first_match=none within initial CX");
        }
        if (scan->post_valid) {
            const bool zf = (scan->post_flags & MD_X86_FLAG_ZF) != 0u;
            printf("[trace] scan post CX=%04X DI=%04X F=%04X ZF=%u",
                   scan->post_cx, scan->post_di, scan->post_flags, zf ? 1u : 0u);
            if (expected_valid) {
                printf(" expected_CX=%04X expected_DI=%04X match=%s",
                       expected_cx, expected_di,
                       scan->post_cx == expected_cx && scan->post_di == expected_di
                           ? "yes" : "NO");
            }
            putchar('\n');
            if (scan->cx == 0xFFFFu) {
                derived_count = (uint16_t)(0u - scan->post_cx);
                derived_count = (uint16_t)(derived_count - 2u);
                printf("[trace] scan derived STRING_OUT count after NEG/DEC/DEC=%04X (%u)\n",
                       derived_count, (unsigned)derived_count);
                if (origin->valid) {
                    printf("[trace] scan-derived count vs originating AH=40 CX: %04X vs %04X (%s)\n",
                           derived_count, origin->cx,
                           derived_count == origin->cx ? "match" : "DIFFER");
                }
            }
        } else {
            puts("[trace] scan post-state was not captured");
        }
    } else {
        puts("[trace] no COMMAND STRING_OUT REPNZ SCASB was captured");
    }

    if (origin->valid && origin->last_valid) {
        if (origin->last_byte == (uint8_t)'$') {
            puts("[trace] boundary result: caller AH=40 count ALREADY INCLUDES '$'");
        } else if (origin->next_byte == (uint8_t)'$') {
            puts("[trace] boundary result: caller AH=40 excludes '$'; DOS wrote beyond caller CX");
        } else {
            puts("[trace] boundary result: '$' is neither caller last byte nor immediate next byte");
        }
    }
}

static void md_trace_dump(const MdTraceRing *ring, const MdRuntime *runtime,
                          const MdMsdos2Boot *boot,
                          const MdUserWriteOrigin *origin,
                          const MdCommandStringScan *scan)
{
    size_t i;
    const size_t first = (ring->next + MD_TRACE_RING_SIZE - ring->count) % MD_TRACE_RING_SIZE;
    const MdX86 *cpu = &runtime->cpu;

    puts("");
    puts("[trace] first post-COMMAND '$' byte reached the CON device");
    printf("[trace] request=%04X:%04X transfer=%04X:%04X count=%u driver_return=%04X:%04X\n",
           boot->console_first_dollar_request_segment,
           boot->console_first_dollar_request_offset,
           boot->console_first_dollar_data_segment,
           boot->console_first_dollar_data_offset,
           (unsigned)boot->console_first_dollar_count,
           boot->console_first_dollar_return_segment,
           boot->console_first_dollar_return_offset);
    printf("[trace] device-state CS:IP=%04X:%04X SS:SP=%04X:%04X\n",
           cpu->cs, cpu->ip, cpu->ss, cpu->r[MD_X86_SP]);
    md_dump_origin_trace(origin, scan);
    printf("[trace] stack words:");
    for (i = 0u; i < 12u; ++i) {
        printf(" %04X", md_x86_read16(cpu, cpu->ss,
                                      (uint16_t)(cpu->r[MD_X86_SP] + (uint16_t)(i * 2u))));
    }
    putchar('\n');
    printf("[trace] previous %zu guest instructions (state is before each instruction):\n",
           ring->count);

    for (i = 0u; i < ring->count; ++i) {
        const MdTraceEntry *entry = &ring->entries[(first + i) % MD_TRACE_RING_SIZE];
        const bool cmp_dollar = entry->code[0] == 0x3Cu && entry->code[1] == 0x24u;
        printf("[trace] %8llu %04X:%04X  %02X %02X %02X %02X %02X %02X"
               "  AX=%04X BX=%04X CX=%04X DX=%04X BP=%04X SI=%04X DI=%04X"
               " DS=%04X ES=%04X SS:SP=%04X:%04X F=%04X%s\n",
               (unsigned long long)entry->instruction,
               entry->cs, entry->ip,
               entry->code[0], entry->code[1], entry->code[2],
               entry->code[3], entry->code[4], entry->code[5],
               entry->ax, entry->bx, entry->cx, entry->dx,
               entry->bp, entry->si, entry->di,
               entry->ds, entry->es, entry->ss, entry->sp, entry->flags,
               cmp_dollar ? "  <CMP AL,'$'>" : "");
    }
    puts("[trace] diagnostic stop after first post-COMMAND '$' console write");
}

typedef struct MdHostDisk {
    uint8_t *data;
    size_t size;
    bool dirty;
} MdHostDisk;

typedef struct MdHostConsole {
    FILE *out;
    bool have_pending;
    uint8_t pending;
    bool quit_requested;
} MdHostConsole;

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

static bool md_host_console_fetch(MdHostConsole *console, bool block, uint8_t *value)
{
    int ch;

#if defined(_WIN32)
    if (!block && !_kbhit()) return false;
    ch = _getch();
    if (ch == 0 || ch == 0xE0) {
        /* Extended PC keys are two-byte console events. DOS 2's sample
           character driver discards null characters, so consume the scan
           code and wait for an ordinary byte. */
        if (block || _kbhit()) (void)_getch();
        return md_host_console_fetch(console, block, value);
    }
#else
    if (!block) {
        fd_set set;
        struct timeval timeout;
        int ready;
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        ready = select(STDIN_FILENO + 1, &set, NULL, NULL, &timeout);
        if (ready <= 0) return false;
    }
    {
        unsigned char byte;
        const ssize_t got = read(STDIN_FILENO, &byte, 1u);
        if (got != 1) return false;
        ch = (int)byte;
    }
#endif

    if (ch == 0x1D) { /* Ctrl+] is reserved as the host escape. */
        console->quit_requested = true;
        *value = 0x1Bu;
        return true;
    }
    if (ch == '\n') ch = '\r';
    *value = (uint8_t)ch;
    return true;
}

static bool md_write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) return false;
    if (size != 0u && fwrite(data, 1u, size, fp) != size) {
        fclose(fp);
        return false;
    }
    return fclose(fp) == 0;
}

static void md_host_console_write(void *user, const uint8_t *data, size_t size)
{
    MdHostConsole *console = (MdHostConsole *)user;
    if (size == 0u) return;
    (void)fwrite(data, 1u, size, console->out);
    (void)fflush(console->out);
}

static bool md_host_console_peek(void *user, uint8_t *value)
{
    MdHostConsole *console = (MdHostConsole *)user;
    if (!console->have_pending) {
        if (!md_host_console_fetch(console, false, &console->pending)) return false;
        console->have_pending = true;
    }
    *value = console->pending;
    return true;
}

static bool md_host_console_read(void *user, uint8_t *value)
{
    MdHostConsole *console = (MdHostConsole *)user;
    if (console->have_pending) {
        *value = console->pending;
        console->have_pending = false;
        return true;
    }
    return md_host_console_fetch(console, true, value);
}

static void md_host_console_flush(void *user)
{
    MdHostConsole *console = (MdHostConsole *)user;
    console->have_pending = false;
#if defined(_WIN32)
    while (_kbhit()) {
        int ch = _getch();
        if ((ch == 0 || ch == 0xE0) && _kbhit()) (void)_getch();
    }
#endif
}

static bool md_host_disk_read(void *user, uint32_t sector, uint8_t *data, size_t size)
{
    MdHostDisk *disk = (MdHostDisk *)user;
    const size_t offset = (size_t)sector * size;
    if (size != 512u || offset > disk->size || disk->size - offset < size) return false;
    memcpy(data, disk->data + offset, size);
    return true;
}

static bool md_host_disk_write(void *user, uint32_t sector, const uint8_t *data, size_t size)
{
    MdHostDisk *disk = (MdHostDisk *)user;
    const size_t offset = (size_t)sector * size;
    if (size != 512u || offset > disk->size || disk->size - offset < size) return false;
    memcpy(disk->data + offset, data, size);
    disk->dirty = true;
    return true;
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

static bool md_guest_command_image_matches(const MdRuntime *runtime,
                                           uint16_t segment,
                                           const uint8_t *command,
                                           size_t command_size)
{
    size_t i;
    if (command == NULL || command_size == 0u || command_size > 0xFF00u) return false;
    for (i = 0u; i < command_size; ++i) {
        if (md_x86_read8(&runtime->cpu, segment,
                         (uint16_t)(MD_MSDOS2_COMMAND_ENTRY_OFFSET + (uint16_t)i)) != command[i]) {
            return false;
        }
    }
    return true;
}

static bool md_guest_command_psp_valid(const MdRuntime *runtime, uint16_t segment)
{
    const MdX86 *cpu = &runtime->cpu;
    return md_x86_read8(cpu, segment, 0x0000u) == 0xCDu &&
           md_x86_read8(cpu, segment, 0x0001u) == 0x20u &&
           md_x86_read8(cpu, segment, 0x0080u) == 2u &&
           md_x86_read8(cpu, segment, 0x0081u) == (uint8_t)'/' &&
           md_x86_read8(cpu, segment, 0x0082u) == (uint8_t)'P' &&
           md_x86_read8(cpu, segment, 0x0083u) == 0x0Du &&
           cpu->cs == segment && cpu->ds == segment &&
           cpu->es == segment && cpu->ss == segment;
}

static bool md_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static uint16_t md_days_since_1980(int year, int month, int day)
{
    static const unsigned days_before_month[12] = {
        0u,31u,59u,90u,120u,151u,181u,212u,243u,273u,304u,334u
    };
    uint32_t days = 0u;
    int y;
    if (year < 1980) return 0u;
    for (y = 1980; y < year; ++y) days += md_is_leap_year(y) ? 366u : 365u;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    days += days_before_month[month - 1];
    if (month > 2 && md_is_leap_year(year)) ++days;
    if (day > 0) days += (uint32_t)(day - 1);
    return (uint16_t)(days & 0xFFFFu);
}

static void md_host_clock_init(MdMsdos2Boot *boot)
{
    const time_t now = time(NULL);
    struct tm local_tm;
#if defined(_WIN32)
    if (localtime_s(&local_tm, &now) != 0) return;
#else
    if (localtime_r(&now, &local_tm) == NULL) return;
#endif
    boot->clock_days = md_days_since_1980(local_tm.tm_year + 1900,
                                          local_tm.tm_mon + 1, local_tm.tm_mday);
    boot->clock_hours = (uint8_t)local_tm.tm_hour;
    boot->clock_minutes = (uint8_t)local_tm.tm_min;
    boot->clock_seconds = (uint8_t)local_tm.tm_sec;
    boot->clock_hundredths = 0u;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "third_party/msdos/v2.0/bin/MSDOS.SYS";
    uint64_t budget = 2000000u;
    const char *disk_path = NULL;
    const char *command_path = NULL;
    bool system_mode = false;
    uint8_t *image;
    size_t image_size;
    uint8_t *command = NULL;
    size_t command_size = 0u;
    MdHostDisk disk = {0};
    MdHostConsole console = {0};
    uint8_t *memory;
    MdRuntime runtime;
    MdBlockCache cache;
    MdMsdos2Boot boot;
    MdHooks hooks;
    uint64_t steps = 0u;
    uint32_t error_traces = 0u;
    uint32_t disk_traces = 0u;
    MdTraceRing trace_ring = {0};
    MdCommandStringScan command_scan = {0};
    MdUserWriteOrigin user_write = {0};
    uint32_t command_scan_sequence = 0u;
    uint32_t user_write_sequence = 0u;
    const bool trace_first_dollar = md_env_enabled("MICRODOS_TRACE_FIRST_DOLLAR");
    bool trace_stop = false;

    if (argc > 2) {
        budget = (uint64_t)strtoull(argv[2], NULL, 0);
        if (budget == 0u) budget = 1u;
    }
    if (argc > 3) {
        disk_path = argv[3];
        system_mode = true;
    }
    if (argc > 4) command_path = argv[4];

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

    if (system_mode) {
        disk.data = md_read_file(disk_path, &disk.size);
        if (disk.data == NULL || disk.size != MD_FAT12_IMAGE_SIZE) {
            fprintf(stderr, "microDOS: expected a 360 KiB FAT12 image at %s\n", disk_path);
            free(disk.data);
            free(image);
            return 2;
        }
        if (command_path != NULL) {
            command = md_read_file(command_path, &command_size);
            if (command == NULL) {
                fprintf(stderr, "microDOS: unable to read COMMAND.COM reference %s\n", command_path);
                free(disk.data);
                free(image);
                return 2;
            }
        }
    }

    memory = (uint8_t *)calloc(1u, MD_X86_ADDRESS_SPACE);
    if (memory == NULL) {
        free(command);
        free(disk.data);
        free(image);
        return 2;
    }

    md_msdos2_boot_init(&boot);
    md_host_clock_init(&boot);
    console.out = stdout;
    boot.console.write = md_host_console_write;
    boot.console.peek = md_host_console_peek;
    boot.console.read = md_host_console_read;
    boot.console.flush = md_host_console_flush;
    boot.console.user = &console;
    boot.continue_after_dosinit = system_mode;
    if (system_mode) {
        boot.disk.read = md_host_disk_read;
        boot.disk.write = md_host_disk_write;
        boot.disk.user = &disk;
        boot.disk.sector_size = 512u;
        boot.disk.sector_count = 720u;
        boot.disk.writable = true;
    }

    memset(&hooks, 0, sizeof(hooks));
    hooks.interrupt = md_msdos2_boot_interrupt;
    hooks.user = &boot;
    md_runtime_init(&runtime, memory, &hooks);
    md_block_cache_init(&cache);
    md_runtime_set_block_cache(&runtime, &cache);
    md_msdos2_boot_prepare_cpu(&runtime, &boot, image, image_size);

    printf("microDOS MS-DOS 2.0 %s\n", system_mode ? "system bring-up" : "kernel bring-up");
    printf("  image:   %s (%zu bytes)\n", path, image_size);
    printf("  kernel:  %04X:0000 -> DOSINIT %04X:%04X\n",
           boot.dos_segment, boot.dos_segment, MD_DOSINIT_OFFSET);
    printf("  devices: %04X:%04X CON -> AUX -> PRN -> CLOCK -> DISK\n",
           boot.bios_segment, MD_MSDOS2_CON_OFFSET);
    if (system_mode) printf("  disk:    %s (%zu bytes)\n", disk_path, disk.size);
    printf("  memory:  %u paragraphs (%u KiB)\n",
           boot.memory_paragraphs, (unsigned)(boot.memory_paragraphs / 64u));
    printf("  budget:  %llu guest instructions\n", (unsigned long long)budget);
    if (system_mode && trace_first_dollar) {
        puts("  trace:   stop/dump on first post-COMMAND '$' CON byte");
    }

    while (runtime.stop_reason == MD_STOP_NONE && steps < budget) {
        if (system_mode && command != NULL && !boot.command_entered &&
            runtime.cpu.ip == MD_MSDOS2_COMMAND_ENTRY_OFFSET &&
            runtime.cpu.cs != boot.dos_segment && runtime.cpu.cs != boot.bios_segment &&
            md_guest_command_image_matches(&runtime, runtime.cpu.cs, command, command_size)) {
            boot.command_entered = true;
            boot.command_segment = runtime.cpu.cs;
            boot.command_image_match = true;
            boot.command_psp_valid = md_guest_command_psp_valid(&runtime, runtime.cpu.cs);
            boot.console_dollar_writes = 0u;
            boot.console_first_dollar_valid = false;
            md_runtime_mark_code_range(&runtime, runtime.cpu.cs,
                                       MD_MSDOS2_COMMAND_ENTRY_OFFSET,
                                       (uint32_t)command_size);
            printf("[system] COMMAND.COM entered at %04X:%04X; image=%s PSP=%s\n",
                   boot.command_segment, MD_MSDOS2_COMMAND_ENTRY_OFFSET,
                   boot.command_image_match ? "match" : "mismatch",
                   boot.command_psp_valid ? "valid" : "invalid");
            puts("[system] COMMAND.COM execution continuing; interactive CON active (Ctrl+] exits microDOS)");
        }

        if (!boot.entered_dosinit && runtime.cpu.cs == boot.dos_segment &&
            runtime.cpu.ip == MD_DOSINIT_OFFSET) {
            boot.entered_dosinit = true;
            printf("[boot] entered DOSINIT at %04X:%04X after %llu instructions\n",
                   runtime.cpu.cs, runtime.cpu.ip,
                   (unsigned long long)runtime.instructions);
        }

        {
            const uint32_t before_calls = boot.device_calls;
            const uint32_t before_dollars = boot.console_dollar_writes;
            const bool before_return = boot.returned_from_dosinit;
            const bool before_post = boot.postinit_started;
            MdStopReason reason;

            bool captured_string_scan = false;

            if (trace_first_dollar && boot.command_entered) {
                md_trace_record(&trace_ring, &runtime);
                if (md_is_user_write_call(&runtime, &boot)) {
                    md_capture_user_write_origin(&user_write, &user_write_sequence, &runtime);
                }
                captured_string_scan =
                    md_capture_command_string_scan_pre(&command_scan,
                                                       &command_scan_sequence,
                                                       &runtime);
            }
            reason = md_interp_step(&runtime);
            ++steps;
            if (captured_string_scan) {
                md_capture_command_string_scan_post(&command_scan, &runtime);
            }

            if (!before_return && boot.returned_from_dosinit) {
                puts("[boot] DOSINIT returned to SYSINIT boundary");
            }
            if (!before_post && boot.postinit_started) {
                puts("[system] DOS EXEC: load/execute A:\\COMMAND.COM through INT 21h AH=4B00h");
            }

            if (boot.device_calls != before_calls) {
                const uint16_t status = md_x86_read16(&runtime.cpu, boot.request_segment,
                                                       (uint16_t)(boot.request_offset + 3u));
                if (boot.last_request_function == 0u) {
                    printf("[device] %-5s func=%u request=%04X:%04X status=%04X\n",
                           md_msdos2_device_name(boot.last_device_offset),
                           boot.last_request_function,
                           boot.request_segment, boot.request_offset, status);
                } else if (system_mode && boot.last_device_offset == MD_MSDOS2_DISK_OFFSET &&
                           (status & 0x8000u) == 0u && disk_traces < 24u) {
                    if (boot.last_request_function == 4u || boot.last_request_function == 8u ||
                        boot.last_request_function == 9u) {
                        const uint16_t count = md_x86_read16(&runtime.cpu, boot.request_segment,
                                                              (uint16_t)(boot.request_offset + 18u));
                        const uint16_t start = md_x86_read16(&runtime.cpu, boot.request_segment,
                                                              (uint16_t)(boot.request_offset + 20u));
                        printf("[disk] func=%u start=%u count=%u status=%04X\n",
                               boot.last_request_function, start, count, status);
                    } else {
                        printf("[disk] func=%u status=%04X\n",
                               boot.last_request_function, status);
                    }
                    ++disk_traces;
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
            if (trace_first_dollar && boot.command_entered && !trace_stop &&
                boot.console_dollar_writes != before_dollars &&
                boot.console_first_dollar_valid) {
                md_trace_dump(&trace_ring, &runtime, &boot,
                              &user_write, &command_scan);
                trace_stop = true;
                runtime.stop_reason = MD_STOP_HALT;
            }
            if (console.quit_requested && runtime.stop_reason == MD_STOP_NONE) {
                puts("\n[host] Ctrl+] requested; stopping microDOS");
                runtime.stop_reason = MD_STOP_HALT;
            }
            if (reason != MD_STOP_NONE || runtime.stop_reason != MD_STOP_NONE) break;
        }
    }

    if (runtime.stop_reason == MD_STOP_NONE && steps >= budget) {
        runtime.stop_reason = MD_STOP_BUDGET;
    }

    printf("\n[boot] stop=%s instructions=%llu strategy=%u device=%u init=%u unknown=%u\n",
           trace_stop ? "diagnostic" : md_stop_reason_name(runtime.stop_reason),
           (unsigned long long)runtime.instructions,
           (unsigned)boot.strategy_calls,
           (unsigned)boot.device_calls,
           (unsigned)boot.init_calls,
           (unsigned)boot.unknown_device_calls);
    printf("[boot] console polls=%u reads=%u bytes_read=%u writes=%u bytes_written=%u dollars=%u\n",
           (unsigned)boot.console_poll_calls,
           (unsigned)boot.console_read_calls,
           (unsigned)boot.console_bytes_read,
           (unsigned)boot.console_write_calls,
           (unsigned)boot.console_bytes_written,
           (unsigned)boot.console_dollar_writes);
    printf("[boot] dosinit_entered=%s dosinit_returned=%s\n",
           boot.entered_dosinit ? "yes" : "no",
           boot.returned_from_dosinit ? "yes" : "no");
    if (boot.clock_read_calls != 0u || boot.clock_write_calls != 0u) {
        printf("[clock] reads=%u writes=%u days=%u time=%02u:%02u:%02u.%02u\n",
               (unsigned)boot.clock_read_calls, (unsigned)boot.clock_write_calls,
               (unsigned)boot.clock_days, (unsigned)boot.clock_hours,
               (unsigned)boot.clock_minutes, (unsigned)boot.clock_seconds,
               (unsigned)boot.clock_hundredths);
    }

    if (system_mode) {
        printf("[disk] media=%u bpb=%u reads=%u sectors_read=%u writes=%u sectors_written=%u\n",
               (unsigned)boot.disk_media_checks,
               (unsigned)boot.disk_bpb_calls,
               (unsigned)boot.disk_read_calls,
               (unsigned)boot.disk_sectors_read,
               (unsigned)boot.disk_write_calls,
               (unsigned)boot.disk_sectors_written);
        printf("[system] exec_returned=%s exec_return_ok=%s exec_error=%u\n",
               boot.postinit_completed ? "yes" : "no",
               boot.postinit_succeeded ? "yes" : "no",
               (unsigned)boot.postinit_error);
        printf("[system] command_entered=%s segment=%04X image_match=%s psp_valid=%s\n",
               boot.command_entered ? "yes" : "no",
               boot.command_segment,
               boot.command_image_match ? "yes" : "no",
               boot.command_psp_valid ? "yes" : "no");
    }

    if (runtime.stop_reason == MD_STOP_FAULT) {
        printf("[fault] linear=%05X opcode=%02X\n",
               (unsigned)runtime.fault_linear, runtime.fault_opcode);
    }
    md_dump_cpu(&runtime);

    if (system_mode && disk.dirty) {
        if (!md_write_file(disk_path, disk.data, disk.size)) {
            fprintf(stderr, "microDOS: unable to persist modified disk image %s\n", disk_path);
        } else {
            printf("[disk] persisted modified image: %s\n", disk_path);
        }
    }

    free(memory);
    free(command);
    free(disk.data);
    free(image);

    if (system_mode) {
        if (runtime.stop_reason == MD_STOP_FAULT) return 3;
        if (runtime.stop_reason == MD_STOP_BUDGET) return 4;
        if (trace_stop) return 6;
        if (console.quit_requested && boot.command_entered &&
            boot.command_image_match && boot.command_psp_valid) return 0;
        return 5;
    }
    if (boot.returned_from_dosinit) return 0;
    if (runtime.stop_reason == MD_STOP_FAULT) return 3;
    if (runtime.stop_reason == MD_STOP_BUDGET) return 4;
    return 1;
}
