# Native v2 Phase 3I — guarded unsigned MUL/DIV loop

Phase 3H checkpoint:

- Host regression suite PASS
- DOS2TEST 25/25
- MDSTRESS checksum 0xA298
- Native coverage 9.1%
- Native retirement 1,742,559 guest instructions
- Active 3.073 MIPS
- Runtime fallback 0
- Store guard rejection 0
- Phase-6 multi-CFG region 14CB:03E8:
  - phase=9
  - dyn=2
  - ops=15
  - retired=309,047

Phase 3I targets the measured MDSTRESS Phase-5 inner loop:

```asm
.inner:
    xor dx,dx
    mul bx
    div di
    xor ax,dx
    add ax,1357h
    rol ax,1
    ror dx,1
    loop .inner
```

The Phase-3C profile measured 131,064 taken LOOP backedges at this site.

## Native MUL/DIV proof

The production compiler accepts one adjacent unsigned `MUL r16` / `DIV r16`
pair only when both source registers are invariant across the counted loop.

Runtime then requires:

```text
divisor != 0
multiplier < divisor
```

For any 16-bit AX:

```text
AX * multiplier < 65536 * divisor
```

so the unsigned DIV quotient is guaranteed to fit in 16 bits. A state that
does not satisfy the proof remains in the threaded interpreter.

The measured loop has:

```text
multiplier = BX = 251
divisor    = DI = 257
```

and therefore satisfies the guard for every iteration.

## Thumb-2 lowering

`MUL r16`:

- one 32-bit Thumb MUL
- low word -> AX
- high word -> DX

`DIV r16`:

- rebuild DX:AX in r12
- Thumb UDIV for quotient
- Thumb MLS for remainder
- quotient -> AX
- remainder -> DX

No C semantic helper is called from the hot loop.

## Exit flags

The final flag sequence is:

```asm
add ax,1357h
rol ax,1
ror dx,1
loop
```

ROL/ROR preserve SZAP, so:

- SF/ZF/AF/PF are reconstructed from the ADD
- CF/OF are reconstructed from the final ROR DX,1

The final AX is inverse-rotated once to recover the exact ADD result before
building canonical microDOS flags.

Expected production metadata:

```text
ops=8
phase=10
dyn=0
terminal=E2
mul=BX
div=DI
exit_flags_mode=1
```
