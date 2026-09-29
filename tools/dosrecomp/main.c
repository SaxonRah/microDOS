#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_COM_BASE 0x0100u
#define DR_MAX_IMAGE 0xFF00u
#define DR_MAX_IP 0x10000u

typedef enum DrKind {
    DR_INVALID = 0,
    DR_MOV_R8_IMM,
    DR_MOV_R16_IMM,
    DR_INC_R16,
    DR_DEC_R16,
    DR_PUSH_R16,
    DR_POP_R16,
    DR_ADD_AL_IMM,
    DR_ADD_AX_IMM,
    DR_SUB_AL_IMM,
    DR_SUB_AX_IMM,
    DR_CMP_AL_IMM,
    DR_CMP_AX_IMM,
    DR_JZ_REL8,
    DR_JNZ_REL8,
    DR_NOP,
    DR_MOV_AL_MOFFS,
    DR_MOV_AX_MOFFS,
    DR_MOV_MOFFS_AL,
    DR_MOV_MOFFS_AX,
    DR_RET,
    DR_INT,
    DR_CALL_REL16,
    DR_JMP_REL16,
    DR_JMP_REL8,
    DR_HLT
} DrKind;

typedef struct DrInst {
    DrKind kind;
    uint16_t ip;
    uint16_t next_ip;
    uint16_t target;
    uint16_t imm16;
    uint8_t imm8;
    uint8_t opcode;
    uint8_t reg;
} DrInst;

typedef struct DrBlock {
    uint16_t start;
    size_t first_inst;
    size_t inst_count;
    uint16_t fallback_ip;
    int has_fallback;
} DrBlock;

typedef struct DrProgram {
    uint8_t *image;
    size_t image_size;
    uint16_t code_start;
    uint32_t code_end;

    DrInst *decoded;
    DrInst *insts;
    size_t inst_count;
    size_t inst_capacity;

    DrBlock *blocks;
    size_t block_count;
    size_t block_capacity;

    uint8_t reachable[DR_MAX_IP];
    uint8_t fallback_at[DR_MAX_IP];
    uint8_t is_block_start[DR_MAX_IP];
    uint8_t processed[DR_MAX_IP];
    uint8_t queued[DR_MAX_IP];
    uint16_t queue[DR_MAX_IP];
    size_t queue_head;
    size_t queue_tail;
    size_t fallback_blocks;
} DrProgram;

typedef struct DrOptions {
    const char *input;
    const char *output_c;
    const char *output_h;
    const char *symbol;
    uint16_t code_start;
    uint32_t code_end;
    int code_end_set;
    int dump;
} DrOptions;

static void dr_usage(const char *exe)
{
    fprintf(stderr,
            "usage: %s --input file.com --output-c out.c --output-h out.h "
            "--symbol name [options]\n"
            "\n"
            "options:\n"
            "  --code-start N   first guest offset treated as code (default 0x100)\n"
            "  --code-end N     exclusive guest offset treated as code (default image end)\n"
            "  --dump           print discovered blocks\n",
            exe);
}

static int dr_parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > 0x10000ul) {
        return 0;
    }

    *out = (uint32_t)value;
    return 1;
}

static int dr_parse_options(int argc, char **argv, DrOptions *opt)
{
    int i;

    memset(opt, 0, sizeof(*opt));
    opt->code_start = DR_COM_BASE;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            opt->input = argv[++i];
        } else if (strcmp(argv[i], "--output-c") == 0 && i + 1 < argc) {
            opt->output_c = argv[++i];
        } else if (strcmp(argv[i], "--output-h") == 0 && i + 1 < argc) {
            opt->output_h = argv[++i];
        } else if (strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
            opt->symbol = argv[++i];
        } else if (strcmp(argv[i], "--code-start") == 0 && i + 1 < argc) {
            uint32_t value;
            if (!dr_parse_u32(argv[++i], &value) || value > 0xFFFFu) {
                return 0;
            }
            opt->code_start = (uint16_t)value;
        } else if (strcmp(argv[i], "--code-end") == 0 && i + 1 < argc) {
            if (!dr_parse_u32(argv[++i], &opt->code_end)) {
                return 0;
            }
            opt->code_end_set = 1;
        } else if (strcmp(argv[i], "--dump") == 0) {
            opt->dump = 1;
        } else {
            return 0;
        }
    }

    return opt->input != NULL &&
           opt->output_c != NULL &&
           opt->output_h != NULL &&
           opt->symbol != NULL;
}

