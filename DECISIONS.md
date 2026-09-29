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


## M10: DOS owns FAT and file loading

The desktop host may construct and expose a FAT12 block image, but it must not satisfy
DOS path, directory, handle, or file-read operations by translating them to the host
filesystem. The DOS-facing native boundary is sectors only. This keeps the same
contract usable for a host image, Pico SD storage, RAM disk, and later physical media.

`boot msdos2` remains the minimal kernel/DOSINIT regression. `run dos2` is the
full-system bring-up path and may advance beyond the synthetic SYSINIT return boundary.
The first post-DOSINIT step is a guest-side INT 21h OPEN/READ/CLOSE smoke test for the
released `COMMAND.COM`; EXEC is intentionally deferred until sector/FAT behavior is
proven independently.


## M11: prove EXEC before interactive shell I/O

M11 stops when the released DOS EXEC path transfers to the released
`COMMAND.COM:0100h`, after verifying the complete loaded image and PSP. It does not
add host-side COM loading or filesystem shortcuts. Keyboard/interactive shell work
is deliberately deferred until the DOS process loader is independently proven.

## 23. Host console is a callback boundary, not a DOS shortcut

M12 keeps keyboard input below the DOS character-device interface. The desktop
host provides nonblocking peek, blocking read, output, and flush callbacks;
MS-DOS still performs its own cooked/raw console semantics. `Ctrl+]` is reserved
only as a host escape. The same callback boundary is intended for Pico/catBUS.

## 24. DOS disk images persist until explicitly rebuilt

`md.bat run dos2` no longer recreates the FAT12 image on every invocation. This
allows DOS writes to persist across interactive sessions. `md.bat image dos2`
is the explicit reset operation and reconstructs the image from the pinned
released `COMMAND.COM`.


## 25. Captured DOS diagnostics keep native stdout live

The PowerShell capture runner must not assign the result of a function that invokes a
native child process, because PowerShell treats that child's stdout as function output.
Doing so collapsed the complete DOS session into the `run exit` variable. The runner
now streams `md.bat` output directly to the console/transcript and communicates only
the exit status through a script-scoped variable. This keeps `_kbhit/_getch` attached
to the real console and preserves line-oriented diagnostics in `logs\latest.txt`.

## 26. The FAT12 image is persistent guest state

A normal `md.bat clean` removes compiler/recompiler/analysis outputs but deliberately
preserves `build-disk\msdos2.img`. `md.bat image dos2` is the explicit disk reset,
while `md.bat clean all` removes the disk as well. This prevents the standard
clean-build-test-run capture workflow from silently destroying DOS filesystem changes.

## 27. Diagnose the first impossible console byte, not the resulting flood

The first M12 interactive run showed that DOS reached `COMMAND.COM` correctly but then
printed literal `$` terminators and adjacent resident/transient data. Console write-call
and byte counters were essentially one-to-one, so the initial hypothesis is not a giant
host-side transfer count. M12.1 therefore latches the first post-COMMAND `$` reaching
CON and, when `MICRODOS_TRACE_FIRST_DOLLAR=1`, dumps a bounded ring of the preceding
192 guest instructions plus request, stack, register, and flag state. The diagnostic
then stops immediately rather than allowing the corrupted output path to consume the
entire instruction budget.


## 28. Trace character-device corruption at both sides of INT 21h/AH=40h

The M12.1 captures reproduced the first bad `$` with both a preserved and freshly
rebuilt FAT12 image. DOS passed that byte to CON in a one-character request, so the
native CON backend is not extending a caller buffer. M12.2 keeps the bounded first-`$`
stop but also snapshots the most recent user `INT 21h/AH=40h` write and recognizes the
released COMMAND.COM `REPNZ SCASB / NEG CX / DEC CX / DEC CX / AH=40h` STRING_OUT
sequence. The trace records whether `$` was already inside the caller's CX or was the
first byte beyond it, and checks the REP scan's actual post-CX/DI against the expected
8086 result. This distinguishes a COMMAND-side scan/count error from a DOS write-loop
overrun without changing guest behavior.

