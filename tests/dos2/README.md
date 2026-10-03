# DOS2TEST

`DOS2TEST.COM` is the full-system MS-DOS 2.0 regression program.

Run it from the DOS prompt:

```text
A>DOS2TEST
```

A successful run reports all checks as `PASS`.

The suite covers representative INT 21h behavior including console I/O, date/time, directories, file I/O, attributes, seek, rename/delete, memory allocation, FCB operations and EXEC.

The assembled COM image is committed so normal Windows builds do not require an assembler.

To rebuild after editing `dos2test.asm`:

```text
nasm -f bin -O9 -o DOS2TEST.COM dos2test.asm
```

Recreate the DOS disk image afterward:

```powershell
.\md.bat image dos2
```

Keep `DOS2TEST.entries` and the configured code range synchronized with source changes.