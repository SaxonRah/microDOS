# microDOS

microDOS is a source-assisted 8086 recompilation and execution project targeting the RP2350 / Pico 2 family, with the Pimoroni Pico Plus 2 as the first hardware target.

The project has two execution paths that share one architectural state and one semantics layer:

1. **AOT recompilation** — known DOS binaries are analyzed and translated into portable C, then compiled natively for the target, in the spirit of N64Recomp-style static recompilation.
2. **High-performance 8086 interpretation** — unknown, indirect, dynamically generated, self-modifying, or not-yet-recompiled code runs through a fast interpreter and can cross back into compiled blocks.

The interpreter is not a disposable fallback. On GCC/Clang builds it uses direct-threaded dispatch for hot opcodes, direct 20-bit guest-memory access, inline register helpers, and host callbacks only at interrupt/I/O boundaries. MSVC uses a portable switch dispatcher so the same runtime can be differentially tested on Windows.

## Bootstrap status

The first milestone proves that interpreted and recompiled code use the same machine model.

```text
8086 .COM bytes ---> interpreter ---+
                                     +--> MdRuntime --> INT 21h host shim
recompiled C ------------------------+
```

Both paths currently execute the same five-instruction hello program and are checked for identical output, instruction count, exit status, register aliasing, 20-bit address wrapping, and real IVT fallback behavior.

## Build

Windows:

```bat
.\md.bat build host
.\md.bat run host
.\md.bat test
.\md.bat bench 1000
```

Portable CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/microdos_host
```

Expected output includes:

```text
interp: stop=exit instructions=5 exit=0
recomp: stop=exit instructions=5 exit=0
output: Hello from microDOS!
```

## Direction

The intended path is:

```text
8086 binary
   |
   +--> dosrecomp analyzer --> generated portable C --> ARM/RISC-V native code
   |
   +--> fast interpreter for unresolved/dynamic regions

both paths
   |
   v
shared x86 architectural state
   |
   +--> DOS / BIOS interrupt routing
   +--> MicroRender video backend
   +--> MicroWave audio backend
   +--> MicroConsole / catBUS input and storage
```

The original MS-DOS source is useful as source-level metadata and documentation, but the binary remains the authority for recompilation behavior.

See `docs/ARCHITECTURE.md` and `DECISIONS.md`.

## Performance regression harness

`microdos_bench` executes a small register/branch loop with no device callbacks so dispatcher and instruction-semantic costs are visible. Host MIPS are useful only as a regression metric; RP2350 measurements will be tracked separately once the Pico Plus 2 backend is live.
