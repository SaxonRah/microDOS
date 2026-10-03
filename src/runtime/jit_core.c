#include "microdos/jit.h"
#include "microdos/ops.h"
#include "microdos/region.h"

#include "microdos/decode.h"

#include <stddef.h>
#include <string.h>

/* The emitted Thumb (resident regions, direct memory paths) wraps guest
   addresses at 20 bits. Smaller experimental address spaces are not
   supported by the JIT. */
#if MD_X86_ADDRESS_BITS != 20
#error "the runtime JIT requires MD_X86_ADDRESS_BITS == 20"
#endif


_Static_assert(offsetof(MdRuntime, cpu) == 0u,
               "M19.2 generated code requires MdX86 at MdRuntime offset 0");
_Static_assert((MD_JIT_BLOCK_SLOTS & (MD_JIT_BLOCK_SLOTS - 1u)) == 0u,
               "MD_JIT_BLOCK_SLOTS must be a power of two");
_Static_assert((MD_JIT_HOTNESS_SLOTS & (MD_JIT_HOTNESS_SLOTS - 1u)) == 0u,
               "MD_JIT_HOTNESS_SLOTS must be a power of two");

#define MD_JIT_CODE_ALIGN 4u
#define MD_JIT_NATIVE_TMP 1536u

/*
 * M22.0: if generated code cannot retire even one guest instruction, do not
 * bounce straight back through lookup/dispatch after interpreting only one
 * instruction.  Run a small canonical-interpreter burst, then give the JIT
 * another chance.  This is deliberately local and bounded: cold admission,
 * compile failures and exact budget fallbacks keep their existing behavior.
 *
 * MDSTRESS v2 showed zero-progress retry churn making the current JIT up to
 * ~6.7x slower than the plain interpreter on control-heavy code.  Sixteen
 * instructions is long enough to cross dense unsupported sequences while
 * remaining short enough to return quickly to a hot native region.
 */
#ifndef MD_JIT_ZERO_ESCAPE_BURST
#define MD_JIT_ZERO_ESCAPE_BURST 16u
#endif
#if MD_JIT_ZERO_ESCAPE_BURST < 1
#error "MD_JIT_ZERO_ESCAPE_BURST must be at least 1"
#endif

#if MD_JIT_PROFILE
#define MD_JIT_STAT(expr_) do { expr_; } while (0)
#else
#define MD_JIT_STAT(expr_) do { } while (0)
#endif

enum MdJitCfState {
    MD_JIT_CF_RAW = 0,      /* flags_raw contains the preserved CF */
    MD_JIT_CF_ZERO,
    MD_JIT_CF_LAZY_UNKNOWN
};

static unsigned md_jit_hash(uint16_t cs, uint16_t ip)
{
    return (unsigned)(((uint32_t)cs * 33u + ip) & (MD_JIT_BLOCK_SLOTS - 1u));
}

static int md_jit_hot_ready(MdJit *jit, uint16_t cs, uint16_t ip)
{
#if MD_JIT_HOT_THRESHOLD <= 1 || !MD_JIT_LEGACY_HOTNESS
    (void)jit; (void)cs; (void)ip;
    return 1;
#else
    const unsigned slot = (unsigned)(((uint32_t)cs * 33u + ip) & (MD_JIT_HOTNESS_SLOTS - 1u));
    MdJitHotness *h = &jit->hotness[slot];
    if (!h->valid || h->cs != cs || h->ip != ip) {
        h->cs = cs; h->ip = ip; h->count = 1u; h->valid = 1u;
        return MD_JIT_HOT_THRESHOLD <= 1u;
    }
    if (h->count < 255u) ++h->count;
    return h->count >= MD_JIT_HOT_THRESHOLD;
#endif
}

static uint16_t md_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

const char *md_jit_exit_reason_name(unsigned reason)
{
    switch (reason) {
        case MD_JIT_EXIT_COMPILE_FAIL: return "compile";
        case MD_JIT_EXIT_BUDGET_FALLBACK: return "budget";
        case MD_JIT_EXIT_ZERO_PROGRESS: return "zero";
        case MD_JIT_EXIT_COLD_FALLBACK: return "cold";
        case MD_JIT_EXIT_NATIVE_RETURN: return "return";
        case MD_JIT_EXIT_CS_CHANGE: return "cs-change";
        case MD_JIT_EXIT_STOP: return "stop";
        default: return "none";
    }
}

/* Space-saving hot-site table. Counts are exact for a site once admitted;
 * replacement uses min+1, so newly admitted sites are conservative upper
 * bounds until they become genuinely hot.  The aggregate reason counters
 * remain exact. */
static void md_jit_record_site_at(MdJit *jit, const MdRuntime *runtime, unsigned reason,
                                  uint16_t cs, uint16_t ip, uint8_t opcode,
                                  uint16_t dst_cs, uint16_t dst_ip)
{
#if !MD_JIT_PROFILE
    (void)jit; (void)runtime; (void)reason; (void)cs; (void)ip;
    (void)opcode; (void)dst_cs; (void)dst_ip;
    return;
#else
    MdJitHotSite *empty = NULL, *least = NULL;
    unsigned i;
    (void)runtime;
    if (jit == NULL || reason >= MD_JIT_EXIT_REASON_COUNT || reason == 0u) return;
    for (i = 0u; i < MD_JIT_HOT_SITES; ++i) {
        MdJitHotSite *site = &jit->hot_sites[i];
        if (site->count != 0u && site->cs == cs && site->ip == ip &&
            site->dst_cs == dst_cs && site->dst_ip == dst_ip &&
            site->opcode == opcode && site->reason == reason) {
            ++site->count;
            return;
        }
        if (site->count == 0u && empty == NULL) empty = site;
        if (least == NULL || site->count < least->count) least = site;
    }
    if (empty != NULL) least = empty;
    if (least != NULL) {
        const uint64_t base = least->count;
        least->cs = cs;
        least->ip = ip;
        least->dst_cs = dst_cs;
        least->dst_ip = dst_ip;
        least->opcode = opcode;
        least->reason = (uint8_t)reason;
        least->count = base + 1u;
    }
#endif
}

#if MD_JIT_PROFILE
static void md_jit_record_site(MdJit *jit, const MdRuntime *runtime, unsigned reason)
{
    uint16_t cs, ip;
    uint8_t opcode;
    if (jit == NULL || runtime == NULL) return;
    cs = runtime->cpu.cs;
    ip = runtime->cpu.ip;
    opcode = md_x86_read8(&runtime->cpu, cs, ip);
    md_jit_record_site_at(jit, runtime, reason, cs, ip, opcode, cs, ip);
}
#endif

static void md_jit_record_transfer(MdJit *jit, const MdRuntime *runtime, unsigned reason,
                                   uint16_t src_cs, uint16_t src_ip, uint8_t opcode)
{
    if (jit == NULL || runtime == NULL) return;
    md_jit_record_site_at(jit, runtime, reason, src_cs, src_ip, opcode,
                          runtime->cpu.cs, runtime->cpu.ip);
}

static void md_jit_record_exit(MdJit *jit, const MdRuntime *runtime, unsigned reason)
{
#if MD_JIT_PROFILE
    if (jit == NULL || reason >= MD_JIT_EXIT_REASON_COUNT || reason == 0u) return;
    ++jit->exit_reason[reason];
    md_jit_record_site(jit, runtime, reason);
#else
    (void)jit; (void)runtime; (void)reason;
#endif
}

static int md_jit_boundary(const MdDecodedInstruction *inst)
{
    if (inst->flow != MD_DECODE_FLOW_FALLTHROUGH) return 1;
    switch (inst->opcode) {
        case 0x0Fu: /* POP CS */
        case 0x9Au: /* far CALL */
        case 0xCAu: case 0xCBu: /* far RET */
        case 0xCCu: case 0xCDu: case 0xCEu: /* software interrupts */
        case 0xCFu: /* IRET */
        case 0xEAu: /* far JMP */
        case 0xF4u: /* HLT */
            return 1;
        default:
            return 0;
    }
}

/* M20.1: keep decoding through forward conditional edges so a bounded CFG
 * candidate contains both the fallthrough path and its in-window target.
 * Backward conditionals still terminate discovery; they are natural loop
 * backedges. */
