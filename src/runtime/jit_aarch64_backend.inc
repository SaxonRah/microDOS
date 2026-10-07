/*
 * microDOS AArch64 JIT backend v1.5
 *
 * This is included by jit_core.c only when building for AArch64.
 *
 * v1.5 goals:
 *   - prove runtime-generated AArch64 code executes correctly from the JIT arena;
 *   - keep all admission, IR, invalidation, profiling and fallback policy shared;
 *   - retain exact canonical fallback for unsupported instructions;
 *   - keep the first lowering deliberately small.
 *
 * Native lowering in this first slice:
 *   MOV r16, imm16
 *   MOV r8,  imm8
 *   NOP
 *   JMP near
 *
 * Hot resident shapes use tiny emitted AArch64 tail-call trampolines into
 * shared C region helpers. v1.5 also chains already-compiled semantic helper
 * blocks internally across CALL/RET/Jcc/JMP/LOOP, eliminating repeated outer
 * dispatcher crossings on control-heavy code without recursive native calls.
 */

#if !defined(__aarch64__)
#error "jit_aarch64_backend.inc requires AArch64"
#endif

#define MD_JIT_A64_CODE_ALIGN 4u

typedef struct MdA64Buf {
    uint32_t words[MD_JIT_NATIVE_TMP / 4u];
    size_t count;
    int failed;
} MdA64Buf;

static void a64_emit(MdA64Buf *b, uint32_t insn)
{
    if (b->failed) return;
    if ((b->count + 1u) * sizeof(uint32_t) > sizeof(b->words)) {
        b->failed = 1;
        return;
    }
    b->words[b->count++] = insn;
}

/* MOVZ Wd,#imm16 */
static uint32_t a64_movz_w(unsigned rd, uint16_t imm)
{
    return 0x52800000u | ((uint32_t)imm << 5) | (rd & 31u);
}

/* MOVZ/MOVK Xd,#imm16,LSL #(16*hw) */
static uint32_t a64_movz_x(unsigned rd, uint16_t imm, unsigned hw)
{
    return 0xD2800000u | ((hw & 3u) << 21) | ((uint32_t)imm << 5) | (rd & 31u);
}

static uint32_t a64_movk_x(unsigned rd, uint16_t imm, unsigned hw)
{
    return 0xF2800000u | ((hw & 3u) << 21) | ((uint32_t)imm << 5) | (rd & 31u);
}

/* STRB Wt,[Xn,#byte_offset] */
static uint32_t a64_strb(unsigned wt, unsigned xn, unsigned off)
{
    return 0x39000000u |
           ((off & 0x0fffu) << 10) |
           ((xn & 31u) << 5) |
           (wt & 31u);
}

/* STRH Wt,[Xn,#byte_offset], unsigned scaled immediate. */
static uint32_t a64_strh(unsigned wt, unsigned xn, unsigned off)
{
    return 0x79000000u |
           (((off >> 1) & 0x0fffu) << 10) |
           ((xn & 31u) << 5) |
           (wt & 31u);
}

static uint32_t a64_br(unsigned xn)
{
    return 0xD61F0000u | ((xn & 31u) << 5);
}

static void a64_ret(MdA64Buf *b)
{
    a64_emit(b, 0xD65F03C0u); /* RET X30 */
}

static void a64_load_abs_x16(MdA64Buf *b, uintptr_t p)
{
    a64_emit(b, a64_movz_x(16u, (uint16_t)(p >> 0), 0u));
    a64_emit(b, a64_movk_x(16u, (uint16_t)(p >> 16), 1u));
    a64_emit(b, a64_movk_x(16u, (uint16_t)(p >> 32), 2u));
    a64_emit(b, a64_movk_x(16u, (uint16_t)(p >> 48), 3u));
}

/*
 * Generated A64 lives in Normal WB cacheable RAM on Pi Zero 2 W.
 * Clean D-cache to PoU, then invalidate the matching I-cache lines before
 * publishing the function pointer.
 */
static void md_jit_a64_sync_code(void *start, size_t bytes)
{
    uintptr_t first = (uintptr_t)start;
    uintptr_t end = first + bytes;
    uintptr_t p;
    uint64_t ctr;
    size_t dline;
    size_t iline;

    if (bytes == 0u) return;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));

    dline = (size_t)4u << ((ctr >> 16) & 0x0fu); /* DminLine: words -> bytes */
    iline = (size_t)4u << (ctr & 0x0fu);         /* IminLine: words -> bytes */

    p = first & ~((uintptr_t)dline - 1u);
    while (p < end) {
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
        p += dline;
    }

    __asm__ volatile("dsb sy" ::: "memory");

    p = first & ~((uintptr_t)iline - 1u);
    while (p < end) {
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
        p += iline;
    }

    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

static int md_jit_a64_install(MdJit *jit, MdJitBlock *block, const MdA64Buf *b)
{
    size_t arena_at;
    size_t bytes;

    if (jit == NULL || block == NULL || b == NULL || b->failed || b->count == 0u)
        return 0;

    bytes = b->count * sizeof(uint32_t);
    arena_at = (jit->code_used + (MD_JIT_A64_CODE_ALIGN - 1u)) &
               ~(size_t)(MD_JIT_A64_CODE_ALIGN - 1u);

    if (arena_at + bytes > jit->code_size) return 0;

    memcpy(jit->code + arena_at, b->words, bytes);
    block->native_offset = (uint32_t)arena_at;
    jit->code_used = arena_at + bytes;

    md_jit_a64_sync_code(jit->code + arena_at, bytes);

    block->native = (MdJitNativeFn)(uintptr_t)(jit->code + arena_at);
    return 1;
}

/*
 * Emit a five-instruction leaf trampoline:
 *
 *   movz/movk x16, absolute_helper
 *   br x16
 *
 * BR preserves X30, so the helper's RET returns directly to the C caller that
 * invoked block->native(). X0/X1/W2 already contain runtime/block/budget.
 */
static int md_jit_a64_tailcall(MdJit *jit, MdJitBlock *block, uintptr_t helper)
{
    MdA64Buf b;

    memset(&b, 0, sizeof(b));
    a64_load_abs_x16(&b, helper);
    a64_emit(&b, a64_br(16u));

    return md_jit_a64_install(jit, block, &b);
}

