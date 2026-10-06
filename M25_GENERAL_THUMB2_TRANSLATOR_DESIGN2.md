# microDOS M25 — General Thumb-2 Translator Design

**Status:** v2 — reviewed against the codebase; M25.1 prototype implemented and differential-tested  
**Target:** Raspberry Pi Pico 2 / RP2350, Cortex-M33 / Thumb-2  
**Repository baseline:** `8f15d94` (`Remove temporary Native-v2 benchmark target`)  
**Correctness baseline:** 624,744 / 624,744 hard 8086 silicon vectors + 1000 / 1000 randomized router differential  
**Current measured reference:** DOS2TEST ~3.365 MIPS, Native-v2 DOS coverage ~8%, native-heavy MDSTRESS ~36.45 MIPS at 300 MHz

---

## 0. v2 review changes (read first)

v1's architecture stands. v2 corrects or adds the following, each confirmed by
the M25.1 prototype (`src/runtime/translate.c`, `src/runtime/thumb2_emit.h`,
`tests/test_translate_diff.c`):

1. **Fused compares must shift operands into the top bits** (§13). v1's
   `CMP r4,r7 / BLT` is wrong for 16-bit signed values held in 32-bit
   registers. ARM carry is also inverted for subtraction, so the x86→ARM
   condition map depends on the producer class.
2. **Upper-half garbage invariant** (§10). Pinned registers hold the guest
   value in bits 0..15 only; no UXTH after ALU results.
3. **Absolute addresses are translation-time constants** (§10). `r12` =
   `MdRuntime*`, `lr` = guest RAM base, reloaded with MOVW/MOVT after helpers.
   No translation frame loads except the budget word at `[sp]`.
4. **1 MiB-aligned guest RAM** turns base-add + 20-bit wrap into one BFI (§14).
5. **The budget is also the preemption mechanism** (§23). Without it a
   chained loop never returns to C and starves timers/USB.
6. **Lazy state is the exit flag representation** (§13). Producers write
   `lazy_op/a/b/res` only when observable; no materialisation on exit.
7. **Self-modifying-code granularity is the #1 performance risk** (§16).
   4 KiB page invalidation punishes .COM programs whose code and data share a
   page. M25 also requires `MD_X86_TRACK_WRITES=1`, which today's Native-v2
   firmware disables.
8. **DIV/IDIV helpers are conditionally resumable** (§11): #DE changes CS:IP.
9. **Gate on MDSTRESS with Native-v2 off, not DOS2TEST** (§32, §40).
10. **Development does not need hardware** (§35): `qemu-arm` executes the
    generated Thumb-2 next to the real interpreter.
11. The differential harness found a **pre-existing threaded-interpreter bug**:
    DIV/IDIV raising INT 0 did not refresh the cached CS base (`op_group3`
    ended with `MD_NEXT()`). Fixed; it was also the cause of the 6
    `test_aot_muldiv_fault` checks failing on GCC host builds, and it affects
    GCC/threaded Pico firmware today.

---

## 1. Executive summary

microDOS is no longer primarily limited by PSRAM bandwidth. Controlled PSRAM/SRAM A/B measurements show only about a 1.3% DOS2TEST difference and essentially no difference in heavily interpreted MDSTRESS.

The dominant cost is execution overhead per guest instruction:

- decode and dispatch;
- guest state loads/stores;
- flag bookkeeping;
- entry/exit overhead between execution tiers;
- repeated address-generation work;
- returning to C between short compiled regions.

Native-v2 demonstrates the property required for high performance: keep guest state resident in ARM registers and execute many guest instructions per host control-flow transition.

M25 should therefore not be another pattern-specific Native-v2 expansion and should not revive the old JIT unchanged.

M25 is a **general 8086 -> Thumb-2 translator** with:

1. broad block translation rather than pattern rejection;
2. resident guest registers across translated block boundaries;
3. direct block-to-block chaining;
4. helper calls only for semantics that are not yet directly lowered;
5. hard exits only for instructions that truly require canonical runtime control;
6. flag liveness and producer/consumer fusion;
7. existing decoder, code-page generations, router heat sampling, and Native-v2 Thumb emitter reused wherever possible.

The goal is not that every instruction is hand-lowered on day one. The goal is that every hot site can be represented by the translator from day one.

---

# 2. Performance interpretation

At 300 MHz:

| Path | Approximate throughput | Approximate host cycles / guest instruction |
|---|---:|---:|
| Current DOS2TEST | 3.365 MIPS | 89 |
| Current native-heavy MDSTRESS | 36.45 MIPS | 8.2 |
| 40 MIPS target | 40 MIPS | 7.5 |
| 50 MIPS target | 50 MIPS | 6 |
| 100 MIPS target | 100 MIPS | 3 |

A general 8086 instruction cannot normally execute in three Cortex-M33 cycles when considered in isolation. Fetch, effective-address calculation, memory access, flag semantics, guest IP/accounting, and control transfer all cost real work.

However, **effective guest throughput above 100 MIPS may be reachable for the best fused hot traces**, because multiple guest instructions can share one native control-flow decision and one set of resident guest registers. (v2 note: a realistic estimate for the LODSB example below, including the per-iteration budget/guard check, is ~18–22 Thumb instructions per 5 guest instructions, i.e. roughly 60–75 MIPS. Treat 100+ as a stretch case.)

Example guest loop:

```asm
LODSB
CMP AL,20h
JNZ skip
INC BX
LOOP loop
```

If five guest instructions lower to approximately 8–12 Thumb instructions per iteration, effective guest throughput can exceed the raw per-instruction translation ratio.

The route to very high MIPS is therefore:

> amortize host work across many guest instructions, not merely make opcode lookup O(1).

---

# 3. Lessons from the existing engines

## 3.1 Canonical interpreter

The canonical interpreter is correct and now contains several very good optimizations:

- threaded dispatch;
- cached code and stack locality;
- hot exact paths;
- closed-form shift/rotate semantics;
- branchless flag materialization;
- direct signed lazy-condition evaluation;
- table/mask-based EA work.

It remains the authoritative correctness fallback.

It should stay.

