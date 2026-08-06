#include "engine_internal.h"
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *database_path = "test-public-transaction-crash.tmp";
static const char *wal_path = "test-public-transaction-crash.tmp.wal";
static const char *lock_path = "test-public-transaction-crash.tmp.lock";
static const uint8_t namespace_name[] = "crash";
static const uint8_t first_key[] = "first";
static const uint8_t second_key[] = "second";
static const uint8_t old_value[] = "old-value";
static const uint8_t new_value[] = "new-value";

static void remove_test_files(void)
{
    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
}

static void prepare(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    remove_test_files();
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        database_path, &options, &database
    ) == SDB_OK);
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name),
        first_key, sizeof(first_key), old_value, sizeof(old_value)
    ) == SDB_OK);
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name),
        second_key, sizeof(second_key), old_value, sizeof(old_value)
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
}

static bool is_new(
    sdb_database *database, const uint8_t *key, size_t key_size
)
{
    uint8_t actual[32];
    size_t actual_size = 0U;
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name), key, key_size,
        actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    if (actual_size == sizeof(new_value)
        && memcmp(actual, new_value, sizeof(new_value)) == 0) {
        return true;
    }
    assert(actual_size == sizeof(old_value));
    assert(memcmp(actual, old_value, sizeof(old_value)) == 0);
    return false;
}

static void child_commit(size_t boundary, bool fail_wal)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_transaction *transaction = NULL;
    sdb_file *file;
    sdb_database_options_init(&options);
    if (sdb_database_open(database_path, &options, &database) != SDB_OK
        || sdb_transaction_begin(database, &transaction) != SDB_OK) {
        _Exit(71);
    }
    file = sdb_database_file_for_testing(database);
    if (fail_wal) {
        sdb_wal_fail_after_for_testing(boundary);
    } else {
        sdb_file_fail_after_for_testing(file, boundary);
    }
    (void)sdb_transaction_kv_put(
        transaction, namespace_name, sizeof(namespace_name),
        first_key, sizeof(first_key), new_value, sizeof(new_value)
    );
    (void)sdb_transaction_kv_put(
        transaction, namespace_name, sizeof(namespace_name),
        second_key, sizeof(second_key), new_value, sizeof(new_value)
    );
    (void)sdb_transaction_commit(transaction);
    _Exit(0);
}

static bool run_boundary(size_t boundary, bool fail_wal)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    pid_t child;
    int wait_status;
    bool first_new;
    bool second_new;

    prepare();
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        child_commit(boundary, fail_wal);
    }
    assert(waitpid(child, &wait_status, 0) == child);
    assert(WIFEXITED(wait_status));
    assert(WEXITSTATUS(wait_status) == 0);
    sdb_database_options_init(&options);
    assert(sdb_database_open(
        database_path, &options, &database
    ) == SDB_OK);
    first_new = is_new(database, first_key, sizeof(first_key));
    second_new = is_new(database, second_key, sizeof(second_key));
    assert(first_new == second_new);
    assert(sdb_database_close(database) == SDB_OK);
    return first_new;
}

int main(void)
{
    size_t boundary;
    bool committed;
    bool saw_new = false;
    bool saw_old = false;
    for (boundary = 0U; boundary < 8U; ++boundary) {
        committed = run_boundary(boundary, true);
        if (committed) {
            saw_new = true;
        } else {
            saw_old = true;
        }
    }
    for (boundary = 0U; boundary < 16U; ++boundary) {
        committed = run_boundary(boundary, false);
        if (committed) {
            saw_new = true;
        } else {
            saw_old = true;
        }
    }
    /*
     * The crash-point sweep only proves durability semantics if the child
     * process was actually killed on both sides of the durable commit:
     * at least one boundary must crash BEFORE the commit is durable (parent
     * reopens to the OLD values) and at least one AFTER (parent reopens to
     * the NEW values). If every child happened to crash on the same side the
     * test would stay green even if the crash points never bracketed the
     * commit at all.
     */
    assert(saw_new);
    assert(saw_old);
    remove_test_files();
    (void)puts("public transaction crash tests: ok");
    return 0;
}
