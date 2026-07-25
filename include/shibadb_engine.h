/**
 * @file shibadb_engine.h
 * @brief ShibaDB — high-level engine API: databases, transactions,
 *        KV, blobs, documents, and indexes.
 *
 * This header layers a document-oriented interface on top of the
 * low-level primitives in `shibadb.h`. Everything declared here is
 * ABI-stable within the current `SDB_ENGINE_API_VERSION`.
 *
 * @section handles Handles and ownership
 *
 * `sdb_database` and `sdb_transaction` are opaque handles. The
 * library owns their storage; callers must free them exclusively
 * via `sdb_database_close` / `sdb_transaction_close`. Every
 * handle returned via an out-parameter is caller-owned; the
 * library never retains a reference to caller-supplied storage.
 *
 * All byte-buffer parameters (`namespace_name`, `key`, `value`,
 * `collection`, `document_id`, `document`) are borrowed for the
 * duration of the call only. They are UTF-8-agnostic — every API
 * treats them as opaque octet sequences; storage is binary-safe.
 *
 * @section threading Threading model
 *
 * A single `sdb_database` handle may be shared across threads;
 * the library serialises operations on it via an internal
 * recursive mutex. Only one handle (across processes) may own a
 * given database path at a time; a competing open returns
 * `SDB_E_BUSY`. Transactions are NOT thread-safe: a
 * `sdb_transaction *` must not be touched from any thread other
 * than the one that called `sdb_transaction_begin`.
 *
 * @section errors Errors and rollback
 *
 * Every function returns an `sdb_status`. On any transactional
 * failure other than `SDB_E_INVALID_ARGUMENT` on the transaction
 * pointer itself, the transaction is poisoned; subsequent calls
 * on the same transaction return `SDB_E_INVALID_ARGUMENT` until
 * the caller invokes `sdb_transaction_rollback` +
 * `sdb_transaction_close`.
 *
 * @section budget Transaction budget
 *
 * Every transaction has two ceilings: `SDB_TRANSACTION_MAX_OPERATIONS`
 * caps the number of mutating operations, and
 * `SDB_TRANSACTION_MAX_LOGICAL_BYTES` caps the total bytes written.
 * Exhausting either budget fails the offending call with
 * `SDB_E_OVERFLOW`.
 */
#ifndef SHIBADB_ENGINE_H
#define SHIBADB_ENGINE_H

#include "shibadb.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Version of this engine API (distinct from the ABI
 *        version in `shibadb.h`).
 *
 * The engine surface is guaranteed source- and binary-compatible
 * across minor bumps within the same value; a bump indicates a
 * breaking change and requires a recompile.
 */
#define SDB_ENGINE_API_VERSION UINT32_C(1)

/**
 * @deprecated Historical alias for `SDB_ENGINE_API_VERSION`. Do
 *             not use; will be removed in a future major version.
 */
#define SDB_ENGINE_API_EXPERIMENTAL SDB_ENGINE_API_VERSION

/**
 * Default logical page size (4 KiB) used when
 * `sdb_database_options::page_size` is left at zero.
 */
#define SDB_ENGINE_DEFAULT_PAGE_SIZE UINT32_C(4096)

/**
 * Maximum byte length for names supplied to the engine —
 * `namespace_name`, `key`, `collection`, `document_id`, and
 * `index_name`. Longer inputs fail with
 * `SDB_E_INVALID_ARGUMENT`.
 */
#define SDB_ENGINE_MAX_NAME_SIZE ((size_t)1024)

/** Maximum number of mutating operations in one transaction. */
#define SDB_TRANSACTION_MAX_OPERATIONS ((size_t)10000)

/** Maximum total logical bytes written by one transaction. */
#define SDB_TRANSACTION_MAX_LOGICAL_BYTES ((size_t)67108864)

/** Opaque handle to an open database. */
typedef struct sdb_database sdb_database;

/** Opaque handle to an in-flight transaction. */
typedef struct sdb_transaction sdb_transaction;

/**
 * @brief Options accepted by `sdb_database_create` and
 *        `sdb_database_open`.
 *
 * Zero-initialise via `sdb_database_options_init` before setting
 * fields; the `struct_size` field lets the library detect
 * forward/backward compatibility. The `reserved` array is a
 * forward-compat pad and must remain zero.
 *
 * A non-empty `password` at create time turns on encryption
 * (`SDB_FLAG_ENCRYPTED`). Open MUST then supply the same
 * password bytes verbatim.
 */
