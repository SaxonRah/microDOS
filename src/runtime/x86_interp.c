#include "microdos/runtime.h"
#include "runtime_internal.h"

#include <stddef.h>
#include <string.h>

#if MD_INTERP_OPCODE_PROFILE
static uint32_t g_md_opcode_profile[256];
static uint32_t g_md_unpref_modrm_profile[6u * 256u];
static uint32_t g_md_prefix_profile[7u * 256u];
static uint32_t g_md_hot_modrm_profile[10u * 256u];

const uint32_t *md_interp_opcode_profile_counts(void)
{
    return g_md_opcode_profile;
}

const uint32_t *md_interp_unpref_modrm_profile_counts(void)
{
    return g_md_unpref_modrm_profile;
}

const uint32_t *md_interp_prefix_profile_counts(void)
{
    return g_md_prefix_profile;
}

const uint32_t *md_interp_hot_modrm_profile_counts(void)
{
    return g_md_hot_modrm_profile;
}

void md_interp_opcode_profile_reset(void)
{
    memset(g_md_opcode_profile, 0, sizeof(g_md_opcode_profile));
    memset(g_md_unpref_modrm_profile, 0, sizeof(g_md_unpref_modrm_profile));
    memset(g_md_prefix_profile, 0, sizeof(g_md_prefix_profile));
    memset(g_md_hot_modrm_profile, 0, sizeof(g_md_hot_modrm_profile));
}

#define MD_OPCODE_PROFILE_HIT(op) (++g_md_opcode_profile[(uint8_t)(op)])

static inline unsigned md_unpref_modrm_profile_row(uint8_t opcode)
{
    switch (opcode) {
        case 0x33u: return 0u;
        case 0x83u: return 1u;
        case 0x88u: return 2u;
        case 0x8Bu: return 3u;
        case 0xF6u: return 4u;
        case 0xF7u: return 5u;
        default: return 6u;
    }
}

static inline void md_unpref_modrm_profile_hit(uint8_t opcode, uint8_t modrm)
{
    const unsigned row = md_unpref_modrm_profile_row(opcode);
    if (row < 6u) ++g_md_unpref_modrm_profile[row * 256u + modrm];
}

#define MD_UNPREF_MODRM_PROFILE_HIT(op, modrm) \
    md_unpref_modrm_profile_hit((uint8_t)(op), (uint8_t)(modrm))

static inline unsigned md_prefix_profile_row(uint8_t prefix)
{
    switch (prefix) {
        case 0x26u: return 0u;
        case 0x2Eu: return 1u;
        case 0x36u: return 2u;
        case 0x3Eu: return 3u;
        case 0xF0u: return 4u;
        case 0xF2u: return 5u;
        case 0xF3u: return 6u;
        default: return 7u;
    }
}

static inline void md_prefix_profile_hit(uint8_t prefix, uint8_t opcode)
{
    const unsigned row = md_prefix_profile_row(prefix);
    if (row < 7u) ++g_md_prefix_profile[row * 256u + opcode];
}

#define MD_PREFIX_PROFILE_HIT(prefix, op) md_prefix_profile_hit((uint8_t)(prefix), (uint8_t)(op))

static inline unsigned md_hot_modrm_profile_row(uint8_t prefix, uint8_t opcode)
{
    if (prefix == 0x36u) {
        switch (opcode) {
            case 0x8Cu: return 0u;
            case 0xFFu: return 1u;
            case 0xC7u: return 2u;
            case 0x80u: return 3u;
            case 0x8Bu: return 4u;
            default: return 10u;
        }
    }
    if (prefix == 0x2Eu) {
        if (opcode == 0xFFu) return 5u;
        if (opcode == 0x8Fu) return 6u;
        return 10u;
    }
    if (prefix == 0x26u) {
        if (opcode == 0x8Au) return 7u;
        if (opcode == 0x03u) return 8u;
        if (opcode == 0x2Bu) return 9u;
    }
    return 10u;
}

static inline void md_hot_modrm_profile_hit(uint8_t prefix, uint8_t opcode, uint8_t modrm)
{
    const unsigned row = md_hot_modrm_profile_row(prefix, opcode);
    if (row < 10u) ++g_md_hot_modrm_profile[row * 256u + modrm];
}

#define MD_HOT_MODRM_PROFILE_HIT(prefix, op, modrm) \
    md_hot_modrm_profile_hit((uint8_t)(prefix), (uint8_t)(op), (uint8_t)(modrm))
#else
#define MD_OPCODE_PROFILE_HIT(op) ((void)0)
#define MD_UNPREF_MODRM_PROFILE_HIT(op, modrm) ((void)0)
#define MD_PREFIX_PROFILE_HIT(prefix, op) ((void)0)
#define MD_HOT_MODRM_PROFILE_HIT(prefix, op, modrm) ((void)0)
#endif

typedef struct MdOperand {
    uint8_t is_register;
    uint8_t reg;
    uint16_t segment;
    uint16_t offset;
} MdOperand;

typedef struct MdPrefixState {
    uint8_t segment_override;
    uint8_t repeat;
    uint8_t lock;
} MdPrefixState;

static inline int md_is_prefix_byte(uint8_t opcode)
{
    /* 26/2E/36/3E share the pattern 001xx110; F0/F2/F3 are 111100xx minus F1. */
    return (opcode & 0xE7u) == 0x26u ||
           ((opcode & 0xFCu) == 0xF0u && opcode != 0xF1u);
}

static inline void md_apply_prefix(MdPrefixState *prefix, uint8_t opcode)
{
    switch (opcode) {
        case 0x26u: case 0x2Eu: case 0x36u: case 0x3Eu:
            prefix->segment_override = opcode;
            break;
        case 0xF0u:
            prefix->lock = 1u;
            break;
        case 0xF2u: case 0xF3u:
            prefix->repeat = opcode;
            break;
        default:
            break;
    }
}

static inline uint16_t md_get_sreg(const MdX86 *cpu, unsigned reg)
{
    switch (reg & 3u) {
        case 0u: return cpu->es;
        case 1u: return cpu->cs;
        case 2u: return cpu->ss;
        default: return cpu->ds;
    }
}

static inline uint16_t md_prefixed_segment(const MdX86 *cpu,
                                            const MdPrefixState *prefix,
                                            uint16_t fallback)
{
    if (prefix == NULL || prefix->segment_override == 0u) return fallback;
    /* Override bytes 26/2E/36/3E carry the Sreg number in bits 4:3. */
    return md_get_sreg(cpu, (prefix->segment_override >> 3) & 3u);
}

static inline MdOperand md_decode_rm(MdRuntime *runtime, uint8_t modrm, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const unsigned mod = modrm >> 6;
    const unsigned rm = modrm & 7u;
    MdOperand op;
    int16_t displacement = 0;
    uint16_t base = 0;
    uint16_t segment = cpu->ds;
    int uses_bp = 0;

    op.is_register = 0u;
    op.reg = (uint8_t)rm;
    op.segment = 0u;
    op.offset = 0u;

    if (mod == 3u) {
        op.is_register = 1u;
        return op;
    }

    if (mod == 0u && rm == 6u) {
        op.segment = md_prefixed_segment(cpu, prefix, cpu->ds);
        op.offset = md_fetch16(runtime);
        return op;
    }

    /* Table-driven 8086 EA: base register, optional index register (masked
       to zero for rm 4..7), and the BP-relative set {2,3,6} as the bitmask
       0x4C. Replaces an 8-way switch with two loads and an AND. */
    {
        static const uint8_t ea_base[8]  = { MD_X86_BX, MD_X86_BX, MD_X86_BP, MD_X86_BP,
                                             MD_X86_SI, MD_X86_DI, MD_X86_BP, MD_X86_BX };
        static const uint8_t ea_index[8] = { MD_X86_SI, MD_X86_DI, MD_X86_SI, MD_X86_DI,
                                             0u, 0u, 0u, 0u };
        const uint16_t index_mask = (uint16_t)(0u - (unsigned)((rm >> 2) ^ 1u));
        base = (uint16_t)(cpu->r[ea_base[rm]] + (cpu->r[ea_index[rm]] & index_mask));
        uses_bp = (int)((0x4Cu >> rm) & 1u);
    }

    if (mod == 1u) displacement = (int8_t)md_fetch8(runtime);
    else if (mod == 2u) displacement = (int16_t)md_fetch16(runtime);

    if (uses_bp) segment = cpu->ss;
    op.segment = md_prefixed_segment(cpu, prefix, segment);
    op.offset = (uint16_t)(base + displacement);
    return op;
}

static inline uint8_t md_operand_read8(MdRuntime *runtime, MdOperand op)
{
    return op.is_register ? md_x86_get_reg8(&runtime->cpu, op.reg)
                          : md_x86_read8(&runtime->cpu, op.segment, op.offset);
}

static inline uint16_t md_operand_read16(MdRuntime *runtime, MdOperand op)
{
    return op.is_register ? runtime->cpu.r[op.reg]
                          : md_x86_read16(&runtime->cpu, op.segment, op.offset);
}

static inline void md_operand_write8(MdRuntime *runtime, MdOperand op, uint8_t value)
{
    if (op.is_register) md_x86_set_reg8(&runtime->cpu, op.reg, value);
    else md_x86_write8(&runtime->cpu, op.segment, op.offset, value);
}

static inline void md_operand_write16(MdRuntime *runtime, MdOperand op, uint16_t value)
{
    if (op.is_register) runtime->cpu.r[op.reg] = value;
    else md_x86_write16(&runtime->cpu, op.segment, op.offset, value);
}

static inline int md_set_sreg(MdX86 *cpu, unsigned reg, uint16_t value)
{
    switch (reg) {
        case 0u: cpu->es = value; return 1;
        case 2u: cpu->ss = value; return 1;
        case 3u: cpu->ds = value; return 1;
        default: return 0; /* MOV CS,r/m16 is not a valid 8086 instruction. */
    }
}

static inline void md_fault(MdRuntime *runtime, uint8_t opcode, uint16_t ip_before)
{
    runtime->fault_opcode = opcode;
    runtime->fault_linear = md_x86_linear(runtime->cpu.cs, ip_before);
    runtime->stop_reason = MD_STOP_FAULT;
}

/* One ALU implementation for interpreter and generated code (ops.h). */
static inline uint8_t md_alu8(MdX86 *cpu, unsigned operation, uint8_t lhs, uint8_t rhs)
{
    return md_x86_alu8(cpu, operation, lhs, rhs);
}

static inline uint16_t md_alu16(MdX86 *cpu, unsigned operation, uint16_t lhs, uint16_t rhs)
{
    return md_x86_alu16(cpu, operation, lhs, rhs);
}

static inline void md_op_mov_r8_imm(MdRuntime *runtime, uint8_t opcode)
{
    md_x86_set_reg8(&runtime->cpu, opcode & 7u, md_fetch8(runtime));
}

static inline void md_op_mov_r16_imm(MdRuntime *runtime, uint8_t opcode)
{
    runtime->cpu.r[opcode & 7u] = md_fetch16(runtime);
}

static inline void md_op_inc_r16(MdRuntime *runtime, uint8_t opcode)
{
    MdX86 *cpu = &runtime->cpu;
    const unsigned reg = opcode & 7u;
    cpu->r[reg] = md_x86_inc16(cpu, cpu->r[reg]);
}

static inline void md_op_dec_r16(MdRuntime *runtime, uint8_t opcode)
{
    MdX86 *cpu = &runtime->cpu;
    const unsigned reg = opcode & 7u;
    cpu->r[reg] = md_x86_dec16(cpu, cpu->r[reg]);
}

static inline void md_op_mov_rm_r(MdRuntime *runtime, uint8_t opcode, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if (opcode == 0x88u) md_operand_write8(runtime, rm, md_x86_get_reg8(cpu, reg));
    else if (opcode == 0x89u) md_operand_write16(runtime, rm, cpu->r[reg]);
    else if (opcode == 0x8Au) md_x86_set_reg8(cpu, reg, md_operand_read8(runtime, rm));
    else cpu->r[reg] = md_operand_read16(runtime, rm);
}

static inline void md_op_mov_sreg(MdRuntime *runtime, uint8_t opcode, uint16_t ip_before, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    /*
     * Original 8086 checks only the low two Sreg selector bits.  ModR/M.reg
     * values 4..7 therefore alias 0..3.  MOV-to-CS remains rejected by
     * md_set_sreg(), matching the architecturally unusable selector 1 form.
     */
    const unsigned reg = (modrm >> 3) & 3u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if (opcode == 0x8Cu) {
        md_operand_write16(runtime, rm, md_get_sreg(cpu, reg));
    } else if (!md_set_sreg(cpu, reg, md_operand_read16(runtime, rm))) {
        md_fault(runtime, opcode, ip_before);
    }
}

