# M21.1b — guest store path

## Finding

M21.1's A/B firmware showed guest memory placement is not the DOS
bottleneck (128 KiB guest: SRAM 3.532 vs PSRAM 3.491 MIPS, +1.2%).
Profiling the same DOS2TEST session by function then showed where the time
goes: word stores 40.9% of all host work, plus 12.9% in disk sector copies
through the same path; the compiled kernel's own work about 12%. DOS keeps
its stack and variables on the same pages as its compiled code, so nearly
every store paid the full code-tracking cost (page checks per byte, page
generation and global epoch bumps, a scan of every AOT guard per byte).

## Changes

- Live-code bitmaps (`MdX86.aot_live_bits`, pool in `MdRuntime`): per code
  page a merged bitmap of live compiled bytes. A store whose bit is clear
  needs no guard work; only live hits take the out-of-line guard path.
  Bitmaps are built on attach and cleared per chunk as chunks die, with
  bits another live guard still needs re-derived. Pool: 8 pages
  (`MD_AOT_LIVE_PAGES`); further pages use an all-ones bitmap (= always
  take the exact guard path).
- Per-page owner map (`aot_page_owner`): the guard path checks only the
  owning guard unless a page is shared.
- Two page flags: `MD_X86_PAGE_TRANSLATED` (decoded cache/JIT code here:
  page generations must change) and `MD_X86_PAGE_AOT` (compiled code may be
  live here). Compiled images set only AOT on attach; loaders
  (`md_runtime_load_raw/com`) no longer mark pages, because the cache and
  JIT mark exactly what they translate.
- Tracked stores are out of line (`md_x86_store8/16_tracked`) with the
  plain-page case inline; generated code always calls the shared
  `md_aot_store8/16/push` functions.
- `md_x86_write_block`: disk sectors are copied with one memcpy and
  per-page bookkeeping instead of 512 tracked byte stores.
- `MdAotGuard.invalidations` now counts stores that hit a LIVE compiled
  byte (statistic only; nothing else reads it).
- Build: generated sources have a single owner target (`md_generated`);
  previously each consuming target regenerated them, and a parallel build
  could compile a half-written file.

## Validation

- `tests/test_store_tracking.c` (ctest `store_tracking`): 200,000 random
  byte/word/block stores (incl. 64 KiB-wrapping blocks, page edges, a page
  shared by two guards, an AOT-only guard, live-pool overflow) compared after
  every operation against the pre-M21.1b algorithm kept as the reference;
  plus a deterministic overlapping-attachment case. Planted bugs in the
  owner map and in the overlap re-derivation are each caught.
- Host suite 14/14 (all DOS e2e modes, lockstep, self-modifying code, JIT,
  regions).
- Host work for the DOS2TEST session: 73.3M -> 60.1M instructions (-18%).
  Store checks resolved by one bit test: 581,125 of 581,183.
- SRAM: about +6-7 KB (bitmap pool, pointer table, owner map).
