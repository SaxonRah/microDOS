/*
 * microDOS silicon-vector runner.
 *
 * Input is the compact MDV1 stream produced by scripts/md_silicon_tests.py.
 * The upstream source of truth is SingleStepTests' physical 8086/8088 data.
 *
 * This runner intentionally uses the normal microDOS MdRuntime + interpreter.
 * No parallel CPU semantics are implemented here.
 */

#include "microdos/runtime.h"
#include "microdos/x86.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDV_REG_COUNT 14u
#define MDV_ARCH_FLAG_MASK 0x0FD5u
#define MDV_MAX_NAME 512u
#define MDV_MAX_BYTES 32u

enum {
    V_AX = 0, V_BX, V_CX, V_DX,
    V_CS, V_SS, V_DS, V_ES,
    V_SP, V_BP, V_SI, V_DI,
    V_IP, V_FLAGS
};

typedef struct MdvCase {
    uint32_t index;
    uint16_t flag_mask;
    uint16_t name_len;
    uint8_t byte_len;
    uint16_t initial[MDV_REG_COUNT];
    uint16_t expected[MDV_REG_COUNT];
    uint32_t init_ram_count;
    uint32_t final_ram_count;
    char name[MDV_MAX_NAME + 1u];
    uint8_t bytes[MDV_MAX_BYTES];
} MdvCase;

static uint8_t *g_memory;
static uint8_t *g_expected_memory;

static int read_exact(FILE *fp, void *dst, size_t n)
{
    return n == 0u || fread(dst, 1u, n, fp) == n;
}

static int read_u8(FILE *fp, uint8_t *v)
{
    return read_exact(fp, v, 1u);
}

static int read_u16(FILE *fp, uint16_t *v)
{
    uint8_t b[2];
    if (!read_exact(fp, b, sizeof(b))) return 0;
    *v = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return 1;
}

static int read_u32(FILE *fp, uint32_t *v)
{
    uint8_t b[4];
    if (!read_exact(fp, b, sizeof(b))) return 0;
    *v = (uint32_t)b[0] |
         ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
    return 1;
}

static uint8_t port_in_ff(MdRuntime *runtime, uint16_t port, void *user)
{
    (void)runtime;
    (void)port;
    (void)user;
    return 0xFFu;
}

static void port_out_ignore(MdRuntime *runtime, uint16_t port, uint8_t value, void *user)
{
    (void)runtime;
    (void)port;
    (void)value;
    (void)user;
}

static void load_regs(MdX86 *cpu, const uint16_t v[MDV_REG_COUNT])
{
    cpu->r[MD_X86_AX] = v[V_AX];
    cpu->r[MD_X86_BX] = v[V_BX];
    cpu->r[MD_X86_CX] = v[V_CX];
    cpu->r[MD_X86_DX] = v[V_DX];
    cpu->cs = v[V_CS];
    cpu->ss = v[V_SS];
    cpu->ds = v[V_DS];
    cpu->es = v[V_ES];
    cpu->r[MD_X86_SP] = v[V_SP];
    cpu->r[MD_X86_BP] = v[V_BP];
    cpu->r[MD_X86_SI] = v[V_SI];
    cpu->r[MD_X86_DI] = v[V_DI];
    cpu->ip = v[V_IP];
    md_x86_set_flags(cpu, v[V_FLAGS]);
}

static void save_regs(MdX86 *cpu, uint16_t v[MDV_REG_COUNT])
{
    v[V_AX] = cpu->r[MD_X86_AX];
    v[V_BX] = cpu->r[MD_X86_BX];
    v[V_CX] = cpu->r[MD_X86_CX];
    v[V_DX] = cpu->r[MD_X86_DX];
    v[V_CS] = cpu->cs;
    v[V_SS] = cpu->ss;
    v[V_DS] = cpu->ds;
    v[V_ES] = cpu->es;
    v[V_SP] = cpu->r[MD_X86_SP];
    v[V_BP] = cpu->r[MD_X86_BP];
    v[V_SI] = cpu->r[MD_X86_SI];
    v[V_DI] = cpu->r[MD_X86_DI];
    v[V_IP] = cpu->ip;
    v[V_FLAGS] = md_x86_flags(cpu);
}

