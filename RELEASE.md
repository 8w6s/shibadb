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

## Current local evidence

The Linux Release build has passed the complete 44-test non-soak suite and a
deterministic 10,000-operation encrypted soak with reopen, deep verification,
backup/snapshot verification, and compaction. The workload exposed an
order-dependent leaf-split failure at operation 6497; after object chunks were
bounded to half of the usable leaf payload, the same workload passed in full.
The affected B+Tree/engine suites also pass under ASan/UBSan.
Clang Static Analyzer reports no findings across the complete C core.

This local evidence does not substitute for native macOS/Windows runs or an
independent security review.
