#include "microdos/decode.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DP_ADDRESS_SPACE 65536u
#define DP_DEFAULT_TOP 20u
#define DP_MAX_EXAMPLES 4u

typedef struct DpOptions {
    const char *input;
    const char *json;
    const char *label;
    uint16_t base;
    uint16_t entry;
    unsigned top;
    int base_set;
    int entry_set;
    int selftest;
} DpOptions;

typedef struct DpOpcodeStat {
    uint64_t count;
    uint16_t examples[DP_MAX_EXAMPLES];
    unsigned example_count;
} DpOpcodeStat;

typedef struct DpStats {
    uint64_t instructions;
    uint64_t reachable_bytes;
    uint64_t interp_supported;
    uint64_t aot_supported;
    uint64_t prefixed;
    uint64_t non_8086;
    uint64_t decode_errors;
    uint64_t conditional;
    uint64_t direct_calls;
    uint64_t direct_jumps;
    uint64_t indirect_calls;
    uint64_t indirect_jumps;
    uint64_t returns;
    uint64_t stops;
    DpOpcodeStat opcode[256];
    DpOpcodeStat unsupported[256];
    uint64_t prefixes[256];
    uint64_t int_vector[256];       /* reachable CD imm8 only */
} DpStats;

typedef struct DpScan {
    uint8_t *image;
    size_t image_size;
    uint16_t base;
    uint16_t entry;
    uint8_t seen_ip[DP_ADDRESS_SPACE];
    uint8_t queued[DP_ADDRESS_SPACE];
    uint8_t *seen_byte;
    uint16_t queue[DP_ADDRESS_SPACE];
    size_t queue_head;
    size_t queue_tail;
    DpStats stats;
} DpScan;

static void dp_usage(const char *exe)
{
    fprintf(stderr,
            "usage: %s --input file --base N --entry N [options]\n"
            "       %s --selftest\n\n"
            "options:\n"
            "  --base N       guest offset corresponding to byte 0 (COM: 0x100)\n"
            "  --entry N      first recursive-descent entry point\n"
            "  --label text   display label (defaults to input path)\n"
            "  --json path    also write a machine-readable report\n"
            "  --top N        unsupported-opcode rows to print (default 20)\n",
            exe, exe);
}

static int dp_parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > 0xFFFFul) return 0;
    *out = (uint32_t)value;
    return 1;
}

static int dp_parse_options(int argc, char **argv, DpOptions *opt)
{
    int i;
    memset(opt, 0, sizeof(*opt));
    opt->base = 0x0100u;
    opt->top = DP_DEFAULT_TOP;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            opt->input = argv[++i];
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            uint32_t v;
            if (!dp_parse_u32(argv[++i], &v)) return 0;
            opt->base = (uint16_t)v;
            opt->base_set = 1;
        } else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
            uint32_t v;
            if (!dp_parse_u32(argv[++i], &v)) return 0;
            opt->entry = (uint16_t)v;
            opt->entry_set = 1;
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            opt->json = argv[++i];
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            opt->label = argv[++i];
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            uint32_t v;
            if (!dp_parse_u32(argv[++i], &v) || v == 0u || v > 256u) return 0;
            opt->top = (unsigned)v;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            opt->selftest = 1;
        } else {
            return 0;
        }
    }

    if (opt->selftest) return argc == 2;
    if (opt->input == NULL) return 0;
    if (!opt->entry_set) opt->entry = opt->base;
    return 1;
}

static uint8_t *dp_read_file(const char *path, size_t *size_out)
{
    FILE *file;
    long length;
    uint8_t *data;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    length = ftell(file);
    if (length <= 0 || (unsigned long)length > DP_ADDRESS_SPACE) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL) { fclose(file); return NULL; }
    got = fread(data, 1u, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) { free(data); return NULL; }
    *size_out = got;
    return data;
}

static int dp_ip_in_image(const DpScan *scan, uint16_t ip)
{
    const uint32_t begin = scan->base;
    const uint32_t end = begin + (uint32_t)scan->image_size;
    const uint32_t address = ip;
    return end <= 0x10000u && address >= begin && address < end;
}

