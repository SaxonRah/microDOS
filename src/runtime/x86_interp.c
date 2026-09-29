#include "microdos/runtime.h"
#include "runtime_internal.h"

#include <stddef.h>

typedef struct MdOperand {
    uint8_t is_register;
    uint8_t reg;
    uint16_t segment;
    uint16_t offset;
} MdOperand;

static inline MdOperand md_decode_rm(MdRuntime *runtime, uint8_t modrm)
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
        op.segment = cpu->ds;
        op.offset = md_fetch16(runtime);
        return op;
    }

    switch (rm) {
        case 0: base = (uint16_t)(cpu->r[MD_X86_BX] + cpu->r[MD_X86_SI]); break;
        case 1: base = (uint16_t)(cpu->r[MD_X86_BX] + cpu->r[MD_X86_DI]); break;
        case 2: base = (uint16_t)(cpu->r[MD_X86_BP] + cpu->r[MD_X86_SI]); uses_bp = 1; break;
        case 3: base = (uint16_t)(cpu->r[MD_X86_BP] + cpu->r[MD_X86_DI]); uses_bp = 1; break;
        case 4: base = cpu->r[MD_X86_SI]; break;
        case 5: base = cpu->r[MD_X86_DI]; break;
        case 6: base = cpu->r[MD_X86_BP]; uses_bp = 1; break;
        default: base = cpu->r[MD_X86_BX]; break;
    }

    if (mod == 1u) {
        displacement = (int8_t)md_fetch8(runtime);
    } else if (mod == 2u) {
        displacement = (int16_t)md_fetch16(runtime);
    }

    if (uses_bp) segment = cpu->ss;
    op.segment = segment;
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

static inline void md_fault(MdRuntime *runtime, uint8_t opcode, uint16_t ip_before)
{
    runtime->fault_opcode = opcode;
    runtime->fault_linear = md_x86_linear(runtime->cpu.cs, ip_before);
    runtime->stop_reason = MD_STOP_FAULT;
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
    const uint16_t old_cf = cpu->flags & MD_X86_FLAG_CF;
    const unsigned reg = opcode & 7u;
    cpu->r[reg] = md_x86_add16(cpu, cpu->r[reg], 1u);
    cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | old_cf);
}

static inline void md_op_dec_r16(MdRuntime *runtime, uint8_t opcode)
{
    MdX86 *cpu = &runtime->cpu;
    const uint16_t old_cf = cpu->flags & MD_X86_FLAG_CF;
    const unsigned reg = opcode & 7u;
    cpu->r[reg] = md_x86_sub16(cpu, cpu->r[reg], 1u);
    cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | old_cf);
}

static inline void md_op_mov_rm_r(MdRuntime *runtime, uint8_t opcode)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm);

    if (opcode == 0x88u) md_operand_write8(runtime, rm, md_x86_get_reg8(cpu, reg));
    else if (opcode == 0x89u) md_operand_write16(runtime, rm, cpu->r[reg]);
    else if (opcode == 0x8Au) md_x86_set_reg8(cpu, reg, md_operand_read8(runtime, rm));
    else cpu->r[reg] = md_operand_read16(runtime, rm);
}

static inline void md_op_alu_rm_r(MdRuntime *runtime, uint8_t opcode)
{
    MdX86 *cpu = &runtime->cpu;
    const uint8_t modrm = md_fetch8(runtime);
    const unsigned reg = (modrm >> 3) & 7u;
    MdOperand rm = md_decode_rm(runtime, modrm);
    const unsigned family = opcode & 0xF8u;
    const int direction = (opcode >> 1) & 1u;
    const int width16 = opcode & 1u;

    if (!width16) {
        const uint8_t lhs = direction ? md_x86_get_reg8(cpu, reg) : md_operand_read8(runtime, rm);
        const uint8_t rhs = direction ? md_operand_read8(runtime, rm) : md_x86_get_reg8(cpu, reg);
        uint8_t result;
        if (family == 0x00u) result = md_x86_add8(cpu, lhs, rhs);
        else if (family == 0x28u) result = md_x86_sub8(cpu, lhs, rhs);
        else result = md_x86_sub8(cpu, lhs, rhs);
        if (family != 0x38u) {
            if (direction) md_x86_set_reg8(cpu, reg, result);
            else md_operand_write8(runtime, rm, result);
        }
    } else {
        const uint16_t lhs = direction ? cpu->r[reg] : md_operand_read16(runtime, rm);
        const uint16_t rhs = direction ? md_operand_read16(runtime, rm) : cpu->r[reg];
        uint16_t result;
        if (family == 0x00u) result = md_x86_add16(cpu, lhs, rhs);
        else if (family == 0x28u) result = md_x86_sub16(cpu, lhs, rhs);
        else result = md_x86_sub16(cpu, lhs, rhs);
        if (family != 0x38u) {
            if (direction) cpu->r[reg] = result;
            else md_operand_write16(runtime, rm, result);
        }
    }
}

