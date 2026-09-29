#include "microdos/block_cache.h"
#include "microdos/ops.h"

#include <string.h>

static unsigned md_cache_index(uint16_t cs, uint16_t ip)
{
    uint32_t x = ((uint32_t)cs << 16) | (uint32_t)ip;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    return (unsigned)(x & (MD_BLOCK_CACHE_SLOTS - 1u));
}

static uint8_t md_peek8(const MdX86 *cpu, uint16_t cs, uint16_t ip)
{
    return md_x86_read8(cpu, cs, ip);
}

static uint16_t md_peek16(const MdX86 *cpu, uint16_t cs, uint16_t ip)
{
    return md_x86_read16(cpu, cs, ip);
}

static int md_is_terminator(MdDecodedKind kind)
{
    switch (kind) {
        case MD_DOP_FALLBACK:
        case MD_DOP_DEC_JNZ:
        case MD_DOP_JZ:
        case MD_DOP_JNZ:
        case MD_DOP_INT:
        case MD_DOP_CALL:
        case MD_DOP_JMP:
        case MD_DOP_RET:
        case MD_DOP_HLT:
            return 1;
        default:
            return 0;
    }
}

static int md_decode_one(const MdX86 *cpu, uint16_t cs, uint16_t ip,
                         MdDecodedOp *op)
{
    const uint8_t opcode = md_peek8(cpu, cs, ip);
    uint16_t next = (uint16_t)(ip + 1u);

    memset(op, 0, sizeof(*op));
    op->opcode = opcode;
    op->next_ip = next;
    op->guest_count = 1u;

    if ((opcode & 0xF8u) == 0xB0u) {
        op->kind = MD_DOP_MOV_R8_IMM;
        op->reg = opcode & 7u;
        op->arg = md_peek8(cpu, cs, next);
        op->next_ip = (uint16_t)(next + 1u);
        return 1;
    }
    if ((opcode & 0xF8u) == 0xB8u) {
        op->kind = MD_DOP_MOV_R16_IMM;
        op->reg = opcode & 7u;
        op->arg = md_peek16(cpu, cs, next);
        op->next_ip = (uint16_t)(next + 2u);
        return 1;
    }
    if ((opcode & 0xF8u) == 0x40u) {
        op->kind = MD_DOP_INC_R16;
        op->reg = opcode & 7u;
        return 1;
    }
    if ((opcode & 0xF8u) == 0x48u) {
        /* Very common 8086 loop shape. Predecode DEC r16 + JNZ rel8 as one
           super-op, while preserving a two-instruction architectural count. */
        if (md_peek8(cpu, cs, next) == 0x75u) {
            const int8_t rel = (int8_t)md_peek8(cpu, cs, (uint16_t)(next + 1u));
            op->kind = MD_DOP_DEC_JNZ;
            op->reg = opcode & 7u;
            op->next_ip = (uint16_t)(next + 2u);
            op->arg = (uint16_t)(op->next_ip + rel);
            op->guest_count = 2u;
            return 1;
        }
        op->kind = MD_DOP_DEC_R16;
        op->reg = opcode & 7u;
        return 1;
    }
    if ((opcode & 0xF8u) == 0x50u) {
        op->kind = MD_DOP_PUSH_R16;
        op->reg = opcode & 7u;
        return 1;
    }
    if ((opcode & 0xF8u) == 0x58u) {
        op->kind = MD_DOP_POP_R16;
        op->reg = opcode & 7u;
        return 1;
    }

    switch (opcode) {
        case 0x04:
            op->kind = MD_DOP_ADD_AL_IMM;
            op->arg = md_peek8(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 1u);
            return 1;
        case 0x05:
            op->kind = MD_DOP_ADD_AX_IMM;
            op->arg = md_peek16(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 2u);
            return 1;
        case 0x2C:
            op->kind = MD_DOP_SUB_AL_IMM;
            op->arg = md_peek8(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 1u);
            return 1;
        case 0x2D:
            op->kind = MD_DOP_SUB_AX_IMM;
            op->arg = md_peek16(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 2u);
            return 1;
        case 0x3C:
            op->kind = MD_DOP_CMP_AL_IMM;
            op->arg = md_peek8(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 1u);
            return 1;
        case 0x3D:
            op->kind = MD_DOP_CMP_AX_IMM;
            op->arg = md_peek16(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 2u);
            return 1;
        case 0x74:
        case 0x75: {
            const int8_t rel = (int8_t)md_peek8(cpu, cs, next);
            op->kind = opcode == 0x74u ? MD_DOP_JZ : MD_DOP_JNZ;
            op->next_ip = (uint16_t)(next + 1u);
            op->arg = (uint16_t)(op->next_ip + rel);
            return 1;
        }
        case 0x90:
            op->kind = MD_DOP_NOP;
            return 1;
        case 0xA0:
            op->kind = MD_DOP_MOV_AL_MOFFS;
            op->arg = md_peek16(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 2u);
            return 1;
        case 0xA1:
            op->kind = MD_DOP_MOV_AX_MOFFS;
            op->arg = md_peek16(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 2u);
            return 1;
        case 0xA2:
            op->kind = MD_DOP_MOV_MOFFS_AL;
            op->arg = md_peek16(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 2u);
            return 1;
        case 0xA3:
            op->kind = MD_DOP_MOV_MOFFS_AX;
            op->arg = md_peek16(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 2u);
            return 1;
        case 0xC3:
            op->kind = MD_DOP_RET;
            return 1;
        case 0xCD:
            op->kind = MD_DOP_INT;
            op->arg = md_peek8(cpu, cs, next);
            op->next_ip = (uint16_t)(next + 1u);
            return 1;
        case 0xE8: {
            const int16_t rel = (int16_t)md_peek16(cpu, cs, next);
            op->kind = MD_DOP_CALL;
            op->next_ip = (uint16_t)(next + 2u);
            op->arg = (uint16_t)(op->next_ip + rel);
            return 1;
        }
        case 0xE9: {
            const int16_t rel = (int16_t)md_peek16(cpu, cs, next);
            op->kind = MD_DOP_JMP;
            op->next_ip = (uint16_t)(next + 2u);
            op->arg = (uint16_t)(op->next_ip + rel);
            return 1;
        }
        case 0xEB: {
            const int8_t rel = (int8_t)md_peek8(cpu, cs, next);
            op->kind = MD_DOP_JMP;
            op->next_ip = (uint16_t)(next + 1u);
            op->arg = (uint16_t)(op->next_ip + rel);
            return 1;
        }
        case 0xF4:
            op->kind = MD_DOP_HLT;
            return 1;
        default:
            op->kind = MD_DOP_FALLBACK;
            op->next_ip = ip;
            return 0;
    }
}


