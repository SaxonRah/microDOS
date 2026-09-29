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

## Mixed execution after milestone 3

Generated code now includes a compact resume predicate listing valid compiled block entries. If execution reaches unknown code, an indirect target, or code invalidated by a guest write, AOT enters the runtime-owned decoded block cache with `md_interp_run_cached_until()`.

The cached path may execute any number of blocks and canonical interpreter fallbacks before returning. It hands control back only when `CS:IP` reaches a still-valid compiled entry. If guest code has modified a compiled code page, the generated image-wide write epoch is stale and AOT remains disabled for the rest of that invocation.

This keeps correctness simple for overlays and self-modifying code while preserving a fast path back into native code for ordinary unresolved control flow.