## 29. Standard handles come from SYSINIT's real OPEN/XDUP contract

The M12.2 capture proved COMMAND.COM's `STRING_OUT` scan was exact (16 bytes,
terminator excluded) yet the kernel's cooked console loop `WRCONLP`
(`MSDOS.SYS 1000:18D8`) was entered with CX=0800h: the 17th byte (`$`) was
output with the loop counter at 07F0h = 0800h - 16.

Root cause: in DOS 2, handle I/O is FCB record I/O. `$Write` calls
`$FCB_RANDOM_WRITE_BLOCK` with CX as a *record count* on the FCB embedded in the
SFT entry, and `SETUP` multiplies by `fcb_RECSIZ`, substituting 128 when it is
zero. Only `$Open` sets `sf_FCB.fcb_RECSIZ = 1` ("byte io only"). DOSINIT merely
points JFN 0/1/2 at a bootstrap SFT entry 0 so internal console messages work;
Microsoft's SYSINIT then closes those handles, OPENs `\DEV\CON`, and XDUPs it to
STDOUT/STDERR before EXEC. The M11 continuation skipped that step, so every
COMMAND.COM handle write was multiplied by 128.

The guest-side continuation now performs SYSINIT's exact sequence: CLOSE 0,
CLOSE 2..FILES+1, OPEN `\DEV\CON` (read/write), CLOSE 1, XDUP, XDUP, OPEN
`\DEV\AUX`, OPEN `\DEV\PRN`, then EXEC. A failed CON OPEN or XDUP stops through a
dedicated native vector (`F5h`) rather than executing COMMAND.COM on the
bootstrap SFT entry. AUX/PRN failures are ignored (SYSINIT falls back to NUL).

General rule: when the host stands in for a Microsoft init component, it
reproduces that component's DOS calls, not just its final register state.

## 30. Interactive runs are unbounded; idle is a host concern

A finite instruction budget is right for regression runs and wrong for a
shell: DOS legitimately spends unbounded time polling CON at a prompt. Budget 0
now means unlimited and is the `run dos2` default. `boot msdos2` and tests keep
finite budgets.

Idle detection lives in the host console callbacks, not in the DOS device
layer: after 256 consecutive empty CON polls with no console output, console
input, or disk transfer in between, each further empty poll sleeps ~1 ms.
DOS's own ^C checks during output never reach the threshold because every
write resets the counter. The Pico backend can map the same point to a
low-power wait without changing the DOS request-packet code.

## 31. DOS conformance is tested from inside DOS

`DOS2TEST.COM` runs at the real `A>` prompt and checks INT 21h results
itself, so it exercises the released kernel, COMMAND.COM's EXEC path, the
interpreter, and the CON/CLOCK/DISK device boundary together. Host-side
counters (bytes written, sectors written) remain the oracle for the device
layer; DOS2TEST is the oracle for guest-visible behavior. It leaves the
persistent image as it found it, so it can be re-run on the same disk.

## 32. The disk image is written through, sector by sector

The FAT12 image is persistent guest state (#26), so its durability must not
depend on how the host process ends. The first interactive COPY CON session
lost its file because the image was saved only after a clean Ctrl+] exit and
the user left with Ctrl+C, which Windows turned into process termination.

Each DOS sector write now updates the in-memory image and the image file
immediately (fseek/fwrite/fflush). The whole-image save at exit remains only
as a fallback when a write-through fails. This matches a real floppy: once the
block driver returns DONE, the sector is on the medium.

## 33. Ctrl+C is guest input, not a host signal

On a PC, Ctrl+C is a DOS keystroke (03h, handled by DOS's ^C checks and
INT 23h). The Windows runner therefore clears ENABLE_PROCESSED_INPUT for the
session so Ctrl+C reaches `_getch()` as 03h, and restores the console mode at
exit. Host-level interruption is Ctrl+] (normal) or Ctrl+Break / window close
(emergency); both set a flag the main loop polls, so every stop path runs the
same shutdown code. POSIX builds treat SIGINT the same way.