## 3.2 Decoded block cache

`src/runtime/x86_block_cache.c` already demonstrates useful ideas:

- decode once;
- cache blocks;
- fuse `DEC r16 + JNZ`;
- execute small resident interpreted regions;
- use generation/epoch validation.

Its main limitation is that execution still dispatches each decoded operation through C and unsupported operations fall immediately back to the canonical interpreter.

M25 should reuse the idea of predecoded blocks, not this execution mechanism as the final hot path.

## 3.3 Old JIT

The old JIT already proved:

- runtime Thumb generation works on RP2350;
- local direct edges work;
- resident register regions are fast;
- generated stores can cooperate with self-modifying-code tracking.

It also demonstrated the failure mode to avoid:

- limited direct coverage;
- repeated fallback/zero-progress transitions;
- helper-heavy translation;
- short native episodes;
- repeated router/JIT entry cost.

M25 should reuse infrastructure, not the old policy.

## 3.4 Static AOT

AOT coverage alone is not enough.

Measured DOS2TEST showed high AOT instruction coverage but only a moderate throughput increase. Short AOT episodes repeatedly cross the native/runtime boundary.

The important metric is therefore not merely:

```text
percent compiled
```

but:

```text
average guest instructions per native residency episode
```

and eventually:

```text
time spent without returning to the dispatcher
```

## 3.5 Native-v2

Native-v2 is the proof of concept for the desired execution model:

- guest registers resident;
- helper-free hot loop;
- local CFG;
- amortized admission;
- precise exits;
- reusable structural classes;
- direct Thumb-2 lowering.

M25 generalizes those properties without requiring a loop to match a proof-specific Native-v2 class.

---

# 4. M25 goals

## Required

1. Hot code can always be represented by the translator.
2. Common 8086 instructions lower directly to Thumb-2.
3. Unsupported-but-resumable instructions can call a semantic helper and continue.
4. Control/system instructions that cannot safely resume terminate the translated trace cleanly.
5. Guest GPRs remain resident across translated block boundaries.
6. Direct control-flow edges chain block-to-block.
7. Self-modifying code remains exact.
8. Exact guest retirement accounting is preserved.
9. The canonical interpreter remains the semantic oracle.
10. Translation must be optional and independently disableable.

## Performance targets

### First useful milestone

```text
DOS2TEST              >= 4.0 MIPS
native/translated DOS >= 20%
no MDSTRESS regression
```

### M25 target

```text
DOS2TEST              >= 5.0 MIPS
translated hot code   >= 40 MIPS
hot translated share  >= 30%
```

### Longer-term target

```text
ordinary hot traces   50–75 MIPS
best fused loops      100+ effective guest MIPS
time-weighted DOS     increasingly dominated by translated execution
```

---

# 5. Non-goals

M25 is not:

- a speculative optimizing compiler;
- SSA infrastructure for its own sake;
- a full x86 dynamic binary translator framework;
- a replacement for the canonical interpreter;
- a system that recompiles cold one-shot DOS initialization code;
- a giant handwritten 256-way ARM opcode table;
- a return to per-instruction C helper calls.

The implementation should remain small enough to understand and verify.

---

# 6. High-level architecture

```text
                    +----------------------+
8086 bytes -------->| existing x86 decoder |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    | M25 semantic uops    |
                    | + EA descriptions    |
                    | + flag metadata      |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    | trace/block analysis |
                    | flag liveness        |
                    | helper classification|
                    | edge discovery       |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    | Thumb-2 emitter      |
                    | reused from NV2      |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    | translation cache    |
                    | guard + native body  |
                    +----------+-----------+
                               |
                 +-------------+-------------+
                 |                           |
                 v                           v
          direct linked edge          helper / hard exit
                 |                           |
                 +-------------+-------------+
                               |
                               v
                        canonical runtime
```

---

# 7. Proposed source layout

New public interface:

```text
include/microdos/translate.h
```

New runtime files:

```text
src/runtime/translate.c
src/runtime/translate_ir.h
src/runtime/translate_runtime.c
src/runtime/thumb2_emit.c
src/runtime/thumb2_emit.h
```

Existing files reused:

```text
src/decode/x86_decode.c
src/runtime/native_v2.c
src/runtime/native_v2_runtime.c
src/runtime/exec_router.c
src/runtime/x86_block_cache.c
include/microdos/ops.h
include/microdos/x86.h
```

Long term, Thumb encoding helpers duplicated between old JIT and Native-v2 should move into `thumb2_emit.*`.

These helpers run during translation, not guest execution, so making them ordinary shared C functions has no hot guest-runtime cost and should reduce firmware duplication.

---

# 8. Translation IR

The IR should remain deliberately small.

Example:

```c
typedef enum MdTrKind {
    MD_TR_MOV_RR8,
    MD_TR_MOV_RR16,
    MD_TR_MOV_RI8,
    MD_TR_MOV_RI16,

    MD_TR_LOAD8,
    MD_TR_LOAD16,
    MD_TR_STORE8,
    MD_TR_STORE16,

    MD_TR_ALU_RR8,
    MD_TR_ALU_RR16,
    MD_TR_ALU_RI8,
    MD_TR_ALU_RI16,

    MD_TR_INC16,
    MD_TR_DEC16,

    MD_TR_SHIFT8,
    MD_TR_SHIFT16,

    MD_TR_LODS,
    MD_TR_STOS,
    MD_TR_MOVS,
    MD_TR_CMPS,
    MD_TR_SCAS,

    MD_TR_JCC,
    MD_TR_JMP,
    MD_TR_CALL,
    MD_TR_RET,

    MD_TR_HELPER,
    MD_TR_EXIT
} MdTrKind;
```

A compact operation:

```c
typedef struct MdTrOp {
    uint16_t ip;
    uint16_t next_ip;
    uint16_t target;
    uint16_t imm;

    uint8_t kind;
    uint8_t dst;
    uint8_t src;
    uint8_t aux;

    uint8_t flags_read;
    uint8_t flags_write;
    uint8_t ea_kind;
    uint8_t helper_kind;
} MdTrOp;
```

IR is compile scratch only.

