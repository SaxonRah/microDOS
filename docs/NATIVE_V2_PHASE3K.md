# Native v2 Phase 3K — budgeted E2 LOOP chunking

Phase 3J Fix-1 checkpoint:

- DOS2TEST 25/25
- MDSTRESS checksum 0xA298
- Native coverage 16.2%
- Native retirement 3,113,500 guest instructions
- Active 3.442 MIPS
- store-guard=0
- stack-guard=0
- muldiv-guard=0
- runtime fallback=0
- Phase-7 FLAGS/stack loop is live as phase 11

## Why Phase 3K

The runtime previously admitted a native counted region only when the *entire
remaining loop* fit the current md_dos2_system_run instruction budget.

That is unnecessarily conservative for `E2 LOOP` because LOOP changes only
CX/IP; it does not modify x86 FLAGS.

Large already-proven regions therefore spent most of each outer loop in the
threaded interpreter before Native v2 was allowed to execute the tail.

## Chunk execution

For an E2 region whose body does not read CX:

1. Save the real remaining iteration count.
2. Choose `run_iterations = min(remaining, budget / op_count)`.
3. Temporarily load CX with `run_iterations`.
4. Execute the existing native region normally until its temporary CX reaches
   zero.
5. If iterations remain, restore the real remaining CX and set IP back to the
   native loop header.
6. Continue normal scheduling.

Since LOOP itself preserves FLAGS, the flags after the last instruction of the
chunk are exactly the flags that would exist after the same real guest
iteration. No flag repair is required.

## Compile-time proof

`chunkable_loop=1` is emitted only when:

- terminal edge is `E2 LOOP`
- no body operation reads CX
- the terminal operation is the only CX consumer needed by the region

An E2 loop that reads CX in its body remains non-chunkable and retains the old
whole-loop budget behavior.

## Existing safety proofs remain active

Store span checks use the *chunk* iteration count, so a streaming store can be
accepted for one safe chunk and rechecked at the next header.

The Phase-7 PUSHF/POPF stack guard and Phase-5 MUL/DIV proof are unchanged.

## Expected effect

This does not add a new x86 opcode. It should instead raise coverage of already
compiled Phase-5/6/7 loops dramatically by eliminating most budget-only
interpreter time.
