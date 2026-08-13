
#ifndef SHIBADB_ENGINE_H
#define SHIBADB_ENGINE_H

#include "shibadb.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDB_ENGINE_API_VERSION UINT32_C(2)

#define SDB_ENGINE_API_EXPERIMENTAL SDB_ENGINE_API_VERSION

#define SDB_ENGINE_DEFAULT_PAGE_SIZE UINT32_C(4096)

#define SDB_ENGINE_MAX_NAME_SIZE ((size_t)1024)

/*
 * Hard caps per explicit transaction, bounding the memory and WAL a single
 * commit can consume. Exceeding EITHER the operation count or the accumulated
 * logical byte count deterministically returns SDB_E_OVERFLOW — split a larger
 * unit of work across multiple transactions. See docs/PUBLIC_TRANSACTIONS.md.
 */
#define SDB_TRANSACTION_MAX_OPERATIONS ((size_t)10000)

#define SDB_TRANSACTION_MAX_LOGICAL_BYTES ((size_t)67108864)

typedef struct sdb_database sdb_database;

typedef struct sdb_transaction sdb_transaction;

/* Commit durability modes for sdb_database_options.synchronous. */
#define SDB_SYNCHRONOUS_FULL 0U
#define SDB_SYNCHRONOUS_NORMAL 1U

/*
 * Sentinel for sdb_database_options.cache_bytes: size the page cache
 * automatically from the machine's physical RAM (a bounded fraction, clamped
 * to a sane floor/ceiling) instead of the small fixed default. Use when the
 * database is the primary consumer of a box with plenty of RAM.
 */
#define SDB_CACHE_AUTO ((uint64_t)0xFFFFFFFFFFFFFFFFULL)

typedef struct sdb_database_options {

    uint32_t struct_size;

    uint32_t page_size;

    uint32_t kdf_iterations;

    const uint8_t *password;

    size_t password_size;

    /*
     * Page-cache budget in bytes (0 = engine default). Claimed from the
     * head of the former reserved block, so sizeof and every prior field
     * offset are unchanged: callers compiled against the old layout leave
     * these bytes zeroed, which reads back as "default" here.
     */
    uint64_t cache_bytes;

    /*
     * Commit durability mode (0 = default). Claimed from the reserved block
     * like cache_bytes above, so sizeof and every prior offset are unchanged
     * and old callers read back 0 == SDB_SYNCHRONOUS_FULL.
     *
     *   SDB_SYNCHRONOUS_FULL   (0): every auto-commit fsyncs the WAL before it
     *                               is acknowledged — durable across power loss.
     *   SDB_SYNCHRONOUS_NORMAL (1): auto-commits are acknowledged after the WAL
     *                               pwrite but before fsync; durability is
     *                               folded into the next checkpoint. Commits
     *                               survive a process crash (bytes are in the OS
     *                               page cache) but the most recent ones may be
     *                               lost on sudden power loss. Either way there
     *                               is no corruption — checkpoint fsyncs the WAL
     *                               before copying frames into the data file, so
     *                               recovery discards any torn tail. Explicit
     *                               transactions stay fully durable regardless.
     */
    uint64_t synchronous;

    uint64_t reserved[2];
} sdb_database_options;

typedef struct sdb_index_term {

    const uint8_t *index_name;

    size_t index_name_size;

    const uint8_t *value;

    size_t value_size;
} sdb_index_term;

typedef struct sdb_verify_result {

    uint64_t allocated_page_count;

    uint64_t free_page_count;

    uint64_t btree_node_count;

    uint64_t btree_leaf_count;

    uint64_t raw_entry_count;

    uint64_t object_count;

    uint64_t live_chunk_count;

    uint64_t stale_entry_count;

    uint64_t logical_byte_count;

    uint32_t btree_height;

    uint32_t reserved_alignment;

    uint64_t reserved[3];
} sdb_verify_result;

