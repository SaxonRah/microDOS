# microDOS architecture

## Core state

All execution engines operate on the same `MdRuntime` / `MdX86` state.

```text
8086 code
   |
   +--> interpreter
   +--> decoded block cache
   +--> static AOT
   +--> runtime JIT
            |
            v
      MdRuntime / MdX86
            |
            v
       shared semantics
```

Optimized paths must produce the same architectural state as canonical interpretation.

## Decoder

`src/decode/` describes instruction structure and direct control flow without executing instructions.

It is shared by `dosprobe`, `dosrecomp`, block discovery and JIT discovery.

Structural decoding and execution semantics are separate.

## Runtime

`src/runtime/` contains:

- `x86_interp.c` — canonical interpreter;
- `x86_block_cache.c` — decoded-block execution;
- `region.c` — architecture-neutral hot-region helpers;
- `exec_router.c` — execution-tier selection;
- JIT core/backend code — runtime translation;
- `runtime.c` — shared state and invalidation.

Guest code writes update executable-page generations. Cached or generated code validates the relevant generations before reuse.

## Static AOT

`dosrecomp` discovers reachable instructions and emits portable C against the shared runtime.

Unsupported or invalidated regions return to canonical execution.

AOT attachment metadata belongs to `MdRuntime` and does not outlive a runtime reset.

## Runtime JIT

The JIT owns decoded IR, hot-region admission, generated-code storage, architecture-specific native emission and fallback.

Backend-specific machine code should reuse shared semantic helpers for complex behavior instead of creating a second guest-semantics implementation.

## MS-DOS system layer

`src/system/md_dos2_system.c` owns the platform-neutral DOS execution loop and engine handoff.

DOS filesystem behavior stays inside DOS. Platform code provides device boundaries such as console, clock and block storage.

## Platforms

- Host: tests, analysis and differential validation.
- RP2350 / Pico 2: startup, USB console and target memory placement.
- Raspberry Pi Zero 2 W: AArch64 bare-metal startup, EL2 memory setup, UART and platform integration.

## Correctness boundary

A fast path may execute only while its assumptions remain valid. Otherwise it returns to the shared runtime at an exact guest CS:IP.

That rule applies to AOT, block-cache and JIT execution.