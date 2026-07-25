#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *test_path = "test-engine.tmp";
static const char *wal_path = "test-engine.tmp.wal";
static const char *lock_path = "test-engine.tmp.lock";
static const char *backup_path = "test-engine-backup.tmp";
static const char *backup_wal_path = "test-engine-backup.tmp.wal";
static const char *backup_lock_path = "test-engine-backup.tmp.lock";

typedef struct id_list {
    char ids[8][32];
    size_t count;
} id_list;

typedef struct mutating_visit_context {
    sdb_database *database;
    size_t visited;
    sdb_status close_status;
    sdb_status compact_status;
} mutating_visit_context;

static bool collect_id(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    id_list *list = (id_list *)context;
    assert(list->count < 8U);
    assert(document_id_size < sizeof(list->ids[0]));
    (void)memcpy(list->ids[list->count], document_id, document_id_size);
    list->ids[list->count][document_id_size] = '\0';
    ++list->count;
    return true;
}

static bool has_id(const id_list *list, const char *id)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        if (strcmp(list->ids[index], id) == 0) {
            return true;
        }
    }
    return false;
}

static void test_invalid_database_path(void)
{
    sdb_database_options options;
    sdb_database *database = (sdb_database *)(uintptr_t)1U;
    sdb_database_options_init(&options);
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    assert(sdb_database_create(
        "", &options, &database
    ) == SDB_E_INVALID_ARGUMENT);
    assert(database == NULL);
    database = (sdb_database *)(uintptr_t)1U;
    assert(sdb_database_open(
        "", &options, &database
    ) == SDB_E_INVALID_ARGUMENT);
    assert(database == NULL);
}