static inline void md_op_mov_rm_imm(MdRuntime *runtime, uint8_t opcode, uint16_t ip_before, const MdPrefixState *prefix)
{
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned ext = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if (ext != 0u) {
        md_fault(runtime, opcode, ip_before);
        return;
    }
    if (opcode == 0xC6u) md_operand_write8(runtime, rm, md_fetch8(runtime));
    else md_operand_write16(runtime, rm, md_fetch16(runtime));
}

static inline void md_op_alu_rm_r(MdRuntime *runtime, uint8_t opcode, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    const unsigned operation = (opcode >> 3) & 7u;
    const int direction = (opcode >> 1) & 1u;
    const int width16 = opcode & 1u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if (!width16) {
        const uint8_t lhs = direction ? md_x86_get_reg8(cpu, reg) : md_operand_read8(runtime, rm);
        const uint8_t rhs = direction ? md_operand_read8(runtime, rm) : md_x86_get_reg8(cpu, reg);
        const uint8_t result = md_alu8(cpu, operation, lhs, rhs);
        if (operation != 7u) {
            if (direction) md_x86_set_reg8(cpu, reg, result);
            else md_operand_write8(runtime, rm, result);
        }
    } else {
        const uint16_t lhs = direction ? cpu->r[reg] : md_operand_read16(runtime, rm);
        const uint16_t rhs = direction ? md_operand_read16(runtime, rm) : cpu->r[reg];
        const uint16_t result = md_alu16(cpu, operation, lhs, rhs);
        if (operation != 7u) {
            if (direction) cpu->r[reg] = result;
            else md_operand_write16(runtime, rm, result);
        }
    }
}

static inline void md_op_alu_acc_imm(MdRuntime *runtime, uint8_t opcode)
{
    MdX86 *cpu = &runtime->cpu;
    const unsigned operation = (opcode >> 3) & 7u;
    const int width16 = opcode & 1u;

    if (!width16) {
        const uint8_t lhs = md_x86_get_reg8(cpu, 0u);
        const uint8_t result = md_alu8(cpu, operation, lhs, md_fetch8(runtime));
        if (operation != 7u) md_x86_set_reg8(cpu, 0u, result);
    } else {
        const uint16_t lhs = cpu->r[MD_X86_AX];
        const uint16_t result = md_alu16(cpu, operation, lhs, md_fetch16(runtime));
        if (operation != 7u) cpu->r[MD_X86_AX] = result;
    }
}

static inline void md_op_group1_imm(MdRuntime *runtime, uint8_t opcode, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned operation = (modrm >> 3) & 7u;
    const int width16 = opcode == 0x81u || opcode == 0x83u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if (!width16) {
        const uint8_t lhs = md_operand_read8(runtime, rm);
        const uint8_t rhs = md_fetch8(runtime);
        const uint8_t result = md_alu8(cpu, operation, lhs, rhs);
        if (operation != 7u) md_operand_write8(runtime, rm, result);
    } else {
        const uint16_t lhs = md_operand_read16(runtime, rm);
        const uint16_t rhs = opcode == 0x83u
            ? (uint16_t)(int16_t)(int8_t)md_fetch8(runtime)
            : md_fetch16(runtime);
        const uint16_t result = md_alu16(cpu, operation, lhs, rhs);
        if (operation != 7u) md_operand_write16(runtime, rm, result);
    }
}

static inline int md_execute_opcode(MdRuntime *runtime, uint8_t opcode,
                                    uint16_t ip_before, const MdPrefixState *prefix);

static inline int16_t md_string_delta(const MdX86 *cpu, unsigned width)
{
    return (cpu->flags_raw & MD_X86_FLAG_DF) != 0u ? -(int16_t)width : (int16_t)width;
}

static inline void md_advance_index(uint16_t *value, int16_t delta)
{
    *value = (uint16_t)(*value + delta);
}

static inline void md_op_string_once(MdRuntime *runtime, uint8_t opcode,
                                     const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const unsigned width = (opcode & 1u) != 0u ? 2u : 1u;
    const int16_t delta = md_string_delta(cpu, width);
    const uint16_t source_segment = md_prefixed_segment(cpu, prefix, cpu->ds);

    switch (opcode) {
        case 0xA4u: /* MOVSB */
            md_x86_write8(cpu, cpu->es, cpu->r[MD_X86_DI],
                          md_x86_read8(cpu, source_segment, cpu->r[MD_X86_SI]));
            md_advance_index(&cpu->r[MD_X86_SI], delta);
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        case 0xA5u: /* MOVSW */
            md_x86_write16(cpu, cpu->es, cpu->r[MD_X86_DI],
                           md_x86_read16(cpu, source_segment, cpu->r[MD_X86_SI]));
            md_advance_index(&cpu->r[MD_X86_SI], delta);
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        case 0xA6u: { /* CMPSB */
            const uint8_t lhs = md_x86_read8(cpu, source_segment, cpu->r[MD_X86_SI]);
            const uint8_t rhs = md_x86_read8(cpu, cpu->es, cpu->r[MD_X86_DI]);
            (void)md_x86_sub8(cpu, lhs, rhs);
            md_advance_index(&cpu->r[MD_X86_SI], delta);
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        }
        case 0xA7u: { /* CMPSW */
            const uint16_t lhs = md_x86_read16(cpu, source_segment, cpu->r[MD_X86_SI]);
            const uint16_t rhs = md_x86_read16(cpu, cpu->es, cpu->r[MD_X86_DI]);
            (void)md_x86_sub16(cpu, lhs, rhs);
            md_advance_index(&cpu->r[MD_X86_SI], delta);
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        }
        case 0xAAu: /* STOSB */
            md_x86_write8(cpu, cpu->es, cpu->r[MD_X86_DI], md_x86_get_reg8(cpu, 0u));
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        case 0xABu: /* STOSW */
            md_x86_write16(cpu, cpu->es, cpu->r[MD_X86_DI], cpu->r[MD_X86_AX]);
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        case 0xACu: /* LODSB */
            md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, source_segment, cpu->r[MD_X86_SI]));
            md_advance_index(&cpu->r[MD_X86_SI], delta);
            break;
        case 0xADu: /* LODSW */
            cpu->r[MD_X86_AX] = md_x86_read16(cpu, source_segment, cpu->r[MD_X86_SI]);
            md_advance_index(&cpu->r[MD_X86_SI], delta);
            break;
        case 0xAEu: /* SCASB */
            (void)md_x86_sub8(cpu, md_x86_get_reg8(cpu, 0u),
                              md_x86_read8(cpu, cpu->es, cpu->r[MD_X86_DI]));
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        case 0xAFu: /* SCASW */
            (void)md_x86_sub16(cpu, cpu->r[MD_X86_AX],
                               md_x86_read16(cpu, cpu->es, cpu->r[MD_X86_DI]));
            md_advance_index(&cpu->r[MD_X86_DI], delta);
            break;
        default:
            break;
    }
}

static inline void md_op_string(MdRuntime *runtime, uint8_t opcode,
                                const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t repeat = prefix != NULL ? prefix->repeat : 0u;
    const int compares = opcode == 0xA6u || opcode == 0xA7u ||
                         opcode == 0xAEu || opcode == 0xAFu;

    if (repeat == 0u) {
        md_op_string_once(runtime, opcode, prefix);
        return;
    }

    while (cpu->r[MD_X86_CX] != 0u) {
        md_op_string_once(runtime, opcode, prefix);
        cpu->r[MD_X86_CX] = (uint16_t)(cpu->r[MD_X86_CX] - 1u);

        if (compares) {
            const int zf = md_x86_zf(cpu);
            if ((repeat == 0xF3u && !zf) || (repeat == 0xF2u && zf)) break;
        }
    }
}

static inline void md_op_loop(MdRuntime *runtime, uint8_t opcode)
{
    MdX86 *cpu = &runtime->cpu;
    const int8_t rel = (int8_t)md_fetch8(runtime);
    int take = 0;

    if (opcode == 0xE3u) {
        take = cpu->r[MD_X86_CX] == 0u;
    } else {
        cpu->r[MD_X86_CX] = (uint16_t)(cpu->r[MD_X86_CX] - 1u);
        if (opcode == 0xE2u) take = cpu->r[MD_X86_CX] != 0u;
        else if (opcode == 0xE1u) take = cpu->r[MD_X86_CX] != 0u &&
                                       md_x86_zf(cpu);
        else take = cpu->r[MD_X86_CX] != 0u &&
                    !md_x86_zf(cpu);
    }

    if (take) cpu->ip = (uint16_t)(cpu->ip + rel);
}

static inline void md_set_cf_of(MdX86 *cpu, int set)
{
    md_x86_update_flags(cpu, (uint16_t)(MD_X86_FLAG_CF | MD_X86_FLAG_OF),
                        set ? (uint16_t)(MD_X86_FLAG_CF | MD_X86_FLAG_OF) : 0u);
}

static inline void md_op_test_rm_r(MdRuntime *runtime, uint8_t opcode,
                                   const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if ((opcode & 1u) == 0u) {
        (void)md_x86_logic8(cpu, (uint8_t)(md_operand_read8(runtime, rm) &
                                           md_x86_get_reg8(cpu, reg)));
    } else {
        (void)md_x86_logic16(cpu, (uint16_t)(md_operand_read16(runtime, rm) &
                                             cpu->r[reg]));
    }
}

static inline void md_op_xchg_rm_r(MdRuntime *runtime, uint8_t opcode,
                                   const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if ((opcode & 1u) == 0u) {
        const uint8_t a = md_operand_read8(runtime, rm);
        const uint8_t b = md_x86_get_reg8(cpu, reg);
        md_operand_write8(runtime, rm, b);
        md_x86_set_reg8(cpu, reg, a);
    } else {
        const uint16_t a = md_operand_read16(runtime, rm);
        const uint16_t b = cpu->r[reg];
        md_operand_write16(runtime, rm, b);
        cpu->r[reg] = a;
    }
}

static inline void md_op_lea(MdRuntime *runtime, uint16_t ip_before,
                             const MdPrefixState *prefix)
{
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);
    if (rm.is_register) {
        md_fault(runtime, 0x8Du, ip_before);
        return;
    }
    runtime->cpu.r[reg] = rm.offset;
}

static inline void md_op_pop_rm(MdRuntime *runtime, uint16_t ip_before,
                                const MdPrefixState *prefix)
{
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned ext = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);
    if (ext != 0u) {
        md_fault(runtime, 0x8Fu, ip_before);
        return;
    }
    md_operand_write16(runtime, rm, md_x86_pop(&runtime->cpu));
}

static inline void md_op_les_lds(MdRuntime *runtime, uint8_t opcode,
                                 uint16_t ip_before, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);
    uint16_t offset;
    uint16_t segment;

    if (rm.is_register) {
        md_fault(runtime, opcode, ip_before);
        return;
    }
    offset = md_x86_read16(cpu, rm.segment, rm.offset);
    segment = md_x86_read16(cpu, rm.segment, (uint16_t)(rm.offset + 2u));
    cpu->r[reg] = offset;
    if (opcode == 0xC4u) cpu->es = segment;
    else cpu->ds = segment;
}

static inline void md_op_shift(MdRuntime *runtime, uint8_t opcode,
                               const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned operation = (modrm >> 3) & 7u;
    const unsigned count = (opcode >= 0xD2u) ? md_x86_get_reg8(cpu, 1u) : 1u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if ((opcode & 1u) == 0u) {
        const uint8_t value = md_operand_read8(runtime, rm);
        md_operand_write8(runtime, rm, md_x86_shift8(cpu, operation, value, count));
    } else {
        const uint16_t value = md_operand_read16(runtime, rm);
        md_operand_write16(runtime, rm, md_x86_shift16(cpu, operation, value, count));
    }
}

static inline void md_divide_error(MdRuntime *runtime)
{
    /*
     * 8086 divide-by-zero and quotient overflow are Type-0 interrupts, not
     * emulator stop conditions.  cpu->ip already points at the next guest
     * instruction, which is the return IP pushed by original 8086 hardware.
     */
    (void)md_runtime_interrupt(runtime, 0u);
}

/* MUL/IMUL/DIV/IDIV (M18): one implementation shared by the interpreter's
   group 3 and dosrecomp-generated code (md_interp_muldiv).  DIV/IDIV
   exceptions dispatch through real-mode vector 0. `ext` is ModR/M.reg (4..7). */