static inline int md_execute_opcode(MdRuntime *runtime, uint8_t opcode, uint16_t ip_before)
{
    MdX86 *cpu = &runtime->cpu;

    if ((opcode & 0xF8u) == 0xB0u) {
        md_op_mov_r8_imm(runtime, opcode);
        return 1;
    }
    if ((opcode & 0xF8u) == 0xB8u) {
        md_op_mov_r16_imm(runtime, opcode);
        return 1;
    }
    if ((opcode & 0xF8u) == 0x50u) {
        md_x86_push(cpu, cpu->r[opcode & 7u]);
        return 1;
    }
    if ((opcode & 0xF8u) == 0x58u) {
        cpu->r[opcode & 7u] = md_x86_pop(cpu);
        return 1;
    }
    if ((opcode & 0xF8u) == 0x40u) {
        md_op_inc_r16(runtime, opcode);
        return 1;
    }
    if ((opcode & 0xF8u) == 0x48u) {
        md_op_dec_r16(runtime, opcode);
        return 1;
    }

    if ((opcode >= 0x88u && opcode <= 0x8Bu)) {
        md_op_mov_rm_r(runtime, opcode);
        return 1;
    }

    if ((opcode <= 0x03u) || (opcode >= 0x28u && opcode <= 0x2Bu) ||
        (opcode >= 0x38u && opcode <= 0x3Bu)) {
        md_op_alu_rm_r(runtime, opcode);
        return 1;
    }

    switch (opcode) {
        case 0x04: md_x86_set_reg8(cpu, 0u, md_x86_add8(cpu, md_x86_get_reg8(cpu, 0u), md_fetch8(runtime))); break;
        case 0x05: cpu->r[MD_X86_AX] = md_x86_add16(cpu, cpu->r[MD_X86_AX], md_fetch16(runtime)); break;
        case 0x2C: md_x86_set_reg8(cpu, 0u, md_x86_sub8(cpu, md_x86_get_reg8(cpu, 0u), md_fetch8(runtime))); break;
        case 0x2D: cpu->r[MD_X86_AX] = md_x86_sub16(cpu, cpu->r[MD_X86_AX], md_fetch16(runtime)); break;
        case 0x3C: (void)md_x86_sub8(cpu, md_x86_get_reg8(cpu, 0u), md_fetch8(runtime)); break;
        case 0x3D: (void)md_x86_sub16(cpu, cpu->r[MD_X86_AX], md_fetch16(runtime)); break;

        case 0x74: {
            const int8_t rel = (int8_t)md_fetch8(runtime);
            if (cpu->flags & MD_X86_FLAG_ZF) cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0x75: {
            const int8_t rel = (int8_t)md_fetch8(runtime);
            if (!(cpu->flags & MD_X86_FLAG_ZF)) cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0x90: break;

        case 0xA0: md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, cpu->ds, md_fetch16(runtime))); break;
        case 0xA1: cpu->r[MD_X86_AX] = md_x86_read16(cpu, cpu->ds, md_fetch16(runtime)); break;
        case 0xA2: md_x86_write8(cpu, cpu->ds, md_fetch16(runtime), md_x86_get_reg8(cpu, 0u)); break;
        case 0xA3: md_x86_write16(cpu, cpu->ds, md_fetch16(runtime), cpu->r[MD_X86_AX]); break;

        case 0xC3: cpu->ip = md_x86_pop(cpu); break;
        case 0xCD: {
            const uint8_t vector = md_fetch8(runtime);
            (void)md_runtime_interrupt(runtime, vector);
            break;
        }
        case 0xE8: {
            const int16_t rel = (int16_t)md_fetch16(runtime);
            md_x86_push(cpu, cpu->ip);
            cpu->ip = (uint16_t)(cpu->ip + rel);
            break;
        }
        case 0xE9: cpu->ip = (uint16_t)(cpu->ip + (int16_t)md_fetch16(runtime)); break;
        case 0xEB: cpu->ip = (uint16_t)(cpu->ip + (int8_t)md_fetch8(runtime)); break;
        case 0xF4: runtime->stop_reason = MD_STOP_HALT; break;

        default:
            md_fault(runtime, opcode, ip_before);
            return 0;
    }
    return 1;
}

