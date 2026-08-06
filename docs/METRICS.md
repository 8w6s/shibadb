# Metrics — Phase 0 baseline

_Snapshot: 2026-07-28 sau Phase 0 clean slate._

## Health signals

| Metric | Baseline | Target 1.0 | Hvpdb red line |
|---|---:|---:|---:|
| Production LOC (`src/` + `include/`) | 12747 | — | — |
| Test LOC (`tests/`) | 7182 | — | — |
| Test / Production ratio | 56.3% | ≥30% | 3% |
| Duplicate doc files (`*_YYYY_MM_*.md`) | 0 | 0 | 16 |
| Comment lines in `*.c`/`*.h` | 0 | 0 | — |
| Comment lines in `*.py` (ngoài shebang) | 0 | 0 | — |

## Anti-pattern checks (Phase 0 gates)

- [x] Xóa Python legacy `shibadb/`
- [x] Xóa `refs/rust-v0`, `refs/python-v0-docs`
- [x] Xóa `docs/hvpdb-compatibility.md`
- [x] Xóa `docs/README_legacy.md`
- [x] Gộp AUDIT/COVERAGE/SECURITY_REVIEW duplicates
- [x] Xóa build dir
- [x] Xóa toàn bộ comment trong `.c`/`.h`/`.py`
- [x] Fresh git repo, sole author = satoharuki
- [x] Move `shibadb-c/*` lên root

## Build health

- CMake preset `gcc`: OK
- ctest: 38/38 PASS (380s, real)

## Performance — 2026-07-29 (first evidence)

> **Note:** the auto-commit throughput figures in this section and the next
> were measured *before* the persistent-WAL-fd (R2a) and leader/follower
> group-commit (R4) work landed. They are retained as a historical baseline;
> current auto-commit behaviour issues fewer fsyncs and coalesces concurrent
> commits, so these ~60 ops/s single-thread numbers no longer reflect the
> engine. See "Optimization status" below.

_Setup: single SSD (non-rotational), 8-core / 7.6 GB, `bench_kv` + `bench_churn`
(preset `bench`), MIN KDF iterations, fixed value pattern._

### KV throughput (encrypted, value 100 B)

| Op | Auto-commit | Batched (1000/txn) |
|---|---:|---:|
| put | ~60 ops/s (P50 ~16 ms) | ~10,000 ops/s |
| get | ~3,700 ops/s (P50 ~0.28 ms) | — |
| delete | ~70 ops/s (P50 ~14 ms) | — |

- Auto-commit is durability-bound: each commit issues ~5-6 fsync/dir-sync
  (WAL write x2 + data + superblock + WAL-clear). ~60 commits/s even on SSD.
- Batching amortises fsync (~200x). Batch-size sweep (16 B, batched put):
  batch 100 -> 4.1k, 500 -> 6.7k, 1000 -> 9.9k, 5000 -> 14.3k ops/s;
  throughput keeps rising while commit latency grows to ~350 ms at 5000.
  Recommended batch ~1000 for the throughput/latency balance.
- Encryption overhead: ~0% on writes (I/O-bound), ~3-5% on small reads,
  ~21% on 4 KB reads (per-byte AEAD decrypt).
- get latency ~0.27-0.30 ms is stable regardless of stale entries -> lazy-GC
  stale data causes no measurable read amplification.

### Compaction (batched-write fix, 2026-07-29)

- Before: compact copied live entries via per-entry auto-commit
  (`sdb_btree_put`) -> ~28 objects/s -> 70.6 s for a 2000-object DB
  (linear; minutes for large DBs).
- After: compact batches target writes (flush every 4096 entries) -> 4.85 s
  for the same DB (~14.5x), identical result (raw_entries 6000->4000, objects
  preserved). Remaining cost is CPU (per-entry liveness check + tree build).

### Optimization status (must preserve crash-safety)

- **Shipped — persistent WAL fd + single-fsync commit (R2a):** the WAL file
  descriptor is kept open instead of open/truncate/parent-dir-sync per commit,
  and the redundant WAL-clear sync + dir-sync are gone. The auto-commit
  throughput numbers earlier in this file predate this change and no longer
  reflect current behaviour.
- **Shipped — leader/follower group commit (R4):** concurrent committers
  coalesce into a single fsync via a `commit_in_flight` counter; a leader
  performs the durability barrier on behalf of the followers it batches.
  Covered by `test_group_commit` / `test_group_commit_batch`. This raises
  auto-commit throughput under concurrency without weakening durability.
- Still open: the compact liveness check is O(entries) source lookups and
  could be streamed.

## Performance — 2026-07-29 (encrypted vs plaintext, clean measurement)

Single SSD, MIN KDF, `bench_kv`, 1M soak paused during measurement. Auto-commit
is one transaction per op; batched is 1000 ops/txn.

100-byte values:

| op | encrypted | plaintext |
|---|---:|---:|
| put (auto) | 59 ops/s | 59 ops/s |
| delete (auto) | 76 ops/s | 71 ops/s |
| get (auto) | 2432 ops/s | 3326 ops/s |
| put (batched) | 8608 ops/s | 10874 ops/s |

4 KB values:

| op | encrypted | plaintext |
|---|---:|---:|
| put (auto) | 71 ops/s | 66 ops/s |
| delete (auto) | 76 ops/s | 80 ops/s |
| get (auto) | 1136 ops/s | 1758 ops/s |
| put (batched) | 1363 ops/s | 1847 ops/s |

Reading:

- Auto-commit put/delete (~60-80 ops/s) is identical encrypted vs plaintext at
  both value sizes — the bottleneck is the per-commit fsync barrier
  (durability I/O), not encryption. This is the expected floor of an
  fsync-per-commit ACID database on a consumer SSD.
- Encryption cost appears only on reads: get is ~27% slower at 100 B
  (2432 vs 3326) and ~35% at 4 KB (1136 vs 1758) — per-byte AEAD decrypt.
- Batching amortises the fsync: ~145x over auto-commit at 100 B. At 4 KB a
  1000-op batch moves ~4 MB WAL+data per txn, so it is I/O-bound
  (~1.4-1.8k ops/s) rather than fsync-bound.
- Guidance: batch bulk writes (docs/OPERATIONS.md).

Batched commit latency, 100 transactions of 1000 ops each (encrypted, clean —
the earlier 2-3-txn percentiles were not statistically meaningful):

| value | throughput | commit P50 | commit P99 |
|---|---:|---:|---:|
| 100 B | 10770 ops/s | 92 ms | 156 ms |
| 4 KB | 884 ops/s | 971 ms | 2178 ms |

A 1000-op batch of 4 KB values writes ~4 MB WAL + ~4 MB data per commit, so its
commit latency is ~1 s (P99 2.2 s). Optimal batch size depends on value size:
1000 balances throughput and latency for small values, but for large values a
smaller batch (~100-200) keeps commit latency and tail bounded.

## Hardening evidence — 2026-07-29 (local, CachyOS, 8-core)

| Check | Result |
|---|---|
| Fuzzing (7 targets, 2 rounds ~2.5h, libFuzzer+ASan) | 0 crashes; ~384M execs btree_page, 94M superblock, 29M wal_recover, 23M page, 12M pager_open, 9M xchacha20poly1305, 0.4M encrypted_envelope |
| ThreadSanitizer (full ctest) | 38/38 pass, 0 data races |
| ASan+UBSan soak (20k ops) | 0 leaks, 0 errors |
| LLVM source coverage (ctest) | line 86.25%, function 97.55% (7/286 uncalled), region 88.49%, branch 62.92% |
| Long encrypted soak | ran ~9h14m (stopped manually before reaching 1M); hundreds of thousands of mixed put/delete/get ops with verify every 250, backup+reopen every 500, compact every 1000 — 0 assertion failures. The KDF-reopen + backup + compact per few-hundred ops (not the KV ops) dominate its wall-clock; it is a lifecycle stress test, not a throughput benchmark |

Lowest coverage is `random.c` (39% line) and `sync.c` (77%): syscall error
paths (getrandom / pthread) that need fault injection, not correctness-critical.

## Windows-native evidence — 2026-08-07 (MSVC, local)

First native-Windows gate run. Toolchain: MSVC 14.51 (`cl` 19.51) + Ninja,
`RelWithDebInfo`. The project had previously only been gated on Linux.

| Check | Result |
|---|---|
| Build `/W4 /WX`, no suppressions | Clean. Every C4701 (false positives from MSVC's inter-procedural flow analysis not modelling the `status == SDB_OK ⇒ out-param written` contract) and C4996 (`sscanf` in two tests) was fixed at source, so `/wd4701` and `_CRT_SECURE_NO_WARNINGS` were removed from the build entirely |
| `ctest` (57 tests) | 57/57 pass |
| Engine soak (`test_engine_soak`, `SDB_SOAK_OPERATIONS=50000`) | exit 0 in ~10.5 min; mixed put/delete/get with periodic verify, backup+reopen, and compact; 0 assertion failures |
| Path robustness fix | `sdb_database_open` now accepts forward-slash paths (was `SDB_E_IO` because the `\\?\` extended-length prefix disables Win32 `/`→`\` normalization); create/open/lock are now consistent. See `docs/WINDOWS.md` |

Not yet run on Windows (Clang-only, deferred to the Linux/Clang gate): UBSan,
ThreadSanitizer, libFuzzer. AddressSanitizer via MSVC `/fsanitize=address` is
being added as a Windows memory-safety pass.

See `docs/WINDOWS.md` for the full Windows build/portability standard.

## Next

Phase 1 hardening — 4 pillar parallel + CI infra. See `docs/superpowers/plans/2026-07-28-phase-1-4pillar.md` (to be written).
