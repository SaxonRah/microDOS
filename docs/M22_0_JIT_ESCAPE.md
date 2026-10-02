# M22.0 — bounded zero-progress JIT escape

## Why

MDSTRESS v2 was run twice on the same 300 MHz RP2350 firmware family:

- AOT DOS kernel + application interpreted
- AOT DOS kernel + profiled runtime JIT application

Across the same 20.8 million guest instructions, the plain-interpreter
application runs completed in about 7.63 seconds of active time; the current
JIT runs took about 31.27 seconds. The largest losses were not compilation
time. They were repeated zero-progress exits where native code retired no
guest instruction, the C dispatcher interpreted exactly one instruction, and
then immediately retried JIT lookup.

Examples from the clean phase-isolated capture:

- ALU/flags: 1,311,132 zero exits
- CALL/RET/stack: 3,408,230 zero exits
- MUL/DIV/rotate: 524,704 zero exits
- branch/LFSR: 2,031,198 zero exits
- rare-opcode sweep: 2,883,871 zero exits

M22.0 establishes the invariant: enabling the JIT should not turn an
unsupported-dense program into a dispatcher benchmark.

## Change

When a native block returns **zero retired instructions**, the JIT now enters
the canonical interpreter for up to 16 guest instructions before retrying
native lookup.

The burst:

- never exceeds the caller's remaining instruction budget;
- stops immediately on a guest stop condition;
- stops immediately on CS change when `md_jit_run_until_cs_change()` is in
  use, and records the actual instruction that changed CS;
- uses the normal interpreter and normal memory/code-invalidation paths;
- counts every instruction it executes in `fallback_instructions`;
- keeps `zero_progress_fallbacks` as a zero-exit event count.

Cold admission, compile failure and budget fallback behavior are unchanged.

The burst length is compile-time tunable:

```c
-DMD_JIT_ZERO_ESCAPE_BURST=8
-DMD_JIT_ZERO_ESCAPE_BURST=16
-DMD_JIT_ZERO_ESCAPE_BURST=32
```

Default: **16**.

## Host validation

The non-ARM reference executor now stops before `MD_JIT_OP_FALLBACK`, matching
the emitted Thumb backend instead of silently interpreting the fallback
inside the block. This means the existing `jit_diff` fixtures and their
7/50-instruction budget slicing exercise the same M22.0 zero-progress path on
the host.

No guest semantics, opcode implementation, region recognizer or invalidation
rule is changed in M22.0.

## Expected hardware signature

With `microdos_pico_jit_profile.uf2`, unsupported-heavy MDSTRESS phases should
show:

- a large drop in `entry=` and `return=`;
- a large drop in `zero=` events;
- `fallback=` may remain substantial because it counts all guest instructions
  deliberately executed by the interpreter escape;
- active MIPS should move toward the plain-interpreter baseline.

The most important first comparison is phase 4:

- interpreter baseline: ~4.020 MIPS
- pre-M22 JIT baseline: ~0.603 MIPS

M22.0 is successful if JIT-enabled execution stops being catastrophically
slower even before M22.1 adds more native opcode coverage.