static inline void md_muldiv_core(MdRuntime *runtime, uint8_t opcode, unsigned ext,
                                  uint16_t operand, uint16_t ip_before,
                                  uint8_t repeat_prefix)
{
    MdX86 *cpu = &runtime->cpu;
    (void)ip_before;
    if (opcode == 0xF6u) {
        const uint8_t value = (uint8_t)operand;
        switch (ext) {
            case 4u: {
                const uint16_t product = (uint16_t)md_x86_get_reg8(cpu, 0u) * (uint16_t)value;
                cpu->r[MD_X86_AX] = product;
                md_set_cf_of(cpu, (product & 0xFF00u) != 0u);
                break;
            }
            case 5u: {
                const int16_t a = (int16_t)(int8_t)md_x86_get_reg8(cpu, 0u);
                const int16_t b = (int16_t)(int8_t)value;
                const int16_t product = (int16_t)(a * b);
                cpu->r[MD_X86_AX] = (uint16_t)product;
                md_set_cf_of(cpu, product < -128 || product > 127);
                break;
            }
            case 6u: {
                const uint16_t dividend = cpu->r[MD_X86_AX];
                uint16_t q;
                uint16_t r;
                if (value == 0u) { md_divide_error(runtime); break; }
                q = (uint16_t)(dividend / value);
                r = (uint16_t)(dividend % value);
                if (q > 0xFFu) { md_divide_error(runtime); break; }
                md_x86_set_reg8(cpu, 0u, (uint8_t)q);
                md_x86_set_reg8(cpu, 4u, (uint8_t)r);
                break;
            }
            default: {
                const uint16_t raw = cpu->r[MD_X86_AX];
                const int32_t dividend = (raw & 0x8000u) != 0u ? (int32_t)raw - 0x10000L : (int32_t)raw;
                const int32_t divisor = (value & 0x80u) != 0u ? (int32_t)value - 0x100L : (int32_t)value;
                int32_t q;
                int32_t r;
                if (divisor == 0) { md_divide_error(runtime); break; }
                q = dividend / divisor;
                r = dividend % divisor;
                if (q <= -128 || q > 127) { md_divide_error(runtime); break; }
                if (repeat_prefix != 0u) q = -q; /* original-8086 REP/REPNE IDIV sign latch */
                md_x86_set_reg8(cpu, 0u, (uint8_t)q);
                md_x86_set_reg8(cpu, 4u, (uint8_t)r);
                break;
            }
        }
    } else {
        const uint16_t value = operand;
        switch (ext) {
            case 4u: {
                const uint32_t product = (uint32_t)cpu->r[MD_X86_AX] * (uint32_t)value;
                cpu->r[MD_X86_AX] = (uint16_t)product;
                cpu->r[MD_X86_DX] = (uint16_t)(product >> 16);
                md_set_cf_of(cpu, cpu->r[MD_X86_DX] != 0u);
                break;
            }
            case 5u: {
                const int32_t a = (int32_t)(int16_t)cpu->r[MD_X86_AX];
                const int32_t b = (int32_t)(int16_t)value;
                const int32_t product = a * b;
                cpu->r[MD_X86_AX] = (uint16_t)product;
                cpu->r[MD_X86_DX] = (uint16_t)((uint32_t)product >> 16);
                md_set_cf_of(cpu, product < -32768L || product > 32767L);
                break;
            }
            case 6u: {
                const uint32_t dividend = ((uint32_t)cpu->r[MD_X86_DX] << 16) | cpu->r[MD_X86_AX];
                uint32_t q;
                uint32_t r;
                if (value == 0u) { md_divide_error(runtime); break; }
                q = dividend / value;
                r = dividend % value;
                if (q > 0xFFFFu) { md_divide_error(runtime); break; }
                cpu->r[MD_X86_AX] = (uint16_t)q;
                cpu->r[MD_X86_DX] = (uint16_t)r;
                break;
            }
            default: {
                /* DX:AX is exactly an int32_t, so a 32-bit SDIV suffices.
                   Cortex-M33 has no 64-bit divide: the old int64_t form
                   called __aeabi_ldivmod for every IDIV r/m16. The only
                   32-bit overflow case (INT32_MIN / -1) has a quotient far
                   outside the 16-bit range, so it is a #DE anyway. */
                const uint32_t bits = ((uint32_t)cpu->r[MD_X86_DX] << 16) | cpu->r[MD_X86_AX];
                const int32_t dividend = (int32_t)bits;
                const int32_t divisor = (int32_t)(int16_t)value;
                int32_t q;
                int32_t r;
                if (divisor == 0 || (divisor == -1 && bits == 0x80000000u)) {
                    md_divide_error(runtime);
                    break;
                }
                q = dividend / divisor;
                r = dividend % divisor;
                if (q <= -32768L || q > 32767L) { md_divide_error(runtime); break; }
                if (repeat_prefix != 0u) q = -q; /* original-8086 REP/REPNE IDIV sign latch */
                cpu->r[MD_X86_AX] = (uint16_t)q;
                cpu->r[MD_X86_DX] = (uint16_t)r;
                break;
            }
        }
    }
}

static inline void md_op_group3(MdRuntime *runtime, uint8_t opcode,
                                uint16_t ip_before, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned ext = (modrm >> 3) & 7u;
    const int width16 = opcode == 0xF7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if (!width16) {
        const uint8_t value = md_operand_read8(runtime, rm);
        switch (ext) {
            case 0u:
            case 1u: { /* /1 is an original-8086 alias of TEST */
                const uint8_t imm = md_fetch8(runtime);
                (void)md_x86_logic8(cpu, (uint8_t)(value & imm));
                break;
            }
            case 2u:
                md_operand_write8(runtime, rm, (uint8_t)~value);
                break;
            case 3u:
                md_operand_write8(runtime, rm, md_x86_sub8(cpu, 0u, value));
                break;
            default:   /* 4 MUL, 5 IMUL, 6 DIV, 7 IDIV */
                md_muldiv_core(runtime, opcode, ext, value, ip_before, prefix != NULL ? prefix->repeat : 0u);
                break;
        }
    } else {
        const uint16_t value = md_operand_read16(runtime, rm);
        switch (ext) {
            case 0u:
            case 1u: { /* /1 is an original-8086 alias of TEST */
                const uint16_t imm = md_fetch16(runtime);
                (void)md_x86_logic16(cpu, (uint16_t)(value & imm));
                break;
            }
            case 2u:
                md_operand_write16(runtime, rm, (uint16_t)~value);
                break;
            case 3u:
                md_operand_write16(runtime, rm, md_x86_sub16(cpu, 0u, value));
                break;
            default:   /* 4 MUL, 5 IMUL, 6 DIV, 7 IDIV */
                md_muldiv_core(runtime, opcode, ext, value, ip_before, prefix != NULL ? prefix->repeat : 0u);
                break;
        }
    }
}

static inline void md_op_group45(MdRuntime *runtime, uint8_t opcode,
                                 uint16_t ip_before, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned ext = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm, prefix);

    if (opcode == 0xFEu) {
        uint8_t value;
        if (ext > 1u) { md_fault(runtime, opcode, ip_before); return; }
        value = md_operand_read8(runtime, rm);
        value = ext == 0u ? md_x86_inc8(cpu, value) : md_x86_dec8(cpu, value);
        md_operand_write8(runtime, rm, value);
        return;
    }

    switch (ext) {
        case 0u: case 1u: {
            uint16_t value = md_operand_read16(runtime, rm);
            value = ext == 0u ? md_x86_inc16(cpu, value) : md_x86_dec16(cpu, value);
            md_operand_write16(runtime, rm, value);
            break;
        }
        case 2u: { /* near CALL r/m16 */
            const uint16_t target = md_operand_read16(runtime, rm);
            const uint16_t return_ip = cpu->ip;
            md_x86_push(cpu, return_ip);
            cpu->ip = target;
            break;
        }
        case 3u: { /* far CALL m16:16 */
            uint16_t target_ip;
            uint16_t target_cs;
            const uint16_t return_ip = cpu->ip;
            if (rm.is_register) { md_fault(runtime, opcode, ip_before); break; }
            target_ip = md_x86_read16(cpu, rm.segment, rm.offset);
            target_cs = md_x86_read16(cpu, rm.segment, (uint16_t)(rm.offset + 2u));
            md_x86_push(cpu, cpu->cs);
            md_x86_push(cpu, return_ip);
            cpu->cs = target_cs;
            cpu->ip = target_ip;
            break;
        }
        case 4u: /* near JMP r/m16 */
            cpu->ip = md_operand_read16(runtime, rm);
            break;
        case 5u: { /* far JMP m16:16 */
            uint16_t target_ip;
            uint16_t target_cs;
            if (rm.is_register) { md_fault(runtime, opcode, ip_before); break; }
            target_ip = md_x86_read16(cpu, rm.segment, rm.offset);
            target_cs = md_x86_read16(cpu, rm.segment, (uint16_t)(rm.offset + 2u));
            cpu->cs = target_cs;
            cpu->ip = target_ip;
            break;
        }
        case 6u:
        case 7u: /* /7 is an original-8086 alias of PUSH r/m16 */
            if (rm.is_register) {
                /* Includes the original-8086 PUSH SP post-decrement value. */
                md_x86_push_reg(cpu, rm.reg);
            } else {
                md_x86_push(cpu, md_operand_read16(runtime, rm));
            }
            break;
        default:
            md_fault(runtime, opcode, ip_before);
            break;
    }
}

static inline void md_op_daa(MdX86 *cpu)
{
    md_x86_flags_materialize(cpu);   /* BCD adjusts stay eager (M18) */
    uint8_t al = md_x86_get_reg8(cpu, 0u);
    const uint8_t old_al = al;
    const int old_cf = (cpu->flags_raw & MD_X86_FLAG_CF) != 0u;
    int af = 0;
    int cf = 0;

    if ((al & 0x0Fu) > 9u || (cpu->flags_raw & MD_X86_FLAG_AF) != 0u) {
        al = (uint8_t)(al + 6u);
        af = 1;
    }
    /*
     * Original 8086 DAA/DAS upper-digit correction is not the later-x86
     * old_AL>99h rule and not a simple >9Fh threshold either.
     *
     * Physical 8086 behavior for the invalid-BCD window 9Ah..9Fh depends on
     * the incoming AF flag:
     *   AF=0 -> perform the +/-60h upper correction
     *   AF=1 -> do not perform the upper correction
     *
     * A0h..FFh always qualify, and incoming CF still forces correction.
     * flags_raw still contains the incoming AF here; the local `af` result is
     * committed only after this decision.
     */
    if (old_cf ||
        old_al > 0x9Fu ||
        (old_al > 0x99u && (cpu->flags_raw & MD_X86_FLAG_AF) == 0u)) {
        al = (uint8_t)(al + 0x60u);
        cf = 1;
    }
    cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_AF | MD_X86_FLAG_CF);
    if (af) cpu->flags_raw |= MD_X86_FLAG_AF;
    if (cf) cpu->flags_raw |= MD_X86_FLAG_CF;
    md_x86_set_reg8(cpu, 0u, al);
    md_x86_set_szp8(cpu, al);
}

static inline void md_op_das(MdX86 *cpu)
{
    md_x86_flags_materialize(cpu);   /* BCD adjusts stay eager (M18) */
    uint8_t al = md_x86_get_reg8(cpu, 0u);
    const uint8_t old_al = al;
    const int old_cf = (cpu->flags_raw & MD_X86_FLAG_CF) != 0u;
    int af = 0;
    int cf = 0;

    if ((al & 0x0Fu) > 9u || (cpu->flags_raw & MD_X86_FLAG_AF) != 0u) {
        al = (uint8_t)(al - 6u);
        af = 1;
    }
    /*
     * Original 8086 DAA/DAS upper-digit correction is not the later-x86
     * old_AL>99h rule and not a simple >9Fh threshold either.
     *
     * Physical 8086 behavior for the invalid-BCD window 9Ah..9Fh depends on
     * the incoming AF flag:
     *   AF=0 -> perform the +/-60h upper correction
     *   AF=1 -> do not perform the upper correction
     *
     * A0h..FFh always qualify, and incoming CF still forces correction.
     * flags_raw still contains the incoming AF here; the local `af` result is
     * committed only after this decision.
     */
    if (old_cf ||
        old_al > 0x9Fu ||
        (old_al > 0x99u && (cpu->flags_raw & MD_X86_FLAG_AF) == 0u)) {
        al = (uint8_t)(al - 0x60u);
        cf = 1;
    }
    cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_AF | MD_X86_FLAG_CF);
    if (af) cpu->flags_raw |= MD_X86_FLAG_AF;
    if (cf) cpu->flags_raw |= MD_X86_FLAG_CF;
    md_x86_set_reg8(cpu, 0u, al);
    md_x86_set_szp8(cpu, al);
}

