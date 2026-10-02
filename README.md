# microDOS

M23 adds an interpreter-first execution router with optional selective hot-region
promotion. See [implementation and rollout](docs/M23_IMPLEMENTATION.md) and the
[design proposal](docs/M23_UNIFIED_TIERED_ENGINE_DESIGN.md). Promotion is disabled
by default until Pico phase benchmarks validate it.

microDOS is a source-assisted 8086 recompilation and execution project targeting the RP2350 / Pico 2 family, with the Pimoroni Pico Plus 2 as the first hardware target.

The project has two permanent execution paths sharing one architectural state and one instruction-semantics layer:

1. **AOT recompilation** — known DOS binaries are analyzed by `dosrecomp`, translated into portable C, and compiled natively for the target in the spirit of N64Recomp-style static recompilation.
2. **High-performance 8086 interpretation** — unknown, indirect, dynamically generated, self-modifying, or not-yet-recompiled code runs through the interpreter and can return to compiled blocks.

The interpreted path itself has two tiers: a fast decoded basic-block cache and the canonical opcode interpreter used for unsupported instructions and exact boundary cases.

## Current status

Milestone 10 moves beyond kernel initialization into the real DOS filesystem path.
The released `MSDOS.SYS` already completes `DOSINIT` and prints its own MS-DOS 2.00
banner through the native CON device. M10 keeps that kernel-only path as a permanent
regression test and adds a separate full-system bring-up path.

The native DOS 2 block device now implements MEDIA CHECK, BUILD BPB, sector READ,
sector WRITE, and WRITE+VERIFY using a platform-neutral block callback. The desktop
host binds that callback to a 360 KiB in-memory FAT12 disk image; a future Pico target
can bind the same sector interface to SD or other storage without changing the DOS
request-packet layer.

`mkfat12` builds a deterministic 720-sector image containing the released
`COMMAND.COM`. DOS owns the filesystem: the host never translates DOS path or handle
operations into host filesystem calls. After `DOSINIT` returns, a tiny guest-side
SYSINIT continuation invokes real INT 21h OPEN/READ/CLOSE on `A:\COMMAND.COM` and
reads its first 64 bytes through `MSDOS.SYS` and the block driver. The host compares
those returned guest bytes against the pinned `COMMAND.COM` only as a validation
check.

The permanent execution architecture remains generated AOT for known native blocks,
a high-performance decoded cache for general legacy code, and one canonical 8086
interpreter/semantics layer for correctness and fallback.

## MS-DOS 2.0 reference bring-up

Fetch the exact pinned Microsoft reference tree, analyze it, and enter the real
MS-DOS 2.0 kernel:

```bat
.\md.bat deps msdos
.\md.bat analyze dos2
.\md.bat boot msdos2
.\md.bat image dos2
.\md.bat run dos2
```

This analyzes `v2.0/bin/MSDOS.SYS` as a raw image at `0000h` and
`v2.0/bin/COMMAND.COM` as a COM image at `0100h`. JSON reports are written under
`build-analysis/`. The upstream checkout is local and ignored by Git.

The structural decoder is intentionally separate from instruction semantics: it can
recover lengths and direct control flow for the full 8086 map, flag possible 80186+
paths, and compare reachable code against what the current interpreter and AOT
emitter can actually execute. See `docs/MSDOS2_BRINGUP.md`.

On the same pinned recursive-descent frontier, canonical interpreter coverage has
progressed from **55.17% -> 77.54% -> 89.55% -> 100%** for `MSDOS.SYS` and from
**66.61% -> 88.01% -> 96.43% -> 100%** for `COMMAND.COM` across the bring-up
milestones. This means every instruction on the *currently discovered static
frontier* has canonical semantics; it does not mean every dynamically reachable
DOS block has been discovered yet. Indirect targets and source-assisted alternate
entries will expand the frontier during real execution. AOT coverage intentionally
remains conservative while semantics stabilize.

## Build

Windows:

```bat
.\md.bat build host
.\md.bat run host
.\md.bat test
.\md.bat bench 1000
.\md.bat boot msdos2
```

Recompile a `.COM` manually:

```bat
.\md.bat recomp tests\programs\hello.com hello 0x10c
```

