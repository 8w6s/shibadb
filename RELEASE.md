# Release gate

A production release requires evidence, not only workflow configuration:

- GCC and Clang warnings-as-errors;
- public multi-operation commit/rollback with crash-atomic recovery;
- ASan/UBSan and ThreadSanitizer;
- ABI layout and shared-symbol allowlist;
- static/shared external consumer and Python wheel installation;
- Linux, macOS, and Windows native build plus complete test suite;
- deterministic source archives, checksums, and SPDX manifest;
- long encrypted soak with reopen, verify, backup, and compact;
- clean dependency/symbol audit;
- project owner selection of a distribution license;
- independent review of cryptography and crash consistency.

The current readiness percentage must remain below 100% until every item has
current release evidence.

Native and soak evidence must match the release commit, bind the attested
CTest JUnit report and positive test count, and pass GitHub
Artifact Attestation verification against the expected signer workflow.
Independent audit evidence must identify the reviewer, report, report hash,
review-bundle hash, commit, and required cryptography/crash-consistency scope.
The exact formats
and strict-audit command are documented in
[`docs/RELEASE_EVIDENCE.md`](docs/RELEASE_EVIDENCE.md).

## Baseline tagged-commit evidence

The last recorded baseline Linux Release build passed its complete 47-test
CTest suite (including
`group_commit`, `mixed_workload`, `compact_encryption`, `crash_injection`,
`encryption_fault`, and `python_binding`) plus a battery of durability
evidence:

- a model-verified 500,000-operation encrypted soak with reopen, deep
  verification, backup/snapshot verification, and compaction;
- SIGKILL crash-injection (`crash_injection`): across 24 SIGKILLs mid-commit,
  621 acked commits (245 plaintext + 376 encrypted) were durable with 0
  corruption;
- a dm-flakey power-loss gate (`tests/powerloss_test.sh`): 5000/5000 acked
  commits durable, with an anti-vacuous sentinel confirming the harness would
  have caught a lost write;
- a ~1 GB encrypted dataset (16000 × 64 KB) intact after reopen.

The affected B+Tree/engine suites also passed under ASan/UBSan; ThreadSanitizer
reports 0 races and ASan 0 leaks; 7 libFuzzer targets are clean. Clang Static
Analyzer reports no findings across the complete C core.

The current candidate contains later WAL v5, authenticated-superblock, and
reclamation changes and registers 53 tests. Its local tests are useful
development evidence, but the baseline soak, dm-flakey, TSan, analyzer, and
release artifacts above must be regenerated from the exact candidate commit
before release. Neither set of local evidence substitutes for native
macOS/Windows runs or an independent security review.

On 2026-08-02, a fresh GCC 16 Release build with distribution hardening and
test instrumentation enabled passed the 51 core/runtime tests sequentially in
694.68 seconds. This includes encryption fault injection, SIGKILL crash
injection, compact/migration encryption, Python binding coverage, and the
historical encrypted fixture. The two packaging tests subsequently passed: an
installed static/shared/C++ consumer and an isolated Python wheel install. A
second production-shaped Release build (`SDB_TESTING=OFF`) compiled cleanly,
installed successfully, ran all three external consumers, and exported no
test-hook symbols. The four concurrency/group-commit tests also passed under
ThreadSanitizer in 39.04 seconds. The tested source was subsequently frozen in
the candidate commit containing this record, but this remains local development
evidence and deliberately is not presented as signed release attestation.

Clang 22 Static Analyzer initially reported an uninitialized checkpoint status
and one compact cleanup dead store. Both were corrected; a clean rebuild of the
complete C core then reported no findings.

The production-shaped encrypted soak passed 10,000 deterministic operations in
357.48 seconds, including periodic verify/reopen, backup verification, and
compaction. A dm-flakey run against that exact `build-soak` shared library then
verified 5,000/5,000 acknowledged encrypted commits after dropped writes,
SIGKILL, page-cache loss, unmount, and remount; its post-cut sentinel was absent,
so the result was non-vacuous. The mapper, loop device, and temporary work tree
were removed by the harness. These remain local results rather than signed
workflow attestation.

After the analyzer fixes and packaging-gate wiring, the complete hardened
Release suite was rerun sequentially: all 53/53 tests passed in 749.74 seconds.

The subsequent portability candidate repaired Windows identity locking,
atomic replacement/compaction handle sequencing, directory synchronization,
and extended-length drive/UNC paths. MinGW compiled all Windows targets with
warnings as errors, and all 48 applicable native C tests passed sequentially
under Wine in 976.09 seconds. A separate end-to-end test created, wrote,
reopened, and removed a database whose absolute path exceeded 260 characters.
After the portability changes, the hardened Linux suite again passed 53/53 in
784.53 seconds, including packaging. These are local development results;
native signed Windows/macOS runs from the frozen commit are still mandatory.

## Current suite state — 2026-10-01

The paragraphs above are historical evidence records and are deliberately left
as written: each describes what was run, on what build, at what date, and
rewriting those numbers would falsify the record. This section is the
current-state counterpart, so a reader does not have to guess which count is
live.

The build registers **71 CTest cases**. Verified on Linux, GCC 12.2, 2 cores:

- `Release`: **71/71 PASS** (58.5 s).
- GCC `Debug` with `-DSDB_ENABLE_SANITIZERS=ON` (ASan+UBSan): **70/70 PASS**
  (253 s). The count differs by one because `release_audit` is intentionally not
  registered for instrumented builds.

Two release gates named in the checklist at the top of this file existed as
scripts but were wired into nothing, so they were reported as satisfied while
never running. Both are now registered CTest cases:

- `abi_symbols` — compares the shared library's exported symbol set against
  `abi/symbols-v1.txt` in both directions. Wiring it up immediately showed the
  allowlist had gone stale by 20 symbols: the whole cursor/scan/snapshot slice
  plus `sdb_database_migrate_file`, `sdb_list_namespaces` and
  `sdb_version_number` were exported but unlisted. The list is now 72 symbols
  and matches.
- `release_reproducible` — runs `scripts/check_reproducible.py`, packaging the
  source tree twice with a pinned epoch and comparing bytes.
- `release_audit` — runs `scripts/release_audit.py` non-strict: forbidden
  dependency scan, LICENSE present, symbol allowlist, `DT_NEEDED` limited to
  libc. Registered only for production-shaped builds; under ASan the library
  legitimately gains `libasan.so.N` and `libubsan.so.N`, so the dependency
  assertion would be reporting the truth about the wrong artifact.

Strict-mode `release_audit` still requires commit-bound native and soak evidence
plus `gh attestation verification`, and therefore still only runs at release
time per [`docs/RELEASE_EVIDENCE.md`](docs/RELEASE_EVIDENCE.md). That
requirement is unchanged and unmet.

### Gates that remain manual

`tests/powerloss_test.sh` (dm-flakey power-loss) and `tests/powerloss_helper.c`
are **not** registered in CTest and run in no workflow. They need root, a loop
device and device-mapper, so they cannot run on a hosted runner. The
5000/5000-acked-commits result quoted above is therefore reproducible only by
hand, on a Linux host, by an operator with privileges. Until that run is
re-executed against the exact candidate commit and its evidence recorded in the
format `docs/RELEASE_EVIDENCE.md` specifies, treat the power-loss claim as
unverified at this commit even though the harness exists.

Likewise unchanged and still blocking: native macOS and Windows release
evidence bound to a tagged commit, and independent cryptography /
crash-consistency review.