It should not consume permanent per-block SRAM unless a helper genuinely requires metadata at runtime.

---

# 9. Effective-address representation

Do not rediscover ModR/M semantics in every emitter.

Decode memory operands into a small EA recipe:

```c
typedef struct MdTrEa {
    uint8_t base;
    uint8_t index;
    uint8_t segment;
    uint8_t width;
    int16_t displacement;
} MdTrEa;
```

Special values:

```text
base  = NONE
index = NONE
segment = DS / SS / ES / CS / override
```

The existing decoder already gives opcode, prefixes, ModR/M, instruction size and flow. M25 adds semantic extraction once during translation.

Hot execution never decodes ModR/M again.

---

# 10. Persistent translated register ABI

This is the central M25 rule.

Use ARM callee-saved registers for guest GPRs:

```text
r4  = AX
r5  = CX
r6  = DX
r7  = BX
r8  = SP
r9  = BP
r10 = SI
r11 = DI
```

Why callee-saved registers?

Because normal C helper calls are required by the ARM ABI to preserve r4–r11.

That means a translated block can call a carefully designed helper without automatically losing resident guest registers.

Available scratch (v2):

```text
r0-r3    scratch, destroyed by helpers
r12      MdRuntime* (== &rt->cpu), invariant at every block boundary
lr       guest RAM base, invariant at every block boundary
APSR     scratch except between a fused producer and its Bcc
[sp]     remaining instruction budget
```

### v2: upper-half garbage invariant

A pinned register holds the guest value in bits 0..15; bits 16..31 are
undefined. Every consumer already ignores them: compares use `LSL #16`
operand forms, addresses pass through UXTH, write-back uses STRH, byte
registers use UBFX/BFI. Therefore `ADD AX,BX` is one `ADD.W r4,r4,r7`, with
no UXTH. The trampoline loads with LDRH and the exit stores with STRH.

### v2: constants instead of frame loads

The runtime pointer, guest RAM base, page-generation array and helper
addresses are fixed for the life of a translation, so they are emitted as
MOVW/MOVT immediates. `r12` and `lr` are reloaded after each helper call
(4 instructions). A whole-cache flush on runtime reset keeps this valid.

### v2: why this differs from Native-v2's ABI

Native-v2 keeps guest registers in r0–r7 because its hot loops never call C.
M25 calls helpers, so it uses callee-saved r4–r11. The encoders are shared
(`thumb2_emit.h`); lowering code is not.

A persistent translation frame on the native stack stores:

```text
MdRuntime *
current translation context
remaining guest budget
optional spill temporaries
return address / engine state
```

Generated code can reload the runtime pointer into `r0` or `r12` when required.

## Entry

The translator trampoline:

1. saves native callee-saved state once;
2. loads AX..DI into r4..r11 once;
3. initializes translation-frame state;
4. branches to translated block body.

## Chaining

A direct translated edge does **not** spill guest GPRs.

It branches directly to the next translated block.

## Exit

Only when leaving translated execution:

1. write dirty guest registers back;
2. materialize required guest flags;
3. set exact CS:IP;
4. record retirement;
5. restore native ABI state;
6. return to runtime/router.

This is how M25 avoids the AOT short-entry problem.

---

# 11. Helper classes

“Never reject code” must be implemented carefully.

Not every instruction can safely call an arbitrary helper and resume.

Use three classes.

## Class A — direct native

Examples:

```text
MOV
ADD/SUB
AND/OR/XOR
CMP/TEST
INC/DEC
common shifts
simple memory access
Jcc
JMP
LODS/STOS
```

No C transition.

## Class B — resumable semantic helper

A helper is allowed when:

- CS:IP flow is known to remain at the translated continuation;
- it does not asynchronously replace execution state;
- it has a precise guest register/flag contract.

Examples initially could include:

```text
complex shift variants
MUL forms
DAA/DAS/AAA/AAS
some string semantics
special flag operations
```

v2: DIV/IDIV (and INTO) are **conditionally resumable**: a divide error
dispatches INT 0 and changes CS:IP. The helper returns a status; on fault,
translated code exits to the runtime instead of continuing.

Prefer helpers with explicit operands:

```c
uint16_t md_tr_helper_shift16(
    MdX86 *cpu,
    unsigned op,
    uint16_t value,
    unsigned count);
```

This is much better than calling a helper that expects all guest GPRs to have already been written back.

Pinned GPRs stay resident.

## Class C — canonical exit

These terminate the translated episode at exact CS:IP:

```text
INT
IRET
far CALL/JMP/RET initially
I/O initially
HLT
faulting/invalid instruction
complex system boundary
unsupported semantic operation with unknown control effect
```

The canonical interpreter executes the instruction.

Afterward, the router may immediately re-enter translated code at the new hot CS:IP.

This means translation itself never has to fail merely because the next instruction is complex.

---

# 12. Dirty register tracking

Each translated trace maintains a compile-time dirty GPR mask:

```text
bit 0 AX
bit 1 CX
...
bit 7 DI
```

For a hard exit:

```text
spill only dirty GPRs
```

For a helper:

- Class B helpers normally do not require GPR spills.
- Helpers that inspect specific architectural GPRs get an explicit required-sync mask.
- Only those registers are written before the call and reloaded afterward if modified.

This is important.

A general `spill all / call C / reload all` path is acceptable for initial correctness but must not become the normal helper ABI.

---

# 13. Flag architecture

Flags are one of the largest opportunities.

## Rule 1: flags are dataflow inside a trace

Do not eagerly produce canonical guest flags after each ALU operation.

Example:

```asm
CMP AX,BX
JL target
```

v1 proposed `CMP r4,r7 / BLT`. **That is wrong**: the registers hold 16-bit
values in 32-bit registers (with v2's garbage invariant, not even
zero-extended), so a 32-bit signed compare misjudges negative 16-bit values.
Correct lowering shifts both operands into the top bits, which makes ARM
N/Z/C/V exactly the 16-bit result's flags:

```asm
LSL.W r3, r4, #16
CMP.W r3, r7, LSL #16
BLT   target
```

