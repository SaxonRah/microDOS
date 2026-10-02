#include "microdos/jit.h"
#include "microdos/ops.h"

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

#if MD_JIT_PROFILE
#define MD_JIT_STAT(expr_) do { expr_; } while (0)
#else
#define MD_JIT_STAT(expr_) do { } while (0)
#endif

/* Generated functions use low ARM registers only:
 *   r4 = MdRuntime/MdX86 *, r5 = MdJitBlock *, r6 = retired, r7 = budget
 *   r0-r3 = temporaries
 *
 * Keeping runtime==cpu is intentional: MdRuntime begins with MdX86. */

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
#if MD_JIT_HOT_THRESHOLD <= 1
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
static void md_jit_sem_one(MdRuntime *runtime, MdJitBlock *block, unsigned index)
{
    md_jit_sem_apply(runtime, &block->ops[index]);
}

static unsigned md_jit_loop_one(MdRuntime *runtime, MdJitBlock *block, unsigned index)
{
    return (unsigned)md_jit_loop_taken(runtime, &block->ops[index]);
}

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

static void md_jit_sync_code(void)
{
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("dsb sy\n\tisb sy" ::: "memory");
#endif
}

/* ---- tiny Thumb emitter ------------------------------------------------ */

typedef struct MdThumbBuf {
    uint8_t bytes[MD_JIT_NATIVE_TMP];
    size_t at;
    int failed;
} MdThumbBuf;

static void th16(MdThumbBuf *b, uint16_t hw)
{
    if (b->failed) return;
    if (b->at + 2u > sizeof(b->bytes)) { b->failed = 1; return; }
    b->bytes[b->at++] = (uint8_t)hw;
    b->bytes[b->at++] = (uint8_t)(hw >> 8);
}

/* Patch an already-emitted halfword. A placeholder created after the scratch
   buffer filled up points at or past its end; patching it must not write
   out of bounds (it used to overwrite MdThumbBuf.at/.failed on the stack). */
static int th_patch16(MdThumbBuf *b, size_t at, uint16_t hw)
{
    if (b->failed || at + 2u > sizeof(b->bytes)) { b->failed = 1; return 0; }
    b->bytes[at] = (uint8_t)hw;
    b->bytes[at + 1u] = (uint8_t)(hw >> 8);
    return 1;
}

