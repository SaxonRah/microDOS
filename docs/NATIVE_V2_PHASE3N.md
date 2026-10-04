# Native v2 Phase 3N — CALL-graph admission bridge

Phase 3M proved and emitted the static MDSTRESS Phase-4 local CALL/RET graph,
but the real Pico run showed that it never became resident. The compiler
expected the hot inner root at `14CB:034E`, while event-driven Native-v2
admission presented the enclosing outer-loop header at `14CB:034C`:

```asm
034C: xor cx,cx
034E: call proc_a
0351: loop 034E
0353: dec bp
0354: jnz 034C
```

The result was correct but performance-neutral: Phase 3M still reported the
`14CB:034C` compile reject and no phase-12 slot.

## Phase 3N change

Phase 3N adds a narrow admission bridge for exactly this proven shape.

When admission arrives at `XOR CX,CX ; CALL rel16 ; LOOP -5`, the runtime:

1. recognizes that the actual compiled graph starts two bytes later at CALL;
2. keys and caches the native region at that real entry IP;
3. reserves one guest retirement for the skipped `XOR CX,CX`;
4. treats the XOR result as the architectural 65,536-iteration LOOP count;
5. commits CX=0 only after every compile/store/stack/MUL-DIV guard has passed;
6. on a partial scheduler chunk, returns to the real `CALL` header, never to
   the XOR prelude, so the remaining CX is preserved;
7. if native entry falls back, restores CX and the original observed IP before
   returning to the interpreter.

The XOR flags are dead before the first consumer in this graph: the callee
reaches `ADD AX,1` before any flag-dependent instruction, so redirecting the
prelude does not hide an architecturally observable flag value.

## Safety retained

Phase 3N does not weaken any Phase 3M proof:

- all caller/callee bytes are still validated before native re-entry;
- the bounded 16-byte guest stack window still uses the dedicated stack guard;
- CALL/PUSH/POP/RET still perform real SS:SP guest-memory accesses;
- native store/stack/MUL-DIV/SMC guards remain unchanged;
- no C semantic helper is called in the generated hot loop;
- scheduler retirement remains exact, including the one skipped XOR only on
  the first entry of each outer-loop iteration.

## Expected Pico telemetry

The Phase-4 region should now appear as approximately:

```text
[native-v2] slot... 14CB:034E ... phase=12 ... chunk=1 ...
```

Phase 4 contains `4 * 65536 * 24 = 6,291,456` inner guest instructions. If the
region activates, total Native-v2 coverage should move from about 58% toward
roughly 91% on the same 19.2M-instruction MDSTRESS run.

Hard invariants remain DOS2TEST 25/25, checksum `0xA298`, disk writes 129, and
zero unexpected runtime/store/stack/MUL-DIV/stale failures.