typedef struct sdb_database_options {
    /** Set by `sdb_database_options_init` — do not change. */
    uint32_t struct_size;
    /**
     * Logical page size for a new database. Ignored on open.
     * Zero picks `SDB_ENGINE_DEFAULT_PAGE_SIZE`.
     */
    uint32_t page_size;
    /**
     * PBKDF2 iteration count for a new encrypted database.
     * Ignored on open (the value is read from the superblock).
     * Zero picks `SDB_DEFAULT_KDF_ITERATIONS`. Bounded by
     * `[SDB_MIN_KDF_ITERATIONS, SDB_MAX_KDF_ITERATIONS]`.
     */
    uint32_t kdf_iterations;
    /** Password bytes, or NULL for plaintext. Borrowed for the call only. */
    const uint8_t *password;
    /** Password length in bytes. Must be 0 iff `password` is NULL. */
    size_t password_size;
    /** Forward-compat pad; must remain zero. */
    uint64_t reserved[4];
} sdb_database_options;

/**
 * @brief One term to index for a document.
 *
 * Passed as an array to `sdb_transaction_document_put` /
 * `sdb_document_put`. The buffers are borrowed for the call only.
 */
typedef struct sdb_index_term {
    /** Index name — matches an existing `sdb_transaction_index_create`. */
    const uint8_t *index_name;
    /** Length of `index_name`. */
    size_t index_name_size;
    /** Value to bind under `index_name` for this document. */
    const uint8_t *value;
    /** Length of `value`. */
    size_t value_size;
} sdb_index_term;

/**
 * @brief Physical + logical statistics returned by
 *        `sdb_database_verify`.
 *
 * Callers may zero-initialise before the call; the library writes
 * every field. Populated only on success.
 */
typedef struct sdb_verify_result {
    /** Number of pages currently in use. */
    uint64_t allocated_page_count;
    /** Number of free pages on the freelist. */
    uint64_t free_page_count;
    /** Total B-tree internal + leaf node count. */
    uint64_t btree_node_count;
    /** B-tree leaf-only count. */
    uint64_t btree_leaf_count;
    /** Raw entries stored, including tombstones. */
    uint64_t raw_entry_count;
    /** Logical live objects (KV + blob + document). */
    uint64_t object_count;
    /** Live blob chunks referenced by an object. */
    uint64_t live_chunk_count;
    /** Tombstoned entries pending compaction. */
    uint64_t stale_entry_count;
    /** Total logical byte count across all live objects. */
    uint64_t logical_byte_count;
    /** Height of the primary B-tree. */
    uint32_t btree_height;
    /** Alignment pad, always zero. */
    uint32_t reserved_alignment;
    /** Forward-compat pad. */
    uint64_t reserved[3];
} sdb_verify_result;

/**
 * @brief Statistics returned by `sdb_database_backup`.
 */
typedef struct sdb_backup_result {
    /** Total bytes written to the destination file. */
    uint64_t byte_count;
    /** Raw entries copied into the backup. */
    uint64_t raw_entry_count;
    /** Forward-compat pad. */
    uint64_t reserved[3];
} sdb_backup_result;

/**
 * @brief Statistics returned by `sdb_database_compact` and
 *        `sdb_database_migrate`.
 */
typedef struct sdb_compact_result {
    /** File size before compaction. */
    uint64_t byte_count_before;
    /** File size after compaction. */
    uint64_t byte_count_after;
    /** Raw entry count before (including tombstones). */
    uint64_t raw_entries_before;
    /** Raw entry count after (live only). */
    uint64_t raw_entries_after;
    /** Forward-compat pad. */
    uint64_t reserved[4];
} sdb_compact_result;

/**
 * @brief Callback invoked for each document matching an index
 *        lookup via `sdb_index_visit`.
 *
 * The callback returns `true` to continue iteration, `false` to
 * stop early. The document_id buffer is valid only for the
 * duration of the callback; copy it if you need to retain it.
 *
 * @param context Caller-supplied context pointer.
 * @param document_id Non-owned bytes identifying the matched doc.
 * @param document_id_size Length of `document_id`.
 * @return `true` to continue, `false` to stop iteration.
 */
typedef bool (*sdb_index_visit_fn)(
    void *context, const uint8_t *document_id, size_t document_id_size
);

/**
 * @brief Initialise an options struct to library defaults.
 *
 * Sets `struct_size` and zeroes every other field. Callers MUST
 * call this before touching an `sdb_database_options`; forgetting
 * it means the library cannot detect struct-size mismatch and
 * may misinterpret trailing bytes.
 *
 * @param options Non-NULL destination.
 */
SDB_API void sdb_database_options_init(sdb_database_options *options);

