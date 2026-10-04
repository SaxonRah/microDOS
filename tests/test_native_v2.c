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


static const uint8_t kCountedBody[] = {
    0x8B,0x04,
    0x03,0xD0,
    0x8D,0x74,0x02,
    0x49,
    0x75,0xF6
};

static const uint8_t kBadCounterWrite[] = {
    0x41,
    0x49,
    0x75,0xFC
};

static const uint8_t kLoopLodsw[] = {
    0xAD,
    0x03,0xD0,
    0xE2,0xFB
};


static const uint8_t kLoopBxSiRead[] = {
    0x8B,0x00,        /* mov ax,[bx+si] */
    0x03,0xD0,        /* add dx,ax */
    0x33,0xD6,        /* xor dx,si */
    0x83,0xC6,0x02,   /* add si,2 */
    0xE2,0xF5         /* loop 0100 */
};


static const uint8_t kLoopBxSiFill[] = {
    0x05,0x37,0x9E,
    0x33,0xC6,
    0x89,0x00,
    0x83,0xC6,0x02,
    0xE2,0xF4
};


static const uint8_t kSmallCfgPhase1[] = {
    0x03,0xC3,             /* add ax,bx */
    0x13,0xF0,             /* adc si,ax */
    0x33,0xDE,             /* xor bx,si */
    0x2B,0xFB,             /* sub di,bx */
    0xD1,0xC0,             /* rol ax,1 */
    0xD1,0xCB,             /* ror bx,1 */
    0xF7,0xC6,0x01,0x00,   /* test si,1 */
    0x74,0x01,             /* jz +1 */
    0x47,                  /* inc di */
    0x49,                  /* dec cx */
    0x75,0xEA              /* jnz 0100 */
};


static const uint8_t kMultiCfgPhase6[] = {
    0xA9,0x01,0x00,       /* test ax,1 */
    0x74,0x07,            /* jz even */
    0xD1,0xE8,            /* shr ax,1 */
    0x35,0x00,0xB4,       /* xor ax,B400h */
    0xEB,0x04,            /* jmp merge */
    0xD1,0xC8,            /* even: ror ax,1 */
    0x13,0xD0,            /* adc dx,ax */
    0x3D,0x00,0x80,       /* merge: cmp ax,8000h */
    0x72,0x02,            /* jb low */
    0xF7,0xD2,            /* not dx */
    0xF7,0xC2,0x00,0x01,  /* low: test dx,0100h */
    0x74,0x02,            /* jz noneg */
    0xF7,0xDA,            /* neg dx */
    0x03,0xC2,            /* noneg: add ax,dx */
    0xE2,0xDD             /* loop 0100 */
};


static const uint8_t kMulDivPhase5[] = {
    0x33,0xD2,            /* xor dx,dx */
    0xF7,0xE3,            /* mul bx */
    0xF7,0xF7,            /* div di */
    0x33,0xC2,            /* xor ax,dx */
    0x05,0x57,0x13,       /* add ax,1357h */
    0xD1,0xC0,            /* rol ax,1 */
    0xD1,0xCA,            /* ror dx,1 */
    0xE2,0xEF             /* loop 0100 */
};


static const uint8_t kDecLoopReadsCx[] = {
    0x8B,0xC1,        /* mov ax,cx */
    0x49,             /* dec cx */
    0x75,0xFB         /* jnz 0100 */
};

static const uint8_t kLoopReadsCx[] = {
    0x8B,0xC1,        /* mov ax,cx */
    0x83,0xC0,0x01,   /* add ax,1 */
    0xE2,0xF9         /* loop 0100 */
};

static const uint8_t kFlagsStackPhase7[] = {
    0x93,                   /* xchg ax,bx */
    0x9C,                   /* pushf */
    0x9F,                   /* lahf */
    0x80,0xF4,0x5A,        /* xor ah,5Ah */
    0x9E,                   /* sahf */
    0x9D,                   /* popf */
    0xF7,0xD0,              /* not ax */
    0xF7,0xDB,              /* neg bx */
    0x98,                   /* cbw */
    0x99,                   /* cwd */
    0x87,0xD6,              /* xchg dx,si */
    0x40,                   /* inc ax */
    0x4B,                   /* dec bx */
    0xE2,0xEC               /* loop 0100 */
};