typedef struct sdb_backup_result {

    uint64_t byte_count;

    uint64_t raw_entry_count;

    uint64_t reserved[3];
} sdb_backup_result;

typedef struct sdb_compact_result {

    uint64_t byte_count_before;

    uint64_t byte_count_after;

    uint64_t raw_entries_before;

    uint64_t raw_entries_after;

    uint64_t reserved[4];
} sdb_compact_result;

/**
 * struct sdb_info_result - O(1) configuration snapshot of an open database.
 * @struct_size:         sizeof(sdb_info_result); lets the struct grow compatibly.
 * @page_size:           Page size in bytes.
 * @encrypted:           True when the database uses authenticated encryption.
 * @reserved_alignment:  Padding; always zero.
 * @kdf_iterations:      PBKDF2 work factor (0 when @encrypted is false).
 * @generation:          Monotonic superblock generation (bumped every commit).
 * @checkpoint_lsn:      Log-sequence number of the most recent checkpoint.
 * @page_count:          Highest page id ever allocated (file high-water mark).
 * @wal_size_bytes:      Current WAL byte tail (bytes written since the last
 *                       checkpoint reset); compare against
 *                       @checkpoint_threshold_bytes to decide when to checkpoint.
 * @checkpoint_threshold_bytes: WAL byte size at which the engine triggers a
 *                       deferred checkpoint.
 * @freelist_head_page:  Head page id of the on-disk freelist, or 0 when no
 *                       pages are free. This is the freelist HEAD, not a count;
 *                       a nonzero value means compact can reclaim space. For an
 *                       exact free-page or stale-entry count use
 *                       sdb_database_verify (a full scan).
 * @reserved:            Reserved for future fields; always zero.
 *
 * Unlike sdb_verify_result, this is read straight from the in-memory
 * superblock/pager: it costs O(1), never walks the tree, and never touches
 * disk. wal_size_bytes, checkpoint_threshold_bytes and freelist_head_page are
 * all in-memory reads with the same guarantee.
 */
typedef struct sdb_info_result {
    uint32_t struct_size;
    uint32_t page_size;
    bool encrypted;
    uint8_t reserved_alignment[3];
    uint32_t kdf_iterations;
    uint64_t generation;
    uint64_t checkpoint_lsn;
    uint64_t page_count;
    uint64_t wal_size_bytes;
    uint64_t checkpoint_threshold_bytes;
    uint64_t freelist_head_page;
    uint64_t reserved[1];
} sdb_info_result;

typedef bool (*sdb_index_visit_fn)(
    void *context, const uint8_t *document_id, size_t document_id_size
);

SDB_API void sdb_database_options_init(sdb_database_options *options);

SDB_API sdb_status sdb_database_create(
    const char *path,
    const sdb_database_options *options,
    sdb_database **database_out
);

SDB_API sdb_status sdb_database_open(
    const char *path,
    const sdb_database_options *options,
    sdb_database **database_out
);

/*
 * Close a database handle. SDB_E_INVALID_ARGUMENT and SDB_E_BUSY are early
 * returns and do not consume the handle; after removing the busy condition the
 * caller may retry. Every other result, including a late checkpoint, pager, or
 * lock-release error, consumes the handle and it must not be reused.
 */
SDB_API sdb_status sdb_database_close(sdb_database *database);

SDB_API sdb_status sdb_database_verify(
    sdb_database *database, sdb_verify_result *result_out
);

/**
 * sdb_database_info() - Read an open database's configuration snapshot.
 * @database:   An open database handle.
 * @result_out: Filled with the snapshot on success.
 *
 * O(1) health / configuration probe: takes the handle lock, copies fields from
 * the in-memory superblock, and returns. It does not scan the tree — use
 * sdb_database_verify() for deep structural counters.
 *
 * Return: SDB_OK on success; SDB_E_INVALID_ARGUMENT if @database is NULL or not
 * open, or if @result_out is NULL.
 */
