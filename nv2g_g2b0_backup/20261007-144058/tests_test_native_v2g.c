#include "microdos/native_v2g.h"

#include <stdio.h>
#include <string.h>

static int expect_ok(const char *name, const uint8_t *code, size_t n,
                     uint16_t ip, unsigned needs_memory)
{
    MdNativeV2Code out;
    size_t guest = 0u;
    const MdNativeV2Status st =
        md_native_v2g_compile_loop(code, n, ip, &out, &guest);

    if (st != MD_NATIVE_V2_OK) {
        fprintf(stderr, "%s: expected OK, got %s (%d)\n",
                name, md_native_v2_status_name(st), (int)st);
        return 0;
    }
    if (guest != n || out.start_ip != ip || out.end_ip != (uint16_t)(ip + n) ||
        out.phase != 17u || out.dynamic_retire != 3u ||
        out.has_local_loop == 0u || out.has_store != 0u ||
        out.needs_memory != needs_memory || out.size == 0u) {
        fprintf(stderr,
                "%s: bad metadata guest=%u start=%04X end=%04X phase=%u dyn=%u "
                "loop=%u store=%u mem=%u size=%u\n",
                name, (unsigned)guest, out.start_ip, out.end_ip,
                (unsigned)out.phase, (unsigned)out.dynamic_retire,
                (unsigned)out.has_local_loop, (unsigned)out.has_store,
                (unsigned)out.needs_memory, (unsigned)out.size);
        return 0;
    }
    if (!md_native_v2g_is_code(&out)) {
        fprintf(stderr, "%s: md_native_v2g_is_code rejected compiled code\n", name);
        return 0;
    }
    return 1;
}

static int expect_reject(const char *name, const uint8_t *code, size_t n,
                         uint16_t ip)
{
    MdNativeV2Code out;
    size_t guest = 0u;
    const MdNativeV2Status st =
        md_native_v2g_compile_loop(code, n, ip, &out, &guest);
    if (st == MD_NATIVE_V2_OK) {
        fprintf(stderr, "%s: expected conservative reject\n", name);
        return 0;
    }
    return 1;
}

int main(void)
{
    int ok = 1;

    /* General non-counted natural loop: INC; CMP; JNE header. */
    static const uint8_t noncounted[] = {
        0x40,                   /* inc ax */
        0x3D,0x10,0x00,        /* cmp ax,0010h */
        0x75,0xFA              /* jne 0100 */
    };

    /*
     * Common byte scanner: general DS:[SI] load, one delimiter side exit,
     * another producer, then backward JNE.  Two architectural exits are
     * represented by G-1 exit tags.
     */
    static const uint8_t scanner[] = {
        0x8A,0x04,              /* mov al,[si] */
        0x3C,0x20,              /* cmp al,' ' */
        0x74,0x05,              /* je 010Bh (outside region) */
        0x46,                   /* inc si */
        0x3C,0x00,              /* cmp al,0 */
        0x75,0xF5               /* jne 0100 */
    };

    /* Full signed Jcc fusion inside the region, plus a normal JNE latch. */
    static const uint8_t signed_cfg[] = {
        0x3D,0x10,0x00,        /* cmp ax,0010h */
        0x7C,0x01,              /* jl +1 -> INC AX */
        0x90,                   /* nop */
        0x40,                   /* inc ax */
        0x3D,0x20,0x00,        /* cmp ax,0020h */
        0x75,0xF4              /* jne 0100 */
    };

    /* Word load gets the required 16-bit offset-wrap guard and exact recipe. */
    static const uint8_t word_load[] = {
        0x3D,0x00,0x00,        /* cmp ax,0 -- flags available to guard */
        0x8B,0x00,              /* mov ax,[bx+si] */
        0x3D,0x34,0x12,        /* cmp ax,1234h */
        0x75,0xF6              /* jne 0100 */
    };

    /* G-1 table deliberately leaves parity conditions unfused. */
    static const uint8_t parity[] = {
        0x3D,0x00,0x00,
        0x7A,0x00,              /* jp next */
        0x75,0xF9              /* jne header */
    };

    /*
     * Word load before this iteration's first producer is conservatively
     * rejected in G-1A; otherwise a later wrap-side-exit could expose stale
     * previous-iteration FLAGS.
     */
    static const uint8_t early_word[] = {
        0x8B,0x00,
        0x3D,0x00,0x00,
        0x75,0xF9
    };

    ok &= expect_ok("noncounted", noncounted, sizeof(noncounted), 0x0100u, 0u);
    ok &= expect_ok("scanner", scanner, sizeof(scanner), 0x0100u, 1u);
    ok &= expect_ok("signed-cfg", signed_cfg, sizeof(signed_cfg), 0x0100u, 0u);
    ok &= expect_ok("word-load", word_load, sizeof(word_load), 0x0100u, 1u);
    ok &= expect_reject("parity", parity, sizeof(parity), 0x0100u);
    ok &= expect_reject("early-word", early_word, sizeof(early_word), 0x0100u);

    if (!ok)
        return 1;

    puts("native-v2g G-1A compiler tests PASS");
    return 0;
}