/*
 * Standalone md_jit_run() sees loop.com initially as:
 *
 *   MOV CX,imm
 *   DEC CX
 *   JNZ DEC
 *
 * The router path can instead discover the two-op DEC/JNZ live region after
 * interpretation has already advanced past MOV.  Support both shapes.
 *
 * This helper is intentionally shared C semantics behind an emitted A64
 * tail-call trampoline for v1.1.  It executes MOV once, then delegates the
 * counted DEC/JNZ body to the already-proven algebraic region helper.
 */
static uint32_t md_jit_a64_shared_full_dec_loop(MdRuntime *rt,
                                                 MdJitBlock *block,
                                                 uint32_t budget)
{
    const MdJitOp *o;
    uint32_t body;

    if (rt == NULL || block == NULL || block->ops == NULL || budget == 0u)
        return 0u;
    if (rt->stop_reason != MD_STOP_NONE || rt->cpu.cs != block->cs ||
        rt->cpu.ip != block->ip)
        return 0u;

    o = block->ops;
    if (block->op_count != 3u ||
        o[0].kind != MD_JIT_OP_MOV_R16_IMM ||
        o[0].reg != MD_X86_CX ||
        o[1].kind != MD_JIT_OP_DEC_R16 ||
        o[1].reg != MD_X86_CX ||
        o[2].kind != MD_JIT_OP_JCC ||
        o[2].aux != 5u ||
        o[2].target != o[1].ip)
        return 0u;

    rt->cpu.r[MD_X86_CX] = o[0].imm;
    rt->cpu.ip = o[1].ip;

    if (budget == 1u) return 1u;

    body = md_region_try_dec_jnz(rt, MD_X86_CX, o[1].ip,
                                 o[2].next_ip, budget - 1u);
    return 1u + body;
}

/* Emit a valid zero-progress native block for an unsupported A64 shape.
 *
 * The shared compiler's retry/flush path is reserved for code-arena pressure.
 * Returning emitter failure for an ordinary unsupported opcode caused the
 * standalone JIT benchmark to flush the entire arena on every boundary.
 *
 * A compiled zero stub instead returns through the normal zero-progress
 * escape path, which executes the canonical interpreter burst and keeps the
 * translation cache intact.
 */
static int md_jit_a64_emit_zero(MdJit *jit, MdJitBlock *block)
{
    MdA64Buf b;

    memset(&b, 0, sizeof(b));
    a64_emit(&b, a64_movz_w(0u, 0u)); /* W0 = retired 0 */
    a64_ret(&b);

    if (!md_jit_a64_install(jit, block, &b)) return 0;

    block->direct_prefix_ops = 0u;
    return 1;
}

static int md_jit_a64_emit_full_dec_loop(MdJit *jit, MdJitBlock *block)
{
    const MdJitOp *o;

    if (jit == NULL || block == NULL || block->ops == NULL)
        return 0;

    o = block->ops;
    if (block->op_count != 3u ||
        o[0].kind != MD_JIT_OP_MOV_R16_IMM ||
        o[0].reg != MD_X86_CX ||
        o[1].kind != MD_JIT_OP_DEC_R16 ||
        o[1].reg != MD_X86_CX ||
        o[2].kind != MD_JIT_OP_JCC ||
        o[2].aux != 5u ||
        o[2].target != o[1].ip)
        return 0;

    /*
     * The helper dereferences block->ops at runtime, so explicitly retain
     * this compact IR even after the compile scratch is reused.
     */
    block->keep_ops = 1u;

    if (!md_jit_a64_tailcall(
            jit, block,
            (uintptr_t)(void *)&md_jit_a64_shared_full_dec_loop))
        return 0;

    block->resident = 2u;
    block->direct_prefix_ops = 3u;
    block->local_edges = 1u;

    ++jit->resident_regions;
    ++jit->local_edges;
    ++jit->helper_sites;
    return 1;
}


/* Exact M21 memloop shape, shared with the Thumb backend's recognizer.
 *
 * Keep the hot guest state in C locals for the entire counted loop.  The
 * executable-page guard is identical in spirit to the Thumb resident path:
 * direct host-memory stores are allowed only when pages 8..15 are proven to
 * be pure data pages.
 *
 * Only the final OR and DEC need canonical flag bookkeeping. Earlier loop
 * iterations are algebraically dead with respect to architectural FLAGS.
 */
static uint32_t md_jit_a64_shared_memloop(MdRuntime *rt,
                                           MdJitBlock *block,
                                           uint32_t budget)
{
    MdX86 *cpu;
    const MdJitOp *o;
    uint32_t trips;
    uint32_t retired;
    uint32_t i;
    uint16_t cx;
    uint16_t si;
    uint8_t al;

    if (rt == NULL || block == NULL || block->ops == NULL)
        return 0u;
    if (rt->stop_reason != MD_STOP_NONE ||
        rt->cpu.cs != block->cs ||
        rt->cpu.ip != block->ip)
        return 0u;

    o = block->ops;
    if (!md_jit_is_resident_memloop(block))
        return 0u;

    cpu = &rt->cpu;
    if (cpu->memory == NULL ||
        cpu->code_page_executable == NULL ||
        cpu->ds != 0u)
        return 0u;

    /* The exact fixture keeps every [SI] access inside 8000h..FFFFh. */
    for (i = 8u; i <= 15u; ++i) {
        if (cpu->code_page_executable[i] != 0u)
            return 0u;
    }

    trips = o[0].imm != 0u ? (uint32_t)o[0].imm : 65536u;
    retired = 2u + trips * 7u;
    if (budget < retired)
        return 0u;

    cx = o[0].imm;
    si = o[1].imm;
    al = md_x86_get_reg8(cpu, 0u);

    /* All but the final iteration may use raw arithmetic: later ALU/DEC
       operations overwrite their flags before the region exits. */
    for (i = 1u; i < trips; ++i) {
        al = cpu->memory[si];
        al = (uint8_t)(al + (uint8_t)o[3].imm);
        cpu->memory[si] = al;
        si = (uint16_t)(si + o[5].imm);
        si = (uint16_t)(si | o[6].imm);
        cx = (uint16_t)(cx - 1u);
    }

    /* Final iteration: reproduce exact architectural lazy FLAGS.
       OR makes CF=0; DEC then preserves that CF while defining OSZAP. */
    al = cpu->memory[si];
    al = (uint8_t)(al + (uint8_t)o[3].imm);
    cpu->memory[si] = al;
    si = (uint16_t)(si + o[5].imm);
    si = md_aot_alu16(cpu, 1u, si, o[6].imm); /* OR SI,mask */
    cx = md_aot_incdec16(cpu, cx, 1);          /* final DEC CX */

    cpu->r[MD_X86_CX] = cx;
    cpu->r[MD_X86_SI] = si;
    md_x86_set_reg8(cpu, 0u, al);
    cpu->ip = o[8].next_ip;

    return retired;
}

