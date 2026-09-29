# microDOS

microDOS is a source-assisted 8086 recompilation and execution project targeting the RP2350 / Pico 2 family, with the Pimoroni Pico Plus 2 as the first hardware target.

The project has two permanent execution paths sharing one architectural state and one instruction-semantics layer:

1. **AOT recompilation** — known DOS binaries are analyzed by `dosrecomp`, translated into portable C, and then compiled natively for the target in the spirit of N64Recomp-style static recompilation.
2. **High-performance 8086 interpretation** — unknown, indirect, dynamically generated, self-modifying, or not-yet-recompiled code runs through the interpreter and can return to compiled blocks.

The interpreter is not a disposable fallback. GCC/Clang builds use direct-threaded dispatch for hot opcodes, direct 20-bit guest-memory access, inline register helpers, and callbacks only at interrupt/I/O boundaries. MSVC keeps a portable switch dispatcher for validation.

## Current status

The recompiler is now real rather than hand-shaped proof code.

The build compiles `tools/dosrecomp`, runs it over three `.COM` fixtures, compiles the emitted C, and differential-tests that output against the interpreter.

```text
                     +--> AOT block 0100 ----+
.COM -> dosrecomp ---+                        |
                     +--> fallback -----------+--> MdRuntime
                              ^               |
                              |               v
                              +-- interpreter-+
```

Current validation covers:

- mechanically recompiled `Hello from microDOS!` through `INT 21h`;
- backward-branch CFG splitting with a 65,535-iteration loop;
- AOT -> interpreter -> AOT handoff on deliberately unsupported AOT code;
- identical guest instruction counts between AOT and interpretation;
- AOT instruction budgets;
- 8-bit register aliasing;
- 20-bit real-mode address wrapping;
- real IVT fallback semantics.

The interpreter and generated C share `include/microdos/ops.h` for arithmetic/FLAGS behavior so the AOT path does not become a second CPU implementation.

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

Expected host output includes:

```text
interp: stop=exit instructions=5 exit=0
recomp: stop=exit instructions=5 exit=0
output: Hello from microDOS!
```

## Performance regression harness

`microdos_bench` runs the same tight 8086 register/branch workload through both execution paths. Host MIPS are regression numbers only; they are not RP2350 estimates.

On the development container for this milestone:

```text
interp  ~257 MIPS
aot     ~506 MIPS
```

The important number for the interpreter is that direct-threaded dispatch remains fast while AOT pulls ahead on a branch-heavy loop. RP2350 measurements become the meaningful target once the Pico Plus 2 backend is live.

## Direction

```text
8086 binary
   |
   +--> dosrecomp analyzer --> portable C --> native ARM/RISC-V
   |
   +--> fast interpreter --> decoded-block cache --> direct block chaining

both paths
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
