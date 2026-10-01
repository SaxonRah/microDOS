# microDOS on Pico 2 (Pimoroni Pico Plus 2)

Boots the released MS-DOS 2.0 kernel and COMMAND.COM on an RP2350 with 8 MiB
PSRAM, with a USB serial console. DOS2TEST.COM is on the disk and runs as
dosrecomp-compiled native code when you start it at `A>`.

## Firmware variants (M21 compact resident execution)

`.\md.bat build pico` builds:

| UF2 | Purpose |
|---|---|
| `microdos_pico.uf2` | DOS with compiled/AOT kernel, code in SRAM, 300 MHz reference |
| `microdos_pico_150.uf2` | the same at 150 MHz |
| `microdos_pico_nokernel.uf2` | canonical threaded kernel at 300 MHz |
| `microdos_pico_region.uf2` | decoded-cache kernel with M21 resident C regions |
| `microdos_pico_jitkernel.uf2` | runtime-JIT kernel, lean profiling |
| `microdos_pico_jit.uf2` | compiled kernel + lean runtime JIT for COMMAND.COM/arbitrary programs |
| `microdos_pico_jit_profile.uf2` | same hybrid with full M20/M21 JIT observability |
| `microdos_bench.uf2` / `_150` | benchmark matrix including the M21 checksum convergence workload |

The DOS defaults follow the M15 measurements on DOS2TEST: code in SRAM was
+33% over flash, and turning the decoded-block cache off was +60%.

The benchmark firmwares print a table as soon as a terminal connects (any key
reruns it). Rows include `loop`, `memloop`, and the M21 `checksum` workload
(32 KiB sequential LODSW/ADD/LOOP) across guest SRAM/PSRAM and every engine,
followed by the M20 region/CFG/control proofs.  `checksum` is the primary
cross-tier convergence metric because it cannot be reduced to a register-only
counted loop:

```text
step
threaded
cs-run
cache
aot
jit
```

`jit` is the M20.2 on-chip translator. The M19.2 exact resident fast paths remain,
but M20 additionally recognises generic counted loops ending in `DEC CX/JNZ`,
keeps AX/BX/CX/SI resident, and compiles supported ALU/[SI] bodies without an
exact byte-pattern match. Unsafe shapes fall through to M19.2/M19.1/canonical
execution. See `docs/M20_GENERAL_REGIONS.md`.

Every row verifies the guest result (`ok`/`FAIL`). JIT rows also print compile,
hit/miss, native-entry, direct/fallback, invalidation, flush and code-size
statistics.

In the DOS firmwares, press **Ctrl+]** right before `DOS2TEST` and again right
after it: the second report's "since previous Ctrl+]" block is DOS2TEST alone,
with measured idle, console, input and disk time removed.

## Build

Prerequisites (the same as microconsole/microrender): Pico SDK 2.3.0,
ARM GCC 14_2_Rel1 (or 13_3_Rel1), Ninja, and picotool 2.3.0, normally
installed by the VS Code Raspberry Pi Pico extension under
`%USERPROFILE%\.pico-sdk`. `PICO_SDK_PATH` / `PICO_TOOLCHAIN_PATH` /
`NINJA_EXE` override the lookup (see `scripts\md_pico_env.bat`).

```text
.\md.bat deps msdos        (once)
.\md.bat build pico
```

Outputs include:

```text
build-pico\out\microdos_pico.uf2
build-pico\out\microdos_pico_150.uf2
build-pico\out\microdos_pico_nokernel.uf2
build-pico\out\microdos_pico_jit.uf2
build-pico\out\microdos_bench.uf2
build-pico\out\microdos_bench_150.uf2
```

## Flash and run DOS

1. Hold BOOTSEL and plug in the Pico Plus 2; an `RP2350` drive appears.
2. Copy `microdos_pico.uf2` to it. The board reboots.
3. Open the USB serial port in a terminal (PuTTY, Tera Term, the VS Code
   serial monitor, `python -m serial.tools.miniterm COMx 115200`). The
   firmware waits for the terminal before it boots DOS.
4. Press Enter at the date and time prompts, then `DOS2TEST` at `A>`.
5. Press **Ctrl+]** at any time for statistics. It is not passed to DOS.

