#include "shibadb_engine.h"
#include "engine_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *database_path = "test-public-transaction.tmp";
static const char *wal_path = "test-public-transaction.tmp.wal";
static const char *lock_path = "test-public-transaction.tmp.lock";
static const uint8_t namespace_name[] = "transaction";

typedef struct transaction_visit_context {
    sdb_transaction *transaction;
    sdb_status mutation_status;
    size_t visits;
} transaction_visit_context;

static bool mutate_during_visit(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    static const uint8_t key[] = "callback";
    static const uint8_t value[] = "blocked";
    transaction_visit_context *visit =
        (transaction_visit_context *)context;
    assert(document_id != NULL);
    assert(document_id_size != 0U);
    visit->mutation_status = sdb_transaction_kv_put(
        visit->transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), value, sizeof(value)
    );
    ++visit->visits;
    return true;
}

static void remove_test_files(void)
{
    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
}

static void assert_kv(
    sdb_database *database,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *expected,
    size_t expected_size
)
{
    uint8_t actual[8192];
    size_t actual_size = 0U;
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name), key, key_size,
        actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == expected_size);
    assert(memcmp(actual, expected, expected_size) == 0);
}

static void test_rollback_and_lifecycle(sdb_database *database)
{
    static const uint8_t key[] = "rollback-key";
    static const uint8_t value[] = "staged-value";
    uint8_t actual[64];
    size_t actual_size = 0U;
    sdb_transaction *transaction = NULL;
    sdb_transaction *second = NULL;

    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    assert(transaction != NULL);
    assert(sdb_transaction_begin(database, &second) == SDB_E_BUSY);
    assert(second == NULL);
    assert(sdb_database_close(database) == SDB_E_BUSY);
    assert(sdb_transaction_close(transaction) == SDB_E_BUSY);
    assert(sdb_transaction_kv_put(
        transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), value, sizeof(value)
    ) == SDB_OK);
    assert(sdb_transaction_kv_get(
        transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(value));
    assert(memcmp(actual, value, sizeof(value)) == 0);
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name), key, sizeof(key),
        actual, sizeof(actual), &actual_size
    ) == SDB_E_NOT_FOUND);
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name), key, sizeof(key),
        value, sizeof(value)
    ) == SDB_E_BUSY);
    assert(sdb_transaction_rollback(transaction) == SDB_OK);
    assert(sdb_transaction_rollback(transaction) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_transaction_kv_get(
        transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_transaction_close(transaction) == SDB_OK);
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name), key, sizeof(key),
        actual, sizeof(actual), &actual_size
    ) == SDB_E_NOT_FOUND);

    transaction = NULL;
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    assert(sdb_transaction_kv_put(
        transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), value, SDB_TRANSACTION_MAX_LOGICAL_BYTES + 1U
    ) == SDB_E_OVERFLOW);
    assert(sdb_transaction_rollback(transaction) == SDB_OK);
    assert(sdb_transaction_close(transaction) == SDB_OK);
}

