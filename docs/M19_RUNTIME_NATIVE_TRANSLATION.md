# M19 — Runtime Native Translation

M19 adds a second native-code producer beside offline `dosrecomp`: code that
DOS loads at runtime can be translated on the RP2350 itself.

```text
                       guest CS:IP
                           |
                  +--------+--------+
                  |                 |
             static AOT?       JIT block?
                  |                 |
                  +--------+--------+
                           |
                     native execution
                           |
                    unknown CS:IP
                           |
                    translate block
                           |
                     native execution
```

The canonical interpreter remains the semantic authority and exact fallback.
The intended rule is: **unknown code causes translation; it does not imply a
permanent switch back to interpretation.**

## M19.0 result

M19.0 proved on-device structural decode, Thumb emission into executable SRAM,
`(CS,IP)` lookup and self-modification invalidation. It was intentionally
call-threaded: every translated x86 instruction performed a `BLX` into C, and
every block returned to the C dispatcher.

The RP2350 result was correct but slower than the threaded interpreter:

```text
loop / SRAM       threaded 4.918 MIPS   JIT 1.473   AOT 16.615
memloop / PSRAM   threaded 2.401 MIPS   JIT 1.805   AOT 10.363
```

The counters showed only three translations and zero fallback. Therefore the
cost was execution-mode/dispatch overhead, not decode or cache misses.

## M19.1 — direct Thumb regions

M19.1 changes the execution shape rather than widening opcode coverage.

A translated region now:

1. enters once from C;
2. executes simple x86 semantics as directly emitted Thumb;
3. holds the retired-instruction count in `r6` and the native budget in `r7`;
4. turns a backward direct edge into a Thumb branch when the target is already
   an instruction label inside the same region;
5. returns to C only for a genuine boundary (fallthrough out of the region,
   unsupported instruction, budget edge, HLT, or self-modifying store).

For the existing fixtures that means the hot loops are native loops:

```text
loop.com
    MOV CX,FFFF
.loop:
    DEC CX
    JNZ .loop      -> Thumb branch to translated DEC

memloop.com
    setup
.loop:
    MOV AL,[SI]
    ADD AL,3
    MOV [SI],AL
    ADD SI,97
    OR  SI,8000h
    DEC CX
    JNZ .loop      -> Thumb branch to translated MOV AL,[SI]
```

The direct subset is still intentionally small:

- `MOV r8,imm8`
- `MOV r16,imm16`
- `INC/DEC r16` when CF can be preserved without duplicating lazy-CF logic
- accumulator ADD/OR/AND/SUB/XOR/CMP immediate
- group-1 register ADD/OR/AND/SUB/XOR/CMP immediate
- `MOV AL,[SI]`
- `MOV [SI],AL`
- JNZ after direct INC/DEC
- direct near JMP (local backward target chains; external target exits)
- NOP
- HLT

ADC/SBB, general ModR/M effective addresses, arbitrary Jcc and other complex
instructions remain exact interpreter boundaries for now.

## Flags

M18 lazy flags remain the architectural representation. Generated code writes
the same `lazy_op/lazy_a/lazy_b/lazy_res/lazy_carry` fields as the shared C
helpers. A region that contains INC/DEC materialises incoming flags once at
entry so preserved CF is authoritative. Logic operations make CF known-zero;
if a later INC/DEC would need CF from an unresolved lazy ADD/SUB, direct
lowering stops before that instruction instead of inventing a second flag
implementation.

## Memory and invalidation

Guest reads are direct. `MOV [SI],AL` calculates the real-mode 20-bit linear
address and checks `code_page_executable[linear>>12]` inline:

```text
ordinary data page -> direct STRB
executable page    -> canonical md_x86_write8_linear()
                       complete x86 store
                       return from native region
```

Therefore hot data writes avoid a helper while self-modifying code retains the
existing executable-page generation and AOT invalidation semantics. Static AOT
continues using its finer 64-byte chunk guards.

## Budget semantics

The C dispatcher only enters a region when the first straight-line pass fits.
Before a native backward edge is taken, generated code checks that the entire
next target-to-branch span fits the remaining instruction budget. If it does
not, the region stores the exact target IP and returns. The existing canonical
boundary path handles the final instructions exactly.

## Cache

The prototype remains bounded and deterministic:

- 128 direct-mapped descriptors;
- up to 12 x86 instructions discovered per region;
- 32 KiB executable SRAM arena in `microdos_bench`;
- whole-JIT flush when the arena fills.

Collisions and flushes are performance events only.

## M19.1 measured result

On the RP2350 at 300 MHz, M19.1 removed the C-dispatch bottleneck completely:

```text
loop / SRAM       threaded 4.959   AOT 16.612   JIT 22.126 MIPS
loop / PSRAM      threaded 4.959   AOT 16.575   JIT 22.073 MIPS
memloop / SRAM    threaded 2.772   AOT 28.572   JIT 20.524 MIPS
memloop / PSRAM   threaded 2.382   AOT 10.357   JIT  9.081 MIPS
```

The hot regions each compiled once and entered only once before their final HLT
block; fallback remained zero. Runtime JIT was already faster than static-C AOT
on the register-only loop and within about 12% of AOT on the PSRAM memloop.

## M19.2 — resident ARM-register regions

M19.1 still synchronized guest registers and lazy-flag metadata throughout the
native loop and performed exact-budget bookkeeping at every backward edge.
M19.2 recognizes the two benchmark loop shapes and treats them as native regions.

The region contract is deliberately guarded:

- remaining budget must be enough to finish the region; otherwise it executes
  nothing and the canonical path advances exactly;
- `loop.com` materialises flags once so incoming CF can be preserved by DEC;
- `memloop.com` requires DS=0 and proves pages 8000h-FFFFh are not executable
  before using raw stores; otherwise it drops to M19.1;
- guest register/lazy-flag state is written back only on region exit.

The register-only hot loop therefore reduces to essentially:

```asm
subs r0,#1      ; resident CX
bne  loop
```

The memory loop keeps CX, SI, AL and the guest-memory base resident and executes:

```asm
ldrb r2,[r5,r1]
adds r2,#3
strb r2,[r5,r1]
adds r1,#97
uxth r1,r1
orrs r1,r3
subs r0,#1
bne  loop
```

This milestone explicitly separates the RP2350 compute ceiling from the PSRAM
ceiling. Very small register/branch regions may approach hundreds of guest MIPS;
PSRAM-heavy DOS code cannot, because external memory latency becomes dominant.

## Next

After the M19.2 benchmark:

1. generalize resident-register analysis beyond the two fixtures;
2. keep x86 condition state in ARM APSR when producer/consumer stay in-region;
3. patch direct edges whose target is a different translated region;
4. widen ModR/M/effective-address lowering using shared decode metadata;
5. emit native helper calls for complex instructions without returning through
   the system dispatcher;
6. add JIT to non-static-AOT DOS segments and run JIT-vs-interpreter lockstep
   across COMMAND.COM and DOS2TEST.