static int dr_valid_symbol(const char *symbol)
{
    size_t i;

    if (symbol == NULL || symbol[0] == '\0') {
        return 0;
    }

    if (!(symbol[0] == '_' ||
          (symbol[0] >= 'A' && symbol[0] <= 'Z') ||
          (symbol[0] >= 'a' && symbol[0] <= 'z'))) {
        return 0;
    }

    for (i = 1u; symbol[i] != '\0'; ++i) {
        const char ch = symbol[i];
        if (!(ch == '_' ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9'))) {
            return 0;
        }
    }

    return 1;
}

static uint8_t *dr_read_file(const char *path, size_t *size_out)
{
    FILE *file;
    long length;
    uint8_t *data;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0 || (unsigned long)length > DR_MAX_IMAGE) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    data = (uint8_t *)malloc((size_t)length != 0u ? (size_t)length : 1u);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }

    got = fread(data, 1u, (size_t)length, file);
    fclose(file);

    if (got != (size_t)length) {
        free(data);
        return NULL;
    }

    *size_out = got;
    return data;
}

static int dr_ip_in_image(const DrProgram *program, uint16_t ip)
{
    const uint32_t image_end = DR_COM_BASE + (uint32_t)program->image_size;
    return (uint32_t)ip >= DR_COM_BASE && (uint32_t)ip < image_end;
}

static int dr_ip_in_code(const DrProgram *program, uint16_t ip)
{
    return (uint32_t)ip >= program->code_start &&
           (uint32_t)ip < program->code_end &&
           dr_ip_in_image(program, ip);
}

static int dr_have_bytes(const DrProgram *program, uint16_t ip, unsigned count)
{
    const uint32_t begin = (uint32_t)ip;
    const uint32_t end = begin + count;
    const uint32_t image_end = DR_COM_BASE + (uint32_t)program->image_size;

    return begin >= DR_COM_BASE &&
           end <= image_end &&
           end <= program->code_end;
}

static uint8_t dr_u8(const DrProgram *program, uint16_t ip)
{
    return program->image[(size_t)((uint32_t)ip - DR_COM_BASE)];
}

