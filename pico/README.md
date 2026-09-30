# microDOS on Pico 2 (Pimoroni Pico Plus 2)

Boots the released MS-DOS 2.0 kernel and COMMAND.COM on an RP2350 with 8 MiB
PSRAM, with a USB serial console. DOS2TEST.COM is on the disk and runs as
dosrecomp-compiled native code when you start it at `A>`.

## Firmware variants (M18)

`.\md.bat build pico` builds:

| UF2 | Purpose |
|---|---|
| `microdos_pico.uf2` | **default** DOS: compiled kernel, code in SRAM, 300 MHz |
| `microdos_pico_150.uf2` | the same at 150 MHz |
| `microdos_pico_nokernel.uf2` | interpreted kernel at 300 MHz, for A/B |
| `microdos_bench.uf2` / `_150` | benchmark matrix |

The defaults follow the M15 measurements on DOS2TEST: code in SRAM was +33%
over flash, and turning the decoded-block cache off was +60%.

The benchmark firmwares print a table as soon as a terminal connects (any key
reruns it). Rows: workload (`loop` registers only, `memloop` 32 KiB
read-modify-write) x guest memory (SRAM, PSRAM) x engine (`step`, `threaded`,
`cs-run` = the M16 DOS loop engine, `cache`, `aot`). Every row verifies the
guest result (`ok`/`FAIL`).

In the DOS firmwares, press **Ctrl+]** right before `DOS2TEST` and again right
after it: the second report's "since previous Ctrl+]" block is DOS2TEST alone,
with measured idle, console, input and disk time removed.

## Build

Prerequisites (the same as microconsole/microrender): Pico SDK 2.3.0,
ARM GCC 14_2_Rel1 (or 13_3_Rel1), Ninja, and picotool 2.3.0, normally
installed by the VS Code Raspberry Pi Pico extension under
`%USERPROFILE%\.pico-sdk`. `PICO_SDK_PATH` / `PICO_TOOLCHAIN_PATH` /
`NINJA_EXE` override the lookup (see `scripts\md_pico_env.bat`).

```
.\md.bat deps msdos        (once)
.\md.bat build pico
```

Output: `build-pico\out\microdos_pico.uf2`

## Flash and run

1. Hold BOOTSEL and plug in the Pico Plus 2; an `RP2350` drive appears.
2. Copy `microdos_pico.uf2` to it. The board reboots.
3. Open the USB serial port in a terminal (PuTTY, Tera Term, the VS Code
   serial monitor, `python -m serial.tools.miniterm COMx 115200`). The
   firmware waits for the terminal before it boots DOS.
4. Press Enter at the date and time prompts, then `DOS2TEST` at `A>`.
5. Press **Ctrl+]** at any time for statistics. It is not passed to DOS.

## Memory layout

| What | Where |
|---|---|
| guest 1 MiB address space | PSRAM (uninitialised section, zeroed at boot) |
| 360 KiB disk | flash image, copied to PSRAM at boot |
| MSDOS.SYS and disk blobs | flash (`.flashdata`, all variants) |
| interpreter, cache, compiled DOS2TEST | SRAM in `microdos_pico*`, flash in `*_flash*` |
| runtime state, decoded-block cache | SRAM |

Disk writes go to the PSRAM copy and are **lost at reset** in this milestone.
The clock starts at 1983-03-08 12:00; set it at the DOS prompts.

## What to report

1. Both benchmark tables (`microdos_bench_flash`, `microdos_bench_sram`).
2. For each DOS firmware you try: Ctrl+] at `A>`, `DOS2TEST`, Ctrl+] again,
   and the whole terminal output. DOS2TEST must still say 25/25 and
   `attaches=2 enters=6959`.
