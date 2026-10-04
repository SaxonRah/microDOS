# microDOS Native v2 — Pico-first native engine

## Purpose

Native v2 is the production-direction execution engine for microDOS.

The direct-threaded interpreter remains the correctness oracle, cold-code
executor, and unsupported-instruction fallback. Native v2 is not an extension
of the old helper-heavy JIT ABI.

The core rule is:

> Hot guest state stays in host registers across a native region.

Phase 1 exists to prove that rule on RP2350 with real runtime-generated Thumb
code before the engine is connected to MS-DOS dispatch.

## RP2350 register convention

The phase-1 mapping favors the 8086 registers most useful to low-register
Thumb arithmetic:

| 8086 | ARM |
|---|---|
| AX | r4 |
| CX | r5 |
| SI | r6 |
| DI | r7 |
| BX | r8 |
| DX | r9 |
| SP | r10 |
| BP | r11 |

r3 holds the `MdX86 *` frame pointer for the region. r0-r2 and r12 are scratch.

The ABI trampoline saves r4-r11 once at region entry and restores them once at
region exit. Generated code does not load/store the guest GPR array per guest
instruction.

## Phase-1 compiler

Supported guest operations:

- `MOV r16,imm16`
- `INC r16`
- `DEC r16`
- one local backward `JZ` or `JNZ`
- `NOP`

The compiler:

1. uses the canonical `md_decode_8086()` decoder;
2. lowers into a transient fixed-size operation array;
3. emits Thumb directly into SRAM;
4. discards the transient operations;
5. executes the native region;
6. spills all eight guest GPRs once when the region exits.

The first benchmark compiles this real 8086 stream at runtime:

```asm
mov ax,0000h
mov cx,8000h
loop:
inc ax
dec cx
jnz loop
```

It executes 98,306 guest instructions per native-region invocation.

## Why the subset is intentionally tiny

This is an architectural proof, not an opcode-coverage milestone.

The first measurements must answer:

- Does register residency work correctly on RP2350?
- How many host cycles per guest instruction does the region achieve?
- What is the native byte expansion?
- Is region entry/exit overhead negligible at realistic loop lengths?
- Does the runtime-generated SRAM code path remain stable at 300 MHz?

Only after those answers are known do we widen semantics.

## Phase 2

Phase 2 should add:

- virtual/native flag state rather than phase-1 Z-only tracking;
- register-register ALU;
- `83h` register-immediate ALU;
- common ModR/M memory forms;
- cached DS/SS bases;
- direct guest memory access with 20-bit wrap;
- more than one local CFG edge;
- native near CALL/RET;
- safepoint/budget accounting at region boundaries.

No hot operation should call a C semantic helper unless there is a measured
reason that the helper is cheaper.

## Phase 3

Integrate with full DOS:

```text
lookup native region
    |
    +-- hit --> execute native region
    |
    +-- miss/cold --> threaded interpreter
                         |
                         +-- hot --> compile region
```

Self-modifying-code invalidation remains byte/page correct. The existing
interpreter remains the differential reference.

## Performance target

At the current 300 MHz RP2350 operating point:

| Throughput | Host cycles / guest instruction |
|---:|---:|
| 20 MIPS | 15 |
| 30 MIPS | 10 |
| 50 MIPS | 6 |
| 75 MIPS | 4 |
| 100 MIPS | 3 |

The first phase-1 loop should be judged primarily by host cycles per guest
instruction, not by comparison with the old JIT.
