# M21.1 — guest memory placement A/B (SRAM vs PSRAM)

## Question

Is guest memory placement the biggest remaining lever for real DOS speed?

## Evidence that motivated it (host profile, DOS2TEST session)

Guest memory accesses (754,119):

| range | share |
|---|---|
| 10000-1FFFF (DOS kernel segment: data, buffers, stack) | 87.4% |
| 90000-9FFFF (COMMAND.COM transient) | 7.5% |
| 00000-0FFFF (IVT, device layer, COMMAND resident) | 5.2% |
| hottest 32 KiB | 92.9% |
| hottest 64 KiB | 100% |

All of it lives in PSRAM on the Pico. The bench shows PSRAM costing compiled
code 2.8-5x at 300 MHz (memloop AOT 28.9 vs 10.4 MIPS, checksum 54.9 vs
19.4, regionmix JIT 83 vs 16). How much of the compiled kernel's 3.3 MIPS is
memory stalls is not known; this experiment measures it.

Compiled-kernel hotness (same session): 99% of executed kernel instructions
come from 2,383 of 6,100 static instructions (39%); 99.9% from 3,891 (64%).

## The experiment

Two firmwares, identical except for where guest RAM lives:

| | `microdos_pico_g128_psram` | `microdos_pico_g128_sram` |
|---|---|---|
| guest address space | 128 KiB (`MD_X86_ADDRESS_BITS=17`) | same |
| DOS memory size | 128 KiB (`MD_MSDOS2_DEFAULT_MEMORY_PARAGRAPHS=0x2000`) | same |
| compiled kernel / cache / JIT / clock | on / off / off / 300 MHz | same |
| DOS2TEST precompiled | no (so the SRAM variant fits) | no |
| guest RAM | PSRAM | **SRAM** |
| SRAM used | 333 KB | 464 KB |

DOS2TEST passes 25/25 in this configuration on the host
(`ctest`: `dos2_e2e_g128`, compiled kernel, 128 KiB guest).

Supporting changes (defaults unchanged):

- `MD_X86_ADDRESS_BITS` (default 20) defines the guest address space; mask
  and size derive from it. The runtime JIT refuses to build with any other
  width (`#error`), because its emitted code wraps at 20 bits.
- `MD_MSDOS2_DEFAULT_MEMORY_PARAGRAPHS` is overridable.
- Pico firmware: `MICRODOS_PICO_GUEST_SRAM`, `MICRODOS_PICO_DOS2TEST_AOT`.

## Side result

Not precompiling DOS2TEST saves about 61 KB of SRAM (microdos_pico 394 KB ->
333 KB in the A/B base). DOS2TEST is a test program; production firmware
does not need its compiled code.

## How to read the result

Run DOS2TEST on each firmware with Ctrl+] before and after; compare
"active MIPS" (since previous Ctrl+]).

- SRAM >= ~1.5x PSRAM: memory placement is a major lever. Next: a guest
  page table (4 KiB pages mapped to SRAM or PSRAM) with the kernel segment
  and low memory in SRAM, plus cold compiled-kernel code moved to flash to
  make room.
- SRAM < ~1.2x: memory is not the bottleneck for DOS; prioritise code-path
  work (M21.2, interrupt/dispatch overhead).
