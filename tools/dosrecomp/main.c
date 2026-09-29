/* dosrecomp v2 - source-assisted 8086 static recompiler for .COM images.
 *
 * Discovery uses the shared structural decoder (md_decode_8086), so every
 * reachable instruction is seen even if the emitter cannot compile it.
 * Instructions the emitter does not support become explicit interpreter
 * "holes": generated code hands the current CS:IP to the interpreter, which
 * executes that one instruction (or more) and returns at the next compiled
 * entry. The binary is authoritative; --entry/--entries/--interp-at supply
 * source-derived metadata that static analysis cannot recover.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "microdos/decode.h"

#define DR_COM_BASE 0x0100u
#define DR_MAX_IMAGE 0xFF00u
#define DR_IP_SPACE 0x10000u
#define DR_MAX_EXTRA 1024u

typedef struct DrOptions {
    const char *input;
    const char *output_c;
    const char *output_h;
    const char *symbol;
    const char *program_name;
    uint16_t code_start;
    uint32_t code_end;
    int code_end_set;
    int dump;
    uint16_t entries[DR_MAX_EXTRA];
    size_t entry_count;
    uint16_t interp_at[DR_MAX_EXTRA];
    size_t interp_count;
} DrOptions;

enum {
    DR_GEN_CONTINUE = 0,   /* emitted; execution falls through to next_ip */
    DR_GEN_TERMINAL = 1,   /* emitted; ends with an explicit transfer */
    DR_GEN_HOLE = 2        /* not compiled: interpreter executes it */
};

typedef struct DrProgram {
    uint8_t *image;
    size_t image_size;
    uint16_t code_start;
    uint32_t code_end;

    uint8_t reachable[DR_IP_SPACE];
    uint8_t block_start[DR_IP_SPACE];
    uint8_t forced_hole[DR_IP_SPACE];
    uint8_t kind[DR_IP_SPACE];          /* DR_GEN_* per reachable ip */
    MdDecodedInstruction *dec;          /* indexed by ip, valid if reachable */
    uint8_t *code_bits;                 /* one bit per image byte */

    uint16_t queue[DR_IP_SPACE];
    uint8_t queued[DR_IP_SPACE];
    size_t head, tail;

    size_t inst_count, hole_count, block_count, entry_count;
} DrProgram;

/* ------------------------------------------------------------------------ */

static void dr_usage(const char *exe)
{
    fprintf(stderr,
            "usage: %s --input file.com --output-c out.c --output-h out.h --symbol name\n"
            "          [--code-start N] [--code-end N] [--entry N]... [--entries FILE]\n"
            "          [--interp-at N]... [--name NAME] [--dump]\n", exe);
}

static int dr_parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > 0xFFFFFFFFul) return 0;
    *out = (uint32_t)value;
    return 1;
}

static int dr_load_entries(const char *path, DrOptions *opt)
{
    FILE *fp = fopen(path, "r");
    char line[256];
    if (fp == NULL) {
        fprintf(stderr, "dosrecomp: cannot open entries file %s: %s\n", path, strerror(errno));
        return 0;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        char *end;
        unsigned long v;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        v = strtoul(p, &end, 0);
        if (end == p || v > 0xFFFFul) continue;
        if (strncmp(end, " interp", 7) == 0) {
            if (opt->interp_count < DR_MAX_EXTRA) opt->interp_at[opt->interp_count++] = (uint16_t)v;
        } else if (opt->entry_count < DR_MAX_EXTRA) {
            opt->entries[opt->entry_count++] = (uint16_t)v;
        }
    }
    fclose(fp);
    return 1;
}

static int dr_parse_options(int argc, char **argv, DrOptions *opt)
{
    int i;
    memset(opt, 0, sizeof(*opt));
    opt->code_start = DR_COM_BASE;
    opt->code_end = DR_IP_SPACE;
    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        uint32_t v;
        if (strcmp(a, "--dump") == 0) { opt->dump = 1; continue; }
        if (i + 1 >= argc) return 0;
        if (strcmp(a, "--input") == 0) opt->input = argv[++i];
        else if (strcmp(a, "--output-c") == 0) opt->output_c = argv[++i];
        else if (strcmp(a, "--output-h") == 0) opt->output_h = argv[++i];
        else if (strcmp(a, "--symbol") == 0) opt->symbol = argv[++i];
        else if (strcmp(a, "--name") == 0) opt->program_name = argv[++i];
        else if (strcmp(a, "--entries") == 0) { if (!dr_load_entries(argv[++i], opt)) return 0; }
        else if (strcmp(a, "--code-start") == 0) {
            if (!dr_parse_u32(argv[++i], &v) || v > 0xFFFFu) return 0;
            opt->code_start = (uint16_t)v;
        } else if (strcmp(a, "--code-end") == 0) {
            if (!dr_parse_u32(argv[++i], &v) || v > DR_IP_SPACE) return 0;
            opt->code_end = v;
            opt->code_end_set = 1;
        } else if (strcmp(a, "--entry") == 0) {
            if (!dr_parse_u32(argv[++i], &v) || v > 0xFFFFu || opt->entry_count >= DR_MAX_EXTRA) return 0;
            opt->entries[opt->entry_count++] = (uint16_t)v;
        } else if (strcmp(a, "--interp-at") == 0) {
            if (!dr_parse_u32(argv[++i], &v) || v > 0xFFFFu || opt->interp_count >= DR_MAX_EXTRA) return 0;
            opt->interp_at[opt->interp_count++] = (uint16_t)v;
        } else return 0;
    }
    return opt->input && opt->output_c && opt->output_h && opt->symbol;
}

