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
