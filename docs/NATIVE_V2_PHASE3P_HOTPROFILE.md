# Native v2 Phase 3P hot-reject profiler

This is a diagnostic build on top of the known-good Phase 3P compiler/runtime.
It does **not** add a new native compiler feature.

Purpose: rank unsupported DOS/COMMAND candidate sites by repeated Native-v2
admission sightings before deciding what the next production compiler feature
should be.

Changes relative to production Phase 3P:

- Native-v2 direct runtime table grows from 32 to 64 slots to reduce diagnostic
  hash collisions.
- Rejected slots encode a saturated sighting count in the existing `reason=`
  string, e.g. `reason=compile/hits=17`. No Pico frontend change is required.
- The first failed compile/store probe counts as sighting 1. A cache hit on the
  same rejected CS:IP/signature increments the count, saturated at 63.
- Compiler lowering, native code generation, runtime guards, retirement and
  guest semantics are otherwise unchanged from Phase 3P.

The bundled PowerShell runner executes DOS2TEST three times and requests
Ctrl+] statistics after each run. It parses all reject-slot reports and prints
the maximum observed sighting count for each CS:IP, sorted hottest first.

This build is for profiling only. Restore `microdos_native_v2_phase3p_fix1.zip`
after collecting the log so the production table returns to 32 slots and the
normal `reason=compile` / `reason=store` strings.
