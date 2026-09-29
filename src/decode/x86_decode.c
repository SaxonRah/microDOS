#include "microdos/decode.h"

#include <string.h>

typedef struct MdDecodeCursor {
    const uint8_t *image;
    size_t size;
    size_t pos;
} MdDecodeCursor;

static int md_take(MdDecodeCursor *c, unsigned count)
{
    if (c->pos + count > c->size) return 0;
    c->pos += count;
    return 1;
}

static int md_is_prefix(uint8_t opcode)
{
    switch (opcode) {
        case 0x26u: /* ES */
        case 0x2Eu: /* CS */
        case 0x36u: /* SS */
        case 0x3Eu: /* DS */
        case 0xF0u: /* LOCK */
        case 0xF2u: /* REPNE */
        case 0xF3u: /* REP/REPE */
            return 1;
        default:
            return 0;
    }
}

static int md_take_modrm(MdDecodeCursor *c, MdDecodedInstruction *out)
{
    uint8_t modrm;
    unsigned mod;
    unsigned rm;
    unsigned displacement = 0u;

    if (c->pos >= c->size) return 0;
    modrm = c->image[c->pos++];
    mod = modrm >> 6;
    rm = modrm & 7u;

    out->has_modrm = 1u;
    out->modrm = modrm;

    if (mod == 0u && rm == 6u) displacement = 2u;
    else if (mod == 1u) displacement = 1u;
    else if (mod == 2u) displacement = 2u;

    return md_take(c, displacement);
}

static int md_take_modrm_imm(MdDecodeCursor *c,
                             MdDecodedInstruction *out,
                             unsigned immediate)
{
    return md_take_modrm(c, out) && md_take(c, immediate);
}

static int md_in_image(size_t image_size, uint16_t image_base, uint16_t ip, size_t *offset)
{
    const uint32_t begin = image_base;
    const uint32_t end = begin + (uint32_t)image_size;
    const uint32_t address = ip;
    if (end > 0x10000u || address < begin || address >= end) return 0;
    *offset = (size_t)(address - begin);
    return 1;
}

