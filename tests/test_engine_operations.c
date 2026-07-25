#include "engine_internal.h"
#include "pager.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static const char *test_path = "test-engine-operations.tmp";
static const char *wal_path = "test-engine-operations.tmp.wal";
static const char *lock_path = "test-engine-operations.tmp.lock";
static const char *backup_path = "test-engine-operations-backup.tmp";
static const char *backup_wal_path = "test-engine-operations-backup.tmp.wal";
static const char *backup_lock_path = "test-engine-operations-backup.tmp.lock";
#ifndef _WIN32
static const char *backup_alias_path =
    "test-engine-operations-backup-alias.tmp";
static const char *backup_hardlink_path =
    "test-engine-operations-backup-hardlink.tmp";
static const char *source_alias_path =
    "test-engine-operations-source-alias.tmp";
#endif
static const uint8_t namespace_name[] = "operations";
static const uint8_t key[] = "value";

static void cleanup(void)
{
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
    (void)remove(backup_wal_path);
    (void)remove(backup_path);
    (void)remove(backup_lock_path);
#ifndef _WIN32
    (void)remove(backup_alias_path);
    (void)remove(backup_hardlink_path);
    (void)remove(source_alias_path);
    {
        DIR *directory = opendir(".");
        struct dirent *entry;
        assert(directory != NULL);
        while ((entry = readdir(directory)) != NULL) {
            static const char source_prefix[] =
                "test-engine-operations.tmp.tmp-";
            static const char backup_prefix[] =
                "test-engine-operations-backup.tmp.tmp-";
            if (strncmp(
                    entry->d_name,
                    source_prefix,
                    sizeof(source_prefix) - 1U
                ) == 0
                || strncmp(
                    entry->d_name,
                    backup_prefix,
                    sizeof(backup_prefix) - 1U
                ) == 0) {
                (void)remove(entry->d_name);
            }
        }
        assert(closedir(directory) == 0);
    }
#endif
}

static void fill(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        bytes[index] = (uint8_t)(seed + (uint8_t)(index * 29U));
    }
}

static void create_stale_database(
    const sdb_database_options *options,
    const uint8_t *old_value,
    size_t old_size,
    const uint8_t *new_value,
    size_t new_size
)
{
    sdb_database *database = NULL;
    assert(sdb_database_create(test_path, options, &database) == SDB_OK);
    assert(sdb_blob_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        old_value,
        old_size
    ) == SDB_OK);
    assert(sdb_blob_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        new_value,
        new_size
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
}

#ifndef _WIN32
static void verify_new_value(
    const sdb_database_options *options,
    const uint8_t *new_value,
    size_t new_size
)
{
    sdb_database *database = NULL;
    sdb_verify_result verify;
    uint8_t actual[15000];
    size_t actual_size = 0U;
    assert(sdb_database_open(test_path, options, &database) == SDB_OK);
    assert(sdb_database_verify(database, &verify) == SDB_OK);
    assert(sdb_blob_get(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        actual,
        sizeof(actual),
        &actual_size
    ) == SDB_OK);
    assert(actual_size == new_size);
    assert(memcmp(actual, new_value, new_size) == 0);
    assert(sdb_database_close(database) == SDB_OK);
}
#endif

static void test_backup_atomicity(
    const sdb_database_options *options,
    const uint8_t *old_value,
    size_t old_size,
    const uint8_t *new_value,
    size_t new_size
)
{
    sdb_database *database = NULL;
    sdb_backup_result backup_result;
    size_t boundary;
    cleanup();
    assert(sdb_database_create(test_path, options, &database) == SDB_OK);
    assert(sdb_blob_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        old_value,
        old_size
    ) == SDB_OK);
    assert(sdb_database_backup(
        database, backup_path, false, &backup_result
    ) == SDB_OK);
    {
        sdb_database *open_backup = NULL;
        assert(sdb_database_open(
            backup_path, options, &open_backup
        ) == SDB_OK);
        assert(sdb_database_backup(
            database, backup_path, true, &backup_result
        ) == SDB_E_BUSY);
        assert(sdb_database_close(open_backup) == SDB_OK);
    }
#ifndef _WIN32
    assert(symlink(backup_path, backup_alias_path) == 0);
    {
        sdb_database *open_backup = NULL;
        assert(sdb_database_open(
            backup_alias_path, options, &open_backup
        ) == SDB_OK);
        assert(sdb_database_backup(
            database, backup_path, true, &backup_result
        ) == SDB_E_BUSY);
        assert(sdb_database_close(open_backup) == SDB_OK);
    }
    assert(sdb_database_backup(
        database, backup_alias_path, true, &backup_result
    ) == SDB_OK);
    assert(link(backup_path, backup_hardlink_path) == 0);
    assert(sdb_database_backup(
        database, backup_hardlink_path, true, &backup_result
    ) == SDB_E_INVALID_ARGUMENT);
    assert(unlink(backup_hardlink_path) == 0);
    assert(symlink(test_path, source_alias_path) == 0);
    assert(sdb_database_backup(
        database, source_alias_path, true, &backup_result
    ) == SDB_E_INVALID_ARGUMENT);
