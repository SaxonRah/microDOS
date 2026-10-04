# Native v2 Phase 3M — Local CALL/RET graph

Phase 3M moves the hottest remaining MDSTRESS control-flow workload into Native v2: the Phase-4 inner loop at `14CB:034E`.

The guest shape is:

```asm
.inner:
    call proc_a
    loop .inner

proc_a:
    push ax
    push bx
    push dx
    call proc_b
    pop dx
    pop bx
    pop ax
    inc ax
    xor bx,ax
    ret

proc_b:
    push si
    push di
    call proc_c
    xchg si,di
    pop di
    pop si
    ret

proc_c:
    add ax,1
    adc bx,ax
    xor si,bx
    rol di,1
    ret
```

## What is native

The complete static local call graph is lowered into one helper-free Thumb-2 loop. AX/CX/DX/BX/SP/BP/SI/DI remain register-resident.

Guest control-stack semantics are **not** skipped. Every guest `CALL`, `PUSH`, `POP`, and `RET` performs the corresponding SS:SP word access. The maximum proven stack depth for this graph is 16 bytes.

`E2 LOOP` remains Phase-3K chunkable because no procedure in the proven graph reads or writes CX. A scheduler slice can therefore run only the number of complete call-graph iterations that fit its budget, then restore the real remaining CX and resume at `034E`.

## Safety

Phase 3M adds multi-span guest-byte validation. Before every native re-entry, all four non-contiguous code spans are compared byte-for-byte:

1. caller `CALL + LOOP`
2. `proc_a`
3. `proc_b`
4. `proc_c`

The dedicated CALL-stack guard rejects native execution when:

- SS would make direct 16-bit stack words cross the 1 MiB physical wrap,
- SP is too small for the proven 16-byte maximum depth, or
- the 16-byte guest stack window overlaps **any** caller/callee code span.

The existing native-store, PUSHF/POPF stack, MUL/DIV, SMC, and scheduler guards remain unchanged.

## Retirement

One inner iteration retires exactly 24 guest instructions, including guest CALL/PUSH/POP/RET and the terminal LOOP. Phase 3M therefore keeps exact retirement accounting while scheduler chunking uses `24 * iterations` as its admission cost.

## Scope

This milestone intentionally recognizes only the proven MDSTRESS Phase-4 static graph. It does **not** yet admit arbitrary near CALL/RET, indirect calls, far calls/returns, or recursive graphs. The purpose is to validate the execution model and safety machinery on the dominant real workload before generalizing it.

Expected production telemetry is a new `phase=12` native region at `14CB:034E`, with `chunk=1`, and a large reduction in Phase-4 interpreted retirement.