static void test_default_kdf_strength(void)
{
    static const uint8_t password[] = "default-kdf-password";
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_superblock_read_result result;
    (void)remove(test_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    options.password = password;
    options.password_size = sizeof(password) - 1U;
    assert(options.kdf_iterations == SDB_DEFAULT_KDF_ITERATIONS);
    assert(sdb_database_create(
        test_path, &options, &database
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.superblock.kdf_iterations
        == SDB_DEFAULT_KDF_ITERATIONS);
    database = NULL;
    assert(sdb_database_open(
        test_path, &options, &database
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(test_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
}

static bool compact_during_visit(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    mutating_visit_context *visit = (mutating_visit_context *)context;
    assert(document_id != NULL);
    assert(document_id_size == 4U);
    if (visit->visited == 0U) {
        sdb_database_options options;
        sdb_compact_result result;
        visit->close_status = sdb_database_close(visit->database);
        sdb_database_options_init(&options);
        visit->compact_status = sdb_database_compact(
            visit->database, &options, &result
        );
    }
    ++visit->visited;
    return true;
}

static void test_mutating_index_visitor(void)
{
    static const uint8_t collection[] = "visitor";
    static const uint8_t index_name[] = "group";
    static const uint8_t index_value[] = "all";
    static const uint8_t document[] = "{}";
    sdb_database_options options;
    sdb_database *database;
    mutating_visit_context context;
    sdb_verify_result verify;
    size_t match_count = 0U;
    size_t index;

    (void)remove(test_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        test_path, &options, &database
    ) == SDB_OK);
    assert(sdb_index_create(
        database,
        collection,
        sizeof(collection),
        index_name,
        sizeof(index_name),
        false
    ) == SDB_OK);
    for (index = 0U; index < 96U; ++index) {
        uint8_t document_id[5];
        sdb_index_term term = {
            index_name, sizeof(index_name), index_value, sizeof(index_value)
        };
        const int written = snprintf(
            (char *)document_id, sizeof(document_id), "%04zu", index
        );
        assert(written == 4);
        assert(sdb_document_put(
            database,
            collection,
            sizeof(collection),
            document_id,
            4U,
            document,
            sizeof(document),
            &term,
            1U
        ) == SDB_OK);
    }
    (void)memset(&context, 0, sizeof(context));
    context.database = database;
    context.close_status = SDB_E_INTERNAL;
    context.compact_status = SDB_E_INTERNAL;
    assert(sdb_index_visit(
        database,
        collection,
        sizeof(collection),
        index_name,
        sizeof(index_name),
        index_value,
        sizeof(index_value),
        compact_during_visit,
        &context,
        &match_count
    ) == SDB_OK);
    assert(context.close_status == SDB_E_BUSY);
    assert(context.compact_status == SDB_OK);
    assert(context.visited == 96U);
    assert(match_count == 96U);
    assert(sdb_database_verify(database, &verify) == SDB_OK);
    assert(verify.object_count == 96U);
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
}

static void test_balanced_chunk_regression(void)
{
    static const uint8_t namespace_name[] = "split";
    static const uint8_t key[] = "guard";
    uint8_t value[4096];
    sdb_database_options options;
    sdb_database *database;
    sdb_verify_result result;
    size_t index;

    (void)remove(test_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    for (index = 0U; index < sizeof(value); ++index) {
        value[index] = (uint8_t)((index * 31U) & 0xffU);
    }
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        test_path, &options, &database
    ) == SDB_OK);
    assert(sdb_kv_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        value,
        sizeof(value)
    ) == SDB_OK);
    assert(sdb_database_verify(database, &result) == SDB_OK);
    /*
     * A 4 KiB object must use at least three chunks.  Two chunks would make
     * each leaf entry larger than half the usable payload and reintroduce the
     * order-dependent split failure found by the 10,000-operation soak.
     */
    assert(result.object_count == 1U);
    assert(result.live_chunk_count >= 3U);
    assert(result.logical_byte_count == sizeof(value));
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
}

static void run_conformance(bool encrypted)
{
    static const uint8_t password[] = "engine-conformance-password";
    static const uint8_t migrated_password[] =
        "engine-migrated-password";
    static const uint8_t namespace_name[] = "settings";
    static const uint8_t key[] = "profile";
    static const uint8_t collection[] = "users";
    static const uint8_t color_index[] = "color";
    static const uint8_t email_index[] = "email";
    static const uint8_t red[] = "red";
    static const uint8_t blue[] = "blue";
    static const uint8_t email_a[] = "a@example.test";
    static const uint8_t email_b[] = "b@example.test";
    static const uint8_t doc1_id[] = "doc-1";
    static const uint8_t doc2_id[] = "doc-2";
    static const uint8_t doc3_id[] = "doc-3";
    static const uint8_t doc1[] = "{\"name\":\"one\"}";
    static const uint8_t doc1_updated[] = "{\"name\":\"one-updated\"}";
    static const uint8_t doc2[] = "{\"name\":\"two\"}";
    static const uint8_t doc3[] = "{\"name\":\"three\"}";
    sdb_database_options options;
    sdb_database *database;
    uint8_t *large;
    uint8_t *actual;
    size_t actual_size;
    size_t index;
    sdb_verify_result verify_result;
    sdb_backup_result backup_result;
    sdb_compact_result compact_result;

    (void)remove(test_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)remove(backup_path);
    (void)remove(backup_wal_path);
    (void)remove(backup_lock_path);
    sdb_database_options_init(&options);
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    if (encrypted) {
        options.password = password;
        options.password_size = sizeof(password) - 1U;
    }
    assert(sdb_database_create(
        test_path, &options, &database
    ) == SDB_OK);

    large = (uint8_t *)malloc(100000U);
    actual = (uint8_t *)malloc(100000U);
    assert(large != NULL && actual != NULL);
    for (index = 0U; index < 100000U; ++index) {
        large[index] = (uint8_t)((index * 37U) & 0xffU);
    }

    assert(sdb_kv_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        large,
        7000U
    ) == SDB_OK);
    assert(sdb_kv_get(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        NULL,
        0U,
        &actual_size
    ) == SDB_E_BUFFER_TOO_SMALL);
    assert(actual_size == 7000U);
    assert(sdb_kv_get(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        actual,
        7000U,
        &actual_size
    ) == SDB_OK);
    assert(memcmp(actual, large, 7000U) == 0);
    assert(sdb_kv_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        NULL,
        0U
    ) == SDB_OK);
    assert(sdb_kv_get(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        NULL,
        0U,
        &actual_size
    ) == SDB_OK);
    assert(actual_size == 0U);

    assert(sdb_blob_put(
        database,
        (const uint8_t *)"media",
        5U,
        (const uint8_t *)"large",
        5U,
        large,
        100000U
    ) == SDB_OK);
    assert(sdb_blob_get(
        database,
        (const uint8_t *)"media",
        5U,
        (const uint8_t *)"large",
        5U,
        actual,
        100000U,
        &actual_size
    ) == SDB_OK);
    assert(actual_size == 100000U);
    assert(memcmp(actual, large, 100000U) == 0);

    assert(sdb_index_create(
        database,
        collection,
        sizeof(collection),
        color_index,
        sizeof(color_index),
        false
    ) == SDB_OK);
    assert(sdb_index_create(
        database,
        collection,
        sizeof(collection),
        email_index,
        sizeof(email_index),
        true
    ) == SDB_OK);
    assert(sdb_index_create(
        database,
        collection,
        sizeof(collection),
        email_index,
        sizeof(email_index),
        false
    ) == SDB_E_CONFLICT);
    {
        sdb_index_term terms[] = {
            {color_index, sizeof(color_index), red, sizeof(red)},
            {email_index, sizeof(email_index), email_a, sizeof(email_a)}
        };
        assert(sdb_document_put(
            database,
            collection,
            sizeof(collection),
            doc1_id,
            sizeof(doc1_id),
            doc1,
            sizeof(doc1),
            terms,
            2U
        ) == SDB_OK);
    }
    {
        sdb_index_term terms[] = {
            {color_index, sizeof(color_index), red, sizeof(red)},
            {email_index, sizeof(email_index), email_b, sizeof(email_b)}
        };
        assert(sdb_document_put(
            database,
            collection,
            sizeof(collection),
            doc2_id,
            sizeof(doc2_id),
            doc2,
            sizeof(doc2),
            terms,
            2U
        ) == SDB_OK);
    }
    {
        id_list matches = {{{0}}, 0U};
        size_t match_count;
        assert(sdb_index_visit(
            database,
            collection,
            sizeof(collection),
            color_index,
            sizeof(color_index),
            red,
            sizeof(red),
            collect_id,
            &matches,
            &match_count
        ) == SDB_OK);
        assert(match_count == 2U);
        assert(has_id(&matches, "doc-1"));
        assert(has_id(&matches, "doc-2"));
    }
    {
        sdb_index_term conflict_terms[] = {
            {email_index, sizeof(email_index), email_a, sizeof(email_a)}
        };
        assert(sdb_document_put(
            database,
            collection,
            sizeof(collection),
            doc3_id,
            sizeof(doc3_id),
            doc3,
            sizeof(doc3),
            conflict_terms,
            1U
        ) == SDB_E_CONFLICT);
        assert(sdb_document_get(
            database,
            collection,
            sizeof(collection),
            doc3_id,
            sizeof(doc3_id),
            actual,
            100000U,
            &actual_size
        ) == SDB_E_NOT_FOUND);
    }
    {
        sdb_index_term update_terms[] = {
            {color_index, sizeof(color_index), blue, sizeof(blue)},
            {email_index, sizeof(email_index), email_a, sizeof(email_a)}
        };
        assert(sdb_document_put(
            database,
            collection,
            sizeof(collection),
            doc1_id,
            sizeof(doc1_id),
            doc1_updated,
            sizeof(doc1_updated),
            update_terms,
            2U
        ) == SDB_OK);
    }
    {
        id_list red_matches = {{{0}}, 0U};
        id_list blue_matches = {{{0}}, 0U};
        size_t red_count;
        size_t blue_count;
        assert(sdb_index_visit(
            database,
            collection,
            sizeof(collection),
            color_index,
            sizeof(color_index),
            red,
            sizeof(red),
            collect_id,
            &red_matches,
            &red_count
        ) == SDB_OK);
        assert(red_count == 1U && has_id(&red_matches, "doc-2"));
        assert(sdb_index_visit(
            database,
            collection,
            sizeof(collection),
            color_index,
            sizeof(color_index),
            blue,
            sizeof(blue),
            collect_id,
            &blue_matches,
            &blue_count
        ) == SDB_OK);
        assert(blue_count == 1U && has_id(&blue_matches, "doc-1"));
    }
    assert(sdb_document_delete(
        database,
        collection,
        sizeof(collection),
        doc2_id,
        sizeof(doc2_id)
    ) == SDB_OK);
    {
        sdb_index_term reused_unique[] = {
            {email_index, sizeof(email_index), email_b, sizeof(email_b)}
        };
        assert(sdb_document_put(
            database,
            collection,
            sizeof(collection),
            doc3_id,
            sizeof(doc3_id),
            doc3,
            sizeof(doc3),
            reused_unique,
            1U
        ) == SDB_OK);
    }

    assert(sdb_database_close(database) == SDB_OK);
    assert(sdb_database_open(test_path, &options, &database) == SDB_OK);
    assert(sdb_blob_get(
        database,
        (const uint8_t *)"media",
        5U,
        (const uint8_t *)"large",
        5U,
        actual,
        100000U,
        &actual_size
    ) == SDB_OK);
    assert(memcmp(actual, large, 100000U) == 0);
    assert(sdb_document_get(
        database,
        collection,
        sizeof(collection),
        doc1_id,
        sizeof(doc1_id),
        actual,
        100000U,
        &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(doc1_updated));
    assert(memcmp(actual, doc1_updated, sizeof(doc1_updated)) == 0);

    assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    assert(verify_result.btree_node_count != 0U);
    assert(verify_result.btree_leaf_count != 0U);
    assert(verify_result.raw_entry_count != 0U);
    assert(verify_result.object_count != 0U);
    assert(verify_result.live_chunk_count != 0U);
    assert(verify_result.stale_entry_count != 0U);
    assert(verify_result.logical_byte_count >= 100000U);
    assert(sdb_database_backup(
        database, backup_path, false, &backup_result
    ) == SDB_OK);
    assert(backup_result.byte_count != 0U);
    assert(sdb_database_backup(
        database, backup_path, false, &backup_result
    ) == SDB_E_IO);
    {
        sdb_database *backup_database = NULL;
        assert(sdb_database_open(
            backup_path, &options, &backup_database
        ) == SDB_OK);
        assert(sdb_database_verify(
            backup_database, &verify_result
        ) == SDB_OK);
        assert(sdb_blob_get(
            backup_database,
            (const uint8_t *)"media",
            5U,
            (const uint8_t *)"large",
            5U,
            actual,
            100000U,
            &actual_size
        ) == SDB_OK);
        assert(actual_size == 100000U);
        assert(memcmp(actual, large, 100000U) == 0);
        assert(sdb_database_close(backup_database) == SDB_OK);
    }

    assert(sdb_database_migrate(
        database,
        (uint16_t)(SDB_FORMAT_VERSION_V1 + 1U),
        &options,
        &compact_result
    ) == SDB_E_UNSUPPORTED_VERSION);
    assert(sdb_database_compact(
        database, &options, &compact_result
    ) == SDB_OK);
    assert(compact_result.raw_entries_after
        < compact_result.raw_entries_before);
    assert(compact_result.byte_count_after
        < compact_result.byte_count_before);
    assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    assert(verify_result.stale_entry_count == 0U);
    {
        sdb_database_options migration_options = options;
        sdb_superblock_read_result migrated_superblock;
        migration_options.page_size = UINT32_C(8192);
        migration_options.password = migrated_password;
        migration_options.password_size =
            sizeof(migrated_password) - 1U;
        assert(sdb_database_migrate(
            database,
            SDB_FORMAT_VERSION_V1,
            &migration_options,
            &compact_result
        ) == SDB_OK);
        assert(sdb_superblock_store_read(
            test_path, &migrated_superblock
        ) == SDB_OK);
        assert(migrated_superblock.superblock.page_size == UINT32_C(8192));
        assert((migrated_superblock.superblock.flags & SDB_FLAG_ENCRYPTED)
            != 0U);
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        assert(sdb_blob_get(
            database,
            (const uint8_t *)"media",
            5U,
            (const uint8_t *)"large",
            5U,
            actual,
            100000U,
            &actual_size
        ) == SDB_OK);
        assert(actual_size == 100000U);
        assert(memcmp(actual, large, 100000U) == 0);
        assert(sdb_database_close(database) == SDB_OK);
        options = migration_options;
        assert(sdb_database_open(
            test_path, &options, &database
        ) == SDB_OK);
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    }

    assert(sdb_kv_delete(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key)
    ) == SDB_OK);
    assert(sdb_kv_get(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        actual,
        100000U,
        &actual_size
    ) == SDB_E_NOT_FOUND);
    assert(sdb_blob_delete(
        database,
        (const uint8_t *)"media",
        5U,
        (const uint8_t *)"large",
        5U
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
    free(actual);
    free(large);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
    (void)remove(backup_wal_path);
    (void)remove(backup_path);
    (void)remove(backup_lock_path);
}

int main(void)
{
    test_invalid_database_path();
    test_default_kdf_strength();
    test_mutating_index_visitor();
    test_balanced_chunk_regression();
    run_conformance(false);
    run_conformance(true);
    (void)puts("engine conformance tests: ok");
    return 0;
}