static int md_jit_continue_forward_conditional(const MdDecodedInstruction *inst,
                                                uint16_t start_ip, size_t avail)
{
    uint32_t end = (uint32_t)start_ip + (uint32_t)avail;
    return inst->flow == MD_DECODE_FLOW_CONDITIONAL &&
           inst->target > inst->next_ip &&
           (uint32_t)inst->target < end;
}

static void md_jit_classify(const uint8_t *image, uint16_t image_base,
                            const MdDecodedInstruction *inst, MdJitOp *op)
{
    const size_t off = (size_t)(uint16_t)(inst->ip - image_base);
    const uint8_t *q = image + off;
    const uint8_t opcode = inst->opcode;

    memset(op, 0, sizeof(*op));
    op->ip = inst->ip;
    op->next_ip = inst->next_ip;
    op->target = inst->target;
    op->opcode = opcode;
    op->kind = MD_JIT_OP_FALLBACK;

    if (inst->prefix_count != 0u) return;

    if ((opcode & 0xF8u) == 0xB0u) {
        op->kind = MD_JIT_OP_MOV_R8_IMM;
        op->reg = (uint8_t)(opcode & 7u);
        op->imm = q[1];
        return;
    }
    if ((opcode & 0xF8u) == 0xB8u) {
        op->kind = MD_JIT_OP_MOV_R16_IMM;
        op->reg = (uint8_t)(opcode & 7u);
        op->imm = md_u16(q + 1u);
        return;
    }
    if ((opcode & 0xF8u) == 0x40u) {
        op->kind = MD_JIT_OP_INC_R16;
        op->reg = (uint8_t)(opcode & 7u);
        return;
    }
    if ((opcode & 0xF8u) == 0x48u) {
        op->kind = MD_JIT_OP_DEC_R16;
        op->reg = (uint8_t)(opcode & 7u);
        return;
    }

    if (opcode <= 0x3Du && (opcode & 0x06u) == 0x04u) {
        op->kind = MD_JIT_OP_ALU_ACC_IMM;
        op->aux = (uint8_t)((opcode >> 3) & 7u);
        op->reg = (uint8_t)(opcode & 1u); /* 0 AL, 1 AX */
        op->imm = (opcode & 1u) ? md_u16(q + 1u) : q[1];
        return;
    }

    if ((opcode == 0x81u || opcode == 0x83u) && inst->has_modrm &&
        (inst->modrm >> 6) == 3u) {
        op->kind = MD_JIT_OP_GRP1_R16_IMM;
        op->aux = (uint8_t)((inst->modrm >> 3) & 7u);
        op->reg = (uint8_t)(inst->modrm & 7u);
        if (opcode == 0x83u) op->imm = (uint16_t)(int16_t)(int8_t)q[2];
        else op->imm = md_u16(q + 2u);
        return;
    }

    if (opcode == 0x8Au && inst->has_modrm && inst->modrm == 0x04u) {
        op->kind = MD_JIT_OP_MOV_AL_SI; /* MOV AL,[SI], default DS */
        return;
    }
    if (opcode == 0x88u && inst->has_modrm && inst->modrm == 0x04u) {
        op->kind = MD_JIT_OP_MOV_SI_AL; /* MOV [SI],AL, default DS */
        return;
    }

    /* M20.3: LODS, register-register 16-bit ALU, LOOP family. ADC/SBB are
       left to the interpreter (carry-in through lazy state). */
    if (opcode == 0xACu || opcode == 0xADu) {
        op->kind = MD_JIT_OP_LODS;
        op->aux = (uint8_t)((opcode & 1u) ? 2u : 1u);
        return;
    }
    if (opcode <= 0x3Bu && ((opcode & 0x07u) == 0x01u || (opcode & 0x07u) == 0x03u) &&
        inst->has_modrm && (inst->modrm >> 6) == 3u) {
        const unsigned alu = (opcode >> 3) & 7u;
        const unsigned mreg = (inst->modrm >> 3) & 7u, mrm = inst->modrm & 7u;
        if (alu == 2u || alu == 3u) return;
        op->kind = MD_JIT_OP_ALU_RR16;
        op->aux = (uint8_t)alu;
        op->reg = (uint8_t)((opcode & 2u) ? mreg : mrm);      /* destination */
        op->imm = (uint16_t)((opcode & 2u) ? mrm : mreg);     /* source */
        return;
    }
    if (opcode <= 0x3Au && ((opcode & 0x07u) == 0x00u || (opcode & 0x07u) == 0x02u) &&
        inst->has_modrm && (inst->modrm >> 6) == 3u) {
        const unsigned alu = (opcode >> 3) & 7u;
        const unsigned mreg = (inst->modrm >> 3) & 7u, mrm = inst->modrm & 7u;
        if (alu == 2u || alu == 3u) return;
        op->kind = MD_JIT_OP_ALU_RR8;
        op->aux = (uint8_t)alu;
        op->reg = (uint8_t)((opcode & 2u) ? mreg : mrm);
        op->imm = (uint16_t)((opcode & 2u) ? mrm : mreg);
        return;
    }
    if (opcode >= 0xE0u && opcode <= 0xE3u) {
        op->kind = MD_JIT_OP_LOOP;
        op->aux = (uint8_t)(opcode & 3u);
        return;
    }

    if (opcode >= 0x70u && opcode <= 0x7Fu) {
        op->kind = MD_JIT_OP_JCC;
        op->aux = (uint8_t)(opcode & 0x0Fu);
        return;
    }
    if (opcode == 0xE9u || opcode == 0xEBu) {
        op->kind = MD_JIT_OP_JMP;
        return;
    }
    if (opcode == 0xE8u) {
        op->kind = MD_JIT_OP_CALL_NEAR;
        return;
    }
    if (opcode == 0xC3u) { op->kind = MD_JIT_OP_RET_NEAR; return; }
    if (opcode == 0xC2u) { op->kind = MD_JIT_OP_RET_NEAR_IMM; op->imm = md_u16(q + 1u); return; }
    if (opcode == 0xCBu) { op->kind = MD_JIT_OP_RET_FAR; return; }
    if (opcode == 0xCAu) { op->kind = MD_JIT_OP_RET_FAR_IMM; op->imm = md_u16(q + 1u); return; }
    if (opcode == 0xCCu) { op->kind = MD_JIT_OP_INT; op->imm = 3u; return; }
    if (opcode == 0xCDu) { op->kind = MD_JIT_OP_INT; op->imm = q[1]; return; }
    if (opcode == 0xCEu) { op->kind = MD_JIT_OP_INT; op->aux = 1u; op->imm = 4u; return; }
    if (opcode == 0xCFu) { op->kind = MD_JIT_OP_IRET; return; }
    if (opcode == 0x90u) {
        op->kind = MD_JIT_OP_NOP;
        return;
    }
    if (opcode == 0xF4u) {
        op->kind = MD_JIT_OP_HLT;
        return;
    }
}

static int md_jit_block_current(const MdJitBlock *block, const MdRuntime *runtime);

/* M20.3 shared semantics. They perform the instruction's effects only; the
   caller retires it and sets IP (exec_one: C path; native code: r6). LODS
   goes through the interpreter's own string implementation, the ALU through
   the canonical lazy-flag helpers. */
static void md_jit_sem_apply(MdRuntime *runtime, const MdJitOp *op)
{
    MdX86 *cpu = &runtime->cpu;
    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_LODS:
            md_interp_string_op(runtime, op->opcode, 0u, 0u);
            break;
        case MD_JIT_OP_ALU_RR16: {
            const uint16_t r = md_x86_alu16(cpu, op->aux, cpu->r[op->reg & 7u], cpu->r[op->imm & 7u]);
            if (op->aux != 7u) cpu->r[op->reg & 7u] = r;            /* CMP: flags only */
            break;
        }
        case MD_JIT_OP_ALU_RR8: {
            const uint8_t r = md_x86_alu8(cpu, op->aux, md_x86_get_reg8(cpu, op->reg & 7u),
                                          md_x86_get_reg8(cpu, op->imm & 7u));
            if (op->aux != 7u) md_x86_set_reg8(cpu, op->reg & 7u, r);
            break;
        }
        default:
            break;
    }
}

/* LOOP family: updates CX (not for JCXZ) and returns 1 if the branch is
   taken. Flags are read, never written. */