/**
 * @brief Create a fresh database at `path`.
 *
 * Fails if `path` already exists. On success, `*database_out`
 * receives a handle the caller must eventually free via
 * `sdb_database_close`. On any failure the output is left NULL
 * and no file remains on disk.
 *
 * @param path         Non-NULL, valid path in a writable directory.
 * @param options      Non-NULL, initialised via `sdb_database_options_init`.
 * @param database_out Non-NULL out-parameter.
 * @retval SDB_OK on success.
 * @retval SDB_E_INVALID_ARGUMENT for NULL pointers, bad
 *         page_size, or malformed options.
 * @retval SDB_E_IO if a syscall fails or `path` already exists.
 */
SDB_API sdb_status sdb_database_create(
    const char *path,
    const sdb_database_options *options,
    sdb_database **database_out
);

/**
 * @brief Open an existing database at `path`.
 *
 * Runs `.replace` marker recovery, WAL replay, and superblock
 * mirror reconciliation before returning. On success,
 * `*database_out` receives a handle the caller must free via
 * `sdb_database_close`.
 *
 * @param path         Non-NULL path to an existing database.
 * @param options      Non-NULL, initialised. `password` must
 *                     match the create-time password when the
 *                     database was created with encryption.
 * @param database_out Non-NULL out-parameter.
 * @retval SDB_OK on success.
 * @retval SDB_E_NOT_FOUND if `path` does not exist.
 * @retval SDB_E_BUSY if another handle already holds the path.
 * @retval SDB_E_AUTHENTICATION for wrong password.
 * @retval SDB_E_CORRUPT if on-disk data fails validation
 *         (includes stored KDF iterations below the current
 *         minimum — see `ENCRYPTION.md`).
 * @retval SDB_E_UNSUPPORTED_VERSION if the file was written by
 *         a newer major version.
 */
SDB_API sdb_status sdb_database_open(
    const char *path,
    const sdb_database_options *options,
    sdb_database **database_out
);

/**
 * @brief Close a database handle and release its resources.
 *
 * Any active transaction on this handle is rolled back before
 * close returns. Passing NULL is currently `SDB_E_INVALID_ARGUMENT`
 * — callers that want a NULL-safe close should guard it
 * themselves.
 *
 * @param database Handle previously returned by `create` or `open`.
 * @retval SDB_OK on success.
 * @retval SDB_E_INVALID_ARGUMENT for a NULL handle.
 * @retval SDB_E_IO on final fsync failure.
 */
SDB_API sdb_status sdb_database_close(sdb_database *database);

/**
 * @brief Walk every page, authenticate every payload, and
 *        reconcile every index.
 *
 * A "deep verify" — reads and cryptographically validates every
 * page. Expensive for large databases. Use before backup and
 * after suspected corruption.
 *
 * @param database   Non-NULL open handle.
 * @param result_out Non-NULL; populated on success.
 * @retval SDB_OK if the database is internally consistent.
 * @retval SDB_E_CORRUPT if any check fails; caller should treat
 *         the file as compromised.
 */
SDB_API sdb_status sdb_database_verify(
    sdb_database *database, sdb_verify_result *result_out
);

/**
 * @brief Copy the database to `destination_path` via the online
 *        backup API — no downtime, transactionally consistent.
 *
 * The destination is created if `replace_existing` is true and
 * an existing file will be overwritten atomically. Otherwise, an
 * existing destination fails with `SDB_E_IO`.
 *
 * @param database         Non-NULL open handle.
 * @param destination_path Non-NULL target path.
 * @param replace_existing Whether to allow overwriting.
 * @param result_out       Optional; may be NULL.
 * @retval SDB_OK on success.
 * @retval SDB_E_IO on any syscall failure.
 */
SDB_API sdb_status sdb_database_backup(
    sdb_database *database,
    const char *destination_path,
    bool replace_existing,
    sdb_backup_result *result_out
);

/**
 * @brief Rewrite the database, compacting freed pages and
 *        applying `target_options` (page size, KDF work factor,
 *        password rotation).
 *
 * A no-op-safe way to change page_size or rotate the password.
 * Atomic: on success, the file at the database path IS the
 * compacted version, and the pre-compact file has been unlinked.
 *
 * @param database       Non-NULL open handle.
 * @param target_options Non-NULL options for the new file.
 * @param result_out     Optional; may be NULL.
 * @retval SDB_OK on success.
 * @retval SDB_E_INVALID_ARGUMENT if `target_options` would remove
 *         encryption from a currently-encrypted database (guard
 *         against silent downgrade).
 * @retval SDB_E_IO on any syscall failure; original file intact.
 */
SDB_API sdb_status sdb_database_compact(
    sdb_database *database,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
);

