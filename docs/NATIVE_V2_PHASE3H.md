# Native v2 Phase 3H — multi-branch CFG

Phase 3G Fix 1 checkpoint:

- host regression suite PASS
- DOS2TEST 25/25
- MDSTRESS checksum 0xA298
- native coverage 7.7%
- native retired 1,476,905
- active 3.028 MIPS
- runtime fallback 0
- store guard rejection 0
- Phase-1 CFG region 14CB:0239 retired 746,886 instructions

Phase 3H targets the measured MDSTRESS Phase-6 inner loop:

```asm
.inner:
    test ax,1
    jz .even
    shr ax,1
    xor ax,0B400h
    jmp short .merge
.even:
    ror ax,1
    adc dx,ax
.merge:
    cmp ax,8000h
    jb .low
    not dx
.low:
    test dx,0100h
    jz .noneg
    neg dx
.noneg:
    add ax,dx
    loop .inner
```

## Compiler additions

- TEST AX,imm16 (`A9 iw`)
- XOR AX,imm16 (`35 iw`)
- CMP AX,imm16 (`3D iw`)
- SHR r16,1 (`D1 /5`)
- ROR r16,1 CF generation
- NOT r16 (`F7 /2`)
- NEG r16 (`F7 /3`) where its flags are dead before the next producer
- JB short (`72 cb`) via virtual CF
- short internal JMP (`EB cb`)
- multiple forward internal control-flow edges in an E2 LOOP region
- CF liveness through ROR -> ADC and CMP -> JB

The existing final lazy flag handoff is retained. The final instruction before
LOOP is `ADD AX,DX`; after native exit microDOS reconstructs canonical
`MD_LAZY_ADD16` from final AX and DX.

## Dynamic retirement mode 2

Phase 3G used one dynamic correction count for one optional instruction.

Phase 3H introduces a more general non-memory CFG mode:

```text
dynamic_retire = 2
retire_base_ops = 0
```

Generated Thumb uses free host register r9 to count every guest instruction
that actually executes, including Jcc/JMP/LOOP instructions. This makes
retirement exact across mutually exclusive CFG paths.

Runtime admission still budgets conservatively using:

```text
iterations * op_count
```

before entering native code.

The Phase-6 region is expected to compile as:

```text
ops=15
phase=9
dyn=2
base=0
CFsites=2
Zsites=2
terminal=E2
exit lazy=ADD16 AX,DX
```