static int md_jit_loop_taken(MdRuntime *runtime, const MdJitOp *op)
{
    MdX86 *cpu = &runtime->cpu;
    if (op->aux == 3u) return cpu->r[MD_X86_CX] == 0u;               /* JCXZ */
    cpu->r[MD_X86_CX] = (uint16_t)(cpu->r[MD_X86_CX] - 1u);
    if (cpu->r[MD_X86_CX] == 0u) return 0;
    if (op->aux == 2u) return 1;                                     /* LOOP */
    return op->aux == 1u ? md_x86_zf(cpu) != 0 : md_x86_zf(cpu) == 0; /* LOOPZ/NZ */
}

/* Native helpers (BLX targets). Same contract as md_jit_control_one: no
   retirement, the generated code counts the instruction in r6. */


/* Reference/host path. On RP2350 M19.1 does not call this for direct ops; it
 * exists to keep the predecoded representation independently executable and
 * for host-side semantic checking. */
static void md_jit_exec_one(MdRuntime *runtime, MdJitBlock *block, unsigned index)
{
    MdX86 *cpu;
    const MdJitOp *op;
    uint16_t value16;
    uint8_t value8;

    if (runtime == NULL || block == NULL || index >= block->op_count) return;
    if (runtime->stop_reason != MD_STOP_NONE) return;
    if (!md_jit_block_current(block, runtime)) return;

    cpu = &runtime->cpu;
    op = &block->ops[index];
    if (cpu->cs != block->cs || cpu->ip != op->ip) return;

    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R8_IMM:
            md_x86_set_reg8(cpu, op->reg, (uint8_t)op->imm);
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_MOV_R16_IMM:
            cpu->r[op->reg] = op->imm;
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_INC_R16:
            cpu->r[op->reg] = md_aot_incdec16(cpu, cpu->r[op->reg], 0);
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_DEC_R16:
            cpu->r[op->reg] = md_aot_incdec16(cpu, cpu->r[op->reg], 1);
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_ALU_ACC_IMM:
            if (op->reg != 0u) {
                value16 = md_aot_alu16(cpu, op->aux, cpu->r[MD_X86_AX], op->imm);
                if (op->aux != 7u) cpu->r[MD_X86_AX] = value16;
            } else {
                value8 = md_aot_alu8(cpu, op->aux, md_x86_get_reg8(cpu, 0u), (uint8_t)op->imm);
                if (op->aux != 7u) md_x86_set_reg8(cpu, 0u, value8);
            }
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_GRP1_R16_IMM:
            value16 = md_aot_alu16(cpu, op->aux, cpu->r[op->reg], op->imm);
            if (op->aux != 7u) cpu->r[op->reg] = value16;
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_MOV_AL_SI:
            value8 = md_x86_read8(cpu, cpu->ds, cpu->r[MD_X86_SI]);
            md_x86_set_reg8(cpu, 0u, value8);
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_MOV_SI_AL:
            md_x86_write8(cpu, cpu->ds, cpu->r[MD_X86_SI], md_x86_get_reg8(cpu, 0u));
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_JCC:
            cpu->ip = md_aot_condition(cpu, op->aux) ? op->target : op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_JMP:
            cpu->ip = op->target;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_CALL_NEAR:
            md_x86_push(cpu, op->next_ip);
            cpu->ip = op->target;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            ++block->owner->control_instructions;
            break;
        case MD_JIT_OP_RET_NEAR:
            cpu->ip = md_x86_pop(cpu);
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            ++block->owner->control_instructions;
            break;
        case MD_JIT_OP_RET_NEAR_IMM:
            cpu->ip = md_x86_pop(cpu);
            cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + op->imm);
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            ++block->owner->control_instructions;
            break;
        case MD_JIT_OP_RET_FAR:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            ++block->owner->control_instructions;
            break;
        case MD_JIT_OP_RET_FAR_IMM:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + op->imm);
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            ++block->owner->control_instructions;
            break;
        case MD_JIT_OP_INT:
            cpu->ip = op->next_ip;
            if (op->aux == 0u || md_x86_of(cpu)) (void)md_runtime_interrupt(runtime, (uint8_t)op->imm);
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            ++block->owner->control_instructions;
            break;
        case MD_JIT_OP_IRET:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            md_x86_set_flags(cpu, (uint16_t)(md_x86_pop(cpu) | MD_X86_FLAG_ALWAYS1));
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            ++block->owner->control_instructions;
            break;
        case MD_JIT_OP_NOP:
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_HLT:
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            runtime->stop_reason = MD_STOP_HALT;
            break;
        case MD_JIT_OP_LODS:
        case MD_JIT_OP_ALU_RR16:
        case MD_JIT_OP_ALU_RR8:
            md_jit_sem_apply(runtime, op);
            cpu->ip = op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_LOOP:
            cpu->ip = md_jit_loop_taken(runtime, op) ? op->target : op->next_ip;
            ++runtime->instructions;
            ++block->owner->direct_instructions;
            break;
        case MD_JIT_OP_FALLBACK:
        default:
            ++block->owner->fallback_instructions;
            (void)md_interp_step(runtime);
            break;
    }
}

static void md_jit_control_one(MdRuntime *runtime, MdJitBlock *block, unsigned index)
{
    MdX86 *cpu;
    const MdJitOp *op;
    int did_control = 1;
    if (runtime == NULL || block == NULL || index >= block->op_count) return;
    if (runtime->stop_reason != MD_STOP_NONE || !md_jit_block_current(block, runtime)) return;
    cpu = &runtime->cpu;
    op = &block->ops[index];
    if (cpu->cs != block->cs || cpu->ip != op->ip) return;
    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_CALL_NEAR:
            md_x86_push(cpu, op->next_ip);
            cpu->ip = op->target;
            break;
        case MD_JIT_OP_RET_NEAR:
            cpu->ip = md_x86_pop(cpu);
            break;
        case MD_JIT_OP_RET_NEAR_IMM:
            cpu->ip = md_x86_pop(cpu);
            cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + op->imm);
            break;
        case MD_JIT_OP_RET_FAR:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            break;
        case MD_JIT_OP_RET_FAR_IMM:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            cpu->r[MD_X86_SP] = (uint16_t)(cpu->r[MD_X86_SP] + op->imm);
            break;
        case MD_JIT_OP_INT:
            cpu->ip = op->next_ip;
            if (op->aux == 0u || md_x86_of(cpu)) (void)md_runtime_interrupt(runtime, (uint8_t)op->imm);
            break;
        case MD_JIT_OP_IRET:
            cpu->ip = md_x86_pop(cpu);
            cpu->cs = md_x86_pop(cpu);
            md_x86_set_flags(cpu, (uint16_t)(md_x86_pop(cpu) | MD_X86_FLAG_ALWAYS1));
            break;
        default:
            did_control = 0;
            break;
    }
    if (did_control) ++block->owner->control_instructions;
}

static void md_jit_materialize(MdRuntime *runtime)
{
    md_x86_flags_materialize(&runtime->cpu);
}

static void md_jit_store8_slow(MdRuntime *runtime, uint32_t linear, uint8_t value)
{
    md_x86_write8_linear(&runtime->cpu, linear, value);
}

/* -------------------------------------------------------------------------
 * Architecture-neutral JIT admission / shape helpers.
 *
 * Thumb-2 was the first native backend, so these helpers originally lived
 * inside its emitter file. They are shared policy/IR logic and belong here.
 * ------------------------------------------------------------------------- */

static int md_jit_direct_alu(unsigned operation)
{
    return operation == 0u || operation == 1u || operation == 4u ||
           operation == 5u || operation == 6u || operation == 7u;
}

static int md_jit_find_op_ip(const MdJitBlock *block, uint16_t ip)
{
    unsigned i;
    for (i = 0u; i < block->op_count; ++i) if (block->ops[i].ip == ip) return (int)i;
    return -1;
}