/**
 * @brief Migrate the database on-disk format to
 *        `target_format_version`.
 *
 * Currently only `SDB_FORMAT_VERSION_V1` is supported; the call
 * exists for forward compatibility.
 *
 * @param database              Non-NULL open handle.
 * @param target_format_version Target format version code.
 * @param target_options        Non-NULL options for the migrated file.
 * @param result_out            Optional; may be NULL.
 * @retval SDB_OK on success.
 * @retval SDB_E_UNSUPPORTED_VERSION if the target is unknown.
 */
SDB_API sdb_status sdb_database_migrate(
    sdb_database *database,
    uint16_t target_format_version,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
);

/**
 * @name Explicit transactions
 *
 * Start a transaction with `begin`, mutate via the
 * `sdb_transaction_*` calls below, then finalise with either
 * `commit` or `rollback`, and free the handle with `close`.
 *
 * A transaction handle is not thread-safe; touch it only from
 * the thread that opened it.
 * @{
 */

/**
 * @brief Begin a transaction on `database`.
 *
 * @param database        Non-NULL open handle.
 * @param transaction_out Non-NULL; receives the transaction
 *                        handle on success.
 * @retval SDB_OK on success.
 * @retval SDB_E_BUSY if a competing writer holds the database.
 */
SDB_API sdb_status sdb_transaction_begin(
    sdb_database *database, sdb_transaction **transaction_out
);

/**
 * @brief Durably apply the transaction's changes.
 *
 * On success the caller MUST still free the transaction handle
 * with `sdb_transaction_close`. On failure the transaction is
 * poisoned; call `rollback` then `close`.
 *
 * @param transaction Non-NULL active transaction.
 * @retval SDB_OK on success.
 * @retval SDB_E_CONFLICT if a concurrent commit invalidated this
 *         one — retry from scratch.
 * @retval SDB_E_OVERFLOW if the transaction exceeded its budget.
 */
SDB_API sdb_status sdb_transaction_commit(sdb_transaction *transaction);

/**
 * @brief Discard the transaction's changes without applying.
 *
 * Always succeeds structurally; the returned status reflects the
 * cleanup path only. Caller must still `close`.
 *
 * @param transaction Non-NULL transaction handle.
 * @retval SDB_OK on clean rollback.
 */
SDB_API sdb_status sdb_transaction_rollback(sdb_transaction *transaction);

/**
 * @brief Free the transaction handle.
 *
 * If neither `commit` nor `rollback` was called first, this
 * behaves as an implicit rollback.
 *
 * @param transaction Non-NULL transaction handle.
 * @retval SDB_OK on success.
 */
SDB_API sdb_status sdb_transaction_close(sdb_transaction *transaction);

/** @} */

/**
 * @name Transactional KV operations
 *
 * Namespace-scoped byte-oriented map. `namespace_name` and `key`
 * are opaque; both are size-bounded by `SDB_ENGINE_MAX_NAME_SIZE`.
 * @{
 */

/**
 * @brief Insert or overwrite a KV pair inside a transaction.
 *
 * @retval SDB_OK on success.
 * @retval SDB_E_INVALID_ARGUMENT for NULL / oversize keys.
 * @retval SDB_E_OVERFLOW on transaction budget exhaustion.
 */
