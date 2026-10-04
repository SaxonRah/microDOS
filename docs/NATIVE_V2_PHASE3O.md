# Native v2 Phase 3O — real MDSTRESS CALL-graph encoding fix

Phase 3N established that event-driven admission reaches the Phase-4 graph:
`14CB:034C` disappeared from the reject-slot telemetry, proving that the
`034C -> 034E` redirect recognizer fired. Yet no phase-12 slot became resident.

The remaining failure was a byte-pattern mismatch inside the local CALL/RET
graph compiler.

## Root cause

The Phase-3M host fixture hand-encoded two instructions using legal encodings
that differ from the bytes NASM chose for the checked-in `MDSTRESS.COM`:

```text
source          real MDSTRESS.COM   old synthetic fixture
--------------  ------------------  ---------------------
xchg si,di      87 F7               87 FE
add ax,1        05 01 00            83 C0 01
```

`XCHG` is symmetric, so `87 F7` and `87 FE` both exchange SI and DI. `ADD AX,1`
can use either the accumulator-immediate form `05 iw` or the sign-extended
Group-1 `83 /0 ib` form; both encodings above are three bytes and have the same
8086 architectural effect for immediate 1.

Because the graph compiler deliberately proves a static callee graph before
emitting native code, the overly literal matcher rejected the real DOS binary
while the synthetic host fixture passed.

## Phase 3O change

Phase 3O keeps the narrow graph proof but makes these two checks semantic:

- XCHG accepts exactly the two ModR/M forms that exchange SI and DI;
- ADD accepts exactly `ADD AX,0001h` in either three-byte encoding.

All following bytes, call targets, procedure sizes, return sites, and graph
spans remain exact. No new opcode family is generally admitted.

The primary host fixture now uses the exact bytes from the checked-in
`tests/dos2/MDSTRESS.COM`:

```text
015F: 50 53 52 E8 07 00 5A 5B 58 40 33 D8 C3
016C: 56 57 E8 05 00 87 F7 5F 5E C3
0176: 05 01 00 13 D8 33 F3 D1 C7 C3
034C: 33 C9 E8 0E FE E2 FB ...
```

A second regression rewrites only those two instructions to the alternate
encodings and requires the same phase-12 graph to compile.

## Safety unchanged

Phase 3O does not alter generated Thumb code or runtime scheduling:

- the 034C -> 034E one-shot admission bridge is unchanged;
- the graph still retires exactly 24 guest instructions per inner iteration;
- E2 scheduler chunking is unchanged;
- all four non-contiguous guest spans are byte-validated before re-entry;
- the dedicated 16-byte SS:SP overlap/wrap guard is unchanged;
- CALL/PUSH/POP/RET still perform real guest stack memory accesses;
- no C semantic helper is called in the hot native loop;
- store, stack, MUL/DIV, stale-code, and runtime fallback protections remain.

## Expected Pico telemetry

```text
[native-v2] slot... 14CB:034E ... phase=12 ... ops=24 ... chunk=1 ...
```

The Phase-4 inner graph contains `4 * 65536 * 24 = 6,291,456` guest
instructions. With the same 19.2M MDSTRESS workload, successful admission
should move Native-v2 coverage from about 58% toward 90-91%.

Hard invariants remain DOS2TEST `25/25`, checksum `0xA298`, disk writes 129,
and zero unexpected runtime/store/stack/MUL-DIV/stale failures.