SDB_API sdb_status sdb_database_info(
    sdb_database *database, sdb_info_result *result_out
);

SDB_API sdb_status sdb_database_backup(
    sdb_database *database,
    const char *destination_path,
    bool replace_existing,
    sdb_backup_result *result_out
);

SDB_API sdb_status sdb_database_compact(
    sdb_database *database,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
);

SDB_API sdb_status sdb_database_migrate(
    sdb_database *database,
    uint16_t target_format_version,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
);

/*
 * Upgrade a V1 database file to the current V2 on-disk format, writing the
 * result to a NEW destination path (the source is left untouched). This is the
 * only supported way to read data written by a pre-2026-08 build, whose object
 * key layout the current engine refuses to open (SDB_E_UNSUPPORTED_VERSION).
 *
 * source_options carries the source's password (to decrypt it); target_options
 * carries the destination's (which may differ, rotating the key). The source
 * must be a V1 file — a V2 (or unknown) source returns SDB_E_UNSUPPORTED_VERSION
 * and nothing is written. On any failure the partial destination is removed.
 *
 * The rewrite transforms only object/chunk keys; it does not reclaim stale
 * index rows a churned source may hold. Run sdb_database_compact on the
 * destination afterwards if you need those reclaimed.
 */
SDB_API sdb_status sdb_database_migrate_file(
    const char *source_path,
    const char *destination_path,
    const sdb_database_options *source_options,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
);

SDB_API sdb_status sdb_transaction_begin(
    sdb_database *database, sdb_transaction **transaction_out
);

/*
 * A return before the database is acquired (SDB_E_INVALID_ARGUMENT or an early
 * SDB_E_BUSY) does not change the handle's prior state. Once commit acquires the
 * database, it is terminal regardless of its returned status. A late I/O error
 * may be reported after the WAL commit record is
 * durable; therefore an error does not prove the transaction was absent after
 * recovery. Use an application-level idempotency key when retrying externally
 * visible work.
 */
SDB_API sdb_status sdb_transaction_commit(sdb_transaction *transaction);

/* Rollback is terminal on success; close the inactive handle afterward. */
SDB_API sdb_status sdb_transaction_rollback(sdb_transaction *transaction);

/*
 * Frees an inactive transaction handle. An active handle returns SDB_E_BUSY
 * without consuming it; commit or roll it back first, then retry close.
 */
SDB_API sdb_status sdb_transaction_close(sdb_transaction *transaction);

