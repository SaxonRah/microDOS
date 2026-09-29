# microDOS design decisions

## 1. Two execution paths are a permanent feature

microDOS uses AOT recompilation for known code and a high-performance 8086 interpreter for everything else. The interpreter is not scheduled for removal.

Reasons:

- DOS software commonly uses indirect far calls, overlays, writable code, interrupt-vector replacement, and self-modification.
- Static analysis does not need to prove every target before a program becomes useful.
- The same process can move between compiled and interpreted regions while preserving exact architectural state.

## 2. One machine state and one semantics model

Both execution paths operate on `MdRuntime` / `MdX86`.

Generated code must not invent a second CPU representation. Registers, flags, segmented addresses, interrupt routing, memory writes, and I/O are shared. Differential testing between interpreter and generated code is a first-class feature.

## 3. The interpreter is performance-critical

The RP2350 build should prefer:

- direct-threaded dispatch on GCC/Clang;
- hot opcode-class handlers rather than a single monolithic decoder;
- inline 8/16-bit register and memory access;
- a directly addressable 1 MiB guest memory region on Pico Plus 2 PSRAM;
- no allocation, formatting, logging, or callback traffic in the per-instruction hot path;
- native callbacks only for interrupt, port-I/O, device, and block-transition boundaries;
- future basic-block decode caching for frequently interpreted regions.

MSVC keeps a switch dispatcher for portability and validation. It is not the performance reference for RP2350.

## 4. Preserve 8086 real-mode semantics where they matter

The guest address calculation is `(segment << 4) + offset`, wrapped to 20 bits. Unclaimed software interrupts fall through to the real IVT and push FLAGS, CS, and IP exactly as an 8086 interrupt does.

This is important because DOS software can install interrupt handlers. Native BIOS/DOS acceleration is an interception layer, not an excuse to delete the underlying mechanism.

## 5. Do not emulate a PC unless software requires a device

The target is DOS compatibility, not a cycle-accurate IBM PC motherboard.

We should avoid emulating 8259/8237/PIT/CGA hardware when a DOS/BIOS boundary can be implemented natively. Hardware emulation is added only for programs that directly depend on a device interface.

## 6. Pico Plus 2 first

The first hardware target is the Pimoroni Pico Plus 2. Its external PSRAM is the intended backing store for the full 1 MiB DOS physical address space. RP2350 internal SRAM remains available for stacks, hot runtime state, display/audio buffers, caches, and device queues.

## 7. MicroRender / MicroWave / MicroConsole are backends, not dependencies of the CPU core

The core runtime stays platform-neutral. Pico integration will connect BIOS/device services to:

- MicroRender for display;
- MicroWave for audio;
- MicroConsole/catBUS for controls, SD, and hardware integration.

This keeps the interpreter/recompiler testable without hardware.
