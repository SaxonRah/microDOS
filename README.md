# microDOS

microDOS is an 8086/MS-DOS execution environment targeting desktop hosts, RP2350/Pico 2, and Raspberry Pi Zero 2 W bare metal.

The project keeps one architectural state and one semantics layer while supporting several execution engines:

- canonical 8086 interpreter;
- decoded-block cache;
- static AOT recompilation with `dosrecomp`;
- runtime JIT for hot regions.

Known binaries can run through AOT while unknown or changing code falls back to the canonical runtime.

## Quick start

Windows host:

```powershell
.\md.bat deps msdos
.\md.bat build host
.\md.bat test
.\md.bat run dos2
```

Pico 2 / RP2350:

```powershell
.\md.bat build pico
```

Raspberry Pi Zero 2 W bare metal:

```powershell
.\md_pi0w_baremetal.ps1 build
.\md_pi0w_run.ps1 -NoBuild -Interactive
```

The Pi target is a freestanding AArch64 image; Linux is not involved.

## Repository layout

| Path | Purpose |
|---|---|
| `include/microdos/` | public runtime, decoder, JIT and AOT interfaces |
| `src/runtime/` | interpreter, block cache, regions and JIT |
| `src/system/` | platform-neutral MS-DOS system loop |
| `aot/` | source-assisted AOT metadata |
| `tools/dosprobe/` | structural 8086 binary analysis |
| `tools/dosrecomp/` | static recompilation to portable C |
| `tests/` | runtime and DOS regression tests |
| `pico/` | RP2350 firmware |
| `pi0w/` | Raspberry Pi Zero 2 W bare-metal target |
| `third_party/` | local dependency instructions only |

## MS-DOS 2.0

`md.bat deps msdos` fetches the pinned MS-DOS dependency into the ignored `third_party/msdos/` checkout.

The released binaries remain the behavioral authority. Source-derived metadata is used to discover entry points and improve recompilation coverage.

`DOS2TEST.COM` is the primary full-system regression program.

## Development rules

- Keep 8086 architectural behavior in the shared runtime/semantics layer.
- Platform code owns hardware, startup, clocks, console and storage bindings.
- Optimized engines must fall back to canonical execution when they cannot prove a safe fast path.
- Generated binaries, captures, benchmark output, local backups and temporary patches are not tracked.
- Historical milestone notes belong in Git history, not the current tree.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## License

See [LICENSE](LICENSE).