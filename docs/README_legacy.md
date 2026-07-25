# ShibaDB C Core

An embedded key–value database in portable C11. ShibaDB gives you an
ACID-safe B+Tree with a WAL, page cache, encrypted-at-rest storage, and
crash-consistent backup/compact, in one static library with no runtime
dependencies. The kernel is what a persistent embedded store looks like
when you keep every layer separately testable: parsers first, then the
pager, WAL, B+Tree, transactions, encryption, and finally the higher-level
document/index/backup engine.

Licensed under the MIT license — see [`LICENSE`](LICENSE).

Following the SQLite adoption model, the C core is the only artifact
this repository releases. Language bindings (Python, Go, Rust, …) live in
separate repositories with their own release cadence and packaging.

Status: **`v1.0.0-rc1`** — release candidate. The C ABI is frozen at
version 1; the on-disk file format is stable and 1.0-locked. Post-
adversarial-audit hardening pass is complete (see the audit residue
closure between `v0.2-rc1` and `v1.0.0-rc1` in `CHANGELOG.md`).
The remaining gate to `v1.0.0` proper is native macOS + Windows CI
evidence for a tagged commit and an independent cryptography /
crash-consistency review.

Overall production-readiness progress: **98%**. See
[`ROADMAP.md`](ROADMAP.md) for the fixed, evidence-based scoring model
and [`CONTRIBUTING.md`](CONTRIBUTING.md) for how to submit a change.

## Quick start

```sh
cmake --preset clang && cmake --build build-clang && (cd build-clang && ctest -j4)
```

That produces `libshibadb_core.a` (static, hidden visibility) and
`libshibadb.so.0.1.0` (shared, versioned) under `build-clang/`, and runs
the full 44-test suite including the sanitize-clean crash matrix. For a
stripped release build with no test hooks compiled in, add
`-DSDB_BUILD_TESTS=OFF -DSDB_TESTING=OFF`.

## Auditing this codebase

Every internal invariant is written down. Before modifying the storage
layers, read [`docs/PAGER_INVARIANTS.md`](docs/PAGER_INVARIANTS.md) — it
records ~40 formal invariants cross-referenced to enforcing source lines.

For a recent adversarial-audit trail, see
[`docs/AUDIT.md`](docs/AUDIT.md): 98 findings across nine
subsystems, of which 22 were resolved with atomic test-first commits.
The residue is documented per-severity, so a new reader knows what has
been ruled out and what remains open.

## Milestone 1

- Stable status-code contract.
- Fixed-width little-endian codec helpers.
- Checked size arithmetic.
- CRC32.
- Strict, versioned superblock encoder/decoder.
- Unit tests and a libFuzzer entrypoint.
- GCC/Clang warnings-as-errors and optional ASan/UBSan.

## Milestone 2

- Full positional read/write loops with deterministic short-I/O testing.
- Explicit fsync barriers and parent-directory fsync on POSIX creation.
- Two fixed 4 KiB superblock slots.
- Highest-valid-generation recovery.
- Split-brain rejection when equal generations disagree.
- Recovery from a corrupt, truncated, or partially written mirror.
- Failure injection at every mirrored-update I/O boundary.

## Milestone 3

- Checksummed, self-identifying fixed-size pages with strict zero padding.
- Persistent allocation watermark and on-disk LIFO freelist.
- Bounded 64-page LRU cache with deterministic eviction.
- Page reuse, double-free rejection, and allocation initialization.
- Deterministic allocate/free/reopen property test.
- Recovery tests cutting allocation at successive file-I/O boundaries.
- GCC, Clang, and ASan/UBSan gates across the complete test suite.

## Milestone 4

- Versioned, checksummed redo WAL tied to the database file identity.
- WAL body and commit marker separated by explicit sync barriers.
- Atomic multi-page transactions with commit and abort.
- Idempotent recovery and mirrored-superblock checkpoints.
- Single active transaction per pager; unsafe direct mutations are blocked.
- Validation of page bounds, page LSNs, duplicate records, and reserved bytes.
- A 256 MiB WAL resource ceiling is enforced before allocation; committed WAL
  records are fully validated with O(n) duplicate detection before any page
  is applied, preventing corrupt-WAL partial replay.
- Crash matrix across every WAL, database apply, and checkpoint boundary.
- WAL creation includes parent-directory fsync on POSIX.

The normative layout and durability sequence are documented in
[`docs/WAL_FORMAT.md`](docs/WAL_FORMAT.md).

## Milestone 5