static void dp_record(DpOpcodeStat *stat, uint16_t ip)
{
    ++stat->count;
    if (stat->example_count < DP_MAX_EXAMPLES) {
        stat->examples[stat->example_count++] = ip;
    }
}

static int dp_enqueue(DpScan *scan, uint16_t ip)
{
    if (!dp_ip_in_image(scan, ip)) return 1;
    if (scan->seen_ip[ip] || scan->queued[ip]) return 1;
    if (scan->queue_tail >= DP_ADDRESS_SPACE) return 0;
    scan->queued[ip] = 1u;
    scan->queue[scan->queue_tail++] = ip;
    return 1;
}

static int dp_scan(DpScan *scan)
{
    if (!dp_enqueue(scan, scan->entry)) return 0;

    while (scan->queue_head < scan->queue_tail) {
        uint16_t ip = scan->queue[scan->queue_head++];
        scan->queued[ip] = 0u;

        while (dp_ip_in_image(scan, ip) && !scan->seen_ip[ip]) {
            MdDecodedInstruction inst;
            size_t offset;
            unsigned i;

            if (!md_decode_8086(scan->image, scan->image_size, scan->base, ip, &inst)) {
                ++scan->stats.decode_errors;
                break;
            }

            scan->seen_ip[ip] = 1u;
            ++scan->stats.instructions;
            dp_record(&scan->stats.opcode[inst.opcode], ip);
            if (!md_decode_interp_supported(&inst)) dp_record(&scan->stats.unsupported[inst.opcode], ip);
            else ++scan->stats.interp_supported;
            if (md_decode_aot_supported(&inst)) ++scan->stats.aot_supported;
            if (!inst.valid_8086) ++scan->stats.non_8086;
            if (inst.prefix_count != 0u) ++scan->stats.prefixed;
            for (i = 0u; i < inst.prefix_count && i < sizeof(inst.prefixes); ++i) {
                ++scan->stats.prefixes[inst.prefixes[i]];
            }

            offset = (size_t)((uint32_t)ip - scan->base);
            if (inst.opcode == 0xCDu &&
                offset + inst.prefix_count + 1u < scan->image_size) {
                const uint8_t vector = scan->image[offset + inst.prefix_count + 1u];
                ++scan->stats.int_vector[vector];
            }
            for (i = 0u; i < inst.length && offset + i < scan->image_size; ++i) {
                if (!scan->seen_byte[offset + i]) {
                    scan->seen_byte[offset + i] = 1u;
                    ++scan->stats.reachable_bytes;
                }
            }

            switch (inst.flow) {
                case MD_DECODE_FLOW_CONDITIONAL:
                    ++scan->stats.conditional;
                    if (!dp_enqueue(scan, inst.target)) return 0;
                    ip = inst.next_ip;
                    break;
                case MD_DECODE_FLOW_CALL:
                    ++scan->stats.direct_calls;
                    if (!dp_enqueue(scan, inst.target)) return 0;
                    ip = inst.next_ip;
                    break;
                case MD_DECODE_FLOW_JUMP:
                    ++scan->stats.direct_jumps;
                    if (!dp_enqueue(scan, inst.target)) return 0;
                    ip = 0u; /* terminate this linear walk */
                    goto next_seed;
                case MD_DECODE_FLOW_INDIRECT_CALL:
                    ++scan->stats.indirect_calls;
                    ip = inst.next_ip;
                    break;
                case MD_DECODE_FLOW_INDIRECT_JUMP:
                    ++scan->stats.indirect_jumps;
                    goto next_seed;
                case MD_DECODE_FLOW_RETURN:
                    ++scan->stats.returns;
                    goto next_seed;
                case MD_DECODE_FLOW_STOP:
                    ++scan->stats.stops;
                    goto next_seed;
                case MD_DECODE_FLOW_FALLTHROUGH:
                default:
                    ip = inst.next_ip;
                    break;
            }
        }
next_seed:
        ;
    }
    return 1;
}

