# Public transactions

The public C API now exposes a multi-operation transaction handle. Applications
can atomically update KV records, blobs, documents, index definitions, index
entries, and unique guards in one WAL commit.

The implemented contract has these semantics:

- begin, commit, and rollback on an existing database handle;
- atomic KV, blob, document, and index mutations in one WAL commit;
- read-your-writes for transaction reads;
- a stable read view for records not changed by the transaction;
- unique-index validation against both the committed tree and staged writes;
- a second transaction, normal mutation, callback mutation, or database close
  is rejected while a transaction is active;
- bounded resource accounting and deterministic overflow/OOM failure;
- rollback on validation, allocation, I/O, or explicit abort;
- recovery exposes all or none of a committed logical transaction.

## API lifecycle

`sdb_transaction_begin` creates a handle. Mutation and get functions accept
that handle and provide read-your-writes. `sdb_transaction_commit` and
`sdb_transaction_rollback` are terminal operations; afterward
`sdb_transaction_close` releases the handle. Closing an active transaction
returns `SDB_E_BUSY`, preventing accidental implicit rollback.

Transactions are bounded to `SDB_TRANSACTION_MAX_OPERATIONS` and
`SDB_TRANSACTION_MAX_LOGICAL_BYTES`. Exceeding either limit deterministically
returns `SDB_E_OVERFLOW`.

## Storage implementation

The B+Tree change set must become an explicit mutation context rather than a
temporary object owned by one `sdb_btree_put`. Reads in that context must
prefer staged nodes over pager nodes. Multiple inserts, replacements, and
deletes can then share one staged tree and be emitted through one pager
transaction.

Phase 1 implemented the internal B+Tree batch and transactional page
reservation. WAL v2 carries the target allocation watermark and freelist head,
so abort or a crash before the WAL commit cannot leave orphan pages. Existing
single-operation B+Tree calls use the same batch path.

The high-level object layer must accept that context for chunk, metadata,
index-entry, and unique-guard writes. Existing one-operation functions remain
source- and ABI-compatible by creating an internal one-operation transaction.

Phase 2 threaded that context through the object layer. Each existing KV,
blob, and document mutation creates one internal engine transaction; document
chunks, metadata, index entries, and unique guards are committed by the same
WAL record. Reads used for unique validation see staged tree pages.

Phase 3 exposed the public handle, all principal object mutations,
read-your-writes, explicit lifecycle control, deterministic resource limits,
and serialized cross-thread access.

## Reliability evidence

The milestone gate is covered by:

- `test_public_transaction`: lifecycle, isolation, read-your-writes,
  document/index conformance, unique conflict, resource overflow, commit and
  rollback;
- `test_public_transaction_property`: 200 deterministic model-based
  multi-record transactions;
- `test_public_transaction_fault`: 118 WAL/database fault boundaries proving
  all-or-none visibility;
- `test_public_transaction_crash`: 24 forced-process-exit recovery
  boundaries;
- `test_public_transaction_concurrency`: one transaction shared by four
  application threads;
- GCC, Clang, ASan/UBSan, ThreadSanitizer, static analyzer, ABI symbol, install
  consumer, release, and soak gates.

An in-memory queue replayed through separate existing `put` calls does not
satisfy this milestone because a crash could expose only a prefix.