static int md_kind_may_write(MdDecodedKind kind)
{
    switch (kind) {
        case MD_DOP_PUSH_R16:
        case MD_DOP_MOV_MOFFS_AL:
        case MD_DOP_MOV_MOFFS_AX:
        case MD_DOP_INT:
        case MD_DOP_CALL:
            return 1;
        default:
            return 0;
    }
}

static void md_snapshot_block_pages(MdRuntime *runtime, MdDecodedBlock *block,
                                    uint16_t cs, uint16_t ip, unsigned byte_count)
{
    unsigned i;

    block->page_count = 0u;
    if (byte_count == 0u) byte_count = 1u;

    for (i = 0u; i < byte_count; ++i) {
        const uint32_t linear = md_x86_linear(cs, (uint16_t)(ip + (uint16_t)i));
        const uint8_t page = (uint8_t)md_x86_code_page(linear);
        unsigned j;
        int seen = 0;

        runtime->code_page_executable[page] = 1u;

        for (j = 0u; j < block->page_count; ++j) {
            if (block->code_page[j] == page) {
                seen = 1;
                break;
            }
        }

        if (!seen && block->page_count < 2u) {
            const unsigned dst = block->page_count++;
            block->code_page[dst] = page;
            block->page_generation[dst] = runtime->code_page_generation[page];
        }
    }
}