Bytes use `#24`. INC/DEC use `ADDS/SUBS r3, r3, #(1<<16)`. Logic ops use
`LSL r3,res,#16 ; CMP r3,#0`, which yields C=1 and V=0 — exactly the x86
CF=0/OF=0 under the subtract mapping.

Condition map (x86 cc 0..F → ARM), `-` = not fusable, use the helper:

| producer | O | NO | B | AE | E | NE | BE | A | S | NS | P | NP | L | GE | LE | G |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SUB/CMP/logic | VS | VC | CC | CS | EQ | NE | LS | HI | MI | PL | - | - | LT | GE | LE | GT |
| ADD | VS | VC | CS | CC | EQ | NE | - | - | MI | PL | - | - | LT | GE | LE | GT |
| INC/DEC | VS | VC | - | - | EQ | NE | - | - | MI | PL | - | - | LT | GE | LE | GT |

ARM C after subtraction is NOT-borrow, so x86 JB (CF=1) is ARM CC after SUB
but CS after ADD. JBE/JA after ADD have no single ARM condition. JP/JNP are
never fusable.

There is no reason to create:

```text
lazy_a
lazy_b
lazy_res
lazy_op
```

when the next consumer is already known.

## Rule 2: dead flags do not exist

For:

```asm
ADD AX,BX
MOV CX,1
XOR AX,AX
```

if the ADD flags are overwritten before any read, emit no ADD flag bookkeeping.

## Rule 3: analyze individual flag bits

Each IR operation records:

```text
flags_read
flags_write
```

using:

```text
CF PF AF ZF SF OF
```

A backwards liveness pass determines which produced flags matter.

## Rule 4: start with adjacent fusion

M25.1 does not need a complex virtual-flags compiler.

Implement the highest-value cases first:

```text
CMP + Jcc
TEST + Jcc
DEC + JNZ
INC/DEC + JZ/JNZ
SUB + Jcc when result is retained
```

Later generalize to trace-wide flag liveness.

## Rule 5: materialize only on exits that require it

v2: there is nothing to reconstruct. Translated code writes the canonical
lazy fields (`lazy_op`, `lazy_a`, `lazy_b`, `lazy_res`) directly, which the
interpreter already understands bit-exactly. A producer writes them only if
its flags can be observed before the next producer:

```text
last producer in the block          (live-out)
producer followed by a possible side exit (a tracked store)
producer followed by a helper-evaluated Jcc
producer whose CF an INC/DEC lazy write needs
```

A dead CMP/TEST emits nothing at all.

INC/DEC preserve CF, so their lazy write must capture `lazy_carry`. The
translator finds the nearest earlier non-INC/DEC producer in the block and
computes CF inline from its lazy fields (`(a+b)>>w` or `(a-b)>>31`). With no
such producer, it checks whether the incoming `lazy_op` is already INC/DEC
(CF already in `lazy_carry`) and otherwise calls a one-line helper.

---

# 14. Memory lowering

The PSRAM A/B says guest-RAM placement is not the primary problem.

Therefore optimize **work around memory**, not memory placement.

## 14.1 Compute EA once

A decoded EA recipe becomes direct Thumb arithmetic.

Do not re-run ModR/M decoding.

## 14.2 Hoist segment bases inside traces

If several operations use DS:

```text
DS << 4
```

should be computed once for the trace/basic-block section where DS is invariant.

Same for SS/ES.

## 14.3 Preserve 20-bit correctness

The generic safe form remains:

```text
((segment << 4) + offset) & 0xFFFFF
```

v2: if guest RAM is aligned to its own size (1 MiB), the host pointer is

```asm
BFI lr, rlin, #0, #20      ; lr = base | (lin & 0xFFFFF)
LDRB r0, [lr]
```

one instruction for both the base add and the wrap. `lr`'s top bits never
change, so it stays usable as the base. The Pico currently places `g_guest`
with `aligned(16)`; M25 firmware should use `aligned(MD_GUEST_BYTES)`. The
prototype falls back to UBFX+ADD when RAM is not aligned.

Word accesses keep exact 8086 wrap semantics with two cheap checks, taken
out of line to a C helper when they fire:

```text
offset == FFFFh            -> high byte at seg:0000
(lin + 1) & mask == 0      -> linear wrap at the top of the space
```

## 14.4 Direct loads/stores

Use direct Thumb byte/halfword operations where legal:

```text
LDRB
LDRH
STRB
STRH
```

Store paths continue to cooperate with existing translated/AOT page tracking.

## 14.5 Self-modifying stores

Reuse the existing fast rule:

```text
page has no translated/AOT code
    -> direct store

page may contain translated/AOT code
    -> slow invalidation helper
```

Do not put the full invalidation machinery inline at every store site.

v2 detail: the slow helper reports whether `code_write_epoch` changed. If it
did, the store may have modified the currently executing block, so translated
code leaves immediately at the next instruction (with exact retirement).
Word stores also take the slow path when the two bytes straddle a page,
detected with `((lin+1) ^ lin) >> 12 != 0`.

---

# 15. Translation cache

Initial recommendation:

```text
code arena: 32 KiB
metadata:   128 translation slots
hash:       direct mapped initially
```

A translated block:

```c
typedef struct MdTrBlock {
    uint32_t code_offset;
    uint32_t page_generation[2];

    uint16_t cs;
    uint16_t ip;
    uint16_t end_ip;
    uint16_t native_bytes;

    uint8_t page[2];
    uint8_t page_count;
    uint8_t valid;
    uint8_t flags;

    MdTrLink exit[2];
} MdTrBlock;
```

Do not permanently store decoded IR.

Compile into shared scratch, emit, discard.

That lesson already exists in the old JIT.

---

# 16. Block validation and self-modifying code

Every translated block records the code pages and expected generation values covering its guest bytes.

A translated target has two conceptual entries:

```text
guard_entry
body_entry
```

## Initial chaining policy

Direct linked edges branch to `guard_entry`.

The guard:

1. checks expected page generation(s);
2. exits to the translator/router if invalid;
3. branches to `body_entry` if valid.

This costs a few instructions per translated block but makes chaining correct immediately.

## Internal trace edges

