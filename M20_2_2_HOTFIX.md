# microDOS M20.2.2 — external JNZ exits, emitter bounds, QEMU JIT oracle

## 1. callmix fallback=32768 (would have failed M20.2.1 acceptance)

Run as native Thumb-2 under qemu-arm, `callmix` reported
`fallback=32768 control=65536` instead of the required `fallback=0`.

Cause: the general block emitter only lowered `JNZ` when its target was an
op *inside the same block*. In callmix, `DEC CX; JNZ` jumps back to the
`CALL` at 0106h, which belongs to another block. The JNZ was therefore left
out of the block, the block exited before it, and the one-op block at 010Ah
could not lower it either, so every iteration took a zero-progress
interpreter step.

Fix: `th_emit_jnz()` now also handles targets outside the block (or forward
in it) by evaluating the condition natively and leaving through one of two
exact exits (IP = target, or IP = next), the same shape the external `JMP`
already used. Internal backward edges keep the native branch + budget check.

Result under qemu-arm: `callmix fallback=0 control=65536`, results identical
to the interpreter. With the old emitter the new differential test fails on
exactly this fixture.

## 2. Out-of-bounds write when the scratch buffer fills

`th16()` correctly marks the buffer failed when the 1536-byte scratch area is
full, but a branch placeholder created after that point pointed at or past
the end, and `th_patch_bcond()` / the store fast-path patch then wrote two
bytes past `bytes[]` (into `MdThumbBuf.at/.failed` on the stack). The block
was discarded anyway, but the write had already happened. Large blocks in
full-DOS workloads are the likely trigger. Both patch sites now go through a
bounds-checked `th_patch16()`. (GCC's -Wstringop-overflow pointed at it.)

## 3. JIT differential test + QEMU oracle

`tests/test_jit_diff.c` runs every bench fixture (loop, memloop, regionmix,
branchmix, callmix) plus M20.3 LODS/LOOP-family fixtures through the
interpreter and the JIT, and requires identical registers, segment
registers, IP, architectural FLAGS, all 1 MiB of memory, stop reason and
instruction count.

- Host build (`ctest`: `jit_diff`): checks decode/region logic and the C
  reference semantics.
- `tools/jit_qemu_check.sh` (Linux/WSL, needs `gcc-arm-linux-gnueabihf
  qemu-user`): builds the same test as ARMv7-A Thumb-2 and runs it under
  qemu-arm, which executes the generated code itself and also checks shape
  (fallback limits, resident regions, native control counts). It also runs
  `tests/test_jit.c` (the M20.3 spec), which is expected to fail until M20.3.

ARMv7-A Thumb-2 under QEMU is a stand-in for the Cortex-M33: it validates the
emitted instruction semantics, not M33 timing. Hardware remains the final
check, but generator bugs no longer need a UF2 to be found.

## Hardware validation for this drop

1. `microdos_bench.uf2`: callmix JIT rows must show `result=ok`,
   `fallback=0`, `control=65536` (previously expected to fail).
2. `microdos_pico_jit.uf2`: DOS2TEST 25/25.
