# microDOS architecture

## Execution model

```text
                         executable image
                               |
                        dosrecomp analysis
                               |
                 +-------------+-------------+
                 |                           |
            known AOT block            unresolved code
                 |                           |
                 v                           v
          generated native C          8086 interpreter
                 |                           |
                 +-------------+-------------+
                               |
                         guest CS:IP
                               |
                           MdRuntime
                               |
             +-----------------+----------------+
             |                 |                |
          memory           interrupts           I/O
             |                 |                |
         20-bit RAM       DOS / BIOS / IVT    devices
```

Generated code and interpreted code use the same `MdRuntime` and `MdX86`. A known guest offset enters a generated C block. An unknown offset executes through the interpreter; when the interpreter reaches a known offset again, execution returns to AOT.

The current mixed-mode fallback checks dispatch after each interpreted instruction. This is intentionally the correctness-first implementation. The high-performance version will execute cached decoded blocks and return to the common dispatcher only at block boundaries.

## CPU state and shared semantics

`MdX86` stores AX/CX/DX/BX/SP/BP/SI/DI, ES/CS/SS/DS, IP, FLAGS, and the guest-memory base. Register order follows 8086 opcode encoding.

`include/microdos/ops.h` is shared by the interpreter and generated C for arithmetic and FLAGS behavior. Recompilation changes control-flow representation, not instruction semantics.

```text
interpreter opcode ----+
                       +--> shared op helper --> MdX86
AOT generated C -------+
```

Future ADC/SBB, shifts/rotates, logic, multiply/divide, BCD, and string primitives should follow the same rule.

## Memory

Guest physical space is exactly 1 MiB and wraps at 20 bits:

```text
physical = ((segment << 4) + offset) & 0xFFFFF
```

Host builds use a normal 1 MiB allocation. Pico Plus 2 is intended to back this directly with PSRAM so ordinary guest memory accesses remain pointer-based.

Decoded-block invalidation for self-modifying code will be page-granular. Pages become tracked only when executable code is decoded/AOT-associated; ordinary data pages should avoid expensive cache-maintenance work.

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

AOT code advances guest IP before calling the shared interrupt routine, matching the interpreter's architectural return address.

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

Known branches currently return through the dispatcher. Once executable-page generations exist, safe hot edges can chain directly without a central switch.

## Hybrid proof

`tests/programs/hybrid.com` intentionally contains an instruction supported by the interpreter but not by the first AOT decoder:

```text
AOT 0100  mov ax,1234h
AOT 0103  jz 0108h       ; not taken
             |
             v
fallback 0105
interp    mov bx,ax      ; 89 C3
interp    nop
             |
             v
AOT 0108  hlt
```

The test verifies `BX == 1234h` and exactly five guest instructions executed. This is the first real proof that the two paths form one execution engine rather than two separate demos.

## Interpreter performance plan

The normal interpreter remains `md_interp_run()`. GCC/Clang uses direct-threaded dispatch for hot opcodes; MSVC keeps switch dispatch for portability. `md_interp_step()` is not the performance path.

Next layers:

1. complete 8086/8088 instruction and prefix coverage;
2. factor decode into a shared instruction-description layer;
3. hot ModR/M forms in threaded handlers;
4. decoded basic-block cache keyed by guest address + executable-page generation;
5. interpreted block return to the common AOT dispatcher;
6. safe direct block chaining;
7. block counters for AOT candidate discovery.

The current validation machine measured roughly 257 MIPS for threaded interpretation and 506 MIPS for generated AOT on the same branch-heavy guest loop. These are host regression measurements, not RP2350 predictions. Switch-dispatch interpretation measured roughly 192 MIPS in the same run.

## Hardware boundary

```text
DOS / BIOS service
       |
platform/pico2
       +--> MicroRender --> ST7796 / other displays
       +--> MicroWave   --> I2S audio
       +--> MicroConsole/catBUS --> controls, SD, USB
```

The CPU/recompiler core remains platform-neutral so the same execution state can be tested on desktop and RP2350.
