# microDOS Unified Tiered Engine — Design Change Proposal

**Working milestone:** M23 — Unified Execution Router  
**Repository:** `SaxonRah/microDOS`  
**Committed baseline reviewed:** `d2cb08d5ea32a931a95a3de8c40c516fbac58a95` (`M21.1b optimize guest store tracking`)  
**Also incorporates measured local work after that commit:** M21.1c region fast path, MDSTRESS v2, and M22.0 bounded JIT escape.

---

## 1. Purpose

microDOS now has enough performance evidence to stop treating the interpreter, decoded cache, AOT, JIT, and resident-region machinery as competing whole-system engines.

The measurements show a much better architecture:

> **Use each execution mechanism only where it has already demonstrated an advantage, and avoid switching between mechanisms until the expected useful work is large enough to amortize the transition.**

The proposed system is a **Unified Tiered Engine** built around a small execution router.

The router does not attempt to predict exact future MIPS. It makes cheap, sticky decisions using:

- whether valid static AOT exists;
- whether the current code is known to be unstable/self-modifying;
- whether execution is hot enough to justify native compilation;
- whether a translated region has enough native coverage;
- whether previous JIT attempts at the site were profitable;
- whether an existing shared semantic region can handle the workload more efficiently than normal translation.

The intended end state is not:

```text
interpreter -> cache -> JIT -> AOT
```

and not:

```text
always JIT anything not statically compiled
```

It is:

```text
                         current CS:IP
                              |
                              v
                       +--------------+
                       | EXEC ROUTER  |
                       +------+-------+
                              |
         +--------------------+-----------------------+
         |                    |                       |
         v                    v                       v
    valid static AOT    proven hot native       threaded interpreter
         |              region / region JIT            |
         |                    |                       |
         +-------------+------+-----------------------+
                       |
                       v
                  MdRuntime state
```

The decoded block cache remains useful as a development/reference implementation and as a source of region-recognition ideas, but should not remain on the normal shipping execution ladder unless later measurements reverse the current evidence.

---

# 2. Why change the architecture now?

The existing engine grew correctly and incrementally:

1. canonical interpreter;
2. decoded block cache;
3. static `dosrecomp` AOT;
4. runtime Thumb-2 JIT;
5. resident native loops;
6. generic counted regions;
7. CFG regions;
8. shared semantic region helpers;
9. store-path optimization;
10. bounded JIT fallback escape.

Each experiment answered an important question.

The combined answer is now clear:

- **AOT is excellent for code known ahead of time.**
- **The threaded interpreter is a very strong universal fallback.**
- **The runtime JIT is spectacular when it can remain native for long enough.**
- **The runtime JIT is terrible when it repeatedly enters/exits on unsupported or control-heavy code.**
- **Shared semantic regions can beat ordinary translation by very large margins.**
- **The decoded cache is useful for research but has repeatedly lost to the threaded interpreter on general DOS execution.**
- **Transition frequency itself is a first-order performance cost.**

Therefore the next major performance gain is likely to come from **execution policy**, not from adding another isolated opcode optimization.

---

# 3. Measured evidence

This section separates measured behavior from the proposed design.

## 3.1 Real DOS kernel

M21.1b moved the DOS2TEST interval from roughly:

```text
3.49 MIPS
```

to approximately:

```text
3.91 MIPS
```

after optimizing guest store tracking.

The improvement came from reducing bookkeeping around ordinary DOS stack/data stores, not from adding x86 opcode coverage.

The DOS2TEST interval was already overwhelmingly static AOT:

```text
~95% static AOT
~5% interpreted / BIOS / holes
```

This demonstrates that **static AOT should remain the highest-priority execution tier for known DOS code**.

---

## 3.2 Synthetic resident/JIT wins

The runtime-native machinery has proven that it can be extremely fast when execution remains inside a useful native region.

Representative historical measurements at 300 MHz include approximately:

```text
resident DEC/JNZ JIT loop     ~190-200 MIPS
SRAM memloop JIT              ~200 MIPS
branchmix JIT                 ~105-108 MIPS
regionmix JIT                  ~83-84 MIPS
```

These results are important because they prove:

> The runtime translator itself is not fundamentally slow.

The performance collapse occurs when the translated execution unit is too small, has too many unsupported instructions, or returns through the C dispatcher too frequently.

---

## 3.3 Shared semantic regions

The M21 checksum experiment made AOT and decoded-cache execution converge when both used the same region implementation.

M21.1c then optimized the forward contiguous checksum region dramatically.

Representative M21.1c checksum results:

```text
SRAM cache/shared region      ~264 MIPS
SRAM AOT/shared region        ~218 MIPS

PSRAM cache/shared region      ~31 MIPS
PSRAM AOT/shared region        ~30 MIPS
```

This demonstrates two things:

1. **Whole-region semantic implementations are one of the most valuable optimization mechanisms in microDOS.**
2. **Once execution overhead is removed, guest memory placement becomes the limiting factor for memory-heavy workloads.**

The region mechanism should therefore become a first-class shared backend used by both AOT and runtime-native execution.

---

## 3.4 CALL/RET weakness

Synthetic `callmix` already showed the problem:

```text
threaded interpreter     ~3.8-3.9 MIPS
warm JIT                 ~1.4-1.5 MIPS
```

Warm JIT testing proved compile cost was not responsible.

MDSTRESS then reproduced the problem in a more realistic DOS application.

