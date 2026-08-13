#include "shibadb_engine.h"

#include "btree.h"
#include "crypto.h"
#include "engine_internal.h"
#include "internal.h"
#include "replace.h"
#include "sync.h"
#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Resolve the caller's cache_bytes request into a byte budget for the pager.
 * SDB_CACHE_AUTO picks 25% of physical RAM, clamped to [4 MiB, 1 GiB] so the
 * automatic default never balloons (the pager clamps again to its page-count
 * ceiling). Any other value — including 0, the engine default — passes through
 * unchanged. An unknown RAM size falls back to the engine default. */
static size_t sdb_engine_resolve_cache_bytes(uint64_t requested)
{
    const uint64_t floor_bytes = (uint64_t)4U * 1024U * 1024U;
    const uint64_t ceil_bytes = (uint64_t)1024U * 1024U * 1024U;
    uint64_t ram;
    uint64_t budget;
    if (requested != SDB_CACHE_AUTO) {
        return (size_t)requested;
    }
    ram = sdb_physical_memory_bytes();
    if (ram == 0U) {
        return 0U;
    }
    budget = ram / 4U;
    if (budget < floor_bytes) {
        budget = floor_bytes;
    } else if (budget > ceil_bytes) {
        budget = ceil_bytes;
    }
    return (size_t)budget;
}

#if defined(_WIN32) && SDB_TESTING
#define SDB_WINDOWS_TEST_DIAG(stage_, status_) \
    do { \
        if ((status_) != SDB_OK) { \
            (void)fprintf(stderr, "shibadb test diagnostic: %s: %s (%d)\n", \
                (stage_), sdb_status_string(status_), (int)(status_)); \
        } \
    } while (0)
#else
#define SDB_WINDOWS_TEST_DIAG(stage_, status_) \
    do { (void)(stage_); (void)(status_); } while (0)
#endif

#if SDB_TESTING
static unsigned sdb_compact_crash_phase_for_testing;
static unsigned sdb_backup_crash_phase_for_testing;
static size_t sdb_backup_file_fail_after_for_testing = SIZE_MAX;
#endif

/*
 * System-row prefix (0x10) sorts before every object/index prefix, so the
 * generation counter row is the first key a full cursor scan meets. It holds
 * the monotonic object-generation counter (ABA fix): a single fixed key whose
 * 8-byte little-endian value is the highest generation ever handed out. It
 * rides the normal btree/WAL path, so recovery restores it automatically and
 * no superblock/WAL format change is needed.
 */
#define SDB_SEQUENCE_KEY_PREFIX UINT8_C(0x10)
#define SDB_OBJECT_KEY_PREFIX UINT8_C(0x40)
#define SDB_CHUNK_KEY_PREFIX UINT8_C(0x41)
#define SDB_INDEX_DEFINITION_PREFIX UINT8_C(0x50)
#define SDB_INDEX_ENTRY_PREFIX UINT8_C(0x51)
#define SDB_UNIQUE_GUARD_PREFIX UINT8_C(0x52)
#define SDB_INDEX_REVERSE_PREFIX UINT8_C(0x53)
/*
 * Largest per-term reverse-row value: [generation u64][name_size u16][name]
 * [value_size u16][value], with name and value each capped at the engine's max
 * name size. One row per term (not one packed value for all terms) keeps every
 * reverse row well under the single-leaf payload limit — the b-tree has no
 * overflow pages.
 */
#define SDB_INDEX_REVERSE_MAX_VALUE (12U + 2U * SDB_ENGINE_MAX_NAME_SIZE)

/* The generation counter lives under a single fixed one-byte key. */
static const uint8_t sdb_generation_counter_key[1] = {SDB_SEQUENCE_KEY_PREFIX};
#define SDB_GENERATION_COUNTER_VALUE_SIZE ((size_t)8)
#define SDB_OBJECT_METADATA_SIZE ((size_t)64)
#define SDB_OBJECT_CHUNK_TARGET ((size_t)2048)

typedef enum sdb_object_kind {
    SDB_OBJECT_KV = 1,
    SDB_OBJECT_BLOB = 2,
    SDB_OBJECT_DOCUMENT = 3
} sdb_object_kind;

typedef struct sdb_object_metadata {
    uint16_t kind;
    uint64_t generation;
    uint64_t total_size;
    uint32_t chunk_count;
    uint32_t chunk_size;
    uint8_t digest[32];
} sdb_object_metadata;

struct sdb_database {
    sdb_pager pager;
    sdb_btree tree;
    sdb_mutex mutex;
    sdb_process_lock path_lock;
    sdb_process_lock process_lock;
    char *path;
    size_t callback_depth;
    sdb_transaction *active_transaction;
    /*
     * COUNT (maintained under `mutex`) of auto-commit txns currently in their
     * off-lock durability window: each committer increments before releasing
     * the engine mutex to drive its txn to durability via the R4 group-commit
     * coordinator, and decrements once it re-takes the mutex. While it is > 0,
     * at least one durability round is in flight on database->pager with the
     * engine mutex NOT held, so maintenance ops (compact/backup/migrate) and
     * close must refuse with SDB_E_BUSY — otherwise they could close/swap the
     * pager out from under an in-flight leader (use-after-destroy of
     * commit_mutex/cond).
     *
     * This MUST be a counter, not a bool: R4 lets several committers overlap in
     * the window (leader A finishes and re-takes the mutex to clear its share
     * while a later committer B, whose lsn A did not cover, is still a leader
     * mid-fsync). A single bool cleared by A would wrongly signal "idle" while
     * B is live, letting close/backup/compact tear the pager out from under B.
     * Explicit transactions hold the engine mutex across their whole commit, so
     * they never open this window and are covered by the active_transaction
     * guard instead.
     */
    uint32_t commit_in_flight;
    /*
     * At most one live read snapshot per handle (single-mutex isolation): a
     * snapshot takes this slot for its whole lifetime so every mutating op, a
     * second snapshot, a transaction, compact/backup/migrate, and close all
     * refuse with SDB_E_BUSY while it is held — the reader sees a stable tree
     * because no writer can advance it. Set/cleared under `mutex`.
     */
    sdb_snapshot *active_snapshot;
    bool open;
};

typedef struct sdb_engine_mutation {
    sdb_database *database;
    sdb_btree_batch batch;
    bool active;
} sdb_engine_mutation;

struct sdb_transaction {
    sdb_database *database;
    sdb_engine_mutation mutation;
    size_t operation_count;
    size_t logical_byte_count;
    bool active;
};