static int md_jit_a64_emit_full_memloop(MdJit *jit, MdJitBlock *block)
{
    if (jit == NULL || block == NULL || block->ops == NULL)
        return 0;
    if (!md_jit_is_resident_memloop(block))
        return 0;

    /* Runtime helper reads the compact decoded descriptors. */
    block->keep_ops = 1u;

    if (!md_jit_a64_tailcall(
            jit, block,
            (uintptr_t)(void *)&md_jit_a64_shared_memloop))
        return 0;

    block->resident = 1u;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = 1u;

    ++jit->resident_regions;
    ++jit->local_edges;
    ++jit->helper_sites;
    return 1;
}

/* Full prefixed checksum shape used by the shared Pico M21 benchmark:
 *
 *   MOV SI,imm
 *   MOV CX,imm
 *   XOR DX,DX
 *   CLD
 * L:LODSW
 *   ADD DX,AX
 *   LOOP L
 *
 * The loop body is delegated to the existing architecture-neutral resident
 * checksum helper after executing the four-instruction prefix exactly.
 */
static int md_jit_a64_is_full_checksum(const MdJitBlock *block)
{
    const MdJitOp *o;

    if (block == NULL || block->ops == NULL || block->op_count != 7u)
        return 0;

    o = block->ops;

    return
        o[0].kind == MD_JIT_OP_MOV_R16_IMM &&
        o[0].reg == MD_X86_SI &&

        o[1].kind == MD_JIT_OP_MOV_R16_IMM &&
        o[1].reg == MD_X86_CX &&

        o[2].kind == MD_JIT_OP_ALU_RR16 &&
        o[2].aux == 6u &&                 /* XOR */
        o[2].reg == MD_X86_DX &&
        o[2].imm == MD_X86_DX &&

        o[3].kind == MD_JIT_OP_FALLBACK &&
        o[3].opcode == 0xFCu &&           /* CLD */

        o[4].kind == MD_JIT_OP_LODS &&
        o[4].aux == 2u &&

        o[5].kind == MD_JIT_OP_ALU_RR16 &&
        o[5].aux == 0u &&                 /* ADD */
        o[5].reg == MD_X86_DX &&
        o[5].imm == MD_X86_AX &&

        o[6].kind == MD_JIT_OP_LOOP &&
        o[6].aux == 2u &&
        o[6].target == o[4].ip;
}

static uint32_t md_jit_a64_shared_checksum(MdRuntime *rt,
                                            MdJitBlock *block,
                                            uint32_t budget)
{
    MdX86 *cpu;
    const MdJitOp *o;
    uint32_t trips;
    uint32_t required;
    uint32_t body;

    if (rt == NULL || block == NULL || block->ops == NULL)
        return 0u;
    if (rt->stop_reason != MD_STOP_NONE ||
        rt->cpu.cs != block->cs ||
        rt->cpu.ip != block->ip)
        return 0u;
    if (!md_jit_a64_is_full_checksum(block))
        return 0u;

    o = block->ops;
    trips = o[1].imm != 0u ? (uint32_t)o[1].imm : 65536u;
    required = 4u + trips * 3u;

    /* v1.2 admits this as one exact whole resident region. */
    if (budget < required)
        return 0u;

    cpu = &rt->cpu;

    cpu->r[MD_X86_SI] = o[0].imm;
    cpu->r[MD_X86_CX] = o[1].imm;

    /* XOR DX,DX through canonical lazy arithmetic semantics. */
    cpu->r[MD_X86_DX] =
        md_aot_alu16(cpu, 6u,
                     cpu->r[MD_X86_DX],
                     cpu->r[MD_X86_DX]);

    /* CLD modifies only DF; lazy arithmetic state from XOR remains valid. */
    cpu->flags_raw =
        (uint16_t)(cpu->flags_raw & (uint16_t)~MD_X86_FLAG_DF);

    cpu->ip = o[4].ip;

    body = md_region_try_lodsw_add_dx_ax_loop(
        rt, o[4].ip, o[6].next_ip, budget - 4u);

    if (body != trips * 3u)
        return 0u; /* budget proof above makes this unreachable on success */

    return 4u + body;
}

static int md_jit_a64_emit_full_checksum(MdJit *jit, MdJitBlock *block)
{
    if (jit == NULL || block == NULL || block->ops == NULL)
        return 0;
    if (!md_jit_a64_is_full_checksum(block))
        return 0;

    block->keep_ops = 1u;

    if (!md_jit_a64_tailcall(
            jit, block,
            (uintptr_t)(void *)&md_jit_a64_shared_checksum))
        return 0;

    block->resident = 1u;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = 1u;

    ++jit->resident_regions;
    ++jit->local_edges;
    ++jit->helper_sites;
    return 1;
}


/* ---- v1.3 generic counted / bounded-CFG resident helper ---------------
 *
 * Admission is NOT duplicated here. The existing shared M20 shape validators
 * remain authoritative:
 *
 *   md_jit_shape_counted()
 *   md_jit_shape_cfg()
 *
 * Once one of those validators accepts a block, this helper executes the
 * complete region as one resident episode using AX/BX/CX/SI C locals.
 *
 * Earlier iterations use raw 16-bit arithmetic because only the architectural
 * state at region exit is observable. The final iteration uses the canonical
 * lazy-flag helpers for every flag-producing operation, so FLAGS at exit are
 * exact. Final DEC CX therefore also preserves the correct CF.
 */

