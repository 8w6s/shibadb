# Experimental engine API

`include/shibadb_engine.h` is the dependency-free, high-level C API over the
ShibaDB pager, WAL, B+Tree, and encryption layers. Its C ABI is frozen at
version 1 and the on-disk format is stable for v1.0.

## Database lifecycle

`sdb_database_create` creates the database and its WAL. Encryption is enabled
when the options contain a non-empty password. `sdb_database_open` must then
receive the same password. Call `sdb_database_options_init` before changing
individual options.

The password KDF work factor is bounded by
`SDB_MIN_KDF_ITERATIONS` (600,000) and `SDB_MAX_KDF_ITERATIONS`
(10,000,000). The creation default `SDB_DEFAULT_KDF_ITERATIONS`
equals the minimum (600,000), matching the current OWASP
PBKDF2-HMAC-SHA256 work-factor recommendation. Callers may supply
any value in `[SDB_MIN_KDF_ITERATIONS, SDB_MAX_KDF_ITERATIONS]`;
values outside that range fail with `SDB_E_INVALID_ARGUMENT` at create.
A file whose *stored* work-factor is below `SDB_MIN_KDF_ITERATIONS` — created
by a build from before the floor was raised in 2026-07 — can no longer be
opened (`sdb_key_unwrap` rejects it), and since the rewrite APIs must open the
source first, it cannot be migrated in place; such a legacy file has no
in-place public upgrade path and must be re-exported with a build predating
the raise. This is a known limitation.

To rotate the password or raise the work-factor of a database that *does*
open, rewrite it with `sdb_database_compact` / `sdb_database_migrate` supplying
a `target_options` password (and `kdf_iterations`) — the rewrite re-encrypts,
so a new target password rotates the key. There is no standalone
`sdb_database_rotate_password` in the public API — see
[`ENCRYPTION.md`](ENCRYPTION.md).

A handle may be shared by multiple threads; public operations are serialized.
Only one handle/process owns a database path at a time. A competing open
returns `SDB_E_BUSY`. See [`CONCURRENCY.md`](CONCURRENCY.md).

## KV and blobs

KV values and blobs accept arbitrary binary keys and values. Large values are
split into bounded chunks. Each version has a generation number and SHA-256
digest. New chunks are written before the metadata head, so interrupted
replacement exposes either the complete old generation or the complete new
generation after recovery.

The current KV and blob functions share the same storage mechanism. Separate
names preserve intent and allow future streaming blob operations without
changing the KV contract.

## Documents and indexes

Documents are opaque binary values. ShibaDB does not embed a JSON parser in
the storage kernel. A binding or application extracts index terms and supplies
them to `sdb_document_put`.

Create an index before inserting indexed documents. Index creation currently
does not scan and backfill existing opaque documents. Recreating an identical
definition is idempotent; changing its uniqueness is rejected.

Document updates use generation-tagged index entries. A query validates the
entry against the current document generation, so stale entries left by an
interrupted or superseded update are invisible. Unique guards are validated
the same way. Physical stale-entry reclamation is performed by
`sdb_database_compact`.

## Transactions

`sdb_transaction_begin` creates one explicit mutation context over an open
database. KV, blob, document, and index-definition operations on that handle
share one staged B+Tree and one WAL commit. Transaction gets provide
read-your-writes. `commit` and `rollback` are terminal; call
`sdb_transaction_close` afterward to release the opaque handle.

Only one transaction may be active per database. Normal mutations, a second
transaction, and database close return `SDB_E_BUSY` until it terminates.
Transactions have fixed operation and logical-byte limits declared in the
public header.

## Output buffers

Get operations always report the required size. Passing a null output buffer
with zero capacity is the sizing operation. A too-small buffer returns
`SDB_E_BUFFER_TOO_SMALL`.

## Index visitor reentrancy

An index visitor may call another operation on the same database handle,
including writes, compaction, or migration. ShibaDB closes the current B+Tree
cursor before invoking application code and resumes by seeking the complete
last-seen index key in the current tree. Memory use therefore remains bounded
by the page size even for very large result sets.

Traversal is forward and weakly consistent under callback mutation: entries
inserted after the resume key may be observed, while entries inserted before
it are not revisited. Every returned entry is generation-validated when it is
read. Calling `sdb_database_close` from a visitor returns `SDB_E_BUSY`; the
outer visit owns the handle until it returns.

## Snapshots, cursors, and enumeration

