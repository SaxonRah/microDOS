# NV2-G — general loop admission for Native v2

**Status:** design, ready for G-1 implementation
**Base:** commit `319755a`, measurement sweep `20261007-045818`

## 1. Why Native v2, and why "G"

Sweep results (RP2350, 300 MHz, guest in PSRAM, code in SRAM):

| engine | DOS2TEST | MDSTRESS | notes |
|---|---|---|---|
| interpreter | 3.47 | 2.77 | the floor |
| **Native v2** | 3.59 (8% native) | **37.36** (97% native) | best everywhere |
| AOT (93% compiled) | 3.43 | 2.67 | compiling flat DOS code does not pay |
| M25 / M25+NV2 | 2.73→1.86 | 6.55 / 15.69 | translation churn; steals NV2's loops |
| Native-3 (SRAM) | 0.69 | 3.87 | site-table thrash, NV2 re-probed at every IP |
| block cache | 0.75 | 1.25 | slower than the interpreter |

Microbenchmarks with the Native v2 emitter: regmix **132.7 MIPS** (2.26 cyc/guest),
callmix **86.4**. Native v2's code generator already runs above 100 MIPS on
register-heavy loops. Its limit is **admission**: the shapes it accepts.

NV2-G keeps Native v2's ABI, emitter style and runtime contract, and replaces
its per-shape admission rules with general machinery.

## 2. What Native v2 rejects today (code references)

All in `src/runtime/native_v2.c` `md_nv2_lower()` unless noted.

| rule | effect on DOS code |
|---|---|
| `inst.prefix_count != 0` → reject | no segment overrides (`ES:` scans, `CS:` tables) |
| conditional set = `72/74/75` + `E2` | JL/JG/JA/JBE/JS… loops and exits rejected |
| at most one side exit | multi-delimiter scanners rejected (`cmp al,' ' / je / cmp al,9 / je …`) |
| non-LOOP terminal must be backward `75` fed by `DEC r16` (not AX) | `lodsb/stosb/or al,al/jnz` (strcpy) rejected |
| `E2` exit flags only from a fixed producer list at `count-2` | most LOOP bodies rejected |
| memory forms mostly `DS` mod 0 | `[bx+si+disp]`, `[bp+…]` rejected |
| stores only in proven shapes; refused whenever `code_page_executable != NULL` (`md_native_v2_execute`) | general stores rejected |
| runtime requires `counter_reg`; retirement = iterations × ops (`native_v2_runtime.c`) | loops not counted by CX cannot run at all |
| runtime table: 32 direct-mapped slots | collisions evict/re-probe |

## 3. Invariants kept from Native v2

- **Resident ABI:** r0–r7 = AX CX DX BX SP BP SI DI, r8 = `MdX86*`, r9 = guest
  memory base, r10 = DS<<4. Guest registers are loaded once at entry and
  stored once at exit.
- **Helper-free hot path:** no C call inside a loop iteration.
- **Exact retirement and exact flags** at every exit.
- **Byte-validated re-entry:** `md_nv2_rt_code_matches()` on every entry.
- Native v2's existing special compilers stay first in the cascade, unchanged.

## 4. NV2-G region model