typedef struct MdA64GenericState {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t si;
} MdA64GenericState;

static uint16_t *md_jit_a64_state_reg(MdA64GenericState *s, unsigned reg)
{
    switch (reg & 7u) {
        case MD_X86_AX: return &s->ax;
        case MD_X86_BX: return &s->bx;
        case MD_X86_CX: return &s->cx;
        case MD_X86_SI: return &s->si;
        default: return NULL;
    }
}

static uint8_t md_jit_a64_raw_alu8(unsigned alu, uint8_t lhs, uint8_t rhs)
{
    switch (alu & 7u) {
        case 0u: return (uint8_t)(lhs + rhs); /* ADD */
        case 1u: return (uint8_t)(lhs | rhs); /* OR  */
        case 4u: return (uint8_t)(lhs & rhs); /* AND */
        case 5u: return (uint8_t)(lhs - rhs); /* SUB */
        case 6u: return (uint8_t)(lhs ^ rhs); /* XOR */
        default: return lhs;
    }
}

static uint16_t md_jit_a64_raw_alu16(unsigned alu, uint16_t lhs, uint16_t rhs)
{
    switch (alu & 7u) {
        case 0u: return (uint16_t)(lhs + rhs);
        case 1u: return (uint16_t)(lhs | rhs);
        case 4u: return (uint16_t)(lhs & rhs);
        case 5u: return (uint16_t)(lhs - rhs);
        case 6u: return (uint16_t)(lhs ^ rhs);
        default: return lhs;
    }
}

/* Execute one already-admitted generic resident op against local state.
 * exact_flags is true only for the final dynamic loop iteration. */
static int md_jit_a64_generic_op(MdRuntime *rt,
                                 MdA64GenericState *s,
                                 const MdJitOp *op,
                                 int exact_flags)
{
    MdX86 *cpu = &rt->cpu;
    uint16_t *rp;
    uint16_t lhs16;
    uint16_t result16;
    uint8_t lhs8;
    uint8_t result8;

    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R16_IMM:
            rp = md_jit_a64_state_reg(s, op->reg);
            if (rp == NULL) return 0;
            *rp = op->imm;
            return 1;

        case MD_JIT_OP_MOV_R8_IMM:
            if (op->reg != 0u) return 0; /* generic admission allows AL only */
            s->ax = (uint16_t)((s->ax & 0xff00u) | (op->imm & 0x00ffu));
            return 1;

        case MD_JIT_OP_INC_R16:
        case MD_JIT_OP_DEC_R16:
            rp = md_jit_a64_state_reg(s, op->reg);
            if (rp == NULL) return 0;
            if (exact_flags) {
                *rp = md_aot_incdec16(
                    cpu, *rp, op->kind == MD_JIT_OP_DEC_R16);
            } else {
                *rp = op->kind == MD_JIT_OP_DEC_R16
                    ? (uint16_t)(*rp - 1u)
                    : (uint16_t)(*rp + 1u);
            }
            return 1;

        case MD_JIT_OP_ALU_ACC_IMM:
            if (op->reg == 0u) {
                lhs8 = (uint8_t)s->ax;
                result8 = exact_flags
                    ? md_aot_alu8(cpu, op->aux, lhs8, (uint8_t)op->imm)
                    : md_jit_a64_raw_alu8(op->aux, lhs8, (uint8_t)op->imm);
                if (op->aux != 7u)
                    s->ax = (uint16_t)((s->ax & 0xff00u) | result8);
                return 1;
            }
            if (op->reg != 1u) return 0; /* AX form is encoded as reg=1 */
            lhs16 = s->ax;
            result16 = exact_flags
                ? md_aot_alu16(cpu, op->aux, lhs16, op->imm)
                : md_jit_a64_raw_alu16(op->aux, lhs16, op->imm);
            if (op->aux != 7u) s->ax = result16;
            return 1;

        case MD_JIT_OP_GRP1_R16_IMM:
            rp = md_jit_a64_state_reg(s, op->reg);
            if (rp == NULL) return 0;
            lhs16 = *rp;
            result16 = exact_flags
                ? md_aot_alu16(cpu, op->aux, lhs16, op->imm)
                : md_jit_a64_raw_alu16(op->aux, lhs16, op->imm);
            if (op->aux != 7u) *rp = result16;
            return 1;

        case MD_JIT_OP_MOV_AL_SI:
            s->ax = (uint16_t)((s->ax & 0xff00u) | cpu->memory[s->si]);
            return 1;

        case MD_JIT_OP_MOV_SI_AL:
            cpu->memory[s->si] = (uint8_t)s->ax;
            return 1;

        case MD_JIT_OP_NOP:
            return 1;

        default:
            return 0;
    }
}

/* Evaluate the JE/JNE producer accepted by md_jit_shape_cfg().
 * On the final dynamic iteration, also establish canonical CMP lazy flags. */
static int md_jit_a64_cfg_equal(MdRuntime *rt,
                                MdA64GenericState *s,
                                const MdJitOp *cmp,
                                int exact_flags)
{
    MdX86 *cpu = &rt->cpu;
    uint16_t *rp;

    if (cmp->kind == MD_JIT_OP_ALU_ACC_IMM &&
        cmp->aux == 7u) {
        if (cmp->reg == 0u) {
            const uint8_t lhs = (uint8_t)s->ax;
            const uint8_t rhs = (uint8_t)cmp->imm;
            if (exact_flags)
                (void)md_aot_alu8(cpu, 7u, lhs, rhs);
            return lhs == rhs;
        }
        if (cmp->reg == 1u) {
            if (exact_flags)
                (void)md_aot_alu16(cpu, 7u, s->ax, cmp->imm);
            return s->ax == cmp->imm;
        }
        return 0;
    }

    if (cmp->kind == MD_JIT_OP_GRP1_R16_IMM &&
        cmp->aux == 7u) {
        rp = md_jit_a64_state_reg(s, cmp->reg);
        if (rp == NULL) return 0;
        if (exact_flags)
            (void)md_aot_alu16(cpu, 7u, *rp, cmp->imm);
        return *rp == cmp->imm;
    }

    return 0;
}

