# dosrecomp

`dosrecomp` is microDOS's source-assisted 8086 static recompiler. The binary is authoritative; original source, maps, symbols, and manual metadata improve analysis but do not replace the machine code.

## Working milestone

The tool now:

1. loads a `.COM` image at its architectural `0100h` origin;
2. discovers reachable instructions before forming blocks;
3. marks direct branch/call targets and control-flow fallthroughs;
4. forms non-overlapping basic blocks;
5. emits readable portable C against `MdRuntime`/`MdX86`;
6. embeds the complete original image so guest data addresses remain valid;
7. emits interpreter fallback points for reachable instructions the AOT decoder does not yet understand;
8. re-enters AOT when interpreted execution reaches a known compiled `CS:IP`.

Current AOT coverage includes immediate register moves, INC/DEC/PUSH/POP, AL/AX immediate ADD/SUB/CMP, JZ/JNZ/JMP/CALL/RET, moffs MOV forms, INT, NOP, and HLT.

The high-throughput interpreter remains separate; `md_interp_step()` exists only for correctness-first mixed-mode handoff. The future decoded-block cache will return to AOT at block boundaries rather than checking after every fallback instruction.

## Usage

```bat
.\md.bat recomp tests\programs\hello.com hello 0x10c
```

Direct form:

```text
dosrecomp --input file.com --output-c out.c --output-h out.h --symbol md_recomp_name [--code-start N] [--code-end N] [--dump]
```

`--code-end` is exclusive and controls analysis only. This is useful when executable bytes are immediately followed by data in a `.COM` image.

## Next

The next compiler/runtime milestone is a shared instruction-description decoder with complete 8086 ModR/M and prefix handling. Both `dosrecomp` and the decoded-block interpreter cache should consume that description instead of growing two decoders.
