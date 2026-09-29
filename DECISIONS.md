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


## 16. Real-DOS opcode priority beats numeric opcode completeness

Milestone 5 implements the first semantic expansion directly from the pinned
`MSDOS.SYS` / `COMMAND.COM` `dosprobe` histogram instead of filling opcode numbers in
order. Canonical interpretation is the first landing point for each family; cache
specialization and AOT follow only after those semantics are tested.

The first tranche is deliberately coherent: segment-register transfers and stack
ops, the eight ALU operations across register/memory and immediate forms, C6/C7
immediate stores, and all short Jcc predicates. This raises the same initial
recursive-descent frontier to about 77.54% interpreter support for `MSDOS.SYS` and
88.01% for `COMMAND.COM` while leaving prefixed instructions unsupported.

Logic instructions deterministically clear AF in microDOS even though real 8086 AF
is architecturally undefined for AND/OR/XOR. Software must not depend on undefined AF;
the deterministic choice makes differential tests reproducible.


## 17. Prefix and string semantics stay canonical before optimization

Milestone 6 adds real 8086 prefix state to the canonical interpreter. Segment overrides
select ES/CS/SS/DS for ordinary memory operands and for the source side of MOVS/CMPS/
LODS; the destination side of MOVS/CMPS/STOS/SCAS remains fixed to ES as on 8086. The
last segment prefix wins. REPNE and REP/REPE are retained as instruction-local state;
LOCK is accepted as a no-op synchronization prefix because microDOS currently executes
a single guest CPU.

REP on MOVS/STOS/LODS repeats until CX reaches zero. REP/REPE and REPNE on CMPS/SCAS
stop according to ZF after each comparison. A repeated string operation still counts as
one architectural guest instruction in the current instruction-budget model; interrupt
windows inside REP are a later timing/interrupt-model concern.

The decoded cache intentionally falls back to this canonical implementation for these
new families in milestone 6. That keeps prefix parsing, direction behavior, repeat
termination, and self-modifying writes in one correctness implementation before adding
predecoded string super-ops or AOT emission.

On the pinned MS-DOS 2.0 recursive-descent frontier this tranche brings estimated
canonical interpreter support to 3086/3446 (89.55%) for MSDOS.SYS and 595/617
(96.43%) for COMMAND.COM. The remaining concentration is LES/LDS, shifts/rotates,
Group 3, Group 4/5, POP r/m, XCHG/LEA, and a small set of flag/stack/control opcodes.


## 18. Complete the discovered canonical frontier before boot scaffolding

Milestone 7 implements every opcode family still present on the pinned DOS 2.0
recursive-descent frontier before constructing the OEM/SYSINIT boot environment.
This includes LES/LDS, rotate/shift, Group 3, Group 4/5, POP r/m, XCHG/LEA,
flag-stack/control instructions, TEST, CBW/CWD, IRET, direct/far control transfer,
and the remaining small original-8086 operations needed to avoid semantic holes.

`dosprobe` therefore reports 100% canonical interpreter support for both currently
discovered frontiers: 3446/3446 reachable `MSDOS.SYS` instructions and 617/617
reachable `COMMAND.COM` instructions. This is explicitly **frontier completeness**,
not whole-program reachability proof. The five unresolved kernel indirect transfers
and three COMMAND.COM indirect transfers can reveal additional code once runtime
execution or source-assisted target recovery resolves them.

New families remain canonical-interpreter-first. The decoded cache and AOT emitter
are not bulk-expanded merely to make percentages look symmetric; they will promote
operations only after real DOS execution identifies hot regions and correctness is
stable.

The original 8086 `PUSH SP` quirk is architectural behavior, not an optimization
corner case. `md_x86_push_reg()` centralizes it so canonical interpretation, decoded
cache execution, and generated AOT all push the post-decrement SP value for register
SP while preserving ordinary PUSH behavior for the other registers.



## 19. DOSINIT is entered through the real SYSINIT/OEM contract

Milestone 8 stops treating kernel bring-up as an abstract future task. The host runner
loads the released `MSDOS.SYS` as a raw image, enters offset `0000h`, and recreates the
state that Microsoft's `SYSINIT` establishes immediately before its far call: DS:SI
points at the OEM device list, DX is the memory limit in paragraphs, and the caller's
far-return address remains on the original stack while DOSINIT temporarily switches to
its own stack.

The OEM devices are real guest-memory DOS 2.x device headers. Strategy and interrupt
entry points are tiny far-callable real-mode trampolines into private native interrupt
hooks. This preserves DOS's own `DEVIOCALL2` path and request-packet behavior instead
of replacing device initialization with host-side special cases inside the kernel.

M8 implements only DEVINIT. Character initialization completes successfully; the block
INIT response returns one unit and a 360 KiB FAT12 BPB. All other request functions
return an explicit unknown-command error and are counted/reported by the bring-up
runner. The next implementation work is therefore driven by requests actually made by
the released kernel rather than by speculative BIOS completeness.

`md_runtime_load_raw()` is the permanent non-COM loader primitive introduced for this
path. It shares the same 20-bit guest memory, code-page marking, self-modification
tracking, hooks, and interpreter state as COM execution.

## 20. Fetch side effects must never share an unsequenced C expression with IP

The first M8 raw-kernel run exposed a host-language correctness bug in the canonical
interpreter before DOSINIT itself ran. The released kernel begins `E9 78 3E`; 8086
semantics require the relative displacement to be added to IP *after* the two-byte
immediate has been consumed, producing target `3E7Bh`.

The old E9/EB implementation combined `cpu->ip` and `md_fetch16/md_fetch8()` in one C
expression. C does not sequence those operand evaluations, so one compiler observed
IP before the fetch side effect and produced `3E79h`. E9, EB, and the threaded EB hot
handler now fetch the displacement into a local first and only then add it to the
post-fetch IP.

The exact DOS entry bytes are a permanent regression test. More generally, decoder
fetch helpers are treated as state-changing operations and must be sequenced in their
own statements whenever the same expression also depends on IP or another value they
can mutate.


## 21. DOS character-device status is modeled, not approximated

The first real M8 DOSINIT run initialized CON/AUX/PRN/CLOCK/DISK and then entered a
repeat loop of CON request functions 5, 10, and 8 because the bring-up shim returned
`8103h` (error + done + unknown command) for all non-INIT requests. That behavior was
useful as a boundary detector but is not a valid console driver.

M9 follows the DOS 2 request contract instead: non-destructive read with no character
available returns `0300h` (busy + done), output status returns `0100h`, and character
writes consume the request packet transfer pointer and count. This is important because
DOS uses status bits as control flow; treating "not ready" as an error changes kernel
behavior and can create artificial retry loops.

## 22. Native DOS devices expose platform callbacks, not host stdio

The DOS-facing CON implementation does not depend on stdio. `MdMsdos2Boot` carries
write/peek/read/flush callbacks. The Windows runner currently binds write to stdout,
while a Pico 2 backend can later bind the same interface to MicroConsole/MicroRender.
Destructive input is not invented when no callback exists: such a request remains an
explicit unsupported boundary until a platform input source is connected.

High-frequency successful console polls/status calls are not logged individually, and
repeated error traces are capped. Counters remain authoritative, so diagnostics stay
useful without drowning the actual DOS output.
