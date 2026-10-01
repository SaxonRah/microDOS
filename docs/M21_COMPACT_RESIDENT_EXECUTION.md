# M21 — compact resident execution

M21 changes the optimization target from "add more JIT opcode coverage" to
"make all three execution tiers use the same region model while reducing
RP2350 SRAM pressure."

Baseline is `4a3df05` (M20.3).  On the real DOS2TEST workload M20.3 reduced
runtime-JIT fallback from 81,407 to 2,924 instructions and raised JIT-native
execution from 539 to 79,022 instructions.  Coverage is therefore no longer
the dominant problem.

## M21.0 — measure the right things

Every Pico target now emits a linker map next to its ELF/UF2.  The benchmark
prints `sizeof(MdRuntime)`, `sizeof(MdBlockCache)`, `sizeof(MdJit)`, and
`sizeof(MdJitBlock)`.

A new checksum fixture:

    MOV SI,2000h
    MOV CX,4000h
    XOR DX,DX
    CLD
L:  LODSW
    ADD DX,AX
    LOOP L
    HLT

reads 32 KiB of guest memory and retires 49,157 instructions.  Unlike the
synthetic DEC/JNZ loop it cannot be reduced to a constant-time register
update, so it is the primary AOT/JIT/resident-interpreter convergence metric.

## M21.1 — compact runtime JIT metadata

M20.3 kept `MdJitOp ops[24]` in every one of the 64 full-DOS JIT block slots
after native Thumb had already been emitted.  M21 uses one shared compile
scratch array instead.

A compiled block keeps decoded ops only when its generated code contains a
runtime helper that needs `block + op_index`.  Pure resident/direct blocks
discard the IR completely.  Helper descriptors are copied into the existing
JIT arena, so the fixed block table becomes dramatically smaller without
adding another SRAM arena.

Host differential tests always retain descriptors because the host executes
the predecoded reference path rather than emitted Thumb.

## M21.2 — release versus profiling

`MD_JIT_PROFILE` defaults to 1 for tests and the benchmark.  The lean
`microdos_pico_jit` build sets it to 0 so hot-site/statistical accounting is
compiled out of the high-frequency JIT path.  `microdos_pico_jit_profile`
keeps the full M20 observability.

`MD_CACHE_PROFILE` does the same for decoded-cache counters.  The dedicated
resident-interpreter DOS build uses the lean setting.

## M21.3/M21.4 — one C resident-region semantics layer

`src/runtime/region.c` is shared by AOT-generated code and cached
interpretation.  The first two exact, semantics-preserving regions are:

* self `DEC r16 / JNZ` counted loops;
* `LODSW / ADD DX,AX / LOOP` checksum loops.

The helper contract is deliberately small: it either admits the whole region
and returns its exact retired-instruction count, or returns zero without
changing guest state.  Callers retain exact instruction-budget behavior.

The checksum implementation keeps AX/DX/SI/CX in C locals, performs physical
20-bit wrap on each byte of LODSW, respects DF, and records only the final
ADD's lazy flags because LODSW and LOOP do not modify FLAGS.

`dosrecomp` recognizes the same regions and emits one shared-region call
instead of repeatedly crossing generated block dispatch.  The decoded cache
recognizes the same checksum body and invokes the same helper after the loop
head becomes hot/re-entered.

## Pico comparison images

`.\md.bat build pico` now builds:

* `microdos_pico.uf2` — compiled-kernel AOT reference;
* `microdos_pico_region.uf2` — no kernel AOT, decoded-cache resident regions;
* `microdos_pico_jitkernel.uf2` — no kernel AOT, runtime JIT kernel;
* `microdos_pico_jit.uf2` — AOT kernel + lean runtime JIT applications;
* `microdos_pico_jit_profile.uf2` — the same hybrid with full JIT profiling;
* the existing 150 MHz and benchmark images.

This gives us comparable full-DOS paths instead of inferring engine behavior
from unrelated synthetic programs.

## M21.5/M21.6 next measurements

The linker maps intentionally come before hot/cold placement.  M20.3's
`jit_thumb2.c` mixes the native execution helpers with the cold compiler and
Thumb emitter.  The next split should be driven by the maps: keep the
threaded interpreter, AOT resident bodies, JIT entry/helpers, and emitted code
in SRAM; move compiler/decoder/diagnostics/rare fault paths toward flash or
size-optimized sections where the RP2350 linker layout proves it worthwhile.

The correctness gate remains the M20.3 differential suite plus the M21 shared
region tests.  QEMU Thumb-2 and hardware remain required before deleting an
older execution path.
