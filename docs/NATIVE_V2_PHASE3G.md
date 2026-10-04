# Native v2 Phase 3G — first real small-CFG region

Phase 3F result:

- DOS2TEST: 25/25
- MDSTRESS: 0xA298
- Native retirement: 726,301 guest instructions
- Native coverage: 3.8%
- Native entries: 134
- Runtime fallback: 0
- Store guard reject: 0
- Whole-run active rate: 2.876 MIPS

Phase 3G targets MDSTRESS phase 1's measured hot inner loop:

```asm
.inner:
    add ax,bx
    adc si,ax
    xor bx,si
    sub di,bx
    rol ax,1
    ror bx,1
    test si,1
    jz .noinc
    inc di
.noinc:
    dec cx
    jnz .inner
```

This is the first Native-v2 production region with an internal CFG edge.

## New execution semantics

Added:

- ADC r16,r16 using virtual x86 CF in r11.
- ROL r16,1 and ROR r16,1.
- TEST r16,imm16.
- One internal forward JZ plus terminal backward JNZ.
- Backward CF liveness now understands ADC as both a CF consumer and producer.
- The final DEC/JNZ lazy-flag handoff remains unchanged.

## Exact guest retirement

The internal JZ conditionally skips one guest instruction (`INC DI`), so
`iterations * op_count` is no longer exact.

For this first small-CFG shape:

- r9 is free because the region uses no guest memory.
- generated Thumb code counts executions of the optional guest instruction.
- the native function returns that count.
- runtime computes exact retirement as:

```text
iterations * 10 + executed_INC_DI
```

The scheduler budgets conservatively with the maximum `iterations * 11`.

## Code-cache retention

The runtime table grows from 16 to 32 slots.

A rejected candidate that collides with an existing compiled slot no longer
evicts that compiled region merely to cache the rejection. A different
candidate replaces a compiled slot only if it actually compiles successfully.