`sdb_snapshot_open` takes the handle's single active-session slot for its whole
lifetime, so while a snapshot is live every mutating op, a transaction, a second
snapshot, backup/compact/migrate, and close return `SDB_E_BUSY` — the reader
therefore sees a tree no writer can advance (snapshot isolation under the
single-mutex model). `sdb_cursor_open` iterates one keyspace (KV or blob) within
one namespace in ascending user-key order (or descending when the `reverse`
option is set), with an optional half-open `[lower, upper)` range and a limit.
A cursor must be closed
before its snapshot — `sdb_snapshot_close` returns `SDB_E_BUSY` while any cursor
on it remains open. `sdb_kv_scan_prefix` is the auto-commit convenience wrapper
(ephemeral snapshot + walk + visitor). `sdb_list_namespaces` enumerates the
distinct `(keyspace_kind, namespace)` pairs that hold data, each once — the
"list the tables" primitive.

Direction is a property of the iterator, not of the seek. Setting `reverse` on
`sdb_cursor_options` (or on `sdb_scan_options` for the sugar) makes
`sdb_cursor_first` land on the range's **greatest** entry and `sdb_cursor_next`
step down toward its least; the range stays the same half-open
`[lower, upper)`, so a reverse walk starts at the last key strictly below
`upper_bound` and stops once it falls below `lower_bound`. `sdb_cursor_seek`
keeps lower-bound semantics in both directions.

Because the direction is applied inside the walk, `limit` composes with it the
way callers expect: `reverse` plus `limit = n` returns the **greatest** n
matches, not the smallest n reversed — which is what a client-side
`list.reverse()` after a forward scan would have given. A reverse
`sdb_kv_scan_prefix` derives the prefix's exclusive byte-successor as the
range's upper edge; a prefix of all `0xFF` bytes has no successor, and since
nothing sorts above it the walk correctly starts at the namespace's end. The
prefix itself is capped at `SDB_ENGINE_MAX_NAME_SIZE`.

## On-disk format and migration

The current on-disk format is V2. Its object/chunk key layout stores the user
key as the trailing remainder (no embedded length), so a namespace is contiguous
and ordered by user key — the property cursors and scans rely on. A V1 file
(written before 2026-08) uses the old layout and `sdb_database_open` refuses it
with `SDB_E_UNSUPPORTED_VERSION` rather than decoding it wrong.
`sdb_database_migrate_file` upgrades a V1 file to a new V2 destination (the
source is cloned and left byte-for-byte untouched); it transforms object/chunk
keys and copies index and sequence rows verbatim. It does not reclaim stale
index rows — run `sdb_database_compact` on the destination if needed.

## Convenience API (engine API version 2)

Additive ergonomic helpers that compose the primitives above — they change no
on-disk format and never touch the pager/WAL/B+Tree directly:

- **Reads.** `sdb_kv_get_alloc` returns the value in a freshly allocated buffer
  (free it with `sdb_free`), removing the probe-size-then-read dance;
  `sdb_kv_exists` tests presence without copying.
- **Counting.** `sdb_kv_count` / `sdb_kv_count_prefix` report namespace or
  prefix cardinality over a read snapshot.
- **Atomic single-key updates.** `sdb_kv_put_if_absent` (create-only, else
  `SDB_E_CONFLICT`), `sdb_kv_compare_and_swap` (swap iff the current value
  equals `expected`), and `sdb_kv_increment` (an 8-byte little-endian counter;
  a missing key starts at 0, overflow returns `SDB_E_OVERFLOW`). Each runs in a
  single transaction.
- **Atomic batches.** `sdb_kv_batch_apply` applies an array of put/delete
  operations in one all-or-nothing transaction, subject to the per-transaction
  caps.
- **Document queries.** `sdb_index_query_documents` is the `find({field:
  value})` primitive: it resolves an exact secondary-index value to the matching
  document bodies (not just ids), collecting ids under the visit lock and then
  reading each body, skipping any deleted in between.

The native `shibadb` CLI surfaces several of these directly: `exists`, `count`
(with `--prefix`), and `incr`. Its `scan` accepts `--prefix`, `--limit`, and
`--reverse`, which map onto `sdb_scan_options` unchanged.

## Current limitations

- no parallel reader throughput or shared read-only process handles;
- no index backfill or query planner;
- no streaming blob interface;
- no incremental/online backup or online compaction;
- no post-1.0 on-disk format compatibility promise yet.
