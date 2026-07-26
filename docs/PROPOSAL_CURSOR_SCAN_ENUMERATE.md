# ShibaDB-C Public Cursor, Scan, and Enumerate API

**Status:** Proposal (revision 2, after adversarial review)
**Audience:** ShibaDB-C library authors and consumers
**ABI impact:** Append-only. No changes to existing `SDB_API` declarations. All new symbols are additive and covered by `SDB_ENGINE_API_VERSION = 1`.

---

## 1. Design summary

This proposal adds three orthogonal capabilities to the ShibaDB-C public API:

1. **Public cursors** — opaque, snapshot-scoped iterators over the KV and blob namespaces, matching the semantics of `sdb_btree_cursor_*` but bound to a snapshot handle the caller owns. Forward and reverse iteration are shipped together in v1.
2. **Prefix and range scans** — sugar over the cursor primitive, exposed in both transactional and auto-commit forms, plus reverse variants and a `limit` field. One primitive (seek + next/prev), one lifetime rule (valid until next cursor op), one status channel. This follows the LMDB/RocksDB/LevelDB shape and rejects the callback-only pattern used by `sdb_index_visit`.
3. **Callback-based enumeration** — `sdb_list_namespaces`, `sdb_list_collections`, `sdb_list_indexes`, plus a range-restricted `sdb_index_visit_range`. Fill-buffer variants (`sdb_list_*_into`) are provided for the common "give me all names as a contiguous array" case.

The design is deliberately layered so callers can pick their level:

| Layer | Callers who want... | Use... |
|---|---|---|
| Sugar (auto-commit) | one-shot scans, no explicit txn | `sdb_kv_scan_prefix`, `sdb_kv_scan_range` |
| Sugar (transactional) | scans batched with mutations | `sdb_transaction_kv_scan_prefix`, `sdb_transaction_kv_scan_range` |
| Primitive | manual cursor control, seek/next/prev | `sdb_cursor_*` on a `sdb_snapshot` |
| Enumeration | catalog listings | `sdb_list_*`, `sdb_list_*_into`, `sdb_index_visit_range` |

**Slice lifetime rule (uniform across every new API):** any pointer returned via an out-param or passed to a callback is valid *only* until the next state-mutating call on the same cursor / snapshot / callback boundary. Copy before crossing that boundary. This matches Rocks/LevelDB and is documented on every function.

**Crash-safety rule (uniform):** No handle in this API — snapshot, cursor, or otherwise — survives a process crash. On restart, every handle is invalid; callers must not persist handles to shared memory across process boundaries. All snapshot floors held by dead processes are reclaimed during database open (see §9).

---

## 2. Dependency on the concurrency model

This proposal is a strict client of the concurrency model chosen for `shibadb-c` (see the "Multi-Reader Concurrency" proposal). It is **not compatible with any snapshot model that permits in-place page overwrites while a reader's snapshot floor references the old version.** Concretely, this proposal requires the following invariants from the pager layer:

**Invariant P1 (snapshot floor).** Every live `sdb_snapshot` reserves an LSN floor in a pager-owned reader-floor table. The WAL retains post-image *and* pre-image records for every page whose latest committed LSN exceeds the minimum floor, OR the pager uses copy-on-write for any page still referenced by a live snapshot. Either mechanism is acceptable; the proposal does not care which. What is **not** acceptable is a pure redo-log-plus-in-place-writeback model, which cannot reconstruct a pre-image and would silently violate snapshot isolation (see also §11, rejected variant).

**Invariant P2 (freelist pinning).** A page whose latest committed LSN exceeds the minimum reader floor MUST NOT be reclaimed to the freelist. Reallocation of such a page to a different logical role is a fatal invariant violation that this proposal has no defense against — AAD authentication cannot distinguish a legitimate reallocation from corruption.

**Invariant P3 (crash-durable snapshot floors).** Reader floors are held in shared memory keyed by process identity (pid + session-nonce). On database open, the pager scans the floor table, evicts entries whose owning process is dead, and only then advances `checkpoint_lsn`. Without this, a crashed reader would prevent WAL truncation forever.

**Invariant P4 (read-in-progress refcount for scalar reads).** Any implicit-snapshot scalar read (e.g. `sdb_kv_get` with no explicit snapshot) MUST take a read-in-progress refcount *before* sampling `checkpoint_lsn` and hold it until every decrypt on the read path completes. `sdb_database_close` returns `SDB_E_BUSY` while the refcount is nonzero. This is a real refcount, distinct from the existing `callback_depth` (which is only bumped inside visitor callbacks and does not protect scalar reads).

If the concurrency proposal cannot honor P1–P4, the cursor API described here MUST NOT ship as-is. See §11 for what was rejected on this basis.

---

## 3. New opaque types

```c
/**
 * Read snapshot handle. A snapshot pins a consistent view of the database
 * across an arbitrary number of cursor and scan operations. It is the
 * lifetime anchor for every cursor and scan iterator in this API surface.
 *
 * A snapshot reserves an LSN floor in the pager's reader-floor table
 * (see Invariant P1). It MUST be closed with sdb_snapshot_close to
 * release the floor; leaked snapshots prevent WAL truncation and, once
 * enough have accumulated, disk pressure. Crashed processes' floors are
 * reclaimed on database open (Invariant P3).
 *
 * Snapshots are NOT thread-safe. A snapshot AND every cursor derived from
 * it must be touched from a single thread; passing them across threads is
 * undefined behavior. To scan concurrently, open one snapshot per thread;
 * multiple snapshots on the same database are supported.
 *
 * Snapshots do NOT survive process crashes. On restart every prior snapshot
 * handle is invalid, even if the caller placed it in shared memory.
 */
typedef struct sdb_snapshot sdb_snapshot;

/**
 * Forward/reverse iterator over a keyspace (KV namespace or blob namespace)
 * within a snapshot. A cursor observes exactly the committed state at the
 * moment its parent snapshot was opened, subject to the concurrency
 * invariants (§2). Committed mutations that happened AFTER the snapshot
 * are invisible.
 *
 * A cursor is NOT thread-safe and must be used from the same thread as its
 * parent snapshot.
 *
 * Every successfully-opened cursor MUST be closed exactly once with
 * sdb_cursor_close. sdb_snapshot_close invalidates outstanding cursors
 * (marking them errored) rather than refusing to close — see §4.5 Rule 3.
 */
typedef struct sdb_cursor sdb_cursor;
```