static unsigned md_jit_direct_prefix(const MdJitBlock *block, int *materialize)
{
    unsigned i, direct = 0u;
    enum MdJitCfState cf = MD_JIT_CF_RAW;
    int needs_materialize = 0;
    /* Decide how far direct lowering can proceed without duplicating subtle
       flag semantics. ADD/SUB leave CF lazy-unknown; a following INC/DEC
       would need md_x86_cf(), so that boundary remains canonical for now. */
    for (i = 0u; i < block->op_count; ++i) {
        const MdJitOp *op = &block->ops[i];
        int ok = 1;
        switch ((MdJitOpKind)op->kind) {
            case MD_JIT_OP_MOV_R8_IMM:
            case MD_JIT_OP_MOV_R16_IMM:
            case MD_JIT_OP_MOV_AL_SI:
            case MD_JIT_OP_MOV_SI_AL:
            case MD_JIT_OP_CALL_NEAR:
            case MD_JIT_OP_RET_NEAR:
            case MD_JIT_OP_RET_NEAR_IMM:
            case MD_JIT_OP_RET_FAR:
            case MD_JIT_OP_RET_FAR_IMM:
            case MD_JIT_OP_INT:
            case MD_JIT_OP_IRET:
            case MD_JIT_OP_NOP:
            case MD_JIT_OP_HLT:
                break;
            case MD_JIT_OP_INC_R16:
            case MD_JIT_OP_DEC_R16:
                needs_materialize = 1;
                if (cf == MD_JIT_CF_LAZY_UNKNOWN) ok = 0;
                break;
            case MD_JIT_OP_ALU_ACC_IMM:
            case MD_JIT_OP_GRP1_R16_IMM:
                if (!md_jit_direct_alu(op->aux)) ok = 0;
                else if (op->aux == 1u || op->aux == 4u || op->aux == 6u) cf = MD_JIT_CF_ZERO;
                else cf = MD_JIT_CF_LAZY_UNKNOWN;
                break;
            case MD_JIT_OP_JCC:
                /* JNZ after INC/DEC; internal back-edge or external exit
                   (th_emit_jnz handles both). */
                if (op->aux != 5u || i == 0u ||
                    (block->ops[i - 1u].kind != MD_JIT_OP_DEC_R16 &&
                     block->ops[i - 1u].kind != MD_JIT_OP_INC_R16)) ok = 0;
                break;
            case MD_JIT_OP_LODS:
            case MD_JIT_OP_LOOP:
                break;                       /* helper-backed (M20.3) */
            case MD_JIT_OP_ALU_RR16:
            case MD_JIT_OP_ALU_RR8:
                cf = (op->aux == 1u || op->aux == 4u || op->aux == 6u) ? MD_JIT_CF_ZERO
                                                                     : MD_JIT_CF_LAZY_UNKNOWN;
                break;
            case MD_JIT_OP_JMP:
                if (md_jit_find_op_ip(block, op->target) < 0) {
                    /* External direct jumps still execute natively, then
                       return to the dispatcher at their exact target. */
                }
                break;
            case MD_JIT_OP_FALLBACK:
            default:
                ok = 0;
                break;
        }
        if (!ok) break;
        ++direct;
    }
    *materialize = needs_materialize;
    return direct;
}

static int md_jit_is_resident_loop(const MdJitBlock *block)
{
    const MdJitOp *o = block->ops;
    return block->op_count == 3u &&
           o[0].kind == MD_JIT_OP_MOV_R16_IMM && o[0].reg == MD_X86_CX && o[0].imm != 0u &&
           o[1].kind == MD_JIT_OP_DEC_R16 && o[1].reg == MD_X86_CX &&
           o[2].kind == MD_JIT_OP_JCC && o[2].aux == 5u &&
           o[2].target == o[1].ip;
}

static int md_jit_is_resident_memloop(const MdJitBlock *block)
{
    const MdJitOp *o = block->ops;
    return block->op_count == 9u &&
           o[0].kind == MD_JIT_OP_MOV_R16_IMM && o[0].reg == MD_X86_CX && o[0].imm != 0u &&
           o[1].kind == MD_JIT_OP_MOV_R16_IMM && o[1].reg == MD_X86_SI && o[1].imm == 0x8000u &&
           o[2].kind == MD_JIT_OP_MOV_AL_SI &&
           o[3].kind == MD_JIT_OP_ALU_ACC_IMM && o[3].reg == 0u && o[3].aux == 0u &&
           o[4].kind == MD_JIT_OP_MOV_SI_AL &&
           o[5].kind == MD_JIT_OP_GRP1_R16_IMM && o[5].reg == MD_X86_SI && o[5].aux == 0u &&
           o[6].kind == MD_JIT_OP_GRP1_R16_IMM && o[6].reg == MD_X86_SI && o[6].aux == 1u && o[6].imm == 0x8000u &&
           o[7].kind == MD_JIT_OP_DEC_R16 && o[7].reg == MD_X86_CX &&
           o[8].kind == MD_JIT_OP_JCC && o[8].aux == 5u && o[8].target == o[2].ip &&
           o[3].imm <= 255u && o[5].imm <= 255u;
}

static int md_jit_op_modifies_reg(const MdJitOp *op, unsigned reg)
{
    if (op == NULL) return 0;
    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R16_IMM:
        case MD_JIT_OP_INC_R16:
        case MD_JIT_OP_DEC_R16:
        case MD_JIT_OP_GRP1_R16_IMM:
            return op->reg == (reg & 7u) && op->aux != 7u;
        case MD_JIT_OP_MOV_R8_IMM:
            return (reg == MD_X86_AX) && ((op->reg & 3u) == MD_X86_AX);
        case MD_JIT_OP_ALU_ACC_IMM:
            return reg == MD_X86_AX && op->aux != 7u;
        case MD_JIT_OP_MOV_AL_SI:
            return reg == MD_X86_AX;
        case MD_JIT_OP_LODS:
            return reg == MD_X86_AX || reg == MD_X86_SI;
        case MD_JIT_OP_ALU_RR16:
            return op->reg == (reg & 7u) && op->aux != 7u;
        case MD_JIT_OP_ALU_RR8:
            return ((op->reg & 3u) == (reg & 7u)) && reg < 4u && op->aux != 7u;
        case MD_JIT_OP_LOOP:
            return reg == MD_X86_CX && op->aux != 3u;
        default:
            return 0;
    }
}

static int md_jit_region_reg_supported(unsigned reg)
{
    switch (reg & 7u) {
        case MD_X86_AX:
        case MD_X86_BX:
        case MD_X86_CX:
        case MD_X86_SI:
            return 1;
        default:
            return 0;
    }
}

static int md_jit_generic_op_supported(const MdJitOp *op)
{
    if (op == NULL) return 0;
    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R16_IMM:
        case MD_JIT_OP_INC_R16:
        case MD_JIT_OP_DEC_R16:
            return md_jit_region_reg_supported(op->reg);
        case MD_JIT_OP_MOV_R8_IMM:
            return op->reg == 0u;                 /* AL only for now */
        case MD_JIT_OP_ALU_ACC_IMM:
            return op->aux == 0u || op->aux == 1u || op->aux == 4u ||
                   op->aux == 5u || op->aux == 6u;
        case MD_JIT_OP_GRP1_R16_IMM:
            return md_jit_region_reg_supported(op->reg) &&
                   (op->aux == 0u || op->aux == 1u || op->aux == 4u ||
                    op->aux == 5u || op->aux == 6u);
        case MD_JIT_OP_MOV_AL_SI:
        case MD_JIT_OP_MOV_SI_AL:
        case MD_JIT_OP_NOP:
            return 1;
        default:
            return 0;
    }
}

static enum MdJitCfState md_jit_generic_cf_after(const MdJitBlock *block, unsigned final_dec)
{
    enum MdJitCfState cf = MD_JIT_CF_RAW;
    unsigned i;
    for (i = 0u; i < final_dec; ++i) {
        const MdJitOp *op = &block->ops[i];
        if (op->kind == MD_JIT_OP_ALU_ACC_IMM || op->kind == MD_JIT_OP_GRP1_R16_IMM) {
            if (op->aux == 1u || op->aux == 4u || op->aux == 6u) cf = MD_JIT_CF_ZERO;
            else if (op->aux == 0u || op->aux == 5u || op->aux == 7u) cf = MD_JIT_CF_LAZY_UNKNOWN;
        }
    }
    return cf;
}