Before M22.0:

```text
MDSTRESS CALL/RET
interpreter              4.020 MIPS
runtime JIT              0.603 MIPS
```

The JIT produced millions of:

- native entries;
- native returns;
- zero-progress fallbacks;
- dispatcher transitions.

This is direct evidence that **switch cost is part of the bottleneck**.

---

## 3.5 M22.0 proves that transition frequency matters

M22.0 changed zero-progress handling from:

```text
native -> zero progress -> interpret 1 -> probe JIT again
```

to roughly:

```text
native -> zero progress -> interpret up to 16 -> probe JIT again
```

The result was a large recovery.

Representative MDSTRESS v2 comparison:

| Phase | Interpreter | Pre-M22 JIT | M22.0 |
|---|---:|---:|---:|
| ALU/flags | 2.257 | 0.775 | 1.207 |
| Memory/store | 3.037 | 1.736 | 1.649 |
| REP strings | 2.224 | 2.186 | 2.227 |
| CALL/RET | 4.020 | 0.603 | 1.365 |
| MUL/DIV | 1.970 | 0.749 | 1.234 |
| Branch/LFSR | 2.532 | 0.624 | 1.270 |
| Rare opcodes | 2.220 | 0.511 | 1.278 |
| DOS services | 3.925 | 3.821 | 3.944 |
| Self-modify | 3.854 | 2.109 | 2.890 |

The fixed escape improved most bad cases substantially, but also exposed a new problem:

```text
interpret 16
probe JIT
fail
interpret 16
probe JIT
fail
...
```

For several phases, the measured ratio of interpreted fallback instructions to zero-progress exits was almost exactly 16.

That means **a fixed fallback interval is still an unnecessary form of tier swapping**.

The router should instead make a persistent classification decision.

---

# 4. Core design principle

The main design rule is:

> **Do not ask “which engine is globally fastest?” Ask “which engine is appropriate for this code region, and how long can we stay there before another transition is justified?”**

Tier transitions should be comparatively rare.

A useful target is to think in terms of:

```text
guest instructions retired per tier transition
```

rather than only:

```text
guest instructions per second
```

If one mechanism runs 20% faster internally but causes a tier transition every 2-5 guest instructions, it can lose badly to a slower engine that stays resident for hundreds or thousands of instructions.

---

# 5. Proposed production hierarchy

The normal release path should eventually have four mechanisms:

```text
1. static AOT
2. shared native semantic regions
3. selective hot-region JIT
4. threaded interpreter
```

The decoded cache becomes a development/reference engine rather than a normal production tier.

Priority:

```text
valid static AOT
    >
already-proven native region
    >
threaded interpreter
    >
promotion to JIT only after evidence
```

A newly loaded unknown program should therefore begin in the interpreter, **not** in the JIT.

---

# 6. Static AOT policy

Static AOT should remain the highest-priority path.

Current `md_dos2_system_run()` already does the most important part correctly:

```text
if valid AOT exists at CS:IP:
    enter AOT before considering JIT/interpreter
```

This should remain unchanged in principle.

## 6.1 Why AOT wins

AOT avoids:

- runtime translation cost;
- hotness tracking;
- JIT code arena pressure;
- runtime lookup tables;
- repeated block compilation;
- most JIT ownership transitions.

It also enables whole-program/static optimization opportunities that a small runtime JIT does not have.

## 6.2 AOT should be used for

- `MSDOS.SYS`;
- known bundled utilities;
- optional precompiled application packs;
- frequently used fixed COM files where firmware size is acceptable;
- generated helper-heavy code where static knowledge permits better regions.

## 6.3 AOT should not be mandatory

Unknown DOS programs must remain fully functional through the interpreter and optional JIT.

The design should never make microDOS depend on a precompiled software catalog.

---

# 7. Threaded interpreter policy

The threaded interpreter becomes the **default engine for unknown code**.

This is a conceptual inversion of the current M20/M21 JIT-enabled path.

Current behavior is essentially:

```text
if no AOT:
    stay in JIT until CS changes
```

The new behavior should be:

```text
if no AOT:
    interpret
    collect cheap heat information
    promote only when a native region has a high probability of winning
```

## 7.1 Why the interpreter is the correct default

It is:

- complete;
- semantically canonical;
- small;
- predictable;
- resistant to pathological transition overhead;
- already capable of roughly 2-4 MIPS on many realistic DOS workloads.

For unsupported-heavy MDSTRESS phases it currently beats the JIT by large margins.

Therefore native translation must **earn promotion**.

---

# 8. Execution router

Introduce a small execution policy layer.

Suggested files:

```text
include/microdos/exec_router.h
src/runtime/exec_router.c
```

Do not put large policy tables into `MdRuntime`; keep the architectural runtime relatively clean.

A router can live in `MdDos2System`, because the DOS system loop already owns the choice between:

- AOT;
- JIT;
- cache;
- interpreter.

---

# 9. Router site table

The router needs a tiny sticky site table.

A possible initial structure:

```c
typedef enum MdExecMode {
    MD_EXEC_INTERP = 0,
    MD_EXEC_JIT_CANDIDATE,
    MD_EXEC_JIT_REGION,
    MD_EXEC_INTERP_COOLDOWN,
    MD_EXEC_UNSTABLE
} MdExecMode;

typedef struct MdExecSite {
    uint16_t cs;
    uint16_t ip;

    uint8_t heat;
    uint8_t penalty;
    uint8_t mode;
    uint8_t cooldown;
} MdExecSite;
```

