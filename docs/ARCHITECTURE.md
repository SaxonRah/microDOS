# microDOS architecture

## Execution model

```text
                              guest CS:IP
                                  |
                   +--------------+--------------+
                   |                             |
             known AOT block              interpreted region
                   |                             |
                   |                   +---------+---------+
                   |                   |                   |
                   |              decoded cache       opcode decoder
                   |                   |                   |
                   |                   +---------+---------+
                   |                             |
                   +--------------+--------------+
                                  |
                              MdRuntime
                                  |
                 +----------------+----------------+
                 |                |                |
              memory          interrupts           I/O
                 |                |                |
             20-bit RAM      DOS / BIOS / IVT    devices
```

Generated code, cached interpretation, and canonical opcode interpretation all use the same `MdRuntime` / `MdX86`. Execution mode changes control-flow representation; it does not change the architectural machine state.

## CPU state and shared semantics

`MdX86` stores AX/CX/DX/BX/SP/BP/SI/DI, ES/CS/SS/DS, IP, FLAGS, and the guest-memory base. Register order follows 8086 opcode encoding.

`include/microdos/ops.h` is shared by every execution path for arithmetic and FLAGS behavior:

```text
opcode interpreter ----+
                       |
decoded block cache ---+--> shared op helper --> MdX86
generated AOT C -------+
```

Future ADC/SBB, shifts/rotates, logic, multiply/divide, BCD, and string primitives should follow the same rule.

## Memory

Guest physical space is exactly 1 MiB and wraps at 20 bits:

```text
physical = ((segment << 4) + offset) & 0xFFFFF
```

Host builds use a normal 1 MiB allocation. Pico Plus 2 is intended to back this directly with PSRAM so ordinary guest memory accesses remain pointer-based.

`MdRuntime` also carries a `code_epoch`. Loading a new COM image advances the epoch. Cached decoded blocks record that epoch, so old blocks become misses without a table clear. `md_runtime_invalidate_code()` provides a coarse explicit invalidation hook.

Automatic self-modifying-code support will move from this global generation to executable-page generations. Pages should become tracked only when decoded/AOT code references them so normal data writes remain cheap.

## Interrupts

`md_runtime_interrupt()` offers an interrupt to the native hook first. If unclaimed it performs real IVT dispatch:

```text
push FLAGS
push CS
push IP
clear IF/TF
IP = word [vector*4]
CS = word [vector*4+2]
```

AOT and cached execution advance guest IP before calling the shared interrupt routine, matching the canonical interpreter's return address.

## Canonical opcode interpreter

`md_interp_run()` is the authoritative dynamic decoder. GCC/Clang builds can use direct-threaded dispatch for hot opcode classes; MSVC uses the portable switch path. `md_interp_step()` executes exactly one guest instruction and is used at mixed-mode boundaries.

This layer remains important even after the block cache is complete because it is the simplest correctness fallback for an instruction form that a higher tier has not learned yet.

## Decoded block cache

`MdBlockCache` is a caller-owned, fixed-size direct-mapped cache. The default configuration is:

```text
128 block slots
16 decoded ops per block
~17.5 KiB total storage
```

There is no allocation in the run loop. A cache lookup is keyed by full guest `CS:IP` plus `MdRuntime::code_epoch`; using full `CS:IP` rather than only the 20-bit linear address preserves near-control-flow semantics under real-mode aliasing.

A miss decodes one basic block. Decoding stops at:

- conditional or unconditional control flow;
- CALL/RET;
- INT;
- HLT;
- an unsupported opcode;
- the configured maximum decoded-op count.

An unsupported opcode becomes a one-instruction fallback block:

```text
cached block
    |
unsupported opcode
    |
md_interp_step()
    |
new CS:IP
    |
cache lookup
```

### Super-ops

Decoded interpretation is allowed to fuse instruction patterns when architectural behavior remains exact. The first super-op is:

```asm
dec r16
jnz target
```

The cache stores this as one decoded operation but records a guest count of two. This removes one dispatch from a very common loop idiom.

Instruction budgets remain exact. If a budget ends after the `DEC` but before the `JNZ`, the cached executor temporarily uses `md_interp_step()` so the externally visible stop point is still between the original instructions.

### Direct self-chaining

If executing a cached block leaves `CS:IP` equal to that same block's entry address, the executor immediately repeats the decoded block without another hash lookup. This is the first direct-block chaining optimization and is particularly useful for tight DOS loops on an in-order microcontroller core.

General inter-block links are intentionally deferred until they can be profiled against real DOS workloads. An attempted more-general edge link in development slowed the tight-loop fast path, so it was not retained.

## dosrecomp CFG

The analyzer uses two phases.

### 1. Instruction reachability

Start at the configured code entry and decode one reachable instruction at a time. Direct branch/call targets and fallthroughs are queued before block construction.

This avoids overlapping blocks on backward branches. The loop fixture:

```asm
0100  mov cx,ffffh
0103  dec cx
0104  jnz 0103
0106  hlt
```

becomes:

```text
block 0100 : mov cx,ffffh
block 0103 : dec cx / jnz 0103
block 0106 : hlt
```

### 2. Basic-block formation

Reachable instructions are grouped using entry, branch-target, and control-flow-fallthrough boundaries. A reachable instruction outside current AOT coverage becomes a fallback point rather than a compiler error.

`.COM` files may place data immediately after code, so `--code-start` and `--code-end` constrain analysis. The complete input image is still embedded in generated output.

## Generated C

A generated translation contains the original image, an instruction budget, a `CS:IP` dispatcher, known blocks, and an interpreter fallback.

Conceptually:

```c
md_dispatch:
    if (cpu->cs != segment) goto md_fallback;
    switch (cpu->ip) {
        case 0x0100: goto md_block_0100;
        case 0x0107: goto md_block_0107;
        default:     goto md_fallback;
    }

md_fallback:
    md_interp_step(runtime);
    goto md_dispatch;
```

The next mixed-mode optimization is to let generated AOT code hand an unknown region to the decoded cache until the guest reaches a known AOT address again, instead of retrying after every fallback instruction.

## Hybrid proof

`tests/programs/hybrid.com` intentionally contains `MOV BX,AX` (`89 C3`), supported by the canonical interpreter but left out of the first static AOT decoder and the first cache predecoder.

The tests now prove both forms of mixed execution:

```text
AOT -> canonical interpreter -> AOT
cache -> canonical interpreter -> cache
```

Both preserve `AX == BX == 1234h` and execute exactly five guest instructions.

## Performance tiers

Representative Release measurements on the development container for the 131,072-instruction loop:

```text
GCC threaded baseline       ~245 MIPS
cached decoded interpreter  ~346 MIPS
generated AOT               ~499 MIPS
```

When the baseline interpreter is deliberately forced to switch dispatch:

```text
switch baseline             ~186 MIPS
cached decoded interpreter  ~389 MIPS
generated AOT               ~503 MIPS
```

These are host regression figures, not RP2350 predictions. They demonstrate the intended tiering: cached interpretation can materially outperform repeated opcode decode while remaining fully portable C, and AOT still has additional headroom.

## Hardware boundary

```text
DOS / BIOS service
       |
platform/pico2
       +--> MicroRender --> ST7796 / other displays
       +--> MicroWave   --> I2S audio
       +--> MicroConsole/catBUS --> controls, SD, USB
```

The CPU/recompiler core remains platform-neutral so the same execution state can be differential-tested on desktop and RP2350.