static sdb_status sdb_engine_mutation_begin(
    sdb_database *database, sdb_engine_mutation *mutation_out
)
{
    sdb_status status;
    if (database == NULL || !database->open || mutation_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(mutation_out, 0, sizeof(*mutation_out));
    status = sdb_btree_batch_begin(&database->tree, &mutation_out->batch);
    if (status == SDB_OK) {
        mutation_out->database = database;
        mutation_out->active = true;
    }
    return status;
}

static sdb_status sdb_engine_mutation_commit(
    sdb_engine_mutation *mutation
)
{
    sdb_status status;
    if (mutation == NULL || !mutation->active) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_btree_batch_commit(&mutation->batch);
    mutation->active = false;
    return status;
}

static void sdb_engine_mutation_abort(sdb_engine_mutation *mutation)
{
    if (mutation != NULL && mutation->active) {
        sdb_btree_batch_abort(&mutation->batch);
    }
    if (mutation != NULL) {
        mutation->active = false;
    }
}

static sdb_status sdb_engine_mutation_finish(
    sdb_engine_mutation *mutation,
    sdb_status status
)
{
    if (status == SDB_OK) {
        return sdb_engine_mutation_commit(mutation);
    }
    sdb_engine_mutation_abort(mutation);
    return status;
}

static sdb_status sdb_engine_tree_get(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    return mutation == NULL
        ? sdb_btree_get(
            &database->tree,
            key,
            key_size,
            value_out,
            value_capacity,
            value_size_out
        )
        : sdb_btree_batch_get(
            &mutation->batch,
            key,
            key_size,
            value_out,
            value_capacity,
            value_size_out
        );
}

static sdb_status sdb_engine_tree_put(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    return mutation == NULL
        ? sdb_btree_put(
            &database->tree, key, key_size, value, value_size
        )
        : sdb_btree_batch_put(
            &mutation->batch, key, key_size, value, value_size
        );
}

static sdb_status sdb_engine_tree_delete(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *key,
    size_t key_size
)
{
    return mutation == NULL
        ? sdb_btree_delete(&database->tree, key, key_size)
        : sdb_btree_batch_delete(&mutation->batch, key, key_size);
}

/*
 * Extract the object generation stamped on a stored row, or 0 for rows that
 * carry none (index definitions, the counter row, unknown prefixes). The
 * offsets mirror exactly where sdb_engine_entry_is_live reads the generation,
 * so a full scan sees every generation the database has ever handed out —
 * including those on stale rows whose object was deleted.
 */
static uint64_t sdb_row_generation(
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    if (key == NULL || key_size == 0U) {
        return 0U;
    }
    switch (key[0]) {
    case SDB_OBJECT_KEY_PREFIX:
        return value_size == SDB_OBJECT_METADATA_SIZE
            ? sdb_read_u64_le(value + 8U) : 0U;
    case SDB_CHUNK_KEY_PREFIX:
        return key_size >= 12U
            ? sdb_read_u64_le(key + key_size - 12U) : 0U;
    case SDB_INDEX_ENTRY_PREFIX:
        return key_size >= 8U
            ? sdb_read_u64_le(key + key_size - 8U) : 0U;
    case SDB_UNIQUE_GUARD_PREFIX:
        return value_size >= 8U ? sdb_read_u64_le(value) : 0U;
    case SDB_INDEX_REVERSE_PREFIX:
        return value_size >= 8U ? sdb_read_u64_le(value) : 0U;
    default:
        return 0U;
    }
}

/*
 * Full-scan fallback that finds the highest generation present on any row.
 * Runs once when opening a database that has no counter row yet (a legacy
 * file written before this fix, or a freshly created empty database). After
 * the first object write the counter row exists and this never runs again.
 */
static sdb_status sdb_seed_generation_by_scan(
    sdb_database *database, uint64_t *max_out
)
{
    sdb_btree_cursor cursor;
    uint8_t *key;
    uint8_t *value;
    const size_t capacity = sdb_pager_payload_capacity(&database->pager);
    uint64_t max_gen = 0U;
    sdb_status status;
    key = (uint8_t *)malloc(capacity);
    value = (uint8_t *)malloc(capacity);
    if (key == NULL || value == NULL) {
        free(key);
        free(value);
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    status = sdb_btree_cursor_first(&database->tree, &cursor);
    while (status == SDB_OK && cursor.valid) {
        size_t key_size;
        size_t value_size;
        status = sdb_btree_cursor_read(
            &cursor, key, capacity, &key_size, value, capacity, &value_size
        );
        if (status != SDB_OK) {
            break;
        }
        {
            const uint64_t generation =
                sdb_row_generation(key, key_size, value, value_size);
            if (generation > max_gen) {
                max_gen = generation;
            }
        }
        status = sdb_btree_cursor_next(&cursor);
    }
    sdb_btree_cursor_close(&cursor);
    free(key);
    free(value);
    if (status == SDB_OK) {
        *max_out = max_gen;
    }
    return status;
}

/*
 * Seed the in-memory generation counter at open. The persisted counter row is
 * the fast, authoritative source (recovery already replayed it through the
 * WAL, so it reflects every committed generation); only when it is absent do
 * we pay for the one-time scan.
 */
static sdb_status sdb_engine_seed_generation(sdb_database *database)
{
    uint8_t value[SDB_GENERATION_COUNTER_VALUE_SIZE];
    size_t value_size = 0U;
    sdb_status status = sdb_btree_get(
        &database->tree,
        sdb_generation_counter_key,
        sizeof(sdb_generation_counter_key),
        value,
        sizeof(value),
        &value_size
    );
    if (status == SDB_OK) {
        if (value_size != SDB_GENERATION_COUNTER_VALUE_SIZE) {
            return SDB_E_CORRUPT;
        }
        database->pager.next_object_generation = sdb_read_u64_le(value);
        return SDB_OK;
    }
    if (status != SDB_E_NOT_FOUND) {
        return status;
    }
    {
        uint64_t max_gen = 0U;
        status = sdb_seed_generation_by_scan(database, &max_gen);
        if (status == SDB_OK) {
            database->pager.next_object_generation = max_gen;
        }
        return status;
    }
}

/*
 * Hand out the next monotonic generation and stage the updated counter row in
 * the SAME mutation, so the counter commits atomically with the object it
 * stamps. The in-memory counter is only advanced after the row stages
 * successfully; if the transaction later aborts, the row is not committed and
 * a reopen re-seeds from the last committed value — the skipped generation is
 * simply never reused by any live row, which is harmless (monotonic, no ABA).
 */
static sdb_status sdb_engine_alloc_generation(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    uint64_t *generation_out
)
{
    uint8_t value[SDB_GENERATION_COUNTER_VALUE_SIZE];
    uint64_t next;
    sdb_status status;
    if (database->pager.next_object_generation == UINT64_MAX) {
        return SDB_E_OVERFLOW;
    }
    next = database->pager.next_object_generation + 1U;
    sdb_write_u64_le(value, next);
    status = sdb_engine_tree_put(
        database,
        mutation,
        sdb_generation_counter_key,
        sizeof(sdb_generation_counter_key),
        value,
        sizeof(value)
    );
    if (status != SDB_OK) {
        return status;
    }
    database->pager.next_object_generation = next;
    *generation_out = next;
    return SDB_OK;
}

/*
 * Poison the mutation's batch when an operation fails AFTER it began staging
 * rows, so a later commit aborts instead of persisting the half-written
 * chunks/index entries as orphans. A multi-step put stages chunks, index
 * entries and the generation-counter row before the metadata row; if an error
 * strikes between those steps WITHOUT flowing through sdb_btree_batch_put/
 * _delete (e.g. a key-build malloc failure), the batch's own failure flag is
 * never set, so an explicit-transaction caller that ignores the returned error
 * and commits anyway would otherwise persist the partial rows. The
 * `revision != revision_before` guard limits poisoning to operations that
 * actually staged something: a pure validation error before any staging leaves
 * the transaction commitable. It keys off the batch revision (which ticks on
 * every stage, including an in-place re-stage of an already-present page)
 * rather than the distinct-page count, because an op that only mutates a
 * staged page in place before failing leaves the count unchanged yet has
 * committed a partial change that must not survive. Never clobbers an existing
 * failure.
 */
static sdb_status sdb_mutation_poison_on_partial(
    sdb_engine_mutation *mutation, uint64_t revision_before, sdb_status status
)
{
    if (status != SDB_OK && mutation != NULL
        && mutation->batch.failure == SDB_OK
        && mutation->batch.revision != revision_before) {
        mutation->batch.failure = status;
    }
    return status;
}

#if SDB_TESTING
sdb_file *sdb_database_file_for_testing(sdb_database *database)
{
    return database == NULL ? NULL : &database->pager.file;
}

void sdb_engine_crash_after_compact_phase_for_testing(unsigned phase)
{
    sdb_compact_crash_phase_for_testing = phase;
}

void sdb_engine_crash_after_backup_phase_for_testing(unsigned phase)
{
    sdb_backup_crash_phase_for_testing = phase;
}

void sdb_engine_backup_file_fail_after_for_testing(size_t boundary)
{
    sdb_backup_file_fail_after_for_testing = boundary;
}

void sdb_engine_backup_file_clear_failure_for_testing(void)
{
    sdb_backup_file_fail_after_for_testing = SIZE_MAX;
}
#endif

static void sdb_compact_test_crash(unsigned phase)
{
#if SDB_TESTING
    if (sdb_compact_crash_phase_for_testing == phase) {
        _Exit(99);
    }
#else
    (void)phase;
#endif
}

static void sdb_backup_test_crash(unsigned phase)
{
#if SDB_TESTING
    if (sdb_backup_crash_phase_for_testing == phase) {
        _Exit(98);
    }
#else
    (void)phase;
#endif
}

static sdb_status sdb_index_definition(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool *unique_out
);

static bool sdb_engine_bytes_valid(const uint8_t *bytes, size_t size)
{
    return bytes != NULL && size != 0U
        && size <= SDB_ENGINE_MAX_NAME_SIZE
        && size <= (size_t)UINT16_MAX;
}

static sdb_status sdb_engine_pair_key(
    uint8_t prefix,
    uint8_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    size_t suffix_size,
    uint8_t **output,
    size_t *output_size
)
{
    size_t size;
    uint8_t *result;
    /*
     * Object/chunk composite key layout (format V2):
     *   [prefix u8][kind u8][ns_len u16 LE][namespace bytes][user_key bytes]
     *   (+ suffix bytes for chunk keys: [generation u64][chunk_index u32])
     * The user_key length is NOT stored — it is the trailing remainder after
     * the fixed 4-byte header, the namespace, and any fixed suffix. This is
     * deliberate: an earlier layout put a key_len field at byte 4, ahead of
     * the namespace and key, which made the b-tree sort by key length before
     * user key. That broke user-key-ordered iteration and made a namespace
     * non-contiguous. With key_len gone the sort order is
     * kind -> ns_len -> namespace -> user_key, so entries in one namespace are
     * contiguous and ordered by user key. V1 databases use the old 6-byte
     * header and must be migrated (see sdb_database_migrate).
     */
    if (!sdb_engine_bytes_valid(namespace_name, namespace_size)
        || !sdb_engine_bytes_valid(key, key_size)
        || output == NULL || output_size == NULL
        || namespace_size > SIZE_MAX - key_size - 4U
        || suffix_size > SIZE_MAX - namespace_size - key_size - 4U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    size = 4U + namespace_size + key_size + suffix_size;
    result = (uint8_t *)malloc(size);
    if (result == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    result[0] = prefix;
    result[1] = kind;
    sdb_write_u16_le(result + 2U, (uint16_t)namespace_size);
    (void)memcpy(result + 4U, namespace_name, namespace_size);
    (void)memcpy(result + 4U + namespace_size, key, key_size);
    *output = result;
    *output_size = size;
    return SDB_OK;
}

static sdb_status sdb_object_key(
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t **output,
    size_t *output_size
)
{
    return sdb_engine_pair_key(
        SDB_OBJECT_KEY_PREFIX,
        (uint8_t)kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        0U,
        output,
        output_size
    );
}

#if SDB_TESTING
static bool sdb_test_chunk_key_fail_armed = false;
static uint32_t sdb_test_chunk_key_fail_index = 0U;
void sdb_engine_chunk_key_fail_for_testing(uint32_t chunk_index)
{
    sdb_test_chunk_key_fail_armed = true;
    sdb_test_chunk_key_fail_index = chunk_index;
}
void sdb_engine_chunk_key_clear_failure_for_testing(void)
{
    sdb_test_chunk_key_fail_armed = false;
    sdb_test_chunk_key_fail_index = 0U;
}
#endif

static sdb_status sdb_chunk_key(
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint64_t generation,
    uint32_t chunk_index,
    uint8_t **output,
    size_t *output_size
)
{
    sdb_status status;
#if SDB_TESTING
    /*
     * Fault-injection hook: force the composite-key allocation for one chunk
     * index to fail, exercising the mid-delete key-build error path that the
     * batch poison guard must catch (a chunk deleted in place before the
     * failure must still poison the batch so a later commit aborts).
     */
    if (sdb_test_chunk_key_fail_armed
        && chunk_index == sdb_test_chunk_key_fail_index) {
        return SDB_E_OUT_OF_MEMORY;
    }
#endif
    status = sdb_engine_pair_key(
        SDB_CHUNK_KEY_PREFIX,
        (uint8_t)kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        12U,
        output,
        output_size
    );
    if (status == SDB_OK) {
        sdb_write_u64_le(*output + *output_size - 12U, generation);
        sdb_write_u32_le(*output + *output_size - 4U, chunk_index);
    }
    return status;
}

static void sdb_object_metadata_encode(
    const sdb_object_metadata *metadata,
    uint8_t output[SDB_OBJECT_METADATA_SIZE]
)
{
    static const uint8_t magic[4] = {
        (uint8_t)'S', (uint8_t)'O', (uint8_t)'M', (uint8_t)'1'
    };
    (void)memset(output, 0, SDB_OBJECT_METADATA_SIZE);
    (void)memcpy(output, magic, sizeof(magic));
    sdb_write_u16_le(output + 4U, UINT16_C(1));
    sdb_write_u16_le(output + 6U, metadata->kind);
    sdb_write_u64_le(output + 8U, metadata->generation);
    sdb_write_u64_le(output + 16U, metadata->total_size);
    sdb_write_u32_le(output + 24U, metadata->chunk_count);
    sdb_write_u32_le(output + 28U, metadata->chunk_size);
    (void)memcpy(output + 32U, metadata->digest, 32U);
}

static sdb_status sdb_object_metadata_decode(
    const uint8_t *input,
    size_t input_size,
    uint16_t expected_kind,
    sdb_object_metadata *metadata_out
)
{
    static const uint8_t magic[4] = {
        (uint8_t)'S', (uint8_t)'O', (uint8_t)'M', (uint8_t)'1'
    };
    sdb_object_metadata metadata;
    if (input == NULL || metadata_out == NULL
        || input_size != SDB_OBJECT_METADATA_SIZE
        || memcmp(input, magic, sizeof(magic)) != 0
        || sdb_read_u16_le(input + 4U) != UINT16_C(1)
        || sdb_read_u16_le(input + 6U) != expected_kind) {
        return SDB_E_CORRUPT;
    }
    (void)memset(&metadata, 0, sizeof(metadata));
    metadata.kind = sdb_read_u16_le(input + 6U);
    metadata.generation = sdb_read_u64_le(input + 8U);
    metadata.total_size = sdb_read_u64_le(input + 16U);
    metadata.chunk_count = sdb_read_u32_le(input + 24U);
    metadata.chunk_size = sdb_read_u32_le(input + 28U);
    (void)memcpy(metadata.digest, input + 32U, 32U);
    if (metadata.generation == 0U
        || metadata.total_size > (uint64_t)SIZE_MAX
        || metadata.chunk_size > (uint32_t)SDB_OBJECT_CHUNK_TARGET
        || (metadata.total_size == 0U
            && (metadata.chunk_count != 0U || metadata.chunk_size != 0U))
        || (metadata.total_size != 0U
            && (metadata.chunk_count == 0U || metadata.chunk_size == 0U))
        || (metadata.chunk_count != 0U
            && (metadata.total_size / (uint64_t)metadata.chunk_size
                + (metadata.total_size
                    % (uint64_t)metadata.chunk_size != 0U ? 1U : 0U))
                != (uint64_t)metadata.chunk_count)) {
        return SDB_E_CORRUPT;
    }
    *metadata_out = metadata;
    return SDB_OK;
}

static sdb_status sdb_object_read_metadata(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    sdb_object_metadata *metadata_out
)
{
    uint8_t *metadata_key = NULL;
    size_t metadata_key_size;
    uint8_t encoded[SDB_OBJECT_METADATA_SIZE];
    size_t encoded_size = 0U;
    sdb_status status = sdb_object_key(
        kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        &metadata_key,
        &metadata_key_size
    );
    if (status == SDB_OK) {
        status = sdb_engine_tree_get(
            database,
            mutation,
            metadata_key,
            metadata_key_size,
            encoded,
            sizeof(encoded),
            &encoded_size
        );
    }
    free(metadata_key);
    if (status != SDB_OK) {
        return status;
    }
    return sdb_object_metadata_decode(
        encoded, encoded_size, kind, metadata_out
    );
}

static sdb_status sdb_object_plan(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size,
    sdb_object_metadata *metadata_out
)
{
    sdb_object_metadata current;
    uint8_t *sample_chunk_key;
    size_t sample_chunk_key_size;
    size_t leaf_capacity;
    size_t balanced_entry_capacity;
    size_t chunk_size;
    sdb_status status = SDB_OK;
    if (database == NULL || !database->open
        || (value == NULL && value_size != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(metadata_out, 0, sizeof(*metadata_out));
    metadata_out->kind = kind;
    status = sdb_object_read_metadata(
        database,
        mutation,
        kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        &current
    );
    if (status != SDB_OK && status != SDB_E_NOT_FOUND) {
        return status;
    }
    /*
     * ABA fix: the generation now comes from a monotonic counter that is never
     * reused, NOT from the previous metadata's generation + 1. The old scheme
     * reset generation to 1 whenever the metadata was absent (i.e. after a
     * delete), so a stale index/chunk/guard row left behind by the delete
     * matched the recreated object again and silently came back to life. The
     * read above is kept only to surface a genuine read error; `current`'s
     * generation is intentionally ignored.
     */
    status = sdb_engine_alloc_generation(
        database, mutation, &metadata_out->generation
    );
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_chunk_key(
        kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        metadata_out->generation,
        0U,
        &sample_chunk_key,
        &sample_chunk_key_size
    );
    if (status != SDB_OK) {
        return status;
    }
    leaf_capacity = sdb_pager_payload_capacity(&database->pager);
    if (leaf_capacity <= SDB_BTREE_NODE_HEADER_SIZE) {
        free(sample_chunk_key);
        return SDB_E_BUFFER_TOO_SMALL;
    }

    balanced_entry_capacity =
        (leaf_capacity - SDB_BTREE_NODE_HEADER_SIZE) / 2U;
    if (sample_chunk_key_size > balanced_entry_capacity
        || balanced_entry_capacity - sample_chunk_key_size <= 6U) {
        free(sample_chunk_key);
        return SDB_E_BUFFER_TOO_SMALL;
    }
    chunk_size = balanced_entry_capacity - sample_chunk_key_size - 6U;
    if (chunk_size > SDB_OBJECT_CHUNK_TARGET) {
        chunk_size = SDB_OBJECT_CHUNK_TARGET;
    }
    free(sample_chunk_key);
    metadata_out->total_size = (uint64_t)value_size;
    if (value_size != 0U) {
        const size_t chunk_count = value_size / chunk_size
            + (value_size % chunk_size != 0U ? 1U : 0U);
        if (chunk_count > (size_t)UINT32_MAX) {
            return SDB_E_OVERFLOW;
        }
        metadata_out->chunk_size = (uint32_t)chunk_size;
        metadata_out->chunk_count = (uint32_t)chunk_count;
    }
    sdb_sha256(value, value_size, metadata_out->digest);
    return SDB_OK;
}

static sdb_status sdb_object_write_chunks(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const sdb_object_metadata *metadata,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value
)
{
    uint32_t index;
    size_t consumed = 0U;
    for (index = 0U; index < metadata->chunk_count; ++index) {
        uint8_t *chunk_key = NULL;
        size_t chunk_key_size;
        size_t remaining = (size_t)metadata->total_size - consumed;
        size_t take = remaining < (size_t)metadata->chunk_size
            ? remaining : (size_t)metadata->chunk_size;
        sdb_status status = sdb_chunk_key(
            metadata->kind,
            namespace_name,
            namespace_size,
            key,
            key_size,
            metadata->generation,
            index,
            &chunk_key,
            &chunk_key_size
        );
        if (status == SDB_OK) {
            status = sdb_engine_tree_put(
                database,
                mutation,
                chunk_key,
                chunk_key_size,
                value + consumed,
                take
            );
        }
        free(chunk_key);
        if (status != SDB_OK) {
            return status;
        }
        consumed += take;
    }
    return SDB_OK;
}

static sdb_status sdb_object_commit_metadata(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const sdb_object_metadata *metadata,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    uint8_t *metadata_key = NULL;
    size_t metadata_key_size;
    uint8_t encoded[SDB_OBJECT_METADATA_SIZE];
    sdb_status status = sdb_object_key(
        metadata->kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        &metadata_key,
        &metadata_key_size
    );
    sdb_object_metadata_encode(metadata, encoded);
    if (status == SDB_OK) {
        status = sdb_engine_tree_put(
            database,
            mutation,
            metadata_key,
            metadata_key_size,
            encoded,
            sizeof(encoded)
        );
    }
    free(metadata_key);
    return status;
}

/*
 * Delete every chunk row of one object version, identified by the generation
 * and chunk_count from its metadata. Shared by the delete path and the
 * overwrite path in put: both must reclaim a superseded version's chunks,
 * because the monotonic generation means a new version writes FRESH chunk keys
 * and never overwrites the old ones — so without this the old chunks orphan.
 */
static sdb_status sdb_object_delete_chunks(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint64_t generation,
    uint32_t chunk_count
)
{
    uint32_t index;
    sdb_status status = SDB_OK;
    for (index = 0U; index < chunk_count && status == SDB_OK; ++index) {
        uint8_t *chunk_key = NULL;
        size_t chunk_key_size;
        status = sdb_chunk_key(
            kind, namespace_name, namespace_size, key, key_size,
            generation, index, &chunk_key, &chunk_key_size
        );
        if (status == SDB_OK) {
            status = sdb_engine_tree_delete(
                database, mutation, chunk_key, chunk_key_size
            );
        }
        free(chunk_key);
    }
    return status;
}

static sdb_status sdb_object_put(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_object_metadata metadata;
    sdb_object_metadata previous;
    bool has_previous = false;
    const uint64_t revision_before =
        mutation != NULL ? mutation->batch.revision : 0U;
    sdb_status status;
    /*
     * Read the current version (if any) BEFORE planning the new one, so an
     * overwrite can reclaim the old version's chunks. object_plan assigns a
     * fresh monotonic generation and writes new chunk keys, so the old chunks
     * would otherwise orphan on every update. A genuine read error is fatal;
     * NOT_FOUND just means this is a first insert (nothing to reclaim).
     */
    status = sdb_object_read_metadata(
        database, mutation, kind,
        namespace_name, namespace_size, key, key_size, &previous
    );
    if (status == SDB_OK) {
        has_previous = true;
    } else if (status != SDB_E_NOT_FOUND) {
        return sdb_mutation_poison_on_partial(mutation, revision_before, status);
    }
    status = sdb_object_plan(
        database,
        mutation,
        kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        value,
        value_size,
        &metadata
    );
    if (status == SDB_OK) {
        status = sdb_object_write_chunks(
            database,
            mutation,
            &metadata,
            namespace_name,
            namespace_size,
            key,
            key_size,
            value
        );
    }
    if (status == SDB_OK) {
        status = sdb_object_commit_metadata(
            database,
            mutation,
            &metadata,
            namespace_name,
            namespace_size,
            key,
            key_size
        );
    }
    /*
     * Reclaim the superseded version's chunks (overwrite). Staged in the same
     * mutation batch as the new version and keyed by the OLD generation, so it
     * cannot collide with the freshly-written new-generation chunks and commits
     * atomically with them.
     */
    if (status == SDB_OK && has_previous) {
        status = sdb_object_delete_chunks(
            database, mutation, kind, namespace_name, namespace_size,
            key, key_size, previous.generation, previous.chunk_count
        );
    }
    return sdb_mutation_poison_on_partial(mutation, revision_before, status);
}

static sdb_status sdb_object_get(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    sdb_object_metadata metadata;
    uint8_t digest[32];
    uint8_t empty_output = 0U;
    uint8_t *output;
    uint32_t index;
    size_t produced = 0U;
    sdb_status status;
    if (value_size_out == NULL
        || (value_out == NULL && value_capacity != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_object_read_metadata(
        database,
        mutation,
        kind,
        namespace_name,
        namespace_size,
        key,
        key_size,
        &metadata
    );
    if (status != SDB_OK) {
        return status;
    }
    *value_size_out = (size_t)metadata.total_size;
    if (value_capacity < (size_t)metadata.total_size) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    if (metadata.total_size != 0U && value_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    output = value_out == NULL ? &empty_output : value_out;
    for (index = 0U; index < metadata.chunk_count; ++index) {
        uint8_t *chunk_key = NULL;
        size_t chunk_key_size;
        size_t chunk_size = 0U;
        status = sdb_chunk_key(
            kind,
            namespace_name,
            namespace_size,
            key,
            key_size,
            metadata.generation,
            index,
            &chunk_key,
            &chunk_key_size
        );
        if (status == SDB_OK) {
            status = sdb_engine_tree_get(
                database,
                mutation,
                chunk_key,
                chunk_key_size,
                output + produced,
                value_capacity - produced,
                &chunk_size
            );
        }
        free(chunk_key);
        if (status != SDB_OK
            || chunk_size == 0U
            || chunk_size > (size_t)metadata.chunk_size
            || produced > (size_t)metadata.total_size - chunk_size) {
            if (produced != 0U) {
                sdb_secure_zero(output, produced);
            }
            return status != SDB_OK ? status : SDB_E_CORRUPT;
        }
        produced += chunk_size;
    }
    if (produced != (size_t)metadata.total_size) {
        if (produced != 0U) {
            sdb_secure_zero(output, produced);
        }
        return SDB_E_CORRUPT;
    }
    sdb_sha256(output, produced, digest);
    if (!sdb_constant_time_equal(digest, metadata.digest, sizeof(digest))) {
        sdb_secure_zero(digest, sizeof(digest));
        if (produced != 0U) {
            sdb_secure_zero(output, produced);
        }
        return SDB_E_CORRUPT;
    }
    sdb_secure_zero(digest, sizeof(digest));
    return SDB_OK;
}

static sdb_status sdb_object_delete(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    uint16_t kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    uint8_t *metadata_key = NULL;
    size_t metadata_key_size;
    sdb_object_metadata metadata;
    const uint64_t revision_before =
        mutation != NULL ? mutation->batch.revision : 0U;
    sdb_status meta_status;
    sdb_status status = SDB_OK;
    /*
     * Reclaim the object's chunk rows, not just its metadata row. Deleting only
     * the metadata used to orphan every chunk: the monotonic generation (the
     * ABA fix) meant a later re-insert wrote NEW chunk keys and never touched
     * the old ones, so a delete+reinsert churn accumulated dead chunk rows
     * without bound and grew the file until a full compact() ran. Read the
     * current metadata to learn the live generation and chunk count, then
     * delete exactly those chunk rows. The monotonic generation still protects
     * against ABA (a recreate gets a fresh generation), so eager cleanup here
     * is strictly safe — it removes this object's own rows, nothing a recreate
     * could ever reuse.
     */
    meta_status = sdb_object_read_metadata(
        database, mutation, kind,
        namespace_name, namespace_size, key, key_size, &metadata
    );
    if (meta_status == SDB_OK) {
        status = sdb_object_delete_chunks(
            database, mutation, kind, namespace_name, namespace_size,
            key, key_size, metadata.generation, metadata.chunk_count
        );
    } else if (meta_status != SDB_E_NOT_FOUND) {
        /*
         * A genuine read error (corrupt/IO) — surface it. NOT_FOUND falls
         * through so deleting an absent key stays idempotent as before.
         */
        return sdb_mutation_poison_on_partial(
            mutation, revision_before, meta_status
        );
    }
    /*
     * Delete the metadata row last (unchanged behaviour, including for an
     * absent key: tree_delete is idempotent).
     */
    if (status == SDB_OK) {
        status = sdb_object_key(
            kind, namespace_name, namespace_size, key, key_size,
            &metadata_key, &metadata_key_size
        );
        if (status == SDB_OK) {
            status = sdb_engine_tree_delete(
                database, mutation, metadata_key, metadata_key_size
            );
        }
        free(metadata_key);
    }
    return sdb_mutation_poison_on_partial(mutation, revision_before, status);
}

void sdb_database_options_init(sdb_database_options *options)
{
    if (options != NULL) {
        (void)memset(options, 0, sizeof(*options));
        options->struct_size = (uint32_t)sizeof(*options);
        options->page_size = SDB_ENGINE_DEFAULT_PAGE_SIZE;
        options->kdf_iterations = SDB_DEFAULT_KDF_ITERATIONS;
    }
}

static sdb_status sdb_database_options_validate(
    const sdb_database_options *options
)
{
    size_t reserved_index;
    if (options == NULL
        || options->struct_size < (uint32_t)sizeof(*options)
        || options->page_size < SDB_MIN_PAGE_SIZE
        || options->page_size > SDB_MAX_PAGE_SIZE
        || (options->page_size & (options->page_size - 1U)) != 0U
        || (options->password == NULL && options->password_size != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    for (reserved_index = 0U;
         reserved_index
            < sizeof(options->reserved) / sizeof(options->reserved[0]);
         ++reserved_index) {
        if (options->reserved[reserved_index] != 0U) {
            return SDB_E_INVALID_ARGUMENT;
        }
    }
    if (options->synchronous > SDB_SYNCHRONOUS_NORMAL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (options->password_size != 0U
        && (options->kdf_iterations < SDB_MIN_KDF_ITERATIONS
            || options->kdf_iterations > SDB_MAX_KDF_ITERATIONS)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    return SDB_OK;
}

sdb_status sdb_database_create(
    const char *path,
    const sdb_database_options *options,
    sdb_database **database_out
)
{
    sdb_database *database;
    char *resolved_path = NULL;
    uint8_t salt[16];
    uint8_t file_id[16];
    sdb_status status;
    if (database_out != NULL) {
        *database_out = NULL;
    }
    if (path == NULL || path[0] == '\0' || database_out == NULL
        || sdb_database_options_validate(options) != SDB_OK) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_file_resolve_database_path(
        path, false, &resolved_path
    );
    if (status != SDB_OK) {
        return status;
    }
    database = (sdb_database *)calloc(1U, sizeof(*database));
    if (database == NULL) {
        free(resolved_path);
        return SDB_E_OUT_OF_MEMORY;
    }
    database->path = resolved_path;

    status = sdb_mutex_init(&database->mutex);
    if (status == SDB_OK) {
        status = sdb_process_lock_acquire(
            database->path, &database->path_lock
        );
    }
    if (status != SDB_OK) {
        sdb_mutex_destroy(&database->mutex);
        free(database->path);
        free(database);
        return status;
    }
    status = sdb_random_bytes(salt, sizeof(salt));
    if (status == SDB_OK) {
        status = sdb_random_bytes(file_id, sizeof(file_id));
    }
    if (status == SDB_OK && options->password_size != 0U) {
        status = sdb_pager_create_encrypted_ex(
            database->path,
            options->page_size,
            salt,
            file_id,
            options->password,
            options->password_size,
            options->kdf_iterations,
            sdb_engine_resolve_cache_bytes(options->cache_bytes),
            &database->pager
        );
    } else if (status == SDB_OK) {
        status = sdb_pager_create_ex(
            database->path,
            options->page_size,
            salt,
            file_id,
            sdb_engine_resolve_cache_bytes(options->cache_bytes),
            &database->pager
        );
    }
    if (status == SDB_OK) {
        status = sdb_process_lock_acquire_database(
            database->path, &database->process_lock
        );
    }
    sdb_secure_zero(salt, sizeof(salt));
    sdb_secure_zero(file_id, sizeof(file_id));
    if (status == SDB_OK) {
        status = sdb_btree_create(&database->pager, &database->tree);
    }
    if (status == SDB_OK) {
        sdb_pager_set_sync_relaxed(
            &database->pager,
            options->synchronous == SDB_SYNCHRONOUS_NORMAL
        );
    }
    if (status == SDB_OK) {
        status = sdb_engine_seed_generation(database);
    }
    if (status != SDB_OK) {
        if (database->pager.open) {
            (void)sdb_pager_close(&database->pager);
        }
        if (database->process_lock.held) {
            (void)sdb_process_lock_release(&database->process_lock);
        }
        if (database->path_lock.held) {
            (void)sdb_process_lock_release(&database->path_lock);
        }
        sdb_mutex_destroy(&database->mutex);
        free(database->path);
        free(database);
        return status;
    }
    database->open = true;
    *database_out = database;
    return SDB_OK;
}

static sdb_status sdb_database_open_internal(
    const char *path,
    const sdb_database_options *options,
    bool allow_legacy,
    sdb_database **database_out
)
{
    sdb_database *database;
    char *resolved_path = NULL;
    sdb_superblock_read_result result = {0};
    sdb_status status;
    if (database_out != NULL) {
        *database_out = NULL;
    }
    if (path == NULL || path[0] == '\0' || database_out == NULL
        || sdb_database_options_validate(options) != SDB_OK) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_file_resolve_database_path(
        path, true, &resolved_path
    );
    if (status != SDB_OK) {
        return status;
    }
    database = (sdb_database *)calloc(1U, sizeof(*database));
    if (database == NULL) {
        free(resolved_path);
        return SDB_E_OUT_OF_MEMORY;
    }
    database->path = resolved_path;

    status = sdb_mutex_init(&database->mutex);
    if (status == SDB_OK) {
        status = sdb_process_lock_acquire(
            database->path, &database->path_lock
        );
    }
    if (status == SDB_OK) {
        status = sdb_process_lock_acquire_database(
            database->path, &database->process_lock
        );
    }
    if (status == SDB_OK) {
        status = sdb_superblock_store_read(database->path, &result);
    }
    if (status == SDB_OK && !allow_legacy
        && result.format_version != SDB_FORMAT_VERSION_CURRENT) {
        /*
         * A V1 file uses the pre-2026-08 object-key layout (a key_len field
         * embedded before the namespace + user key), which the current V2
         * engine would sort and decode incorrectly — the whole reason the
         * layout changed. Refuse to open it with the V2 layout rather than
         * silently return misordered or misdecoded rows. Fail closed here,
         * before any pager/WAL work touches the file, so a legacy database is
         * never corrupted by a mismatched open; it must be upgraded through
         * the migration path first. The migration path itself opens with
         * allow_legacy = true and only ever raw-scans the tree (never the
         * V2 object decoder), so it reads V1 keys byte-for-byte.
         */
        status = SDB_E_UNSUPPORTED_VERSION;
    }
    if (status != SDB_OK) {
        if (database->process_lock.held) {
            (void)sdb_process_lock_release(&database->process_lock);
        }
        if (database->path_lock.held) {
            (void)sdb_process_lock_release(&database->path_lock);
        }
        sdb_mutex_destroy(&database->mutex);
        free(database->path);
        free(database);
        return status;
    }
    if ((result.superblock.flags & SDB_FLAG_ENCRYPTED) != 0U) {
        /*
         * Encrypted database opened without a password: report a missing
         * credential as SDB_E_AUTHENTICATION ("password required"), not the
         * generic SDB_E_INVALID_ARGUMENT the encrypted-open wrapper would
         * return for a NULL password. A forgotten password is a common
         * mistake, and "invalid argument" sent users debugging their API call
         * instead of their credential; this matches the SDB_E_AUTHENTICATION a
         * wrong (but present) password already produces.
         */
        status = (options->password == NULL || options->password_size == 0U)
            ? SDB_E_AUTHENTICATION
            : sdb_pager_open_encrypted_ex(
                database->path,
                options->password,
                options->password_size,
                sdb_engine_resolve_cache_bytes(options->cache_bytes),
                &database->pager
            );
    } else {
        status = options->password_size == 0U
            ? sdb_pager_open_ex(
                  database->path,
                  sdb_engine_resolve_cache_bytes(options->cache_bytes),
                  &database->pager
              )
            : SDB_E_INVALID_ARGUMENT;
    }
    if (status == SDB_OK) {
        status = sdb_btree_open(&database->pager, &database->tree);
    }
    if (status == SDB_OK) {
        sdb_pager_set_sync_relaxed(
            &database->pager,
            options->synchronous == SDB_SYNCHRONOUS_NORMAL
        );
    }
    if (status == SDB_OK) {
        status = sdb_engine_seed_generation(database);
    }
    if (status != SDB_OK) {
        if (database->pager.open) {
            (void)sdb_pager_close(&database->pager);
        }
        if (database->process_lock.held) {
            (void)sdb_process_lock_release(&database->process_lock);
        }
        if (database->path_lock.held) {
            (void)sdb_process_lock_release(&database->path_lock);
        }
        sdb_mutex_destroy(&database->mutex);
        free(database->path);
        free(database);
        return status;
    }
    database->open = true;
    *database_out = database;
    return SDB_OK;
}

sdb_status sdb_database_open(
    const char *path,
    const sdb_database_options *options,
    sdb_database **database_out
)
{
    return sdb_database_open_internal(path, options, false, database_out);
}

sdb_status sdb_database_close(sdb_database *database)
{
    sdb_status status;
    sdb_status lock_status;
    sdb_status path_lock_status;
    if (database == NULL || !database->open) {
        return SDB_E_INVALID_ARGUMENT;
    }
    sdb_mutex_lock(&database->mutex);
    if (database->callback_depth != 0U
        || database->active_transaction != NULL
        || database->active_snapshot != NULL
        || database->commit_in_flight != 0U) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    /*
     * Checkpoint on close so the closed database file is the complete,
     * authoritative on-disk image of every committed txn: drain the
     * WAL-resident pages into the data file, fsync, advance and persist
     * checkpoint_lsn, then clear the WAL. The ordering (apply -> fsync
     * data -> advance+fsync superblock -> clear WAL) lives inside
     * sdb_pager_checkpoint and preserves absolute durability — a crash
     * between any two steps leaves the WAL intact and replayable on the
     * next open, so no committed txn is ever lost. A no-op when the WAL
     * index is empty; skipped when a prior failure flagged needs_recovery
     * (the intact WAL is then the source of truth for the next open).
     */
    if (!database->pager.needs_recovery) {
        status = sdb_pager_checkpoint(&database->pager);
    } else {
        status = SDB_OK;
    }
    {
        const sdb_status close_status = sdb_pager_close(&database->pager);
        if (status == SDB_OK) {
            status = close_status;
        }
    }
    database->open = false;
    lock_status = sdb_process_lock_release(&database->process_lock);
    path_lock_status = sdb_process_lock_release(&database->path_lock);
    sdb_mutex_unlock(&database->mutex);
    sdb_mutex_destroy(&database->mutex);
    free(database->path);
    free(database);
    if (status != SDB_OK) {
        return status;
    }
    return lock_status != SDB_OK ? lock_status : path_lock_status;
}

static sdb_status sdb_kv_put_unlocked(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_engine_mutation mutation = {0};
    sdb_status status = sdb_engine_mutation_begin(database, &mutation);
    if (status == SDB_OK) {
        status = sdb_object_put(
        database,
        &mutation,
        (uint16_t)SDB_OBJECT_KV,
        namespace_name,
        namespace_size,
        key,
        key_size,
        value,
        value_size);
    }
    return status == SDB_OK || mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
}

static sdb_status sdb_kv_get_unlocked(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    return sdb_object_get(
        database,
        NULL,
        (uint16_t)SDB_OBJECT_KV,
        namespace_name,
        namespace_size,
        key,
        key_size,
        value_out,
        value_capacity,
        value_size_out
    );
}

static sdb_status sdb_kv_delete_unlocked(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    sdb_engine_mutation mutation = {0};
    sdb_status status = sdb_engine_mutation_begin(database, &mutation);
    if (status == SDB_OK) {
        status = sdb_object_delete(
        database,
        &mutation,
        (uint16_t)SDB_OBJECT_KV,
        namespace_name,
        namespace_size,
        key,
        key_size);
    }
    return status == SDB_OK || mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
}

static sdb_status sdb_blob_put_unlocked(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_engine_mutation mutation = {0};
    sdb_status status = sdb_engine_mutation_begin(database, &mutation);
    if (status == SDB_OK) {
        status = sdb_object_put(
        database,
        &mutation,
        (uint16_t)SDB_OBJECT_BLOB,
        namespace_name,
        namespace_size,
        key,
        key_size,
        value,
        value_size);
    }
    return status == SDB_OK || mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
}

static sdb_status sdb_blob_get_unlocked(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    return sdb_object_get(
        database,
        NULL,
        (uint16_t)SDB_OBJECT_BLOB,
        namespace_name,
        namespace_size,
        key,
        key_size,
        value_out,
        value_capacity,
        value_size_out
    );
}

static sdb_status sdb_blob_delete_unlocked(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    sdb_engine_mutation mutation = {0};
    sdb_status status = sdb_engine_mutation_begin(database, &mutation);
    if (status == SDB_OK) {
        status = sdb_object_delete(
        database,
        &mutation,
        (uint16_t)SDB_OBJECT_BLOB,
        namespace_name,
        namespace_size,
        key,
        key_size);
    }
    return status == SDB_OK || mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
}

static sdb_status sdb_index_base_key(
    uint8_t prefix,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    const uint8_t *value,
    size_t value_size,
    size_t suffix_size,
    uint8_t **key_out,
    size_t *key_size_out
)
{
    size_t size;
    uint8_t *key = NULL;
    if (!sdb_engine_bytes_valid(collection, collection_size)
        || !sdb_engine_bytes_valid(index_name, index_name_size)
        || (value == NULL && value_size != 0U)
        || value_size > SDB_ENGINE_MAX_NAME_SIZE
        || collection_size > SIZE_MAX - index_name_size - value_size - 7U
        || suffix_size
            > SIZE_MAX - collection_size - index_name_size - value_size - 7U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    size = 7U + collection_size + index_name_size + value_size + suffix_size;
    key = (uint8_t *)malloc(size);
    if (key == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    key[0] = prefix;
    sdb_write_u16_le(key + 1U, (uint16_t)collection_size);
    sdb_write_u16_le(key + 3U, (uint16_t)index_name_size);
    sdb_write_u16_le(key + 5U, (uint16_t)value_size);
    (void)memcpy(key + 7U, collection, collection_size);
    (void)memcpy(key + 7U + collection_size, index_name, index_name_size);
    if (value_size != 0U) {
        (void)memcpy(
            key + 7U + collection_size + index_name_size,
            value,
            value_size
        );
    }
    *key_out = key;
    *key_size_out = size;
    return SDB_OK;
}

static sdb_status sdb_index_create_in_mutation(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
)
{
    uint8_t *key = NULL;
    size_t key_size;
    uint8_t definition = unique ? 1U : 0U;
    sdb_status status;
    if (database == NULL || !database->open) {
        return SDB_E_INVALID_ARGUMENT;
    }
    {
        bool existing_unique;
        status = sdb_index_definition(
            database,
            mutation,
            collection,
            collection_size,
            index_name,
            index_name_size,
            &existing_unique
        );
        if (status == SDB_OK) {
            return existing_unique == unique ? SDB_OK : SDB_E_CONFLICT;
        }
        if (status != SDB_E_NOT_FOUND) {
            return status;
        }
    }
    status = sdb_index_base_key(
        SDB_INDEX_DEFINITION_PREFIX,
        collection,
        collection_size,
        index_name,
        index_name_size,
        NULL,
        0U,
        0U,
        &key,
        &key_size
    );
    if (status == SDB_OK) {
        status = sdb_engine_tree_put(
            database, mutation, key, key_size,
            &definition, sizeof(definition)
        );
    }
    free(key);
    return status;
}

static sdb_status sdb_index_create_unlocked(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
)
{
    sdb_engine_mutation mutation = {0};
    sdb_status status = sdb_engine_mutation_begin(database, &mutation);
    if (status == SDB_OK) {
        status = sdb_index_create_in_mutation(
            database, &mutation, collection, collection_size,
            index_name, index_name_size, unique
        );
    }
    return mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
}

static sdb_status sdb_index_definition(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool *unique_out
)
{
    uint8_t *key = NULL;
    size_t key_size;
    uint8_t definition = 0U;
    size_t definition_size = 0U;
    sdb_status status = sdb_index_base_key(
        SDB_INDEX_DEFINITION_PREFIX,
        collection,
        collection_size,
        index_name,
        index_name_size,
        NULL,
        0U,
        0U,
        &key,
        &key_size
    );
    if (status == SDB_OK) {
        status = sdb_engine_tree_get(
            database,
            mutation,
            key,
            key_size,
            &definition,
            sizeof(definition),
            &definition_size
        );
    }
    free(key);
    if (status != SDB_OK) {
        return status;
    }
    if (definition_size != 1U || definition > 1U) {
        return SDB_E_CORRUPT;
    }
    *unique_out = definition != 0U;
    return SDB_OK;
}

static sdb_status sdb_unique_guard_check(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const sdb_index_term *term,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    uint8_t *guard_key = NULL;
    size_t guard_key_size;
    uint8_t guard_value[2U + SDB_ENGINE_MAX_NAME_SIZE + 8U];
    size_t guard_value_size = 0U;
    sdb_status status = sdb_index_base_key(
        SDB_UNIQUE_GUARD_PREFIX,
        collection,
        collection_size,
        term->index_name,
        term->index_name_size,
        term->value,
        term->value_size,
        0U,
        &guard_key,
        &guard_key_size
    );
    if (status == SDB_OK) {
        status = sdb_engine_tree_get(
            database,
            mutation,
            guard_key,
            guard_key_size,
            guard_value,
            sizeof(guard_value),
            &guard_value_size
        );
    }
    free(guard_key);
    if (status == SDB_E_NOT_FOUND) {
        return SDB_OK;
    }
    if (status != SDB_OK) {
        return status;
    }
    if (guard_value_size < 10U) {
        return SDB_E_CORRUPT;
    }
    {
        const size_t guarded_id_size =
            (size_t)sdb_read_u16_le(guard_value + 8U);
        const uint64_t guarded_generation = sdb_read_u64_le(guard_value);
        sdb_object_metadata guarded_metadata;
        if (guarded_id_size != guard_value_size - 10U) {
            return SDB_E_CORRUPT;
        }
        if (guarded_id_size == document_id_size
            && memcmp(
                guard_value + 10U, document_id, document_id_size
            ) == 0) {
            return SDB_OK;
        }
        status = sdb_object_read_metadata(
            database,
            mutation,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            collection,
            collection_size,
            guard_value + 10U,
            guarded_id_size,
            &guarded_metadata
        );
        if (status == SDB_E_NOT_FOUND) {
            return SDB_OK;
        }
        if (status != SDB_OK) {
            return status;
        }
        return guarded_metadata.generation == guarded_generation
            ? SDB_E_CONFLICT : SDB_OK;
    }
}

static sdb_status sdb_index_write_term(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const sdb_index_term *term,
    const uint8_t *document_id,
    size_t document_id_size,
    uint64_t generation,
    bool unique
)
{
    uint8_t *entry_key = NULL;
    size_t entry_key_size;
    size_t suffix_size;
    sdb_status status;
    if (document_id_size > SIZE_MAX - 10U) {
        return SDB_E_OVERFLOW;
    }
    suffix_size = 2U + document_id_size + 8U;
    status = sdb_index_base_key(
        SDB_INDEX_ENTRY_PREFIX,
        collection,
        collection_size,
        term->index_name,
        term->index_name_size,
        term->value,
        term->value_size,
        suffix_size,
        &entry_key,
        &entry_key_size
    );
    if (status == SDB_OK) {
        uint8_t *suffix = entry_key + entry_key_size - suffix_size;
        sdb_write_u16_le(suffix, (uint16_t)document_id_size);
        (void)memcpy(suffix + 2U, document_id, document_id_size);
        sdb_write_u64_le(suffix + 2U + document_id_size, generation);
        status = sdb_engine_tree_put(
            database, mutation, entry_key, entry_key_size, NULL, 0U
        );
    }
    free(entry_key);
    if (status != SDB_OK || !unique) {
        return status;
    }
    {
        uint8_t *guard_key = NULL;
        size_t guard_key_size;
        uint8_t guard_value[2U + SDB_ENGINE_MAX_NAME_SIZE + 8U];
        status = sdb_index_base_key(
            SDB_UNIQUE_GUARD_PREFIX,
            collection,
            collection_size,
            term->index_name,
            term->index_name_size,
            term->value,
            term->value_size,
            0U,
            &guard_key,
            &guard_key_size
        );
        if (status == SDB_OK) {
            sdb_write_u64_le(guard_value, generation);
            sdb_write_u16_le(
                guard_value + 8U, (uint16_t)document_id_size
            );
            (void)memcpy(
                guard_value + 10U, document_id, document_id_size
            );
            status = sdb_engine_tree_put(
                database,
                mutation,
                guard_key,
                guard_key_size,
                guard_value,
                10U + document_id_size
            );
        }
        free(guard_key);
    }
    return status;
}

/*
 * Reverse rows: per-term (document, term-ordinal) -> (generation, one index
 * term) bookkeeping rows. Each row records ONE (index_name, value) term the
 * current version of a document wrote, plus that version's generation. They
 * exist for churn reclamation: a later overwrite or delete enumerates a
 * document's reverse rows to learn which per-term index_entry / unique_guard
 * rows the previous version left behind (keyed by that OLD generation) and
 * deletes exactly those, so indexed documents no longer orphan index rows.
 *
 * ONE row PER TERM (rather than one row packing every term) so each stays well
 * under the single-leaf payload limit: the b-tree has no overflow pages, so a
 * document indexing several ~1KB terms would otherwise fail to store an
 * all-terms value. The monotonic generation still guards against ABA, so a
 * stale row a crash might leave is never mistaken for live.
 *
 * Key   = pair_key(0x53, DOCUMENT, collection, document_id) + [ordinal u16].
 *         Ordinals are dense: 0 .. term_count-1, assigned in term order.
 * Value = [generation u64][name_size u16][name][value_size u16][value].
 */
static sdb_status sdb_index_reverse_key(
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    uint16_t ordinal,
    uint8_t **key_out,
    size_t *key_size_out
)
{
    uint8_t *key = NULL;
    size_t key_size;
    sdb_status status = sdb_engine_pair_key(
        SDB_INDEX_REVERSE_PREFIX,
        (uint8_t)SDB_OBJECT_DOCUMENT,
        collection,
        collection_size,
        document_id,
        document_id_size,
        2U,
        &key,
        &key_size
    );
    if (status != SDB_OK) {
        return status;
    }
    sdb_write_u16_le(key + key_size - 2U, ordinal);
    *key_out = key;
    *key_size_out = key_size;
    return SDB_OK;
}

static sdb_status sdb_index_reverse_encode_term(
    uint64_t generation,
    const sdb_index_term *term,
    uint8_t *value,
    size_t value_capacity,
    size_t *value_size_out
)
{
    size_t needed;
    if (!sdb_engine_bytes_valid(term->index_name, term->index_name_size)
        || !sdb_engine_bytes_valid(term->value, term->value_size)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    needed = 12U + term->index_name_size + term->value_size;
    if (value_capacity < needed) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    sdb_write_u64_le(value, generation);
    sdb_write_u16_le(value + 8U, (uint16_t)term->index_name_size);
    (void)memcpy(value + 10U, term->index_name, term->index_name_size);
    sdb_write_u16_le(
        value + 10U + term->index_name_size, (uint16_t)term->value_size
    );
    (void)memcpy(
        value + 12U + term->index_name_size, term->value, term->value_size
    );
    *value_size_out = needed;
    return SDB_OK;
}

/*
 * Decode one reverse row's value into its generation and term. The returned
 * term points INTO `value`, valid only while the caller keeps it alive. Every
 * field is bounds-checked so a corrupt row is rejected, not read out of range.
 */
static sdb_status sdb_index_reverse_decode_term(
    const uint8_t *value,
    size_t value_size,
    uint64_t *generation_out,
    sdb_index_term *term_out
)
{
    size_t name_size;
    size_t term_value_size;
    if (value == NULL || value_size < 12U) {
        return SDB_E_CORRUPT;
    }
    name_size = (size_t)sdb_read_u16_le(value + 8U);
    if (name_size == 0U || name_size > SDB_ENGINE_MAX_NAME_SIZE
        || value_size < 12U + name_size) {
        return SDB_E_CORRUPT;
    }
    term_value_size = (size_t)sdb_read_u16_le(value + 10U + name_size);
    if (term_value_size == 0U || term_value_size > SDB_ENGINE_MAX_NAME_SIZE
        || value_size != 12U + name_size + term_value_size) {
        return SDB_E_CORRUPT;
    }
    *generation_out = sdb_read_u64_le(value);
    term_out->index_name = value + 10U;
    term_out->index_name_size = name_size;
    term_out->value = value + 12U + name_size;
    term_out->value_size = term_value_size;
    return SDB_OK;
}

/*
 * Delete a term's unique_guard ONLY IF this document still owns it. The guard
 * key carries no generation (one guard per index value), so we must not blindly
 * delete it — another document may have taken the value. The guard value is
 * [generation u64][doc_id_size u16][doc_id]; ownership is decided by the stored
 * doc_id, matching the ownership semantics sdb_unique_guard_check enforces.
 */
static sdb_status sdb_document_reclaim_guard(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const sdb_index_term *term,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    uint8_t *guard_key = NULL;
    size_t guard_key_size;
    uint8_t guard_value[2U + SDB_ENGINE_MAX_NAME_SIZE + 8U];
    size_t guard_value_size = 0U;
    sdb_status status = sdb_index_base_key(
        SDB_UNIQUE_GUARD_PREFIX,
        collection,
        collection_size,
        term->index_name,
        term->index_name_size,
        term->value,
        term->value_size,
        0U,
        &guard_key,
        &guard_key_size
    );
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_engine_tree_get(
        database, mutation, guard_key, guard_key_size,
        guard_value, sizeof(guard_value), &guard_value_size
    );
    if (status == SDB_E_NOT_FOUND) {
        free(guard_key);
        return SDB_OK;
    }
    if (status == SDB_OK) {
        if (guard_value_size < 10U) {
            status = SDB_E_CORRUPT;
        } else {
            const size_t guarded_id_size =
                (size_t)sdb_read_u16_le(guard_value + 8U);
            if (guarded_id_size != guard_value_size - 10U) {
                status = SDB_E_CORRUPT;
            } else if (guarded_id_size == document_id_size
                && memcmp(
                    guard_value + 10U, document_id, document_id_size
                ) == 0) {
                status = sdb_engine_tree_delete(
                    database, mutation, guard_key, guard_key_size
                );
            }
            /* else: another document owns the guard — leave it in place. */
        }
    }
    free(guard_key);
    return status;
}

/*
 * Reclaim the index rows a single superseded term left behind: delete its
 * per-generation index_entry row and, for a unique index, its unique_guard iff
 * this document still owns it. index_entry rows are generation-keyed, so the
 * OLD one never collides with a freshly written new-generation entry.
 */
static sdb_status sdb_document_reclaim_one_term(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const sdb_index_term *term,
    const uint8_t *document_id,
    size_t document_id_size,
    uint64_t generation
)
{
    uint8_t *entry_key = NULL;
    size_t entry_key_size;
    size_t suffix_size;
    bool unique = false;
    sdb_status status;
    if (document_id_size > SIZE_MAX - 10U) {
        return SDB_E_OVERFLOW;
    }
    suffix_size = 2U + document_id_size + 8U;
    status = sdb_index_base_key(
        SDB_INDEX_ENTRY_PREFIX,
        collection,
        collection_size,
        term->index_name,
        term->index_name_size,
        term->value,
        term->value_size,
        suffix_size,
        &entry_key,
        &entry_key_size
    );
    if (status == SDB_OK) {
        uint8_t *suffix = entry_key + entry_key_size - suffix_size;
        sdb_write_u16_le(suffix, (uint16_t)document_id_size);
        (void)memcpy(suffix + 2U, document_id, document_id_size);
        sdb_write_u64_le(suffix + 2U + document_id_size, generation);
        status = sdb_engine_tree_delete(
            database, mutation, entry_key, entry_key_size
        );
    }
    free(entry_key);
    if (status == SDB_E_NOT_FOUND) {
        /*
         * Duplicate index terms on one document (e.g. a multi-value index over
         * tags=["red","red"]) map to the SAME index_entry key, which the first
         * write collapses into one row. A later duplicate term's reclaim then
         * finds that shared key already deleted; treat NOT_FOUND as idempotent
         * success instead of aborting the whole put/delete (which would leave
         * the document permanently un-updatable/un-deletable).
         */
        status = SDB_OK;
    }
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_index_definition(
        database, mutation, collection, collection_size,
        term->index_name, term->index_name_size, &unique
    );
    if (status == SDB_E_NOT_FOUND) {
        /*
         * Index definition absent (never created / dropped): the index_entry we
         * just deleted is all there was to reclaim.
         */
        return SDB_OK;
    }
    if (status != SDB_OK) {
        return status;
    }
    if (unique) {
        status = sdb_document_reclaim_guard(
            database, mutation, collection, collection_size,
            term, document_id, document_id_size
        );
    }
    return status;
}

/*
 * Enumerate and reclaim a document's reverse rows (its previous version's index
 * bookkeeping). Walks dense ordinals 0,1,2,... reading each reverse row THROUGH
 * the mutation (so it observes rows staged earlier in the SAME transaction, not
 * only committed ones — a raw tree cursor would miss those); stops at the first
 * absent ordinal. For each row it reclaims that term's index_entry + owned guard
 * and then deletes the reverse row itself.
 *
 * ORDERING CONTRACT (overwrite path): the caller MUST invoke this BEFORE staging
 * the new version's index_write_term rows. index_entry rows are generation-keyed
 * (old != new, always safe to delete), but the unique_guard key has NO
 * generation, so an unchanged unique term reuses the same guard key across
 * versions; reclaiming first deletes the old guard and lets the later
 * index_write_term re-create it (last write to a key in the batch wins).
 * Reclaiming AFTER the new write would delete the just-written guard. The
 * pure-delete path stages no new rows, so ordering is irrelevant there.
 */
static sdb_status sdb_document_reclaim_reverse(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    bool *found_out
)
{
    uint32_t ordinal;
    sdb_status status = SDB_OK;
    bool found = false;
    for (ordinal = 0U; ordinal <= (uint32_t)UINT16_MAX; ++ordinal) {
        uint8_t *reverse_key = NULL;
        size_t reverse_key_size;
        uint8_t reverse_value[SDB_INDEX_REVERSE_MAX_VALUE];
        size_t reverse_value_size;
        uint64_t generation = 0U;
        sdb_index_term term;
        status = sdb_index_reverse_key(
            collection, collection_size, document_id, document_id_size,
            (uint16_t)ordinal, &reverse_key, &reverse_key_size
        );
        if (status != SDB_OK) {
            break;
        }
        status = sdb_engine_tree_get(
            database, mutation, reverse_key, reverse_key_size,
            reverse_value, sizeof(reverse_value), &reverse_value_size
        );
        if (status == SDB_E_NOT_FOUND) {
            free(reverse_key);
            status = SDB_OK;
            break;
        }
        found = true;
        if (status == SDB_OK) {
            status = sdb_index_reverse_decode_term(
                reverse_value, reverse_value_size, &generation, &term
            );
        }
        if (status == SDB_OK && generation == 0U) {
            status = SDB_E_CORRUPT;
        }
        if (status == SDB_OK) {
            status = sdb_document_reclaim_one_term(
                database, mutation, collection, collection_size,
                &term, document_id, document_id_size, generation
            );
        }
        if (status == SDB_OK) {
            status = sdb_engine_tree_delete(
                database, mutation, reverse_key, reverse_key_size
            );
        }
        free(reverse_key);
        if (status != SDB_OK) {
            break;
        }
    }
    if (found_out != NULL) {
        *found_out = found;
    }
    return status;
}

typedef struct sdb_legacy_index_term {
    sdb_index_term term;
    uint64_t generation;
} sdb_legacy_index_term;

static void sdb_legacy_index_terms_destroy(
    sdb_legacy_index_term **terms, size_t count
)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        free(terms[i]);
    }
    free(terms);
}

static sdb_status sdb_document_collect_legacy_terms(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    sdb_legacy_index_term ***terms_out,
    size_t *count_out
)
{
    sdb_btree_cursor cursor;
    sdb_legacy_index_term **terms = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    const size_t buffer_capacity = sdb_pager_payload_capacity(&database->pager);
    uint8_t *key = (uint8_t *)malloc(buffer_capacity);
    uint8_t *value = (uint8_t *)malloc(buffer_capacity);
    const uint8_t prefix[1] = {SDB_INDEX_ENTRY_PREFIX};
    sdb_status status;
    if (key == NULL || value == NULL) {
        free(key);
        free(value);
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    status = sdb_btree_cursor_seek(
        &database->tree, prefix, sizeof(prefix), &cursor
    );
    while (status == SDB_OK && cursor.valid) {
        size_t key_size;
        size_t value_size;
        status = sdb_btree_cursor_read(
            &cursor, key, buffer_capacity, &key_size,
            value, buffer_capacity, &value_size
        );
        if (status != SDB_OK) {
            break;
        }
        if (key_size == 0U || key[0] != SDB_INDEX_ENTRY_PREFIX) {
            break;
        }
        {
            size_t stored_collection_size;
            size_t name_size;
            size_t term_value_size;
            size_t base_size;
            size_t stored_id_size;
            if (key_size < 17U || value_size != 0U) {
                status = SDB_E_CORRUPT;
                break;
            }
            stored_collection_size = (size_t)sdb_read_u16_le(key + 1U);
            name_size = (size_t)sdb_read_u16_le(key + 3U);
            term_value_size = (size_t)sdb_read_u16_le(key + 5U);
            if (stored_collection_size == 0U || name_size == 0U
                || term_value_size == 0U
                || stored_collection_size > SDB_ENGINE_MAX_NAME_SIZE
                || name_size > SDB_ENGINE_MAX_NAME_SIZE
                || term_value_size > SDB_ENGINE_MAX_NAME_SIZE
                || stored_collection_size > key_size - 7U
                || name_size > key_size - 7U - stored_collection_size
                || term_value_size > key_size - 7U
                    - stored_collection_size - name_size) {
                status = SDB_E_CORRUPT;
                break;
            }
            base_size = 7U + stored_collection_size + name_size
                + term_value_size;
            if (key_size < base_size + 10U) {
                status = SDB_E_CORRUPT;
                break;
            }
            stored_id_size = (size_t)sdb_read_u16_le(key + base_size);
            if (stored_id_size == 0U
                || stored_id_size > SDB_ENGINE_MAX_NAME_SIZE
                || key_size != base_size + 10U + stored_id_size) {
                status = SDB_E_CORRUPT;
                break;
            }
            if (stored_collection_size == collection_size
                && stored_id_size == document_id_size
                && memcmp(key + 7U, collection, collection_size) == 0
                && memcmp(
                    key + base_size + 2U, document_id, document_id_size
                ) == 0) {
                sdb_legacy_index_term *copy;
                uint8_t *bytes;
                if (count == capacity) {
                    size_t next_capacity = capacity == 0U ? 4U : capacity * 2U;
                    sdb_legacy_index_term **grown;
                    if (next_capacity < capacity
                        || next_capacity > SIZE_MAX / sizeof(*terms)) {
                        status = SDB_E_OVERFLOW;
                        break;
                    }
                    grown = (sdb_legacy_index_term **)realloc(
                        terms, next_capacity * sizeof(*terms)
                    );
                    if (grown == NULL) {
                        status = SDB_E_OUT_OF_MEMORY;
                        break;
                    }
                    terms = grown;
                    capacity = next_capacity;
                }
                if (name_size > SIZE_MAX - term_value_size
                    || sizeof(*copy) > SIZE_MAX - name_size - term_value_size) {
                    status = SDB_E_OVERFLOW;
                    break;
                }
                copy = (sdb_legacy_index_term *)malloc(
                    sizeof(*copy) + name_size + term_value_size
                );
                if (copy == NULL) {
                    status = SDB_E_OUT_OF_MEMORY;
                    break;
                }
                bytes = (uint8_t *)(copy + 1);
                (void)memcpy(
                    bytes, key + 7U + stored_collection_size, name_size
                );
                (void)memcpy(
                    bytes + name_size,
                    key + 7U + stored_collection_size + name_size,
                    term_value_size
                );
                copy->term.index_name = bytes;
                copy->term.index_name_size = name_size;
                copy->term.value = bytes + name_size;
                copy->term.value_size = term_value_size;
                copy->generation = sdb_read_u64_le(
                    key + base_size + 2U + stored_id_size
                );
                if (copy->generation == 0U) {
                    free(copy);
                    status = SDB_E_CORRUPT;
                    break;
                }
                terms[count++] = copy;
            }
        }
        status = sdb_btree_cursor_next(&cursor);
    }
    sdb_btree_cursor_close(&cursor);
    free(key);
    free(value);
    if (status != SDB_OK) {
        sdb_legacy_index_terms_destroy(terms, count);
        return status;
    }
    *terms_out = terms;
    *count_out = count;
    return SDB_OK;
}

static sdb_status sdb_document_reclaim_legacy_indexes(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    sdb_legacy_index_term **terms = NULL;
    size_t count = 0U;
    size_t i;
    sdb_status status = sdb_document_collect_legacy_terms(
        database, collection, collection_size, document_id, document_id_size,
        &terms, &count
    );
    for (i = 0U; status == SDB_OK && i < count; ++i) {
        status = sdb_document_reclaim_one_term(
            database, mutation, collection, collection_size,
            &terms[i]->term, document_id, document_id_size,
            terms[i]->generation
        );
    }
    sdb_legacy_index_terms_destroy(terms, count);
    return status;
}

/*
 * Write the reverse rows for a document's current version: one row per term,
 * dense ordinals 0..term_count-1, each carrying the new generation. On an
 * overwrite the caller has already reclaimed (and deleted) the previous
 * version's reverse rows, so these overwrite/re-create ordinals cleanly. A
 * termless document gets one format-compatible marker row so it is distinct
 * from a legacy document created before reverse rows existed; the marker uses
 * the ordinary reverse-term encoding and is safe for older v1 readers.
 * term_count is capped at UINT16_MAX by the caller.
 */
static sdb_status sdb_document_write_reverse(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    uint64_t generation,
    const sdb_index_term *terms,
    size_t term_count
)
{
    size_t i;
    sdb_status status = SDB_OK;
    if (term_count == 0U) {
        const uint8_t marker_byte[1] = {UINT8_C(0xff)};
        const sdb_index_term marker_term = {
            marker_byte, sizeof(marker_byte), marker_byte, sizeof(marker_byte)
        };
        uint8_t *reverse_key = NULL;
        size_t reverse_key_size;
        uint8_t marker[SDB_INDEX_REVERSE_MAX_VALUE];
        size_t marker_size = 0U;
        status = sdb_index_reverse_key(
            collection, collection_size, document_id, document_id_size,
            0U, &reverse_key, &reverse_key_size
        );
        if (status == SDB_OK) {
            status = sdb_index_reverse_encode_term(
                generation, &marker_term, marker, sizeof(marker), &marker_size
            );
        }
        if (status == SDB_OK) {
            status = sdb_engine_tree_put(
                database, mutation, reverse_key, reverse_key_size,
                marker, marker_size
            );
        }
        free(reverse_key);
        return status;
    }
    for (i = 0U; status == SDB_OK && i < term_count; ++i) {
        uint8_t *reverse_key = NULL;
        size_t reverse_key_size;
        uint8_t reverse_value[SDB_INDEX_REVERSE_MAX_VALUE];
        size_t reverse_value_size = 0U;
        status = sdb_index_reverse_key(
            collection, collection_size, document_id, document_id_size,
            (uint16_t)i, &reverse_key, &reverse_key_size
        );
        if (status == SDB_OK) {
            status = sdb_index_reverse_encode_term(
                generation, &terms[i],
                reverse_value, sizeof(reverse_value), &reverse_value_size
            );
        }
        if (status == SDB_OK) {
            status = sdb_engine_tree_put(
                database, mutation, reverse_key, reverse_key_size,
                reverse_value, reverse_value_size
            );
        }
        free(reverse_key);
    }
    return status;
}

#if SDB_TESTING
sdb_status sdb_engine_strip_document_reverse_for_testing(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    sdb_engine_mutation mutation = {0};
    uint32_t ordinal;
    sdb_status status;
    if (database == NULL || !database->open) {
        return SDB_E_INVALID_ARGUMENT;
    }
    sdb_mutex_lock(&database->mutex);
    status = sdb_engine_mutation_begin(database, &mutation);
    for (ordinal = 0U; status == SDB_OK
         && ordinal <= (uint32_t)UINT16_MAX; ++ordinal) {
        uint8_t *key = NULL;
        size_t key_size;
        status = sdb_index_reverse_key(
            collection, collection_size, document_id, document_id_size,
            (uint16_t)ordinal, &key, &key_size
        );
        if (status == SDB_OK) {
            status = sdb_engine_tree_delete(
                database, &mutation, key, key_size
            );
        }
        free(key);
        if (status == SDB_E_NOT_FOUND) {
            status = SDB_OK;
            break;
        }
    }
    status = mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
    sdb_mutex_unlock(&database->mutex);
    return status;
}
#endif

static sdb_status sdb_document_put_in_mutation(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size,
    const sdb_index_term *terms,
    size_t term_count
)
{
    sdb_object_metadata metadata;
    sdb_object_metadata previous;
    bool has_previous = false;
    bool *unique;
    size_t index;
    uint64_t revision_before = 0U;
    sdb_status status;
    if (database == NULL || !database->open
        || !sdb_engine_bytes_valid(document_id, document_id_size)
        || (document == NULL && document_size != 0U)
        || (terms == NULL && term_count != 0U)
        || term_count > SIZE_MAX / sizeof(*unique)
        || term_count > (size_t)UINT16_MAX) {
        /*
         * term_count is capped at UINT16_MAX because each term maps to a
         * reverse row addressed by a u16 ordinal.
         */
        return SDB_E_INVALID_ARGUMENT;
    }
    unique = term_count == 0U
        ? NULL : (bool *)calloc(term_count, sizeof(*unique));
    if (term_count != 0U && unique == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    revision_before = mutation != NULL ? mutation->batch.revision : 0U;
    /*
     * Read the PREVIOUS version's metadata before staging anything, so an
     * overwrite can reclaim what it superseded: the old generation + chunk_count
     * to delete the old chunks, and (its mere presence) tells us to enumerate
     * and reclaim the old reverse rows / index rows below. object_plan assigns a
     * FRESH monotonic generation and writes new keys, so without this the old
     * chunks and index rows would orphan on every update. NOT_FOUND just means a
     * first insert (nothing to reclaim); a genuine read error is fatal.
     */
    status = sdb_object_read_metadata(
        database, mutation, (uint16_t)SDB_OBJECT_DOCUMENT,
        collection, collection_size, document_id, document_id_size, &previous
    );
    if (status == SDB_OK) {
        has_previous = true;
    } else if (status != SDB_E_NOT_FOUND) {
        free(unique);
        return sdb_mutation_poison_on_partial(mutation, revision_before, status);
    }
    status = sdb_object_plan(
        database,
        mutation,
        (uint16_t)SDB_OBJECT_DOCUMENT,
        collection,
        collection_size,
        document_id,
        document_id_size,
        document,
        document_size,
        &metadata
    );
    for (index = 0U; status == SDB_OK && index < term_count; ++index) {
        if (!sdb_engine_bytes_valid(
                terms[index].index_name, terms[index].index_name_size
            )
            || !sdb_engine_bytes_valid(
                terms[index].value, terms[index].value_size
            )) {
            status = SDB_E_INVALID_ARGUMENT;
            break;
        }
        status = sdb_index_definition(
            database,
            mutation,
            collection,
            collection_size,
            terms[index].index_name,
            terms[index].index_name_size,
            &unique[index]
        );
        if (status == SDB_OK && unique[index]) {
            status = sdb_unique_guard_check(
                database,
                mutation,
                collection,
                collection_size,
                &terms[index],
                document_id,
                document_id_size
            );
        }
    }
    if (status == SDB_OK) {
        status = sdb_object_write_chunks(
            database,
            mutation,
            &metadata,
            collection,
            collection_size,
            document_id,
            document_id_size,
            document
        );
    }
    /*
     * Reclaim the previous version's index rows BEFORE writing the new ones.
     * See sdb_document_reclaim_reverse: this ordering keeps an unchanged unique
     * term's guard intact (delete old guard, then the new index_write_term below
     * re-creates it — last write to the key wins). Only an overwrite (a document
     * that already existed) can have reverse rows to reclaim.
     */
    if (status == SDB_OK && has_previous) {
        bool reverse_found = false;
        status = sdb_document_reclaim_reverse(
            database, mutation, collection, collection_size,
            document_id, document_id_size, &reverse_found
        );
        if (status == SDB_OK && !reverse_found) {
            status = sdb_document_reclaim_legacy_indexes(
                database, mutation, collection, collection_size,
                document_id, document_id_size
            );
        }
    }
    for (index = 0U; status == SDB_OK && index < term_count; ++index) {
        status = sdb_index_write_term(
            database,
            mutation,
            collection,
            collection_size,
            &terms[index],
            document_id,
            document_id_size,
            metadata.generation,
            unique[index]
        );
    }
    /*
     * Record the new version's terms: one reverse row per term (the previous
     * version's reverse rows were reclaimed above).
     */
    if (status == SDB_OK) {
        status = sdb_document_write_reverse(
            database, mutation, collection, collection_size,
            document_id, document_id_size, metadata.generation,
            terms, term_count
        );
    }
    if (status == SDB_OK) {
        status = sdb_object_commit_metadata(
            database,
            mutation,
            &metadata,
            collection,
            collection_size,
            document_id,
            document_id_size
        );
    }
    /*
     * Reclaim the previous version's chunks (keyed by the OLD generation, so
     * they cannot collide with the freshly-written new-generation chunks).
     */
    if (status == SDB_OK && has_previous) {
        status = sdb_object_delete_chunks(
            database, mutation, (uint16_t)SDB_OBJECT_DOCUMENT,
            collection, collection_size, document_id, document_id_size,
            previous.generation, previous.chunk_count
        );
    }
    free(unique);
    return sdb_mutation_poison_on_partial(mutation, revision_before, status);
}

static sdb_status sdb_document_put_unlocked(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size,
    const sdb_index_term *terms,
    size_t term_count
)
{
    sdb_engine_mutation mutation = {0};
    sdb_status status = sdb_engine_mutation_begin(database, &mutation);
    if (status == SDB_OK) {
        status = sdb_document_put_in_mutation(
            database, &mutation, collection, collection_size,
            document_id, document_id_size, document, document_size,
            terms, term_count
        );
    }
    return mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
}

static sdb_status sdb_document_get_unlocked(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    uint8_t *document_out,
    size_t document_capacity,
    size_t *document_size_out
)
{
    return sdb_object_get(
        database,
        NULL,
        (uint16_t)SDB_OBJECT_DOCUMENT,
        collection,
        collection_size,
        document_id,
        document_id_size,
        document_out,
        document_capacity,
        document_size_out
    );
}

/*
 * Delete a document AND the index rows it left behind. Enumerates the
 * document's per-term reverse rows, reclaims each term's index_entry and owned
 * unique_guard and deletes each reverse row, then falls through to
 * sdb_object_delete (metadata + chunks, unchanged). Shared by the auto-commit
 * and explicit-transaction delete paths so both reclaim identically. Deleting
 * an absent document stays idempotent: no reverse rows to enumerate and
 * object_delete treats a missing metadata row as a no-op.
 */
static sdb_status sdb_document_delete_in_mutation(
    sdb_database *database,
    sdb_engine_mutation *mutation,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    uint64_t revision_before;
    sdb_object_metadata metadata;
    bool reverse_found = false;
    sdb_status status;
    if (database == NULL || !database->open
        || !sdb_engine_bytes_valid(document_id, document_id_size)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    revision_before = mutation != NULL ? mutation->batch.revision : 0U;
    status = sdb_object_read_metadata(
        database, mutation, (uint16_t)SDB_OBJECT_DOCUMENT,
        collection, collection_size, document_id, document_id_size, &metadata
    );
    if (status == SDB_OK) {
        status = sdb_document_reclaim_reverse(
            database, mutation, collection, collection_size,
            document_id, document_id_size, &reverse_found
        );
    }
    if (status == SDB_OK && !reverse_found) {
        status = sdb_document_reclaim_legacy_indexes(
            database, mutation, collection, collection_size,
            document_id, document_id_size
        );
    }
    if (status == SDB_E_NOT_FOUND) {
        status = SDB_OK;
    }
    if (status == SDB_OK) {
        status = sdb_object_delete(
            database,
            mutation,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            collection,
            collection_size,
            document_id,
            document_id_size
        );
    }
    return sdb_mutation_poison_on_partial(mutation, revision_before, status);
}

static sdb_status sdb_document_delete_unlocked(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    sdb_engine_mutation mutation = {0};
    sdb_status status = sdb_engine_mutation_begin(database, &mutation);
    if (status == SDB_OK) {
        status = sdb_document_delete_in_mutation(
            database,
            &mutation,
            collection,
            collection_size,
            document_id,
            document_id_size
        );
    }
    return mutation.active
        ? sdb_engine_mutation_finish(&mutation, status) : status;
}

static sdb_status sdb_index_visit_unlocked(
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
)
{
    uint8_t *prefix;
    size_t prefix_size;
    sdb_btree_cursor cursor;
    uint8_t *key_buffer;
    uint8_t *resume_key;
    uint8_t *value_buffer;
    size_t capacity;
    size_t matches = 0U;
    sdb_status status;
    if (database == NULL || !database->open || visitor == NULL
        || match_count_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_index_base_key(
        SDB_INDEX_ENTRY_PREFIX,
        collection,
        collection_size,
        index_name,
        index_name_size,
        value,
        value_size,
        0U,
        &prefix,
        &prefix_size
    );
    if (status != SDB_OK) {
        return status;
    }
    capacity = sdb_pager_payload_capacity(&database->pager);
    key_buffer = (uint8_t *)malloc(capacity);
    resume_key = (uint8_t *)malloc(capacity);
    value_buffer = (uint8_t *)malloc(capacity);
    if (key_buffer == NULL || resume_key == NULL || value_buffer == NULL) {
        free(prefix);
        free(key_buffer);
        free(resume_key);
        free(value_buffer);
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    status = sdb_btree_cursor_seek(
        &database->tree, prefix, prefix_size, &cursor
    );
    while (status == SDB_OK && cursor.valid) {
        size_t current_key_size;
        size_t current_value_size;
        size_t document_id_size;
        uint64_t generation;
        sdb_object_metadata metadata;
        status = sdb_btree_cursor_read(
            &cursor,
            key_buffer,
            capacity,
            &current_key_size,
            value_buffer,
            capacity,
            &current_value_size
        );
        if (status != SDB_OK) {
            break;
        }
        if (current_key_size < prefix_size
            || memcmp(key_buffer, prefix, prefix_size) != 0) {
            break;
        }
        if (current_value_size != 0U
            || current_key_size < prefix_size + 10U) {
            status = SDB_E_CORRUPT;
            break;
        }
        document_id_size =
            (size_t)sdb_read_u16_le(key_buffer + prefix_size);
        if (document_id_size == 0U
            || current_key_size
                != prefix_size + 2U + document_id_size + 8U) {
            status = SDB_E_CORRUPT;
            break;
        }
        generation = sdb_read_u64_le(
            key_buffer + prefix_size + 2U + document_id_size
        );
        status = sdb_object_read_metadata(
            database,
            NULL,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            collection,
            collection_size,
            key_buffer + prefix_size + 2U,
            document_id_size,
            &metadata
        );
        if (status == SDB_OK && metadata.generation == generation) {
            bool keep_visiting;
            size_t resumed_key_size;
            size_t resumed_value_size;
            if (database->callback_depth == SIZE_MAX) {
                status = SDB_E_OVERFLOW;
                break;
            }
            (void)memcpy(
                resume_key, key_buffer, current_key_size
            );

            sdb_btree_cursor_close(&cursor);
            ++database->callback_depth;
            ++matches;
            keep_visiting = visitor(
                context,
                key_buffer + prefix_size + 2U,
                document_id_size
            );
            --database->callback_depth;
            if (!keep_visiting) {
                status = SDB_OK;
                break;
            }
            status = sdb_btree_cursor_seek(
                &database->tree, resume_key, current_key_size, &cursor
            );
            if (status != SDB_OK || !cursor.valid) {
                continue;
            }
            status = sdb_btree_cursor_read(
                &cursor,
                key_buffer,
                capacity,
                &resumed_key_size,
                value_buffer,
                capacity,
                &resumed_value_size
            );
            if (status == SDB_OK
                && resumed_key_size == current_key_size
                && memcmp(
                    key_buffer, resume_key, current_key_size
                ) == 0) {
                status = sdb_btree_cursor_next(&cursor);
            }
            continue;
        } else if (status == SDB_E_NOT_FOUND
            || (status == SDB_OK && metadata.generation != generation)) {
            status = SDB_OK;
        }
        if (status == SDB_OK) {
            status = sdb_btree_cursor_next(&cursor);
        }
    }
    sdb_btree_cursor_close(&cursor);
    free(prefix);
    free(key_buffer);
    free(resume_key);
    free(value_buffer);
    if (status == SDB_OK) {
        *match_count_out = matches;
    }
    return status;
}

static sdb_status sdb_verify_pair_key(
    const uint8_t *key,
    size_t key_size,
    size_t suffix_size,
    uint16_t *kind_out,
    const uint8_t **namespace_out,
    size_t *namespace_size_out,
    const uint8_t **object_key_out,
    size_t *object_key_size_out
)
{
    size_t namespace_size;
    size_t object_key_size;
    if (key == NULL || key_size < 4U + suffix_size
        || kind_out == NULL || namespace_out == NULL
        || namespace_size_out == NULL || object_key_out == NULL
        || object_key_size_out == NULL) {
        return SDB_E_CORRUPT;
    }
    /*
     * V2 layout: [prefix][kind][ns_len u16][namespace][user_key][suffix].
     * user_key length is the trailing remainder (not stored).
     */
    namespace_size = (size_t)sdb_read_u16_le(key + 2U);
    if (namespace_size == 0U
        || namespace_size > SDB_ENGINE_MAX_NAME_SIZE
        || namespace_size > key_size - 4U - suffix_size
        || (key[1] != (uint8_t)SDB_OBJECT_KV
            && key[1] != (uint8_t)SDB_OBJECT_BLOB
            && key[1] != (uint8_t)SDB_OBJECT_DOCUMENT)) {
        return SDB_E_CORRUPT;
    }
    object_key_size = key_size - 4U - suffix_size - namespace_size;
    if (object_key_size == 0U
        || object_key_size > SDB_ENGINE_MAX_NAME_SIZE) {
        return SDB_E_CORRUPT;
    }
    *kind_out = (uint16_t)key[1];
    *namespace_out = key + 4U;
    *namespace_size_out = namespace_size;
    *object_key_out = key + 4U + namespace_size;
    *object_key_size_out = object_key_size;
    return SDB_OK;
}

static sdb_status sdb_verify_object(
    sdb_database *database,
    const uint8_t *metadata_key,
    size_t metadata_key_size,
    const uint8_t *encoded,
    size_t encoded_size,
    sdb_verify_result *result
)
{
    sdb_object_metadata metadata = {0};
    sdb_sha256_context hash;
    uint8_t digest[32];
    uint8_t *chunk;
    const uint8_t *namespace_name;
    const uint8_t *object_key;
    size_t namespace_size;
    size_t object_key_size;
    uint16_t kind;
    uint32_t index;
    uint64_t produced = 0U;
    sdb_status status = sdb_verify_pair_key(
        metadata_key,
        metadata_key_size,
        0U,
        &kind,
        &namespace_name,
        &namespace_size,
        &object_key,
        &object_key_size
    );
    if (status == SDB_OK) {
        status = sdb_object_metadata_decode(
            encoded, encoded_size, kind, &metadata
        );
    }
    if (status != SDB_OK) {
        return status;
    }
    chunk = metadata.chunk_size == 0U
        ? NULL : (uint8_t *)malloc((size_t)metadata.chunk_size);
    if (metadata.chunk_size != 0U && chunk == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    sdb_sha256_init(&hash);
    for (index = 0U; status == SDB_OK && index < metadata.chunk_count; ++index) {
        uint8_t *chunk_key = NULL;
        size_t chunk_key_size;
        size_t chunk_size = 0U;
        status = sdb_chunk_key(
            kind,
            namespace_name,
            namespace_size,
            object_key,
            object_key_size,
            metadata.generation,
            index,
            &chunk_key,
            &chunk_key_size
        );
        if (status == SDB_OK) {
            status = sdb_btree_get(
                &database->tree,
                chunk_key,
                chunk_key_size,
                chunk,
                (size_t)metadata.chunk_size,
                &chunk_size
            );
        }
        free(chunk_key);
        if (status == SDB_OK
            && (chunk_size == 0U
                || chunk_size > (size_t)metadata.chunk_size
                || produced > metadata.total_size
                || (uint64_t)chunk_size > metadata.total_size - produced)) {
            status = SDB_E_CORRUPT;
        }
        if (status == SDB_OK) {
            sdb_sha256_update(&hash, chunk, chunk_size);
            produced += (uint64_t)chunk_size;
            ++result->live_chunk_count;
        }
    }
    if (status == SDB_OK && produced != metadata.total_size) {
        status = SDB_E_CORRUPT;
    }
    if (status == SDB_OK) {
        sdb_sha256_final(&hash, digest);
        if (!sdb_constant_time_equal(
                digest, metadata.digest, sizeof(digest)
            )) {
            status = SDB_E_CORRUPT;
        }
        sdb_secure_zero(digest, sizeof(digest));
    } else {
        sdb_secure_zero(&hash, sizeof(hash));
    }
    if (chunk != NULL) {
        sdb_secure_zero(chunk, (size_t)metadata.chunk_size);
    }
    free(chunk);
    if (status == SDB_OK) {
        ++result->object_count;
        if (metadata.total_size
            > UINT64_MAX - result->logical_byte_count) {
            return SDB_E_OVERFLOW;
        }
        result->logical_byte_count += metadata.total_size;
    }
    return status;
}

static sdb_status sdb_verify_index_key(
    sdb_database *database,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size,
    sdb_verify_result *result
)
{
    size_t collection_size;
    size_t index_name_size;
    size_t term_size;
    size_t base_size;
    if (key_size < 7U) {
        return SDB_E_CORRUPT;
    }
    collection_size = (size_t)sdb_read_u16_le(key + 1U);
    index_name_size = (size_t)sdb_read_u16_le(key + 3U);
    term_size = (size_t)sdb_read_u16_le(key + 5U);
    if (collection_size == 0U || index_name_size == 0U
        || collection_size > SDB_ENGINE_MAX_NAME_SIZE
        || index_name_size > SDB_ENGINE_MAX_NAME_SIZE
        || term_size > SDB_ENGINE_MAX_NAME_SIZE
        || collection_size > key_size - 7U
        || index_name_size > key_size - 7U - collection_size
        || term_size > key_size - 7U - collection_size - index_name_size) {
        return SDB_E_CORRUPT;
    }
    base_size = 7U + collection_size + index_name_size + term_size;
    if (key[0] == SDB_INDEX_DEFINITION_PREFIX) {
        return term_size == 0U && key_size == base_size
            && value_size == 1U && value[0] <= 1U
            ? SDB_OK : SDB_E_CORRUPT;
    }
    if (key[0] == SDB_INDEX_ENTRY_PREFIX) {
        size_t document_id_size;
        uint64_t generation;
        sdb_object_metadata metadata;
        sdb_status status;
        if (value_size != 0U || key_size < base_size + 10U) {
            return SDB_E_CORRUPT;
        }
        document_id_size = (size_t)sdb_read_u16_le(key + base_size);
        if (document_id_size == 0U
            || document_id_size > SDB_ENGINE_MAX_NAME_SIZE
            || key_size != base_size + 2U + document_id_size + 8U) {
            return SDB_E_CORRUPT;
        }
        generation = sdb_read_u64_le(
            key + base_size + 2U + document_id_size
        );
        if (generation == 0U) {
            return SDB_E_CORRUPT;
        }
        status = sdb_object_read_metadata(
            database,
            NULL,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            key + 7U,
            collection_size,
            key + base_size + 2U,
            document_id_size,
            &metadata
        );
        if (status == SDB_E_NOT_FOUND
            || (status == SDB_OK && metadata.generation != generation)) {
            ++result->stale_entry_count;
            return SDB_OK;
        }
        return status;
    }
    if (key[0] == SDB_UNIQUE_GUARD_PREFIX) {
        size_t document_id_size;
        uint64_t generation;
        sdb_object_metadata metadata;
        sdb_status status;
        if (key_size != base_size || value_size < 10U) {
            return SDB_E_CORRUPT;
        }
        generation = sdb_read_u64_le(value);
        document_id_size = (size_t)sdb_read_u16_le(value + 8U);
        if (generation == 0U || document_id_size == 0U
            || document_id_size > SDB_ENGINE_MAX_NAME_SIZE
            || value_size != 10U + document_id_size) {
            return SDB_E_CORRUPT;
        }
        status = sdb_object_read_metadata(
            database,
            NULL,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            key + 7U,
            collection_size,
            value + 10U,
            document_id_size,
            &metadata
        );
        if (status == SDB_E_NOT_FOUND
            || (status == SDB_OK && metadata.generation != generation)) {
            ++result->stale_entry_count;
            return SDB_OK;
        }
        return status;
    }
    return SDB_E_CORRUPT;
}

static sdb_status sdb_engine_entry_is_live(
    sdb_database *database,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size,
    bool *live_out
)
{
    if (live_out == NULL || key == NULL || key_size == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *live_out = false;
    if (key[0] == SDB_SEQUENCE_KEY_PREFIX) {
        /*
         * The generation-counter system row is always live: it must survive
         * compaction (which keeps only live rows) so the monotonic counter is
         * preserved across a compact+reopen and generations never rewind.
         */
        *live_out = true;
        return SDB_OK;
    }
    if (key[0] == SDB_OBJECT_KEY_PREFIX) {
        const uint8_t *namespace_name;
        const uint8_t *object_key;
        size_t namespace_size;
        size_t object_key_size;
        sdb_object_metadata metadata;
        uint16_t kind;
        sdb_status status = sdb_verify_pair_key(
            key,
            key_size,
            0U,
            &kind,
            &namespace_name,
            &namespace_size,
            &object_key,
            &object_key_size
        );
        (void)namespace_name;
        (void)namespace_size;
        (void)object_key;
        (void)object_key_size;
        if (status == SDB_OK) {
            status = sdb_object_metadata_decode(
                value, value_size, kind, &metadata
            );
        }
        if (status == SDB_OK) {
            *live_out = true;
        }
        return status;
    }
    if (key[0] == SDB_CHUNK_KEY_PREFIX) {
        const uint8_t *namespace_name;
        const uint8_t *object_key;
        size_t namespace_size;
        size_t object_key_size;
        sdb_object_metadata metadata;
        uint16_t kind;
        uint64_t generation = 0U;
        uint32_t chunk_index;
        sdb_status status = sdb_verify_pair_key(
            key,
            key_size,
            12U,
            &kind,
            &namespace_name,
            &namespace_size,
            &object_key,
            &object_key_size
        );
        if (status != SDB_OK || value_size == 0U) {
            return status != SDB_OK ? status : SDB_E_CORRUPT;
        }
        generation = sdb_read_u64_le(key + key_size - 12U);
        chunk_index = sdb_read_u32_le(key + key_size - 4U);
        if (generation == 0U) {
            return SDB_E_CORRUPT;
        }
        status = sdb_object_read_metadata(
            database,
            NULL,
            kind,
            namespace_name,
            namespace_size,
            object_key,
            object_key_size,
            &metadata
        );
        if (status == SDB_E_NOT_FOUND) {
            return SDB_OK;
        }
        if (status != SDB_OK) {
            return status;
        }
        *live_out = generation == metadata.generation
            && chunk_index < metadata.chunk_count;
        return SDB_OK;
    }
    if (key[0] == SDB_INDEX_DEFINITION_PREFIX) {
        sdb_verify_result ignored;
        sdb_status status;
        (void)memset(&ignored, 0, sizeof(ignored));
        status = sdb_verify_index_key(
            database, key, key_size, value, value_size, &ignored
        );
        if (status == SDB_OK) {
            *live_out = true;
        }
        return status;
    }
    if (key[0] == SDB_INDEX_REVERSE_PREFIX) {
        /*
         * A per-term reverse row is live iff it still describes the current
         * version of its document. Its key is pair_key(...) + a u16 ordinal
         * suffix (hence suffix_size 2 below), and its value decodes to one term
         * carrying that version's generation. This liveness check mirrors the
         * chunk/index branches so a stale row a crash might leave (document
         * gone, or a newer generation present) is dropped by compact and counted
         * dead by verify instead of resurrecting.
         */
        const uint8_t *namespace_name;
        const uint8_t *object_key;
        size_t namespace_size;
        size_t object_key_size;
        uint16_t kind;
        uint64_t generation = 0U;
        sdb_index_term term;
        sdb_object_metadata metadata;
        sdb_status status = sdb_verify_pair_key(
            key,
            key_size,
            2U,
            &kind,
            &namespace_name,
            &namespace_size,
            &object_key,
            &object_key_size
        );
        if (status == SDB_OK && kind != (uint16_t)SDB_OBJECT_DOCUMENT) {
            status = SDB_E_CORRUPT;
        }
        if (status == SDB_OK) {
            status = sdb_index_reverse_decode_term(
                value, value_size, &generation, &term
            );
        }
        if (status == SDB_OK && generation == 0U) {
            status = SDB_E_CORRUPT;
        }
        if (status != SDB_OK) {
            return status;
        }
        status = sdb_object_read_metadata(
            database,
            NULL,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            namespace_name,
            namespace_size,
            object_key,
            object_key_size,
            &metadata
        );
        if (status == SDB_E_NOT_FOUND) {
            return SDB_OK;
        }
        if (status != SDB_OK) {
            return status;
        }
        *live_out = metadata.generation == generation;
        return SDB_OK;
    }
    if (key[0] == SDB_INDEX_ENTRY_PREFIX
        || key[0] == SDB_UNIQUE_GUARD_PREFIX) {
        size_t collection_size;
        size_t index_name_size;
        size_t term_size;
        size_t base_size;
        size_t document_id_size;
        const uint8_t *document_id;
        uint64_t generation;
        sdb_object_metadata metadata;
        sdb_status status;
        if (key_size < 7U) {
            return SDB_E_CORRUPT;
        }
        collection_size = (size_t)sdb_read_u16_le(key + 1U);
        index_name_size = (size_t)sdb_read_u16_le(key + 3U);
        term_size = (size_t)sdb_read_u16_le(key + 5U);
        if (collection_size == 0U || index_name_size == 0U
            || collection_size > key_size - 7U
            || index_name_size
                > key_size - 7U - collection_size
            || term_size
                > key_size - 7U - collection_size - index_name_size) {
            return SDB_E_CORRUPT;
        }
        base_size = 7U + collection_size + index_name_size + term_size;
        if (key[0] == SDB_INDEX_ENTRY_PREFIX) {
            if (value_size != 0U || key_size < base_size + 10U) {
                return SDB_E_CORRUPT;
            }
            document_id_size =
                (size_t)sdb_read_u16_le(key + base_size);
            if (document_id_size == 0U
                || key_size
                    != base_size + 2U + document_id_size + 8U) {
                return SDB_E_CORRUPT;
            }
            document_id = key + base_size + 2U;
            generation = sdb_read_u64_le(
                document_id + document_id_size
            );
        } else {
            if (key_size != base_size || value_size < 10U) {
                return SDB_E_CORRUPT;
            }
            generation = sdb_read_u64_le(value);
            document_id_size =
                (size_t)sdb_read_u16_le(value + 8U);
            if (document_id_size == 0U
                || value_size != 10U + document_id_size) {
                return SDB_E_CORRUPT;
            }
            document_id = value + 10U;
        }
        if (generation == 0U
            || document_id_size > SDB_ENGINE_MAX_NAME_SIZE) {
            return SDB_E_CORRUPT;
        }
        status = sdb_object_read_metadata(
            database,
            NULL,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            key + 7U,
            collection_size,
            document_id,
            document_id_size,
            &metadata
        );
        if (status == SDB_E_NOT_FOUND) {
            return SDB_OK;
        }
        if (status != SDB_OK) {
            return status;
        }
        *live_out = metadata.generation == generation;
        return SDB_OK;
    }
    return SDB_E_CORRUPT;
}

static sdb_status sdb_database_verify_unlocked(
    sdb_database *database, sdb_verify_result *result_out
)
{
    sdb_btree_verify_result tree_result;
    sdb_btree_cursor cursor;
    sdb_verify_result result;
    uint8_t *key;
    uint8_t *value;
    uint8_t *page = NULL;
    bool *free_pages = NULL;
    bool *linked_free_pages = NULL;
    size_t capacity;
    size_t page_count;
    sdb_status status = SDB_OK;
    if (database == NULL || !database->open || result_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(&result, 0, sizeof(result));
    if (database->pager.superblock.next_page_id > (uint64_t)SIZE_MAX) {
        return SDB_E_OVERFLOW;
    }
    page_count = (size_t)database->pager.superblock.next_page_id;
    page = (uint8_t *)malloc(
        (size_t)database->pager.superblock.page_size
    );
    free_pages = (bool *)calloc(page_count, sizeof(*free_pages));
    linked_free_pages = (bool *)calloc(
        page_count, sizeof(*linked_free_pages)
    );
    if (page == NULL || free_pages == NULL || linked_free_pages == NULL) {
        free(page);
        free(free_pages);
        free(linked_free_pages);
        return SDB_E_OUT_OF_MEMORY;
    }
    {
        uint64_t page_id;
        for (page_id = 1U;
             page_id < database->pager.superblock.next_page_id;
             ++page_id) {
            sdb_page_view view;
            status = sdb_pager_read(
                &database->pager,
                page_id,
                page,
                (size_t)database->pager.superblock.page_size,
                &view
            );
            if (status != SDB_OK) {
                free(page);
                free(free_pages);
                free(linked_free_pages);
                return status;
            }
            if (view.type == (uint16_t)SDB_PAGE_TYPE_FREE) {
                if (view.payload_size != 8U
                    || sdb_read_u64_le(view.payload)
                        >= database->pager.superblock.next_page_id) {
                    free(page);
                    free(free_pages);
                    free(linked_free_pages);
                    return SDB_E_CORRUPT;
                }
                free_pages[(size_t)page_id] = true;
                ++result.free_page_count;
            } else if (view.type != (uint16_t)SDB_PAGE_TYPE_DATA) {
                free(page);
                free(free_pages);
                free(linked_free_pages);
                return SDB_E_CORRUPT;
            }
        }
    }
    {
        uint64_t page_id = database->pager.superblock.freelist_page;
        while (page_id != 0U) {
            sdb_page_view view;
            if (page_id >= database->pager.superblock.next_page_id
                || !free_pages[(size_t)page_id]
                || linked_free_pages[(size_t)page_id]) {
                free(page);
                free(free_pages);
                free(linked_free_pages);
                return SDB_E_CORRUPT;
            }
            linked_free_pages[(size_t)page_id] = true;
            status = sdb_pager_read(
                &database->pager,
                page_id,
                page,
                (size_t)database->pager.superblock.page_size,
                &view
            );
            if (status != SDB_OK) {
                free(page);
                free(free_pages);
                free(linked_free_pages);
                return status;
            }
            page_id = sdb_read_u64_le(view.payload);
        }
    }
    {
        size_t page_index;
        for (page_index = 1U; page_index < page_count; ++page_index) {
            if (free_pages[page_index] != linked_free_pages[page_index]) {
                free(page);
                free(free_pages);
                free(linked_free_pages);
                return SDB_E_CORRUPT;
            }
        }
    }
    free(page);
    page = NULL;
    status = sdb_btree_verify_pages(
        &database->tree, &tree_result, linked_free_pages, page_count
    );
    if (status != SDB_OK) {
        free(free_pages);
        free(linked_free_pages);
        return status;
    }
    {
        size_t page_index;
        for (page_index = 1U; page_index < page_count; ++page_index) {
            if (free_pages[page_index] == linked_free_pages[page_index]) {
                free(free_pages);
                free(linked_free_pages);
                return SDB_E_CORRUPT;
            }
        }
    }
    free(free_pages);
    free(linked_free_pages);
    result.allocated_page_count =
        database->pager.superblock.next_page_id - 1U;
    result.btree_node_count = tree_result.node_count;
    result.btree_leaf_count = tree_result.leaf_count;
    result.raw_entry_count = tree_result.entry_count;
    result.btree_height = tree_result.height;
    capacity = sdb_pager_payload_capacity(&database->pager);
    key = (uint8_t *)malloc(capacity);
    value = (uint8_t *)malloc(capacity);
    if (key == NULL || value == NULL) {
        free(key);
        free(value);
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    status = sdb_btree_cursor_first(&database->tree, &cursor);
    while (status == SDB_OK && cursor.valid) {
        size_t key_size;
        size_t value_size;
        status = sdb_btree_cursor_read(
            &cursor,
            key,
            capacity,
            &key_size,
            value,
            capacity,
            &value_size
        );
        if (status != SDB_OK) {
            break;
        }
        if (key[0] == SDB_OBJECT_KEY_PREFIX) {
            status = sdb_verify_object(
                database, key, key_size, value, value_size, &result
            );
        } else if (key[0] == SDB_CHUNK_KEY_PREFIX) {
            const uint8_t *namespace_name;
            const uint8_t *object_key;
            size_t namespace_size;
            size_t object_key_size;
            uint16_t kind;
            status = sdb_verify_pair_key(
                key,
                key_size,
                12U,
                &kind,
                &namespace_name,
                &namespace_size,
                &object_key,
                &object_key_size
            );
            (void)kind;
            (void)namespace_name;
            (void)namespace_size;
            (void)object_key;
            (void)object_key_size;
            if (status == SDB_OK
                && (sdb_read_u64_le(key + key_size - 12U) == 0U
                    || value_size == 0U)) {
                status = SDB_E_CORRUPT;
            }
            if (status == SDB_OK) {
                bool live;
                status = sdb_engine_entry_is_live(
                    database, key, key_size, value, value_size, &live
                );
                if (status == SDB_OK && !live) {
                    ++result.stale_entry_count;
                }
            }
        } else if (key[0] == SDB_SEQUENCE_KEY_PREFIX) {
            /* System counter row: not an object/index, nothing to verify. */
        } else if (key[0] == SDB_INDEX_REVERSE_PREFIX) {
            /*
             * Reverse (doc -> terms) bookkeeping row. entry_is_live validates
             * its structure and compares its generation to the live document;
             * count it toward the dead-row total when it no longer matches, the
             * same way chunk/index rows are counted, so verify's stale metric
             * stays honest and a malformed row is reported as corruption.
             */
            bool live;
            status = sdb_engine_entry_is_live(
                database, key, key_size, value, value_size, &live
            );
            if (status == SDB_OK && !live) {
                ++result.stale_entry_count;
            }
        } else {
            status = sdb_verify_index_key(
                database, key, key_size, value, value_size, &result
            );
        }
        if (status == SDB_OK) {
            status = sdb_btree_cursor_next(&cursor);
        }
    }
    sdb_btree_cursor_close(&cursor);
    free(key);
    free(value);
    if (status == SDB_OK) {
        *result_out = result;
    }
    return status;
}

static sdb_status sdb_backup_temp_path(
    const char *destination, char **path_out
)
{
    static const char suffix[] = ".tmp-";
    static const char hex[] = "0123456789abcdef";
    uint8_t random[8];
    size_t destination_size;
    size_t total_size;
    size_t index;
    char *path;
    sdb_status status;
    if (destination == NULL || destination[0] == '\0' || path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    destination_size = strlen(destination);
    if (destination_size > SIZE_MAX - sizeof(suffix) - 16U) {
        return SDB_E_OVERFLOW;
    }
    total_size = destination_size + sizeof(suffix) - 1U + 16U + 1U;
    path = (char *)malloc(total_size);
    if (path == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    status = sdb_random_bytes(random, sizeof(random));
    if (status != SDB_OK) {
        free(path);
        return status;
    }
    (void)memcpy(path, destination, destination_size);
    (void)memcpy(
        path + destination_size, suffix, sizeof(suffix) - 1U
    );
    for (index = 0U; index < sizeof(random); ++index) {
        const size_t offset =
            destination_size + sizeof(suffix) - 1U + (index * 2U);
        path[offset] = hex[random[index] >> 4U];
        path[offset + 1U] = hex[random[index] & 0x0fU];
    }
    path[total_size - 1U] = '\0';
    sdb_secure_zero(random, sizeof(random));
    *path_out = path;
    return SDB_OK;
}

static sdb_status sdb_engine_append_suffix(
    const char *path, const char *suffix, char **output
)
{
    size_t path_size;
    size_t suffix_size;
    char *result;
    if (path == NULL || suffix == NULL || output == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    path_size = strlen(path);
    suffix_size = strlen(suffix);
    if (path_size > SIZE_MAX - suffix_size - 1U) {
        return SDB_E_OVERFLOW;
    }
    result = (char *)malloc(path_size + suffix_size + 1U);
    if (result == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memcpy(result, path, path_size);
    (void)memcpy(result + path_size, suffix, suffix_size + 1U);
    *output = result;
    return SDB_OK;
}

static sdb_status sdb_database_backup_unlocked(
    sdb_database *database,
    const char *destination_path,
    bool replace_existing,
    sdb_backup_result *result_out
)
{
    sdb_process_lock destination_lock;
    sdb_verify_result verify_result = {0};
    sdb_file temporary;
    char *resolved_destination = NULL;
    char *temporary_path = NULL;
    uint8_t *buffer = NULL;
    uint64_t file_size = 0U;
    uint64_t offset = 0U;
    bool temporary_open = false;
    bool temporary_exists = false;
    bool replacement_done = false;
    bool destination_exists = false;
    sdb_status status;
    if (database == NULL || !database->open || destination_path == NULL
        || destination_path[0] == '\0' || result_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(&destination_lock, 0, sizeof(destination_lock));
#ifndef _WIN32
    destination_lock.descriptor = -1;
#endif
    status = sdb_file_path_exists(
        destination_path, &destination_exists
    );
    if (status == SDB_OK) {
        status = sdb_file_resolve_database_path(
            destination_path,
            destination_exists,
            &resolved_destination
        );
    }
    if (status == SDB_OK
        && strcmp(database->path, resolved_destination) == 0) {
        status = SDB_E_INVALID_ARGUMENT;
    }
    if (status == SDB_OK && destination_exists && !replace_existing) {
        /*
         * Destination already exists and the caller did not ask to replace
         * it. Report SDB_E_CONFLICT ("already exists") here rather than
         * letting the atomic-replace step surface a generic SDB_E_IO — it
         * mirrors the create-onto-existing mapping so "already there" is
         * distinguishable from a real disk error. Done before acquiring the
         * destination lock or creating the temp file, so nothing to unwind.
         */
        status = SDB_E_CONFLICT;
    }
    if (status == SDB_OK) {
        status = sdb_process_lock_acquire(
            resolved_destination, &destination_lock
        );
    }
    if (status == SDB_OK) {
        status = sdb_database_verify_unlocked(database, &verify_result);
    }
    if (status == SDB_OK) {
        /*
         * WAL-mode: page contents live in the WAL until checkpoint runs.
         * A raw byte-for-byte copy of the data file would omit committed-
         * but-not-checkpointed pages, so flush WAL frames into the data
         * file first — this leaves the source in a self-consistent state
         * that copies verbatim into the backup destination.
         *
         * A backup is semantically a read: a checkpoint failure here must
         * NOT poison the live source. sdb_pager_checkpoint is crash-safe
         * on failure in either phase:
         *   - Failure BEFORE the superblock is advanced leaves the WAL,
         *     wal_index and wal_tail intact (reads keep serving from the
         *     WAL) and flags needs_recovery. Clearing that flag here keeps
         *     the source usable: nothing durable moved, so the next commit
         *     appends at the correct tail and the full WAL replays on the
         *     next open.
         *   - Failure AFTER the superblock is advanced (e.g. wal_clear hit
         *     EIO) leaves needs_recovery unset and an already self-
         *     consistent state: wal_index is empty and wal_tail is reset to
         *     the header, so the next commit appends from the start and no
         *     committed txn is stranded past a stale tail. The clear below
         *     is then a harmless no-op.
         * Either way the backup itself still fails via `status`.
         *
         * Only clear a fence THIS checkpoint raised: if needs_recovery was
         * already set on entry (a prior commit failure fenced the DB), leave
         * it — erasing it would re-enable mutations on an un-recovered pager.
         * In practice verify_unlocked above already fails via the pager read
         * gate when the fence is pre-set, so this block is not reached then;
         * capturing the prior state makes the invariant explicit and robust
         * regardless of that ordering.
         */
        const bool recovery_fenced_before = database->pager.needs_recovery;
        status = sdb_pager_checkpoint(&database->pager);
        if (status != SDB_OK && !recovery_fenced_before) {
            database->pager.needs_recovery = false;
        }
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&database->pager.file);
    }
    if (status == SDB_OK) {
        status = sdb_file_size(&database->pager.file, &file_size);
    }
    if (status == SDB_OK) {
        status = sdb_backup_temp_path(
            resolved_destination, &temporary_path
        );
    }
    if (status == SDB_OK) {
        status = sdb_file_create_new(temporary_path, &temporary);
        temporary_open = status == SDB_OK;
        temporary_exists = status == SDB_OK;
        if (temporary_open) {
#if SDB_TESTING
            sdb_file_fail_after_for_testing(
                &temporary, sdb_backup_file_fail_after_for_testing
            );
#endif
        }
    }
    if (status == SDB_OK) {
        buffer = (uint8_t *)malloc((size_t)65536U);
        if (buffer == NULL) {
            status = SDB_E_OUT_OF_MEMORY;
        }
    }
    while (status == SDB_OK && offset < file_size) {
        const uint64_t remaining = file_size - offset;
        const size_t chunk = remaining < UINT64_C(65536)
            ? (size_t)remaining : (size_t)65536U;
        status = sdb_file_read_full(
            &database->pager.file, offset, buffer, chunk
        );
        if (status == SDB_OK) {
            status = sdb_file_write_full(
                &temporary, offset, buffer, chunk
            );
        }
        offset += status == SDB_OK ? (uint64_t)chunk : 0U;
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&temporary);
    }
    if (temporary_open) {
        const sdb_status close_status = sdb_file_close(&temporary);
        if (status == SDB_OK) {
            status = close_status;
        }
    }
    if (status == SDB_OK) {
        sdb_backup_test_crash(1U);
    }
    if (status == SDB_OK) {
        status = sdb_replace_prepare(
            resolved_destination, database->pager.superblock.file_id
        );
    }
    if (status == SDB_OK) {
        sdb_backup_test_crash(2U);
    }
    if (status == SDB_OK) {
        status = sdb_file_replace(
            temporary_path, resolved_destination, replace_existing
        );
        if (status == SDB_OK) {
            temporary_exists = false;
            replacement_done = true;
            sdb_backup_test_crash(3U);
        }
    }
    if (status == SDB_OK) {
        status = sdb_file_sync_parent_directory(resolved_destination);
    }
    if (status == SDB_OK) {
        sdb_backup_test_crash(4U);
    }
    if (replacement_done) {
        const sdb_status finish_status =
            sdb_replace_finish(resolved_destination);
        if (status == SDB_OK) {
            status = finish_status;
        }
    } else {
        (void)sdb_replace_abort(resolved_destination);
    }
    if (temporary_exists) {
        (void)sdb_file_remove(temporary_path, true);
    }
    free(buffer);
    free(temporary_path);
    if (destination_lock.held) {
        const sdb_status lock_status =
            sdb_process_lock_release(&destination_lock);
        if (status == SDB_OK) {
            status = lock_status;
        }
    }
    if (status == SDB_OK) {
        (void)memset(result_out, 0, sizeof(*result_out));
        result_out->byte_count = file_size;

        result_out->raw_entry_count = verify_result.raw_entry_count;
    }
    free(resolved_destination);
    return status;
}

static void sdb_compact_remove_artifacts(
    const char *path, const char *wal_path, const char *lock_path
)
{
    if (wal_path != NULL) {
        (void)sdb_file_remove(wal_path, true);
    }
    if (path != NULL) {
        (void)sdb_file_remove(path, true);
    }
    if (lock_path != NULL) {
        (void)sdb_file_remove(lock_path, true);
    }
}

static sdb_status sdb_database_compact_unlocked(
    sdb_database *database,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
)
{
    sdb_verify_result source_verify;
    sdb_verify_result target_verify = {0};
    sdb_database *target = NULL;
    sdb_btree_cursor cursor;
    sdb_btree_batch target_batch;
    bool target_batch_active = false;
#ifdef _WIN32
    bool target_file_suspended = false;
#endif
    size_t staged_in_batch = 0U;
    const size_t compact_batch = 4096U;
    uint8_t *key = NULL;
    uint8_t *value = NULL;
    char *temporary_path = NULL;
    char *temporary_wal_path = NULL;
    char *temporary_lock_path = NULL;
    char *source_wal_path = NULL;
    size_t capacity;
    uint64_t before_size = 0U;
    uint64_t after_size = 0U;
    bool replaced = false;
    sdb_status status;
    if (database == NULL || !database->open || result_out == NULL
        || sdb_database_options_validate(target_options) != SDB_OK) {
        return SDB_E_INVALID_ARGUMENT;
    }

    if (database->active_transaction != NULL
        || database->active_snapshot != NULL
        || database->commit_in_flight != 0U) {
        return SDB_E_BUSY;
    }
    /*
     * Encryption-at-rest must not be silently dropped. If the source is
     * encrypted but the target options carry no password, sdb_database_create
     * below would build a PLAINTEXT target and the atomic swap would replace
     * the encrypted file with a world-readable one — a caller who merely
     * wanted to reclaim space (e.g. the Python binding's compact()/migrate()
     * default password=None) losing the product's core guarantee. Refuse: the
     * caller must supply a password to keep (or rotate) encryption. Encrypting
     * a plaintext source via a password-bearing target is still allowed; that
     * only adds protection.
     */
    if (database->pager.encryption_enabled
        && target_options->password_size == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_database_verify_unlocked(database, &source_verify);
    if (status == SDB_OK) {
        status = sdb_file_size(&database->pager.file, &before_size);
    }
    if (status == SDB_OK) {
        status = sdb_backup_temp_path(database->path, &temporary_path);
    }
    if (status == SDB_OK) {
        status = sdb_engine_append_suffix(
            temporary_path, ".wal", &temporary_wal_path
        );
    }
    if (status == SDB_OK) {
        status = sdb_engine_append_suffix(
            temporary_path, ".lock", &temporary_lock_path
        );
    }
    if (status == SDB_OK) {
        status = sdb_engine_append_suffix(
            database->path, ".wal", &source_wal_path
        );
    }
    if (status == SDB_OK) {
        status = sdb_database_create(
            temporary_path, target_options, &target
        );
    }
    capacity = sdb_pager_payload_capacity(&database->pager);
    if (status == SDB_OK) {
        key = (uint8_t *)malloc(capacity);
        value = (uint8_t *)malloc(capacity);
        if (key == NULL || value == NULL) {
            status = SDB_E_OUT_OF_MEMORY;
        }
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    (void)memset(&target_batch, 0, sizeof(target_batch));
    if (status == SDB_OK) {
        status = sdb_btree_cursor_first(&database->tree, &cursor);
    }
    if (status == SDB_OK) {
        status = sdb_btree_batch_begin(&target->tree, &target_batch);
        target_batch_active = status == SDB_OK;
    }
    while (status == SDB_OK && cursor.valid) {
        size_t key_size;
        size_t value_size;
        bool live = false;
        status = sdb_btree_cursor_read(
            &cursor,
            key,
            capacity,
            &key_size,
            value,
            capacity,
            &value_size
        );
        if (status == SDB_OK) {
            status = sdb_engine_entry_is_live(
                database, key, key_size, value, value_size, &live
            );
        }
        if (status == SDB_OK && live) {
            status = sdb_btree_batch_put(
                &target_batch, key, key_size, value, value_size
            );
            if (status == SDB_OK
                && ++staged_in_batch >= compact_batch) {
                status = sdb_btree_batch_commit(&target_batch);
                target_batch_active = false;
                staged_in_batch = 0U;
                if (status == SDB_OK) {
                    status = sdb_btree_batch_begin(
                        &target->tree, &target_batch
                    );
                    target_batch_active = status == SDB_OK;
                }
            }
        }
        if (status == SDB_OK) {
            status = sdb_btree_cursor_next(&cursor);
        }
    }
    sdb_btree_cursor_close(&cursor);
    if (status == SDB_OK && target_batch_active) {
        /*
         * commit consumes the batch even when it reports an error; clear the
         * ownership flag first so the cleanup path cannot abort it twice.
         */
        target_batch_active = false;
        status = sdb_btree_batch_commit(&target_batch);
    }
    if (target_batch_active) {
        sdb_btree_batch_abort(&target_batch);
    }
    if (status == SDB_OK) {
        /*
         * WAL-mode: the target's commits during compact went to
         * temporary_wal_path. After the file_replace below we discard
         * target's WAL, so drain it into the target data file now —
         * otherwise the swapped-in pager would carry a wal_index whose
         * offsets point at a WAL file that is about to disappear.
         */
        status = sdb_pager_checkpoint(&target->pager);
        SDB_WINDOWS_TEST_DIAG("compact target checkpoint", status);
    }
    if (status == SDB_OK) {
        status = sdb_database_verify_unlocked(target, &target_verify);
    }
    if (status == SDB_OK) {
        status = sdb_file_size(&target->pager.file, &after_size);
    }
    if (status == SDB_OK) {
        sdb_compact_test_crash(1U);
    }
    if (status == SDB_OK) {
        status = sdb_replace_prepare(
            database->path, target->pager.superblock.file_id
        );
    }
    if (status == SDB_OK) {
        sdb_compact_test_crash(2U);
    }
#ifdef _WIN32
    /*
     * ReplaceFileW requires the replacement source itself to be closed. The
     * destination pager may stay open because all ShibaDB data handles share
     * delete access. Reopen the target through its canonical destination
     * immediately after the atomic swap, before adopting its pager.
     */
    if (status == SDB_OK) {
        status = sdb_file_close(&target->pager.file);
        target_file_suspended = status == SDB_OK;
        SDB_WINDOWS_TEST_DIAG("compact target file suspend", status);
    }
#endif
    if (status == SDB_OK) {
        status = sdb_file_replace(
            temporary_path, database->path, true
        );
        SDB_WINDOWS_TEST_DIAG("compact file replace", status);
        replaced = status == SDB_OK;
    }
#ifdef _WIN32
    if (target_file_suspended) {
        const char *reopen_path = replaced ? database->path : temporary_path;
        const sdb_status reopen_status = sdb_file_open_existing(
            reopen_path, true, &target->pager.file
        );
        target_file_suspended = reopen_status != SDB_OK;
        if (status == SDB_OK) {
            status = reopen_status;
        }
        SDB_WINDOWS_TEST_DIAG("compact target file reopen", reopen_status);
    }
#endif
    if (replaced) {
        const sdb_status directory_status =
            sdb_file_sync_parent_directory(database->path);
        sdb_compact_test_crash(3U);
        {
            const sdb_status finish_status =
                sdb_replace_finish(database->path);
            SDB_WINDOWS_TEST_DIAG("compact replace finish", finish_status);
            if (status == SDB_OK) {
                status = finish_status;
            }
        }
        const sdb_status close_status =
            sdb_pager_close(&database->pager);
        SDB_WINDOWS_TEST_DIAG("compact source pager close", close_status);
        char *old_target_wal = target->pager.wal_path;
        /*
         * R2a: the swapped-in pager still carries target's persistent WAL fd,
         * which points at temporary_wal_path — the very file unlinked below.
         * Close it now so the adopted pager (wal_path reset to
         * source_wal_path just after) lazily reopens the correct source WAL
         * on its next commit instead of writing through an fd whose inode has
         * been unlinked. target's WAL was already drained by the checkpoint
         * above, so nothing is lost.
         */
        if (target->pager.wal_file_open) {
            const sdb_status target_wal_close =
                sdb_file_close(&target->pager.wal_file);
            SDB_WINDOWS_TEST_DIAG("compact target WAL close", target_wal_close);
            target->pager.wal_file_open = false;
            if (status == SDB_OK) {
                status = target_wal_close;
            }
        }
        /*
         * CRITICAL: the swapped-in pager adopts source_wal_path, but its
         * in-memory wal_index is empty and wal_tail sits at the header
         * (target's checkpoint already drained its WAL, so the compacted
         * data file is authoritative). The on-disk source WAL, however,
         * still holds committed-but-uncheckpointed frames from the
         * PRE-compact page layout. sdb_wal_recover_all scans frames from
         * SDB_WAL_HEADER_SIZE unconditionally (no file_id/header gate), so
         * on the next open a stale frame whose txn_id == checkpoint_lsn+1
         * would validate and be replayed over the compacted B-tree —
         * destructive corruption. Reset the adopted WAL on disk so it
         * matches the empty in-memory index; that data already lives in
         * the compacted file, so nothing is lost.
         */
        {
            sdb_status wal_reset_status = sdb_wal_clear(source_wal_path);
            if (wal_reset_status != SDB_OK) {
                /*
                 * sdb_wal_clear only truncates the file; a transient failure
                 * (EIO/ENOSPC) leaves the source .wal alive carrying ITS OWN
                 * identity header (file_id = source). After the swap the data
                 * file advertises file_id = target, so the next open's
                 * recover_all identity gate would see crc_ok && file_id
                 * mismatch and return SDB_E_CORRUPT -- permanently wedging the
                 * freshly-compacted (and fully durable) database until the
                 * stale .wal is deleted by hand. Remove it outright instead:
                 * the adopted in-memory wal_index is already empty and
                 * wal_tail sits at the header, so those frames live in the
                 * compacted data file and nothing is lost. A clean absence
                 * beats a foreign-identity sidecar that trips the gate.
                 */
                wal_reset_status = sdb_file_remove(source_wal_path, true);
            }
            SDB_WINDOWS_TEST_DIAG("compact adopted WAL reset", wal_reset_status);
            if (status == SDB_OK) {
                status = wal_reset_status;
            }
        }
        target->pager.wal_path = source_wal_path;
        source_wal_path = NULL;
        database->pager = target->pager;
        /*
         * target->pager still holds a copy of the compacted DB's 32-byte
         * data_key. Scrub it with sdb_secure_zero (volatile-backed), NOT plain
         * memset: this is a dead store before free(target) below, which the
         * compiler's dead-store elimination would drop at -O2/LTO, leaving the
         * key recoverable in freed heap. Matches the codebase's zero-secrets
         * policy used elsewhere.
         */
        sdb_secure_zero(&target->pager, sizeof(target->pager));
        database->tree = target->tree;
        database->tree.pager = &database->pager;
        (void)memset(&target->tree, 0, sizeof(target->tree));
        target->open = false;
        if (status == SDB_OK) {
            /*
             * Re-seed the in-memory generation counter for the freshly adopted
             * tree. The target was created on an empty tree (next_object_
             * generation = 0) and the live rows were copied raw via
             * sdb_btree_batch_put, which never advances the counter, so the
             * adopted pager still reads 0 while the compacted rows carry the
             * source's high generations. The counter row itself is copied (it
             * is always live), so a reopen would re-seed correctly — but
             * without this an in-place write on the still-open handle allocates
             * a generation that collides with a live object's, and object_put's
             * delete of the "previous" version erases the row it just wrote:
             * silent data loss that persists because alloc rewrites the counter
             * row back down. Seeding from the adopted tree mirrors the open
             * path exactly, so the in-memory counter and a later reopen agree.
             */
            status = sdb_engine_seed_generation(database);
        }
        {
            const sdb_status identity_status =
                sdb_process_lock_release(&database->process_lock);
            SDB_WINDOWS_TEST_DIAG("compact identity release", identity_status);
            database->process_lock = target->process_lock;
            target->process_lock.held = false;
            if (status == SDB_OK) {
                status = identity_status;
            }
        }
        {
            const sdb_status target_path_status =
                sdb_process_lock_release(&target->path_lock);
            SDB_WINDOWS_TEST_DIAG("compact target path release", target_path_status);
            if (status == SDB_OK) {
                status = target_path_status;
            }
        }
        sdb_mutex_destroy(&target->mutex);
        free(target->path);
        free(target);
        target = NULL;
        (void)sdb_file_remove(old_target_wal, true);
        free(old_target_wal);
        if (status == SDB_OK) {
            status = directory_status;
        }
        SDB_WINDOWS_TEST_DIAG("compact directory sync", directory_status);
        if (status == SDB_OK) {
            status = close_status;
        }
        if (status == SDB_OK) {
            (void)memset(result_out, 0, sizeof(*result_out));
            result_out->byte_count_before = before_size;
            result_out->byte_count_after = after_size;
            result_out->raw_entries_before = source_verify.raw_entry_count;
            result_out->raw_entries_after = target_verify.raw_entry_count;
        } else {

            database->pager.needs_recovery = true;
        }
    }
    if (target != NULL) {
        (void)sdb_database_close(target);
    }
    if (!replaced) {
        (void)sdb_replace_abort(database->path);
        sdb_compact_remove_artifacts(
            temporary_path, temporary_wal_path, temporary_lock_path
        );
    } else {
        (void)sdb_file_remove(temporary_wal_path, true);
        (void)sdb_file_remove(temporary_lock_path, true);
    }
    free(key);
    free(value);
    free(temporary_path);
    free(temporary_wal_path);
    free(temporary_lock_path);
    free(source_wal_path);
    return status;
}

#define SDB_ENGINE_LOCK_OR_RETURN(database_) \
    do { \
        if ((database_) == NULL || !(database_)->open) { \
            return SDB_E_INVALID_ARGUMENT; \
        } \
        sdb_mutex_lock(&(database_)->mutex); \
    } while (0)

/*
 * R4 group-commit tail for the auto-commit API wrappers. Entered with
 * database->mutex HELD and deferred-commit armed; the PREPARE phase (the
 * *_unlocked op that just ran) has staged the txn into the coordinator queue
 * and advanced all in-memory state. This helper:
 *   1. reads the pending ticket and disarms defer (still under the mutex),
 *   2. RELEASES database->mutex so other threads can PREPARE concurrently,
 *   3. drives the staged txn to durability via the leader/follower protocol,
 *   4. on success, re-takes the mutex only long enough to run a deferred
 *      checkpoint if one is due (coordinator fully drained), and on failure
 *      flags needs_recovery so the poisoned handle is rejected until reopened.
 * A no-op mutation (e.g. deleting an absent key) stages nothing, so
 * has_pending is false and the fast path just unlocks and returns.
 */
static sdb_status sdb_engine_group_commit_finish(
    sdb_database *database, sdb_status status
)
{
    uint64_t pending_txn = 0U;
    const bool has_pending =
        sdb_pager_take_pending(&database->pager, &pending_txn);
    const bool will_commit = status == SDB_OK && has_pending;
    sdb_pager_set_defer_commit(&database->pager, false);
    /*
     * Join the off-mutex durability window BEFORE releasing the engine mutex,
     * so a compact/backup/close that acquires the mutex the instant we drop it
     * sees a non-zero count and refuses instead of swapping/closing the pager
     * while sdb_pager_commit_durable is still using commit_mutex/wal_file. The
     * count (not a bool) lets overlapping committers each hold their own share:
     * the window stays "busy" until the LAST in-flight committer leaves it.
     */
    if (will_commit) {
        database->commit_in_flight++;
    }
    sdb_mutex_unlock(&database->mutex);
    if (!will_commit) {
        return status;
    }
    status = sdb_pager_commit_durable(&database->pager, pending_txn);
    sdb_mutex_lock(&database->mutex);
    database->commit_in_flight--;
    if (status == SDB_OK) {
        /*
         * Only auto-checkpoint if no txn became active while we were off the
         * mutex doing the leader fsync. A concurrent sdb_transaction_begin
         * sets pager.transaction_active; sdb_pager_checkpoint would then
         * return SDB_E_BUSY, which must NOT turn this already-durable commit
         * into an error or flag needs_recovery. The next commit (or that
         * txn's own commit) will checkpoint instead. transaction_active is
         * read under the mutex, so it cannot change between this check and
         * the checkpoint call.
         */
        if (!database->pager.transaction_active) {
            /*
             * The txn is already durable; a checkpoint failure here must NOT
             * turn this successful commit into a reported error. Discard the
             * checkpoint status (sdb_pager_group_checkpoint_if_due flags
             * needs_recovery itself on failure, so the next op is rejected and
             * a reopen recovers from the intact WAL) and keep status == SDB_OK.
             */
            (void)sdb_pager_group_checkpoint_if_due(&database->pager);
        }
    } else {
        database->pager.needs_recovery = true;
    }
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_database_verify(
    sdb_database *database, sdb_verify_result *result_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    status = sdb_database_verify_unlocked(database, result_out);
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_database_info(
    sdb_database *database, sdb_info_result *result_out
)
{
    const sdb_superblock_v1 *superblock;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (result_out == NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_INVALID_ARGUMENT;
    }
    /*
     * O(1): the pager keeps the authoritative superblock in memory, so a
     * config/health probe needs no tree walk and no disk read. Copy under the
     * handle lock so it cannot tear against an in-flight commit that rotates
     * the superblock.
     */
    superblock = &database->pager.superblock;
    (void)memset(result_out, 0, sizeof(*result_out));
    result_out->struct_size = (uint32_t)sizeof(*result_out);
    result_out->page_size = superblock->page_size;
    result_out->encrypted = (superblock->flags & SDB_FLAG_ENCRYPTED) != 0U;
    result_out->kdf_iterations = superblock->kdf_iterations;
    result_out->generation = superblock->generation;
    result_out->checkpoint_lsn = superblock->checkpoint_lsn;
    result_out->page_count = superblock->next_page_id;
    sdb_mutex_unlock(&database->mutex);
    return SDB_OK;
}

sdb_status sdb_database_backup(
    sdb_database *database,
    const char *destination_path,
    bool replace_existing,
    sdb_backup_result *result_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_snapshot != NULL
        || database->commit_in_flight != 0U) {
        /*
         * A live snapshot must exclude backup for parity with compact/migrate
         * (the writer-blocking contract), and an auto-commit driving durability
         * off the engine mutex would have backup checkpoint/clear the WAL under
         * the in-flight leader.
         */
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    status = sdb_database_backup_unlocked(
        database, destination_path, replace_existing, result_out
    );
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_database_compact(
    sdb_database *database,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    status = sdb_database_compact_unlocked(
        database, target_options, result_out
    );
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_database_migrate(
    sdb_database *database,
    uint16_t target_format_version,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    status = target_format_version == SDB_FORMAT_VERSION_V1
        ? sdb_database_compact_unlocked(
            database, target_options, result_out
        )
        : SDB_E_UNSUPPORTED_VERSION;
    sdb_mutex_unlock(&database->mutex);
    return status;
}

/*
 * Byte-copy a file to a fresh destination (which must not yet exist). Used to
 * clone a V1 source before migration so the migration opens the CLONE, never
 * the original: opening a database can heal a superblock mirror or seal a
 * legacy header, and since encode now always stamps V2, opening the V1 source
 * directly would rewrite its version to V2 while leaving V1-layout keys — a
 * corrupt file. Cloning keeps the caller's source byte-for-byte untouched.
 */
static sdb_status sdb_migrate_clone_file(
    const char *source, const char *destination
)
{
    sdb_file input;
    sdb_file output;
    uint8_t *buffer = NULL;
    uint64_t size = 0U;
    uint64_t offset = 0U;
    const size_t chunk = 65536U;
    sdb_status status;
    sdb_status close_status;
    status = sdb_file_open_existing(source, false, &input);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_file_size(&input, &size);
    if (status == SDB_OK) {
        status = sdb_file_create_new(destination, &output);
    }
    if (status != SDB_OK) {
        (void)sdb_file_close(&input);
        return status;
    }
    buffer = (uint8_t *)malloc(chunk);
    if (buffer == NULL) {
        status = SDB_E_OUT_OF_MEMORY;
    }
    while (status == SDB_OK && offset < size) {
        const uint64_t remaining = size - offset;
        const size_t take = remaining < (uint64_t)chunk
            ? (size_t)remaining : chunk;
        status = sdb_file_read_full(&input, offset, buffer, take);
        if (status == SDB_OK) {
            status = sdb_file_write_full(&output, offset, buffer, take);
        }
        offset += (uint64_t)take;
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&output);
    }
    free(buffer);
    close_status = sdb_file_close(&output);
    if (status == SDB_OK) {
        status = close_status;
    }
    close_status = sdb_file_close(&input);
    if (status == SDB_OK) {
        status = close_status;
    }
    return status;
}

/*
 * Rewrite one V1 object/chunk composite key into the V2 layout: drop the
 * 2-byte key_len that V1 stored at offset 4, ahead of the namespace. The user
 * key length is recomputed from the total size so the trailing bytes (a chunk
 * key's [generation u64][chunk_index u32] suffix) are preserved verbatim.
 *   V1: [prefix][kind][ns_len u16][key_len u16][ns][user_key][suffix]
 *   V2: [prefix][kind][ns_len u16][ns][user_key][suffix]
 */
static sdb_status sdb_migrate_transform_object_key(
    const uint8_t *v1,
    size_t v1_size,
    uint8_t *v2_out,
    size_t v2_capacity,
    size_t *v2_size_out
)
{
    size_t namespace_size;
    size_t key_size;
    size_t suffix_size;
    size_t v2_size;
    if (v1_size < 6U) {
        return SDB_E_CORRUPT;
    }
    namespace_size = (size_t)sdb_read_u16_le(v1 + 2U);
    key_size = (size_t)sdb_read_u16_le(v1 + 4U);
    if (namespace_size > v1_size - 6U
        || key_size > v1_size - 6U - namespace_size) {
        return SDB_E_CORRUPT;
    }
    suffix_size = v1_size - 6U - namespace_size - key_size;
    v2_size = v1_size - 2U;
    *v2_size_out = v2_size;
    if (v2_size > v2_capacity) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    v2_out[0] = v1[0];
    v2_out[1] = v1[1];
    sdb_write_u16_le(v2_out + 2U, (uint16_t)namespace_size);
    (void)memcpy(v2_out + 4U, v1 + 6U, namespace_size);
    (void)memcpy(v2_out + 4U + namespace_size, v1 + 6U + namespace_size, key_size);
    (void)memcpy(
        v2_out + 4U + namespace_size + key_size,
        v1 + 6U + namespace_size + key_size,
        suffix_size
    );
    return SDB_OK;
}

sdb_status sdb_database_migrate_file(
    const char *source_path,
    const char *destination_path,
    const sdb_database_options *source_options,
    const sdb_database_options *target_options,
    sdb_compact_result *result_out
)
{
    sdb_superblock_read_result source_super;
    sdb_database *source = NULL;
    sdb_database *target = NULL;
    sdb_btree_cursor cursor;
    sdb_btree_batch batch;
    bool batch_active = false;
    uint8_t *key = NULL;
    uint8_t *value = NULL;
    uint8_t *v2_key = NULL;
    size_t capacity;
    size_t v2_capacity;
    size_t staged = 0U;
    const size_t flush_every = 4096U;
    uint64_t before_size = 0U;
    uint64_t after_size = 0U;
    char *clone_path = NULL;
    char *clone_wal_path = NULL;
    char *clone_lock_path = NULL;
    char *source_wal_path = NULL;
    sdb_verify_result target_verify = {0};
    sdb_status status;
    if (source_path == NULL || destination_path == NULL
        || result_out == NULL
        || sdb_database_options_validate(source_options) != SDB_OK
        || sdb_database_options_validate(target_options) != SDB_OK) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(result_out, 0, sizeof(*result_out));
    /*
     * Only a V1 file needs (and is transformed by) this path. Reading the
     * superblock first lets us reject a V2 (or unknown) source before opening
     * or creating anything.
     */
    status = sdb_superblock_store_read(source_path, &source_super);
    if (status != SDB_OK) {
        return status;
    }
    if (source_super.format_version != SDB_FORMAT_VERSION_V1) {
        return SDB_E_UNSUPPORTED_VERSION;
    }
    /*
     * Clone the source (and its WAL, if any) next to the destination and open
     * the CLONE — opening a database may rewrite its superblock, which under
     * the always-V2 encoder would corrupt the real V1 source.
     */
    status = sdb_backup_temp_path(destination_path, &clone_path);
    if (status == SDB_OK) {
        status = sdb_engine_append_suffix(clone_path, ".wal", &clone_wal_path);
    }
    if (status == SDB_OK) {
        status = sdb_engine_append_suffix(
            clone_path, ".lock", &clone_lock_path
        );
    }
    if (status == SDB_OK) {
        status = sdb_engine_append_suffix(
            source_path, ".wal", &source_wal_path
        );
    }
    if (status == SDB_OK) {
        status = sdb_migrate_clone_file(source_path, clone_path);
    }
    if (status == SDB_OK) {
        const sdb_status wal_status = sdb_migrate_clone_file(
            source_wal_path, clone_wal_path
        );
        if (wal_status != SDB_OK && wal_status != SDB_E_NOT_FOUND) {
            status = wal_status;
        }
    }
    if (status != SDB_OK) {
        (void)sdb_file_remove(clone_path, true);
        (void)sdb_file_remove(clone_wal_path, true);
        (void)sdb_file_remove(clone_lock_path, true);
        free(clone_path);
        free(clone_wal_path);
        free(clone_lock_path);
        free(source_wal_path);
        return status;
    }
    status = sdb_database_open_internal(
        clone_path, source_options, true, &source
    );
    if (status != SDB_OK) {
        (void)sdb_file_remove(clone_path, true);
        (void)sdb_file_remove(clone_wal_path, true);
        (void)sdb_file_remove(clone_lock_path, true);
        free(clone_path);
        free(clone_wal_path);
        free(clone_lock_path);
        free(source_wal_path);
        return status;
    }
    (void)sdb_file_size(&source->pager.file, &before_size);
    status = sdb_database_create(destination_path, target_options, &target);
    if (status != SDB_OK) {
        (void)sdb_database_close(source);
        (void)sdb_file_remove(clone_path, true);
        (void)sdb_file_remove(clone_wal_path, true);
        (void)sdb_file_remove(clone_lock_path, true);
        free(clone_path);
        free(clone_wal_path);
        free(clone_lock_path);
        free(source_wal_path);
        return status;
    }
    capacity = sdb_pager_payload_capacity(&source->pager);
    v2_capacity = capacity;
    key = (uint8_t *)malloc(capacity);
    value = (uint8_t *)malloc(capacity);
    v2_key = (uint8_t *)malloc(v2_capacity);
    if (key == NULL || value == NULL || v2_key == NULL) {
        status = SDB_E_OUT_OF_MEMORY;
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    (void)memset(&batch, 0, sizeof(batch));
    if (status == SDB_OK) {
        status = sdb_btree_cursor_first(&source->tree, &cursor);
    }
    if (status == SDB_OK) {
        status = sdb_btree_batch_begin(&target->tree, &batch);
        batch_active = status == SDB_OK;
    }
    while (status == SDB_OK && cursor.valid) {
        size_t key_size = 0U;
        size_t value_size = 0U;
        const uint8_t *put_key;
        size_t put_key_size;
        status = sdb_btree_cursor_read(
            &cursor, key, capacity, &key_size,
            value, capacity, &value_size
        );
        if (status != SDB_OK) {
            break;
        }
        if (key_size != 0U
            && (key[0] == SDB_OBJECT_KEY_PREFIX
                || key[0] == SDB_CHUNK_KEY_PREFIX)) {
            size_t v2_size = 0U;
            status = sdb_migrate_transform_object_key(
                key, key_size, v2_key, v2_capacity, &v2_size
            );
            if (status != SDB_OK) {
                break;
            }
            put_key = v2_key;
            put_key_size = v2_size;
        } else {
            /*
             * sequence counter (0x10) and index rows (0x50-0x53) keep their
             * layout across the format change, so copy them verbatim.
             */
            put_key = key;
            put_key_size = key_size;
        }
        status = sdb_btree_batch_put(
            &batch, put_key, put_key_size, value, value_size
        );
        if (status == SDB_OK && ++staged >= flush_every) {
            status = sdb_btree_batch_commit(&batch);
            batch_active = false;
            staged = 0U;
            if (status == SDB_OK) {
                status = sdb_btree_batch_begin(&target->tree, &batch);
                batch_active = status == SDB_OK;
            }
        }
        if (status == SDB_OK) {
            status = sdb_btree_cursor_next(&cursor);
        }
    }
    sdb_btree_cursor_close(&cursor);
    if (status == SDB_OK && batch_active) {
        batch_active = false;
        status = sdb_btree_batch_commit(&batch);
    }
    if (batch_active) {
        sdb_btree_batch_abort(&batch);
    }
    if (status == SDB_OK) {
        status = sdb_pager_checkpoint(&target->pager);
    }
    if (status == SDB_OK) {
        status = sdb_database_verify_unlocked(target, &target_verify);
    }
    if (status == SDB_OK) {
        status = sdb_file_size(&target->pager.file, &after_size);
    }
    free(key);
    free(value);
    free(v2_key);
    {
        const sdb_status target_close = sdb_database_close(target);
        if (status == SDB_OK) {
            status = target_close;
        }
    }
    (void)sdb_database_close(source);
    /*
     * The clone is throwaway scratch this call created; remove it, its WAL, and
     * its .lock (sdb_process_lock_release only unlocks the fd) regardless of
     * outcome.
     */
    (void)sdb_file_remove(clone_path, true);
    (void)sdb_file_remove(clone_wal_path, true);
    (void)sdb_file_remove(clone_lock_path, true);
    free(clone_path);
    free(clone_wal_path);
    free(clone_lock_path);
    free(source_wal_path);
    if (status != SDB_OK) {
        /*
         * The destination is a freshly created file this call owns, so a
         * failed migration removes it rather than leaving a partial image.
         */
        (void)sdb_file_remove(destination_path, true);
        {
            char *dest_wal = NULL;
            if (sdb_engine_append_suffix(
                    destination_path, ".wal", &dest_wal
                ) == SDB_OK) {
                (void)sdb_file_remove(dest_wal, true);
                free(dest_wal);
            }
        }
        return status;
    }
    result_out->byte_count_before = before_size;
    result_out->byte_count_after = after_size;
    result_out->raw_entries_after = target_verify.raw_entry_count;
    return SDB_OK;
}

static sdb_status sdb_transaction_lock(
    sdb_transaction *transaction, sdb_database **database_out
)
{
    sdb_database *database;
    if (transaction == NULL || !transaction->active
        || transaction->database == NULL || database_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    database = transaction->database;
    sdb_mutex_lock(&database->mutex);
    if (!database->open || !transaction->active
        || transaction->database != database
        || database->active_transaction != transaction
        || database->callback_depth != 0U) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    *database_out = database;
    return SDB_OK;
}

static sdb_status sdb_transaction_reserve(
    sdb_transaction *transaction, size_t logical_bytes
)
{
    if (transaction->operation_count >= SDB_TRANSACTION_MAX_OPERATIONS
        || logical_bytes > SDB_TRANSACTION_MAX_LOGICAL_BYTES
        || transaction->logical_byte_count
            > SDB_TRANSACTION_MAX_LOGICAL_BYTES - logical_bytes) {
        return SDB_E_OVERFLOW;
    }
    ++transaction->operation_count;
    transaction->logical_byte_count += logical_bytes;
    return SDB_OK;
}

sdb_status sdb_transaction_begin(
    sdb_database *database, sdb_transaction **transaction_out
)
{
    sdb_transaction *transaction;
    sdb_status status;
    if (transaction_out != NULL) {
        *transaction_out = NULL;
    }
    if (database == NULL || transaction_out == NULL || !database->open) {
        return SDB_E_INVALID_ARGUMENT;
    }
    transaction = (sdb_transaction *)calloc(1U, sizeof(*transaction));
    if (transaction == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    sdb_mutex_lock(&database->mutex);
    if (!database->open || database->callback_depth != 0U
        || database->active_transaction != NULL
        || database->active_snapshot != NULL
        || database->commit_in_flight != 0U) {
        sdb_mutex_unlock(&database->mutex);
        free(transaction);
        return SDB_E_BUSY;
    }
    status = sdb_engine_mutation_begin(database, &transaction->mutation);
    if (status == SDB_OK) {
        transaction->database = database;
        transaction->active = true;
        database->active_transaction = transaction;
        *transaction_out = transaction;
    }
    sdb_mutex_unlock(&database->mutex);
    if (status != SDB_OK) {
        free(transaction);
    }
    return status;
}

sdb_status sdb_transaction_commit(sdb_transaction *transaction)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_engine_mutation_commit(&transaction->mutation);
    database->active_transaction = NULL;
    transaction->active = false;
    transaction->database = NULL;
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_transaction_rollback(sdb_transaction *transaction)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status != SDB_OK) {
        return status;
    }
    sdb_engine_mutation_abort(&transaction->mutation);
    database->active_transaction = NULL;
    transaction->active = false;
    transaction->database = NULL;
    sdb_mutex_unlock(&database->mutex);
    return SDB_OK;
}

sdb_status sdb_transaction_close(sdb_transaction *transaction)
{
    if (transaction == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }

    if (transaction->active) {
        return SDB_E_BUSY;
    }
    free(transaction);
    return SDB_OK;
}

sdb_status sdb_transaction_kv_put(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        if (namespace_size > SIZE_MAX - key_size
            || namespace_size + key_size > SIZE_MAX - value_size) {
            status = SDB_E_OVERFLOW;
        } else {
            status = sdb_transaction_reserve(
                transaction, namespace_size + key_size + value_size
            );
        }
    }
    if (status == SDB_OK) {
        status = sdb_object_put(
            database, &transaction->mutation, (uint16_t)SDB_OBJECT_KV,
            namespace_name, namespace_size, key, key_size, value, value_size
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_kv_get(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        status = sdb_object_get(
            database, &transaction->mutation, (uint16_t)SDB_OBJECT_KV,
            namespace_name, namespace_size, key, key_size,
            value_out, value_capacity, value_size_out
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_kv_delete(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        status = namespace_size > SIZE_MAX - key_size
            ? SDB_E_OVERFLOW
            : sdb_transaction_reserve(
                transaction, namespace_size + key_size
            );
    }
    if (status == SDB_OK) {
        status = sdb_object_delete(
            database, &transaction->mutation, (uint16_t)SDB_OBJECT_KV,
            namespace_name, namespace_size, key, key_size
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_blob_put(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        if (namespace_size > SIZE_MAX - key_size
            || namespace_size + key_size > SIZE_MAX - value_size) {
            status = SDB_E_OVERFLOW;
        } else {
            status = sdb_transaction_reserve(
                transaction, namespace_size + key_size + value_size
            );
        }
    }
    if (status == SDB_OK) {
        status = sdb_object_put(
            database, &transaction->mutation, (uint16_t)SDB_OBJECT_BLOB,
            namespace_name, namespace_size, key, key_size, value, value_size
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_blob_get(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        status = sdb_object_get(
            database, &transaction->mutation, (uint16_t)SDB_OBJECT_BLOB,
            namespace_name, namespace_size, key, key_size,
            value_out, value_capacity, value_size_out
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_blob_delete(
    sdb_transaction *transaction,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        status = namespace_size > SIZE_MAX - key_size
            ? SDB_E_OVERFLOW
            : sdb_transaction_reserve(
                transaction, namespace_size + key_size
            );
    }
    if (status == SDB_OK) {
        status = sdb_object_delete(
            database, &transaction->mutation, (uint16_t)SDB_OBJECT_BLOB,
            namespace_name, namespace_size, key, key_size
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_index_create(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        status = collection_size > SIZE_MAX - index_name_size
            ? SDB_E_OVERFLOW
            : sdb_transaction_reserve(
                transaction, collection_size + index_name_size
            );
    }
    if (status == SDB_OK) {
        status = sdb_index_create_in_mutation(
            database, &transaction->mutation, collection, collection_size,
            index_name, index_name_size, unique
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_document_put(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size,
    const sdb_index_term *terms,
    size_t term_count
)
{
    sdb_database *database;
    size_t logical_bytes;
    size_t index;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        if (collection_size > SIZE_MAX - document_id_size
            || collection_size + document_id_size > SIZE_MAX - document_size) {
            status = SDB_E_OVERFLOW;
        } else {
            logical_bytes = collection_size + document_id_size + document_size;
            for (index = 0U; index < term_count; ++index) {
                if (terms == NULL
                    || terms[index].index_name_size
                        > SIZE_MAX - terms[index].value_size
                    || logical_bytes > SIZE_MAX
                        - terms[index].index_name_size
                        - terms[index].value_size) {
                    status = terms == NULL
                        ? SDB_E_INVALID_ARGUMENT : SDB_E_OVERFLOW;
                    break;
                }
                logical_bytes += terms[index].index_name_size
                    + terms[index].value_size;
            }
            if (status == SDB_OK) {
                status = sdb_transaction_reserve(
                    transaction, logical_bytes
                );
            }
        }
    }
    if (status == SDB_OK) {
        status = sdb_document_put_in_mutation(
            database, &transaction->mutation, collection, collection_size,
            document_id, document_id_size, document, document_size,
            terms, term_count
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_document_get(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    uint8_t *document_out,
    size_t document_capacity,
    size_t *document_size_out
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        status = sdb_object_get(
            database, &transaction->mutation,
            (uint16_t)SDB_OBJECT_DOCUMENT,
            collection, collection_size, document_id, document_id_size,
            document_out, document_capacity, document_size_out
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_transaction_document_delete(
    sdb_transaction *transaction,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    sdb_database *database;
    const sdb_status lock_status =
        sdb_transaction_lock(transaction, &database);
    sdb_status status = lock_status;
    if (status == SDB_OK) {
        status = collection_size > SIZE_MAX - document_id_size
            ? SDB_E_OVERFLOW
            : sdb_transaction_reserve(
                transaction, collection_size + document_id_size
            );
    }
    if (status == SDB_OK) {
        status = sdb_document_delete_in_mutation(
            database, &transaction->mutation,
            collection, collection_size, document_id, document_id_size
        );
    }
    if (lock_status == SDB_OK) {
        sdb_mutex_unlock(&database->mutex);
    }
    return status;
}

sdb_status sdb_kv_put(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    sdb_pager_set_defer_commit(&database->pager, true);
    status = sdb_kv_put_unlocked(
        database, namespace_name, namespace_size, key, key_size,
        value, value_size
    );
    return sdb_engine_group_commit_finish(database, status);
}

sdb_status sdb_kv_get(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    status = sdb_kv_get_unlocked(
        database,
        namespace_name,
        namespace_size,
        key,
        key_size,
        value_out,
        value_capacity,
        value_size_out
    );
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_kv_delete(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    sdb_pager_set_defer_commit(&database->pager, true);
    status = sdb_kv_delete_unlocked(
        database, namespace_name, namespace_size, key, key_size
    );
    return sdb_engine_group_commit_finish(database, status);
}

sdb_status sdb_blob_put(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    sdb_pager_set_defer_commit(&database->pager, true);
    status = sdb_blob_put_unlocked(
        database, namespace_name, namespace_size, key, key_size,
        value, value_size
    );
    return sdb_engine_group_commit_finish(database, status);
}

sdb_status sdb_blob_get(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    status = sdb_blob_get_unlocked(
        database,
        namespace_name,
        namespace_size,
        key,
        key_size,
        value_out,
        value_capacity,
        value_size_out
    );
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_blob_delete(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    sdb_pager_set_defer_commit(&database->pager, true);
    status = sdb_blob_delete_unlocked(
        database, namespace_name, namespace_size, key, key_size
    );
    return sdb_engine_group_commit_finish(database, status);
}

sdb_status sdb_index_create(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    bool unique
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    sdb_pager_set_defer_commit(&database->pager, true);
    status = sdb_index_create_unlocked(
        database, collection, collection_size,
        index_name, index_name_size, unique
    );
    return sdb_engine_group_commit_finish(database, status);
}

sdb_status sdb_document_put(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size,
    const sdb_index_term *terms,
    size_t term_count
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    sdb_pager_set_defer_commit(&database->pager, true);
    status = sdb_document_put_unlocked(
        database, collection, collection_size,
        document_id, document_id_size, document, document_size,
        terms, term_count
    );
    return sdb_engine_group_commit_finish(database, status);
}

sdb_status sdb_document_get(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size,
    uint8_t *document_out,
    size_t document_capacity,
    size_t *document_size_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    status = sdb_document_get_unlocked(
        database,
        collection,
        collection_size,
        document_id,
        document_id_size,
        document_out,
        document_capacity,
        document_size_out
    );
    sdb_mutex_unlock(&database->mutex);
    return status;
}

sdb_status sdb_document_delete(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *document_id,
    size_t document_id_size
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    sdb_pager_set_defer_commit(&database->pager, true);
    status = sdb_document_delete_unlocked(
        database, collection, collection_size,
        document_id, document_id_size
    );
    return sdb_engine_group_commit_finish(database, status);
}

sdb_status sdb_index_visit(
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
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    status = sdb_index_visit_unlocked(
        database,
        collection,
        collection_size,
        index_name,
        index_name_size,
        value,
        value_size,
        visitor,
        context,
        match_count_out
    );
    sdb_mutex_unlock(&database->mutex);
    return status;
}

/*
 * ============================================================================
 * Read snapshots, forward cursors, prefix scan (read-only iteration).
 * ============================================================================
 */

void sdb_cursor_options_init(sdb_cursor_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
}

void sdb_scan_options_init(sdb_scan_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->struct_size = (uint32_t)sizeof(*options);
}

struct sdb_snapshot {
    sdb_database *database;
    uint64_t version;
    /*
     * Count of cursors opened on this snapshot and not yet closed (maintained
     * under database->mutex). sdb_snapshot_close refuses with SDB_E_BUSY while
     * any remain, so a cursor can never outlive the snapshot slot that excludes
     * writers — without this a cursor would keep reading a tree a writer is
     * free to mutate (or that database_close has torn down).
     */
    size_t open_cursors;
};

struct sdb_cursor {
    sdb_database *database;
    sdb_snapshot *snapshot;
    uint16_t kind;
    /*
     * [SDB_OBJECT_KEY_PREFIX][kind][ns_len u16][namespace]: the shared prefix
     * of every composite key in this keyspace+namespace. namespace bytes begin
     * at prefix + 4.
     */
    uint8_t *prefix;
    size_t prefix_size;
    size_t namespace_size;
    uint8_t *lower_bound;
    size_t lower_bound_size;
    uint8_t *upper_bound;
    size_t upper_bound_size;
    uint64_t limit;
    uint64_t produced;
    sdb_btree_cursor btcur;
    bool reverse;
    bool started;
    bool valid;
    sdb_status status;
    uint8_t *key_buffer;
    size_t key_capacity;
    const uint8_t *user_key;
    size_t user_key_size;
    uint8_t *value_buffer;
    size_t value_capacity;
    size_t value_size;
};

sdb_status sdb_snapshot_open(
    sdb_database *database, sdb_snapshot **snapshot_out
)
{
    sdb_snapshot *snapshot;
    if (snapshot_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *snapshot_out = NULL;
    SDB_ENGINE_LOCK_OR_RETURN(database);
    if (database->active_transaction != NULL
        || database->active_snapshot != NULL
        || database->commit_in_flight != 0U
        || database->callback_depth != 0U) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    snapshot = (sdb_snapshot *)calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_OUT_OF_MEMORY;
    }
    snapshot->database = database;
    snapshot->version = database->pager.superblock.generation;
    database->active_snapshot = snapshot;
    sdb_mutex_unlock(&database->mutex);
    *snapshot_out = snapshot;
    return SDB_OK;
}

sdb_status sdb_snapshot_close(sdb_snapshot *snapshot)
{
    sdb_database *database;
    if (snapshot == NULL) {
        return SDB_OK;
    }
    database = snapshot->database;
    sdb_mutex_lock(&database->mutex);
    if (snapshot->open_cursors != 0U) {
        /*
         * A cursor still borrows this snapshot's writer-excluding slot. Closing
         * now would free the slot and let a writer mutate the tree under the
         * live cursor (wrong results / SDB_E_CORRUPT), or let database_close
         * tear the handle down beneath it (use-after-free). Refuse until every
         * cursor is closed.
         */
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    if (database->active_snapshot == snapshot) {
        database->active_snapshot = NULL;
    }
    sdb_mutex_unlock(&database->mutex);
    free(snapshot);
    return SDB_OK;
}

uint64_t sdb_snapshot_version(const sdb_snapshot *snapshot)
{
    return snapshot == NULL ? 0U : snapshot->version;
}

static int sdb_user_key_compare(
    const uint8_t *a, size_t a_size, const uint8_t *b, size_t b_size
)
{
    size_t min = a_size < b_size ? a_size : b_size;
    int order = min == 0U ? 0 : memcmp(a, b, min);
    if (order != 0) {
        return order;
    }
    if (a_size == b_size) {
        return 0;
    }
    return a_size < b_size ? -1 : 1;
}

/*
 * Read the entry the b-tree cursor sits on, and if it belongs to this cursor's
 * keyspace+namespace and range, decode its user key and reassemble its value.
 * Leaves cursor->valid true with user_key/value populated, or false at the end
 * of the range. A structural/IO fault is returned (and leaves valid false).
 */
static sdb_status sdb_cursor_load_current(sdb_cursor *cursor)
{
    size_t composite_size = 0U;
    size_t discard = 0U;
    uint16_t kind;
    const uint8_t *namespace_name;
    const uint8_t *object_key;
    size_t namespace_size;
    size_t object_key_size;
    sdb_status status;

    cursor->valid = false;
    cursor->user_key = NULL;
    cursor->user_key_size = 0U;
    cursor->value_size = 0U;
    if (!cursor->btcur.valid) {
        return SDB_OK;
    }
    if (cursor->limit != 0U && cursor->produced >= cursor->limit) {
        return SDB_OK;
    }
    /*
     * The metadata value is not needed here (the value is reassembled below via
     * sdb_object_get); read it into value_buffer only to satisfy the b-tree
     * cursor API, then overwrite it. An object metadata row is a fixed 64 bytes,
     * well under the 256-byte initial value_capacity, so this read never trips
     * SDB_E_BUFFER_TOO_SMALL.
     */
    status = sdb_btree_cursor_read(
        &cursor->btcur,
        cursor->key_buffer,
        cursor->key_capacity,
        &composite_size,
        cursor->value_buffer,
        cursor->value_capacity,
        &discard
    );
    if (status != SDB_OK) {
        return status;
    }
    if (composite_size < cursor->prefix_size
        || memcmp(cursor->key_buffer, cursor->prefix, cursor->prefix_size)
            != 0) {
        /* Walked past this keyspace+namespace: end of range. */
        return SDB_OK;
    }
    status = sdb_verify_pair_key(
        cursor->key_buffer,
        composite_size,
        0U,
        &kind,
        &namespace_name,
        &namespace_size,
        &object_key,
        &object_key_size
    );
    if (status != SDB_OK) {
        return status;
    }
    if (cursor->reverse) {
        /* Reverse walk stops once it steps below the (inclusive) lower bound. */
        if (cursor->lower_bound != NULL
            && sdb_user_key_compare(
                object_key, object_key_size,
                cursor->lower_bound, cursor->lower_bound_size
            ) < 0) {
            return SDB_OK;
        }
    } else if (cursor->upper_bound != NULL
        && sdb_user_key_compare(
            object_key, object_key_size,
            cursor->upper_bound, cursor->upper_bound_size
        ) >= 0) {
        return SDB_OK;
    }
    for (;;) {
        status = sdb_object_get(
            cursor->database,
            NULL,
            cursor->kind,
            cursor->prefix + 4U,
            cursor->namespace_size,
            object_key,
            object_key_size,
            cursor->value_buffer,
            cursor->value_capacity,
            &cursor->value_size
        );
        if (status == SDB_E_BUFFER_TOO_SMALL
            && cursor->value_size > cursor->value_capacity) {
            uint8_t *grown = (uint8_t *)realloc(
                cursor->value_buffer, cursor->value_size
            );
            if (grown == NULL) {
                return SDB_E_OUT_OF_MEMORY;
            }
            cursor->value_buffer = grown;
            cursor->value_capacity = cursor->value_size;
            continue;
        }
        break;
    }
    if (status != SDB_OK) {
        return status;
    }
    cursor->user_key = object_key;
    cursor->user_key_size = object_key_size;
    cursor->valid = true;
    cursor->produced += 1U;
    return SDB_OK;
}

sdb_status sdb_cursor_open(
    sdb_snapshot *snapshot,
    sdb_keyspace_kind keyspace_kind,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const sdb_cursor_options *options,
    sdb_cursor **cursor_out
)
{
    sdb_cursor *cursor;
    uint16_t kind;
    if (cursor_out != NULL) {
        *cursor_out = NULL;
    }
    if (snapshot == NULL || cursor_out == NULL
        || !sdb_engine_bytes_valid(namespace_name, namespace_size)
        || namespace_size == 0U
        || namespace_size > SDB_ENGINE_MAX_NAME_SIZE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (keyspace_kind == SDB_KEYSPACE_KV) {
        kind = (uint16_t)SDB_OBJECT_KV;
    } else if (keyspace_kind == SDB_KEYSPACE_BLOB) {
        kind = (uint16_t)SDB_OBJECT_BLOB;
    } else {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (options != NULL) {
        if (options->struct_size < (uint32_t)sizeof(*options)
            || (options->lower_bound == NULL && options->lower_bound_size != 0U)
            || (options->upper_bound == NULL && options->upper_bound_size != 0U)
            || options->lower_bound_size > SDB_ENGINE_MAX_NAME_SIZE
            || options->upper_bound_size > SDB_ENGINE_MAX_NAME_SIZE) {
            return SDB_E_INVALID_ARGUMENT;
        }
    }
    cursor = (sdb_cursor *)calloc(1U, sizeof(*cursor));
    if (cursor == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    cursor->database = snapshot->database;
    cursor->snapshot = snapshot;
    cursor->kind = kind;
    cursor->namespace_size = namespace_size;
    cursor->status = SDB_OK;
    cursor->prefix_size = 4U + namespace_size;
    cursor->prefix = (uint8_t *)malloc(cursor->prefix_size);
    /*
     * Composite keys never exceed the 4-byte header + namespace + a user key
     * bounded by SDB_ENGINE_MAX_NAME_SIZE, so this capacity never overflows.
     */
    cursor->key_capacity = cursor->prefix_size + SDB_ENGINE_MAX_NAME_SIZE + 16U;
    cursor->key_buffer = (uint8_t *)malloc(cursor->key_capacity);
    cursor->value_capacity = 256U;
    cursor->value_buffer = (uint8_t *)malloc(cursor->value_capacity);
    if (cursor->prefix == NULL || cursor->key_buffer == NULL
        || cursor->value_buffer == NULL) {
        free(cursor->prefix);
        free(cursor->key_buffer);
        free(cursor->value_buffer);
        free(cursor);
        return SDB_E_OUT_OF_MEMORY;
    }
    cursor->prefix[0] = SDB_OBJECT_KEY_PREFIX;
    cursor->prefix[1] = (uint8_t)kind;
    sdb_write_u16_le(cursor->prefix + 2U, (uint16_t)namespace_size);
    (void)memcpy(cursor->prefix + 4U, namespace_name, namespace_size);
    if (options != NULL && options->lower_bound_size != 0U) {
        cursor->lower_bound = (uint8_t *)malloc(options->lower_bound_size);
        if (cursor->lower_bound == NULL) {
            free(cursor->prefix);
            free(cursor->key_buffer);
            free(cursor->value_buffer);
            free(cursor);
            return SDB_E_OUT_OF_MEMORY;
        }
        (void)memcpy(
            cursor->lower_bound, options->lower_bound, options->lower_bound_size
        );
        cursor->lower_bound_size = options->lower_bound_size;
    }
    if (options != NULL && options->upper_bound_size != 0U) {
        cursor->upper_bound = (uint8_t *)malloc(options->upper_bound_size);
        if (cursor->upper_bound == NULL) {
            free(cursor->lower_bound);
            free(cursor->prefix);
            free(cursor->key_buffer);
            free(cursor->value_buffer);
            free(cursor);
            return SDB_E_OUT_OF_MEMORY;
        }
        (void)memcpy(
            cursor->upper_bound, options->upper_bound, options->upper_bound_size
        );
        cursor->upper_bound_size = options->upper_bound_size;
    }
    if (options != NULL) {
        cursor->limit = options->limit;
        cursor->reverse = options->reverse;
    }
    sdb_mutex_lock(&cursor->database->mutex);
    snapshot->open_cursors += 1U;
    sdb_mutex_unlock(&cursor->database->mutex);
    *cursor_out = cursor;
    return SDB_OK;
}

/*
 * Seek the underlying b-tree cursor to the first composite key >= the given
 * user key (clamped up to the lower bound), holding the engine mutex.
 */
static sdb_status sdb_cursor_seek_locked(
    sdb_cursor *cursor, const uint8_t *user_key, size_t user_key_size
)
{
    uint8_t stack_key[512];
    uint8_t *seek_key = stack_key;
    size_t seek_size = cursor->prefix_size + user_key_size;
    sdb_status status;
    if (seek_size > sizeof(stack_key)) {
        seek_key = (uint8_t *)malloc(seek_size);
        if (seek_key == NULL) {
            return SDB_E_OUT_OF_MEMORY;
        }
    }
    (void)memcpy(seek_key, cursor->prefix, cursor->prefix_size);
    if (user_key_size != 0U) {
        (void)memcpy(seek_key + cursor->prefix_size, user_key, user_key_size);
    }
    /*
     * sdb_btree_cursor_seek memset-clears the cursor without freeing an
     * existing pinned leaf, so a re-seek on an already-positioned cursor would
     * leak the previous leaf node. Release it first.
     */
    if (cursor->started) {
        sdb_btree_cursor_close(&cursor->btcur);
    }
    status = sdb_btree_cursor_seek(
        &cursor->database->tree, seek_key, seek_size, &cursor->btcur
    );
    if (seek_key != stack_key) {
        free(seek_key);
    }
    return status;
}

/*
 * Position the b-tree cursor on the greatest in-range entry (reverse start):
 * seek just past the range's upper edge, then step back once. Building the seek
 * through sdb_btree_cursor_seek records the descent path, so the following
 * _prev (and every _prev after) can reach the predecessor leaf.
 */
static sdb_status sdb_cursor_seek_end_locked(sdb_cursor *cursor)
{
    uint8_t stack_key[512];
    uint8_t *end_key = stack_key;
    size_t end_size;
    sdb_status status;
    if (cursor->started) {
        sdb_btree_cursor_close(&cursor->btcur);
    }
    if (cursor->upper_bound != NULL) {
        /* Half-open [lower, upper): seek prefix+upper, prev -> largest < upper. */
        end_size = cursor->prefix_size + cursor->upper_bound_size;
        if (end_size > sizeof(stack_key)) {
            end_key = (uint8_t *)malloc(end_size);
            if (end_key == NULL) {
                return SDB_E_OUT_OF_MEMORY;
            }
        }
        (void)memcpy(end_key, cursor->prefix, cursor->prefix_size);
        (void)memcpy(
            end_key + cursor->prefix_size,
            cursor->upper_bound, cursor->upper_bound_size
        );
    } else {
        /*
         * No upper bound: seek the successor of the namespace prefix (the
         * smallest key that does NOT begin with it), so prev lands on the
         * greatest key that does. The prefix starts with 0x40, so a byte < 0xFF
         * always exists and a successor always does.
         */
        end_size = cursor->prefix_size;
        if (end_size > sizeof(stack_key)) {
            end_key = (uint8_t *)malloc(end_size);
            if (end_key == NULL) {
                return SDB_E_OUT_OF_MEMORY;
            }
        }
        (void)memcpy(end_key, cursor->prefix, cursor->prefix_size);
        while (end_size > 0U && end_key[end_size - 1U] == 0xFFU) {
            end_size -= 1U;
        }
        if (end_size == 0U) {
            /* Unreachable for a 0x40-led prefix; fall back to the tree end. */
            if (end_key != stack_key) {
                free(end_key);
            }
            return sdb_btree_cursor_last(&cursor->database->tree, &cursor->btcur);
        }
        end_key[end_size - 1U] += 1U;
    }
    status = sdb_btree_cursor_seek_floor(
        &cursor->database->tree, end_key, end_size, &cursor->btcur
    );
    if (end_key != stack_key) {
        free(end_key);
    }
    if (status == SDB_OK) {
        status = sdb_btree_cursor_prev(&cursor->btcur);
    }
    return status;
}

sdb_status sdb_cursor_first(sdb_cursor *cursor)
{
    sdb_status status;
    if (cursor == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    sdb_mutex_lock(&cursor->database->mutex);
    cursor->produced = 0U;
    if (cursor->reverse) {
        status = sdb_cursor_seek_end_locked(cursor);
    } else {
        status = sdb_cursor_seek_locked(
            cursor, cursor->lower_bound, cursor->lower_bound_size
        );
    }
    if (status == SDB_OK) {
        cursor->started = true;
        status = sdb_cursor_load_current(cursor);
    }
    cursor->status = status;
    sdb_mutex_unlock(&cursor->database->mutex);
    return status;
}

sdb_status sdb_cursor_seek(
    sdb_cursor *cursor, const uint8_t *key, size_t key_size
)
{
    sdb_status status;
    const uint8_t *target = key;
    size_t target_size = key_size;
    if (cursor == NULL || (key == NULL && key_size != 0U)
        || key_size > SDB_ENGINE_MAX_NAME_SIZE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    /* Never seek below the configured lower bound. */
    if (cursor->lower_bound != NULL
        && sdb_user_key_compare(
            target, target_size,
            cursor->lower_bound, cursor->lower_bound_size
        ) < 0) {
        target = cursor->lower_bound;
        target_size = cursor->lower_bound_size;
    }
    sdb_mutex_lock(&cursor->database->mutex);
    /*
     * A seek restarts iteration from a new position, so reset the limit counter
     * exactly like sdb_cursor_first — otherwise a seek after the limit was
     * consumed would return a spuriously-invalid cursor.
     */
    cursor->produced = 0U;
    status = sdb_cursor_seek_locked(cursor, target, target_size);
    if (status == SDB_OK) {
        cursor->started = true;
        status = sdb_cursor_load_current(cursor);
    }
    cursor->status = status;
    sdb_mutex_unlock(&cursor->database->mutex);
    return status;
}

sdb_status sdb_cursor_next(sdb_cursor *cursor)
{
    sdb_status status;
    if (cursor == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (!cursor->started) {
        return SDB_E_INVALID_ARGUMENT;
    }
    sdb_mutex_lock(&cursor->database->mutex);
    if (!cursor->btcur.valid) {
        cursor->valid = false;
        sdb_mutex_unlock(&cursor->database->mutex);
        return SDB_OK;
    }
    status = cursor->reverse
        ? sdb_btree_cursor_prev(&cursor->btcur)
        : sdb_btree_cursor_next(&cursor->btcur);
    if (status == SDB_OK) {
        status = sdb_cursor_load_current(cursor);
    } else {
        cursor->valid = false;
    }
    cursor->status = status;
    sdb_mutex_unlock(&cursor->database->mutex);
    return status;
}

bool sdb_cursor_valid(const sdb_cursor *cursor)
{
    return cursor != NULL && cursor->valid;
}

sdb_status sdb_cursor_status(const sdb_cursor *cursor)
{
    return cursor == NULL ? SDB_E_INVALID_ARGUMENT : cursor->status;
}

sdb_status sdb_cursor_key(
    const sdb_cursor *cursor, const uint8_t **key_out, size_t *key_size_out
)
{
    if (cursor == NULL || key_out == NULL || key_size_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (!cursor->valid) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *key_out = cursor->user_key;
    *key_size_out = cursor->user_key_size;
    return SDB_OK;
}

sdb_status sdb_cursor_value(
    const sdb_cursor *cursor, const uint8_t **value_out, size_t *value_size_out
)
{
    if (cursor == NULL || value_out == NULL || value_size_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (!cursor->valid) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *value_out = cursor->value_buffer;
    *value_size_out = cursor->value_size;
    return SDB_OK;
}

sdb_status sdb_cursor_get(
    const sdb_cursor *cursor,
    const uint8_t **key_out,
    size_t *key_size_out,
    const uint8_t **value_out,
    size_t *value_size_out
)
{
    sdb_status status = sdb_cursor_key(cursor, key_out, key_size_out);
    if (status != SDB_OK) {
        return status;
    }
    return sdb_cursor_value(cursor, value_out, value_size_out);
}

sdb_status sdb_cursor_read(
    const sdb_cursor *cursor,
    uint8_t *key_out,
    size_t key_capacity,
    size_t *key_size_out,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    if (cursor == NULL || key_size_out == NULL || value_size_out == NULL
        || (key_out == NULL && key_capacity != 0U)
        || (value_out == NULL && value_capacity != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (!cursor->valid) {
        return SDB_E_INVALID_ARGUMENT;
    }
    /*
     * Report required sizes before the capacity check so a caller can probe
     * with capacity 0, mirroring sdb_kv_get.
     */
    *key_size_out = cursor->user_key_size;
    *value_size_out = cursor->value_size;
    if (key_capacity < cursor->user_key_size
        || value_capacity < cursor->value_size) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    if (cursor->user_key_size != 0U) {
        (void)memcpy(key_out, cursor->user_key, cursor->user_key_size);
    }
    if (cursor->value_size != 0U) {
        (void)memcpy(value_out, cursor->value_buffer, cursor->value_size);
    }
    return SDB_OK;
}

sdb_status sdb_cursor_close(sdb_cursor *cursor)
{
    if (cursor == NULL) {
        return SDB_OK;
    }
    if (cursor->snapshot != NULL) {
        sdb_mutex_lock(&cursor->database->mutex);
        if (cursor->snapshot->open_cursors != 0U) {
            cursor->snapshot->open_cursors -= 1U;
        }
        sdb_mutex_unlock(&cursor->database->mutex);
    }
    if (cursor->started) {
        sdb_btree_cursor_close(&cursor->btcur);
    }
    free(cursor->prefix);
    free(cursor->lower_bound);
    free(cursor->upper_bound);
    free(cursor->key_buffer);
    free(cursor->value_buffer);
    free(cursor);
    return SDB_OK;
}

sdb_status sdb_kv_scan_prefix(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *prefix,
    size_t prefix_size,
    const sdb_scan_options *options,
    sdb_scan_visit_fn visitor,
    void *context,
    size_t *match_count_out
)
{
    sdb_snapshot *snapshot = NULL;
    sdb_cursor *cursor = NULL;
    sdb_cursor_options cursor_options;
    size_t matches = 0U;
    sdb_status status;
    if (visitor == NULL || (prefix == NULL && prefix_size != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (options != NULL
        && (options->struct_size < (uint32_t)sizeof(*options)
            || options->reverse)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (match_count_out != NULL) {
        *match_count_out = 0U;
    }
    status = sdb_snapshot_open(database, &snapshot);
    if (status != SDB_OK) {
        return status;
    }
    sdb_cursor_options_init(&cursor_options);
    /*
     * Seek straight to the prefix; the walk stops at the first key that no
     * longer begins with it (keys sharing a prefix are contiguous).
     */
    cursor_options.lower_bound = prefix_size != 0U ? prefix : NULL;
    cursor_options.lower_bound_size = prefix_size;
    if (options != NULL) {
        cursor_options.limit = options->limit;
    }
    status = sdb_cursor_open(
        snapshot, SDB_KEYSPACE_KV, namespace_name, namespace_size,
        &cursor_options, &cursor
    );
    if (status == SDB_OK) {
        status = sdb_cursor_first(cursor);
    }
    while (status == SDB_OK && sdb_cursor_valid(cursor)) {
        if (prefix_size != 0U
            && (cursor->user_key_size < prefix_size
                || memcmp(cursor->user_key, prefix, prefix_size) != 0)) {
            break;
        }
        matches += 1U;
        if (!visitor(
                context,
                cursor->user_key,
                cursor->user_key_size,
                cursor->value_buffer,
                cursor->value_size
            )) {
            break;
        }
        status = sdb_cursor_next(cursor);
    }
    (void)sdb_cursor_close(cursor);
    (void)sdb_snapshot_close(snapshot);
    if (status == SDB_OK && match_count_out != NULL) {
        *match_count_out = matches;
    }
    return status;
}

sdb_status sdb_list_namespaces(
    sdb_database *database,
    sdb_namespace_visit_fn visitor,
    void *context,
    size_t *count_out
)
{
    sdb_snapshot *snapshot = NULL;
    sdb_btree_cursor cursor;
    const uint8_t object_prefix[1] = {SDB_OBJECT_KEY_PREFIX};
    uint8_t *key = NULL;
    uint8_t *value = NULL;
    uint8_t *last_namespace = NULL;
    uint16_t last_kind = 0U;
    size_t last_namespace_size = 0U;
    bool have_last = false;
    size_t emitted = 0U;
    size_t capacity;
    sdb_status status;
    if (visitor == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (count_out != NULL) {
        *count_out = 0U;
    }
    status = sdb_snapshot_open(database, &snapshot);
    if (status != SDB_OK) {
        return status;
    }
    capacity = sdb_pager_payload_capacity(&database->pager);
    key = (uint8_t *)malloc(capacity);
    value = (uint8_t *)malloc(capacity);
    last_namespace = (uint8_t *)malloc(SDB_ENGINE_MAX_NAME_SIZE);
    if (key == NULL || value == NULL || last_namespace == NULL) {
        status = SDB_E_OUT_OF_MEMORY;
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    sdb_mutex_lock(&database->mutex);
    if (status == SDB_OK) {
        /*
         * Seek to the first object-metadata row (prefix 0x40) and walk while the
         * lead byte stays 0x40. Chunk rows (0x41) sort immediately after, so the
         * walk stops cleanly at the first non-object key.
         */
        status = sdb_btree_cursor_seek(
            &database->tree, object_prefix, 1U, &cursor
        );
    }
    while (status == SDB_OK && cursor.valid) {
        size_t key_size = 0U;
        size_t value_size = 0U;
        uint16_t kind;
        const uint8_t *namespace_name;
        const uint8_t *object_key;
        size_t namespace_size;
        size_t object_key_size;
        status = sdb_btree_cursor_read(
            &cursor, key, capacity, &key_size, value, capacity, &value_size
        );
        if (status != SDB_OK) {
            break;
        }
        if (key_size == 0U || key[0] != SDB_OBJECT_KEY_PREFIX) {
            break;
        }
        status = sdb_verify_pair_key(
            key, key_size, 0U, &kind, &namespace_name, &namespace_size,
            &object_key, &object_key_size
        );
        if (status != SDB_OK) {
            break;
        }
        /*
         * Rows sharing a (kind, namespace) are contiguous, so a change from the
         * last emitted pair marks a new namespace to report exactly once.
         */
        if (!have_last || last_kind != kind
            || last_namespace_size != namespace_size
            || memcmp(last_namespace, namespace_name, namespace_size) != 0) {
            if (!visitor(context, kind, namespace_name, namespace_size)) {
                break;
            }
            emitted += 1U;
            last_kind = kind;
            last_namespace_size = namespace_size;
            (void)memcpy(last_namespace, namespace_name, namespace_size);
            have_last = true;
        }
        status = sdb_btree_cursor_next(&cursor);
    }
    sdb_btree_cursor_close(&cursor);
    sdb_mutex_unlock(&database->mutex);
    free(key);
    free(value);
    free(last_namespace);
    (void)sdb_snapshot_close(snapshot);
    if (status == SDB_OK && count_out != NULL) {
        *count_out = emitted;
    }
    return status;
}

#undef SDB_ENGINE_LOCK_OR_RETURN

