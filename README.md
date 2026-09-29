# microDOS

microDOS is a source-assisted 8086 recompilation and execution project targeting the RP2350 / Pico 2 family, with the Pimoroni Pico Plus 2 as the first hardware target.

The project has two permanent execution paths sharing one architectural state and one instruction-semantics layer:

1. **AOT recompilation** — known DOS binaries are analyzed by `dosrecomp`, translated into portable C, and compiled natively for the target in the spirit of N64Recomp-style static recompilation.
2. **High-performance 8086 interpretation** — unknown, indirect, dynamically generated, self-modifying, or not-yet-recompiled code runs through the interpreter and can return to compiled blocks.

The interpreter is not a disposable fallback. microDOS now has both the original opcode interpreter and a caller-owned decoded basic-block cache. The cache removes repeated instruction decoding, supports compact super-ops, directly self-chains tight loops, and falls back to the canonical interpreter for instructions it cannot predecode.

## Current status

The recompiler and the first cached-interpreter layer are both operational.

```text
                     +--> AOT block -----------------------+
.COM -> dosrecomp ---+                                     |
                     +--> fallback --> opcode interpreter -+--> MdRuntime
                                                           |
unknown guest code --> decoded block cache ----------------+
                         |       ^
                         +-------+
                        hot chain
```

Current validation covers:

- mechanically recompiled `Hello from microDOS!` through `INT 21h`;
- the same program through baseline interpretation, cached interpretation, and AOT;
- backward-branch CFG splitting with a 65,535-iteration loop;
- decoded-block cache hits/misses and direct self-chaining;
- a fused `DEC r16` + `JNZ` super-op with exact two-instruction accounting;
- cached interpreter -> canonical interpreter -> cached interpreter fallback;
- AOT -> interpreter -> AOT handoff on deliberately unsupported AOT code;
- instruction-budget boundaries, including stopping inside a fused super-op;
- coarse code-generation invalidation;
- 8-bit register aliasing and 20-bit real-mode address wrapping;
- real IVT fallback semantics.

The interpreter, cache executor, and generated C all share `include/microdos/ops.h` for arithmetic/FLAGS behavior.

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

Expected host output now includes all three execution modes:

```text
interp: stop=exit instructions=5 exit=0
cache:  stop=exit instructions=5 exit=0 blocks=2 fallback=0
recomp: stop=exit instructions=5 exit=0
output: Hello from microDOS!
```

## Performance regression harness

`microdos_bench` runs the same 131,072-instruction 8086 register/branch workload through baseline interpretation, cached interpretation, and AOT. Host MIPS are regression numbers only; they are not RP2350 estimates.

A representative GCC Release run for this milestone:

```text
interp  ~245 MIPS
cache   ~346 MIPS
aot     ~499 MIPS
```

With the normal interpreter deliberately forced to portable switch dispatch, representative results were:

```text
interp  ~186 MIPS
cache   ~389 MIPS
aot     ~503 MIPS
```

That second comparison is useful for the Windows/MSVC build: the decoded cache does not depend on GNU computed-goto support and is intended to become the fast portable interpreted path.

The default cache is about 17.5 KiB (`128` direct-mapped blocks, up to `16` decoded ops per block), is caller-owned, and performs no allocation during execution. Both dimensions are compile-time configurable.

## Code invalidation

`MdRuntime::code_epoch` provides coarse cache invalidation. Loading a new `.COM` automatically advances the epoch. `md_runtime_invalidate_code()` is available when code memory is changed explicitly.

Automatic page-granular invalidation for arbitrary self-modifying guest writes is the next cache milestone. Until then, the baseline interpreter remains the correctness path for workloads that modify executable memory without notifying the runtime.

## Direction

```text
8086 binary
   |
   +--> dosrecomp analyzer --> portable C --> native ARM/RISC-V
   |
   +--> opcode interpreter
   |
   +--> decoded-block cache
          +--> super-ops
          +--> direct hot-block chaining
          +--> opcode fallback

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

See `docs/ARCHITECTURE.md`, `DECISIONS.md`, and `tools/dosrecomp/README.md`.
