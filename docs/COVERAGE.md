# Coverage snapshot

> **Historical snapshot.** These figures come from the 2026-07-28 (Phase 0)
> `coverage`-preset runs at 41/43 tests. The suite has since grown to 47
> tests (adding group-commit, crash-injection, power-loss, and Python-binding
> coverage); re-run the `coverage` preset (see "How to reproduce this report")
> for current numbers.

_Last updated: 2026-07-28 (Phase 0 cleanup)_


Two runs of the `coverage` preset:
1. **Initial baseline** at wrap-up commit `aac2445` (41 tests).
2. **Post-follow-through** at commit `f890a2c` (43 tests, +new tests
   from the post-wrap-up session).

Both had 2 pre-existing preset-artifact failures (`abi_symbols`,
`release_audit`) documented in the "Known preset-artifact failures"
section below.

## Overall

| Metric | Baseline (41 tests) | After follow-through (43 tests) | Δ |
|---|---:|---:|---:|
| Lines (src + tests) | 64.81% (12,613 / 19,461) | **64.61%** (13,096 / 20,270) | -0.20 pp* |
| Functions | 98.17% (430 / 438) | **98.26%** (453 / 461) | +0.09 pp |
| Regions | 89.87% (11,957 / 13,305) | **90.71%** (12,379 / 13,647) | +0.84 pp |
| Branches | 56.41% (4,636 / 8,219) | **57.02%** (4,837 / 8,483) | +0.61 pp |

\* The tiny drop in raw line-percentage is an artifact of adding 809
lines of new test code; the src-only line coverage improved (see
per-file table below).

The line total includes test files. Restricting to `src/*.c` only, the
weighted line coverage is **~86%** across ~9,600 core lines (up from
~85% at baseline).

## src/*.c per-file delta (baseline → follow-through)

| File | Baseline | Follow-through | Δ |
|---|---:|---:|---:|
| `src/status.c` | 38.10% | **100.00%** | +61.90 pp |
| `src/btree_page.c` | 89.27% | **93.66%** | +4.39 pp |
| `src/key_manager.c` | 94.81% | 98.70% | +3.89 pp |
| `src/sync.c` | 71.61% | 74.19% | +2.58 pp |
| `src/superblock_store.c` | 91.94% | 94.09% | +2.15 pp |
| `src/random.c` | 38.30% | 40.43% | +2.13 pp |
| `src/file.c` | 83.05% | 84.01% | +0.96 pp |
| `src/xchacha20poly1305.c` | 94.52% | 95.43% | +0.91 pp |
| `src/btree.c` | 87.03% | 87.13% | +0.10 pp |
| all others | (unchanged) | | 0 |

## Per-source-file breakdown

Sorted by absolute uncovered-line count (most impact for future work):

| File | Lines | Uncovered | Coverage | Notes |
|---|---:|---:|---:|---|
| `src/engine.c` | 2,002 | 225 | 88.76% | High-level object/index/backup APIs; some paths only reachable via specific engine tests. |
| `src/btree.c` | 1,064 | 138 | 87.03% | Includes the new reclaim_internal_up edge branches (all currently defensive, only fire on corrupt tree state). |
| `src/pager.c` | 796 | 78 | 90.20% | Freelist edge cases; some txn-cancel-alloc paths added post-audit. |
| `src/file.c` | 419 | 71 | 83.05% | Windows paths (`sdb_windows_*`) not covered on Linux CI. |
| `src/sync.c` | 155 | 44 | 71.61% | Platform-specific mutex primitives; some POSIX robust-mutex paths not exercised. |
| `src/wal.c` | 461 | 40 | 91.32% | Some corrupt-WAL recovery arms only reached by fault-injection. |
| `src/random.c` | 47 | 29 | 38.30% | getrandom `ENOSYS`/`EPERM` fallback paths not reachable on modern Linux. |
| `src/btree_page.c` | 205 | 22 | 89.27% | Boundary condition on max-fanout encoding. |
| `src/superblock_store.c` | 186 | 15 | 91.94% | Mirror-torn recovery path partially covered. |
| `src/status.c` | 21 | 13 | 38.10% | **BEFORE**: 12/15 status codes had 0 direct hits. Fixed by `tests/test_coverage_gap.c` this session. |
| `src/xchacha20poly1305.c` | 219 | 12 | 94.52% | Some over-large AAD paths not exercised. |
| `src/replace.c` | 144 | 11 | 92.36% | Some `sdb_replace_recover` mismatched-file_id branches. |
| `src/encrypted_page.c` | 110 | 9 | 91.82% | Bad-tag rejection path partially covered. |
| `src/sha256.c` | 123 | 9 | 92.68% | Over-block final-flush edge. |
| `src/page_cache.c` | 91 | 7 | 92.31% | Eviction-of-locked path not exercised. |
| `src/page.c` | 110 | 7 | 93.64% | Bad-flags rejection covered by field-diag test. |
| `src/superblock.c` | 174 | 6 | 96.55% | New field-diag surface covers 11 branches. |
| `src/key_manager.c` | 77 | 4 | 94.81% | **BEFORE**: no direct test file. Fixed by `tests/test_coverage_gap.c`. |
| `src/checksum.c` | 32 | 3 | 90.62% | 3-byte tail-partial-block edge. |
| `src/codec.c` | 45 | 0 | 100.00% | Full coverage. |
| `src/windows_path.c` | 84 | — | (not built) | Only compiled on Windows. |