This can be exactly 8 bytes.

With 64 entries:

```text
64 * 8 = 512 bytes
```

With 128 entries:

```text
128 * 8 = 1024 bytes
```

Start with **64 entries**.

The current JIT already owns a 64-entry hotness table:

```c
MdJitHotness hotness[MD_JIT_HOTNESS_SLOTS];
```

The preferred implementation is to **replace or repurpose this concept**, not simply add another table.

The router should not increase fixed SRAM by more than about 512-1024 bytes.

---

# 10. Router lookup

Use a cheap direct-mapped or two-way hashed table.

For the first implementation, direct mapped is enough:

```c
index = hash(cs, ip) & (MD_EXEC_SITE_SLOTS - 1);
```

Collision behavior:

```text
same key -> use existing record
different key -> replace record with fresh INTERP state
```

Do not implement LRU initially.

The router is advisory; losing a classification entry only causes a site to warm up again. It cannot affect correctness.

---

# 11. Avoid per-instruction routing

This is extremely important.

Do **not** call the router after every interpreted instruction.

The whole purpose is to reduce transitions.

Suggested interpreter quantum:

```text
MD_EXEC_INTERP_QUANTUM = 256 guest instructions
```

or possibly:

```text
128
```

for early tuning.

The DOS system loop can run:

```c
md_interp_run_until_cs_change(rt, quantum);
```

and inspect the resulting CS:IP only at the end of the quantum.

This makes sampling cost roughly:

```text
1 router decision / 256 guest instructions
```

instead of:

```text
1 router decision / guest instruction
```

That should be cheap enough to leave enabled in the lean JIT firmware.

---

# 12. Heat detection

Initially use coarse sampling rather than instrumenting every branch.

For each interpreter quantum that ends without a stop:

```text
site = router_lookup(current CS:IP)
site.heat++
```

Use saturating counters.

Suggested initial promotion threshold:

```text
MD_EXEC_HOT_THRESHOLD = 4
```

At 256-instruction quanta, this means a repeatedly sampled site must survive roughly a thousand guest instructions before we even consider compiling it.

This is intentional.

The JIT should not spend SRAM or compile time on short-lived setup code.

---

# 13. Candidate analysis before compilation

Do not immediately compile when a site becomes hot.

Add a JIT probe/analyze API that decodes a candidate using the existing compile scratch but does not emit Thumb yet.

Proposed API:

```c
typedef struct MdJitProbe {
    uint8_t decoded_ops;
    uint8_t direct_ops;
    uint8_t fallback_ops;
    uint8_t control_ops;

    uint8_t has_backedge;
    uint8_t has_call;
    uint8_t has_return;
    uint8_t resident_kind;

    uint8_t pages;
    uint8_t unstable;
} MdJitProbe;

bool md_jit_probe(MdJit *jit,
                  MdRuntime *runtime,
                  uint16_t cs,
                  uint16_t ip,
                  MdJitProbe *probe);
```

This API should use the existing decoder and `compile_ops`.

No persistent decoded IR is required.

---

# 14. Promotion rules

The first version should be conservative.

### Always promote

If the probe recognizes a known profitable region:

```text
DEC/JNZ resident loop
LODSW/ADD/LOOP region
memloop specialization
generic counted resident region
bounded CFG region with proven local edges
```

These have already demonstrated large wins.

### Consider promotion

For ordinary direct Thumb blocks:

```text
direct coverage >= 80%
AND
direct prefix >= 8 guest ops
AND
first op is native
AND
page is stable
```

These numbers are initial tuning values.

### Reject / cooldown

Reject immediately if:

```text
first op unsupported
fallback density high
region dominated by CALL/RET before native chaining exists
page repeatedly invalidated
predicted useful native run is very short
```

Rejected sites become:

```text
MD_EXEC_INTERP_COOLDOWN
```

rather than being probed again every few instructions.

---

# 15. Sticky cooldown

A site that fails JIT admission should remain interpreted for a meaningful time.

Suggested initial cooldown:

```text
4096 guest instructions
```

The router need not count individual instructions.

At a 256-instruction interpreter quantum:

```text
cooldown = 16 quanta
```

Use an 8-bit quantum counter.

On repeated failed promotion:

```text
first rejection      16 quanta   (~4096 instructions)
second rejection     64 quanta   (~16384 instructions)
third+ rejection    255 quanta   (~65280 instructions)
```

This is effectively exponential backoff with almost no code/state.

---

# 16. JIT success feedback

Promotion should not be permanent merely because compilation succeeded.

The router needs feedback after a JIT run.

Measure:

```text
guest instructions retired
native instructions retired
fallback instructions
zero-progress exits
native entries
native returns
invalidations
```

A simple success test:

```text
native guest work >= 64 instructions
AND
fallback/native ratio acceptable
AND
no invalidation
```

A very strong region:

```text
native >= 256 instructions
```

should become sticky immediately.

A weak region:

```text
native < 16
```

or:

```text
zero progress
```

should receive a penalty.

---

# 17. Demotion

Suggested initial logic:

```c
if (zero_progress)
    penalty += 2;
else if (native_retired < 16)
    penalty += 1;
else if (native_retired >= 64)
    penalty = penalty > 0 ? penalty - 1 : 0;

if (penalty >= 3)
    mode = MD_EXEC_INTERP_COOLDOWN;
```

