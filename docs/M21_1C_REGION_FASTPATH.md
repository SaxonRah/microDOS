# M21.1c — partial resident admission and checksum fast path

## Goal

M21.1b moved the real DOS2TEST interval from about 3.49 MIPS to 3.91 MIPS by
removing unnecessary guest-store tracking work. M21.1c keeps that store path
unchanged and targets resident-region execution.

## Changes

### Whole-iteration partial admission

The two exact shared regions no longer require the entire logical loop to fit
inside the caller's remaining guest-instruction budget.

- `DEC r16 / JNZ self`: admits `floor(budget / 2)` iterations.
- `LODSW / ADD DX,AX / LOOP`: admits `floor(budget / 3)` iterations.

Admission is still atomic at an iteration boundary. If no whole iteration
fits, the helper returns zero without changing guest state.

On partial progress the helper leaves `cpu->ip` at the region entry. On full
completion it moves `cpu->ip` to the region exit. Flags are those of the final
executed DEC/ADD, and the caller still owns instruction accounting.

Generated AOT now trusts the helper's IP instead of unconditionally forcing
the region exit after every nonzero return.

### Forward contiguous checksum path

For DF=0, when the admitted LODSW range cannot wrap either 16-bit SI or guest
physical memory, the checksum region walks one contiguous host pointer rather
than recomputing and masking the physical address for every word.

The raw additions are unrolled four words per branch. Only the final ADD uses
the canonical arithmetic helper, because LODSW and LOOP do not change FLAGS.

Backward, SI-wrapping and physical-wrapping cases retain the exact generic
path.

## Correctness coverage

`region` now checks:
- full DEC/JNZ including CX=0 -> 65536 trips;
- partial DEC/JNZ and CF preservation;
- full and partial forward checksum;
- full and partial DF=1 checksum;
- physical wrap;
- cache execution.

`region_aot` additionally enters an attached generated checksum image with a
25-instruction budget (four setup instructions + seven whole loop iterations),
requires IP=0109h and CX=4000h-7, compares against the interpreter, then
continues both executions to HALT.

## Hardware measurements to capture

First flash `microdos_bench.uf2` and compare the checksum rows against M21.1b:

- SRAM cache 54.863 MIPS
- SRAM AOT 52.518 MIPS
- PSRAM cache 19.499 MIPS
- PSRAM AOT 19.150 MIPS

Then flash `microdos_pico.uf2`, press Enter twice, Ctrl+], run `DOS2TEST`,
Ctrl+] again. M21.1b's DOS2TEST interval baseline is 3.911 active MIPS.