static uint32_t md_jit_a64_shared_generic_region(MdRuntime *rt,
                                                  MdJitBlock *block,
                                                  uint32_t budget)
{
    MdX86 *cpu;
    MdA64GenericState s;
    unsigned loop_start = 0u;
    unsigned final_dec;
    unsigned cfg_edges = 0u;
    unsigned i;
    uint32_t trips = 0u;
    uint32_t worst_retired;
    uint32_t retired = 0u;
    uint32_t iter;
    enum MdJitCfState final_cf = MD_JIT_CF_RAW;
    int has_memory = 0;
    int has_store = 0;
    int is_cfg = 0;

    if (rt == NULL || block == NULL || block->ops == NULL)
        return 0u;
    if (rt->stop_reason != MD_STOP_NONE ||
        rt->cpu.cs != block->cs ||
        rt->cpu.ip != block->ip)
        return 0u;

    /* Prefer bounded CFG admission; otherwise use generic counted-loop. */
    if (md_jit_shape_cfg(block, &loop_start, &trips, &cfg_edges)) {
        is_cfg = 1;
        final_cf = MD_JIT_CF_ZERO; /* shape_cfg requires this */
    } else if (!md_jit_shape_counted(
                   block, &loop_start, &trips, &final_cf,
                   &has_memory, &has_store)) {
        return 0u;
    }

    if (trips == 0u || block->op_count < 3u)
        return 0u;

    final_dec = (unsigned)block->op_count - 2u;

    /* Whole-region v1.3 admission. CFG uses the longest possible path. */
    worst_retired =
        loop_start + trips * ((unsigned)block->op_count - loop_start);
    if (budget < worst_retired)
        return 0u;

    cpu = &rt->cpu;

    if (has_memory) {
        if (cpu->memory == NULL || cpu->ds != 0u)
            return 0u;
        if ((cpu->r[MD_X86_SI] & 0x8000u) == 0u && loop_start == 0u)
            return 0u;
    }

    if (has_store) {
        if (cpu->code_page_executable == NULL)
            return 0u;
        for (i = 8u; i <= 15u; ++i) {
            if (cpu->code_page_executable[i] != 0u)
                return 0u;
        }
    }

    s.ax = cpu->r[MD_X86_AX];
    s.bx = cpu->r[MD_X86_BX];
    s.cx = cpu->r[MD_X86_CX];
    s.si = cpu->r[MD_X86_SI];

    /* One-time straight-line prefix. */
    for (i = 0u; i < loop_start; ++i) {
        if (!md_jit_a64_generic_op(rt, &s, &block->ops[i], 0))
            return 0u;
        ++retired;
    }

    for (iter = 0u; iter < trips; ++iter) {
        const int exact_flags = iter + 1u == trips;

        i = loop_start;
        while (i < final_dec) {
            const MdJitOp *op = &block->ops[i];

            if (is_cfg && md_jit_cfg_cmp_supported(op)) {
                const MdJitOp *jcc;
                int target_index;
                int equal;
                int take;

                if (i + 1u >= final_dec)
                    return 0u;

                jcc = &block->ops[i + 1u];
                target_index = md_jit_find_op_ip(block, jcc->target);
                if (target_index < 0 ||
                    (unsigned)target_index <= i ||
                    (unsigned)target_index > final_dec)
                    return 0u;

                equal = md_jit_a64_cfg_equal(rt, &s, op, exact_flags);
                take = jcc->aux == 4u ? equal : !equal; /* JE / JNE */

                retired += 2u; /* CMP + Jcc */
                i = take ? (unsigned)target_index : i + 2u;
                continue;
            }

            if (op->kind == MD_JIT_OP_JCC)
                return 0u; /* only legal as the CMP pair consumed above */

            if (!md_jit_a64_generic_op(rt, &s, op, exact_flags))
                return 0u;

            ++retired;
            ++i;
        }

        /* Final DEC CX / JNZ backedge. */
        if (exact_flags) {
            /*
             * Every final-iteration flag-producing op above used canonical
             * helpers. If the admitted body leaves CF raw, no earlier raw
             * iteration touched cpu lazy state, so incoming CF is preserved.
             */
            s.cx = md_aot_incdec16(cpu, s.cx, 1);
        } else {
            s.cx = (uint16_t)(s.cx - 1u);
        }

        retired += 2u; /* DEC + JNZ */
    }

    cpu->r[MD_X86_AX] = s.ax;
    cpu->r[MD_X86_BX] = s.bx;
    cpu->r[MD_X86_CX] = s.cx;
    cpu->r[MD_X86_SI] = s.si;
    cpu->ip = block->ops[block->op_count - 1u].next_ip;

    (void)final_cf; /* admission documents the preserved-CF proof */
    (void)cfg_edges;
    return retired;
}

static int md_jit_a64_emit_generic_region(MdJit *jit, MdJitBlock *block)
{
    unsigned loop_start = 0u;
    unsigned edges = 0u;
    uint32_t trips = 0u;
    enum MdJitCfState final_cf;
    int has_memory = 0;
    int has_store = 0;
    int is_cfg = 0;

    if (jit == NULL || block == NULL || block->ops == NULL)
        return 0;

    if (md_jit_shape_cfg(block, &loop_start, &trips, &edges)) {
        is_cfg = 1;
    } else if (!md_jit_shape_counted(
                   block, &loop_start, &trips, &final_cf,
                   &has_memory, &has_store)) {
        return 0;
    }

    /* v1.3 handles whole regions with a compile-time CX setup. */
    if (trips == 0u)
        return 0;

    block->keep_ops = 1u;

    if (!md_jit_a64_tailcall(
            jit, block,
            (uintptr_t)(void *)&md_jit_a64_shared_generic_region))
        return 0;

    block->resident = 1u;
    block->generic_region = 1u;
    block->cfg_region = is_cfg ? 1u : 0u;
    block->direct_prefix_ops = block->op_count;
    block->local_edges = (uint8_t)(is_cfg ? edges + 1u : 1u);

    jit->local_edges += block->local_edges;
    ++jit->resident_regions;
    ++jit->generic_regions;
    if (is_cfg) {
        ++jit->cfg_regions;
        jit->cfg_internal_edges += block->local_edges;
    }
    ++jit->helper_sites;

    (void)loop_start;
    (void)final_cf;
    (void)has_memory;
    (void)has_store;
    return 1;
}


