# M23 execution router: implementation and rollout

The [design proposal](M23_UNIFIED_TIERED_ENGINE_DESIGN.md) is preserved as supplied.
This implementation starts from `e8aa314` (M21.1c/M22.0) and implements the
interpreter-first baseline plus optional promotion and invalidation policy.
Pico performance acceptance remains a hardware gate, not a host-test result.

## What changes

In the non-cache DOS system loop, valid static AOT retains first refusal and
BIOS execution remains canonical. Unknown code runs in interpreter quanta of
at most 256 guest instructions, stopping at CS changes. A direct-mapped table
samples the resulting CS:IP and preserves heat, penalties, and cooldowns.
Tiny caller slices do not manufacture heat; only full quanta or CS boundaries
count as observations.

The table has 64 eight-byte sites: 512 bytes lean, 568 bytes with profiling.
Pico DOS JIT targets omit the old 384-byte JIT hotness table, so the net fixed
policy-state increase is 128 bytes lean or 184 bytes with profiling. The
`MdJitBlock` layout stays unchanged (64 bytes on the 32-bit ARM target). The
existing arena and block-slot settings are retained.

| Stage | Implementation | Default |
|---|---|---|
| M23.0 | Router state, reset, counters, profile reporting | Available |
| M23.1 | Interpreter-first unknown code, coarse heat sampling | Enabled in DOS JIT targets |
| M23.2 | Proven resident regions and shared semantic regions | Promotion off pending Pico measurements |
| M23.3 | Read-only probe and conservative direct-prefix admission | Direct admission off pending phase tuning |
| M23.4 | Generation-based invalidation feedback, sticky unstable cooldown | Used when promotion is enabled |
| M23.5 | Arena/slot reduction, flash placement, release-size experiments | Deferred as specified by the proposal |

### Admission and execution

`md_jit_probe()` uses the compiler's decoder, prefix-admission logic, and
resident shape validators. It does not emit Thumb, mark pages executable,
allocate a block, change guest state, or retain decoded IR. CALL/RET checks use
the decoder's flow metadata, including far and indirect forms.

At four samples, optional admission accepts existing profitable region shapes.
Optional direct admission requires at least eight native-prefix operations and
80% coverage, with no CALL/RET. Unsupported first instructions are rejected.
Admission failures back off for 16, 64, then 255 observations of that site.

`md_jit_prepare_region()` bypasses the legacy JIT hotness gate because the router
already owns admission. `md_jit_run_region()` runs only the admitted block or
region: it never compiles or interprets a new site internally. It returns at the
exact architectural exit CS:IP. Its local result includes retired/native work,
entries, zero exits, invalidation, CS changes, and budget refusal even in lean
builds. The fallback field is currently zero because interpretation belongs to
the system loop. Budget zero performs no work.

A refused native entry returns to a bounded canonical interpreter quantum.
Budget refusals do not penalize native coverage. Repeated weak or zero-progress
runs demote; useful native work reduces the penalty. Existing page generations,
code epochs, and block validity remain the correctness guards. Four accumulated
invalidation penalties mark a site unstable and hold it interpreted for a
255-observation cooldown. This is an advisory site policy, not a page-write
monitor or permanent blacklist. After cooldown it can be admitted again.

The shared DEC/JNZ and LODSW/ADD DX,AX/LOOP helpers execute sampled loop heads
with whole-iteration budget admission. Counted and CFG Thumb regions also accept
loop-head entry using the live CX value, including CX=0's 65536 iterations. Their
whole-run budget guard precedes guest mutation. Memory regions additionally
guard DS=0, SI's high bit, and the existing executable-page exclusion. Legacy
whole-run JIT/benchmark APIs retain their original region discovery behavior.

### Profiling

`MD_EXEC_PROFILE=0` removes aggregate router counters and reporting; the recording
calls become inline no-ops. Native profitability feedback is independent of
`MD_JIT_PROFILE`, so lean builds still demote safely. Profile firmware prints
retirement by tier, promotions/rejections/demotions, unstable demotions, tier
entries, switches per 1000 instructions, average JIT work per episode, and at
most four site candidates. Zero-progress JIT episodes count as tier entries.