static uint16_t th_mov(unsigned rd, unsigned rm) { return (uint16_t)(0x4600u | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_movs(unsigned rd, unsigned imm) { return (uint16_t)(0x2000u | ((rd & 7u) << 8) | (imm & 0xFFu)); }
static uint16_t th_add_imm(unsigned rd, unsigned imm) { return (uint16_t)(0x3000u | ((rd & 7u) << 8) | (imm & 0xFFu)); }
static uint16_t th_sub_imm(unsigned rd, unsigned imm) { return (uint16_t)(0x3800u | ((rd & 7u) << 8) | (imm & 0xFFu)); }
static uint16_t th_cmp_imm(unsigned rn, unsigned imm) { return (uint16_t)(0x2800u | ((rn & 7u) << 8) | (imm & 0xFFu)); }
/* MOV with high registers (T1 encoding, any of r0-r15). */
static uint16_t th_mov_hi(unsigned rd, unsigned rm) { return (uint16_t)(0x4600u | ((rd & 8u) << 4) | ((rm & 15u) << 3) | (rd & 7u)); }
static uint16_t th_cmp_reg(unsigned rn, unsigned rm) { return (uint16_t)(0x4280u | ((rm & 7u) << 3) | (rn & 7u)); }
static uint16_t th_lsl_imm(unsigned rd, unsigned rm, unsigned imm) { return (uint16_t)(((imm & 31u) << 6) | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_lsr_imm(unsigned rd, unsigned rm, unsigned imm) { return (uint16_t)(0x0800u | ((imm & 31u) << 6) | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_add_reg(unsigned rd, unsigned rn, unsigned rm) { return (uint16_t)(0x1800u | ((rm & 7u) << 6) | ((rn & 7u) << 3) | (rd & 7u)); }
static uint16_t th_sub_reg(unsigned rd, unsigned rn, unsigned rm) { return (uint16_t)(0x1A00u | ((rm & 7u) << 6) | ((rn & 7u) << 3) | (rd & 7u)); }
static uint16_t th_and_reg(unsigned rd, unsigned rm) { return (uint16_t)(0x4000u | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_eor_reg(unsigned rd, unsigned rm) { return (uint16_t)(0x4040u | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_orr_reg(unsigned rd, unsigned rm) { return (uint16_t)(0x4300u | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_uxtb(unsigned rd, unsigned rm) { return (uint16_t)(0xB2C0u | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_uxth(unsigned rd, unsigned rm) { return (uint16_t)(0xB280u | ((rm & 7u) << 3) | (rd & 7u)); }
static uint16_t th_ldrh(unsigned rt, unsigned rn, unsigned off) { return (uint16_t)(0x8800u | (((off >> 1) & 31u) << 6) | ((rn & 7u) << 3) | (rt & 7u)); }
static uint16_t th_strh(unsigned rt, unsigned rn, unsigned off) { return (uint16_t)(0x8000u | (((off >> 1) & 31u) << 6) | ((rn & 7u) << 3) | (rt & 7u)); }
static uint16_t th_ldrb(unsigned rt, unsigned rn, unsigned off) { return (uint16_t)(0x7800u | ((off & 31u) << 6) | ((rn & 7u) << 3) | (rt & 7u)); }
static uint16_t th_strb(unsigned rt, unsigned rn, unsigned off) { return (uint16_t)(0x7000u | ((off & 31u) << 6) | ((rn & 7u) << 3) | (rt & 7u)); }
static uint16_t th_ldr(unsigned rt, unsigned rn, unsigned off) { return (uint16_t)(0x6800u | (((off >> 2) & 31u) << 6) | ((rn & 7u) << 3) | (rt & 7u)); }
static uint16_t th_ldrb_reg(unsigned rt, unsigned rn, unsigned rm) { return (uint16_t)(0x5C00u | ((rm & 7u) << 6) | ((rn & 7u) << 3) | (rt & 7u)); }
static uint16_t th_strb_reg(unsigned rt, unsigned rn, unsigned rm) { return (uint16_t)(0x5400u | ((rm & 7u) << 6) | ((rn & 7u) << 3) | (rt & 7u)); }

static int th_offset_ok_h(unsigned off) { return (off & 1u) == 0u && off <= 62u; }
static int th_offset_ok_b(unsigned off) { return off <= 31u; }
static int th_offset_ok_w(unsigned off) { return (off & 3u) == 0u && off <= 124u; }

static void th_load_imm16(MdThumbBuf *b, unsigned rd, uint16_t value)
{
    const unsigned hi = value >> 8;
    const unsigned lo = value & 0xFFu;
    if (hi == 0u) {
        th16(b, th_movs(rd, lo));
    } else {
        th16(b, th_movs(rd, hi));
        th16(b, th_lsl_imm(rd, rd, 8u));
        if (lo != 0u) th16(b, th_add_imm(rd, lo));
    }
}

/* Compact, literal-pool-free 32-bit constant load. This is deliberately used
 * only at region entry/exit, never in a hot loop. */
static void th_load_imm32(MdThumbBuf *b, unsigned rd, uint32_t value)
{
    unsigned shift;
    int started = 0;
    for (shift = 24u;; shift -= 8u) {
        const unsigned byte = (unsigned)((value >> shift) & 0xFFu);
        if (!started) {
            if (byte != 0u || shift == 0u) {
                th16(b, th_movs(rd, byte));
                started = 1;
            }
        } else {
            th16(b, th_lsl_imm(rd, rd, 8u));
            if (byte != 0u) th16(b, th_add_imm(rd, byte));
        }
        if (shift == 0u) break;
    }
}

static void th_return_const(MdThumbBuf *b, uint32_t retired)
{
    th_load_imm32(b, 0u, retired);
    th16(b, 0xBDF8u);                    /* pop {r3,r4,r5,r6,r7,pc} */
}

static void th_store_ip(MdThumbBuf *b, uint16_t ip)
{
    const unsigned off = (unsigned)offsetof(MdX86, ip);
    if (!th_offset_ok_h(off)) { b->failed = 1; return; }
    th_load_imm16(b, 0u, ip);
    th16(b, th_strh(0u, 4u, off));
}

static void th_return(MdThumbBuf *b)
{
    th16(b, th_mov(0u, 6u));             /* r0 = retired */
    th16(b, 0xBDF8u);                    /* pop {r3,r4,r5,r6,r7,pc} */
}

static int th_patch_bcond(MdThumbBuf *b, size_t at, unsigned cond, size_t target)
{
    const intptr_t delta = (intptr_t)target - (intptr_t)(at + 4u);
    intptr_t imm;
    uint16_t hw;
    if ((delta & 1) != 0) return 0;
    imm = delta / 2;
    if (imm < -128 || imm > 127) return 0;
    hw = (uint16_t)(0xD000u | ((cond & 0xFu) << 8) | ((uint8_t)imm));
    return th_patch16(b, at, hw);
}

static int th_emit_b(MdThumbBuf *b, size_t target)
{
    const intptr_t delta = (intptr_t)target - (intptr_t)(b->at + 4u);
    intptr_t imm;
    if ((delta & 1) != 0) return 0;
    imm = delta / 2;
    if (imm < -1024 || imm > 1023) return 0;
    th16(b, (uint16_t)(0xE000u | ((uint16_t)imm & 0x07FFu)));
    return !b->failed;
}

static size_t th_emit_bcond_placeholder(MdThumbBuf *b, unsigned cond)
{
    const size_t at = b->at;
    th16(b, (uint16_t)(0xD000u | ((cond & 0xFu) << 8)));
    return at;
}

/* Lazy flag fields all live close to MdX86's beginning on ARM. */
static void th_lazy_common(MdThumbBuf *b, unsigned lazy_op,
                           unsigned a_reg, uint16_t imm_b, unsigned result_reg)
{
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    const unsigned aoff = (unsigned)offsetof(MdX86, lazy_a);
    const unsigned boff = (unsigned)offsetof(MdX86, lazy_b);
    const unsigned roff = (unsigned)offsetof(MdX86, lazy_res);
    if (!th_offset_ok_b(opoff) || !th_offset_ok_h(aoff) ||
        !th_offset_ok_h(boff) || !th_offset_ok_h(roff)) { b->failed = 1; return; }
    th16(b, th_movs(2u, lazy_op));
    th16(b, th_strb(2u, 4u, opoff));
    th16(b, th_strh(a_reg, 4u, aoff));
    th_load_imm16(b, 2u, imm_b);
    th16(b, th_strh(2u, 4u, boff));
    th16(b, th_strh(result_reg, 4u, roff));
}

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

static void th_emit_materialize(MdThumbBuf *b)
{
    const unsigned off = (unsigned)offsetof(MdJitBlock, materialize);
    if (!th_offset_ok_w(off)) { b->failed = 1; return; }
    th16(b, th_mov(0u, 4u));
    th16(b, th_ldr(3u, 5u, off));
    th16(b, 0x4798u);                    /* blx r3 */
}

static void th_emit_incdec16(MdThumbBuf *b, const MdJitOp *op, int dec, enum MdJitCfState cf)
{
    const unsigned roff = (unsigned)(offsetof(MdX86, r) + (size_t)op->reg * 2u);
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    const unsigned aoff = (unsigned)offsetof(MdX86, lazy_a);
    const unsigned boff = (unsigned)offsetof(MdX86, lazy_b);
    const unsigned resoff = (unsigned)offsetof(MdX86, lazy_res);
    if (!th_offset_ok_h(roff) || !th_offset_ok_h(foff) || !th_offset_ok_b(coff) ||
        !th_offset_ok_b(opoff) || !th_offset_ok_h(aoff) || !th_offset_ok_h(boff) ||
        !th_offset_ok_h(resoff)) { b->failed = 1; return; }

    th16(b, th_ldrh(0u, 4u, roff));      /* r0 old */
    th16(b, th_mov(1u, 0u));             /* r1 result */
    th16(b, dec ? th_sub_imm(1u, 1u) : th_add_imm(1u, 1u));
    th16(b, th_strh(1u, 4u, roff));

    if (cf == MD_JIT_CF_ZERO) {
        th16(b, th_movs(2u, 0u));
    } else {
        /* CF is materialised in flags_raw and INC/DEC preserve it. */
        th16(b, th_ldrh(2u, 4u, foff));
        th16(b, th_movs(3u, 1u));
        th16(b, th_and_reg(2u, 3u));
    }
    th16(b, th_strb(2u, 4u, coff));
    th16(b, th_movs(2u, dec ? MD_LAZY_DEC16 : MD_LAZY_INC16));
    th16(b, th_strb(2u, 4u, opoff));
    th16(b, th_strh(0u, 4u, aoff));
    th16(b, th_movs(2u, 1u));
    th16(b, th_strh(2u, 4u, boff));
    th16(b, th_strh(1u, 4u, resoff));
}

static void th_emit_alu_acc(MdThumbBuf *b, const MdJitOp *op)
{
    const unsigned operation = op->aux;
    const unsigned axoff = (unsigned)offsetof(MdX86, r);
    unsigned lazy;

    if (op->reg == 0u) {                 /* AL */
        if (!th_offset_ok_b(axoff)) { b->failed = 1; return; }
        th16(b, th_ldrb(0u, 4u, axoff)); /* old/result */
        th16(b, th_mov(1u, 0u));         /* r1 old */
        th_load_imm16(b, 2u, op->imm);
        if (operation == 0u) th16(b, th_add_reg(0u, 0u, 2u));
        else if (operation == 1u) th16(b, th_orr_reg(0u, 2u));
        else if (operation == 4u) th16(b, th_and_reg(0u, 2u));
        else if (operation == 5u || operation == 7u) th16(b, th_sub_reg(0u, 0u, 2u));
        else if (operation == 6u) th16(b, th_eor_reg(0u, 2u));
        else { b->failed = 1; return; }
        th16(b, th_uxtb(0u, 0u));
        if (operation != 7u) th16(b, th_strb(0u, 4u, axoff));
        if (operation == 0u) lazy = MD_LAZY_ADD8;
        else if (operation == 5u || operation == 7u) lazy = MD_LAZY_SUB8;
        else lazy = MD_LAZY_LOGIC8;
        th_lazy_common(b, lazy, 1u, op->imm, 0u);
    } else {                              /* AX */
        if (!th_offset_ok_h(axoff)) { b->failed = 1; return; }
        th16(b, th_ldrh(0u, 4u, axoff));
        th16(b, th_mov(1u, 0u));
        th_load_imm16(b, 2u, op->imm);
        if (operation == 0u) th16(b, th_add_reg(0u, 0u, 2u));
        else if (operation == 1u) th16(b, th_orr_reg(0u, 2u));
        else if (operation == 4u) th16(b, th_and_reg(0u, 2u));
        else if (operation == 5u || operation == 7u) th16(b, th_sub_reg(0u, 0u, 2u));
        else if (operation == 6u) th16(b, th_eor_reg(0u, 2u));
        else { b->failed = 1; return; }
        if (operation != 7u) th16(b, th_strh(0u, 4u, axoff));
        if (operation == 0u) lazy = MD_LAZY_ADD16;
        else if (operation == 5u || operation == 7u) lazy = MD_LAZY_SUB16;
        else lazy = MD_LAZY_LOGIC16;
        th_lazy_common(b, lazy, 1u, op->imm, 0u);
    }
}

static void th_emit_grp1_r16(MdThumbBuf *b, const MdJitOp *op)
{
    const unsigned operation = op->aux;
    const unsigned roff = (unsigned)(offsetof(MdX86, r) + (size_t)op->reg * 2u);
    unsigned lazy;
    if (!th_offset_ok_h(roff)) { b->failed = 1; return; }
    th16(b, th_ldrh(0u, 4u, roff));
    th16(b, th_mov(1u, 0u));              /* r1 old */
    th_load_imm16(b, 2u, op->imm);
    if (operation == 0u) th16(b, th_add_reg(0u, 0u, 2u));
    else if (operation == 1u) th16(b, th_orr_reg(0u, 2u));
    else if (operation == 4u) th16(b, th_and_reg(0u, 2u));
    else if (operation == 5u || operation == 7u) th16(b, th_sub_reg(0u, 0u, 2u));
    else if (operation == 6u) th16(b, th_eor_reg(0u, 2u));
    else { b->failed = 1; return; }
    if (operation != 7u) th16(b, th_strh(0u, 4u, roff));
    if (operation == 0u) lazy = MD_LAZY_ADD16;
    else if (operation == 5u || operation == 7u) lazy = MD_LAZY_SUB16;
    else lazy = MD_LAZY_LOGIC16;
    th_lazy_common(b, lazy, 1u, op->imm, 0u);
}

static void th_emit_linear_ds_si(MdThumbBuf *b)
{
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned sioff = (unsigned)(offsetof(MdX86, r) + MD_X86_SI * 2u);
    if (!th_offset_ok_h(dsoff) || !th_offset_ok_h(sioff)) { b->failed = 1; return; }
    th16(b, th_ldrh(0u, 4u, dsoff));
    th16(b, th_lsl_imm(0u, 0u, 4u));
    th16(b, th_ldrh(1u, 4u, sioff));
    th16(b, th_add_reg(0u, 0u, 1u));
    th16(b, th_lsl_imm(0u, 0u, 12u));     /* real-mode 20-bit wrap */
    th16(b, th_lsr_imm(0u, 0u, 12u));
}

static void th_emit_mov_al_si(MdThumbBuf *b)
{
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned axoff = (unsigned)offsetof(MdX86, r);
    if (!th_offset_ok_w(moff) || !th_offset_ok_b(axoff)) { b->failed = 1; return; }
    th_emit_linear_ds_si(b);               /* r0 linear */
    th16(b, th_ldr(2u, 4u, moff));
    th16(b, th_ldrb_reg(1u, 2u, 0u));
    th16(b, th_strb(1u, 4u, axoff));
}

/* Emits MOV [SI],AL. Returns 1 when the common fast path falls through. The
 * executable-page slow path completes the store and returns from the native
 * region immediately, so stale generated instructions are never executed. */
static void th_emit_mov_si_al(MdThumbBuf *b, const MdJitOp *op)
{
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned xoff = (unsigned)offsetof(MdX86, code_page_executable);
    const unsigned axoff = (unsigned)offsetof(MdX86, r);
    const unsigned soff = (unsigned)offsetof(MdJitBlock, store8_slow);
    size_t bne_slow, b_skip_slow;

    if (!th_offset_ok_w(moff) || !th_offset_ok_w(xoff) || !th_offset_ok_b(axoff) ||
        !th_offset_ok_w(soff)) { b->failed = 1; return; }

    th_emit_linear_ds_si(b);               /* r0 = linear */
    th16(b, th_ldrb(1u, 4u, axoff));       /* r1 = AL */
    th16(b, th_ldr(2u, 4u, xoff));
    th16(b, th_lsr_imm(3u, 0u, MD_X86_CODE_PAGE_SHIFT));
    th16(b, th_ldrb_reg(3u, 2u, 3u));
    th16(b, th_cmp_imm(3u, 0u));
    bne_slow = th_emit_bcond_placeholder(b, 1u); /* BNE slow */

    /* Fast path: ordinary data page, so no invalidation work is needed. */
    th16(b, th_ldr(2u, 4u, moff));
    th16(b, th_strb_reg(1u, 2u, 0u));
    b_skip_slow = b->at;
    th16(b, 0xE000u);                      /* patched B done */

    {
        const size_t slow = b->at;
        if (!th_patch_bcond(b, bne_slow, 1u, slow)) { b->failed = 1; return; }
        th16(b, th_mov(2u, 1u));           /* r2=value */
        th16(b, th_mov(1u, 0u));           /* r1=linear */
        th16(b, th_mov(0u, 4u));           /* r0=runtime */
        th16(b, th_ldr(3u, 5u, soff));
        th16(b, 0x4798u);                  /* blx r3 */
        th16(b, th_add_imm(6u, 1u));       /* store instruction retired */
        th_store_ip(b, op->next_ip);
        th_return(b);                      /* invalidation boundary */
    }
    {
        const size_t done = b->at;
        /* Patch unconditional B from fast path. */
        const intptr_t delta = (intptr_t)done - (intptr_t)(b_skip_slow + 4u);
        intptr_t imm;
        uint16_t hw;
        if ((delta & 1) != 0) { b->failed = 1; return; }
        imm = delta / 2;
        if (imm < -1024 || imm > 1023) { b->failed = 1; return; }
        hw = (uint16_t)(0xE000u | ((uint16_t)imm & 0x07FFu));
        (void)th_patch16(b, b_skip_slow, hw);
    }
}

/* Direct JNZ after a direct flag-producing operation. For the M19.1 hot
 * loops the producer is DEC CX, so reading lazy_res is unnecessary: test the
 * destination register itself and native-branch to an earlier op label. */
static int th_emit_jnz(MdThumbBuf *b, const MdJitBlock *block, unsigned index,
                       const size_t *op_native)
{
    const MdJitOp *op = &block->ops[index];
    const MdJitOp *prev;
    int target_index;
    unsigned roff;
    unsigned span;
    size_t beq_not_taken, blo_budget;

    if (op->aux != 5u || index == 0u) return 0; /* only JNZ in M19.1 direct set */
    prev = &block->ops[index - 1u];
    if (prev->kind != MD_JIT_OP_DEC_R16 && prev->kind != MD_JIT_OP_INC_R16) return 0;
    target_index = md_jit_find_op_ip(block, op->target);
    roff = (unsigned)(offsetof(MdX86, r) + (size_t)prev->reg * 2u);
    if (!th_offset_ok_h(roff)) return 0;

    th16(b, th_ldrh(0u, 4u, roff));
    th16(b, th_cmp_imm(0u, 0u));
    beq_not_taken = th_emit_bcond_placeholder(b, 0u); /* BEQ */

    if (target_index < 0 || (unsigned)target_index > index) {
        /* M20.3: target outside this block (or forward): evaluate the
           condition natively and leave through one of two exact exits.
           Previously the JNZ was excluded from the block, which forced a
           zero-progress interpreter step every time (callmix: 32768). */
        th_store_ip(b, op->target);
        th_return(b);
        {
            const size_t not_taken = b->at;
            if (!th_patch_bcond(b, beq_not_taken, 0u, not_taken)) return 0;
            th_store_ip(b, op->next_ip);
            th_return(b);
        }
        return !b->failed;
    }

    /* Exact budget for the next native trip around the backedge. */
    span = index - (unsigned)target_index + 1u;
    th16(b, th_mov(0u, 7u));
    th16(b, th_sub_reg(0u, 0u, 6u));       /* remaining = budget-retired */
    th16(b, th_cmp_imm(0u, span));
    blo_budget = th_emit_bcond_placeholder(b, 3u); /* BLO */
    if (!th_emit_b(b, op_native[target_index])) return 0;

    {
        const size_t budget_exit = b->at;
        if (!th_patch_bcond(b, blo_budget, 3u, budget_exit)) return 0;
        th_store_ip(b, op->target);
        th_return(b);
    }
    {
        const size_t not_taken = b->at;
        if (!th_patch_bcond(b, beq_not_taken, 0u, not_taken)) return 0;
        th_store_ip(b, op->next_ip);
        th_return(b);
    }
    return !b->failed;
}

/* ---- M19.2 resident regions ------------------------------------------- */

/* Write the architectural lazy state corresponding to the final DEC r16
 * (old=1, rhs=1, result=0). CF is supplied by the caller: loop.com preserves
 * incoming CF, while memloop's preceding OR makes it zero. */
static void th_emit_final_dec16(MdThumbBuf *b, int carry_already_stored)
{
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    const unsigned aoff = (unsigned)offsetof(MdX86, lazy_a);
    const unsigned boff = (unsigned)offsetof(MdX86, lazy_b);
    const unsigned roff = (unsigned)offsetof(MdX86, lazy_res);
    if (!th_offset_ok_b(opoff) || !th_offset_ok_b(coff) ||
        !th_offset_ok_h(aoff) || !th_offset_ok_h(boff) || !th_offset_ok_h(roff)) {
        b->failed = 1;
        return;
    }
    if (!carry_already_stored) {
        th16(b, th_movs(2u, 0u));
        th16(b, th_strb(2u, 4u, coff));
    }
    th16(b, th_movs(2u, MD_LAZY_DEC16));
    th16(b, th_strb(2u, 4u, opoff));
    th16(b, th_movs(2u, 1u));
    th16(b, th_strh(2u, 4u, aoff));
    th16(b, th_strh(2u, 4u, boff));
    th16(b, th_movs(2u, 0u));
    th16(b, th_strh(2u, 4u, roff));
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

static int md_jit_install_native(MdJit *jit, MdJitBlock *block, const MdThumbBuf *b)
{
    size_t arena_at;
    if (b->failed || b->at == 0u) return 0;
    arena_at = (jit->code_used + (MD_JIT_CODE_ALIGN - 1u)) & ~(size_t)(MD_JIT_CODE_ALIGN - 1u);
    if (arena_at + b->at > jit->code_size) return 0;
    memcpy(jit->code + arena_at, b->bytes, b->at);
    block->native_offset = (uint32_t)arena_at;
    jit->code_used = arena_at + b->at;
    md_jit_sync_code();
#if defined(__arm__) || defined(__thumb__)
    block->native = (MdJitNativeFn)(uintptr_t)(jit->code + block->native_offset + 1u);
#else
    block->native = NULL;
#endif
    return 1;
}

/*
 * M21 compact IR: emitted native code owns only the descriptors that a
 * runtime helper will dereference by op index.  Resident/direct blocks drop
 * their compile IR completely.  Host builds always retain descriptors because
 * their differential/reference path executes MdJitOp rather than Thumb.
 */
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

/* loop.com resident region:
 *   MOV CX,imm; DEC CX; JNZ DEC
 *
 * CX lives in r0 for the whole dynamic loop. There is no per-iteration
 * MdX86 traffic, retired counter, or budget check. The entry guard proves the
 * supplied budget can finish the entire region; otherwise it returns zero and
 * the canonical M19.1/interpreter path advances exactly. */
static int md_jit_emit_resident_loop(MdJit *jit, MdJitBlock *block)
{
    MdThumbBuf b;
    const MdJitOp *o = block->ops;
    const uint32_t trips = o[0].imm != 0u ? o[0].imm : 65536u;
    const uint32_t retired = 1u + trips * 2u;
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    const unsigned cxoff = (unsigned)(offsetof(MdX86, r) + MD_X86_CX * 2u);
    const unsigned moff = (unsigned)offsetof(MdJitBlock, materialize);
    size_t blo_budget, bne_loop;
    size_t loop_at, fail_at;

    if (!th_offset_ok_h(foff) || !th_offset_ok_b(coff) || !th_offset_ok_h(cxoff) ||
        !th_offset_ok_w(moff)) return 0;
    memset(&b, 0, sizeof(b));

    th16(&b, 0xB5F8u);                    /* push {r3,r4,r5,r6,r7,lr} */
    th16(&b, th_mov(4u, 0u));             /* r4=runtime */
    th16(&b, th_mov(5u, 1u));             /* r5=block */
    th16(&b, th_mov(7u, 2u));             /* r7=budget */

    th_load_imm32(&b, 3u, retired);
    th16(&b, th_cmp_reg(7u, 3u));
    blo_budget = th_emit_bcond_placeholder(&b, 3u); /* BLO fail */

    /* DEC preserves CF. Materialise once, capture incoming CF once, and do not
       touch architectural flags again until the region exits. */
    th16(&b, th_mov(0u, 4u));
    th16(&b, th_ldr(3u, 5u, moff));
    th16(&b, 0x4798u);                    /* blx r3 */
    th16(&b, th_ldrh(1u, 4u, foff));
    th16(&b, th_movs(2u, 1u));
    th16(&b, th_and_reg(1u, 2u));
    th16(&b, th_strb(1u, 4u, coff));

    th_load_imm16(&b, 0u, o[0].imm);      /* resident CX */
    loop_at = b.at;
    th16(&b, th_sub_imm(0u, 1u));         /* DEC CX, ARM flags feed JNZ */
    bne_loop = th_emit_bcond_placeholder(&b, 1u);
    if (!th_patch_bcond(&b, bne_loop, 1u, loop_at)) return 0;

    th16(&b, th_strh(0u, 4u, cxoff));     /* CX == 0 */
    th_emit_final_dec16(&b, 1);
    th_store_ip(&b, o[2].next_ip);
    th_return_const(&b, retired);

    fail_at = b.at;
    if (!th_patch_bcond(&b, blo_budget, 3u, fail_at)) return 0;
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0xBDF8u);

    if (!md_jit_install_native(jit, block, &b)) return 0;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = 1u;
    block->resident = 1u;
    ++jit->local_edges;
    ++jit->helper_sites;                  /* one entry materialise call */
    ++jit->resident_regions;
    return 1;
}

/* memloop resident region. The specialization is guarded at entry:
 *  - DS must be zero, so [SI] is linear SI;
 *  - pages 8..15 (8000h..FFFFh) must never have been marked executable.
 * If either assumption is false it returns zero before changing guest state.
 *
 * Registers in the hot loop:
 *   r0=CX, r1=SI, r2=AL scratch, r3=0x8000, r5=guest memory base.
 * The seven guest instructions in the loop execute with no MdX86 register
 * loads/stores and no budget bookkeeping. */
static int md_jit_emit_resident_memloop(MdJit *jit, MdJitBlock *block)
{
    MdThumbBuf b;
    const MdJitOp *o = block->ops;
    const uint32_t trips = o[0].imm != 0u ? o[0].imm : 65536u;
    const uint32_t retired = 2u + trips * 7u;
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned xoff = (unsigned)offsetof(MdX86, code_page_executable);
    const unsigned memoff = (unsigned)offsetof(MdX86, memory);
    const unsigned cxoff = (unsigned)(offsetof(MdX86, r) + MD_X86_CX * 2u);
    const unsigned sioff = (unsigned)(offsetof(MdX86, r) + MD_X86_SI * 2u);
    const unsigned axoff = (unsigned)offsetof(MdX86, r);
    size_t blo_budget, bne_ds, bne_exec, bne_loop;
    size_t loop_at, fail_at;

    if (!th_offset_ok_h(dsoff) || !th_offset_ok_w(xoff) || !th_offset_ok_w(memoff) ||
        !th_offset_ok_h(cxoff) || !th_offset_ok_h(sioff) || !th_offset_ok_b(axoff)) return 0;
    /* The resident loop uses one 8-bit ADD and one 8-bit ADD-to-SI immediate. */
    if (o[3].imm > 255u || o[5].imm > 255u) return 0;
    memset(&b, 0, sizeof(b));

    th16(&b, 0xB5F8u);
    th16(&b, th_mov(4u, 0u));             /* runtime */
    th16(&b, th_mov(5u, 1u));             /* block until guards finish */
    th16(&b, th_mov(7u, 2u));             /* budget */

    th_load_imm32(&b, 3u, retired);
    th16(&b, th_cmp_reg(7u, 3u));
    blo_budget = th_emit_bcond_placeholder(&b, 3u);

    th16(&b, th_ldrh(0u, 4u, dsoff));
    th16(&b, th_cmp_imm(0u, 0u));
    bne_ds = th_emit_bcond_placeholder(&b, 1u);

    /* One-time proof that every address the loop can store to is a pure data
       page. code_page_executable is byte-per-page; pages 8..15 are two words. */
    th16(&b, th_ldr(0u, 4u, xoff));
    th16(&b, th_ldr(1u, 0u, 8u));
    th16(&b, th_ldr(2u, 0u, 12u));
    th16(&b, th_orr_reg(1u, 2u));
    th16(&b, th_cmp_imm(1u, 0u));
    bne_exec = th_emit_bcond_placeholder(&b, 1u);

    th16(&b, th_ldr(5u, 4u, memoff));     /* r5 = guest memory base */
    th_load_imm16(&b, 0u, o[0].imm);      /* CX */
    th_load_imm16(&b, 1u, o[1].imm);      /* SI */
    th_load_imm16(&b, 3u, o[6].imm);      /* OR mask (8000h in fixture) */

    loop_at = b.at;
    th16(&b, th_ldrb_reg(2u, 5u, 1u));    /* MOV AL,[SI] */
    th16(&b, th_add_imm(2u, (uint8_t)o[3].imm)); /* ADD AL,imm; low byte stored */
    th16(&b, th_strb_reg(2u, 5u, 1u));    /* MOV [SI],AL */
    th16(&b, th_add_imm(1u, (uint8_t)o[5].imm)); /* ADD SI,imm */
    th16(&b, th_uxth(1u, 1u));            /* 8086 16-bit wrap */
    th16(&b, th_orr_reg(1u, 3u));         /* OR SI,mask */
    th16(&b, th_sub_imm(0u, 1u));         /* DEC CX */
    bne_loop = th_emit_bcond_placeholder(&b, 1u);
    if (!th_patch_bcond(&b, bne_loop, 1u, loop_at)) return 0;

    th16(&b, th_strh(0u, 4u, cxoff));
    th16(&b, th_strh(1u, 4u, sioff));
    th16(&b, th_strb(2u, 4u, axoff));     /* preserve AH, commit AL only */
    th_emit_final_dec16(&b, 0);           /* preceding OR made CF=0 */
    th_store_ip(&b, o[8].next_ip);
    th_return_const(&b, retired);

    fail_at = b.at;
    if (!th_patch_bcond(&b, blo_budget, 3u, fail_at) ||
        !th_patch_bcond(&b, bne_ds, 1u, fail_at) ||
        !th_patch_bcond(&b, bne_exec, 1u, fail_at)) return 0;
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0xBDF8u);

    if (!md_jit_install_native(jit, block, &b)) return 0;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = 1u;
    block->resident = 1u;
    ++jit->local_edges;
    ++jit->resident_regions;
    return 1;
}

/* ---- M20 generic counted-loop resident regions ------------------------ */

static int md_jit_resident_arm_reg(unsigned x86reg)
{
    switch (x86reg & 7u) {
        case MD_X86_AX: return 0;   /* r0 */
        case MD_X86_BX: return 1;   /* r1 */
        case MD_X86_CX: return 6;   /* r6 */
        case MD_X86_SI: return 7;   /* r7 */
        default: return -1;
    }
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

static int md_jit_generic_op_supported(const MdJitOp *op)
{
    int ar;
    if (op == NULL) return 0;
    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R16_IMM:
        case MD_JIT_OP_INC_R16:
        case MD_JIT_OP_DEC_R16:
            return md_jit_resident_arm_reg(op->reg) >= 0;
        case MD_JIT_OP_MOV_R8_IMM:
            return op->reg == 0u;                 /* AL only for now */
        case MD_JIT_OP_ALU_ACC_IMM:
            return op->aux == 0u || op->aux == 1u || op->aux == 4u ||
                   op->aux == 5u || op->aux == 6u;
        case MD_JIT_OP_GRP1_R16_IMM:
            ar = md_jit_resident_arm_reg(op->reg);
            return ar >= 0 && (op->aux == 0u || op->aux == 1u || op->aux == 4u ||
                               op->aux == 5u || op->aux == 6u);
        case MD_JIT_OP_MOV_AL_SI:
        case MD_JIT_OP_MOV_SI_AL:
        case MD_JIT_OP_NOP:
            return 1;
        default:
            return 0;
    }
}

/* Tracks the x86 CF that DEC CX must preserve at the region exit.  Arithmetic
 * ADD/SUB make it dynamic/unknown; a later logic operation resets it to zero.
 * INC/DEC/MOV/memory operations preserve it. */
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

/* Proves that every [SI] access is in 8000h..FFFFh.  The prefix must establish
 * SI's high bit, every access must occur while that fact is known, and the
 * backedge must restore the fact for the next iteration. */
static int md_jit_generic_si_window(const MdJitBlock *block, unsigned loop_start,
                                    unsigned final_dec, int *has_memory, int *has_store)
{
    int known_high = 0;
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

static void th_resident_load_reg(MdThumbBuf *b, unsigned x86reg)
{
    const int ar = md_jit_resident_arm_reg(x86reg);
    const unsigned off = (unsigned)(offsetof(MdX86, r) + (x86reg & 7u) * 2u);
    if (ar < 0 || !th_offset_ok_h(off)) { b->failed = 1; return; }
    th16(b, th_ldrh((unsigned)ar, 4u, off));
}

static void th_resident_store_reg(MdThumbBuf *b, unsigned x86reg)
{
    const int ar = md_jit_resident_arm_reg(x86reg);
    const unsigned off = (unsigned)(offsetof(MdX86, r) + (x86reg & 7u) * 2u);
    if (ar < 0 || !th_offset_ok_h(off)) { b->failed = 1; return; }
    th16(b, th_strh((unsigned)ar, 4u, off));
}

static void th_resident_set_al_from_r2(MdThumbBuf *b)
{
    /* r0 is resident AX, r2 contains the new AL.  Preserve AH. */
    th16(b, th_uxtb(2u, 2u));
    th16(b, th_lsr_imm(0u, 0u, 8u));
    th16(b, th_lsl_imm(0u, 0u, 8u));
    th16(b, th_orr_reg(0u, 2u));
}

static int th_emit_generic_resident_op(MdThumbBuf *b, const MdJitOp *op)
{
    int ar;
    unsigned imm;
    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R16_IMM:
            ar = md_jit_resident_arm_reg(op->reg);
            if (ar < 0) return 0;
            th_load_imm16(b, (unsigned)ar, op->imm);
            return !b->failed;

        case MD_JIT_OP_MOV_R8_IMM:
            if (op->reg != 0u) return 0;
            th_load_imm16(b, 2u, (uint16_t)(op->imm & 0xFFu));
            th_resident_set_al_from_r2(b);
            return !b->failed;

        case MD_JIT_OP_INC_R16:
        case MD_JIT_OP_DEC_R16:
            ar = md_jit_resident_arm_reg(op->reg);
            if (ar < 0) return 0;
            th16(b, op->kind == MD_JIT_OP_DEC_R16 ? th_sub_imm((unsigned)ar, 1u)
                                                  : th_add_imm((unsigned)ar, 1u));
            th16(b, th_uxth((unsigned)ar, (unsigned)ar));
            return !b->failed;

        case MD_JIT_OP_ALU_ACC_IMM:
            imm = op->imm;
            if (op->reg == 0u) {                   /* AL, AX lives in r0 */
                if (op->aux == 1u || op->aux == 6u) {
                    th_load_imm16(b, 2u, (uint16_t)(imm & 0xFFu));
                    th16(b, op->aux == 1u ? th_orr_reg(0u, 2u) : th_eor_reg(0u, 2u));
                } else if (op->aux == 4u) {
                    th_load_imm16(b, 2u, (uint16_t)(0xFF00u | (imm & 0xFFu)));
                    th16(b, th_and_reg(0u, 2u));
                } else if (op->aux == 0u || op->aux == 5u) {
                    th16(b, th_uxtb(2u, 0u));
                    if ((imm & 0xFFFFu) <= 255u) {
                        th16(b, op->aux == 0u ? th_add_imm(2u, imm) : th_sub_imm(2u, imm));
                    } else return 0;
                    th_resident_set_al_from_r2(b);
                } else return 0;
                return !b->failed;
            }
            /* AX form */
            if (op->reg != 1u) return 0;
            if ((op->aux == 0u || op->aux == 5u) && imm <= 255u) {
                th16(b, op->aux == 0u ? th_add_imm(0u, imm) : th_sub_imm(0u, imm));
            } else {
                th_load_imm16(b, 2u, (uint16_t)imm);
                if (op->aux == 0u) th16(b, th_add_reg(0u, 0u, 2u));
                else if (op->aux == 1u) th16(b, th_orr_reg(0u, 2u));
                else if (op->aux == 4u) th16(b, th_and_reg(0u, 2u));
                else if (op->aux == 5u) th16(b, th_sub_reg(0u, 0u, 2u));
                else if (op->aux == 6u) th16(b, th_eor_reg(0u, 2u));
                else return 0;
            }
            th16(b, th_uxth(0u, 0u));
            return !b->failed;

        case MD_JIT_OP_GRP1_R16_IMM:
            ar = md_jit_resident_arm_reg(op->reg);
            if (ar < 0) return 0;
            th_load_imm16(b, 2u, op->imm);
            if (op->aux == 0u) th16(b, th_add_reg((unsigned)ar, (unsigned)ar, 2u));
            else if (op->aux == 1u) th16(b, th_orr_reg((unsigned)ar, 2u));
            else if (op->aux == 4u) th16(b, th_and_reg((unsigned)ar, 2u));
            else if (op->aux == 5u) th16(b, th_sub_reg((unsigned)ar, (unsigned)ar, 2u));
            else if (op->aux == 6u) th16(b, th_eor_reg((unsigned)ar, 2u));
            else return 0;
            th16(b, th_uxth((unsigned)ar, (unsigned)ar));
            return !b->failed;

        case MD_JIT_OP_MOV_AL_SI:
            th16(b, th_ldrb_reg(2u, 5u, 7u));
            th_resident_set_al_from_r2(b);
            return !b->failed;

        case MD_JIT_OP_MOV_SI_AL:
            th16(b, th_uxtb(2u, 0u));
            th16(b, th_strb_reg(2u, 5u, 7u));
            return !b->failed;

        case MD_JIT_OP_NOP:
            return 1;
        default:
            return 0;
    }
}

/* ---- M20.1 bounded CFG resident regions ----------------------------- */

static int md_jit_cfg_cmp_supported(const MdJitOp *op)
{
    if (op->kind == MD_JIT_OP_ALU_ACC_IMM && op->aux == 7u) return op->reg <= 1u;
    if (op->kind == MD_JIT_OP_GRP1_R16_IMM && op->aux == 7u)
        return md_jit_resident_arm_reg(op->reg) >= 0;
    return 0;
}

static int th_emit_cfg_cmp(MdThumbBuf *b, const MdJitOp *op)
{
    int ar;
    if (op->kind == MD_JIT_OP_ALU_ACC_IMM) {
        if (op->reg == 0u) {                 /* CMP AL,imm8 */
            th16(b, th_uxtb(2u, 0u));
            th_load_imm16(b, 5u, (uint16_t)(op->imm & 0xFFu));
            th16(b, th_cmp_reg(2u, 5u));
            return !b->failed;
        }
        th_load_imm16(b, 2u, op->imm);       /* CMP AX,imm16 */
        th16(b, th_cmp_reg(0u, 2u));
        return !b->failed;
    }
    if (op->kind == MD_JIT_OP_GRP1_R16_IMM) {
        ar = md_jit_resident_arm_reg(op->reg);
        if (ar < 0) return 0;
        th_load_imm16(b, 2u, op->imm);
        th16(b, th_cmp_reg((unsigned)ar, 2u));
        return !b->failed;
    }
    return 0;
}

/* A small multi-block CFG region.  V1 deliberately targets the shape that is
 * common in parsers and command loops: one counted backward edge plus zero or
 * more forward JE/JNE edges whose producer is an immediately preceding CMP.
 * AX/BX/CX/SI stay resident.  The forward edges may skip arbitrary supported
 * native operations and reconverge in the same region.
 *
 * Guest retirement is path-exact: r3 counts executed guest instructions.
 * Budget admission uses the conservative longest path, so the region either
 * executes completely within budget or executes zero instructions. */
static int md_jit_emit_resident_cfg(MdJit *jit, MdJitBlock *block)
{
    MdThumbBuf b;
    size_t op_native[MD_JIT_MAX_OPS];
    size_t patch_at[MD_JIT_MAX_OPS];
    unsigned patch_target[MD_JIT_MAX_OPS];
    unsigned patch_cond[MD_JIT_MAX_OPS];
    unsigned patch_count = 0u;
    const unsigned n = block->op_count;
    unsigned final_dec, loop_start, i, forward_edges = 0u;
    int cx_setup = -1;
    uint32_t trips = 0u, worst_retired;
    enum MdJitCfState final_cf;
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    size_t blo_budget, loop_at, bne_loop, fail_at;

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
    if (cx_setup < 0 || trips == 0u || forward_edges == 0u) return 0;

    /* Require a deterministic CF at the final DEC. A logic op after every
       branch path is sufficient and is easy to prove conservatively. */
    final_cf = md_jit_generic_cf_after(block, final_dec);
    if (final_cf != MD_JIT_CF_ZERO) return 0;
    if (!th_offset_ok_h(foff) || !th_offset_ok_b(coff)) return 0;

    worst_retired = loop_start + trips * (n - loop_start);
    if (worst_retired == 0u) return 0;

    memset(&b, 0, sizeof(b));
    memset(op_native, 0, sizeof(op_native));
    th16(&b, 0xB5F8u);                    /* push {r3,r4,r5,r6,r7,lr} */
    th16(&b, th_mov(4u, 0u));             /* r4 runtime */
    th16(&b, th_mov(5u, 1u));             /* r5 block; free after admission */
    th_load_imm32(&b, 0u, worst_retired);
    th16(&b, th_cmp_reg(2u, 0u));          /* budget >= conservative longest path */
    blo_budget = th_emit_bcond_placeholder(&b, 3u);

    th_resident_load_reg(&b, MD_X86_AX);
    th_resident_load_reg(&b, MD_X86_BX);
    th_resident_load_reg(&b, MD_X86_CX);
    th_resident_load_reg(&b, MD_X86_SI);
    th16(&b, th_movs(3u, 0u));             /* exact dynamically executed guest instructions */

    for (i = 0u; i < loop_start && !b.failed; ++i) {
        op_native[i] = b.at;
        if (!th_emit_generic_resident_op(&b, &block->ops[i])) return 0;
        th16(&b, th_add_imm(3u, 1u));
    }

    loop_at = b.at;
    for (i = loop_start; i < final_dec && !b.failed; ++i) {
        const MdJitOp *op = &block->ops[i];
        op_native[i] = b.at;
        if (md_jit_cfg_cmp_supported(op)) {
            const MdJitOp *jcc = &block->ops[i + 1u];
            const int ti = md_jit_find_op_ip(block, jcc->target);
            /* CMP and its Jcc are both architecturally executed before either
               path diverges; account for both before setting ARM condition flags. */
            th16(&b, th_add_imm(3u, 2u));
            if (!th_emit_cfg_cmp(&b, op)) return 0;
            op_native[i + 1u] = b.at;
            if (patch_count >= MD_JIT_MAX_OPS || ti < 0) return 0;
            patch_at[patch_count] = th_emit_bcond_placeholder(&b, jcc->aux == 4u ? 0u : 1u);
            patch_target[patch_count] = (unsigned)ti;
            patch_cond[patch_count] = jcc->aux == 4u ? 0u : 1u;
            ++patch_count;
            ++i;
            continue;
        }
        if (op->kind == MD_JIT_OP_JCC) return 0;   /* only consumed with CMP above */
        if (!th_emit_generic_resident_op(&b, op)) return 0;
        th16(&b, th_add_imm(3u, 1u));
    }

    /* Count and execute final DEC/JNZ as one native backedge pair. */
    op_native[final_dec] = b.at;
    th16(&b, th_add_imm(3u, 2u));
    th16(&b, th_sub_imm(6u, 1u));
    th16(&b, th_uxth(6u, 6u));
    op_native[n - 1u] = b.at;
    bne_loop = th_emit_bcond_placeholder(&b, 1u);
    if (!th_patch_bcond(&b, bne_loop, 1u, loop_at)) return 0;

    /* Forward CFG edges now have concrete native labels. */
    for (i = 0u; i < patch_count; ++i) {
        if (patch_target[i] >= n || op_native[patch_target[i]] == 0u) return 0;
        if (!th_patch_bcond(&b, patch_at[i], patch_cond[i], op_native[patch_target[i]])) return 0;
    }

    th_resident_store_reg(&b, MD_X86_AX);
    th_resident_store_reg(&b, MD_X86_BX);
    th_resident_store_reg(&b, MD_X86_CX);
    th_resident_store_reg(&b, MD_X86_SI);
    th_emit_final_dec16(&b, 0);            /* final proven logic op made CF=0 */
    th_store_ip(&b, block->ops[n - 1u].next_ip);
    th16(&b, th_mov(0u, 3u));
    th16(&b, 0xBDF8u);

    fail_at = b.at;
    if (!th_patch_bcond(&b, blo_budget, 3u, fail_at)) return 0;
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0xBDF8u);

    if (!md_jit_install_native(jit, block, &b)) return 0;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = (uint8_t)(forward_edges + 1u);
    block->resident = 1u;
    block->generic_region = 1u;
    block->cfg_region = 1u;
    jit->local_edges += forward_edges + 1u;
    jit->cfg_internal_edges += forward_edges + 1u;
    ++jit->resident_regions;
    ++jit->generic_regions;
    ++jit->cfg_regions;
    return 1;
}

/* Generic M20 counted-loop region.  It is intentionally conservative but no
 * longer tied to the exact loop.com/memloop.com byte shapes.  The accepted CFG
 * is a straight-line prefix followed by an arbitrary supported body ending in
 * DEC CX / JNZ back to an earlier body instruction.  AX/BX/CX/SI are resident
 * in r0/r1/r6/r7 and [SI] uses r5 as the guest-memory base. */
static int md_jit_emit_resident_counted(MdJit *jit, MdJitBlock *block)
{
    MdThumbBuf b;
    const unsigned n = block->op_count;
    unsigned final_dec, loop_start, i;
    uint32_t trips = 0u, retired;
    int cx_setup = -1;
    int has_memory = 0, has_store = 0;
    enum MdJitCfState final_cf;
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned xoff = (unsigned)offsetof(MdX86, code_page_executable);
    const unsigned memoff = (unsigned)offsetof(MdX86, memory);
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    const unsigned moff = (unsigned)offsetof(MdJitBlock, materialize);
    size_t blo_budget, bne_ds = 0u, bne_exec = 0u, bne_loop, loop_at, fail_at;

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
    if (cx_setup < 0 || trips == 0u) return 0;

    final_cf = md_jit_generic_cf_after(block, final_dec);
    if (final_cf == MD_JIT_CF_LAZY_UNKNOWN) return 0;

    /* If memory is used, prove an 8000h..FFFFh [SI] window. */
    for (i = 0u; i < final_dec; ++i) {
        if (block->ops[i].kind == MD_JIT_OP_MOV_AL_SI || block->ops[i].kind == MD_JIT_OP_MOV_SI_AL) {
            if (!md_jit_generic_si_window(block, loop_start, final_dec, &has_memory, &has_store)) return 0;
            break;
        }
    }

    retired = loop_start + trips * (n - loop_start);
    if (retired == 0u || !th_offset_ok_h(foff) || !th_offset_ok_b(coff) || !th_offset_ok_w(moff)) return 0;
    if (has_memory && (!th_offset_ok_h(dsoff) || !th_offset_ok_w(memoff))) return 0;
    if (has_store && !th_offset_ok_w(xoff)) return 0;

    memset(&b, 0, sizeof(b));
    th16(&b, 0xB5F8u);                    /* 24-byte AAPCS-aligned frame */
    th16(&b, th_mov(4u, 0u));             /* r4 runtime */
    th16(&b, th_mov(5u, 1u));             /* r5 block during entry guards */

    th_load_imm32(&b, 3u, retired);
    th16(&b, th_cmp_reg(2u, 3u));          /* r2 is budget argument */
    blo_budget = th_emit_bcond_placeholder(&b, 3u);

    if (has_memory) {
        th16(&b, th_ldrh(0u, 4u, dsoff));
        th16(&b, th_cmp_imm(0u, 0u));
        bne_ds = th_emit_bcond_placeholder(&b, 1u);
    }
    if (has_store) {
        th16(&b, th_ldr(0u, 4u, xoff));
        th16(&b, th_ldr(1u, 0u, 8u));
        th16(&b, th_ldr(2u, 0u, 12u));
        th16(&b, th_orr_reg(1u, 2u));
        th16(&b, th_cmp_imm(1u, 0u));
        bne_exec = th_emit_bcond_placeholder(&b, 1u);
    }

    if (final_cf == MD_JIT_CF_RAW) {
        /* Preserve incoming CF once; the generic accepted body never changes
           it if final_cf remained RAW. */
        th16(&b, th_mov(0u, 4u));
        th16(&b, th_ldr(3u, 5u, moff));
        th16(&b, 0x4798u);                /* blx materialize */
        th16(&b, th_ldrh(1u, 4u, foff));
        th16(&b, th_movs(2u, 1u));
        th16(&b, th_and_reg(1u, 2u));
        th16(&b, th_strb(1u, 4u, coff));
        ++jit->helper_sites;
    }

    /* Load the resident architectural set once.  Unused registers cost a few
       one-time loads/stores but let one emitter handle many loop shapes. */
    th_resident_load_reg(&b, MD_X86_AX);
    th_resident_load_reg(&b, MD_X86_BX);
    th_resident_load_reg(&b, MD_X86_CX);
    th_resident_load_reg(&b, MD_X86_SI);
    if (has_memory) th16(&b, th_ldr(5u, 4u, memoff));
    th_load_imm32(&b, 3u, retired);        /* C helper may have clobbered r3 */

    for (i = 0u; i < loop_start && !b.failed; ++i) {
        if (!th_emit_generic_resident_op(&b, &block->ops[i])) return 0;
    }

    loop_at = b.at;
    for (i = loop_start; i < final_dec && !b.failed; ++i) {
        if (!th_emit_generic_resident_op(&b, &block->ops[i])) return 0;
    }

    /* Final DEC CX/JNZ stays entirely native. */
    th16(&b, th_sub_imm(6u, 1u));
    th16(&b, th_uxth(6u, 6u));
    bne_loop = th_emit_bcond_placeholder(&b, 1u);
    if (!th_patch_bcond(&b, bne_loop, 1u, loop_at)) return 0;

    th_resident_store_reg(&b, MD_X86_AX);
    th_resident_store_reg(&b, MD_X86_BX);
    th_resident_store_reg(&b, MD_X86_CX);
    th_resident_store_reg(&b, MD_X86_SI);
    th_emit_final_dec16(&b, final_cf == MD_JIT_CF_RAW ? 1 : 0);
    th_store_ip(&b, block->ops[n - 1u].next_ip);
    th16(&b, th_mov(0u, 3u));             /* return exact retired count */
    th16(&b, 0xBDF8u);

    fail_at = b.at;
    if (!th_patch_bcond(&b, blo_budget, 3u, fail_at)) return 0;
    if (has_memory && !th_patch_bcond(&b, bne_ds, 1u, fail_at)) return 0;
    if (has_store && !th_patch_bcond(&b, bne_exec, 1u, fail_at)) return 0;
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0xBDF8u);

    if (!md_jit_install_native(jit, block, &b)) return 0;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = 1u;
    block->resident = 1u;
    block->generic_region = 1u;
    ++jit->local_edges;
    ++jit->resident_regions;
    ++jit->generic_regions;
    return 1;
}


/* ---- M20.3 resident LODS/ALU/LOOP region ------------------------------
 *
 * Shape: [body ops...] + LOOP back to the first op, where the body is LODSB/
 * LODSW (no prefix), 16-bit register ALU (ADD/SUB/AND/OR/XOR/CMP) and NOP,
 * and contains at least one LODS. This is COMMAND.COM's transient checksum
 * (LODSW / ADD DX,AX / LOOP) and the common "sum/scan a table" idiom.
 *
 * Registers: r0=AX, r6=CX, r7=SI resident; r1 = DS*16 (constant: nothing
 * in the body writes DS); r5 = guest memory; r3 = retired; r12 = budget;
 * r2 scratch. Other 8086 registers stay in MdX86 (one operand of each ALU
 * op must be resident, so one scratch register suffices).
 *
 * Exactness:
 * - the trip count is dynamic (CX at entry, including CX=0 -> 65536);
 *   admission is per iteration: the region only starts an iteration whose
 *   whole body fits the remaining budget, so it stops exactly at a loop head;
 * - every memory byte address is ((DS<<4)+SI) mod 2^20, and LODSW's second
 *   byte wraps independently (the interpreter's read16 physical wrap);
 * - DF is read once at entry and selects one of two loop copies (+/- step);
 * - flags: each ALU op stores its lazy operands/result; lazy_op is written
 *   only on exit and only if at least one iteration ran. LODS/LOOP never
 *   touch flags, so the architectural flags are exactly those of the last
 *   ALU op executed (or unchanged if the body has none).
 */
static int md_jit_lods_reg(unsigned x86reg)
{
    switch (x86reg & 7u) {
        case MD_X86_AX: return 0;
        case MD_X86_CX: return 6;
        case MD_X86_SI: return 7;
        default: return -1;
    }
}

static int th_emit_lods_alu(MdThumbBuf *b, const MdJitOp *op)
{
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    const unsigned aoff = (unsigned)offsetof(MdX86, lazy_a);
    const unsigned boff = (unsigned)offsetof(MdX86, lazy_b);
    const unsigned roff = (unsigned)offsetof(MdX86, lazy_res);
    const int rd = md_jit_lods_reg(op->reg), rs = md_jit_lods_reg(op->imm);
    const unsigned doff = (unsigned)(offsetof(MdX86, r) + (op->reg & 7u) * 2u);
    const unsigned soff = (unsigned)(offsetof(MdX86, r) + (op->imm & 7u) * 2u);
    const unsigned alu = op->aux;
    unsigned dst, src, res;

    if (!th_offset_ok_h(aoff) || !th_offset_ok_h(boff) || !th_offset_ok_h(roff) ||
        !th_offset_ok_h(doff) || !th_offset_ok_h(soff) || !th_offset_ok_b(opoff)) return 0;
    if (rd < 0 && rs < 0 && (op->reg & 7u) != (op->imm & 7u)) return 0;

    if (rd < 0 && rs < 0) {
        /* same non-resident register on both sides (XOR DX,DX, SUB BX,BX...) */
        th16(b, th_ldrh(2u, 4u, doff));
        dst = 2u;
        src = 2u;
    } else if (rd >= 0) {
        dst = (unsigned)rd;
        if (rs >= 0) src = (unsigned)rs;
        else { th16(b, th_ldrh(2u, 4u, soff)); src = 2u; }
    } else {
        th16(b, th_ldrh(2u, 4u, doff));
        dst = 2u;
        src = (unsigned)rs;
    }
    /* lazy a/b are the operands before the operation */
    if (alu != 1u && alu != 4u && alu != 6u) {
        th16(b, th_strh(dst, 4u, aoff));
        th16(b, th_strh(src, 4u, boff));
    }
    if (alu == 7u) {                                   /* CMP: result in r2 only */
        th16(b, th_sub_reg(2u, dst, src));
        res = 2u;
    } else {
        switch (alu) {
            case 0u: th16(b, th_add_reg(dst, dst, src)); break;
            case 5u: th16(b, th_sub_reg(dst, dst, src)); break;
            case 4u: th16(b, th_and_reg(dst, src)); break;
            case 1u: th16(b, th_orr_reg(dst, src)); break;
            case 6u: th16(b, th_eor_reg(dst, src)); break;
            default: return 0;
        }
        if (dst != 2u) th16(b, th_uxth(dst, dst));
        else th16(b, th_strh(2u, 4u, doff));           /* memory-resident destination */
        res = dst;
    }
    th16(b, th_strh(res, 4u, roff));
    /* lazy kind recorded per op: the flags are always those of the last ALU
       op executed, wherever the region exits */
    th16(b, th_movs(2u, alu == 0u ? MD_LAZY_ADD16
                      : (alu == 5u || alu == 7u) ? MD_LAZY_SUB16 : MD_LAZY_LOGIC16));
    th16(b, th_strb(2u, 4u, opoff));
    return !b->failed;
}

static int th_emit_lods_read(MdThumbBuf *b, unsigned width, int backwards)
{
    th16(b, th_add_reg(2u, 1u, 7u));                  /* r2 = DS*16 + SI */
    th16(b, th_lsl_imm(2u, 2u, 12u));
    th16(b, th_lsr_imm(2u, 2u, 12u));                 /* mod 2^20 */
    if (width == 1u) {
        th16(b, th_ldrb_reg(2u, 5u, 2u));
        th_resident_set_al_from_r2(b);
    } else {
        th16(b, th_ldrb_reg(0u, 5u, 2u));             /* AL; AX fully replaced */
        th16(b, th_add_imm(2u, 1u));
        th16(b, th_lsl_imm(2u, 2u, 12u));
        th16(b, th_lsr_imm(2u, 2u, 12u));             /* second byte wraps too */
        th16(b, th_ldrb_reg(2u, 5u, 2u));
        th16(b, th_lsl_imm(2u, 2u, 8u));
        th16(b, th_orr_reg(0u, 2u));
    }
    th16(b, backwards ? th_sub_imm(7u, width) : th_add_imm(7u, width));
    th16(b, th_uxth(7u, 7u));
    return !b->failed;
}

static void th_lods_store_regs(MdThumbBuf *b)
{
    th16(b, th_strh(0u, 4u, (unsigned)(offsetof(MdX86, r) + MD_X86_AX * 2u)));
    th16(b, th_strh(6u, 4u, (unsigned)(offsetof(MdX86, r) + MD_X86_CX * 2u)));
    th16(b, th_strh(7u, 4u, (unsigned)(offsetof(MdX86, r) + MD_X86_SI * 2u)));
}

static int th_emit_lods_op(MdThumbBuf *b, const MdJitOp *op, int backwards)
{
    if (op->kind == MD_JIT_OP_LODS) return th_emit_lods_read(b, op->aux, backwards);
    if (op->kind == MD_JIT_OP_ALU_RR16) return th_emit_lods_alu(b, op);
    return op->kind == MD_JIT_OP_NOP;
}

static int th_emit_lods_copy(MdThumbBuf *b, const MdJitBlock *block, int backwards,
                             unsigned loop_start)
{
    const unsigned n = block->op_count;
    const unsigned body = n - loop_start;           /* loop body incl. LOOP */
    size_t top, blo_budget, bne_top, blo_prologue = 0u;
    unsigned i;

    if (loop_start != 0u) {
        /* straight-line prologue, executed once, admitted on its own */
        th16(b, th_mov_hi(2u, 12u));
        th16(b, th_sub_reg(2u, 2u, 3u));
        th16(b, th_cmp_imm(2u, loop_start));
        blo_prologue = th_emit_bcond_placeholder(b, 3u);
        th16(b, th_add_imm(3u, loop_start));
        for (i = 0u; i < loop_start && !b->failed; ++i) {
            if (!th_emit_lods_op(b, &block->ops[i], backwards)) return 0;
        }
    }

    top = b->at;
    th16(b, th_mov_hi(2u, 12u));                      /* r2 = budget */
    th16(b, th_sub_reg(2u, 2u, 3u));                  /* remaining */
    th16(b, th_cmp_imm(2u, body));
    blo_budget = th_emit_bcond_placeholder(b, 3u);    /* BLO: iteration doesn't fit */
    th16(b, th_add_imm(3u, body));                    /* whole iteration retires */

    for (i = loop_start; i + 1u < n && !b->failed; ++i) {
        if (!th_emit_lods_op(b, &block->ops[i], backwards)) return 0;
    }
    /* LOOP: CX-1; continue while non-zero (SUBS sets Z before the UXTH) */
    th16(b, th_sub_imm(6u, 1u));
    th16(b, th_uxth(6u, 6u));
    bne_top = th_emit_bcond_placeholder(b, 1u);
    if (!th_patch_bcond(b, bne_top, 1u, top)) return 0;

    /* loop finished */
    th_lods_store_regs(b);
    th_store_ip(b, block->ops[n - 1u].next_ip);
    th16(b, th_mov(0u, 3u));
    th16(b, 0xBDF8u);                                 /* pop {r3-r7,pc} */

    /* budget exit at the loop head (prologue already retired) */
    if (!th_patch_bcond(b, blo_budget, 3u, b->at)) return 0;
    th_lods_store_regs(b);
    th_store_ip(b, block->ops[loop_start].ip);
    th16(b, th_mov(0u, 3u));
    th16(b, 0xBDF8u);

    if (loop_start != 0u) {
        /* prologue doesn't fit: nothing executed, nothing changed */
        if (!th_patch_bcond(b, blo_prologue, 3u, b->at)) return 0;
        th16(b, th_movs(0u, 0u));
        th16(b, 0xBDF8u);
    }
    return !b->failed;
}

static int md_jit_emit_resident_lodsloop(MdJit *jit, MdJitBlock *block)
{
    MdThumbBuf b;
    const unsigned n = block->op_count;
    const MdJitOp *last;
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    int has_lods = 0, li;
    unsigned loop_start;
    size_t bmi_back;
    unsigned i;

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
        if (md_jit_lods_reg(op->reg) < 0 && md_jit_lods_reg(op->imm) < 0 &&
            (op->reg & 7u) != (op->imm & 7u)) return 0;
        if (op->aux == 2u || op->aux == 3u) return 0;
    }
    if (!has_lods) return 0;
    if (!th_offset_ok_h(dsoff) || !th_offset_ok_w(moff) || !th_offset_ok_h(foff) ||
        !th_offset_ok_b(opoff)) return 0;

    memset(&b, 0, sizeof(b));
    th16(&b, 0xB5F8u);                                /* push {r3-r7,lr} */
    th16(&b, th_mov(4u, 0u));                         /* r4 = runtime */
    th16(&b, th_mov_hi(12u, 2u));                     /* r12 = budget */
    th16(&b, th_ldr(5u, 4u, moff));                   /* r5 = guest memory */
    th16(&b, th_ldrh(1u, 4u, dsoff));
    th16(&b, th_lsl_imm(1u, 1u, 4u));                 /* r1 = DS*16 */
    th_resident_load_reg(&b, MD_X86_AX);
    th_resident_load_reg(&b, MD_X86_CX);
    th_resident_load_reg(&b, MD_X86_SI);
    th16(&b, th_movs(3u, 0u));                        /* retired = 0 */
    th16(&b, th_ldrh(2u, 4u, foff));
    th16(&b, th_lsl_imm(2u, 2u, 21u));                /* DF (bit 10) -> N */
    bmi_back = th_emit_bcond_placeholder(&b, 4u);     /* BMI */
    if (!th_emit_lods_copy(&b, block, 0, loop_start)) return 0;
    if (!th_patch_bcond(&b, bmi_back, 4u, b.at)) {
        /* the forward copy is too long for a short branch: give up on the
           resident form; the general emitter still handles the block */
        return 0;
    }
    if (!th_emit_lods_copy(&b, block, 1, loop_start)) return 0;

    if (b.failed || !md_jit_install_native(jit, block, &b)) return 0;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = 1u;
    block->resident = 1u;
    ++jit->local_edges;
    ++jit->resident_regions;
    return 1;
}

static int md_jit_emit_resident(MdJit *jit, MdJitBlock *block)
{
    if (md_jit_emit_resident_lodsloop(jit, block)) return 1;   /* M20.3 */
    if (md_jit_is_resident_loop(block)) return md_jit_emit_resident_loop(jit, block);
    if (md_jit_is_resident_memloop(block)) return md_jit_emit_resident_memloop(jit, block);
    if (md_jit_emit_resident_cfg(jit, block)) return 1;
    if (md_jit_emit_resident_counted(jit, block)) return 1;
    return 0;
}

static void th_emit_control_one(MdThumbBuf *b, const MdJitOp *op, unsigned index)
{
    /* M20.2.1 correctness fix: direct Thumb instructions before this control
       op update architectural registers/flags but intentionally do not store
       IP after every instruction. md_jit_control_one() validates cpu->ip ==
       op->ip before performing CALL/RET/INT/IRET, so synchronize IP at the
       helper boundary. Without this, a control op later in a translated block
       silently becomes a no-op and the stale block start is re-entered. */
    th_store_ip(b, op->ip);
    th16(b, th_mov(0u, 4u));             /* runtime */
    th16(b, th_mov(1u, 5u));             /* block */
    th16(b, th_movs(2u, index));
    th_load_imm32(b, 3u, (uint32_t)(uintptr_t)&md_jit_control_one);
    th16(b, 0x4798u);                    /* blx r3 */
    th16(b, th_add_imm(6u, 1u));         /* control instruction retired */
    th_return(b);                        /* target/CS is now architectural */
}

/* M20.3: LODS / reg-reg ALU executed by the shared C semantics without
   leaving the native block (no dispatcher round trip, no fallback). The
   helper reads/writes architectural state in MdX86, which the general
   emitter keeps current, and clobbers only r0-r3/r12. */
static void th_emit_sem_one(MdThumbBuf *b, unsigned index)
{
    th16(b, th_mov(0u, 4u));             /* runtime */
    th16(b, th_mov(1u, 5u));             /* block */
    th16(b, th_movs(2u, index));
    th_load_imm32(b, 3u, (uint32_t)(uintptr_t)&md_jit_sem_one);
    th16(b, 0x4798u);                    /* blx r3 */
    th16(b, th_add_imm(6u, 1u));         /* retired */
}

/* M20.3: LOOP/LOOPZ/LOOPNZ/JCXZ. The helper updates CX and returns taken;
   an internal backward target becomes a native branch with the same exact
   budget check as th_emit_jnz, otherwise the block exits at the precise
   target or fallthrough IP. */
static int th_emit_loop(MdThumbBuf *b, const MdJitBlock *block, unsigned index,
                        const size_t *op_native)
{
    const MdJitOp *op = &block->ops[index];
    const int ti = md_jit_find_op_ip(block, op->target);
    size_t beq_not_taken;
    th16(b, th_mov(0u, 4u));
    th16(b, th_mov(1u, 5u));
    th16(b, th_movs(2u, index));
    th_load_imm32(b, 3u, (uint32_t)(uintptr_t)&md_jit_loop_one);
    th16(b, 0x4798u);                    /* blx r3: r0 = taken */
    th16(b, th_add_imm(6u, 1u));         /* LOOP itself retired */
    th16(b, th_cmp_imm(0u, 0u));
    beq_not_taken = th_emit_bcond_placeholder(b, 0u);
    if (ti >= 0 && (unsigned)ti <= index) {
        const unsigned span = index - (unsigned)ti + 1u;
        size_t blo;
        th16(b, th_mov(0u, 7u));
        th16(b, th_sub_reg(0u, 0u, 6u)); /* remaining = budget - retired */
        th16(b, th_cmp_imm(0u, span));
        blo = th_emit_bcond_placeholder(b, 3u);
        if (!th_emit_b(b, op_native[ti])) return 0;
        if (!th_patch_bcond(b, blo, 3u, b->at)) return 0;
    }
    th_store_ip(b, op->target);
    th_return(b);
    if (!th_patch_bcond(b, beq_not_taken, 0u, b->at)) return 0;
    th_store_ip(b, op->next_ip);
    th_return(b);
    return !b->failed;
}

static int md_jit_emit_thumb(MdJit *jit, MdJitBlock *block)
{
    MdThumbBuf b;

    /* M19.2: recognize loop-shaped regions before the conservative per-op
       emitter. A failed/unsafe specialization simply falls through to M19.1. */
    if (md_jit_emit_resident(jit, block)) return 1;
    size_t op_native[MD_JIT_MAX_OPS];
    unsigned i;
    unsigned direct = 0u;
    enum MdJitCfState cf = MD_JIT_CF_RAW;
    int needs_materialize = 0;
    memset(&b, 0, sizeof(b));
    memset(op_native, 0, sizeof(op_native));

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
    block->direct_prefix_ops = (uint8_t)direct;

    th16(&b, 0xB5F8u);                    /* push {r3,r4,r5,r6,r7,lr} */
    th16(&b, th_mov(4u, 0u));             /* r4=runtime */
    th16(&b, th_mov(5u, 1u));             /* r5=block */
    th16(&b, th_mov(7u, 2u));             /* r7=budget */
    th16(&b, th_movs(6u, 0u));            /* retired=0 */
    if (needs_materialize && direct != 0u) {
        th_emit_materialize(&b);
        ++jit->helper_sites;
    }

    /* Re-run the CF state while emitting. Materialisation makes RAW valid. */
    cf = MD_JIT_CF_RAW;
    for (i = 0u; i < direct && !b.failed; ++i) {
        const MdJitOp *op = &block->ops[i];
        op_native[i] = b.at;

        switch ((MdJitOpKind)op->kind) {
            case MD_JIT_OP_MOV_R8_IMM: {
                const unsigned off = (unsigned)(offsetof(MdX86, r) + (op->reg & 3u) * 2u + ((op->reg & 4u) ? 1u : 0u));
                if (!th_offset_ok_b(off)) { b.failed = 1; break; }
                th16(&b, th_movs(0u, (uint8_t)op->imm));
                th16(&b, th_strb(0u, 4u, off));
                th16(&b, th_add_imm(6u, 1u));
                break;
            }
            case MD_JIT_OP_MOV_R16_IMM: {
                const unsigned off = (unsigned)(offsetof(MdX86, r) + (size_t)op->reg * 2u);
                if (!th_offset_ok_h(off)) { b.failed = 1; break; }
                th_load_imm16(&b, 0u, op->imm);
                th16(&b, th_strh(0u, 4u, off));
                th16(&b, th_add_imm(6u, 1u));
                break;
            }
            case MD_JIT_OP_INC_R16:
                th_emit_incdec16(&b, op, 0, cf);
                th16(&b, th_add_imm(6u, 1u));
                break;
            case MD_JIT_OP_DEC_R16:
                th_emit_incdec16(&b, op, 1, cf);
                th16(&b, th_add_imm(6u, 1u));
                break;
            case MD_JIT_OP_ALU_ACC_IMM:
                th_emit_alu_acc(&b, op);
                th16(&b, th_add_imm(6u, 1u));
                if (op->aux == 1u || op->aux == 4u || op->aux == 6u) cf = MD_JIT_CF_ZERO;
                else cf = MD_JIT_CF_LAZY_UNKNOWN;
                break;
            case MD_JIT_OP_GRP1_R16_IMM:
                th_emit_grp1_r16(&b, op);
                th16(&b, th_add_imm(6u, 1u));
                if (op->aux == 1u || op->aux == 4u || op->aux == 6u) cf = MD_JIT_CF_ZERO;
                else cf = MD_JIT_CF_LAZY_UNKNOWN;
                break;
            case MD_JIT_OP_MOV_AL_SI:
                th_emit_mov_al_si(&b);
                th16(&b, th_add_imm(6u, 1u));
                break;
            case MD_JIT_OP_MOV_SI_AL:
                th_emit_mov_si_al(&b, op);
                /* slow path above already retires/returns; fast path retires here */
                th16(&b, th_add_imm(6u, 1u));
                ++jit->helper_sites;               /* emitted slow store site */
                break;
            case MD_JIT_OP_JCC:
                th16(&b, th_add_imm(6u, 1u));      /* Jcc itself */
                if (!th_emit_jnz(&b, block, i, op_native)) b.failed = 1;
                if (!b.failed) { ++block->local_edges; ++jit->local_edges; }
                /* th_emit_jnz emitted all exits/branches; nothing follows. */
                i = direct;
                break;
            case MD_JIT_OP_JMP: {
                const int ti = md_jit_find_op_ip(block, op->target);
                th16(&b, th_add_imm(6u, 1u));
                if (ti >= 0 && (unsigned)ti < i) {
                    /* Exact budget: need all instructions target..here next time. */
                    const unsigned span = i - (unsigned)ti + 1u;
                    size_t blo;
                    th16(&b, th_mov(0u, 7u));
                    th16(&b, th_sub_reg(0u, 0u, 6u));
                    th16(&b, th_cmp_imm(0u, span));
                    blo = th_emit_bcond_placeholder(&b, 3u);
                    if (!th_emit_b(&b, op_native[ti])) { b.failed = 1; break; }
                    {
                        const size_t ex = b.at;
                        if (!th_patch_bcond(&b, blo, 3u, ex)) { b.failed = 1; break; }
                        th_store_ip(&b, op->target);
                        th_return(&b);
                    }
                    ++block->local_edges; ++jit->local_edges;
                } else {
                    th_store_ip(&b, op->target);
                    th_return(&b);
                }
                i = direct;
                break;
            }
            case MD_JIT_OP_CALL_NEAR:
            case MD_JIT_OP_RET_NEAR:
            case MD_JIT_OP_RET_NEAR_IMM:
            case MD_JIT_OP_RET_FAR:
            case MD_JIT_OP_RET_FAR_IMM:
            case MD_JIT_OP_INT:
            case MD_JIT_OP_IRET:
                th_emit_control_one(&b, op, i);
                block->keep_ops = 1u;
                ++jit->helper_sites;
                i = direct;
                break;
            case MD_JIT_OP_LODS:
            case MD_JIT_OP_ALU_RR16:
            case MD_JIT_OP_ALU_RR8:
                th_emit_sem_one(&b, i);
                block->keep_ops = 1u;
                ++jit->helper_sites;
                if (op->kind != MD_JIT_OP_LODS)
                    cf = (op->aux == 1u || op->aux == 4u || op->aux == 6u) ? MD_JIT_CF_ZERO
                                                                         : MD_JIT_CF_LAZY_UNKNOWN;
                break;
            case MD_JIT_OP_LOOP:
                if (!th_emit_loop(&b, block, i, op_native)) b.failed = 1;
                block->keep_ops = 1u;
                ++jit->helper_sites;
                i = direct;
                break;
            case MD_JIT_OP_NOP:
                th16(&b, th_add_imm(6u, 1u));
                break;
            case MD_JIT_OP_HLT: {
                const unsigned soff = (unsigned)offsetof(MdRuntime, stop_reason);
                if (!th_offset_ok_w(soff)) { b.failed = 1; break; }
                th16(&b, th_add_imm(6u, 1u));
                th_store_ip(&b, op->next_ip);
                th16(&b, th_movs(0u, MD_STOP_HALT));
                th16(&b, (uint16_t)(0x6000u | (((soff >> 2) & 31u) << 6) | (4u << 3) | 0u)); /* str r0,[r4,#off] */
                th_return(&b);
                i = direct;
                break;
            }
            default:
                b.failed = 1;
                break;
        }
    }

    if (!b.failed) {
        /* Straight-line prefix ended without an emitted control exit. */
        if (direct == 0u) {
            th_store_ip(&b, block->ip);
            th_return(&b);
        } else if (block->ops[direct - 1u].kind != MD_JIT_OP_JCC &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_JMP &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_CALL_NEAR &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_RET_NEAR &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_RET_NEAR_IMM &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_RET_FAR &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_RET_FAR_IMM &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_INT &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_IRET &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_LOOP &&
                   block->ops[direct - 1u].kind != MD_JIT_OP_HLT) {
            const uint16_t next = direct < block->op_count ? block->ops[direct].ip
                                                           : block->ops[direct - 1u].next_ip;
            th_store_ip(&b, next);
            th_return(&b);
        }
    }

    if (b.failed || b.at == 0u) return 0;
    return md_jit_install_native(jit, block, &b);
}

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

static MdJitBlock *md_jit_compile(MdJit *jit, MdRuntime *runtime)
{
    uint8_t image[MD_JIT_DECODE_WINDOW];
    const uint16_t cs = runtime->cpu.cs;
    const uint16_t start_ip = runtime->cpu.ip;
    const size_t avail = (size_t)((0x10000u - (uint32_t)start_ip) < MD_JIT_DECODE_WINDOW
                                      ? (0x10000u - (uint32_t)start_ip)
                                      : MD_JIT_DECODE_WINDOW);
    MdJitBlock *block;
    unsigned slot;
    uint16_t ip;
    size_t i;
    uint32_t linear0, linear1;

    if (avail == 0u) return NULL;
    for (i = 0u; i < avail; ++i) image[i] = md_x86_read8(&runtime->cpu, cs, (uint16_t)(start_ip + i));

    slot = md_jit_hash(cs, start_ip);
    block = &jit->blocks[slot];
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
            if (block->op_count == 0u) return NULL;
            break;
        }
        op = &block->ops[block->op_count++];
        md_jit_classify(image, start_ip, &inst, op);
        ip = inst.next_ip;
        if (md_jit_boundary(&inst) &&
            !md_jit_continue_forward_conditional(&inst, start_ip, avail)) break;
    }

    if (block->op_count == 0u || ip < start_ip) return NULL;
    block->end_ip = ip;
    block->source_bytes = (uint16_t)(ip - start_ip);

    linear0 = md_x86_linear(cs, start_ip);
    if (linear0 + block->source_bytes > MD_X86_ADDRESS_SPACE) return NULL;
    linear1 = linear0 + block->source_bytes - 1u;
    block->page0 = (uint8_t)md_x86_code_page(linear0);
    block->page1 = (uint8_t)md_x86_code_page(linear1);
    block->page_count = block->page0 == block->page1 ? 1u : 2u;

    md_runtime_mark_code_range(runtime, cs, start_ip, block->source_bytes);
    block->code_epoch = runtime->code_epoch;
    block->page_gen0 = runtime->code_page_generation[block->page0];
    block->page_gen1 = runtime->code_page_generation[block->page1];

    if (!md_jit_emit_thumb(jit, block) || !md_jit_persist_ops(jit, block)) {
        /* Arena full: discard translations and compile this block into the
           fresh arena. Guest state/decoder metadata remain authoritative. */
        md_jit_flush_code(jit);
        slot = md_jit_hash(cs, start_ip);
        block = &jit->blocks[slot];
        memset(block, 0, sizeof(*block));
        block->ops = jit->compile_ops;
        block->materialize = md_jit_materialize;
        block->store8_slow = md_jit_store8_slow;
        block->exec_one = md_jit_exec_one;
        block->owner = jit;
        block->cs = cs;
        block->ip = start_ip;
        ip = start_ip;
        for (i = 0u; i < avail && block->op_count < MD_JIT_MAX_OPS;) {
            MdDecodedInstruction inst;
            MdJitOp *op;
            if (!md_decode_8086(image, avail, start_ip, ip, &inst) || !inst.valid_8086) return NULL;
            op = &block->ops[block->op_count++];
            md_jit_classify(image, start_ip, &inst, op);
            ip = inst.next_ip;
            i = (size_t)(uint16_t)(ip - start_ip);
            if (md_jit_boundary(&inst) &&
            !md_jit_continue_forward_conditional(&inst, start_ip, avail)) break;
        }
        block->end_ip = ip;
        block->source_bytes = (uint16_t)(ip - start_ip);
        block->page0 = (uint8_t)md_x86_code_page(linear0);
        block->page1 = (uint8_t)md_x86_code_page(linear0 + block->source_bytes - 1u);
        block->page_count = block->page0 == block->page1 ? 1u : 2u;
        block->code_epoch = runtime->code_epoch;
        block->page_gen0 = runtime->code_page_generation[block->page0];
        block->page_gen1 = runtime->code_page_generation[block->page1];
        if (!md_jit_emit_thumb(jit, block) || !md_jit_persist_ops(jit, block)) return NULL;
    }

    block->valid = 1u;
    MD_JIT_STAT(++jit->compiles);
    return block;
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
    return md_jit_compile(jit, runtime);
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
#if defined(__arm__) || defined(__thumb__)
    return block->native(runtime, block, budget);
#else
    unsigned i;
    uint32_t before = (uint32_t)runtime->instructions;
    for (i = 0u; i < block->op_count && runtime->stop_reason == MD_STOP_NONE; ++i) {
        if ((uint32_t)(runtime->instructions - before) >= budget) break;
        block->exec_one(runtime, block, i);
        if (runtime->cpu.ip != block->ops[i].next_ip) break;
    }
    return (uint32_t)(runtime->instructions - before);
#endif
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
#if defined(__arm__) || defined(__thumb__)
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
            const uint16_t fcs = runtime->cpu.cs, fip = runtime->cpu.ip;
            const uint8_t fop = md_x86_read8(&runtime->cpu, fcs, fip);
            MD_JIT_STAT(++jit->boundary_fallbacks);
            MD_JIT_STAT(++jit->fallback_instructions);
            MD_JIT_STAT(++jit->zero_progress_fallbacks);
            md_jit_record_exit(jit, runtime, MD_JIT_EXIT_ZERO_PROGRESS);
            (void)md_interp_step(runtime);
            if (watch_cs && runtime->cpu.cs != cs0) {
                MD_JIT_STAT(++jit->cs_change_exits); ++jit->exit_reason[MD_JIT_EXIT_CS_CHANGE];
                md_jit_record_transfer(jit, runtime, MD_JIT_EXIT_CS_CHANGE, fcs, fip, fop);
                return MD_STOP_NONE;
            }
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