Branches within one translated trace go directly body-to-body with no generation check because the trace was admitted as one validated unit.

## v2: granularity is the main risk

Translations are validated per 4 KiB page. In a .COM program, code and its
data commonly share a page, so every store to a nearby variable bumps the
page generation, forces a side exit, and invalidates the block: a loop that
updates a variable on its own page would retranslate every iteration, which
is far slower than interpreting.

Fix before M25.2 benchmarks are trusted: byte-granular live-code tracking for
translated blocks, reusing the AOT engine's per-page live-byte bitmaps
(`aot_live_bits`) so that stores to data bytes on a code page neither bump
generations nor exit translated code.

Also: M25 needs `MD_X86_TRACK_WRITES=1`. The current Native-v2 firmware is
built with `MD_X86_TRACK_WRITES=0`, so an M25 firmware pays the store-tracking
check in the interpreter as well. The g128 A/B builds (tracking on) suggest
that cost is small, but it should be measured.

## Later optimization

Maintain reverse inbound-link lists.

On invalidation:

```text
patch inbound branches back to guard/dispatcher
```

Then known-stable linked edges can branch directly to `body_entry`.

Do not implement this in M25.1.

---

# 17. Block chaining

This is the core difference from short AOT episodes.

## Direct JMP

If target is translated:

```text
B target_guard
```

If not:

```text
exit with target CS:IP
```

The runtime may translate it and patch the source link.

## Conditional branch

Emit:

```text
Bcc translated_taken
B translated_fallthrough
```

when both targets exist.

If one target is untranslated, that edge exits.

## Fall-through

Patch exactly like a JMP.

## Near CALL

Phase 1:

- perform correct guest stack push;
- branch/exit to callee target;
- allow target to become translated.

Phase 2:

- direct-chain translated callee.

## RET

RET is indirect, so it requires lookup.

M25.1:

```text
pop guest IP
exit to translator lookup
```

M25.2:

inline tiny translation-cache lookup.

M25.3:

shadow return stack.

---

# 18. Shadow return stack

A translated CALL can record:

```text
guest return CS:IP
guest SP after push
native continuation entry
```

RET:

1. performs architectural guest pop;
2. compares returned guest IP/SP with shadow top;
3. on match, jumps directly to native continuation;
4. on mismatch, discards shadow entry and uses normal indirect lookup.

This is only a cache.

Architectural guest stack semantics remain authoritative.

It therefore remains correct when software manipulates return addresses.

---

# 19. Indirect translation lookup

For RET and indirect JMP/CALL, eventually use a small inline table.

Example:

```text
hash(CS,IP)
load slot tag
compare CS/IP
if match -> branch translated guard
else      -> runtime exit
```

Start with 64 or 128 slots.

This must be measured against simply returning to C.

Do not add it until direct-edge chaining already works.

---

# 20. Trace formation

Do not limit M25 to one tiny basic block.

A useful first trace builder should:

1. start at a hot CS:IP;
2. decode sequentially;
3. include fall-through instructions;
4. follow bounded forward conditional structure;
5. include one natural backward edge if it remains local;
6. stop at:
   - maximum operations;
   - far/system control;
   - code-page limit;
   - unsafe helper;
   - excessive CFG growth.

Initial limits:

```text
max guest ops:    32
max basic blocks: 4
max source bytes: 96
max code pages:   2
```

These can be tuned after profiling.

This is deliberately larger than an AOT entry and more general than Native-v2.

---

# 21. Router integration

The current execution router already provides:

- compact site heat state;
- promotion;
- cooldown;
- instability tracking;
- execution-tier profiling.

Reuse it.

Add a new tier:

```c
MD_EXEC_TIER_TRANSLATED
```

and mode:

```c
MD_EXEC_TRANSLATE_CANDIDATE
MD_EXEC_TRANSLATED
```

The new translator should not inherit old JIT acceptance rules such as requiring a high percentage of already-direct ops.

Instead:

```text
cold site
    -> interpreter

hot site
    -> build translator trace

trace with some helpers
    -> still accepted

unstable self-modifying site
    -> cooldown / interpreter
```

Admission should care about **expected residency**, not direct-op percentage.

Useful admission metric:

```text
expected_retired_per_entry
```

A helper-heavy trace that executes 500 guest instructions before exiting may still be excellent.

---

# 22. Why M25 differs from the old JIT

The old JIT is not useless; it contains proven components.

The new architecture differs in policy:

| Old JIT tendency | M25 rule |
|---|---|
| Unsupported op causes fallback churn | Resume-safe helper or clean trace boundary |
| Direct coverage threshold controls admission | Hot traces accepted even with helpers |
| Short native episodes common | Chaining is a first-class requirement |
| Resident regions are special cases | Resident GPRs are the default translated ABI |
| Flag handling tied to instruction lowering | Flag liveness/fusion is a separate analysis |
| Translation unit often small | bounded multi-block trace |
| Router/JIT re-entry frequent | stay inside translated execution across edges |

---

# 23. Retirement and budget accounting

Do not decrement a C-visible instruction counter after every guest instruction.

For straight-line translated blocks:

```text
retired = static guest count
```

For branches:

```text
each exit edge carries its retired count
```

For local loops:

- use static per-iteration counts;
- derive iteration count from the guest loop counter when possible, as Native-v2 already does;
- otherwise maintain a native retirement accumulator only when necessary.

The runtime receives one retired count per translated episode.

Exact-budget boundary cases may fall back to the canonical interpreter.

That is acceptable because budget-edge execution is rare and correctness-critical.

v2: one mechanism does retirement, budget and preemption. `[sp]` holds the
remaining budget. Each block's guard subtracts its full guest-instruction
count before executing; if the result would go negative it exits *before*
the block and the C loop finishes the budget with `md_interp_step`. A side
exit after k of n instructions adds back n−k. The C side computes retired as
`given − remaining` once per episode.

Because every chained block, including a block chained to itself, passes the
guard, translated execution always returns to C within the budget. That is
what lets timers, USB and the router run. v1 did not address this.

---

# 24. Translation-frame ABI

Suggested native frame:

