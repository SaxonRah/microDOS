# Native v2 Phase 2A

Phase 1 proved runtime-generated register-resident Thumb at **2.667 host
cycles per guest instruction** on the 300 MHz RP2350.

Phase 2A changes the fundamental register convention so that every 8086
general-purpose register occupies a Thumb low register:

| 8086 | ARM |
|---|---|
| AX | r0 |
| CX | r1 |
| DX | r2 |
| BX | r3 |
| SP | r4 |
| BP | r5 |
| SI | r6 |
| DI | r7 |

`r8` holds the `MdX86 *` frame across the region.

Future Phase 2B allocation:

- r9: guest RAM host pointer
- r10: cached DS linear base
- r11: cached SS linear base
- r12: address/scratch

This arrangement is deliberately Pico-first. The full x86 integer register
file can participate in compact Thumb ALU instructions without loading or
spilling architectural state.

## New native operations

Phase 2A adds:

- `MOV r16,r16`
- register-register `ADD`
- register-register `OR`
- register-register `AND`
- register-register `SUB`
- register-register `XOR`
- register-register `CMP`
- `83h ADD r16,imm8`
- `83h SUB r16,imm8`
- positive-immediate `83h CMP`
- existing MOV immediate / INC / DEC / JZ / JNZ

No C semantic helper calls are made from generated code.

## Flags

Phase 2A intentionally tracks only native Z for JZ/JNZ. It is **not yet
safe to hand a region exit directly to the DOS interpreter**, because complete
x86 OSZAPC state is not materialized on exit.

Phase 2B must add virtual x86 flag state and exit materialization before the
engine is integrated into the DOS dispatcher.

## Benchmarks

`loop`:
- original Phase-1 INC/DEC/JNZ workload
- proves the new all-low-register mapping does not regress the basic loop

`regmix`:
- exercises AX/BX/CX/DX as native registers
- ADD reg/reg
- XOR reg/reg
- multiple live guest registers
- DEC/JNZ loop

The benchmark prints MIPS and host-cycles/guest for both.