static void test_multi_object_commit(sdb_database *database)
{
    static const uint8_t key_one[] = "one";
    static const uint8_t key_two[] = "two";
    static const uint8_t old_value[] = "old";
    static const uint8_t new_value[] = "new";
    uint8_t large_value[7000];
    uint8_t actual[8192];
    size_t actual_size = 0U;
    size_t index;
    sdb_transaction *transaction = NULL;

    for (index = 0U; index < sizeof(large_value); ++index) {
        large_value[index] = (uint8_t)(index * 31U);
    }
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name),
        key_one, sizeof(key_one), old_value, sizeof(old_value)
    ) == SDB_OK);
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    assert(sdb_transaction_kv_put(
        transaction, namespace_name, sizeof(namespace_name),
        key_one, sizeof(key_one), new_value, sizeof(new_value)
    ) == SDB_OK);
    assert(sdb_transaction_blob_put(
        transaction, namespace_name, sizeof(namespace_name),
        key_two, sizeof(key_two), large_value, sizeof(large_value)
    ) == SDB_OK);
    assert(sdb_transaction_kv_get(
        transaction, namespace_name, sizeof(namespace_name),
        key_one, sizeof(key_one), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(new_value));
    assert(memcmp(actual, new_value, sizeof(new_value)) == 0);
    assert(sdb_transaction_blob_get(
        transaction, namespace_name, sizeof(namespace_name),
        key_two, sizeof(key_two), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(large_value));
    assert(memcmp(actual, large_value, sizeof(large_value)) == 0);
    assert_kv(
        database, key_one, sizeof(key_one), old_value, sizeof(old_value)
    );
    assert(sdb_transaction_commit(transaction) == SDB_OK);
    assert(sdb_transaction_commit(transaction) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_transaction_close(transaction) == SDB_OK);
    assert_kv(
        database, key_one, sizeof(key_one), new_value, sizeof(new_value)
    );
    assert(sdb_blob_get(
        database, namespace_name, sizeof(namespace_name),
        key_two, sizeof(key_two), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(large_value));
    assert(memcmp(actual, large_value, sizeof(large_value)) == 0);
}

static void test_document_and_unique_index(sdb_database *database)
{
    static const uint8_t collection[] = "users";
    static const uint8_t index_name[] = "email";
    static const uint8_t index_value[] = "same@example.test";
    static const uint8_t document_one[] = "{\"id\":1}";
    static const uint8_t document_two[] = "{\"id\":2}";
    static const uint8_t id_one[] = "user-1";
    static const uint8_t id_two[] = "user-2";
    uint8_t actual[128];
    size_t actual_size = 0U;
    sdb_index_term term = {
        index_name, sizeof(index_name), index_value, sizeof(index_value)
    };
    sdb_transaction *transaction = NULL;
    transaction_visit_context visit = {NULL, SDB_E_INTERNAL, 0U};
    size_t match_count = 0U;

    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    assert(sdb_transaction_index_create(
        transaction, collection, sizeof(collection),
        index_name, sizeof(index_name), true
    ) == SDB_OK);
    assert(sdb_transaction_document_put(
        transaction, collection, sizeof(collection), id_one, sizeof(id_one),
        document_one, sizeof(document_one), &term, 1U
    ) == SDB_OK);
    assert(sdb_transaction_document_get(
        transaction, collection, sizeof(collection), id_one, sizeof(id_one),
        actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(document_one));
    assert(memcmp(actual, document_one, sizeof(document_one)) == 0);
    assert(sdb_document_get(
        database, collection, sizeof(collection), id_one, sizeof(id_one),
        actual, sizeof(actual), &actual_size
    ) == SDB_E_NOT_FOUND);
    assert(sdb_transaction_document_put(
        transaction, collection, sizeof(collection), id_two, sizeof(id_two),
        document_two, sizeof(document_two), &term, 1U
    ) == SDB_E_CONFLICT);
    assert(sdb_transaction_rollback(transaction) == SDB_OK);
    assert(sdb_transaction_close(transaction) == SDB_OK);

    transaction = NULL;
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    assert(sdb_transaction_index_create(
        transaction, collection, sizeof(collection),
        index_name, sizeof(index_name), true
    ) == SDB_OK);
    assert(sdb_transaction_document_put(
        transaction, collection, sizeof(collection), id_one, sizeof(id_one),
        document_one, sizeof(document_one), &term, 1U
    ) == SDB_OK);
    assert(sdb_transaction_commit(transaction) == SDB_OK);
    assert(sdb_transaction_close(transaction) == SDB_OK);
    assert(sdb_document_get(
        database, collection, sizeof(collection), id_one, sizeof(id_one),
        actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(document_one));
    assert(memcmp(actual, document_one, sizeof(document_one)) == 0);

    transaction = NULL;
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    visit.transaction = transaction;
    assert(sdb_index_visit(
        database, collection, sizeof(collection),
        index_name, sizeof(index_name), index_value, sizeof(index_value),
        mutate_during_visit, &visit, &match_count
    ) == SDB_OK);
    assert(visit.visits == 1U);
    assert(match_count == 1U);
    assert(visit.mutation_status == SDB_E_BUSY);
    assert(sdb_transaction_rollback(transaction) == SDB_OK);
    assert(sdb_transaction_close(transaction) == SDB_OK);

    transaction = NULL;
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    assert(sdb_transaction_document_delete(
        transaction, collection, sizeof(collection), id_one, sizeof(id_one)
    ) == SDB_OK);
    assert(sdb_transaction_document_get(
        transaction, collection, sizeof(collection), id_one, sizeof(id_one),
        actual, sizeof(actual), &actual_size
    ) == SDB_E_NOT_FOUND);
    assert(sdb_transaction_rollback(transaction) == SDB_OK);
    assert(sdb_transaction_close(transaction) == SDB_OK);
    assert(sdb_document_get(
        database, collection, sizeof(collection), id_one, sizeof(id_one),
        actual, sizeof(actual), &actual_size
    ) == SDB_OK);
}

static void test_operation_limit(sdb_database *database)
{
    static const uint8_t key[] = "operation-limit";
    uint8_t value = 0U;
    size_t index;
    sdb_transaction *transaction = NULL;

    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    for (index = 0U;
         index < SDB_TRANSACTION_MAX_OPERATIONS;
         ++index) {
        value = (uint8_t)index;
        assert(sdb_transaction_kv_put(
            transaction, namespace_name, sizeof(namespace_name),
            key, sizeof(key), &value, sizeof(value)
        ) == SDB_OK);
    }
    assert(sdb_transaction_kv_put(
        transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), &value, sizeof(value)
    ) == SDB_E_OVERFLOW);
    assert(sdb_transaction_commit(transaction) == SDB_OK);
    assert(sdb_transaction_close(transaction) == SDB_OK);
    {
        uint8_t actual = 0U;
        size_t actual_size = 0U;
        assert(sdb_kv_get(
            database, namespace_name, sizeof(namespace_name),
            key, sizeof(key), &actual, sizeof(actual), &actual_size
        ) == SDB_OK);
        assert(actual_size == sizeof(actual));
        assert(actual == value);
    }
}

/*
 * Regression for the batch-poison gap on in-place mutations. A large
 * (multi-chunk) value is put and then deleted in the SAME explicit
 * transaction. The delete removes chunk 0 in place on a leaf already staged by
 * the put (that leaf still holds chunk 1, so it is not reclaimed and the
 * batch's distinct-page count does not change), then hits an injected
 * key-build failure at chunk 1. That error is returned to the caller; a caller
 * that ignores it and commits anyway must NOT get a successful commit of the
 * half-deleted object. The batch must be poisoned so commit aborts. Before the
 * revision-counter fix, poison keyed off the distinct-page count and missed
 * the in-place mutation, so commit succeeded and left the object unreadable.
 */
static void test_poison_partial_inplace_delete(sdb_database *database)
{
    sdb_transaction *transaction = NULL;
    static const uint8_t key[] = "poison-key";
    uint8_t big[5000];
    sdb_status commit_status;
    (void)memset(big, 0x5a, sizeof(big));
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    assert(sdb_transaction_kv_put(
        transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), big, sizeof(big)
    ) == SDB_OK);
    sdb_engine_chunk_key_fail_for_testing(1U);
    /* Deliberately ignore the delete's error to exercise the poison guard. */
    (void)sdb_transaction_kv_delete(
        transaction, namespace_name, sizeof(namespace_name), key, sizeof(key)
    );
    sdb_engine_chunk_key_clear_failure_for_testing();
    commit_status = sdb_transaction_commit(transaction);
    assert(commit_status != SDB_OK);
}

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_verify_result verify;
    sdb_transaction *invalid_transaction =
        (sdb_transaction *)(uintptr_t)1U;

    remove_test_files();
    assert(sdb_transaction_begin(
        NULL, &invalid_transaction
    ) == SDB_E_INVALID_ARGUMENT);
    assert(invalid_transaction == NULL);
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        database_path, &options, &database
    ) == SDB_OK);
    test_rollback_and_lifecycle(database);
    test_multi_object_commit(database);
    test_document_and_unique_index(database);
    test_operation_limit(database);
    test_poison_partial_inplace_delete(database);
    assert(sdb_database_verify(database, &verify) == SDB_OK);
    /*
     * verify() must actually reflect the data the tests committed, not merely
     * return SDB_OK. Exactly four object-metadata rows survive at this point:
     * kv "one" and blob "two" (test_multi_object_commit), the document
     * "user-1" (test_document_and_unique_index; id_two and the
     * delete-then-rollback never persist), and kv "operation-limit"
     * (test_operation_limit). Index definitions and index terms live under
     * different key prefixes and are NOT counted as objects, so object_count
     * is precisely four. A verify that silently stopped walking the tree, or
     * double-counted, would break this.
     */
    assert(verify.object_count == 4U);
    assert(sdb_database_close(database) == SDB_OK);

    database = NULL;
    assert(sdb_database_open(database_path, &options, &database) == SDB_OK);
    {
        static const uint8_t key[] = "one";
        static const uint8_t value[] = "new";
        assert_kv(database, key, sizeof(key), value, sizeof(value));
    }
    assert(sdb_database_close(database) == SDB_OK);
    remove_test_files();
    (void)puts("public transaction tests: ok");
    return 0;
}
