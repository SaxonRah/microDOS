# dosrecomp

`dosrecomp` is microDOS's source-assisted 8086 static recompiler. The binary is
authoritative; source-derived metadata (entry points, forced interpreter
points) improves analysis but never replaces the machine code.

## v2 (M13)

1. Loads a `.COM` image at its architectural `0100h` origin.
2. Discovers reachable code with the shared structural decoder
   (`md_decode_8086`, the same one `dosprobe` uses), so discovery never stops
   at an instruction the emitter cannot compile.
3. Forms blocks at every branch target, call return point, and re-entry point.
4. Emits portable C against `MdRuntime`/`MdX86`, using the same `ops.h`
   helpers as the interpreter (`md_x86_alu8/16`, shifts, conditions).
5. Leaves anything it does not compile as an explicit **interpreter hole**:
   generated code hands CS:IP to the interpreter, which executes it and
   returns at the next compiled entry.
6. Embeds a one-bit-per-byte code map and arms a byte-exact write guard, so
   writes to data that shares a page with code do not disable compiled code,
   while writes to instruction bytes do, immediately.
7. Exports two ways to run: standalone (`md_recomp_X(runtime, seg, budget)`,
   loads the embedded image) and attach mode (`md_recomp_X_program`, see
   `include/microdos/aot.h`) for images loaded by a real DOS.

Compiled natively: all ALU forms (00-3D, 80-83), MOV/XCHG/TEST/LEA/LES/LDS,
segment moves and pushes, INC/DEC/PUSH/POP (register and r/m), all Jcc,
LOOP/LOOPZ/LOOPNZ/JCXZ, CALL/RET/JMP (near direct and indirect, RETF),
shifts and rotates, NOT/NEG, CBW/CWD, flag instructions, PUSHF/POPF/SAHF/LAHF,
single string operations, INT, HLT. Holes: MUL/IMUL/DIV/IDIV, REP strings,
BCD adjust, IRET, IN/OUT, far CALL/JMP, and anything invalid.

## Usage

```text
dosrecomp --input file.com --output-c out.c --output-h out.h --symbol md_recomp_name
          [--code-start N] [--code-end N] [--entry N]... [--entries FILE]
          [--interp-at N]... [--name NAME] [--dump]
```

- `--code-end` is exclusive: data after code in a `.COM` image is not decoded.
- `--entry N` / `--entries FILE` add entry points that static analysis cannot
  find (for example procedures reached only through a table of pointers).
  The file has one address per line; `#` starts a comment; a line
  `0xNNNN interp` is the same as `--interp-at`.
- `--interp-at N` forces the instruction at N to run in the interpreter.

Example (what the build does for DOS2TEST):

```text
dosrecomp --input tests/dos2/DOS2TEST.COM --code-end 0x0B97 \
          --entries tests/dos2/DOS2TEST.entries --name DOS2TEST.COM \
          --output-c dos2test_recomp.c --output-h dos2test_recomp.h --symbol md_recomp_dos2test
```

Result: 1140 instructions, 1132 compiled, 8 interpreter holes.
