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

/*
 * G-2B0: store/stack loops in their real DOS shape (store before the
 * iteration's flag producer). These must compile; the G-2A build rejected
 * every one of them with reject_memory.
 */
static int expect_ok_mem(const char *name, const uint8_t *code, size_t n,
                         unsigned has_store)
{
    MdNativeV2Code out;
    size_t guest = 0u;
    const MdNativeV2Status st =
        md_native_v2g_compile_loop(code, n, 0x0100u, &out, &guest);

    if (st != MD_NATIVE_V2_OK) {
        fprintf(stderr, "%s: expected OK, got %s (%d)\n",
                name, md_native_v2_status_name(st), (int)st);
        return 0;
    }
    if (guest != n || out.dynamic_retire != 3u || out.needs_memory != 1u ||
        out.has_store != has_store || !md_native_v2g_is_code(&out)) {
        fprintf(stderr, "%s: bad metadata guest=%u dyn=%u mem=%u store=%u\n",
                name, (unsigned)guest, (unsigned)out.dynamic_retire,
                (unsigned)out.needs_memory, (unsigned)out.has_store);
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
     * Word load before this iteration's first producer. G-1A rejected it;
     * G-2B0 hoists its wrap guard to the loop header because BX/SI are not
     * written earlier in the body.
     */
    static const uint8_t early_word[] = {
        0x8B,0x00,
        0x3D,0x00,0x00,
        0x75,0xF9
    };

    /* Same, but SI is rewritten before the load: not hoistable -> reject. */
    static const uint8_t early_word_moved[] = {
        0x8B,0xF7,              /* mov si,di */
        0x8B,0x04,              /* mov ax,[si] */
        0x3D,0x00,0x00,         /* cmp ax,0 */
        0x75,0xF7               /* jne 0100h */
    };

    /* G-2B0 real shapes (all rejected by G-2A). */
    static const uint8_t strcpy_loop[] = {
        0xAC, 0xAA, 0x0A,0xC0, 0x75,0xFA    /* lodsb/stosb/or al,al/jnz */
    };
    static const uint8_t stosb_pre[] = {
        0xAA, 0x40, 0x3B,0xC2, 0x75,0xFA    /* stosb/inc ax/cmp ax,dx/jne */
    };
    static const uint8_t store16_pre[] = {
        0x89,0x00, 0x83,0xC6,0x02, 0x81,0xFE,0x40,0x30, 0x75,0xF5
                                            /* mov [bx+si],ax/add si,2/cmp/jne */
    };
    static const uint8_t push_pre[] = {
        0x50, 0x40, 0x3B,0xC2, 0x75,0xFA    /* push ax/inc ax/cmp ax,dx/jne */
    };
    static const uint8_t pop_pre[] = {
        0x5B, 0x3B,0xDA, 0x75,0xFB          /* pop bx/cmp bx,dx/jne */
    };

    /* STOSB after DI is rewritten: not hoistable -> reject. */
    static const uint8_t stos_moved[] = {
        0x8B,0xFE, 0xAA, 0x3A,0xC2, 0x75,0xF9  /* mov di,si/stosb/cmp/jne */
    };

    /*
     * G-1 exactness hole: JCXZ side exit ahead of the producer in a loop that
     * writes CX could fire on iteration >= 2 with the entry FLAGS. Reject.
     */
    static const uint8_t jcxz_head_cx[] = {
        0xE3,0x08, 0x83,0xE9,0x01, 0x3D,0x34,0x12, 0x75,0xF6
    };

    ok &= expect_ok("noncounted", noncounted, sizeof(noncounted), 0x0100u, 0u);
    ok &= expect_ok("scanner", scanner, sizeof(scanner), 0x0100u, 1u);
    ok &= expect_ok("signed-cfg", signed_cfg, sizeof(signed_cfg), 0x0100u, 0u);
    ok &= expect_ok("word-load", word_load, sizeof(word_load), 0x0100u, 1u);
    ok &= expect_reject("parity", parity, sizeof(parity), 0x0100u);
    ok &= expect_ok_mem("early-word", early_word, sizeof(early_word), 0u);
    ok &= expect_reject("early-word-moved", early_word_moved,
                        sizeof(early_word_moved), 0x0100u);
    ok &= expect_ok_mem("strcpy", strcpy_loop, sizeof(strcpy_loop), 1u);
    ok &= expect_ok_mem("stosb-pre", stosb_pre, sizeof(stosb_pre), 1u);
    ok &= expect_ok_mem("store16-pre", store16_pre, sizeof(store16_pre), 1u);
    ok &= expect_ok_mem("push-pre", push_pre, sizeof(push_pre), 1u);
    ok &= expect_ok_mem("pop-pre", pop_pre, sizeof(pop_pre), 0u);
    ok &= expect_reject("stos-moved", stos_moved, sizeof(stos_moved), 0x0100u);
    ok &= expect_reject("jcxz-head-cx", jcxz_head_cx,
                        sizeof(jcxz_head_cx), 0x0100u);

    if (!ok)
        return 1;

    puts("native-v2g G-1A/G-2B0 compiler tests PASS");
    return 0;
}
