#include "shibadb_engine.h"

#include "btree.h"
#include "crypto.h"
#include "engine_internal.h"
#include "internal.h"
#include "replace.h"
#include "sync.h"

#include <stdlib.h>
#include <string.h>

#if SDB_TESTING
static unsigned sdb_compact_crash_phase_for_testing;
static unsigned sdb_backup_crash_phase_for_testing;
static size_t sdb_backup_file_fail_after_for_testing = SIZE_MAX;
#endif

#define SDB_OBJECT_KEY_PREFIX UINT8_C(0x40)
#define SDB_CHUNK_KEY_PREFIX UINT8_C(0x41)
#define SDB_INDEX_DEFINITION_PREFIX UINT8_C(0x50)
#define SDB_INDEX_ENTRY_PREFIX UINT8_C(0x51)
#define SDB_UNIQUE_GUARD_PREFIX UINT8_C(0x52)
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
    if (!sdb_engine_bytes_valid(namespace_name, namespace_size)
        || !sdb_engine_bytes_valid(key, key_size)
        || output == NULL || output_size == NULL
        || namespace_size > SIZE_MAX - key_size - 6U
        || suffix_size > SIZE_MAX - namespace_size - key_size - 6U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    size = 6U + namespace_size + key_size + suffix_size;
    result = (uint8_t *)malloc(size);
    if (result == NULL) {
        return SDB_E_INTERNAL;
    }
    result[0] = prefix;
    result[1] = kind;
    sdb_write_u16_le(result + 2U, (uint16_t)namespace_size);
    sdb_write_u16_le(result + 4U, (uint16_t)key_size);
    (void)memcpy(result + 6U, namespace_name, namespace_size);
    (void)memcpy(result + 6U + namespace_size, key, key_size);
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
    sdb_status status = sdb_engine_pair_key(
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
    size_t encoded_size;
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
    if (status == SDB_OK) {
        if (current.generation == UINT64_MAX) {
            return SDB_E_OVERFLOW;
        }
        metadata_out->generation = current.generation + 1U;
    } else if (status == SDB_E_NOT_FOUND) {
        metadata_out->generation = 1U;
    } else {
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
    /*
     * Keep every object-chunk entry at or below half of a leaf's usable
     * payload.  This guarantees that an overflowing leaf always has a valid
     * two-way split, regardless of key ordering.
     */
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
    sdb_status status = sdb_object_plan(
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
    return status;
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
        size_t chunk_size;
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
        status = sdb_engine_tree_delete(
            database, mutation, metadata_key, metadata_key_size
        );
    }
    free(metadata_key);
    return status;
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
        return SDB_E_INTERNAL;
    }
    database->path = resolved_path;
    /*
     * Do NOT copy the password onto database->password here. The
     * plaintext password is needed only during sdb_pager_create_encrypted
     * (which derives the data key and discards the caller buffer);
     * no other code path on `database` reads it back. Leaving it on
     * the heap for the DB lifetime is a needless disclosure surface
     * for core dumps and swap-file exfiltration.
     */
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
        status = sdb_pager_create_encrypted(
            database->path,
            options->page_size,
            salt,
            file_id,
            options->password,
            options->password_size,
            options->kdf_iterations,
            &database->pager
        );
    } else if (status == SDB_OK) {
        status = sdb_pager_create(
            database->path,
            options->page_size,
            salt,
            file_id,
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
    sdb_database *database;
    char *resolved_path = NULL;
    sdb_superblock_read_result result;
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
        return SDB_E_INTERNAL;
    }
    database->path = resolved_path;
    /* Same reasoning as sdb_database_create: the password buffer is
     * consumed inside sdb_pager_open_encrypted below and never read
     * from database->password again on this handle. */
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
        status = sdb_pager_open_encrypted(
            database->path,
            options->password,
            options->password_size,
            &database->pager
        );
    } else {
        status = options->password_size == 0U
            ? sdb_pager_open(database->path, &database->pager)
            : SDB_E_INVALID_ARGUMENT;
    }
    if (status == SDB_OK) {
        status = sdb_btree_open(&database->pager, &database->tree);
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
        || database->active_transaction != NULL) {
        sdb_mutex_unlock(&database->mutex);
        return SDB_E_BUSY;
    }
    status = sdb_pager_close(&database->pager);
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
        return SDB_E_INTERNAL;
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
    uint8_t definition;
    size_t definition_size;
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
    size_t guard_value_size;
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
    bool *unique;
    size_t index;
    sdb_status status;
    if (database == NULL || !database->open
        || !sdb_engine_bytes_valid(document_id, document_id_size)
        || (document == NULL && document_size != 0U)
        || (terms == NULL && term_count != 0U)
        || term_count > SIZE_MAX / sizeof(*unique)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    unique = term_count == 0U
        ? NULL : (bool *)calloc(term_count, sizeof(*unique));
    if (term_count != 0U && unique == NULL) {
        return SDB_E_INTERNAL;
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
    free(unique);
    return status;
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
        status = sdb_object_delete(
            database,
            &mutation,
            (uint16_t)SDB_OBJECT_DOCUMENT,
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
        return SDB_E_INTERNAL;
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
            /*
             * A recursive visitor may split, compact, or migrate the tree.
             * Release the decoded leaf before calling application code, then
             * seek on the current full index key in the possibly new tree.
             */
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
    if (key == NULL || key_size < 6U + suffix_size
        || kind_out == NULL || namespace_out == NULL
        || namespace_size_out == NULL || object_key_out == NULL
        || object_key_size_out == NULL) {
        return SDB_E_CORRUPT;
    }
    namespace_size = (size_t)sdb_read_u16_le(key + 2U);
    object_key_size = (size_t)sdb_read_u16_le(key + 4U);
    if (namespace_size == 0U || object_key_size == 0U
        || namespace_size > SDB_ENGINE_MAX_NAME_SIZE
        || object_key_size > SDB_ENGINE_MAX_NAME_SIZE
        || namespace_size > key_size - 6U - suffix_size
        || object_key_size
            != key_size - 6U - suffix_size - namespace_size
        || (key[1] != (uint8_t)SDB_OBJECT_KV
            && key[1] != (uint8_t)SDB_OBJECT_BLOB
            && key[1] != (uint8_t)SDB_OBJECT_DOCUMENT)) {
        return SDB_E_CORRUPT;
    }
    *kind_out = (uint16_t)key[1];
    *namespace_out = key + 6U;
    *namespace_size_out = namespace_size;
    *object_key_out = key + 6U + namespace_size;
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
    sdb_object_metadata metadata;
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
        return SDB_E_INTERNAL;
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
        uint64_t generation;
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
        return SDB_E_INTERNAL;
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
        return SDB_E_INTERNAL;
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
        return SDB_E_INTERNAL;
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
        return SDB_E_INTERNAL;
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
    sdb_verify_result verify_result;
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
    if (status == SDB_OK) {
        status = sdb_process_lock_acquire(
            resolved_destination, &destination_lock
        );
    }
    if (status == SDB_OK) {
        status = sdb_database_verify_unlocked(database, &verify_result);
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
            status = SDB_E_INTERNAL;
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
        /*
         * Surface the pre-backup verify count so callers can compare against
         * a subsequent verify or compact for consistency.
         */
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
    sdb_verify_result target_verify;
    sdb_database *target = NULL;
    sdb_btree_cursor cursor;
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
    /*
     * Compact/migrate hot-swap the pager; a live transaction still holds a
     * batch pointing at the old pager, so replacing it silently orphans the
     * caller's txn and leaks the old pager's fd/cache. Refuse up-front.
     */
    if (database->active_transaction != NULL) {
        return SDB_E_BUSY;
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
            status = SDB_E_INTERNAL;
        }
    }
    (void)memset(&cursor, 0, sizeof(cursor));
    if (status == SDB_OK) {
        status = sdb_btree_cursor_first(&database->tree, &cursor);
    }
    while (status == SDB_OK && cursor.valid) {
        size_t key_size;
        size_t value_size;
        bool live;
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
            status = sdb_btree_put(
                &target->tree, key, key_size, value, value_size
            );
        }
        if (status == SDB_OK) {
            status = sdb_btree_cursor_next(&cursor);
        }
    }
    sdb_btree_cursor_close(&cursor);
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
    if (status == SDB_OK) {
        status = sdb_file_replace(
            temporary_path, database->path, true
        );
        replaced = status == SDB_OK;
    }
    if (replaced) {
        const sdb_status directory_status =
            sdb_file_sync_parent_directory(database->path);
        sdb_compact_test_crash(3U);
        {
            const sdb_status finish_status =
                sdb_replace_finish(database->path);
            if (status == SDB_OK) {
                status = finish_status;
            }
        }
        const sdb_status close_status =
            sdb_pager_close(&database->pager);
        char *old_target_wal = target->pager.wal_path;
        target->pager.wal_path = source_wal_path;
        source_wal_path = NULL;
        database->pager = target->pager;
        (void)memset(&target->pager, 0, sizeof(target->pager));
        database->tree = target->tree;
        database->tree.pager = &database->pager;
        (void)memset(&target->tree, 0, sizeof(target->tree));
        target->open = false;
        {
            const sdb_status identity_status =
                sdb_process_lock_release(&database->process_lock);
            database->process_lock = target->process_lock;
            target->process_lock.held = false;
            if (status == SDB_OK) {
                status = identity_status;
            }
        }
        {
            const sdb_status target_path_status =
                sdb_process_lock_release(&target->path_lock);
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
            /*
             * Post-swap error: the pager has already been replaced but the
             * caller sees a non-OK status. Force needs_recovery so no further
             * op can slip through and observe the half-installed state — the
             * caller must close and reopen, at which point sdb_replace_recover
             * will resolve any stranded marker.
             */
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

/*
 * The public handle API is serialized here rather than in the pager. This
 * keeps the storage primitives usable by deterministic tests while making a
 * database handle safe to share between application threads.
 *
 * Note: the `!database_->open` fast-path read below is *unsynchronized*
 * by design — the invariant is that callers do not close a handle while
 * another thread is still issuing operations on it. A concurrent close
 * with concurrent operations is undefined behavior at the API layer; that
 * cannot be defended in one macro without turning every call into a full
 * mutex acquisition, which defeats the fast-fail contract. The pre-lock
 * read is a cheap best-effort check, not a race defense. Real API-misuse
 * protection lives one layer up in the caller-serialize contract.
 */
#define SDB_ENGINE_LOCK_OR_RETURN(database_) \
    do { \
        if ((database_) == NULL || !(database_)->open) { \
            return SDB_E_INVALID_ARGUMENT; \
        } \
        sdb_mutex_lock(&(database_)->mutex); \
    } while (0)

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

sdb_status sdb_database_backup(
    sdb_database *database,
    const char *destination_path,
    bool replace_existing,
    sdb_backup_result *result_out
)
{
    sdb_status status;
    SDB_ENGINE_LOCK_OR_RETURN(database);
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
        return SDB_E_INTERNAL;
    }
    sdb_mutex_lock(&database->mutex);
    if (!database->open || database->callback_depth != 0U
        || database->active_transaction != NULL) {
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
    /*
     * Return BUSY for an active transaction so misuse is detectable, but do
     * not implicitly rollback+free — an existing test contract (and any
     * caller mirroring it) inspects transaction pointer post-BUSY. Callers
     * that want RAII semantics wrap this: `if (BUSY) rollback(); then close()`.
     * The prior wedge only occurs when the caller ignores BUSY and also
     * skips rollback; the API contract is now documented.
     */
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
        status = sdb_object_delete(
            database, &transaction->mutation,
            (uint16_t)SDB_OBJECT_DOCUMENT,
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
    status = database->active_transaction != NULL
        ? SDB_E_BUSY
        : sdb_kv_put_unlocked(
            database, namespace_name, namespace_size, key, key_size,
            value, value_size
        );
    sdb_mutex_unlock(&database->mutex);
    return status;
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
    status = database->active_transaction != NULL
        ? SDB_E_BUSY
        : sdb_kv_delete_unlocked(
            database, namespace_name, namespace_size, key, key_size
        );
    sdb_mutex_unlock(&database->mutex);
    return status;
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
    status = database->active_transaction != NULL
        ? SDB_E_BUSY
        : sdb_blob_put_unlocked(
            database, namespace_name, namespace_size, key, key_size,
            value, value_size
        );
    sdb_mutex_unlock(&database->mutex);
    return status;
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
    status = database->active_transaction != NULL
        ? SDB_E_BUSY
        : sdb_blob_delete_unlocked(
            database, namespace_name, namespace_size, key, key_size
        );
    sdb_mutex_unlock(&database->mutex);
    return status;
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
    status = database->active_transaction != NULL
        ? SDB_E_BUSY
        : sdb_index_create_unlocked(
            database, collection, collection_size,
            index_name, index_name_size, unique
        );
    sdb_mutex_unlock(&database->mutex);
    return status;
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
    status = database->active_transaction != NULL
        ? SDB_E_BUSY
        : sdb_document_put_unlocked(
            database, collection, collection_size,
            document_id, document_id_size, document, document_size,
            terms, term_count
        );
    sdb_mutex_unlock(&database->mutex);
    return status;
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
    status = database->active_transaction != NULL
        ? SDB_E_BUSY
        : sdb_document_delete_unlocked(
            database, collection, collection_size,
            document_id, document_id_size
        );
    sdb_mutex_unlock(&database->mutex);
    return status;
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

#undef SDB_ENGINE_LOCK_OR_RETURN
