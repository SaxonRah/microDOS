# microDOS design decisions

## 1. Two execution paths are permanent

microDOS uses AOT recompilation for known code and a high-performance 8086 interpreter for everything else. The interpreter is not scheduled for removal.

Reasons:

- DOS software commonly uses indirect far calls, overlays, writable code, interrupt-vector replacement, and self-modification.
- Static analysis does not need to prove every target before a program becomes useful.
- A process can move between compiled and interpreted regions while preserving the same architectural state.

The first `dosrecomp` milestone already proves AOT -> interpreter -> AOT return using an instruction intentionally left out of the AOT decoder.

## 2. One machine state and one semantics model

Both execution paths operate on `MdRuntime` / `MdX86`.

Generated code must not invent a second CPU representation. Registers, flags, segmented addresses, interrupt routing, memory writes, and I/O are shared. Arithmetic/FLAGS helpers live in `include/microdos/ops.h` and are consumed by both the interpreter and generated C.

Differential testing is a first-class feature.

## 3. The interpreter is performance-critical

The RP2350 build should prefer:

- direct-threaded dispatch on GCC/Clang;
- hot opcode-class handlers rather than a single monolithic decoder;
- inline 8/16-bit register and memory access;
- a directly addressable 1 MiB guest memory region on Pico Plus 2 PSRAM;
- no allocation, formatting, logging, or callback traffic in the per-instruction hot path;
- native callbacks only at interrupt, port-I/O, device, and block-transition boundaries;
- decoded basic-block caching for frequently interpreted regions;
- direct block chaining when page/code-generation state proves it safe.

MSVC keeps a switch dispatcher for portability and validation. It is not the performance reference for RP2350.

`md_interp_step()` exists for mixed-mode dispatch, not as the high-throughput main interpreter loop. Normal interpreted execution remains `md_interp_run()` so the threaded dispatcher is not sacrificed for AOT interoperability.

## 4. CFG discovery must not assume linear code

`dosrecomp` first performs instruction-level reachability, then forms basic blocks from discovered branch targets and fallthrough boundaries. This avoids overlapping blocks when a backward target lands inside a region discovered earlier.

Code/data boundaries are metadata. `.COM` files often place data immediately after executable code, so `--code-start`/`--code-end` constrain analysis while the entire original image remains embedded and addressable by guest code.

## 5. Partial recompilation is success, not failure

If `dosrecomp` reaches an instruction it cannot statically decode yet, it emits a fallback point rather than rejecting the program.

Generated execution uses guest `CS:IP` as the common rendezvous:

```text
known CS:IP   -> generated block
unknown CS:IP -> interpreter
interpreter reaches known CS:IP -> generated block
```

The first implementation retries dispatch after each fallback instruction for correctness and simplicity. The decoded-block cache will move that boundary to whole interpreted blocks for speed.

## 6. Preserve 8086 real-mode semantics where they matter

The guest address calculation is `(segment << 4) + offset`, wrapped to 20 bits. Unclaimed software interrupts fall through to the real IVT and push FLAGS, CS, and IP as an 8086 interrupt does.

DOS software can install interrupt handlers. Native BIOS/DOS acceleration is an interception layer, not an excuse to delete the underlying mechanism.

## 7. Do not emulate a PC unless software requires a device

The target is DOS compatibility, not a cycle-accurate IBM PC motherboard.

Avoid emulating 8259/8237/PIT/CGA hardware when a DOS/BIOS boundary can be implemented natively. Hardware emulation is added only for software that directly depends on a device interface.

## 8. Pico Plus 2 first

The first hardware target is the Pimoroni Pico Plus 2. External PSRAM is intended to back the full 1 MiB DOS physical address space. RP2350 internal SRAM remains available for stacks, hot runtime state, display/audio buffers, decoded-block caches, and device queues.

## 9. MicroRender / MicroWave / MicroConsole are backends

The CPU/recompiler runtime stays platform-neutral. Pico integration will connect BIOS/device services to:

- MicroRender for display;
- MicroWave for audio;
- MicroConsole/catBUS for controls, SD, and hardware integration.

This keeps the execution core testable on the desktop without hardware.

## 10. Decoded interpretation is a separate optimized tier

Milestone 2 adds `MdBlockCache` rather than replacing the canonical opcode interpreter. The three layers have distinct jobs:

```text
AOT             fastest known code
block cache     fastest general interpreted code
opcode interp   canonical decoder / unsupported fallback
```