SDB_API sdb_status sdb_transaction_kv_put(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

/**
 * @brief Look up a KV pair inside a transaction.
 *
 * On success writes the value into `value_out` (up to
 * `value_capacity`) and reports the actual size in
 * `*value_size_out`. If the buffer is too small, returns
 * `SDB_E_BUFFER_TOO_SMALL` with `*value_size_out` set to the
 * required size so callers can retry.
 *
 * `value_out` may be NULL and `value_capacity` 0 to probe size.
 *
 * @retval SDB_OK on hit.
 * @retval SDB_E_NOT_FOUND on miss.
 * @retval SDB_E_BUFFER_TOO_SMALL when `value_capacity` is
 *         insufficient; `*value_size_out` still receives the
 *         required size.
 */
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

/**
 * @brief Remove a KV pair inside a transaction.
 *
 * @retval SDB_OK on delete (or if the key was absent — deletion
 *         is idempotent).
 */
SDB_API sdb_status sdb_transaction_kv_delete(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

/** @} */

/**
 * @name Transactional blob operations
 *
 * Same shape as KV but keyed for values that may exceed one page.
 * The engine transparently chunks large blobs across multiple
 * pages.
 * @{
 */

/** @brief Insert or overwrite a blob inside a transaction. */
SDB_API sdb_status sdb_transaction_blob_put(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

/**
 * @brief Look up a blob inside a transaction.
 *
 * Semantics of `value_out` / `value_capacity` / `value_size_out`
 * mirror `sdb_transaction_kv_get`.
 */
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

/** @brief Remove a blob inside a transaction. */
SDB_API sdb_status sdb_transaction_blob_delete(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

/** @} */

/**
 * @name Transactional document + index operations
 *
 * Documents live inside collections and may have index terms
 * that support `sdb_index_visit` lookups.
 * @{
 */

/**
 * @brief Register an index for a collection.
 *
 * Idempotent: creating an index that already exists with matching
 * `unique` returns `SDB_OK`. Mismatched `unique` returns
 * `SDB_E_CONFLICT`.
 *
 * @param unique When true, subsequent puts fail with
 *               `SDB_E_CONFLICT` if a term binds a value already
 *               held by another document.
 */
SDB_API sdb_status sdb_transaction_index_create(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
);

/**
 * @brief Insert or overwrite a document with an index-term list.
 *
 * Every entry in `terms[0 .. term_count)` must reference an
 * `index_name` previously created for the same `collection`.
 * Unknown index names fail with `SDB_E_NOT_FOUND`.
 */
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

/**
 * @brief Look up a document by id inside a transaction.
 *
 * Semantics of `document_out` / `document_capacity` /
 * `document_size_out` mirror `sdb_transaction_kv_get`.
 */
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

/** @brief Remove a document (and all its index terms). */
SDB_API sdb_status sdb_transaction_document_delete(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
);

/** @} */

/**
 * @name Auto-commit convenience wrappers
 *
 * Each of these is exactly equivalent to `begin` + the matching
 * `sdb_transaction_*` call + `commit` + `close`, with proper
 * cleanup on error paths. Use them for simple single-write
 * scripts; use explicit transactions when you need to batch.
 * @{
 */

/** @brief Auto-commit `sdb_transaction_kv_put`. */
SDB_API sdb_status sdb_kv_put(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

/** @brief Auto-commit `sdb_transaction_kv_get`. */
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

/** @brief Auto-commit `sdb_transaction_kv_delete`. */
SDB_API sdb_status sdb_kv_delete(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

/** @brief Auto-commit `sdb_transaction_blob_put`. */
SDB_API sdb_status sdb_blob_put(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);

/** @brief Auto-commit `sdb_transaction_blob_get`. */
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

/** @brief Auto-commit `sdb_transaction_blob_delete`. */
SDB_API sdb_status sdb_blob_delete(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
);

/** @brief Auto-commit `sdb_transaction_index_create`. */
SDB_API sdb_status sdb_index_create(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
);

/** @brief Auto-commit `sdb_transaction_document_put`. */
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

/** @brief Auto-commit `sdb_transaction_document_get`. */
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

/** @brief Auto-commit `sdb_transaction_document_delete`. */
SDB_API sdb_status sdb_document_delete(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
);

/**
 * @brief Iterate documents whose `index_name` term equals
 *        `value`.
 *
 * The `visitor` callback is invoked once per live INDEX ENTRY
 * matching (name, value), NOT once per distinct document.
 * Returning `false` stops iteration. On completion,
 * `*match_count_out` receives the number of matches visited
 * (including the last one before an early stop).
 *
 * @section reentry Mutation during the walk
 *
 * The walk holds the database lock but is REENTRANCY-SAFE for a
 * limited set of operations that the callback may perform on the
 * same database handle:
 *
 *   - Insert a new document: if its new index entry sorts after
 *     the walk's current resume point, it will be yielded before
 *     the walk finishes.
 *   - Update an existing document (same or different indexed
 *     value): the OLD index entry is filtered out by the
 *     stale-generation check; the NEW entry, if still matching,
 *     is yielded again — so an update of an already-yielded
 *     document produces two yields for the same document_id. If
 *     the caller needs to see each document at most once, track
 *     document_ids in the visitor context.
 *   - Delete an existing document: the walk continues past the
 *     deleted entry cleanly.
 *   - `sdb_database_compact` or `sdb_database_migrate`: the walk
 *     releases the cursor before the callback, re-seeks on the
 *     full resume key afterwards, and picks up the rebuilt index
 *     transparently.
 *
 * The callback MUST NOT call `sdb_database_close` on the walking
 * handle — that returns `SDB_E_BUSY` (the walk holds
 * `callback_depth != 0`). Nested `sdb_index_visit` calls on the
 * same handle are permitted but each recursion consumes one
 * `callback_depth` slot.
 *
 * @retval SDB_OK on success.
 * @retval SDB_E_NOT_FOUND if the index does not exist.
 */
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

/** @} */

#ifdef __cplusplus
}
#endif

#endif