Rationale for opaque pointers rather than caller-stack structs: the internal `sdb_btree_cursor` embeds a decoded leaf node that owns malloc'd storage, and the public API must reserve the right to grow that state (buffered leaves, prefetch state, per-scan filters) without breaking ABI. Opacity is the standard ShibaDB pattern (`sdb_database`, `sdb_transaction`) — cursors follow suit.

---

## 4. Snapshot lifecycle

```c
/**
 * Open a read snapshot on database. The snapshot observes the last committed
 * state at the moment of this call and reserves an LSN floor in the pager's
 * reader-floor table.
 *
 * @param database        The open database handle. Must be non-NULL and open.
 * @param snapshot_out    Receives the snapshot handle on success. Must be
 *                        non-NULL. On any error, *snapshot_out is unmodified.
 *
 * @retval SDB_OK                    Snapshot opened; *snapshot_out is valid.
 * @retval SDB_E_INVALID_ARGUMENT    database is NULL, closed, or snapshot_out is NULL.
 * @retval SDB_E_OUT_OF_MEMORY       Allocation failed.
 * @retval SDB_E_IO                  Underlying pager read failure while resolving root.
 */
SDB_API sdb_status sdb_snapshot_open(
    sdb_database *database,
    sdb_snapshot **snapshot_out
);

/**
 * Open a read snapshot bound to a transaction's view. The returned snapshot
 * observes the transaction's own in-flight writes merged with the committed
 * base state, allowing cursor-shaped access to a transaction (see §5).
 *
 * The snapshot's lifetime is tied to the transaction: committing or rolling
 * back the transaction implicitly invalidates the snapshot and every cursor
 * derived from it. Live transaction-bound snapshots do NOT block commit;
 * they enter a terminal error state.
 *
 * @retval SDB_OK                    Snapshot opened.
 * @retval SDB_E_INVALID_ARGUMENT    transaction is NULL or snapshot_out is NULL.
 * @retval SDB_E_BUSY                Transaction is no longer active or poisoned.
 * @retval SDB_E_OUT_OF_MEMORY       Allocation failed.
 */
SDB_API sdb_status sdb_transaction_snapshot_open(
    sdb_transaction *transaction,
    sdb_snapshot **snapshot_out
);

/**
 * Release a snapshot and its LSN floor.
 *
 * If any cursor opened from this snapshot is still live, close invalidates
 * every such cursor (they enter the error state described in §4.5 Rule 6,
 * with sdb_cursor_status returning SDB_E_INVALID_ARGUMENT) and then
 * releases the snapshot. This matches sdb_transaction_close's
 * "implicit rollback" semantics: close always succeeds, cleanup paths do
 * not need to track cursor lifetimes.
 *
 * The invalidated cursors MUST still be closed exactly once with
 * sdb_cursor_close (no double-free — the handles remain allocated,
 * just non-functional, until the caller closes them). This preserves
 * the caller's ownership model.
 *
 * NULL-tolerant: sdb_snapshot_close(NULL) is a no-op returning SDB_OK.
 *
 * @param snapshot        Snapshot to close, or NULL.
 *
 * @retval SDB_OK                    Always (even for NULL, even with live cursors).
 */
SDB_API sdb_status sdb_snapshot_close(sdb_snapshot *snapshot);

/**
 * Return the snapshot's opaque monotonic version identifier. Diagnostic
 * only — the value has no meaning beyond "two snapshots with the same
 * version observe the same committed state". NULL-tolerant: returns 0.
 */
SDB_API uint64_t sdb_snapshot_version(const sdb_snapshot *snapshot);
```

**Note on `sdb_database_close`.** The existing `sdb_database_close` continues to return `SDB_E_INVALID_ARGUMENT` on NULL for backward compatibility. New APIs (`sdb_snapshot_close`, `sdb_cursor_close`) are NULL-tolerant. This inconsistency is a known deviation from the "one convention" ideal; the pragmatic answer is that new cleanup code should mix them freely because the new APIs handle NULL and the legacy database close is called once at the end of a `goto fail`-style path. A future proposal may deprecate the NULL-hostility on `sdb_database_close`.

**Snapshot-vs-database lifetime.** Closing the database while any snapshot is live returns `SDB_E_BUSY` from `sdb_database_close`. This is enforced via the reader-floor table (§2 Invariant P3) rather than a separate refcount.

---

## 5. Cursor lifecycle and navigation

### 5.1 Keyspace selector

A cursor scans exactly one keyspace. Because ShibaDB partitions storage into KV namespaces, blob namespaces, and document collections that share a b-tree but use distinct key prefixes internally, the public cursor API exposes a small enum to name the target keyspace explicitly:

```c
/**
 * Keyspace selector for public cursors and scans. The zero value is reserved
 * as an invalid sentinel so that a zero-initialised struct is rejected
 * loudly rather than silently defaulting to KV.
 */
typedef enum sdb_keyspace_kind {
    SDB_KEYSPACE_UNSPECIFIED = 0,   /* Rejected by every API that consumes this enum. */
    SDB_KEYSPACE_KV = 1,
    SDB_KEYSPACE_BLOB = 2
} sdb_keyspace_kind;

/**
 * List-filter for catalog enumeration. Bit-flag form so callers can filter
 * KV, BLOB, or ANY without overloading sdb_keyspace_kind.
 */
typedef enum sdb_list_filter {
    SDB_LIST_FILTER_KV = 1 << 0,
    SDB_LIST_FILTER_BLOB = 1 << 1,
    SDB_LIST_FILTER_ANY = SDB_LIST_FILTER_KV | SDB_LIST_FILTER_BLOB
} sdb_list_filter;
```

Document collections are enumerable via `sdb_list_collections`, indexes via `sdb_list_indexes` / `sdb_index_visit_range`, and every document in a collection via `sdb_collection_document_visit` (§8) — a general enumeration for backup/export tooling that does not require the caller to have a covering index.

### 5.2 Cursor open

