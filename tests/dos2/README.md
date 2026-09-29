# DOS2TEST.COM

A self-checking MS-DOS 2.0 conformance program. It runs at the `A>` prompt of
the real, booted MS-DOS 2.0 kernel and COMMAND.COM, so every call goes through
the released `MSDOS.SYS`, the microDOS interpreter, and the native device
boundary (CON, CLOCK, DISK).

```
A>DOS2TEST
```

Each line prints `PASS`, or `FAIL  code=XXXX`. The program exits through
INT 21h AH=4Ch with AL = number of failures.

Failure codes: a value below `0100h` is the DOS error returned in AX (for
example `0002` file not found, `0005` access denied). Values `E0xx` are the
test's own checks; the `xx` identifies which check inside that test failed
(see the `.badN` labels in `dos2test.asm`).

## What it covers

| Test | INT 21h services |
|------|------------------|
| version | 30h |
| STDOUT byte count | 40h on handle 1 returns the requested count (the M12 bug class) |
| char I/O | 02h, 06h output, 0Bh input status |
| drive | 19h, 0Eh |
| date / time | 2Ah/2Bh, 2Ch/2Dh round trips (CLOCK device read and write) |
| DTA | 1Ah, 2Fh |
| vectors | 35h, 25h (INT 60h, restored afterwards) |
| flags | 33h ctrl-break, 54h/2Eh verify |
| disk free | 36h (checks 512-byte sectors, 2 sectors/cluster) |
| IOCTL | 44h/00h: STDOUT is a console-output device |
| DUP | 45h, 46h, 3Eh |
| missing file | 3Dh returns error 2 |
| directories | 39h, 3Bh, 47h |
| create/write | 3Ch, 40h: 3000 bytes across three clusters in a subdirectory |
| read/seek | 3Dh, 3Fh (short read at EOF), 42h modes 0/1/2 across a cluster boundary |
| append | 3Dh read/write, 42h to end, 40h, verified by reopening |
| attributes | 43h: archive bit, read-only refuses write-open with error 5 |
| file time | 57h get/set, persisted through close/reopen |
| rename/find | 56h, 4Eh/4Fh with wildcards and the directory attribute, error 18 at end |
| delete/rmdir | 41h, 3Ah (non-empty refused), errors 2 and 3 afterwards |
| memory | 48h (including the "largest block" failure), 4Ah, 49h |
| FCB | 29h parse, 16h create, 15h/14h sequential write/read, 0Fh, 10h, 11h, 13h |
| EXEC child | 4Bh runs `DOS2TEST /CHILD`, which exits with code 42; 4Dh reads it |
| EXEC shell | 4Bh runs `COMMAND.COM /C VER` (its output appears inline) |

Scratch files live in `A:\D2T` and `A:\FCBTEST.DAT` and are removed at the
end; leftovers from an interrupted run are removed at startup. Free space is
the same before and after a run.

## Building

The assembled `DOS2TEST.COM` is committed so Windows builds need no assembler.
To rebuild after editing the source (NASM 2.x):

```
nasm -f bin -O9 -o DOS2TEST.COM dos2test.asm
```

`md.bat image dos2` places it on the FAT12 image automatically. An existing
persistent image does not get it until it is reset:

```
.\md.bat image dos2
```

## Compiled mode (M13)

The build also recompiles `DOS2TEST.COM` with `dosrecomp` using
`DOS2TEST.entries` and links the result into `microdos_msdos2`. Running
`DOS2TEST` at `A>` then executes the compiled code (look for the `[aot]`
summary line at exit). To compare against the pure interpreter:

```
set MICRODOS_NO_AOT=1        (cmd)   /   $env:MICRODOS_NO_AOT = '1'   (PowerShell)
```

If you edit `dos2test.asm`, reassemble with a listing and regenerate the
entries (every `t_*` label address) and the `--code-end` value in
`CMakeLists.txt` (address of label `cs_seg`).