static int md_block_pages_valid(const MdRuntime *runtime, const MdDecodedBlock *block)
{
    unsigned i;

    if (block->epoch != runtime->code_epoch) return 0;
    for (i = 0u; i < block->page_count; ++i) {
        const unsigned page = block->code_page[i];
        if (runtime->code_page_generation[page] != block->page_generation[i]) {
            return 0;
        }
    }
    return 1;
}

static MdDecodedBlock *md_decode_block(MdRuntime *runtime, MdBlockCache *cache,
                                        uint16_t cs, uint16_t ip)
{
    const unsigned slot = md_cache_index(cs, ip);
    MdDecodedBlock *block = &cache->slots[slot];
    uint16_t cursor = ip;
    unsigned count = 0u;
    unsigned guest_count = 0u;
    unsigned byte_count = 0u;

    memset(block, 0, sizeof(*block));
    block->epoch = runtime->code_epoch;
    block->cs = cs;
    block->ip = ip;

    while (count < MD_BLOCK_MAX_OPS) {
        MdDecodedOp op;
        const int supported = md_decode_one(&runtime->cpu, cs, cursor, &op);

        if (!supported && count != 0u) {
            break;
        }

        block->ops[count++] = op;
        guest_count += op.guest_count;
        if (md_kind_may_write((MdDecodedKind)op.kind)) block->may_write = 1u;
        if (!supported) {
            block->fallback = 1u;
            byte_count += 1u;
            break;
        }

        byte_count += (unsigned)(uint16_t)(op.next_ip - cursor);
        cursor = op.next_ip;
        if (md_is_terminator((MdDecodedKind)op.kind) ||
            md_kind_may_write((MdDecodedKind)op.kind)) break;
    }

    block->count = (uint8_t)count;
    block->guest_count = (uint8_t)guest_count;
    md_snapshot_block_pages(runtime, block, cs, ip, byte_count);
    ++cache->decodes;
    return block;
}

static MdDecodedBlock *md_lookup_block(MdRuntime *runtime, MdBlockCache *cache)
{
    const uint16_t cs = runtime->cpu.cs;
    const uint16_t ip = runtime->cpu.ip;
    const unsigned slot = md_cache_index(cs, ip);
    MdDecodedBlock *block = &cache->slots[slot];

    if (block->cs == cs && block->ip == ip && block->count != 0u) {
        if (md_block_pages_valid(runtime, block)) {
            ++cache->hits;
            return block;
        }
        if (block->epoch == runtime->code_epoch) {
            ++cache->invalidations;
        }
    }

    ++cache->misses;
    return md_decode_block(runtime, cache, cs, ip);
}