static int md_jit_generic_si_window(const MdJitBlock *block, unsigned loop_start,
                                    unsigned final_dec, int *has_memory, int *has_store)
{
    int known_high = loop_start == 0u;
    unsigned i;
    *has_memory = 0;
    *has_store = 0;

    for (i = 0u; i < loop_start; ++i) {
        const MdJitOp *op = &block->ops[i];
        if (op->kind == MD_JIT_OP_MOV_R16_IMM && op->reg == MD_X86_SI) {
            known_high = (op->imm & 0x8000u) != 0u;
        } else if (md_jit_op_modifies_reg(op, MD_X86_SI)) {
            return 0;                              /* setup no longer provable */
        }
        if (op->kind == MD_JIT_OP_MOV_AL_SI || op->kind == MD_JIT_OP_MOV_SI_AL) return 0;
    }
    if (!known_high) return 0;

    for (i = loop_start; i < final_dec; ++i) {
        const MdJitOp *op = &block->ops[i];
        if (op->kind == MD_JIT_OP_MOV_AL_SI || op->kind == MD_JIT_OP_MOV_SI_AL) {
            if (!known_high) return 0;
            *has_memory = 1;
            if (op->kind == MD_JIT_OP_MOV_SI_AL) *has_store = 1;
            continue;
        }
        if (!md_jit_op_modifies_reg(op, MD_X86_SI)) continue;

        if (op->kind == MD_JIT_OP_MOV_R16_IMM) {
            known_high = (op->imm & 0x8000u) != 0u;
        } else if (op->kind == MD_JIT_OP_GRP1_R16_IMM && op->reg == MD_X86_SI) {
            switch (op->aux) {
                case 1u: /* OR */
                    if ((op->imm & 0x8000u) != 0u) known_high = 1;
                    break;
                case 4u: /* AND */
                    if ((op->imm & 0x8000u) == 0u) known_high = 0;
                    break;
                default:
                    known_high = 0;                /* ADD/SUB/XOR can cross */
                    break;
            }
        } else {
            known_high = 0;
        }
    }
    return known_high;
}

static int md_jit_cfg_cmp_supported(const MdJitOp *op)
{
    if (op->kind == MD_JIT_OP_ALU_ACC_IMM && op->aux == 7u) return op->reg <= 1u;
    if (op->kind == MD_JIT_OP_GRP1_R16_IMM && op->aux == 7u)
        return md_jit_region_reg_supported(op->reg);
    return 0;
}

static int md_jit_shape_cfg(const MdJitBlock *block, unsigned *out_start, uint32_t *out_trips, unsigned *out_edges)
{
    const unsigned n = block->op_count;
    unsigned final_dec, loop_start, i, forward_edges = 0u;
    int cx_setup = -1;
    uint32_t trips = 0u;
    enum MdJitCfState final_cf;
    if (n < 6u) return 0;
    if (block->ops[n - 1u].kind != MD_JIT_OP_JCC || block->ops[n - 1u].aux != 5u) return 0;
    final_dec = n - 2u;
    if (block->ops[final_dec].kind != MD_JIT_OP_DEC_R16 || block->ops[final_dec].reg != MD_X86_CX) return 0;
    {
        const int li = md_jit_find_op_ip(block, block->ops[n - 1u].target);
        if (li < 0 || (unsigned)li >= final_dec) return 0;
        loop_start = (unsigned)li;
    }

    /* Establish CX once before the loop; no memory in CFG-v1.  Internal Jccs
       must be JE/JNE immediately after a CMP and target a later op in-region. */
    for (i = 0u; i < final_dec; ++i) {
        const MdJitOp *op = &block->ops[i];
        if (op->kind == MD_JIT_OP_MOV_AL_SI || op->kind == MD_JIT_OP_MOV_SI_AL) return 0;
        if (md_jit_op_modifies_reg(op, MD_X86_CX)) {
            if (i < loop_start && op->kind == MD_JIT_OP_MOV_R16_IMM && op->reg == MD_X86_CX && cx_setup < 0) {
                cx_setup = (int)i;
                trips = op->imm;
            } else return 0;
        }
        if (op->kind == MD_JIT_OP_JCC) {
            int ti;
            if (i == 0u || (op->aux != 4u && op->aux != 5u) || !md_jit_cfg_cmp_supported(&block->ops[i - 1u])) return 0;
            ti = md_jit_find_op_ip(block, op->target);
            if (ti < 0 || (unsigned)ti <= i || (unsigned)ti > final_dec) return 0;
            ++forward_edges;
            continue;
        }
        if (md_jit_cfg_cmp_supported(op)) {
            if (i + 1u >= final_dec || block->ops[i + 1u].kind != MD_JIT_OP_JCC) return 0;
            continue;
        }
        if (!md_jit_generic_op_supported(op)) return 0;
    }
    if ((cx_setup < 0 && loop_start != 0u) || (cx_setup >= 0 && trips == 0u) || forward_edges == 0u) return 0;

    /* Require a deterministic CF at the final DEC. A logic op after every
       branch path is sufficient and is easy to prove conservatively. */
    final_cf = md_jit_generic_cf_after(block, final_dec);
    if (final_cf != MD_JIT_CF_ZERO) return 0;
    *out_start = loop_start; *out_trips = trips; *out_edges = forward_edges;
    return 1;
}

static int md_jit_shape_counted(const MdJitBlock *block, unsigned *out_start, uint32_t *out_trips, enum MdJitCfState *out_cf, int *out_memory, int *out_store)
{
    const unsigned n = block->op_count;
    unsigned final_dec, loop_start, i;
    int cx_setup = -1, has_memory = 0, has_store = 0;
    uint32_t trips = 0u;
    enum MdJitCfState final_cf;
    if (n < 3u) return 0;
    if (block->ops[n - 1u].kind != MD_JIT_OP_JCC || block->ops[n - 1u].aux != 5u) return 0;
    final_dec = n - 2u;
    if (block->ops[final_dec].kind != MD_JIT_OP_DEC_R16 || block->ops[final_dec].reg != MD_X86_CX) return 0;
    {
        const int li = md_jit_find_op_ip(block, block->ops[n - 1u].target);
        if (li < 0 || (unsigned)li >= final_dec) return 0;
        loop_start = (unsigned)li;
    }

    /* CX must be established exactly once before the loop, so the whole-region
       budget can be proven before any guest state changes. */
    for (i = 0u; i < final_dec; ++i) {
        const MdJitOp *op = &block->ops[i];
        if (!md_jit_generic_op_supported(op)) return 0;
        if (md_jit_op_modifies_reg(op, MD_X86_CX)) {
            if (i < loop_start && op->kind == MD_JIT_OP_MOV_R16_IMM && op->reg == MD_X86_CX && cx_setup < 0) {
                cx_setup = (int)i;
                trips = op->imm;
            } else {
                return 0;
            }
        }
    }
    if ((cx_setup < 0 && loop_start != 0u) || (cx_setup >= 0 && trips == 0u)) return 0;

    final_cf = md_jit_generic_cf_after(block, final_dec);
    if (final_cf == MD_JIT_CF_LAZY_UNKNOWN) return 0;

    /* If memory is used, prove an 8000h..FFFFh [SI] window. */
    for (i = 0u; i < final_dec; ++i) {
        if (block->ops[i].kind == MD_JIT_OP_MOV_AL_SI || block->ops[i].kind == MD_JIT_OP_MOV_SI_AL) {
            if (!md_jit_generic_si_window(block, loop_start, final_dec, &has_memory, &has_store)) return 0;
            break;
        }
    }

    *out_start = loop_start; *out_trips = trips; *out_cf = final_cf;
    *out_memory = has_memory; *out_store = has_store;
    return 1;
}

static int md_jit_lods_reg_supported(unsigned reg)
{
    switch (reg & 7u) {
        case MD_X86_AX:
        case MD_X86_CX:
        case MD_X86_SI:
            return 1;
        default:
            return 0;
    }
}

static int md_jit_shape_lodsloop(const MdJitBlock *block, unsigned *out_start)
{
    const unsigned n = block->op_count;
    const MdJitOp *last;
    int has_lods = 0, li;
    unsigned loop_start, i;
    if (n < 2u || n > 32u) return 0;
    last = &block->ops[n - 1u];
    if (last->kind != MD_JIT_OP_LOOP || last->aux != 2u) return 0;
    li = md_jit_find_op_ip(block, last->target);
    if (li < 0 || (unsigned)li >= n - 1u) return 0;
    loop_start = (unsigned)li;
    for (i = 0u; i + 1u < n; ++i) {
        const MdJitOp *op = &block->ops[i];
        if (op->kind == MD_JIT_OP_LODS) { if (i >= loop_start) has_lods = 1; continue; }
        if (op->kind == MD_JIT_OP_NOP) continue;
        if (op->kind != MD_JIT_OP_ALU_RR16) return 0;
        if ((op->reg & 7u) == MD_X86_SP || (op->imm & 7u) == MD_X86_SP) return 0;
        if (!md_jit_lods_reg_supported(op->reg) &&
            !md_jit_lods_reg_supported(op->imm) &&
            (op->reg & 7u) != (op->imm & 7u)) return 0;
        if (op->aux == 2u || op->aux == 3u) return 0;
    }
    if (!has_lods) return 0;
    *out_start = loop_start;
    return 1;
}

