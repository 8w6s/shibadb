# Changelog

All notable changes to shibadb-c are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The
project uses semantic versioning; the C ABI and on-disk format are
frozen at v1.0.

## [Unreleased]

Performance and durability hardening on top of the `v1.0.0-rc*` line. The
C ABI and on-disk format remain frozen at v1.0.

### Performance
- **Opt-in relaxed durability (`SDB_SYNCHRONOUS_NORMAL`).** A new `synchronous`
  field on `sdb_database_options` (default `SDB_SYNCHRONOUS_FULL`, unchanged
  behaviour) lets auto-commits acknowledge after the WAL pwrite but before the
  fsync, folding durability into the next checkpoint — ~4x higher single-thread
  auto-commit throughput. Checkpoint fsyncs the WAL before copying frames into
  the data file, so the write-ahead invariant holds: a power loss can only drop
  the newest un-checkpointed commits, never corrupt the database, and acked
  commits still survive a process crash. Explicit transactions stay fully
  durable regardless. Carved from the reserved block (ABI/`sizeof` unchanged);
  exposed via the C option and the CLI `--synchronous full|normal` flag.
- **Persistent WAL file descriptor + single-fsync commit (R2a).** The WAL
  is no longer opened/truncated/parent-dir-synced on every commit; the fd
  stays open and a commit issues a single fsync barrier. Checkpoint reads
  the WAL through that same persistent fd (also fixes Windows file
  sharing). Commits `7187883`, `5181f6d`.
- **Leader/follower group commit (R4).** Concurrent committers coalesce
  into one fsync via a `commit_in_flight` counter (promoted from a bool);
  a leader performs the durability barrier for the followers it batches,
  raising auto-commit throughput under concurrency without weakening
  durability. New `test_group_commit` / `test_group_commit_batch`. Commit
  `fa2932e`.
- **CRC32 slice-by-8 page integrity.** The per-page checksum now uses a
  slice-by-8 table implementation.
- **Hash-indexed, byte-configurable page cache.** Commit `47fb8cf`.

### Fixed
- **Compaction stripped encryption.** Compact copied entries in a way that
  could drop the page encryption envelope; the target now preserves it.
- **Python binding closed the handle on `SDB_E_BUSY`.** The binding no
  longer tears down the underlying database on a BUSY result.

### Security
- **`data_key` scrubbed after compact.** Compact's stale `data_key` copy is
  wiped with `sdb_secure_zero` before release. Commit `cd28c53`.
- **Authenticated encrypted superblocks.** Encrypted slots now carry an
  HMAC-SHA256 over all control and key-wrap header fields. Existing encrypted
  databases upgrade after successful password authentication.
- **Database-bound WAL v5 frames.** Every current frame carries `file_id`, so
  recovery rejects foreign replay even when the separate identity header is
  torn. Bound v3/v4 WALs remain readable.

### Testing
- **Crash-injection gate (`crash_injection`).** SIGKILL mid-commit across
  24 kills: 621 acked commits (245 plaintext + 376 encrypted) durable, 0
  corruption.
- **dm-flakey power-loss gate (`tests/powerloss_test.sh`).** 5000/5000
  acked commits durable after simulated power loss, with an anti-vacuous
  sentinel confirming the harness catches a lost write. The harness now accepts
  `SDB_POWERLOSS_LIBRARY`, preventing a release check from silently exercising
  a stale hard-coded build directory. Commit `d76f6ab` contains the original
  gate; the configurable-library hardening is in the current tree.
- **Python binding wired into CTest (`python_binding`).** The Python
  binding is back **in-tree** (`python/`) and is now exercised by the
  suite (it had earlier been moved out of tree; see the rc1 entry below).
  The current working tree registers 53 tests, including legacy encrypted
  fixture, index-reclamation compatibility, installed C/C++ consumers, and an
  isolated wheel-install conformance test.
- **Packaging gates are now part of CTest.** The previously dormant external
  install-consumer and Python wheel scripts are registered as serial packaging
  tests. Enabling the gate exposed and fixed a stale `find_package(ShibaDB
  0.1)` requirement after the package version moved to 1.0.

### Portability
- **Windows I/O and locking repaired.** Database identity exclusion now uses a
  file-identity-keyed named semaphore rather than a mandatory whole-file byte
  lock that blocked the pager itself. Atomic replacement uses `ReplaceFileW`,
  compaction closes/reopens the replacement handle as required by Win32, and
  file sharing permits the engine's coordinated secondary handles.