static inline void md_op_aaa(MdX86 *cpu, int subtract)
{
    md_x86_flags_materialize(cpu);   /* BCD adjusts stay eager (M18) */
    uint8_t al = md_x86_get_reg8(cpu, 0u);
    uint8_t ah = md_x86_get_reg8(cpu, 4u);
    if ((al & 0x0Fu) > 9u || (cpu->flags_raw & MD_X86_FLAG_AF) != 0u) {
        al = subtract ? (uint8_t)(al - 6u) : (uint8_t)(al + 6u);
        ah = subtract ? (uint8_t)(ah - 1u) : (uint8_t)(ah + 1u);
        cpu->flags_raw |= (uint16_t)(MD_X86_FLAG_AF | MD_X86_FLAG_CF);
    } else {
        cpu->flags_raw &= (uint16_t)~(MD_X86_FLAG_AF | MD_X86_FLAG_CF);
    }
    md_x86_set_reg8(cpu, 0u, (uint8_t)(al & 0x0Fu));
    md_x86_set_reg8(cpu, 4u, ah);
}

static inline uint8_t md_port_in8(MdRuntime *runtime, uint16_t port)
{
    return runtime->hooks.in8 != NULL
        ? runtime->hooks.in8(runtime, port, runtime->hooks.user) : 0xFFu;
}

static inline void md_port_out8(MdRuntime *runtime, uint16_t port, uint8_t value)
{
    if (runtime->hooks.out8 != NULL) runtime->hooks.out8(runtime, port, value, runtime->hooks.user);
}