- Variable-length binary keys and values.
- Multi-level B+Tree lookup, insert, replacement, and deletion.
- Recursive leaf/internal splits and stable-root splitting.
- Atomic WAL commit of every page changed by a split.
- Forward-linked leaves and ordered first/seek/next cursors.
- Strict node parser with sorted-key, child-ID, bounds, and reserved-byte
  validation.
- Reopen test with 600 shuffled large records that forces multi-level splits.
- Model-based differential test across random put/update/delete/get operations.
- Parser round-trip fuzzing; the campaign found and fixed a corrupted-input
  cleanup leak, then passed 300,000 ASan/UBSan executions.

The node layout is documented in
[`docs/BTREE_FORMAT.md`](docs/BTREE_FORMAT.md).

## Milestone 6

- Dependency-free SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, ChaCha20,
  HChaCha20, Poly1305, and XChaCha20-Poly1305 C implementations.
- Byte-for-byte SHA/HMAC/PBKDF2/HChaCha/XChaCha standard vectors.
- Random 256-bit data keys sourced from the operating-system CSPRNG.
- Authenticated encryption of data, freelist, B+Tree, and WAL page images.
- Password-derived key wrapping and atomic password/KDF rotation.
- 600,000-iteration PBKDF2-HMAC-SHA256 creation/rekey default and
  enforced lower bound, matching current OWASP guidance.
- Constant-time tag comparison and key/staged-plaintext zeroization.
- Wrong-key, plaintext-absence, nonce-uniqueness, ciphertext-tamper, rotation,
  and interrupted-rotation tests.
- 200,000 malformed-ciphertext fuzz executions requiring no plaintext output
  before successful authentication.

The threat model and key hierarchy are documented in
[`docs/ENCRYPTION.md`](docs/ENCRYPTION.md).

## Milestone 7

- Experimental dependency-free database lifecycle API.
- Binary KV and blob values split into bounded chunks with SHA-256 integrity.
- Generation-based, commit-last visibility across interrupted large updates.
- Opaque binary documents with application-supplied index terms.
- Secondary and unique indexes with stale-generation filtering.
- Encrypted and unencrypted public API conformance tests.
- Model-based random KV/reopen testing.
- Crash injection at 80 write boundaries during a multi-chunk replacement.
- Full GCC, Clang, and ASan/UBSan gates.

The contracts and current limitations are documented in
[`docs/ENGINE_API.md`](docs/ENGINE_API.md).

## Milestone 8

- Recursive per-handle mutex serializes every public database operation.
- Canonical absolute paths and an exclusive database-file identity lock span
  open, WAL recovery, and close. A second canonical-path lock keeps atomic
  replacement serialized while inode identity changes; relative and symbolic
  aliases cannot bypass either writer lock or destination backup lock.
- Hardlinked database files are rejected because they cannot name one unique
  WAL/replacement sidecar.
- Explicit `SDB_E_BUSY` result for a competing handle or process.
- Kernel lock recovery after forced process death.
- Four-thread shared-handle put/get/reopen soak.
- Four-process lock-handoff and durable-record soak.
- GCC, Clang, ASan/UBSan, and ThreadSanitizer gates.

The ownership and deployment contract is documented in
[`docs/CONCURRENCY.md`](docs/CONCURRENCY.md).

## Milestone 9

- Deep physical, allocator, B+Tree, object, digest, and index verification.
- Consistent physical snapshots installed by synchronized atomic replacement.
- File-identity replacement marker for crash-safe stale-WAL cleanup.
- Logical compaction that removes stale generations and reduces the file.
- Version-gated migration with page-size and encryption/password changes.
- Backup reliability matrix cuts every destination chunk write/final `fsync`
  and kills the process around marker, replacement, and directory-sync phases;
  every reopen must yield a complete verified old or new destination.
- Process termination at three compact replacement phases.
- Plaintext and encrypted conformance plus GCC, Clang, and ASan/UBSan gates.

The operational contracts are documented in
[`docs/OPERATIONS.md`](docs/OPERATIONS.md).

## Milestone 10

- Frozen C ABI version 1 with runtime version negotiation.
- Hidden-by-default shared library with an exact public symbol allowlist.
- `SOVERSION 1`, public layout assertions, and ABI regression tests.
- Installable static/shared libraries and public headers.
- Relocatable CMake package targets and pkg-config metadata.
- External consumer configuration, compilation, linking, and execution.