SDB_API sdb_status sdb_transaction_kv_put(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

SDB_API sdb_status sdb_transaction_kv_get(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);

SDB_API sdb_status sdb_transaction_kv_delete(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

SDB_API sdb_status sdb_transaction_blob_put(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

SDB_API sdb_status sdb_transaction_blob_get(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);

SDB_API sdb_status sdb_transaction_blob_delete(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

SDB_API sdb_status sdb_transaction_index_create(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
);

SDB_API sdb_status sdb_transaction_document_put(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size,
    const sdb_index_term *terms,
    size_t term_count
);

SDB_API sdb_status sdb_transaction_document_get(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    uint8_t *document_out,
    size_t document_capacity,
    size_t *document_size_out
);

SDB_API sdb_status sdb_transaction_document_delete(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
);

SDB_API sdb_status sdb_kv_put(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

SDB_API sdb_status sdb_kv_get(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);

SDB_API sdb_status sdb_kv_delete(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

SDB_API sdb_status sdb_blob_put(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

SDB_API sdb_status sdb_blob_get(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);

SDB_API sdb_status sdb_blob_delete(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

SDB_API sdb_status sdb_index_create(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
);

SDB_API sdb_status sdb_document_put(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size,
    const sdb_index_term *terms,
    size_t term_count
);

SDB_API sdb_status sdb_document_get(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    uint8_t *document_out,
    size_t document_capacity,
    size_t *document_size_out
);

SDB_API sdb_status sdb_document_delete(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
);

SDB_API sdb_status sdb_index_visit(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    const uint8_t *value,
    size_t value_size,
    sdb_index_visit_fn visitor,
    void *context,
    size_t *match_count_out
);

/*
 * ============================================================================
 * Scan / cursor / enumerate (append-only, SDB_ENGINE_API_VERSION 1).
 * See docs/PROPOSAL_CURSOR_SCAN_ENUMERATE.md. This first slice ships
 * forward-only KV cursors, prefix scan sugar, and read snapshots. Reverse
 * iteration and blob/collection enumeration are added by later slices; the
 * declarations here are stable and additive.
 * ============================================================================
 */

/*
 * Which keyspace a cursor iterates. UNSPECIFIED (zero) is always rejected so a
 * zero-initialised argument fails closed.
 */
typedef enum sdb_keyspace_kind {
    SDB_KEYSPACE_UNSPECIFIED = 0,
    SDB_KEYSPACE_KV = 1,
    SDB_KEYSPACE_BLOB = 2
} sdb_keyspace_kind;

/*
 * Opaque read snapshot: holds the handle's active-session slot for its whole
 * lifetime (writer exclusion == snapshot isolation under the single-mutex
 * model). While one is live, sdb_transaction_begin, a second sdb_snapshot_open,
 * sdb_database_backup / _compact / _migrate, sdb_database_close, and every
 * mutating op on the same handle return SDB_E_BUSY.
 */
typedef struct sdb_snapshot sdb_snapshot;

/*
 * Opaque forward cursor over one keyspace + one namespace within a snapshot.
 * A cursor MUST be closed before the snapshot it was opened on: sdb_snapshot_close
 * returns SDB_E_BUSY while any cursor on it is still open (the cursor relies on
 * the snapshot's slot to keep writers out of the tree it is walking).
 */
typedef struct sdb_cursor sdb_cursor;

/*
 * Cursor open options. Zero-initialise via sdb_cursor_options_init. Defaults
 * iterate the whole namespace in ascending byte-lexicographic order.
 * lower_bound / upper_bound define an optional half-open range [lower, upper);
 * both are COPIED into cursor storage at open time (callers may free/reuse the
 * buffers immediately). limit: if non-zero, the cursor becomes invalid after
 * `limit` successful positioning steps. `reverse` is reserved for a later
 * slice and MUST be false for now (non-false is rejected).
 */
typedef struct sdb_cursor_options {
    uint32_t struct_size;
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

/* Scan sugar options. `reverse` reserved (must be false for now). */
typedef struct sdb_scan_options {
    uint32_t struct_size;
    bool reverse;
    uint8_t reserved_alignment[3];
    uint64_t limit;
    uint64_t reserved[4];
} sdb_scan_options;

SDB_API void sdb_scan_options_init(sdb_scan_options *options);

/*
 * Visitor for scan sugar. Return true to continue, false to stop early.
 * The key/value pointers are borrowed for the call only.
 */
typedef bool (*sdb_scan_visit_fn)(
    void *context,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

/*
 * Open a read snapshot on the handle. Takes the active-session slot; fails with
 * SDB_E_BUSY if a transaction, another snapshot, or an in-flight commit holds
 * it. Must be closed with sdb_snapshot_close (NULL-tolerant, returns SDB_OK).
 */
SDB_API sdb_status sdb_snapshot_open(
    sdb_database *database,
    sdb_snapshot **snapshot_out
);

/*
 * Close a snapshot and release its writer-excluding slot. NULL-tolerant
 * (returns SDB_OK). Returns SDB_E_BUSY, changing nothing, if any cursor opened
 * on it is still open — close every cursor first.
 */
SDB_API sdb_status sdb_snapshot_close(sdb_snapshot *snapshot);

/* Diagnostic monotonic version (== committed generation observed). NULL -> 0. */
SDB_API uint64_t sdb_snapshot_version(const sdb_snapshot *snapshot);

/*
 * Open a forward cursor over keyspace_kind + namespace within snapshot. After
 * open the cursor is UNPOSITIONED: sdb_cursor_valid is false and sdb_cursor_next
 * returns SDB_E_INVALID_ARGUMENT until sdb_cursor_first or sdb_cursor_seek is
 * called. options may be NULL (defaults).
 */
SDB_API sdb_status sdb_cursor_open(
    sdb_snapshot *snapshot,
    sdb_keyspace_kind keyspace_kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const sdb_cursor_options *options,
    sdb_cursor **cursor_out
);

/*
 * Position at the first in-range entry (SDB_OK; sdb_cursor_valid false if the
 * range is empty).
 */
SDB_API sdb_status sdb_cursor_first(sdb_cursor *cursor);

/*
 * Lower-bound seek: position at the first entry with key >= the given key
 * (within the cursor's namespace/range).
 */
SDB_API sdb_status sdb_cursor_seek(
    sdb_cursor *cursor,
    const uint8_t *key,
    size_t key_size
);

/*
 * Advance to the next entry. On a positioned cursor at the last entry, leaves
 * the cursor invalid and returns SDB_OK. On an unpositioned cursor returns
 * SDB_E_INVALID_ARGUMENT.
 */
SDB_API sdb_status sdb_cursor_next(sdb_cursor *cursor);

/* True iff the cursor is positioned on a live in-range entry. NULL -> false. */
SDB_API bool sdb_cursor_valid(const sdb_cursor *cursor);

/* Last status produced by a navigation call (SDB_OK when never failed). */
SDB_API sdb_status sdb_cursor_status(const sdb_cursor *cursor);

/*
 * Zero-copy peek at the current key. The pointer is valid only until the next
 * state-mutating call on this cursor. Fails if unpositioned.
 */
SDB_API sdb_status sdb_cursor_key(
    const sdb_cursor *cursor,
    const uint8_t **key_out,
    size_t *key_size_out
);

/* Zero-copy peek at the current value. Same lifetime rules as sdb_cursor_key. */
SDB_API sdb_status sdb_cursor_value(
    const sdb_cursor *cursor,
    const uint8_t **value_out,
    size_t *value_size_out
);

/* Peek key and value in one call. */
SDB_API sdb_status sdb_cursor_get(
    const sdb_cursor *cursor,
    const uint8_t **key_out,
    size_t *key_size_out,
    const uint8_t **value_out,
    size_t *value_size_out
);

/*
 * Copy the current key and value into caller buffers. Mirrors sdb_kv_get:
 * *size_out are written before the capacity check (probe with capacity 0).
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

SDB_API sdb_status sdb_cursor_close(sdb_cursor *cursor);

/*
 * Auto-commit prefix scan over one KV namespace. Opens an ephemeral snapshot
 * (takes the active-session slot for the whole walk -> writer-blocking under
 * the single-mutex model), invokes visitor for every key beginning with prefix
 * in ascending order. prefix may be NULL iff prefix_size == 0 (whole namespace).
 * options may be NULL. match_count_out is optional.
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

/*
 * Visitor for namespace enumeration. keyspace_kind is the object kind that owns
 * the namespace: 1 = KV, 2 = blob, 3 = document. Return true to continue, false
 * to stop early. The namespace pointer is borrowed for the call only.
 */
typedef bool (*sdb_namespace_visit_fn)(
    void *context,
    uint16_t keyspace_kind,
    const uint8_t *namespace_name,
    size_t namespace_size
);

/*
 * Enumerate the distinct (keyspace_kind, namespace) pairs that hold at least
 * one object, in ascending on-disk order, each reported once. This is the
 * "list the tables" primitive: a KV/blob/document namespace appears iff it
 * currently has data. Opens an ephemeral read snapshot (writer-blocking under
 * the single-mutex model) for the walk. count_out is optional.
 */
SDB_API sdb_status sdb_list_namespaces(
    sdb_database *database,
    sdb_namespace_visit_fn visitor,
    void *context,
    size_t *count_out
);

/*
 * ============================================================================
 * Convenience API (SDB_ENGINE_API_VERSION 2). Additive, ergonomic wrappers
 * over the primitives above: allocating reads, existence and count, atomic
 * put-if-absent / compare-and-swap / increment, one-call atomic batches. None
 * of these change the on-disk format or touch the audited pager/WAL/B+Tree —
 * they compose the public KV, scan, and transaction entry points.
 * ============================================================================
 */

/**
 * sdb_free() - Release a buffer returned by an allocating shibadb call.
 * @pointer: A buffer handed back by sdb_kv_get_alloc(), or NULL.
 *
 * Frees with the same allocator shibadb allocated it from. This matters when
 * the application and the library link different C runtimes (e.g. a Windows
 * DLL): freeing a library allocation with the app's free() is undefined.
 * NULL is a no-op.
 */
SDB_API void sdb_free(void *pointer);

/**
 * sdb_kv_get_alloc() - Fetch a KV value into a newly allocated buffer.
 * @database:        An open database handle.
 * @namespace_name:  KV namespace.
 * @namespace_size:  Length of @namespace_name in bytes.
 * @key:             Key to look up.
 * @key_size:        Length of @key in bytes.
 * @value_out:       Receives a malloc'd buffer with the value; free with
 *                   sdb_free(). Set to NULL and size 0 on any non-OK return.
 * @value_size_out:  Receives the value length.
 *
 * Removes the two-call "probe the size, then read" dance of sdb_kv_get(). A
 * zero-length value yields a valid non-NULL 1-byte buffer with size 0.
 *
 * Return: SDB_OK; SDB_E_NOT_FOUND; SDB_E_OUT_OF_MEMORY; SDB_E_INVALID_ARGUMENT.
 */
SDB_API sdb_status sdb_kv_get_alloc(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t **value_out,
    size_t *value_size_out
);

/**
 * sdb_kv_exists() - Test whether a KV key is present without copying its value.
 * @exists_out: Receives true iff the key exists.
 *
 * Return: SDB_OK (regardless of presence); SDB_E_INVALID_ARGUMENT.
 */
SDB_API sdb_status sdb_kv_exists(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    bool *exists_out
);

/**
 * sdb_kv_count() - Count all keys in a KV namespace.
 * @count_out: Receives the number of keys.
 *
 * Walks a read snapshot of the namespace. Return: SDB_OK; SDB_E_INVALID_ARGUMENT.
 */
SDB_API sdb_status sdb_kv_count(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    uint64_t *count_out
);

/**
 * sdb_kv_count_prefix() - Count keys in a namespace beginning with a prefix.
 * @prefix:      Prefix to match; may be NULL iff @prefix_size is 0 (count all).
 * @count_out:   Receives the number of matching keys.
 */
SDB_API sdb_status sdb_kv_count_prefix(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *prefix,
    size_t prefix_size,
    uint64_t *count_out
);

/**
 * sdb_kv_put_if_absent() - Store a value only if the key does not yet exist.
 *
 * Atomic (single transaction). Return: SDB_OK on write; SDB_E_CONFLICT if the
 * key already exists (nothing written).
 */
SDB_API sdb_status sdb_kv_put_if_absent(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

/**
 * sdb_kv_compare_and_swap() - Replace a value only if it equals @expected.
 * @expected:      Bytes the current value must equal for the swap to happen.
 * @desired:       Bytes to store when the comparison succeeds.
 *
 * Atomic (single transaction). Return: SDB_OK on swap; SDB_E_CONFLICT if the
 * key is absent or the current value differs (nothing written).
 */
SDB_API sdb_status sdb_kv_compare_and_swap(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *expected,
    size_t expected_size,
    const uint8_t *desired,
    size_t desired_size
);

/**
 * sdb_kv_increment() - Atomically add @delta to an 8-byte little-endian counter.
 * @delta:          Signed amount to add.
 * @new_value_out:  Receives the counter value after the add (optional, may be
 *                  NULL).
 *
 * A missing key is treated as 0 and created. Atomic (single transaction).
 * Return: SDB_OK; SDB_E_INVALID_ARGUMENT if the existing value is not exactly
 * 8 bytes; SDB_E_OVERFLOW if the signed addition would wrap.
 */
SDB_API sdb_status sdb_kv_increment(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    int64_t delta,
    int64_t *new_value_out
);

/* Operation kind for sdb_batch_op. */
typedef enum sdb_batch_op_kind {
    SDB_BATCH_OP_KV_PUT = 1,
    SDB_BATCH_OP_KV_DELETE = 2
} sdb_batch_op_kind;

/**
 * struct sdb_batch_op - One operation in an sdb_kv_batch_apply() batch.
 * @kind:            An sdb_batch_op_kind value.
 * @reserved_alignment: Padding; set to 0.
 * @namespace_name:  KV namespace.
 * @namespace_size:  Length of @namespace_name.
 * @key:             Key to write or delete.
 * @key_size:        Length of @key.
 * @value:           Value bytes (ignored, may be NULL, for a delete).
 * @value_size:      Length of @value.
 */
typedef struct sdb_batch_op {
    uint32_t kind;
    uint32_t reserved_alignment;
    const uint8_t *namespace_name;
    size_t namespace_size;
    const uint8_t *key;
    size_t key_size;
    const uint8_t *value;
    size_t value_size;
} sdb_batch_op;

/**
 * sdb_kv_batch_apply() - Apply many KV puts/deletes in one atomic transaction.
 * @ops:       Array of operations, applied in order.
 * @op_count:  Number of operations.
 *
 * All-or-nothing: if any operation fails the whole batch is rolled back and the
 * failing status is returned. Subject to the per-transaction caps
 * (SDB_TRANSACTION_MAX_OPERATIONS, SDB_TRANSACTION_MAX_LOGICAL_BYTES), which
 * report SDB_E_OVERFLOW. An empty batch (op_count 0) is a no-op returning
 * SDB_OK.
 */
SDB_API sdb_status sdb_kv_batch_apply(
    sdb_database *database,
    const sdb_batch_op *ops,
    size_t op_count
);

/*
 * Visitor for sdb_index_query_documents(). Receives both the matching
 * document id and its full body (both borrowed for the call only). Return true
 * to continue, false to stop early.
 */
typedef bool (*sdb_document_visit_fn)(
    void *context,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size
);

/**
 * sdb_index_query_documents() - Find documents by an exact secondary-index value.
 * @database:         An open database handle.
 * @collection:       Document collection.
 * @collection_size:  Length of @collection.
 * @index_name:       Secondary index to look up (created with sdb_index_create).
 * @index_name_size:  Length of @index_name.
 * @value:            Index value to match exactly.
 * @value_size:       Length of @value.
 * @visitor:          Called once per matching document with its id and body.
 * @context:          Opaque pointer passed to @visitor.
 * @match_count_out:  Optional; receives the number of documents visited.
 *
 * This is the "find({field: value})" primitive: sdb_index_visit() only yields
 * document ids, so callers otherwise fetch each body themselves. It collects
 * the matching ids, then reads each body — a document deleted between the two
 * steps is silently skipped (best-effort, not a single atomic snapshot).
 *
 * Return: SDB_OK; SDB_E_OUT_OF_MEMORY; SDB_E_INVALID_ARGUMENT; or a read error.
 */
SDB_API sdb_status sdb_index_query_documents(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    const uint8_t *value,
    size_t value_size,
    sdb_document_visit_fn visitor,
    void *context,
    size_t *match_count_out
);

#ifdef __cplusplus
}
#endif

#endif