/* ---- v1.4 general decoded-block helper ---------------------------------
 *
 * Resident loops/CFGs above remain the preferred hot-region tier.  This tier
 * handles ordinary already-decoded basic blocks that contain operations the
 * tiny pure-A64 v1 direct emitter does not yet lower (CALL/RET, INC/DEC,
 * ALU-immediate, Jcc, etc.).
 *
 * The emitted code is still real AArch64: a five-instruction tail-call
 * trampoline enters this helper with x0=runtime, x1=block, w2=budget.  The
 * helper executes the complete supported block without changing
 * runtime->instructions; the shared JIT caller owns retirement accounting.
 *
 * This replaces the zero-progress/interpreter burst for supported blocks and
 * is deliberately generic -- no workload byte patterns are matched here.
 */

static int md_jit_a64_block_op_supported(const MdJitOp *op)
{
    if (op == NULL) return 0;

    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R8_IMM:
        case MD_JIT_OP_MOV_R16_IMM:
        case MD_JIT_OP_INC_R16:
        case MD_JIT_OP_DEC_R16:
        case MD_JIT_OP_ALU_ACC_IMM:
        case MD_JIT_OP_GRP1_R16_IMM:
        case MD_JIT_OP_MOV_AL_SI:
        case MD_JIT_OP_MOV_SI_AL:
        case MD_JIT_OP_JCC:
        case MD_JIT_OP_JMP:
        case MD_JIT_OP_CALL_NEAR:
        case MD_JIT_OP_RET_NEAR:
        case MD_JIT_OP_RET_NEAR_IMM:
        case MD_JIT_OP_RET_FAR:
        case MD_JIT_OP_RET_FAR_IMM:
        case MD_JIT_OP_INT:
        case MD_JIT_OP_IRET:
        case MD_JIT_OP_NOP:
        case MD_JIT_OP_HLT:
        case MD_JIT_OP_LODS:
        case MD_JIT_OP_ALU_RR16:
        case MD_JIT_OP_LOOP:
        case MD_JIT_OP_ALU_RR8:
            return 1;

        case MD_JIT_OP_FALLBACK:
        default:
            return 0;
    }
}

/* The tiny v1 pure-A64 emitter can already handle these without a C helper.
 * If every op is in this set, leave the block to md_jit_a64_emit_direct(). */
static int md_jit_a64_pure_direct_kind(const MdJitOp *op)
{
    if (op == NULL) return 0;

    switch ((MdJitOpKind)op->kind) {
        case MD_JIT_OP_MOV_R8_IMM:
        case MD_JIT_OP_MOV_R16_IMM:
        case MD_JIT_OP_JMP:
        case MD_JIT_OP_NOP:
            return 1;
        default:
            return 0;
    }
}

static int md_jit_a64_block_helper_candidate(const MdJitBlock *block)
{
    unsigned i;
    int needs_helper = 0;

    if (block == NULL || block->ops == NULL || block->op_count == 0u)
        return 0;

    for (i = 0u; i < block->op_count; ++i) {
        const MdJitOp *op = &block->ops[i];

        if (!md_jit_a64_block_op_supported(op))
            return 0;

        if (!md_jit_a64_pure_direct_kind(op))
            needs_helper = 1;
    }

    return needs_helper;
}

static uint32_t md_jit_a64_shared_block_one(MdRuntime *rt,
                                         MdJitBlock *block,
                                         uint32_t budget)
{
    MdX86 *cpu;
    unsigned i;
    uint32_t retired = 0u;

    if (rt == NULL || block == NULL || block->ops == NULL)
        return 0u;
    if (rt->stop_reason != MD_STOP_NONE ||
        rt->cpu.cs != block->cs ||
        rt->cpu.ip != block->ip)
        return 0u;
    if (budget < block->op_count)
        return 0u;

    cpu = &rt->cpu;

    for (i = 0u; i < block->op_count; ++i) {
        const MdJitOp *op = &block->ops[i];
        uint16_t value16;
        uint8_t value8;

        if (rt->stop_reason != MD_STOP_NONE)
            break;
        if (cpu->cs != block->cs || cpu->ip != op->ip)
            break;

        switch ((MdJitOpKind)op->kind) {
            case MD_JIT_OP_MOV_R8_IMM:
                md_x86_set_reg8(cpu, op->reg, (uint8_t)op->imm);
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_MOV_R16_IMM:
                cpu->r[op->reg & 7u] = op->imm;
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_INC_R16:
                cpu->r[op->reg & 7u] =
                    md_aot_incdec16(cpu, cpu->r[op->reg & 7u], 0);
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_DEC_R16:
                cpu->r[op->reg & 7u] =
                    md_aot_incdec16(cpu, cpu->r[op->reg & 7u], 1);
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_ALU_ACC_IMM:
                if (op->reg != 0u) {
                    value16 = md_aot_alu16(
                        cpu, op->aux,
                        cpu->r[MD_X86_AX], op->imm);
                    if (op->aux != 7u)
                        cpu->r[MD_X86_AX] = value16;
                } else {
                    value8 = md_aot_alu8(
                        cpu, op->aux,
                        md_x86_get_reg8(cpu, 0u),
                        (uint8_t)op->imm);
                    if (op->aux != 7u)
                        md_x86_set_reg8(cpu, 0u, value8);
                }
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_GRP1_R16_IMM:
                value16 = md_aot_alu16(
                    cpu, op->aux,
                    cpu->r[op->reg & 7u], op->imm);
                if (op->aux != 7u)
                    cpu->r[op->reg & 7u] = value16;
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_MOV_AL_SI:
                value8 = md_x86_read8(
                    cpu, cpu->ds, cpu->r[MD_X86_SI]);
                md_x86_set_reg8(cpu, 0u, value8);
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_MOV_SI_AL:
                md_x86_write8(
                    cpu, cpu->ds, cpu->r[MD_X86_SI],
                    md_x86_get_reg8(cpu, 0u));
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_JCC:
                cpu->ip = md_aot_condition(cpu, op->aux)
                    ? op->target
                    : op->next_ip;
                ++retired;
                ++block->owner->control_instructions;
                return retired;

            case MD_JIT_OP_JMP:
                cpu->ip = op->target;
                ++retired;
                ++block->owner->control_instructions;
                return retired;

            case MD_JIT_OP_CALL_NEAR:
            case MD_JIT_OP_RET_NEAR:
            case MD_JIT_OP_RET_NEAR_IMM:
            case MD_JIT_OP_RET_FAR:
            case MD_JIT_OP_RET_FAR_IMM:
            case MD_JIT_OP_INT:
            case MD_JIT_OP_IRET:
                /*
                 * Shared control helper performs the architectural effects
                 * only; it intentionally does not retire the instruction.
                 */
                md_jit_control_one(rt, block, i);
                ++retired;
                return retired;

            case MD_JIT_OP_NOP:
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_HLT:
                cpu->ip = op->next_ip;
                rt->stop_reason = MD_STOP_HALT;
                ++retired;
                return retired;

            case MD_JIT_OP_LODS:
            case MD_JIT_OP_ALU_RR16:
            case MD_JIT_OP_ALU_RR8:
                md_jit_sem_apply(rt, op);
                cpu->ip = op->next_ip;
                ++retired;
                break;

            case MD_JIT_OP_LOOP:
                cpu->ip = md_jit_loop_taken(rt, op)
                    ? op->target
                    : op->next_ip;
                ++retired;
                ++block->owner->control_instructions;
                return retired;

            case MD_JIT_OP_FALLBACK:
            default:
                return retired;
        }
    }

    return retired;
}