```c
/**
 * Cursor open options. Zero-initialise via sdb_cursor_options_init before
 * use; leaving all fields at defaults produces a cursor that iterates the
 * entire keyspace in ascending byte-lexicographic order.
 *
 * lower_bound / upper_bound: optional half-open range [lower_bound, upper_bound).
 * If lower_bound is non-NULL, the cursor starts (in forward mode) at the first
 * key >= lower_bound; in reverse mode, starts at the last key < upper_bound.
 * If upper_bound is non-NULL, forward iteration terminates once the current
 * key >= upper_bound. Reverse iteration terminates once the current key <
 * lower_bound.
 *
 * The bound byte buffers are COPIED into cursor-owned storage at
 * sdb_cursor_open time. Callers may free or reuse the input buffers as
 * soon as sdb_cursor_open returns. (Rationale: the alternative "borrowed
 * for the cursor's lifetime" rule is a persistent footgun when options are
 * stack-allocated in a caller function that returns before the cursor
 * closes. Copying is cheap for the typical <256-byte bound.)
 *
 * reverse: when true, iteration proceeds in descending key order.
 * sdb_cursor_first positions at the LAST entry; sdb_cursor_next advances
 * toward the FIRST entry. sdb_cursor_seek still uses lower-bound semantics
 * on the given key. Reverse iteration is fully supported in v1.
 *
 * limit: if non-zero, sdb_cursor_valid returns false after `limit` successful
 * navigation steps (first, seek, or next). Zero means no limit. This is
 * cheaper than a caller-side counter and composes with bounds naturally.
 */
typedef struct sdb_cursor_options {
    uint32_t struct_size;       /* Set by sdb_cursor_options_init. */
    const uint8_t *lower_bound;
    size_t lower_bound_size;
    const uint8_t *upper_bound;
    size_t upper_bound_size;
    bool reverse;
    uint8_t reserved_alignment[7];
    uint64_t limit;
    uint64_t reserved[3];
} sdb_cursor_options;

SDB_API void sdb_cursor_options_init(sdb_cursor_options *options);

/**
 * Options struct versioning. The library accepts any struct_size >= the size
 * of struct_size itself and <= the current sizeof(sdb_cursor_options).
 * Fields beyond the caller's struct_size are treated as zero. If a caller's
 * struct_size exceeds the library's known size (caller is newer than lib),
 * the call returns SDB_E_INVALID_ARGUMENT. Callers MUST invoke
 * sdb_cursor_options_init before setting fields.
 */

/**
 * Open a cursor over the specified keyspace within snapshot.
 *
 * After a successful open, the cursor is positioned BEFORE the first entry
 * (in reverse mode: AFTER the last entry). sdb_cursor_valid returns false
 * until sdb_cursor_first or sdb_cursor_seek is called. A first call to
 * sdb_cursor_next on an unpositioned cursor returns SDB_E_INVALID_ARGUMENT
 * — callers MUST issue an explicit sdb_cursor_first or sdb_cursor_seek to
 * position the cursor. (Rationale: making next-from-unpositioned land on
 * entry 0 is the LMDB pattern; making it an error is the Bolt pattern.
 * We chose the error to force explicitness and avoid the off-by-one hazard
 * documented in every LMDB port.)
 *
 * @param snapshot            Parent snapshot. Must be non-NULL.
 * @param keyspace_kind       SDB_KEYSPACE_KV or SDB_KEYSPACE_BLOB.
 *                            SDB_KEYSPACE_UNSPECIFIED is rejected.
 * @param namespace_name      Namespace name bytes (borrowed for the call only).
 * @param namespace_size      Length of namespace_name in bytes. 1..SDB_ENGINE_MAX_NAME_SIZE.
 * @param options             Optional pointer to init'd options.
 *                            NULL is equivalent to a default-initialised struct.
 * @param cursor_out          Receives the cursor handle on success.
 */
SDB_API sdb_status sdb_cursor_open(
    sdb_snapshot *snapshot,
    sdb_keyspace_kind keyspace_kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const sdb_cursor_options *options,
    sdb_cursor **cursor_out
);
```

### 5.3 Cursor navigation

```c
/**
 * Position the cursor at the first entry (forward mode) or last entry
 * (reverse mode) within its bounded range. Cursor is "positioned" after
 * this call — sdb_cursor_next/prev/key/value/read are now legal.
 */
SDB_API sdb_status sdb_cursor_first(sdb_cursor *cursor);

/**
 * Position the cursor at the last entry (forward mode) or first entry
 * (reverse mode). Symmetric with sdb_cursor_first. Provided for parity
 * with LMDB MDB_LAST and RocksDB SeekToLast.
 */
SDB_API sdb_status sdb_cursor_last(sdb_cursor *cursor);

/**
 * Position the cursor at the first entry with key >= key (lower-bound seek).
 * Works identically in forward and reverse modes; iteration direction is a
 * property of the cursor, not the seek.
 */
SDB_API sdb_status sdb_cursor_seek(
    sdb_cursor *cursor,
    const uint8_t *key,
    size_t key_size
);

/**
 * Position the cursor at the entry with key == key exactly. If no such
 * entry exists, returns SDB_E_NOT_FOUND and the cursor enters the terminal
 * state (sdb_cursor_valid == false). Provided for parity with LMDB MDB_SET
 * and the "does this key exist in a snapshot" idiom that would otherwise
 * cost 4 calls.
 */
SDB_API sdb_status sdb_cursor_seek_exact(
    sdb_cursor *cursor,
    const uint8_t *key,
    size_t key_size
);

/**
 * Advance the cursor by one entry in its configured direction (ascending in
 * forward mode, descending in reverse mode). Past the end/start of range,
 * sdb_cursor_valid returns false and the call still returns SDB_OK. This is
 * the terminal end-of-iteration signal; SDB_E_NOT_FOUND is never used for
 * end-of-iteration.
 */
SDB_API sdb_status sdb_cursor_next(sdb_cursor *cursor);

/**
 * Step the cursor by one entry in the direction opposite to its configured
 * direction. This is symmetric with sdb_cursor_next: forward-mode cursors
 * step backward with prev, reverse-mode cursors step forward with prev.
 * Note: this is NOT "undo the last next" — it moves relative to the current
 * key.
 */
SDB_API sdb_status sdb_cursor_prev(sdb_cursor *cursor);

/**
 * True iff the cursor currently addresses a live entry within its range.
 * Cheap (non-blocking, no I/O). NULL-tolerant: returns false.
 */
SDB_API bool sdb_cursor_valid(const sdb_cursor *cursor);

/**
 * Status of the most recent state-mutating cursor call (open, first, last,
 * seek, seek_exact, next, prev). Cleared to SDB_OK by any subsequent
 * successful state-mutating call (see §5.5 Rule 6).
 */
SDB_API sdb_status sdb_cursor_status(const sdb_cursor *cursor);
```