static const char *reg_name(unsigned i)
{
    static const char *const names[MDV_REG_COUNT] = {
        "AX","BX","CX","DX","CS","SS","DS","ES",
        "SP","BP","SI","DI","IP","FLAGS"
    };
    return i < MDV_REG_COUNT ? names[i] : "?";
}

static int read_ram_entries(FILE *fp, uint32_t count, uint8_t *dst)
{
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        uint32_t addr;
        uint8_t value;
        if (!read_u32(fp, &addr) || !read_u8(fp, &value)) return 0;
        dst[addr & MD_X86_ADDRESS_MASK] = value;
    }
    return 1;
}


static int mdv_type0_exception(const MdvCase *tc)
{
    unsigned i = 0u;
    uint8_t opcode;

    while (i < tc->byte_len) {
        const uint8_t b = tc->bytes[i];
        if (b == 0x26u || b == 0x2Eu || b == 0x36u || b == 0x3Eu ||
            b == 0xF0u || b == 0xF2u || b == 0xF3u) {
            ++i;
            continue;
        }
        break;
    }
    if (i >= tc->byte_len) return 0;

    opcode = tc->bytes[i++];
    if (opcode == 0xD4u) {
        return i < tc->byte_len && tc->bytes[i] == 0u &&
               tc->expected[V_CS] == 0u && tc->expected[V_IP] == 0x0400u;
    }
    if ((opcode == 0xF6u || opcode == 0xF7u) && i < tc->byte_len) {
        const unsigned ext = (tc->bytes[i] >> 3) & 7u;
        return (ext == 6u || ext == 7u) &&
               tc->expected[V_CS] == 0u && tc->expected[V_IP] == 0x0400u;
    }
    return 0;
}

static int mdv_ignore_memory_addr(const MdvCase *tc, uint32_t address)
{
    uint32_t f0, f1;
    uint16_t flags_off;

    if (!mdv_type0_exception(tc)) return 0;

    /*
     * SingleStepTests documents arithmetic flag bits on the Type-0 stack
     * frame as undefined.  The final SP points at return IP; FLAGS was the
     * first word pushed, therefore it lives at final SS:(SP+4).
     */
    flags_off = (uint16_t)(tc->expected[V_SP] + 4u);
    f0 = md_x86_linear(tc->expected[V_SS], flags_off);
    f1 = md_x86_linear(tc->expected[V_SS], (uint16_t)(flags_off + 1u));
    return address == f0 || address == f1;
}

static int mdv_memory_matches(const MdvCase *tc)
{
    uint32_t a;
    for (a = 0u; a < MD_X86_ADDRESS_SPACE; ++a) {
        if (mdv_ignore_memory_addr(tc, a)) continue;
        if (g_memory[a] != g_expected_memory[a]) return 0;
    }
    return 1;
}

