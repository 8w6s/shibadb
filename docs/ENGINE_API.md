# Experimental engine API

`include/shibadb_engine.h` is the dependency-free, high-level C API over the
ShibaDB pager, WAL, B+Tree, and encryption layers. Its C ABI is frozen at
version 1; the on-disk format compatibility promise remains pre-1.0.

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
values outside that range fail `sdb_database_options_validate`
with `SDB_E_INVALID_ARGUMENT`. Files created with a stored
work-factor below `SDB_MIN_KDF_ITERATIONS` (i.e. legacy files
predating 2026-07) must first be migrated via
`sdb_database_rotate_password` to a compliant value before opening
under a current build — see [`ENCRYPTION.md`](ENCRYPTION.md).

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

## Current limitations

- no parallel reader throughput or shared read-only process handles;
- no index backfill or query planner;
- no streaming blob interface;
- no incremental/online backup or online compaction;
- no post-1.0 on-disk format compatibility promise yet.