#if defined(MD_THREADED_DISPATCH) && (defined(__GNUC__) || defined(__clang__))
static MdStopReason md_interp_run_threaded(MdRuntime *runtime, uint64_t instruction_budget)
{
    static void *dispatch[256] = {0};
    static int initialized = 0;
    uint8_t opcode;
    uint16_t ip_before;
    uint64_t remaining = instruction_budget;
    unsigned i;

    /* The table maps hot opcode classes directly to labels. Unsupported entries
       land in the generic decoder, which also handles less-common implemented ops.
       This keeps the RP2350 fast path free of a giant central switch. */
    if (!initialized) {
        for (i = 0; i < 256u; ++i) dispatch[i] = &&op_generic;
        for (i = 0xB0u; i <= 0xB7u; ++i) dispatch[i] = &&op_mov_r8_imm;
        for (i = 0xB8u; i <= 0xBFu; ++i) dispatch[i] = &&op_mov_r16_imm;
        for (i = 0x40u; i <= 0x47u; ++i) dispatch[i] = &&op_inc_r16;
        for (i = 0x48u; i <= 0x4Fu; ++i) dispatch[i] = &&op_dec_r16;
        for (i = 0x50u; i <= 0x57u; ++i) dispatch[i] = &&op_push_r16;
        for (i = 0x58u; i <= 0x5Fu; ++i) dispatch[i] = &&op_pop_r16;
        dispatch[0x74] = &&op_jcc8;
        dispatch[0x75] = &&op_jcc8;
        dispatch[0x90] = &&op_nop;
        dispatch[0xCD] = &&op_int;
        dispatch[0xEB] = &&op_jmp8;
        dispatch[0xF4] = &&op_hlt;
        initialized = 1;
    }

#define MD_NEXT() do { \
        if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason; \
        if (remaining == 0u) { runtime->stop_reason = MD_STOP_BUDGET; return MD_STOP_BUDGET; } \
        --remaining; \
        ++runtime->instructions; \
        ip_before = runtime->cpu.ip; \
        opcode = md_fetch8(runtime); \
        goto *dispatch[opcode]; \
    } while (0)

    MD_NEXT();

op_mov_r8_imm:
    md_op_mov_r8_imm(runtime, opcode);
    MD_NEXT();

op_mov_r16_imm:
    md_op_mov_r16_imm(runtime, opcode);
    MD_NEXT();

op_inc_r16:
    md_op_inc_r16(runtime, opcode);
    MD_NEXT();

op_dec_r16:
    md_op_dec_r16(runtime, opcode);
    MD_NEXT();

op_push_r16:
    md_x86_push(&runtime->cpu, runtime->cpu.r[opcode & 7u]);
    MD_NEXT();

op_pop_r16:
    runtime->cpu.r[opcode & 7u] = md_x86_pop(&runtime->cpu);
    MD_NEXT();

op_jcc8: {
    const int8_t rel = (int8_t)md_fetch8(runtime);
    const int zf = (runtime->cpu.flags & MD_X86_FLAG_ZF) != 0u;
    if ((opcode == 0x74u && zf) || (opcode == 0x75u && !zf)) {
        runtime->cpu.ip = (uint16_t)(runtime->cpu.ip + rel);
    }
    MD_NEXT();
}

op_nop:
    MD_NEXT();

op_int: {
    const uint8_t vector = md_fetch8(runtime);
    (void)md_runtime_interrupt(runtime, vector);
    MD_NEXT();
}

op_jmp8:
    runtime->cpu.ip = (uint16_t)(runtime->cpu.ip + (int8_t)md_fetch8(runtime));
    MD_NEXT();

op_hlt:
    runtime->stop_reason = MD_STOP_HALT;
    return MD_STOP_HALT;

op_generic:
    (void)md_execute_opcode(runtime, opcode, ip_before);
    MD_NEXT();

#undef MD_NEXT
}
#endif

#if !defined(MD_THREADED_DISPATCH) || !(defined(__GNUC__) || defined(__clang__))
static MdStopReason md_interp_run_switch(MdRuntime *runtime, uint64_t instruction_budget)
{
    uint64_t remaining = instruction_budget;
    while (runtime->stop_reason == MD_STOP_NONE) {
        uint16_t ip_before;
        uint8_t opcode;
        if (remaining == 0u) {
            runtime->stop_reason = MD_STOP_BUDGET;
            break;
        }
        --remaining;
        ++runtime->instructions;
        ip_before = runtime->cpu.ip;
        opcode = md_fetch8(runtime);
        (void)md_execute_opcode(runtime, opcode, ip_before);
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
    (void)md_execute_opcode(runtime, opcode, ip_before);
    return runtime->stop_reason;
}

MdStopReason md_interp_run(MdRuntime *runtime, uint64_t instruction_budget)
{
#if defined(MD_THREADED_DISPATCH) && (defined(__GNUC__) || defined(__clang__))
    return md_interp_run_threaded(runtime, instruction_budget);
#else
    return md_interp_run_switch(runtime, instruction_budget);
#endif
}
