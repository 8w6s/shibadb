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
a clean pager drain followed by a failed lock release returns
`SDB_E_LOCK`. The two errors are distinct on-purpose so operators can
decide their response:

- **`SDB_E_IO` from close**: the last WAL record may or may not have
  reached disk. Assume the database is in the same state that a crash
  before the last commit would leave — recovery on next open is exactly
  the WAL replay path, so no manual intervention is needed. Alert.
- **`SDB_E_LOCK` from close**: durability is fine (pager drained
  cleanly); the on-disk file identity lock could not be released. This
  usually indicates process-level state (e.g. a fork that inherited the
  fd). The file is safe to re-open; the stale lock is cleaned up by the
  next successful open+close cycle.

Either error is safe to retry `close` on. Neither leaks storage.

## Current limitations

- operations are blocking and require exclusive ownership of the handle;
- backup is a full physical snapshot, not incremental;
- compact temporarily needs space for a second complete database;
- there is no repair mode; verification reports corruption without guessing
  how to modify damaged data.