Portable CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/microdos_host
./build/microdos_bench 1000
```

Expected host output includes all three execution modes:

```text
interp: stop=exit instructions=5 exit=0
cache:  stop=exit instructions=5 exit=0 blocks=2 fallback=0
recomp: stop=exit instructions=5 exit=0
output: Hello from microDOS!
```

## Real DOSINIT runner

`microdos_msdos2` is intentionally a bring-up executable, not a simulator shell. It
loads `third_party/msdos/v2.0/bin/MSDOS.SYS` as a raw binary at segment `1000h`,
constructs the DOS 2 OEM device-header chain at segment `0800h`, sets `DX=A000h`
(640 KiB), and recreates the far-return frame that SYSINIT would have placed on the
stack before `CALL MSDOS`.

Run it through the front door:

```bat
.\md.bat boot msdos2
```

An optional second argument is the canonical-interpreter instruction budget:

```bat
.\md.bat boot msdos2 5000000
```

The runner reports entry into `DOSINIT`, device initialization and failures, final CPU
state, interpreter faults, budget stops, and whether the kernel returned through the
synthetic SYSINIT return address. Successful high-frequency console status/poll calls
are intentionally not printed one-by-one; DOS character output is written directly
through the host console callback. Repeated device errors are capped in the trace so a
missing service cannot produce megabytes of duplicate diagnostics.

## Performance regression harness

`microdos_bench` runs the same 131,072-instruction 8086 register/branch workload through baseline interpretation, cached interpretation, and AOT. Host MIPS are regression numbers only; they are not RP2350 estimates.

The cache now has a dedicated hot executor for single-op `DEC/JNZ` blocks, avoiding the generic decoded-op switch on that common loop shape. A representative GCC Release run for milestone 3 is roughly:

```text
interp  ~250 MIPS
cache   ~400 MIPS
aot     ~520 MIPS
```

With the baseline interpreter deliberately forced to portable switch dispatch:

```text
interp  ~190 MIPS
cache   ~415 MIPS
aot     ~525 MIPS
```

The latest validated MSVC milestone-6 run on the primary Windows development machine is:

```text
interp  105.19 MIPS
cache   221.78 MIPS
aot     445.82 MIPS
```

All three tiers moved down together relative to the preceding run, including AOT,
so that cross-run change is treated as host timing/frequency variance rather than a
prefix/string hot-path regression. The benchmark remains a regression harness, not
a stable machine rating.

The default cache is caller-owned, fixed-size, and performs no allocation while executing. Both slot count and maximum decoded operations per block are compile-time configurable.

## Self-modifying code and invalidation

The 1 MiB guest address space is divided into 4 KiB code-tracking pages. Decoding a block marks the source pages executable and snapshots their generation counters. Guest writes through the normal 8086 memory helpers increment a page generation only when the destination page contains executable code.

```text
guest write
    |
    +--> ordinary data page ----> no cache work
    |
    +--> executable page
             |
             +--> page generation++
             +--> code-write epoch++
```

Decoded blocks validate only the one or two pages containing their source bytes, so unrelated code remains cached.

Generated AOT is deliberately more conservative: `dosrecomp` marks its static code range executable and snapshots the global code-write epoch. If guest code writes to any compiled code page, the generated dispatcher stops using that AOT image and remains in cached interpretation. This guarantees correctness before we attempt finer per-AOT-block versioning.

Memory-writing decoded blocks end at the write boundary so a self-modification cannot leave stale decoded instructions later in the same cached block. Generated straight-line stores and pushes similarly check the code-write epoch immediately before executing another compiled instruction.

## Direction

```text
8086 binary
   |
   +--> dosrecomp analyzer --> portable C --> native ARM/RISC-V
   |                               ^
   |                               |
   |                        valid AOT target
   |                               |
   +--> decoded-block cache -------+
   |       +--> super-ops
   |       +--> hot chaining
   |       +--> page validation
   |       +--> opcode fallback
   |
   +--> canonical opcode interpreter

all paths
   |
   v
shared MdRuntime / MdX86 / instruction semantics
   |
   +--> recompiled MS-DOS
   +--> BIOS/device boundary
           +--> MicroRender
           +--> MicroWave
           +--> MicroConsole / catBUS
