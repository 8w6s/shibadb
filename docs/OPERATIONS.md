# Operational APIs

Milestone 9 adds failure-safe verification, backup, compaction, and migration
to the experimental engine API.

## Verify

`sdb_database_verify` runs while holding the database handle mutex and checks:

- every allocated page checksum or authenticated-encryption tag;
- page identifiers, types, and the complete freelist chain;
- exclusive page ownership: every allocated page is reachable from exactly
  one of the B+Tree or freelist, with no valid-but-orphaned DATA pages;
- B+Tree child ownership, cycles, bounds, uniform leaf depth, and leaf links;
- every object metadata record and every live chunk;
- streamed SHA-256 of each object without loading the whole database in RAM;
- structural validity of index definitions, entries, and unique guards;
- live versus stale generation references.

The result reports physical pages, tree shape, raw entries, live objects,
chunks, stale entries, and logical bytes.

After WAL recovery, open validates the committed allocation watermark against
the physical file length before allocating verification state. A short file is
rejected as truncated; bytes beyond the last committed page are synchronized
away as an abandoned allocation tail.

## Backup

`sdb_database_backup` first performs a full verify, synchronizes the source,
copies it to a randomized temporary file in the destination directory,
synchronizes that file, and atomically installs it. The destination process
lock prevents replacing a database that is currently open. Destination paths
are canonicalized before self-backup checks, locking, marker creation, and
replacement, so relative and symbolic aliases cannot bypass that lock.
Hardlinked destinations are rejected.

A replacement marker containing the new database `file_id` is synchronized
before installation. If the process dies between replacing the main file and
removing an older destination WAL, open-time recovery uses this marker to
discard only the WAL known to belong to the replaced file.

The snapshot is a physical copy. An encrypted backup uses the same password as
the source at snapshot time.

## Compact

`sdb_database_compact` verifies the source, creates a new database with the
target options, and copies only records reachable from current object and
index generations. It therefore removes superseded chunks, stale index
entries, unused pages, and B+Tree fragmentation. The replacement database is
fully verified before atomic installation.

The target options may change page size, password, or encryption state. The
existing handle remains usable after the swap.

## Migration

`sdb_database_migrate` uses the same verified logical rewrite. The caller must
name the target format version; unknown versions return
`SDB_E_UNSUPPORTED_VERSION` before mutation. Version 1 migration currently
supports page-size changes and plaintext-to-encrypted or password-changing
rewrites. Future format versions require an explicit decoder before they can
be accepted.

## Crash guarantees

The source process lock is held through the operation. The main database path
always names either the complete old image or the complete verified new image.
Compaction transfers the identity lock from the old inode to the verified new
inode while retaining the canonical path lock, so no opener can enter between
rename and replacement cleanup.
The test suite terminates compaction at three durability phases: before the
marker, after the marker but before replacement, and after replacement but
before cleanup. Every image must reopen, verify, and return the complete
current payload.

Backup I/O failure injection likewise requires the destination to remain a
complete old or new snapshot. The matrix now cuts every temporary-destination
chunk write and its final `fsync`, then separately kills the process at four
durability phases: after the temporary snapshot sync, after marker sync,
after replacement, and after destination-directory sync. Reopen must recover
to a verified complete old or new snapshot at every boundary.

## Close semantics

`sdb_database_close` runs two ordered steps: it drains the pager (final
sync + WAL clear + inode-lock release) and then releases the mutex plus
frees the handle. If the pager-drain step reports failure and the
mutex/lock-release step succeeds, `close` returns the pager-drain error
(e.g. `SDB_E_IO` from a failed final sync). The reverse can also happen:
a clean pager drain followed by a failed lock release ALSO returns
`SDB_E_IO`. Both close-failure paths surface as `SDB_E_IO`, so the status
code alone does not tell "durability suspect" apart from "lock not
released" — treat any close error conservatively:

- **`SDB_E_IO` from close**: the last WAL record may or may not have
  reached disk. Assume the database is in the same state that a crash
  before the last commit would leave — recovery on the next open is exactly
  the WAL replay path, so no manual intervention is needed. The file is
  always safe to re-open; a stale on-disk identity lock (e.g. from a fork
  that inherited the fd) is cleaned up by the next successful open+close
  cycle. Alert and re-open.

Either error is safe to retry `close` on. Neither leaks storage.

## WAL mode

Writes go through an append-only WAL sidecar (`<database>.wal`). The current
v5 format packs one `frames + commit-record` block per transaction and appends
without rewriting older blocks. Every frame carries the database `file_id`, so
recovery can reject a foreign WAL even when the separate identity header is
torn. See `docs/WAL_FORMAT.md` for the byte layout and legacy rules.

A commit stages the frames and their 44-byte commit record together, appends
the complete buffer, and executes one ordered `fsync` against the WAL:

1. Append `[frames | commit-record]` at the current WAL tail.
2. `fsync` the WAL once, then publish the new tail/index in memory.

Only after the sync returns is the commit durable. A partial append cannot
produce an accepted transaction: recovery verifies the commit-record checksum
and its running CRC over every frame before applying any page.

Reads are served through an in-memory WAL index that maps every WAL-resident
page to its latest committed frame. `sdb_kv_get` and every internal page
read consult the index first and fall back to the main data file only when
the page has never been written through the WAL, so a checkpoint is never
on the read path. The index is rebuilt on open by scanning the sidecar.

Checkpoint drains every committed txn into the main data file, `fsync`s the
data file, advances `checkpoint_lsn` in the mirrored superblock, and
truncates the WAL back to zero. It runs when the sidecar crosses a size
threshold during a write and unconditionally on `sdb_database_close`, so a
clean shutdown always leaves an empty WAL. A checkpoint is not required for
durability — it exists to bound the WAL's on-disk footprint and the recovery
scan cost.

Recovery on open replays every committed txn with `txn_id > checkpoint_lsn`
in the order they were appended. Each txn is validated end-to-end
(commit-record magic + version + self-checksum, running CRC over its
frames, contiguous `txn_id` from `checkpoint_lsn + 1`, monotone
`next_page_id`, `freelist_page` in range, every frame's page checksum and
LSN) before any of its frames touch the data file. If any validation fails
or the tail is torn, the whole remainder of the file is dropped and every
txn before the tear is still applied — recovery is all-or-nothing per txn
and all-committed-below-the-tear across the file.

## Write throughput and batching

Every auto-commit write (`sdb_kv_put`, `sdb_kv_delete`) is one transaction and
is durability-bound by at least one WAL sync. Checkpoint adds data-file,
superblock, and WAL-clear syncs when the threshold is reached or the database
is closed. Throughput therefore depends strongly on storage latency,
checkpoint frequency, and whether group commit can coalesce callers; use the
benchmark harness on the target filesystem instead of treating historical
figures as a guarantee.

For bulk loading or any write-heavy path, group writes into an explicit
transaction (`sdb_transaction_begin`, repeated `sdb_transaction_kv_put`,
`sdb_transaction_commit`). One `fsync` barrier then amortises across the whole
batch. Exact gains and the best batch size are workload- and filesystem-specific;
measure them with `bench_kv`/`bench_concurrent`. A batch is atomic:
a crash mid-transaction discards the whole batch and never a partial subset,
and already-committed batches are unaffected.

Reads (`sdb_kv_get`) do not commit and avoid durability barriers. Current
delete/overwrite paths eagerly reclaim chunk and index rows; `compact` remains
useful for B+Tree fragmentation and historical images that predate eager
reclamation.

`sdb_database_compact` copies live records through the same batched-write path
internally, so a large compaction runs at bulk-load speed rather than
auto-commit speed.

## Current limitations

- operations are blocking and require exclusive ownership of the handle;
- backup is a full physical snapshot, not incremental;
- compact temporarily needs space for a second complete database;
- there is no repair mode; verification reports corruption without guessing
  how to modify damaged data.
