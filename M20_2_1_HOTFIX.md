# microDOS M20.2.1 — control helper IP synchronization hotfix

M20.2 introduced native helper-backed CALL/RET/INT/IRET control instructions.
On RP2350, translated blocks that executed one or more direct Thumb operations
before a control instruction could enter the helper with a stale `MdX86.ip`.

`md_jit_control_one()` intentionally validates that architectural CS:IP still
matches the predecoded control operation before touching the stack, dispatching
an interrupt, or returning. The M20.2 Thumb emitter did not store the current
control operation's IP before making that helper call. Therefore the validation
failed, the helper silently returned without executing the control transfer,
and the generated thunk nevertheless retired the operation and returned to the
C dispatcher. Re-entering the stale block with partially updated architectural
state corrupted real COMMAND.COM execution; on Pico this first appeared as a
large NUL-byte flood immediately after the date line.

## Fix

`th_emit_control_one()` now receives the `MdJitOp` and emits:

```text
store op->ip -> MdX86.ip
r0 = runtime
r1 = block
r2 = op index
BLX md_jit_control_one
retire one guest control instruction
return to dispatcher at architectural target
```

This preserves the existing helper validation rather than deleting it. The
validation remains valuable for invalidation/state bugs; the emitter now honors
its contract.

## Required hardware validation order

1. Build host, tests, Pico.
2. Flash `microdos_bench.uf2` first.
3. Confirm the complete `callmix` JIT rows appear. They must not hang.
4. Require `result=ok`, `fallback=0`, and `control=65536`.
5. Only then flash `microdos_pico_jit.uf2`.
6. Confirm normal COMMAND.COM date/time prompt with no NUL flood.
7. Run DOS2TEST; require 25/25.

Do not use the broken M20.2 `microdos_pico_jit.uf2`.