The decoded-cache research target remains available. It does not participate in
the non-cache production router. Native CALL/RET chaining and additional x86
opcode lowering are deferred.

## Build and validate

Normal Windows workflow:

```powershell
.\md.bat deps msdos
.\md.bat build host
.\md.bat test
.\md.bat build pico
```

The default Pico DOS JIT images use the M23.1 interpreter-first baseline.
After building once, select the next experiments through the existing CMake
cache; `md.bat build pico` preserves those settings:

```powershell
# M23.2: resident regions only
cmake -S pico -B build-pico/out -DMD_EXEC_ENABLE_PROMOTION=ON -DMD_EXEC_ENABLE_DIRECT=OFF
.\md.bat build pico

# M23.3: also admit high-coverage direct prefixes
cmake -S pico -B build-pico/out -DMD_EXEC_ENABLE_PROMOTION=ON -DMD_EXEC_ENABLE_DIRECT=ON
.\md.bat build pico

# Return to the baseline
cmake -S pico -B build-pico/out -DMD_EXEC_ENABLE_PROMOTION=OFF -DMD_EXEC_ENABLE_DIRECT=OFF
.\md.bat build pico
```

Use `microdos_pico_jit_profile.uf2` to collect router and JIT diagnostics;
`microdos_pico_jit.uf2` uses the same routing choices with lean counters.
`microdos_pico.uf2` remains the static-kernel/interpreter reference, and
`microdos_bench.uf2` retains the legacy whole-run native benchmark path.

Portable host validation:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
HOST_BUILD="$PWD/build" sh tools/jit_qemu_check.sh
```

The QEMU script requires `gcc-arm-linux-gnueabihf` and `qemu-user`; it executes
emitted Thumb and the system router in baseline, promotion, and lean builds.
With the pinned DOS binaries and host image available, it also boots DOS2TEST
in all three configurations.

## Validation recorded for this change

- Host CTest passes 22/22 with both threaded and switch dispatch and pinned
  Microsoft MS-DOS 2.0 binaries.
- AddressSanitizer/UndefinedBehaviorSanitizer passes the promotion-enabled
  system differential fixtures (leak scanning disabled because the execution
  environment blocks LeakSanitizer process enumeration).
- Policy tests cover saturation, admission thresholds, rejection backoff,
  successful feedback, zero progress, budget refusal, invalidation cooldown,
  collisions, reset, and lean/profile sizes.
- System differential tests compare registers, segments, IP, architectural
  FLAGS, all guest memory, stop reasons, and instruction counts after slices
  of 1, 2, 3, 7, 16, 31, 64, 4096, and 1000000 instructions. They cover automatic
  heat-based admission and manually admitted regions, CALL/RET, INT/IRET,
  unsupported code, self-modification, invalidation, memory guard refusals,
  direct prefixes, shared regions, counted/CFG loop heads, and static AOT.
- Native Thumb QEMU runs retain the existing JIT differential and M20.3 gates,
  add router differential configurations, and require DOS2TEST 25/25.
- The original M22 QEMU shape assertions allowed only a one-instruction warm-up
  fallback. The untouched baseline reproduced three assertion failures despite
  identical architectural state. Nonzero allowances now account for one
  configured escape burst; zero-fallback requirements remain exact.
- Pico SDK 2.3.0 cross-compilation checks AOT reference, lean/profile baseline,
  native benchmark, and lean/profile promotion-enabled targets. Local checks
  use ELF outputs (`PICO_NO_PICOTOOL=1`), not flashed hardware.

## Hardware acceptance still required

Run all nine MDSTRESS phases in baseline, resident-only promotion, and direct
promotion configurations. Save separate CSVs and compare phase-by-phase native
residency, zero exits, rejections, and transition density. Run DOS2TEST 25/25 and
the synthetic benchmarks on Pico at the same clock and memory placement as M22.

Do not claim the proposal's MIPS/overhead goals from QEMU or desktop timing.
Before enabling promotion by default, verify unsupported-heavy phases remain
within 10% of interpreter performance (5% stretch), strong native region wins
remain, and kernel AOT retains its existing performance. Only then investigate
M23.5 arena/slot/placement savings. This change does not claim the future
10–15 KiB release-size goal or implement native CALL/RET chaining.