static int md_jit_persist_ops(MdJit *jit, MdJitBlock *block)
{
    size_t at, bytes;
    unsigned si;

    if (jit == NULL || block == NULL || block->ops == NULL || block->op_count == 0u) return 0;

    block->profile_ip = block->ip;
    block->profile_opcode = block->ops[0].opcode;
    if (block->direct_prefix_ops != 0u) {
        si = (unsigned)block->direct_prefix_ops - 1u;
        if (si < block->op_count) {
            block->profile_ip = block->ops[si].ip;
            block->profile_opcode = block->ops[si].opcode;
        }
    }

#if !defined(__arm__) && !defined(__thumb__)
    block->keep_ops = 1u;
#endif

    if (!block->keep_ops) {
        block->ops = NULL;
        return 1;
    }

    bytes = (size_t)block->op_count * sizeof(MdJitOp);
    at = (jit->code_used + 3u) & ~(size_t)3u;
    if (at + bytes > jit->code_size) return 0;

    memcpy(jit->code + at, block->ops, bytes);
    block->ops = (MdJitOp *)(void *)(jit->code + at);
    jit->code_used = at + bytes;
    return 1;
}

/* Shared resident-region helper entrypoints used by native backends. */
static uint32_t md_jit_shared_dec(MdRuntime *rt, MdJitBlock *block, uint32_t budget)
{
    unsigned reg = md_x86_read8(&rt->cpu, block->cs, block->ip) & 7u;
    return md_region_try_dec_jnz(rt, reg, block->ip, block->end_ip, budget);
}

static uint32_t md_jit_shared_checksum(MdRuntime *rt, MdJitBlock *block, uint32_t budget)
{
    return md_region_try_lodsw_add_dx_ax_loop(rt, block->ip, block->end_ip, budget);
}

/* -------------------------------------------------------------------------
 * Native backend selector.
 *
 * Host/reference and RP2350 builds use the Thumb-2 backend. AArch64 builds
 * include only the AArch64 emitter.
 * ------------------------------------------------------------------------- */
#if defined(__aarch64__)
#include "jit_aarch64_backend.inc"
#define MD_JIT_EMIT_ARCH md_jit_emit_aarch64
#else
#include "jit_thumb2_backend.inc"
#define MD_JIT_EMIT_ARCH md_jit_emit_thumb
#endif

static int md_jit_block_current(const MdJitBlock *block, const MdRuntime *runtime)
{
    if (!block->valid || block->code_epoch != runtime->code_epoch) return 0;
    if (runtime->code_page_generation[block->page0] != block->page_gen0) return 0;
    if (block->page_count > 1u &&
        runtime->code_page_generation[block->page1] != block->page_gen1) return 0;
    return 1;
}

static void md_jit_flush_code(MdJit *jit)
{
    memset(jit->blocks, 0, sizeof(jit->blocks));
    jit->code_used = 0u;
    MD_JIT_STAT(++jit->flushes);
}

static bool md_jit_decode_block(MdJit *jit, MdRuntime *runtime, uint16_t cs, uint16_t start_ip, MdJitBlock *block, MdJitProbe *probe)
{
    uint8_t image[MD_JIT_DECODE_WINDOW];
    const size_t avail = (size_t)((0x10000u - (uint32_t)start_ip) < MD_JIT_DECODE_WINDOW
                                      ? (0x10000u - (uint32_t)start_ip)
                                      : MD_JIT_DECODE_WINDOW);
    uint16_t ip;
    size_t i;
    uint32_t linear0, linear1;

    if (avail == 0u) return false;
    for (i = 0u; i < avail; ++i) image[i] = md_x86_read8(&runtime->cpu, cs, (uint16_t)(start_ip + i));

    memset(block, 0, sizeof(*block));
    block->ops = jit->compile_ops;
    block->materialize = md_jit_materialize;
    block->store8_slow = md_jit_store8_slow;
    block->exec_one = md_jit_exec_one;
    block->owner = jit;
    block->cs = cs;
    block->ip = start_ip;
    ip = start_ip;

    while (block->op_count < MD_JIT_MAX_OPS) {
        MdDecodedInstruction inst;
        MdJitOp *op;
        if (!md_decode_8086(image, avail, start_ip, ip, &inst) || !inst.valid_8086) {
            if (block->op_count == 0u) return false;
            break;
        }
        op = &block->ops[block->op_count++];
        md_jit_classify(image, start_ip, &inst, op);
        if (probe != NULL) {
            if (inst.flow == MD_DECODE_FLOW_CALL || inst.flow == MD_DECODE_FLOW_INDIRECT_CALL)
                probe->has_call = 1u;
            if (inst.flow == MD_DECODE_FLOW_RETURN) probe->has_return = 1u;
            if (inst.flow != MD_DECODE_FLOW_FALLTHROUGH) ++probe->control_ops;
        }
        ip = inst.next_ip;
        if (md_jit_boundary(&inst) &&
            !md_jit_continue_forward_conditional(&inst, start_ip, avail)) break;
    }

    if (block->op_count == 0u || ip < start_ip) return false;
    block->end_ip = ip;
    block->source_bytes = (uint16_t)(ip - start_ip);

    linear0 = md_x86_linear(cs, start_ip);
    if (linear0 + block->source_bytes > MD_X86_ADDRESS_SPACE) return false;
    linear1 = linear0 + block->source_bytes - 1u;
    block->page0 = (uint8_t)md_x86_code_page(linear0);
    block->page1 = (uint8_t)md_x86_code_page(linear1);
    block->page_count = block->page0 == block->page1 ? 1u : 2u;

    return true;
}

static MdJitBlock *md_jit_compile(MdJit *jit, MdRuntime *runtime, int allow_live)
{
    const uint16_t cs = runtime->cpu.cs, start_ip = runtime->cpu.ip;
    MdJitBlock *block = &jit->blocks[md_jit_hash(cs, start_ip)];
    if (!md_jit_decode_block(jit, runtime, cs, start_ip, block, NULL)) return NULL;
    md_runtime_mark_code_range(runtime, cs, start_ip, block->source_bytes);
    block->code_epoch = runtime->code_epoch;
    block->page_gen0 = runtime->code_page_generation[block->page0];
    block->page_gen1 = runtime->code_page_generation[block->page1];
    if (!MD_JIT_EMIT_ARCH(jit, block, allow_live) || !md_jit_persist_ops(jit, block)) {
        md_jit_flush_code(jit);
        block = &jit->blocks[md_jit_hash(cs, start_ip)];
        if (!md_jit_decode_block(jit, runtime, cs, start_ip, block, NULL)) return NULL;
        block->code_epoch = runtime->code_epoch;
        block->page_gen0 = runtime->code_page_generation[block->page0];
        block->page_gen1 = runtime->code_page_generation[block->page1];
        if (!MD_JIT_EMIT_ARCH(jit, block, allow_live) || !md_jit_persist_ops(jit, block)) return NULL;
    }
    block->valid = 1u;
    MD_JIT_STAT(++jit->compiles);
    return block;
}