### 5.4 Reading the current entry

Two shapes are provided: a zero-copy peek that returns pointers into the cursor's decoded leaf, and an atomic get that returns both key and value in one call (matching LMDB `mdb_cursor_get`).

```c
/**
 * Peek at the current entry's key WITHOUT copying. The returned pointer is
 * valid ONLY until the next state-mutating call on THIS cursor
 * (first/last/seek/seek_exact/next/prev/close). To retain the bytes, copy
 * them or use sdb_cursor_get / sdb_cursor_read.
 *
 * Cursor MUST be valid. NULL-tolerant: if cursor is NULL or invalid, sets
 * *key_out = NULL, *key_size_out = 0, returns SDB_E_INVALID_ARGUMENT.
 */
SDB_API sdb_status sdb_cursor_key(
    const sdb_cursor *cursor,
    const uint8_t **key_out,
    size_t *key_size_out
);

/**
 * Peek at the current entry's value WITHOUT copying. Same lifetime and
 * error rules as sdb_cursor_key.
 *
 * For SDB_KEYSPACE_BLOB cursors, this transparently materializes overflow
 * blobs into cursor-owned scratch storage on first access; subsequent
 * sdb_cursor_value calls at the same position return the same pointer.
 * The scratch buffer is reused on the next state-mutating call. There is
 * NO SDB_E_UNSUPPORTED for overflow blobs — the bimodal behavior of the
 * v1 draft was a footgun that only manifested at scale.
 *
 * Callers who want to avoid the implicit allocation for large blobs
 * should use sdb_cursor_read with their own buffer.
 */
SDB_API sdb_status sdb_cursor_value(
    const sdb_cursor *cursor,
    const uint8_t **value_out,
    size_t *value_size_out
);

/**
 * Peek at both key and value in one call. Equivalent to sdb_cursor_key +
 * sdb_cursor_value but saves a call. Same lifetime rules.
 */
SDB_API sdb_status sdb_cursor_get(
    const sdb_cursor *cursor,
    const uint8_t **key_out,
    size_t *key_size_out,
    const uint8_t **value_out,
    size_t *value_size_out
);

/**
 * Copy the current entry's key and value into caller-provided buffers.
 * Mirrors sdb_kv_get contract exactly: *size_out params are always written
 * with real sizes before capacity check, so callers may probe by passing
 * capacity=0. Zero-size fields skip the memcpy. Does NOT advance the cursor.
 */
SDB_API sdb_status sdb_cursor_read(
    const sdb_cursor *cursor,
    uint8_t *key_out,
    size_t key_capacity,
    size_t *key_size_out,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);

/**
 * Duplicate a cursor at its current position, including bounds, direction,
 * and remaining limit. The duplicate is independent of the original; both
 * may be navigated separately, both must be closed separately. Useful for
 * paginated APIs that want to save a resume point without stringifying
 * the current key.
 *
 * @retval SDB_OK                    Duplicate created.
 * @retval SDB_E_INVALID_ARGUMENT    Source cursor NULL or in error state.
 * @retval SDB_E_OUT_OF_MEMORY       Allocation failed.
 */
SDB_API sdb_status sdb_cursor_dup(
    const sdb_cursor *cursor,
    sdb_cursor **cursor_out
);
```

### 5.5 Cursor close and invalidation rules

```c
/**
 * Release a cursor. NULL-tolerant. Must be called exactly once for every
 * cursor returned by sdb_cursor_open or sdb_cursor_dup, even after the
 * cursor has been invalidated by sdb_snapshot_close.
 */
SDB_API sdb_status sdb_cursor_close(sdb_cursor *cursor);
```

**Rule 1 — Snapshot-scoped consistency (with concurrency-model caveat).**
A cursor observes exactly the committed state at the moment its parent snapshot was opened, PROVIDED the concurrency invariants in §2 hold. Under a single-mutex or reader-writer model, snapshot isolation is trivially achieved by writer exclusion. Under a multi-version model (WAL-with-snapshot-floor or copy-on-write), the reader floor in the pager reader-floor table (P1) and the freelist-pinning invariant (P2) together guarantee that a cursor can never observe a page that has been repurposed after its snapshot open. Concurrent commits on any handle to the same database file are invisible to the cursor.

**Rule 2 — Peek pointer lifetime.**
Pointers returned by `sdb_cursor_key`, `sdb_cursor_value`, and `sdb_cursor_get` are borrowed from cursor-owned storage (either the decoded leaf or the overflow scratch buffer). They are valid until the next state-mutating call on the same cursor. Reading them after such a call is undefined behavior. Callers who need durable bytes must `memcpy` or use `sdb_cursor_read`.

The cursor's decoded leaf is a private copy — not a reference into the shared page cache — precisely so that concurrent commits under the multi-reader model cannot invalidate a peek pointer mid-callback. This is a deliberate trade-off: slightly higher memory per cursor, but Rule 2 becomes an unconditional guarantee rather than "true unless the shared cache evicts the underlying entry".

**Rule 3 — Parent-snapshot lifetime.**
`sdb_snapshot_close` invalidates every outstanding cursor and then succeeds. Invalidated cursors have `sdb_cursor_valid == false`, `sdb_cursor_status == SDB_E_INVALID_ARGUMENT`, and all read calls fail — but the handle remains allocated until the caller calls `sdb_cursor_close`. This is the same lifecycle model as `sdb_transaction_close`'s implicit rollback: cleanup paths that use `goto fail` sequences can call the close functions in the natural order without tracking cursor lifetimes explicitly.

**Rule 4 — Parent-database lifetime.**
Closing the database while any snapshot is live returns `SDB_E_BUSY` from `sdb_database_close`. The invariant is: **database > snapshot ≥ cursor** in reverse lifetime order. Snapshots block database close; cursors do not block snapshot close (they are invalidated instead).