static uint16_t dr_u16(const DrProgram *program, uint16_t ip)
{
    const uint8_t lo = dr_u8(program, ip);
    const uint8_t hi = dr_u8(program, (uint16_t)(ip + 1u));
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static int dr_decode(const DrProgram *program, uint16_t ip, DrInst *out)
{
    uint8_t opcode;

    memset(out, 0, sizeof(*out));
    out->ip = ip;

    if (!dr_have_bytes(program, ip, 1u)) {
        return 0;
    }

    opcode = dr_u8(program, ip);
    out->opcode = opcode;

    if ((opcode & 0xF8u) == 0xB0u) {
        if (!dr_have_bytes(program, ip, 2u)) return 0;
        out->kind = DR_MOV_R8_IMM;
        out->reg = opcode & 7u;
        out->imm8 = dr_u8(program, (uint16_t)(ip + 1u));
        out->next_ip = (uint16_t)(ip + 2u);
        return 1;
    }

    if ((opcode & 0xF8u) == 0xB8u) {
        if (!dr_have_bytes(program, ip, 3u)) return 0;
        out->kind = DR_MOV_R16_IMM;
        out->reg = opcode & 7u;
        out->imm16 = dr_u16(program, (uint16_t)(ip + 1u));
        out->next_ip = (uint16_t)(ip + 3u);
        return 1;
    }

    if ((opcode & 0xF8u) == 0x40u) {
        out->kind = DR_INC_R16;
        out->reg = opcode & 7u;
        out->next_ip = (uint16_t)(ip + 1u);
        return 1;
    }

    if ((opcode & 0xF8u) == 0x48u) {
        out->kind = DR_DEC_R16;
        out->reg = opcode & 7u;
        out->next_ip = (uint16_t)(ip + 1u);
        return 1;
    }

    if ((opcode & 0xF8u) == 0x50u) {
        out->kind = DR_PUSH_R16;
        out->reg = opcode & 7u;
        out->next_ip = (uint16_t)(ip + 1u);
        return 1;
    }

    if ((opcode & 0xF8u) == 0x58u) {
        out->kind = DR_POP_R16;
        out->reg = opcode & 7u;
        out->next_ip = (uint16_t)(ip + 1u);
        return 1;
    }

    switch (opcode) {
        case 0x04:
        case 0x2C:
        case 0x3C:
            if (!dr_have_bytes(program, ip, 2u)) return 0;
            out->kind = opcode == 0x04u ? DR_ADD_AL_IMM
                      : opcode == 0x2Cu ? DR_SUB_AL_IMM
                                        : DR_CMP_AL_IMM;
            out->imm8 = dr_u8(program, (uint16_t)(ip + 1u));
            out->next_ip = (uint16_t)(ip + 2u);
            return 1;

        case 0x05:
        case 0x2D:
        case 0x3D:
            if (!dr_have_bytes(program, ip, 3u)) return 0;
            out->kind = opcode == 0x05u ? DR_ADD_AX_IMM
                      : opcode == 0x2Du ? DR_SUB_AX_IMM
                                        : DR_CMP_AX_IMM;
            out->imm16 = dr_u16(program, (uint16_t)(ip + 1u));
            out->next_ip = (uint16_t)(ip + 3u);
            return 1;

        case 0x74:
        case 0x75: {
            int8_t rel;
            if (!dr_have_bytes(program, ip, 2u)) return 0;
            rel = (int8_t)dr_u8(program, (uint16_t)(ip + 1u));
            out->kind = opcode == 0x74u ? DR_JZ_REL8 : DR_JNZ_REL8;
            out->next_ip = (uint16_t)(ip + 2u);
            out->target = (uint16_t)(out->next_ip + rel);
            return 1;
        }

        case 0x90:
            out->kind = DR_NOP;
            out->next_ip = (uint16_t)(ip + 1u);
            return 1;

        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA3:
            if (!dr_have_bytes(program, ip, 3u)) return 0;
            out->kind = opcode == 0xA0u ? DR_MOV_AL_MOFFS
                      : opcode == 0xA1u ? DR_MOV_AX_MOFFS
                      : opcode == 0xA2u ? DR_MOV_MOFFS_AL
                                        : DR_MOV_MOFFS_AX;
            out->imm16 = dr_u16(program, (uint16_t)(ip + 1u));
            out->next_ip = (uint16_t)(ip + 3u);
            return 1;

        case 0xC3:
            out->kind = DR_RET;
            out->next_ip = (uint16_t)(ip + 1u);
            return 1;

        case 0xCD:
            if (!dr_have_bytes(program, ip, 2u)) return 0;
            out->kind = DR_INT;
            out->imm8 = dr_u8(program, (uint16_t)(ip + 1u));
            out->next_ip = (uint16_t)(ip + 2u);
            return 1;

        case 0xE8:
        case 0xE9: {
            int16_t rel;
            if (!dr_have_bytes(program, ip, 3u)) return 0;
            rel = (int16_t)dr_u16(program, (uint16_t)(ip + 1u));
            out->kind = opcode == 0xE8u ? DR_CALL_REL16 : DR_JMP_REL16;
            out->next_ip = (uint16_t)(ip + 3u);
            out->target = (uint16_t)(out->next_ip + rel);
            return 1;
        }

        case 0xEB: {
            int8_t rel;
            if (!dr_have_bytes(program, ip, 2u)) return 0;
            rel = (int8_t)dr_u8(program, (uint16_t)(ip + 1u));
            out->kind = DR_JMP_REL8;
            out->next_ip = (uint16_t)(ip + 2u);
            out->target = (uint16_t)(out->next_ip + rel);
            return 1;
        }

        case 0xF4:
            out->kind = DR_HLT;
            out->next_ip = (uint16_t)(ip + 1u);
            return 1;

        default:
            return 0;
    }
}

static int dr_terminates_block(DrKind kind)
{
    return kind == DR_JZ_REL8 ||
           kind == DR_JNZ_REL8 ||
           kind == DR_RET ||
           kind == DR_INT ||
           kind == DR_CALL_REL16 ||
           kind == DR_JMP_REL16 ||
           kind == DR_JMP_REL8 ||
           kind == DR_HLT;
}

static int dr_reserve_insts(DrProgram *program)
{
    DrInst *next;
    size_t capacity;

    if (program->inst_count < program->inst_capacity) {
        return 1;
    }

    capacity = program->inst_capacity != 0u ? program->inst_capacity * 2u : 128u;
    next = (DrInst *)realloc(program->insts, capacity * sizeof(*next));
    if (next == NULL) {
        return 0;
    }

    program->insts = next;
    program->inst_capacity = capacity;
    return 1;
}

static int dr_reserve_blocks(DrProgram *program)
{
    DrBlock *next;
    size_t capacity;

    if (program->block_count < program->block_capacity) {
        return 1;
    }

    capacity = program->block_capacity != 0u ? program->block_capacity * 2u : 64u;
    next = (DrBlock *)realloc(program->blocks, capacity * sizeof(*next));
    if (next == NULL) {
        return 0;
    }

    program->blocks = next;
    program->block_capacity = capacity;
    return 1;
}

static void dr_queue(DrProgram *program, uint16_t ip)
{
    if (!dr_ip_in_code(program, ip) || program->queued[ip]) {
        return;
    }

    program->queued[ip] = 1u;
    program->queue[program->queue_tail++] = ip;
}

static void dr_mark_block_start(DrProgram *program, uint16_t ip)
{
    if (!dr_ip_in_code(program, ip)) {
        return;
    }

    program->is_block_start[ip] = 1u;
    dr_queue(program, ip);
}

static int dr_discover(DrProgram *program)
{
    uint32_t ip32;

    program->decoded = (DrInst *)calloc(DR_MAX_IP, sizeof(*program->decoded));
    if (program->decoded == NULL) {
        return 0;
    }

    dr_mark_block_start(program, program->code_start);

    /*
     * Phase 1: instruction reachability.
     *
     * This is intentionally separate from block construction. A backward branch
     * can discover that an address already scanned linearly is a block entry; if
     * blocks were built during scanning, that would create overlapping blocks.
     */
    while (program->queue_head < program->queue_tail) {
        const uint16_t ip = program->queue[program->queue_head++];
        DrInst inst;

        if (program->processed[ip]) {
            continue;
        }
        program->processed[ip] = 1u;

        if (!dr_decode(program, ip, &inst)) {
            program->fallback_at[ip] = 1u;
            continue;
        }

        program->decoded[ip] = inst;
        program->reachable[ip] = 1u;

        switch (inst.kind) {
            case DR_JZ_REL8:
            case DR_JNZ_REL8:
                dr_mark_block_start(program, inst.target);
                dr_mark_block_start(program, inst.next_ip);
                break;

            case DR_CALL_REL16:
                dr_mark_block_start(program, inst.target);
                dr_mark_block_start(program, inst.next_ip);
                break;

            case DR_JMP_REL16:
            case DR_JMP_REL8:
                dr_mark_block_start(program, inst.target);
                break;

            case DR_INT:
                /* An intercepted INT may return normally; an unclaimed INT may
                   dispatch through the IVT. The generated runtime decides which. */
                dr_mark_block_start(program, inst.next_ip);
                break;

            case DR_RET:
            case DR_HLT:
                break;

            default:
                dr_queue(program, inst.next_ip);
                break;
        }
    }

    /* Phase 2: form non-overlapping blocks from the reachable graph. */
    for (ip32 = program->code_start; ip32 < program->code_end; ++ip32) {
        const uint16_t start = (uint16_t)ip32;
        uint16_t ip = start;
        DrBlock block;

        if (!program->is_block_start[start]) {
            continue;
        }
        if (!program->reachable[start] && !program->fallback_at[start]) {
            continue;
        }

        memset(&block, 0, sizeof(block));
        block.start = start;
        block.first_inst = program->inst_count;

        while (dr_ip_in_code(program, ip)) {
            const DrInst *inst;

            if (ip != start && program->is_block_start[ip]) {
                break;
            }

            if (program->fallback_at[ip]) {
                block.has_fallback = 1;
                block.fallback_ip = ip;
                ++program->fallback_blocks;
                break;
            }

            if (!program->reachable[ip]) {
                break;
            }

            inst = &program->decoded[ip];
            if (!dr_reserve_insts(program)) {
                return 0;
            }

            program->insts[program->inst_count++] = *inst;
            ++block.inst_count;
            ip = inst->next_ip;

            if (dr_terminates_block(inst->kind)) {
                break;
            }
        }

        if (!dr_reserve_blocks(program)) {
            return 0;
        }
        program->blocks[program->block_count++] = block;
    }

    return 1;
}

static int dr_block_compare(const void *a, const void *b)
{
    const DrBlock *aa = (const DrBlock *)a;
    const DrBlock *bb = (const DrBlock *)b;

    if (aa->start < bb->start) return -1;
    if (aa->start > bb->start) return 1;
    return 0;
}

static const char *dr_kind_name(DrKind kind)
{
    switch (kind) {
        case DR_MOV_R8_IMM: return "mov r8,imm8";
        case DR_MOV_R16_IMM: return "mov r16,imm16";
        case DR_INC_R16: return "inc r16";
        case DR_DEC_R16: return "dec r16";
        case DR_PUSH_R16: return "push r16";
        case DR_POP_R16: return "pop r16";
        case DR_ADD_AL_IMM: return "add al,imm8";
        case DR_ADD_AX_IMM: return "add ax,imm16";
        case DR_SUB_AL_IMM: return "sub al,imm8";
        case DR_SUB_AX_IMM: return "sub ax,imm16";
        case DR_CMP_AL_IMM: return "cmp al,imm8";
        case DR_CMP_AX_IMM: return "cmp ax,imm16";
        case DR_JZ_REL8: return "jz rel8";
        case DR_JNZ_REL8: return "jnz rel8";
        case DR_NOP: return "nop";
        case DR_MOV_AL_MOFFS: return "mov al,[moffs]";
        case DR_MOV_AX_MOFFS: return "mov ax,[moffs]";
        case DR_MOV_MOFFS_AL: return "mov [moffs],al";
        case DR_MOV_MOFFS_AX: return "mov [moffs],ax";
        case DR_RET: return "ret";
        case DR_INT: return "int imm8";
        case DR_CALL_REL16: return "call rel16";
        case DR_JMP_REL16: return "jmp rel16";
        case DR_JMP_REL8: return "jmp rel8";
        case DR_HLT: return "hlt";
        default: return "invalid";
    }
}

static void dr_dump(const DrProgram *program)
{
    size_t b;

    for (b = 0u; b < program->block_count; ++b) {
        const DrBlock *block = &program->blocks[b];
        size_t i;

        printf("block %04X", block->start);
        if (block->has_fallback) {
            printf(" fallback=%04X", block->fallback_ip);
        }
        putchar('\n');

        for (i = 0u; i < block->inst_count; ++i) {
            const DrInst *inst = &program->insts[block->first_inst + i];
            printf("  %04X  %-16s", inst->ip, dr_kind_name(inst->kind));

            if (inst->kind == DR_JZ_REL8 ||
                inst->kind == DR_JNZ_REL8 ||
                inst->kind == DR_CALL_REL16 ||
                inst->kind == DR_JMP_REL16 ||
                inst->kind == DR_JMP_REL8) {
                printf(" -> %04X", inst->target);
            }

            putchar('\n');
        }
    }
}

static const char *dr_basename(const char *path)
{
    const char *base = path;
    const char *cursor;

    for (cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }

    return base;
}

static void dr_make_guard(const char *symbol, char *guard, size_t guard_size)
{
    const char *prefix = "MICRODOS_RECOMP_";
    size_t n = 0u;
    size_t i;

    while (*prefix != '\0' && n + 1u < guard_size) {
        guard[n++] = *prefix++;
    }

    for (i = 0u; symbol[i] != '\0' && n + 2u < guard_size; ++i) {
        char ch = symbol[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            ch = '_';
        }
        guard[n++] = ch;
    }

    if (n + 2u < guard_size) {
        guard[n++] = '_';
        guard[n++] = 'H';
    }
    guard[n] = '\0';
}

static int dr_emit_header(const DrOptions *opt)
{
    FILE *file = fopen(opt->output_h, "wb");
    char guard[256];

    if (file == NULL) {
        return 0;
    }

    dr_make_guard(opt->symbol, guard, sizeof(guard));
    fprintf(file,
            "#ifndef %s\n"
            "#define %s\n"
            "\n"
            "#include <stdint.h>\n"
            "#include \"microdos/runtime.h\"\n"
            "\n"
            "#ifdef __cplusplus\n"
            "extern \"C\" {\n"
            "#endif\n"
            "\n"
            "MdStopReason %s(MdRuntime *runtime, uint16_t segment, "
            "uint64_t instruction_budget);\n"
            "\n"
            "#ifdef __cplusplus\n"
            "}\n"
            "#endif\n"
            "#endif\n",
            guard,
            guard,
            opt->symbol);

    return fclose(file) == 0;
}

static void dr_emit_tick(FILE *file, uint16_t next_ip)
{
    fprintf(file, "    MD_AOT_TICK(0x%04Xu);\n", next_ip);
}

static void dr_emit_inst(FILE *file, const DrInst *inst)
{
    fprintf(file, "    /* %04X: %s */\n", inst->ip, dr_kind_name(inst->kind));
    dr_emit_tick(file, inst->next_ip);

    switch (inst->kind) {
        case DR_MOV_R8_IMM:
            fprintf(file,
                    "    md_x86_set_reg8(cpu, %uu, 0x%02Xu);\n",
                    (unsigned)inst->reg,
                    (unsigned)inst->imm8);
            break;

        case DR_MOV_R16_IMM:
            fprintf(file,
                    "    cpu->r[%uu] = 0x%04Xu;\n",
                    (unsigned)inst->reg,
                    (unsigned)inst->imm16);
            break;

        case DR_INC_R16:
            fprintf(file,
                    "    { const uint16_t cf = cpu->flags & MD_X86_FLAG_CF; "
                    "cpu->r[%uu] = md_x86_add16(cpu, cpu->r[%uu], 1u); "
                    "cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | cf); }\n",
                    (unsigned)inst->reg,
                    (unsigned)inst->reg);
            break;

        case DR_DEC_R16:
            fprintf(file,
                    "    { const uint16_t cf = cpu->flags & MD_X86_FLAG_CF; "
                    "cpu->r[%uu] = md_x86_sub16(cpu, cpu->r[%uu], 1u); "
                    "cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | cf); }\n",
                    (unsigned)inst->reg,
                    (unsigned)inst->reg);
            break;

        case DR_PUSH_R16:
            fprintf(file,
                    "    md_x86_push(cpu, cpu->r[%uu]);\n",
                    (unsigned)inst->reg);
            break;

        case DR_POP_R16:
            fprintf(file,
                    "    cpu->r[%uu] = md_x86_pop(cpu);\n",
                    (unsigned)inst->reg);
            break;

        case DR_ADD_AL_IMM:
            fprintf(file,
                    "    md_x86_set_reg8(cpu, 0u, md_x86_add8(cpu, "
                    "md_x86_get_reg8(cpu, 0u), 0x%02Xu));\n",
                    (unsigned)inst->imm8);
            break;

        case DR_ADD_AX_IMM:
            fprintf(file,
                    "    cpu->r[MD_X86_AX] = md_x86_add16(cpu, "
                    "cpu->r[MD_X86_AX], 0x%04Xu);\n",
                    (unsigned)inst->imm16);
            break;

        case DR_SUB_AL_IMM:
            fprintf(file,
                    "    md_x86_set_reg8(cpu, 0u, md_x86_sub8(cpu, "
                    "md_x86_get_reg8(cpu, 0u), 0x%02Xu));\n",
                    (unsigned)inst->imm8);
            break;

        case DR_SUB_AX_IMM:
            fprintf(file,
                    "    cpu->r[MD_X86_AX] = md_x86_sub16(cpu, "
                    "cpu->r[MD_X86_AX], 0x%04Xu);\n",
                    (unsigned)inst->imm16);
            break;

        case DR_CMP_AL_IMM:
            fprintf(file,
                    "    (void)md_x86_sub8(cpu, md_x86_get_reg8(cpu, 0u), "
                    "0x%02Xu);\n",
                    (unsigned)inst->imm8);
            break;

        case DR_CMP_AX_IMM:
            fprintf(file,
                    "    (void)md_x86_sub16(cpu, cpu->r[MD_X86_AX], "
                    "0x%04Xu);\n",
                    (unsigned)inst->imm16);
            break;

        case DR_JZ_REL8:
            fprintf(file,
                    "    if ((cpu->flags & MD_X86_FLAG_ZF) != 0u) "
                    "cpu->ip = 0x%04Xu;\n"
                    "    goto md_dispatch;\n",
                    inst->target);
            break;

        case DR_JNZ_REL8:
            fprintf(file,
                    "    if ((cpu->flags & MD_X86_FLAG_ZF) == 0u) "
                    "cpu->ip = 0x%04Xu;\n"
                    "    goto md_dispatch;\n",
                    inst->target);
            break;

        case DR_NOP:
            break;

        case DR_MOV_AL_MOFFS:
            fprintf(file,
                    "    md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, cpu->ds, "
                    "0x%04Xu));\n",
                    inst->imm16);
            break;

        case DR_MOV_AX_MOFFS:
            fprintf(file,
                    "    cpu->r[MD_X86_AX] = md_x86_read16(cpu, cpu->ds, "
                    "0x%04Xu);\n",
                    inst->imm16);
            break;

        case DR_MOV_MOFFS_AL:
            fprintf(file,
                    "    md_x86_write8(cpu, cpu->ds, 0x%04Xu, "
                    "md_x86_get_reg8(cpu, 0u));\n",
                    inst->imm16);
            break;

        case DR_MOV_MOFFS_AX:
            fprintf(file,
                    "    md_x86_write16(cpu, cpu->ds, 0x%04Xu, "
                    "cpu->r[MD_X86_AX]);\n",
                    inst->imm16);
            break;

        case DR_RET:
            fprintf(file,
                    "    cpu->ip = md_x86_pop(cpu);\n"
                    "    goto md_dispatch;\n");
            break;

        case DR_INT:
            fprintf(file,
                    "    (void)md_runtime_interrupt(runtime, 0x%02Xu);\n"
                    "    if (runtime->stop_reason != MD_STOP_NONE) "
                    "return runtime->stop_reason;\n"
                    "    goto md_dispatch;\n",
                    (unsigned)inst->imm8);
            break;

        case DR_CALL_REL16:
            fprintf(file,
                    "    md_x86_push(cpu, 0x%04Xu);\n"
                    "    cpu->ip = 0x%04Xu;\n"
                    "    goto md_dispatch;\n",
                    inst->next_ip,
                    inst->target);
            break;

        case DR_JMP_REL16:
        case DR_JMP_REL8:
            fprintf(file,
                    "    cpu->ip = 0x%04Xu;\n"
                    "    goto md_dispatch;\n",
                    inst->target);
            break;

        case DR_HLT:
            fprintf(file,
                    "    runtime->stop_reason = MD_STOP_HALT;\n"
                    "    return MD_STOP_HALT;\n");
            break;

        default:
            break;
    }
}

static int dr_emit_c(const DrProgram *program, const DrOptions *opt)
{
    FILE *file = fopen(opt->output_c, "wb");
    size_t i;
    size_t b;

    if (file == NULL) {
        return 0;
    }

    fprintf(file,
            "/* Generated by dosrecomp. Do not hand-edit. */\n"
            "#include \"microdos/ops.h\"\n"
            "#include \"microdos/runtime.h\"\n"
            "#include \"%s\"\n"
            "\n",
            dr_basename(opt->output_h));

    fprintf(file,
            "static const uint8_t md_image[%lu] = {\n",
            (unsigned long)program->image_size);

    for (i = 0u; i < program->image_size; ++i) {
        if ((i % 12u) == 0u) {
            fprintf(file, "    ");
        }

        fprintf(file,
                "0x%02Xu%s",
                (unsigned)program->image[i],
                i + 1u == program->image_size ? "" : ", ");

        if ((i % 12u) == 11u || i + 1u == program->image_size) {
            fputc('\n', file);
        }
    }

    fprintf(file,
            "};\n"
            "\n"
            "MdStopReason %s(MdRuntime *runtime, uint16_t segment, "
            "uint64_t instruction_budget)\n"
            "{\n"
            "    MdX86 *cpu = &runtime->cpu;\n"
            "    uint64_t remaining = instruction_budget;\n"
            "\n"
            "    md_runtime_load_com(runtime, md_image, sizeof(md_image), segment);\n"
            "    cpu->ip = 0x%04Xu;\n"
            "\n"
            "#define MD_AOT_TICK(next_ip_) do { \\\n"
            "        if (remaining == 0u) { runtime->stop_reason = MD_STOP_BUDGET; return MD_STOP_BUDGET; } \\\n"
            "        --remaining; \\\n"
            "        ++runtime->instructions; \\\n"
            "        cpu->ip = (uint16_t)(next_ip_); \\\n"
            "    } while (0)\n"
            "\n"
            "    goto md_dispatch;\n"
            "\n"
            "md_dispatch:\n"
            "    if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason;\n"
            "    if (cpu->cs != segment) goto md_fallback;\n"
            "    switch (cpu->ip) {\n",
            opt->symbol,
            program->code_start);

    for (b = 0u; b < program->block_count; ++b) {
        fprintf(file,
                "        case 0x%04Xu: goto md_block_%04X;\n",
                program->blocks[b].start,
                program->blocks[b].start);
    }

    fprintf(file,
            "        default: goto md_fallback;\n"
            "    }\n"
            "\n"
            "md_fallback:\n"
            "    if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason;\n"
            "    if (remaining == 0u) { runtime->stop_reason = MD_STOP_BUDGET; return MD_STOP_BUDGET; }\n"
            "    --remaining;\n"
            "    (void)md_interp_step(runtime);\n"
            "    if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason;\n"
            "    goto md_dispatch;\n"
            "\n");

    for (b = 0u; b < program->block_count; ++b) {
        const DrBlock *block = &program->blocks[b];

        fprintf(file, "md_block_%04X:\n", block->start);

        for (i = 0u; i < block->inst_count; ++i) {
            dr_emit_inst(file, &program->insts[block->first_inst + i]);
        }

        if (block->has_fallback) {
            fprintf(file,
                    "    /* %04X: not statically decoded; enter the shared interpreter. */\n"
                    "    cpu->ip = 0x%04Xu;\n"
                    "    goto md_fallback;\n",
                    block->fallback_ip,
                    block->fallback_ip);
        } else if (block->inst_count == 0u ||
                   !dr_terminates_block(
                       program->insts[block->first_inst + block->inst_count - 1u].kind)) {
            fprintf(file, "    goto md_dispatch;\n");
        }

        fputc('\n', file);
    }

    fprintf(file, "#undef MD_AOT_TICK\n}\n");
    return fclose(file) == 0;
}

int main(int argc, char **argv)
{
    DrOptions opt;
    DrProgram program;
    uint32_t image_end;
    int result = 1;

    if (!dr_parse_options(argc, argv, &opt) || !dr_valid_symbol(opt.symbol)) {
        dr_usage(argv[0]);
        return 2;
    }

    memset(&program, 0, sizeof(program));
    program.image = dr_read_file(opt.input, &program.image_size);
    if (program.image == NULL) {
        fprintf(stderr,
                "dosrecomp: could not read '%s' (COM images must be <= 65280 bytes)\n",
                opt.input);
        return 1;
    }

    image_end = DR_COM_BASE + (uint32_t)program.image_size;
    program.code_start = opt.code_start;
    program.code_end = opt.code_end_set ? opt.code_end : image_end;

    if ((uint32_t)program.code_start < DR_COM_BASE ||
        (uint32_t)program.code_start >= image_end ||
        program.code_end <= program.code_start ||
        program.code_end > image_end) {
        fprintf(stderr,
                "dosrecomp: invalid code range %04X..%04lX for %lu-byte COM image\n",
                program.code_start,
                (unsigned long)program.code_end,
                (unsigned long)program.image_size);
        goto done;
    }

    if (!dr_discover(&program)) {
        fprintf(stderr, "dosrecomp: out of memory while discovering control flow\n");
        goto done;
    }

    qsort(program.blocks,
          program.block_count,
          sizeof(*program.blocks),
          dr_block_compare);

    if (opt.dump) {
        dr_dump(&program);
    }

    if (!dr_emit_header(&opt) || !dr_emit_c(&program, &opt)) {
        fprintf(stderr, "dosrecomp: could not write generated output\n");
        goto done;
    }

    printf("dosrecomp: %s: %lu bytes, %lu blocks, %lu AOT instructions, "
           "%lu fallback block%s\n",
           opt.input,
           (unsigned long)program.image_size,
           (unsigned long)program.block_count,
           (unsigned long)program.inst_count,
           (unsigned long)program.fallback_blocks,
           program.fallback_blocks == 1u ? "" : "s");
    result = 0;

done:
    free(program.image);
    free(program.decoded);
    free(program.insts);
    free(program.blocks);
    return result;
}