bool md_jit_probe(MdJit *jit, MdRuntime *runtime, uint16_t cs, uint16_t ip, MdJitProbe *probe)
{
    MdJitBlock block;
    unsigned i, loop_start, edges;
    uint32_t trips;
    enum MdJitCfState cf;
    int materialize, memory, store;
    if (probe == NULL) return false;
    memset(probe, 0, sizeof(*probe));
    if (jit == NULL || runtime == NULL || !md_jit_decode_block(jit, runtime, cs, ip, &block, probe))
        return false;
    probe->decoded_ops = block.op_count;
    probe->direct_ops = (uint8_t)md_jit_direct_prefix(&block, &materialize);
    probe->fallback_ops = (uint8_t)(block.op_count - probe->direct_ops);
    probe->pages = block.page_count;
    for (i = 0u; i < block.op_count; ++i) {
        const MdJitOp *op = &block.ops[i];
        if (op->kind == MD_JIT_OP_CALL_NEAR) probe->has_call = 1u;
        if (op->kind >= MD_JIT_OP_RET_NEAR && op->kind <= MD_JIT_OP_RET_FAR_IMM)
            probe->has_return = 1u;
        if ((op->kind == MD_JIT_OP_JCC || op->kind == MD_JIT_OP_JMP || op->kind == MD_JIT_OP_LOOP) &&
            op->target <= op->ip && md_jit_find_op_ip(&block, op->target) >= 0)
            probe->has_backedge = 1u;
    }
    if (md_jit_is_resident_loop(&block) ||
        (block.op_count == 2u && block.ops[0].kind == MD_JIT_OP_DEC_R16 &&
         block.ops[1].kind == MD_JIT_OP_JCC && block.ops[1].aux == 5u &&
         block.ops[1].target == block.ops[0].ip))
        probe->resident_kind = MD_JIT_RESIDENT_DEC_JNZ;
    else if (md_jit_is_resident_memloop(&block)) probe->resident_kind = MD_JIT_RESIDENT_MEMLOOP;
    else if (md_jit_shape_lodsloop(&block, &loop_start)) probe->resident_kind = MD_JIT_RESIDENT_LODS_LOOP;
    else if (md_jit_shape_cfg(&block, &loop_start, &trips, &edges)) probe->resident_kind = MD_JIT_RESIDENT_CFG;
    else if (md_jit_shape_counted(&block, &loop_start, &trips, &cf, &memory, &store))
        probe->resident_kind = memory ? MD_JIT_RESIDENT_MEMLOOP : MD_JIT_RESIDENT_COUNTED;
    return true;
}

bool md_jit_prepare_region(MdJit *jit, MdRuntime *runtime)
{
    MdJitBlock *block;
    if (jit == NULL || runtime == NULL || jit->code == NULL || jit->code_size < 64u)
        return false;
    MD_JIT_STAT(++jit->lookups);
    block = &jit->blocks[md_jit_hash(runtime->cpu.cs, runtime->cpu.ip)];
    if (block->cs == runtime->cpu.cs && block->ip == runtime->cpu.ip &&
        md_jit_block_current(block, runtime)) {
        MD_JIT_STAT(++jit->hits);
        return true;
    }
    MD_JIT_STAT(++jit->misses);
    /* Router heat replaces the legacy per-lookup hotness gate. */
    return md_jit_compile(jit, runtime, 1) != NULL;
}

static MdJitBlock *md_jit_get(MdJit *jit, MdRuntime *runtime)
{
    MdJitBlock *block;
    const unsigned slot = md_jit_hash(runtime->cpu.cs, runtime->cpu.ip);

    MD_JIT_STAT(++jit->lookups);
    block = &jit->blocks[slot];
    if (block->valid && block->cs == runtime->cpu.cs && block->ip == runtime->cpu.ip) {
        if (md_jit_block_current(block, runtime)) {
            MD_JIT_STAT(++jit->hits);
            return block;
        }
        block->valid = 0u;
        MD_JIT_STAT(++jit->invalidations);
    }

    MD_JIT_STAT(++jit->misses);
    jit->last_lookup_cold = 0u;
    if (!md_jit_hot_ready(jit, runtime->cpu.cs, runtime->cpu.ip)) {
        jit->last_lookup_cold = 1u;
        return NULL;
    }
    return md_jit_compile(jit, runtime, 0);
}

void md_jit_init(MdJit *jit, void *code, size_t code_size)
{
    if (jit == NULL) return;
    memset(jit, 0, sizeof(*jit));
    jit->code = (uint8_t *)code;
    jit->code_size = code_size & ~(size_t)1u;
}

void md_jit_reset(MdJit *jit)
{
    uint8_t *code;
    size_t code_size;
    if (jit == NULL) return;
    code = jit->code;
    code_size = jit->code_size;
    memset(jit, 0, sizeof(*jit));
    jit->code = code;
    jit->code_size = code_size;
}

static uint32_t md_jit_execute_block(MdRuntime *runtime, MdJitBlock *block, uint32_t budget)
{
    if (block->resident == 2u || block->resident == 3u) {
        uint32_t retired = block->native(runtime, block, budget);
#if !defined(__arm__) && !defined(__thumb__) && !defined(__aarch64__)
        runtime->instructions += retired;
        MD_JIT_STAT(block->owner->direct_instructions += retired);
#endif
        return retired;
    }
#if defined(__arm__) || defined(__thumb__) || defined(__aarch64__)
    return block->native(runtime, block, budget);
#else
    unsigned i;
    uint32_t before = (uint32_t)runtime->instructions;
    for (i = 0u; i < block->op_count && runtime->stop_reason == MD_STOP_NONE; ++i) {
        if ((uint32_t)(runtime->instructions - before) >= budget) break;
        /*
         * Match the Thumb backend: an unsupported first op means native
         * execution made zero progress.  Previously the host reference path
         * interpreted MD_JIT_OP_FALLBACK inside exec_one(), so host tests
         * could not exercise the zero-progress escape path at all.
         */
        if (block->ops[i].kind == MD_JIT_OP_FALLBACK) break;
        block->exec_one(runtime, block, i);
        if (runtime->cpu.ip != block->ops[i].next_ip) break;
    }
    return (uint32_t)(runtime->instructions - before);
#endif
}

MdStopReason md_jit_run_region(MdJit *jit, MdRuntime *runtime, uint64_t budget,
                               MdJitRunResult *result)
{
    MdJitBlock *block;
    uint16_t cs, ip;
    uint32_t left;
    if (result == NULL) return MD_STOP_FAULT;
    memset(result, 0, sizeof(*result));
    if (jit == NULL || runtime == NULL) return MD_STOP_FAULT;
    if (runtime->stop_reason != MD_STOP_NONE || budget == 0u) return runtime->stop_reason;
    cs = runtime->cpu.cs; ip = runtime->cpu.ip;
    left = (uint32_t)(budget > 0x7fffffffu ? 0x7fffffffu : budget);
    block = &jit->blocks[md_jit_hash(cs, ip)];
    if (!block->valid || block->cs != cs || block->ip != ip) return MD_STOP_NONE;
    if (!md_jit_block_current(block, runtime)) {
        result->invalidated = 1u;
        MD_JIT_STAT(++jit->invalidations);
        return MD_STOP_NONE;
    }
    while (left != 0u && runtime->stop_reason == MD_STOP_NONE &&
           runtime->cpu.cs == cs && runtime->cpu.ip == ip) {
        uint32_t retired;
        if (left < block->op_count) { result->budget_limited = 1u; break; }
        if (!md_jit_block_current(block, runtime)) { result->invalidated = 1u; break; }
        ++result->entries;
        MD_JIT_STAT(++jit->native_entries);
        if (block->resident) MD_JIT_STAT(++jit->resident_entries);
        if (block->generic_region) MD_JIT_STAT(++jit->generic_entries);
        if (block->cfg_region) MD_JIT_STAT(++jit->cfg_entries);
        retired = md_jit_execute_block(runtime, block, left);
#if defined(__arm__) || defined(__thumb__) || defined(__aarch64__)
        runtime->instructions += retired;
        MD_JIT_STAT(jit->direct_instructions += retired);
#endif
        result->retired += retired;
        result->native += retired;
        MD_JIT_STAT(++jit->native_returns);
        left -= retired;
        if (block->resident) MD_JIT_STAT(jit->resident_instructions += retired);
        if (block->generic_region) MD_JIT_STAT(jit->generic_instructions += retired);
        if (block->cfg_region) MD_JIT_STAT(jit->cfg_instructions += retired);
        if (!md_jit_block_current(block, runtime)) { result->invalidated = 1u; break; }
        if (retired == 0u) {
            const uint32_t trips = runtime->cpu.r[MD_X86_CX] ? runtime->cpu.r[MD_X86_CX] : 65536u;
            if (block->generic_region && (uint64_t)trips * block->op_count > left)
                result->budget_limited = 1u;
            else { ++result->zero_exits; MD_JIT_STAT(++jit->zero_progress_fallbacks); }
            break;
        }
    }
    if (left == 0u && runtime->cpu.cs == cs && runtime->cpu.ip == ip)
        result->budget_limited = 1u;
    result->cs_changed = runtime->cpu.cs != cs;
    return runtime->stop_reason;
}

