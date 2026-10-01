# microDOS M20.0 overlay — generic resident loops + full-DOS JIT hybrid

Base: apply this over the working M19.2 tree (which itself was based on repo head
`5af25bf`).  This overlay does not replace the proven default DOS firmware.

## What M20.0 adds

1. **Generic counted-loop resident compiler**
   - No exact `loop.com`/`memloop.com` byte-pattern dependency.
   - Recognises a straight-line setup + supported loop body ending in
     `DEC CX / JNZ` to an earlier instruction.
   - AX/BX/CX/SI live in ARM r0/r1/r6/r7 for the whole region.
   - `[SI]` uses a resident guest-memory base.
   - Intermediate x86 flags are omitted when dead; only the final architectural
     DEC state is committed.
   - Unsafe shapes fall back to the proven M19.2/M19.1 path.

2. **`regionmix` benchmark**
   - Deliberately different from both old resident recognisers.
   - Uses AX/BX/CX/SI, AL memory traffic, XOR/ADD/OR, a native backedge and
     the same high-memory safety proof.
   - No static AOT row on purpose: it is a generic-JIT proof.

3. **Full DOS hybrid firmware: `microdos_pico_jit.uf2`**
   - Static MSDOS.SYS AOT remains first priority.
   - Static DOS2TEST AOT remains first priority.
   - COMMAND.COM and arbitrary segments with no matching static image run in
     the runtime translator until CS changes (INT/far transfer/IRET), at which
     point the system loop immediately gives static kernel AOT first chance.
   - JIT code arena: 24 KiB; block table: 64 slots for the full-DOS image.
   - Existing `microdos_pico.uf2` is unchanged.

## Build

```powershell
cd C:\microDOS
.\md.bat clean
.\md.bat build host
.\md.bat test
.\md.bat build pico
```

Flash the benchmark first:

```text
build-pico\out\microdos_bench.uf2
```

Expected new rows:

```text
regionmix SRAM  step
regionmix SRAM  threaded
regionmix SRAM  cs-run
regionmix SRAM  cache
regionmix SRAM  jit
regionmix PSRAM ...
```

The JIT row should show:

```text
generic-regions=1
generic-entry=1
generic-instr≈294915
fallback=0
```

Then flash the full hybrid:

```text
build-pico\out\microdos_pico_jit.uf2
```

Run the normal DOS flow, including DOS2TEST.  The acceptance bar remains:

```text
passed: 25
failed: 0
ALL TESTS PASSED
```

The key M20 question is whether COMMAND.COM / ordinary app execution remains
correct while spending useful time in the runtime translator between DOS
interrupts.

## Safety/correctness constraints retained

- Static AOT wins over JIT.
- JIT returns on CS changes so compiled kernel AOT is never bypassed.
- AOT holes/invalid chunks still take the conservative interpreter path.
- Generic memory regions require DS=0 and a proven `[SI]` 8000h..FFFFh window.
- Stores use the generic resident path only if pages 8..15 are not executable.
- Entire-region budget is proven before guest state changes.
- Unsupported/dynamic shapes fall back rather than weakening semantics.