**Rule 5 — Terminal state.**
After navigation moves past the end of range, the cursor enters a **terminal but still-open** state: `sdb_cursor_valid` returns false, `sdb_cursor_status` returns `SDB_OK`, read calls return `SDB_E_INVALID_ARGUMENT`. The cursor must still be closed. `sdb_cursor_seek`, `sdb_cursor_first`, or `sdb_cursor_last` may reposition it.

**Rule 6 — Error state and error-clear semantics.**
If a state-mutating call returns `SDB_E_IO` or `SDB_E_CORRUPT`, the cursor enters an **error state**. Any *subsequent successful* state-mutating call (first/last/seek/seek_exact/next/prev) clears the error and `sdb_cursor_status` returns `SDB_OK`. Read calls in the error state fail with `SDB_E_INVALID_ARGUMENT`. `sdb_cursor_close` always succeeds on an errored cursor.

**Rule 7 — Not thread-safe.**
A snapshot and every cursor derived from it must all be touched from the same thread — no exceptions, not even one cursor per thread on a shared snapshot. To scan concurrently, open **one snapshot per thread**. This is a design constraint, not an implementation limitation: the per-cursor decoded-leaf buffers and the snapshot's reader-floor table entry are not synchronized.

Example (correct):
```
thread A: snapshot_open, cursor_open on snapshot_A, iterate, close, close
thread B: snapshot_open, cursor_open on snapshot_B, iterate, close, close
```
Example (undefined):
```
main:   snapshot_open
thread A: cursor_open on shared snapshot
thread B: cursor_open on shared snapshot   /* UB */
```

**Rule 8 — Crash safety.**
No cursor or snapshot handle survives a process crash. Placing a handle in shared memory does not extend its lifetime across `exec` or crash-restart. On database open, the pager reclaims floors held by dead processes (§2 P3).

**Rule 9 — Timing side-channel disclosure.**
The zero-copy peek pair (`key`, `value`, `get`) permits fine-grained timing measurement of per-entry accesses, which is a stronger side-channel than the bulk visitor pattern of `sdb_index_visit`. In multi-tenant deployments where cache warmth, overflow-chain resolution, or page-decrypt cost could leak information about plaintext content, callers should treat cursor navigation as timing-sensitive. This is not a nonce-reuse or AAD break; it is a per-call cost variance disclosure that comes with any zero-copy iterator API.

---

## 6. Scan sugar: prefix and range

The scan APIs are implemented as thin wrappers around `sdb_cursor_open` + `sdb_cursor_seek` + a bounded walk. They are provided because (a) they are the most common cursor pattern by an order of magnitude, and (b) exposing them as first-class removes the "compute the successor of a prefix" footgun.

### 6.1 Scan visitor typedef

```c
/**
 * Visitor callback for scan_prefix / scan_range. Invoked once per matching
 * entry in the configured direction.
 *
 * Buffers key and value are borrowed for the duration of the callback ONLY.
 * The visitor must copy any bytes it wishes to retain.
 *
 * @return true to continue, false to stop early. Stopping early is not an
 *         error; the scan returns SDB_OK.
 *
 * The visitor MUST NOT call sdb_database_close on the walking handle.
 * Doing so returns SDB_E_BUSY (enforced via the same callback_depth
 * counter used by sdb_index_visit — see §9). Auto-commit scans hold no
 * write lock; calling a mutating API on the same handle from the visitor
 * returns SDB_E_BUSY. Transactional scans permit mutations on the same
 * transaction; the transactional walker sees the txn's write set merged
 * with the underlying committed state (see §6.3 for the merge rules).
 *
 * Idempotency contract for side-effecting visitors: the scan API does NOT
 * promise atomicity between visitor invocation and match_count_out update.
 * If the visitor performs external side effects (writing to another
 * database, sending network messages), the caller MUST make those side
 * effects idempotent (e.g., keyed by the entry's key) so that a
 * process-crash-and-retry does not double-apply them. There is no
 * resume-token API; callers who need one use sdb_cursor_dup on a raw
 * cursor and re-seek to the last processed key.
 */
typedef bool (*sdb_scan_visit_fn)(
    void *context,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);
```

### 6.2 Scan options

```c
/**
 * Options for sdb_kv_scan_*. Zero-init via sdb_scan_options_init.
 *
 * reverse: iterate in descending key order.
 * limit:   stop after at most this many visitor invocations. Zero = unlimited.
 */
typedef struct sdb_scan_options {
    uint32_t struct_size;
    bool reverse;
    uint8_t reserved_alignment[3];
    uint64_t limit;
    uint64_t reserved[4];
} sdb_scan_options;

SDB_API void sdb_scan_options_init(sdb_scan_options *options);
```

### 6.3 KV prefix and range scans

