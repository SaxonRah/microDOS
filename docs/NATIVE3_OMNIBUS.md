# Native-3 Omnibus v1 — N3.0 through N3.8

Native-3 is a new parallel execution engine. It does not replace or mutate the M30/M32 engines; build-time selection keeps them available as controls.

## Goal

- RP2350 @ 300 MHz: >=40 MIPS on hot CPU-bound 8086 workloads (7.5 host cycles/guest maximum average).
- Pi Zero 2 W @ 1 GHz: >=40 MIPS minimum (25 host cycles/guest).
- Exact 8086 semantics remain authoritative; unsupported native cases exit to the canonical interpreter.

## N3.0 — measurement + common frontend

`native3.c` owns canonical `(CS,IP)` sites, code signatures, page-generation snapshots, native/interpreter retirement counters, first-hit admission, and the hardware 40-MIPS gates.

## N3.1 — Pico/M33 backend

The first Pico backend promotes the already-proven helper-free Native-v2 emitter into Native-3’s first-choice hot backend. Native-v2 already uses the high-performance resident ABI (all eight guest GPRs in ARM registers) and has measured 5–7 cycle/guest loops. General canonical code is handled by the shared JIT rather than by a separate trace engine.

## N3.2 — canonical blocks / CFG / CALL-RET locality

- no per-root traces;
- canonical JIT blocks remain keyed by `(CS,IP)`;
- first admission prefetches a bounded forward successor chain as independent canonical blocks;
- backward edges are not expanded into traces;
- a guest return shadow stack records CALL/RET locality and provides the metadata contract for native return linking.

## N3.3 — memory

Native-v2 keeps helper-free direct-memory regions; general JIT code uses existing direct load/store lowering and tracked-store slow paths. Translated instruction fetches disappear from guest RAM once code is native.

## N3.4 — SMC + exact fallback

- `code_epoch` resets Native-3 on image replacement/reset;
- every site snapshots code page generations;
- JIT block guards remain authoritative;
- unsupported/zero-progress sites execute bounded interpreter escapes and never spin at an unchanged IP.

## N3.5 — DOS integration

`MICRODOS_ENABLE_NATIVE3=1` inserts Native-3 before legacy dynamic tiers in `md_dos2_system_run`. BIOS remains on the canonical path. The M30/M32 targets remain untouched.

## N3.6 — AArch64 direct backend

`native3_a64_direct.inc` extends the existing AArch64 emitter with helper-free direct prefixes for common 16-bit:

- MOV reg,imm;
- ADD/SUB/AND/OR/XOR/CMP reg,reg;
- ADD/SUB/AND/OR/XOR/CMP AX,imm;
- Group-1 reg,imm;
- NOP/JMP.

It writes canonical lazy-flag metadata directly, so arithmetic prefixes no longer require semantic C helpers. Unsupported/control tails stop at an exact guest IP and use the existing shared engine.

## N3.7 — AOT / persistent prewarm cache

Native-3 cache manifests are pointer-free, versioned, checksummed, and host-architecture independent. They store hot site identities/fingerprints, then re-emit host-native code when imported. `native3_cache_tool` inspects manifests.

## N3.8 — size

- compact site metadata;
- shared MdJit compile IR rather than per-site IR arrays;
- no M31/M32 trace copies or alias bodies;
- profile counters compile out with `MD_N3_PROFILE=0`;
- persistent cache stores manifests rather than native blobs.

## One-shot validation

```powershell
cd C:\microDOS
.\scripts\md_native3_all.ps1
```

Hardware benchmark output contains `GATE40=PASS/FAIL`. Do not call the engine “40 MIPS overall” until the hardware rows and DOS splitbench both prove it.
