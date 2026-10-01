# microDOS M20.2 overlay

Apply this over the hardware-validated M20.1 tree.

M20.2 is driven directly by the first full COMMAND.COM profile:

- excludes the synthetic BIOS trampoline segment from the JIT;
- adds a 2-hit hotness gate to the full-DOS JIT build;
- lowers near CALL, near/far RET, INT and IRET through native control stubs;
- records CS-changing exits as source -> destination;
- adds a `callmix` benchmark for CALL/RET correctness and throughput;
- keeps M19.2 resident loops and M20/M20.1 CFG regions unchanged.

## Build

```powershell
cd C:\microDOS
.\md.bat clean
.\md.bat build host
.\md.bat test
.\md.bat build pico
```

Check the `microdos_pico_jit.elf` SRAM line. M20.1 measured 465,588 B / 512 KiB
(88.80%); M20.2 adds only a small hotness/source-destination profiling table and
keeps the 24 KiB executable arena fixed.

## Benchmark

Flash `build-pico\out\microdos_bench.uf2`.

All old rows must remain `ok`. New acceptance row:

```text
callmix SRAM  jit ... ok
callmix PSRAM jit ... ok
```

JIT stats should show:

```text
fallback=0
control=65536
```

for callmix.

## Full DOS

Flash `build-pico\out\microdos_pico_jit.uf2`, boot normally, and run
`DOS2TEST` (25/25 mandatory).

Then press the actual **Ctrl+]** key combination, run:

```text
VER
DIR
ECHO HELLO WORLD
MD TEST
CD TEST
CD ..
RD TEST
```

and press **Ctrl+]** again.

Do not type the literal text `Ctrl+]` at `A>`.

The interval report should show:

- `bios-bypass` carrying the synthetic BIOS traffic;
- a much smaller `zero=` fallback count;
- `control=` increasing for CALL/RET/INT/IRET executed through generated stubs;
- `cold=` showing one-shot paths intentionally not compiled;
- hot transfer sites in `source -> destination` form;
- fewer code-cache flushes/compiles than M20.1 for comparable workloads.
