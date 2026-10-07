param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"

function Read-Normalized([string]$Path) {
    return ([IO.File]::ReadAllText($Path) -replace "`r`n", "`n")
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $enc = [System.Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($Path, $Text, $enc)
}

function Replace-Once(
    [string]$Text,
    [string]$Old,
    [string]$New,
    [string]$Label
) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) {
        throw "anchor not found: $Label"
    }

    $second = $Text.IndexOf(
        $Old,
        $first + $Old.Length,
        [StringComparison]::Ordinal
    )

    if ($second -ge 0) {
        throw "anchor is not unique: $Label"
    }

    return $Text.Substring(0, $first) +
        $New +
        $Text.Substring($first + $Old.Length)
}

$Nv2   = Join-Path $Repo "src\runtime\native_v2.c"
$Test  = Join-Path $Repo "tests\test_native_v2.c"
$Bench = Join-Path $Repo "pico\microdos_native3_bench.c"

foreach ($p in @($Nv2, $Test, $Bench)) {
    if (-not (Test-Path $p)) {
        throw "missing file: $p"
    }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backup = Join-Path $Repo "native3_leafcall_backup\$stamp"
New-Item -ItemType Directory -Force -Path $backup | Out-Null

Copy-Item $Nv2   (Join-Path $backup "native_v2.c") -Force
Copy-Item $Test  (Join-Path $backup "test_native_v2.c") -Force
Copy-Item $Bench (Join-Path $backup "microdos_native3_bench.c") -Force

Write-Host "backup: $backup"

# ===========================================================================
# src/runtime/native_v2.c
# ===========================================================================

$src = Read-Normalized $Nv2

# A RET in the bounded leaf graph cannot observe a different return word:
# the compiler admits no memory operation inside the leaf, and the runtime
# proves the two-byte CALL stack window is safe and untracked. Therefore the
# native fast path may perform the architectural SP pop without consuming
# r11, which is holding the guest CF that DEC must preserve.
$old = @'
static void md_nv2_call_ret_pop(MdThumbBuf *b)
{
    md_nv2_call_stack_addr(b);
    th_ldrh_w_reg(b, 11u, 9u, 12u);
    th_add_w_imm(b, 4u, 4u, 2u);
    th16(b, th_uxth(4u, 4u));
}

'@

$new = @'
static void md_nv2_call_ret_pop(MdThumbBuf *b)
{
    md_nv2_call_stack_addr(b);
    th_ldrh_w_reg(b, 11u, 9u, 12u);
    th_add_w_imm(b, 4u, 4u, 2u);
    th16(b, th_uxth(4u, 4u));
}

/*
 * Phase 3Q leaf-call graph RET.
 *
 * The leaf class admitted below contains no guest memory operation between
 * CALL and RET. The runtime separately proves the exact two-byte SS:SP window
 * is non-wrapping, disjoint from every compiled code span and untracked.
 * Therefore the return word at [SS:SP] is exactly the word the immediately
 * preceding CALL stored. We only need the architectural SP effect here.
 *
 * Do not use r11 as scratch: it carries the leaf's guest CF into the terminal
 * DEC, which preserves CF.
 */
static void md_nv2_leaf_ret_drop(MdThumbBuf *b)
{
    th_add_w_imm(b, 4u, 4u, 2u);
    th16(b, th_uxth(4u, 4u));
}

'@

$src = Replace-Once $src $old $new "insert leaf RET helper"

$anchor = @'
MdNativeV2Status md_native_v2_compile_local_call_loop(const uint8_t *memory,
                                                       uint16_t cs,
                                                       uint16_t entry_ip,
                                                       MdNativeV2Code *out,
                                                       uint8_t *counter_reg_out)
'@

$leafCompiler = @'
/*
 * Phase 3Q: bounded leaf CALL/RET counted-loop lowering.
 *
 * Production class:
 *
 *     entry:
 *         call leaf
 *         dec  counter
 *         jnz  entry
 *
 *     leaf:
 *         83 /op r16,imm8
 *         ret
 *
 * /op is one of ADD, SUB, CMP, or OR reg,0. The destination may be any
 * ordinary 16-bit GPR except SP and the loop counter. The loop counter may be
 * any supported non-SP register except AX (matching the existing DEC/JNZ
 * counted-loop admission rule).
 *
 * This is intentionally an instruction class, not a benchmark byte matcher:
 * register, immediate, CALL displacement and code addresses are all free.
 *
 * The generated Thumb region performs the exact guest CALL stack write and
 * RET stack pop effect, keeps all guest GPRs resident, preserves the leaf's
 * CF through DEC, and branches natively around the complete loop. Runtime
 * guards both non-contiguous code spans and the exact two-byte stack window
 * before entry.
 */
static MdNativeV2Status md_nv2_compile_leaf_call_dec_loop(
    const uint8_t *memory,
    uint16_t cs,
    uint16_t entry_ip,
    MdNativeV2Code *out,
    uint8_t *counter_reg_out)
{
    MdThumbBuf b;
    uint16_t callee;
    uint16_t root_end;
    uint16_t callee_end;
    uint8_t dec_opcode;
    uint8_t counter;
    uint8_t modrm;
    uint8_t alu;
    uint8_t dst_reg;
    uint8_t imm8;
    int8_t simm;
    int8_t back_rel;
    const unsigned roff = (unsigned)offsetof(MdX86, r);
    const unsigned ssoff = (unsigned)offsetof(MdX86, ss);
    const unsigned moff = (unsigned)offsetof(MdX86, memory);
    const unsigned ipoff = (unsigned)offsetof(MdX86, ip);
    size_t loop_native;
    size_t bne_loop;
    unsigned dst;
    unsigned counter_arm;
    unsigned i;

    if (memory == NULL || out == NULL || counter_reg_out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    /* Root is exactly CALL rel16 ; DEC r16 ; JNZ root. */
    if (entry_ip > 0xFFF9u ||
        md_nv2_guest8(memory, cs, entry_ip) != 0xE8u)
        return MD_NATIVE_V2_UNSUPPORTED;

    dec_opcode = md_nv2_guest8(memory, cs, (uint16_t)(entry_ip + 3u));
    if ((dec_opcode & 0xF8u) != 0x48u ||
        md_nv2_guest8(memory, cs, (uint16_t)(entry_ip + 4u)) != 0x75u)
        return MD_NATIVE_V2_UNSUPPORTED;

    back_rel = (int8_t)md_nv2_guest8(
        memory, cs, (uint16_t)(entry_ip + 5u));
    if ((uint16_t)(entry_ip + 6u + back_rel) != entry_ip)
        return MD_NATIVE_V2_UNSUPPORTED;

    counter = (uint8_t)(dec_opcode & 7u);

    /*
     * Keep the first production class aligned with the already-proven
     * DEC/JNZ counted-loop restrictions. SP cannot be the counter because
     * CALL/RET own it; AX remains excluded by the existing production rule.
     */
    if (counter == MD_X86_AX || counter == MD_X86_SP)
        return MD_NATIVE_V2_UNSUPPORTED;

    callee = md_nv2_rel16_target(
        (uint16_t)(entry_ip + 3u),
        md_nv2_guest16(memory, cs, (uint16_t)(entry_ip + 1u)));

    if (callee > 0xFFFCu)
        return MD_NATIVE_V2_UNSUPPORTED;

    root_end = (uint16_t)(entry_ip + 6u);
    callee_end = (uint16_t)(callee + 4u);

    /* Caller and leaf spans must be distinct and non-wrapping. */
    if (!(callee_end <= entry_ip || callee >= root_end))
        return MD_NATIVE_V2_UNSUPPORTED;

    /*
     * Leaf v1 is one register/immediate operation followed by RET.
     * Match the same safe 83-group subset already supported by the generic
     * Native-v2 counted-loop compiler.
     */
    if (md_nv2_guest8(memory, cs, callee) != 0x83u ||
        md_nv2_guest8(memory, cs, (uint16_t)(callee + 3u)) != 0xC3u)
        return MD_NATIVE_V2_UNSUPPORTED;

    modrm = md_nv2_guest8(memory, cs, (uint16_t)(callee + 1u));
    if ((modrm >> 6) != 3u)
        return MD_NATIVE_V2_UNSUPPORTED;

    alu = (uint8_t)((modrm >> 3) & 7u);
    dst_reg = (uint8_t)(modrm & 7u);
    imm8 = md_nv2_guest8(memory, cs, (uint16_t)(callee + 2u));
    simm = (int8_t)imm8;

    if (dst_reg == MD_X86_SP || dst_reg == counter)
        return MD_NATIVE_V2_UNSUPPORTED;

    if (!((alu == 0u || alu == 5u || alu == 7u) && simm >= 0) &&
        !(alu == 1u && simm == 0))
        return MD_NATIVE_V2_UNSUPPORTED;

    if (!th_offset_ok_h(roff) || !th_offset_ok_h(ipoff) ||
        ssoff > 0x0FFFu || moff > 0x0FFFu)
        return MD_NATIVE_V2_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    b.bytes = out->bytes;
    b.capacity = sizeof(out->bytes);
    b.at = 0u;
    b.failed = 0;

    /* Resident guest GPRs, memory pointer, and SS physical base. */
    th16(&b, th_mov_hi(8u, 0u));
    for (i = 1u; i < 8u; ++i) {
        if (!th_offset_ok_h(roff + i * 2u))
            return MD_NATIVE_V2_UNSUPPORTED;
        th16(&b, th_ldrh(md_nv2_arm_reg[i], 0u, roff + i * 2u));
    }

    th_ldr_w_imm(&b, 9u, 8u, moff);
    th_ldrh_w_imm(&b, 10u, 8u, ssoff);
    th32(&b, 0xEA4Fu, 0x1A0Au); /* lsl.w r10,r10,#4 */
    th_ldrh_w_imm(&b, 0u, 8u, roff);

    dst = md_nv2_arm_reg[dst_reg];
    counter_arm = md_nv2_arm_reg[counter];

    loop_native = b.at;

    /* Exact near CALL stack effect. */
    md_nv2_call_push_imm(&b, (uint16_t)(entry_ip + 3u));

    /*
     * Leaf operation. Only CF survives the terminal DEC, so we compute and
     * retain exactly that bit in r11; DEC exit reconstruction supplies the
     * remaining arithmetic flags.
     */
    if (alu == 0u) { /* ADD */
        th_add_w_imm(&b, dst, dst, (unsigned)imm8);
        th_cf_from_bit16(&b, dst);
        th16(&b, th_uxth(dst, dst));
    } else if (alu == 5u) { /* SUB */
        th_sub_w_imm(&b, dst, dst, (unsigned)imm8);
        th_cf_from_bit16(&b, dst);
        th16(&b, th_uxth(dst, dst));
    } else if (alu == 7u) { /* CMP */
        th16(&b, th_mov_hi(12u, dst));
        th_sub_w_imm(&b, 12u, 12u, (unsigned)imm8);
        th_cf_from_bit16(&b, 12u);
    } else { /* OR reg,0 */
        th_cf_zero(&b);
    }

    /* Exact RET SP effect; r11 must keep the leaf CF. */
    md_nv2_leaf_ret_drop(&b);

    /* DEC counter / JNZ entry. DEC preserves r11's guest CF. */
    th_sub_w_imm(&b, counter_arm, counter_arm, 1u);
    th16(&b, th_uxth(counter_arm, counter_arm));
    th16(&b, th_cmp_imm(counter_arm, 0u));
    bne_loop = th_emit_bcond_placeholder(&b, 1u);

    if (!th_patch_bcond(&b, bne_loop, 1u, loop_native))
        return MD_NATIVE_V2_BRANCH_RANGE;

    md_nv2_emit_exit_dec_flags(&b, counter);

    th_load_imm16(&b, 1u, root_end);
    th16(&b, th_strh(1u, 0u, ipoff));
    th16(&b, th_movs(0u, 0u));
    th16(&b, 0x4770u);

    if (b.failed)
        return MD_NATIVE_V2_TOO_LARGE;

    out->_align_word = 0x4E563242u;
    out->size = (uint16_t)b.at;

    /*
     * One architectural iteration:
     * CALL + leaf-op + RET + DEC + JNZ = five guest instructions.
     */
    out->op_count = 5u;
    out->start_ip = entry_ip;
    out->end_ip = root_end;
    out->has_local_loop = 1u;
    out->phase = 16u;
    out->needs_memory = 1u;
    out->has_store = 1u;
    out->requires_safe_ss_word = 1u;
    out->exit_flags_reg = counter;
    out->needs_entry_cf = 0u;
    out->cf_sites = 1u;
    out->z_sites = 1u;
    out->loop_terminal = 0x75u;
    out->exit_lazy_op = MD_LAZY_DEC16;
    out->exit_flag_dst = counter;
    out->exit_flag_src = 0xFFu;
    out->exit_flag_imm = 1u;
    out->retire_base_ops = 5u;
    out->chunkable_loop = 2u;
    out->local_call_graph = 1u;
    out->call_stack_bytes = 2u;

    out->guest_span_count = 2u;
    out->guest_span_ip[0] = entry_ip;
    out->guest_span_len[0] = 6u;
    out->guest_span_ip[1] = callee;
    out->guest_span_len[1] = 4u;

    *counter_reg_out = counter;
    return MD_NATIVE_V2_OK;
}

MdNativeV2Status md_native_v2_compile_local_call_loop(const uint8_t *memory,
                                                       uint16_t cs,
                                                       uint16_t entry_ip,
                                                       MdNativeV2Code *out,
                                                       uint8_t *counter_reg_out)
'@

$src = Replace-Once $src $anchor $leafCompiler "insert Phase 3Q leaf compiler"

$old = @'
    if (memory == NULL || out == NULL || counter_reg_out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    /* Root must be exactly: CALL rel16 ; LOOP back to the CALL. */
'@

$new = @'
    if (memory == NULL || out == NULL || counter_reg_out == NULL)
        return MD_NATIVE_V2_BAD_ARGUMENT;

    /*
     * Phase 3Q first: the general bounded leaf CALL + DEC/JNZ class. The
     * historical Phase-3M MDSTRESS graph below remains unchanged and is used
     * when this shape does not match.
     */
    {
        const MdNativeV2Status leaf_st =
            md_nv2_compile_leaf_call_dec_loop(
                memory, cs, entry_ip, out, counter_reg_out);

        if (leaf_st != MD_NATIVE_V2_UNSUPPORTED)
            return leaf_st;
    }

    /* Root must be exactly: CALL rel16 ; LOOP back to the CALL. */
'@

$src = Replace-Once $src $old $new "admit leaf graph before Phase 3M"

Write-Utf8NoBom $Nv2 $src

# ===========================================================================
# tests/test_native_v2.c
# ===========================================================================

$t = Read-Normalized $Test

$anchor = @'
static int check(const char *name,
'@

$builder = @'
static void build_leaf_call_graph(uint8_t *memory)
{
    /*
     * Generic Phase-3Q class:
     *
     *   0106: call 010d
     *   0109: dec cx
     *   010a: jnz 0106
     *
     *   010d: add bx,3
     *   0110: ret
     */
    static const uint8_t caller[] = {
        0xE8,0x04,0x00,
        0x49,
        0x75,0xFA
    };
    static const uint8_t leaf[] = {
        0x83,0xC3,0x03,
        0xC3
    };

    memset(memory, 0, 1u << 20);
    memcpy(memory + 0x0106u, caller, sizeof(caller));
    memcpy(memory + 0x010Du, leaf, sizeof(leaf));
}

static int check(const char *name,
'@

$t = Replace-Once $t $anchor $builder "insert leaf graph fixture"

$anchor = @'
    {
        static uint8_t call_memory[1u << 20];
        MdNativeV2Code code;
        MdNativeV2Status st;
        uint8_t counter = 0xFFu;

        build_phase4_call_graph(call_memory);
'@

$leafTest = @'
    {
        static uint8_t leaf_memory[1u << 20];
        MdNativeV2Code code;
        MdNativeV2Status st;
        uint8_t counter = 0xFFu;

        build_leaf_call_graph(leaf_memory);

        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_local_call_loop(
            leaf_memory, 0u, 0x0106u, &code, &counter);

        if (st != MD_NATIVE_V2_OK ||
            counter != MD_X86_CX ||
            code.phase != 16u ||
            code.op_count != 5u ||
            code.start_ip != 0x0106u ||
            code.end_ip != 0x010Cu ||
            code.loop_terminal != 0x75u ||
            code.chunkable_loop != 2u ||
            !code.local_call_graph ||
            code.call_stack_bytes != 2u ||
            code.guest_span_count != 2u ||
            code.guest_span_ip[0] != 0x0106u ||
            code.guest_span_len[0] != 6u ||
            code.guest_span_ip[1] != 0x010Du ||
            code.guest_span_len[1] != 4u ||
            !code.needs_memory ||
            !code.has_store ||
            !code.requires_safe_ss_word ||
            code.exit_flags_reg != MD_X86_CX ||
            code.exit_lazy_op != MD_LAZY_DEC16 ||
            code.size == 0u) {
            fprintf(stderr,
                    "phase3q leaf CALL graph compile failed st=%s "
                    "phase=%u ops=%u end=%04x counter=%u graph=%u "
                    "depth=%u spans=%u size=%u\n",
                    md_native_v2_status_name(st),
                    (unsigned)code.phase,
                    (unsigned)code.op_count,
                    code.end_ip,
                    (unsigned)counter,
                    (unsigned)code.local_call_graph,
                    (unsigned)code.call_stack_bytes,
                    (unsigned)code.guest_span_count,
                    (unsigned)code.size);
            return 1;
        }

        /* Destination register/immediate are class parameters, not constants. */
        leaf_memory[0x010Eu] = 0xC2u; /* ADD DX,imm8 */
        leaf_memory[0x010Fu] = 0x17u;
        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_local_call_loop(
            leaf_memory, 0u, 0x0106u, &code, &counter);
        if (st != MD_NATIVE_V2_OK || code.phase != 16u) {
            fprintf(stderr,
                    "phase3q parameterized leaf rejected st=%s\n",
                    md_native_v2_status_name(st));
            return 1;
        }

        /* A leaf may not modify the counted register. */
        leaf_memory[0x010Eu] = 0xC1u; /* ADD CX,imm8 */
        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_local_call_loop(
            leaf_memory, 0u, 0x0106u, &code, &counter);
        if (st != MD_NATIVE_V2_UNSUPPORTED) {
            fprintf(stderr,
                    "phase3q counter-clobber leaf admitted st=%s\n",
                    md_native_v2_status_name(st));
            return 1;
        }

        /* RET is part of the proof. */
        build_leaf_call_graph(leaf_memory);
        leaf_memory[0x0110u] = 0x90u;
        memset(&code, 0, sizeof(code));
        st = md_native_v2_compile_local_call_loop(
            leaf_memory, 0u, 0x0106u, &code, &counter);
        if (st != MD_NATIVE_V2_UNSUPPORTED) {
            fprintf(stderr,
                    "phase3q missing-RET leaf admitted st=%s\n",
                    md_native_v2_status_name(st));
            return 1;
        }
    }

    {
        static uint8_t call_memory[1u << 20];
        MdNativeV2Code code;
        MdNativeV2Status st;
        uint8_t counter = 0xFFu;

        build_phase4_call_graph(call_memory);
'@

$t = Replace-Once $t $anchor $leafTest "insert Phase 3Q host tests"

$old = @'
    puts("native-v2 phase3p REP/string + CALL/RET graph tests: PASS");
'@

$new = @'
    puts("native-v2 phase3q leaf + phase3p REP/string + CALL/RET graph tests: PASS");
'@

$t = Replace-Once $t $old $new "update native-v2 PASS label"

Write-Utf8NoBom $Test $t

# ===========================================================================
# pico/microdos_native3_bench.c
# ===========================================================================

$b = Read-Normalized $Bench

# The file is intentionally compact/minified. Add an exact-state accumulator to
# the timed loop and expose Native-v2 retirement on the existing result line.
$old = @'
static uint64_t one(const char*n,const uint8_t*p,size_t z,unsigned runs){MdHooks h;uint64_t t0,us,ret=0;unsigned i;memset(&h,0,sizeof(h));memset(guest,0,GUEST_BYTES);memcpy(guest+0x100,p,z);md_runtime_init(&rt,guest,&h);md_native3_init(&n3,code,sizeof(code));for(i=0;i<4;i++){MdN3RunResult rr;rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[MD_X86_SP]=0xFFFE;rt.stop_reason=MD_STOP_NONE;(void)md_native3_run(&n3,&rt,2000000,&rr);}t0=time_us_64();for(i=0;i<runs;i++){MdN3RunResult rr;memset(rt.cpu.r,0,sizeof(rt.cpu.r));rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[MD_X86_SP]=0xFFFE;rt.stop_reason=MD_STOP_NONE;while(rt.stop_reason==MD_STOP_NONE){if(!md_native3_run(&n3,&rt,2000000,&rr))break;ret+=rr.retired;}}us=time_us_64()-t0;{uint64_t m=us?ret*1000u/us:0;uint64_t c=m?(uint64_t)MICRODOS_PICO_SYS_KHZ*1000u/m:0;printf("[n3] %-8s ",n);f3(m);printf(" MIPS ");f3(c);printf(" cyc/guest GATE40=%s native=%llu interp=%llu shadow=%llu/%llu/%llu\n",m>=40000u?"PASS":"FAIL",(unsigned long long)n3.stats.native_retired,(unsigned long long)n3.stats.interp_retired,(unsigned long long)n3.stats.shadow_pushes,(unsigned long long)n3.stats.shadow_hits,(unsigned long long)n3.stats.shadow_misses);stdio_flush();return m;}}
'@

$new = @'
static uint64_t one(const char*n,const uint8_t*p,size_t z,unsigned runs){MdHooks h;uint64_t t0,us,ret=0;unsigned i;int exact=1;memset(&h,0,sizeof(h));memset(guest,0,GUEST_BYTES);memcpy(guest+0x100,p,z);md_runtime_init(&rt,guest,&h);md_native3_init(&n3,code,sizeof(code));for(i=0;i<4;i++){MdN3RunResult rr;rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[MD_X86_SP]=0xFFFE;rt.stop_reason=MD_STOP_NONE;(void)md_native3_run(&n3,&rt,2000000,&rr);}t0=time_us_64();for(i=0;i<runs;i++){MdN3RunResult rr;memset(rt.cpu.r,0,sizeof(rt.cpu.r));rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[MD_X86_SP]=0xFFFE;rt.stop_reason=MD_STOP_NONE;while(rt.stop_reason==MD_STOP_NONE){if(!md_native3_run(&n3,&rt,2000000,&rr))break;ret+=rr.retired;}if(p==callmix&&(rt.cpu.r[MD_X86_BX]!=0x8000u||rt.cpu.r[MD_X86_CX]!=0u||rt.cpu.r[MD_X86_SP]!=0xFFFEu||rt.stop_reason!=MD_STOP_HALT))exact=0;}us=time_us_64()-t0;{uint64_t m=us?ret*1000u/us:0;uint64_t c=m?(uint64_t)MICRODOS_PICO_SYS_KHZ*1000u/m:0;printf("[n3] %-8s ",n);f3(m);printf(" MIPS ");f3(c);printf(" cyc/guest GATE40=%s native=%llu interp=%llu nv2=%llu exact=%s shadow=%llu/%llu/%llu\n",m>=40000u?"PASS":"FAIL",(unsigned long long)n3.stats.native_retired,(unsigned long long)n3.stats.interp_retired,(unsigned long long)n3.stats.nv2_retired,p==callmix?(exact?"PASS":"FAIL"):"-",(unsigned long long)n3.stats.shadow_pushes,(unsigned long long)n3.stats.shadow_hits,(unsigned long long)n3.stats.shadow_misses);stdio_flush();return m;}}
'@

$b = Replace-Once $b $old $new "add callmix exact/NV2 observability"

Write-Utf8NoBom $Bench $b

Write-Host ""
Write-Host "Applied Native-3 / Native-v2 Phase 3Q leaf CALL fast path."
Write-Host "Changed:"
Write-Host "  src\runtime\native_v2.c"
Write-Host "  tests\test_native_v2.c"
Write-Host "  pico\microdos_native3_bench.c"
Write-Host ""
Write-Host "Recommended validation:"
Write-Host '  .\md.bat build host'
Write-Host '  ctest --test-dir .\build-host -C Release -R "^(native_v2_phase1|native3)$" -V'
Write-Host '  .\scripts\md_pico_flash_capture.ps1 -Build -Label "native3-leafcall-3q" -CaptureSeconds 30'