## Tests added this session

**`tests/test_coverage_gap.c`** — targets the biggest zero-hit gaps:

1. `sdb_status_string` — exercises all 15 documented status codes plus
   the default branch. Prior: 3 codes hit.
2. `sdb_version_string`, `sdb_abi_version` — sanity assertions.
3. `sdb_random_bytes` bad-arg branches (NULL, size 0) plus one positive
   call for direct coverage of the getrandom path.
4. `sdb_key_wrap` / `sdb_key_unwrap` — round-trip, wrong-password
   (SDB_E_AUTHENTICATION), and every bad-arg rejection. Prior: no
   direct test file existed for this module.

**`tests/test_superblock_field_diag.c`** (A4.5 companion, added earlier
this session) — exercises the 11 field-level failure codes returned by
the new `sdb_validate_superblock_detailed` helper.

## Known preset-artifact failures under `coverage`

- **`abi_symbols`** — *fixed since this snapshot.* Under the coverage
  preset `libshibadb.so.1.0.0` contains an extra
  `__llvm_write_custom_profile` symbol from the LLVM coverage runtime,
  which the check-symbols script compared literally and failed on.
  `tests/check_symbols.cmake` now filters toolchain-runtime symbols by
  prefix (`__llvm_*`, `__gcov*`, `__asan_*`, …), so the coverage and
  sanitizer presets check the ShibaDB surface as strictly as a plain
  build instead of failing. The test is also registered in CTest again
  (it had been dropped from `CMakeLists.txt`) and now reads PE and
  Mach-O exports as well as ELF, so it runs everywhere except MSVC
  (see `docs/ABI.md`).
- **`release_audit`** — `scripts/release_audit.py` requires evidence
  paths configured through environment variables that only exist in the
  release-CI environment. Fails in every developer build; not a
  regression caused by this session.

## Uncovered hot paths worth future attention

Top 5 candidates for future coverage-fill sessions:

1. **`src/random.c` getrandom fallback (lines 42-88)** — reaching
   `ENOSYS`/`EPERM`/`EACCES` requires a syscall-injection harness
   (LD_PRELOAD or seccomp). Blast-radius: none in production; hardening
   only.
2. **`src/sync.c` robust-mutex recovery (~44 uncovered lines)** —
   requires a process-crash test that a surviving process can detect
   via `pthread_mutex_consistent`. High blast-radius (concurrency), low
   feasibility.
3. **`src/wal.c` torn-trailer paths (~40 uncovered)** — partially
   covered by `test_transaction_crash`; the specific
   `sdb_wal_body_checksum_mismatch` arm needs a byte-flip harness.
4. **`src/btree.c` reclaim edge branches (~138 uncovered)** — the new
   guards added this session are defensive; they only trigger on
   corrupt on-disk state. Reaching them requires the fuzz corpus to
   exercise a hand-crafted internal page with duplicate/aliased
   children.
5. **`src/engine.c` migrate paths (~225 uncovered)** — the migration
   subsystem has partial coverage; a targeted migrate test would help.

## How to reproduce this report

```sh
cd /home/satoharuki/project/shibadb-c
cmake --preset coverage
cmake --build build-coverage
mkdir -p build-coverage/profiles
ctest --preset coverage -j4                    # ~18 min
find build-coverage/profiles -name '*.profraw' \
    | xargs llvm-profdata merge -sparse -o build-coverage/merged.profdata
TESTBINS=$(find build-coverage -maxdepth 1 -type f -executable -name 'test_*' | tr '\n' ' ')
llvm-cov report build-coverage/libshibadb.so.1.0.0 \
    $(echo $TESTBINS | awk '{for(i=1;i<=NF;i++) printf "-object %s ", $i}') \
    -instr-profile=build-coverage/merged.profdata \
    > build-coverage/summary.txt
```

## Baseline comparison

No prior coverage snapshot existed before this session. This report is
the first coverage baseline for shibadb-c.