static int dr_valid_symbol(const char *s)
{
    size_t i;
    if (s == NULL || s[0] == '\0') return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return 0;
    for (i = 1; s[i] != '\0'; ++i) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static uint8_t *dr_read_file(const char *path, size_t *size_out)
{
    FILE *fp = fopen(path, "rb");
    long length;
    uint8_t *data;
    *size_out = 0u;
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    data = (uint8_t *)malloc(length > 0 ? (size_t)length : 1u);
    if (data == NULL) { fclose(fp); return NULL; }
    if (length != 0 && fread(data, 1u, (size_t)length, fp) != (size_t)length) {
        free(data); fclose(fp); return NULL;
    }
    fclose(fp);
    *size_out = (size_t)length;
    return data;
}

/* ------------------------------------------------------------------------ */

static int dr_in_image(const DrProgram *p, uint32_t ip)
{
    return ip >= DR_COM_BASE && ip < DR_COM_BASE + p->image_size;
}

static int dr_in_code(const DrProgram *p, uint32_t ip)
{
    return dr_in_image(p, ip) && ip >= p->code_start && ip < p->code_end;
}

static uint8_t dr_u8(const DrProgram *p, uint32_t ip)
{
    return dr_in_image(p, ip) ? p->image[ip - DR_COM_BASE] : 0u;
}

static uint16_t dr_u16(const DrProgram *p, uint32_t ip)
{
    return (uint16_t)(dr_u8(p, ip) | (dr_u8(p, ip + 1u) << 8));
}

static void dr_queue(DrProgram *p, uint32_t ip)
{
    if (!dr_in_code(p, ip) || p->queued[ip]) return;
    p->queued[ip] = 1u;
    p->queue[p->tail++] = (uint16_t)ip;
}

static void dr_mark_start(DrProgram *p, uint32_t ip)
{
    if (dr_in_code(p, ip)) p->block_start[ip] = 1u;
}

static int dr_discover(DrProgram *p, const DrOptions *opt)
{
    size_t i;
    dr_queue(p, p->code_start);
    dr_mark_start(p, p->code_start);
    for (i = 0; i < opt->entry_count; ++i) {
        dr_queue(p, opt->entries[i]);
        dr_mark_start(p, opt->entries[i]);
    }
    while (p->head < p->tail) {
        const uint16_t ip = p->queue[p->head++];
        MdDecodedInstruction d;
        uint32_t b;
        if (!md_decode_8086(p->image, p->image_size, DR_COM_BASE, ip, &d)) continue;
        if (!dr_in_code(p, (uint32_t)ip + d.length - 1u)) continue;
        p->reachable[ip] = 1u;
        p->dec[ip] = d;
        ++p->inst_count;
        for (b = ip; b < (uint32_t)ip + d.length; ++b) {
            const uint32_t off = b - DR_COM_BASE;
            p->code_bits[off >> 3] |= (uint8_t)(1u << (off & 7u));
        }
        switch (d.flow) {
            case MD_DECODE_FLOW_FALLTHROUGH:
                dr_queue(p, d.next_ip);
                break;
            case MD_DECODE_FLOW_CONDITIONAL:
                dr_queue(p, d.target); dr_mark_start(p, d.target);
                dr_queue(p, d.next_ip); dr_mark_start(p, d.next_ip);
                break;
            case MD_DECODE_FLOW_CALL:
                if (!d.far_control) { dr_queue(p, d.target); dr_mark_start(p, d.target); }
                dr_queue(p, d.next_ip); dr_mark_start(p, d.next_ip);
                break;
            case MD_DECODE_FLOW_JUMP:
                if (!d.far_control) { dr_queue(p, d.target); dr_mark_start(p, d.target); }
                break;
            case MD_DECODE_FLOW_INDIRECT_CALL:
                dr_queue(p, d.next_ip); dr_mark_start(p, d.next_ip);
                break;
            default:
                break;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Emitter. dr_gen(p, ip, f) with f == NULL only classifies.                  */

typedef struct DrOut { FILE *f; } DrOut;

static void o(DrOut *out, const char *fmt, ...)
{
    va_list ap;
    if (out->f == NULL) return;
    va_start(ap, fmt);
    vfprintf(out->f, fmt, ap);
    va_end(ap);
}

static const char *const kSeg[4] = { "cpu->es", "cpu->cs", "cpu->ss", "cpu->ds" };

typedef struct DrModrm {
    int is_reg;
    unsigned mod, reg, rm;
    unsigned length;          /* modrm byte + displacement */
    char seg[16];
    char off[96];
} DrModrm;

static int dr_seg_override(const MdDecodedInstruction *d)
{
    int seg = -1;
    unsigned i;
    for (i = 0; i < d->prefix_count; ++i) {
        switch (d->prefixes[i]) {
            case 0x26: seg = 0; break;
            case 0x2E: seg = 1; break;
            case 0x36: seg = 2; break;
            case 0x3E: seg = 3; break;
            default: break;
        }
    }
    return seg;
}

static int dr_has_rep(const MdDecodedInstruction *d)
{
    unsigned i;
    for (i = 0; i < d->prefix_count; ++i) {
        if (d->prefixes[i] == 0xF2 || d->prefixes[i] == 0xF3) return 1;
    }
    return 0;
}

static void dr_modrm(const DrProgram *p, uint32_t at, int seg_override, DrModrm *m)
{
    static const char *const base[8] = {
        "cpu->r[3] + cpu->r[6]", "cpu->r[3] + cpu->r[7]",
        "cpu->r[5] + cpu->r[6]", "cpu->r[5] + cpu->r[7]",
        "cpu->r[6]", "cpu->r[7]", "cpu->r[5]", "cpu->r[3]"
    };
    const uint8_t modrm = dr_u8(p, at);
    int seg = 3;
    memset(m, 0, sizeof(*m));
    m->mod = modrm >> 6;
    m->reg = (modrm >> 3) & 7u;
    m->rm = modrm & 7u;
    m->length = 1u;
    if (m->mod == 3u) { m->is_reg = 1; return; }
    if (m->mod == 0u && m->rm == 6u) {
        snprintf(m->off, sizeof(m->off), "0x%04Xu", dr_u16(p, at + 1u));
        m->length = 3u;
    } else {
        int disp = 0;
        if (m->mod == 1u) { disp = (int8_t)dr_u8(p, at + 1u); m->length = 2u; }
        else if (m->mod == 2u) { disp = (int16_t)dr_u16(p, at + 1u); m->length = 3u; }
        if (m->rm == 2u || m->rm == 3u || m->rm == 6u) seg = 2;
        if (disp != 0) {
            snprintf(m->off, sizeof(m->off), "(uint16_t)(%s + 0x%04Xu)", base[m->rm], (unsigned)(uint16_t)disp);
        } else {
            snprintf(m->off, sizeof(m->off), "(uint16_t)(%s)", base[m->rm]);
        }
    }
    if (seg_override >= 0) seg = seg_override;
    snprintf(m->seg, sizeof(m->seg), "%s", kSeg[seg]);
}

/* rm read/write snippets; memory forms use locals s_/o_ set by dr_ea(). */
static void dr_ea(DrOut *out, const DrModrm *m)
{
    if (!m->is_reg) o(out, "        const uint16_t s_ = %s; const uint16_t o_ = %s;\n", m->seg, m->off);
}
static void dr_rd(char *buf, size_t n, const DrModrm *m, int w16)
{
    if (m->is_reg) snprintf(buf, n, w16 ? "cpu->r[%u]" : "md_x86_get_reg8(cpu, %uu)", m->rm);
    else snprintf(buf, n, w16 ? "md_x86_read16(cpu, s_, o_)" : "md_x86_read8(cpu, s_, o_)");
}
static void dr_wr(DrOut *out, const DrModrm *m, int w16, const char *value)
{
    if (m->is_reg) {
        if (w16) o(out, "        cpu->r[%u] = %s;\n", m->rm, value);
        else o(out, "        md_x86_set_reg8(cpu, %uu, %s);\n", m->rm, value);
    } else {
        o(out, "        md_x86_write%s(cpu, s_, o_, %s);\n", w16 ? "16" : "8", value);
        o(out, "        MD_AOT_WCHK();\n");
    }
}

static void dr_goto(DrOut *out, const DrProgram *p, uint16_t target)
{
    if (dr_in_code(p, target) && p->block_start[target] && p->reachable[target] &&
        p->kind[target] != DR_GEN_HOLE) {
        o(out, "    MD_AOT_GOTO(0x%04Xu, md_block_%04X);\n", target, target);
    } else {
        o(out, "    cpu->ip = 0x%04Xu; goto md_dispatch;\n", target);
    }
}

static const char *const kAluName[8] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };

static int dr_gen(DrProgram *p, uint16_t ip, FILE *file)
{
    DrOut out_ = { file };
    DrOut *out = &out_;
    const MdDecodedInstruction *d = &p->dec[ip];
    const uint32_t at = (uint32_t)ip + d->prefix_count;       /* opcode */
    const uint8_t op = dr_u8(p, at);
    const uint16_t next = d->next_ip;
    const int seg_ov = dr_seg_override(d);
    const int rep = dr_has_rep(d);
    const int w16 = (op & 1u) != 0u;
    char a[96], b[96];
    DrModrm m;

#define TICK() o(out, "    MD_AOT_TICK(0x%04Xu, 0x%04Xu);\n", ip, next)
#define OPEN() o(out, "    {\n")
#define CLOSE() o(out, "    }\n")

    if (p->forced_hole[ip] || !d->valid_8086) return DR_GEN_HOLE;
    if (rep && !(op >= 0xA4u && op <= 0xAFu)) return DR_GEN_HOLE;   /* REP on non-string */

    /* ---- ALU r/m,reg / reg,r/m / acc,imm (00-3D) ---------------------- */
    if (op < 0x40u && (op & 7u) <= 5u) {
        const unsigned alu = op >> 3;
        const unsigned form = op & 7u;
        if (form <= 3u) {
            const int wide = (form & 1u) != 0u;
            const int to_reg = (form & 2u) != 0u;
            dr_modrm(p, at + 1u, seg_ov, &m);
            TICK(); OPEN(); dr_ea(out, &m);
            dr_rd(a, sizeof(a), &m, wide);
            if (wide) snprintf(b, sizeof(b), "cpu->r[%u]", m.reg);
            else snprintf(b, sizeof(b), "md_x86_get_reg8(cpu, %uu)", m.reg);
            if (to_reg) {
                if (alu == 7u) o(out, "        (void)md_x86_alu%s(cpu, 7u, %s, %s);\n", wide ? "16" : "8", b, a);
                else if (wide) o(out, "        cpu->r[%u] = md_x86_alu16(cpu, %uu, %s, %s);\n", m.reg, alu, b, a);
                else o(out, "        md_x86_set_reg8(cpu, %uu, md_x86_alu8(cpu, %uu, %s, %s));\n", m.reg, alu, b, a);
            } else {
                char v[256];
                if (alu == 7u) o(out, "        (void)md_x86_alu%s(cpu, 7u, %s, %s);\n", wide ? "16" : "8", a, b);
                else {
                    snprintf(v, sizeof(v), "md_x86_alu%s(cpu, %uu, %s, %s)", wide ? "16" : "8", alu, a, b);
                    dr_wr(out, &m, wide, v);
                }
            }
            CLOSE();
            (void)kAluName;
            return DR_GEN_CONTINUE;
        }
        TICK();
        if (form == 4u) {
            if (alu == 7u) o(out, "    (void)md_x86_alu8(cpu, 7u, md_x86_get_reg8(cpu, 0u), 0x%02Xu);\n", dr_u8(p, at + 1u));
            else o(out, "    md_x86_set_reg8(cpu, 0u, md_x86_alu8(cpu, %uu, md_x86_get_reg8(cpu, 0u), 0x%02Xu));\n", alu, dr_u8(p, at + 1u));
        } else {
            if (alu == 7u) o(out, "    (void)md_x86_alu16(cpu, 7u, cpu->r[0], 0x%04Xu);\n", dr_u16(p, at + 1u));
            else o(out, "    cpu->r[0] = md_x86_alu16(cpu, %uu, cpu->r[0], 0x%04Xu);\n", alu, dr_u16(p, at + 1u));
        }
        return DR_GEN_CONTINUE;
    }

    switch (op) {
        /* ---- segment register push/pop ------------------------------- */
        case 0x06: case 0x0E: case 0x16: case 0x1E:
            TICK();
            o(out, "    md_x86_push(cpu, %s);\n    MD_AOT_WCHK();\n", kSeg[(op >> 3) & 3u]);
            return DR_GEN_CONTINUE;
        case 0x07: case 0x17: case 0x1F:
            TICK();
            o(out, "    %s = md_x86_pop(cpu);\n", kSeg[(op >> 3) & 3u]);
            return DR_GEN_CONTINUE;
        default: break;
    }

    if (op >= 0x40u && op <= 0x4Fu) {           /* INC/DEC r16, CF preserved */
        const unsigned r = op & 7u;
        TICK();
        o(out, "    { const uint16_t cf_ = cpu->flags & MD_X86_FLAG_CF;\n"
               "      cpu->r[%u] = md_x86_%s16(cpu, cpu->r[%u], 1u);\n"
               "      cpu->flags = (uint16_t)((cpu->flags & (uint16_t)~MD_X86_FLAG_CF) | cf_); }\n",
          r, op < 0x48u ? "add" : "sub", r);
        return DR_GEN_CONTINUE;
    }
    if (op >= 0x50u && op <= 0x57u) {
        TICK();
        o(out, "    md_x86_push_reg(cpu, %uu);\n    MD_AOT_WCHK();\n", op & 7u);
        return DR_GEN_CONTINUE;
    }
    if (op >= 0x58u && op <= 0x5Fu) {
        TICK();
        o(out, "    cpu->r[%u] = md_x86_pop(cpu);\n", op & 7u);
        return DR_GEN_CONTINUE;
    }
    if (op >= 0x70u && op <= 0x7Fu) {           /* Jcc rel8 */
        TICK();
        o(out, "    if (md_x86_condition(cpu, 0x%Xu)) {\n", op & 0x0Fu);
        dr_goto(out, p, d->target);
        o(out, "    }\n");
        dr_goto(out, p, next);
        return DR_GEN_TERMINAL;
    }
    if (op >= 0x80u && op <= 0x83u) {           /* group 1 r/m,imm */
        const int wide = (op & 1u) != 0u;
        unsigned imm;
        dr_modrm(p, at + 1u, seg_ov, &m);
        if (op == 0x81u) imm = dr_u16(p, at + 1u + m.length);
        else if (op == 0x83u) imm = (uint16_t)(int16_t)(int8_t)dr_u8(p, at + 1u + m.length);
        else imm = dr_u8(p, at + 1u + m.length);
        TICK(); OPEN(); dr_ea(out, &m);
        dr_rd(a, sizeof(a), &m, wide);
        if (m.reg == 7u) {
            o(out, "        (void)md_x86_alu%s(cpu, 7u, %s, 0x%04Xu);\n", wide ? "16" : "8", a, imm);
        } else {
            char v[256];
            snprintf(v, sizeof(v), "md_x86_alu%s(cpu, %uu, %s, 0x%04Xu)", wide ? "16" : "8", m.reg, a, imm);
            dr_wr(out, &m, wide, v);
        }
        CLOSE();
        return DR_GEN_CONTINUE;
    }
    if (op >= 0xB0u && op <= 0xB7u) {
        TICK();
        o(out, "    md_x86_set_reg8(cpu, %uu, 0x%02Xu);\n", op & 7u, dr_u8(p, at + 1u));
        return DR_GEN_CONTINUE;
    }
    if (op >= 0xB8u && op <= 0xBFu) {
        TICK();
        o(out, "    cpu->r[%u] = 0x%04Xu;\n", op & 7u, dr_u16(p, at + 1u));
        return DR_GEN_CONTINUE;
    }
    if (op >= 0x91u && op <= 0x97u) {
        TICK();
        o(out, "    { const uint16_t t_ = cpu->r[0]; cpu->r[0] = cpu->r[%u]; cpu->r[%u] = t_; }\n", op & 7u, op & 7u);
        return DR_GEN_CONTINUE;
    }

    switch (op) {
        case 0x84: case 0x85:                    /* TEST r/m,reg */
            dr_modrm(p, at + 1u, seg_ov, &m);
            TICK(); OPEN(); dr_ea(out, &m);
            dr_rd(a, sizeof(a), &m, w16);
            if (w16) o(out, "        (void)md_x86_logic16(cpu, (uint16_t)(%s & cpu->r[%u]));\n", a, m.reg);
            else o(out, "        (void)md_x86_logic8(cpu, (uint8_t)(%s & md_x86_get_reg8(cpu, %uu)));\n", a, m.reg);
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0x86: case 0x87:                    /* XCHG r/m,reg */
            dr_modrm(p, at + 1u, seg_ov, &m);
            TICK(); OPEN(); dr_ea(out, &m);
            dr_rd(a, sizeof(a), &m, w16);
            if (w16) {
                o(out, "        const uint16_t a_ = %s; const uint16_t b_ = cpu->r[%u];\n", a, m.reg);
                dr_wr(out, &m, 1, "b_");
                o(out, "        cpu->r[%u] = a_;\n", m.reg);
            } else {
                o(out, "        const uint8_t a_ = %s; const uint8_t b_ = md_x86_get_reg8(cpu, %uu);\n", a, m.reg);
                dr_wr(out, &m, 0, "b_");
                o(out, "        md_x86_set_reg8(cpu, %uu, a_);\n", m.reg);
            }
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0x88: case 0x89:                    /* MOV r/m,reg */
            dr_modrm(p, at + 1u, seg_ov, &m);
            TICK(); OPEN(); dr_ea(out, &m);
            if (w16) snprintf(b, sizeof(b), "cpu->r[%u]", m.reg);
            else snprintf(b, sizeof(b), "md_x86_get_reg8(cpu, %uu)", m.reg);
            dr_wr(out, &m, w16, b);
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0x8A: case 0x8B:                    /* MOV reg,r/m */
            dr_modrm(p, at + 1u, seg_ov, &m);
            TICK(); OPEN(); dr_ea(out, &m);
            dr_rd(a, sizeof(a), &m, w16);
            if (w16) o(out, "        cpu->r[%u] = %s;\n", m.reg, a);
            else o(out, "        md_x86_set_reg8(cpu, %uu, %s);\n", m.reg, a);
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0x8C:                               /* MOV r/m16,sreg */
            dr_modrm(p, at + 1u, seg_ov, &m);
            TICK(); OPEN(); dr_ea(out, &m);
            dr_wr(out, &m, 1, kSeg[m.reg & 3u]);
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0x8E:                               /* MOV sreg,r/m16 (not CS) */
            dr_modrm(p, at + 1u, seg_ov, &m);
            if ((m.reg & 3u) == 1u || m.reg > 3u) return DR_GEN_HOLE;
            TICK(); OPEN(); dr_ea(out, &m);
            dr_rd(a, sizeof(a), &m, 1);
            o(out, "        %s = %s;\n", kSeg[m.reg], a);
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0x8D:                               /* LEA */
            dr_modrm(p, at + 1u, seg_ov, &m);
            if (m.is_reg) return DR_GEN_HOLE;
            TICK();
            o(out, "    cpu->r[%u] = %s;\n", m.reg, m.off);
            return DR_GEN_CONTINUE;

        case 0x90:
            TICK();
            return DR_GEN_CONTINUE;
        case 0x98:
            TICK();
            o(out, "    cpu->r[0] = (uint16_t)(int16_t)(int8_t)md_x86_get_reg8(cpu, 0u);\n");
            return DR_GEN_CONTINUE;
        case 0x99:
            TICK();
            o(out, "    cpu->r[2] = (cpu->r[0] & 0x8000u) != 0u ? 0xFFFFu : 0u;\n");
            return DR_GEN_CONTINUE;
        case 0x9C:
            TICK();
            o(out, "    md_x86_push(cpu, (uint16_t)(cpu->flags | MD_X86_FLAG_ALWAYS1));\n    MD_AOT_WCHK();\n");
            return DR_GEN_CONTINUE;
        case 0x9D:
            TICK();
            o(out, "    cpu->flags = (uint16_t)(md_x86_pop(cpu) | MD_X86_FLAG_ALWAYS1);\n");
            return DR_GEN_CONTINUE;
        case 0x9E:
            TICK();
            o(out, "    { const uint16_t mk_ = MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_AF | MD_X86_FLAG_PF | MD_X86_FLAG_CF;\n"
                   "      cpu->flags = (uint16_t)((cpu->flags & (uint16_t)~mk_) | (md_x86_get_reg8(cpu, 4u) & mk_) | MD_X86_FLAG_ALWAYS1); }\n");
            return DR_GEN_CONTINUE;
        case 0x9F:
            TICK();
            o(out, "    md_x86_set_reg8(cpu, 4u, (uint8_t)((cpu->flags & 0x00D5u) | 0x02u));\n");
            return DR_GEN_CONTINUE;

        case 0xA0: case 0xA1: case 0xA2: case 0xA3: {   /* MOV acc <-> moffs */
            const char *sg = kSeg[seg_ov >= 0 ? seg_ov : 3];
            const unsigned addr = dr_u16(p, at + 1u);
            TICK();
            if (op == 0xA0u) o(out, "    md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, %s, 0x%04Xu));\n", sg, addr);
            else if (op == 0xA1u) o(out, "    cpu->r[0] = md_x86_read16(cpu, %s, 0x%04Xu);\n", sg, addr);
            else if (op == 0xA2u) o(out, "    md_x86_write8(cpu, %s, 0x%04Xu, md_x86_get_reg8(cpu, 0u));\n    MD_AOT_WCHK();\n", sg, addr);
            else o(out, "    md_x86_write16(cpu, %s, 0x%04Xu, cpu->r[0]);\n    MD_AOT_WCHK();\n", sg, addr);
            return DR_GEN_CONTINUE;
        }

        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
            const char *src = kSeg[seg_ov >= 0 ? seg_ov : 3];
            const char *wd = w16 ? "16" : "8";
            if (rep) return DR_GEN_HOLE;                 /* REP strings stay canonical */
            TICK();
            o(out, "    { const uint16_t dl_ = (cpu->flags & MD_X86_FLAG_DF) != 0u ? (uint16_t)-%d : %du;\n",
              w16 ? 2 : 1, w16 ? 2 : 1);
            switch (op & 0xFEu) {
                case 0xA4:
                    o(out, "      md_x86_write%s(cpu, cpu->es, cpu->r[7], md_x86_read%s(cpu, %s, cpu->r[6]));\n", wd, wd, src);
                    o(out, "      cpu->r[6] = (uint16_t)(cpu->r[6] + dl_); cpu->r[7] = (uint16_t)(cpu->r[7] + dl_);\n");
                    o(out, "      MD_AOT_WCHK(); }\n");
                    break;
                case 0xA6:
                    o(out, "      (void)md_x86_sub%s(cpu, md_x86_read%s(cpu, %s, cpu->r[6]), md_x86_read%s(cpu, cpu->es, cpu->r[7]));\n", wd, wd, src, wd);
                    o(out, "      cpu->r[6] = (uint16_t)(cpu->r[6] + dl_); cpu->r[7] = (uint16_t)(cpu->r[7] + dl_); }\n");
                    break;
                case 0xAA:
                    if (w16) o(out, "      md_x86_write16(cpu, cpu->es, cpu->r[7], cpu->r[0]);\n");
                    else o(out, "      md_x86_write8(cpu, cpu->es, cpu->r[7], md_x86_get_reg8(cpu, 0u));\n");
                    o(out, "      cpu->r[7] = (uint16_t)(cpu->r[7] + dl_);\n      MD_AOT_WCHK(); }\n");
                    break;
                case 0xAC:
                    if (w16) o(out, "      cpu->r[0] = md_x86_read16(cpu, %s, cpu->r[6]);\n", src);
                    else o(out, "      md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, %s, cpu->r[6]));\n", src);
                    o(out, "      cpu->r[6] = (uint16_t)(cpu->r[6] + dl_); }\n");
                    break;
                default: /* AE SCAS */
                    if (w16) o(out, "      (void)md_x86_sub16(cpu, cpu->r[0], md_x86_read16(cpu, cpu->es, cpu->r[7]));\n");
                    else o(out, "      (void)md_x86_sub8(cpu, md_x86_get_reg8(cpu, 0u), md_x86_read8(cpu, cpu->es, cpu->r[7]));\n");
                    o(out, "      cpu->r[7] = (uint16_t)(cpu->r[7] + dl_); }\n");
                    break;
            }
            return DR_GEN_CONTINUE;
        }

        case 0xA8:
            TICK();
            o(out, "    (void)md_x86_logic8(cpu, (uint8_t)(md_x86_get_reg8(cpu, 0u) & 0x%02Xu));\n", dr_u8(p, at + 1u));
            return DR_GEN_CONTINUE;
        case 0xA9:
            TICK();
            o(out, "    (void)md_x86_logic16(cpu, (uint16_t)(cpu->r[0] & 0x%04Xu));\n", dr_u16(p, at + 1u));
            return DR_GEN_CONTINUE;

        case 0xC2: case 0xC3:                    /* RET near */
            TICK();
            o(out, "    cpu->ip = md_x86_pop(cpu);\n");
            if (op == 0xC2u) o(out, "    cpu->r[4] = (uint16_t)(cpu->r[4] + 0x%04Xu);\n", dr_u16(p, at + 1u));
            o(out, "    goto md_dispatch;\n");
            return DR_GEN_TERMINAL;

        case 0xCA: case 0xCB:                    /* RETF */
            TICK();
            o(out, "    cpu->ip = md_x86_pop(cpu);\n    cpu->cs = md_x86_pop(cpu);\n");
            if (op == 0xCAu) o(out, "    cpu->r[4] = (uint16_t)(cpu->r[4] + 0x%04Xu);\n", dr_u16(p, at + 1u));
            o(out, "    goto md_dispatch;\n");
            return DR_GEN_TERMINAL;

        case 0xC4: case 0xC5:                    /* LES/LDS */
            dr_modrm(p, at + 1u, seg_ov, &m);
            if (m.is_reg) return DR_GEN_HOLE;
            TICK(); OPEN(); dr_ea(out, &m);
            o(out, "        const uint16_t lo_ = md_x86_read16(cpu, s_, o_);\n"
                   "        const uint16_t hi_ = md_x86_read16(cpu, s_, (uint16_t)(o_ + 2u));\n"
                   "        cpu->r[%u] = lo_; %s = hi_;\n", m.reg, op == 0xC4u ? "cpu->es" : "cpu->ds");
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0xC6: case 0xC7:                    /* MOV r/m,imm */
            dr_modrm(p, at + 1u, seg_ov, &m);
            if (m.reg != 0u) return DR_GEN_HOLE;
            TICK(); OPEN(); dr_ea(out, &m);
            if (w16) snprintf(b, sizeof(b), "0x%04Xu", dr_u16(p, at + 1u + m.length));
            else snprintf(b, sizeof(b), "0x%02Xu", dr_u8(p, at + 1u + m.length));
            dr_wr(out, &m, w16, b);
            CLOSE();
            return DR_GEN_CONTINUE;

        case 0xCD:                               /* INT imm8 -> DOS/BIOS */
            TICK();
            o(out, "    (void)md_runtime_interrupt(runtime, 0x%02Xu);\n    goto md_dispatch;\n", dr_u8(p, at + 1u));
            return DR_GEN_TERMINAL;

        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {   /* shifts/rotates */
            const char *count = op >= 0xD2u ? "md_x86_get_reg8(cpu, 1u)" : "1u";
            char v[256];
            dr_modrm(p, at + 1u, seg_ov, &m);
            TICK(); OPEN(); dr_ea(out, &m);
            dr_rd(a, sizeof(a), &m, w16);
            snprintf(v, sizeof(v), "md_x86_shift%s(cpu, %uu, %s, %s)", w16 ? "16" : "8", m.reg, a, count);
            dr_wr(out, &m, w16, v);
            CLOSE();
            return DR_GEN_CONTINUE;
        }

        case 0xE0: case 0xE1: case 0xE2: case 0xE3:   /* LOOPNZ/LOOPZ/LOOP/JCXZ */
            TICK();
            if (op == 0xE3u) o(out, "    if (cpu->r[1] == 0u) {\n");
            else {
                o(out, "    cpu->r[1] = (uint16_t)(cpu->r[1] - 1u);\n");
                if (op == 0xE2u) o(out, "    if (cpu->r[1] != 0u) {\n");
                else if (op == 0xE1u) o(out, "    if (cpu->r[1] != 0u && (cpu->flags & MD_X86_FLAG_ZF) != 0u) {\n");
                else o(out, "    if (cpu->r[1] != 0u && (cpu->flags & MD_X86_FLAG_ZF) == 0u) {\n");
            }
            dr_goto(out, p, d->target);
            o(out, "    }\n");
            dr_goto(out, p, next);
            return DR_GEN_TERMINAL;

        case 0xE8:                               /* CALL rel16 */
            TICK();
            o(out, "    md_x86_push(cpu, 0x%04Xu);\n    cpu->ip = 0x%04Xu;\n    MD_AOT_WCHK();\n", next, d->target);
            dr_goto(out, p, d->target);
            return DR_GEN_TERMINAL;

        case 0xE9: case 0xEB:                    /* JMP rel */
            TICK();
            dr_goto(out, p, d->target);
            return DR_GEN_TERMINAL;

        case 0xF4:
            TICK();
            o(out, "    runtime->stop_reason = MD_STOP_HALT;\n    return MD_STOP_HALT;\n");
            return DR_GEN_TERMINAL;

        case 0xF5: TICK(); o(out, "    cpu->flags ^= MD_X86_FLAG_CF;\n"); return DR_GEN_CONTINUE;
        case 0xF8: TICK(); o(out, "    cpu->flags &= (uint16_t)~MD_X86_FLAG_CF;\n"); return DR_GEN_CONTINUE;
        case 0xF9: TICK(); o(out, "    cpu->flags |= MD_X86_FLAG_CF;\n"); return DR_GEN_CONTINUE;
        case 0xFA: TICK(); o(out, "    cpu->flags &= (uint16_t)~MD_X86_FLAG_IF;\n"); return DR_GEN_CONTINUE;
        case 0xFB: TICK(); o(out, "    cpu->flags |= MD_X86_FLAG_IF;\n"); return DR_GEN_CONTINUE;
        case 0xFC: TICK(); o(out, "    cpu->flags &= (uint16_t)~MD_X86_FLAG_DF;\n"); return DR_GEN_CONTINUE;
        case 0xFD: TICK(); o(out, "    cpu->flags |= MD_X86_FLAG_DF;\n"); return DR_GEN_CONTINUE;

        case 0xF6: case 0xF7: {                  /* group 3: TEST/NOT/NEG only */
            char v[256];
            dr_modrm(p, at + 1u, seg_ov, &m);
            if (m.reg != 0u && m.reg != 2u && m.reg != 3u) return DR_GEN_HOLE;   /* MUL/DIV... */
            TICK(); OPEN(); dr_ea(out, &m);
            dr_rd(a, sizeof(a), &m, w16);
            if (m.reg == 0u) {
                if (w16) o(out, "        (void)md_x86_logic16(cpu, (uint16_t)(%s & 0x%04Xu));\n", a, dr_u16(p, at + 1u + m.length));
                else o(out, "        (void)md_x86_logic8(cpu, (uint8_t)(%s & 0x%02Xu));\n", a, dr_u8(p, at + 1u + m.length));
            } else {
                if (m.reg == 2u) snprintf(v, sizeof(v), "(uint%s_t)~%s", w16 ? "16" : "8", a);
                else snprintf(v, sizeof(v), "md_x86_sub%s(cpu, 0u, %s)", w16 ? "16" : "8", a);
                dr_wr(out, &m, w16, v);
            }
            CLOSE();
            return DR_GEN_CONTINUE;
        }

        case 0xFE: case 0xFF: {                  /* group 4/5 */
            dr_modrm(p, at + 1u, seg_ov, &m);
            if (m.reg == 0u || m.reg == 1u) {
                char v[256];
                const int wide = op == 0xFFu;
                TICK(); OPEN(); dr_ea(out, &m);
                dr_rd(a, sizeof(a), &m, wide);
                o(out, "        const uint16_t cf_ = cpu->flags & MD_X86_FLAG_CF;\n");
                snprintf(v, sizeof(v), "md_x86_%s%s(cpu, %s, 1u)", m.reg == 0u ? "add" : "sub", wide ? "16" : "8", a);
                o(out, "        const uint%s_t r_ = %s;\n", wide ? "16" : "8", v);
                o(out, "        cpu->flags = (uint16_t)((cpu->flags & (uint16_t)~MD_X86_FLAG_CF) | cf_);\n");
                dr_wr(out, &m, wide, "r_");
                CLOSE();
                return DR_GEN_CONTINUE;
            }
            if (op != 0xFFu) return DR_GEN_HOLE;
            if (m.reg == 2u || m.reg == 4u) {    /* CALL/JMP near indirect */
                TICK(); OPEN(); dr_ea(out, &m);
                dr_rd(a, sizeof(a), &m, 1);
                o(out, "        const uint16_t t_ = %s;\n", a);
                if (m.reg == 2u) o(out, "        md_x86_push(cpu, 0x%04Xu);\n", next);
                o(out, "        cpu->ip = t_;\n");
                if (m.reg == 2u) o(out, "        MD_AOT_WCHK();\n");
                CLOSE();
                o(out, "    goto md_dispatch;\n");
                return DR_GEN_TERMINAL;
            }
            if (m.reg == 6u) {                   /* PUSH r/m16 */
                TICK(); OPEN(); dr_ea(out, &m);
                dr_rd(a, sizeof(a), &m, 1);
                o(out, "        md_x86_push(cpu, %s);\n        MD_AOT_WCHK();\n", a);
                CLOSE();
                return DR_GEN_CONTINUE;
            }
            return DR_GEN_HOLE;
        }

        default:
            return DR_GEN_HOLE;
    }
#undef TICK
#undef OPEN
#undef CLOSE
}

/* ------------------------------------------------------------------------ */

static void dr_classify(DrProgram *p)
{
    uint32_t ip;
    for (ip = 0; ip < DR_IP_SPACE; ++ip) {
        if (!p->reachable[ip]) continue;
        p->kind[ip] = (uint8_t)dr_gen(p, (uint16_t)ip, NULL);
    }
    /* Anything that leaves straight-line compiled code must be followed by a
       block entry so execution can come back in. */
    for (ip = 0; ip < DR_IP_SPACE; ++ip) {
        if (!p->reachable[ip]) continue;
        if (p->kind[ip] != DR_GEN_CONTINUE) dr_mark_start(p, p->dec[ip].next_ip);
        if (p->kind[ip] == DR_GEN_HOLE) ++p->hole_count;
    }
    for (ip = 0; ip < DR_IP_SPACE; ++ip) {
        if (p->block_start[ip] && p->reachable[ip]) {
            ++p->block_count;
            if (p->kind[ip] != DR_GEN_HOLE) ++p->entry_count;
        }
    }
}

static int dr_is_entry(const DrProgram *p, uint32_t ip)
{
    return p->block_start[ip] && p->reachable[ip] && p->kind[ip] != DR_GEN_HOLE;
}

static void dr_make_guard(const char *symbol, char *guard, size_t n)
{
    size_t i, j = 0;
    const char *prefix = "MICRODOS_GENERATED_";
    for (i = 0; prefix[i] != '\0' && j + 1 < n; ++i) guard[j++] = prefix[i];
    for (i = 0; symbol[i] != '\0' && j + 3 < n; ++i) {
        char c = symbol[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        guard[j++] = c;
    }
    if (j + 3 < n) { guard[j++] = '_'; guard[j++] = 'H'; }
    guard[j] = '\0';
}

static int dr_emit_header(const DrOptions *opt)
{
    FILE *f = fopen(opt->output_h, "w");
    char g[256];
    if (f == NULL) return 0;
    dr_make_guard(opt->symbol, g, sizeof(g));
    fprintf(f,
            "/* Generated by dosrecomp. Do not hand-edit. */\n"
            "#ifndef %s\n#define %s\n\n"
            "#include \"microdos/aot.h\"\n#include \"microdos/runtime.h\"\n\n"
            "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
            "/* Standalone: load the embedded image at segment:0100h and run it. */\n"
            "MdStopReason %s(MdRuntime *runtime, uint16_t segment, uint64_t instruction_budget);\n\n"
            "/* Attach mode for images loaded by a real DOS (see microdos/aot.h). */\n"
            "extern const MdAotProgram %s_program;\n\n"
            "#ifdef __cplusplus\n}\n#endif\n\n#endif\n",
            g, g, opt->symbol, opt->symbol);
    return fclose(f) == 0;
}

static void dr_emit_bytes(FILE *f, const char *name, const uint8_t *data, size_t size)
{
    size_t i;
    fprintf(f, "static const uint8_t %s[%zu] = {", name, size ? size : 1u);
    for (i = 0; i < size; ++i) {
        if (i % 16u == 0u) fprintf(f, "\n    ");
        fprintf(f, "0x%02Xu,", data[i]);
    }
    if (size == 0u) fprintf(f, "0");
    fprintf(f, "\n};\n\n");
}

static int dr_emit_c(DrProgram *p, const DrOptions *opt, const char *header_name)
{
    FILE *f = fopen(opt->output_c, "w");
    const char *s = opt->symbol;
    uint32_t ip, lo = DR_IP_SPACE, hi = 0;
    if (f == NULL) return 0;

    for (ip = 0; ip < DR_IP_SPACE; ++ip) {
        if (p->reachable[ip]) {
            if (ip < lo) lo = ip;
            if ((uint32_t)ip + p->dec[ip].length > hi) hi = (uint32_t)ip + p->dec[ip].length;
        }
    }
    if (lo == DR_IP_SPACE) { lo = DR_COM_BASE; hi = DR_COM_BASE; }

    fprintf(f, "/* Generated by dosrecomp v2. Do not hand-edit.\n"
               " * %zu instructions: %zu compiled, %zu interpreter holes; %zu blocks, %zu entries. */\n",
            p->inst_count, p->inst_count - p->hole_count, p->hole_count, p->block_count, p->entry_count);
    fprintf(f, "#include <string.h>\n#include \"microdos/block_cache.h\"\n#include \"microdos/ops.h\"\n"
               "#include \"microdos/runtime.h\"\n#include \"%s\"\n\n", header_name);
    dr_emit_bytes(f, "md_image", p->image, p->image_size);
    dr_emit_bytes(f, "md_code_bits", p->code_bits, (p->image_size + 7u) / 8u);

    fprintf(f,
        "#define MD_AOT_SLOTS 4u\n"
        "static MdAotGuard md_guard[MD_AOT_SLOTS];\n"
        "static uint16_t md_guard_segment[MD_AOT_SLOTS];\n"
        "static unsigned md_guard_next;\n\n"
        "static int md_is_entry(uint16_t ip)\n{\n    switch (ip) {\n");
    for (ip = 0; ip < DR_IP_SPACE; ++ip) if (dr_is_entry(p, ip)) fprintf(f, "        case 0x%04Xu:\n", ip);
    fprintf(f, "            return 1;\n        default: return 0;\n    }\n}\n\n");

    fprintf(f,
        "static MdAotGuard *md_slot(uint16_t segment, int create)\n{\n"
        "    unsigned i;\n"
        "    for (i = 0; i < MD_AOT_SLOTS; ++i)\n"
        "        if (md_guard[i].code_bits != NULL && md_guard_segment[i] == segment) return &md_guard[i];\n"
        "    if (!create) return NULL;\n"
        "    i = md_guard_next++ %% MD_AOT_SLOTS;\n"
        "    md_guard_segment[i] = segment;\n"
        "    return &md_guard[i];\n}\n\n"
        "static void md_arm(MdRuntime *runtime, MdAotGuard *guard, uint16_t segment)\n{\n"
        "    guard->base = md_x86_linear(segment, 0x0100u);\n"
        "    guard->size = (uint32_t)sizeof(md_image);\n"
        "    guard->code_bits = md_code_bits;\n"
        "    guard->valid = 1u;\n"
        "    md_runtime_register_aot_guard(runtime, guard);\n"
        "    md_runtime_mark_code_range(runtime, segment, 0x%04Xu, %uu);\n}\n\n",
        lo, (unsigned)(hi - lo));

    fprintf(f,
        "typedef struct MdResume { uint16_t segment; const MdAotGuard *guard; } MdResume;\n\n"
        "static bool md_resume(const MdRuntime *runtime, void *user)\n{\n"
        "    const MdResume *r = (const MdResume *)user;\n"
        "    return r->guard->valid && runtime->cpu.cs == r->segment && md_is_entry(runtime->cpu.ip);\n}\n\n");

    /* body */
    fprintf(f,
        "static MdStopReason md_body(MdRuntime *runtime, uint16_t segment, MdAotGuard *guard,\n"
        "                            uint64_t instruction_budget, int attach)\n{\n"
        "    MdX86 *cpu = &runtime->cpu;\n"
        "    uint64_t remaining = instruction_budget;\n\n"
        "#define MD_AOT_TICK(ip_, next_) do { \\\n"
        "        if (remaining == 0u) { cpu->ip = (uint16_t)(ip_); \\\n"
        "            if (attach) return MD_STOP_NONE; \\\n"
        "            runtime->stop_reason = MD_STOP_BUDGET; return MD_STOP_BUDGET; } \\\n"
        "        --remaining; ++runtime->instructions; ++runtime->aot_instructions; \\\n"
        "        cpu->ip = (uint16_t)(next_); \\\n"
        "    } while (0)\n"
        "#define MD_AOT_WCHK() do { if (!guard->valid) goto md_fallback; } while (0)\n"
        "#define MD_AOT_GOTO(ip_, label_) do { cpu->ip = (uint16_t)(ip_); goto label_; } while (0)\n\n"
        "md_dispatch:\n"
        "    if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason;\n"
        "    if (!guard->valid || cpu->cs != segment) goto md_fallback;\n"
        "    switch (cpu->ip) {\n");
    for (ip = 0; ip < DR_IP_SPACE; ++ip) {
        if (dr_is_entry(p, ip)) fprintf(f, "        case 0x%04Xu: goto md_block_%04X;\n", ip, ip);
    }
    fprintf(f,
        "        default: goto md_fallback;\n    }\n\n"
        "md_fallback:\n"
        "    if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason;\n"
        "    if (attach) return MD_STOP_NONE;\n"
        "    if (runtime->block_cache != NULL) {\n"
        "        MdResume resume;\n"
        "        const uint64_t before = runtime->instructions;\n"
        "        MdStopReason st;\n"
        "        uint64_t used;\n"
        "        resume.segment = segment; resume.guard = guard;\n"
        "        st = md_interp_run_cached_until(runtime, runtime->block_cache, remaining, md_resume, &resume);\n"
        "        used = runtime->instructions - before;\n"
        "        if (used > remaining) { runtime->stop_reason = MD_STOP_FAULT; return MD_STOP_FAULT; }\n"
        "        remaining -= used;\n"
        "        if (st != MD_STOP_NONE) return st;\n"
        "        goto md_dispatch;\n"
        "    }\n"
        "    if (remaining == 0u) { runtime->stop_reason = MD_STOP_BUDGET; return MD_STOP_BUDGET; }\n"
        "    --remaining;\n"
        "    (void)md_interp_step(runtime);\n"
        "    goto md_dispatch;\n\n");

    for (ip = 0; ip < DR_IP_SPACE; ++ip) {
        uint32_t cur;
        if (!dr_is_entry(p, ip)) continue;
        fprintf(f, "md_block_%04X:\n", ip);
        cur = ip;
        for (;;) {
            int k;
            const MdDecodedInstruction *d = &p->dec[cur];
            unsigned i;
            fprintf(f, "    /* %04X:", (unsigned)cur);
            for (i = 0; i < d->length; ++i) fprintf(f, " %02X", dr_u8(p, cur + i));
            fprintf(f, " */\n");
            if (p->kind[cur] == DR_GEN_HOLE) {
                fprintf(f, "    cpu->ip = 0x%04Xu; goto md_fallback; /* interpreter hole */\n", (unsigned)cur);
                break;
            }
            k = dr_gen(p, (uint16_t)cur, f);
            if (k != DR_GEN_CONTINUE) break;
            cur = d->next_ip;
            if (!p->reachable[cur] || !dr_in_code(p, cur)) {
                fprintf(f, "    goto md_dispatch;\n");
                break;
            }
            if (p->block_start[cur]) {
                if (dr_is_entry(p, cur)) fprintf(f, "    goto md_block_%04X;\n", (unsigned)cur);
                else fprintf(f, "    cpu->ip = 0x%04Xu; goto md_fallback;\n", (unsigned)cur);
                break;
            }
        }
        fprintf(f, "\n");
    }
    fprintf(f, "#undef MD_AOT_TICK\n#undef MD_AOT_WCHK\n#undef MD_AOT_GOTO\n}\n\n");

    /* public API */
    fprintf(f,
        "MdStopReason %s(MdRuntime *runtime, uint16_t segment, uint64_t instruction_budget)\n{\n"
        "    MdAotGuard *guard = md_slot(segment, 1);\n"
        "    md_runtime_load_com(runtime, md_image, sizeof(md_image), segment);\n"
        "    md_arm(runtime, guard, segment);\n"
        "    runtime->cpu.ip = 0x%04Xu;\n"
        "    return md_body(runtime, segment, guard, instruction_budget, 0);\n}\n\n",
        s, p->code_start);
    fprintf(f,
        "static bool md_attach(MdRuntime *runtime, uint16_t segment)\n{\n"
        "    const uint32_t base = md_x86_linear(segment, 0x0100u);\n"
        "    if (base + sizeof(md_image) > MD_X86_ADDRESS_SPACE) return false;\n"
        "    if (memcmp(runtime->cpu.memory + base, md_image, sizeof(md_image)) != 0) return false;\n"
        "    md_arm(runtime, md_slot(segment, 1), segment);\n"
        "    return true;\n}\n\n"
        "static bool md_ready(const MdRuntime *runtime, uint16_t segment)\n{\n"
        "    const MdAotGuard *guard = md_slot(segment, 0);\n"
        "    (void)runtime;\n"
        "    return guard != NULL && guard->valid;\n}\n\n"
        "static bool md_entry(uint16_t ip) { return md_is_entry(ip) != 0; }\n\n"
        "static MdStopReason md_enter(MdRuntime *runtime, uint64_t budget)\n{\n"
        "    MdAotGuard *guard = md_slot(runtime->cpu.cs, 0);\n"
        "    if (guard == NULL || !guard->valid) return MD_STOP_NONE;\n"
        "    return md_body(runtime, runtime->cpu.cs, guard, budget, 1);\n}\n\n"
        "const MdAotProgram %s_program = {\n"
        "    \"%s\", %zuu, %zuu, %zuu, %zuu,\n"
        "    md_attach, md_ready, md_entry, md_enter\n};\n",
        s, opt->program_name ? opt->program_name : s, p->image_size,
        p->inst_count - p->hole_count, p->hole_count, p->entry_count);

    return fclose(f) == 0;
}

static void dr_dump(const DrProgram *p)
{
    uint32_t ip;
    for (ip = 0; ip < DR_IP_SPACE; ++ip) {
        if (!p->reachable[ip]) continue;
        printf("%s%04X  %-5s len=%u op=%02X\n", p->block_start[ip] ? "*" : " ", (unsigned)ip,
               p->kind[ip] == DR_GEN_HOLE ? "HOLE" : "aot", p->dec[ip].length,
               dr_u8(p, ip + p->dec[ip].prefix_count));
    }
}

static const char *dr_basename(const char *path)
{
    const char *b = path, *q;
    for (q = path; *q; ++q) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

int main(int argc, char **argv)
{
    DrOptions opt;
    DrProgram *p;
    size_t i;

    if (!dr_parse_options(argc, argv, &opt) || !dr_valid_symbol(opt.symbol)) {
        dr_usage(argv[0]);
        return 2;
    }
    p = (DrProgram *)calloc(1u, sizeof(*p));
    if (p == NULL) return 2;
    p->image = dr_read_file(opt.input, &p->image_size);
    if (p->image == NULL || p->image_size == 0u || p->image_size > DR_MAX_IMAGE) {
        fprintf(stderr, "dosrecomp: cannot read .COM image %s\n", opt.input);
        return 2;
    }
    p->dec = (MdDecodedInstruction *)calloc(DR_IP_SPACE, sizeof(MdDecodedInstruction));
    p->code_bits = (uint8_t *)calloc((p->image_size + 7u) / 8u + 1u, 1u);
    if (p->dec == NULL || p->code_bits == NULL) return 2;
    p->code_start = opt.code_start;
    p->code_end = opt.code_end;
    for (i = 0; i < opt.interp_count; ++i) p->forced_hole[opt.interp_at[i]] = 1u;

    dr_discover(p, &opt);
    dr_classify(p);

    if (!dr_emit_header(&opt)) { fprintf(stderr, "dosrecomp: cannot write %s\n", opt.output_h); return 2; }
    /* The generated .c includes its header by basename (same directory). */
    if (!dr_emit_c(p, &opt, dr_basename(opt.output_h))) { fprintf(stderr, "dosrecomp: cannot write %s\n", opt.output_c); return 2; }

    if (opt.dump) dr_dump(p);
    printf("dosrecomp: %s: %zu bytes, %zu instructions (%zu compiled, %zu holes), %zu blocks, %zu entries\n",
           opt.input, p->image_size, p->inst_count, p->inst_count - p->hole_count,
           p->hole_count, p->block_count, p->entry_count);
    return 0;
}
