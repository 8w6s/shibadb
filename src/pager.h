#ifndef SHIBADB_PAGER_H
#define SHIBADB_PAGER_H

#include "file.h"
#include "crypto.h"
#include "page.h"
#include "page_cache.h"
#include "sync.h"
#include "wal.h"
#include "wal_index.h"

#define SDB_PAGER_CACHE_CAPACITY 64U

/*
 * Bounds for a byte-configured page cache (sdb_database_options.cache_bytes).
 * MIN keeps a tiny request from degenerating into a single-frame cache;
 * MAX caps the pinned RAM a caller can request (~1M frames, e.g. 4 GiB at a
 * 4 KiB page size). A request of 0 bytes keeps the historical default.
 */
#define SDB_PAGER_CACHE_CAPACITY_MIN ((size_t)8)
#define SDB_PAGER_CACHE_CAPACITY_MAX ((size_t)1 << 20)

/*
 * Default checkpoint threshold: ~1000 pages worth of raw WAL bytes.
 * The threshold governs when the deferred-checkpoint trigger fires
 * (wired in Task 5). In WAL-mode this dispatch we only track the
 * threshold; the trigger itself is not yet plumbed in.
 */
#define SDB_PAGER_CHECKPOINT_THRESHOLD_PAGES 1000U

typedef struct sdb_pager {
    sdb_file file;
    sdb_superblock_v1 superblock;
    sdb_page_cache cache;
    sdb_wal_index wal_index;
    char *wal_path;
    /*
     * Persistent WAL fd (R2a). Held open across commits instead of the former
     * open()+close() per commit. Opened lazily on the first commit (a
     * read-only session or a never-committed DB creates no .wal), reset to
     * the header offset on checkpoint/clear, re-pointed at the swapped-in file
     * during compact, and closed by sdb_pager_close. wal_file is only valid
     * when wal_file_open is true.
     */
    sdb_file wal_file;
    bool wal_file_open;
    uint8_t data_key[SDB_CRYPTO_KEY_SIZE];
    uint64_t wal_tail;
    uint64_t current_lsn;
    uint64_t checkpoint_threshold;
    /*
     * Monotonic object-generation counter (ABA fix). Never reuses a value for
     * the lifetime of the database, so a deleted-then-recreated document can
     * never collide with its own stale index/chunk/guard rows. Persisted as a
     * system row in the btree (see SDB_SEQUENCE_KEY_PREFIX in engine.c) so it
     * rides the WAL/recovery path automatically; this field is the in-memory
     * cache seeded at open. Read/advanced only under database->mutex (commit
     * PREPARE is serialized there), so no separate lock is needed.
     */
    uint64_t next_object_generation;
    /*
     * R4 group-commit coordinator. Standard "one shared fsync" design: the
     * PREPARE phase (under database->mutex) encodes a txn, PWRITES its WAL
     * bytes with NO fsync (fast, buffered — so read-through-WAL still finds
     * the bytes on disk), advances all in-memory state, and bumps
     * `written_lsn`. The DURABILITY phase runs off the engine mutex: the first
     * thread with an unsynced txn becomes leader, snapshots `written_lsn`,
     * fsyncs ONCE (covering every txn pwritten so far), and advances
     * `durable_lsn`; concurrent committers pile onto the same fsync as
     * followers waiting on `commit_cond`.
     *
     * `commit_mutex` guards written_lsn / durable_lsn / leader_active / the
     * failure flags and pairs with `commit_cond`. It is a SEPARATE lock from
     * database->mutex; PREPARE nests it briefly (order always
     * database->mutex ⊃ commit_mutex — never the reverse), DURABILITY takes
     * commit_mutex only (engine mutex released) so other threads can PREPARE
     * during the fsync. `defer_commit` / `pending_*` are touched only under
     * database->mutex (single preparer at a time) so the engine wrapper learns
     * which txn to drive to durability after releasing the engine mutex.
     */
    sdb_mutex commit_mutex;
    sdb_cond commit_cond;
    uint64_t written_lsn;
    uint64_t durable_lsn;
    sdb_status commit_error;
    bool leader_active;
    bool commit_failed;
    bool commit_coord_ready;
    bool defer_commit;
    bool pending_valid;
    uint64_t pending_txn_id;
    bool open;
    bool needs_recovery;
    bool transaction_active;
    bool encryption_enabled;
} sdb_pager;