The exact constants should be tuned with MDSTRESS.

The important principle is:

> A site that repeatedly proves unprofitable should stop paying JIT overhead.

---

# 18. Do not continuously measure wall-clock time

Do not put timers in the hot router path.

Timing each region would itself add overhead and would make decisions noisy.

Use structural and retirement metrics:

```text
native guest instructions
fallback guest instructions
number of transitions
zero-progress events
invalidations
```

These are cheap and deterministic.

Wall-clock time belongs in profiling builds and MDSTRESS.

---

# 19. Tier switching policy

Not all switching is bad.

Necessary transitions include:

- application -> DOS kernel through INT;
- DOS kernel -> application through return;
- valid AOT block -> AOT hole;
- page invalidation;
- CS change;
- stop/fault.

The problematic transitions are those with too little useful guest work between them.

Therefore add a router metric:

```text
guest instructions per execution-tier entry
```

and optionally:

```text
tier transitions per 1000 guest instructions
```

A healthy JIT region should retire dozens, hundreds, or thousands of guest instructions per entry.

A region retiring 0-3 instructions per entry is not a useful JIT region.

---

# 20. Shared semantic regions become a first-class backend

`src/runtime/region.c` should become increasingly important.

The current exact regions already demonstrate the design:

```text
DEC r16 / JNZ
LODSW / ADD DX,AX / LOOP
```

The long-term architecture should be:

```text
                    semantic region
                    /             \
            static recognizer    runtime recognizer
                  |                    |
                  v                    v
              dosrecomp             router/JIT
                  \                    /
                   \                  /
                    shared execution
```

It is not necessary to unify all recognition code immediately.

The key requirement is:

> Once a region shape is recognized, AOT and runtime execution should share one semantic implementation or one clearly equivalent lowering.

---

# 21. Future region candidates

Good candidates are operations where an entire guest sequence can be implemented more efficiently than individual translated instructions.

Examples:

```text
REP MOVS/STOS/CMPS/SCAS
simple memcpy/memset-style loops
counted register loops
checksum/scanning loops
small reduction loops
small induction-variable loops
tight forward CFGs
```

Do not add a region just because it can be recognized.

Require an MDSTRESS or real-DOS workload demonstrating value.

---

# 22. REP/string strategy

MDSTRESS REP-string behavior is already near parity across engines.

Therefore REP strings should not be an immediate JIT expansion priority.

Longer term, however, they are excellent candidates for shared semantic/native kernels because a single guest instruction can perform many memory operations.

Optimization should focus on:

```text
bulk guest memory work per host call
```

not on turning the REP instruction itself into another tiny translated block.

---

# 23. CALL/RET policy

Until native chaining exists, CALL/RET-dense regions should generally remain interpreted.

Current evidence strongly supports this.

The router can detect call/return density during the JIT probe.

Initial rule:

```text
if probe contains CALL/RET
and no resident/local native call chain is available:
    reject ordinary JIT promotion
```

This deliberately leaves performance on the table until M24/M25 native chaining exists, but it prevents the much worse current behavior.

---

# 24. Future native CALL/RET chaining

Once the router is stable, CALL/RET should receive a dedicated native design.

Desired path:

```text
translated caller
    |
    v
translated callee
    |
    v
validated translated return target
```

instead of:

```text
translated caller
    |
    v
C dispatcher
    |
    v
lookup
    |
    v
translated callee
    |
    v
C dispatcher
...
```

Possible mechanisms:

- direct known-target call chaining;
- tiny return-address -> native-block cache;
- native shadow return cache while still preserving guest stack semantics;
- validation using existing page-generation/invalidation state.

Do not implement this in the first router milestone.

---

# 25. Self-modifying code policy

MDSTRESS phase 9 proves that continuously recompiling unstable code is not ideal.

Introduce page/site instability scoring.

Possible rule:

```text
if translated page invalidates >= 4 times in a short routing window:
    mark unstable
    interpret for long cooldown
```

The code remains fully functional because interpreter execution is canonical.

After sufficient interpreted execution without new writes:

```text
clear/reduce instability score
allow promotion again
```

No permanent blacklist is required.

---

# 26. Decoded block cache policy

Do not delete the decoded-cache code immediately.

Keep:

```text
microdos_pico_region.uf2
```

as a development image.

Keep cache code for:

- correctness comparisons;
- region experimentation;
- decoding experiments;
- research measurements.

But remove it from the intended release execution ladder.

Current evidence repeatedly shows:

```text
threaded interpreter > generic decoded-cache execution
```

on general DOS/control-heavy code.

This also means the final production firmware may eventually reclaim the cache's fixed state and code.

---

# 27. Existing code that should remain canonical

Do not duplicate semantics in the router.

The canonical semantics remain:

```text
x86 interpreter helpers
MdRuntime
tracked memory stores
AOT guards
region helpers
```

The router chooses execution mechanisms.

It does not implement x86 behavior.

---

# 28. Proposed `MdExecRouter`

A possible first interface:

```c
#ifndef MICRODOS_EXEC_ROUTER_H
#define MICRODOS_EXEC_ROUTER_H

#include <stdint.h>

#ifndef MD_EXEC_SITE_SLOTS
#define MD_EXEC_SITE_SLOTS 64u
#endif

#ifndef MD_EXEC_INTERP_QUANTUM
#define MD_EXEC_INTERP_QUANTUM 256u
#endif

typedef enum MdExecMode {
    MD_EXEC_INTERP = 0,
    MD_EXEC_JIT_CANDIDATE,
    MD_EXEC_JIT_REGION,
    MD_EXEC_INTERP_COOLDOWN,
    MD_EXEC_UNSTABLE
} MdExecMode;

typedef struct MdExecSite {
    uint16_t cs;
    uint16_t ip;
    uint8_t heat;
    uint8_t penalty;
    uint8_t mode;
    uint8_t cooldown;
} MdExecSite;

typedef struct MdExecRouter {
    MdExecSite site[MD_EXEC_SITE_SLOTS];

    uint64_t interp_instructions;
    uint64_t aot_instructions;
    uint64_t jit_instructions;

    uint32_t promotions;
    uint32_t rejections;
    uint32_t demotions;
    uint32_t unstable_demotions;
    uint32_t tier_entries;
} MdExecRouter;

void md_exec_router_init(MdExecRouter *router);

#endif
```

The 64-bit counters should be behind a profile macro in lean builds.

---

# 29. Lean/profile separation

Follow the existing M21 pattern.

Suggested:

```c
#ifndef MD_EXEC_PROFILE
#define MD_EXEC_PROFILE 1
#endif
```

Profile firmware:

```text
full counters
promotion/rejection reasons
site table reporting
tier-transition statistics
```

Lean firmware:

```text
routing state only
no high-frequency 64-bit statistics
```

This keeps observability without paying for it in the release path.

---

# 30. Integrating into `MdDos2System`

Current `md_dos2_system_run()` has the right centralized location for routing.

The current non-cache path is approximately:

```text
AOT?
  yes -> enter AOT

BIOS?
  yes -> threaded interpreter

JIT enabled?
  yes -> keep JIT running until CS change

otherwise:
  interpreter
```

Replace the arbitrary-program part with:

```text
AOT?
  yes -> AOT

BIOS?
  yes -> interpreter

router says proven JIT region?
  yes -> JIT

otherwise:
  interpreter quantum
  update heat
  maybe analyze/promote
```

---

# 31. Proposed system-loop pseudocode

```c
while (rt->stop_reason == MD_STOP_NONE) {
    left = budget_remaining();

    /* 1. Static AOT always gets first refusal. */
    prog = md_aot_here(sys);

    if (prog != NULL) {
        run_aot(prog);
        continue;
    }

    /* 2. BIOS remains canonical interpreter. */
    if (rt->cpu.cs == sys->boot.bios_segment) {
        run_interpreter_until_cs_change(left);
        continue;
    }

#if MICRODOS_ENABLE_JIT
    if (sys->jit != NULL) {
        MdExecSite *site =
            md_exec_router_lookup(&sys->router, rt->cpu.cs, rt->cpu.ip);

        if (site->mode == MD_EXEC_JIT_REGION) {
            before = rt->instructions;
            before_native = jit->direct_instructions;
            before_fallback = jit->fallback_instructions;

            st = md_jit_run_region(...);

            native = jit->direct_instructions - before_native;
            fallback = jit->fallback_instructions - before_fallback;

            md_exec_router_jit_feedback(site,
                                        rt->instructions - before,
                                        native,
                                        fallback,
                                        ...);

            if (st != MD_STOP_NONE)
                ...
            continue;
        }

        q = min(left, MD_EXEC_INTERP_QUANTUM);

        before = rt->instructions;
        st = md_interp_run_until_cs_change(rt, q);
        retired = rt->instructions - before;

        md_exec_router_interp_feedback(...);

        if (md_exec_router_should_probe(site)) {
            MdJitProbe probe;

            if (md_jit_probe(..., &probe) &&
                md_exec_router_accept_probe(site, &probe)) {
                if (md_jit_compile_candidate(...))
                    site->mode = MD_EXEC_JIT_REGION;
            } else {
                md_exec_router_reject(site);
            }
        }

        ...
        continue;
    }
#endif

    md_interp_run_until_cs_change(rt, left);
}
```

This pseudocode is conceptual; preserve the existing exact stop/budget behavior.

---

# 32. JIT API changes

The current JIT API exposes whole-run functions:

```c
md_jit_run()
md_jit_run_until_cs_change()
```

For the router architecture, eventually add narrower primitives.

Suggested:

```c
bool md_jit_probe(...);

bool md_jit_prepare_region(...);

MdStopReason md_jit_run_region(MdJit *jit,
                               MdRuntime *runtime,
                               uint64_t budget,
                               MdJitRunResult *result);
```

Possible result:

```c
typedef struct MdJitRunResult {
    uint32_t retired;
    uint32_t native;
    uint32_t fallback;
    uint16_t entries;
    uint16_t zero_exits;
    uint8_t invalidated;
    uint8_t cs_changed;
} MdJitRunResult;
```

Do not put this into the hot native calling convention.

This is a C-side summary returned after a routing episode.

---

# 33. Do not expand `MdJitBlock` casually

M21 deliberately reduced `MdJitBlock` to 64 bytes.

Preserve this win.

Do not put router policy in every JIT block.

Router state belongs in a separate tiny site table.

A block describes compiled code.

A site record describes policy.

Those are different responsibilities.

---

# 34. JIT arena policy

The current full-DOS JIT uses a 24 KiB executable SRAM arena.

A selective region JIT may need much less code because it will no longer compile large amounts of short-lived or low-value code.

After the router works, test:

