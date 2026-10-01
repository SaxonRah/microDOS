# microDOS M20.3 — LODS / register ALU / LOOP family, resident checksum loops

## Why

The M20.2.1 full-DOS profile's top JIT zero-progress sites were
`1410:044E LODSW`, `044F ADD DX,AX (03 D0)`, `0451 LOOP` (and the same at
`1717:` for the child shell): COMMAND.COM 2.0's transient checksum
(`CHECK_SUM` in COMMAND.ASM), about 15.6K instructions each time COMMAND
regains control, all of it interpreter fallback.

## What

New JIT op kinds (appended to `MdJitOpKind`):

| kind | instructions |
|---|---|
| `MD_JIT_OP_LODS` | LODSB / LODSW (no prefix) |
| `MD_JIT_OP_ALU_RR16` | ADD/OR/AND/SUB/XOR/CMP r16,r16 in both encodings (01/03, 09/0B ...) |
| `MD_JIT_OP_ALU_RR8` | the same for 8-bit registers (OR AL,AL, CMP AL,AL ...) |
| `MD_JIT_OP_LOOP` | LOOPNZ / LOOPZ / LOOP / JCXZ |

ADC/SBB stay with the interpreter.

Semantics are single-source: LODS calls the interpreter's string op, the ALU
uses the canonical lazy-flag helpers, and LOOP is one small function shared by
the C path and the native helpers.

General block emitter: LODS and register ALU run as helper calls *inside* the
native block; LOOP evaluates through a helper and then either branches
natively (internal back edge, exact budget check) or exits at the precise
target/fallthrough IP. No dispatcher round trip, no fallback.

Resident region (`md_jit_emit_resident_lodsloop`): an optional straight-line
prologue, then `[LODS / ALU r16 / NOP]* LOOP` back to the loop head, with at
least one LODS. AX, CX and SI live in r0/r6/r7, DS*16 in r1, guest memory in
r5, the budget in r12. Exactness:

- dynamic trip count (CX at entry, CX=0 -> 65536); admission per iteration,
  so a budget exit always lands on the loop head;
- every byte address is ((DS<<4)+SI) mod 2^20; LODSW's second byte wraps
  independently (interpreter read16 behaviour);
- DF is read once at entry and selects one of two loop copies;
- each ALU op records its lazy operands/result and lazy kind, so the flags
  are always those of the last ALU op executed;
- one ALU operand must be resident, or both operands the same register
  (XOR DX,DX).

COMMAND.COM's checksum block (`XOR DX,DX` prologue + `LODSW / ADD DX,AX /
LOOP`) becomes a resident region; `CLD`, `SHR CX,1` and `MOV DS,AX` before it
are still interpreted once per call.

## Validation

- `tests/test_jit.c` (M20.3 spec): passes as native Thumb-2 under qemu-arm;
  registered in ctest (host: semantic checks; code-shape checks are native
  only because 64-bit host offsets exceed Thumb immediates).
- `tests/test_jit_diff.c`: 14 fixtures, identical registers/flags/1 MiB of
  memory/instruction counts vs the interpreter, plus budget slicing (7 and
  50 instructions) with the interpreter advanced by exactly the retired count
  after every slice. New fixtures: COMMAND.COM checksum (real encodings,
  DS != 0), LODSW across the 1 MiB wrap, CX=0 at entry, an ALU mix covering
  every operand placement, LODSB+LOOPNZ, LOOPZ, JCXZ+LOOP.
- Mutations planted in the resident code (no second-byte wrap; backward copy
  stepping forward) are each caught by the fixture aimed at them.
- Host ctest 10/10; Cortex-M33 compile clean; JIT firmware 478 KB SRAM.

## Hardware validation

1. `microdos_bench.uf2`: all rows `ok` (unchanged fixtures, no regressions).
2. `microdos_pico_jit.uf2`: DOS2TEST 25/25. In the `[jit]` block the
   `1410:044E/044F/0451` zero-progress sites should be gone and
   `resident=` should be non-zero.

## Hardware result and full-DOS reproduction (M20.4 tooling)

On RP2350, per DOS2TEST session: JIT fallback 81,407 -> 2,924, JIT-native
539 -> 79,022, resident-region instructions 78,093; DOS2TEST 25/25.

`tools/jit_qemu_check.sh` now also runs the whole DOS2TEST session with the
runtime JIT (Pico configuration: 64 block slots, 64 hotness slots,
threshold 2, 24 KiB arena) as native Thumb-2 under qemu-arm. It reproduces
the hardware counters to within one instruction (native 79,021 / fallback
2,925 / cold 2,096 / zero 828 / compiles 558) in about a second.

### Reading the hot-site list

The firmware's hot-site table is a space-saving summary with 8 entries: a
newly admitted site inherits the evicted count + 1, so `~` counts are upper
bounds. After M20.3 it showed `9D1F:0216...021C` around 387 "cold" each;
exact per-site counting (instrumented QEMU run) shows:

- 2,096 cold events over 1,259 distinct sites;
- 762 sites cold exactly once (the threshold-2 warm-up, by design);
- 497 sites cold repeatedly from 64-slot hotness collisions, at most 14 each.

Total cold cost is about 0.06% of the session's instructions. A larger
hotness table would recover at most ~1,300 instructions per session and is
not worth the SRAM at 91% use. After M20.3 the JIT tier has no significant
hotspot in this workload: ~95% compiled kernel, ~2% JIT-native, ~0.09% JIT
fallback; the remaining interpreted ~2.6% is the synthetic BIOS trampoline
layer at 0800h, dominated by the native device services it calls.
