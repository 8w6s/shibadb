# Concurrency model

ShibaDB milestone 8 uses a deliberately coarse concurrency model that favors
correctness and predictable recovery.

## Threads

One `sdb_database` handle may be shared by multiple threads. Every public KV,
blob, document, and index operation is serialized by a recursive mutex. The
recursive property allows an index visitor to perform another database
operation on the same thread. The visitor closes its decoded B+Tree cursor
before calling application code and reseeks afterward, so a recursive split,
compaction, or migration cannot leave it traversing stale page identifiers.
Calling close from inside a visitor returns `SDB_E_BUSY`.

The application must not call `sdb_database_close` while another thread is
using the handle. Close is the ownership boundary and requires the caller to
join or otherwise stop all users first.

This model prevents data races in the pager, page cache, WAL, B+Tree, and
encryption state. It does not yet provide parallel reader throughput.

One public transaction may also be shared by application threads. Individual
transaction calls use the owning database mutex, so they are serialized and
provide read-your-writes over a single staged tree. Commit, rollback, and
close remain ownership boundaries: callers must stop and join transaction
users before terminating its handle. A database close or second transaction
while it is active returns `SDB_E_BUSY`.

## Processes

Opening a database resolves its canonical absolute path and acquires an
exclusive, non-blocking pair of operating-system locks: one for the canonical
path namespace and one for the database file identity itself. A second handle,
whether in the same or a different process and even through a relative or
symbolic-link alias, returns `SDB_E_BUSY`. Both locks span superblock reading,
WAL recovery, normal operations, and close, so two processes cannot recover,
checkpoint, or atomically replace the same database concurrently.

Creation locks `<database-path>.lock` while the file does not yet exist, then
adds the database file-identity lock before initializing the B+Tree. The
canonical path lock remains held for the handle lifetime. Its sidecar remains
on disk after close; its existence does not mean the database is locked.

Hardlinked database files are rejected because multiple directory identities
cannot provide a unique WAL/replacement sidecar. Symbolic links are supported
because they resolve to one canonical database and WAL path.

The supported deployment target is a local filesystem with normal POSIX
`flock` or Windows `LockFileEx` semantics. Network/distributed filesystems
require separate qualification.

## Reliability gate

The concurrency test covers:

- rejection of a second handle in the same process;
- rejection of a competing child process;
- rejection of relative/symbolic aliases while the canonical file is locked;
- rejection of hardlinked database aliases;
- four threads performing interleaved put/get operations on one handle;
- four competing processes acquiring the database in turn and committing
  durable records;
- forced owner death followed by successful lock reacquisition;
- reopen and verification of every thread/process record;
- ThreadSanitizer execution of the shared-handle test;
- four threads sharing one public transaction followed by atomic commit and
  durable verification.