```text
24 KiB
16 KiB
12 KiB
8 KiB
```

against:

- MDSTRESS;
- full DOS2TEST;
- synthetic loop/memloop/branchmix/regionmix;
- real applications.

Do not shrink the arena before the router exists.

---

# 35. Block-slot policy

Current:

```text
MD_JIT_BLOCK_SLOTS = 128
MdJitBlock = 64 bytes
```

Fixed block table cost:

```text
128 * 64 = 8192 bytes
```

A region-selective JIT may not require 128 slots.

After the router:

```text
128 -> 64
```

would recover approximately:

```text
4 KiB
```

Test 64 before considering 32.

Again: do this after behavior is stable.

---

# 36. Potential SRAM recovery

Current local measurements are approximately:

```text
AOT-only firmware              ~400 KiB
lean AOT + JIT                 ~463 KiB
profile AOT + JIT              ~465 KiB
```

The current JIT therefore costs on the order of 60+ KiB over the AOT-only firmware.

Potential later savings:

```text
block slots 128 -> 64              ~4 KiB
arena 24 KiB -> 16 KiB             ~8 KiB
remove production decoded cache    depends on target
replace old hotness with router    ~neutral
cold compiler/emitter placement    map-dependent
remove unprofitable generic paths  code-dependent
```

A realistic long-term goal is to recover at least 10-15 KiB from the release JIT without losing the region wins.

Do not make this an M23.0 acceptance requirement.

---

# 37. Hot/cold placement

The JIT source currently mixes:

- hot execution helpers;
- compiler;
- decoder;
- Thumb emitter;
- profiling;
- rare error paths.

Once routing is selective, revisit linker placement.

Keep in SRAM:

```text
interpreter hot dispatch
AOT hot helpers
router hot lookup
JIT region entry
store fast paths
resident semantic helpers
emitted code arena
```

Consider placing colder code in flash/XIP:

```text
JIT compiler
Thumb emitter
diagnostics
profiling formatter
rare fault paths
analysis/probe code
```

Use linker maps before making placement changes.

---

# 38. Production firmware targets

Keep the existing research matrix, but introduce one canonical release target.

Suggested meaning:

```text
microdos_pico.uf2
    AOT kernel
    interpreter fallback
    no runtime JIT

microdos_pico_jit_profile.uf2
    full unified router
    full JIT/router profiling

microdos_pico_jit.uf2
    lean unified router
    selective runtime region JIT

microdos_pico_region.uf2
    research/reference decoded-cache path

microdos_pico_nokernel.uf2
    canonical interpreter research/reference
```

Eventually rename only if desired; functionality matters more than names.

---

# 39. M23 implementation stages

Do not rewrite everything at once.

## M23.0 — router skeleton, no optimization

Goal:

```text
introduce MdExecRouter
preserve current behavior
add counters
```

Tasks:

- new `exec_router.h/.c`;
- add router storage to `MdDos2System`;
- initialize/reset router;
- expose stats;
- no change to tier selection yet.

Acceptance:

```text
all host tests pass
DOS2TEST 25/25
no measurable release regression
router fixed state <= 1 KiB
```

---

## M23.1 — interpreter-first unknown code

Change arbitrary application execution from:

```text
JIT until CS change
```

to:

```text
interpreter quantum
```

Collect coarse site heat.

Do **not** compile anything yet.

This establishes the new baseline and verifies router overhead.

Acceptance:

```text
MDSTRESS roughly matches interpreter baseline
DOS2TEST unchanged
router overhead < ~5%
```

Ideally router overhead is closer to 1-2%.

---

## M23.2 — promote proven existing JIT regions

Enable promotion only for region shapes already known to win:

```text
resident DEC/JNZ
memloop specialization
LODS/ALU/LOOP
generic counted regions
bounded CFG regions
```

Do not enable ordinary short direct blocks yet.

This should recover the spectacular synthetic region numbers while leaving unsupported-heavy MDSTRESS in the interpreter.

Acceptance:

```text
loop / memloop / branchmix / regionmix recover near previous JIT performance
MDSTRESS unsupported phases remain near interpreter baseline
```

This milestone is the central proof of the unified architecture.

---

## M23.3 — selective direct-block promotion

Add `md_jit_probe()`.

Allow direct blocks only when predicted native coverage is high enough.

Tune with MDSTRESS.

Reject:

```text
fallback-heavy
CALL/RET-heavy
unstable
tiny useful prefixes
```

---

## M23.4 — instability / self-modifying policy

Add invalidation feedback.

Repeated invalidation:

```text
JIT_REGION -> UNSTABLE -> interpreter cooldown
```

Use MDSTRESS phase 9 as the primary fixture.

---

## M23.5 — release-size pass

Only after the router is proven:

- 64 JIT slots;
- smaller arena experiments;
- remove cache from release;
- remove obsolete M22 fixed escape if router supersedes it;
- hot/cold code placement.

---

# 40. Later milestone — native CALL/RET

After M23 stabilizes, implement native CALL/RET chaining.

Do not force CALL/RET through a generic JIT before then.

Primary metric:

```text
MDSTRESS phase 4
```

Current targets:

```text
interpreter baseline ~4.0 MIPS
old JIT             ~0.6 MIPS
M22.0               ~1.36 MIPS
```

First success criterion:

```text
native CALL/RET path > interpreter baseline
```

If it does not beat the interpreter, the router should keep that code interpreted.

---