**Auto-commit variants** open an ephemeral snapshot for the duration of the call, iterate, close the snapshot, and return. Semantically they are `sdb_snapshot_open` + `sdb_cursor_open` + walk + close. They hold `database->mutex` only across the snapshot-open, and release it while the visitor runs — the visitor sees a stable snapshot even if concurrent writers commit during the scan, and mutations from other threads on the same handle are permitted while the visitor is running (they will not appear in this scan's results).

**Transactional variants** iterate against the transaction's write set merged with the underlying committed state. The merge is spelled out below because a naive walker will leak plaintext from tombstoned entries.

**Merge algorithm (transactional scans).** For every candidate entry `(K, V_committed)` yielded by the underlying committed-state cursor:

1. Look up `K` in the transaction's write set.
2. If the txn recorded a **tombstone** (delete) for `K`, SKIP the entry — do NOT invoke the visitor.
3. If the txn recorded a **replacement** `(K, V_txn)`, invoke the visitor with `(K, V_txn)`. Do NOT invoke with `V_committed`.
4. Otherwise, invoke the visitor with `(K, V_committed)`.

Then, for every key `K_txn` in the transaction's write set that is **within the scan's bounds but was not encountered by the underlying cursor** (a txn insert of a key that doesn't exist in the committed state), invoke the visitor with the txn's value. Insertions are delivered in the correct sort position via a merge-of-sorted-streams; the walker holds the txn's write set as a sorted view for this purpose.

The reverse-direction merge is identical with the sort order flipped.

```c
/**
 * Auto-commit prefix scan. Enumerates every KV entry in the namespace whose
 * key begins with prefix, in the direction configured by options.
 *
 * @param prefix                  Prefix bytes; may be NULL iff prefix_size == 0.
 *                                Zero-length prefix scans the whole namespace.
 * @param options                 May be NULL for default (forward, unlimited).
 * @param match_count_out         Optional. On success or early stop, receives
 *                                the number of visitor invocations that
 *                                occurred. On error, unmodified.
 *                                Note: this is INVOCATION count, including
 *                                the invocation that returned false to stop.
 */
SDB_API sdb_status sdb_kv_scan_prefix(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *prefix,
    size_t prefix_size,
    const sdb_scan_options *options,
    sdb_scan_visit_fn visitor,
    void *context,
    size_t *match_count_out
);

/**
 * Auto-commit range scan over half-open [lower_bound, upper_bound).
 * Either bound may be NULL (with a zero size) to leave that end unbounded.
 *
 * Empty range (lower_bound == upper_bound as byte sequences) is valid and
 * returns SDB_OK with match_count_out = 0.
 *
 * @retval SDB_E_INVALID_ARGUMENT    Any argument invalid, or lower_bound >
 *                                   upper_bound as byte sequences (empty
 *                                   range with lower == upper is OK).
 */
SDB_API sdb_status sdb_kv_scan_range(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *lower_bound,
    size_t lower_bound_size,
    const uint8_t *upper_bound,
    size_t upper_bound_size,
    const sdb_scan_options *options,
    sdb_scan_visit_fn visitor,
    void *context,
    size_t *match_count_out
);

/**
 * Transactional prefix scan. Sees the transaction's uncommitted writes
 * merged with the committed base state per the merge algorithm in §6.3.
 * A transactional scan on a POISONED transaction (any prior op returned
 * a fatal error on this txn) returns SDB_E_TXN_POISONED — NOT SDB_E_BUSY —
 * so callers can distinguish "retry later" from "this txn is dead".
 *
 * @retval SDB_OK                    Scan completed.
 * @retval SDB_E_INVALID_ARGUMENT    Any argument invalid.
 * @retval SDB_E_TXN_POISONED        Transaction has encountered a fatal
 *                                   error and must be rolled back.
 * @retval SDB_E_BUSY                Another callback is already running on
 *                                   this handle (reentry guard).
 * @retval SDB_E_NOT_FOUND           Namespace does not exist in the
 *                                   transaction's merged view.
 * @retval SDB_E_OUT_OF_MEMORY       Allocation failed.
 * @retval SDB_E_IO                  Pager read failure.
 * @retval SDB_E_CORRUPT             B-tree invariant violated.
 */
SDB_API sdb_status sdb_transaction_kv_scan_prefix(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *prefix,
    size_t prefix_size,
    const sdb_scan_options *options,
    sdb_scan_visit_fn visitor,
    void *context,
    size_t *match_count_out
);

/**
 * Transactional range scan. Same semantics as sdb_kv_scan_range with the
 * merge rules of §6.3 applied, and the SDB_E_TXN_POISONED distinction from
 * sdb_transaction_kv_scan_prefix.
 */
SDB_API sdb_status sdb_transaction_kv_scan_range(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *lower_bound,
    size_t lower_bound_size,
    const uint8_t *upper_bound,
    size_t upper_bound_size,
    const sdb_scan_options *options,
    sdb_scan_visit_fn visitor,
    void *context,
    size_t *match_count_out
);
```

`SDB_E_TXN_POISONED` is a new status code added to `sdb_status` in the same version bump as this proposal. It is returned only by transaction-scoped APIs and preserves the "poisoned transaction rejects further work" invariant that `SDB_E_BUSY` would otherwise mask.

### 6.4 On not shipping a cursor-factory sugar

An earlier draft included `sdb_kv_cursor_open_prefix(...) -> sdb_cursor*`. It was cut because the cursor primitive already covers the case (open a cursor with `lower_bound=prefix`, `upper_bound=next(prefix)`), and shipping the factory as a stable symbol would lock us into it before we know whether prefix scans want their own bloom filter or prefix extractor optimization. Callers who want a raw cursor over a prefix use `sdb_cursor_open` directly.

---

## 7. Extended index visitor: range variant

`sdb_index_visit` currently walks all documents whose index entry equals a single value. `sdb_index_visit_range` generalises to a half-open range, matching the KV scan APIs. Existing `sdb_index_visit` is unchanged.

```c
/**
 * Enumerate documents whose index entry falls in [lower_value, upper_value).
 * Ascending by index-value (with document_id as secondary sort). Either
 * bound may be NULL/0-sized for unbounded.
 *
 * Auto-commit: opens an ephemeral snapshot for the walk. The document_id
 * buffer is borrowed for the callback only. Reentry rules match
 * sdb_index_visit.
 *
 * See §6.1 idempotency contract for visitor side effects.
 */
SDB_API sdb_status sdb_index_visit_range(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    const uint8_t *lower_value,
    size_t lower_value_size,
    const uint8_t *upper_value,
    size_t upper_value_size,
    sdb_index_visit_fn visitor,
    void *context,
    size_t *match_count_out
);
```

A transactional index visitor is deferred to a follow-up proposal (it requires threading the transaction's index-mutation batch into the walker; out of scope here).

---

## 8. Catalog and document enumeration

Catalog listings (namespaces, collections, indexes) are small — dozens to hundreds. Both a callback shape and a fill-buffer shape are provided; the callback matches the existing `sdb_index_visit` culture, and the fill-buffer removes the "malloc-in-visitor" dance for the common case.

Document enumeration within a collection is provided via a snapshot cursor (`sdb_collection_document_visit`) so that backup and export tooling do not need to synthesize a covering index.

### 8.1 Enumeration visitor typedefs

```c
/**
 * Visitor for sdb_list_namespaces_visit / sdb_list_collections_visit.
 * Delivers (name, kind) — kind distinguishes KV vs BLOB for namespaces
 * and is always SDB_KEYSPACE_UNSPECIFIED for collections.
 */
typedef bool (*sdb_name_visit_fn)(
    void *context,
    sdb_keyspace_kind kind,
    const uint8_t *name,
    size_t name_size
);

/**
 * Visitor for sdb_list_indexes_visit.
 */
typedef bool (*sdb_index_descriptor_visit_fn)(
    void *context,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
);

/**
 * Visitor for sdb_collection_document_visit — delivers each document_id
 * and its payload (borrowed for the callback).
 */
typedef bool (*sdb_document_visit_fn)(
    void *context,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document_payload,
    size_t document_payload_size
);
```

### 8.2 Visitor-form and fill-buffer-form list APIs

```c
/**
 * Callback form: enumerate namespaces matching filter, ascending
 * byte-lex order (secondary by kind: KV before BLOB at equal name).
 */
SDB_API sdb_status sdb_list_namespaces_visit(
    sdb_database *database,
    sdb_list_filter filter,
    sdb_name_visit_fn visitor,
    void *context,
    size_t *total_count_out
);

/**
 * Fill-buffer form: write all namespace names into a single contiguous
 * buffer as (kind: uint8, name_size: uint32 LE, name_bytes...) records.
 * Zero-copy for the caller: one allocation, one call, no visitor.
 *
 * Probe by passing capacity=0 and reading *bytes_needed_out.
 *
 * @retval SDB_OK                    Buffer filled.
 * @retval SDB_E_BUFFER_TOO_SMALL    capacity insufficient; *bytes_needed_out set.
 */
SDB_API sdb_status sdb_list_namespaces_into(
    sdb_database *database,
    sdb_list_filter filter,
    uint8_t *buffer,
    size_t capacity,
    size_t *bytes_needed_out,
    size_t *count_out
);

SDB_API sdb_status sdb_list_collections_visit(
    sdb_database *database,
    sdb_name_visit_fn visitor,
    void *context,
    size_t *total_count_out
);

SDB_API sdb_status sdb_list_collections_into(
    sdb_database *database,
    uint8_t *buffer,
    size_t capacity,
    size_t *bytes_needed_out,
    size_t *count_out
);

SDB_API sdb_status sdb_list_indexes_visit(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    sdb_index_descriptor_visit_fn visitor,
    void *context,
    size_t *total_count_out
);

SDB_API sdb_status sdb_list_indexes_into(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    uint8_t *buffer,
    size_t capacity,
    size_t *bytes_needed_out,
    size_t *count_out
);

/**
 * Enumerate every document in a collection, ascending by document_id.
 * Snapshot-scoped: opens an ephemeral snapshot for the walk. This is the
 * general-purpose enumeration for backup/export tooling and does not
 * require a covering index.
 *
 * Warning: on large collections this walks the entire collection subtree.
 * For filtered enumeration, use sdb_index_visit_range on a covering index.
 */
SDB_API sdb_status sdb_collection_document_visit(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    sdb_document_visit_fn visitor,
    void *context,
    size_t *total_count_out
);
```

The visitor form's `total_count_out` reports the number of entries **delivered to the visitor**, including the invocation that returned false to stop. This is invocation-count, not full-catalog-count; callers who need the full count either iterate to completion or use the `_into` form's `count_out` which is always the full catalog count.

---

## 9. Concurrency and transaction interaction

The engine lock model applies unchanged to every new function. Concrete rules, per API:

| API family | Handle mutex | Reader-floor | Interaction with active transaction |
|---|---|---|---|
| `sdb_snapshot_open` | Briefly | Reserves | No conflict. |
| `sdb_transaction_snapshot_open` | Briefly | Reserves | Snapshot tied to txn lifetime; invalidated on commit/rollback. |
| `sdb_snapshot_close` | Briefly | Releases | No conflict. |
| `sdb_cursor_*` navigation | Not held | Inherited from snapshot | No conflict. |
| `sdb_kv_scan_*` (auto-commit) | Briefly on setup | Ephemeral reserve/release | Does NOT return `SDB_E_BUSY` when a txn is active — asymmetric with writes, same as `sdb_kv_get`. Visitor sees last-committed durable state (i.e., state that would survive a crash at this instant — reads sample `checkpoint_lsn` after WAL fsync, before superblock swap only if the pager exposes the committed WAL frame; otherwise the read is delayed until the superblock reflects the commit). |
| `sdb_transaction_kv_scan_*` | Via `sdb_transaction_lock` | Inherited | Requires active, non-poisoned txn. Merge rules per §6.3. |
| `sdb_index_visit_range` | Via `SDB_ENGINE_LOCK_OR_RETURN` | Ephemeral snapshot | Same reentry semantics as `sdb_index_visit`. |
| `sdb_list_*_visit` / `sdb_list_*_into` | Briefly | Not required (catalog reads are metadata) | Auto-commit; no conflict with active txn; does not see txn's uncommitted `sdb_transaction_index_create`. |
| `sdb_collection_document_visit` | Briefly on setup | Ephemeral reserve/release | Auto-commit, same as `sdb_kv_scan_*`. |

**Reentry guard.** The visitor of any callback API is executed with the handle's `callback_depth` counter bumped. Attempts to call mutating APIs on the same handle from the visitor return `SDB_E_BUSY`. Attempts to close the database return `SDB_E_BUSY`. Attempts to call another callback API (e.g., nested `sdb_kv_scan_prefix` from a visitor) also return `SDB_E_BUSY` — the reentry guard is depth-1, not full reentrance. This is a deliberate footgun-reduction over an "unlimited nesting" design.

**Committed-vs-durable clarification.** For every auto-commit read/scan/list API, "sees committed state" is defined as: sees any commit `C` for which `wal.fsync(C)` has completed. This is the *durable* state — the state that would survive a crash at this instant. It is a superset of the *superblock-visible* state during the pager.c commit window (fsync-then-superblock-swap). Concretely, if a writer has fsynced commit `C` at LSN `L` but not yet swapped the superblock, an auto-commit reader started *now* observes `C`. This choice removes the "read may or may not see C during the commit window" ambiguity of the v1 draft.

---

## 10. Example: minimal cursor loop and minimal scan

Common-case cursor loop, forward, unbounded:
```c
sdb_snapshot *snap = NULL;
sdb_cursor *cur = NULL;
sdb_status s;

s = sdb_snapshot_open(db, &snap);
if (s != SDB_OK) goto out;

s = sdb_cursor_open(snap, SDB_KEYSPACE_KV, ns, ns_len, NULL, &cur);
if (s != SDB_OK) goto out;

for (s = sdb_cursor_first(cur); s == SDB_OK && sdb_cursor_valid(cur); s = sdb_cursor_next(cur)) {
    const uint8_t *k, *v;
    size_t kl, vl;
    if (sdb_cursor_get(cur, &k, &kl, &v, &vl) != SDB_OK) break;
    /* use k/v within this iteration only */
}

out:
    sdb_cursor_close(cur);      /* NULL-tolerant */
    sdb_snapshot_close(snap);   /* NULL-tolerant, invalidates cur if still open */
```

Common-case scan (5 lines of actual logic):
```c
static bool dump(void *ctx, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl) {
    fwrite(k, 1, kl, stdout); fputc('\t', stdout);
    fwrite(v, 1, vl, stdout); fputc('\n', stdout);
    return true;
}
sdb_kv_scan_range(db, ns, ns_len, NULL, 0, NULL, 0, NULL, dump, NULL, NULL);
```

Paged scan with limit (no stateful visitor):
```c
sdb_scan_options opt;
sdb_scan_options_init(&opt);
opt.limit = 100;
sdb_kv_scan_prefix(db, ns, ns_len, prefix, prefix_len, &opt, dump, NULL, NULL);
```

---

## 11. Rejected refinements

The following pieces of adversarial feedback were considered and deliberately NOT incorporated. Each is called out here so future readers do not silently reintroduce the change.

**Verify-lens findings NOT addressed:**

- **"Path B implicit snapshot readers can read a page whose plaintext envelope was zeroed by close" (verify #1).** Correct and important — but this is a critique of the *concurrency proposal*, not the cursor proposal. This proposal responds by declaring §2 Invariant P4 (read-in-progress refcount for scalar reads, distinct from `callback_depth`). The refcount must be implemented in the pager; enforcing it is out of scope for this proposal's text.
- **"Path B checkpoint/WAL-truncation admits a page_lsn/AAD mismatch under in-place page store" (verify #2).** Correct. This proposal responds by explicitly declaring §2 Invariant P1: this cursor API is incompatible with a redo-only + in-place-writeback pager. The concurrency proposal must ship either copy-on-write or true WAL semantics with pre-image retention. If it ships neither, this proposal is blocked.
- **"Path A thread-local plaintext side-cache reintroduces zeroization hazard" (verify #3).** Correct and important, but again a critique of the concurrency proposal. Noted in §2 by dependency; not resolved here.

**Crash-consistency findings NOT addressed as written:**

- **CC-2 "match_count_out semantics after mid-scan crash undefined — caller cannot resume idempotently."** Partially incorporated: §6.1 now documents the idempotency contract for visitor side effects and directs callers to `sdb_cursor_dup` + re-seek for resumption. A first-class "resume token" API was rejected because it would leak internal cursor state into the ABI; the primitive `sdb_cursor_dup` plus caller-side memoization of the last processed key is sufficient. Adding a resume-token would prematurely commit to a specific WAL-frame identification scheme.
- **CC-6 "Reverse-iteration reservation is a forward-compat crash risk."** Moot after this revision — reverse iteration is fully shipped in v1 with the required snapshot-floor discipline (§2 P1/P2), so there is no future-work window in which a partial reverse implementation could ship.

**Design-lens findings NOT incorporated:**

- **"sdb_cursor_read has 7 parameters — drop it entirely."** Rejected. The 7-parameter shape mirrors `sdb_kv_get` exactly, which is the point: callers already know this pattern. Dropping `sdb_cursor_read` in favor of peek+memcpy would force every caller who wants owned bytes to write the two-call dance manually. Peek+get+read is a three-tool set with distinct use cases (zero-copy hot path, atomic peek, owned copy); shipping all three costs nothing.
- **"sdb_cursor_read does not advance the cursor — add sdb_cursor_read_next."** Rejected for v1. The peek pair + `sdb_cursor_next` composition is 3 calls per iteration; `sdb_cursor_read` is intended for the rarer "I want owned bytes and I'll advance on my schedule" case. Adding a combined `read_next` doubles the surface for a marginal ergonomic gain and can be added in a later minor bump without breaking anyone.
- **"sdb_cursor_count for pre-sizing result buffers."** Rejected for v1. An accurate count requires a full walk, and an inaccurate estimate is a footgun (callers will treat it as accurate). The fill-buffer `_into` APIs' probe-then-fill pattern already covers the pre-sizing need for enumeration; for KV scans, the two-pass problem is real but not worth committing to a specific estimation scheme in the ABI.
- **"Reverse variants of sdb_kv_scan_prefix / sdb_kv_scan_range as separate symbols."** Addressed by folding `reverse` into `sdb_scan_options`, not by adding new symbols. The user-visible feature is shipped; the ABI stays minimal.
- **"Snapshots should be shareable across threads like LMDB read txns."** Rejected. LMDB's read-txn thread-sharing is a well-known footgun in practice (callers accidentally share MDB_txn across threads and hit races in the txn's dirty-page list). ShibaDB's snapshot-per-thread rule is stricter but simpler and matches Bolt.
- **"Callback-only scan visitor's `MUST NOT call sdb_database_close` — enforce or specify."** Enforced via the reentry guard documented in §9 (`callback_depth` returns `SDB_E_BUSY`). The text was strengthened; the design was not changed.
- **"SDB_KEYSPACE_ANY = 0 conflicts with sdb_cursor_open rejecting 0."** Fixed in a way the reviewer did not suggest: `sdb_keyspace_kind` now has an explicit `SDB_KEYSPACE_UNSPECIFIED = 0` that is universally rejected, and catalog filtering uses a separate bit-flag enum `sdb_list_filter`. One rule, no overloading.
- **"Snapshot handle bounds are borrowed for cursor lifetime — footgun."** Fixed by copying bound bytes at `sdb_cursor_open` time (§5.2 doc updated). The reviewer suggested a separate `sdb_cursor_set_bounds()` call; we chose the copy-at-open because it keeps the options struct as the single configuration surface and callers do not need to remember a separate call.
