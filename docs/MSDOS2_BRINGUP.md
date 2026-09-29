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

## Initial binary-first findings

A reference scan of the pinned release, using entry `0000h` for `MSDOS.SYS` and
`0100h` for `COMMAND.COM`, establishes substantial direct-control-flow regions
without pretending indirect targets are known:

```text
MSDOS.SYS    16690-byte image
  about 3446 recursively reached instructions
  about 8331 reached bytes
  about 55% currently executable by the canonical interpreter
  about 44% currently AOT-supported
  398 conditional branches
  266 direct calls
  96 direct jumps
  118 returns
  5 unresolved indirect control transfers

COMMAND.COM  15480-byte image
  about 617 recursively reached instructions
  about 1461 reached bytes
  about 67% currently executable by the canonical interpreter
  about 59% currently AOT-supported
  75 conditional branches
  26 direct calls
  27 direct jumps
  11 returns
  3 unresolved indirect control transfers
```

These numbers are analysis-frontier numbers, not a statement that the remaining
bytes are data. Additional source-assisted seeds and indirect-target recovery will
expand them.

The first high-value missing opcode families seen in reachable code are:

```text
80/81/83   ALU r/m, immediate
8C/8E      MOV segment register
C6/C7      MOV r/m, immediate
70-7F      complete conditional-jump family
30-33      XOR r/m,reg
06/07/...  segment PUSH/POP
A4-AF      string operations
F2/F3      REP prefixes
C4/C5      LES/LDS
D0-D3      shift/rotate
E0-E3      LOOP/JCXZ
F6/F7      TEST/NOT/NEG/MUL/IMUL/DIV/IDIV group
FE/FF      INC/DEC and indirect CALL/JMP/PUSH groups
```

That list should drive runtime implementation order. It is more useful than adding
opcodes according to numeric order.

## Bring-up sequence from here

1. Implement the high-frequency real-DOS opcode tranche in the canonical
   interpreter, with tests for flags and addressing.
2. Add prefix state and string operations, including REP/REPE/REPNE and direction
   flag behavior.
3. Move the shared structural decoder underneath `dosrecomp` and the decoded block
   cache so all engines consume one instruction-boundary implementation.
4. Generalize `dosrecomp` from COM-only loading to raw images with explicit image
   base and entry, allowing `MSDOS.SYS` to be generated without COM assumptions.
5. Build a host-side minimal OEM/SYSINIT environment matching the documented DOS
   initialization contract and enter the real `DOSINIT` path.
6. Bring enough character/block device behavior online to reach the point where DOS
   installs INT 21h and can EXEC the released `COMMAND.COM`.
7. Replace host devices one boundary at a time with Pico 2 backends.

The success milestone is not merely printing a DOS-looking prompt. It is the
released `MSDOS.SYS` installing its own services and starting the released
`COMMAND.COM` through the hybrid execution engine.
