# Pico 2 / RP2350 target

Build:

```powershell
.\md.bat deps msdos
.\md.bat build pico
```

Firmware is written under `build-pico/out/`.

The target uses the shared microDOS runtime with Pico-specific startup, USB console, clocks and memory placement. Exact firmware variants are defined by `pico/CMakeLists.txt`.

For a DOS firmware:

1. flash the desired UF2;
2. open the USB serial console;
3. answer the DOS date/time prompts;
4. run `DOS2TEST`.

`DOS2TEST` must complete with all checks passing.

Generated UF2 files and captured serial output are build artifacts and are not tracked.