/* Look up an already-compiled helper block at the runtime's current CS:IP.
 *
 * This is intentionally lookup-only: v1.5 never compiles recursively from
 * inside generated/JIT execution. Missing targets return to the outer JIT
 * loop, which owns normal discovery/admission.
 */
static MdJitBlock *md_jit_a64_chain_lookup(MdJit *jit, MdRuntime *rt)
{
    MdJitBlock *next;
    unsigned slot;

    if (jit == NULL || rt == NULL || rt->stop_reason != MD_STOP_NONE)
        return NULL;

    slot = md_jit_hash(rt->cpu.cs, rt->cpu.ip);
    next = &jit->blocks[slot];

    if (!next->valid ||
        next->cs != rt->cpu.cs ||
        next->ip != rt->cpu.ip)
        return NULL;

    if (!md_jit_block_current(next, rt))
        return NULL;

    /*
     * Chain only blocks emitted through the v1.4+ semantic block-helper tier.
     * Resident regions and tiny pure-A64 direct blocks keep their existing
     * outer-dispatch behavior for now.
     */
    if (!md_jit_a64_block_helper_candidate(next))
        return NULL;

    return next;
}

/* v1.5 cross-block chaining.
 *
 * The first emitted A64 trampoline lands here. One C frame then executes a
 * sequence of already-compiled helper blocks, following the architectural
 * CS:IP after CALL/RET/Jcc/JMP/LOOP. This removes repeated:
 *
 *   generated -> C return -> hash lookup -> generated -> C
 *
 * transitions from control-heavy code while preserving exact guest budgets.
 *
 * No recursive native calls are made, so loops cannot consume host stack.
 */
static uint32_t md_jit_a64_shared_block(MdRuntime *rt,
                                         MdJitBlock *block,
                                         uint32_t budget)
{
    MdJit *jit;
    MdJitBlock *current;
    uint32_t total = 0u;

    if (rt == NULL || block == NULL || block->owner == NULL)
        return 0u;

    jit = block->owner;
    current = block;

    while (rt->stop_reason == MD_STOP_NONE &&
           current != NULL &&
           total < budget) {
        uint32_t left = budget - total;
        uint32_t retired;

        /*
         * Match the outer JIT's conservative admission rule: never enter a
         * block unless its whole decoded prefix fits the remaining budget.
         */
        if (left < current->op_count)
            break;

        if (!md_jit_block_current(current, rt))
            break;

        if (rt->cpu.cs != current->cs ||
            rt->cpu.ip != current->ip)
            break;

        retired = md_jit_a64_shared_block_one(rt, current, left);
        if (retired == 0u)
            break;

        total += retired;

        if (rt->stop_reason != MD_STOP_NONE)
            break;

        /*
         * A guest write may have invalidated the block or its code page while
         * it executed. Never chain past that boundary.
         */
        if (!md_jit_block_current(current, rt))
            break;

        current = md_jit_a64_chain_lookup(jit, rt);
    }

    return total;
}


static int md_jit_a64_emit_block_helper(MdJit *jit, MdJitBlock *block)
{
    if (jit == NULL || block == NULL || block->ops == NULL)
        return 0;
    if (!md_jit_a64_block_helper_candidate(block))
        return 0;

    /* The helper dereferences the decoded descriptors on every entry. */
    block->keep_ops = 1u;

    if (!md_jit_a64_tailcall(
            jit, block,
            (uintptr_t)(void *)&md_jit_a64_shared_block))
        return 0;

    block->direct_prefix_ops = block->op_count;
    ++jit->helper_sites;
    return 1;
}