The cache is caller-owned and fixed-size. It performs no allocation while executing and is small enough to live in RP2350 internal SRAM. A direct-mapped table is intentional for the first implementation: lookup is cheap, replacement is deterministic, and correctness never depends on retaining a block.

Decoded blocks may use semantically exact super-ops. The first is `DEC r16` + `JNZ rel8`, a common loop shape. It counts as two guest instructions even though the cached executor handles it as one decoded operation. If an instruction budget ends between the two guest instructions, execution falls back to the canonical single-step interpreter for the boundary case.

Hot self-loops chain directly without hashing the guest `CS:IP` again on every iteration. More general edge chaining should be added only when it improves real workloads without slowing this hot path.

## 11. Code-cache correctness uses generations

`MdRuntime::code_epoch` is advanced whenever a new COM image is loaded and can be advanced explicitly with `md_runtime_invalidate_code()`. Cached blocks record the epoch in which they were decoded, so stale entries become instant misses without clearing the whole table.

This is deliberately coarse. Automatic page-granular executable-write tracking is still required before cached interpretation is safe for arbitrary self-modifying programs. Until that exists, software that modifies executable bytes without notifying the runtime must use the canonical interpreter or explicitly invalidate the cache.

## 12. Executable writes are tracked at 4 KiB page granularity

Milestone 3 replaces manual invalidation as the normal self-modifying-code mechanism. `MdRuntime` owns 256 executable-page flags and 256 generation counters, covering the complete 1 MiB real-mode physical address space with 4 KiB pages.

A decoded block snapshots the generation of the source page(s) it covers. A guest memory write increments a generation only when the destination page has previously been marked executable. Ordinary data, stack, framebuffer, and DOS-memory writes therefore do not churn the code cache unless those pages also contain executed code.

The page size is intentionally coarse. A 4 KiB page costs only about 1.25 KiB of runtime tracking metadata for the complete address space, which is a better RP2350 tradeoff than an 8+ KiB fine-grained table. Correctness does not depend on the granularity; smaller pages can be evaluated later from real workloads.

## 13. AOT invalidation is conservative before it becomes clever

Decoded blocks have precise page generations. Generated AOT currently uses a coarser image-wide rule: `dosrecomp` marks the static compiled code range executable and snapshots `code_write_epoch`. If any compiled code page is written, that generated invocation stops entering AOT and continues in the cached interpreter.

This is intentionally conservative. It guarantees that self-modifying code, overlays, patchers, and code/data aliasing cannot execute stale native translations. Future per-AOT-block page-version tables may recover native execution for untouched compiled pages, but only after profiling shows the extra metadata and dispatch checks are worthwhile.

Memory-writing instructions are also execution boundaries for stale-code safety. Cached blocks terminate after a write-capable instruction, and generated straight-line `PUSH`/direct-memory stores verify the write epoch before executing another compiled instruction.

## 14. AOT fallback is block-granular

Generated code no longer has to interpret exactly one unknown instruction and immediately retry the AOT dispatcher. When a runtime-owned `MdBlockCache` is attached, generated fallback calls `md_interp_run_cached_until()` and remains in the cached interpreter across an arbitrary unknown region.

The cache returns `MD_STOP_NONE` without consuming the current instruction when guest `CS:IP` reaches a known, still-valid AOT block. Generated dispatch then resumes native execution. If the AOT image has been invalidated by a code write, the resume predicate refuses all native targets and the cache owns execution until the program stops.


## 15. Real DOS bring-up is binary-first and source-assisted

Milestone 4 pins Microsoft/MS-DOS commit
`2d04cacc5322951f187bb17e017c12920ac8ebe2` and treats the released
`v2.0/bin/MSDOS.SYS` and `v2.0/bin/COMMAND.COM` as executable truth. The released
assembly, linker files, and documentation are metadata for understanding names,
layout, initialization contracts, and indirect targets.

A separate structural decoder exists because semantic support and instruction-boundary
knowledge are different problems. `dosprobe` must be able to walk an instruction that
the runtime cannot execute yet; otherwise the first missing opcode would hide every
reachable block behind it and give a misleading implementation priority list.

`COMMAND.COM` is analyzed with image base/entry `0100h`. `MSDOS.SYS` is a raw image
with image base/entry `0000h`; its released first instruction jumps to the linked
`DOSINIT` region. This does **not** mean the kernel can be invoked like a COM program.
The OEM/SYSINIT register/device-chain contract must be constructed explicitly before
real kernel execution begins.

Recursive-descent coverage is intentionally reported as an analysis frontier, not as
a percentage of bytes proven to be executable code. Indirect control transfers and
source-known alternate entries will expand that frontier over time.
