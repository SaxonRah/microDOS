# microDOS M19.2 resident-region DBT overlay

Baseline: apply on top of the M19.1 overlay (or a tree containing the same M19.1 files).

M19.1 proved direct Thumb lowering + local native chaining on RP2350:

- loop / SRAM: 22.126 MIPS (AOT 16.612)
- loop / PSRAM: 22.073 MIPS (AOT 16.575)
- memloop / SRAM: 20.524 MIPS (AOT 28.572)
- memloop / PSRAM: 9.081 MIPS (AOT 10.357)

M19.2 attacks the remaining execution overhead with **resident native regions**.

## What changes

For the recognized hot loop regions, guest state no longer bounces through `MdX86` every iteration.

### loop.com

The dynamic `MOV CX,imm / DEC CX / JNZ` region keeps CX in an ARM register. The hot loop is effectively:

```asm
subs r0, #1
bne  loop
```

Incoming CF is materialized/captured once because 8086 `DEC` preserves CF. Final x86 lazy-flag state and CX are committed once when the loop exits.

### memloop.com

The hot state is pinned as:

```text
r0 = CX
r1 = SI
r2 = AL scratch
r3 = 8000h mask
r5 = guest-memory base
```

The hot loop is effectively:

```asm
ldrb r2,[r5,r1]
adds r2,#3
strb r2,[r5,r1]
adds r1,#97
uxth r1,r1
orrs r1,r3
subs r0,#1
bne  loop
```

No `MdX86` register traffic occurs inside that loop.

## Safety guards

Resident regions are optimizations, not new guest semantics.

- Exact budget is checked once before entering a resident region. If the remaining budget cannot finish it, the native region returns without changing guest state and the canonical path advances normally.
- `memloop` specialization requires `DS == 0`.
- Its entire possible store window (linear 8000h-FFFFh) is checked once to ensure none of those pages has ever been marked executable. If that guard fails, M19.1/canonical execution is used instead, preserving self-modifying-code invalidation.
- All unrecognized code continues through the M19.1 direct-lowering/JIT fallback.

## New counters

The JIT benchmark now also prints:

```text
resident-regions=
resident-entry=
resident-instr=
```

For the supplied fixtures, the expected shape is approximately:

```text
loop:
  resident-regions=1
  resident-entry=1
  resident-instr=131071

memloop:
  resident-regions=1
  resident-entry=1
  resident-instr=229378
```

The separate HLT remains a second tiny native entry.

## Build

```powershell
cd C:\microDOS
.\md.bat clean
.\md.bat build host
.\md.bat test
.\md.bat build pico
```

Flash:

```text
C:\microDOS\build-pico\out\microdos_bench.uf2
```

Open serial:

```powershell
python -m serial.tools.miniterm COM5 115200 --exit-char 24
```

## What to report

Send the full table, especially:

```text
loop     SRAM  jit
loop     PSRAM jit
memloop  SRAM  jit
memloop  PSRAM jit
```

plus their two `[jit]` lines.

M19.2 is specifically testing the compute ceiling of resident DBT versus the PSRAM bandwidth/latency ceiling. A 200-300 MIPS result is not expected for memory-heavy DOS code at 300 MHz; it is a plausible stretch target only for extremely small register/branch regions where guest instructions approach one native instruction each.

## Validation performed while producing this overlay

- `src/runtime/jit_thumb2.c` compiles warning-clean as C11 with `-Wall -Wextra -Werror` under a host ABI stub.
- The same source cross-compiles cleanly for `armv8m.main` / Cortex-M33 Thumb with Clang 17.
- ARM record layouts were dumped during the cross-compile and confirm the generated-code offsets (`MdX86.memory=36`, `code_page_executable=44`, `ds=22`, lazy fields 28-34, `MdRuntime.stop_reason=96`).
- The exact compact Thumb encodings newly used by M19.2 (`CMP reg`, `UXTH`, resident backedges) were independently assembled/disassembled for Cortex-M33.
- The generated call frame was hardened from a 20-byte save to a 24-byte save (`push {r3-r7,lr}`), preserving the AAPCS 8-byte stack alignment at generated-code calls into C.

The RP2350 hardware run remains the authority for performance and the final generated-code execution check.