typedef struct sdb_txn_page {
    uint64_t page_id;
    uint16_t type;
    uint8_t *payload;
    size_t payload_size;
} sdb_txn_page;

typedef struct sdb_txn {
    sdb_pager *pager;
    sdb_superblock_v1 target_superblock;
    sdb_txn_page *pages;
    size_t page_count;
    size_t page_capacity;
    uint64_t *allocated_pages;
    size_t allocated_page_count;
    size_t allocated_page_capacity;
    bool active;
} sdb_txn;

sdb_status sdb_pager_create(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    sdb_pager *pager_out
);
sdb_status sdb_pager_create_ex(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    size_t cache_bytes,
    sdb_pager *pager_out
);
sdb_status sdb_pager_open(const char *path, sdb_pager *pager_out);
/*
 * Cache-byte-aware variants. `cache_bytes == 0` selects the default
 * capacity; otherwise the frame count is cache_bytes/page_size clamped to
 * [SDB_PAGER_CACHE_CAPACITY_MIN, SDB_PAGER_CACHE_CAPACITY_MAX]. The engine
 * uses these to honour sdb_database_options.cache_bytes; the plain
 * open/create wrappers keep the default and are unchanged for other callers.
 */
sdb_status sdb_pager_open_ex(
    const char *path, size_t cache_bytes, sdb_pager *pager_out
);
sdb_status sdb_pager_open_encrypted_ex(
    const char *path,
    const uint8_t *password,
    size_t password_size,
    size_t cache_bytes,
    sdb_pager *pager_out
);
/*
 * Translate a cache_bytes request into a frame capacity. Exposed for
 * testing; see the clamp contract above.
 */
size_t sdb_pager_cache_capacity(size_t cache_bytes, size_t page_size);
sdb_status sdb_pager_create_encrypted(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    const uint8_t *password,
    size_t password_size,
    uint32_t kdf_iterations,
    sdb_pager *pager_out
);
sdb_status sdb_pager_create_encrypted_ex(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    const uint8_t *password,
    size_t password_size,
    uint32_t kdf_iterations,
    size_t cache_bytes,
    sdb_pager *pager_out
);
sdb_status sdb_pager_open_encrypted(
    const char *path,
    const uint8_t *password,
    size_t password_size,
    sdb_pager *pager_out
);
sdb_status sdb_pager_rotate_password(
    sdb_pager *pager,
    const uint8_t *new_password,
    size_t new_password_size,
    uint32_t new_kdf_iterations
);
size_t sdb_pager_payload_capacity(const sdb_pager *pager);
sdb_status sdb_pager_close(sdb_pager *pager);
sdb_status sdb_pager_store_superblock(
    sdb_pager *pager, const sdb_superblock_v1 *next
);
sdb_status sdb_pager_checkpoint(sdb_pager *pager);
sdb_status sdb_pager_allocate(sdb_pager *pager, uint64_t *page_id_out);
sdb_status sdb_pager_free(sdb_pager *pager, uint64_t page_id);
sdb_status sdb_pager_write(
    sdb_pager *pager,
    uint64_t page_id,
    uint16_t type,
    uint64_t page_lsn,
    const uint8_t *payload,
    size_t payload_size
);
sdb_status sdb_pager_read(
    sdb_pager *pager,
    uint64_t page_id,
    uint8_t *page_buffer,
    size_t page_buffer_size,
    sdb_page_view *view_out
);
sdb_status sdb_txn_begin(sdb_pager *pager, sdb_txn *txn_out);
sdb_status sdb_txn_put(
    sdb_txn *txn,
    uint64_t page_id,
    uint16_t type,
    const uint8_t *payload,
    size_t payload_size
);
sdb_status sdb_txn_allocate(sdb_txn *txn, uint64_t *page_id_out);
sdb_status sdb_txn_free(sdb_txn *txn, uint64_t page_id);
sdb_status sdb_txn_commit(sdb_txn *txn);
void sdb_txn_abort(sdb_txn *txn);