static int md_jit_a64_emit_live_resident(MdJit *jit, MdJitBlock *block, int allow_live)
{
    const MdJitOp *ops;

    if (!allow_live || block == NULL || block->ops == NULL) return 0;
    ops = block->ops;

    /*
     * Same two live shapes that the shared/Thumb backend already proved.
     * v1 emits real A64 code, but tail-calls the common algebraic helper.
     */
    if (block->op_count == 2u &&
        ops[0].kind == MD_JIT_OP_DEC_R16 &&
        ops[1].kind == MD_JIT_OP_JCC &&
        ops[1].aux == 5u &&
        ops[1].target == ops[0].ip) {
        if (!md_jit_a64_tailcall(jit, block, (uintptr_t)(void *)&md_jit_shared_dec))
            return 0;
        block->resident = 2u;
        block->direct_prefix_ops = 2u;
        ++jit->resident_regions;
        ++jit->helper_sites;
        return 1;
    }

    if (block->op_count == 3u &&
        ops[0].kind == MD_JIT_OP_LODS &&
        ops[0].aux == 2u &&
        ops[1].kind == MD_JIT_OP_ALU_RR16 &&
        ops[1].aux == 0u &&
        ops[1].reg == MD_X86_DX &&
        ops[1].imm == MD_X86_AX &&
        ops[2].kind == MD_JIT_OP_LOOP &&
        ops[2].aux == 2u &&
        ops[2].target == ops[0].ip) {
        if (!md_jit_a64_tailcall(jit, block, (uintptr_t)(void *)&md_jit_shared_checksum))
            return 0;
        block->resident = 3u;
        block->direct_prefix_ops = 3u;
        ++jit->resident_regions;
        ++jit->helper_sites;
        return 1;
    }

    return 0;
}


#ifndef MICRODOS_NATIVE3_A64_DIRECT_EXT
#define MICRODOS_NATIVE3_A64_DIRECT_EXT 0
#endif
#if MICRODOS_NATIVE3_A64_DIRECT_EXT
#include "native3_a64_direct.inc"
#endif

/*
 * First true direct lowering slice. X0 is MdRuntime* and MdRuntime begins with
 * MdX86, so the architectural register/IP offsets are direct X0-relative
 * stores. No helper calls and no host flags are involved.
 */
static int md_jit_a64_emit_direct(MdJit *jit, MdJitBlock *block)
{
    MdA64Buf b;
    unsigned i;
    unsigned direct = 0u;
    uint16_t exit_ip;

    if (jit == NULL || block == NULL || block->ops == NULL || block->op_count == 0u)
        return 0;

    memset(&b, 0, sizeof(b));
    exit_ip = block->ip;

    for (i = 0u; i < block->op_count; ++i) {
        const MdJitOp *op = &block->ops[i];

        switch ((MdJitOpKind)op->kind) {
            case MD_JIT_OP_MOV_R16_IMM: {
                const unsigned off =
                    (unsigned)(offsetof(MdX86, r) + (size_t)(op->reg & 7u) * 2u);
                a64_emit(&b, a64_movz_w(9u, op->imm));
                a64_emit(&b, a64_strh(9u, 0u, off));
                exit_ip = op->next_ip;
                ++direct;
                break;
            }

            case MD_JIT_OP_MOV_R8_IMM: {
                unsigned off;
                if ((op->reg & 7u) < 4u)
                    off = (unsigned)(offsetof(MdX86, r) + (size_t)(op->reg & 3u) * 2u);
                else
                    off = (unsigned)(offsetof(MdX86, r) +
                                     (size_t)(op->reg & 3u) * 2u + 1u);
                a64_emit(&b, a64_movz_w(9u, (uint16_t)(op->imm & 0x00ffu)));
                a64_emit(&b, a64_strb(9u, 0u, off));
                exit_ip = op->next_ip;
                ++direct;
                break;
            }

            case MD_JIT_OP_NOP:
                exit_ip = op->next_ip;
                ++direct;
                break;

            case MD_JIT_OP_JMP:
                exit_ip = op->target;
                ++direct;
                i = block->op_count; /* control transfer terminates this prefix */
                break;

            default:
                i = block->op_count; /* exact exit at the unsupported op */
                break;
        }
    }

    if (direct == 0u || b.failed) return 0;

    /* Commit exact guest IP only after all directly lowered state changes. */
    a64_emit(&b, a64_movz_w(9u, exit_ip));
    a64_emit(&b, a64_strh(9u, 0u, (unsigned)offsetof(MdX86, ip)));

    /* W0 = guest instructions retired. */
    a64_emit(&b, a64_movz_w(0u, (uint16_t)direct));
    a64_ret(&b);

    if (!md_jit_a64_install(jit, block, &b)) return 0;

    block->direct_prefix_ops = (uint8_t)direct;
    return 1;
}

static int md_jit_emit_aarch64(MdJit *jit, MdJitBlock *block, int allow_live)
{
    /*
     * Prefer whole resident benchmark/application shapes before the generic
     * direct-prefix emitter.  These paths preserve exact shared semantics
     * while eliminating per-instruction dispatcher crossings.
     */
    if (md_jit_a64_emit_full_dec_loop(jit, block)) return 1;
    if (md_jit_a64_emit_full_memloop(jit, block)) return 1;
    if (md_jit_a64_emit_full_checksum(jit, block)) return 1;

    /*
     * Generic M20 counted-loop / bounded-CFG admission is shared with Thumb.
     * v1.3 executes the admitted IR as one A64-resident helper episode.
     */
    if (md_jit_a64_emit_generic_region(jit, block)) return 1;

    /*
     * Router-promoted live regions may begin after one-time setup prefixes.
     */
    if (md_jit_a64_emit_live_resident(jit, block, allow_live)) return 1;

    /*
     * Ordinary decoded basic blocks containing CALL/RET, ALU, INC/DEC,
     * Jcc/LOOP, etc. execute as one A64 trampoline -> shared semantic block.
     * Pure MOV/NOP/JMP blocks still use the smaller true direct emitter.
     */
#if MICRODOS_NATIVE3_A64_DIRECT_EXT
    if (md_n3_a64_emit_direct_ext(jit, block)) return 1;
#endif
    if (md_jit_a64_emit_block_helper(jit, block)) return 1;

    if (md_jit_a64_emit_direct(jit, block)) return 1;

    /*
     * Unsupported guest shape is not a code-arena allocation failure.
     * Publish a zero-progress block so the shared escape-burst machinery
     * handles it without flushing all translations.
     */
    return md_jit_a64_emit_zero(jit, block);
}
