# Independent security review scope

This document defines the minimum independent review required before ShibaDB
may claim 100% production readiness. A review of wrappers alone is not
sufficient.

## Cryptography

Reviewers must examine:

- SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, HChaCha20, ChaCha20, Poly1305,
  and XChaCha20-Poly1305 implementations;
- operating-system random-number acquisition and nonce generation;
- password-derived key wrapping, file identity binding, rotation, and KDF
  bounds/defaults;
- page associated data, authentication-before-plaintext behavior,
  constant-time tag comparison, and secret zeroization;
- encrypted WAL images and the absence of logical plaintext from persistent
  database/WAL storage.

The review must state whether every standard vector used by the tests was
independently traced to its normative source. Side-channel claims beyond
constant-time tag comparison are out of scope unless the report explicitly
adds them.

## Crash consistency

Reviewers must trace:

- WAL v1/v2 parsing, body sync, commit marker sync, database-page apply,
  database sync, mirrored checkpoint, and WAL removal;
- transactional page allocation/freelist targets and all-or-none recovery;
- B+Tree split staging and public multi-operation transaction atomicity;
- file-identity checks preventing replay into another database;
- backup/compact replacement markers, parent-directory durability barriers,
  stale-WAL cleanup, and process-death recovery;
- POSIX and Windows file replacement/locking semantics.

The required invariant is: after any process death or injected I/O failure,
reopen exposes either the complete state before a logical transaction or its
complete committed state, never a prefix assembled from both.

## Evidence expected from the reviewer

The report must identify the exact 40-character commit, bundle SHA-256,
reviewer and organization, independence declaration, reviewed files,
methodology, findings and disposition. It must cover both `cryptography` and
`crash-consistency` in the machine-readable evidence described by
`RELEASE_EVIDENCE.md`.

The deterministic security-review bundle contains a manifest with the SHA-256
of every scoped source, test, fuzz target, and design document. Reviewers
should verify that manifest before beginning work and cite its digest in their
report.

## Reproduction commands

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error

cmake -S . -B build-asan -G Ninja \
  -DSDB_ENABLE_SANITIZERS=ON -DCMAKE_C_COMPILER=clang
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure --no-tests=error

python scripts/check_reproducible.py
python scripts/release_audit.py --library build/libshibadb.so.0.1.0
```