/*
 * R4 group commit — engine-level entry points.
 *
 * sdb_pager_set_defer_commit: arm/disarm the deferred-commit mode for the
 *   current PREPARE. Must be called under database->mutex. When armed, a
 *   subsequent sdb_txn_commit stages the txn (encode + in-memory index/cache/
 *   superblock advance) and enqueues it WITHOUT fsync, then reports the txn id
 *   via sdb_pager_take_pending; when disarmed sdb_txn_commit keeps its
 *   original synchronous single-fsync behaviour (explicit txns, internal
 *   btree_put/delete, compact).
 *
 * sdb_pager_take_pending: under database->mutex, read+clear the pending ticket
 *   left by the last deferred sdb_txn_commit. Returns true and *txn_id_out if a
 *   txn was staged and must be driven to durability.
 *
 * sdb_pager_commit_durable: off database->mutex, drive txn `txn_id` to disk.
 *   The first caller with an undurable txn becomes the leader (coalesces the
 *   whole queue into one pwrite-run + one fsync); the rest wait on commit_cond
 *   until their txn is durable or the batch is poisoned. Returns SDB_OK once
 *   txn_id <= durable_lsn, else the leader's fsync error (batch poisoned).
 *
 * sdb_pager_group_checkpoint_if_due: under database->mutex, run a deferred
 *   checkpoint iff the WAL crossed its threshold AND the coordinator is fully
 *   drained (no queued buffers, no active leader, everything durable) so no
 *   leader can be mid-fsync against the WAL being truncated.
 */
void sdb_pager_set_defer_commit(sdb_pager *pager, bool defer);
bool sdb_pager_take_pending(sdb_pager *pager, uint64_t *txn_id_out);
sdb_status sdb_pager_commit_durable(sdb_pager *pager, uint64_t txn_id);
sdb_status sdb_pager_group_checkpoint_if_due(sdb_pager *pager);

#if SDB_TESTING
/*
 * Fault-injection hook for sdb_pager_checkpoint. Fires SDB_E_IO at the
 * chosen durability boundary so the crash matrix can prove all-or-nothing
 * over a mid-checkpoint crash. Stages fire strictly at the boundary named
 * — writes / syncs BEFORE the marked boundary have already run.
 *
 *   NONE                    — no injection (default).
 *   BEFORE_DATA_SYNC        — after per-page data-file writes, before the
 *                             consolidating fsync on the data file.
 *   BEFORE_SUPERBLOCK       — after the data-file fsync, before the
 *                             superblock rotation write.
 *   BEFORE_WAL_CLEAR        — after the superblock is durable, before
 *                             sdb_wal_clear truncates the WAL.
 */
typedef enum sdb_checkpoint_fail_stage {
    SDB_CHECKPOINT_FAIL_NONE = 0,
    SDB_CHECKPOINT_FAIL_BEFORE_DATA_SYNC,
    SDB_CHECKPOINT_FAIL_BEFORE_SUPERBLOCK,
    SDB_CHECKPOINT_FAIL_BEFORE_WAL_CLEAR
} sdb_checkpoint_fail_stage;
void sdb_pager_checkpoint_fail_at_for_testing(
    sdb_checkpoint_fail_stage stage
);
void sdb_pager_checkpoint_clear_failure_for_testing(void);
/*
 * R4: make the NEXT group-commit leader flush fail its fsync (once), poisoning
 * the batch so every participating follower observes the error. Proves
 * leader-failure propagation and all-or-nothing over a mid-batch fsync fault.
 */
void sdb_pager_group_commit_fail_next_flush_for_testing(void);
void sdb_pager_group_commit_clear_failure_for_testing(void);
#endif

#endif