**Region = one natural loop** rooted at a back-edge target `T` (what the
interpreter's back-edge exit already reports):

- body = guest bytes `[T, L_end)` where `L_end` follows the latch (the
  backward branch to `T`);
- internal forward branches stay native;
- **exits** = any branch whose target is outside `[T, L_end)`, plus the
  fall-through after the latch; up to 8 exits;
- no CALL/RET/INT/far/indirect in G-1 (the local CALL graph compiler remains
  for its shapes).

## 5. Retirement and budget without a counter register

Native v2 schedules by `iterations = CX`. NV2-G loops need not be counted, so:

- **r11 = remaining budget** (Native v2 uses r11 for virtual CF; NV2-G does not
  keep CF virtual, see §6).
- At the latch: `SUBS r11, r11, #n_path; BLT budget_exit; B T`. Here `n_path`
  is the static guest instruction count of the path that reached this
  latch. With internal branches each path has its own count, accumulated per
  basic block with at most one `SUB` per block.
- **Exit stubs** retire their static partial count (instructions executed in
  the partial iteration, including the exiting branch).
- **Budget exit** happens before an iteration starts. IP = `T`, nothing
  partial, exact.
- The runtime computes `retired = budget_in − r11_out`, which is
  `dynamic_retire` mode 3. The budget also bounds non-terminating loops, so
  timers and USB keep running.

## 6. Flags: decided at exits, not per iteration

- Liveness over the loop CFG (Native v2 already does per-site `need_cf` /
  `need_z`).
- **Producer + consumer fusion:** a producer feeding a branch emits a native
  compare on operands shifted into the top bits (`LSL #16` for words, `#24` for
  bytes). That makes ARM N/Z/C/V the exact 8086 SF/ZF/(inverted) CF/OF. This
  is the full 16-condition table, already validated in M25:

| producer | O | NO | B | AE | E | NE | BE | A | S | NS | P | NP | L | GE | LE | G |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SUB/CMP/logic | VS | VC | CC | CS | EQ | NE | LS | HI | MI | PL | – | – | LT | GE | LE | GT |
| ADD | VS | VC | CS | CC | EQ | NE | – | – | MI | PL | – | – | LT | GE | LE | GT |
| INC/DEC | VS | VC | – | – | EQ | NE | – | – | MI | PL | – | – | LT | GE | LE | GT |

  Logic ops use `LSL rX,res,#16 ; CMP rX,#0`. `–` means not fusable, so G-1
  rejects the loop.
- **Exit flag recipes (the Phase 3Q idea, generalized):** each exit records the
  producer that reaches it (lazy op + operand sources). The exit stub writes the
  canonical lazy state (`lazy_op/a/b/res`, `lazy_carry` for INC/DEC). Nothing is
  written per iteration.
- A recipe needs the producer's operands intact at the exit. If a later
  instruction overwrites one, the producer saves that operand to a frame slot
  (one `STRH`). G-1 may simply reject such loops.
- `CF` needed on entry (ADC/SBB, INC/DEC lazy carry) comes from
  `md_x86_flags_materialize` at entry, as Native v2 does today.

## 7. Memory

- **G-1:** loads in every ModR/M form through DS (r10) plus a 16-bit offset
  wrap check, taken as a side exit. Linear wrap is excluded by the existing
  `ds <= EFFFh` runtime guard.
- **G-2:**
  - segment overrides. ES/SS/CS bases are loaded from the frame at use (two
    instructions; segment registers are invariant because segment writes are
    rejected).
  - stores, with an inline guard before each store:
    1. offset `FFFFh` → exit;
    2. the linear address hits the region's own code span → exit;
    3. in tracked builds, the page flag (`code_page_executable`) is nonzero →
       exit.

    Every guard exits *before* the store, so the instruction has not executed
    and the exit is exact. This lifts the blanket "no stores when tracking"
    rule in `md_native_v2_execute`.

## 8. Coverage roadmap (helper-free)

- **G-1:** everything `md_nv2_lower` already lowers, plus all Jcc, JCXZ,
  LOOPZ/LOOPNZ, CMP/TEST/OR as loop producers, and general ModR/M loads.
- **G-2:** segment overrides, guarded stores, LODS/STOS/SCAS/CMPS single
  (`DF==0` entry guard), PUSH/POP r16.
- **G-3:** ADC/SBB, shifts by CL, CBW/CWD, XCHG, then whatever the Phase 3P
  hot-reject profiler ranks next on DOS.

## 9. Runtime changes (`native_v2_runtime.c`)

- NV2-G is the 4th compiler in the cascade, after counted, local-call and
  REP-string.
- G slots: no `counter_reg`. Execute with `budget_in` in r11, then
  `retired = budget_in − r11_out`, then set IP from the exit tag table. Flags
  are already written by the exit stub.
- Slot table 32 → 128, 2-way. Keep signature-keyed rejects so a rejected loop
  costs one lookup.
- Admission only at back-edge targets (the interpreter's existing exit). The
  firmware integration is the `microdos_pico_nativev2` path, with no M25 and no
  Native-3 in front.

## 10. Verification

1. **qemu differential (host, no hardware).**
   - `md_native_v2_execute` is Thumb-2 and runs under `qemu-arm` exactly like
     the M25 harness.
   - Loops come from three sources:
     - the DOS reject-log byte patterns (strcpy, delimiter scanners, `ES:`
       compare scans);
     - random generated loop bodies;
     - MDSTRESS phases.
   - Compare against the interpreter at many budgets: registers, flags,
     retirement, memory.
2. **Existing `test_native_v2` suite unchanged** (special compilers untouched).
3. **On device:**
   - DOS splitbench on `microdos_pico_nativev2g`;
   - 624k silicon conformance through the interpreter, as a regression check.

## 11. Milestones and gates

| step | content | gate |
|---|---|---|
| G-1 | region model, budget-in-r11 retirement, full Jcc, many exits, exit flag recipes, general loads | 0 mismatches (qemu); MDSTRESS ≥ 37 (no regression); DOS2TEST Native v2 share rises from 8% |
| G-2 | segment overrides, guarded stores, single string ops, PUSH/POP | DOS2TEST share up again; MDSTRESS unchanged |
| G-3 | ADC/SBB, CL shifts, profiler-ranked ops | next profiler round |
| G-4 | AArch64 port of the Native v2 + G emitter (Pi Zero 2 W) | ≥ 100 MIPS on hot loops at 1 GHz |

**Expectation on DOS2TEST.** Its code is flat (AOT with 93% coverage gained
nothing), so NV2-G will raise the native share there more than the MIPS.
The real test of NV2-G is hot loops in real programs, so add an assembler,
compiler or archiver run to the splitbench basket alongside it.
