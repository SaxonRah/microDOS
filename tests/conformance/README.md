# microDOS Broad 8086 Conformance v3

This directory adds tests that are deliberately independent of DOS2TEST and
MDSTRESS.  The purpose is to make optimization decisions from broad 8086 and
MS-DOS behavior instead of over-fitting one workload.

## 1. Physical-silicon instruction vectors

`scripts/md_cpu_conformance.ps1` downloads the public
SingleStepTests 8086 vectors generated from a physical Intel P80C86A-2 and
executes them against the canonical microDOS interpreter.

The vector files are **not** checked into microDOS. They are cached under
`build-conformance/vectors/`, which is already ignored by `/build-*/`.

The comparison is architectural, not cycle-accurate:

- AX/BX/CX/DX
- CS/SS/DS/ES
- SP/BP/SI/DI
- IP
- architecturally meaningful FLAGS, with SingleStepTests undefined-flag masks
- the complete 1 MiB guest memory image after every instruction

I/O reads return `FFh`, matching the SingleStepTests convention. I/O writes are
accepted and ignored.

The prefetch queue and bus-cycle trace are intentionally not modeled. Those are
valuable for an 8088/8086 bus emulator but are outside microDOS's execution
model.

### Modes

```powershell
.\scripts\md_cpu_conformance.ps1 -Mode smoke
.\scripts\md_cpu_conformance.ps1 -Mode quick
.\scripts\md_cpu_conformance.ps1 -Mode full
.\scripts\md_cpu_conformance.ps1 -Mode hard
```

- `smoke`: small cross-section, 32 vectors per selected opcode.
- `quick`: broad opcode cross-section, 250 vectors per selected opcode.
- `full`: every available defined/alias opcode vector, no per-file limit.
- `hard`: full plus documented-by-silicon undocumented opcodes. Truly
  undefined instructions remain excluded; add them only when microDOS decides
  to emulate those quirks intentionally.

To isolate one opcode:

```powershell
.\scripts\md_cpu_conformance.ps1 -Mode full -Opcode F7
```

To compare against 8088 silicon as a secondary diagnostic:

```powershell
.\scripts\md_cpu_conformance.ps1 -Mode quick -Cpu both -ReportOnly
```

The primary target remains the 8086. 8088-specific differences should be
classified rather than blindly treated as 8086 regressions.

## 2. Deterministic randomized differential execution

`microdos_random_router_diff` generates bounded multi-instruction real-mode
programs and runs each program through:

1. the canonical interpreter; and
2. the normal DOS execution router with JIT promotion enabled.

It compares architectural state and the entire guest memory image after every
scheduler slice. Budgets rotate through:

`1, 2, 3, 7, 16, 31, 64, 257, 4096`

Generated programs mix:

- MOV/ALU immediate operations
- INC/DEC
- PUSH/POP and PUSHF/POPF
- direct memory loads/stores
- segment overrides
- shifts/rotates
- bounded LOOP sequences
- CALL/RET
- REP MOVSB/MOVSW
- flag operations

The PRNG seed is fixed by default, so a failure is reproducible.

## What failures mean

Do **not** create an allow-list merely to make the dashboard green.

Classify failures into:

1. **microDOS bug** — documented 8086 behavior is wrong.
2. **missing 8086 behavior** — e.g. a real exception or alias currently becomes
   `MD_STOP_FAULT`.
3. **intentional unsupported silicon quirk** — an undocumented/undefined form
   outside the project's compatibility target.
4. **test-harness mismatch** — usually prefetch/bus behavior, which this runner
   intentionally ignores.

Every optimization engine should ultimately agree with a silicon-validated
canonical interpreter.

## Next layer: real DOS program corpus

DOS2TEST already exercises 25 DOS service groups, including files/directories,
DTA/find-first/find-next, FCB calls, memory allocation, EXEC/child return,
COMMAND `/C`, vectors, IOCTL, DUP, date/time and file metadata.

The next corpus should therefore be **real applications**, not duplicate INT 21h
micro-tests. Candidate classes:

- archive/compression
- assembler/compiler/linker
- BASIC or another interpreter
- text/file utilities
- overlay-heavy executables
- games using ordinary DOS/BIOS services
- programs with self-modifying code
- programs with large REP/string workloads
- programs that EXEC children repeatedly

Profiles used for AOT/SRAM placement should be merged across that corpus rather
than derived from DOS2TEST alone.


## v2 harness corrections

v2 fixes two harness-only mistakes from v1:

- the randomized router target now compiles `src/host/msdos2_boot.c` and includes
  `src/host`, matching the repository's existing `exec_router_diff` wiring;
- the randomized test now calls `md_dos2_system_init(..., NULL)` with the
  correct `MdBlockCache *` argument and then clears the boot hooks for a neutral
  CPU-only differential environment.

The PowerShell driver also builds/runs the silicon test independently before
building the randomized target, so a future randomized-harness problem cannot
hide silicon conformance results in `-ReportOnly` mode.


## v3: prefetch-safe architectural comparison

The physical 8086 suite starts instructions from a populated hardware
instruction queue. microDOS intentionally has no bus/prefetch-queue model and
fetches guest code directly from RAM.

A small number of SingleStepTests vectors can therefore contain *stale queued
instruction bytes*: `initial.queue` / `bytes` describe the instruction the
physical CPU executes while backing RAM at one of those CS:IP byte locations
has already changed. Running such a vector unchanged on a no-prefetch core does
not test the same instruction.

v3 detects this condition byte-for-byte and excludes those vectors from the
architectural pass/fail total by default. They are reported separately as
`prefetch-filtered`; sample identities and exact conflicting addresses are
stored in the JSON report.

To investigate them deliberately:

```powershell
.\scripts\md_cpu_conformance.ps1 `
    -Mode full `
    -Cpu 8086 `
    -Opcode A5 `
    -ReportOnly `
    -IncludePrefetchConflicts
```

This is a classification, not a compatibility allow-list. Only cases where the
instruction expected from the physical prefetch queue differs from the bytes a
RAM-fetching microDOS core would actually execute are filtered.

### Type-0 exception stack validation

SingleStepTests documents several arithmetic FLAGS bits stored by DIV/IDIV
Type-0 exceptions as undefined on original hardware. v3 still validates the
entire exception transition and stack frame, but ignores only the two bytes of
the stacked FLAGS word for recognized Type-0 vectors. Return IP, CS, SP and all
other memory bytes remain strict.