static void dump_failure(const MdvCase *tc,
                         MdRuntime *rt,
                         const uint16_t actual[MDV_REG_COUNT],
                         unsigned detail_number)
{
    unsigned i;
    unsigned shown = 0u;
    uint32_t a;

    fprintf(stderr,
            "\n[silicon] FAIL #%u \"%s\" bytes=",
            (unsigned)tc->index, tc->name);
    for (i = 0u; i < tc->byte_len; ++i) fprintf(stderr, "%02X%s", tc->bytes[i],
                                                 i + 1u == tc->byte_len ? "" : " ");
    fputc('\n', stderr);

    fprintf(stderr,
            "  initial AX=%04X BX=%04X CX=%04X DX=%04X "
            "CS:IP=%04X:%04X SS:SP=%04X:%04X DS=%04X ES=%04X "
            "BP=%04X SI=%04X DI=%04X FLAGS=%04X\n",
            tc->initial[V_AX], tc->initial[V_BX], tc->initial[V_CX],
            tc->initial[V_DX], tc->initial[V_CS], tc->initial[V_IP],
            tc->initial[V_SS], tc->initial[V_SP], tc->initial[V_DS],
            tc->initial[V_ES], tc->initial[V_BP], tc->initial[V_SI],
            tc->initial[V_DI], tc->initial[V_FLAGS]);

    if (mdv_type0_exception(tc))
        fprintf(stderr, "  note: Type-0 stacked FLAGS bytes are masked as undefined\n");

    for (i = 0u; i < MDV_REG_COUNT; ++i) {
        if (i == V_FLAGS) {
            const uint16_t mask = (uint16_t)(tc->flag_mask & MDV_ARCH_FLAG_MASK);
            if (((actual[i] ^ tc->expected[i]) & mask) != 0u) {
                fprintf(stderr,
                        "  %-5s expected=%04X actual=%04X mask=%04X diff=%04X\n",
                        reg_name(i), tc->expected[i], actual[i], mask,
                        (uint16_t)((actual[i] ^ tc->expected[i]) & mask));
            }
        } else if (actual[i] != tc->expected[i]) {
            fprintf(stderr, "  %-5s expected=%04X actual=%04X\n",
                    reg_name(i), tc->expected[i], actual[i]);
        }
    }

    if (!mdv_memory_matches(tc)) {
        fprintf(stderr, "  memory differs (first locations):\n");
        for (a = 0u; a < MD_X86_ADDRESS_SPACE && shown < 12u; ++a) {
            if (mdv_ignore_memory_addr(tc, a)) continue;
            if (g_memory[a] != g_expected_memory[a]) {
                fprintf(stderr, "    %05X expected=%02X actual=%02X\n",
                        (unsigned)a, g_expected_memory[a], g_memory[a]);
                ++shown;
            }
        }
    }

    if (rt->stop_reason != MD_STOP_NONE) {
        fprintf(stderr,
                "  stop=%s fault=%05X opcode=%02X instructions=%" PRIu64 "\n",
                md_stop_reason_name(rt->stop_reason),
                (unsigned)rt->fault_linear,
                (unsigned)rt->fault_opcode,
                rt->instructions);
    }

    (void)detail_number;
}

static int case_matches(const MdvCase *tc, MdRuntime *rt,
                        const uint16_t actual[MDV_REG_COUNT])
{
    unsigned i;
    const uint16_t fmask = (uint16_t)(tc->flag_mask & MDV_ARCH_FLAG_MASK);

    if (rt->stop_reason != MD_STOP_NONE) return 0;

    for (i = 0u; i < MDV_REG_COUNT; ++i) {
        if (i == V_FLAGS) {
            if (((actual[i] ^ tc->expected[i]) & fmask) != 0u) return 0;
        } else if (actual[i] != tc->expected[i]) {
            return 0;
        }
    }

    return mdv_memory_matches(tc);
}

