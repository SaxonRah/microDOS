# M20 — general resident regions

M19.2 demonstrated ~190–200 MIPS on resident RP2350 SRAM workloads by keeping
guest state in ARM registers for the whole loop.  Its two region emitters were
still recognisers for the benchmark shapes.  M20 starts turning that mechanism
into a general DBT tier.

## M20.0 accepted region

The first generic region class is intentionally narrow and auditable:

```text
straight-line setup
        |
        v
loop target <--------------------+
        |                         |
 supported resident operations   |
        |                         |
     DEC CX                       |
     JNZ loop --------------------+
```

Supported resident architectural registers are currently AX, BX, CX and SI.
Supported operations include MOV immediate, INC/DEC, ADD/SUB/AND/OR/XOR
immediate, AL loads/stores through `[SI]`, and NOP.  The generic region only
accepts a loop when final CF semantics can be proven: either CF is preserved
from region entry or a later logical operation makes it known zero.

Memory regions must prove the same high-half window used by memloop.  SI is
established with bit 15 set, every `[SI]` access occurs while that invariant is
known, and the backedge restores it.  Stores additionally require guest pages
8..15 to be non-executable at region entry.

This gives a reusable compiler without introducing speculative correctness.

## Register convention

```text
r4 = MdRuntime / MdX86 base
r5 = guest memory base (after entry guards)
r0 = resident AX
r1 = resident BX
r6 = resident CX
r7 = resident SI
r2 = scratch
r3 = exact retired-instruction count
```

All four resident x86 registers are loaded once and flushed once.  The fixed
mapping is deliberately simple; demand-driven allocation is a later M20 step.

## Full DOS hybrid

`microdos_pico_jit.uf2` adds the translator to the portable DOS system loop.
The priority is:

```text
1. live static AOT image (MSDOS.SYS, DOS2TEST, future known apps)
2. runtime JIT for a segment with no matching static AOT image
3. canonical interpreter fallback inside the JIT
```

The runtime JIT uses `md_jit_run_until_cs_change()`.  An INT/far transfer/IRET
therefore returns to the outer system loop immediately.  If the new CS is the
DOS kernel, static kernel AOT resumes before the JIT gets another chance.

This is the first build where COMMAND.COM can execute under the on-chip dynamic
translator while the real DOS kernel remains the compiled M17/M18 image.

## Next

The next generalisation should remove the single-backedge restriction: build a
bounded work-list CFG, assign resident registers by region liveness, patch local
direct edges, and generate side exits.  Only after that should JIT invalidation
move from its current page granularity to the 64-byte chunk model already used
by static AOT.
