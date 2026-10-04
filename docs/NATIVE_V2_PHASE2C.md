# microDOS Native v2 Phase 2C

Phase 2B proved direct guest-memory execution and correct native-to-interpreter
lazy-flag handoff:

- loop: 112.425 MIPS / 2.668 cycles per guest
- regmix: 83.309 MIPS / 3.601 cycles per guest
- SRAM memmix: 95.007 MIPS / 3.157 cycles per guest

Two production issues remain before DOS integration.

## 1. Flag liveness

Phase 2B updated virtual CF and native Z after every ALU instruction. That is
unnecessary.

Phase 2C performs backward flag liveness on the transient native op stream.

For the current subset:

- JZ/JNZ consume Z.
- No native instruction consumes CF yet.
- Region exit needs the CF preserved through the final DEC.

Example `regmix`:

```asm
add ax,bx       ; CF dies at XOR -> do not emit CF tracking
xor bx,ax       ; CF=0 dies at next ADD -> do not emit CF tracking
add dx,ax       ; CF is preserved through DEC to exit -> track this one
dec cx          ; Z is consumed by JNZ -> produce Z
jnz loop
```

The compiler records `CFsites`, `Zsites`, and whether incoming architectural
CF must be loaded at region entry.

## 2. Real Pico guest-memory placement

Phase 2B's 95 MIPS memory result used on-chip SRAM. Full DOS uses the 1 MiB
guest in Pico Plus 2 PSRAM.

Phase 2C therefore runs the identical compiled `memmix` region twice:

- `mem-sram`: 64 KiB internal SRAM guest
- `mem-psram`: 1 MiB `__uninitialized_psram` guest

Both run at the same 300 MHz system clock and use the board's normal PSRAM
reinitialization path.

This is the last benchmark-only milestone. If the PSRAM result is acceptable,
the next change is Native v2 admission inside the actual DOS system loop.