## Flash and run M20.2 JIT benchmark

Flash:

```text
build-pico\out\microdos_bench.uf2
```

Open the same USB serial terminal. The complete table prints immediately.
Capture the entire table. The important rows are the resident/CFG workloads plus the new `callmix` control-transfer proof on SRAM/PSRAM. Generic rows add `generic-regions`, `generic-entry` and
`generic-instr` counters.

For a rated-clock comparison, flash:

```text
build-pico\out\microdos_bench_150.uf2
```

## Memory layout

| What | Where |
|---|---|
| guest 1 MiB address space | PSRAM (uninitialised section, zeroed at boot) |
| 360 KiB disk | flash image, copied to PSRAM at boot |
| MSDOS.SYS and disk blobs | flash (`.flashdata`, all variants) |
| interpreter, cache, compiled DOS2TEST | SRAM in `microdos_pico*` |
| benchmark JIT code arena | 32 KiB SRAM |
| full-DOS M20 JIT code arena | 24 KiB SRAM in `microdos_pico_jit` |
| runtime state, decoded-block cache, JIT metadata | SRAM (M20.2 keeps growth bounded; code arena remains 24 KiB) |

Disk writes go to the PSRAM copy and are **lost at reset** in this milestone.
The clock starts at 1983-03-08 12:00; set it at the DOS prompts.

## What to report

For M20.2, first capture the complete serial output from:

```text
microdos_bench.uf2
```

All rows must remain `ok`. The old loop/memloop numbers should stay close to
M19.2. `regionmix` must report `generic-regions=1`, `generic-entry=1`,
`fallback=0`. In particular, report:

```text
loop     SRAM  jit ...
loop     PSRAM jit ...
memloop  SRAM  jit ...
memloop  PSRAM jit ...
regionmix SRAM  jit ...
regionmix PSRAM jit ...
branchmix SRAM jit ...
callmix SRAM jit ...
callmix PSRAM jit ...
[jit] ...
```

Then flash `microdos_pico_jit.uf2`, boot DOS normally, run `DOS2TEST`, and
exercise a few COMMAND.COM built-ins. The acceptance bar remains 25/25. The
proven `microdos_pico.uf2` remains unchanged as the fallback/reference image.

## M20.1 profiling build

M20.1 adds a bounded multi-block resident CFG proof (`branchmix`) and replaces
the old M18 Ctrl+] output in `microdos_pico_jit.uf2` with tier-aware JIT
profiling.  Use Ctrl+] immediately before and after a shell workload.  The
interval report separates static AOT, runtime-native JIT and interpreted guest
instructions, then prints JIT fallback reasons and sampled hot CS:IP sites.

Recommended first interval:

```text
Ctrl+]
VER
DIR
ECHO HELLO WORLD
MD TEST
CD TEST
CD ..
RD TEST
Ctrl+]
```

See `docs/M20_1_CFG_OBSERVABILITY.md`.


## M20.2 profile-driven cleanup

M20.1 hardware profiling showed that the synthetic BIOS trampoline segment
(`0800h`) dominated JIT zero-progress traffic and polluted the 24 KiB code
arena. M20.2 therefore keeps that segment on the canonical threaded path and
uses the runtime translator only for real application segments.

The full-DOS JIT build also uses a two-hit hotness gate: the first visit to a
new `CS:IP` executes canonically; the second visit compiles it. This avoids
spending SRAM code space on one-shot command paths while preserving immediate
compilation in the standalone benchmark build.

Near CALL, near/far RET, INT and IRET now have native generated control stubs
that call a shared semantic helper and return to the JIT/system dispatcher at
the architectural destination. They no longer appear as zero-progress
interpreter boundaries once hot.

Ctrl+] hot-site output now prints the source and destination:

```text
CS:IP op=xx -> CS:IP reason
```

For `cs-change` this fixes the M20.1 ambiguity where the old report showed the
destination kernel opcode instead of the application/BIOS instruction that
caused the transfer.

**Press the actual Ctrl+] key combination. Do not type the characters
`Ctrl+]` at the DOS prompt**; COMMAND.COM will correctly treat that text as a
command name.

See `docs/M20_2_CONTROL_HOTNESS.md`.
