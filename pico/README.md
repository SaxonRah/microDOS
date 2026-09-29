# microDOS on Pico 2 (Pimoroni Pico Plus 2)

Boots the released MS-DOS 2.0 kernel and COMMAND.COM on an RP2350 with 8 MiB
PSRAM, with a USB serial console. DOS2TEST.COM is on the disk and runs as
dosrecomp-compiled native code when you start it at `A>`.

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
5. Press **Ctrl+]** at any time for statistics (instructions, MIPS, AOT,
   cache). It is not passed to DOS.

## Memory layout

| What | Where |
|---|---|
| guest 1 MiB address space | PSRAM (uninitialised section, zeroed at boot) |
| 360 KiB disk | flash image, copied to PSRAM at boot |
| MSDOS.SYS, interpreter, compiled DOS2TEST | flash (XIP) |
| runtime state, decoded-block cache | SRAM (~30 KB) |

Disk writes go to the PSRAM copy and are **lost at reset** in this milestone.
The clock starts at 1983-03-08 12:00; set it at the DOS prompts.

## What to report

Everything the terminal prints from the banner through `A>`, the DOS2TEST
result, and one Ctrl+] statistics block after DOS2TEST finishes. The MIPS
figure is the first real RP2350 measurement for this project.
