# microDOS architecture

## Execution model

```text
                    executable image
                          |
                +---------+---------+
                |                   |
          known AOT block      unresolved code
                |                   |
                v                   v
          generated C        8086 interpreter
                |                   |
                +---------+---------+
                          |
                       MdRuntime
                          |
          +---------------+----------------+
          |               |                |
       memory          interrupts          I/O
          |               |                |
      20-bit RAM     DOS / BIOS / IVT    devices
```

A compiled block and the interpreter can hand control to one another. The eventual block dispatcher will key native blocks by guest `CS:IP` / linear target. A miss enters the interpreter. An interpreted jump into a known block returns to native execution.

## CPU state

`MdX86` stores the 8086 general registers, segment registers, IP, FLAGS, and the guest memory base. General registers use the 8086 opcode encoding order (`AX,CX,DX,BX,SP,BP,SI,DI`) so ModR/M and opcode-low-bit register selection avoid translation tables.

8-bit registers are views of the first four 16-bit registers:

```text
0 AL   1 CL   2 DL   3 BL
4 AH   5 CH   6 DH   7 BH
```

## Guest memory

The architectural address space is exactly 1 MiB and addresses wrap at 20 bits.

Host builds allocate the region normally. On Pico Plus 2 the target design is a directly addressable PSRAM window so ordinary memory instructions stay pointer-based instead of calling a generic memory callback for every byte.

Memory-mapped device interception, if needed later, should be page-based so normal RAM remains the fast case.

## Interrupts

`md_runtime_interrupt()` first offers the vector to the native platform hook. If the hook declines it, the runtime performs real 8086 IVT dispatch:

```text
push FLAGS
push CS
push IP
clear IF/TF
IP = word [vector*4]
CS = word [vector*4+2]
```

During early bootstrap the host shim claims a tiny subset of INT 21h. Later, recompiled DOS will own its actual DOS service vector and the native layer will primarily implement BIOS/device boundaries.

## Interpreter performance plan

Phase 1 already separates dispatcher choice from opcode semantics. GCC/Clang builds use direct-threaded dispatch for selected hot opcodes; MSVC uses the generic decoder loop.

Next performance layers, in order:

1. fill out the complete 8086/8088 instruction set;
2. add prefix state without allocating decoder objects;
3. split common ModR/M forms into hot handlers;
4. add decoded basic-block cache entries keyed by physical start address and code-generation epoch;
5. terminate cached blocks at control flow, interrupt, I/O, segment-state hazards, or writes to covered code pages;
6. chain cached blocks directly where safe;
7. collect low-cost block counters so frequently interpreted regions become candidates for AOT metadata.

The target is not cycle accuracy by default. An optional timing model can accumulate approximate 8088/8086 cycles when required by software.

## Recompiler shape

The recompiler should emit readable portable C close to the architectural operations rather than reconstructing high-level source.

Example input:

```asm
mov ah,09h
mov dx,message
int 21h
```

Conceptual output:

```c
md_x86_set_reg8(cpu, 4, 0x09);
cpu->r[MD_X86_DX] = MESSAGE;
md_runtime_interrupt(runtime, 0x21);
```

Control-flow targets that resolve to known compiled blocks become native calls or dispatcher transitions. Unknown indirect targets enter the shared block dispatcher and can fall back to interpretation.

## Hardware integration

Pico hardware support is intentionally above the CPU core:

```text
DOS/BIOS service
     |
platform/pico2
     +--> MicroRender --> ST7796 / other displays
     +--> MicroWave   --> I2S audio
     +--> MicroConsole/catBUS --> buttons, sticks, SD, USB
```

This lets the exact same DOS execution core run under a desktop validation frontend.