int main(int argc, char **argv)
{
    FILE *fp;
    uint8_t magic[4];
    uint32_t case_count;
    uint16_t cpu_tag, reserved;
    uint32_t n;
    unsigned max_details = 20u;
    unsigned details = 0u;
    uint32_t passed = 0u, failed = 0u;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s suite.mdv [max-failure-details]\n", argv[0]);
        return 2;
    }
    if (argc == 3) {
        char *end = NULL;
        unsigned long v = strtoul(argv[2], &end, 0);
        if (end == NULL || *end != '\0') return 2;
        max_details = (unsigned)v;
    }

    fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        perror("microdos_silicon_runner: fopen");
        return 2;
    }

    if (!read_exact(fp, magic, sizeof(magic)) ||
        memcmp(magic, "MDV1", 4u) != 0 ||
        !read_u32(fp, &case_count) ||
        !read_u16(fp, &cpu_tag) ||
        !read_u16(fp, &reserved)) {
        fprintf(stderr, "microdos_silicon_runner: invalid MDV1 header\n");
        fclose(fp);
        return 2;
    }
    (void)reserved;

    g_memory = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    g_expected_memory = (uint8_t *)malloc(MD_X86_ADDRESS_SPACE);
    if (g_memory == NULL || g_expected_memory == NULL) {
        fprintf(stderr, "microdos_silicon_runner: out of memory\n");
        fclose(fp);
        free(g_memory);
        free(g_expected_memory);
        return 2;
    }

    printf("[silicon] cpu=%u cases=%u file=%s\n",
           (unsigned)cpu_tag, (unsigned)case_count, argv[1]);

    for (n = 0u; n < case_count; ++n) {
        MdvCase tc;
        MdRuntime rt;
        MdHooks hooks;
        uint16_t actual[MDV_REG_COUNT];
        unsigned i;

        memset(&tc, 0, sizeof(tc));
        memset(g_memory, 0, MD_X86_ADDRESS_SPACE);
        memset(g_expected_memory, 0, MD_X86_ADDRESS_SPACE);

        if (!read_u32(fp, &tc.index) ||
            !read_u16(fp, &tc.flag_mask) ||
            !read_u16(fp, &tc.name_len) ||
            !read_u8(fp, &tc.byte_len)) {
            fprintf(stderr, "microdos_silicon_runner: truncated case header at %u\n",
                    (unsigned)n);
            failed++;
            break;
        }

        {
            uint8_t pad[3];
            if (!read_exact(fp, pad, sizeof(pad))) {
                fprintf(stderr, "microdos_silicon_runner: truncated padding\n");
                failed++;
                break;
            }
        }

        if (tc.name_len > MDV_MAX_NAME || tc.byte_len > MDV_MAX_BYTES) {
            fprintf(stderr, "microdos_silicon_runner: oversized case fields\n");
            failed++;
            break;
        }

        for (i = 0u; i < MDV_REG_COUNT; ++i)
            if (!read_u16(fp, &tc.initial[i])) goto truncated;
        for (i = 0u; i < MDV_REG_COUNT; ++i)
            if (!read_u16(fp, &tc.expected[i])) goto truncated;
        if (!read_u32(fp, &tc.init_ram_count) ||
            !read_u32(fp, &tc.final_ram_count)) goto truncated;
        if (!read_exact(fp, tc.name, tc.name_len)) goto truncated;
        tc.name[tc.name_len] = '\0';
        if (!read_exact(fp, tc.bytes, tc.byte_len)) goto truncated;

        memset(&hooks, 0, sizeof(hooks));
        hooks.in8 = port_in_ff;
        hooks.out8 = port_out_ignore;
        md_runtime_init(&rt, g_memory, &hooks);

        if (!read_ram_entries(fp, tc.init_ram_count, g_memory)) goto truncated;
        memcpy(g_expected_memory, g_memory, MD_X86_ADDRESS_SPACE);
        if (!read_ram_entries(fp, tc.final_ram_count, g_expected_memory)) goto truncated;

        load_regs(&rt.cpu, tc.initial);
        (void)md_interp_step(&rt);
        save_regs(&rt.cpu, actual);

        if (case_matches(&tc, &rt, actual)) {
            ++passed;
        } else {
            ++failed;
            if (details < max_details) {
                dump_failure(&tc, &rt, actual, details);
                ++details;
            }
        }
        continue;

truncated:
        fprintf(stderr, "microdos_silicon_runner: truncated case payload at %u\n",
                (unsigned)n);
        ++failed;
        break;
    }

    printf("[mdv-summary] total=%u passed=%u failed=%u details=%u\n",
           (unsigned)(passed + failed), (unsigned)passed, (unsigned)failed, details);

    free(g_expected_memory);
    free(g_memory);
    fclose(fp);
    return failed == 0u ? 0 : 1;
}