static void build_phase3_rep_loop(uint8_t *memory)
{
    /* Exact checked-in MDSTRESS.COM bytes at 14CB:02E3..0310. */
    static const uint8_t body[] = {
        0x8B,0xC5,                   /* mov ax,bp */
        0xBF,0x00,0x09,             /* mov di,0900h */
        0xB9,0x00,0x08,             /* mov cx,0800h */
        0xF3,0xAB,                   /* rep stosw */
        0xBE,0x00,0x09,             /* mov si,0900h */
        0xBF,0x00,0x19,             /* mov di,1900h */
        0xB9,0x00,0x08,             /* mov cx,0800h */
        0xF3,0xA5,                   /* rep movsw */
        0xBE,0x00,0x09,             /* mov si,0900h */
        0xBF,0x00,0x19,             /* mov di,1900h */
        0xB9,0x00,0x08,             /* mov cx,0800h */
        0xF3,0xA7,                   /* repe cmpsw */
        0xB8,0xFF,0xFF,             /* mov ax,0FFFFh */
        0xBF,0x00,0x19,             /* mov di,1900h */
        0xB9,0x00,0x08,             /* mov cx,0800h */
        0xF2,0xAF,                   /* repne scasw */
        0x4D,                        /* dec bp */
        0x75,0xD2                    /* jnz 02E3h */
    };

    memset(memory, 0, 1u << 20);
    memcpy(memory + 0x02E3u, body, sizeof(body));
}

