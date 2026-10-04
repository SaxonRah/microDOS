# microDOS Native v2 Phase 2B

Phase 2A established:

- simple loop: **112.468 MIPS, 2.667 host cycles/guest**
- live-register ALU mix: **107.126 MIPS, 2.800 host cycles/guest**

Phase 2B moves from compute-only proof toward an engine that can safely hand
execution back to the canonical interpreter.

## Register model

```text
r0  AX
r1  CX
r2  DX
r3  BX
r4  SP
r5  BP
r6  SI
r7  DI
r8  MdX86 *
r9  guest RAM pointer
r10 DS << 4
r11 virtual x86 CF
r12 effective-address scratch
```

## Added native operations

- `MOV r16,[SI]`
- `MOV r16,[DI]`
- `MOV [SI],r16`
- `MOV [DI],r16`
- `LEA r16,[SI+disp8]`
- `LEA r16,[DI+disp8]`
- 20-bit linear-address generation
- runtime CF tracking for ADD/SUB/CMP
- logic operations set virtual CF=0
- INC/DEC preserve virtual CF

## Native -> interpreter flags handoff

The production interpreter already has a canonical lazy flag representation.

Phase 2B reconstructs that state at a region exit when the final flag producer
is `DEC r16`:

```text
lazy_op    = MD_LAZY_DEC16
lazy_carry = virtual CF
lazy_a     = result + 1
lazy_b     = 1
lazy_res   = result
```

Therefore `md_x86_cf/zf/sf/of()` and `md_x86_flags()` see the same state after
the native region that they would see after interpretation.

## Conservative correctness guards

Phase 2B does not pretend unfinished slow paths are safe.

A native memory region returns `MD_NATIVE_V2_EXEC_FALLBACK` when:

- guest RAM is absent;
- DS is above `EFFFh`, where a word could cross the physical 1 MiB wrap;
- the region stores memory while code-page/AOT write tracking is installed.

Phase 2C adds those slow paths before full DOS admission.

## `memmix`

The new memory workload repeatedly performs:

```asm
mov ax,[si]
add ax,1
mov [di],ax
lea si,[si+2]
lea di,[di+2]
or ax,0
dec cx
jnz loop
```

It moves/transforms 32 KiB of guest data and verifies:

- output hash;
- AX/CX/SI/DI;
- IP;
- CF;
- ZF.

This is the first Native v2 benchmark with direct guest memory traffic and a
real native-to-interpreter architectural handoff.