- **Windows filesystem edge cases covered.** UTF-8 absolute paths receive the
  extended-length `\\?\\` / `\\?\\UNC\\` prefix, drive-root parent handling is
  correct, and unsupported directory flushes are handled explicitly. All 48
  cross-built native C tests pass under Wine; MinGW warnings-as-errors and
  native Windows/macOS pull-request gates are now part of CI. Wine evidence is
  supplementary and does not replace the native release attestation.
- **macOS/BSD randomness hardened.** `getentropy(3)` is now the primary CSPRNG
  path on supported Apple/BSD targets, chunked to the API's 256-byte limit.
- **macOS directory durability strengthened.** Parent-directory barriers now
  attempt `F_FULLFSYNC` before falling back to `fsync()` when unsupported.

## [v1.0.0-rc6] — 2026-07-26

Retry of the rc5 tag with four CI fixes surfaced by the first native
matrix run.

### Fixed
- **`src/random.c`**: precede `<bcrypt.h>` with `<windows.h>` so
  `NTSTATUS`/`ULONG`/`LONG` resolve. The current Windows 10.0.26100 SDK
  (MSVC) and mingw-w64 headers both require this ordering under strict
  C11 mode.
- **`CMakeLists.txt` hardening**: guard `-fcf-protection=full` behind
  `CMAKE_SYSTEM_PROCESSOR MATCHES` x86 architectures. Apple Clang on
  ARM64 rejects the flag rather than treating it as a no-op, breaking
  the macOS release build.
- **`CMakeLists.txt` soak wiring**: hoist `enable_testing()` and move
  `test_engine_soak` out of the `SDB_BUILD_TESTS` block into a
  standalone `SDB_ENABLE_SOAK_TESTS` block. The soak test does not use
  `SDB_TESTING` instrumentation, so it now ships as a Release build.
  `-UNDEBUG` applied to the soak target so its `assert()`-based checks
  execute in Release.
- **`release-candidate.yml`**: bump the hard-coded security-review zip
  name from `shibadb-0.1.0-security-review.zip` to
  `shibadb-1.0.0-security-review.zip`, matching `package_release.py`.

## [v1.0.0-rc5] — 2026-07-26

Public-repo hygiene and the first tagged commit that drives native
macOS/Windows release evidence.

### Added
- **README rewritten** as a public-repo landing page: pitch, pre-1.0
  warning, feature bullets, platform matrix, runnable API example,
  quickstart, and per-doc index. No code change.
- **`CODE_OF_CONDUCT.md`** (Contributor Covenant 2.1); enforcement
  routes to the repository's private security-advisory mechanism.
- **`.github/dependabot.yml`** covering `github-actions` (`/`) and
  `pip` (`/python`) on a weekly Monday cadence.
- **`.github/workflows/codeql.yml`** — `c-cpp` static analysis on
  push/PR to main plus a weekly cron, built via the existing `clang`
  CMake preset.
- **`.github/workflows/soak.yml`** — Linux/macOS/Windows encrypted-soak
  matrix on `v*` tags, running the `engine_soak` test with
  `SDB_SOAK_OPERATIONS=10000` and recording signed evidence via
  `scripts/release_evidence.py`. Resolves the `soak.yml` reference
  hard-coded at `scripts/release_audit.py:129-142`.
- **`docs/PROPOSAL_CURSOR_SCAN_ENUMERATE.md`** rev 2 — public
  cursor/scan/enumerate API draft after adversarial review. No
  implementation this release.