#endif
    assert(sdb_blob_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        new_value,
        new_size
    ) == SDB_OK);
    {
        sdb_file *source_file = sdb_database_file_for_testing(database);
        uint64_t source_size;
        size_t destination_operations;
        assert(source_file != NULL);
        assert(sdb_file_size(source_file, &source_size) == SDB_OK);
        destination_operations =
            (size_t)(source_size / UINT64_C(65536))
            + (source_size % UINT64_C(65536) != 0U ? 1U : 0U)
            + 1U;
        for (boundary = 0U;
             boundary <= destination_operations;
             ++boundary) {
            sdb_database *backup = NULL;
            sdb_status status;
            sdb_status backup_status;
            uint8_t actual[15000];
            size_t actual_size = 0U;
            sdb_engine_backup_file_fail_after_for_testing(boundary);
            backup_status = sdb_database_backup(
                database, backup_path, true, &backup_result
            );
            sdb_engine_backup_file_clear_failure_for_testing();
            assert(
                (boundary < destination_operations
                    && backup_status == SDB_E_IO)
                || (boundary == destination_operations
                    && backup_status == SDB_OK)
            );
            assert(sdb_database_open(
                backup_path, options, &backup
            ) == SDB_OK);
            assert(sdb_blob_get(
                backup,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                actual,
                sizeof(actual),
                &actual_size
            ) == SDB_OK);
            status = actual_size == old_size
                && memcmp(actual, old_value, old_size) == 0
                ? SDB_OK
                : (actual_size == new_size
                    && memcmp(actual, new_value, new_size) == 0
                    ? SDB_OK : SDB_E_CORRUPT);
            assert(status == SDB_OK);
            assert(sdb_database_close(backup) == SDB_OK);
        }
    }
    for (boundary = 0U; boundary < 24U; ++boundary) {
        sdb_database *backup = NULL;
        sdb_file *file = sdb_database_file_for_testing(database);
        uint8_t actual[15000];
        size_t actual_size = 0U;
        sdb_status status;
        assert(file != NULL);
        sdb_file_fail_after_for_testing(file, boundary);
        (void)sdb_database_backup(
            database, backup_path, true, &backup_result
        );
        sdb_file_clear_failure_for_testing(file);
        assert(sdb_database_open(
            backup_path, options, &backup
        ) == SDB_OK);
        assert(sdb_blob_get(
            backup,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            actual,
            sizeof(actual),
            &actual_size
        ) == SDB_OK);
        status = actual_size == old_size
            && memcmp(actual, old_value, old_size) == 0
            ? SDB_OK
            : (actual_size == new_size
                && memcmp(actual, new_value, new_size) == 0
                ? SDB_OK : SDB_E_CORRUPT);
        assert(status == SDB_OK);
        assert(sdb_database_close(backup) == SDB_OK);
    }
    assert(sdb_database_close(database) == SDB_OK);
}

#ifndef _WIN32
static void test_backup_crash_phases(
    const sdb_database_options *options,
    const uint8_t *old_value,
    size_t old_size,
    const uint8_t *new_value,
    size_t new_size
)
{
    unsigned phase;
    for (phase = 1U; phase <= 4U; ++phase) {
        sdb_database *destination = NULL;
        pid_t child;
        int child_status = 0;
        cleanup();
        create_stale_database(
            options, old_value, old_size, new_value, new_size
        );
        assert(sdb_database_create(
            backup_path, options, &destination
        ) == SDB_OK);
        assert(sdb_blob_put(
            destination,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            old_value,
            old_size
        ) == SDB_OK);
        assert(sdb_database_close(destination) == SDB_OK);
        child = fork();
        assert(child >= 0);
        if (child == 0) {
            sdb_database *source = NULL;
            sdb_backup_result result;
            if (sdb_database_open(
                    test_path, options, &source
                ) != SDB_OK) {
                _exit(10);
            }
            sdb_engine_crash_after_backup_phase_for_testing(phase);
            (void)sdb_database_backup(
                source, backup_path, true, &result
            );
            _exit(11);
        }
        assert(waitpid(child, &child_status, 0) == child);
        assert(WIFEXITED(child_status));
        assert(WEXITSTATUS(child_status) == 98);
        {
            sdb_database *recovered = NULL;
            uint8_t actual[15000];
            size_t actual_size = 0U;
            assert(sdb_database_open(
                backup_path, options, &recovered
            ) == SDB_OK);
            assert(sdb_blob_get(
                recovered,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                actual,
                sizeof(actual),
                &actual_size
            ) == SDB_OK);
            assert(
                (actual_size == old_size
                    && memcmp(actual, old_value, old_size) == 0)
                || (actual_size == new_size
                    && memcmp(actual, new_value, new_size) == 0)
            );
            assert(sdb_database_close(recovered) == SDB_OK);
        }
    }
}

