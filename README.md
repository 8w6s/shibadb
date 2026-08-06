# ShibaDB

Embedded ACID key-value database in C11 — optional authenticated encryption, single-file,
crash-safe, ~15k LOC, MIT-licensed.

[![CI](https://github.com/8w6s/shibadb/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/8w6s/shibadb/actions/workflows/ci.yml)
[![Release candidate](https://github.com/8w6s/shibadb/actions/workflows/release-candidate.yml/badge.svg?branch=main)](https://github.com/8w6s/shibadb/actions/workflows/release-candidate.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

> **Pre-1.0 / release candidate.** shibadb-c is 98% ready per
> [ROADMAP.md](ROADMAP.md). Crash consistency now has direct evidence —
> SIGKILL crash-injection and a dm-flakey power-loss gate (see
> [RELEASE.md](RELEASE.md)) — but an *independent* cryptography /
> crash-consistency audit has **not** happened yet, and signed native
> macOS/Windows release evidence is still pending (see
> [docs/PORTABILITY.md](docs/PORTABILITY.md)). **Do not use for production data
> yet.** See [SECURITY.md](SECURITY.md) and [RELEASE.md](RELEASE.md) for the
> remaining gates.

The public ABI and on-disk format use the `1.0.0` version number as their
compatibility target. That number is not a production-readiness claim: until
the release gates above are complete, published builds remain release
candidates.

## What it is

ShibaDB is an embedded ACID key-value database written in portable C11. It
links into your application as a static or shared library — no server, no
network, no separate daemon. One database is one file (plus a WAL sidecar
during writes).

- **ACID transactions** over a write-ahead log with atomic page commit,
  a persistent WAL file descriptor, and single-fsync leader/follower group
  commit that coalesces concurrent committers into one durability barrier.
- **Optional authenticated encryption**: supply a non-empty database password
  to enable XChaCha20-Poly1305 page and WAL envelopes, PBKDF2-HMAC-SHA256 key
  derivation with a 600k-iteration OWASP floor, authenticated superblock
  control fields, tag verification at recovery time, and CRC32 (slice-by-8)
  page integrity checks. Passwordless creation is intentionally plaintext and
  provides damage detection, not authenticity.
- **Serialized shared handles**: one mutex per handle, one process per
  database path, `SDB_E_BUSY` on contention. See [docs/CONCURRENCY.md](docs/CONCURRENCY.md).
- **Storage primitives**: variable-length B+Tree, chunked KV values, blob
  namespace, opaque documents with explicit secondary/unique indexes.
- **Operational tooling**: deep `verify`, atomic snapshot `backup`, logical
  `compact`, explicit `migrate` (password rotation, page-size change).
- **Stable C ABI**: `SDB_ABI_VERSION = 1`, semver-tagged, versioned headers,
  CMake `find_package(shibadb)` and pkg-config integration.
- **Ergonomic convenience API**: allocating reads (`sdb_kv_get_alloc`),
  `sdb_kv_exists` / `sdb_kv_count`, atomic `put_if_absent` /
  `compare_and_swap` / `increment` counters, one-call atomic batches, and
  `sdb_index_query_documents` (find-by-field returning document bodies). See
  [examples/quickstart.c](examples/quickstart.c).
- **Native CLI**: a dependency-free `shibadb` binary for KV/document access,
  `find`, `verify`, `backup`, `compact`, and scripting.

## Platform matrix

| Platform | Compiler           | Status                                        |
|----------|--------------------|-----------------------------------------------|
| Linux    | GCC 12+, Clang 15+ | Verified — full CTest, 500k-op encrypted soak, crash-injection, dm-flakey power-loss |
| macOS    | Apple Clang        | Native PR/RC gates configured; signed candidate evidence pending |
| Windows  | MSVC 2022 / MinGW | Native PR/RC gates configured; MinGW + 48 Wine C tests pass; signed candidate evidence pending |

Both 32-bit LFS (`_FILE_OFFSET_BITS=64`) and 64-bit builds are supported.
Distribution hardening (RELRO, BIND_NOW, CET, stack canaries,
FORTIFY_SOURCE) is on by default in Release builds.

## Example

```c
#include <shibadb_engine.h>
#include <string.h>

int main(void) {
    sdb_database_options opts;
    sdb_database_options_init(&opts);
    opts.password      = (const uint8_t *)"correct horse battery staple";
    opts.password_size = 28;

    sdb_database *db = NULL;
    if (sdb_database_create("mydb.shiba", &opts, &db) != SDB_OK) return 1;

    const uint8_t ns[]  = "users";
    const uint8_t key[] = "alice";
    const uint8_t val[] = "{\"role\":\"admin\"}";
    sdb_kv_put(db, ns, sizeof(ns) - 1, key, sizeof(key) - 1,
               val, sizeof(val) - 1);

    uint8_t out[128];
    size_t  out_size = 0;
    sdb_kv_get(db, ns, sizeof(ns) - 1, key, sizeof(key) - 1,
               out, sizeof(out), &out_size);

    sdb_database_close(db);
    return 0;
}
```

All functions return an `sdb_status`; `SDB_OK` is success. Get operations
report the required buffer size — pass `NULL`/`0` to size a value before
allocating, or use `sdb_kv_get_alloc` to have the library allocate for you.
A fuller tour of the ergonomic API is in
[examples/quickstart.c](examples/quickstart.c).

## Command-line tool

The build produces a dependency-free `shibadb` executable for inspecting and
scripting databases:

```bash
shibadb create  data.shiba
shibadb put     data.shiba users alice '{"role":"admin"}'
shibadb get     data.shiba users alice
shibadb scan    data.shiba users --prefix a
shibadb incr    data.shiba counters visits
shibadb mkindex data.shiba people by_role
shibadb docput  data.shiba people u1 '{"name":"alice"}' --index by_role=admin
shibadb find    data.shiba people by_role admin
shibadb verify  data.shiba
```

Run `shibadb help` for the full command list. Pass `--password` (or set the
`SHIBADB_PASSWORD` environment variable) to work with encrypted databases.

## Quickstart

```bash
git clone https://github.com/8w6s/shibadb.git
cd shibadb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requirements: a C11 compiler (GCC, Clang, MSVC), CMake >= 3.20, POSIX or
Windows. No third-party runtime dependencies.

### System install

```bash
sudo cmake --install build
```

Installs headers to `<prefix>/include/shibadb.h` and `shibadb_engine.h`, the
static and shared libraries to `<prefix>/lib/`, plus CMake package config
and pkg-config files.

### Use in a CMake project

```cmake
find_package(shibadb CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE shibadb::shibadb)
```

### Use with pkg-config

```bash
cc myapp.c $(pkg-config --cflags --libs shibadb) -o myapp
```

## Documentation

- [docs/ENGINE_API.md](docs/ENGINE_API.md) — high-level engine API,
  lifecycle, transactions, output buffers, visitor reentrancy.
- [docs/CONCURRENCY.md](docs/CONCURRENCY.md) — handle sharing, process
  locking, `SDB_E_BUSY` semantics.
- [docs/ENCRYPTION.md](docs/ENCRYPTION.md) — key hierarchy, KDF work factor,
  password rotation, legacy-file migration.
- [docs/PAGER_INVARIANTS.md](docs/PAGER_INVARIANTS.md) — pager cache, WAL,
  and page-envelope invariants.
- [docs/PUBLIC_TRANSACTIONS.md](docs/PUBLIC_TRANSACTIONS.md) —
  multi-operation transaction contract.
- [docs/WAL_FORMAT.md](docs/WAL_FORMAT.md) and
  [docs/BTREE_FORMAT.md](docs/BTREE_FORMAT.md) — on-disk formats.
- [docs/OPERATIONS.md](docs/OPERATIONS.md) — verify, backup, compact,
  migrate.
- [docs/ABI.md](docs/ABI.md) — versioning and symbol-export policy.
- [docs/WINDOWS.md](docs/WINDOWS.md) — Windows build, path handling, sanitizers, and portability standard.

## Roadmap and release

- [ROADMAP.md](ROADMAP.md) — weighted readiness table and per-area exit gates.
- [RELEASE.md](RELEASE.md) — evidence required for a production release.
- [CHANGELOG.md](CHANGELOG.md) — release-candidate history.
- [SECURITY.md](SECURITY.md) — supported versions, in-scope surface, private
  reporting.
- [CONTRIBUTING.md](CONTRIBUTING.md) — build presets, sanitizer runs, test
  layout, review workflow.

Remaining pre-1.0 work: native macOS + Windows release evidence bound to a
tagged commit, and an independent cryptography / crash-consistency review.

## License

MIT. See [LICENSE](LICENSE).