```c
typedef struct MdTrFrame {
    MdRuntime *runtime;
    MdTranslator *translator;
    uint32_t remaining_budget;
    uint32_t retired;
    uint32_t exit_reason;
} MdTrFrame;
```

The actual generated-code stack layout should be fixed constants rather than C structure dereferences where possible.

Helpers can receive the runtime pointer loaded from this frame.

---

# 25. Code-cache lifecycle

Start simple:

```text
append-only executable SRAM arena
```

When full:

```text
flush whole translator cache
increment translation epoch
restart allocation
```

Do not implement code compaction.

A whole-cache flush is rare if hot admission is reasonable, and it makes branch patching much simpler.

Measure before building an eviction allocator.

---

# 26. Translation compile pipeline

```text
md_translate_prepare(cs, ip)
    |
    +-- decode existing x86 instructions
    |
    +-- semantic lower to MdTrOp[]
    |
    +-- discover local CFG
    |
    +-- classify helpers
    |
    +-- flag-use analysis
    |
    +-- GPR dirty analysis
    |
    +-- emit guard
    |
    +-- emit prologue only if engine entry
    |
    +-- emit native body
    |
    +-- emit exit/link stubs
    |
    +-- publish block atomically
```

Never publish a partially emitted block.

---

# 27. First direct-lowering set

M25.1 should begin with instructions already well understood by Native-v2 and the old JIT:

```text
MOV r8/r16, immediate
MOV r8/r16, register
MOV common register/memory forms

ADD
SUB
CMP
AND
OR
XOR
TEST

INC
DEC

PUSH
POP

Jcc
JMP
LOOP

LODSB/W
STOSB/W
```

Then:

```text
shift/rotate
MUL
DIV
SCAS
CMPS
MOVS
CALL/RET
segment-prefixed loads/stores
```

The closed-form helpers recently validated by the 624k suite give us a safe semantic fallback for the latter group.

---

# 28. First fusion set

Highest priority:

```text
CMP + Jcc
TEST + Jcc
DEC + JNZ
INC/DEC + JZ/JNZ
```

Next:

```text
LOAD + CMP + Jcc
LODS + CMP + Jcc
ALU + LOOP
```

Do not invent hundreds of named superinstructions.

Fusion should emerge from IR analysis.

---

# 29. Compile-time register strategy

M25.1 does not need a general register allocator.

Guest GPR placement is fixed.

Scratch allocation can be a tiny local allocator for:

```text
r0
r1
r2
r3
r12
```

Each emitted operation declares temporary requirements.

When a C helper is emitted, scratch state is considered destroyed.

Pinned r4–r11 remains valid by ABI.

This keeps the compiler understandable.

---

# 30. Segment handling

Segment registers remain canonical in `MdX86` initially.

For a block using a segment repeatedly:

```text
load segment once
shift << 4 once
reuse base while valid
```

If an instruction writes that segment register, invalidate the local cached base.

A later version can add trace specialization on stable segment values if measurement justifies it.

Do not put segment values permanently in precious callee-saved registers in M25.1.

---

# 31. Helper ABI examples

## Arithmetic helper

```c
uint16_t md_tr_shift16(
    MdX86 *cpu,
    unsigned op,
    uint16_t value,
    unsigned count);
```

Generated code:

```text
load MdX86* into r0
op -> r1
value -> r2
count -> r3
BL helper
move result r0 -> pinned guest destination
```

r4–r11 survive.

## Canonical instruction helper

For an operation that cannot resume safely:

```text
spill dirty regs
materialize flags
store exact IP
return MD_TR_EXIT_CANONICAL
```

Runtime calls the canonical interpreter once.

Then translator lookup resumes at the resulting CS:IP.

Do not embed `md_interp_step()` inside a hot translated block.

---

# 32. Profiling required before implementation

Before significant M25 code, extend the pure Pico benchmark to report cycles or MIPS for:

```text
register ALU
CMP/Jcc
ModR/M register
ModR/M memory
direct byte load
direct word load
store
PUSH/POP
CALL/RET
prefix handling
shift count 1
shift CL
MUL/DIV
REP string
block-cache hit
translation entry
translation exit
C helper call from Thumb
```

v2 gate correction: DOS2TEST is a poor primary gate. With 87% AOT coverage it
moved only from ~3.4 to ~3.7 MIPS, and the stats counter reports round 100k
multiples, so short intervals are approximate. Use **MDSTRESS with Native-v2
off** (2.72 MIPS interpreted baseline on the g128 builds) as the primary gate,
DOS2TEST as a secondary regression check.

Also benchmark fixed native episode lengths:

```text
1 guest op
2
4
8
16
32
64
128
```

This will directly measure the fixed entry/exit tax and show the minimum profitable translation length.

---

# 33. M25 implementation milestones

## M25.0 — Measurement and shared emitter extraction

Deliverables:

- Pico cycle/microbenchmark matrix;
- extract common Thumb builder from Native-v2/old JIT;
- no execution-policy change.

Gate:

```text
23/23 host
no Pico regression
```

## M25.1 — General translated straight-line blocks

Features:

- fixed r4–r11 guest GPR ABI;
- translation frame;
- 16–32-op blocks;
- core MOV/ALU/CMP/TEST/INC/DEC;
- direct memory loads;
- hard exits for unsupported control operations;
- no block chaining yet.

Goal:

```text
prove resident ABI and broad translation
```

## M25.2 — Direct block chaining

Features:

- guard/body entries;
- direct fall-through/JMP/Jcc patching;
- generation guards;
- stay native across blocks.

This is expected to be the first major DOS speed milestone.

## M25.3 — Flag liveness + fusion

Features:

- CMP/TEST + Jcc;
- dead flag elimination;
- DEC/JNZ fusion;
- fewer lazy-state writes.

Goal:

```text
>= 5 MIPS DOS2TEST
```

## M25.4 — Resumable helpers

Features:

- helper ABI;
- shift/MUL/DIV/special arithmetic;
- selective sync masks;
- continue translated execution after helper.

## M25.5 — Calls/returns

Features:

- direct near CALL chaining;
- inline RET translation lookup;
- optional shadow return stack.