```

The original MS-DOS source is source-level metadata and documentation; the binary remains the behavioral authority for recompilation.

See `docs/ARCHITECTURE.md`, `docs/MSDOS2_BRINGUP.md`, `DECISIONS.md`, `tools/dosrecomp/README.md`, and `tools/dosprobe/README.md`.


### MS-DOS 2.0 milestone 11

The current `run dos2` path asks the released DOS 2 kernel to `EXEC` the released
`COMMAND.COM` from the generated FAT12 image. Success requires DOS to allocate the
child, build a valid PSP, load the complete COM image, and transfer to offset
`0100h`; the runner verifies the full loaded image and `/P` PSP command tail before
stopping at the entry boundary.


### Interactive DOS 2 shell bring-up (M12)

After the M11 EXEC milestone, `md.bat run dos2` now continues into the released
`COMMAND.COM` with an interactive host console. Keyboard input reaches DOS
through the normal CON request packets; `Ctrl+]` exits back to the host. The
CLOCK request packet is implemented as well. Existing `build-disk\msdos2.img`
is preserved between runs; use `md.bat image dos2` to reset it.

```powershell
.\md.bat run dos2
# at the DOS prompt try: VER, DIR, ECHO HELLO
# Ctrl+] returns to the host
```


### M18 hot holes compiled, cheap re-entry, lazy flags

- MUL/IMUL/DIV/IDIV, REP string instructions and far indirect CALL/JMP are
  compiled, calling the interpreter's own implementations (exported as
  `md_interp_muldiv` / `md_interp_string_op`), so semantics stay single-source.
  The kernel now has 1 hole in 6100 instructions; DOS2TEST has none.
- Re-entry: a 16-byte-bucket entry index replaces binary search, and the
  system loop enters compiled code directly instead of re-validating it.
- Lazy flags: ADD/ADC/SUB/SBB/CMP/logic/INC/DEC record operands and result;
  flags are derived when read (Jcc uses direct predicates). An exhaustive test
  proves identical flags to the old eager code (kept as
  `tests/eager_flags_ref.h`); the field is now `flags_raw`, read through
  `md_x86_flags()` / `md_x86_cf()` etc.
- Host: compiled `loop` ~460 -> ~760 MIPS, DOS2TEST session ~83 -> ~147 MIPS.
  Pico default is now 300 MHz.

### M17 the MS-DOS 2.0 kernel itself is compiled

`MSDOS.SYS` is recompiled at build time (6100 instructions discovered, 6023
compiled, 77 interpreter holes that run inline) and attached at `1000:0000`
at boot. In a
DOS2TEST session 77-86% of all guest instructions now run as compiled kernel
code. Supporting changes:

- `dosrecomp --base` (raw images), pointer tables (the INT 21h dispatch
  table), native IRET and far CALL/JMP, profile-guided entries
  (`aot/msdos2.entries`).
- Invalidation per 64-byte chunk instead of per image: DOS reuses its init
  code as buffers, which now disables only those blocks. An attachment dies
  when every compiled chunk is overwritten (another program loaded there).
- Store checks run after an instruction's last effect (the kernel exposed an
  XCHG that handed over half-done; fixture `xchgself.com`).
- Denser code: per-block budget/count accounting, compact sorted dispatch.
  The compiled kernel is 244 KB of ARM (806 KB before) and runs from SRAM.
- `dos2_e2e_lockstep`: compiled system vs pure interpreter, compared after
  every compiled unit, over the whole DOS2TEST session.

### M16 threaded DOS loop and RP2350 defaults

- `md_interp_run_until_cs_change()`: the threaded interpreter, returning when
  CS changes. The DOS system loop now runs the kernel, COMMAND.COM and
  non-compiled programs on it instead of single-stepping; compiled code can
  only become reachable through a CS change, so no AOT entry is missed.
- Direct threaded entries for the opcodes that dominate MS-DOS 2.0 (ALU
  ModR/M forms, group 1, MOV r/m, CALL/RET/JMP, LOOP, string ops, segment
  push/pop), using the same `md_op_*` semantics as the generic executor.
- Guard walks are skipped until something attaches, so DOS kernel stack
  writes (its stack shares pages with its code) cost nothing extra.
- Host: the DOS2TEST session through the system loop went from ~72 to ~103
  MIPS. Pico defaults: code in SRAM, block cache off; a 300 MHz variant.

### M15 correctness fixes and RP2350 performance matrix

- Word stores now check both bytes against AOT guards (a same-page word
  write whose second byte hit compiled code used to be missed).
- AOT attachments live in `MdRuntime` (8 slots, LRU), so they die with a
  runtime reset/init and never leak between runtimes.
- `memloop.com` joins the fixtures; a unit test requires identical memory
  and instruction counts from interpreter, block cache and AOT.
- The Pico build produces a benchmark matrix (code in flash vs SRAM, guest in
  SRAM vs PSRAM, step/threaded/cache/AOT) and four DOS variants (code
  flash/SRAM x cache on/off) with exact active-time accounting. See
  `pico/README.md`.

### M14 Pico 2 target and portable system loop

`src/system/md_dos2_system.c` is a platform-neutral MS-DOS 2.0 loop: boot
contract, decoded-block cache, and AOT attach/enter. Two things use it:

- `pico/` builds firmware for the Pimoroni Pico Plus 2 (`.\md.bat build pico`,
  see `pico/README.md`): USB serial console, 1 MiB guest memory and the disk
  in PSRAM, kernel and compiled DOS2TEST in flash.
- `microdos_dos2_e2e`, a ctest that boots DOS through the same loop, answers
  the date/time prompts, runs DOS2TEST at `A>`, and requires 25/25, once with
  compiled code and once interpreted (`dos2_e2e_aot`, `dos2_e2e_interp`).
  It runs automatically when `third_party/msdos` is present.

### M13 compiled DOS programs inside real DOS

`dosrecomp` v2 discovers code with the shared full 8086 decoder and compiles
most of the instruction set to C; the rest become explicit interpreter holes.
DOS2TEST.COM is recompiled at build time (1132 of 1140 instructions native)
and linked into `microdos_msdos2`. When you type `DOS2TEST` at `A>`, DOS
loads the file itself; the runner recognises the bytes at `XXXX:0100`,
attaches the compiled image, and enters native code at every compiled entry.
DOS calls still go through the real MS-DOS 2.0 kernel. The summary line
reports it:

```
[aot] DOS2TEST.COM attached 2 time(s), last at 1717:0100; ... (87.9% native)
```

Guest output is identical with `MICRODOS_NO_AOT=1` (pure interpreter). A
write to any compiled instruction byte disables that copy immediately; writes
to its variables do not (DECISIONS #34).

### M12.5 disk writes survive any exit; Ctrl+C belongs to DOS

Every sector DOS writes is now written straight through to
`build-disk\msdos2.img`, so files survive even if the host process is killed.
(Previously the image was saved only on a clean Ctrl+] exit, and a Ctrl+C in
the console killed the process and discarded the session's writes.)

While DOS runs, Ctrl+C is delivered to DOS as `^C`, as on a real PC.
Ctrl+] is the normal way out; Ctrl+Break (or closing the window) is an
emergency exit that still stops cleanly.

### M12.4 usable interactive shell and DOS2TEST

`md.bat run dos2` now runs with an unlimited budget by default (a budget of 0
means unlimited; Ctrl+] exits). While DOS sits at a prompt polling CON, the
host sleeps about 1 ms per poll after 256 consecutive empty polls, so an idle
prompt no longer pins a host core. MEDIA CHECK and BUILD BPB requests are
counted but never printed, and disk transfers are only printed during boot,
so DOS output is no longer interleaved with `[disk]` lines
(`capture.bat -TraceDisk` or `MICRODOS_TRACE_DISK=1` restores them). The
M12.2 `$` diagnostic is now opt-in (`capture.bat -StringTrace`).

The FAT12 image now carries `DOS2TEST.COM`, a self-checking program covering
about 50 INT 21h services, including handle and FCB file I/O, directories,
memory allocation, and EXEC. Reset the disk once to get it, then run it at
the prompt:

```
.\md.bat image dos2
.\capture.bat
A>DOS2TEST
```

See `tests/dos2/README.md`.

### M12.3 standard handles (fix for the M12 `$` flood)

The M12.2 capture showed COMMAND.COM's `STRING_OUT` count was correct and the
kernel console loop was entered with 16 x 128 = 2048 bytes. DOS 2 handle writes
are record I/O and only `$Open` sets the record size to 1; the continuation now
performs SYSINIT's real `OPEN \DEV\CON` + `XDUP` sequence before EXEC. See
`DECISIONS.md` #29. The runner summary reports this as `[system] stdio=...`.

### M12.2 upstream string-output diagnostic

The first interactive M12 run reached the released `COMMAND.COM` and printed its
`Command v. 2.02` banner, but later emitted `$` delimiters and adjacent command
interpreter data as ordinary console characters until the execution budget expired.
The host CON driver received almost entirely one-byte writes, so M12.1 does not assume
a bad request length. Instead it records the guest execution immediately preceding the
first post-COMMAND literal `$` that reaches CON.

Use the permanent capture front door:

```powershell
.\capture.bat
```

By default it runs clean -> build -> tests -> DOS and enables the bounded string
diagnostic. M12.1 proved that the bad `$` arrives at CON as a normal one-byte DOS
device request, so M12.2 additionally records the originating user `INT 21h/AH=40h`
WRITE and COMMAND.COM's immediately preceding `REPNZ SCASB` string scan. The first
post-COMMAND `$` therefore dumps: the original caller DS:DX/CX, the requested last
byte and first byte beyond the request, the STRING_OUT scan pre/post state and derived
count, plus the preceding 192 guest instructions and DOS request/stack state. It then
exits the DOS runner with code 6. The capture script allows that diagnostic exit and
writes both a timestamped transcript and `logs\latest.txt`.

For a normal interactive run without the diagnostic stop:

```powershell
.\capture.bat -NoStringTrace
```

`md.bat clean` now preserves `build-disk\msdos2.img`, because the disk image is
persistent guest state rather than a compiler build product. Use `md.bat image dos2`
to explicitly reset the FAT12 disk, or `md.bat clean all` when a complete removal of
build products *and* the disk image is desired.