## 34. AOT invalidation is byte-exact per image

Decision #13 disabled an entire generated image on the first write to any of
its 4 KiB pages. Real `.COM` programs keep variables beside their code, so
DOS2TEST would have lost native execution on its first counter update.

Each generated image now carries a one-bit-per-byte map of the instructions
it compiled. Attaching an image registers an `MdAotGuard` (linear base, size,
map) with the CPU. The existing write-tracking hook, which already runs only
for executable pages, checks registered guards and clears `valid` when a
written byte is compiled code. Generated code checks `valid` after every
store it performs, so self-modification is caught before the next compiled
instruction (the selfmod fixture still passes unchanged). Writes by DOS, the
interpreter, or the disk driver go through the same hook, which is how a
program loaded over a previous one at the same segment is never run with
stale native code.

## 35. dosrecomp discovers with the shared decoder; unsupported code is a hole

Discovery and emission are separate problems (#15). `dosrecomp` now uses
`md_decode_8086` for boundaries and control flow, so the whole program is
discovered even where the emitter has no translation. Untranslated
instructions become holes: compiled code hands CS:IP to the interpreter and
re-enters at the instruction after, which is always a block entry. A block
that begins with a hole is never a dispatch entry, so entering can always
make progress.

Emission uses the same `ops.h` semantics as the interpreter (the ALU dispatch
moved there so there is one implementation). Correctness is checked three
ways: the existing fixtures, DOS2TEST's own assertions, and a differential
run with `MICRODOS_NO_AOT=1` whose guest output must be identical.

## 36. Attach, don't load: DOS owns program loading

The runner never loads a compiled program itself. DOS reads the file, builds
the PSP, and jumps to `XXXX:0100`; the runner then compares the bytes there
with the embedded image and attaches on an exact match. This keeps EXEC, the
PSP, memory ownership, and return codes fully DOS-controlled, lets multiple
copies (a program EXECing itself) attach at different segments, and means a
modified or different file simply runs interpreted.

Entry points that static analysis cannot find are supplied as source-derived
metadata (`DOS2TEST.entries`, generated from the NASM listing), in line with
the source-assisted design (#15).

## 37. One portable system loop, proven on the desktop before hardware

The desktop runner single-steps for tracing, which is the wrong shape for the
RP2350. `md_dos2_system_run()` instead runs slices through the decoded-block
cache and returns to the caller only at a slice boundary or when execution
reaches a place where compiled code may run (a compiled entry in an attached
segment, or `XXXX:0100` where DOS may just have started a known program).

The Pico firmware is a thin wrapper around that loop. Before any hardware
run, the identical loop is exercised by `microdos_dos2_e2e`, which boots the
released kernel, drives the prompts with scripted keys released only after
the prompt text appears (so DOS input flushes cannot eat them), and requires
DOS2TEST to pass with and without AOT. This was also the first full DOS
session run through the block cache.

## 38. Pico memory layout: guest and disk in PSRAM, code in flash

The whole 1 MiB guest space and the working copy of the disk live in PSRAM
(`__uninitialized_psram`, so they do not inflate the UF2), placed and checked
the same way microconsole's FastDoom PSRAM probe does. Runtime state and the
decoded-block cache stay in SRAM. Interpreter, generated code, the kernel
and the disk image run/read from flash through XIP.

Known performance risk, deliberately measured before optimising: flash code
and PSRAM data share the RP2350's 16 KiB XIP cache, and the DOS kernel keeps
data beside its code, which causes frequent decoded-block invalidations
(about 62k in one DOS2TEST session on the desktop). The first hardware run
reports MIPS and cache statistics (Ctrl+]) so the next step, moving hot
interpreter paths to SRAM or refining invalidation, is driven by numbers.
Disk persistence to flash is a later milestone; this one keeps writes in RAM.