static const char *dp_opcode_name(uint8_t op)
{
    if (op <= 0x03u) return "ADD r/m,reg";
    if (op >= 0x08u && op <= 0x0Bu) return "OR r/m,reg";
    if (op >= 0x10u && op <= 0x13u) return "ADC r/m,reg";
    if (op >= 0x18u && op <= 0x1Bu) return "SBB r/m,reg";
    if (op >= 0x20u && op <= 0x23u) return "AND r/m,reg";
    if (op >= 0x28u && op <= 0x2Bu) return "SUB r/m,reg";
    if (op >= 0x30u && op <= 0x33u) return "XOR r/m,reg";
    if (op >= 0x38u && op <= 0x3Bu) return "CMP r/m,reg";
    if (op >= 0x60u && op <= 0x7Fu) return "Jcc rel8";
    if (op == 0x80u || op == 0x81u || op == 0x82u || op == 0x83u) return "ALU r/m,imm";
    if (op >= 0x88u && op <= 0x8Bu) return "MOV r/m,reg";
    if (op == 0x8Cu) return "MOV r/m,Sreg";
    if (op == 0x8Du) return "LEA";
    if (op == 0x8Eu) return "MOV Sreg,r/m";
    if (op == 0x87u) return "XCHG r/m,reg";
    if (op == 0x8Fu) return "POP r/m16";
    if (op == 0xA8u || op == 0xA9u) return "TEST accumulator,imm";
    if (op == 0xA4u || op == 0xA5u) return "MOVS";
    if (op == 0xA6u || op == 0xA7u) return "CMPS";
    if (op == 0xAAu || op == 0xABu) return "STOS";
    if (op == 0xACu || op == 0xADu) return "LODS";
    if (op == 0xAEu || op == 0xAFu) return "SCAS";
    if (op == 0xC4u) return "LES";
    if (op == 0xC5u) return "LDS";
    if (op == 0xC6u || op == 0xC7u) return "MOV r/m,imm";
    if (op >= 0xD0u && op <= 0xD3u) return "shift/rotate";
    if (op >= 0xE0u && op <= 0xE2u) return "LOOPcc/LOOP";
    if (op == 0xE3u) return "JCXZ";
    if (op == 0xF6u || op == 0xF7u) return "group3";
    if (op == 0xFEu) return "INC/DEC r/m8";
    if (op == 0xFFu) return "group5 r/m16";
    switch (op) {
        case 0x06u: return "PUSH ES";
        case 0x07u: return "POP ES";
        case 0x0Eu: return "PUSH CS";
        case 0x16u: return "PUSH SS";
        case 0x17u: return "POP SS";
        case 0x1Eu: return "PUSH DS";
        case 0x1Fu: return "POP DS";
        case 0x98u: return "CBW";
        case 0x99u: return "CWD";
        case 0x9Cu: return "PUSHF";
        case 0x9Du: return "POPF";
        case 0x9Eu: return "SAHF";
        case 0x9Fu: return "LAHF";
        case 0xCFu: return "IRET";
        case 0xF8u: return "CLC";
        case 0xF9u: return "STC";
        case 0xFAu: return "CLI";
        case 0xFBu: return "STI";
        case 0xFCu: return "CLD";
        case 0xFDu: return "STD";
        default: return "other";
    }
}

typedef struct DpRank {
    uint8_t opcode;
    uint64_t count;
} DpRank;

static int dp_rank_compare(const void *a, const void *b)
{
    const DpRank *aa = (const DpRank *)a;
    const DpRank *bb = (const DpRank *)b;
    if (aa->count < bb->count) return 1;
    if (aa->count > bb->count) return -1;
    return (int)aa->opcode - (int)bb->opcode;
}

static size_t dp_build_rank(const DpStats *stats, DpRank rank[256])
{
    size_t n = 0u;
    unsigned op;
    for (op = 0u; op < 256u; ++op) {
        if (stats->unsupported[op].count != 0u) {
            rank[n].opcode = (uint8_t)op;
            rank[n].count = stats->unsupported[op].count;
            ++n;
        }
    }
    qsort(rank, n, sizeof(rank[0]), dp_rank_compare);
    return n;
}