static void build_phase4_call_graph(uint8_t *memory)
{
    static const uint8_t prelude[] = {
        0x33,0xC9              /* 034C: xor cx,cx */
    };
    static const uint8_t caller[] = {
        0xE8,0x0E,0xFE,       /* 034E: call 015F */
        0xE2,0xFB             /* 0351: loop 034E */
    };
    static const uint8_t proc_a[] = {
        0x50,0x53,0x52,       /* push ax,bx,dx */
        0xE8,0x07,0x00,       /* call 016C */
        0x5A,0x5B,0x58,       /* pop dx,bx,ax */
        0x40,                 /* inc ax */
        0x33,0xD8,            /* xor bx,ax */
        0xC3                  /* ret */
    };
    static const uint8_t proc_b[] = {
        0x56,0x57,            /* push si,di */
        0xE8,0x05,0x00,       /* call 0176 */
        0x87,0xF7,            /* xchg si,di (real NASM MDSTRESS.COM) */
        0x5F,0x5E,            /* pop di,si */
        0xC3                  /* ret */
    };
    static const uint8_t proc_c[] = {
        0x05,0x01,0x00,       /* add ax,1 (real NASM MDSTRESS.COM) */
        0x13,0xD8,            /* adc bx,ax */
        0x33,0xF3,            /* xor si,bx */
        0xD1,0xC7,            /* rol di,1 */
        0xC3                  /* ret */
    };

    memset(memory, 0, 1u << 20);
    memcpy(memory + 0x034Cu, prelude, sizeof(prelude));
    memcpy(memory + 0x034Eu, caller, sizeof(caller));
    memcpy(memory + 0x015Fu, proc_a, sizeof(proc_a));
    memcpy(memory + 0x016Cu, proc_b, sizeof(proc_b));
    memcpy(memory + 0x0176u, proc_c, sizeof(proc_c));
}

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

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kCountedBody, sizeof(kCountedBody), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kCountedBody) ||
            counter != MD_X86_CX ||
            code.op_count != 5u ||
            code.end_ip != 0x010Au ||
            code.loop_terminal != 0x75u ||
            code.chunkable_loop != 2u) {
            fprintf(stderr,
                    "counted-loop probe failed st=%s bytes=%u counter=%u "
                    "ops=%u end=%04x terminal=%02x chunk=%u\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)counter,
                    (unsigned)code.op_count,
                    code.end_ip,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.chunkable_loop);
            return 1;
        }

        st = md_native_v2_compile_counted_loop(
            kBadCounterWrite, sizeof(kBadCounterWrite), 0x0100u,
            &code, &guest_size, &counter);
        if (st == MD_NATIVE_V2_OK) {
            fprintf(stderr, "unsafe counted loop was admitted\n");
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kLoopLodsw, sizeof(kLoopLodsw), 0x0100u,
            &code, &guest_size, &counter);
        if (st != MD_NATIVE_V2_OK || guest_size != sizeof(kLoopLodsw) ||
            counter != MD_X86_CX || code.op_count != 3u ||
            code.loop_terminal != 0xE2u ||
            code.exit_lazy_op != MD_LAZY_ADD16 ||
            code.exit_flag_dst != MD_X86_DX ||
            code.exit_flag_src != MD_X86_AX || !code.requires_df_clear) {
            fprintf(stderr, "phase3d LODSW/LOOP compile failed st=%s\n",
                    md_native_v2_status_name(st));
            return 1;
        }

    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kLoopBxSiRead, sizeof(kLoopBxSiRead), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kLoopBxSiRead) ||
            counter != MD_X86_CX ||
            code.op_count != 5u ||
            code.loop_terminal != 0xE2u ||
            !code.needs_memory ||
            code.has_store ||
            code.exit_lazy_op != MD_LAZY_ADD16 ||
            code.exit_flag_dst != MD_X86_SI ||
            code.exit_flag_src != 0xFFu ||
            code.exit_flag_imm != 2u) {
            fprintf(stderr,
                    "phase3e BX+SI LOOP compile failed st=%s "
                    "bytes=%u ops=%u terminal=%02x mem=%u store=%u\\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.op_count,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.needs_memory,
                    (unsigned)code.has_store);
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kLoopBxSiFill, sizeof(kLoopBxSiFill), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kLoopBxSiFill) ||
            counter != MD_X86_CX ||
            code.op_count != 5u ||
            code.loop_terminal != 0xE2u ||
            !code.needs_memory ||
            !code.has_store ||
            !code.safe_store_bx_si_loop ||
            code.exit_lazy_op != MD_LAZY_ADD16 ||
            code.exit_flag_dst != MD_X86_SI ||
            code.exit_flag_src != 0xFFu ||
            code.exit_flag_imm != 2u) {
            fprintf(stderr,
                    "phase3f fill/store compile failed st=%s "
                    "bytes=%u ops=%u store=%u safe=%u terminal=%02x\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.op_count,
                    (unsigned)code.has_store,
                    (unsigned)code.safe_store_bx_si_loop,
                    (unsigned)code.loop_terminal);
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kSmallCfgPhase1, sizeof(kSmallCfgPhase1), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kSmallCfgPhase1) ||
            counter != MD_X86_CX ||
            code.op_count != 11u ||
            code.loop_terminal != 0x75u ||
            !code.dynamic_retire ||
            code.retire_base_ops != 10u ||
            code.needs_entry_cf ||
            code.cf_sites != 2u ||
            code.z_sites != 2u ||
            code.has_store ||
            code.needs_memory) {
            fprintf(stderr,
                    "phase3g small-CFG compile failed st=%s bytes=%u "
                    "ops=%u terminal=%02x dyn=%u base=%u "
                    "entryCF=%u CFsites=%u Zsites=%u\\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.op_count,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.dynamic_retire,
                    (unsigned)code.retire_base_ops,
                    (unsigned)code.needs_entry_cf,
                    (unsigned)code.cf_sites,
                    (unsigned)code.z_sites);
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kMultiCfgPhase6, sizeof(kMultiCfgPhase6), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kMultiCfgPhase6) ||
            counter != MD_X86_CX ||
            code.op_count != 15u ||
            code.loop_terminal != 0xE2u ||
            code.dynamic_retire != 2u ||
            code.retire_base_ops != 0u ||
            code.phase != 9u ||
            !code.chunkable_loop ||
            code.needs_entry_cf ||
            code.cf_sites != 2u ||
            code.z_sites != 2u ||
            code.needs_memory ||
            code.has_store ||
            code.exit_lazy_op != MD_LAZY_ADD16 ||
            code.exit_flag_dst != MD_X86_AX ||
            code.exit_flag_src != MD_X86_DX) {
            fprintf(stderr,
                    "phase3h multi-CFG compile failed st=%s bytes=%u "
                    "ops=%u phase=%u terminal=%02x dyn=%u base=%u "
                    "entryCF=%u CFsites=%u Zsites=%u lazy=%u dst=%u src=%u\\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.op_count,
                    (unsigned)code.phase,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.dynamic_retire,
                    (unsigned)code.retire_base_ops,
                    (unsigned)code.needs_entry_cf,
                    (unsigned)code.cf_sites,
                    (unsigned)code.z_sites,
                    (unsigned)code.exit_lazy_op,
                    (unsigned)code.exit_flag_dst,
                    (unsigned)code.exit_flag_src);
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kMulDivPhase5, sizeof(kMulDivPhase5), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kMulDivPhase5) ||
            counter != MD_X86_CX ||
            code.op_count != 8u ||
            code.loop_terminal != 0xE2u ||
            code.phase != 10u ||
            !code.chunkable_loop ||
            code.dynamic_retire != 0u ||
            !code.requires_safe_muldiv ||
            code.muldiv_mul_reg != MD_X86_BX ||
            code.muldiv_div_reg != MD_X86_DI ||
            code.exit_flags_mode != 1u ||
            code.exit_rot_reg != MD_X86_DX ||
            code.exit_lazy_op != MD_LAZY_ADD16 ||
            code.exit_flag_dst != MD_X86_AX ||
            code.exit_flag_src != 0xFFu ||
            code.exit_flag_imm != 0x1357u) {
            fprintf(stderr,
                    "phase3i MUL/DIV compile failed st=%s bytes=%u "
                    "ops=%u phase=%u terminal=%02x guard=%u mul=%u div=%u "
                    "flagmode=%u rot=%u lazy=%u imm=%04x\\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.op_count,
                    (unsigned)code.phase,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.requires_safe_muldiv,
                    (unsigned)code.muldiv_mul_reg,
                    (unsigned)code.muldiv_div_reg,
                    (unsigned)code.exit_flags_mode,
                    (unsigned)code.exit_rot_reg,
                    (unsigned)code.exit_lazy_op,
                    (unsigned)code.exit_flag_imm);
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kFlagsStackPhase7, sizeof(kFlagsStackPhase7), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kFlagsStackPhase7) ||
            counter != MD_X86_CX ||
            code.op_count != 14u ||
            code.loop_terminal != 0xE2u ||
            code.phase != 11u ||
            !code.chunkable_loop ||
            !code.needs_memory ||
            !code.has_store ||
            !code.needs_entry_flags ||
            !code.requires_safe_ss_word ||
            !code.safe_stack_pushpop ||
            code.dynamic_retire != 0u) {
            fprintf(stderr,
                    "phase3j FLAGS/stack compile failed st=%s bytes=%u "
                    "ops=%u phase=%u terminal=%02x mem=%u store=%u "
                    "flags=%u ss=%u safe=%u dyn=%u\\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.op_count,
                    (unsigned)code.phase,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.needs_memory,
                    (unsigned)code.has_store,
                    (unsigned)code.needs_entry_flags,
                    (unsigned)code.requires_safe_ss_word,
                    (unsigned)code.safe_stack_pushpop,
                    (unsigned)code.dynamic_retire);
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kLoopReadsCx, sizeof(kLoopReadsCx), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kLoopReadsCx) ||
            counter != MD_X86_CX ||
            code.loop_terminal != 0xE2u ||
            code.chunkable_loop) {
            fprintf(stderr,
                    "phase3k CX-read chunk proof failed st=%s bytes=%u "
                    "terminal=%02x chunkable=%u\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.chunkable_loop);
            return 1;
        }
    }

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter = 0xFFu;

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_counted_loop(
            kDecLoopReadsCx, sizeof(kDecLoopReadsCx), 0x0100u,
            &code, &guest_size, &counter);

        if (st != MD_NATIVE_V2_OK ||
            guest_size != sizeof(kDecLoopReadsCx) ||
            counter != MD_X86_CX ||
            code.loop_terminal != 0x75u ||
            code.chunkable_loop != 0u) {
            fprintf(stderr,
                    "phase3l DEC/JNZ CX-read proof failed st=%s bytes=%u "
                    "terminal=%02x chunk=%u\n",
                    md_native_v2_status_name(st),
                    (unsigned)guest_size,
                    (unsigned)code.loop_terminal,
                    (unsigned)code.chunkable_loop);
            return 1;
        }
    }


    {
        static uint8_t rep_memory[1u << 20];
        MdNativeV2Code code;
        MdNativeV2Status st;
        uint8_t counter = 0xFFu;

        build_phase3_rep_loop(rep_memory);
        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_rep_string_loop(
            rep_memory, 0u, 0x02E3u, &code, &counter);

        if (st != MD_NATIVE_V2_OK ||
            counter != MD_X86_BP ||
            code.phase != 13u ||
            code.op_count != 18u ||
            code.start_ip != 0x02E3u ||
            code.end_ip != 0x0311u ||
            code.loop_terminal != 0x75u ||
            code.chunkable_loop != 0u ||
            !code.needs_memory ||
            !code.has_store ||
            !code.requires_safe_ds_word ||
            !code.requires_df_clear ||
            !code.safe_rep_string_loop ||
            !code.requires_ds_eq_es ||
            code.rep_src_off != 0x0900u ||
            code.rep_dst_off != 0x1900u ||
            code.rep_words != 0x0800u ||
            code.exit_flags_reg != MD_X86_BP ||
            code.exit_lazy_op != MD_LAZY_DEC16 ||
            code.size == 0u || code.size > MD_NATIVE_V2_CODE_BYTES) {
            fprintf(stderr,
                    "phase3p REP/string compile failed st=%s phase=%u "
                    "ops=%u end=%04x counter=%u mem=%u store=%u safe=%u "
                    "eq=%u src=%04x dst=%04x words=%04x size=%u\n",
                    md_native_v2_status_name(st),
                    (unsigned)code.phase,
                    (unsigned)code.op_count,
                    code.end_ip,
                    (unsigned)counter,
                    (unsigned)code.needs_memory,
                    (unsigned)code.has_store,
                    (unsigned)code.safe_rep_string_loop,
                    (unsigned)code.requires_ds_eq_es,
                    code.rep_src_off, code.rep_dst_off, code.rep_words,
                    (unsigned)code.size);
            return 1;
        }

        /* Prefix mutation must invalidate the semantic proof. */
        rep_memory[0x02EBu] = 0xF2u;
        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_rep_string_loop(
            rep_memory, 0u, 0x02E3u, &code, &counter);
        if (st != MD_NATIVE_V2_UNSUPPORTED) {
            fprintf(stderr,
                    "phase3p REP/string mutation proof failed st=%s\n",
                    md_native_v2_status_name(st));
            return 1;
        }
    }

    {
        static uint8_t call_memory[1u << 20];
        MdNativeV2Code code;
        MdNativeV2Status st;
        uint8_t counter = 0xFFu;

        build_phase4_call_graph(call_memory);

        {
            uint16_t graph_ip = 0u;
            uint8_t prefix_ops = 0xFFu;
            uint8_t zero_counter = 0u;

            if (!md_native_v2_find_local_call_loop_entry(
                    call_memory, 0u, 0x034Cu,
                    &graph_ip, &prefix_ops, &zero_counter) ||
                graph_ip != 0x034Eu ||
                prefix_ops != 1u ||
                zero_counter != 1u) {
                fprintf(stderr,
                        "phase3o CALL admission redirect failed "
                        "entry=%04x prefix=%u zero=%u\n",
                        graph_ip, (unsigned)prefix_ops,
                        (unsigned)zero_counter);
                return 1;
            }

            graph_ip = 0u;
            prefix_ops = 0xFFu;
            zero_counter = 0xFFu;
            if (!md_native_v2_find_local_call_loop_entry(
                    call_memory, 0u, 0x034Eu,
                    &graph_ip, &prefix_ops, &zero_counter) ||
                graph_ip != 0x034Eu ||
                prefix_ops != 0u ||
                zero_counter != 0u) {
                fprintf(stderr,
                        "phase3o direct CALL root detection failed "
                        "entry=%04x prefix=%u zero=%u\n",
                        graph_ip, (unsigned)prefix_ops,
                        (unsigned)zero_counter);
                return 1;
            }
        }

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_local_call_loop(
            call_memory, 0u, 0x034Eu, &code, &counter);

        if (st != MD_NATIVE_V2_OK ||
            counter != MD_X86_CX ||
            code.phase != 12u ||
            code.op_count != 24u ||
            code.start_ip != 0x034Eu ||
            code.end_ip != 0x0353u ||
            code.loop_terminal != 0xE2u ||
            code.chunkable_loop != 1u ||
            !code.local_call_graph ||
            code.call_stack_bytes != 16u ||
            code.guest_span_count != 4u ||
            code.guest_span_ip[0] != 0x034Eu ||
            code.guest_span_len[0] != 5u ||
            code.guest_span_ip[1] != 0x015Fu ||
            code.guest_span_len[1] != 13u ||
            code.guest_span_ip[2] != 0x016Cu ||
            code.guest_span_len[2] != 10u ||
            code.guest_span_ip[3] != 0x0176u ||
            code.guest_span_len[3] != 10u ||
            !code.needs_memory ||
            !code.has_store ||
            !code.requires_safe_ss_word ||
            code.exit_lazy_op != MD_LAZY_LOGIC16 ||
            code.exit_flag_dst != MD_X86_BX ||
            code.size == 0u) {
            fprintf(stderr,
                    "phase3o CALL graph compile failed st=%s phase=%u "
                    "ops=%u end=%04x graph=%u depth=%u spans=%u size=%u\n",
                    md_native_v2_status_name(st),
                    (unsigned)code.phase,
                    (unsigned)code.op_count,
                    code.end_ip,
                    (unsigned)code.local_call_graph,
                    (unsigned)code.call_stack_bytes,
                    (unsigned)code.guest_span_count,
                    (unsigned)code.size);
            return 1;
        }


        /*
         * Phase 3O: prove the matcher is semantic rather than tied to one
         * assembler spelling. These are equally valid encodings of the two
         * source instructions that caused the real Pico rejection.
         */
        call_memory[0x0171u] = 0x87u;
        call_memory[0x0172u] = 0xFEu; /* xchg si,di, reversed ModR/M roles */
        call_memory[0x0176u] = 0x83u;
        call_memory[0x0177u] = 0xC0u;
        call_memory[0x0178u] = 0x01u; /* add ax,+1 imm8 form */
        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_local_call_loop(
            call_memory, 0u, 0x034Eu, &code, &counter);
        if (st != MD_NATIVE_V2_OK || code.phase != 12u ||
            code.op_count != 24u || code.guest_span_count != 4u) {
            fprintf(stderr,
                    "phase3o alternate CALL graph encoding failed st=%s\n",
                    md_native_v2_status_name(st));
            return 1;
        }

        /* Restore the checked-in MDSTRESS.COM encodings for mutation proof. */
        call_memory[0x0171u] = 0x87u;
        call_memory[0x0172u] = 0xF7u;
        call_memory[0x0176u] = 0x05u;
        call_memory[0x0177u] = 0x01u;
        call_memory[0x0178u] = 0x00u;

        call_memory[0x0179u] ^= 0x01u;
        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_local_call_loop(
            call_memory, 0u, 0x034Eu, &code, &counter);
        if (st != MD_NATIVE_V2_UNSUPPORTED) {
            fprintf(stderr,
                    "phase3o CALL graph mutation proof failed st=%s\n",
                    md_native_v2_status_name(st));
            return 1;
        }
    }

    puts("native-v2 phase3p REP/string + CALL/RET graph tests: PASS");
    return 0;
}
