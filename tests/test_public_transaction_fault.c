#include "engine_internal.h"
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *database_path = "test-public-transaction-fault.tmp";
static const char *wal_path = "test-public-transaction-fault.tmp.wal";
static const char *lock_path = "test-public-transaction-fault.tmp.lock";
static const uint8_t namespace_name[] = "atomic";
static const uint8_t first_key[] = "first";
static const uint8_t second_key[] = "second";

static void remove_test_files(void)
{
    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
}

static void fill(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        bytes[index] = (uint8_t)(seed + (uint8_t)(index * 19U));
    }
}

static bool value_is(
    sdb_database *database,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *expected,
    size_t expected_size
)
{
    uint8_t actual[7000];
    size_t actual_size = 0U;
    assert(sdb_blob_get(
        database, namespace_name, sizeof(namespace_name), key, key_size,
        actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    return actual_size == expected_size
        && memcmp(actual, expected, expected_size) == 0;
}

static bool run_boundary(
    size_t boundary, bool fail_wal,
    const uint8_t *old_value, size_t old_size,
    const uint8_t *new_value, size_t new_size
)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_transaction *transaction = NULL;
    sdb_file *file;
    sdb_status first_status;
    sdb_status second_status;
    bool first_new;
    bool second_new;

    remove_test_files();
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        database_path, &options, &database
    ) == SDB_OK);
    assert(sdb_blob_put(
        database, namespace_name, sizeof(namespace_name),
        first_key, sizeof(first_key), old_value, old_size
    ) == SDB_OK);
    assert(sdb_blob_put(
        database, namespace_name, sizeof(namespace_name),
        second_key, sizeof(second_key), old_value, old_size
    ) == SDB_OK);
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    file = sdb_database_file_for_testing(database);
    assert(file != NULL);
    if (fail_wal) {
        sdb_wal_fail_after_for_testing(boundary);
    } else {
        sdb_file_fail_after_for_testing(file, boundary);
    }
    first_status = sdb_transaction_blob_put(
        transaction, namespace_name, sizeof(namespace_name),
        first_key, sizeof(first_key), new_value, new_size
    );
    second_status = first_status == SDB_OK
        ? sdb_transaction_blob_put(
            transaction, namespace_name, sizeof(namespace_name),
            second_key, sizeof(second_key), new_value, new_size
        )
        : first_status;
    (void)second_status;
    (void)sdb_transaction_commit(transaction);
    sdb_wal_clear_failure_for_testing();
    sdb_file_clear_failure_for_testing(file);
    assert(sdb_transaction_close(transaction) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);

    database = NULL;
    assert(sdb_database_open(
        database_path, &options, &database
    ) == SDB_OK);
    first_new = value_is(
        database, first_key, sizeof(first_key), new_value, new_size
    );
    second_new = value_is(
        database, second_key, sizeof(second_key), new_value, new_size
    );
    assert(first_new == second_new);
    if (!first_new) {
        assert(value_is(
            database, first_key, sizeof(first_key), old_value, old_size
        ));
        assert(value_is(
            database, second_key, sizeof(second_key), old_value, old_size
        ));
    }
    assert(sdb_database_close(database) == SDB_OK);
    return first_new;
}

int main(void)
{
    uint8_t old_value[5000];
    uint8_t new_value[7000];
    size_t boundary;
    bool committed;
    bool saw_committed = false;
    bool saw_rolled_back = false;

    fill(old_value, sizeof(old_value), 0x31U);
    fill(new_value, sizeof(new_value), 0xa1U);
    for (boundary = 0U; boundary < 18U; ++boundary) {
        committed = run_boundary(
            boundary, true,
            old_value, sizeof(old_value), new_value, sizeof(new_value)
        );
        if (committed) {
            saw_committed = true;
        } else {
            saw_rolled_back = true;
        }
    }
    for (boundary = 0U; boundary < 100U; ++boundary) {
        committed = run_boundary(
            boundary, false,
            old_value, sizeof(old_value), new_value, sizeof(new_value)
        );
        if (committed) {
            saw_committed = true;
        } else {
            saw_rolled_back = true;
        }
    }
    /*
     * The boundary sweep is only meaningful if the injected faults actually
     * land on BOTH sides of the commit point: some early enough that the
     * commit fails and the reopened database holds the OLD values (atomic
     * rollback), and some late enough that the commit succeeds and it holds
     * the NEW values. Without this a run where every boundary rolled back
     * (or every boundary committed) would pass while proving nothing about
     * atomicity under partial failure.
     */
    assert(saw_committed);
    assert(saw_rolled_back);
    remove_test_files();
    (void)puts("public transaction fault tests: ok");
    return 0;
}