static void dp_print_examples(FILE *file, const DpOpcodeStat *stat)
{
    unsigned i;
    for (i = 0u; i < stat->example_count; ++i) {
        fprintf(file, "%s%04X", i == 0u ? "" : ",", stat->examples[i]);
    }
}

static void dp_print_report(const DpScan *scan, const DpOptions *opt)
{
    const DpStats *s = &scan->stats;
    DpRank rank[256];
    size_t rank_count = dp_build_rank(s, rank);
    size_t show = rank_count < opt->top ? rank_count : opt->top;
    size_t i;
    const double coverage = scan->image_size != 0u
        ? (100.0 * (double)s->reachable_bytes / (double)scan->image_size) : 0.0;
    const double interp = s->instructions != 0u
        ? (100.0 * (double)s->interp_supported / (double)s->instructions) : 0.0;
    const double aot = s->instructions != 0u
        ? (100.0 * (double)s->aot_supported / (double)s->instructions) : 0.0;

    printf("dosprobe: %s\n", opt->label != NULL ? opt->label : opt->input);
    printf("  image:          %lu bytes @ %04X, entry %04X\n",
           (unsigned long)scan->image_size, scan->base, scan->entry);
    printf("  reachable:      %llu instructions, %llu bytes (%.2f%% of image)\n",
           (unsigned long long)s->instructions,
           (unsigned long long)s->reachable_bytes,
           coverage);
    printf("  current interp: %llu / %llu instructions (%.2f%%)\n",
           (unsigned long long)s->interp_supported,
           (unsigned long long)s->instructions,
           interp);
    printf("  current AOT:    %llu / %llu instructions (%.2f%%)\n",
           (unsigned long long)s->aot_supported,
           (unsigned long long)s->instructions,
           aot);
    printf("  prefixes:       %llu instructions\n", (unsigned long long)s->prefixed);
    printf("  structural:     decode_errors=%llu non8086=%llu indirect_calls=%llu indirect_jumps=%llu\n",
           (unsigned long long)s->decode_errors,
           (unsigned long long)s->non_8086,
           (unsigned long long)s->indirect_calls,
           (unsigned long long)s->indirect_jumps);
    printf("  flow:           conditional=%llu calls=%llu jumps=%llu returns=%llu stops=%llu\n",
           (unsigned long long)s->conditional,
           (unsigned long long)s->direct_calls,
           (unsigned long long)s->direct_jumps,
           (unsigned long long)s->returns,
           (unsigned long long)s->stops);

    {
        uint8_t taken_int[256];
        unsigned k, v, best;
        memset(taken_int, 0, sizeof(taken_int));
        printf("  reachable INT vectors:");
        for (k = 0u; k < 16u; ++k) {
            best = 256u;
            for (v = 0u; v < 256u; ++v)
                if (!taken_int[v] && s->int_vector[v] != 0u &&
                    (best == 256u || s->int_vector[v] > s->int_vector[best])) best = v;
            if (best == 256u) break;
            taken_int[best] = 1u;
            printf(" %02X:%llu", best, (unsigned long long)s->int_vector[best]);
        }
        printf("\n");
    }

    printf("\n  top unsupported reachable opcodes:\n");
    printf("    opcode  count   examples              class\n");
    for (i = 0u; i < show; ++i) {
        const uint8_t op = rank[i].opcode;
        printf("    %02X      %-7llu ", op,
               (unsigned long long)scan->stats.unsupported[op].count);
        dp_print_examples(stdout, &scan->stats.unsupported[op]);
        printf("%*s%s\n",
               scan->stats.unsupported[op].example_count < 4u ?
                   (int)(21u - scan->stats.unsupported[op].example_count * 5u) : 1,
               "",
               dp_opcode_name(op));
    }
}

