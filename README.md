# microDOS

microDOS is a source-assisted 8086 recompilation and execution project targeting the RP2350 / Pico 2 family, with the Pimoroni Pico Plus 2 as the first hardware target.

The project has two permanent execution paths sharing one architectural state and one instruction-semantics layer:

1. **AOT recompilation** — known DOS binaries are analyzed by `dosrecomp`, translated into portable C, and compiled natively for the target in the spirit of N64Recomp-style static recompilation.
2. **High-performance 8086 interpretation** — unknown, indirect, dynamically generated, self-modifying, or not-yet-recompiled code runs through the interpreter and can return to compiled blocks.

The interpreted path itself has two tiers: a fast decoded basic-block cache and the canonical opcode interpreter used for unsupported instructions and exact boundary cases.

## Current status

Milestone 9 is the first real DOS 2.x **runtime character-device** milestone. The
released `MSDOS.SYS` now enters `DOSINIT`, initializes the complete synthetic OEM
device chain, and reaches normal DOS console traffic. The M8 run showed that the
first remaining boundary was not CPU execution at all: DOS repeatedly requested CON
functions 5 (non-destructive input), 10 (output status), and 8 (write), while the M8
shim incorrectly returned unknown-command error `8103h`.

M9 implements those requests using the real DOS 2 request-packet layout and status
semantics. With no pending input, function 5 returns `0300h` (`BUSY|DONE`) rather
than an error; output status returns `0100h`; writes consume the transfer far pointer
and byte count from the request packet and send the bytes through a platform-neutral
console callback. Input/output flush and write-with-verify are also handled according
to the DOS 2 character-device contract.

The console callback layer is intentionally platform-neutral. The Windows bring-up
runner binds output to stdout; Pico 2 can later bind the same DOS-facing device to
MicroRender/MicroConsole without changing kernel-facing code. Destructive input is
only accepted when a platform read callback is present, so unsupported input is not
silently fabricated.

The permanent execution architecture is unchanged: generated AOT for known native
blocks, a high-performance decoded cache for general legacy code, and one canonical
8086 interpreter/semantics layer for correctness and fallback.

## MS-DOS 2.0 reference bring-up

Fetch the exact pinned Microsoft reference tree, analyze it, and enter the real
MS-DOS 2.0 kernel:

```bat
.\md.bat deps msdos
.\md.bat analyze dos2
.\md.bat boot msdos2
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
