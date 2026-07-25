#include "engine_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-engine-fault.tmp";
static const char *wal_path = "test-engine-fault.tmp.wal";
static const char *lock_path = "test-engine-fault.tmp.lock";
static const uint8_t namespace_name[] = "fault";
static const uint8_t key[] = "large";

static void fill(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        bytes[index] = (uint8_t)(seed + (uint8_t)(index * 13U));
    }
}

int main(void)
{
    sdb_database_options options;
    uint8_t old_value[5000];
    uint8_t new_value[7000];
    size_t boundary;
    fill(old_value, sizeof(old_value), 0x21U);
    fill(new_value, sizeof(new_value), 0x91U);
    sdb_database_options_init(&options);

    /*
     * Sweep enough database-file operations to cut chunk writes, B+Tree
     * splits, page apply, checkpoint mirrors, and the final metadata head.
     */
    for (boundary = 0U; boundary < 80U; ++boundary) {
        sdb_database *database;
        sdb_file *file;
        uint8_t actual[7000];
        size_t actual_size;
        sdb_status status;
        (void)remove(test_path);
        (void)remove(wal_path);
        (void)remove(lock_path);
        assert(sdb_database_create(
            test_path, &options, &database
        ) == SDB_OK);
        assert(sdb_blob_put(
            database,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            old_value,
            sizeof(old_value)
        ) == SDB_OK);
        file = sdb_database_file_for_testing(database);
        assert(file != NULL);
        sdb_file_fail_after_for_testing(file, boundary);
        (void)sdb_blob_put(
            database,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            new_value,
            sizeof(new_value)
        );
        sdb_file_clear_failure_for_testing(file);
        assert(sdb_database_close(database) == SDB_OK);
        assert(sdb_database_open(
            test_path, &options, &database
        ) == SDB_OK);
        status = sdb_blob_get(
            database,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            actual,
            sizeof(actual),
            &actual_size
        );
        assert(status == SDB_OK);
        if (actual_size == sizeof(old_value)) {
            assert(memcmp(actual, old_value, sizeof(old_value)) == 0);
        } else {
            assert(actual_size == sizeof(new_value));
            assert(memcmp(actual, new_value, sizeof(new_value)) == 0);
        }
        assert(sdb_database_close(database) == SDB_OK);
    }
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
    (void)puts("engine fault tests: ok");
    return 0;
}
