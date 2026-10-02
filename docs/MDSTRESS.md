# MDSTRESS v2 — clean phase-isolated microDOS profiling

MDSTRESS v2 keeps the nine v1 workloads but fixes the measurement problem
found in the first hardware captures.

The v1 `/S` workflow waited inside DOS character input. `Ctrl+]` was noticed by
the Pico host, but the statistics request could not be serviced until DOS
returned from its blocking input path. That polluted phase boundaries.

v2 adds **single-phase process mode**:

```text
MDSTRESS       run all nine phases
MDSTRESS /S    manual stepped/debug mode
MDSTRESS 1     run phase 1 only and exit
...
MDSTRESS 9     run phase 9 only and exit
```

The automated capture tool now profiles from one `A>` prompt to the next. No
benchmark phase waits inside DOS keyboard input.

## Install

```powershell
cd C:\microDOS

Expand-Archive `
    "$HOME\Downloads\microDOS-mdstress-v2.zip" `
    -DestinationPath . `
    -Force

.\md.bat clean
.\md.bat build host
.\md.bat test
.\md.bat image dos2
.\md.bat build pico
```

The normal DOS/Pico FAT12 image contains `MDSTRESS.COM`.

## Manual verification

```text
A>MDSTRESS 1
A>MDSTRESS 4
A>MDSTRESS 9
```

Every invocation prints its loaded code segment:

```text
[MDSTRESS] CS=0x....
```

This lets JIT hot-site `CS:IP` output be matched unambiguously to
`MDSTRESS.MAP`.

## Automated capture

**Close miniterm first.** Only one Windows program can own COM5.

Then, from PowerShell:

```powershell
cd C:\microDOS
python .\scripts\mdstress_capture.py COM5
```

The script now:

1. synchronizes with COMMAND.COM;
2. automatically accepts the default DOS date/time on a freshly flashed board;
3. takes `Ctrl+]` statistics at the `A>` prompt;
4. runs `MDSTRESS N`;
5. waits for the program to return to `A>`;
6. takes another `Ctrl+]` snapshot;
7. records the second snapshot's `since previous Ctrl+]` interval as phase N;
8. repeats for phases 1 through 9.

It writes three files:

```text
mdstress-v2-YYYYMMDD-HHMMSS.txt   raw serial transcript
mdstress-v2-YYYYMMDD-HHMMSS.csv   one machine-readable row per phase
mdstress-v2-YYYYMMDD-HHMMSS.json  same result with metadata
```

Selected phases are supported:

```powershell
python .\scripts\mdstress_capture.py COM5 --phases 1,4,5,9
```

## Firmware matrix

For the first permanent baseline, capture these separately:

```text
microdos_pico_nokernel.uf2
microdos_pico_region.uf2
microdos_pico.uf2
microdos_pico_jit_profile.uf2
```

Rename/label the resulting captures so they remain obvious.

Example:

```powershell
python .\scripts\mdstress_capture.py COM5 `
    --label jit-profile `
    --output mdstress-jit-profile
```

After flashing another firmware:

```powershell
python .\scripts\mdstress_capture.py COM5 `
    --label aot-kernel `
    --output mdstress-aot-kernel
```

Then compare any CSVs:

```powershell
python .\scripts\mdstress_compare.py `
    .\mdstress-nokernel.csv `
    .\mdstress-region.csv `
    .\mdstress-aot-kernel.csv `
    .\mdstress-jit-profile.csv
```

## Phases

| # | Phase | Primary stress |
|---|---|---|
| 1 | ALU + FLAGS + SHIFT | ADD/ADC/SUB/XOR/TEST, lazy flags, D1 rotates, Jcc |
| 2 | MEMORY + STORE | `[BX+SI]` addressing, guest reads/writes, store tracking |
| 3 | REP STRINGS | REP STOSW/MOVSW, REPE CMPSW, REPNE SCASW |
| 4 | CALL/RET + STACK | nested near calls, returns, PUSH/POP |
| 5 | MUL/DIV + ROTATE | F7 MUL/DIV, rotates, arithmetic dependencies |
| 6 | BRANCH/LFSR | flag-heavy mixed branches and control flow |
| 7 | RARE OPCODES | XCHG, PUSHF/POPF, LAHF/SAHF, CBW/CWD, NOT/NEG |
| 8 | DOS SERVICES | file I/O, seek, memory alloc/free, checksum loop |
| 9 | SELF MODIFY | repeated writes to live code + calls into modified target |

## Important interpretation note

Phase 3 intentionally makes guest-MIPS a strange metric: one REP instruction
can perform thousands of memory operations. For that phase compare active
seconds and engine counters, not only guest MIPS.

## v1 findings that motivated v2

The first hardware sweep already exposed large effects worth preserving for
comparison:

- the general decoded cache could be several times slower than the plain
  threaded interpreter on control-heavy code;
- the JIT showed millions of `zero-progress` fallbacks;
- call-heavy JIT execution repeatedly bounced through the C dispatcher;
- D1/F7/ADC sites appeared prominently in JIT fallback/hot-site output.

v2 exists to measure those effects with clean process-level phase boundaries
before changing the JIT architecture.
