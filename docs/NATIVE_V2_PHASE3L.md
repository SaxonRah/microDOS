# Native v2 Phase 3L — DEC/JNZ scheduler chunking

Phase 3K result:

- DOS2TEST 25/25
- MDSTRESS 0xA298
- Native coverage 47.6%
- Native retirement 9,145,204 guest instructions
- Active 6.075 MIPS
- 112 chunked native entries
- 491,379 chunked loop iterations
- all runtime safety guards zero

Phase 3L extends budgeted native execution to terminal:

```asm
dec counter
jnz loop_header
```

## Compile-time proof

A DEC/JNZ region is chunkable only when:

- the terminal pair is exactly `DEC r16 / JNZ region_header`
- the counter is not read before the terminal DEC
- the counter is not written anywhere else in the body

The compiler records:

```text
chunkable_loop = 2
```

(`1` remains the existing E2 LOOP mode.)

## Partial-chunk semantics

Runtime temporarily loads the counter with the number of iterations that fit
the current scheduler budget. Native code therefore exits naturally with the
temporary counter at zero.

After native return, runtime restores:

```text
counter = original_iterations - run_iterations
IP      = loop_header
```

and repairs canonical lazy DEC flags to the real architectural operation:

```text
DEC (remaining + 1) -> remaining
```

`lazy_carry` is deliberately not changed: DEC preserves CF, and Native v2's
existing exit path already captured the correct CF produced by the final body
iteration.

This makes chunk boundaries indistinguishable from stopping the real x86 loop
after the same iteration.

The main measured beneficiary is MDSTRESS Phase 1 (`14CB:0239`), which was
still `chunk=0` in Phase 3K.
