/*
 * microDOS Thumb-2 JIT backend
 *
 * Included by jit_core.c; do not compile this file directly.
 *
 * The backend remains in the same C translation unit as the shared JIT core
 * during the first refactor stage. That makes this split mechanical:
 * no public ABI, struct layout, static-helper linkage or intended behavior
 * changes.
 */

/* Generated Thumb functions use low registers:
 *   r4 = MdRuntime/MdX86 *, r5 = MdJitBlock *, r6 = retired, r7 = budget
 *   r0-r3 = temporaries.
 * MdRuntime intentionally begins with MdX86, so runtime == cpu here. */

static void md_jit_sync_code(void)
{
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("dsb sy\n\tisb sy" ::: "memory");
#endif
}

/* Thumb-generated helper call targets. */
static void md_jit_sem_one(MdRuntime *runtime, MdJitBlock *block, unsigned index)
{
    md_jit_sem_apply(runtime, &block->ops[index]);
}

static unsigned md_jit_loop_one(MdRuntime *runtime, MdJitBlock *block, unsigned index)
{
    return (unsigned)md_jit_loop_taken(runtime, &block->ops[index]);
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


/* Tracks the x86 CF that DEC CX must preserve at the region exit.  Arithmetic
 * ADD/SUB make it dynamic/unknown; a later logic operation resets it to zero.
 * INC/DEC/MOV/memory operations preserve it. */

/* Proves that every [SI] access is in 8000h..FFFFh.  The prefix must establish
 * SI's high bit, every access must occur while that fact is known, and the
 * backedge must restore the fact for the next iteration. */

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
/* Live loop-head admission; tmp/dst are scratch, budget stays in r2. */
static void th_live_loop_count(MdThumbBuf *b, unsigned dst, unsigned tmp, unsigned stride)
{
    const unsigned cxoff = (unsigned)(offsetof(MdX86, r) + MD_X86_CX * 2u);
    size_t nonzero;
    th16(b, th_ldrh(dst, 4u, cxoff));
    th16(b, th_cmp_imm(dst, 0u));
    nonzero = th_emit_bcond_placeholder(b, 1u);
    th_load_imm32(b, dst, 65536u);
    (void)th_patch_bcond(b, nonzero, 1u, b->at);
    th16(b, th_movs(tmp, stride));
    th16(b, (uint16_t)(0x4340u | (tmp << 3) | dst)); /* MULS dst,tmp */
}


static int md_jit_emit_resident_cfg(MdJit *jit, MdJitBlock *block, int allow_live)
{
    MdThumbBuf b;
    size_t op_native[MD_JIT_MAX_OPS];
    size_t patch_at[MD_JIT_MAX_OPS];
    unsigned patch_target[MD_JIT_MAX_OPS];
    unsigned patch_cond[MD_JIT_MAX_OPS];
    unsigned patch_count = 0u;
    const unsigned n = block->op_count;
    unsigned final_dec, loop_start, i, forward_edges = 0u;
    uint32_t trips = 0u, worst_retired;
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    size_t blo_budget, loop_at, bne_loop, fail_at;

    if (!md_jit_shape_cfg(block, &loop_start, &trips, &forward_edges)) return 0;
    if (trips == 0u && !allow_live) return 0;
    final_dec = n - 2u;
    if (!th_offset_ok_h(foff) || !th_offset_ok_b(coff)) return 0;

    worst_retired = loop_start + trips * (n - loop_start);
    if (trips != 0u && worst_retired == 0u) return 0;

    memset(&b, 0, sizeof(b));
    memset(op_native, 0, sizeof(op_native));
    th16(&b, 0xB5F8u);                    /* push {r3,r4,r5,r6,r7,lr} */
    th16(&b, th_mov(4u, 0u));             /* r4 runtime */
    th16(&b, th_mov(5u, 1u));             /* r5 block; free after admission */
    if (trips != 0u) th_load_imm32(&b, 0u, worst_retired);
    else th_live_loop_count(&b, 0u, 3u, n);
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

static int md_jit_emit_resident_counted(MdJit *jit, MdJitBlock *block, int allow_live)
{
    MdThumbBuf b;
    const unsigned n = block->op_count;
    unsigned final_dec, loop_start, i;
    uint32_t trips = 0u, retired;
    int has_memory = 0, has_store = 0;
    enum MdJitCfState final_cf;
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned xoff = (unsigned)offsetof(MdX86, code_page_executable);
    const unsigned memoff = (unsigned)offsetof(MdX86, memory);
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned coff = (unsigned)offsetof(MdX86, lazy_carry);
    const unsigned moff = (unsigned)offsetof(MdJitBlock, materialize);
    size_t blo_budget, bne_ds = 0u, bne_si = 0u, bne_exec = 0u, bne_loop, loop_at, fail_at;

    if (!md_jit_shape_counted(block, &loop_start, &trips, &final_cf, &has_memory, &has_store)) return 0;
    if (trips == 0u && !allow_live) return 0;
    final_dec = n - 2u;
    retired = loop_start + trips * (n - loop_start);
    if ((trips != 0u && retired == 0u) || !th_offset_ok_h(foff) || !th_offset_ok_b(coff) || !th_offset_ok_w(moff)) return 0;
    if (has_memory && (!th_offset_ok_h(dsoff) || !th_offset_ok_w(memoff))) return 0;
    if (has_store && !th_offset_ok_w(xoff)) return 0;

    memset(&b, 0, sizeof(b));
    th16(&b, 0xB5F8u);                    /* 24-byte AAPCS-aligned frame */
    th16(&b, th_mov(4u, 0u));             /* r4 runtime */
    th16(&b, th_mov(5u, 1u));             /* r5 block during entry guards */

    if (trips != 0u) th_load_imm32(&b, 3u, retired);
    else th_live_loop_count(&b, 3u, 0u, n);
    th16(&b, th_cmp_reg(2u, 3u));          /* r2 is budget argument */
    blo_budget = th_emit_bcond_placeholder(&b, 3u);

    if (has_memory) {
        th16(&b, th_ldrh(0u, 4u, dsoff));
        th16(&b, th_cmp_imm(0u, 0u));
        bne_ds = th_emit_bcond_placeholder(&b, 1u);
        if (trips == 0u) {
            const unsigned sioff = (unsigned)(offsetof(MdX86, r) + MD_X86_SI * 2u);
            th16(&b, th_ldrh(0u, 4u, sioff));
            th16(&b, th_lsr_imm(0u, 0u, 15u));
            th16(&b, th_cmp_imm(0u, 0u));
            /* Share the DS failure target, but retain a separate patch. */
            bne_si = th_emit_bcond_placeholder(&b, 0u);
        }
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

    if (trips == 0u) th_live_loop_count(&b, 3u, 2u, n);

    /* Load the resident architectural set once.  Unused registers cost a few
       one-time loads/stores but let one emitter handle many loop shapes. */
    th_resident_load_reg(&b, MD_X86_AX);
    th_resident_load_reg(&b, MD_X86_BX);
    th_resident_load_reg(&b, MD_X86_CX);
    th_resident_load_reg(&b, MD_X86_SI);
    if (has_memory) th16(&b, th_ldr(5u, 4u, memoff));
    if (trips != 0u) th_load_imm32(&b, 3u, retired); /* helper may clobber r3 */

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
    if (has_memory && trips == 0u && !th_patch_bcond(&b, bne_si, 0u, fail_at)) return 0;
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
    const unsigned dsoff = (unsigned)offsetof(MdX86, ds);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned foff = (unsigned)offsetof(MdX86, flags_raw);
    const unsigned opoff = (unsigned)offsetof(MdX86, lazy_op);
    unsigned loop_start;
    size_t bmi_back;

    if (!md_jit_shape_lodsloop(block, &loop_start)) return 0;
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


static int md_jit_emit_resident(MdJit *jit, MdJitBlock *block, int allow_live)
{
    const MdJitOp *ops = block->ops;
    if (allow_live && block->op_count == 2u && ops[0].kind == MD_JIT_OP_DEC_R16 &&
        ops[1].kind == MD_JIT_OP_JCC && ops[1].aux == 5u && ops[1].target == ops[0].ip) {
        block->native = md_jit_shared_dec;
        block->resident = 2u;
        block->direct_prefix_ops = 2u;
        ++jit->resident_regions;
        return 1;
    }
    if (allow_live && block->op_count == 3u && ops[0].kind == MD_JIT_OP_LODS && ops[0].aux == 2u &&
        ops[1].kind == MD_JIT_OP_ALU_RR16 && ops[1].aux == 0u &&
        ops[1].reg == MD_X86_DX && ops[1].imm == MD_X86_AX &&
        ops[2].kind == MD_JIT_OP_LOOP && ops[2].aux == 2u && ops[2].target == ops[0].ip) {
        block->native = md_jit_shared_checksum;
        block->resident = 3u;
        block->direct_prefix_ops = 3u;
        ++jit->resident_regions;
        return 1;
    }
    if (md_jit_emit_resident_lodsloop(jit, block)) return 1;   /* M20.3 */
    if (md_jit_is_resident_loop(block)) return md_jit_emit_resident_loop(jit, block);
    if (md_jit_is_resident_memloop(block)) return md_jit_emit_resident_memloop(jit, block);
    if (md_jit_emit_resident_cfg(jit, block, allow_live)) return 1;
    if (md_jit_emit_resident_counted(jit, block, allow_live)) return 1;
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


static int md_jit_emit_thumb(MdJit *jit, MdJitBlock *block, int allow_live)
{
    MdThumbBuf b;

    /* M19.2: recognize loop-shaped regions before the conservative per-op
       emitter. A failed/unsafe specialization simply falls through to M19.1. */
    if (md_jit_emit_resident(jit, block, allow_live)) return 1;
    size_t op_native[MD_JIT_MAX_OPS];
    unsigned i;
    unsigned direct = 0u;
    enum MdJitCfState cf = MD_JIT_CF_RAW;
    int needs_materialize = 0;
    memset(&b, 0, sizeof(b));
    memset(op_native, 0, sizeof(op_native));

    direct = md_jit_direct_prefix(block, &needs_materialize);
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
                th16(&b, th_add_imm(6u, 1u));
                th_store_ip(&b, op->next_ip);
                th16(&b, th_movs(0u, MD_STOP_HALT));
                th_load_imm32(&b, 1u, soff);
                th16(&b, th_add_reg(1u, 4u, 1u));
                th16(&b, 0x6008u); /* str r0,[r1,#0], any runtime layout */
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

