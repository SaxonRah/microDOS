# Native v2 Phase 3P — native REP/string micro-loops

Phase 3P targets the last clear CPU-time hole exposed by the Phase 3O
per-phase profile: MDSTRESS Phase 3 (REP string DMA / compare / scan).

Phase 3O already executes 91.5% of the combined MDSTRESS retirement natively,
but guest retirement hides the true cost of REP: one guest `REP MOVSW` counts
as one retired instruction even while the interpreter performs 2048 word
iterations in C. In the measured single-phase run, Phase 3 retired only 600k
guest instructions yet consumed 0.233 s active time and ran at 2.571 MIPS.

## Production region

The first REP region is the exact MDSTRESS Phase-3 outer loop at 14CB:02E3:

```asm
.outer:
    mov ax,bp
    mov di,buffer1
    mov cx,2048
    rep stosw
    mov si,buffer1
    mov di,buffer2
    mov cx,2048
    rep movsw
    mov si,buffer1
    mov di,buffer2
    mov cx,2048
    repe cmpsw
    mov ax,0FFFFh
    mov di,buffer2
    mov cx,2048
    repne scasw
    dec bp
    jnz .outer
```

The generated Thumb-2 body is helper-free. AX/CX/DX/BX/SP/BP/SI/DI remain
resident in r0-r7 and the REP micro-loops access guest PSRAM directly.

The region retires 18 guest instructions per outer iteration, exactly as the
8086 stream does; the thousands of REP element operations are internal work
of those four guest string instructions and do not inflate retirement.

## Safety proof

Phase 3P remains deliberately conservative:

- DF must be clear.
- DS must equal ES, matching the measured `push ds / pop es` setup.
- DS must satisfy the existing direct-word physical-wrap proof.
- Source and destination offsets must be word aligned and not wrap 16-bit
  offset space for the complete REP span.
- The STOSW and MOVSW destination spans are checked before every native entry
  and may not overlap the currently executing guest code.
- Native-v2 still byte-validates the complete guest region before re-entry.
- No C semantics helper is called inside the generated REP loops.

CMPSW and SCASW implement their repeat-stop conditions directly. The final
SCASW carry is preserved through the terminal DEC BP, and exit reconstructs
the canonical lazy DEC16 state exactly as the existing DEC/JNZ compiler does.

## Telemetry

A successful Pico run should add a resident slot similar to:

```
[native-v2] slot... 14CB:02E3 bytes=46 ops=18 phase=13 ...
```

The aggregate native-coverage percentage may move only slightly because REP
counts as one guest instruction regardless of element count. The important
metric is active time / MIPS: Phase 3P is primarily a throughput optimization,
not a retirement-coverage optimization.
