# microDOS

microDOS is a source-assisted 8086 recompilation and execution project targeting the RP2350 / Pico 2 family, with the Pimoroni Pico Plus 2 as the first hardware target.

The project has two permanent execution paths sharing one architectural state and one instruction-semantics layer:

1. **AOT recompilation** — known DOS binaries are analyzed by `dosrecomp`, translated into portable C, and compiled natively for the target in the spirit of N64Recomp-style static recompilation.
2. **High-performance 8086 interpretation** — unknown, indirect, dynamically generated, self-modifying, or not-yet-recompiled code runs through the interpreter and can return to compiled blocks.

The interpreted path itself has two tiers: a fast decoded basic-block cache and the canonical opcode interpreter used for unsupported instructions and exact boundary cases.

## Current status

Milestone 4 begins bring-up against the released MS-DOS 2.0 binaries while keeping
the milestone-3 mixed execution model block-granular and self-modifying-code safe.
The new `dosprobe` tool uses a structural 8086 decoder to walk real binaries even
when their semantics are not implemented yet, producing an opcode/flow inventory
that drives CPU-core work from actual DOS code rather than synthetic guesses.

The execution model remains:

```text
                         known + unchanged
                              |
                              v
.COM -> dosrecomp -> generated AOT blocks
                              |
                unknown / changed / indirect
                              |
                              v
                     decoded block cache
                        |           |
                  supported      unsupported
                        |           |
                        |           v
                        +---- canonical opcode interpreter
                              |
                     reaches valid AOT target
                              |
                              +----------> AOT
```

Current validation covers:

- mechanically recompiled `Hello from microDOS!` through `INT 21h`;
- the same program through baseline interpretation, cached interpretation, and AOT;
- backward-branch CFG splitting with a 65,535-iteration loop;
- decoded-block cache hits/misses and direct self-chaining;
- a fused `DEC r16` + `JNZ` super-op with exact two-instruction accounting;
- cached interpreter -> canonical interpreter -> cached interpreter fallback;
- **AOT -> cached interpreter -> AOT** block-granular handoff;
- automatic 4 KiB executable-page invalidation after guest writes;
- cache re-decode after patching an already-decoded instruction;
- conservative AOT invalidation after any write to a compiled code page;
- self-modifying AOT that patches the very next compiled instruction and correctly abandons stale generated code;
- instruction-budget boundaries, including stopping inside a fused super-op;
- 8-bit register aliasing and 20-bit real-mode address wrapping;
- real IVT fallback semantics.

The interpreter, cache executor, and generated C all share `include/microdos/ops.h` for arithmetic/FLAGS behavior.

## MS-DOS 2.0 reference bring-up

Fetch the exact pinned Microsoft reference tree and analyze the two first real
targets:

```bat
.\md.bat deps msdos
.\md.bat analyze dos2
```

This analyzes `v2.0/bin/MSDOS.SYS` as a raw image at `0000h` and
`v2.0/bin/COMMAND.COM` as a COM image at `0100h`. JSON reports are written under
`build-analysis/`. The upstream checkout is local and ignored by Git.

The structural decoder is intentionally separate from instruction semantics: it can
recover lengths and direct control flow for the full 8086 map, flag possible 80186+
paths, and compare reachable code against what the current interpreter and AOT
emitter can actually execute. See `docs/MSDOS2_BRINGUP.md`.

## Build

Windows:

```bat
.\md.bat build host
.\md.bat run host
.\md.bat test
.\md.bat bench 1000
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

The validated MSVC milestone-3 result on the primary Windows development machine is:

```text
interp  121.25 MIPS
cache   255.50 MIPS
aot     490.91 MIPS
```

That is a roughly 2.1x gain from decoded caching over the portable switch interpreter
on the same build, before AOT is used.

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
