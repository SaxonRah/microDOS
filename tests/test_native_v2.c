#include "microdos/native_v2.h"

#include <stdio.h>
#include <string.h>

static const uint8_t kLoop[] = {
    0xB8,0x00,0x00,
    0xB9,0x00,0x80,
    0x40,
    0x49,
    0x75,0xFC
};

static const uint8_t kRegmix[] = {
    0xB8,0x01,0x00,
    0xBB,0x03,0x00,
    0xB9,0x00,0x80,
    0x31,0xD2,
    0x03,0xC3,
    0x33,0xD8,
    0x03,0xD0,
    0x49,
    0x75,0xF7
};

static const uint8_t kMemmix[] = {
    0xBE,0x00,0x80,
    0xBF,0x00,0x00,
    0xB9,0x00,0x40,
    0x31,0xC0,
    0x8B,0x04,
    0x83,0xC0,0x01,
    0x89,0x05,
    0x8D,0x74,0x02,
    0x8D,0x7D,0x02,
    0x83,0xC8,0x00,
    0x49,
    0x75,0xED
};

static int check(const char *name,
                 const uint8_t *image, size_t n,
                 unsigned expected_ops, uint16_t end_ip,
                 int needs_memory, int has_store,
                 int needs_entry_cf,
                 unsigned cf_sites, unsigned z_sites)
{
    MdNativeV2Code code;
    MdNativeV2Status st;

    memset(&code, 0, sizeof(code));
    st = md_native_v2_compile_8086(
        image, n, 0x0100u, 0x0100u, &code);

    if (st != MD_NATIVE_V2_OK) {
        fprintf(stderr, "%s compile failed: %s\n",
                name, md_native_v2_status_name(st));
        return 0;
    }

    if (code.op_count != expected_ops ||
        code.end_ip != end_ip ||
        code.phase != 4u ||
        code.needs_memory != (unsigned)needs_memory ||
        code.has_store != (unsigned)has_store ||
        code.exit_flags_reg != MD_X86_CX ||
        code.needs_entry_cf != (unsigned)needs_entry_cf ||
        code.cf_sites != cf_sites ||
        code.z_sites != z_sites ||
        code.size == 0u) {
        fprintf(stderr,
                "%s bad shape ops=%u end=%04x phase=%u mem=%u store=%u "
                "flagsreg=%u entrycf=%u cf-sites=%u z-sites=%u bytes=%u\n",
                name,
                (unsigned)code.op_count,
                code.end_ip,
                (unsigned)code.phase,
                (unsigned)code.needs_memory,
                (unsigned)code.has_store,
                (unsigned)code.exit_flags_reg,
                (unsigned)code.needs_entry_cf,
                (unsigned)code.cf_sites,
                (unsigned)code.z_sites,
                (unsigned)code.size);
        return 0;
    }

    return 1;
}

int main(void)
{
    /* No CF producer before DEC: incoming architectural CF is live. */
    if (!check("loop", kLoop, sizeof(kLoop),
               5u, 0x010Au, 0, 0, 1, 0u, 1u))
        return 1;

    /*
     * The first ADD's CF dies at XOR; XOR's CF dies at the final ADD.
     * Only ADD DX,AX needs virtual CF for the DEC-preserved exit state.
     */
    if (!check("regmix", kRegmix, sizeof(kRegmix),
               9u, 0x0114u, 0, 0, 0, 1u, 1u))
        return 1;

    /*
     * ADD AX,1 flags die at OR AX,0. Only OR's CF=0 is live through DEC.
     */
    if (!check("memmix", kMemmix, sizeof(kMemmix),
               12u, 0x011Eu, 1, 1, 0, 1u, 1u))
        return 1;

    puts("native-v2 phase2c liveness compile tests: PASS");
    return 0;
}