void md_block_cache_init(MdBlockCache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

void md_block_cache_clear_stats(MdBlockCache *cache)
{
    cache->hits = 0u;
    cache->misses = 0u;
    cache->decodes = 0u;
    cache->invalidations = 0u;
    cache->fallback_instructions = 0u;
}

static void md_exec_decoded(MdRuntime *runtime, const MdDecodedOp *op)
{
    MdX86 *cpu = &runtime->cpu;
    const uint16_t old_cf = cpu->flags & MD_X86_FLAG_CF;

    cpu->ip = op->next_ip;

    switch ((MdDecodedKind)op->kind) {
        case MD_DOP_MOV_R8_IMM:
            md_x86_set_reg8(cpu, op->reg, (uint8_t)op->arg);
            break;
        case MD_DOP_MOV_R16_IMM:
            cpu->r[op->reg] = op->arg;
            break;
        case MD_DOP_INC_R16:
            cpu->r[op->reg] = md_x86_add16(cpu, cpu->r[op->reg], 1u);
            cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | old_cf);
            break;
        case MD_DOP_DEC_R16:
            cpu->r[op->reg] = md_x86_sub16(cpu, cpu->r[op->reg], 1u);
            cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | old_cf);
            break;
        case MD_DOP_DEC_JNZ:
            cpu->r[op->reg] = md_x86_sub16(cpu, cpu->r[op->reg], 1u);
            cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | old_cf);
            if ((cpu->flags & MD_X86_FLAG_ZF) == 0u) cpu->ip = op->arg;
            break;
        case MD_DOP_PUSH_R16:
            md_x86_push(cpu, cpu->r[op->reg]);
            break;
        case MD_DOP_POP_R16:
            cpu->r[op->reg] = md_x86_pop(cpu);
            break;
        case MD_DOP_ADD_AL_IMM:
            md_x86_set_reg8(cpu, 0u, md_x86_add8(cpu, md_x86_get_reg8(cpu, 0u), (uint8_t)op->arg));
            break;
        case MD_DOP_ADD_AX_IMM:
            cpu->r[MD_X86_AX] = md_x86_add16(cpu, cpu->r[MD_X86_AX], op->arg);
            break;
        case MD_DOP_SUB_AL_IMM:
            md_x86_set_reg8(cpu, 0u, md_x86_sub8(cpu, md_x86_get_reg8(cpu, 0u), (uint8_t)op->arg));
            break;
        case MD_DOP_SUB_AX_IMM:
            cpu->r[MD_X86_AX] = md_x86_sub16(cpu, cpu->r[MD_X86_AX], op->arg);
            break;
        case MD_DOP_CMP_AL_IMM:
            (void)md_x86_sub8(cpu, md_x86_get_reg8(cpu, 0u), (uint8_t)op->arg);
            break;
        case MD_DOP_CMP_AX_IMM:
            (void)md_x86_sub16(cpu, cpu->r[MD_X86_AX], op->arg);
            break;
        case MD_DOP_MOV_AL_MOFFS:
            md_x86_set_reg8(cpu, 0u, md_x86_read8(cpu, cpu->ds, op->arg));
            break;
        case MD_DOP_MOV_AX_MOFFS:
            cpu->r[MD_X86_AX] = md_x86_read16(cpu, cpu->ds, op->arg);
            break;
        case MD_DOP_MOV_MOFFS_AL:
            md_x86_write8(cpu, cpu->ds, op->arg, md_x86_get_reg8(cpu, 0u));
            break;
        case MD_DOP_MOV_MOFFS_AX:
            md_x86_write16(cpu, cpu->ds, op->arg, cpu->r[MD_X86_AX]);
            break;
        case MD_DOP_JZ:
            if ((cpu->flags & MD_X86_FLAG_ZF) != 0u) cpu->ip = op->arg;
            break;
        case MD_DOP_JNZ:
            if ((cpu->flags & MD_X86_FLAG_ZF) == 0u) cpu->ip = op->arg;
            break;
        case MD_DOP_NOP:
            break;
        case MD_DOP_INT:
            (void)md_runtime_interrupt(runtime, (uint8_t)op->arg);
            break;
        case MD_DOP_CALL:
            md_x86_push(cpu, op->next_ip);
            cpu->ip = op->arg;
            break;
        case MD_DOP_JMP:
            cpu->ip = op->arg;
            break;
        case MD_DOP_RET:
            cpu->ip = md_x86_pop(cpu);
            break;
        case MD_DOP_HLT:
            runtime->stop_reason = MD_STOP_HALT;
            break;
        default:
            runtime->stop_reason = MD_STOP_FAULT;
            runtime->fault_opcode = op->opcode;
            runtime->fault_linear = md_x86_linear(cpu->cs, cpu->ip);
            break;
    }
}