static inline uint16_t md_port_in16(MdRuntime *runtime, uint16_t port)
{
    const uint16_t lo = md_port_in8(runtime, port);
    const uint16_t hi = md_port_in8(runtime, (uint16_t)(port + 1u));
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static inline void md_port_out16(MdRuntime *runtime, uint16_t port, uint16_t value)
{
    md_port_out8(runtime, port, (uint8_t)value);
    md_port_out8(runtime, (uint16_t)(port + 1u), (uint8_t)(value >> 8));
}

static inline int md_execute_prefixed(MdRuntime *runtime, uint8_t first_prefix,
                                      uint16_t ip_before)
{
    MdPrefixState prefix;
    uint8_t opcode = first_prefix;
    unsigned count = 0u;

    memset(&prefix, 0, sizeof(prefix));
    while (md_is_prefix_byte(opcode)) {
        md_apply_prefix(&prefix, opcode);
        ++count;
        if (count >= 15u) {
            md_fault(runtime, first_prefix, ip_before);
            return 0;
        }
        opcode = md_fetch8(runtime);
    }
    MD_PREFIX_PROFILE_HIT(prefix.segment_override, opcode);
    MD_PREFIX_PROFILE_HIT(prefix.repeat, opcode);
    if (prefix.lock) MD_PREFIX_PROFILE_HIT(0xF0u, opcode);
    return md_execute_opcode(runtime, opcode, ip_before, &prefix);
}

static inline int md_execute_opcode(MdRuntime *runtime, uint8_t opcode, uint16_t ip_before, const MdPrefixState *prefix)
{
    MdX86 *cpu = &runtime->cpu;

    if ((opcode & 0xF8u) == 0xB0u) { md_op_mov_r8_imm(runtime, opcode); return 1; }
    if ((opcode & 0xF8u) == 0xB8u) { md_op_mov_r16_imm(runtime, opcode); return 1; }
    if ((opcode & 0xF8u) == 0x50u) { md_x86_push_reg(cpu, opcode & 7u); return 1; }
    if ((opcode & 0xF8u) == 0x58u) { cpu->r[opcode & 7u] = md_x86_pop(cpu); return 1; }
    if ((opcode & 0xF8u) == 0x40u) { md_op_inc_r16(runtime, opcode); return 1; }
    if ((opcode & 0xF8u) == 0x48u) { md_op_dec_r16(runtime, opcode); return 1; }

    if (opcode <= 0x3Bu && (opcode & 0x04u) == 0u) {
        md_op_alu_rm_r(runtime, opcode, prefix);
        return 1;
    }
    if (opcode <= 0x3Du && (opcode & 0x06u) == 0x04u) {
        md_op_alu_acc_imm(runtime, opcode);
        return 1;
    }

    if (opcode >= 0x60u && opcode <= 0x7Fu) {
        /* On the original 8086, bit 4 of the Jcc opcode is ignored. */
        const int8_t rel = (int8_t)md_fetch8(runtime);
        if (md_x86_condition(cpu, opcode & 0x0Fu)) cpu->ip = (uint16_t)(cpu->ip + rel);
        return 1;
    }

    if (opcode == 0x80u || opcode == 0x81u || opcode == 0x82u || opcode == 0x83u) {
        md_op_group1_imm(runtime, opcode, prefix);
        return 1;
    }
    if (opcode == 0x84u || opcode == 0x85u) {
        md_op_test_rm_r(runtime, opcode, prefix);
        return 1;
    }
    if (opcode == 0x86u || opcode == 0x87u) {
        md_op_xchg_rm_r(runtime, opcode, prefix);
        return 1;
    }
    if (opcode >= 0x88u && opcode <= 0x8Bu) {
        md_op_mov_rm_r(runtime, opcode, prefix);
        return 1;
    }
    if (opcode == 0x8Cu || opcode == 0x8Eu) {
        md_op_mov_sreg(runtime, opcode, ip_before, prefix);
        return runtime->stop_reason == MD_STOP_NONE;
    }
    if (opcode == 0x8Du) {
        md_op_lea(runtime, ip_before, prefix);
        return runtime->stop_reason == MD_STOP_NONE;
    }
    if (opcode == 0x8Fu) {
        md_op_pop_rm(runtime, ip_before, prefix);
        return runtime->stop_reason == MD_STOP_NONE;
    }
    if (opcode == 0xC4u || opcode == 0xC5u) {
        md_op_les_lds(runtime, opcode, ip_before, prefix);
        return runtime->stop_reason == MD_STOP_NONE;
    }
    if (opcode == 0xC6u || opcode == 0xC7u) {
        md_op_mov_rm_imm(runtime, opcode, ip_before, prefix);
        return runtime->stop_reason == MD_STOP_NONE;
    }
    if (opcode >= 0xD0u && opcode <= 0xD3u) {
        md_op_shift(runtime, opcode, prefix);
        return 1;
    }
    if (opcode == 0xF6u || opcode == 0xF7u) {
        md_op_group3(runtime, opcode, ip_before, prefix);
        return runtime->stop_reason == MD_STOP_NONE;
    }
    if (opcode == 0xFEu || opcode == 0xFFu) {
        md_op_group45(runtime, opcode, ip_before, prefix);
        return runtime->stop_reason == MD_STOP_NONE;
    }

    switch (opcode) {
        case 0x06: md_x86_push(cpu, cpu->es); break;
        case 0x07: cpu->es = md_x86_pop(cpu); break;
        case 0x0E: md_x86_push(cpu, cpu->cs); break;
        case 0x0F: cpu->cs = md_x86_pop(cpu); break; /* 8086 POP CS */
        case 0x16: md_x86_push(cpu, cpu->ss); break;
        case 0x17: cpu->ss = md_x86_pop(cpu); break;
        case 0x1E: md_x86_push(cpu, cpu->ds); break;
        case 0x1F: cpu->ds = md_x86_pop(cpu); break;
        case 0x27: md_op_daa(cpu); break;
        case 0x2F: md_op_das(cpu); break;
        case 0x37: md_op_aaa(cpu, 0); break;
        case 0x3F: md_op_aaa(cpu, 1); break;

        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: {
            const unsigned reg = opcode & 7u;
            const uint16_t value = cpu->r[MD_X86_AX];
            cpu->r[MD_X86_AX] = cpu->r[reg];
            cpu->r[reg] = value;
            break;
        }
        case 0x98: /* CBW */
            cpu->r[MD_X86_AX] = (uint16_t)(int16_t)(int8_t)md_x86_get_reg8(cpu, 0u);
            break;
        case 0x99: /* CWD */
            cpu->r[MD_X86_DX] = (cpu->r[MD_X86_AX] & 0x8000u) != 0u ? 0xFFFFu : 0u;
            break;
        case 0x9A: { /* CALL ptr16:16 */
            const uint16_t target_ip = md_fetch16(runtime);
            const uint16_t target_cs = md_fetch16(runtime);
            const uint16_t return_ip = cpu->ip;
            md_x86_push(cpu, cpu->cs);
            md_x86_push(cpu, return_ip);
            cpu->cs = target_cs;
            cpu->ip = target_ip;
            break;
        }
        case 0x9B: break; /* WAIT: no coprocessor scheduling in the core. */
        case 0x9C: md_x86_push(cpu, (uint16_t)(md_x86_flags(cpu) | MD_X86_FLAG_ALWAYS1)); break;
        case 0x9D: md_x86_set_flags(cpu, (uint16_t)(md_x86_pop(cpu) | MD_X86_FLAG_ALWAYS1)); break;
        case 0x9E: { /* SAHF */
            const uint16_t mask = MD_X86_FLAG_SF | MD_X86_FLAG_ZF | MD_X86_FLAG_AF |
                                  MD_X86_FLAG_PF | MD_X86_FLAG_CF;
            const uint16_t ah = md_x86_get_reg8(cpu, 4u);
            md_x86_update_flags(cpu, mask, (uint16_t)((ah & mask) | MD_X86_FLAG_ALWAYS1));
            break;
        }
        case 0x9F: /* LAHF */
            md_x86_set_reg8(cpu, 4u, (uint8_t)((md_x86_flags(cpu) & 0x00D5u) | 0x02u));
            break;

        case 0xA0: {
            const uint16_t segment = md_prefixed_segment(cpu, prefix, cpu->ds);
            md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, segment, md_fetch16(runtime)));
            break;
        }
        case 0xA1: {
            const uint16_t segment = md_prefixed_segment(cpu, prefix, cpu->ds);
            cpu->r[MD_X86_AX] = md_x86_read16(cpu, segment, md_fetch16(runtime));
            break;
        }
        case 0xA2: {
            const uint16_t segment = md_prefixed_segment(cpu, prefix, cpu->ds);
            md_x86_write8(cpu, segment, md_fetch16(runtime), md_x86_get_reg8(cpu, 0u));
            break;
        }
        case 0xA3: {
            const uint16_t segment = md_prefixed_segment(cpu, prefix, cpu->ds);
            md_x86_write16(cpu, segment, md_fetch16(runtime), cpu->r[MD_X86_AX]);
            break;
        }
        case 0xA8:
            (void)md_x86_logic8(cpu, (uint8_t)(md_x86_get_reg8(cpu, 0u) & md_fetch8(runtime)));
            break;
        case 0xA9:
            (void)md_x86_logic16(cpu, (uint16_t)(cpu->r[MD_X86_AX] & md_fetch16(runtime)));
            break;

        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        case 0xAE: case 0xAF:
            md_op_string(runtime, opcode, prefix);
            break;

        case 0xC0: /* original-8086 alias of C2 */
        case 0xC2: {
            const uint16_t adjust = md_fetch16(runtime);
            cpu->ip = md_x86_pop(cpu);
            cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + adjust);
            break;
        }
        case 0xC1: /* original-8086 alias of C3 */
        case 0xC3: cpu->ip = md_x86_pop(cpu); break;
        case 0xC8: /* original-8086 alias of CA */
        case 0xCA: {
            const uint16_t adjust = md_fetch16(runtime);
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + adjust);
            break;
        }
        case 0xC9: /* original-8086 alias of CB */
        case 0xCB:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            break;
        case 0xCC:
            (void)md_runtime_interrupt(runtime, 3u);
            break;
        case 0xCD: {
            const uint8_t vector = md_fetch8(runtime);
            (void)md_runtime_interrupt(runtime, vector);
            break;
        }
        case 0xCE:
            if (md_x86_of(cpu)) (void)md_runtime_interrupt(runtime, 4u);
            break;
        case 0xCF:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            md_x86_set_flags(cpu, (uint16_t)(md_x86_pop(cpu) | MD_X86_FLAG_ALWAYS1));
            break;
        case 0xD4: { /* AAM imm8 */
            const uint8_t base = md_fetch8(runtime);
            const uint8_t al = md_x86_get_reg8(cpu, 0u);
            if (base == 0u) { md_x86_set_szp8(cpu, 0u); md_divide_error(runtime); break; }
            md_x86_set_reg8(cpu, 4u, (uint8_t)(al / base));
            md_x86_set_reg8(cpu, 0u, (uint8_t)(al % base));
            md_x86_set_szp8(cpu, md_x86_get_reg8(cpu, 0u));
            break;
        }
        case 0xD5: { /* AAD imm8 */
            const uint8_t base = md_fetch8(runtime);
            const uint8_t al = md_x86_get_reg8(cpu, 0u);
            const uint8_t ah = md_x86_get_reg8(cpu, 4u);
            const uint8_t value = (uint8_t)((uint16_t)al + (uint16_t)ah * base);
            md_x86_set_reg8(cpu, 0u, value);
            md_x86_set_reg8(cpu, 4u, 0u);
            md_x86_set_szp8(cpu, value);
            break;
        }
        case 0xD6: /* SALC, undocumented but present on original 8086 */
            md_x86_set_reg8(cpu, 0u, md_x86_cf(cpu) ? 0xFFu : 0u);
            break;
        case 0xD7: { /* XLAT */
            const uint16_t segment = md_prefixed_segment(cpu, prefix, cpu->ds);
            const uint16_t offset = (uint16_t)(cpu->r[MD_X86_BX] + md_x86_get_reg8(cpu, 0u));
            md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, segment, offset));
            break;
        }
        case 0xE8: {
            const int16_t rel = (int16_t)md_fetch16(runtime);
            md_x86_push(cpu, cpu->ip);
            cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
            md_op_loop(runtime, opcode);
            break;
        case 0xE4: md_x86_set_reg8(cpu, 0u, md_port_in8(runtime, md_fetch8(runtime))); break;
        case 0xE5: cpu->r[MD_X86_AX] = md_port_in16(runtime, md_fetch8(runtime)); break;
        case 0xE6: md_port_out8(runtime, md_fetch8(runtime), md_x86_get_reg8(cpu, 0u)); break;
        case 0xE7: md_port_out16(runtime, md_fetch8(runtime), cpu->r[MD_X86_AX]); break;
        case 0xE9: {
            const int16_t rel = (int16_t)md_fetch16(runtime);
            cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0xEA: {
            const uint16_t target_ip = md_fetch16(runtime);
            const uint16_t target_cs = md_fetch16(runtime);
            cpu->ip = target_ip;
            cpu->cs = target_cs;
            break;
        }
        case 0xEB: {
            const int8_t rel = (int8_t)md_fetch8(runtime);
            cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0xEC: md_x86_set_reg8(cpu, 0u, md_port_in8(runtime, cpu->r[MD_X86_DX])); break;
        case 0xED: cpu->r[MD_X86_AX] = md_port_in16(runtime, cpu->r[MD_X86_DX]); break;
        case 0xEE: md_port_out8(runtime, cpu->r[MD_X86_DX], md_x86_get_reg8(cpu, 0u)); break;
        case 0xEF: md_port_out16(runtime, cpu->r[MD_X86_DX], cpu->r[MD_X86_AX]); break;
        case 0xF4: runtime->stop_reason = MD_STOP_HALT; break;
        case 0xF5: md_x86_update_flags(cpu, MD_X86_FLAG_CF, md_x86_cf(cpu) ? 0u : MD_X86_FLAG_CF); break;
        case 0xF8: md_x86_update_flags(cpu, MD_X86_FLAG_CF, 0u); break;
        case 0xF9: md_x86_update_flags(cpu, 0u, MD_X86_FLAG_CF); break;
        /* IF/DF are never lazy: the raw word is authoritative for them */
        case 0xFA: cpu->flags_raw &= (uint16_t)~MD_X86_FLAG_IF; break;
        case 0xFB: cpu->flags_raw |= MD_X86_FLAG_IF; break;
        case 0xFC: cpu->flags_raw &= (uint16_t)~MD_X86_FLAG_DF; break;
        case 0xFD: cpu->flags_raw |= MD_X86_FLAG_DF; break;

        default:
            md_fault(runtime, opcode, ip_before);
            return 0;
    }
    return 1;
}

#if defined(MD_THREADED_DISPATCH) && (defined(__GNUC__) || defined(__clang__))

#if defined(MICRODOS_ENABLE_NATIVE_V2) && defined(MICRODOS_NATIVE_V2_BACKEDGE_PROFILE)
static inline void md_nv2_profile_backedge(MdRuntime *runtime,
                                           uint8_t opcode,
                                           uint16_t source_ip,
                                           uint16_t target_ip)
{
    MdNativeV2BackedgeSite *set;
    MdNativeV2BackedgeSite *slot = NULL;
    MdNativeV2BackedgeSite *weakest = NULL;
    unsigned hash;
    unsigned base;
    unsigned i;

    ++runtime->native_v2_backedge_hits;

    hash = ((unsigned)runtime->cpu.cs * 33u) ^
           ((unsigned)target_ip * 17u) ^
           (unsigned)source_ip ^
           (unsigned)opcode;
    base = hash & (MD_NATIVE_V2_BACKEDGE_SLOTS - 4u);
    base &= ~3u;
    set = &runtime->native_v2_backedge[base];

    for (i = 0u; i < 4u; ++i) {
        MdNativeV2BackedgeSite *s = &set[i];

        if (s->hits != 0u &&
            s->cs == runtime->cpu.cs &&
            s->source_ip == source_ip &&
            s->target_ip == target_ip &&
            s->opcode == opcode) {
            ++s->hits;
            return;
        }

        if (s->hits == 0u && slot == NULL)
            slot = s;

        if (weakest == NULL || s->hits < weakest->hits)
            weakest = s;
    }

    if (slot == NULL)
        slot = weakest;

    if (slot != NULL) {
        const uint32_t linear =
            md_x86_linear(runtime->cpu.cs, target_ip);
        unsigned n = MD_NATIVE_V2_BACKEDGE_BYTES;

        memset(slot, 0, sizeof(*slot));
        slot->cs = runtime->cpu.cs;
        slot->source_ip = source_ip;
        slot->target_ip = target_ip;
        slot->opcode = opcode;
        slot->hits = 1u;

        if (linear + n > MD_X86_ADDRESS_SPACE)
            n = (unsigned)(MD_X86_ADDRESS_SPACE - linear);

        slot->bytes_len = (uint8_t)n;
        if (n != 0u)
            memcpy(slot->bytes, runtime->cpu.memory + linear, n);
    }
}
#define MD_NV2_PROFILE_BACKEDGE(op_, source_, target_) \
    md_nv2_profile_backedge(runtime, (uint8_t)(op_), \
                            (uint16_t)(source_), (uint16_t)(target_))
#else
#define MD_NV2_PROFILE_BACKEDGE(op_, source_, target_) ((void)0)
#endif

/* M16: one threaded loop serves both md_interp_run() and
   md_interp_run_until_cs_change() (watch_cs != 0: return MD_STOP_NONE as
   soon as CS differs from its value on entry). Per-instruction counters are
   32-bit locals; runtime->instructions is updated on every exit and before
   interrupt hooks run, so observers still see exact counts. */
static MdStopReason md_interp_run_threaded(MdRuntime *runtime, uint32_t instruction_budget,
                                           int watch_cs)
{
    static void *dispatch[256] = {0};
    static int initialized = 0;
    uint8_t opcode;
    uint16_t ip_before;
    uint32_t remaining = instruction_budget;
    uint32_t done = 0u;
    const uint16_t cs0 = runtime->cpu.cs;
    MdX86 *const opcode_cpu = &runtime->cpu;
    uint8_t *const opcode_memory = opcode_cpu->memory;
    uint32_t opcode_cs_base = ((uint32_t)opcode_cpu->cs) << 4;
    uint32_t stack_ss_base = ((uint32_t)opcode_cpu->ss) << 4;
    unsigned i;

    if (!initialized) {
        for (i = 0; i < 256u; ++i) dispatch[i] = &&op_generic;
        for (i = 0xB0u; i <= 0xB7u; ++i) dispatch[i] = &&op_mov_r8_imm;
        for (i = 0xB8u; i <= 0xBFu; ++i) dispatch[i] = &&op_mov_r16_imm;
        for (i = 0x40u; i <= 0x47u; ++i) dispatch[i] = &&op_inc_r16;
        for (i = 0x48u; i <= 0x4Fu; ++i) dispatch[i] = &&op_dec_r16;
        for (i = 0x50u; i <= 0x57u; ++i) dispatch[i] = &&op_push_r16;
        for (i = 0x58u; i <= 0x5Fu; ++i) dispatch[i] = &&op_pop_r16;
        dispatch[0x50] = &&op_push_ax;
        dispatch[0x58] = &&op_pop_ax;
        for (i = 0x60u; i <= 0x7Fu; ++i) dispatch[i] = &&op_jcc8;
        dispatch[0x74] = &&op_jz8;
        dispatch[0x75] = &&op_jnz8;
        dispatch[0x3C] = &&op_cmp_al_imm8;
        /* M16: direct entries for the opcodes that dominate the MS-DOS 2.0
           kernel/COMMAND.COM mix (profiled on DOS2TEST). They call the same
           md_op_* semantics as md_execute_opcode, skipping its range-test
           chain. Prefixed forms still take op_prefix. */
        for (i = 0x00u; i <= 0x3Bu; ++i) if ((i & 0x04u) == 0u) dispatch[i] = &&op_alu_rm;
        for (i = 0x00u; i <= 0x3Du; ++i) if ((i & 0x06u) == 0x04u) dispatch[i] = &&op_alu_acc;
        for (i = 0x80u; i <= 0x83u; ++i) dispatch[i] = &&op_group1;
        for (i = 0x88u; i <= 0x8Bu; ++i) dispatch[i] = &&op_mov_rm;
        for (i = 0xE0u; i <= 0xE3u; ++i) dispatch[i] = &&op_loop;
        for (i = 0xA4u; i <= 0xA7u; ++i) dispatch[i] = &&op_string;
        for (i = 0xAAu; i <= 0xAFu; ++i) dispatch[i] = &&op_string;
        dispatch[0x06] = &&op_push_sreg;
        dispatch[0x0E] = &&op_push_sreg;
        dispatch[0x16] = &&op_push_sreg;
        dispatch[0x1E] = &&op_push_sreg;
        dispatch[0x07] = &&op_pop_sreg;
        dispatch[0x17] = &&op_pop_sreg;
        dispatch[0x1F] = &&op_pop_sreg;
        dispatch[0x16] = &&op_push_ss_hot;
        dispatch[0x1F] = &&op_pop_ds_hot;
        dispatch[0x07] = &&op_pop_es_hot;
        dispatch[0xC3] = &&op_ret;
        dispatch[0xE8] = &&op_call16;
        dispatch[0xE9] = &&op_jmp16;
        dispatch[0x26] = &&op_prefix;
        dispatch[0x2E] = &&op_prefix;
        dispatch[0x36] = &&op_prefix;
        dispatch[0x3E] = &&op_prefix;
        dispatch[0xF0] = &&op_prefix;
        dispatch[0xF2] = &&op_prefix;
        dispatch[0xF3] = &&op_prefix;
        dispatch[0x90] = &&op_nop;
        dispatch[0xCD] = &&op_int;
        dispatch[0xEB] = &&op_jmp8;
        dispatch[0xF4] = &&op_hlt;
        dispatch[0xF6] = &&op_group3;
        dispatch[0xF7] = &&op_group3;
        initialized = 1;
    }

/* CS can only change through INT, far CALL/JMP/RET, IRET or a prefixed form
   of those, which all reach op_int, op_generic or op_prefix. Checking CS
   there instead of in every MD_NEXT keeps the fast paths at full speed
   (a per-instruction check cost ~35% on the DEC/JNZ benchmark). */
#define MD_NEXT_CS() do { \
        if (watch_cs && runtime->cpu.cs != cs0) goto md_exit; \
        opcode_cs_base = ((uint32_t)opcode_cpu->cs) << 4; \
        stack_ss_base = ((uint32_t)opcode_cpu->ss) << 4; \
        MD_NEXT(); \
    } while (0)

#define MD_NEXT() do { \
        if (runtime->stop_reason != MD_STOP_NONE) goto md_exit; \
        if (remaining == 0u) { runtime->stop_reason = MD_STOP_BUDGET; goto md_exit; } \
        --remaining; \
        ++done; \
        ip_before = opcode_cpu->ip; \
        opcode = opcode_memory[(opcode_cs_base + (uint32_t)ip_before) & MD_X86_ADDRESS_MASK]; \
        opcode_cpu->ip = (uint16_t)(ip_before + 1u); \
        MD_OPCODE_PROFILE_HIT(opcode); \
        goto *dispatch[opcode]; \
    } while (0)

    MD_NEXT();

/*
 * M24.2: threaded stack locality.
 *
 * The threaded interpreter already keeps stack_ss_base synchronized with SS.
 * Use it for the common stack word access instead of reforming SS:SP through
 * md_x86_push()/md_x86_pop() on every CALL/RET/PUSH/POP.
 *
 * Tracked builds retain exactly the existing SMC/AOT rule: direct writes are
 * used only when every touched page is non-executable; otherwise the shared
 * tracked-store helper performs generation/AOT invalidation.
 *
 * SP=FFFFh remains on the generic segmented-word path because original 8086
 * word semantics fetch/store the high byte at SS:0000, not linear a0+1.
 */