### Notes
- This is the first tag driving native macOS/Windows evidence and
  encrypted-soak evidence across all three platforms. It fills two of
  the four remaining `ROADMAP.md` gate items ("native macOS +
  Windows release evidence bound to a tagged commit"). The
  independent cryptography / crash-consistency review remains open.

## [v1.0.0-rc4] — 2026-07-25

Page-cache plaintext-envelope fix.

### Performance
- **Page cache stored ciphertext for encrypted DBs.** Every read of a
  hot page paid the XChaCha20-Poly1305 decrypt cost even on a cache
  hit. The cache now stores the plaintext-envelope page at all three
  populate sites in `src/pager.c`. `PAGER_INVARIANTS.md` C.2/C.4
  rewritten to require plaintext-envelope entries. Verified: `clang`
  preset 44/44 (201s), `sanitize` preset 44/44 (609s, ASan+UBSan
  clean). Commit `af0ad0e`.

## [v1.0.0-rc3] — 2026-07-25

Windows crash-consistency fix.

### Fixed — HIGH severity
- **`sdb_file_sync_parent_directory` on Windows was a no-op.** It
  now opens the parent directory with `CreateFileW` +
  `FILE_FLAG_BACKUP_SEMANTICS` and calls `FlushFileBuffers`, matching
  the POSIX `fsync(parent_fd)` contract. Commit `4035cd1`.

## [v1.0.0-rc2] — 2026-07-25

B-tree split arithmetic fix on 32-bit targets.

### Fixed — HIGH severity
- **32-bit integer overflow in `sdb_btree_split_leaf`.** The size
  calculation for the split point summed `uint32_t` widths that could
  wrap on 32-bit hosts when a single leaf approached the API-permitted
  limits. Intermediate arithmetic now uses `uint64_t` and the split
  is short-circuited with `SDB_E_OVERFLOW` before any wrap can occur.
  `sdb_btree_remove_child` gains a matching bounds check. Commit
  `e00afea`.

## [v1.0.0-rc1] — 2026-07-25

First `v1.0` release candidate. Fold-through of the audit residue plus
release-facing polish. The C ABI is frozen at v1; the on-disk format
is stable for v1.0. The remaining gate to `v1.0.0` proper is native
macOS + Windows CI evidence for the release commit (see `ROADMAP.md`).

### Security-relevant fixes (MEDIUM)
- **`/dev/urandom` fallback hardened.** `sdb_random_bytes_urandom`
  now opens `/dev/urandom` with `O_NOFOLLOW` and `fstat` + `S_ISCHR`
  verification, refusing to hand back bytes from a rogue `/dev`
  (chroot, hostile container, unprivileged user-namespace). Commit
  `869e451`.
- **32-bit LFS forced.** `_FILE_OFFSET_BITS=64` added to POSIX
  compile definitions plus a `_Static_assert(sizeof(off_t) >= 8)` in
  `src/file.c` — 32-bit builds can no longer silently truncate
  `uint64_t` page offsets. Commit `e8cb090`.
- **Plaintext password no longer lives on the DB handle.** The
  password buffer supplied to `sdb_database_create` /
  `sdb_database_open` was memcpy'd onto `database->password` and kept
  for the DB lifetime, even though nothing on that field was ever
  read again. Field and helpers removed entirely; core dumps and
  swap-file dumps no longer disclose the plaintext. Commit `bbe69b5`.
- **Orphan WAL after replace-crash no longer bricks reopen.**
  `sdb_wal_recover` treats a WAL whose `file_id` or `page_size`
  mismatches the current superblock as sweepable (unlink,
  dir-fsync, `SDB_OK`) rather than fatal `SDB_E_CORRUPT` — mirrors
  the existing "structurally-incomplete WAL" branch. Closes the
  crash-inside-`sdb_replace_finish` brick window. Commit `f3c7e85`.
- **WAL recovery verifies the AEAD tag.** `sdb_wal_recover` gains a
  `data_key` parameter; on encrypted databases it decrypts and
  Poly1305-checks each record's page image before installing it.
  Closes the "WORM DB with writable WAL" forgery window and the
  "replay old WAL after password rotation" arm. Commit `8ef027c`.

### Security-relevant fixes (LOW / defense-in-depth)
- POSIX process lock rejects FIFO, socket, and hardlink at the
  sidecar path (`fstat` + `S_ISREG` + `st_nlink == 1`). Commit
  `4ab3c69`.
- `sdb_txn_free` mirrors `sdb_pager_free`'s double-free guard; a
  page whose current on-disk type is `SDB_PAGE_TYPE_FREE` is refused
  before the freelist write, preventing a durable freelist cycle.
  Commit `5631e19`.
- Superblock update boundary refuses to flip `SDB_FLAG_ENCRYPTED`;
  changing the encryption bit without a matching wrap rewrite would
  leave payload envelopes inconsistent with the superblock. Commit
  `5631e19`.
- B-tree read-path traversals gain the same `depth >= 64U →
  SDB_E_CORRUPT` cap the write paths already have. A corrupt parent
  chain with a cycle can no longer spin forever. Commit `5631e19`.
- `sdb_page_cache_destroy` calls `sdb_secure_zero` on the cache
  storage before free. Commit `5056cd0`.
- `sdb_key_unwrap` guard extended to match `sdb_key_wrap`'s three
  precondition checks (`key_wrap_id != 0`, KDF iterations bounded).
  Commit `5056cd0`.
- Leaf-node decode: `node.count` is hoisted before the value-buffer
  allocation so an OOM after key copy but before value copy does not
  leak the key. Commit `5056cd0`.

### Correctness + documentation
- `sdb_index_visit` reentry semantics documented explicitly: the
  visitor yields once per live INDEX ENTRY, not once per DOCUMENT.
  New `tests/test_index_visitor_reentry.c` locks in insert / update
  / delete-during-walk behaviour. Commit `3342611`.
- `docs/NONCE_MODEL.md` (new): formal per-subsystem argument that
  XChaCha20-Poly1305 nonce collision cannot occur under the
  supported threat model. Commit `3342611`.

### Build / distribution
- `SDB_HARDENED` CMake option enabling `-fstack-protector-strong`,
  `-fcf-protection=full`, `-D_FORTIFY_SOURCE=2`, and the ELF linker
  set `-z relro -z now -z noexecstack`. The `release` preset now
  inherits `SDB_BUILD_TESTS=OFF`, `SDB_TESTING=OFF`, `SDB_HARDENED=ON`.
  Verified via `readelf` on the built shared library. Commit
  `337aa00`.

### Deletions
- The Python ctypes binding was relocated out of tree. Language
  bindings live in separate repositories with their own release
  cadence and packaging. Commit `1382d73`.

## [v0.2-rc1] — 2026-07-25

Initial audit fold-through.

### Fixed
- **B-tree reclaim: aliased sibling.** `sdb_btree_reclaim_internal_up`
  rejects an aliased sibling with `SDB_E_CORRUPT` instead of silently
  corrupting the tree when a grandparent's adjacent child slots point
  to the same page. Both the merge and split branches were affected.
  Commit `f1fd3d7`.
- **B-tree reclaim: additional preamble guards.** The same function
  rejects `node_page == 0`, `survivor_page == 0`,
  `node_depth_in_path == 0`, and `parent_page == node_page` at the
  preamble with `SDB_E_CORRUPT`. Diagnostic-only; no happy-path
  change. Commit `9089600`.
- **Fuzz coverage.** `shibadb_core` compiles with SanitizerCoverage
  instrumentation under the `fuzz` preset. Prior builds ran libFuzzer
  with `cov=1` for billions of runs; after the fix a 60s superblock
  run climbs to `cov=42, ft=51`, discovering new coverage on six
  seed inputs. Commit `8def834`.
- **Fuzz `fuzz_page` target.** The previous `size == 4096` filter
  meant libFuzzer had no growth path from an empty seed corpus. The
  target now selects a `page_size` from the first byte and
  pads/truncates the remainder. Result: `cov 2 → 70`, corpus 0 → 15
  useful inputs. Commit `c15efee`.

### Changed
- **Superblock decode diagnostics.** New private
  `sdb_validate_superblock_detailed` helper returns an `sdb_sb_field`
  enum naming the first failing field. `sdb_validate_superblock` and
  the public status enum are unchanged. New test binary
  `test_superblock_field_diag` covers 11 field-level failure classes.
  Commit `8b4ff10`.
- **Release build hardening.** New CMake option `SDB_TESTING`
  (defaults to `SDB_BUILD_TESTS`). With `-DSDB_TESTING=OFF`, the
  `sdb_file_*_for_testing`, `sdb_wal_*_for_testing`, and
  `sdb_engine_*_for_testing` symbols vanish from the built library
  (verified via `nm` on `libshibadb_core.a`). The `sdb_file` struct
  also loses its `io_limit`, `test_operation_count`, and
  `test_fail_after` fields; the hook-check helper folds to a
  constant-false inline that the optimizer removes. Commit `f9f1dc8`.

### Documentation
- **`.replace.tmp` lifecycle.** Expanded comment near
  `sdb_replace_prepare:112` explaining the crash-safety contract:
  best-effort `tmp` unlink, `O_CREAT|O_EXCL` guarantees on the next
  prepare, and the recovery-pass sweep. Commit `b4cf414`.
- **`PAGER_INVARIANTS` T.4.** Invariant now names the
  alloc-then-free graceful-cancel semantics. Commit `cd8672d`.
- **`OPERATIONS` close semantics.** New section distinguishing
  `SDB_E_IO` (pager-drain failure, treat as pre-commit crash) from
  `SDB_E_LOCK` (drain OK, lock-release failure, cleaned up by the
  next open). Commit `cd8672d`.

### Infrastructure
- `.serena/` metadata (LSP project cache) is gitignored.
  Commit `879d7ee`.
- Seed corpora added for all four fuzz targets, growing corpus totals
  from 5 → 82 inputs across `fuzz_superblock`, `fuzz_page`,
  `fuzz_btree_page`, and `fuzz_xchacha20poly1305`.

## [Baseline] — cbf47ca (2026-07-24)

Codebase snapshot at the start of the hardening pass, after the
initial audit-fix session applied 22 fixes across nine subsystems.
See [`docs/AUDIT.md`](docs/AUDIT.md) for the detailed rollup.

- 40 tests passing under both `clang` and `sanitize` presets.
- ~11.4 kLoC of C across `src/`.
- 4 libFuzzer targets available; only `fuzz_superblock` had a seed
  corpus (5 inputs).
- ABI: 594 exported symbols in `libshibadb_core.a`.