The compatibility contract is documented in
[`docs/ABI.md`](docs/ABI.md). Language bindings are out of scope for
this repository — see the "SQLite adoption model" note in the
[status](#shibadb-c-core) block above.

## Milestone 11 (in progress)

- Linux Release CI/release-candidate workflows and CMake presets for GCC,
  Clang, ASan/UBSan, ThreadSanitizer, macOS Clang, and Windows MSVC.
- Deterministic source archives, SHA-256 checksums, and SPDX 2.3 manifest.
- Automated ABI symbol, forbidden-dependency, and native-library audit.
- Commit-bound native/soak evidence with GitHub/Sigstore provenance
  attestations and strict signer-workflow verification.
- Clang Static Analyzer gate across the complete C core.
- Release tests keep assertions enabled even when the library uses `-O3`.
- Deterministic encrypted soak covering reopen, deep verify, backup snapshots,
  and logical compaction.
- A 10,000-operation soak found and fixed an order-dependent B+Tree leaf split
  failure by bounding object chunks to half of the usable leaf payload.
- Static analysis found and fixed an uninitialized recursive split result on a
  B+Tree read-error path; the complete core now has a clean analyzer run.
- Reentrant index visitors now close and reseek their cursor around callbacks,
  allowing bounded-memory traversal across recursive mutation, compaction, or
  migration; recursive close is rejected with `SDB_E_BUSY`.
- The fixed workload passed all 10,000 operations, the 44-test Linux Release
  gate, and affected ASan/UBSan regression tests.

Still required before this milestone earns its roadmap weight: native macOS and
Windows evidence, and independent cryptography/crash-consistency review. The
distribution license is now settled at MIT (see [`LICENSE`](LICENSE)). See
[`RELEASE.md`](RELEASE.md) and
[`docs/RELEASE_EVIDENCE.md`](docs/RELEASE_EVIDENCE.md).

## Milestone 12 (completed)

- Public multi-operation transactions with begin, read-your-writes, commit,
  rollback, and explicit handle close.
- One atomic WAL commit spanning KV, blob, document, and index mutations.
- Transaction-level unique-index validation and deterministic resource limits.
- Deterministic transaction limits: 10,000 operations and 64 MiB of logical
  mutation input.
- Multi-record model, fault-injection, process-death, sanitizer, concurrency,
  static-analysis, and ABI gates passed.
- Internal foundation completed: reusable B+Tree mutation batches, staged
  reads, multi-put/delete, one WAL commit, and transactional allocation.
- WAL v2 checkpoints the target allocation watermark/freelist atomically while
  retaining WAL v1 recovery compatibility.
- High-level KV, blob, and document mutations now share one internal engine
  transaction per call; document chunks, metadata, index entries, and unique
  guards can no longer commit as separate WAL units.

The semantics, lifecycle, and reliability evidence are documented in
[`docs/PUBLIC_TRANSACTIONS.md`](docs/PUBLIC_TRANSACTIONS.md). Completion of the
public transaction gate raises the evidence-based readiness score from 90% to
96%. The remaining four points require native macOS/Windows release evidence
and the outstanding external release-governance evidence listed above.

## Milestone 13 (in progress)

- Windows filesystem and process-lock paths now accept strict UTF-8 and use
  native UTF-16 Win32 APIs instead of the locale-dependent ANSI API.
- A cross-platform UTF-8 database path create/write/reopen test is part of the
  complete native suite.
- Native/soak evidence schema v2 records runner architecture, positive CTest
  count, and the SHA-256 of its CTest JUnit report.
- Strict audit verifies that the report beside each evidence JSON matches both
  the attested digest and test count.
- Release and soak workflows attest and upload the report together with its
  commit-bound evidence.
- New encrypted databases and rewrites default to 600,000
  PBKDF2-HMAC-SHA256 iterations; this same value is now the enforced
  lower bound on every open, matching the current OWASP recommendation.
  Pre-2026-07 files with lower stored work factors must be migrated via
  password rotation before opening under a current build.
- C create/open/transaction-begin functions deterministically clear supplied
  output handles on failure and reject empty database paths without filesystem
  side effects.
- Every source release now contains a deterministic security-review ZIP with
  the exact crypto/crash-consistency scope, entry points, commit, and SHA-256
  manifest for all reviewed files.
- Release CI verifies the bundle before attestation; independent-audit
  evidence must bind both the report hash and the exact bundle hash.
- Deep verification now proves exclusive physical-page ownership: every page
  must be reachable from either the B+Tree or the freelist, so a checksummed
  but orphaned DATA page is reported as corruption.
- Open now reconciles the committed allocation watermark with physical file
  length after WAL recovery: truncated images fail before verification-state
  allocation, while abandoned trailing allocation bytes are truncated and
  synchronized away.
- B+Tree leaf pages that become empty on delete are now transactionally
  reclaimed to the freelist. When a delete empties a non-root leaf, the same
  WAL commit unlinks the leaf from its parent, rewrites the leaf's predecessor
  in cursor order to skip the freed page, bypasses parent internals that
  collapse to a single child, and shrinks the root when the tree height
  drops. A dedicated churn/reopen test proves the allocation watermark
  survives repeated delete/reinsert cycles at a fixed page budget.

These improvements prepare the remaining native gate but do not substitute for
actual macOS and Windows runs, so readiness remains 96%.

## Build and test

Use the CMake presets shipped with the project. Each preset creates a
sibling `build-<name>` directory so multiple configurations can coexist.

```sh
# Default developer build (GCC, RelWithDebInfo, all tests):
cmake --preset gcc
cmake --build --preset gcc
ctest --preset gcc

# AddressSanitizer + UndefinedBehaviorSanitizer (Clang):
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize

# ThreadSanitizer (Clang) — recommended for concurrency tests:
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -R engine_concurrency

# libFuzzer targets (Clang, requires ASan compatibility):
cmake --preset fuzz
cmake --build --preset fuzz
./build-fuzz/fuzz_superblock -max_total_time=60

# Long-running soak test (Release):
cmake --preset soak
cmake --build --preset soak
ctest --preset soak
```

Available presets: `gcc`, `clang`, `release`, `sanitize`, `tsan`, `fuzz`,
`soak`, `coverage`. Run `cmake --list-presets` to list them.

## Coverage

The `coverage` preset instruments every compilation unit with
`-fprofile-instr-generate -fcoverage-mapping` and directs profraw output
to `build-coverage/profiles/`. Basic workflow:

```sh
cmake --preset coverage
cmake --build build-coverage
mkdir -p build-coverage/profiles
ctest --preset coverage -j4

# Merge and report:
find build-coverage/profiles -name '*.profraw' \
    | xargs llvm-profdata merge -sparse -o build-coverage/merged.profdata
llvm-cov report build-coverage/libshibadb_core.a \
    $(find build-coverage -maxdepth 1 -name 'test_*' -executable) \
    -instr-profile=build-coverage/merged.profdata > build-coverage/summary.txt
```

Latest coverage delta is recorded in
[`docs/COVERAGE.md`](docs/COVERAGE.md) when a session
produces one.

## Continuous fuzzing

The project is fuzzed continuously by [ClusterFuzzLite](https://google.github.io/clusterfuzzlite/),
reusing the OSS-Fuzz toolchain (libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer)
inside GitHub Actions.

- `.github/workflows/cflite_pr.yml` — every pull request touching `src/`,
  `include/`, `fuzz/`, `CMakeLists.txt`, or the `.clusterfuzzlite/` config
  runs each fuzz target for 5 minutes under ASan and UBSan against a corpus
  of previously-seen inputs. A crash blocks the PR and uploads the failing
  input as a workflow artifact.
- `.github/workflows/cflite_batch.yml` — every 6 hours the fuzzers run for
  30 minutes each on `HEAD`, growing the shared corpus.
- `.github/workflows/cflite_build.yml` — every push to `main` produces a
  sanitizer build and stores it as an artifact so PR fuzzing can tell
  which crashes are new.
- `.github/workflows/cflite_cron.yml` — nightly corpus pruning (drops
  redundant inputs) and coverage report.

Fuzz target sources and their seed corpora live under `fuzz/`. The
integration files live under `.clusterfuzzlite/` (project.yaml,
Dockerfile, build.sh). To reproduce a CI failure locally, clone
[oss-fuzz](https://github.com/google/oss-fuzz) once and run:

```sh
cd path/to/oss-fuzz
python3 infra/helper.py build_image --external path/to/shibadb-c
python3 infra/helper.py build_fuzzers --external \
    --sanitizer address path/to/shibadb-c
python3 infra/helper.py run_fuzzer --external \
    path/to/shibadb-c fuzz_xchacha20poly1305 -- -max_total_time=60
```

For faster local iteration without Docker, use the `fuzz` preset instead
(see the Build and test section above).

## Reviewing storage changes

Before modifying `src/pager.c`, `src/wal.c`, `src/superblock_store.c`, or
their supporting headers, read [`docs/PAGER_INVARIANTS.md`](docs/PAGER_INVARIANTS.md).
It records the ~40 formal invariants — file layout, superblock mirroring,
WAL ordering, transaction atomicity, cache coherence, encryption AAD
bindings, recovery — that every code path in the storage layers must
preserve, each cross-referenced to the enforcing line of source. Any
change touching those files should be able to point to the invariant it
preserves or the invariant it deliberately updates.

The C ABI is frozen at version 1. The pre-1.0 file-format compatibility promise
remains experimental until the release-hardening milestone is complete.