## M25.6 — String and loop expansion

Features:

- MOVS/CMPS/SCAS;
- REP fast paths;
- counted-loop native residency;
- integrate proven Native-v2 classes into common translator.

At this point Native-v2 may begin shrinking into specialized optimizations inside the general translator rather than remaining a separate engine.

---

# 34. Acceptance metrics

Every milestone records:

```text
DOS2TEST MIPS
MDSTRESS MIPS
translated instruction percentage
average guest instructions / translated entry
translated entries
helper calls
hard exits
block-chain hits
indirect lookup hits
compile count
translation bytes emitted
FLASH
RAM
checksum
```

The most important new metric:

```text
guest instructions per native residency episode
```

A high translated percentage with short episodes is not success.

---

# 35. Correctness ladder

For each incremental translator change:

0. v2: `tests/test_translate_diff.c` under `qemu-arm` on the desktop
   (`arm-linux-gnueabihf-gcc -mthumb -static ...`). It executes the generated
   Thumb-2 and compares against the real interpreter. No hardware needed.
1. `.\md.bat test all`
2. translator differential tests against interpreter
3. deterministic random translated-vs-interpreter blocks
4. Pico functional benchmark
5. DOS2TEST checksum / deterministic state
6. full 624,744 hard silicon suite only at semantic checkpoints

The recently completed:

```text
624744 / 624744
1000 / 1000 random
```

is the semantic baseline for M25.

---

# 36. Differential translator harness

Add a test that:

1. creates random legal short 8086 sequences;
2. clones the same initial `MdX86`;
3. runs one through canonical interpreter;
4. runs one through translated block;
5. compares:
   - GPRs;
   - segments;
   - IP;
   - defined flags;
   - modified memory;
   - stop reason;
   - retirement count.

This should become the primary M25 development gate.

The full silicon vectors validate instruction semantics; this harness validates translator state mapping.

---

# 37. What to reuse immediately

## From decoder

Reuse:

```text
instruction length
prefixes
ModR/M byte
flow kind
branch target
8086 legality
```

Add semantic operand extraction above it.

## From Native-v2

Reuse:

```text
Thumb encoding helpers
20-bit address operations
byte-register UBFX/BFI work
direct GPR ALU lowering
memory load/store patterns
local branch patching
store safety ideas
retirement techniques
```

## From old JIT

Reuse:

```text
code arena
block hash ideas
compile workspace
page-generation snapshots
hot-site profiling
selected direct control helpers
```

Do not inherit:

```text
direct-coverage admission threshold
zero-progress retry architecture
per-op fallback calls
```

## From exec router

Reuse:

```text
heat
cooldown
unstable-code handling
tier statistics
promotion policy shell
```

Change the accepted tier from the old JIT to the new translator.

---

# 38. Expected performance mechanism

The translator does not need every individual direct instruction to execute in three cycles.

It wins by eliminating repeated overhead.

Interpreter model:

```text
fetch
decode
dispatch
load state
execute
update flags
store state
repeat
```

Translated trace model:

```text
load state once
execute guest operation
execute guest operation
execute guest operation
native branch
execute guest operation
...
spill once
```

If a 20-instruction guest trace costs:

```text
20 native semantic operations
10 addressing/branches
10 miscellaneous operations
```

then approximately 40 ARM instructions retire 20 guest instructions:

```text
~2 ARM instructions / guest instruction
```

Real memory and pipeline costs raise this, but this is the mechanism by which 50–100+ effective guest MIPS becomes plausible for suitable hot code.

---

# 39. Strategic direction

The long-term engine hierarchy should become:

```text
canonical interpreter
    |
    | cold / one-shot / exact fallback
    v
general translator
    |
    | hot reusable traces
    v
aggressively fused Thumb-2 execution
```

Native-v2’s proven ideas should gradually migrate into the general translator.

The old JIT can then be retired rather than maintained as a third competing runtime compiler.

Static AOT remains useful for special images or experiments, but no longer defines the main performance strategy.

---

# 40. Recommended immediate next action

Do **not** write the general translator first.

First create **M25.0 measurement firmware** that answers:

1. What is the fixed native entry/exit cost?
2. What is the C helper-call cost from generated Thumb?
3. Which interpreted operation classes consume the most cycles on RP2350?
4. At what block length does translation become profitable?
5. How much does direct chaining save compared with returning to C?
6. How many DOS2TEST instructions occur in traces long enough to benefit?

Then implement the smallest translator capable of demonstrating:

```text
resident r4-r11 guest registers
+
two directly chained translated blocks
+
CMP/Jcc fusion
```

If that prototype cannot materially beat the current interpreted MDSTRESS baseline (2.72 MIPS with Native-v2 off), stop and re-evaluate before expanding coverage. Do not stop on DOS2TEST alone (see §32).

If it does, M25 has a credible route toward Native-v2-like speed across ordinary DOS code.


---

# 41. M25.1 prototype status (v2)

Implemented:

```text
include/microdos/translate.h        public API: md_tr_init / md_tr_run / md_tr_flush
src/runtime/thumb2_emit.h            shared T32 encoders (verified against GNU objdump)
src/runtime/translate.c              front end, flag liveness, lowering, guards,
                                     exit stubs, chaining, C run loop
tests/test_translate_diff.c          randomized interpreter-vs-translator harness
scripts/md_translate_diff.sh         builds + runs the harness under qemu-arm (Linux/WSL)
```

Lowered directly: MOV (all register/memory/immediate forms), ADD/SUB/CMP/
AND/OR/XOR/TEST (register, memory, immediate, accumulator, group 1),
INC/DEC (r16, r/m8, r/m16), PUSH/POP r16, XCHG AX,r, LEA, CBW, CWD, NOP,
Jcc (fused or helper), JMP, LOOP, JCXZ, near CALL, near RET (+imm),
segment-override prefixes. Everything else (ADC/SBB, shifts, MUL/DIV,
strings, REP, flag ops, segment moves, INT, far control) runs through
`md_interp_step` at an exact block boundary.

Execution model as specified: r4–r11 resident, guard = page generations +
budget, exit stubs patched into direct `B.W` chains, RET as a dynamic exit.