# 41. Correctness rules

The router must never alter architectural correctness.

Tier choice must be transparent.

At every transition:

```text
same MdRuntime
same exact CS:IP
same guest registers
same lazy/architectural FLAGS
same memory
same code invalidation state
same stop behavior
same instruction accounting
```

The router may affect only:

```text
how the next guest instructions are executed
```

not what they mean.

---

# 42. Budget semantics

Instruction budgets are part of correctness.

Every tier must consume no more than the remaining budget.

Interpreter quantum:

```c
q = min(left, MD_EXEC_INTERP_QUANTUM);
```

JIT region:

```text
must obey existing exact-budget contract
```

AOT:

```text
keep existing MD_DOS2_AOT_CHUNK behavior
```

Shared semantic regions:

```text
continue admitting only exact whole iterations that fit
```

Never trade budget correctness for routing speed.

---

# 43. Code invalidation rules

Do not invent a second invalidation system.

Reuse existing:

```text
code_page_generation
code_page_executable
AOT live-byte maps
AOT guard invalidation
JIT block validity
```

The router may store an instability score, but should rely on the existing code mechanisms for correctness.

Router instability is a **performance hint**, not a correctness guard.

---

# 44. Suggested new profiling output

Extend Pico stats with:

```text
[router] interp=...
[router] aot=...
[router] jit=...
[router] promotions=...
[router] reject=...
[router] demote=...
[router] unstable=...
[router] entries=...
[router] switches/1k=...
[router] avg-jit-run=...
```

Profile firmware only.

Also print site candidates:

```text
[router] 14CB:03D3 mode=interp heat=7 penalty=3 cooldown=64
```

Limit to a small heavy-hitter set.

Do not dump all 64 sites every Ctrl+].

---

# 45. MDSTRESS role

MDSTRESS should become the permanent policy benchmark.

Use all nine phases.

Each phase isolates a different routing failure mode:

```text
1 ALU/flags        native opcode coverage
2 memory/store     ModRM memory + tracking
3 REP strings      long semantic operations
4 CALL/RET         transition/control churn
5 MUL/DIV          expensive unsupported operations
6 branch/LFSR      mixed branch coverage
7 rare opcodes     deliberate unsupported-density
8 DOS services     healthy AOT control
9 self-modify      invalidation policy
```

The router is successful when it chooses different tiers for these phases automatically.

That is the point of the design.

---

# 46. Primary M23 performance acceptance criteria

The unified engine should satisfy both “do no harm” and “keep the wins.”

## Unsupported-heavy code

For MDSTRESS phases where interpreter is currently best:

```text
JIT-enabled unified firmware should be within 10% of interpreter
```

Stretch goal:

```text
within 5%
```

until a native implementation actually beats it.

---

## Existing JIT winners

Do not lose the strong native-region results.

Approximate minimum targets:

```text
loop SRAM          >= 180 MIPS
memloop SRAM       >= 190 MIPS
branchmix SRAM     >= 95 MIPS
regionmix SRAM     >= 75 MIPS
```

These are intentionally below historical peaks to allow some router overhead.

---

## Real DOS

Keep:

```text
DOS2TEST 25/25
~3.9 MIPS or better
```

No routing optimization should make the known AOT kernel slower.

---

## Size

M23.0-M23.4:

```text
router fixed SRAM <= ~1 KiB
```

M23.5:

```text
recover >= 10 KiB from lean JIT target if measurements permit
```

---

# 47. Test plan

Add:

```text
tests/test_exec_router.c
tests/test_exec_router_diff.c
```

## `test_exec_router.c`

Test pure policy behavior:

```text
fresh site -> INTERP
heat saturation
promotion threshold
rejection cooldown
repeated rejection exponential cooldown
successful native run lowers penalty
zero-progress increases penalty
demotion threshold
unstable-page behavior
hash collisions
reset behavior
```

No x86 execution needed.

---

## `test_exec_router_diff.c`

Run the same guest fixture through:

```text
canonical interpreter
unified router
```

with very small budgets:

```text
1
2
3
7
16
31
64
```

Compare after every slice:

```text
registers
CS/IP
FLAGS
memory
stop reason
instructions
```

Include:

```text
loops
CALL/RET
INT/IRET
self-modifying code
unsupported opcodes
known shared regions
```

---

# 48. Existing tests remain mandatory

Keep all current gates:

```text
runtime
lockstep
store_tracking
jit
jit_diff
jit_m203
region
region_aot
DOS2 e2e variants
QEMU Thumb validation where applicable
Pico hardware
```

The router should add tests, not replace old ones.

---

# 49. Development workflow

Recommended implementation order:

```powershell
cd C:\microDOS

.\md.bat clean
.\md.bat build host
.\md.bat test
```

Only after host green:

```powershell
.\md.bat build pico
```

Then run:

```text
DOS2TEST
MDSTRESS phase matrix
microdos_bench
```

Keep benchmark CSVs by milestone:

```text
mdstress-m22.csv
mdstress-m23.1.csv
mdstress-m23.2.csv
...
```

Do not compare only aggregate MIPS.

Compare per phase and transition counters.

---

# 50. What not to do

Avoid these tempting approaches.

## Do not JIT every unknown block

Already disproven by MDSTRESS.

## Do not keep tuning fixed escape length

M22.0 proved the concept, but:

```text
16 -> probe -> 16 -> probe
```

is still a repeated-switch policy.

A sticky router is better.