#if defined(MD_X86_TRACK_WRITES) && !MD_X86_TRACK_WRITES
#define MD_STACK_PUSH16_FAST(value_) do { \
        const uint16_t md_stack_value_ = (uint16_t)(value_); \
        const uint16_t md_stack_sp_ = (uint16_t)(opcode_cpu->r[MD_X86_SP] - 2u); \
        const uint32_t md_stack_a0_ = (stack_ss_base + (uint32_t)md_stack_sp_) & MD_X86_ADDRESS_MASK; \
        opcode_cpu->r[MD_X86_SP] = md_stack_sp_; \
        if (md_stack_sp_ != 0xFFFFu) { \
            opcode_memory[md_stack_a0_] = (uint8_t)md_stack_value_; \
            opcode_memory[(md_stack_a0_ + 1u) & MD_X86_ADDRESS_MASK] = \
                (uint8_t)(md_stack_value_ >> 8); \
        } else { \
            md_x86_write16(opcode_cpu, opcode_cpu->ss, md_stack_sp_, md_stack_value_); \
        } \
    } while (0)
#define MD_STACK_POP16_FAST(dst_) do { \
        const uint16_t md_stack_sp_ = opcode_cpu->r[MD_X86_SP]; \
        if (md_stack_sp_ != 0xFFFFu) { \
            const uint32_t md_stack_a0_ = \
                (stack_ss_base + (uint32_t)md_stack_sp_) & MD_X86_ADDRESS_MASK; \
            (dst_) = (uint16_t)((uint16_t)opcode_memory[md_stack_a0_] | \
                     ((uint16_t)opcode_memory[(md_stack_a0_ + 1u) & \
                                              MD_X86_ADDRESS_MASK] << 8)); \
        } else { \
            (dst_) = md_x86_read16(opcode_cpu, opcode_cpu->ss, md_stack_sp_); \
        } \
        opcode_cpu->r[MD_X86_SP] = (uint16_t)(md_stack_sp_ + 2u); \
    } while (0)
#else
#define MD_STACK_PUSH16_FAST(value_) do { \
        const uint16_t md_stack_value_ = (uint16_t)(value_); \
        const uint16_t md_stack_sp_ = (uint16_t)(opcode_cpu->r[MD_X86_SP] - 2u); \
        const uint32_t md_stack_a0_ = \
            (stack_ss_base + (uint32_t)md_stack_sp_) & MD_X86_ADDRESS_MASK; \
        opcode_cpu->r[MD_X86_SP] = md_stack_sp_; \
        if (md_stack_sp_ != 0xFFFFu) { \
            const uint32_t md_stack_a1_ = \
                (md_stack_a0_ + 1u) & MD_X86_ADDRESS_MASK; \
            if (!md_x86_page_executable(opcode_cpu, md_stack_a0_) && \
                ((md_stack_a0_ & MD_X86_CODE_PAGE_MASK) != MD_X86_CODE_PAGE_MASK || \
                 !md_x86_page_executable(opcode_cpu, md_stack_a1_))) { \
                opcode_memory[md_stack_a0_] = (uint8_t)md_stack_value_; \
                opcode_memory[md_stack_a1_] = (uint8_t)(md_stack_value_ >> 8); \
            } else { \
                md_x86_store16_tracked(opcode_cpu, md_stack_a0_, md_stack_value_); \
            } \
        } else { \
            md_x86_write16(opcode_cpu, opcode_cpu->ss, md_stack_sp_, md_stack_value_); \
        } \
    } while (0)
#define MD_STACK_POP16_FAST(dst_) do { \
        const uint16_t md_stack_sp_ = opcode_cpu->r[MD_X86_SP]; \
        if (md_stack_sp_ != 0xFFFFu) { \
            const uint32_t md_stack_a0_ = \
                (stack_ss_base + (uint32_t)md_stack_sp_) & MD_X86_ADDRESS_MASK; \
            (dst_) = (uint16_t)((uint16_t)opcode_memory[md_stack_a0_] | \
                     ((uint16_t)opcode_memory[(md_stack_a0_ + 1u) & \
                                              MD_X86_ADDRESS_MASK] << 8)); \
        } else { \
            (dst_) = md_x86_read16(opcode_cpu, opcode_cpu->ss, md_stack_sp_); \
        } \
        opcode_cpu->r[MD_X86_SP] = (uint16_t)(md_stack_sp_ + 2u); \
    } while (0)
#endif

/*
 * M24.3: cached segment hot-prefix locality.
 *
 * M24.2 proved that keeping SS<<4 in a threaded-loop local materially helps
 * DOS stack traffic.  The exact hot prefix templates below still rebuild
 * SS:/CS: segmented addresses through md_x86_read/write helpers.
 *
 * Reuse stack_ss_base and opcode_cs_base for those known-hot templates.
 * The linear write helpers retain the normal translation/AOT invalidation
 * checks.  Offset FFFFh deliberately falls back to the segmented helpers so
 * the high byte wraps to segment:0000 as on the original 8086.
 */
#define MD_CACHED_SEG_READ8(base_, off_) \
    md_x86_read8_linear(opcode_cpu, \
        ((uint32_t)(base_) + (uint32_t)(uint16_t)(off_)) & MD_X86_ADDRESS_MASK)

#define MD_CACHED_SEG_READ16(base_, seg_, off_) \
    __extension__ ({ \
        const uint16_t md_seg_off_ = (uint16_t)(off_); \
        const uint16_t md_seg_value_ = \
            md_seg_off_ != 0xFFFFu \
                ? md_x86_read16_linear( \
                      opcode_cpu, \
                      ((uint32_t)(base_) + (uint32_t)md_seg_off_) & \
                          MD_X86_ADDRESS_MASK) \
                : md_x86_read16(opcode_cpu, (uint16_t)(seg_), md_seg_off_); \
        md_seg_value_; \
    })

#define MD_CACHED_SEG_WRITE8(base_, off_, value_) do { \
        md_x86_write8_linear( \
            opcode_cpu, \
            ((uint32_t)(base_) + (uint32_t)(uint16_t)(off_)) & \
                MD_X86_ADDRESS_MASK, \
            (uint8_t)(value_)); \
    } while (0)

#define MD_CACHED_SEG_WRITE16(base_, seg_, off_, value_) do { \
        const uint16_t md_seg_off_ = (uint16_t)(off_); \
        const uint16_t md_seg_value_ = (uint16_t)(value_); \
        if (md_seg_off_ != 0xFFFFu) { \
            md_x86_write16_linear( \
                opcode_cpu, \
                ((uint32_t)(base_) + (uint32_t)md_seg_off_) & \
                    MD_X86_ADDRESS_MASK, \
                md_seg_value_); \
        } else { \
            md_x86_write16( \
                opcode_cpu, (uint16_t)(seg_), md_seg_off_, md_seg_value_); \
        } \
    } while (0)

/*
 * M24.1: threaded code-fetch locality.
 *
 * MD_NEXT already fetches opcodes through the cached CS linear base.  Hot
 * handlers used to fall back to md_fetch8/md_fetch16 for every ModR/M,
 * displacement and immediate byte, which recomputed CS:IP linear addresses.
 *
 * These helpers are local to the GCC/Clang threaded interpreter.  FETCH16
 * deliberately treats IP=FFFFh as a segmented word access: the high byte is
 * fetched from CS:0000, matching the original 8086 and md_fetch16().
 */
#define MD_CODE_PEEK8() \
    opcode_memory[(opcode_cs_base + (uint32_t)opcode_cpu->ip) & MD_X86_ADDRESS_MASK]

#define MD_CODE_FETCH8() \
    opcode_memory[(opcode_cs_base + \
                   (uint32_t)(uint16_t)(opcode_cpu->ip++)) & \
                  MD_X86_ADDRESS_MASK]

#define MD_CODE_FETCH16() \
    __extension__ ({ \
        const uint16_t md_code_ip_ = opcode_cpu->ip; \
        const uint32_t md_code_a0_ = \
            (opcode_cs_base + (uint32_t)md_code_ip_) & MD_X86_ADDRESS_MASK; \
        const uint32_t md_code_a1_ = \
            md_code_ip_ == 0xFFFFu \
                ? (opcode_cs_base & MD_X86_ADDRESS_MASK) \
                : ((md_code_a0_ + 1u) & MD_X86_ADDRESS_MASK); \
        const uint16_t md_code_value_ = \
            (uint16_t)((uint16_t)opcode_memory[md_code_a0_] | \
                       ((uint16_t)opcode_memory[md_code_a1_] << 8)); \
        opcode_cpu->ip = (uint16_t)(md_code_ip_ + 2u); \
        md_code_value_; \
    })

op_mov_r8_imm:
    md_x86_set_reg8(opcode_cpu, opcode & 7u, MD_CODE_FETCH8());
    MD_NEXT();

op_mov_r16_imm:
    opcode_cpu->r[opcode & 7u] = MD_CODE_FETCH16();
    MD_NEXT();

op_inc_r16:
    md_op_inc_r16(runtime, opcode);
    MD_NEXT();

op_dec_r16:
    md_op_dec_r16(runtime, opcode);
    MD_NEXT();

op_push_ax:
    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_AX]);
    MD_NEXT();

op_pop_ax:
    MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_AX]);
    MD_NEXT();

op_push_r16: {
    const unsigned reg = opcode & 7u;
    /*
     * Original 8086 PUSH SP stores the already-decremented SP. Compute that
     * value before the macro performs the actual decrement/store.
     */
    const uint16_t value =
        reg == MD_X86_SP
            ? (uint16_t)(opcode_cpu->r[MD_X86_SP] - 2u)
            : opcode_cpu->r[reg];
    MD_STACK_PUSH16_FAST(value);
    MD_NEXT();
}

op_pop_r16: {
    const unsigned reg = opcode & 7u;
    if (reg == MD_X86_SP) {
        /*
         * POP SP's final SP is the popped value, not old-SP+2. Keep the
         * canonical helper for this one architectural aliasing case.
         */
        opcode_cpu->r[MD_X86_SP] = md_x86_pop(opcode_cpu);
    } else {
        MD_STACK_POP16_FAST(opcode_cpu->r[reg]);
    }
    MD_NEXT();
}

op_cmp_al_imm8: {
    const uint8_t rhs = MD_CODE_FETCH8();
    (void)md_x86_alu8(opcode_cpu, 7u,
                      md_x86_get_reg8(opcode_cpu, 0u), rhs);
    MD_NEXT();
}

op_jz8: {
    const int8_t rel = (int8_t)MD_CODE_FETCH8();
    if (md_x86_zf(&runtime->cpu)) {
        const uint16_t target = (uint16_t)(runtime->cpu.ip + rel);
        runtime->cpu.ip = target;
        if (rel < 0)
            MD_NV2_PROFILE_BACKEDGE(opcode, ip_before, target);
    }
    MD_NEXT();
}

op_jnz8: {
    const int8_t rel = (int8_t)MD_CODE_FETCH8();
    if (!md_x86_zf(&runtime->cpu)) {
        const uint16_t target = (uint16_t)(runtime->cpu.ip + rel);
        runtime->cpu.ip = target;
        if (rel < 0)
            MD_NV2_PROFILE_BACKEDGE(opcode, ip_before, target);
#if MD_INTERP_BACKEDGE_EXIT
        if (rel < 0) {
            const unsigned bit =
                ((unsigned)runtime->cpu.cs ^ (unsigned)target) & 63u;
            const uint32_t mask = (uint32_t)1u << (bit & 31u);
            if ((runtime->native_v2_suppress_bloom[bit >> 5] & mask) == 0u) {
                runtime->native_v2_backedge_cs = runtime->cpu.cs;
                runtime->native_v2_backedge_ip = target;
                runtime->native_v2_backedge_hit = 1u;
                goto md_exit;
            }
        }
#endif
    }
    MD_NEXT();
}

op_jcc8: {
    const int8_t rel = (int8_t)MD_CODE_FETCH8();
    if (md_x86_condition(&runtime->cpu, opcode & 0x0Fu)) {
        const uint16_t target = (uint16_t)(runtime->cpu.ip + rel);
        runtime->cpu.ip = target;
        if (rel < 0)
            MD_NV2_PROFILE_BACKEDGE(opcode, ip_before, target);
#if MD_INTERP_BACKEDGE_ALL
        if (rel < 0) {
            const unsigned bit = ((unsigned)runtime->cpu.cs ^ (unsigned)target) & 63u;
            const uint32_t mask = (uint32_t)1u << (bit & 31u);
            if ((runtime->native_v2_suppress_bloom[bit >> 5] & mask) == 0u) {
                runtime->native_v2_backedge_cs = runtime->cpu.cs;
                runtime->native_v2_backedge_ip = target;
                runtime->native_v2_backedge_hit = 1u;
                goto md_exit;
            }
        }
#endif
    }
    MD_NEXT();
}