Two refinements found by inspecting generated code:

- **Self-loop chains skip the page-generation check.** Within one native
  episode a block's code can only change through a slow store, which exits
  immediately; entries from C or from other blocks still check.
- **INC/DEC carry is captured at the producer.** The nearest earlier
  non-INC/DEC producer writes `lazy_carry` from its own registers (CMN/CMP +
  ADC/SBC, 5–6 instructions) instead of writing its full lazy state for the
  INC/DEC to reload.

## 41.1 Generated-code measurement

Loop `mov ax,[si] / add dx,ax / add si,2 / dec cx / jnz` (5 guest
instructions) after both refinements:

| part | Thumb instructions per iteration |
|---|---|
| budget check (self-loop entry) | 4 |
| `mov ax,[si]` fast path | 12 |
| `add dx,ax` (flags dead) | 1 |
| `add si,2` + carry capture for DEC | 7 |
| `dec cx` lazy write + fused `jnz` | 11 |
| back-edge branch | 1 |
| **total** | **~36 (~7.2 per guest instruction)** |

Estimate for Cortex-M33 (to be measured on hardware): ~9 cycles per guest
instruction, i.e. ~30 MIPS for this loop at 300 MHz, against ~110 cycles per
instruction for the threaded interpreter on MDSTRESS today.

## 41.2 Next optimisations, in order of measured weight

1. **Deferred lazy writes on loop latches** (saves ~7 per iteration here).
   DEC CX's lazy state is only observable if the loop exits. For producers
   whose lazy fields are reconstructible from registers at exit (INC/DEC,
   ADD/SUB immediate, CMP/TEST with unchanged operands), write the lazy state
   in the exit stubs (taken-exit, budget exit) instead of every iteration.
2. **Cheaper word-load checks** (5 of the 12 in `mov ax,[si]`). Merge the
   offset-FFFFh and linear-wrap tests, or prove them impossible for an EA.
3. **Byte-granular SMC tracking** (§16). Required before trusting any
   benchmark on .COM programs.
4. Inline RET lookup and shadow return stack (§18–19).
5. Helpers for shifts (closed-form, already in ops.h), MUL/DIV, strings.

## 41.3 Verification

```text
encoders:          45 / 45 forms match objdump
differential:      random programs, aligned and unaligned guest RAM, 0 mismatches:
                   registers, segments, IP, FLAGS, retired count, stop reason,
                   all 1 MiB of guest memory   (counts: see release notes)
host suite:        23 / 23 on GCC (threaded dispatch) after the INT 0 fix
cross-compile:     warning-free with the Pico flags (cortex-m33, -O2 -Wall -Wextra);
                   translate.o text ~24 KiB, bss ~2.3 KiB (+ arena + MdTranslator)
```

SRAM budget to plan for on the Pico: translator code (~24 KiB, copy_to_ram),
`MdTranslator` (8.1 KiB with 256 slots) and the code arena (32 KiB suggested).

Not yet done: router/firmware integration, byte-granular SMC tracking,
deferred lazy writes, inline RET lookup, helpers for shifts/MUL/DIV/strings,
Pico measurement.


## 41.4 Step 2: dispatch, latches, byte-exact SMC, DOS integration

First on-device measurement (RP2350, 300 MHz, guest in PSRAM, before step 2):

| loop | interpreter | translated | speedup |
|---|---|---|---|
| memory-sum | 3.36 MIPS | 31.68 MIPS | x9.4 |
| dec-jnz | 6.67 MIPS | 20.68 MIPS | x3.1 |
| call/ret | 4.58 MIPS | 5.82 MIPS | x1.27 (every RET returned to C) |

Step 2 implements, in that order of weight:

1. **Inline dynamic dispatch.** RET pops into r1 and probes `tr->blocks`
   from generated code with the same hash as `md_tr_lookup()`
   (`x=(cs<<4)+ip; idx=(x^(x>>9))&(SLOTS-1)`; one LDR compares `cs|ip<<16`),
   then BX to the target's guard, which still validates generations and
   budget. A miss takes the dynamic exit and C translates the target.
   Enabled only if `MdTrBlock` is 32 bytes with `cs`,`ip` adjacent.
2. **Deferred-flag self-loop latch.** A block ending in a fused Jcc back to
   its own start, whose flags are dead on entry, writes its last producer's
   lazy state only on the loop exit and on budget exhaustion. The latch is
   `LDR/SUBS/BLT/STR/B body`. INC/DEC rebuild `a = res -/+ 1`; a CF read
   from the incoming state is captured at exit (memory still holds the
   pre-loop state).
3. **Byte-exact SMC tracking.** New page flag `MD_X86_PAGE_TRBYTES` and
   `MdX86.tr_live_bits` (per-page bitmaps from a 16-page pool in the
   translator). `md_x86_note_tr_write()` bumps the page generation only when
   a translated byte is written; stores to data on a code page cost a C
   call but never invalidate or exit. Pool exhaustion falls back to
   page-granular `MD_X86_PAGE_TRANSLATED`. Hooked into the inline byte/word
   store paths and `md_x86_write_block`.
4. **DOS integration.** `MdDos2System.translator` +
   `MICRODOS_SYSTEM_ENABLE_TRANSLATOR`: `md_dos2_system_run()` calls
   `md_tr_run()` (same contract as `md_interp_run()`), so BIOS/DOS hooks fire
   unchanged through `md_interp_step`. Firmware `microdos_pico_m25`:
   translator on; kernel AOT, Native v2, old JIT and block cache off; guest
   RAM aligned to 1 MiB for BFI addressing. Splitbench's native-v2 column
   carries M25-native instructions; `[m25]` lines report exits, chains,
   live/fallback pages and latches.

Verification for step 2: directed programs (deferred latch incl. preserved
CF, CMP latch, same-page data stores with zero invalidations, a loop that
rewrites an immediate inside its own block, recursion, byte scanner) at 112
budgets each, aligned and unaligned, 0 mismatches; random programs 0
mismatches; desktop suite 23/23; both Pico firmwares build warning-free
against SDK 2.3.0.
