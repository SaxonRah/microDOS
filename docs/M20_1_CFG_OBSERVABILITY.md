# M20.1 — bounded CFG regions + full-DOS JIT observability

M20.0 proved two things on RP2350 at 300 MHz:

- generic straight-line counted regions can remain resident (`regionmix`: 83.333 MIPS SRAM);
- the real DOS hybrid is correct: released MS-DOS 2.0 + COMMAND.COM + static kernel/DOS2TEST AOT + runtime JIT, with DOS2TEST 25/25.

The missing information was what the ~1.4M non-static-AOT instructions in a real interactive session were actually doing. M20.1 makes that visible and adds the first multi-block resident CFG region.

## Execution-tier accounting

Ctrl+] now partitions every interval into:

- `static-aot`: instructions retired by dosrecomp-generated code;
- `jit-native`: instructions retired by runtime-generated Thumb;
- `interpreted`: everything else (including JIT fallback steps and AOT holes).

`jit-owned` is also reported: every instruction executed while the JIT tier owned the current non-AOT segment. Its fallback subset is shown separately.

## JIT exit observability

Exact aggregate counters are kept for:

- compile-fail fallbacks;
- budget fallbacks;
- zero-progress/unsupported fallbacks;
- native returns to the dispatcher;
- CS-change exits back to the DOS system loop;
- stops;
- invalidations and arena flushes.

A small space-saving CS:IP table records hot exit sites. Ordinary fallbacks and CS changes are recorded directly. Native-return sites are sampled 1/64 so profiling does not become the bottleneck; the exact aggregate native-return count is still maintained.

The report shows `CS:IP`, opcode byte and reason, for example:

```text
[jit] hot sites (interval delta; ~ means space-saving candidate):
[jit]   ~     420  1234:08A2 op=8B  zero
[jit]   ~     188  1234:0911 op=75  return
```

These sites are the inputs for the next opcode/CFG work.

## Bounded CFG v1

M20.1 discovery may continue across forward conditional branches instead of always terminating at the first Jcc. The first native CFG form is deliberately conservative:

- one counted backward edge ending in `DEC CX / JNZ`;
- zero or more forward `JE/JNE` edges;
- each forward edge consumes an immediately preceding `CMP` immediate;
- the target must be a later instruction in the same region;
- AX/BX/CX/SI use the existing resident register set;
- CFG-v1 is register-only (memory regions still use M20/M19.2 paths);
- final DEC's preserved CF must be provably deterministic;
- unsupported shapes fall back unchanged.

The region budget check uses the conservative longest path. Once admitted, an ARM register counts the actually executed guest instructions, so taken/not-taken paths retire an exact guest instruction count.

## branchmix proof

`branchmix` alternates a forward branch every iteration:

```asm
mov cx,8000h
mov ax,0
mov bx,0
.top:
    xor ax,1
    cmp ax,0
    jz .skip_add
    add bx,3
.skip_add:
    or  bx,0
    dec cx
    jnz .top
hlt
```

Expected final state:

```text
AX=0000
BX=C000
CX=0000
212995 instructions before HLT
212996 including HLT
```

The generated Thumb shape was independently disassembled during development. Its hot control flow is a real `BEQ` around the optional native block and a real backward `BNE`; no C dispatcher is entered inside the region.

## Full-DOS profiling procedure

Flash `microdos_pico_jit.uf2`, boot to `A>`, then isolate COMMAND.COM work:

```text
Ctrl+]
VER
DIR
ECHO HELLO WORLD
MD TEST
CD TEST
CD ..
RD TEST
Ctrl+]
```

Also run a parsing-heavy interval:

```text
Ctrl+]
ECHO A
ECHO B
ECHO C
ECHO D
ECHO E
Ctrl+]
```

and a DOS/filesystem-heavy interval:

```text
Ctrl+]
DIR
DIR
DIR
DIR
DIR
Ctrl+]
```

Send the full second Ctrl+] report for each. The `tiers:` line tells us how much COMMAND.COM is already native; the hot-site table tells us exactly what to compile next.
