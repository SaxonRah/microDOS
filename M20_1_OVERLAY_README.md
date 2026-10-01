# microDOS M20.1 overlay

Apply this overlay on top of the current working M20.0 tree.

M20.1 contains two changes:

1. **Full-DOS JIT observability**: Ctrl+] now reports static-AOT/JIT-native/interpreter partitioning, interval JIT counters, exit reasons, code-cache use, and sampled hot CS:IP sites.
2. **Bounded CFG resident regions**: discovery can continue through forward JE/JNE edges and compile a multi-block counted region with native internal branches.

The M19.2 exact fast paths and M20 generic counted regions remain first-class paths; M20.1 is additive.

## Build

```powershell
cd C:\microDOS
.\md.bat clean
.\md.bat build host
.\md.bat test
.\md.bat build pico
```

The clean command may occasionally report a locked generated `.obj`; if the subsequent host build regenerates successfully and the eight tests pass, no source artifact was lost.

## Benchmark

Flash:

```text
build-pico\out\microdos_bench.uf2
```

The existing loop/memloop/regionmix results should stay correct. New rows:

```text
branchmix SRAM  ... jit ... ok
branchmix PSRAM ... jit ... ok
```

The JIT counters should show a CFG region, approximately:

```text
cfg-regions=1
cfg-entry=1
cfg-edges=2
fallback=0
```

`branchmix` has 212,996 guest instructions including HLT and ends with AX=0000h, BX=C000h, CX=0000h.

## Real DOS

Flash:

```text
build-pico\out\microdos_pico_jit.uf2
```

First re-run `DOS2TEST`; acceptance remains 25/25. Then use Ctrl+] before and after shell workloads. The new report includes:

```text
[perf] tiers: static-aot ... jit-native ... interpreted ...
[jit] native=... fallback=... entry=... compile=... hit=... miss=...
[jit] fallback-reason compile=... budget=... zero=...
[jit] resident=... generic=... cfg=... code=.../24576 B
[jit] exit histogram: ...
[jit] hot sites ...
```

Native-return hot sites are sampled 1/64 to keep profiling overhead small; aggregate return counts are exact.

See `docs/M20_1_CFG_OBSERVABILITY.md` for architecture and the recommended profiling intervals.
