# MS-DOS 2.0 bring-up

## Goal

Run the released MS-DOS 2.0 kernel and command interpreter through the same hybrid
execution system already proven by the microDOS synthetic programs:

```text
known guest block  -> generated native C
unknown guest block -> decoded-cache interpreter
hard/rare instruction -> canonical interpreter
```

The released binary is authoritative. Microsoft's released assembly source is
source-assisted metadata for names, segment layout, initialization contracts,
jump-table discovery, and review.

## Pinned upstream

microDOS pins Microsoft/MS-DOS commit:

```text
2d04cacc5322951f187bb17e017c12920ac8ebe2
```

Install it with:

```powershell
.\md.bat deps msdos
```

No Microsoft source or binary is silently copied into the microDOS repository by
this milestone. The dependency script creates a sparse local checkout under
`third_party/msdos`.

## Why COMMAND.COM and MSDOS.SYS need different treatment

`COMMAND.COM` is a normal COM-style image. Its source explicitly places `PROGSTART`
at `0100h`, so the initial analysis mapping is:

```text
image byte 0 -> guest offset 0100h
entry        -> 0100h
```

`MSDOS.SYS` is a raw linked kernel image rather than a COM process. `MSHEAD.ASM`
defines a `START` segment whose first instruction jumps to `DOSINIT`. The released
binary begins with the corresponding near jump, so the initial mapping is:

```text
image byte 0 -> guest offset 0000h
entry        -> 0000h
```

This is only the kernel's binary layout. Actually invoking `DOSINIT` also requires
the OEM/SYSINIT register and device-chain contract; that is a later bring-up step,
not something the analyzer fabricates.

## Run the real-binary inventory

```powershell
.\md.bat analyze dos2
```

or separately:

```powershell
.\md.bat analyze msdos
.\md.bat analyze command
```

Reports are written to:

```text
build-analysis/msdos-sys.json
build-analysis/command-com.json
```

## Binary-first findings through milestone 7

The pinned release still exposes the same initial recursive-descent frontier:

```text
MSDOS.SYS
  3446 reached instructions / 8331 reached bytes
  398 conditional branches, 266 direct calls, 96 direct jumps, 118 returns
  2 unresolved indirect calls, 3 unresolved indirect jumps

COMMAND.COM
  617 reached instructions / 1461 reached bytes
  75 conditional branches, 26 direct calls, 27 direct jumps, 11 returns
  1 unresolved indirect call, 2 unresolved indirect jumps
```

Canonical interpreter coverage across the bring-up tranches is now:

```text
                 initial     milestone 5     milestone 6     milestone 7
MSDOS.SYS         55.17%        77.54%          89.55%          100.00%
COMMAND.COM       66.61%        88.01%          96.43%          100.00%
```

Milestone 7 closes the discovered semantic gap with LES/LDS, D0-D3 shifts and
rotates, Group 3, FE/FF Group 4/5, POP r/m16, XCHG/LEA, TEST, CBW/CWD,
PUSHF/POPF/SAHF/LAHF, direct and indirect near/far control flow, IRET, flag
control, decimal/ASCII adjust instructions, AAM/AAD/XLAT, and port I/O.

100% here means every instruction **on this currently known static frontier** can be
executed by the canonical interpreter. It does not claim that all code in the image
is now discovered. Runtime indirect targets, interrupt-installed entry points, and
source-known alternate entries will expand the reachable set during actual DOSINIT
bring-up.

## Milestone 8: enter the real kernel

Milestone 8 adds a raw-image loader plus `microdos_msdos2`, which reproduces the
contract visible in Microsoft's `SYSINIT.ASM` and `MSINIT.ASM`:

- SYSINIT performs a far call to offset `0000h` in the final DOS segment.
- `DS:SI` points to the first OEM device header (CON).
- `DX` contains the physical-memory limit in paragraphs.
- DOSINIT saves the caller's `SS:SP`, switches to its own initialization stack, and
  restores the original stack before its final far return.
- DOSINIT initializes CON, follows the character-device chain until CLOCK, then walks
  block devices and consumes the unit count and BPB pointer returned by their INIT
  request.

microDOS therefore installs this guest-visible chain:

```text
CON -> AUX -> PRN -> CLOCK -> DISK -> FFFF:FFFF
```

Each header points to tiny real-mode strategy/interrupt trampolines. Those trampolines
use private `INT F0h/F1h` hooks to cross into the native host layer and then `RETF`
back to DOS exactly like a far-called DOS device driver. `INT F2h` is reserved for
the synthetic SYSINIT return trampoline.

The block INIT reply exposes one FAT12 drive with this BPB:

```text
bytes/sector       512
sectors/cluster      2
reserved sectors     1
FATs                 2
root entries        112
total sectors       720
media               FDh
sectors/FAT           2
```

This is only the initialization contract. Read/write/media/clock/console requests are
not silently accepted in M8; they are surfaced as the next bring-up boundary.

Run:

```powershell
.\md.bat boot msdos2
```

The success criterion is now behavioral: execute the released kernel through its real
`DOSINIT` path and either return to the synthetic SYSINIT caller or report the first
precise missing runtime/OEM contract.

During construction of this runner, the exact released entry sequence `E9 78 3E`
exposed an unsequenced C expression in the canonical E9/EB implementation. The fix
sequences displacement fetch before adding it to IP, and `0000:E9 78 3E -> 3E7B` is
now a permanent regression test. This is a useful example of why bring-up executes the
released binary instead of relying only on static opcode coverage.

## Milestone 9: real DOS console services

The first M8 execution reached normal device I/O after all five INIT requests. DOS then
repeated CON function 5, 10, and 8 calls because M8 returned `8103h` for every non-INIT
request. Microsoft's DOS 2 character-driver contract requires different results:

```text
CON function 5, no key pending   -> 0300h BUSY|DONE
CON function 10, output status   -> 0100h DONE
CON function 8/9, write          -> transfer far pointer + count, then 0100h DONE
```

M9 implements those operations through platform callbacks. The Windows runner binds
console write to stdout; future Pico code can route the same callback to the catBUS
console/display path. Function 4 destructive read remains explicit: it succeeds only
when a platform read callback is installed. This preserves the bring-up rule that
missing services are surfaced rather than fabricated.

The runner also suppresses repeated identical error-volume after the first diagnostic
window, while preserving counters and final CPU state.

## Bring-up sequence after M9

1. Run the released `MSDOS.SYS` under the M9 canonical runner and capture the next
   real stop/boundary after console I/O succeeds.
2. Implement only the device/interrupt behavior the real boot path requests.
3. Record dynamically reached indirect targets and feed them back into `dosprobe`.
4. Once DOSINIT returns cleanly, model enough of SYSINIT to perform its post-kernel
   setup and transition toward `COMMAND.COM`.
5. After the boot path is behaviorally stable, profile it and promote hot decoded
   blocks/opcodes into cache-specialized and AOT execution.
6. Replace host shims one boundary at a time with Pico 2 / MicroRender / MicroWave /
   MicroConsole backends.