static void dp_json_string(FILE *file, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', file);
    while (*p != 0u) {
        if (*p == '"' || *p == '\\') fprintf(file, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", file);
        else if (*p == '\r') fputs("\\r", file);
        else if (*p == '\t') fputs("\\t", file);
        else if (*p < 0x20u) fprintf(file, "\\u%04x", *p);
        else fputc(*p, file);
        ++p;
    }
    fputc('"', file);
}

static int dp_write_json(const DpScan *scan, const DpOptions *opt)
{
    const DpStats *s = &scan->stats;
    DpRank rank[256];
    size_t rank_count = dp_build_rank(s, rank);
    size_t show = rank_count < opt->top ? rank_count : opt->top;
    FILE *file;
    size_t i;

    if (opt->json == NULL) return 1;
    file = fopen(opt->json, "wb");
    if (file == NULL) return 0;

    fputs("{\n  \"label\": ", file);
    dp_json_string(file, opt->label != NULL ? opt->label : opt->input);
    fputs(",\n  \"input\": ", file);
    dp_json_string(file, opt->input);
    fprintf(file,
            ",\n  \"image_bytes\": %lu,\n  \"base\": %u,\n  \"entry\": %u,\n"
            "  \"reachable_instructions\": %llu,\n  \"reachable_bytes\": %llu,\n"
            "  \"interp_supported\": %llu,\n  \"aot_supported\": %llu,\n"
            "  \"prefixed_instructions\": %llu,\n  \"non_8086\": %llu,\n"
            "  \"decode_errors\": %llu,\n  \"flow\": {\n"
            "    \"conditional\": %llu, \"direct_calls\": %llu, \"direct_jumps\": %llu,\n"
            "    \"indirect_calls\": %llu, \"indirect_jumps\": %llu, \"returns\": %llu, \"stops\": %llu\n"
            "  },\n  \"interrupt_vectors\": {",
            (unsigned long)scan->image_size,
            (unsigned)scan->base,
            (unsigned)scan->entry,
            (unsigned long long)s->instructions,
            (unsigned long long)s->reachable_bytes,
            (unsigned long long)s->interp_supported,
            (unsigned long long)s->aot_supported,
            (unsigned long long)s->prefixed,
            (unsigned long long)s->non_8086,
            (unsigned long long)s->decode_errors,
            (unsigned long long)s->conditional,
            (unsigned long long)s->direct_calls,
            (unsigned long long)s->direct_jumps,
            (unsigned long long)s->indirect_calls,
            (unsigned long long)s->indirect_jumps,
            (unsigned long long)s->returns,
            (unsigned long long)s->stops);

    {
        unsigned v;
        int first = 1;
        for (v = 0u; v < 256u; ++v) {
            if (s->int_vector[v] == 0u) continue;
            fprintf(file, "%s\n    \"%02X\": %llu",
                    first ? "" : ",",
                    v, (unsigned long long)s->int_vector[v]);
            first = 0;
        }
        fputs(first ? "},\n  \"top_unsupported\": [\n"
                    : "\n  },\n  \"top_unsupported\": [\n", file);
    }

    for (i = 0u; i < show; ++i) {
        const uint8_t op = rank[i].opcode;
        const DpOpcodeStat *st = &s->unsupported[op];
        unsigned e;
        fprintf(file, "    {\"opcode\": \"%02X\", \"count\": %llu, \"class\": ",
                op, (unsigned long long)st->count);
        dp_json_string(file, dp_opcode_name(op));
        fputs(", \"examples\": [", file);
        for (e = 0u; e < st->example_count; ++e) {
            fprintf(file, "%s%u", e == 0u ? "" : ", ", (unsigned)st->examples[e]);
        }
        fprintf(file, "]}%s\n", i + 1u == show ? "" : ",");
    }
    fputs("  ]\n}\n", file);
    return fclose(file) == 0;
}

static int dp_expect_decode(const uint8_t *bytes,
                            size_t size,
                            uint16_t base,
                            uint16_t ip,
                            unsigned length,
                            uint8_t opcode,
                            MdDecodeFlow flow,
                            int valid)
{
    MdDecodedInstruction inst;
    if (!md_decode_8086(bytes, size, base, ip, &inst)) return 0;
    return inst.length == length && inst.opcode == opcode && inst.flow == flow &&
           (int)inst.valid_8086 == valid;
}

static int dp_selftest(void)
{
    static const uint8_t rep_movsb[] = {0xF3u, 0xA4u};
    static const uint8_t mov_bp[] = {0x8Bu, 0x86u, 0x34u, 0x12u};
    static const uint8_t grp1[] = {0x83u, 0x7Eu, 0xFEu, 0x00u};
    static const uint8_t kernel_jmp[] = {0xE9u, 0x78u, 0x3Eu};
    static const uint8_t indirect_jmp[] = {0xFFu, 0x26u, 0x34u, 0x12u};
    static const uint8_t mov_imm_mem[] = {0xC7u, 0x06u, 0x00u, 0x20u, 0x34u, 0x12u};
    static const uint8_t jz_back[] = {0x74u, 0xFEu};
    static const uint8_t grp3_neg[] = {0xF7u, 0xD8u};
    static const uint8_t shift_cl[] = {0xD3u, 0xE8u};
    static const uint8_t les_mem[] = {0xC4u, 0x06u, 0x34u, 0x12u};
    static const uint8_t jcc_alias_68[] = {0x68u, 0x34u};
    static uint8_t flow_image[] = {
        0xE9u, 0x03u, 0x00u,       /* jmp 0006 */
        0xDEu, 0xADu, 0xBEu,       /* unreachable data */
        0xB9u, 0x02u, 0x00u,       /* mov cx,2 */
        0x49u,                     /* dec cx */
        0x75u, 0xFDu,              /* jnz 0009 */
        0xF4u                      /* hlt */
    };
    MdDecodedInstruction inst;
    int ok = 1;

    ok &= dp_expect_decode(rep_movsb, sizeof(rep_movsb), 0x0100u, 0x0100u, 2u, 0xA4u,
                           MD_DECODE_FLOW_FALLTHROUGH, 1);
    ok &= md_decode_8086(rep_movsb, sizeof(rep_movsb), 0x0100u, 0x0100u, &inst) &&
          inst.prefix_count == 1u && inst.prefixes[0] == 0xF3u;
    ok &= md_decode_interp_supported(&inst) && !md_decode_aot_supported(&inst);
    ok &= dp_expect_decode(mov_bp, sizeof(mov_bp), 0x0100u, 0x0100u, 4u, 0x8Bu,
                           MD_DECODE_FLOW_FALLTHROUGH, 1);
    ok &= dp_expect_decode(grp1, sizeof(grp1), 0x0100u, 0x0100u, 4u, 0x83u,
                           MD_DECODE_FLOW_FALLTHROUGH, 1);
    ok &= md_decode_8086(grp1, sizeof(grp1), 0x0100u, 0x0100u, &inst) &&
          md_decode_interp_supported(&inst) && !md_decode_aot_supported(&inst);
    ok &= dp_expect_decode(kernel_jmp, sizeof(kernel_jmp), 0u, 0u, 3u, 0xE9u,
                           MD_DECODE_FLOW_JUMP, 1);
    ok &= md_decode_8086(kernel_jmp, sizeof(kernel_jmp), 0u, 0u, &inst) && inst.target == 0x3E7Bu;
    ok &= dp_expect_decode(indirect_jmp, sizeof(indirect_jmp), 0x0100u, 0x0100u, 4u, 0xFFu,
                           MD_DECODE_FLOW_INDIRECT_JUMP, 1);
    ok &= dp_expect_decode(mov_imm_mem, sizeof(mov_imm_mem), 0x0100u, 0x0100u, 6u, 0xC7u,
                           MD_DECODE_FLOW_FALLTHROUGH, 1);
    ok &= md_decode_8086(mov_imm_mem, sizeof(mov_imm_mem), 0x0100u, 0x0100u, &inst) &&
          md_decode_interp_supported(&inst) && !md_decode_aot_supported(&inst);
    ok &= dp_expect_decode(jz_back, sizeof(jz_back), 0x0100u, 0x0100u, 2u, 0x74u,
                           MD_DECODE_FLOW_CONDITIONAL, 1);
    ok &= md_decode_8086(jz_back, sizeof(jz_back), 0x0100u, 0x0100u, &inst) && inst.target == 0x0100u;
    ok &= md_decode_8086(grp3_neg, sizeof(grp3_neg), 0x0100u, 0x0100u, &inst) &&
          md_decode_interp_supported(&inst) && !md_decode_aot_supported(&inst);
    ok &= md_decode_8086(shift_cl, sizeof(shift_cl), 0x0100u, 0x0100u, &inst) &&
          md_decode_interp_supported(&inst) && !md_decode_aot_supported(&inst);
    ok &= md_decode_8086(les_mem, sizeof(les_mem), 0x0100u, 0x0100u, &inst) &&
          md_decode_interp_supported(&inst) && !md_decode_aot_supported(&inst);
    ok &= dp_expect_decode(jcc_alias_68, sizeof(jcc_alias_68), 0x0100u, 0x0100u, 2u, 0x68u,
                           MD_DECODE_FLOW_CONDITIONAL, 1);
    ok &= md_decode_8086(jcc_alias_68, sizeof(jcc_alias_68), 0x0100u, 0x0100u, &inst) &&
          inst.target == 0x0136u && md_decode_interp_supported(&inst);

    {
        DpScan scan;
        memset(&scan, 0, sizeof(scan));
        scan.image = flow_image;
        scan.image_size = sizeof(flow_image);
        scan.base = 0u;
        scan.entry = 0u;
        scan.seen_byte = (uint8_t *)calloc(scan.image_size, 1u);
        ok &= scan.seen_byte != NULL;
        if (scan.seen_byte != NULL) {
            ok &= dp_scan(&scan);
            ok &= scan.stats.instructions == 5u;
            ok &= scan.stats.reachable_bytes == 10u;
            ok &= scan.stats.interp_supported == 5u;
            ok &= scan.stats.aot_supported == 5u;
            ok &= scan.stats.conditional == 1u;
            ok &= scan.stats.direct_jumps == 1u;
            ok &= scan.stats.stops == 1u;
            free(scan.seen_byte);
        }
    }

    if (!ok) {
        fprintf(stderr, "dosprobe selftest failed\n");
        return 1;
    }
    puts("dosprobe selftest passed");
    return 0;
}

int main(int argc, char **argv)
{
    DpOptions opt;
    DpScan scan;
    uint32_t end;
    int result = 1;

    if (!dp_parse_options(argc, argv, &opt)) {
        dp_usage(argv[0]);
        return 2;
    }
    if (opt.selftest) return dp_selftest();

    memset(&scan, 0, sizeof(scan));
    scan.image = dp_read_file(opt.input, &scan.image_size);
    if (scan.image == NULL) {
        fprintf(stderr, "dosprobe: could not read '%s' or image is empty/too large\n", opt.input);
        return 1;
    }
    scan.base = opt.base;
    scan.entry = opt.entry;
    end = (uint32_t)scan.base + (uint32_t)scan.image_size;
    if (end > 0x10000u || !dp_ip_in_image(&scan, scan.entry)) {
        fprintf(stderr,
                "dosprobe: image range %04X..%05lX or entry %04X is outside 16-bit offset space\n",
                scan.base, (unsigned long)end, scan.entry);
        goto done;
    }

    scan.seen_byte = (uint8_t *)calloc(scan.image_size, 1u);
    if (scan.seen_byte == NULL) {
        fprintf(stderr, "dosprobe: out of memory\n");
        goto done;
    }

    if (!dp_scan(&scan)) {
        fprintf(stderr, "dosprobe: control-flow queue overflow\n");
        goto done;
    }

    dp_print_report(&scan, &opt);
    if (!dp_write_json(&scan, &opt)) {
        fprintf(stderr, "dosprobe: could not write JSON report '%s'\n", opt.json);
        goto done;
    }
    result = 0;

done:
    free(scan.seen_byte);
    free(scan.image);
    return result;
}