static void md_exec_hot_dec_jnz(MdRuntime *runtime, const MdDecodedOp *op)
{
    MdX86 *cpu = &runtime->cpu;
    const uint16_t old_cf = cpu->flags & MD_X86_FLAG_CF;

    cpu->r[op->reg] = md_x86_sub16(cpu, cpu->r[op->reg], 1u);
    cpu->flags = (uint16_t)((cpu->flags & ~MD_X86_FLAG_CF) | old_cf);
    cpu->ip = ((cpu->flags & MD_X86_FLAG_ZF) == 0u) ? op->arg : op->next_ip;
}

MdStopReason md_interp_run_cached_until(MdRuntime *runtime, MdBlockCache *cache,
                                        uint64_t instruction_budget,
                                        MdCacheStopPredicate stop_predicate,
                                        void *stop_user)
{
    uint64_t remaining = instruction_budget;
    MdDecodedBlock *block = NULL;

    while (runtime->stop_reason == MD_STOP_NONE) {
        unsigned i;

        if (stop_predicate != NULL && stop_predicate(runtime, stop_user)) {
            return MD_STOP_NONE;
        }

        if (remaining == 0u) {
            runtime->stop_reason = MD_STOP_BUDGET;
            break;
        }

        if (block == NULL) block = md_lookup_block(runtime, cache);

        if (block->fallback) {
            --remaining;
            ++cache->fallback_instructions;
            (void)md_interp_step(runtime);
            block = NULL;
            continue;
        }

        /* MSVC cannot use GCC's computed-goto interpreter. Keep the hottest
           cached loop terminator out of the generic decoded-op switch too. */
        if (block->count == 1u &&
            block->ops[0].kind == MD_DOP_DEC_JNZ &&
            remaining >= 2u) {
            remaining -= 2u;
            runtime->instructions += 2u;
            md_exec_hot_dec_jnz(runtime, &block->ops[0]);
        } else if (remaining >= block->guest_count) {
            remaining -= block->guest_count;
            runtime->instructions += block->guest_count;
            for (i = 0u; i < block->count; ++i) {
                md_exec_decoded(runtime, &block->ops[i]);
                if (runtime->stop_reason != MD_STOP_NONE) break;
            }
        } else {
            /* Keep instruction budgets exact even across fused super-ops. If the
               caller stops inside a fused pair, execute the canonical decoder one
               instruction at a time for that boundary case. */
            for (i = 0u; i < block->count && runtime->stop_reason == MD_STOP_NONE; ++i) {
                const MdDecodedOp *op = &block->ops[i];
                if (remaining < op->guest_count) {
                    if (remaining == 0u) {
                        runtime->stop_reason = MD_STOP_BUDGET;
                        break;
                    }
                    --remaining;
                    (void)md_interp_step(runtime);
                    block = NULL;
                    break;
                }
                remaining -= op->guest_count;
                runtime->instructions += op->guest_count;
                md_exec_decoded(runtime, op);
            }
        }

        if (runtime->stop_reason != MD_STOP_NONE) break;

        /* Direct self-chain: tight loops never return to the hash lookup after
           their first iteration. The page generation check is unnecessary here:
           this block did not perform a code write unless its source page was
           invalidated, in which case the next write epoch/page check occurs as
           soon as control leaves and re-enters the cache. */
        if (block != NULL && runtime->cpu.cs == block->cs && runtime->cpu.ip == block->ip) {
            if (!block->may_write || md_block_pages_valid(runtime, block)) {
                ++cache->hits;
                continue;
            }
            ++cache->invalidations;
        }

        block = NULL;
    }

    return runtime->stop_reason;
}

MdStopReason md_interp_run_cached(MdRuntime *runtime, MdBlockCache *cache,
                                  uint64_t instruction_budget)
{
    return md_interp_run_cached_until(runtime, cache, instruction_budget, NULL, NULL);
}
