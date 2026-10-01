# M20.2.1 — profile-driven control transfers and hot code selection

M20.1 proved two separate things on RP2350 at 300 MHz:

- the bounded CFG backend is fast (`branchmix` about 106 MIPS); and
- real COMMAND.COM was still mostly outside native execution.

The interval profile explained why. The dominant JIT zero-progress sites were
synthetic BIOS trampoline instructions at segment `0800h` (`INT` / `RETF`),
while real application traffic repeatedly crossed INT and CALL/RET boundaries.
The 24 KiB code arena also flushed frequently because every cold entry was
compiled immediately.

M20.2 addresses those measured problems without changing the M19.2/M20.1
resident code generator.

## Tier routing

Execution priority remains:

1. exact static AOT (MSDOS.SYS, DOS2TEST);
2. runtime JIT for application code;
3. canonical interpreter fallback.

The synthetic BIOS segment is now explicitly excluded from the runtime JIT.
It runs through `md_interp_run_until_cs_change()`, which prevents its tiny
INT/RETF stubs from occupying JIT block slots or executable SRAM.

## Hotness gate

The full DOS JIT build uses `MD_JIT_HOT_THRESHOLD=2`.

A new `CS:IP` is interpreted on its first encounter and compiled on the second.
The benchmark firmware keeps threshold 1 so benchmark numbers still measure
compiled execution rather than warmup policy.

The small hotness table survives code-arena flushes, so proven-hot entries can
be recompiled immediately after a flush instead of warming up again.

## Native control boundaries

The JIT predecoder now recognizes:

- near CALL (`E8`)
- near RET (`C3`, `C2`)
- far RET (`CB`, `CA`)
- INT3 / INT imm8 / INTO (`CC`, `CD`, `CE`)
- IRET (`CF`)

Generated Thumb calls a shared M20.2 control helper that performs the canonical
stack/interrupt operation, retires exactly one guest instruction, then returns
to the native dispatcher. These instructions are counted as native JIT work
rather than zero-progress interpreter fallback once compiled.

The helper uses the same core primitives (`md_x86_push`, `md_x86_pop`,
`md_runtime_interrupt`, `md_x86_set_flags`) as the interpreter. It does not
reimplement DOS or interrupt services.

## Transfer profiling

M20.1 recorded a CS-changing exit after the transfer, so the hot-site key was
the destination. M20.2 captures the source block/control instruction before
execution and reports both endpoints:

```text
[jit] ~ 12345  9D1F:021C op=CD -> 1000:113A  cs-change
```

This is the form needed to decide which application control edges should be
chained or absorbed into future regions.

## New benchmark: callmix

`callmix` performs 32,768 iterations of:

```asm
call sub
dec  cx
jnz  loop

sub:
add  bx,3
ret
```

Expected final state:

- `CX=0000h`
- `BX=8000h`
- stack pointer restored to its entry value
- 163,843 guest instructions including HLT

The standalone JIT should report zero fallback and 65,536 native control
instructions (32,768 CALL + 32,768 RET).

## Full-DOS acceptance

- host tests remain 8/8;
- DOS2TEST remains 25/25;
- COMMAND.COM built-ins still operate normally;
- BIOS traffic appears under `bios-bypass`, not JIT zero fallback;
- `zero` fallback should fall dramatically;
- code-cache flushes/compiles should fall compared with M20.1;
- hot `cs-change` sites must show the application source and kernel/other
  destination.
