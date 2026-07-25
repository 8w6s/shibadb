# Production-readiness roadmap

Progress is earned only when the capability exists **and** its reliability
gate passes. Lines of code and partially implemented features do not count.

| Area | Weight | Done | Exit gate |
|---|---:|---:|---|
| Format primitives and strict parsers | 3% | 3% | GCC/Clang, ASan/UBSan, parser fuzz |
| Portable file I/O and mirrored superblock | 7% | 7% | short I/O, fsync, torn-write recovery |
| Pager, cache, allocator and freelist | 12% | 12% | reopen/property/fault tests |
| WAL and atomic page transactions | 12% | 12% | crash matrix at every write boundary |
| Public multi-operation transactions | 6% | 6% | commit/rollback/isolation + crash matrix |
| B+Tree and ordered cursors | 17% | 17% | differential/property/fuzz tests |
| Encryption and key lifecycle | 12% | 12% | tamper, nonce, rotation and audit tests |
| KV, documents, blobs and indexes | 11% | 11% | public conformance suite |
| Concurrency and process locking | 7% | 7% | multi-thread/process soak tests |
| Backup, verify, compact and migration | 6% | 6% | failure-safe operational tests |
| Stable C ABI and language bindings | 3% | 3% | ABI compatibility + install-consumer test |
| Cross-platform hardening and release gates | 4% | 2% | Linux/macOS/Windows CI and long soak |
| **Total** | **100%** | **98%** | |

Current status: durable metadata, checked pages, bounded cache, persistent
allocation, atomic transactions, a variable-length B+Tree, authenticated
page/WAL encryption (with recovery-time tag verification), chunked KV/blob
values, opaque documents, and explicit secondary/unique indexes, serialized
shared-handle thread safety with hardened POSIX lock-sidecar (S_ISREG +
nlink==1 checks), crash-released exclusive process locking, deep
verification, atomic snapshots, logical compaction, explicit migration, a
versioned C ABI with Doxygen-documented public headers, installable
static/shared packages with distribution hardening (RELRO, BIND_NOW, CET,
stack canaries, FORTIFY_SOURCE), and MIT licensing. Language bindings live
in separate repositories following the SQLite adoption model. The Linux
release gate has deterministic packaging/auditing and a passing
10,000-operation encrypted reopen/verify/backup/compact soak.

Cross-platform hardening is 2/4 (up from 0/4): the CMake matrix for
Linux GCC/Clang, macOS Clang, and Windows MSVC exists and is set up in
`.github/workflows/ci.yml`; the release-candidate workflow emits
commit-bound evidence with signed provenance for each platform. The
remaining 2% is awarded when the native macOS and Windows runs actually
land — i.e. once the repository is public and the first `v1.0.0-rc*`
tag triggers all three OS jobs successfully.

Not yet fully production-ready: **the native macOS and Windows release
evidence for a tagged commit**, and **independent cryptography /
crash-consistency review**. The MIT license decision is settled (see
`LICENSE`); the audit residue from the initial adversarial round is
closed (see `docs/AUDIT.md` and the resolution commits
between `v0.2-rc1` and `v1.0.0-rc1`).