op_prefix: {
    /* Real DOS overwhelmingly uses a single segment override.  Avoid the
       general prefix parser for that hot case, but leave repeated/mixed
       prefixes, REP and LOCK on the fully general path below. */
    if (opcode == 0x26u || opcode == 0x2Eu || opcode == 0x36u || opcode == 0x3Eu) {
        const uint8_t next_opcode = MD_CODE_PEEK8();
        if (!md_is_prefix_byte(next_opcode)) {
            MdPrefixState prefix = { opcode, 0u, 0u };
            (void)MD_CODE_FETCH8();  /* consume the already-peeked opcode */
#if MD_INTERP_OPCODE_PROFILE
            MD_PREFIX_PROFILE_HIT(opcode, next_opcode);
#endif

#if MD_INTERP_OPCODE_PROFILE
            {
                const unsigned hot_row = md_hot_modrm_profile_row(opcode, next_opcode);
                if (hot_row < 10u) {
                    const uint8_t modrm = MD_CODE_PEEK8();
                    MD_HOT_MODRM_PROFILE_HIT(opcode, next_opcode, modrm);
                }
            }
#endif

            /* Profile-guided hot pairs.  Reuse the exact existing helpers;
               only skip md_execute_opcode()'s classification chain. */
            /* Profile-guided exact ModR/M templates.
               These eleven byte patterns account for ~10.8% of the measured
               COMMAND/DOS stream. Peek first so every non-match falls back
               without consuming anything. */
            {
                const uint8_t hot_modrm = MD_CODE_PEEK8();

                if (opcode == 0x2Eu && next_opcode == 0xFFu && hot_modrm == 0x36u) {
                    uint16_t disp;
                    uint16_t value;
                    (void)MD_CODE_FETCH8();
                    disp = MD_CODE_FETCH16();
                    value = MD_CACHED_SEG_READ16(
                        opcode_cs_base, opcode_cpu->cs, disp);
                    MD_STACK_PUSH16_FAST(value);
                    MD_NEXT();
                }

                if (opcode == 0x2Eu && next_opcode == 0x8Fu && hot_modrm == 0x06u) {
                    uint16_t disp;
                    uint16_t value;
                    (void)MD_CODE_FETCH8();
                    disp = MD_CODE_FETCH16();
                    MD_STACK_POP16_FAST(value);
                    MD_CACHED_SEG_WRITE16(
                        opcode_cs_base, opcode_cpu->cs, disp, value);
                    MD_NEXT();
                }

                if (opcode == 0x36u && next_opcode == 0xFFu && hot_modrm == 0x1Eu) {
                    uint16_t disp;
                    uint16_t target_ip;
                    uint16_t target_cs;
                    uint16_t return_ip;
                    (void)MD_CODE_FETCH8();
                    disp = MD_CODE_FETCH16();
                    target_ip = MD_CACHED_SEG_READ16(
                        stack_ss_base, opcode_cpu->ss, disp);
                    target_cs = MD_CACHED_SEG_READ16(
                        stack_ss_base, opcode_cpu->ss,
                        (uint16_t)(disp + 2u));
                    return_ip = opcode_cpu->ip;
                    MD_STACK_PUSH16_FAST(opcode_cpu->cs);
                    MD_STACK_PUSH16_FAST(return_ip);
                    opcode_cpu->cs = target_cs;
                    opcode_cpu->ip = target_ip;
                    MD_NEXT_CS();
                }

                if (opcode == 0x36u && next_opcode == 0xC7u && hot_modrm == 0x06u) {
                    uint16_t disp;
                    uint16_t imm;
                    (void)MD_CODE_FETCH8();
                    disp = MD_CODE_FETCH16();
                    imm = MD_CODE_FETCH16();
                    MD_CACHED_SEG_WRITE16(
                        stack_ss_base, opcode_cpu->ss, disp, imm);
                    MD_NEXT();
                }

                if (opcode == 0x36u && next_opcode == 0x80u && hot_modrm == 0x3Eu) {
                    uint16_t disp;
                    uint8_t lhs;
                    uint8_t rhs;
                    (void)MD_CODE_FETCH8();
                    disp = MD_CODE_FETCH16();
                    lhs = MD_CACHED_SEG_READ8(stack_ss_base, disp);
                    rhs = MD_CODE_FETCH8();
                    (void)md_alu8(opcode_cpu, 7u, lhs, rhs);
                    MD_NEXT();
                }

                if (opcode == 0x36u && next_opcode == 0x8Cu &&
                    (hot_modrm == 0x1Eu || hot_modrm == 0x16u)) {
                    uint16_t disp;
                    uint16_t value;
                    (void)MD_CODE_FETCH8();
                    disp = MD_CODE_FETCH16();
                    value = hot_modrm == 0x1Eu ? opcode_cpu->ds : opcode_cpu->ss;
                    MD_CACHED_SEG_WRITE16(
                        stack_ss_base, opcode_cpu->ss, disp, value);
                    MD_NEXT();
                }

                if (opcode == 0x36u && next_opcode == 0x8Bu && hot_modrm == 0x3Eu) {
                    uint16_t disp;
                    (void)MD_CODE_FETCH8();
                    disp = MD_CODE_FETCH16();
                    opcode_cpu->r[MD_X86_DI] = MD_CACHED_SEG_READ16(
                        stack_ss_base, opcode_cpu->ss, disp);
                    MD_NEXT();
                }

                if (opcode == 0x26u && next_opcode == 0x8Au && hot_modrm == 0x05u) {
                    (void)MD_CODE_FETCH8();
                    md_x86_set_reg8(&runtime->cpu, 0u,
                                    md_x86_read8(&runtime->cpu, runtime->cpu.es,
                                                 runtime->cpu.r[MD_X86_DI]));
                    MD_NEXT();
                }

                if (opcode == 0x26u &&
                    (next_opcode == 0x03u || next_opcode == 0x2Bu) &&
                    hot_modrm == 0x45u) {
                    int8_t disp;
                    uint16_t rhs;
                    unsigned alu_op;
                    (void)MD_CODE_FETCH8();
                    disp = (int8_t)MD_CODE_FETCH8();
                    rhs = md_x86_read16(&runtime->cpu, runtime->cpu.es,
                                        (uint16_t)(runtime->cpu.r[MD_X86_DI] + disp));
                    alu_op = next_opcode == 0x03u ? 0u : 5u;
                    runtime->cpu.r[MD_X86_AX] =
                        md_alu16(&runtime->cpu, alu_op,
                                 runtime->cpu.r[MD_X86_AX], rhs);
                    MD_NEXT();
                }
            }

            if (opcode == 0x36u) { /* SS: */
                switch (next_opcode) {
                    case 0xA3u: {
                        const uint16_t off = MD_CODE_FETCH16();
                        MD_CACHED_SEG_WRITE16(
                            stack_ss_base, opcode_cpu->ss, off,
                            opcode_cpu->r[MD_X86_AX]);
                        MD_NEXT();
                    }
                    case 0x8Cu:
                        md_op_mov_sreg(runtime, next_opcode, ip_before, &prefix);
                        MD_NEXT_CS();
                    case 0xFFu:
                        md_op_group45(runtime, next_opcode, ip_before, &prefix);
                        MD_NEXT_CS();
                    case 0xC7u:
                        md_op_mov_rm_imm(runtime, next_opcode, ip_before, &prefix);
                        MD_NEXT_CS();
                    case 0x80u:
                        md_op_group1_imm(runtime, next_opcode, &prefix);
                        MD_NEXT_CS();
                    case 0x8Bu:
                        md_op_mov_rm_r(runtime, next_opcode, &prefix);
                        MD_NEXT_CS();
                    case 0xA1u: {
                        const uint16_t off = MD_CODE_FETCH16();
                        opcode_cpu->r[MD_X86_AX] = MD_CACHED_SEG_READ16(
                            stack_ss_base, opcode_cpu->ss, off);
                        MD_NEXT();
                    }
                    default:
                        break;
                }
            } else if (opcode == 0x2Eu) { /* CS: */
                if (next_opcode == 0xFFu) {
                    md_op_group45(runtime, next_opcode, ip_before, &prefix);
                    MD_NEXT_CS();
                }
                if (next_opcode == 0x8Fu) {
                    md_op_pop_rm(runtime, ip_before, &prefix);
                    MD_NEXT_CS();
                }
            } else if (opcode == 0x26u) { /* ES: */
                if (next_opcode == 0x8Au) {
                    md_op_mov_rm_r(runtime, next_opcode, &prefix);
                    MD_NEXT_CS();
                }
                if (next_opcode == 0x03u || next_opcode == 0x2Bu) {
                    md_op_alu_rm_r(runtime, next_opcode, &prefix);
                    MD_NEXT_CS();
                }
            }

            (void)md_execute_opcode(runtime, next_opcode, ip_before, &prefix);
            MD_NEXT_CS();
        }
    }
    (void)md_execute_prefixed(runtime, opcode, ip_before);
    MD_NEXT_CS();
}

op_alu_rm:
#if MD_INTERP_OPCODE_PROFILE
    if (md_unpref_modrm_profile_row(opcode) < 6u) {
        MD_UNPREF_MODRM_PROFILE_HIT(
            opcode,
            MD_CODE_PEEK8());
    }
#endif
    /* Profile-guided exact unprefixed ALU template. */
    if (opcode == 0x33u &&
        MD_CODE_PEEK8() == 0xDBu) {
        (void)MD_CODE_FETCH8();
        runtime->cpu.r[MD_X86_BX] =
            md_alu16(&runtime->cpu, 6u,
                     runtime->cpu.r[MD_X86_BX],
                     runtime->cpu.r[MD_X86_BX]);
        MD_NEXT();
    }

    md_op_alu_rm_r(runtime, opcode, NULL);
    MD_NEXT();

op_alu_acc:
    md_op_alu_acc_imm(runtime, opcode);
    MD_NEXT();

op_group1:
#if MD_INTERP_OPCODE_PROFILE
    if (md_unpref_modrm_profile_row(opcode) < 6u) {
        MD_UNPREF_MODRM_PROFILE_HIT(
            opcode,
            MD_CODE_PEEK8());
    }
#endif
    /* Profile-guided exact unprefixed 83h templates. */
    if (opcode == 0x83u) {
        const uint8_t hot_modrm =
            MD_CODE_PEEK8();

        if (hot_modrm == 0xFFu || hot_modrm == 0xFBu) {
            uint16_t lhs;
            uint16_t rhs;
            (void)MD_CODE_FETCH8();
            lhs = hot_modrm == 0xFFu
                ? runtime->cpu.r[MD_X86_DI]
                : runtime->cpu.r[MD_X86_BX];
            rhs = (uint16_t)(int16_t)(int8_t)MD_CODE_FETCH8();
            (void)md_alu16(&runtime->cpu, 7u, lhs, rhs);
            MD_NEXT();
        }

        if (hot_modrm == 0xC7u || hot_modrm == 0xC6u) {
            uint16_t rhs;
            uint16_t *dst;
            (void)MD_CODE_FETCH8();
            rhs = (uint16_t)(int16_t)(int8_t)MD_CODE_FETCH8();
            dst = hot_modrm == 0xC7u
                ? &runtime->cpu.r[MD_X86_DI]
                : &runtime->cpu.r[MD_X86_SI];
            *dst = md_alu16(&runtime->cpu, 0u, *dst, rhs);
            MD_NEXT();
        }
    }

    md_op_group1_imm(runtime, opcode, NULL);
    MD_NEXT();

op_mov_rm:
#if MD_INTERP_OPCODE_PROFILE
    if (md_unpref_modrm_profile_row(opcode) < 6u) {
        MD_UNPREF_MODRM_PROFILE_HIT(
            opcode,
            MD_CODE_PEEK8());
    }