static void test_compact_crash_phases(
    const sdb_database_options *options,
    const uint8_t *old_value,
    size_t old_size,
    const uint8_t *new_value,
    size_t new_size
)
{
    unsigned phase;
    for (phase = 1U; phase <= 3U; ++phase) {
        pid_t child;
        int child_status = 0;
        cleanup();
        create_stale_database(
            options, old_value, old_size, new_value, new_size
        );
        child = fork();
        assert(child >= 0);
        if (child == 0) {
            sdb_database *database = NULL;
            sdb_compact_result result;
            if (sdb_database_open(
                    test_path, options, &database
                ) != SDB_OK) {
                _exit(10);
            }
            sdb_engine_crash_after_compact_phase_for_testing(phase);
            (void)sdb_database_compact(database, options, &result);
            _exit(11);
        }
        assert(waitpid(child, &child_status, 0) == child);
        assert(WIFEXITED(child_status));
        assert(WEXITSTATUS(child_status) == 99);
        verify_new_value(options, new_value, new_size);
    }
}
#endif

static void test_verify_corruption(
    const sdb_database_options *options,
    const uint8_t *old_value,
    size_t old_size,
    const uint8_t *new_value,
    size_t new_size
)
{
    sdb_file file;
    uint64_t size;
    uint8_t byte;
    sdb_database *database = NULL;
    sdb_verify_result result;
    sdb_status status;
    cleanup();
    create_stale_database(
        options, old_value, old_size, new_value, new_size
    );
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_file_size(&file, &size) == SDB_OK);
    assert(size != 0U);
    assert(sdb_file_read_full(&file, size - 1U, &byte, 1U) == SDB_OK);
    byte ^= UINT8_C(0x80);
    assert(sdb_file_write_full(&file, size - 1U, &byte, 1U) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
    status = sdb_database_open(test_path, options, &database);
    if (status == SDB_OK) {
        status = sdb_database_verify(database, &result);
        assert(sdb_database_close(database) == SDB_OK);
    }
    assert(status == SDB_E_CORRUPT);
}

static void test_verify_rejects_orphan_page(
    const sdb_database_options *options
)
{
    sdb_database *database = NULL;
    sdb_pager pager;
    uint64_t orphan_page;
    sdb_verify_result result;
    cleanup();
    assert(sdb_database_create(test_path, options, &database) == SDB_OK);
    assert(sdb_kv_put(
        database,
        namespace_name,
        sizeof(namespace_name),
        key,
        sizeof(key),
        key,
        sizeof(key)
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &orphan_page) == SDB_OK);
    assert(orphan_page != pager.superblock.root_page);
    assert(sdb_pager_close(&pager) == SDB_OK);
    database = NULL;
    assert(sdb_database_open(test_path, options, &database) == SDB_OK);
    assert(sdb_database_verify(database, &result) == SDB_E_CORRUPT);
    assert(sdb_database_close(database) == SDB_OK);
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_pager_free(&pager, orphan_page) == SDB_OK);
    assert(sdb_pager_close(&pager) == SDB_OK);
    database = NULL;
    assert(sdb_database_open(test_path, options, &database) == SDB_OK);
    assert(sdb_database_verify(database, &result) == SDB_OK);
    assert(result.free_page_count == 1U);
    assert(sdb_database_close(database) == SDB_OK);
}

int main(void)
{
    sdb_database_options options;
    uint8_t old_value[12000];
    uint8_t new_value[15000];
    fill(old_value, sizeof(old_value), 0x17U);
    fill(new_value, sizeof(new_value), 0xa1U);
    sdb_database_options_init(&options);
    test_backup_atomicity(
        &options,
        old_value,
        sizeof(old_value),
        new_value,
        sizeof(new_value)
    );
#ifndef _WIN32
    test_backup_crash_phases(
        &options,
        old_value,
        sizeof(old_value),
        new_value,
        sizeof(new_value)
    );
    test_compact_crash_phases(
        &options,
        old_value,
        sizeof(old_value),
        new_value,
        sizeof(new_value)
    );
#endif
    test_verify_corruption(
        &options,
        old_value,
        sizeof(old_value),
        new_value,
        sizeof(new_value)
    );
    test_verify_rejects_orphan_page(&options);
    cleanup();
    (void)puts("engine operations tests: ok");
    return 0;
}