bool md_decode_8086(const uint8_t *image,
                    size_t image_size,
                    uint16_t image_base,
                    uint16_t ip,
                    MdDecodedInstruction *out)
{
    MdDecodeCursor c;
    size_t offset;
    uint8_t opcode;

    if (image == NULL || out == NULL ||
        !md_in_image(image_size, image_base, ip, &offset)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->ip = ip;
    out->valid_8086 = 1u;
    out->flow = MD_DECODE_FLOW_FALLTHROUGH;

    c.image = image;
    c.size = image_size;
    c.pos = offset;

    while (c.pos < c.size && md_is_prefix(c.image[c.pos])) {
        if (out->prefix_count < sizeof(out->prefixes)) {
            out->prefixes[out->prefix_count] = c.image[c.pos];
        }
        ++out->prefix_count;
        ++c.pos;
        if (out->prefix_count >= 15u) return false;
    }

    if (c.pos >= c.size) return false;
    opcode = c.image[c.pos++];
    out->opcode = opcode;

    /* ALU r/m,reg and reg,r/m families. */
    if ((opcode <= 0x03u) ||
        (opcode >= 0x08u && opcode <= 0x0Bu) ||
        (opcode >= 0x10u && opcode <= 0x13u) ||
        (opcode >= 0x18u && opcode <= 0x1Bu) ||
        (opcode >= 0x20u && opcode <= 0x23u) ||
        (opcode >= 0x28u && opcode <= 0x2Bu) ||
        (opcode >= 0x30u && opcode <= 0x33u) ||
        (opcode >= 0x38u && opcode <= 0x3Bu)) {
        if (!md_take_modrm(&c, out)) return false;
        goto done;
    }

    /* AL/AX immediate ALU families. */
    if (opcode == 0x04u || opcode == 0x0Cu || opcode == 0x14u ||
        opcode == 0x1Cu || opcode == 0x24u || opcode == 0x2Cu ||
        opcode == 0x34u || opcode == 0x3Cu || opcode == 0xA8u) {
        if (!md_take(&c, 1u)) return false;
        goto done;
    }
    if (opcode == 0x05u || opcode == 0x0Du || opcode == 0x15u ||
        opcode == 0x1Du || opcode == 0x25u || opcode == 0x2Du ||
        opcode == 0x35u || opcode == 0x3Du || opcode == 0xA9u) {
        if (!md_take(&c, 2u)) return false;
        goto done;
    }

    if ((opcode >= 0x40u && opcode <= 0x5Fu) ||
        opcode == 0x06u || opcode == 0x07u || opcode == 0x0Eu ||
        opcode == 0x0Fu || opcode == 0x16u || opcode == 0x17u ||
        opcode == 0x1Eu || opcode == 0x1Fu || opcode == 0x27u ||
        opcode == 0x2Fu || opcode == 0x37u || opcode == 0x3Fu) {
        goto done;
    }

    /* 80186+ encodings. Decode their length so a suspicious path can still be
       inspected, but flag them as not valid 8086 instructions. */
    if (opcode >= 0x60u && opcode <= 0x6Fu) {
        out->valid_8086 = 0u;
        switch (opcode) {
            case 0x62u:
            case 0x63u:
                if (!md_take_modrm(&c, out)) return false;
                break;
            case 0x68u:
                if (!md_take(&c, 2u)) return false;
                break;
            case 0x69u:
                if (!md_take_modrm_imm(&c, out, 2u)) return false;
                break;
            case 0x6Au:
                if (!md_take(&c, 1u)) return false;
                break;
            case 0x6Bu:
                if (!md_take_modrm_imm(&c, out, 1u)) return false;
                break;
            default:
                break;
        }
        goto done;
    }

    if (opcode >= 0x70u && opcode <= 0x7Fu) {
        int8_t rel;
        if (c.pos >= c.size) return false;
        rel = (int8_t)c.image[c.pos++];
        out->flow = MD_DECODE_FLOW_CONDITIONAL;
        out->target = (uint16_t)((uint16_t)(image_base + (uint16_t)c.pos) + rel);
        goto done;
    }

    if (opcode == 0x80u || opcode == 0x82u || opcode == 0x83u) {
        if (!md_take_modrm_imm(&c, out, 1u)) return false;
        goto done;
    }
    if (opcode == 0x81u) {
        if (!md_take_modrm_imm(&c, out, 2u)) return false;
        goto done;
    }

    if ((opcode >= 0x84u && opcode <= 0x8Fu) ||
        (opcode >= 0xD0u && opcode <= 0xD3u) ||
        (opcode >= 0xD8u && opcode <= 0xDFu)) {
        if (!md_take_modrm(&c, out)) return false;
        goto done;
    }

    if (opcode >= 0x90u && opcode <= 0x99u) goto done;

    if (opcode == 0x9Au) {
        if (!md_take(&c, 4u)) return false;
        out->flow = MD_DECODE_FLOW_INDIRECT_CALL;
        out->far_control = 1u;
        goto done;
    }
    if (opcode >= 0x9Bu && opcode <= 0x9Fu) goto done;

    if (opcode >= 0xA0u && opcode <= 0xA3u) {
        if (!md_take(&c, 2u)) return false;
        goto done;
    }
    if ((opcode >= 0xA4u && opcode <= 0xA7u) ||
        (opcode >= 0xAAu && opcode <= 0xAFu)) goto done;

    if (opcode >= 0xB0u && opcode <= 0xB7u) {
        if (!md_take(&c, 1u)) return false;
        goto done;
    }
    if (opcode >= 0xB8u && opcode <= 0xBFu) {
        if (!md_take(&c, 2u)) return false;
        goto done;
    }

    if (opcode == 0xC0u || opcode == 0xC1u) {
        out->valid_8086 = 0u;
        if (!md_take_modrm_imm(&c, out, 1u)) return false;
        goto done;
    }
    if (opcode == 0xC2u || opcode == 0xCAu) {
        if (!md_take(&c, 2u)) return false;
        out->flow = MD_DECODE_FLOW_RETURN;
        goto done;
    }
    if (opcode == 0xC3u || opcode == 0xCBu || opcode == 0xCFu) {
        out->flow = MD_DECODE_FLOW_RETURN;
        goto done;
    }
    if (opcode == 0xC4u || opcode == 0xC5u) {
        if (!md_take_modrm(&c, out)) return false;
        goto done;
    }
    if (opcode == 0xC6u) {
        if (!md_take_modrm_imm(&c, out, 1u)) return false;
        goto done;
    }
    if (opcode == 0xC7u) {
        if (!md_take_modrm_imm(&c, out, 2u)) return false;
        goto done;
    }
    if (opcode == 0xC8u) {
        out->valid_8086 = 0u;
        if (!md_take(&c, 3u)) return false;
        goto done;
    }
    if (opcode == 0xC9u) {
        out->valid_8086 = 0u;
        goto done;
    }
    if (opcode == 0xCCu || opcode == 0xCEu) goto done;
    if (opcode == 0xCDu) {
        if (!md_take(&c, 1u)) return false;
        goto done;
    }

    if (opcode == 0xD4u || opcode == 0xD5u) {
        if (!md_take(&c, 1u)) return false;
        goto done;
    }
    if (opcode == 0xD6u || opcode == 0xD7u) goto done;

    if (opcode >= 0xE0u && opcode <= 0xE3u) {
        int8_t rel;
        if (c.pos >= c.size) return false;
        rel = (int8_t)c.image[c.pos++];
        out->flow = MD_DECODE_FLOW_CONDITIONAL;
        out->target = (uint16_t)((uint16_t)(image_base + (uint16_t)c.pos) + rel);
        goto done;
    }
    if (opcode >= 0xE4u && opcode <= 0xE7u) {
        if (!md_take(&c, 1u)) return false;
        goto done;
    }
    if (opcode == 0xE8u || opcode == 0xE9u) {
        int16_t rel;
        if (c.pos + 2u > c.size) return false;
        rel = (int16_t)((uint16_t)c.image[c.pos] |
                        ((uint16_t)c.image[c.pos + 1u] << 8));
        c.pos += 2u;
        out->flow = opcode == 0xE8u ? MD_DECODE_FLOW_CALL : MD_DECODE_FLOW_JUMP;
        out->target = (uint16_t)((uint16_t)(image_base + (uint16_t)c.pos) + rel);
        goto done;
    }
    if (opcode == 0xEAu) {
        if (!md_take(&c, 4u)) return false;
        out->flow = MD_DECODE_FLOW_INDIRECT_JUMP;
        out->far_control = 1u;
        goto done;
    }
    if (opcode == 0xEBu) {
        int8_t rel;
        if (c.pos >= c.size) return false;
        rel = (int8_t)c.image[c.pos++];
        out->flow = MD_DECODE_FLOW_JUMP;
        out->target = (uint16_t)((uint16_t)(image_base + (uint16_t)c.pos) + rel);
        goto done;
    }
    if (opcode >= 0xECu && opcode <= 0xEFu) goto done;

    if (opcode == 0xF1u) {
        out->valid_8086 = 0u;
        goto done;
    }
    if (opcode == 0xF4u) {
        out->flow = MD_DECODE_FLOW_STOP;
        goto done;
    }
    if (opcode == 0xF5u || (opcode >= 0xF8u && opcode <= 0xFDu)) goto done;

    if (opcode == 0xF6u || opcode == 0xF7u) {
        unsigned reg;
        if (!md_take_modrm(&c, out)) return false;
        reg = (out->modrm >> 3) & 7u;
        if (reg == 0u) {
            if (!md_take(&c, opcode == 0xF6u ? 1u : 2u)) return false;
        }
        goto done;
    }
    if (opcode == 0xFEu) {
        if (!md_take_modrm(&c, out)) return false;
        goto done;
    }
    if (opcode == 0xFFu) {
        unsigned reg;
        if (!md_take_modrm(&c, out)) return false;
        reg = (out->modrm >> 3) & 7u;
        if (reg == 2u || reg == 3u) {
            out->flow = MD_DECODE_FLOW_INDIRECT_CALL;
            out->far_control = reg == 3u;
        } else if (reg == 4u || reg == 5u) {
            out->flow = MD_DECODE_FLOW_INDIRECT_JUMP;
            out->far_control = reg == 5u;
        }
        goto done;
    }

    /* Everything not listed above is a one-byte opcode on an 8086, including
       IN/OUT via DX and flag/control instructions. */

done:
    if (c.pos <= offset || c.pos - offset > 255u) return false;
    out->length = (uint8_t)(c.pos - offset);
    out->next_ip = (uint16_t)(ip + out->length);
    return true;
}

const char *md_decode_flow_name(MdDecodeFlow flow)
{
    switch (flow) {
        case MD_DECODE_FLOW_FALLTHROUGH: return "fallthrough";
        case MD_DECODE_FLOW_CONDITIONAL: return "conditional";
        case MD_DECODE_FLOW_CALL: return "call";
        case MD_DECODE_FLOW_JUMP: return "jump";
        case MD_DECODE_FLOW_INDIRECT_CALL: return "indirect-call";
        case MD_DECODE_FLOW_INDIRECT_JUMP: return "indirect-jump";
        case MD_DECODE_FLOW_RETURN: return "return";
        case MD_DECODE_FLOW_STOP: return "stop";
        default: return "unknown";
    }
}

static int md_base_interp_opcode(uint8_t opcode)
{
    if ((opcode & 0xF8u) == 0xB0u || (opcode & 0xF8u) == 0xB8u ||
        (opcode & 0xF8u) == 0x40u || (opcode & 0xF8u) == 0x48u ||
        (opcode & 0xF8u) == 0x50u || (opcode & 0xF8u) == 0x58u) return 1;

    if (opcode <= 0x3Bu && (opcode & 0x04u) == 0u) return 1;
    if (opcode <= 0x3Du && (opcode & 0x06u) == 0x04u) return 1;

    if (opcode >= 0x70u && opcode <= 0x7Fu) return 1;
    if (opcode >= 0x80u && opcode <= 0x8Fu) return 1;
    if (opcode >= 0x90u && opcode <= 0x9Fu) return 1;
    if (opcode >= 0xA0u && opcode <= 0xA9u) return 1;
    if (opcode >= 0xAAu && opcode <= 0xAFu) return 1;
    if (opcode >= 0xC2u && opcode <= 0xC7u) return 1;
    if (opcode >= 0xCAu && opcode <= 0xCFu) return 1;
    if (opcode >= 0xD0u && opcode <= 0xD7u) return 1;
    if (opcode >= 0xE0u && opcode <= 0xEFu) return 1;
    if (opcode >= 0xF4u) return 1;

    switch (opcode) {
        case 0x06u: case 0x07u: case 0x0Eu: case 0x0Fu:
        case 0x16u: case 0x17u: case 0x1Eu: case 0x1Fu:
        case 0x27u: case 0x2Fu: case 0x37u: case 0x3Fu:
            return 1;
        default:
            return 0;
    }
}

bool md_decode_interp_supported(const MdDecodedInstruction *inst)
{
    unsigned ext;
    unsigned mod;
    if (inst == NULL || !inst->valid_8086 ||
        md_base_interp_opcode(inst->opcode) == 0) return false;

    ext = (inst->modrm >> 3) & 7u;
    mod = inst->modrm >> 6;
    if ((inst->opcode == 0x8Cu || inst->opcode == 0x8Eu) && ext >= 4u) return false;
    if (inst->opcode == 0x8Eu && ext == 1u) return false; /* MOV CS,r/m16 */
    if (inst->opcode == 0x8Du && mod == 3u) return false; /* LEA requires memory EA. */
    if (inst->opcode == 0x8Fu && ext != 0u) return false;
    if ((inst->opcode == 0xC4u || inst->opcode == 0xC5u) && mod == 3u) return false;
    if ((inst->opcode == 0xC6u || inst->opcode == 0xC7u) && ext != 0u) return false;
    if ((inst->opcode == 0xF6u || inst->opcode == 0xF7u) && ext == 1u) return false;
    if (inst->opcode == 0xFEu && ext > 1u) return false;
    if (inst->opcode == 0xFFu) {
        if (ext == 7u) return false;
        if ((ext == 3u || ext == 5u) && mod == 3u) return false;
    }
    return true;
}

bool md_decode_aot_supported(const MdDecodedInstruction *inst)
{
    uint8_t opcode;
    if (inst == NULL || !inst->valid_8086 || inst->prefix_count != 0u) return false;
    opcode = inst->opcode;
    if ((opcode & 0xF8u) == 0xB0u || (opcode & 0xF8u) == 0xB8u ||
        (opcode & 0xF8u) == 0x40u || (opcode & 0xF8u) == 0x48u ||
        (opcode & 0xF8u) == 0x50u || (opcode & 0xF8u) == 0x58u) return true;
    switch (opcode) {
        case 0x04u: case 0x05u: case 0x2Cu: case 0x2Du:
        case 0x3Cu: case 0x3Du: case 0x74u: case 0x75u:
        case 0x90u: case 0xA0u: case 0xA1u: case 0xA2u:
        case 0xA3u: case 0xC3u: case 0xCDu: case 0xE8u:
        case 0xE9u: case 0xEBu: case 0xF4u:
            return true;
        default:
            return false;
    }
}