## Do not add every x86 opcode to the JIT

That increases code size without proving a useful native residency benefit.

Implement an opcode natively when:

```text
the router is frequently rejecting otherwise-good hot regions because of it
```

## Do not optimize decoded-cache execution merely because it exists

Use it when measurements show a specific region benefit.

Do not assume it belongs in the final release architecture.

## Do not duplicate x86 semantics in router code

Router code is policy only.

## Do not make profiling mandatory in release firmware

Keep `*_PROFILE` separation.

---

# 51. How opcode work should be prioritized after the router

The router will make future opcode selection much more rational.

Instead of asking:

```text
which opcodes are missing?
```

ask:

```text
which unsupported opcode prevents an otherwise-profitable hot region from
staying native?
```

Example:

If a candidate decodes:

```text
native
native
native
ADC          <- unsupported
native
native
backedge
```

then ADC has high leverage.

But if a region is:

```text
PUSHF
LAHF
SAHF
POPF
XCHG
DIV
RET
```

and is already faster interpreted, implementing one instruction gains little.

This keeps the JIT small.

---

# 52. Proposed implementation checklist

A practical first coding session:

### Step 1

Add:

```text
include/microdos/exec_router.h
src/runtime/exec_router.c
tests/test_exec_router.c
```

Implement only table lookup, heat, penalty, cooldown.

### Step 2

Add `MdExecRouter router;` to `MdDos2System`.

No behavior change.

Build/test.

### Step 3

Add router counters to Pico profile output.

No behavior change.

Build/test/hardware.

### Step 4

Change only arbitrary unknown application execution to 256-instruction interpreter quanta.

No JIT promotion yet.

Run MDSTRESS.

This gives the clean router/interpreter baseline.

### Step 5

Add a runtime switch:

```c
MD_EXEC_ENABLE_PROMOTION
```

default off.

### Step 6

Expose a probe for **existing resident-region detection only**.

Enable promotion for proven regions.

Run benchmark.

### Step 7

Only after those results, add ordinary direct-block probe/admission.

---

# 53. Suggested initial constants

These are starting points, not sacred values.

```c
#define MD_EXEC_SITE_SLOTS          64u
#define MD_EXEC_INTERP_QUANTUM     256u
#define MD_EXEC_HOT_THRESHOLD        4u

#define MD_EXEC_MIN_DIRECT_OPS       8u
#define MD_EXEC_MIN_COVERAGE_PCT    80u

#define MD_EXEC_GOOD_NATIVE_RUN     64u
#define MD_EXEC_STRONG_NATIVE_RUN  256u

#define MD_EXEC_DEMOTE_PENALTY       3u

#define MD_EXEC_COOLDOWN_1          16u
#define MD_EXEC_COOLDOWN_2          64u
#define MD_EXEC_COOLDOWN_3         255u

#define MD_EXEC_UNSTABLE_WRITES      4u
```

Treat cooldown units as interpreter quanta.

---

# 54. Expected router behavior on MDSTRESS

A successful implementation will probably classify roughly like this:

| Phase | Expected preferred execution |
|---|---|
| 1 ALU/flags | interpreter initially; selective JIT only after better coverage |
| 2 memory/store | interpreter until general ModRM memory lowering is worthwhile |
| 3 REP strings | interpreter/shared semantic string helper |
| 4 CALL/RET | interpreter until native call/return chaining |
| 5 MUL/DIV | interpreter unless a profitable surrounding region exists |
| 6 branch/LFSR | interpreter initially; future CFG JIT candidate |
| 7 rare opcodes | interpreter |
| 8 DOS services | static kernel AOT |
| 9 self-modifying | interpreter / unstable cooldown |

The router is not failing if it interprets most of MDSTRESS.

It is succeeding if it refuses to use a slower tier.

---

# 55. Expected behavior on synthetic wins

For:

```text
loop
memloop
checksum
regionmix
branchmix
```

the router should detect enough heat/stability to promote.

Once promoted, execution should remain inside the profitable region rather than continually returning to the interpreter.

This is where JIT earns its SRAM.

---

# 56. Long-term engine identity

After this redesign, it may be useful to stop describing microDOS as having separate “interpreter mode,” “JIT mode,” and “AOT mode.”

The release engine becomes:

> **microDOS adaptive execution engine**

with:

```text
static native code when known,
semantic kernels when recognizable,
runtime native regions when profitable,
canonical interpretation everywhere else.
```

The separate firmware modes remain valuable for validation and research.

---

# 57. Final design conclusion

The performance work so far does not say that the interpreter won.

It does not say that AOT won.

It does not say that the JIT won.

It says something more useful:

> **Each mechanism wins under different conditions, and the cost of choosing the wrong one can be larger than the execution advantage of the right one.**

The next engine therefore needs to optimize **residency and selection**, not merely instruction lowering.

The intended final behavior is:

```text
Known stable code?
    -> AOT

Recognizable high-value semantic region?
    -> native region

Unknown code?
    -> interpreter first

Repeatedly hot and native-friendly?
    -> promote to region JIT

Poor native return?
    -> sticky interpreter cooldown

Repeated self-modification?
    -> interpreter until stable

Known JIT winner?
    -> stay native for as long as possible
```

The most important rule is:

> **A tier transition must be justified by enough expected guest work to pay for it.**

That rule is the common explanation for nearly every important microDOS result from M19 through M22, and it should become the organizing principle for M23.
