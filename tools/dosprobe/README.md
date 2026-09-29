# dosprobe

`dosprobe` is the binary-first intake tool for real DOS software.

It uses the shared structural 8086 decoder to recursively walk direct control flow
without requiring an instruction to be implemented by the interpreter or AOT
emitter. That distinction is important: an unsupported opcode is a coverage item,
not a reason to lose the rest of the binary's instruction boundaries.

Examples:

```text
# ordinary COM image
build-host\Release\dosprobe.exe --input COMMAND.COM --base 0x100 --entry 0x100

# raw DOS kernel image
build-host\Release\dosprobe.exe --input MSDOS.SYS --base 0 --entry 0
```

The report includes:

- recursively reachable instruction and byte counts;
- current interpreter and AOT semantic coverage;
- prefix use;
- direct and indirect control-flow counts;
- possible non-8086 paths;
- the most frequent reachable opcodes that still require interpreter semantics;
- a few guest addresses for every reported opcode.

`--json path` writes the same data in machine-readable form. microDOS stores the
MS-DOS 2.0 reports under `build-analysis/`.

## Important limitation

This is recursive descent, not a proof that every byte of a DOS binary is code.
Indirect calls/jumps cannot be followed without more metadata, while terminal DOS
interrupts are not yet semantically recognized by the scanner. The released DOS
source, linker files, and later symbol/map metadata will be used to seed additional
entry points. Therefore `reachable_bytes` should be read as "code we can establish
from the current seeds," not "percentage of the executable that contains code."