#endif
    /* Profile-guided exact unprefixed MOV templates. */
    {
        const uint8_t hot_modrm =
            MD_CODE_PEEK8();

        if (opcode == 0x8Bu && hot_modrm == 0x44u) {
            int8_t disp;
            (void)MD_CODE_FETCH8();
            disp = (int8_t)MD_CODE_FETCH8();
            runtime->cpu.r[MD_X86_AX] =
                md_x86_read16(&runtime->cpu, runtime->cpu.ds,
                              (uint16_t)(runtime->cpu.r[MD_X86_SI] + disp));
            MD_NEXT();
        }

        if (opcode == 0x8Bu && hot_modrm == 0xFBu) {
            (void)MD_CODE_FETCH8();
            runtime->cpu.r[MD_X86_DI] = runtime->cpu.r[MD_X86_BX];
            MD_NEXT();
        }

        if (opcode == 0x8Bu && hot_modrm == 0xF7u) {
            (void)MD_CODE_FETCH8();
            runtime->cpu.r[MD_X86_SI] = runtime->cpu.r[MD_X86_DI];
            MD_NEXT();
        }

        /*
         * M24.5: hot register-only ModR/M templates.
         *
         * 88 D8 = MOV AL,BL. This is one of the hottest remaining
         * unprefixed MOV forms in DOS2TEST and needs no EA decode.
         */
        if (opcode == 0x88u && hot_modrm == 0xD8u) {
            (void)MD_CODE_FETCH8();
            md_x86_set_reg8(opcode_cpu, 0u, md_x86_get_reg8(opcode_cpu, 3u));
            MD_NEXT();
        }

        if (opcode == 0x88u &&
            (hot_modrm == 0x1Eu || hot_modrm == 0x0Eu || hot_modrm == 0x2Eu)) {
            uint16_t disp;
            unsigned reg8;
            (void)MD_CODE_FETCH8();
            disp = MD_CODE_FETCH16();
            reg8 = hot_modrm == 0x1Eu ? 3u : (hot_modrm == 0x0Eu ? 1u : 5u);
            md_x86_write8(&runtime->cpu, runtime->cpu.ds, disp,
                          md_x86_get_reg8(&runtime->cpu, reg8));
            MD_NEXT();
        }
    }

    md_op_mov_rm_r(runtime, opcode, NULL);
    MD_NEXT();

op_loop: {
#if MD_INTERP_BACKEDGE_EXIT
    const int8_t rel = (int8_t)MD_CODE_PEEK8();
    const uint16_t fallthrough = (uint16_t)(runtime->cpu.ip + 1u);
    const uint16_t target = (uint16_t)(fallthrough + rel);
#endif
    md_op_loop(runtime, opcode);
#if MD_INTERP_BACKEDGE_EXIT
    if (rel < 0 && runtime->cpu.ip == target) {
        MD_NV2_PROFILE_BACKEDGE(opcode, ip_before, target);
        if (opcode == 0xE2u) {
            const unsigned bit =
                ((unsigned)runtime->cpu.cs ^ (unsigned)target) & 63u;
            const uint32_t mask = (uint32_t)1u << (bit & 31u);
            if ((runtime->native_v2_suppress_bloom[bit >> 5] & mask) == 0u) {
                runtime->native_v2_backedge_cs = runtime->cpu.cs;
                runtime->native_v2_backedge_ip = target;
                runtime->native_v2_backedge_hit = 1u;
                goto md_exit;
            }
        }
    }
#endif
    MD_NEXT();
}

op_string:
    md_op_string(runtime, opcode, NULL);
    MD_NEXT();

op_push_ss_hot:
    MD_STACK_PUSH16_FAST(opcode_cpu->ss);
    MD_NEXT();

op_pop_ds_hot:
    MD_STACK_POP16_FAST(opcode_cpu->ds);
    MD_NEXT();

op_pop_es_hot:
    MD_STACK_POP16_FAST(opcode_cpu->es);
    MD_NEXT();

op_push_sreg:
    MD_STACK_PUSH16_FAST(md_get_sreg(opcode_cpu, (opcode >> 3) & 3u));
    MD_NEXT();

op_pop_sreg: {
    uint16_t v;
    MD_STACK_POP16_FAST(v);
    if (opcode == 0x07u) opcode_cpu->es = v;
    else if (opcode == 0x17u) {
        opcode_cpu->ss = v;
        stack_ss_base = ((uint32_t)opcode_cpu->ss) << 4;
    } else opcode_cpu->ds = v;
    MD_NEXT();
}

op_ret:
    MD_STACK_POP16_FAST(opcode_cpu->ip);
    MD_NEXT();

op_call16: {
    const int16_t rel = (int16_t)MD_CODE_FETCH16();
    MD_STACK_PUSH16_FAST(opcode_cpu->ip);
    opcode_cpu->ip = (uint16_t)(opcode_cpu->ip + rel);
    MD_NEXT();
}

op_jmp16: {
    const int16_t rel = (int16_t)MD_CODE_FETCH16();
    const uint16_t target = (uint16_t)(runtime->cpu.ip + rel);
    runtime->cpu.ip = target;
    if (rel < 0)
        MD_NV2_PROFILE_BACKEDGE(opcode, ip_before, target);
    MD_NEXT();
}

op_nop:
    MD_NEXT();

op_int: {
    const uint8_t vector = MD_CODE_FETCH8();
    runtime->instructions += done;     /* hooks may observe the count */
    done = 0u;
    (void)md_runtime_interrupt(runtime, vector);
    MD_NEXT_CS();
}

op_jmp8: {
    const int8_t rel = (int8_t)MD_CODE_FETCH8();
    const uint16_t target = (uint16_t)(runtime->cpu.ip + rel);
    runtime->cpu.ip = target;
    if (rel < 0)
        MD_NV2_PROFILE_BACKEDGE(opcode, ip_before, target);
#if MD_INTERP_BACKEDGE_ALL
    if (rel < 0) {
        const unsigned bit = ((unsigned)runtime->cpu.cs ^ (unsigned)target) & 63u;
        const uint32_t mask = (uint32_t)1u << (bit & 31u);
        if ((runtime->native_v2_suppress_bloom[bit >> 5] & mask) == 0u) {
            runtime->native_v2_backedge_cs = runtime->cpu.cs;
            runtime->native_v2_backedge_ip = target;
            runtime->native_v2_backedge_hit = 1u;
            goto md_exit;
        }
    }
#endif
    MD_NEXT();
}

op_hlt:
    runtime->stop_reason = MD_STOP_HALT;
    goto md_exit;

op_group3:
#if MD_INTERP_OPCODE_PROFILE
    if (md_unpref_modrm_profile_row(opcode) < 6u) {
        MD_UNPREF_MODRM_PROFILE_HIT(
            opcode,
            MD_CODE_PEEK8());
    }
#endif
    /* Profile-guided exact unprefixed Group-3 templates. */
    {
        const uint8_t hot_modrm =
            MD_CODE_PEEK8();

        if (opcode == 0xF6u && hot_modrm == 0x44u) {
            int8_t disp;
            uint8_t lhs;
            uint8_t rhs;
            (void)MD_CODE_FETCH8();
            disp = (int8_t)MD_CODE_FETCH8();
            lhs = md_x86_read8(&runtime->cpu, runtime->cpu.ds,
                               (uint16_t)(runtime->cpu.r[MD_X86_SI] + disp));
            rhs = MD_CODE_FETCH8();
            (void)md_x86_logic8(&runtime->cpu, (uint8_t)(lhs & rhs));
            MD_NEXT();
        }

        if (opcode == 0xF6u && hot_modrm == 0xD4u) {
            const uint8_t ah = md_x86_get_reg8(&runtime->cpu, 4u);
            (void)MD_CODE_FETCH8();
            md_x86_set_reg8(&runtime->cpu, 4u, (uint8_t)~ah);
            MD_NEXT();
        }

        if (opcode == 0xF6u && hot_modrm == 0xE3u) {
            const uint8_t bl = md_x86_get_reg8(&runtime->cpu, 3u);
            (void)MD_CODE_FETCH8();
            md_muldiv_core(runtime, 0xF6u, 4u, bl, ip_before, 0u);
            MD_NEXT();
        }

        /* F6 E4 = MUL AH: same proven MUL core, skipping generic decode. */
        if (opcode == 0xF6u && hot_modrm == 0xE4u) {
            const uint8_t ah = md_x86_get_reg8(opcode_cpu, 4u);
            (void)MD_CODE_FETCH8();
            md_muldiv_core(runtime, 0xF6u, 4u, ah, ip_before, 0u);
            MD_NEXT();
        }

        if (opcode == 0xF7u && hot_modrm == 0xC7u) {
            uint16_t imm;
            (void)MD_CODE_FETCH8();
            imm = MD_CODE_FETCH16();
            (void)md_x86_logic16(&runtime->cpu,
                                 (uint16_t)(runtime->cpu.r[MD_X86_DI] & imm));
            MD_NEXT();
        }
    }

    // md_op_group3(runtime, opcode, ip_before, NULL);
    // MD_NEXT();
    /* DIV/IDIV can raise INT 0, which changes CS: refresh the cached code
    base (and honour watch_cs) exactly like op_generic does. */
    md_op_group3(runtime, opcode, ip_before, NULL);
    MD_NEXT_CS();

op_generic:
    (void)md_execute_opcode(runtime, opcode, ip_before, NULL);
    MD_NEXT_CS();

md_exit:
    runtime->instructions += done;
    return runtime->stop_reason;

#undef MD_CACHED_SEG_READ8
#undef MD_CACHED_SEG_READ16
#undef MD_CACHED_SEG_WRITE8
#undef MD_CACHED_SEG_WRITE16
#undef MD_CODE_PEEK8
#undef MD_CODE_FETCH8
#undef MD_CODE_FETCH16
#undef MD_STACK_PUSH16_FAST
#undef MD_STACK_POP16_FAST
#undef MD_NEXT
#undef MD_NEXT_CS
}
#endif

#if !defined(MD_THREADED_DISPATCH) || !(defined(__GNUC__) || defined(__clang__))
static MdStopReason md_interp_run_switch(MdRuntime *runtime, uint32_t instruction_budget,
                                         int watch_cs)
{
    uint32_t remaining = instruction_budget;
    const uint16_t cs0 = runtime->cpu.cs;
    while (runtime->stop_reason == MD_STOP_NONE) {
        uint16_t ip_before;
        uint8_t opcode;
        if (watch_cs && runtime->cpu.cs != cs0) break;
        if (remaining == 0u) {
            runtime->stop_reason = MD_STOP_BUDGET;
            break;
        }
        --remaining;
        ++runtime->instructions;
        ip_before = runtime->cpu.ip;
        opcode = md_fetch8(runtime);
        if (md_is_prefix_byte(opcode)) (void)md_execute_prefixed(runtime, opcode, ip_before);
        else (void)md_execute_opcode(runtime, opcode, ip_before, NULL);
    }
    return runtime->stop_reason;
}
#endif

MdStopReason md_interp_step(MdRuntime *runtime)
{
    uint16_t ip_before;
    uint8_t opcode;
    if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason;
    ++runtime->instructions;
    ip_before = runtime->cpu.ip;
    opcode = md_fetch8(runtime);
    if (md_is_prefix_byte(opcode)) (void)md_execute_prefixed(runtime, opcode, ip_before);
    else (void)md_execute_opcode(runtime, opcode, ip_before, NULL);
    return runtime->stop_reason;
}

/* Budgets above 2^30 are run in 32-bit chunks so the hot loop never does
   64-bit arithmetic; a chunk boundary is invisible to the guest. */
static MdStopReason md_interp_run_chunked(MdRuntime *runtime, uint64_t instruction_budget,
                                         int watch_cs)
{
    const uint16_t cs0 = runtime->cpu.cs;
    for (;;) {
        const uint32_t chunk = instruction_budget > 0x40000000u ? 0x40000000u
                                                                : (uint32_t)instruction_budget;
        MdStopReason st;
#if defined(MD_THREADED_DISPATCH) && (defined(__GNUC__) || defined(__clang__))
        st = md_interp_run_threaded(runtime, chunk, watch_cs);
#else
        st = md_interp_run_switch(runtime, chunk, watch_cs);
#endif
        instruction_budget -= chunk;
        if (st != MD_STOP_BUDGET || instruction_budget == 0u) return st;
        if (watch_cs && runtime->cpu.cs != cs0) return st;
        runtime->stop_reason = MD_STOP_NONE;     /* chunk boundary, not a stop */
    }
}

MdStopReason md_interp_run(MdRuntime *runtime, uint64_t instruction_budget)
{
    return md_interp_run_chunked(runtime, instruction_budget, 0);
}

MdStopReason md_interp_run_until_cs_change(MdRuntime *runtime, uint64_t instruction_budget)
{
    return md_interp_run_chunked(runtime, instruction_budget, 1);
}

/* ---- M18 exports for dosrecomp-generated code -------------------------- */

void md_interp_muldiv(MdRuntime *runtime, uint8_t opcode, unsigned ext, uint16_t operand,
                      uint16_t ip_before)
{
    md_muldiv_core(runtime, opcode, ext, operand, ip_before, 0u);
}

void md_interp_string_op(MdRuntime *runtime, uint8_t opcode, uint8_t segment_prefix,
                         uint8_t repeat_prefix)
{
    MdPrefixState prefix;
    memset(&prefix, 0, sizeof(prefix));
    prefix.segment_override = segment_prefix;
    prefix.repeat = repeat_prefix;
    md_op_string(runtime, opcode, &prefix);
}