static MdStopReason md_jit_run_common(MdJit *jit, MdRuntime *runtime,
                                      uint64_t instruction_budget, int watch_cs)
{
    const uint64_t start = runtime->instructions;
    const uint16_t cs0 = runtime->cpu.cs;

    if (jit == NULL || runtime == NULL || jit->code == NULL || jit->code_size < 64u) {
        if (runtime != NULL) runtime->stop_reason = MD_STOP_FAULT;
        return runtime != NULL ? runtime->stop_reason : MD_STOP_FAULT;
    }
    if (instruction_budget == 0u) instruction_budget = UINT64_MAX;

    while (runtime->stop_reason == MD_STOP_NONE) {
        const uint64_t used = runtime->instructions - start;
        uint64_t left;
        MdJitBlock *block;
        uint32_t retired;
        uint16_t src_cs, src_ip;
        uint8_t src_opcode;

        if (watch_cs && runtime->cpu.cs != cs0) {
            MD_JIT_STAT(++jit->cs_change_exits);
            md_jit_record_exit(jit, runtime, MD_JIT_EXIT_CS_CHANGE);
            return MD_STOP_NONE;
        }
        if (used >= instruction_budget) {
            runtime->stop_reason = MD_STOP_BUDGET;
            break;
        }
        left = instruction_budget - used;

        block = md_jit_get(jit, runtime);
        if (block == NULL) {
            const uint16_t fcs = runtime->cpu.cs, fip = runtime->cpu.ip;
            const uint8_t fop = md_x86_read8(&runtime->cpu, fcs, fip);
            MD_JIT_STAT(++jit->boundary_fallbacks);
            MD_JIT_STAT(++jit->fallback_instructions);
            if (jit->last_lookup_cold) {
                MD_JIT_STAT(++jit->cold_fallbacks);
                md_jit_record_exit(jit, runtime, MD_JIT_EXIT_COLD_FALLBACK);
            } else {
                MD_JIT_STAT(++jit->compile_fail_fallbacks);
                md_jit_record_exit(jit, runtime, MD_JIT_EXIT_COMPILE_FAIL);
            }
            (void)md_interp_step(runtime);
            if (watch_cs && runtime->cpu.cs != cs0) {
                MD_JIT_STAT(++jit->cs_change_exits);
                ++jit->exit_reason[MD_JIT_EXIT_CS_CHANGE];
                md_jit_record_transfer(jit, runtime, MD_JIT_EXIT_CS_CHANGE, fcs, fip, fop);
                return MD_STOP_NONE;
            }
            continue;
        }

        src_cs = runtime->cpu.cs;
        src_ip = block->profile_ip;
        src_opcode = block->profile_opcode;

        /* First trip through a freshly discovered basic block needs enough
           budget for its statically straight-line prefix. Internal backedges
           do their own exact remaining-budget check before chaining. */
        if (left < block->op_count) {
            const uint16_t fcs = runtime->cpu.cs, fip = runtime->cpu.ip;
            const uint8_t fop = md_x86_read8(&runtime->cpu, fcs, fip);
            MD_JIT_STAT(++jit->boundary_fallbacks);
            MD_JIT_STAT(++jit->fallback_instructions);
            MD_JIT_STAT(++jit->budget_fallbacks);
            md_jit_record_exit(jit, runtime, MD_JIT_EXIT_BUDGET_FALLBACK);
            (void)md_interp_step(runtime);
            if (watch_cs && runtime->cpu.cs != cs0) {
                MD_JIT_STAT(++jit->cs_change_exits); ++jit->exit_reason[MD_JIT_EXIT_CS_CHANGE];
                md_jit_record_transfer(jit, runtime, MD_JIT_EXIT_CS_CHANGE, fcs, fip, fop);
                return MD_STOP_NONE;
            }
            continue;
        }

        MD_JIT_STAT(++jit->native_entries);
        if (block->resident) MD_JIT_STAT(++jit->resident_entries);
        if (block->generic_region) MD_JIT_STAT(++jit->generic_entries);
        if (block->cfg_region) MD_JIT_STAT(++jit->cfg_entries);
#if defined(__arm__) || defined(__thumb__) || defined(__aarch64__)
        retired = md_jit_execute_block(runtime, block,
                    (uint32_t)(left > 0x7FFFFFFFu ? 0x7FFFFFFFu : left));
        runtime->instructions += retired;
        MD_JIT_STAT(jit->direct_instructions += retired);
        if (block->resident) MD_JIT_STAT(jit->resident_instructions += retired);
        if (block->generic_region) MD_JIT_STAT(jit->generic_instructions += retired);
        if (block->cfg_region) MD_JIT_STAT(jit->cfg_instructions += retired);
#else
        /* Host/reference md_jit_exec_one already updates the counters. */
        retired = md_jit_execute_block(runtime, block,
                    (uint32_t)(left > 0x7FFFFFFFu ? 0x7FFFFFFFu : left));
#endif
        if (retired == 0u && runtime->stop_reason == MD_STOP_NONE) {
            const uint32_t escape_budget =
                left > (uint64_t)MD_JIT_ZERO_ESCAPE_BURST
                    ? (uint32_t)MD_JIT_ZERO_ESCAPE_BURST
                    : (uint32_t)left;
            uint32_t escaped = 0u;

            /*
             * M22.0 "JIT must not hurt":
             *
             * A zero-progress native exit means the instruction at the
             * current IP is outside the direct backend.  Interpreting one
             * instruction and immediately retrying JIT lookup caused
             * millions of native->C->interpreter->C->lookup transitions in
             * MDSTRESS.  Stay in the canonical interpreter for a short,
             * exact-budget burst instead.
             *
             * zero_progress_fallbacks remains an EXIT/event counter.
             * fallback_instructions remains an INSTRUCTION counter and is
             * therefore increased by every instruction retired by the burst.
             */
            MD_JIT_STAT(++jit->boundary_fallbacks);
            MD_JIT_STAT(++jit->zero_progress_fallbacks);
            md_jit_record_exit(jit, runtime, MD_JIT_EXIT_ZERO_PROGRESS);

            while (escaped < escape_budget &&
                   runtime->stop_reason == MD_STOP_NONE) {
                const uint16_t fcs = runtime->cpu.cs;
                const uint16_t fip = runtime->cpu.ip;
                const uint8_t fop = md_x86_read8(&runtime->cpu, fcs, fip);
                const uint64_t before_escape = runtime->instructions;
                uint64_t delta;

                (void)md_interp_step(runtime);
                delta = runtime->instructions - before_escape;
                if (delta == 0u) break;
                escaped += (uint32_t)delta;

                if (watch_cs && runtime->cpu.cs != cs0) {
                    MD_JIT_STAT(jit->fallback_instructions += escaped);
                    MD_JIT_STAT(++jit->cs_change_exits);
#if MD_JIT_PROFILE
                    ++jit->exit_reason[MD_JIT_EXIT_CS_CHANGE];
#endif
                    md_jit_record_transfer(jit, runtime, MD_JIT_EXIT_CS_CHANGE,
                                           fcs, fip, fop);
                    return MD_STOP_NONE;
                }
            }

            MD_JIT_STAT(jit->fallback_instructions += escaped);
            continue;
        }

        if (runtime->stop_reason != MD_STOP_NONE) {
            MD_JIT_STAT(++jit->stop_exits);
            md_jit_record_exit(jit, runtime, MD_JIT_EXIT_STOP);
            break;
        }
        if (watch_cs && runtime->cpu.cs != cs0) {
            MD_JIT_STAT(++jit->cs_change_exits);
            ++jit->exit_reason[MD_JIT_EXIT_CS_CHANGE];
            md_jit_record_transfer(jit, runtime, MD_JIT_EXIT_CS_CHANGE,
                                   src_cs, src_ip, src_opcode);
            return MD_STOP_NONE;
        }
#if MD_JIT_PROFILE
        if (retired != 0u) {
            ++jit->native_returns;
            ++jit->exit_reason[MD_JIT_EXIT_NATIVE_RETURN];
            /* Returning to the C dispatcher can be very frequent. Keep the
               aggregate exact but sample 1/64 returns for hot-site profiling
               so observability does not become the new bottleneck. */
            if ((jit->native_returns & 63u) == 0u)
                md_jit_record_site(jit, runtime, MD_JIT_EXIT_NATIVE_RETURN);
        }
#endif
    }
    return runtime->stop_reason;
}

MdStopReason md_jit_run(MdJit *jit, MdRuntime *runtime, uint64_t instruction_budget)
{
    return md_jit_run_common(jit, runtime, instruction_budget, 0);
}

MdStopReason md_jit_run_until_cs_change(MdJit *jit, MdRuntime *runtime,
                                         uint64_t instruction_budget)
{
    return md_jit_run_common(jit, runtime, instruction_budget, 1);
}
